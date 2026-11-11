/* Research harness (night 2026-11-11): generates the same chunks on one WgWorld and on one WgWorld per thread,
 * compares an XXH3 of every chunk, times it, and can write hash lists and PPM pictures.
 *
 *   wgbench hash   <seed> <version> <chunks> <out.txt>     one instance, in order: writes "cx cy rawhash corehash"
 *   wgbench par    <seed> <version> <chunks> <threads> [shuffle]   sequential, then threads: compares, prints times
 *   wgbench time   <seed> <version> <chunks> [repeats]     chunks per second on one instance
 *   wgbench mem    <seed> <version> <chunks>               heap use of one instance (glibc only)
 *   wgbench ppm    <seed> <version> <cx> <cy> <wchunks> <hchunks> <out-prefix>   <prefix>-biome.ppm, -slab.ppm
 *
 * Build (native): gcc -O3 -march=x86-64 -I../../src -I../../deps/xxhash wgbench.c ../../src/world_gen.c -lm -lpthread
 */
#define _GNU_SOURCE
#include "world_gen.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <malloc.h>
#include <pthread.h>
#include <time.h>
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

struct Job {
    int32_t cx, cy;
};

/* the regions: near the start, the middle distance, far out, very far, and negative coordinates */
static const int32_t regions[][2] = {
    {0, 0}, {40, -25}, {-70, 55}, {150, 120}, {-400, 260}, {900, -700}, {-3000, -2000}, {20000, 12345},
    {-150000, 90000}, {70, 70}, {-20, 110}, {260, -330},
};
#define REGIONS ((int)(sizeof regions / sizeof regions[0]))

static struct Job *make_jobs(int n)
{
    struct Job *jobs = (struct Job *)malloc(sizeof(struct Job) * (size_t)n);
    int per = (n + REGIONS - 1) / REGIONS, side = 1, i = 0, r, x, y;
    int stride = getenv("WG_STRIDE") ? atoi(getenv("WG_STRIDE")) : 1; /* >1: chunks that share no cached work */
    while (side * side < per)
        side++;
    for (r = 0; r < REGIONS && i < n; r++)
        for (y = 0; y < side && i < n; y++)
            for (x = 0; x < side && i < n; x++) {
                if (y * side + x >= per)
                    break;
                jobs[i].cx = regions[r][0] + (x - side / 2) * stride;
                jobs[i].cy = regions[r][1] + (y - side / 2) * stride;
                i++;
            }
    while (i < n) { /* (rounding) */
        jobs[i].cx = 5000 + i;
        jobs[i].cy = -5000;
        i++;
    }
    return jobs;
}

struct Hashes {
    uint64_t raw, core;
    int rc;
};

/* raw: every byte of the WgChunk (padding too: wg_generate_chunk clears it first). core: the fields the game uses,
 * one by one, so it is the same on 32-bit and 64-bit builds whatever the padding. */
static void hash_chunk(const struct WgChunk *c, int rc, struct Hashes *h)
{
    XXH3_state_t st;
    int i;
    h->rc = rc;
    h->raw = XXH3_64bits(c, sizeof *c);
    XXH3_64bits_reset(&st);
    XXH3_64bits_update(&st, c->slabs, sizeof c->slabs);
    XXH3_64bits_update(&st, c->owners, sizeof c->owners);
    XXH3_64bits_update(&st, c->biomes, sizeof c->biomes);
    XXH3_64bits_update(&st, &c->has_keeper, 1);
    XXH3_64bits_update(&st, &c->thing_count, sizeof c->thing_count);
    XXH3_64bits_update(&st, &c->struct_count, sizeof c->struct_count);
    for (i = 0; i < c->thing_count; i++) {
        const struct WgThing *t = &c->things[i];
        XXH3_64bits_update(&st, &t->is_creature, 1);
        XXH3_64bits_update(&st, t->name, sizeof t->name);
        XXH3_64bits_update(&st, &t->owner, 1);
        XXH3_64bits_update(&st, &t->x, sizeof t->x);
        XXH3_64bits_update(&st, &t->y, sizeof t->y);
        XXH3_64bits_update(&st, &t->level, 1);
        XXH3_64bits_update(&st, &t->has_gate, 1);
    }
    for (i = 0; i < c->struct_count; i++) {
        const struct WgStructRef *r = &c->structs[i];
        XXH3_64bits_update(&st, &r->origin_x, 6 * sizeof(int64_t)); /* origin and bbox */
        XXH3_64bits_update(&st, &r->distance, sizeof r->distance);
        XXH3_64bits_update(&st, &r->heart_x, 8 * sizeof(int64_t));
        XXH3_64bits_update(&st, &r->threat_radius, 3 * sizeof(int32_t));
        XXH3_64bits_update(&st, r->box, sizeof r->box);
        XXH3_64bits_update(&st, r->rooms, sizeof r->rooms);
        XXH3_64bits_update(&st, &r->creature_kinds, sizeof r->creature_kinds);
        XXH3_64bits_update(&st, &r->biome, 1);
    }
    h->core = XXH3_64bits_digest(&st);
}

static void run_seq(uint64_t seed, int version, const struct Job *jobs, int n, struct Hashes *out, double *secs)
{
    struct WgWorld *w = wg_world_create_version(seed, version);
    struct WgChunk *c = (struct WgChunk *)malloc(sizeof *c);
    double t0 = now_s();
    int i;
    for (i = 0; i < n; i++) {
        int rc = wg_generate_chunk(w, jobs[i].cx, jobs[i].cy, c);
        if (out != NULL)
            hash_chunk(c, rc, &out[i]);
    }
    *secs = now_s() - t0;
    free(c);
    wg_world_destroy(w);
}

#ifndef _WIN32
struct Par {
    uint64_t seed;
    int version, n;
    const struct Job *jobs;
    const int *order;
    struct Hashes *out;
    volatile int next;
};

static void *par_main(void *arg)
{
    struct Par *p = (struct Par *)arg;
    struct WgWorld *w = wg_world_create_version(p->seed, p->version);
    struct WgChunk *c = (struct WgChunk *)malloc(sizeof *c);
    const int block = getenv("WG_BLOCK") ? atoi(getenv("WG_BLOCK")) : 8; /* chunks a thread takes at a time */
    for (;;) {
        int k = __sync_fetch_and_add(&p->next, block), e = k + block, i;
        if (k >= p->n)
            break;
        if (e > p->n)
            e = p->n;
        for (; k < e; k++) {
            int rc;
            i = p->order[k];
            rc = wg_generate_chunk(w, p->jobs[i].cx, p->jobs[i].cy, c);
            hash_chunk(c, rc, &p->out[i]);
        }
    }
    free(c);
    wg_world_destroy(w);
    return NULL;
}

static double run_par(uint64_t seed, int version, const struct Job *jobs, int n, int threads, int shuffle,
                      struct Hashes *out)
{
    struct Par p;
    pthread_t th[64];
    int *order = (int *)malloc(sizeof(int) * (size_t)n), i;
    double t0;
    uint64_t r = 0x9E3779B97F4A7C15ULL ^ (uint64_t)threads;
    for (i = 0; i < n; i++)
        order[i] = i;
    if (shuffle)
        for (i = n - 1; i > 0; i--) {
            int j, t;
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            j = (int)(r % (uint64_t)(i + 1));
            t = order[i]; order[i] = order[j]; order[j] = t;
        }
    p.seed = seed; p.version = version; p.n = n; p.jobs = jobs; p.order = order; p.out = out; p.next = 0;
    t0 = now_s();
    for (i = 0; i < threads; i++)
        pthread_create(&th[i], NULL, par_main, &p);
    for (i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    t0 = now_s() - t0;
    free(order);
    return t0;
}
#endif

static const unsigned char biome_rgb[13][3] = {
    {120, 85, 50}, {110, 110, 110}, {200, 60, 20}, {40, 90, 200}, {200, 230, 255}, {220, 190, 110}, {190, 80, 220},
    {70, 130, 70}, {255, 120, 0}, {0, 40, 140}, {255, 180, 60}, {60, 160, 230}, {30, 30, 30},
};

static void slab_rgb(int s, unsigned char *p)
{
    const char *n = wg_slab_name(s);
    unsigned char r = 255, g = 0, b = 255;
    if (strstr(n, "ROCK") || strstr(n, "Rock") || strstr(n, "rock")) { r = 25; g = 25; b = 25; }
    else if (strstr(n, "DENSE") || strstr(n, "dense")) { r = 255; g = 170; b = 0; }
    else if (strstr(n, "GOLD") || strstr(n, "gold")) { r = 230; g = 210; b = 40; }
    else if (strstr(n, "EARTH") || strstr(n, "earth")) { r = 120; g = 85; b = 50; }
    else if (strstr(n, "LAVA") || strstr(n, "lava")) { r = 230; g = 60; b = 10; }
    else if (strstr(n, "WATER") || strstr(n, "water")) { r = 40; g = 100; b = 220; }
    else if (strstr(n, "GEM") || strstr(n, "gem")) { r = 200; g = 60; b = 230; }
    else if (strstr(n, "PATH") || strstr(n, "path")) { r = 190; g = 170; b = 140; }
    else { r = 240; g = 240; b = 240; }
    p[0] = r; p[1] = g; p[2] = b;
}

static int do_ppm(uint64_t seed, int version, int cx, int cy, int wc, int hc, const char *prefix)
{
    struct WgWorld *w = wg_world_create_version(seed, version);
    struct WgChunk *c = (struct WgChunk *)malloc(sizeof *c);
    int W = wc * 16, H = hc * 16, x, y, i;
    unsigned char *bi = (unsigned char *)malloc((size_t)W * H * 3), *sl = (unsigned char *)malloc((size_t)W * H * 3);
    char name[512];
    FILE *f;
    for (y = 0; y < hc; y++)
        for (x = 0; x < wc; x++) {
            wg_generate_chunk(w, cx + x, cy + y, c);
            for (i = 0; i < 256; i++) {
                size_t o = ((size_t)(y * 16 + i / 16) * W + (size_t)(x * 16 + i % 16)) * 3;
                memcpy(bi + o, biome_rgb[c->biomes[i] % 13], 3);
                slab_rgb(c->slabs[i], sl + o);
            }
        }
    snprintf(name, sizeof name, "%s-biome.ppm", prefix);
    f = fopen(name, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(bi, 3, (size_t)W * H, f); fclose(f);
    snprintf(name, sizeof name, "%s-slab.ppm", prefix);
    f = fopen(name, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(sl, 3, (size_t)W * H, f); fclose(f);
    free(bi); free(sl); free(c);
    wg_world_destroy(w);
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t seed;
    int version, n, i;
    struct Job *jobs;
    struct Hashes *ref;
    double secs;
    if (argc < 5) {
        fprintf(stderr, "usage: see the top of wgbench.c\n");
        return 2;
    }
    seed = strtoull(argv[2], NULL, 10);
    version = atoi(argv[3]);
    if (strcmp(argv[1], "ppm") == 0 && argc >= 9)
        return do_ppm(seed, version, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), argv[8]);
    n = atoi(argv[4]);
    jobs = make_jobs(n);
    ref = (struct Hashes *)calloc((size_t)n, sizeof *ref);
    if (strcmp(argv[1], "hash") == 0 && argc >= 6) {
        FILE *f = fopen(argv[5], "wb");
        int hist[16] = {0}, errs = 0;
        struct WgWorld *w = wg_world_create_version(seed, version);
        struct WgChunk *c = (struct WgChunk *)malloc(sizeof *c);
        double t0 = now_s();
        for (i = 0; i < n; i++) {
            int rc = wg_generate_chunk(w, jobs[i].cx, jobs[i].cy, c);
            hash_chunk(c, rc, &ref[i]);
            hist[c->biomes[8 * 16 + 8] & 15]++;
            errs += rc != 0;
            fprintf(f, "%d %d %016llx %016llx %d\n", jobs[i].cx, jobs[i].cy, (unsigned long long)ref[i].raw,
                    (unsigned long long)ref[i].core, rc);
        }
        secs = now_s() - t0;
        fclose(f);
        printf("hash: %d chunks v%d seed %llu in %.3f s = %.0f chunks/s; rc!=0: %d; sizeof(WgChunk)=%u; biomes:", n,
               version, (unsigned long long)seed, secs, n / secs, errs, (unsigned)sizeof(struct WgChunk));
        for (i = 0; i < 13; i++)
            printf(" %s=%d", wg_biome_name(i), hist[i]);
        printf("\n");
        return 0;
    }
    if (strcmp(argv[1], "time") == 0) {
        int reps = argc >= 6 ? atoi(argv[5]) : 1;
        for (i = 0; i < reps; i++) {
            run_seq(seed, version, jobs, n, NULL, &secs);
            printf("time: v%d %d chunks %.3f s = %.1f chunks/s (%.1f us/chunk)\n", version, n, secs, n / secs,
                   secs / n * 1e6);
            fflush(stdout);
        }
        return 0;
    }
#ifndef _WIN32
    if (strcmp(argv[1], "mem") == 0) {
        struct mallinfo2 m0 = mallinfo2(), m1, m2;
        struct WgWorld *w = wg_world_create_version(seed, version);
        struct WgChunk *c = (struct WgChunk *)malloc(sizeof *c);
        size_t peak = 0;
        m1 = mallinfo2();
        for (i = 0; i < n; i++) {
            wg_generate_chunk(w, jobs[i].cx, jobs[i].cy, c);
            if ((i & 15) == 0) {
                m2 = mallinfo2();
                if (m2.uordblks + m2.hblkhd > peak)
                    peak = m2.uordblks + m2.hblkhd;
            }
        }
        m2 = mallinfo2();
        printf("mem: sizeof(WgChunk)=%zu; a fresh instance %.1f KB; after %d chunks %.1f KB; peak %.1f KB\n",
               sizeof(struct WgChunk), (double)(m1.uordblks + m1.hblkhd - m0.uordblks - m0.hblkhd) / 1024.0, n,
               (double)(m2.uordblks + m2.hblkhd - m0.uordblks - m0.hblkhd) / 1024.0,
               (double)(peak - m0.uordblks - m0.hblkhd) / 1024.0);
        return 0;
    }
    if (strcmp(argv[1], "par") == 0 && argc >= 6) {
        int threads = atoi(argv[5]), shuffle = argc >= 7, bad = 0, badcore = 0;
        struct Hashes *got = (struct Hashes *)calloc((size_t)n, sizeof *got);
        double psecs;
        run_seq(seed, version, jobs, n, ref, &secs);
        psecs = run_par(seed, version, jobs, n, threads, shuffle, got);
        for (i = 0; i < n; i++) {
            bad += got[i].raw != ref[i].raw || got[i].rc != ref[i].rc;
            badcore += got[i].core != ref[i].core;
        }
        printf("par: v%d seed %llu %d chunks; seq %.3f s (%.0f/s); %d threads%s %.3f s (%.0f/s) speed-up %.2fx; "
               "raw mismatches %d, core mismatches %d\n", version, (unsigned long long)seed, n, secs, n / secs,
               threads, shuffle ? " shuffled" : "", psecs, n / psecs, secs / psecs, bad, badcore);
        return bad != 0 || badcore != 0;
    }
#endif
    fprintf(stderr, "unknown mode\n");
    return 2;
}

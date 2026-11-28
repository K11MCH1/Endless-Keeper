/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_genahead.c
 *     Endless world: chunks generated ahead on a worker thread (streaming study 2026-11-28; on by default,
 *     KFX_WORLD_GENAHEAD=0 turns it off).
 * @par Why it keeps the game the same:
 *     The generator's answer for a chunk depends only on the seed, its version and the chunk (tests/world_gen/
 *     order_test.c: two worlds, any order, byte-identical), so a second generator on a worker gives the very bytes the
 *     game's own would. The game asks the worker ahead (the chunk manager's next candidates, a raid's search box) and,
 *     when it needs a chunk, takes the worker's copy if it is there, else generates it itself as before. What the
 *     game does never depends on whether the copy was ready: only how long it took. Replays, servers and joiners see
 *     the same world.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_genahead.h"
#include "globals.h"

#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define WGA_QUEUE 4096
/* slabs + owners, ~530 B each: 4.3 MB in a 32-bit game, 35 MB on a 64-bit one (a server with 100 awake keepers asks
   for some 15,000 chunks of raid search boxes) */
#define WGA_PEEK_SLOTS ((sizeof(void *) >= 8) ? 65536 : 8192)
#define WGA_FULL_SLOTS 64   /* whole chunks (~17 KB each) for the chunk manager's next loads */
#define WGA_PROBE 16

enum { WgaFree = 0, WgaPending, WgaReady };

struct WgaPeek {
    int32_t cx, cy;
    uint8_t state;
    uint32_t age;
    uint8_t slabs[WG_CHUNK_CELLS];
    uint8_t owners[WG_CHUNK_CELLS];
};
struct WgaFull {
    int32_t cx, cy;
    uint8_t state;
    uint32_t age;
    struct WgChunk ch;
};
struct WgaJob {
    int32_t cx, cy;
    uint8_t full;
};

static int wga_on = -1;
static SDL_Thread *wga_thread;
static SDL_Mutex *wga_lock;
static SDL_Condition *wga_cond;
static volatile int wga_quit;
static struct WgWorld *wga_gen;
static struct WgaJob wga_q[WGA_QUEUE];
static uint32_t wga_qhead, wga_qtail; /* (tail - head) jobs waiting */
static struct WgaPeek *wga_peeks;
static struct WgaFull *wga_fulls;
static uint32_t wga_clock;
static unsigned long st_requested, st_used, st_made, st_dropped;

TbBool wga_enabled(void)
{
    if (wga_on < 0)
    {
        const char *v = getenv("KFX_WORLD_GENAHEAD");
        wga_on = ((v == NULL) || (strcmp(v, "0") != 0)) ? 1 : 0;
    }
    return wga_on > 0;
}

static uint32_t wga_hash(int32_t cx, int32_t cy)
{
    const uint64_t key = ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy;
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32);
}

/** The slot of a chunk (state != WgaFree), or with `make` a slot for it (the oldest in its probe window). Locked. */
static struct WgaPeek *peek_slot(int32_t cx, int32_t cy, TbBool make)
{
    const uint32_t h = wga_hash(cx, cy);
    struct WgaPeek *oldest = NULL;
    for (int i = 0; i < WGA_PROBE; i++)
    {
        struct WgaPeek *p = &wga_peeks[(h + i) & (WGA_PEEK_SLOTS - 1)];
        if ((p->state != WgaFree) && (p->cx == cx) && (p->cy == cy))
            return p;
        if (p->state == WgaFree)
        {
            if ((oldest == NULL) || (oldest->state != WgaFree))
                oldest = p; /* the first free one */
        } else if ((oldest == NULL) || ((oldest->state != WgaFree) && (p->age < oldest->age)))
            oldest = p;     /* else the oldest */
    }
    if (!make)
        return NULL;
    oldest->cx = cx;
    oldest->cy = cy;
    oldest->state = WgaFree;
    oldest->age = ++wga_clock;
    return oldest;
}

static struct WgaFull *full_slot(int32_t cx, int32_t cy, TbBool make)
{
    struct WgaFull *oldest = NULL;
    for (int i = 0; i < WGA_FULL_SLOTS; i++)
    {
        struct WgaFull *f = &wga_fulls[i];
        if ((f->state != WgaFree) && (f->cx == cx) && (f->cy == cy))
            return f;
        if (f->state == WgaFree)
        {
            if ((oldest == NULL) || (oldest->state != WgaFree))
                oldest = f;
        } else if ((oldest == NULL) || ((oldest->state != WgaFree) && (f->age < oldest->age)))
            oldest = f;
    }
    if (!make)
        return NULL;
    oldest->cx = cx;
    oldest->cy = cy;
    oldest->state = WgaFree;
    oldest->age = ++wga_clock;
    return oldest;
}

static int wga_worker(void *data)
{
    (void)data;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    struct WgChunk *ch = (struct WgChunk *)malloc(sizeof(struct WgChunk));
    if (ch == NULL)
        return 0;
    SDL_LockMutex(wga_lock);
    while (!wga_quit)
    {
        if (wga_qhead == wga_qtail)
        {
            SDL_WaitCondition(wga_cond, wga_lock);
            continue;
        }
        const struct WgaJob job = wga_q[wga_qhead % WGA_QUEUE];
        wga_qhead++;
        /* still wanted? (a slot taken over by another chunk meanwhile: skip it) */
        TbBool wanted = job.full ? (full_slot(job.cx, job.cy, false) != NULL) : (peek_slot(job.cx, job.cy, false) != NULL);
        if (!wanted)
            continue;
        SDL_UnlockMutex(wga_lock);
        const int err = wg_generate_chunk(wga_gen, job.cx, job.cy, ch);
        SDL_LockMutex(wga_lock);
        st_made++;
        if (err != 0)
            continue; /* (the game generates it itself and logs the error there) */
        struct WgaPeek *p = peek_slot(job.cx, job.cy, false);
        if ((p != NULL) && (p->state == WgaPending))
        {
            memcpy(p->slabs, ch->slabs, WG_CHUNK_CELLS);
            memcpy(p->owners, ch->owners, WG_CHUNK_CELLS);
            p->state = WgaReady;
        }
        struct WgaFull *f = full_slot(job.cx, job.cy, false);
        if ((f != NULL) && (f->state == WgaPending))
        {
            memcpy(&f->ch, ch, sizeof(*ch));
            f->state = WgaReady;
        }
    }
    SDL_UnlockMutex(wga_lock);
    free(ch);
    return 0;
}

void wga_start(uint64_t seed, int version, int rival_min_dist)
{
    wga_stop();
    if (!wga_enabled())
        return;
    wga_peeks = (struct WgaPeek *)calloc(WGA_PEEK_SLOTS, sizeof(struct WgaPeek));
    wga_fulls = (struct WgaFull *)calloc(WGA_FULL_SLOTS, sizeof(struct WgaFull));
    wga_gen = wg_world_create_version(seed, version);
    wga_lock = SDL_CreateMutex();
    wga_cond = SDL_CreateCondition();
    if ((wga_peeks == NULL) || (wga_fulls == NULL) || (wga_gen == NULL) || (wga_lock == NULL) || (wga_cond == NULL))
    {
        wga_stop();
        return;
    }
    if (rival_min_dist > 0)
        wg_world_set_rival_min_dist(wga_gen, rival_min_dist);
    wga_quit = 0;
    wga_qhead = wga_qtail = 0;
    wga_thread = SDL_CreateThread(wga_worker, "genahead", NULL);
    if (wga_thread == NULL)
        wga_stop();
    else
        SYNCLOG("World: chunks are generated ahead on a worker (KFX_WORLD_GENAHEAD)");
}

void wga_stop(void)
{
    if (wga_thread != NULL)
    {
        SDL_LockMutex(wga_lock);
        wga_quit = 1;
        SDL_SignalCondition(wga_cond);
        SDL_UnlockMutex(wga_lock);
        SDL_WaitThread(wga_thread, NULL);
        wga_thread = NULL;
    }
    if (wga_cond != NULL)
        SDL_DestroyCondition(wga_cond);
    if (wga_lock != NULL)
        SDL_DestroyMutex(wga_lock);
    wga_cond = NULL;
    wga_lock = NULL;
    if (wga_gen != NULL)
        wg_world_destroy(wga_gen);
    wga_gen = NULL;
    free(wga_peeks);
    free(wga_fulls);
    wga_peeks = NULL;
    wga_fulls = NULL;
}

void wga_request(int32_t cx, int32_t cy, TbBool full)
{
    if (wga_thread == NULL)
        return;
    SDL_LockMutex(wga_lock);
    TbBool queue = false;
    if (full)
    {
        if (full_slot(cx, cy, false) == NULL)
        {
            struct WgaFull *f = full_slot(cx, cy, true);
            f->state = WgaPending;
            queue = true;
        }
    } else if (peek_slot(cx, cy, false) == NULL)
    {
        struct WgaPeek *p = peek_slot(cx, cy, true);
        p->state = WgaPending;
        queue = true;
    }
    if (queue)
    {
        if (wga_qtail - wga_qhead >= WGA_QUEUE)
        {
            st_dropped++;
            /* (the slot stays pending and is taken over later; the game generates the chunk itself) */
        } else
        {
            wga_q[wga_qtail % WGA_QUEUE].cx = cx;
            wga_q[wga_qtail % WGA_QUEUE].cy = cy;
            wga_q[wga_qtail % WGA_QUEUE].full = full ? 1 : 0;
            wga_qtail++;
            st_requested++;
            SDL_SignalCondition(wga_cond);
        }
    }
    SDL_UnlockMutex(wga_lock);
}

TbBool wga_take_full(int32_t cx, int32_t cy, struct WgChunk *out)
{
    if (wga_thread == NULL)
        return false;
    TbBool got = false;
    SDL_LockMutex(wga_lock);
    struct WgaFull *f = full_slot(cx, cy, false);
    if ((f != NULL) && (f->state == WgaReady))
    {
        memcpy(out, &f->ch, sizeof(*out));
        f->state = WgaFree;
        got = true;
        st_used++;
    }
    SDL_UnlockMutex(wga_lock);
    return got;
}

TbBool wga_peek(int32_t cx, int32_t cy, uint8_t *slabs, uint8_t *owners)
{
    if (wga_thread == NULL)
        return false;
    TbBool got = false;
    SDL_LockMutex(wga_lock);
    struct WgaPeek *p = peek_slot(cx, cy, false);
    if ((p != NULL) && (p->state == WgaReady))
    {
        memcpy(slabs, p->slabs, WG_CHUNK_CELLS);
        memcpy(owners, p->owners, WG_CHUNK_CELLS);
        got = true;
    } else
    {
        struct WgaFull *f = full_slot(cx, cy, false);
        if ((f != NULL) && (f->state == WgaReady))
        {
            memcpy(slabs, f->ch.slabs, WG_CHUNK_CELLS);
            memcpy(owners, f->ch.owners, WG_CHUNK_CELLS);
            got = true;
        }
    }
    if (got)
        st_used++;
    SDL_UnlockMutex(wga_lock);
    return got;
}

void wga_stats(unsigned long *requested, unsigned long *used, unsigned long *made, unsigned long *dropped)
{
    *requested = st_requested;
    *used = st_used;
    *made = st_made;
    *dropped = st_dropped;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

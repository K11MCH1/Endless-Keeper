/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_kscm.c
 *     KSC-M: a thin-client keyframe coded by the shape of the game's memory (research prototype 2026-11-28).
 * @par
 *     A keyframe is the whole state, ~140 MB (struct Game, the statics, the pages), 98% zero bytes. KSC-M walks it
 *     with the build's own save tables (struct Game, Map, SlabMap: each field's offset, size, count, nested struct):
 *       - every field gets its own stream (all things' models together, all map blocks' flags ...); a union is kept
 *         as raw bytes; bytes no field covers (padding, runtime fields) go to a gap stream of their struct, so the
 *         record comes back exactly;
 *       - in an array of structs an element that is all zero is left out and marked in a presence bitmap;
 *       - 2-, 4- and 8-byte integer streams are split into byte planes;
 *       - the streams, back to back, are one zstd frame (level 1).
 *     Measured on four real keyframes: 110-208 KB, against 177-294 KB for zstd and 404-564 KB for zlib (today).
 *     Server and client must have the same layout: the body carries a hash of the tables, a client with another
 *     refuses it (and stops asking for KSC-M).
 * @par Body
 *     "KSM1", u64 layout hash, u32 record length, u32 stream count, per stream a varint length and, when not 0, a u8
 *     transform (bit 0: byte planes), then one zstd frame holding the streams.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "net_kscm.h"

#include <stdlib.h>
#include <string.h>
#include "kfx/save/core/save_schema.h"
#include "kfx/save/core/schema/save_tables_decl.h"
#include "../deps/zstd/zstd.h"
#define XXH_INLINE_ALL
#include "../deps/xxhash/xxhash.h"
#include "post_inc.h"

/******************************************************************************/
#define KSCM_MAX_STRUCTS 512
#define KSCM_MAX_DEPTH 12
#define KSCM_MAX_RAW (256u << 20)

enum { KF_RAW = 0, KF_SCALAR, KF_STRUCT };
enum { KX_STAT, KX_LIGHT, KX_EXT, KX_OTHER, KX_FRAME, KX_N };

struct KscmField {
    uint32_t off, size, count;
    uint8_t kind;
    int sub;               /* KF_STRUCT: the struct's index */
    int stream;            /* data (scalar, raw) or presence (struct array) */
};
struct KscmStruct {
    const struct SaveStructDesc *desc;
    uint32_t size;
    int nf;
    struct KscmField *f;   /* sorted by offset */
    int gap;               /* its gap stream */
};

static struct {
    int ready;
    struct KscmFrame fr;
    int ns;
    struct KscmStruct s[KSCM_MAX_STRUCTS];
    int nst;
    uint8_t *esize;        /* per stream */
    int game, map, slab, x;/* struct indices; x: first extra stream */
    uint64_t hash;
} K;

static int kscm_find(const struct SaveStructDesc *d)
{
    for (int i = 0; i < K.ns; i++)
        if (K.s[i].desc == d)
            return i;
    return -1;
}

static int kscm_add(const struct SaveStructDesc *d)
{
    int i = kscm_find(d);
    if (i >= 0)
        return i;
    if (K.ns >= KSCM_MAX_STRUCTS)
        return -1;
    i = K.ns++;
    K.s[i].desc = d;
    for (uint32_t k = 0; k < d->field_count; k++)
        if ((d->fields[k].sub != NULL) && (kscm_add(d->fields[k].sub) < 0))
            return -1;
    return i;
}

static int cmp_field(const void *a, const void *b)
{
    const struct KscmField *x = (const struct KscmField *)a, *y = (const struct KscmField *)b;
    return (x->off > y->off) - (x->off < y->off);
}

static void hash_u32(XXH3_state_t *h, uint32_t v) { XXH3_64bits_update(h, &v, 4); }

int kscm_init(const struct KscmFrame *frame)
{
    if (K.ready)
        return 1;
    memset(&K, 0, sizeof(K));
    K.fr = *frame;
    if ((kscm_add(&save_desc_Game) < 0) || (kscm_add(&save_desc_Map) < 0) || (kscm_add(&save_desc_SlabMap) < 0))
        return 0;
    K.game = kscm_find(&save_desc_Game);
    K.map = kscm_find(&save_desc_Map);
    K.slab = kscm_find(&save_desc_SlabMap);
    XXH3_state_t *h = XXH3_createState();
    if (h == NULL)
        return 0;
    XXH3_64bits_reset(h);
    /* the frame field by field, not its bytes: struct KscmFrame has a padding byte after region_game, and the caller's
       initialised automatic struct leaves it as the stack had it - two identical builds could hash differently and
       refuse each other's KSC-M keyframes (audit 2026-11-28) */
    hash_u32(h, frame->header_len);
    hash_u32(h, frame->kind_memfull);
    hash_u32(h, frame->kind_pageadd);
    hash_u32(h, frame->region_game);
    hash_u32(h, frame->page_map);
    hash_u32(h, frame->page_stat);
    hash_u32(h, frame->page_light);
    hash_u32(h, frame->page_slab);
    hash_u32(h, frame->page_ext);
    int nst = 0;
    for (int i = 0; i < K.ns; i++)
    {
        struct KscmStruct *s = &K.s[i];
        const struct SaveStructDesc *d = s->desc;
        s->size = d->size;
        s->nf = (int)d->field_count;
        s->f = (struct KscmField *)calloc((size_t)s->nf + 1, sizeof(struct KscmField));
        if (s->f == NULL)
        {
            XXH3_freeState(h);
            return 0;
        }
        XXH3_64bits_update(h, d->name, strlen(d->name));
        hash_u32(h, d->size);
        for (int k = 0; k < s->nf; k++)
        {
            const struct SaveFieldDesc *fd = &d->fields[k];
            struct KscmField *f = &s->f[k];
            uint32_t count = 1;
            for (int dd = 0; (dd < 3) && (fd->dims[dd] != 0); dd++)
                count *= fd->dims[dd];
            f->off = fd->offset;
            f->size = fd->mem_size;
            f->count = count;
            f->sub = (fd->sub != NULL) ? kscm_find(fd->sub) : -1;
            if ((f->sub >= 0) && (fd->uni == NULL) && (count > 0) && ((uint64_t)fd->sub->size * count == fd->mem_size))
                f->kind = KF_STRUCT;
            else if ((fd->uni == NULL) && (fd->sub == NULL))
                f->kind = KF_SCALAR;
            else
                f->kind = KF_RAW;
            XXH3_64bits_update(h, fd->name, strlen(fd->name));
            hash_u32(h, f->off); hash_u32(h, f->size); hash_u32(h, f->count); hash_u32(h, f->kind);
            hash_u32(h, (uint32_t)f->sub); hash_u32(h, fd->stored_type);
        }
        qsort(s->f, (size_t)s->nf, sizeof(struct KscmField), cmp_field);
        for (int k = 0; k < s->nf; k++)
            s->f[k].stream = nst++;
        s->gap = nst++;
    }
    K.x = nst;
    nst += KX_N;
    K.nst = nst;
    K.hash = XXH3_64bits_digest(h);
    XXH3_freeState(h);
    K.esize = (uint8_t *)calloc((size_t)nst, 1);
    if (K.esize == NULL)
        return 0;
    memset(K.esize, 1, (size_t)nst);
    for (int i = 0; i < K.ns; i++)
        for (int k = 0; k < K.s[i].nf; k++)
        {
            const struct KscmField *f = &K.s[i].f[k];
            const struct SaveFieldDesc *fd = NULL;
            for (uint32_t q = 0; q < K.s[i].desc->field_count; q++)
                if (K.s[i].desc->fields[q].offset == f->off && K.s[i].desc->fields[q].mem_size == f->size)
                    fd = &K.s[i].desc->fields[q];
            const uint32_t es = (f->count > 0) ? f->size / f->count : 1;
            if ((f->kind == KF_SCALAR) && (fd != NULL) && (fd->stored_type >= SV_U16) && (fd->stored_type <= SV_I64) &&
                ((es == 2) || (es == 4) || (es == 8)))
                K.esize[f->stream] = (uint8_t)es;
        }
    K.esize[K.x + KX_STAT] = 2;
    K.esize[K.x + KX_LIGHT] = 2;
    K.ready = 1;
    return 1;
}

/******************************************************************************/
struct KStream {
    uint8_t *p;
    size_t n, cap, rpos;
    uint8_t bits;
};
struct KCoder {
    struct KStream *st;
    int decode;
    int bad;
};

static void ks_put(struct KCoder *c, struct KStream *s, const uint8_t *p, size_t n)
{
    if (c->bad || (n == 0))
        return;
    if (s->n + n > s->cap)
    {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->n + n)
            cap *= 2;
        uint8_t *np = (uint8_t *)realloc(s->p, cap);
        if (np == NULL)
        {
            c->bad = 1;
            return;
        }
        s->p = np;
        s->cap = cap;
    }
    memcpy(s->p + s->n, p, n);
    s->n += n;
}
static void ks_take(struct KCoder *c, struct KStream *s, uint8_t *d, size_t n)
{
    if (c->bad || (s->n - s->rpos < n))
    {
        c->bad = 1;
        return;
    }
    memcpy(d, s->p + s->rpos, n);
    s->rpos += n;
}
/** Encode: the bytes into the stream; decode: the stream's bytes back. */
static void ks_move(struct KCoder *c, struct KStream *s, uint8_t *p, size_t n)
{
    if (c->decode)
        ks_take(c, s, p, n);
    else
        ks_put(c, s, p, n);
}
static int ks_zero(const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t a, b, x, y;
        memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); memcpy(&x, p + i + 16, 8); memcpy(&y, p + i + 24, 8);
        if (a | b | x | y)
            return 0;
    }
    for (; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}
static int ks_presence(struct KCoder *c, struct KStream *s, int bit)
{
    if (!c->decode)
    {
        if ((s->bits == 0) || (s->bits == 8))
        {
            const uint8_t z = 0;
            ks_put(c, s, &z, 1);
            s->bits = 0;
        }
        if (bit && !c->bad)
            s->p[s->n - 1] |= (uint8_t)(1u << s->bits);
        s->bits++;
        return bit;
    }
    const size_t byte = s->rpos >> 3;
    if (byte >= s->n)
    {
        c->bad = 1;
        return 0;
    }
    const int b = (s->p[byte] >> (s->rpos & 7)) & 1;
    s->rpos++;
    return b;
}

/** One struct at p (the caller made sure all its bytes are inside the record). */
static void ks_walk(struct KCoder *c, int si, uint8_t *p, int depth)
{
    const struct KscmStruct *s = &K.s[si];
    struct KStream *gap = &c->st[s->gap];
    uint32_t cur = 0;
    for (int k = 0; (k < s->nf) && !c->bad; k++)
    {
        const struct KscmField *f = &s->f[k];
        uint32_t end = f->off + f->size;
        if (end > s->size)
            end = s->size;
        if (end <= cur)
            continue;
        const uint32_t start = (f->off > cur) ? f->off : cur;
        if (start > cur)
            ks_move(c, gap, p + cur, start - cur);
        if ((start != f->off) || (end != f->off + f->size))
        {
            ks_move(c, gap, p + start, end - start); /* (overlaps another field, or runs past the struct) */
            cur = end;
            continue;
        }
        struct KStream *d = &c->st[f->stream];
        if ((f->kind == KF_STRUCT) && (depth < KSCM_MAX_DEPTH))
        {
            const uint32_t es = K.s[f->sub].size;
            if (f->count == 1)
                ks_walk(c, f->sub, p + f->off, depth + 1);
            else
                for (uint32_t e = 0; (e < f->count) && !c->bad; e++)
                {
                    uint8_t *q = p + f->off + (size_t)e * es;
                    if (ks_presence(c, d, c->decode ? 0 : !ks_zero(q, es)))
                        ks_walk(c, f->sub, q, depth + 1);
                    else if (c->decode)
                        memset(q, 0, es);
                }
        } else
            ks_move(c, d, p + f->off, f->size);
        cur = end;
    }
    if ((cur < s->size) && !c->bad)
        ks_move(c, gap, p + cur, s->size - cur);
}

/** The record: header, MemFull of the game (walked), other MemFull, PageAdd (walked), and the rest as it is. */
static void ks_record(struct KCoder *c, uint8_t *rec, size_t len)
{
    struct KStream *fr = &c->st[K.x + KX_FRAME], *other = &c->st[K.x + KX_OTHER];
    const uint32_t page = K.fr.page_map + K.fr.page_stat + K.fr.page_light + K.fr.page_slab + 2 * K.fr.page_ext;
    const uint32_t cells = K.fr.page_map / (K.s[K.map].size ? K.s[K.map].size : 1);
    const uint32_t scells = K.fr.page_slab / (K.s[K.slab].size ? K.s[K.slab].size : 1);
    size_t o = 0;
    if (len < K.fr.header_len)
    {
        c->bad = 1;
        return;
    }
    ks_move(c, fr, rec, K.fr.header_len);
    o = K.fr.header_len;
    while ((o < len) && !c->bad)
    {
        ks_move(c, fr, rec + o, 1);
        const uint8_t kind = rec[o++];
        if ((kind == K.fr.kind_memfull) && (len - o >= 5))
        {
            ks_move(c, fr, rec + o, 5);
            if (c->bad)
                break;
            const uint8_t region = rec[o];
            uint32_t n;
            memcpy(&n, rec + o + 1, 4);
            o += 5;
            if (n > len - o)
            {
                c->bad = 1;
                break;
            }
            if ((region == K.fr.region_game) && (n == K.s[K.game].size))
                ks_walk(c, K.game, rec + o, 0);
            else
                ks_move(c, other, rec + o, n);
            o += n;
        } else
        if ((kind == K.fr.kind_pageadd) && (len - o >= 8 + (size_t)page) && (cells * K.s[K.map].size == K.fr.page_map) &&
            (scells * K.s[K.slab].size == K.fr.page_slab))
        {
            ks_move(c, fr, rec + o, 8);
            o += 8;
            for (uint32_t i = 0; (i < cells) && !c->bad; i++)
                ks_walk(c, K.map, rec + o + (size_t)i * K.s[K.map].size, 0);
            o += K.fr.page_map;
            ks_move(c, &c->st[K.x + KX_STAT], rec + o, K.fr.page_stat);
            o += K.fr.page_stat;
            ks_move(c, &c->st[K.x + KX_LIGHT], rec + o, K.fr.page_light);
            o += K.fr.page_light;
            for (uint32_t i = 0; (i < scells) && !c->bad; i++)
                ks_walk(c, K.slab, rec + o + (size_t)i * K.s[K.slab].size, 0);
            o += K.fr.page_slab;
            ks_move(c, &c->st[K.x + KX_EXT], rec + o, 2 * (size_t)K.fr.page_ext);
            o += 2 * (size_t)K.fr.page_ext;
        } else
        {
            ks_move(c, other, rec + o, len - o); /* (anything else: kept as it is) */
            o = len;
        }
    }
}

static void ks_planes(uint8_t *p, size_t n, int es, int undo, uint8_t *t)
{
    const size_t m = n / (size_t)es;
    if (!undo)
        for (size_t i = 0; i < m; i++)
            for (int b = 0; b < es; b++)
                t[(size_t)b * m + i] = p[i * (size_t)es + (size_t)b];
    else
        for (size_t i = 0; i < m; i++)
            for (int b = 0; b < es; b++)
                t[i * (size_t)es + (size_t)b] = p[(size_t)b * m + i];
    memcpy(p, t, m * (size_t)es);
}

static void ks_free(struct KCoder *c)
{
    for (int i = 0; i < K.nst; i++)
        free(c->st[i].p);
    free(c->st);
}

static size_t put_varint(uint8_t *o, uint64_t v)
{
    size_t n = 0;
    while (v >= 0x80)
    {
        o[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    o[n++] = (uint8_t)v;
    return n;
}

int kscm_encode(const uint8_t *rec, size_t len, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!K.ready || (len > KSCM_MAX_RAW))
        return 0;
    struct KCoder c;
    memset(&c, 0, sizeof(c));
    c.st = (struct KStream *)calloc((size_t)K.nst, sizeof(struct KStream));
    if (c.st == NULL)
        return 0;
    ks_record(&c, (uint8_t *)rec, len); /* (encoding only reads it) */
    size_t total = 0, maxs = 0;
    for (int i = 0; i < K.nst; i++)
    {
        total += c.st[i].n;
        if (c.st[i].n > maxs)
            maxs = c.st[i].n;
    }
    const size_t head_cap = 20 + (size_t)K.nst * 11;
    uint8_t *cat = (uint8_t *)malloc(total + 1), *tmp = (uint8_t *)malloc(maxs + 1), *head = (uint8_t *)malloc(head_cap);
    if (c.bad || (cat == NULL) || (tmp == NULL) || (head == NULL))
    {
        free(cat); free(tmp); free(head);
        ks_free(&c);
        return 0;
    }
    size_t h = 0, at = 0;
    memcpy(head, "KSM1", 4);
    memcpy(head + 4, &K.hash, 8);
    const uint32_t l32 = (uint32_t)len, n32 = (uint32_t)K.nst;
    memcpy(head + 12, &l32, 4);
    memcpy(head + 16, &n32, 4);
    h = 20;
    for (int i = 0; i < K.nst; i++)
    {
        struct KStream *s = &c.st[i];
        h += put_varint(head + h, s->n);
        if (s->n == 0)
            continue;
        uint8_t x = 0;
        const int es = K.esize[i];
        if ((es > 1) && (s->n >= 16) && (s->n % (size_t)es == 0) && !ks_zero(s->p, s->n))
        {
            ks_planes(s->p, s->n, es, 0, tmp);
            x = 1;
        }
        head[h++] = x;
        memcpy(cat + at, s->p, s->n);
        at += s->n;
    }
    free(tmp);
    ks_free(&c);
    const size_t zcap = ZSTD_compressBound(total);
    uint8_t *o = (uint8_t *)malloc(h + zcap);
    if (o == NULL)
    {
        free(cat); free(head);
        return 0;
    }
    memcpy(o, head, h);
    free(head);
    const size_t zl = ZSTD_compress(o + h, zcap, cat, total, 1);
    free(cat);
    if (ZSTD_isError(zl))
    {
        free(o);
        return 0;
    }
    *out = o;
    *out_len = h + zl;
    return 1;
}

int kscm_decode(const uint8_t *in, size_t in_len, uint8_t *rec, size_t len)
{
    if (!K.ready || (in_len < 20) || (memcmp(in, "KSM1", 4) != 0))
        return 0;
    uint64_t hash;
    uint32_t l32, n32;
    memcpy(&hash, in + 4, 8);
    memcpy(&l32, in + 12, 4);
    memcpy(&n32, in + 16, 4);
    if ((hash != K.hash) || (l32 != len) || (n32 != (uint32_t)K.nst))
        return 0;
    size_t pos = 20;
    struct KCoder c;
    memset(&c, 0, sizeof(c));
    c.decode = 1;
    c.st = (struct KStream *)calloc((size_t)K.nst, sizeof(struct KStream));
    uint8_t *xf = (uint8_t *)calloc((size_t)K.nst + 1, 1);
    if ((c.st == NULL) || (xf == NULL))
    {
        free(c.st);
        free(xf);
        return 0;
    }
    uint64_t total = 0;
    for (int i = 0; (i < K.nst) && !c.bad; i++)
    {
        uint64_t v = 0;
        int sh = 0;
        for (;;)
        {
            if ((pos >= in_len) || (sh > 56))
            {
                c.bad = 1;
                break;
            }
            const uint8_t b = in[pos++];
            v |= (uint64_t)(b & 0x7F) << sh;
            if (!(b & 0x80))
                break;
            sh += 7;
        }
        c.st[i].n = (size_t)v;
        total += v;
        if (total > KSCM_MAX_RAW)
            c.bad = 1;
        if ((v > 0) && !c.bad)
        {
            if (pos >= in_len)
                c.bad = 1;
            else
                xf[i] = in[pos++];
        }
    }
    uint8_t *cat = c.bad ? NULL : (uint8_t *)malloc((size_t)total + 1);
    if (cat == NULL)
    {
        free(c.st);
        free(xf);
        return 0;
    }
    const size_t got = ZSTD_decompress(cat, (size_t)total, in + pos, in_len - pos);
    int ok = !ZSTD_isError(got) && (got == total);
    size_t maxs = 0;
    for (int i = 0; i < K.nst; i++)
        if (c.st[i].n > maxs)
            maxs = c.st[i].n;
    uint8_t *tmp = ok ? (uint8_t *)malloc(maxs + 1) : NULL;
    ok = ok && (tmp != NULL);
    size_t at = 0;
    for (int i = 0; (i < K.nst) && ok; i++)
    {
        struct KStream *s = &c.st[i];
        s->p = cat + at; /* (read in place) */
        at += s->n;
        const int es = K.esize[i];
        if ((xf[i] & 1) && (es > 1) && (s->n % (size_t)es == 0))
            ks_planes(s->p, s->n, es, 1, tmp);
    }
    free(tmp);
    if (ok)
    {
        ks_record(&c, rec, len);
        ok = !c.bad;
    }
    free(cat);
    free(c.st);
    free(xf);
    return ok;
}

void kscm_test_spoil_layout(void)
{
    K.hash ^= 0x5A5A5A5A5A5A5A5Aull;
}

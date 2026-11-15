/******************************************************************************/
/** @file world_gen8.inc
 *     Generator version 8 (2026-11-13), included by world_gen.c (it uses its internals; versions 6 and 7's functions
 *     are not touched). worldgen.py's version 8 part, to the bit.
 *
 *     Mother lodes and trails (roadmap "Mother Lodes and Trails"): version 7's ground, with
 *       - gold near home scarcer: a gold square (SEAM_CELL) is kept with a chance that grows with its distance from the
 *         start, a quarter at the start, all of it from TRAIL_THIN_FULL slabs out;
 *       - trails: in each TRAIL_CELL square (and from the start) a seeded trail may run: lumps of gold (3x3) every ~10
 *         slabs, joined by a vein of earth through rock, turning a little at each lump, 8 to 12 lumps long (about a
 *         hundred slabs), ending in a mother lode: a disc of gold and dense gold, larger and denser the farther out.
 *     Integer arithmetic only. Never in Dead Man's Land, never over lava or water.
 */
/******************************************************************************/
#define TRAIL_CELL 160
#define TRAIL_CHANCE 45
#define TRAIL_LUMPS_MIN 8
#define TRAIL_LUMPS_SPAN 5
#define TRAIL_LUMPS_MAX (TRAIL_LUMPS_MIN + TRAIL_LUMPS_SPAN - 1)
#define TRAIL_THIN_FULL 400
#define TRAIL_START_GAP 14
#define TRAIL_CACHE 1024 /* (the ten round a square must not keep pushing each other out) */
enum { TRAIL_NONE = 0, TRAIL_VEIN = 1, TRAIL_LUMP = 2, TRAIL_LODE_GOLD = 3, TRAIL_LODE_DENSE = 4 };
static const int trail_dirs[16][2] = {
    {10, 0}, {9, 4}, {7, 7}, {4, 9}, {0, 10}, {-4, 9}, {-7, 7}, {-9, 4},
    {-10, 0}, {-9, -4}, {-7, -7}, {-4, -9}, {0, -10}, {4, -9}, {7, -7}, {9, -4},
};

/** One trail: its cells on a grid over its box (TRAIL_*), and its mother lode. */
struct Trail8 {
    int valid, start, has;           /* start: the start's own trail (tx, ty unused); has: there is one */
    int64_t tx, ty;
    int64_t x1, y1;
    int w, h;
    uint8_t *grid;
    int64_t lx, ly;
    int lr;
};

struct Trail8Cache {
    struct Trail8 t[TRAIL_CACHE];
    int near_valid;                  /* the trails round the trail square asked last (a chunk's slabs share it) */
    int64_t near_tx, near_ty;
    const struct Trail8 *near[10];
    struct { int64_t cx, cy; int8_t kept; } kept[512]; /* seam_kept8's answers (kept -1: empty) */
    int og_valid, og;                /* the Overgrowth's noise: the last 4x4 block asked */
    int64_t og_bx, og_by;
};

static uint64_t isqrt8(uint64_t n)
{
    uint64_t r, nr;
    if (n == 0)
        return 0;
    r = (uint64_t)sqrt((double)n);
    /* exact (math.isqrt): the largest r with r*r <= n */
    while (r > 0 && r * r > n)
        r--;
    for (;;) {
        nr = r + 1;
        if (nr * nr > n)
            break;
        r = nr;
    }
    return r;
}

static void trail8_build(const struct WgWorld *w, struct Trail8 *t, int64_t sx, int64_t sy, int d, int n, uint64_t h)
{
    int64_t px[TRAIL_LUMPS_MAX], py[TRAIL_LUMPS_MAX];
    int64_t x = sx, y = sy, x1, y1, x2, y2, dist, dx, dy;
    int i, k, r, dense;
    px[0] = sx;
    py[0] = sy;
    for (i = 1; i < n; i++) {
        h = mix64(h + WG_GOLDEN);
        d = (d + (int)(h % 3) - 1 + 16) % 16;
        x += trail_dirs[d][0];
        y += trail_dirs[d][1];
        px[i] = x;
        py[i] = y;
    }
    dx = px[n - 1] - START_X;
    dy = py[n - 1] - START_Y;
    dist = (int64_t)isqrt8((uint64_t)(dx * dx + dy * dy));
    r = 3 + (int)(dist / 300 < 3 ? dist / 300 : 3);
    dense = 20 + (int)(dist / 25 < 40 ? dist / 25 : 40);
    x1 = x2 = px[0];
    y1 = y2 = py[0];
    for (i = 0; i < n; i++) {
        if (px[i] < x1) x1 = px[i];
        if (px[i] > x2) x2 = px[i];
        if (py[i] < y1) y1 = py[i];
        if (py[i] > y2) y2 = py[i];
    }
    x1 -= r; y1 -= r; x2 += r; y2 += r;
    t->x1 = x1;
    t->y1 = y1;
    t->w = (int)(x2 - x1 + 1);
    t->h = (int)(y2 - y1 + 1);
    free(t->grid);
    t->grid = (uint8_t *)calloc((size_t)t->w * (size_t)t->h, 1);
    if (t->grid == NULL) {
        t->has = 0;
        return;
    }
#define T8(xx, yy) t->grid[((yy) - t->y1) * t->w + ((xx) - t->x1)]
    for (i = 0; i < n - 1; i++) {
        const int64_t ax = px[i], ay = py[i], bx = px[i + 1], by = py[i + 1];
        const int64_t ddx = bx - ax, ddy = by - ay;
        const int64_t m = (ddx < 0 ? -ddx : ddx) > (ddy < 0 ? -ddy : ddy) ? (ddx < 0 ? -ddx : ddx) : (ddy < 0 ? -ddy : ddy);
        int ox, oy;
        for (k = 0; k <= m; k++) {
            const int64_t qx = ax + fdiv(2 * k * ddx + m, 2 * m), qy = ay + fdiv(2 * k * ddy + m, 2 * m);
            if (T8(qx, qy) == TRAIL_NONE)
                T8(qx, qy) = TRAIL_VEIN;
        }
        for (oy = -1; oy <= 1; oy++)
            for (ox = -1; ox <= 1; ox++)
                T8(ax + ox, ay + oy) = TRAIL_LUMP;
    }
    {
        const int64_t lx = px[n - 1], ly = py[n - 1];
        int ox, oy;
        for (oy = -r; oy <= r; oy++)
            for (ox = -r; ox <= r; ox++) {
                if (ox * ox + oy * oy > r * r)
                    continue;
                T8(lx + ox, ly + oy) = (hash3(w->s_trail, (uint64_t)(lx + ox), (uint64_t)(ly + oy)) % 100 < (uint64_t)dense) ?
                    TRAIL_LODE_DENSE : TRAIL_LODE_GOLD;
            }
        t->lx = lx;
        t->ly = ly;
        t->lr = r;
    }
#undef T8
    t->has = 1;
}

/** The trail of trail square (tx, ty) (start: the start's own), built once and kept. */
static const struct Trail8 *trail8_get(struct WgWorld *w, int start, int64_t tx, int64_t ty)
{
    uint32_t slot = start ? 0 : (uint32_t)((hash2((uint64_t)tx, (uint64_t)ty) % (TRAIL_CACHE - 1)) + 1);
    struct Trail8 *t;
    uint64_t h;
    int64_t sx, sy;
    int d, n;
    if (w->t8 == NULL) {
        int k;
        w->t8 = (struct Trail8Cache *)calloc(1, sizeof(struct Trail8Cache));
        if (w->t8 == NULL)
            return NULL;
        for (k = 0; k < 512; k++)
            w->t8->kept[k].kept = -1;
    }
    t = &w->t8->t[slot];
    if (t->valid && t->start == start && (start || (t->tx == tx && t->ty == ty)))
        return t;
    w->t8->near_valid = 0; /* (the kept list may name this slot) */
    t->valid = 1;
    t->start = start;
    t->tx = tx;
    t->ty = ty;
    t->has = 0;
    if (start) {
        h = hash2(w->s_trail, 0x5354415254ULL);
        d = (int)((h >> 24) % 16);
        sx = START_X + fdiv((int64_t)trail_dirs[d][0] * TRAIL_START_GAP, 10);
        sy = START_Y + fdiv((int64_t)trail_dirs[d][1] * TRAIL_START_GAP, 10);
    } else {
        h = hash3(w->s_trail, (uint64_t)tx, (uint64_t)ty);
        if (h % 100 >= TRAIL_CHANCE)
            return t;
        sx = tx * TRAIL_CELL + 20 + (int64_t)((h >> 8) % 120);
        sy = ty * TRAIL_CELL + 20 + (int64_t)((h >> 16) % 120);
        d = (int)((h >> 24) % 16);
    }
    n = TRAIL_LUMPS_MIN + (int)((h >> 32) % TRAIL_LUMPS_SPAN);
    trail8_build(w, t, sx, sy, d, n, h);
    return t;
}

static int trail8_kind(const struct Trail8 *t, int64_t x, int64_t y)
{
    if (t == NULL || !t->has || x < t->x1 || y < t->y1 || x >= t->x1 + t->w || y >= t->y1 + t->h)
        return TRAIL_NONE;
    return t->grid[(y - t->y1) * t->w + (x - t->x1)];
}

/** The trails' kind at a world slab: a mother lode over a lump over a vein. The ten trails round a trail square are
 *  kept for the next slab of the same square; building any trail again (a cache slot taken by another) drops them. */
static int trail8_at(struct WgWorld *w, int64_t x, int64_t y)
{
    const int64_t tx0 = fdiv(x, TRAIL_CELL), ty0 = fdiv(y, TRAIL_CELL);
    int best = 0, k, n = 0;
    int64_t tx, ty;
    const struct Trail8 *list[10];
    if (w->t8 != NULL && w->t8->near_valid && w->t8->near_tx == tx0 && w->t8->near_ty == ty0) {
        for (k = 0; k < 10; k++) {
            const int kk = trail8_kind(w->t8->near[k], x, y);
            if (kk > best)
                best = kk;
        }
        return best;
    }
    /* each one read as soon as it is got: a later one may take its cache slot */
    list[n] = trail8_get(w, 1, 0, 0);
    best = trail8_kind(list[n++], x, y);
    for (ty = ty0 - 1; ty <= ty0 + 1; ty++)
        for (tx = tx0 - 1; tx <= tx0 + 1; tx++) {
            int kk;
            list[n] = trail8_get(w, 0, tx, ty);
            kk = trail8_kind(list[n++], x, y);
            if (kk > best)
                best = kk;
        }
    if (w->t8 != NULL) {
        /* (a later one of the ten may have taken the slot of an earlier one: then the list is not kept) */
        int ok = 1;
        for (k = 0; k < n && ok; k++) {
            const struct Trail8 *t = list[k];
            ok = (t != NULL) && (k == 0 ? t->start : (!t->start && t->tx == tx0 - 1 + (k - 1) % 3 && t->ty == ty0 - 1 + (k - 1) / 3));
        }
        w->t8->near_valid = ok;
        if (ok) {
            for (k = 0; k < n; k++)
                w->t8->near[k] = list[k];
            w->t8->near_tx = tx0;
            w->t8->near_ty = ty0;
        }
    }
    return best;
}

/** Version 8: is gold square (cx, cy) kept? */
static int seam_kept8_now(const struct WgWorld *w, int64_t cx, int64_t cy)
{
    const int64_t mx = cx * SEAM_CELL + SEAM_CELL / 2 - START_X, my = cy * SEAM_CELL + SEAM_CELL / 2 - START_Y;
    int64_t keep = 250 + 750 * (int64_t)isqrt8((uint64_t)(mx * mx + my * my)) / TRAIL_THIN_FULL;
    if (keep > 1000)
        keep = 1000;
    return hash3(w->s_thin, (uint64_t)cx, (uint64_t)cy) % 1000 < (uint64_t)keep;
}

/** seam_kept8_now, its answers kept (direct-mapped by the square). */
static int seam_kept8(struct WgWorld *w, int64_t cx, int64_t cy)
{
    uint32_t i;
    if (w->t8 == NULL) {
        int k;
        w->t8 = (struct Trail8Cache *)calloc(1, sizeof(struct Trail8Cache));
        if (w->t8 == NULL)
            return seam_kept8_now(w, cx, cy);
        for (k = 0; k < 512; k++)
            w->t8->kept[k].kept = -1;
    }
    i = (uint32_t)(((uint64_t)cx * 0x9E3779B1u + (uint64_t)cy * 0x85EBCA77u) >> 7) & 511u;
    if (w->t8->kept[i].kept < 0 || w->t8->kept[i].cx != cx || w->t8->kept[i].cy != cy) {
        w->t8->kept[i].cx = cx;
        w->t8->kept[i].cy = cy;
        w->t8->kept[i].kept = (int8_t)seam_kept8_now(w, cx, cy);
    }
    return w->t8->kept[i].kept;
}

/** Version 8's seam_gold: version 7's (seam_gold), with the squares thinned near home. */
static int seam_gold8(struct WgWorld *w, int64_t x, int64_t y)
{
    int64_t gx = fdiv(x, SEAM_CELL), gy = fdiv(y, SEAM_CELL), cx, cy;
    int found = 0;
    for (cy = gy - 2; cy <= gy + 2; cy++)
        for (cx = gx - 2; cx <= gx + 2; cx++) {
            struct SeamEntry *e;
            int k;
            if (!seam_kept8(w, cx, cy))
                continue;
            e = seam_get(w, cx, cy);
            if (e->w == 0 || x < e->x0 || y < e->y0 || x >= e->x0 + e->w || y >= e->y0 + e->h)
                continue;
            if (e->stage != 0) {
                /* (as seam_gold: the deposit is worked out as far as this slab needs) */
                seam_settle7(w, e, x, y);
                if (e->stage != 0 || e->w == 0 || x < e->x0 || y < e->y0 || x >= e->x0 + e->w || y >= e->y0 + e->h)
                    continue;
            }
            k = e->cells[(y - e->y0) * e->w + (x - e->x0)];
            if (k == WgS_DenseGold)
                return WgS_DenseGold;
            if (k != 0)
                found = WgS_Gold;
        }
    return found;
}

/** Version 8's slab from version 7's biome and ground at it (worldgen.World.terrain8). */
static uint8_t terrain8_of(struct WgWorld *w, int64_t xi, int64_t yi, int biome7, uint8_t base)
{
    int t, g;
    if (biome7 == WgB_DeadMans)
        return WgS_Earth;
    t = trail8_at(w, xi, yi);
    if (t >= TRAIL_LUMP && base != WgS_Lava && base != WgS_Water)
        return (uint8_t)(t == TRAIL_LODE_DENSE ? WgS_DenseGold : WgS_Gold);
    g = seam_gold8(w, xi, yi);
    if (g != 0)
        return (uint8_t)g;
    if (t == TRAIL_VEIN && base == WgS_Rock)
        return WgS_Earth;
    return base;
}

/** Version 8's slab (worldgen.World.terrain8). */
static uint8_t terrain8(struct WgWorld *w, int64_t xi, int64_t yi, const struct Clim7 *c)
{
    if (c->special == WgB_DeadMans)
        return WgS_Earth;
    return terrain8_of(w, xi, yi, biome7_of(c), ground7_memo(w, xi, yi, c));
}

static void trail8_free(struct WgWorld *w)
{
    int i;
    if (w->t8 == NULL)
        return;
    for (i = 0; i < TRAIL_CACHE; i++)
        free(w->t8->t[i].grid);
    free(w->t8);
    w->t8 = NULL;
}

/** The mother lodes whose middle is in a box of world slabs (version 8; the engine puts what lives there). */
int wg_mother_lodes_in(struct WgWorld *w, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t *lx, int64_t *ly, int *lr,
    int max)
{
    int n = 0;
    int64_t tx, ty;
    const struct Trail8 *t;
    if (w == NULL || w->version < 8 || max <= 0)
        return 0;
    t = trail8_get(w, 1, 0, 0);
    if (t != NULL && t->has && t->lx >= x1 && t->lx <= x2 && t->ly >= y1 && t->ly <= y2) {
        lx[n] = t->lx; ly[n] = t->ly; lr[n] = t->lr; n++;
    }
    for (ty = fdiv(y1, TRAIL_CELL) - 1; ty <= fdiv(y2, TRAIL_CELL) + 1 && n < max; ty++)
        for (tx = fdiv(x1, TRAIL_CELL) - 1; tx <= fdiv(x2, TRAIL_CELL) + 1 && n < max; tx++) {
            t = trail8_get(w, 0, tx, ty);
            if (t != NULL && t->has && t->lx >= x1 && t->lx <= x2 && t->ly >= y1 && t->ly <= y2) {
                lx[n] = t->lx; ly[n] = t->ly; lr[n] = t->lr; n++;
            }
        }
    return n;
}

/******************************************************************************/
/* The Overgrowth (roadmap "The Overgrowth"): Earthen Caverns where its own noise is high. Its ground, gems, structures
   and creatures are the Earthen Caverns'; only the biome the place reports is its own (the engine gives it its rules:
   the earth grows back over nobody's dug ground at dawn, Druids for heroes, the Maiden of the Nest for wild ones). */
#define OVERGROWTH_SCALE 220.0
#define OVERGROWTH_LEVEL 0.15
#define OVERGROWTH_CELL 4 /* the noise is read at the middle of each 4x4 block of slabs (and the last block's kept) */

/** Version 8's biome at a world slab from version 7's there (Earthen Caverns means no province: the same test). */
static int biome8_from7(const struct WgWorld *w, int64_t x, int64_t y, int biome7)
{
    struct Trail8Cache *tc = w->t8; /* (the world's version 8 caches: the last block's answer is kept there) */
    int64_t bx, by;
    int og;
    if (biome7 != WgB_Earthen)
        return biome7;
    bx = fdiv(x, OVERGROWTH_CELL);
    by = fdiv(y, OVERGROWTH_CELL);
    if (tc != NULL && tc->og_valid && tc->og_bx == bx && tc->og_by == by)
        return tc->og ? WgB_Overgrowth : biome7;
    {
        const int64_t mx = bx * OVERGROWTH_CELL + OVERGROWTH_CELL / 2, my = by * OVERGROWTH_CELL + OVERGROWTH_CELL / 2;
        og = fbm(w->s_og, (double)mx / OVERGROWTH_SCALE, (double)my / OVERGROWTH_SCALE, 3) > OVERGROWTH_LEVEL;
    }
    if (tc != NULL) {
        tc->og_valid = 1;
        tc->og_bx = bx;
        tc->og_by = by;
        tc->og = og;
    }
    return og ? WgB_Overgrowth : biome7;
}

/* The borderlands (roadmap "Borderlands"): nearer than BORDER_MARGIN to the line between two biomes (the climate's
   nearest and next), where the pair is named. Their ground is the land's; the engine gives each its rule. */
#define BORDER_MARGIN 0.025

static int border8_of(const struct Clim7 *c)
{
    int a, b;
    if (c->special >= 0 || !(c->margin < BORDER_MARGIN))
        return -1;
    a = c->base < c->second ? c->base : c->second;
    b = c->base < c->second ? c->second : c->base;
    if (a == WgB_Volcanic && b == WgB_Flooded) return WgB_SteamCountry;
    if (a == WgB_Flooded && b == WgB_Volcanic) return WgB_SteamCountry;
    if (a == WgB_Earthen && b == WgB_Sandstone) return WgB_Oasis;
    if (a == WgB_Earthen && b == WgB_Ruins) return WgB_VaultCountry;
    if (a == WgB_Rocky && b == WgB_Volcanic) return WgB_Obsidian;
    if (a == WgB_Flooded && b == WgB_Frozen) return WgB_Frostmarsh;
    if (a == WgB_Volcanic && b == WgB_Sandstone) return WgB_Ashlands;
    return -1;
}

/** Version 8's biome at a world slab (worldgen.World.biome8): a border, else version 7's with the Overgrowth. */
static int biome8_of(const struct WgWorld *w, int64_t x, int64_t y, const struct Clim7 *c)
{
    const int border = border8_of(c);
    if (border >= 0)
        return border;
    return biome8_from7(w, x, y, biome7_of(c));
}

/******************************************************************************/
/* Priced prizes (roadmap "Priced Prizes"), for gems: a gem cluster is ringed by rock two slabs thick, with one plug of
   gold through it as the only way in, unless it is within PRIZE_FREE slabs of the start (worldgen.World.prize8_gems). */
#define PRIZE_FREE 250
static const int prize_dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

static void prize8_gems(const struct WgWorld *w, struct WgChunk *out, int64_t cx0, int64_t cy0)
{
    int first = -1, i, lx, ly, ox, oy, k, d;
    int64_t dx, dy;
    for (i = 0; i < WG_CHUNK * WG_CHUNK && first < 0; i++)
        if (out->slabs[i] == WgS_Gems)
            first = i;
    if (first < 0)
        return;
    lx = first % WG_CHUNK;
    ly = first / WG_CHUNK;
    dx = cx0 + lx - START_X;
    dy = cy0 + ly - START_Y;
    if ((int64_t)isqrt8((uint64_t)(dx * dx + dy * dy)) < PRIZE_FREE)
        return;
    for (oy = -2; oy <= 2; oy++)
        for (ox = -2; ox <= 2; ox++) {
            const int x = lx + ox, y = ly + oy;
            if ((ox == 0 && oy == 0) || x < 0 || y < 0 || x >= WG_CHUNK || y >= WG_CHUNK)
                continue;
            i = y * WG_CHUNK + x;
            if (out->slabs[i] != WgS_Gems && out->slabs[i] != WgS_Lava && out->slabs[i] != WgS_Water)
                out->slabs[i] = WgS_Rock;
        }
    d = (int)(hash4(w->s_trail, (uint64_t)(int64_t)out->cx, (uint64_t)(int64_t)out->cy, 0x505249ULL) % 4);
    for (k = 1; k <= 2; k++) {
        const int x = lx + prize_dirs[d][0] * k, y = ly + prize_dirs[d][1] * k;
        if (x >= 0 && y >= 0 && x < WG_CHUNK && y < WG_CHUNK && out->slabs[y * WG_CHUNK + x] == WgS_Rock)
            out->slabs[y * WG_CHUNK + x] = WgS_Gold;
    }
}

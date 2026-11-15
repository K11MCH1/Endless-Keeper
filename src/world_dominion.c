/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dominion.c
 *     Endless world: dead dominions (see world_dominion.h).
 * @par Purpose:
 *     Roadmap page "Dead Dominions": "Some of the world's ruins were never the heroes'. They are what is left of a
 *     keeper who died long ago": broken rooms, traps nobody owns that are still armed, and a cracked, empty pedestal
 *     where a Dungeon Heart once beat. The rooms can be claimed, and a claimed room works whether or not you have
 *     learnt to build it (building more still takes the book: stock rules). The farther from home, the better the
 *     rooms, as books get better with distance.
 *
 *     Decided (2026-11-13, the page's open point "which rooms, and how far out"): the rooms follow the books' grades
 *     of the distance from the nearest start (world_progress_grade_at). The starting kit's rooms (Treasure Room,
 *     Lair, Hatchery) never: a keeper has them already.
 *       grade 1 (under 160 slabs): Training Room, Library                          2 rooms of 5x5, 45% broken
 *       grade 2 (160-260):         and Workshop, Barracks, Prison                  3 rooms of 5x5, 40% broken
 *       grade 3 (260-400):         and Torture Chamber, Graveyard, Scavenger Room  3 rooms of 5x5 or 7x7, 30%
 *       grade 4-5 (400 and more):  and Temple                                      4 rooms of 7x7, 20% broken
 *     One room of the newest grade's own kinds comes first, no kind twice. Each room keeps only its largest piece. How a room is broken: a Temple misses tiles (dirt
 *     path where they were), a Workshop is buried (earth over part of it), a Graveyard is full (gravestones over
 *     its tiles), the others miss tiles. A few walls have caved in. Traps nobody owns (stock: they go off for any
 *     creature): 2 to 5 by grade, Poison Gas and Lightning, then Boulders, then Words of Power.
 *
 *     Placement: the world is cut into 128x128-slab regions; a region holds one or not (35%, seeded), at the first
 *     of four seeded places inside it that is clear: at least 100 slabs from the world's start, clear of the generator's keepers, portals,
 *     rewards and fortresses, of the start copies and of any heroes outpost there may be, and (version 7) not in
 *     Dead Man's Land or an ocean. It is decided the first time a chunk it may cover is generated, and only if none
 *     of its ground was made before (so a world made before this gets them only in new ground). What was decided is
 *     saved (XTR2 record "DDM1"): a region, its grade and which place. The layout follows from the seed, the region and the grade.
 *
 *     Layout (33x33 slabs): a heart chamber of 9x9 with the 3x3 pedestal (hard floor, never claimable) in its
 *     middle and a statue in each corner; rooms to the north, east, south or west at the end of a short hall; each
 *     room open on its far side, and every side without a room open by a dirt path to the edge. The land round them
 *     keeps its own ground, rock turned to earth (and 12 slabs of rock to earth from each corner outwards), so the
 *     dominion can always be dug to.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_dominion.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "world_outpost.h"
#include "world_setpiece.h"
#include "world_stream.h"
#include "world_gen.h"
#include "world_progress.h"
#include "bflib_basics.h"
#include "config.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "creature_control.h"
#include "game_legacy.h"
#include "map_data.h"
#include "map_blocks.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_traps.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define DD_REGION 128           /* one at most per 128x128 slabs (8x8 chunks) */
#define DD_CHANCE 35            /* percent of regions with one */
#define DD_N 33                 /* the box's side */
#define DD_C 16                 /* its middle */
#define DD_START_DIST 100       /* slabs from the world's start at least */
#define DD_MARGIN 4             /* slabs kept clear of the generator's structures and the outposts */
#define DD_FT_MARGIN 26         /* and of a fortress's middle (a hero fortress reaches 25 slabs from it) */
#define DD_EXIT 12              /* rock turned to earth from each corner outwards */
#define DD_NONE 0xFF            /* a decided region without one */
#define DD_MAX_TRAPS 8
#define DD_MAX_ITEMS 80
#define DD_CACHE 16
#define DD_SITES 4              /* seeded places tried in a region, the first that is clear is taken */

enum DdCell { DdC_Land = 0, DdC_Earth, DdC_Wall, DdC_Floor, DdC_Path, DdC_Room, DdC_Pedestal };

struct DdPlan {
    int64_t rx, ry;
    int64_t x0, y0;             /* world slab of the box's corner */
    uint8_t grade;              /* 1-5 */
    uint8_t cell[DD_N * DD_N];
    uint8_t room[DD_N * DD_N];  /* the slab kind of a DdC_Room cell */
    int nrooms;
    uint8_t rkind[4];           /* slab kinds */
    int ntraps;
    uint8_t tx[DD_MAX_TRAPS], ty[DD_MAX_TRAPS];
    uint16_t tmodel[DD_MAX_TRAPS];
    int nitems;
    uint8_t ix[DD_MAX_ITEMS], iy[DD_MAX_ITEMS];
    uint16_t imodel[DD_MAX_ITEMS];
};

struct DdState {
    int64_t rx, ry;
    uint8_t grade;              /* DD_NONE: none */
    uint8_t site;               /* which of the region's seeded places */
};

static struct DdState *dd_states;
static int dd_count, dd_cap;

static struct {
    TbBool valid;
    int64_t rx, ry;
    uint8_t grade, site;
    struct DdPlan plan;
} dd_cache[DD_CACHE];

#define DDI(x, y) ((y) * DD_N + (x))

/******************************************************************************/
struct DdRng {
    uint64_t s;
};

static uint64_t dd_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int dd_rand(struct DdRng *r, int n)
{
    r->s = dd_mix(r->s);
    return (n > 0) ? (int)(r->s % (uint64_t)n) : 0;
}

static int64_t dd_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (((a % b) != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/******************************************************************************/
/* what was decided */
static struct DdState *dd_state(int64_t rx, int64_t ry, TbBool add)
{
    for (int i = 0; i < dd_count; i++)
        if ((dd_states[i].rx == rx) && (dd_states[i].ry == ry))
            return &dd_states[i];
    if (!add)
        return NULL;
    if (dd_count >= dd_cap)
    {
        const int ncap = (dd_cap > 0) ? dd_cap * 2 : 32;
        struct DdState *n = (struct DdState *)realloc(dd_states, (size_t)ncap * sizeof(*n));
        if (n == NULL)
            return NULL;
        dd_states = n;
        dd_cap = ncap;
    }
    struct DdState *st = &dd_states[dd_count++];
    st->rx = rx;
    st->ry = ry;
    st->grade = DD_NONE;
    st->site = 0;
    return st;
}

void world_dominions_reset(void)
{
    free(dd_states);
    dd_states = NULL;
    dd_count = dd_cap = 0;
    memset(dd_cache, 0, sizeof(dd_cache));
}

/******************************************************************************/
/* where */

/** A region's seeded place number `site` (the box's corner); false when the region has none, or that place is too
 *  near the world's start. */
static TbBool dd_site(int64_t rx, int64_t ry, int site, int64_t *x0, int64_t *y0)
{
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return false;
    struct DdRng rng;
    rng.s = wg_world_seed(gen) ^ dd_mix((uint64_t)rx * 0x5DEECE66DULL) ^ dd_mix((uint64_t)ry * 0x2545F491ULL + 0x44444F4DULL);
    if (dd_rand(&rng, 100) >= DD_CHANCE)
        return false;
    const int span = DD_REGION - DD_N - 2 * (DD_EXIT + 2);
    for (int k = 0; k <= site; k++)
    {
        *x0 = rx * DD_REGION + DD_EXIT + 2 + dd_rand(&rng, span);
        *y0 = ry * DD_REGION + DD_EXIT + 2 + dd_rand(&rng, span);
    }
    const int64_t dx = *x0 + DD_C - 8, dy = *y0 + DD_C - 8;
    return dx * dx + dy * dy >= (int64_t)DD_START_DIST * DD_START_DIST;
}

/** Everything a dominion at x0, y0 touches: its box and the ways out from its corners. */
static void dd_extent(int64_t x0, int64_t y0, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    *x1 = x0 - DD_EXIT;
    *y1 = y0;
    *x2 = x0 + DD_N - 1 + DD_EXIT;
    *y2 = y0 + DD_N - 1;
}

static TbBool dd_box_made(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int32_t skip_cx, int32_t skip_cy)
{
    for (int64_t cy = dd_floor_div(y1, WG_CHUNK); cy <= dd_floor_div(y2, WG_CHUNK); cy++)
        for (int64_t cx = dd_floor_div(x1, WG_CHUNK); cx <= dd_floor_div(x2, WG_CHUNK); cx++)
        {
            if ((cx == skip_cx) && (cy == skip_cy))
                continue;
            if (world_chunk_made((int32_t)cx, (int32_t)cy))
                return true;
        }
    return false;
}

/* why places were turned down (the survey logs them) */
static int dd_why_made, dd_why_structs, dd_why_fortress, dd_why_stamps, dd_why_outpost, dd_why_land;

static TbBool dd_place_clear(int64_t x0, int64_t y0)
{
    struct WgWorld *gen = world_generator();
    struct WgStructRef refs[2];
    const int64_t x1 = x0, y1 = y0, x2 = x0 + DD_N - 1, y2 = y0 + DD_N - 1;
    const unsigned mask = (1u << WgT_Keeper) | (1u << WgT_Portal) | (1u << WgT_Reward);
    if (wg_structures_in(gen, x1 - DD_MARGIN, y1 - DD_MARGIN, x2 + DD_MARGIN, y2 + DD_MARGIN, mask, refs, 2) > 0)
    {
        dd_why_structs++;
        return false;
    }
    if (wg_structures_in(gen, x1 - DD_FT_MARGIN, y1 - DD_FT_MARGIN, x2 + DD_FT_MARGIN, y2 + DD_FT_MARGIN,
        1u << WgT_Fortress, refs, 2) > 0)
    {
        dd_why_fortress++;
        return false;
    }
    if (world_stamps_overlap(x1 - DD_MARGIN, y1 - DD_MARGIN, x2 + DD_MARGIN, y2 + DD_MARGIN))
    {
        dd_why_stamps++;
        return false;
    }
    /* a set piece decided before it (whichever of the two is decided second keeps off the first) */
    if (world_setpiece_placed_in_box(x1 - DD_MARGIN, y1 - DD_MARGIN, x2 + DD_MARGIN, y2 + DD_MARGIN))
    {
        dd_why_structs++;
        return false;
    }
    /* (the ways out only turn rock into earth, never over an outpost's cells: the box alone keeps off them) */
    if (world_outpost_in_box(x1 - DD_MARGIN, y1 - DD_MARGIN, x2 + DD_MARGIN, y2 + DD_MARGIN))
    {
        dd_why_outpost++;
        return false;
    }
    /* version 7: never in Dead Man's Land or an ocean */
    if (!world_land_fits(x1, y1, x2, y2, false))
    {
        dd_why_land++;
        return false;
    }
    return true;
}

/** Decides a region the first time a chunk it may cover is generated (gen_cx, gen_cy: that chunk, whose ground is
 *  being made now). NULL: the region has none and never had a chance of one. */
static struct DdState *dd_decide(int64_t rx, int64_t ry, int32_t gen_cx, int32_t gen_cy)
{
    struct DdState *st = dd_state(rx, ry, false);
    if (st != NULL)
        return st;
    int64_t x0, y0;
    TbBool any = false;
    for (int k = 0; k < DD_SITES; k++)
        any |= dd_site(rx, ry, k, &x0, &y0);
    if (!any)
        return NULL;
    st = dd_state(rx, ry, true);
    if (st == NULL)
        return NULL;
    /* the first of its places whose ground none was made of before and that is clear */
    int site = -1;
    for (int k = 0; (k < DD_SITES) && (site < 0); k++)
    {
        int64_t x1, y1, x2, y2;
        if (!dd_site(rx, ry, k, &x0, &y0))
            continue;
        dd_extent(x0, y0, &x1, &y1, &x2, &y2);
        if (dd_box_made(x1, y1, x2, y2, gen_cx, gen_cy))
            dd_why_made++;
        else if (dd_place_clear(x0, y0))
            site = k;
    }
    st = dd_state(rx, ry, false);
    if (site < 0)
    {
        st->grade = DD_NONE;
        return st;
    }
    st->site = (uint8_t)site;
    const double d = world_dist_to_nearest_start(x0 + DD_C, y0 + DD_C);
    const int grade = max(1, min(5, world_progress_grade_at(d)));
    st = dd_state(rx, ry, false);
    st->grade = (uint8_t)grade;
    SYNCLOG("World: a dead dominion at world slab (%lld,%lld), %.0f slabs from the nearest start (grade %d)",
        (long long)(x0 + DD_C), (long long)(y0 + DD_C), d, grade);
    return st;
}

/******************************************************************************/
/* the layout */

/* the rooms by grade (slab kinds), first the grade's own */
static const uint8_t dd_rooms_g1[] = {SlbT_TRAINING, SlbT_LIBRARY};
static const uint8_t dd_rooms_g2[] = {SlbT_WORKSHOP, SlbT_BARRACKS, SlbT_PRISON};
static const uint8_t dd_rooms_g3[] = {SlbT_TORTURE, SlbT_GRAVEYARD, SlbT_SCAVENGER};
static const uint8_t dd_rooms_g4[] = {SlbT_TEMPLE};

static int dd_pool(int grade, uint8_t *pool)
{
    int n = 0;
    for (size_t i = 0; i < sizeof(dd_rooms_g1); i++) pool[n++] = dd_rooms_g1[i];
    if (grade >= 2) for (size_t i = 0; i < sizeof(dd_rooms_g2); i++) pool[n++] = dd_rooms_g2[i];
    if (grade >= 3) for (size_t i = 0; i < sizeof(dd_rooms_g3); i++) pool[n++] = dd_rooms_g3[i];
    if (grade >= 4) for (size_t i = 0; i < sizeof(dd_rooms_g4); i++) pool[n++] = dd_rooms_g4[i];
    return n;
}

static const uint8_t *dd_newest(int grade, int *n)
{
    switch (grade)
    {
    case 1: *n = (int)sizeof(dd_rooms_g1); return dd_rooms_g1;
    case 2: *n = (int)sizeof(dd_rooms_g2); return dd_rooms_g2;
    case 3: *n = (int)sizeof(dd_rooms_g3); return dd_rooms_g3;
    default: *n = (int)sizeof(dd_rooms_g4); return dd_rooms_g4;
    }
}

static const char *const dd_traps_by_grade[4][4] = {
    {"POISON_GAS", "LIGHTNING", NULL, NULL},
    {"POISON_GAS", "LIGHTNING", "BOULDER", NULL},
    {"POISON_GAS", "LIGHTNING", "BOULDER", NULL},
    {"POISON_GAS", "LIGHTNING", "BOULDER", "WORD_OF_POWER"},
};

/* direction d: outward step and the step across */
static const int dd_ox[4] = {0, 1, 0, -1}, dd_oy[4] = {-1, 0, 1, 0};
static const int dd_px[4] = {1, 0, -1, 0}, dd_py[4] = {0, 1, 0, -1};

static void dd_put(struct DdPlan *p, int d, int dist, int perp, uint8_t c, uint8_t rk)
{
    const int x = DD_C + dd_ox[d] * dist + dd_px[d] * perp, y = DD_C + dd_oy[d] * dist + dd_py[d] * perp;
    if ((x < 0) || (y < 0) || (x >= DD_N) || (y >= DD_N))
        return;
    p->cell[DDI(x, y)] = c;
    p->room[DDI(x, y)] = rk;
}

static uint8_t dd_get(const struct DdPlan *p, int d, int dist, int perp)
{
    const int x = DD_C + dd_ox[d] * dist + dd_px[d] * perp, y = DD_C + dd_oy[d] * dist + dd_py[d] * perp;
    if ((x < 0) || (y < 0) || (x >= DD_N) || (y >= DD_N))
        return DdC_Land;
    return p->cell[DDI(x, y)];
}

static void dd_item(struct DdPlan *p, int x, int y, const char *name)
{
    const long m = get_id(object_desc, name);
    if ((m <= 0) || (p->nitems >= DD_MAX_ITEMS) || (x < 0) || (y < 0) || (x >= DD_N) || (y >= DD_N))
        return;
    p->ix[p->nitems] = (uint8_t)x;
    p->iy[p->nitems] = (uint8_t)y;
    p->imodel[p->nitems] = (uint16_t)m;
    p->nitems++;
}

static void dd_build(int64_t rx, int64_t ry, int site, int grade, struct DdPlan *p)
{
    memset(p, 0, sizeof(*p));
    p->rx = rx;
    p->ry = ry;
    p->grade = (uint8_t)grade;
    dd_site(rx, ry, site, &p->x0, &p->y0);
    struct WgWorld *gen = world_generator();
    struct DdRng rng;
    rng.s = ((gen != NULL) ? wg_world_seed(gen) : 0) ^ dd_mix((uint64_t)rx * 0x9E3779B1ULL + 0x44444C59ULL) ^
        dd_mix((uint64_t)ry * 0x85EBCA6BULL) ^ (uint64_t)grade;
    /* the heart chamber: 9x9 inside its walls, the pedestal in the middle */
    for (int y = DD_C - 5; y <= DD_C + 5; y++)
        for (int x = DD_C - 5; x <= DD_C + 5; x++)
        {
            const int r = max(abs(x - DD_C), abs(y - DD_C));
            p->cell[DDI(x, y)] = (r == 5) ? DdC_Wall : ((r <= 1) ? DdC_Pedestal : DdC_Floor);
        }
    /* the rooms */
    const int want = (grade >= 4) ? 4 : ((grade >= 2) ? 3 : 2);
    int dirs[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; i--)
    {
        const int j = dd_rand(&rng, i + 1);
        const int t = dirs[i];
        dirs[i] = dirs[j];
        dirs[j] = t;
    }
    uint8_t pool[16];
    const int npool = dd_pool(grade, pool);
    int nnew;
    const uint8_t *newest = dd_newest(grade, &nnew);
    const int broken = (grade >= 4) ? 20 : ((grade == 3) ? 30 : ((grade == 2) ? 40 : 45));
    for (int r = 0; r < 4; r++)
    {
        const int d = dirs[r];
        if (r >= want)
        {
            /* no room this way: a dirt path from the chamber to the edge */
            for (int dist = 5; dist <= DD_C; dist++)
                dd_put(p, d, dist, 0, DdC_Path, 0);
            continue;
        }
        uint8_t kind = newest[dd_rand(&rng, nnew)];
        if (r > 0)
        {
            /* any kind of the grade's pool not here yet (one may come twice only when the pool runs out) */
            uint8_t left[16];
            int nleft = 0;
            for (int k = 0; k < npool; k++)
            {
                TbBool used = false;
                for (int u = 0; u < r; u++)
                    used |= (p->rkind[u] == pool[k]);
                if (!used)
                    left[nleft++] = pool[k];
            }
            kind = (nleft > 0) ? left[dd_rand(&rng, nleft)] : pool[dd_rand(&rng, npool)];
        }
        p->rkind[p->nrooms++] = kind;
        const int s = (grade >= 4) ? 7 : ((grade == 3) ? (dd_rand(&rng, 2) ? 7 : 5) : 5);
        const int half = s / 2;
        /* the hall */
        dd_put(p, d, 5, 0, DdC_Floor, 0);
        for (int dist = 6; dist <= 7; dist++)
        {
            dd_put(p, d, dist, 0, DdC_Floor, 0);
            dd_put(p, d, dist, -1, DdC_Wall, 0);
            dd_put(p, d, dist, 1, DdC_Wall, 0);
        }
        /* the room, its walls, its door from the hall and its way out on the far side */
        for (int dist = 8; dist <= 9 + s; dist++)
            for (int perp = -(half + 1); perp <= half + 1; perp++)
            {
                const TbBool wall = (dist == 8) || (dist == 9 + s) || (abs(perp) == half + 1);
                dd_put(p, d, dist, perp, wall ? DdC_Wall : DdC_Room, wall ? 0 : kind);
            }
        dd_put(p, d, 8, 0, DdC_Floor, 0);
        dd_put(p, d, 9 + s, 0, DdC_Path, 0);
        for (int dist = 10 + s; dist <= DD_C; dist++)
            dd_put(p, d, dist, 0, DdC_Path, 0);
        /* broken: tiles missing, buried or covered with gravestones */
        for (int dist = 9; dist <= 8 + s; dist++)
            for (int perp = -half; perp <= half; perp++)
            {
                if (dd_rand(&rng, 100) >= broken)
                    continue;
                if (kind == SlbT_WORKSHOP)
                    dd_put(p, d, dist, perp, DdC_Earth, 0);
                else if (kind == SlbT_GRAVEYARD)
                    dd_item(p, DD_C + dd_ox[d] * dist + dd_px[d] * perp, DD_C + dd_oy[d] * dist + dd_py[d] * perp, "GRAVE_STONE");
                else
                    dd_put(p, d, dist, perp, DdC_Path, 0);
            }
    }
    /* each room kind keeps its largest piece of tiles: the rest is missing too */
    for (int r = 0; r < p->nrooms; r++)
    {
        static uint8_t seen[DD_N * DD_N];
        static int queue[DD_N * DD_N];
        memset(seen, 0, sizeof(seen));
        int best_start = -1, best_size = 0;
        for (int i = 0; i < DD_N * DD_N; i++)
        {
            if (seen[i] || (p->cell[i] != DdC_Room) || (p->room[i] != p->rkind[r]))
                continue;
            int head = 0, tail = 0;
            queue[tail++] = i;
            seen[i] = 1;
            while (head < tail)
            {
                const int c = queue[head++];
                for (int k = 0; k < 4; k++)
                {
                    const int nx = c % DD_N + dd_ox[k], ny = c / DD_N + dd_oy[k];
                    if ((nx < 0) || (ny < 0) || (nx >= DD_N) || (ny >= DD_N))
                        continue;
                    const int n = DDI(nx, ny);
                    if (!seen[n] && (p->cell[n] == DdC_Room) && (p->room[n] == p->rkind[r]))
                    {
                        seen[n] = 1;
                        queue[tail++] = n;
                    }
                }
            }
            if (tail > best_size)
            {
                best_size = tail;
                best_start = i;
            }
        }
        /* mark the best piece again, then clear the others */
        memset(seen, 0, sizeof(seen));
        if (best_start >= 0)
        {
            int head = 0, tail = 0;
            queue[tail++] = best_start;
            seen[best_start] = 1;
            while (head < tail)
            {
                const int c = queue[head++];
                for (int k = 0; k < 4; k++)
                {
                    const int nx = c % DD_N + dd_ox[k], ny = c / DD_N + dd_oy[k];
                    if ((nx < 0) || (ny < 0) || (nx >= DD_N) || (ny >= DD_N))
                        continue;
                    const int n = DDI(nx, ny);
                    if (!seen[n] && (p->cell[n] == DdC_Room) && (p->room[n] == p->rkind[r]))
                    {
                        seen[n] = 1;
                        queue[tail++] = n;
                    }
                }
            }
        }
        for (int i = 0; i < DD_N * DD_N; i++)
            if ((p->cell[i] == DdC_Room) && (p->room[i] == p->rkind[r]) && !seen[i])
            {
                p->cell[i] = (p->rkind[r] == SlbT_WORKSHOP) ? DdC_Earth : DdC_Path;
                p->room[i] = 0;
            }
    }
    /* caved-in walls (not by a door or a hall) */
    for (int y = 0; y < DD_N; y++)
        for (int x = 0; x < DD_N; x++)
        {
            if ((p->cell[DDI(x, y)] != DdC_Wall) || (dd_rand(&rng, 100) >= 12))
                continue;
            TbBool by_door = false;
            for (int k = 0; k < 4; k++)
            {
                const int nx = x + dd_ox[k], ny = y + dd_oy[k];
                if ((nx >= 0) && (ny >= 0) && (nx < DD_N) && (ny < DD_N) &&
                    ((p->cell[DDI(nx, ny)] == DdC_Floor) && ((abs(nx - DD_C) > 4) || (abs(ny - DD_C) > 4))))
                    by_door = true;
            }
            if (!by_door)
                p->cell[DDI(x, y)] = DdC_Earth;
        }
    /* the pedestal's candles, a statue in each corner of the chamber */
    dd_item(p, DD_C - 1, DD_C - 1, "CANDLESTCK");
    dd_item(p, DD_C + 1, DD_C - 1, "CANDLESTCK");
    dd_item(p, DD_C - 1, DD_C + 1, "CANDLESTCK");
    dd_item(p, DD_C + 1, DD_C + 1, "CANDLESTCK");
    static const char *const statues[4] = {"STATUE2", "STATUE3", "STATUE4", "STATUE5"};
    dd_item(p, DD_C - 4, DD_C - 4, statues[dd_rand(&rng, 4)]);
    dd_item(p, DD_C + 4, DD_C - 4, statues[dd_rand(&rng, 4)]);
    dd_item(p, DD_C - 4, DD_C + 4, statues[dd_rand(&rng, 4)]);
    dd_item(p, DD_C + 4, DD_C + 4, statues[dd_rand(&rng, 4)]);
    /* the traps: in the halls and the chamber, not on the pedestal, its ring, a corner or a door */
    int cx[64], cy[64], nc = 0;
    for (int y = DD_C - 4; y <= DD_C + 4; y++)
        for (int x = DD_C - 4; x <= DD_C + 4; x++)
        {
            const int r = max(abs(x - DD_C), abs(y - DD_C));
            if ((r >= 3) && (r <= 3) && (nc < 64) && (p->cell[DDI(x, y)] == DdC_Floor))
            {
                cx[nc] = x;
                cy[nc] = y;
                nc++;
            }
        }
    for (int r = 0; r < p->nrooms; r++)
        for (int dist = 6; (dist <= 7) && (nc < 64); dist++)
        {
            const int d = dirs[r];
            if (dd_get(p, d, dist, 0) != DdC_Floor)
                continue;
            cx[nc] = DD_C + dd_ox[d] * dist;
            cy[nc] = DD_C + dd_oy[d] * dist;
            nc++;
        }
    const int gi = min(grade, 4) - 1;
    int nkinds = 0;
    while ((nkinds < 4) && (dd_traps_by_grade[gi][nkinds] != NULL))
        nkinds++;
    const int ntraps = min(1 + grade, 5);
    for (int t = 0; (t < ntraps) && (nc > 0) && (p->ntraps < DD_MAX_TRAPS); t++)
    {
        const int c = dd_rand(&rng, nc);
        const long m = get_id(trap_desc, dd_traps_by_grade[gi][dd_rand(&rng, nkinds)]);
        if (m > 0)
        {
            p->tx[p->ntraps] = (uint8_t)cx[c];
            p->ty[p->ntraps] = (uint8_t)cy[c];
            p->tmodel[p->ntraps] = (uint16_t)m;
            p->ntraps++;
        }
        cx[c] = cx[nc - 1];
        cy[c] = cy[nc - 1];
        nc--;
    }
}

static const struct DdPlan *dd_plan_of(const struct DdState *st)
{
    if ((st == NULL) || (st->grade == DD_NONE))
        return NULL;
    const int slot = (int)(((uint64_t)st->rx * 31u + (uint64_t)st->ry * 17u) % DD_CACHE);
    if (!dd_cache[slot].valid || (dd_cache[slot].rx != st->rx) || (dd_cache[slot].ry != st->ry) ||
        (dd_cache[slot].grade != st->grade) || (dd_cache[slot].site != st->site))
    {
        dd_build(st->rx, st->ry, st->site, st->grade, &dd_cache[slot].plan);
        dd_cache[slot].valid = true;
        dd_cache[slot].rx = st->rx;
        dd_cache[slot].ry = st->ry;
        dd_cache[slot].grade = st->grade;
        dd_cache[slot].site = st->site;
    }
    return &dd_cache[slot].plan;
}

/******************************************************************************/
/* the chunks */
static void dd_thing(struct OutpostOverlay *ov, int32_t cx, int32_t cy, int64_t wx, int64_t wy, uint8_t oclass,
    uint16_t model, uint16_t extra)
{
    if ((wx < (int64_t)cx * WG_CHUNK) || (wx >= (int64_t)cx * WG_CHUNK + WG_CHUNK) || (wy < (int64_t)cy * WG_CHUNK) ||
        (wy >= (int64_t)cy * WG_CHUNK + WG_CHUNK) || (ov->thing_count >= OUTPOST_OVERLAY_THINGS) || (model == 0))
        return;
    struct OutpostOverlayThing *t = &ov->things[ov->thing_count++];
    memset(t, 0, sizeof(*t));
    t->oclass = oclass;
    t->model = model;
    t->x = (int32_t)(((wx - (int64_t)cx * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    t->y = (int32_t)(((wy - (int64_t)cy * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    t->extra = extra;
}

static int dd_overlay_plan(const struct DdPlan *p, int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    int decided = 0;
    for (int64_t gy = max(p->y0, wy1); gy <= min(p->y0 + DD_N - 1, wy2); gy++)
        for (int64_t gx = max(p->x0, wx1); gx <= min(p->x0 + DD_N - 1, wx2); gx++)
        {
            const int x = (int)(gx - p->x0), y = (int)(gy - p->y0);
            const int i = (int)((gy - wy1) * WG_CHUNK + (gx - wx1));
            const uint8_t c = p->cell[DDI(x, y)];
            ov->heroes[i] = 0;
            ov->wall[i] = 0;
            ov->door[i] = 0;
            ov->set[i] = 1;
            switch (c)
            {
            case DdC_Land: ov->set[i] = 2; ov->kind[i] = SlbT_EARTH; break; /* rock to earth, the rest as it is */
            case DdC_Earth: ov->kind[i] = SlbT_EARTH; break;
            case DdC_Wall: ov->kind[i] = SlbT_WALLDRAPE; ov->wall[i] = 1; break;
            case DdC_Floor: ov->kind[i] = SlbT_CLAIMED; break;
            case DdC_Path: ov->kind[i] = SlbT_PATH; break;
            case DdC_Room: ov->kind[i] = p->room[DDI(x, y)]; break;
            case DdC_Pedestal: ov->kind[i] = SlbT_ROCK_FLOOR; break;
            default: ov->kind[i] = SlbT_EARTH; break;
            }
            decided++;
        }
    /* the ways out: from each corner, outwards along its row */
    for (int k = 0; k < 4; k++)
    {
        const int64_t ey = p->y0 + ((k & 2) ? DD_N - 1 : 0);
        const int64_t ex = p->x0 + ((k & 1) ? DD_N - 1 : 0);
        const int step = (k & 1) ? 1 : -1;
        for (int n = 1; n <= DD_EXIT; n++)
        {
            const int64_t gx = ex + (int64_t)step * n;
            if ((gx < wx1) || (gx > wx2) || (ey < wy1) || (ey > wy2))
                continue;
            const int i = (int)((ey - wy1) * WG_CHUNK + (gx - wx1));
            if (ov->set[i] == 1)
                continue;
            ov->set[i] = 2;
            ov->kind[i] = SlbT_EARTH;
            decided++;
        }
    }
    for (int t = 0; t < p->ntraps; t++)
    {
        const struct TrapConfigStats *trapst = get_trap_model_stats(p->tmodel[t]);
        dd_thing(ov, cx, cy, p->x0 + p->tx[t], p->y0 + p->ty[t], TCls_Trap, p->tmodel[t], (uint16_t)max(trapst->shots, 1));
    }
    for (int k = 0; k < p->nitems; k++)
        dd_thing(ov, cx, cy, p->x0 + p->ix[k], p->y0 + p->iy[k], TCls_Object, p->imodel[k], 0);
    return decided;
}

int world_dominion_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    memset(ov, 0, sizeof(*ov));
    if (world_generator() == NULL)
        return 0;
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    int decided = 0;
    for (int64_t ry = dd_floor_div(wy1, DD_REGION); ry <= dd_floor_div(wy2, DD_REGION); ry++)
        for (int64_t rx = dd_floor_div(wx1 - DD_EXIT, DD_REGION); rx <= dd_floor_div(wx2 + DD_EXIT, DD_REGION); rx++)
        {
            /* decide only the regions one of whose places reaches this chunk */
            TbBool may = false;
            for (int k = 0; (k < DD_SITES) && !may; k++)
            {
                int64_t x0, y0, x1, y1, x2, y2;
                if (!dd_site(rx, ry, k, &x0, &y0))
                    continue;
                dd_extent(x0, y0, &x1, &y1, &x2, &y2);
                may = (x2 >= wx1) && (x1 <= wx2) && (y2 >= wy1) && (y1 <= wy2);
            }
            if (!may)
                continue;
            const struct DdPlan *p = dd_plan_of(dd_decide(rx, ry, cx, cy));
            if (p == NULL)
                continue;
            int64_t x1, y1, x2, y2;
            dd_extent(p->x0, p->y0, &x1, &y1, &x2, &y2);
            if ((x2 >= wx1) && (x1 <= wx2) && (y2 >= wy1) && (y1 <= wy2))
                decided += dd_overlay_plan(p, cx, cy, ov);
        }
    return decided;
}

TbBool world_dominion_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    if (world_generator() == NULL)
        return false;
    for (int64_t ry = dd_floor_div(y1, DD_REGION); ry <= dd_floor_div(y2, DD_REGION); ry++)
        for (int64_t rx = dd_floor_div(x1 - DD_EXIT, DD_REGION); rx <= dd_floor_div(x2 + DD_EXIT, DD_REGION); rx++)
        {
            const struct DdState *st = dd_state(rx, ry, false);
            if ((st != NULL) && (st->grade == DD_NONE))
                continue;
            for (int k = 0; k < DD_SITES; k++)
            {
                /* decided: its place; not yet: any it may take */
                int64_t x0, y0, bx1, by1, bx2, by2;
                if (((st != NULL) && (k != st->site)) || !dd_site(rx, ry, k, &x0, &y0))
                    continue;
                dd_extent(x0, y0, &bx1, &by1, &bx2, &by2);
                if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
                    return true;
            }
        }
    return false;
}

TbBool world_dominion_placed_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    for (int i = 0; i < dd_count; i++)
    {
        const struct DdState *st = &dd_states[i];
        int64_t x0, y0, bx1, by1, bx2, by2;
        if ((st->grade == DD_NONE) || !dd_site(st->rx, st->ry, st->site, &x0, &y0))
            continue;
        dd_extent(x0, y0, &bx1, &by1, &bx2, &by2);
        if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
            return true;
    }
    return false;
}

void world_dominion_survey(int64_t range)
{
    int regions = 0, with = 0, by_grade[6] = {0};
    for (int64_t ry = dd_floor_div(-range, DD_REGION); ry <= dd_floor_div(range, DD_REGION); ry++)
        for (int64_t rx = dd_floor_div(-range, DD_REGION); rx <= dd_floor_div(range, DD_REGION); rx++)
        {
            regions++;
            const struct DdState *st = dd_decide(rx, ry, INT32_MIN, INT32_MIN);
            if ((st == NULL) || (st->grade == DD_NONE))
                continue;
            with++;
            by_grade[min(5, (int)st->grade)]++;
        }
    SYNCLOG("World: dead dominions within %lld slabs: %d of %d regions; by grade 1-5: %d %d %d %d %d; places turned down: "
        "ground made %d, structures %d, fortresses %d, starts %d, outposts %d, land %d", (long long)range,
        with, regions, by_grade[1], by_grade[2], by_grade[3], by_grade[4], by_grade[5], dd_why_made, dd_why_structs,
        dd_why_fortress, dd_why_stamps, dd_why_outpost, dd_why_land);
}

/******************************************************************************/
/* the test hook */

/** Test hook KFX_WORLD_TESTDOMINION="T" (only read in endless worlds): the dead dominion nearest the start. T: it is
 *  decided, its chunks load and what stands there is logged (rooms by kind and owner, traps and their owner, the
 *  pedestal); T+50: player 0 takes every room in it (as claiming does) and the log says whether player 0 could
 *  build each kind; T+100: a creature of player 0's stands on each trap; T+200: how many traps went off; T+700 (its
 *  chunks left alone freeze before then): they load again and what stands there is logged; T+1500 logged again.
 *  KFX_WORLD_TESTDOMINION_IMPS=1: at T+50 the rooms are not taken: four of player 0's imps start in the heart chamber
 *  instead (the real way: they claim the floor and the rooms). */
static int64_t ddt_x0, ddt_y0;
static int ddt_traps_shots[DD_MAX_TRAPS];
static ThingIndex ddt_traps[DD_MAX_TRAPS];
static int ddt_ntraps;

static void ddt_scan(const char *when)
{
    int rooms[64] = {0}, owned0[64] = {0}, pedestal = 0, traps = 0, neutral_traps = 0;
    MapSlabCoord ex0, ey0;
    if (!world_engine_slab(ddt_x0, ddt_y0, &ex0, &ey0))
    {
        SYNCLOG("World test: dominion %s: its box is not loaded", when);
        return;
    }
    for (int y = 0; y < DD_N; y++)
        for (int x = 0; x < DD_N; x++)
        {
            const struct SlabMap *slb = get_slabmap_block(ex0 + x, ey0 + y);
            if (slabmap_block_invalid(slb))
                continue;
            if (slb->kind == SlbT_ROCK_FLOOR)
                pedestal++;
            if ((slb->kind < 64) && (slab_corresponding_room(slb->kind) > 0))
            {
                rooms[slb->kind]++;
                if (slabmap_owner(slb) == PLAYER0)
                    owned0[slb->kind]++;
            }
        }
    const struct StructureList *slist = get_list_for_thing_class(TCls_Trap);
    long guard = 0;
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        const int sx = subtile_slab(thing->mappos.x.stl.num) - ex0, sy = subtile_slab(thing->mappos.y.stl.num) - ey0;
        if ((sx < 0) || (sy < 0) || (sx >= DD_N) || (sy >= DD_N))
            continue;
        traps++;
        if (thing->owner == game.neutral_player_num)
            neutral_traps++;
    }
    char text[512];
    int len = snprintf(text, sizeof(text), "rooms (slabs, player 0's):");
    for (int k = 0; k < 64; k++)
        if (rooms[k] > 0)
            len += snprintf(text + len, sizeof(text) - (size_t)len, " %s %d/%d",
                room_code_name(slab_corresponding_room((SlabKind)k)), rooms[k], owned0[k]);
    SYNCLOG("World test: dominion %s: %s; pedestal %d slabs; %d traps (%d nobody's)", when, text, pedestal, traps,
        neutral_traps);
}

void world_dominion_test_turn(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_TESTDOMINION");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || (world_generator() == NULL))
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t != 0) && (t != 50) && (t != 100) && (t != 200) && (t != 700) && (t != 1500))
        return;
    if (t == 0)
    {
        /* the nearest region with one, ring by ring */
        const struct DdPlan *best = NULL;
        int64_t best_d = -1;
        for (int r = 0; (r <= 6) && (best == NULL); r++)
            for (int ry = -r; ry <= r; ry++)
                for (int rx = -r; rx <= r; rx++)
                {
                    if ((abs(rx) != r) && (abs(ry) != r))
                        continue;
                    const struct DdPlan *p = dd_plan_of(dd_decide(rx, ry, INT32_MIN, INT32_MIN));
                    if (p == NULL)
                        continue;
                    const int64_t d = (p->x0 + DD_C) * (p->x0 + DD_C) + (p->y0 + DD_C) * (p->y0 + DD_C);
                    if ((best_d < 0) || (d < best_d))
                    {
                        best_d = d;
                        best = p;
                        ddt_x0 = p->x0;
                        ddt_y0 = p->y0;
                    }
                }
        if (best == NULL)
        {
            SYNCLOG("World test: dominion: none near the start");
            base = -1;
            return;
        }
        char text[160];
        int len = 0;
        text[0] = 0;
        for (int k = 0; k < best->nrooms; k++)
            len += snprintf(text + len, sizeof(text) - (size_t)len, " %s", room_code_name(slab_corresponding_room(best->rkind[k])));
        const int n = world_load_chunks_round_world(ddt_x0 + DD_C, ddt_y0 + DD_C, 1);
        SYNCLOG("World test: dominion at world slab (%lld,%lld), grade %d, rooms%s, %d traps: %d chunks loaded",
            (long long)(ddt_x0 + DD_C), (long long)(ddt_y0 + DD_C), (int)best->grade, text, best->ntraps, n);
        ddt_scan("as found");
        /* its traps, as they are now */
        ddt_ntraps = 0;
        MapSlabCoord ex0, ey0;
        if (world_engine_slab(ddt_x0, ddt_y0, &ex0, &ey0))
        {
            const struct StructureList *slist = get_list_for_thing_class(TCls_Trap);
            long guard = 0;
            for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count) && (ddt_ntraps < DD_MAX_TRAPS);)
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_of_class;
                const int sx = subtile_slab(thing->mappos.x.stl.num) - ex0, sy = subtile_slab(thing->mappos.y.stl.num) - ey0;
                if ((sx < 0) || (sy < 0) || (sx >= DD_N) || (sy >= DD_N))
                    continue;
                ddt_traps[ddt_ntraps] = thing->index;
                ddt_traps_shots[ddt_ntraps] = thing->trap.num_shots;
                ddt_ntraps++;
            }
        }
        return;
    }
    MapSlabCoord ex0, ey0;
    if (!world_engine_slab(ddt_x0, ddt_y0, &ex0, &ey0))
    {
        world_load_chunks_round_world(ddt_x0 + DD_C, ddt_y0 + DD_C, 1);
        if (!world_engine_slab(ddt_x0, ddt_y0, &ex0, &ey0))
            return;
    }
    if (t == 700)
    {
        ddt_scan("after its chunks froze and came back");
        return;
    }
    if (t == 1500)
    {
        ddt_scan("at T+1500");
        return;
    }
    if ((t == 50) && (getenv("KFX_WORLD_TESTDOMINION_IMPS") != NULL))
    {
        /* the real way: a claimed slab of player 0's in the heart's chamber, the dominion shown to player 0, and four of
           its imps there: they claim what they can */
        const MapSlabCoord cx = ex0 + DD_C + 2, cy = ey0 + DD_C;
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(cx), slab_subtile_center(cy), PLAYER0, 0);
        for (int y = slab_subtile(ey0, 0); y <= slab_subtile(ey0 + DD_N - 1, 2); y++)
            for (int x = slab_subtile(ex0, 0); x <= slab_subtile(ex0 + DD_N - 1, 2); x++)
                reveal_map_subtile(x, y, PLAYER0);
        for (int i = 0; i < 4; i++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(cx));
            pos.y.val = subtile_coord_center(slab_subtile_center(cy));
            pos.z.val = 0;
            create_creature(&pos, (ThingModel)get_id(creature_desc, "IMP"), PLAYER0);
        }
        /* (its traps go: this is about claiming; the traps were tested on their own) */
        int gone = 0;
        for (int k = 0; k < ddt_ntraps; k++)
        {
            struct Thing *trap = thing_get(ddt_traps[k]);
            if (thing_exists(trap) && (trap->class_id == TCls_Trap))
            {
                delete_thing_structure(trap, 0);
                gone++;
            }
        }
        ddt_ntraps = 0;
        SYNCLOG("World test: dominion: four imps of player 0's in its heart chamber, on a slab of player 0's (%d traps taken away)", gone);
        return;
    }
    if (((t == 100) || (t == 200)) && (getenv("KFX_WORLD_TESTDOMINION_IMPS") != NULL))
        return;
    if (t == 50)
    {
        int taken = 0;
        for (int y = 0; y < DD_N; y++)
            for (int x = 0; x < DD_N; x++)
            {
                const struct SlabMap *slb = get_slabmap_block(ex0 + x, ey0 + y);
                if (slabmap_block_invalid(slb) || (slb->room_index <= 0))
                    continue;
                struct Room *room = room_get(slb->room_index);
                if (room_is_invalid(room) || (room->owner == PLAYER0))
                    continue;
                const TbBool could = is_room_available(PLAYER0, room->kind);
                take_over_room(room, PLAYER0);
                taken++;
                SYNCLOG("World test: dominion: player 0 takes a %s of %d slabs (could build it before: %s); it is now player %d's",
                    room_code_name(room->kind), (int)room->slabs_count, could ? "yes" : "no", (int)room->owner);
            }
        ddt_scan("after taking its rooms");
        return;
    }
    if (t == 100)
    {
        int put = 0;
        for (int k = 0; k < ddt_ntraps; k++)
        {
            struct Thing *trap = thing_get(ddt_traps[k]);
            if (!thing_exists(trap) || (trap->class_id != TCls_Trap))
                continue;
            struct Coord3d pos = trap->mappos;
            pos.z.val = 0;
            struct Thing *crtng = create_creature(&pos, (ThingModel)get_id(creature_desc, "BILE_DEMON"), PLAYER0);
            if (thing_exists(crtng))
            {
                init_creature_level(crtng, 9);
                put++;
            }
        }
        SYNCLOG("World test: dominion: %d creatures of player 0's stand on its %d traps", put, ddt_ntraps);
        return;
    }
    int fired = 0;
    for (int k = 0; k < ddt_ntraps; k++)
    {
        struct Thing *trap = thing_get(ddt_traps[k]);
        if (!thing_exists(trap) || (trap->class_id != TCls_Trap) || (trap->trap.num_shots != ddt_traps_shots[k]) ||
            (trap->trap.revealed != 0))
            fired++;
    }
    SYNCLOG("World test: dominion: %d of its %d traps went off", fired, ddt_ntraps);
}

/******************************************************************************/
/* saves: "DDM1" in XTR2: count, then per region rx, ry (int64), its grade (DD_NONE: none) and its place */
#define DD_SAVED 18
size_t world_dominions_save_size(void)
{
    return (dd_count > 0) ? 4 + (size_t)dd_count * DD_SAVED : 0;
}

void world_dominions_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)dd_count;
    memcpy(buf, &n, 4);
    buf += 4;
    for (int i = 0; i < dd_count; i++)
    {
        memcpy(buf, &dd_states[i].rx, 8);
        memcpy(buf + 8, &dd_states[i].ry, 8);
        buf[16] = dd_states[i].grade;
        buf[17] = dd_states[i].site;
        buf += DD_SAVED;
    }
}

TbBool world_dominions_load(const uint8_t *buf, size_t len)
{
    world_dominions_reset();
    uint32_t n;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if (n > (len - 4) / DD_SAVED)
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint8_t *b = buf + 4 + (size_t)i * DD_SAVED;
        int64_t rx, ry;
        memcpy(&rx, b, 8);
        memcpy(&ry, b + 8, 8);
        struct DdState *st = dd_state(rx, ry, true);
        if (st == NULL)
            return false;
        st->grade = ((b[16] >= 1) && (b[16] <= 5)) ? b[16] : DD_NONE; /* (a damaged record: no dominion there) */
        st->site = (uint8_t)min((int)b[17], DD_SITES - 1);
    }
    return true;
}

#ifdef __cplusplus
}
#endif

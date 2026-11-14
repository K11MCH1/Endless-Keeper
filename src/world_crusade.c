/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_crusade.c
 *     Endless world: the Crusade Road (2026-11-13; see world_crusade.h).
 * @par Purpose:
 *     Decisions (the user was away; the recommended choice each time):
 *     - Disturbed: a keeper broke into one of the fortress's portal rooms (one of its gates sent heroes).
 *     - How fast: a new outpost every two days of the world, one 96-slab region nearer to the nearest human keeper's
 *       heart, up to three.
 *     - Founding: the region is claimed for the road and gets an outpost when its ground is first made (a region a
 *       keeper has already explored keeps its ground: the road takes the region's own outpost if it has one, else
 *       that step is spent). The outpost is sized as any other when its ground is made.
 *     - Away and new: a road doesn't grow towards a keeper whose dungeon the caretaker keeps, nor in a dungeon's first
 *       three days; it waits. A step that would come within 100 slabs (plus a region's half) of any heart is not
 *       taken: the road waits there.
 *     - Answering: when an outpost on the road is broken into, every other outpost on it wakes and sends its heroes.
 *     - Razed: a keeper has claimed ground at the fortress's middle; the road dies (its outposts stay, and no longer
 *       answer one another).
 *     Saved: CRS1.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_crusade.h"
#include "world_outpost.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_caretaker.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_data.h"
#include "slab_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "post_inc.h"

#define CR_ROADS 64
#define CR_STEPS 3
#define CR_REGION 96                 /* world_outpost.c OP_REGION */
#define CR_EVERY_DAYS 2
#define CR_HEART_MIN 100
#define CR_NEW_DAYS 3

#pragma pack(1)
struct CrRoad {
    uint8_t used, dead, count, by;   /* by: 1 + the keeper who disturbed the fortress (0: not known, an older save) */
    int32_t fcx, fcy;                /* the fortress: its generator structure's start chunk */
    int64_t fx, fy;                  /* its middle, world slabs */
    int64_t lx, ly;                  /* the road's last place (the fortress, then its newest outpost) */
    int64_t rx[CR_STEPS], ry[CR_STEPS];
    int32_t next_day;
};
#pragma pack()
static struct CrRoad cr_roads[CR_ROADS];
static int cr_test = -1; /* KFX_WORLD_CRUSADETEST */

static int64_t cr_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (((a % b) != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

TbBool world_crusade_forced(int64_t rx, int64_t ry)
{
    for (int i = 0; i < CR_ROADS; i++)
    {
        if (!cr_roads[i].used || cr_roads[i].dead)
            continue;
        for (int k = 0; k < cr_roads[i].count; k++)
            if ((cr_roads[i].rx[k] == rx) && (cr_roads[i].ry[k] == ry))
                return true;
    }
    return false;
}

void world_crusade_outpost_alarm(int64_t rx, int64_t ry, int level)
{
    for (int i = 0; i < CR_ROADS; i++)
    {
        struct CrRoad *r = &cr_roads[i];
        if (!r->used || r->dead)
            continue;
        TbBool on = false;
        for (int k = 0; k < r->count; k++)
            on = on || ((r->rx[k] == rx) && (r->ry[k] == ry));
        if (!on)
            continue;
        SYNCLOG("World: the outpost of region (%lld,%lld) on the Crusade road of the fortress at chunk (%d,%d) is broken"
            " into: the road answers", (long long)rx, (long long)ry, (int)r->fcx, (int)r->fcy);
        for (int k = 0; k < r->count; k++)
            if ((r->rx[k] != rx) || (r->ry[k] != ry))
                world_outpost_answer(r->rx[k], r->ry[k], level);
    }
}

/* who disturbed a fortress before its road began (not saved: the road takes it within one look, 256 turns) */
#define CR_DISTURBED_MAX 32
static struct { int32_t fcx, fcy; int8_t by; } cr_disturbed[CR_DISTURBED_MAX];

static void cr_start_roads(long day);

void world_crusade_fortress_disturbed(int32_t fcx, int32_t fcy, PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || !world_player_is_human(plyr_idx))
        return;
    for (int i = 0; i < CR_ROADS; i++)
        if (cr_roads[i].used && (cr_roads[i].fcx == fcx) && (cr_roads[i].fcy == fcy))
            return; /* its road has its keeper already */
    int free_i = -1;
    for (int i = 0; i < CR_DISTURBED_MAX; i++)
    {
        if ((cr_disturbed[i].by != 0) && (cr_disturbed[i].fcx == fcx) && (cr_disturbed[i].fcy == fcy))
            return; /* the first keeper to disturb it */
        if ((cr_disturbed[i].by == 0) && (free_i < 0))
            free_i = i;
    }
    if (free_i < 0)
        free_i = (int)(game.play_gameturn % CR_DISTURBED_MAX);
    cr_disturbed[free_i].fcx = fcx;
    cr_disturbed[free_i].fcy = fcy;
    cr_disturbed[free_i].by = (int8_t)(plyr_idx + 1);
    SYNCLOG("World: player %d disturbs the fortress at chunk (%d,%d)", (int)plyr_idx, (int)fcx, (int)fcy);
    cr_start_roads(world_day_number()); /* its road begins now, with its keeper (cr_disturbed is not saved) */
}

/** The human keeper a road grows towards: the keeper who disturbed its fortress, while it is here and not new (else
 *  the road waits). A road from an older save, with nobody known: the nearest heart to its last place, as before. */
static PlayerNumber cr_target(const struct CrRoad *r, int64_t *hx, int64_t *hy)
{
    PlayerNumber best = PLAYER_NONE;
    double best_d = 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if ((r->by != 0) && (p != r->by - 1))
            continue;
        if (!world_player_is_human(p) || world_caretaker_active(p) || (!cr_test && (world_seat_day(p) <= CR_NEW_DAYS)))
            continue;
        struct Thing *heart = find_players_dungeon_heart(p);
        if (!thing_exists(heart))
            continue;
        int64_t wx, wy;
        world_slab_to_world(subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), &wx, &wy);
        const double d = sqrt((double)(wx - r->lx) * (wx - r->lx) + (double)(wy - r->ly) * (wy - r->ly));
        if ((best == PLAYER_NONE) || (d < best_d))
        {
            best = p;
            best_d = d;
            *hx = wx;
            *hy = wy;
        }
    }
    return best;
}

static TbBool cr_near_any_heart(int64_t x, int64_t y, double dist)
{
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!world_player_is_human(p))
            continue;
        struct Thing *heart = find_players_dungeon_heart(p);
        if (!thing_exists(heart))
            continue;
        int64_t wx, wy;
        world_slab_to_world(subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), &wx, &wy);
        if ((double)(wx - x) * (wx - x) + (double)(wy - y) * (wy - y) < dist * dist)
            return true;
    }
    return false;
}

static void cr_grow(struct CrRoad *r, long day)
{
    int64_t hx, hy;
    const PlayerNumber p = cr_target(r, &hx, &hy);
    if (p == PLAYER_NONE)
    {
        r->next_day = (int32_t)(day + 1); /* nobody to grow towards (away, new, none): it waits */
        return;
    }
    const double dx = (double)(hx - r->lx), dy = (double)(hy - r->ly);
    const double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0)
        return;
    const int64_t nx = r->lx + (int64_t)floor(dx / len * CR_REGION + 0.5), ny = r->ly + (int64_t)floor(dy / len * CR_REGION + 0.5);
    if (cr_near_any_heart(nx, ny, CR_HEART_MIN + CR_REGION / 2))
    {
        r->next_day = (int32_t)(day + 1); /* any nearer would come within 100 slabs of a heart: it waits there */
        return;
    }
    const int64_t rx = cr_floor_div(nx, CR_REGION), ry = cr_floor_div(ny, CR_REGION);
    const int state = world_outpost_region_state(rx, ry);
    r->lx = nx;
    r->ly = ny;
    r->next_day = (int32_t)(day + (cr_test ? 1 : CR_EVERY_DAYS));
    if (state == 0)
    {
        SYNCLOG("World: the Crusade road of the fortress at chunk (%d,%d) passes region (%lld,%lld): its ground is made"
            " without an outpost", (int)r->fcx, (int)r->fcy, (long long)rx, (long long)ry);
        r->count++; /* a step spent */
        r->rx[r->count - 1] = INT64_MIN;
        r->ry[r->count - 1] = INT64_MIN;
        return;
    }
    r->rx[r->count] = rx;
    r->ry[r->count] = ry;
    r->count++;
    SYNCLOG("World: the Crusade road of the fortress at chunk (%d,%d) %s an outpost in region (%lld,%lld), towards player"
        " %d (%d of %d)", (int)r->fcx, (int)r->fcy, (state == 1) ? "takes" : "founds", (long long)rx, (long long)ry, (int)p,
        (int)r->count, CR_STEPS);
    if (cr_test)
    {
        /* test: the region decided now, to see the outpost the road claimed */
        int64_t x0, y0, gx, gy, ex, ey;
        int n = 0, tier = -1;
        const TbBool got = world_stronghold_test_target(0, rx * CR_REGION + CR_REGION / 2, ry * CR_REGION + CR_REGION / 2, &x0,
            &y0, &n, &gx, &gy, &ex, &ey, &tier);
        SYNCLOG("World test: crusade: region (%lld,%lld) decided: %s (state %d)", (long long)rx, (long long)ry,
            got ? "an outpost" : "none", world_outpost_region_state(rx, ry));
    }
}

static TbBool cr_razed(const struct CrRoad *r)
{
    MapSlabCoord mx, my;
    if (!world_engine_slab(r->fx, r->fy, &mx, &my))
        return false;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
        {
            struct SlabMap *slb = get_slabmap_block(mx + dx, my + dy);
            if (slabmap_block_invalid(slb))
                continue;
            const PlayerNumber o = slabmap_owner(slb);
            if ((o < PLAYERS_MAX) && world_player_is_human(o))
                return true;
        }
    return false;
}

/** Test hook KFX_WORLD_CRUSADETEST=1: at turn 100 the fortress nearest the world start begins a road (as if disturbed),
 *  and roads grow every day, towards keepers from their first day. */
static void cr_test_start(void)
{
    if (cr_test < 0)
        cr_test = (getenv("KFX_WORLD_CRUSADETEST") != NULL) ? 1 : 0;
    if (!cr_test || (game.play_gameturn != 100))
        return;
    int64_t x0, y0, gx, gy, ex, ey;
    int n = 0, tier = 0;
    if (!world_stronghold_test_target(1, 0, 0, &x0, &y0, &n, &gx, &gy, &ex, &ey, &tier))
    {
        SYNCLOG("World test: crusade: no fortress near the start");
        return;
    }
    struct CrRoad *r = &cr_roads[0];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->fx = r->lx = x0 + n / 2;
    r->fy = r->ly = y0 + n / 2;
    r->next_day = (int32_t)world_day_number();
    r->by = 1; /* as if player 0 disturbed it */
    SYNCLOG("World test: crusade: the fortress at world slab (%lld,%lld) begins a road", (long long)r->fx, (long long)r->fy);
}

/** New roads: the fortresses disturbed that have none yet. */
static void cr_start_roads(long day)
{
    /* new roads: fortresses disturbed */
    int32_t fcx, fcy;
    int64_t cx, cy;
    TbBool disturbed;
    for (int n = 0; world_outpost_fortress_get(n, &fcx, &fcy, &cx, &cy, &disturbed); n++)
    {
        if (!disturbed)
            continue;
        int free_i = -1;
        TbBool have = false;
        for (int i = 0; i < CR_ROADS; i++)
        {
            if (!cr_roads[i].used)
            {
                if (free_i < 0)
                    free_i = i;
                continue;
            }
            have = have || ((cr_roads[i].fcx == fcx) && (cr_roads[i].fcy == fcy));
        }
        if (have || (free_i < 0))
            continue;
        struct CrRoad *r = &cr_roads[free_i];
        memset(r, 0, sizeof(*r));
        r->used = 1;
        r->fcx = fcx;
        r->fcy = fcy;
        r->fx = cx;
        r->fy = cy;
        r->lx = cx;
        r->ly = cy;
        r->next_day = (int32_t)(day + CR_EVERY_DAYS);
        for (int k = 0; k < CR_DISTURBED_MAX; k++)
            if ((cr_disturbed[k].by != 0) && (cr_disturbed[k].fcx == fcx) && (cr_disturbed[k].fcy == fcy))
            {
                r->by = (uint8_t)cr_disturbed[k].by;
                cr_disturbed[k].by = 0;
            }
        SYNCLOG("World: the fortress at chunk (%d,%d) was disturbed and stands: a Crusade road begins (its first outpost on"
            " day %d), towards player %d", (int)fcx, (int)fcy, (int)r->next_day, (int)r->by - 1);
    }
}

void world_crusade_turn(void)
{
    if (!world_mode_active())
        return;
    cr_test_start();
    if ((game.play_gameturn % 256) != 143)
        return;
    const long day = world_day_number();
    cr_start_roads(day);
    for (int i = 0; i < CR_ROADS; i++)
    {
        struct CrRoad *r = &cr_roads[i];
        if (!r->used || r->dead)
            continue;
        if (cr_razed(r))
        {
            r->dead = 1;
            SYNCLOG("World: the fortress at chunk (%d,%d) is razed: its Crusade road dies", (int)r->fcx, (int)r->fcy);
            continue;
        }
        if ((r->count < CR_STEPS) && (day >= r->next_day))
            cr_grow(r, day);
    }
}

void world_crusade_reset(void)
{
    memset(cr_roads, 0, sizeof(cr_roads));
    memset(cr_disturbed, 0, sizeof(cr_disturbed));
}

size_t world_crusade_save_size(void)
{
    for (int i = 0; i < CR_ROADS; i++)
        if (cr_roads[i].used)
            return sizeof(cr_roads);
    return 0;
}

void world_crusade_save(uint8_t *buf)
{
    memcpy(buf, cr_roads, sizeof(cr_roads));
}

TbBool world_crusade_load(const uint8_t *buf, size_t len)
{
    world_crusade_reset();
    if (len < sizeof(cr_roads))
        return false;
    memcpy(cr_roads, buf, sizeof(cr_roads));
    return true;
}

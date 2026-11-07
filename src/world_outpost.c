/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_outpost.c
 *     Endless world: heroes outposts (see world_outpost.h).
 * @par Purpose:
 *     Design (2026-11-07): "Players need to explore to discover Heroes outposts. Heroes outposts are procedurally
 *     generated, are smaller than fortresses but heavily trapped. 21 square tile sized with rooms, can be
 *     symmetrical and not, but there is always 1 entrance and the opposite end side of the entrance (or at the end
 *     of the outpost if its asymmetrical) should be a Heroes heart." The heart drops a spellbook; breaking the
 *     iron entrance door makes every portal send 4+ heroes (Barbarian, Archer, Monk, Thief); the heart's portal
 *     sends 7+ and a Knight once a keeper's creature is within 2 slabs of it. Hero levels follow the strongest
 *     creature of the keeper that woke them.
 *
 *     Placement: the world is cut into 96x96-slab regions; a region holds an outpost or not (seeded), somewhere
 *     inside it, away from the start, the generator's structures and the other players' starts. A plan is a pure
 *     function of the seed and the region, so every machine and every load sees the same outpost; the generated
 *     chunks get its slabs, doors, traps and portals (world_stream's generate_chunk_data). What happened to it
 *     (entrance broken, heart woken, book dropped) is saved (OPS1).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_outpost.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "world_stream.h"
#include "world_gen.h"
#include "world_wild.h"
#include "world_progress.h"
#include "bflib_basics.h"
#include "config.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_trapdoor.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_hero.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define OP_N OUTPOST_SIZE
#define OP_REGION 96            /* one outpost at most per 96x96 slabs (6x6 chunks) */
#define OP_CHANCE 55            /* percent of regions with one */
#define OP_START_DIST 70        /* slabs from the world's start at least */
#define OP_PATH 6               /* earth dug out from the entrance */
#define OP_MARGIN 4             /* slabs kept clear of the generator's structures */
#define OP_MAX_GATES 8
#define OP_MAX_TRAPS 20
#define OP_MAX_LOOT 6
#define OP_CACHE 256 /* direct-mapped by region */
#define OP_WAKE_DIST 2          /* slabs from the heart's portal that wake it */
#define OP_SEEK_DIST 30         /* slabs from the entrance a keeper's creature is looked for when it breaks */

enum OpCell { OpC_Rock = 0, OpC_Wall, OpC_Floor, OpC_Lava, OpC_Causeway, OpC_Heart, OpC_Room, OpC_Door };

struct OpPlan {
    int64_t rx, ry;
    int64_t x0, y0;                    /* world slab of the box's corner */
    uint8_t dir;                       /* the entrance's side: 0 north, 1 east, 2 south, 3 west */
    uint8_t cell[OP_N * OP_N];
    uint8_t room[OP_N * OP_N];         /* the slab kind of an OpC_Room cell */
    int ex, ey;                        /* the entrance door (local) */
    int odx, ody;                      /* outward from the entrance */
    int hx, hy;                        /* the heart pedestal's centre (local) */
    int ngates;                        /* gate 0 is the heart's */
    uint8_t gx[OP_MAX_GATES], gy[OP_MAX_GATES];
    int ntraps;
    uint8_t tx[OP_MAX_TRAPS], ty[OP_MAX_TRAPS];
    uint16_t tmodel[OP_MAX_TRAPS];
    int nloot;
    uint8_t lx[OP_MAX_LOOT], ly[OP_MAX_LOOT];
};

enum OpFlags {
    OPF_Opened = 0x01,                 /* the entrance door is broken: the portals woke */
    OPF_HeartWoken = 0x02,             /* the heart's portal woke */
    OPF_Looted = 0x04,                 /* the heart fell and dropped its book */
    OPF_DoorSeen = 0x08,               /* its iron door stood: it was really built (a door missing counts after this) */
};

struct OpState {
    int64_t rx, ry;
    uint8_t flags;
    uint8_t level;                     /* the heroes' level, from the keeper that broke in */
    uint8_t gates_done;                /* bit g: gate g sent its heroes */
};

static struct OpState *op_states;
static int op_state_count, op_state_cap;
/* a world saved before the outposts has ground explored without them: it gets none (saved: OPS1 is always written) */
static TbBool op_on = true;

static struct {
    TbBool valid, has;
    int64_t rx, ry;
    int stamps;
    struct OpPlan plan;
} op_cache[OP_CACHE];
static int op_cache_next;

/******************************************************************************/
struct OpRng {
    uint64_t s;
};

static uint64_t op_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int op_rand(struct OpRng *r, int n)
{
    r->s = op_mix(r->s);
    return (n > 0) ? (int)(r->s % (uint64_t)n) : 0;
}

static int64_t op_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (((a % b) != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

#define OPI(x, y) ((y) * OP_N + (x))

static void op_fill(uint8_t *cell, uint8_t *room, int x1, int y1, int x2, int y2, uint8_t c, uint8_t rk)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            if ((x >= 1) && (x < OP_N - 1) && (y >= 1) && (y < OP_N - 1))
            {
                cell[OPI(x, y)] = c;
                room[OPI(x, y)] = rk;
            }
}

static TbBool op_open(uint8_t c)
{
    return (c != OpC_Rock) && (c != OpC_Wall);
}

/* hero rooms for the side rooms */
static const uint8_t op_room_kinds[] = {SlbT_TRAINING, SlbT_LIBRARY, SlbT_BARRACKS, SlbT_GUARDPOST, SlbT_TREASURE,
    SlbT_PRISON, SlbT_TORTURE, SlbT_TEMPLE, SlbT_LAIR};
#define OP_ROOM_KINDS ((int)(sizeof(op_room_kinds) / sizeof(op_room_kinds[0])))

/* the traps, by weight */
static const struct { const char *name; int weight; } op_traps[] = {
    {"LIGHTNING", 3}, {"POISON_GAS", 3}, {"WORD_OF_POWER", 2}, {"BOULDER", 2}, {"LAVA", 2}, {"SENTRY", 1}, {"ALARM", 1},
};
#define OP_TRAP_KINDS ((int)(sizeof(op_traps) / sizeof(op_traps[0])))

/** The layout with the entrance at the top (row 0) and the heart at the bottom; things as canonical positions. */
static void op_build(struct OpRng *rng, struct OpPlan *p)
{
    uint8_t *cell = p->cell, *room = p->room;
    memset(cell, OpC_Rock, sizeof(p->cell));
    memset(room, 0, sizeof(p->room));
    p->ngates = 1; /* gate 0: the heart's, set below */
    p->ntraps = 0;
    p->nloot = 0;
    const TbBool sym = (op_rand(rng, 100) < 55);
    const int hx = sym ? 10 : (op_rand(rng, 2) ? 5 : 15);
    /* the entrance, its hall and the spine */
    cell[OPI(10, 0)] = OpC_Door;
    op_fill(cell, room, 8, 1, 12, 3, OpC_Floor, 0);
    op_fill(cell, room, 10, 4, 10, 10, OpC_Floor, 0);
    op_fill(cell, room, 10, 10, hx, 10, OpC_Floor, 0);
    /* a lava strip before the heart, crossed by a causeway */
    if (op_rand(rng, 100) < 60)
        op_fill(cell, room, 1, 11, OP_N - 2, 11, OpC_Lava, 0);
    cell[OPI(hx, 11)] = OpC_Causeway;
    cell[OPI(hx, 12)] = OpC_Door;
    /* the heart room, the heart's portal just inside its door */
    op_fill(cell, room, hx - 3, 13, hx + 3, OP_N - 2, OpC_Floor, 0);
    op_fill(cell, room, hx - 1, 15, hx + 1, 17, OpC_Heart, 0);
    p->gx[0] = (uint8_t)hx;
    p->gy[0] = 13;
    p->hx = hx;
    p->hy = 16;
    /* chambers beside the heart room, each with a portal */
    for (int s = -1; s <= 1; s += 2)
    {
        const int dx = hx + s * 4;
        if ((dx < 2) || (dx > OP_N - 3))
            continue;
        if (sym ? (op_rand(rng, 100) >= 50) : (s == ((hx < 10) ? -1 : 1)))
            continue; /* symmetric: half the time; asymmetric: the open side only */
        int w = sym ? 4 : 4 + op_rand(rng, 5);
        int x2 = hx + s * (4 + w);
        if (x2 < 1)
            x2 = 1;
        if (x2 > OP_N - 2)
            x2 = OP_N - 2;
        if (abs(x2 - (dx + s)) < 2)
            continue;
        cell[OPI(dx, 16)] = OpC_Door;
        op_fill(cell, room, dx + s, 14, x2, 18, OpC_Floor, 0);
        if (p->ngates < OP_MAX_GATES)
        {
            p->gx[p->ngates] = (uint8_t)((dx + s + x2) / 2);
            p->gy[p->ngates] = 16;
            p->ngates++;
        }
    }
    /* side rooms off the spine, each with a portal; mirrored when symmetric */
    for (int s = -1; s <= 1; s += 2)
    {
        if (sym && (s == 1))
            break;
        const int count = 1 + op_rand(rng, 2);
        for (int k = 0; k < count; k++)
        {
            const int top = 2 + k * 4 + ((count == 1) ? op_rand(rng, 4) : 0);
            const int w = 3 + op_rand(rng, 2), d = 4 + op_rand(rng, 2);
            const uint8_t rk = op_room_kinds[op_rand(rng, OP_ROOM_KINDS)];
            const int ymid = top + 1;
            for (int m = 0; m < (sym ? 2 : 1); m++)
            {
                const int ss = sym ? ((m == 0) ? -1 : 1) : s;
                const int edge = 10 + ss * d;                 /* the room's side towards the spine */
                const int x1 = edge, x2 = edge + ss * (w - 1);
                op_fill(cell, room, x1, top, x2, top + 2, OpC_Room, rk);
                cell[OPI(edge - ss, ymid)] = OpC_Door;
                op_fill(cell, room, edge - 2 * ss, ymid, 10 + ss, ymid, OpC_Floor, 0);
                if (p->ngates < OP_MAX_GATES)
                {
                    p->gx[p->ngates] = (uint8_t)((x1 + x2) / 2);
                    p->gy[p->ngates] = (uint8_t)ymid;
                    p->ngates++;
                }
                if ((rk == SlbT_TREASURE) && (p->nloot < OP_MAX_LOOT))
                {
                    p->lx[p->nloot] = (uint8_t)x2;
                    p->ly[p->nloot] = (uint8_t)top;
                    p->nloot++;
                }
            }
        }
    }
    /* walls round everything open; the rest stays rock */
    uint8_t solid[OP_N * OP_N];
    for (int i = 0; i < OP_N * OP_N; i++)
        solid[i] = !op_open(cell[i]);
    for (int y = 0; y < OP_N; y++)
        for (int x = 0; x < OP_N; x++)
        {
            if (!solid[OPI(x, y)])
                continue;
            TbBool near = false;
            for (int dy = -1; (dy <= 1) && !near; dy++)
                for (int dx = -1; (dx <= 1) && !near; dx++)
                {
                    const int nx = x + dx, ny = y + dy;
                    if ((nx >= 0) && (nx < OP_N) && (ny >= 0) && (ny < OP_N) && !solid[OPI(nx, ny)] &&
                        (cell[OPI(nx, ny)] != OpC_Lava))
                        near = true;
                }
            cell[OPI(x, y)] = near ? OpC_Wall : OpC_Rock;
        }
    /* lava touches rock as it likes, but its ends against the outer ring are walls too */
    for (int y = 0; y < OP_N; y++)
        for (int x = 0; x < OP_N; x++)
            if ((cell[OPI(x, y)] == OpC_Rock) && ((x == 0) || (y == 0) || (x == OP_N - 1) || (y == OP_N - 1)))
            {
                TbBool lava = false;
                for (int k = 0; k < 4; k++)
                {
                    static const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
                    const int nx = x + ddx[k], ny = y + ddy[k];
                    if ((nx >= 0) && (nx < OP_N) && (ny >= 0) && (ny < OP_N) && (cell[OPI(nx, ny)] == OpC_Lava))
                        lava = true;
                }
                if (lava)
                    cell[OPI(x, y)] = OpC_Wall;
            }
    /* traps on the floors: not by a door, not on a portal */
    int cand_x[OP_N * OP_N], cand_y[OP_N * OP_N], ncand = 0;
    for (int y = 1; y < OP_N - 1; y++)
        for (int x = 1; x < OP_N - 1; x++)
        {
            if ((cell[OPI(x, y)] != OpC_Floor) || (y <= 1))
                continue;
            TbBool bad = false;
            for (int g = 0; g < p->ngates; g++)
                if ((p->gx[g] == x) && (p->gy[g] == y))
                    bad = true;
            static const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++)
                if (cell[OPI(x + ddx[k], y + ddy[k])] == OpC_Door)
                    bad = true;
            if (bad)
                continue;
            cand_x[ncand] = x;
            cand_y[ncand] = y;
            ncand++;
        }
    int trap_models[OP_TRAP_KINDS], trap_w[OP_TRAP_KINDS], ntk = 0, wsum = 0;
    for (int k = 0; k < OP_TRAP_KINDS; k++)
    {
        const long m = get_id(trap_desc, op_traps[k].name);
        if (m > 0)
        {
            trap_models[ntk] = (int)m;
            trap_w[ntk] = op_traps[k].weight;
            wsum += op_traps[k].weight;
            ntk++;
        }
    }
    int want = 12 + op_rand(rng, 7);
    for (int t = 0; (t < want) && (ncand > 0) && (ntk > 0) && (p->ntraps < OP_MAX_TRAPS); t++)
    {
        const int c = op_rand(rng, ncand);
        int r = op_rand(rng, wsum), k = 0;
        while ((k < ntk - 1) && (r >= trap_w[k]))
            r -= trap_w[k++];
        p->tx[p->ntraps] = (uint8_t)cand_x[c];
        p->ty[p->ntraps] = (uint8_t)cand_y[c];
        p->tmodel[p->ntraps] = (uint16_t)trap_models[k];
        p->ntraps++;
        cand_x[c] = cand_x[ncand - 1];
        cand_y[c] = cand_y[ncand - 1];
        ncand--;
    }
}

static void op_rot(int dir, int x, int y, int *rx, int *ry)
{
    switch (dir & 3)
    {
    case 1: *rx = OP_N - 1 - y; *ry = x; break;
    case 2: *rx = OP_N - 1 - x; *ry = OP_N - 1 - y; break;
    case 3: *rx = y; *ry = OP_N - 1 - x; break;
    default: *rx = x; *ry = y; break;
    }
}

/** Turns a canonical plan so its entrance faces dir. */
static void op_turn(struct OpPlan *p, int dir)
{
    static uint8_t cell[OP_N * OP_N], room[OP_N * OP_N];
    memcpy(cell, p->cell, sizeof(cell));
    memcpy(room, p->room, sizeof(room));
    for (int y = 0; y < OP_N; y++)
        for (int x = 0; x < OP_N; x++)
        {
            int X, Y;
            op_rot(dir, x, y, &X, &Y);
            p->cell[OPI(X, Y)] = cell[OPI(x, y)];
            p->room[OPI(X, Y)] = room[OPI(x, y)];
        }
    int X, Y;
    op_rot(dir, 10, 0, &X, &Y);
    p->ex = X;
    p->ey = Y;
    static const int odx[4] = {0, 1, 0, -1}, ody[4] = {-1, 0, 1, 0};
    p->odx = odx[dir & 3];
    p->ody = ody[dir & 3];
    op_rot(dir, p->hx, p->hy, &X, &Y);
    p->hx = X;
    p->hy = Y;
    for (int g = 0; g < p->ngates; g++)
    {
        op_rot(dir, p->gx[g], p->gy[g], &X, &Y);
        p->gx[g] = (uint8_t)X;
        p->gy[g] = (uint8_t)Y;
    }
    for (int t = 0; t < p->ntraps; t++)
    {
        op_rot(dir, p->tx[t], p->ty[t], &X, &Y);
        p->tx[t] = (uint8_t)X;
        p->ty[t] = (uint8_t)Y;
    }
    for (int l = 0; l < p->nloot; l++)
    {
        op_rot(dir, p->lx[l], p->ly[l], &X, &Y);
        p->lx[l] = (uint8_t)X;
        p->ly[l] = (uint8_t)Y;
    }
    p->dir = (uint8_t)(dir & 3);
}

/** The box an outpost takes with its way in (world slabs). */
static void op_box(const struct OpPlan *p, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    *x1 = p->x0;
    *y1 = p->y0;
    *x2 = p->x0 + OP_N - 1;
    *y2 = p->y0 + OP_N - 1;
    const int64_t px = p->x0 + p->ex + (int64_t)p->odx * OP_PATH, py = p->y0 + p->ey + (int64_t)p->ody * OP_PATH;
    if (px < *x1) *x1 = px;
    if (py < *y1) *y1 = py;
    if (px > *x2) *x2 = px;
    if (py > *y2) *y2 = py;
}

/** The outpost of a region, if it has one. */
static TbBool op_plan_make(int64_t rx, int64_t ry, struct OpPlan *p)
{
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return false;
    struct OpRng rng = {wg_world_seed(gen) ^ op_mix((uint64_t)rx * 0x51ED27F1ULL) ^ op_mix((uint64_t)ry * 0x2545F491ULL + 0x4F555450ULL)};
    if (op_rand(&rng, 100) >= OP_CHANCE)
        return false;
    memset(p, 0, sizeof(*p));
    p->rx = rx;
    p->ry = ry;
    const int span = OP_REGION - OP_N - 2 * (OP_PATH + 2);
    p->x0 = rx * OP_REGION + OP_PATH + 2 + op_rand(&rng, span);
    p->y0 = ry * OP_REGION + OP_PATH + 2 + op_rand(&rng, span);
    /* away from the world's start (its heart at slab 8,8) */
    const int64_t cx = p->x0 + OP_N / 2 - 8, cy = p->y0 + OP_N / 2 - 8;
    if (cx * cx + cy * cy < (int64_t)OP_START_DIST * OP_START_DIST)
        return false;
    op_build(&rng, p);
    op_turn(p, op_rand(&rng, 4));
    int64_t x1, y1, x2, y2;
    op_box(p, &x1, &y1, &x2, &y2);
    /* not on the generator's keepers, fortresses, portals or reward caves, nor on another player's start */
    struct WgStructRef refs[2];
    const unsigned mask = (1u << WgT_Keeper) | (1u << WgT_Fortress) | (1u << WgT_Portal) | (1u << WgT_Reward);
    if (wg_structures_in(gen, x1 - OP_MARGIN, y1 - OP_MARGIN, x2 + OP_MARGIN, y2 + OP_MARGIN, mask, refs, 2) > 0)
        return false;
    if (world_stamps_overlap(x1 - OP_MARGIN, y1 - OP_MARGIN, x2 + OP_MARGIN, y2 + OP_MARGIN))
        return false;
    return true;
}

static const struct OpPlan *op_plan(int64_t rx, int64_t ry)
{
    const int stamps = world_stamp_count();
    const int i = (int)(op_mix((uint64_t)rx * 0x9E3779B1ULL ^ ((uint64_t)ry << 32)) % OP_CACHE);
    if (op_cache[i].valid && (op_cache[i].rx == rx) && (op_cache[i].ry == ry) && (op_cache[i].stamps == stamps))
        return op_cache[i].has ? &op_cache[i].plan : NULL;
    op_cache[i].valid = true;
    op_cache[i].rx = rx;
    op_cache[i].ry = ry;
    op_cache[i].stamps = stamps;
    op_cache[i].has = op_plan_make(rx, ry, &op_cache[i].plan);
    return op_cache[i].has ? &op_cache[i].plan : NULL;
}

/******************************************************************************/
/* the chunks */
static void op_overlay_thing(struct OutpostOverlay *ov, int32_t cx, int32_t cy, int64_t wx, int64_t wy, uint8_t oclass,
    uint16_t model, uint8_t heroes, uint16_t extra)
{
    if ((wx < (int64_t)cx * WG_CHUNK) || (wx >= (int64_t)cx * WG_CHUNK + WG_CHUNK) || (wy < (int64_t)cy * WG_CHUNK) ||
        (wy >= (int64_t)cy * WG_CHUNK + WG_CHUNK) || (ov->thing_count >= OUTPOST_OVERLAY_THINGS) || (model == 0))
        return;
    struct OutpostOverlayThing *t = &ov->things[ov->thing_count++];
    t->oclass = oclass;
    t->heroes = heroes;
    t->model = model;
    t->x = (int32_t)(((wx - (int64_t)cx * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    t->y = (int32_t)(((wy - (int64_t)cy * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    t->extra = extra;
}

int world_outpost_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    memset(ov, 0, sizeof(*ov));
    if ((world_generator() == NULL) || !op_on)
        return 0;
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    const ThingModel steel = (ThingModel)get_id(door_desc, "STEEL");
    const ThingModel gate = (ThingModel)get_id(object_desc, "HERO_GATE");
    const ThingModel chest = (ThingModel)get_id(object_desc, "GOLD_CHEST");
    int decided = 0;
    for (int64_t ry = op_floor_div(wy1 - OP_PATH, OP_REGION); ry <= op_floor_div(wy2 + OP_PATH, OP_REGION); ry++)
    {
        for (int64_t rx = op_floor_div(wx1 - OP_PATH, OP_REGION); rx <= op_floor_div(wx2 + OP_PATH, OP_REGION); rx++)
        {
            const struct OpPlan *p = op_plan(rx, ry);
            if (p == NULL)
                continue;
            int64_t bx1, by1, bx2, by2;
            op_box(p, &bx1, &by1, &bx2, &by2);
            if ((bx2 < wx1) || (bx1 > wx2) || (by2 < wy1) || (by1 > wy2))
                continue;
            for (int y = 0; y < OP_N; y++)
                for (int x = 0; x < OP_N; x++)
                {
                    const int64_t gx = p->x0 + x, gy = p->y0 + y;
                    if ((gx < wx1) || (gx > wx2) || (gy < wy1) || (gy > wy2))
                        continue;
                    const int i = (int)((gy - wy1) * WG_CHUNK + (gx - wx1));
                    const uint8_t c = p->cell[OPI(x, y)];
                    ov->set[i] = 1;
                    ov->heroes[i] = 1;
                    ov->wall[i] = 0;
                    ov->door[i] = 0;
                    switch (c)
                    {
                    case OpC_Wall: ov->kind[i] = SlbT_WALLDRAPE; ov->wall[i] = 1; break;
                    case OpC_Floor: case OpC_Causeway: ov->kind[i] = SlbT_CLAIMED; break;
                    case OpC_Lava: ov->kind[i] = SlbT_LAVA; ov->heroes[i] = 0; break;
                    case OpC_Heart: ov->kind[i] = SlbT_DUNGHEART; break;
                    case OpC_Room: ov->kind[i] = p->room[OPI(x, y)]; break;
                    case OpC_Door: ov->kind[i] = SlbT_CLAIMED; ov->door[i] = (uint8_t)((steel > 0) ? steel : 3); break;
                    default: ov->kind[i] = SlbT_ROCK; ov->heroes[i] = 0; break;
                    }
                    decided++;
                }
            /* the way in: earth from the door outwards */
            for (int k = 1; k <= OP_PATH; k++)
            {
                const int64_t gx = p->x0 + p->ex + (int64_t)p->odx * k, gy = p->y0 + p->ey + (int64_t)p->ody * k;
                if ((gx < wx1) || (gx > wx2) || (gy < wy1) || (gy > wy2))
                    continue;
                const int i = (int)((gy - wy1) * WG_CHUNK + (gx - wx1));
                ov->set[i] = 1;
                ov->kind[i] = SlbT_EARTH;
                ov->heroes[i] = 0;
                ov->wall[i] = 0;
                ov->door[i] = 0;
                decided++;
            }
            for (int g = 0; g < p->ngates; g++)
                op_overlay_thing(ov, cx, cy, p->x0 + p->gx[g], p->y0 + p->gy[g], TCls_Object, (uint16_t)max(gate, 0), 1, 0);
            for (int t = 0; t < p->ntraps; t++)
            {
                const struct TrapConfigStats *trapst = get_trap_model_stats(p->tmodel[t]);
                op_overlay_thing(ov, cx, cy, p->x0 + p->tx[t], p->y0 + p->ty[t], TCls_Trap, p->tmodel[t], 1,
                    (uint16_t)max(trapst->shots, 1));
            }
            for (int l = 0; l < p->nloot; l++)
                op_overlay_thing(ov, cx, cy, p->x0 + p->lx[l], p->y0 + p->ly[l], TCls_Object, (uint16_t)max(chest, 0), 0, 0);
        }
    }
    return decided;
}

TbBool world_outpost_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    if ((world_generator() == NULL) || !op_on)
        return false;
    for (int64_t ry = op_floor_div(y1 - OP_PATH, OP_REGION); ry <= op_floor_div(y2 + OP_PATH, OP_REGION); ry++)
        for (int64_t rx = op_floor_div(x1 - OP_PATH, OP_REGION); rx <= op_floor_div(x2 + OP_PATH, OP_REGION); rx++)
        {
            const struct OpPlan *p = op_plan(rx, ry);
            if (p == NULL)
                continue;
            int64_t bx1, by1, bx2, by2;
            op_box(p, &bx1, &by1, &bx2, &by2);
            if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
                return true;
        }
    return false;
}

TbBool world_outpost_nearest(int64_t wx, int64_t wy, int64_t *x0, int64_t *y0, int64_t *ex, int64_t *ey, int *odx,
    int *ody, int64_t *hx, int64_t *hy, int64_t *gx, int64_t *gy)
{
    static struct OpPlan found; /* a copy: the cache moves on while the regions are looked at */
    const struct OpPlan *best = NULL;
    int64_t best_d = 0;
    const int64_t rx0 = op_floor_div(wx, OP_REGION), ry0 = op_floor_div(wy, OP_REGION);
    for (int64_t ry = ry0 - 3; ry <= ry0 + 3; ry++)
        for (int64_t rx = rx0 - 3; rx <= rx0 + 3; rx++)
        {
            const struct OpPlan *p = op_plan(rx, ry);
            if (p == NULL)
                continue;
            const int64_t dx = p->x0 + OP_N / 2 - wx, dy = p->y0 + OP_N / 2 - wy;
            if ((best == NULL) || (dx * dx + dy * dy < best_d))
            {
                found = *p;
                best = &found;
                best_d = dx * dx + dy * dy;
            }
        }
    if (best == NULL)
        return false;
    *x0 = best->x0;
    *y0 = best->y0;
    *ex = best->x0 + best->ex;
    *ey = best->y0 + best->ey;
    *odx = best->odx;
    *ody = best->ody;
    *hx = best->x0 + best->hx;
    *hy = best->y0 + best->hy;
    *gx = best->x0 + best->gx[0];
    *gy = best->y0 + best->gy[0];
    SYNCLOG("World test: outpost at slab (%lld,%lld), entrance side %d, %d portals, %d traps", (long long)best->x0,
        (long long)best->y0, (int)best->dir, best->ngates, best->ntraps);
    return true;
}

/******************************************************************************/
/* what happened to them */
static struct OpState *op_state(int64_t rx, int64_t ry, TbBool add)
{
    for (int i = 0; i < op_state_count; i++)
        if ((op_states[i].rx == rx) && (op_states[i].ry == ry))
            return &op_states[i];
    if (!add)
        return NULL;
    if (op_state_count >= op_state_cap)
    {
        const int ncap = (op_state_cap > 0) ? op_state_cap * 2 : 32;
        struct OpState *n = (struct OpState *)realloc(op_states, (size_t)ncap * sizeof(*n));
        if (n == NULL)
            return NULL;
        op_states = n;
        op_state_cap = ncap;
    }
    struct OpState *st = &op_states[op_state_count++];
    memset(st, 0, sizeof(*st));
    st->rx = rx;
    st->ry = ry;
    return st;
}

void world_outposts_reset(void)
{
    free(op_states);
    op_states = NULL;
    op_state_count = op_state_cap = 0;
    memset(op_cache, 0, sizeof(op_cache));
    op_cache_next = 0;
    op_on = true;
}

void world_outposts_set_enabled(TbBool on)
{
    op_on = on;
}

/** The highest hero level allowed (100 with the max-level patch's rule, else 10). */
static int op_level_cap(void)
{
#ifdef CREATURE_MAX_LEVEL_LIMIT
    return (int)creature_max_levels_for_owner(PLAYER_GOOD);
#else
    return CREATURE_MAX_LEVEL;
#endif
}

static TbBool op_keeper_owner(PlayerNumber owner)
{
    return (owner >= 0) && (owner < PLAYERS_COUNT) && (owner != PLAYER_GOOD) && (owner != PLAYER_NEUTRAL) &&
        (owner != game.neutral_player_num) && player_is_keeper(owner);
}

static int op_spawn(MapSlabCoord sx, MapSlabCoord sy, const char *name, int level, struct OpRng *rng)
{
    const long model = get_id(creature_desc, name);
    if ((model <= 0) || !creature_count_below_map_limit(0))
        return 0;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(sx)) + (MapCoord)(op_rand(rng, 161) - 80);
    pos.y.val = subtile_coord_center(slab_subtile_center(sy)) + (MapCoord)(op_rand(rng, 161) - 80);
    pos.z.val = 0;
    struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, (ThingModel)model,
        PLAYER_GOOD);
    if (thing_is_invalid(thing))
        return 0;
    const int cap = op_level_cap();
    init_creature_level(thing, (CrtrExpLevel)(max(1, min(level, cap)) - 1));
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (!creature_control_invalid(cctrl))
    {
        cctrl->party.objective = CHeroTsk_DefendRooms;
        cctrl->party.original_objective = CHeroTsk_DefendRooms; /* not wild: they stay through the day */
        cctrl->party.target_plyr_idx = -1;
    }
    return 1;
}

static const char *const op_hero_names[4] = {"BARBARIAN", "ARCHER", "MONK", "THIEF"};

/** A portal sends n heroes (random kinds of the four) at levels round the given one, and a Knight if asked. */
static int op_gate_spawn(const struct OpPlan *p, int g, int n, TbBool knight, int level, struct OpRng *rng)
{
    MapSlabCoord sx, sy;
    if (!world_engine_slab(p->x0 + p->gx[g], p->y0 + p->gy[g], &sx, &sy))
        return -1;
    int made = 0;
    for (int i = 0; i < n; i++)
        made += op_spawn(sx, sy, op_hero_names[op_rand(rng, 4)], level - 1 + op_rand(rng, 3), rng);
    if (knight)
        made += op_spawn(sx, sy, "KNIGHT", level + 2, rng);
    return made;
}

/** The keeper creature nearest a slab within dist (Chebyshev, slabs), or NULL. */
static struct Thing *op_keeper_creature_near(MapSlabCoord sx, MapSlabCoord sy, int dist)
{
    struct Thing *best = NULL;
    long best_d = LONG_MAX;
    const MapSubtlCoord cx = slab_subtile_center(sx), cy = slab_subtile_center(sy);
    const int r = dist * STL_PER_SLB + 1;
    for (MapSubtlCoord y = cy - r; y <= cy + r; y++)
        for (MapSubtlCoord x = cx - r; x <= cx + r; x++)
        {
            if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                continue;
            long i = get_mapwho_thing_index(get_map_block_at(x, y));
            long guard = 0;
            while ((i > 0) && (guard++ < THINGS_COUNT))
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if (!thing_is_creature(thing) || !op_keeper_owner(thing->owner) || creature_is_being_unconscious(thing))
                    continue;
                const long d = labs((long)x - cx) + labs((long)y - cy);
                if (d < best_d)
                {
                    best_d = d;
                    best = thing;
                }
            }
        }
    return best;
}

/** The keeper whose creature is nearest the entrance (a wide search, once); else the strongest human keeper. */
static int op_breaker_level(MapSlabCoord sx, MapSlabCoord sy)
{
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    const struct Thing *best = NULL;
    long best_d = LONG_MAX, guard = 0;
    const MapSubtlCoord cx = slab_subtile_center(sx), cy = slab_subtile_center(sy);
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        const struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!op_keeper_owner(thing->owner))
            continue;
        const long d = max(labs((long)thing->mappos.x.stl.num - cx), labs((long)thing->mappos.y.stl.num - cy));
        if ((d <= OP_SEEK_DIST * STL_PER_SLB) && (d < best_d))
        {
            best_d = d;
            best = thing;
        }
    }
    if (best != NULL)
        return world_keeper_top_level(best->owner);
    int top = 1;
    for (PlayerNumber pl = 0; pl < PLAYERS_COUNT; pl++)
        if (world_player_is_human(pl) && player_exists(get_player(pl)))
            top = max(top, world_keeper_top_level(pl));
    return top;
}

static void op_runtime(const struct OpPlan *p)
{
    struct OpState *st = op_state(p->rx, p->ry, false);
    uint8_t flags = (st != NULL) ? st->flags : 0;
    struct OpRng rng = {op_mix(world_seed() ^ ((uint64_t)game.play_gameturn << 20) ^ (uint64_t)p->rx * 31 ^ (uint64_t)p->ry)};
    /* the entrance: its iron door gone (broken, or taken) */
    if (!(flags & OPF_Opened))
    {
        MapSlabCoord sx, sy;
        if (world_engine_slab(p->x0 + p->ex, p->y0 + p->ey, &sx, &sy))
        {
            const struct Thing *door = get_door_for_position(slab_subtile_center(sx), slab_subtile_center(sy));
            const TbBool standing = thing_exists(door) && (door->owner == PLAYER_GOOD);
            if (standing && !(flags & OPF_DoorSeen))
            {
                st = op_state(p->rx, p->ry, true);
                if (st == NULL)
                    return;
                st->flags |= OPF_DoorSeen;
                flags = st->flags;
            }
            if (!standing && (flags & OPF_DoorSeen))
            {
                st = op_state(p->rx, p->ry, true);
                if (st == NULL)
                    return;
                st->flags |= OPF_Opened;
                st->level = (uint8_t)max(1, min(op_breaker_level(sx, sy), 255));
                flags = st->flags;
                SYNCLOG("World: the heroes outpost at slab (%lld,%lld) is broken into; its portals wake (heroes level %d)",
                    (long long)p->x0, (long long)p->y0, (int)st->level);
            }
        }
    }
    /* the portals (not the heart's) send their heroes, each once, when its ground is on the map */
    if ((flags & OPF_Opened) && (st != NULL))
    {
        for (int g = 1; g < p->ngates; g++)
        {
            if (st->gates_done & (1u << g))
                continue;
            const int made = op_gate_spawn(p, g, 4 + op_rand(&rng, 3), false, st->level, &rng);
            if (made < 0)
                continue;
            st->gates_done |= (uint8_t)(1u << g);
            SYNCLOG("World: outpost portal %d sends %d heroes", g, made);
        }
    }
    /* the heart's portal: a keeper's creature within 2 slabs of it (of an outpost that stood) */
    if (!(flags & OPF_HeartWoken) && (flags & OPF_DoorSeen))
    {
        MapSlabCoord sx, sy;
        if (world_engine_slab(p->x0 + p->gx[0], p->y0 + p->gy[0], &sx, &sy))
        {
            const struct Thing *near = op_keeper_creature_near(sx, sy, OP_WAKE_DIST);
            if (near != NULL)
            {
                st = op_state(p->rx, p->ry, true);
                if (st == NULL)
                    return;
                st->flags |= OPF_HeartWoken;
                st->gates_done |= 1u;
                const int level = world_keeper_top_level(near->owner);
                const int made = op_gate_spawn(p, 0, 7 + op_rand(&rng, 3), true, level, &rng);
                SYNCLOG("World: player %d's creatures reach the outpost heart's portal: %d heroes and a Knight (level %d)",
                    (int)near->owner, max(made - 1, 0), level);
            }
        }
    }
}

#define OP_LOADED_MAX 16384  /* loaded chunks looked at (more than any world loads) */
#define OP_SEEN_SIZE 4096    /* the regions of a pass, an open-addressing set */
void world_outposts_turn(void)
{
    if (!world_mode_active() || !op_on || ((game.play_gameturn % 8) != 3))
        return;
    static int32_t ccx[OP_LOADED_MAX], ccy[OP_LOADED_MAX];
    static int64_t seen_x[OP_SEEN_SIZE], seen_y[OP_SEEN_SIZE];
    static uint32_t seen_pass[OP_SEEN_SIZE], pass;
    static int64_t order_x[OP_SEEN_SIZE], order_y[OP_SEEN_SIZE];
    pass++;
    const int n = world_loaded_chunks(ccx, ccy, OP_LOADED_MAX);
    int nseen = 0;
    for (int c = 0; c < n; c++)
    {
        const int64_t wx1 = (int64_t)ccx[c] * WG_CHUNK, wy1 = (int64_t)ccy[c] * WG_CHUNK;
        for (int64_t ry = op_floor_div(wy1, OP_REGION); ry <= op_floor_div(wy1 + WG_CHUNK - 1, OP_REGION); ry++)
            for (int64_t rx = op_floor_div(wx1, OP_REGION); rx <= op_floor_div(wx1 + WG_CHUNK - 1, OP_REGION); rx++)
            {
                uint32_t h = (uint32_t)(op_mix((uint64_t)rx * 0x9E3779B1ULL ^ ((uint64_t)ry << 32)) % OP_SEEN_SIZE);
                TbBool dup = false;
                for (int probe = 0; probe < OP_SEEN_SIZE; probe++, h = (h + 1) % OP_SEEN_SIZE)
                {
                    if (seen_pass[h] != pass)
                        break;
                    if ((seen_x[h] == rx) && (seen_y[h] == ry))
                    {
                        dup = true;
                        break;
                    }
                }
                if (dup || (nseen >= OP_SEEN_SIZE / 2))
                    continue;
                seen_pass[h] = pass;
                seen_x[h] = rx;
                seen_y[h] = ry;
                order_x[nseen] = rx;
                order_y[nseen] = ry;
                nseen++;
            }
    }
    for (int k = 0; k < nseen; k++)
    {
        const struct OpPlan *p = op_plan(order_x[k], order_y[k]);
        if (p != NULL)
            op_runtime(p);
    }
}

void world_outpost_heart_destroyed(const struct Thing *heartng)
{
    if (!world_mode_active() || !thing_exists(heartng) || (heartng->owner != PLAYER_GOOD))
        return;
    int64_t wx, wy;
    world_slab_to_world(subtile_slab(heartng->mappos.x.stl.num), subtile_slab(heartng->mappos.y.stl.num), &wx, &wy);
    const struct OpPlan *p = op_plan(op_floor_div(wx, OP_REGION), op_floor_div(wy, OP_REGION));
    if ((p == NULL) || (llabs(wx - (p->x0 + p->hx)) > 1) || (llabs(wy - (p->y0 + p->hy)) > 1))
        return;
    struct OpState *st = op_state(p->rx, p->ry, true);
    if ((st == NULL) || (st->flags & OPF_Looted))
        return;
    st->flags |= OPF_Looted;
    SYNCLOG("World: the heroes outpost at slab (%lld,%lld) falls: its heart drops a spellbook", (long long)p->x0, (long long)p->y0);
    world_progress_drop_outpost_book(heartng->mappos.x.stl.num, heartng->mappos.y.stl.num);
}

/******************************************************************************/
/* saving */
#pragma pack(1)
struct OpSaved {
    int64_t rx, ry;
    uint8_t flags, level, gates_done;
};
#pragma pack()

size_t world_outposts_save_size(void)
{
    return op_on ? 4 + (size_t)op_state_count * sizeof(struct OpSaved) : 0;
}

void world_outposts_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)op_state_count;
    memcpy(buf, &n, 4);
    buf += 4;
    for (int i = 0; i < op_state_count; i++)
    {
        struct OpSaved s = {op_states[i].rx, op_states[i].ry, op_states[i].flags, op_states[i].level, op_states[i].gates_done};
        memcpy(buf, &s, sizeof(s));
        buf += sizeof(s);
    }
}

TbBool world_outposts_load(const uint8_t *buf, size_t len)
{
    world_outposts_reset();
    uint32_t n;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if (n > (len - 4) / sizeof(struct OpSaved))
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        struct OpSaved s;
        memcpy(&s, buf + 4 + (size_t)i * sizeof(s), sizeof(s));
        struct OpState *st = op_state(s.rx, s.ry, true);
        if (st == NULL)
            return false;
        st->flags = s.flags;
        st->level = s.level;
        st->gates_done = s.gates_done;
    }
    op_on = true;
    return true;
}

#ifdef __cplusplus
}
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_outpost.c
 *     Endless world: heroes outposts and hero fortresses (see world_outpost.h).
 * @par Purpose:
 *     Design (2026-11-07): "Players need to explore to discover Heroes outposts. Heroes outposts are procedurally
 *     generated, are smaller than fortresses but heavily trapped. 21 square tile sized with rooms, can be
 *     symmetrical and not, but there is always 1 entrance and the opposite end side of the entrance (or at the end
 *     of the outpost if its asymmetrical) should be a Heroes heart."
 *
 *     Sizes (2026-11-08): outposts and fortresses come in five sizes, each with its own layout, picked by the best
 *     creature level of the keeper nearest it when its ground is first made: Small (levels 1-3), Moderate (4-7),
 *     Medium (8-11), Large (12-19) and Massive (20 and up). Small and Moderate ones keep the old rules: a fortress has
 *     its guards waiting inside; breaking an outpost's iron entrance makes every portal send heroes, coming near its
 *     heart's portal makes that one send more and a Knight. From Medium up, each room with a hero portal sends its
 *     heroes (any kind, and a Knight) when a keeper's creature breaks into it. Traps and heroes follow the levels of
 *     the keeper that found them. A fallen outpost heart drops 1-4 heroes' spellbooks, a fortress holds 1-4 research
 *     books (by size). Their heroes roam: anywhere they can walk, out of their stronghold too.
 *
 *     Placement: outposts: the world is cut into 96x96-slab regions; a region holds an outpost or not (seeded),
 *     somewhere inside it, away from the start, the generator's structures and the other players' starts.
 *     Fortresses stand where the generator put its fortresses, over them. The size is decided once, when the first
 *     chunk of its ground is generated, and saved; a stronghold whose ground was made before this (a world from an
 *     older game) keeps what it had: the old 21x21 outpost or the generator's fortress ("legacy"). What happened to
 *     them (entrance broken, portals woken, book dropped) is saved (OPS block, version 2).
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
#include "world_daynight.h"
#include "world_circles.h"
#include "bflib_basics.h"
#include "bflib_math.h"
#include "config.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_hero.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "game_merge.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_stats.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define SH_N_MAX 47             /* the largest box (Massive) */
#define LEGACY_N 21             /* the outposts before the sizes */
#define OP_REGION 96            /* one outpost at most per 96x96 slabs (6x6 chunks) */
#define OP_CHANCE 55            /* percent of regions with one */
#define OP_START_DIST 70        /* slabs from the world's start at least */
#define OP_PATH 6               /* earth dug out from an outpost's entrance */
#define OP_MARGIN 4             /* slabs kept clear of the generator's structures */
#define FT_REACH (SH_N_MAX / 2 + 2) /* slabs from a fortress's middle its ground can reach */
#define SH_MAX_GATES 12
#define SH_MAX_TRAPS 48
#define SH_MAX_ITEMS 96
#define OP_CACHE 256 /* direct-mapped by region */
#define OP_WAKE_DIST 2          /* slabs from the heart's portal that wake it (Small, Moderate, legacy) */
#define OP_SEEK_DIST 30         /* slabs from the entrance a keeper's creature is looked for when it breaks */
#define SH_WANDER_MIN 6         /* slabs a roaming hero walks at least */
#define SH_WANDER_MAX 28        /* and at most */

enum ShFamily { ShF_Outpost = 0, ShF_Fortress = 1 };
enum ShTier { ShT_Small = 0, ShT_Moderate, ShT_Medium, ShT_Large, ShT_Massive, ShT_COUNT, ShT_Legacy = 6, ShT_None = 7 };
static const char *const sh_tier_names[8] = {"small", "moderate", "medium", "large", "massive", "?", "legacy", "none"};
static const int sh_outpost_n[ShT_COUNT] = {19, 25, 31, 39, 47};
static const int sh_fortress_n[ShT_COUNT] = {17, 23, 31, 39, 47};

enum OpCell { OpC_Rock = 0, OpC_Wall, OpC_Floor, OpC_Lava, OpC_Causeway, OpC_Heart, OpC_Room, OpC_Door, OpC_Water, OpC_Earth,
    OpC_Path, OpC_None, OpC_Hall };
/* OpC_None: the land's own ground (the sized strongholds' cells they don't use); OpC_Hall: a claimed hall, walled except
   over lava or water (a causeway there) */

enum ShItemKind { ShI_Object = 0, ShI_Guard, ShI_Book };
struct ShItem {
    uint8_t x, y, kind, level;
    uint16_t model, extra;
};

enum ShGateFlags { ShG_Breach = 0x01, ShG_Heart = 0x02, ShG_Decor = 0x04 };

struct OpPlan {
    uint8_t family, tier;
    int64_t rx, ry;                    /* outposts: the region; fortresses: the generator structure's start chunk */
    int64_t x0, y0;                    /* world slab of the box's corner */
    int n;                             /* the box's side */
    int fill;                          /* fortresses: the box round the middle (half side) whose cells it decides */
    uint8_t dir;                       /* the entrance's side: 0 north, 1 east, 2 south, 3 west */
    uint8_t cell[SH_N_MAX * SH_N_MAX];
    uint8_t room[SH_N_MAX * SH_N_MAX]; /* the slab kind of an OpC_Room cell */
    uint8_t ground[SH_N_MAX * SH_N_MAX]; /* the land's own ground under the box (WgSlab; sized strongholds) */
    uint8_t biome;                     /* the biome at its middle */
    int nexits;                        /* ways out at the box's edge (rock beyond is dug to earth exlen slabs) */
    uint8_t exx[4], exy[4], exdir[4], exlen[4];
    int ex, ey;                        /* the entrance door (local; outposts) */
    int odx, ody;                      /* outward from the entrance */
    int hx, hy;                        /* the heart pedestal's centre / the fortress's middle (local) */
    int ngates;                        /* outposts: gate 0 is the heart's */
    uint8_t gx[SH_MAX_GATES], gy[SH_MAX_GATES], gflags[SH_MAX_GATES];
    uint8_t grx1[SH_MAX_GATES], gry1[SH_MAX_GATES], grx2[SH_MAX_GATES], gry2[SH_MAX_GATES]; /* the gate's room */
    int ntraps;
    uint8_t tx[SH_MAX_TRAPS], ty[SH_MAX_TRAPS];
    uint16_t tmodel[SH_MAX_TRAPS];
    int nitems;
    struct ShItem items[SH_MAX_ITEMS];
};

enum OpFlags {
    OPF_Opened = 0x01,                 /* the entrance door is broken: the portals woke */
    OPF_HeartWoken = 0x02,             /* the heart's portal woke */
    OPF_Looted = 0x04,                 /* the heart fell and dropped its books */
    OPF_DoorSeen = 0x08,               /* its iron door stood: it was really built (a door missing counts after this) */
};

struct OpState {
    uint8_t family;
    int64_t rx, ry;
    int64_t cx, cy;                    /* the middle (world slab) */
    uint8_t tier;
    uint8_t flags;
    uint8_t level;                     /* the heroes' level, from the keeper that broke in */
    uint8_t found_level;               /* the finder's best creature level when its ground was made */
    uint16_t gates_done;               /* bit g: gate g sent its heroes */
};

static struct OpState *op_states;
static int op_state_count, op_state_cap;
/* a world saved before the outposts has ground explored without them: it gets none (saved: OPS is always written) */
static TbBool op_on = true;

static struct {
    TbBool valid, has;
    uint8_t family, tier, found_level;
    int64_t rx, ry;
    int stamps;
    struct OpPlan plan;
} op_cache[OP_CACHE];

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

#define OPI(p, x, y) ((y) * (p)->n + (x))

static TbBool sh_in(const struct OpPlan *p, int x, int y)
{
    return (x >= 0) && (y >= 0) && (x < p->n) && (y < p->n);
}

static void sh_fill(struct OpPlan *p, int x1, int y1, int x2, int y2, uint8_t c, uint8_t rk)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            if (sh_in(p, x, y))
            {
                p->cell[OPI(p, x, y)] = c;
                p->room[OPI(p, x, y)] = rk;
            }
}

static void sh_set(struct OpPlan *p, int x, int y, uint8_t c)
{
    sh_fill(p, x, y, x, y, c, 0);
}

static uint8_t sh_at(const struct OpPlan *p, int x, int y)
{
    return sh_in(p, x, y) ? p->cell[OPI(p, x, y)] : OpC_Rock;
}

static int sh_gate(struct OpPlan *p, int x, int y, uint8_t flags, int rx1, int ry1, int rx2, int ry2)
{
    if (p->ngates >= SH_MAX_GATES)
        return -1;
    const int g = p->ngates++;
    p->gx[g] = (uint8_t)x;
    p->gy[g] = (uint8_t)y;
    p->gflags[g] = flags;
    p->grx1[g] = (uint8_t)min(rx1, rx2);
    p->gry1[g] = (uint8_t)min(ry1, ry2);
    p->grx2[g] = (uint8_t)max(rx1, rx2);
    p->gry2[g] = (uint8_t)max(ry1, ry2);
    return g;
}

static void sh_item(struct OpPlan *p, int x, int y, uint8_t kind, uint16_t model, uint8_t level, uint16_t extra)
{
    if ((p->nitems >= SH_MAX_ITEMS) || (model == 0) || !sh_in(p, x, y))
        return;
    struct ShItem *it = &p->items[p->nitems++];
    it->x = (uint8_t)x;
    it->y = (uint8_t)y;
    it->kind = kind;
    it->model = model;
    it->level = level;
    it->extra = extra;
}

static TbBool sh_item_at(const struct OpPlan *p, int x, int y)
{
    for (int i = 0; i < p->nitems; i++)
        if ((p->items[i].x == x) && (p->items[i].y == y))
            return true;
    return false;
}

static TbBool sh_gate_at(const struct OpPlan *p, int x, int y)
{
    for (int g = 0; g < p->ngates; g++)
        if ((p->gx[g] == x) && (p->gy[g] == y))
            return true;
    return false;
}

static TbBool sh_trap_at(const struct OpPlan *p, int x, int y)
{
    for (int t = 0; t < p->ntraps; t++)
        if ((p->tx[t] == x) && (p->ty[t] == y))
            return true;
    return false;
}

/* hero rooms */
static const uint8_t op_room_kinds[] = {SlbT_TRAINING, SlbT_LIBRARY, SlbT_BARRACKS, SlbT_GUARDPOST, SlbT_TREASURE,
    SlbT_PRISON, SlbT_TORTURE, SlbT_TEMPLE, SlbT_LAIR};
#define OP_ROOM_KINDS ((int)(sizeof(op_room_kinds) / sizeof(op_room_kinds[0])))

static long sh_object_model(const char *name)
{
    const long m = get_id(object_desc, name);
    return (m > 0) ? m : 0;
}

/** Walls on the rock touching open floor (diagonals too); then outposts keep the rest as rock, fortresses make their
 *  leftover inside walls too. Lava and water touch rock as they like, but their ends against the box's edge get walls. */
static TbBool sh_walls_attract(uint8_t c)
{
    return (c == OpC_Floor) || (c == OpC_Causeway) || (c == OpC_Heart) || (c == OpC_Room) || (c == OpC_Door);
}

static void sh_walls(struct OpPlan *p, TbBool fortress)
{
    static uint8_t near[SH_N_MAX * SH_N_MAX];
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
        {
            near[OPI(p, x, y)] = 0;
            if (p->cell[OPI(p, x, y)] != OpC_Rock)
                continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (sh_walls_attract(sh_at(p, x + dx, y + dy)) && sh_in(p, x + dx, y + dy))
                        near[OPI(p, x, y)] = 1;
        }
    for (int i = 0; i < p->n * p->n; i++)
        if (near[i])
            p->cell[i] = OpC_Wall;
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
        {
            if (p->cell[OPI(p, x, y)] != OpC_Rock)
                continue;
            if (fortress)
            {
                p->cell[OPI(p, x, y)] = OpC_Wall;
                continue;
            }
            if ((x != 0) && (y != 0) && (x != p->n - 1) && (y != p->n - 1))
                continue;
            static const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++)
            {
                const uint8_t c = sh_at(p, x + ddx[k], y + ddy[k]);
                if ((c == OpC_Lava) || ((c == OpC_Water) && sh_in(p, x + ddx[k], y + ddy[k])))
                    p->cell[OPI(p, x, y)] = OpC_Wall;
            }
        }
}

/* the traps, by weight: no Lava, Sentry or Ballista traps (2026-11-08) */
static const struct { const char *name; int weight; } op_traps[] = {
    {"LIGHTNING", 3}, {"POISON_GAS", 3}, {"WORD_OF_POWER", 2}, {"BOULDER", 2}, {"ALARM", 1},
};
#define OP_TRAP_KINDS ((int)(sizeof(op_traps) / sizeof(op_traps[0])))

/* the legacy outposts' traps, as they were (their layout stays the same on every load) */
static const struct { const char *name; int weight; } op_traps_legacy[] = {
    {"LIGHTNING", 3}, {"POISON_GAS", 3}, {"WORD_OF_POWER", 2}, {"BOULDER", 2}, {"LAVA", 2}, {"SENTRY", 1}, {"ALARM", 1},
};
#define OP_TRAP_KINDS_LEGACY ((int)(sizeof(op_traps_legacy) / sizeof(op_traps_legacy[0])))

/** Traps on the floors: not by a door, not on a portal or a thing, not on the first row inside an outpost. */
static void sh_traps(struct OpPlan *p, struct OpRng *rng, int want, TbBool legacy)
{
    static int cand_x[SH_N_MAX * SH_N_MAX], cand_y[SH_N_MAX * SH_N_MAX];
    int ncand = 0;
    for (int y = 1; y < p->n - 1; y++)
        for (int x = 1; x < p->n - 1; x++)
        {
            const uint8_t tc = p->cell[OPI(p, x, y)];
            const uint8_t tg = p->ground[OPI(p, x, y)];
            const TbBool hall = (tc == OpC_Hall) && (tg != WgS_Lava) && (tg != WgS_Water);
            if (((tc != OpC_Floor) && !hall) || ((p->family == ShF_Outpost) && (y <= 1)))
                continue;
            TbBool bad = sh_gate_at(p, x, y) || (!legacy && sh_item_at(p, x, y));
            static const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++)
                if (sh_at(p, x + ddx[k], y + ddy[k]) == OpC_Door)
                    bad = true;
            if (bad)
                continue;
            cand_x[ncand] = x;
            cand_y[ncand] = y;
            ncand++;
        }
    const int kinds = legacy ? OP_TRAP_KINDS_LEGACY : OP_TRAP_KINDS;
    int trap_models[8], trap_w[8], ntk = 0, wsum = 0;
    for (int k = 0; k < kinds; k++)
    {
        const char *name = legacy ? op_traps_legacy[k].name : op_traps[k].name;
        const int weight = legacy ? op_traps_legacy[k].weight : op_traps[k].weight;
        const long m = get_id(trap_desc, name);
        if (m > 0)
        {
            trap_models[ntk] = (int)m;
            trap_w[ntk] = weight;
            wsum += weight;
            ntk++;
        }
    }
    for (int t = 0; (t < want) && (ncand > 0) && (ntk > 0) && (p->ntraps < SH_MAX_TRAPS); t++)
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

/* the decorations, by weight: lanterns, banners and statues along the walls */
static const struct { const char *name; int weight; } sh_decor_outpost[] = {
    {"LANTERN_PST", 3}, {"BANNER", 3}, {"CANDLESTCK", 2}, {"STATUE3", 1}, {"STATUE4", 1}, {"GUARDFLAG_WHITE", 1},
};
static const struct { const char *name; int weight; } sh_decor_fortress[] = {
    {"BANNER", 3}, {"LANTERN_PST", 3}, {"GUARDFLAG_WHITE", 2}, {"STATUE2", 1}, {"STATUE5", 1}, {"BARREL", 1},
};

static void sh_decorate(struct OpPlan *p, struct OpRng *rng, int want)
{
    static int cand_x[SH_N_MAX * SH_N_MAX], cand_y[SH_N_MAX * SH_N_MAX];
    int ncand = 0;
    static const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
    for (int y = 1; y < p->n - 1; y++)
        for (int x = 1; x < p->n - 1; x++)
        {
            if ((p->cell[OPI(p, x, y)] != OpC_Floor) && (p->cell[OPI(p, x, y)] != OpC_Hall))
                continue;
            TbBool wall = false, door = false;
            for (int k = 0; k < 4; k++)
            {
                const uint8_t c = sh_at(p, x + ddx[k], y + ddy[k]);
                wall = wall || (c == OpC_Wall);
                door = door || (c == OpC_Door);
            }
            if (!wall || door || sh_gate_at(p, x, y) || sh_item_at(p, x, y) || sh_trap_at(p, x, y))
                continue;
            cand_x[ncand] = x;
            cand_y[ncand] = y;
            ncand++;
        }
    const int kinds = (p->family == ShF_Fortress) ? 6 : 6;
    long models[6];
    int w[6], wsum = 0, nm = 0;
    for (int k = 0; k < kinds; k++)
    {
        const char *name = (p->family == ShF_Fortress) ? sh_decor_fortress[k].name : sh_decor_outpost[k].name;
        const int weight = (p->family == ShF_Fortress) ? sh_decor_fortress[k].weight : sh_decor_outpost[k].weight;
        const long m = sh_object_model(name);
        if (m > 0)
        {
            models[nm] = m;
            w[nm] = weight;
            wsum += weight;
            nm++;
        }
    }
    static const char *const biome_decor[WgB_COUNT][2] = {
        [WgB_Earthen] = {"FERN", "MUSHROOM_YELLOW"}, [WgB_Rocky] = {"ROCK_PILLAR", "ROCK"},
        [WgB_Volcanic] = {"LAVA_PILLAR", "LAVA_ROCK"}, [WgB_Flooded] = {"CATTAILS", "MUSHROOM_GREEN"},
        [WgB_Frozen] = {"ICE_PILLAR", "ICE_ROCK"}, [WgB_Sandstone] = {"ROCK_PILLAR", "FERN_BROWN"},
        [WgB_Crystal] = {"ROCK_PILLAR", "MUSHROOM_RED"}, [WgB_Ruins] = {"GRAVE_STONE", "STATUE_HORNY"},
        [WgB_LavaOcean] = {"LAVA_PILLAR", "LAVA_ROCK"}, [WgB_FloodedOcean] = {"CATTAILS", "FERN_SMALL"},
        [WgB_LavaRivers] = {"LAVA_PILLAR", "LAVA_ROCK"}, [WgB_FloodedRivers] = {"CATTAILS", "FERN_SMALL"},
        [WgB_DeadMans] = {"GRAVE_STONE", "ROCK"},
    };
    for (int d = 0; (d < want) && (ncand > 0) && (nm > 0); d++)
    {
        const int c = op_rand(rng, ncand);
        int r = op_rand(rng, wsum), k = 0;
        while ((k < nm - 1) && (r >= w[k]))
            r -= w[k++];
        long model = models[k];
        if ((p->tier != ShT_Legacy) && (p->biome < WgB_COUNT) && (biome_decor[p->biome][0] != NULL) && (op_rand(rng, 3) == 0))
        {
            const long bm = sh_object_model(biome_decor[p->biome][op_rand(rng, 2)]);
            if (bm > 0)
                model = bm;
        }
        sh_item(p, cand_x[c], cand_y[c], ShI_Object, (uint16_t)model, 0, 0);
        /* keep them apart: drop the neighbours from the candidates */
        const int px = cand_x[c], py = cand_y[c];
        for (int j = 0; j < ncand;)
        {
            if ((abs(cand_x[j] - px) <= 1) && (abs(cand_y[j] - py) <= 1))
            {
                cand_x[j] = cand_x[ncand - 1];
                cand_y[j] = cand_y[ncand - 1];
                ncand--;
            } else
                j++;
        }
    }
}

/** Random free floor cells in a box (for loot, guards, books). Returns how many were found (up to max). */
static int sh_free_floor(const struct OpPlan *p, struct OpRng *rng, int x1, int y1, int x2, int y2, int *xs, int *ys, int max)
{
    static int cx[SH_N_MAX * SH_N_MAX], cy[SH_N_MAX * SH_N_MAX];
    int n = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            if (sh_in(p, x, y) && ((p->cell[OPI(p, x, y)] == OpC_Floor) || (p->cell[OPI(p, x, y)] == OpC_Hall)) &&
                !sh_gate_at(p, x, y) && !sh_item_at(p, x, y))
            {
                cx[n] = x;
                cy[n] = y;
                n++;
            }
    int got = 0;
    while ((n > 0) && (got < max))
    {
        const int k = op_rand(rng, n);
        xs[got] = cx[k];
        ys[got] = cy[k];
        got++;
        cx[k] = cx[n - 1];
        cy[k] = cy[n - 1];
        n--;
    }
    return got;
}

/******************************************************************************/
/* the outposts' layouts, entrance at the top (row 0), heart at the far end */

/** The outpost before the sizes (21x21), exactly as it was. */
static void op_build_legacy(struct OpRng *rng, struct OpPlan *p)
{
    p->n = LEGACY_N;
    memset(p->cell, OpC_Rock, sizeof(p->cell));
    memset(p->room, 0, sizeof(p->room));
    p->ngates = 1; /* gate 0: the heart's, set below */
    p->ntraps = 0;
    p->nitems = 0;
    const int N = LEGACY_N;
    const TbBool sym = (op_rand(rng, 100) < 55);
    const int hx = sym ? 10 : (op_rand(rng, 2) ? 5 : 15);
    sh_set(p, 10, 0, OpC_Door);
    sh_fill(p, 8, 1, 12, 3, OpC_Floor, 0);
    sh_fill(p, 10, 4, 10, 10, OpC_Floor, 0);
    sh_fill(p, 10, 10, hx, 10, OpC_Floor, 0);
    if (op_rand(rng, 100) < 60)
        sh_fill(p, 1, 11, N - 2, 11, OpC_Lava, 0);
    sh_set(p, hx, 11, OpC_Causeway);
    sh_set(p, hx, 12, OpC_Door);
    sh_fill(p, hx - 3, 13, hx + 3, N - 2, OpC_Floor, 0);
    sh_fill(p, hx - 1, 15, hx + 1, 17, OpC_Heart, 0);
    p->gx[0] = (uint8_t)hx;
    p->gy[0] = 13;
    p->gflags[0] = ShG_Heart;
    p->grx1[0] = (uint8_t)(hx - 3); p->gry1[0] = 13; p->grx2[0] = (uint8_t)(hx + 3); p->gry2[0] = (uint8_t)(N - 2);
    p->hx = hx;
    p->hy = 16;
    for (int s = -1; s <= 1; s += 2)
    {
        const int dx = hx + s * 4;
        if ((dx < 2) || (dx > N - 3))
            continue;
        if (sym ? (op_rand(rng, 100) >= 50) : (s == ((hx < 10) ? -1 : 1)))
            continue;
        int w = sym ? 4 : 4 + op_rand(rng, 5);
        int x2 = hx + s * (4 + w);
        if (x2 < 1)
            x2 = 1;
        if (x2 > N - 2)
            x2 = N - 2;
        if (abs(x2 - (dx + s)) < 2)
            continue;
        sh_set(p, dx, 16, OpC_Door);
        sh_fill(p, dx + s, 14, x2, 18, OpC_Floor, 0);
        sh_gate(p, (dx + s + x2) / 2, 16, 0, dx + s, 14, x2, 18);
    }
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
                const int edge = 10 + ss * d;
                const int x1 = edge, x2 = edge + ss * (w - 1);
                sh_fill(p, x1, top, x2, top + 2, OpC_Room, rk);
                sh_set(p, edge - ss, ymid, OpC_Door);
                sh_fill(p, edge - 2 * ss, ymid, 10 + ss, ymid, OpC_Floor, 0);
                sh_gate(p, (x1 + x2) / 2, ymid, 0, x1, top, x2, top + 2);
                if (rk == SlbT_TREASURE)
                    sh_item(p, x2, top, ShI_Object, (uint16_t)sh_object_model("GOLD_CHEST"), 0, 0);
            }
        }
    }
    sh_walls(p, false);
    sh_traps(p, rng, 12 + op_rand(rng, 7), true);
}

/******************************************************************************/
/* the sized strongholds' layouts (2026-11-08): rooms, towers, keeps and courts of many shapes (circles, half and
 * quarter circles, triangles, diamonds, octagons, crosses, rectangles), joined by halls - long ones with rooms off
 * their sides and wings at their ends - laid into the land as it is: what the stronghold doesn't use stays the
 * biome's own ground, halls cross lava and water on causeways, moats are the biome's liquid, ruins crumble.
 * Canonical: an outpost's entrance at the top (row 0), its heart at the far end; a fortress round the middle. */

enum ShShape { ShS_Rect = 0, ShS_Circle, ShS_Octagon, ShS_Diamond, ShS_Cross, ShS_Semi, ShS_HalfOct, ShS_Tri,
    ShS_TriTip, ShS_Quarter, ShS_COUNT };

static TbBool sh_liquid_ground(uint8_t g)
{
    return (g == WgS_Lava) || (g == WgS_Water);
}

/** The stronghold's moat liquid: the biome's own. */
static uint8_t sh_biome_liquid(int biome)
{
    switch (biome)
    {
    case WgB_Volcanic: case WgB_LavaOcean: case WgB_LavaRivers: return OpC_Lava;
    default: return OpC_Water;
    }
}

/* canonical frame of a shape: its door side at the top (y' = -1 is outside), the shape going down (y' >= 0) */
static void sh_rot4(int d, int x, int y, int *rx, int *ry)
{
    switch (d & 3)
    {
    case 1: *rx = -y; *ry = x; break;   /* door side east */
    case 2: *rx = -x; *ry = -y; break;  /* door side south */
    case 3: *rx = y; *ry = -x; break;   /* door side west */
    default: *rx = x; *ry = y; break;   /* door side north */
    }
}

/** Is (u, v) (canonical, origin at the shape's anchor) inside the shape of size r (rect: w x h)? */
static TbBool sh_shape_in(int shape, int r, int w, int h, int u, int v)
{
    const int au = abs(u);
    switch (shape)
    {
    case ShS_Rect: return (u >= -(w / 2)) && (u < w - w / 2) && (v >= 0) && (v < h);
    case ShS_Circle: { const int cv = v - r; return au * au + cv * cv <= r * r + r; }
    case ShS_Octagon: { const int av = abs(v - r); return (au <= r) && (av <= r) && (au + av <= r + r / 2); }
    case ShS_Diamond: return au + abs(v - r) <= r;
    case ShS_Cross: { const int av = abs(v - r), t = max(1, r / 3); return ((au <= t) && (av <= r)) || ((av <= t) && (au <= r)); }
    case ShS_Semi: return (v >= 0) && (au * au + v * v <= r * r + r);
    case ShS_HalfOct: return (v >= 0) && (au <= r) && (v <= r) && (au + v <= r + r / 2);
    case ShS_Tri: return (v >= 0) && (v <= r) && (au <= r - v);
    case ShS_TriTip: return (v >= 0) && (v <= r) && (au <= v);
    case ShS_Quarter: return (v >= 0) && (u >= 0) && (u * u + v * v <= r * r + r);
    default: return false;
    }
}

/** The bounding box of a shape in canonical coordinates. */
static void sh_shape_box(int shape, int r, int w, int h, int *u1, int *v1, int *u2, int *v2)
{
    switch (shape)
    {
    case ShS_Rect: *u1 = -(w / 2); *u2 = w - w / 2 - 1; *v1 = 0; *v2 = h - 1; break;
    case ShS_Quarter: *u1 = 0; *u2 = r; *v1 = 0; *v2 = r; break;
    case ShS_Semi: case ShS_HalfOct: case ShS_Tri: case ShS_TriTip: *u1 = -r; *u2 = r; *v1 = 0; *v2 = r; break;
    default: *u1 = -r; *u2 = r; *v1 = 0; *v2 = 2 * r; break;
    }
}

struct ShRoom {
    int shape, r, w, h, face;          /* face: the side its door is on (0 N, 1 E, 2 S, 3 W) */
    int ax, ay;                        /* the anchor: the cell just inside the door */
    int x1, y1, x2, y2;                /* its box */
    int mx, my;                        /* a cell well inside (gate, heart) */
};

/** The cells of a placed shape: anchor (ax, ay) is the shape's cell just inside its door, the door one step out
 *  towards face. */
static TbBool sh_room_cell(const struct ShRoom *rm, int x, int y)
{
    int u, v;
    /* inverse of sh_rot4: the offset from the anchor in the canonical frame */
    const int dx = x - rm->ax, dy = y - rm->ay;
    switch (rm->face & 3)
    {
    case 1: u = dy; v = -dx; break;
    case 2: u = -dx; v = -dy; break;
    case 3: u = -dy; v = dx; break;
    default: u = dx; v = dy; break;
    }
    if (rm->shape == ShS_Quarter)
        u += rm->r / 2; /* the door is at the middle of the quarter's straight side */
    return sh_shape_in(rm->shape, rm->r, rm->w, rm->h, u, v);
}

static void sh_room_frame(struct ShRoom *rm)
{
    int u1, v1, u2, v2;
    sh_shape_box(rm->shape, rm->r, rm->w, rm->h, &u1, &v1, &u2, &v2);
    if (rm->shape == ShS_Quarter)
    {
        u1 -= rm->r / 2;
        u2 -= rm->r / 2;
    }
    int xa, ya, xb, yb;
    sh_rot4(rm->face, u1, v1, &xa, &ya);
    sh_rot4(rm->face, u2, v2, &xb, &yb);
    rm->x1 = rm->ax + min(xa, xb);
    rm->x2 = rm->ax + max(xa, xb);
    rm->y1 = rm->ay + min(ya, yb);
    rm->y2 = rm->ay + max(ya, yb);
    /* well inside: halfway down from the door side (on the axis; a quarter's middle) */
    int depth = (v1 + v2) / 2, side = (rm->shape == ShS_Quarter) ? (u1 + u2) / 2 + rm->r / 2 : 0;
    if (rm->shape == ShS_Quarter)
        side -= rm->r / 2;
    int mx, my;
    sh_rot4(rm->face, side, depth, &mx, &my);
    rm->mx = rm->ax + mx;
    rm->my = rm->ay + my;
}

/** May a room go there: every cell of it and round it (1 cell, the door excepted) still the land's own? */
static TbBool sh_room_fits(const struct OpPlan *p, const struct ShRoom *rm, int doorx, int doory)
{
    if ((rm->x1 < 1) || (rm->y1 < 1) || (rm->x2 > p->n - 2) || (rm->y2 > p->n - 2))
        return false;
    for (int y = rm->y1 - 1; y <= rm->y2 + 1; y++)
        for (int x = rm->x1 - 1; x <= rm->x2 + 1; x++)
        {
            TbBool near = false;
            for (int dy = -1; (dy <= 1) && !near; dy++)
                for (int dx = -1; (dx <= 1) && !near; dx++)
                    near = sh_room_cell(rm, x + dx, y + dy);
            if (!near)
                continue;
            if ((x == doorx) && (y == doory))
                continue;
            if (sh_at(p, x, y) != OpC_None)
                return false;
        }
    return true;
}

static void sh_room_carve(struct OpPlan *p, const struct ShRoom *rm, uint8_t cell, uint8_t rk)
{
    for (int y = rm->y1; y <= rm->y2; y++)
        for (int x = rm->x1; x <= rm->x2; x++)
            if (sh_room_cell(rm, x, y) && sh_in(p, x, y))
            {
                p->cell[OPI(p, x, y)] = cell;
                p->room[OPI(p, x, y)] = rk;
            }
}

static const int sh_dx[4] = {0, 1, 0, -1}, sh_dy[4] = {-1, 0, 1, 0};

/** A room with its door at (doorx, doory), the room on the far side from dir_to_room... face = the room's side the
 *  door is on. cell: OpC_Room (with a hero room kind and maybe a portal) or OpC_Floor (a walled court). Returns
 *  false (nothing changed) when it doesn't fit. */
static TbBool sh_add_room(struct OpPlan *p, struct OpRng *rng, int shape, int r, int w, int h, int face, int doorx,
    int doory, uint8_t cell, uint8_t gflags, struct ShRoom *out)
{
    struct ShRoom rm;
    memset(&rm, 0, sizeof(rm));
    rm.shape = shape;
    rm.r = r;
    rm.w = w;
    rm.h = h;
    rm.face = face;
    /* the anchor: one step from the door, away from face */
    rm.ax = doorx - sh_dx[face & 3];
    rm.ay = doory - sh_dy[face & 3];
    sh_room_frame(&rm);
    if (!sh_room_cell(&rm, rm.ax, rm.ay) || !sh_room_fits(p, &rm, doorx, doory))
        return false;
    uint8_t rk = 0;
    if (cell == OpC_Room)
        rk = op_room_kinds[op_rand(rng, OP_ROOM_KINDS)];
    sh_room_carve(p, &rm, cell, rk);
    sh_set(p, doorx, doory, OpC_Door);
    if (!sh_room_cell(&rm, rm.mx, rm.my))
    {
        rm.mx = rm.ax;
        rm.my = rm.ay;
    }
    if ((cell == OpC_Room) && (gflags != 0xFF))
        sh_gate(p, rm.mx, rm.my, gflags, rm.x1, rm.y1, rm.x2, rm.y2);
    if (rk == SlbT_TREASURE)
        sh_item(p, rm.mx, rm.my, ShI_Object, (uint16_t)sh_object_model("GOLD_CHEST"), 0, 0);
    if (out != NULL)
        *out = rm;
    return true;
}

/** A random shape for a room of size class r (and its rectangle sides). */
static int sh_pick_shape(struct OpRng *rng, int r, int *w, int *h, TbBool full_only)
{
    static const int full[] = {ShS_Rect, ShS_Circle, ShS_Octagon, ShS_Diamond, ShS_Cross};
    static const int any[] = {ShS_Rect, ShS_Circle, ShS_Octagon, ShS_Diamond, ShS_Cross, ShS_Semi, ShS_HalfOct, ShS_Tri,
        ShS_TriTip, ShS_Quarter, ShS_Semi, ShS_Tri};
    const int shape = full_only ? full[op_rand(rng, 5)] : any[op_rand(rng, 12)];
    *w = 2 * r + 1 - op_rand(rng, 2) * 2;
    *h = r + 2 + op_rand(rng, r + 1);
    if (shape == ShS_Cross && r < 3)
        return ShS_Octagon;
    return shape;
}

/** A hall (claimed, walled except over the biome's liquid, which it crosses on a causeway): an L from (ax, ay) to
 *  (bx, by), width w, the first stretch along x or y. It may not pass next to a room; it may run along and into
 *  other halls. Returns false (nothing changed) when it can't. */
static TbBool sh_hall(struct OpPlan *p, int ax, int ay, int bx, int by, int w, TbBool x_first)
{
    static uint8_t keep[SH_N_MAX * SH_N_MAX];
    memcpy(keep, p->cell, sizeof(keep));
    int pts[2][2] = {{x_first ? bx : ax, x_first ? ay : by}, {bx, by}};
    int cx = ax, cy = ay;
    for (int leg = -1; leg < 2; leg++)
    {
        const int tx = (leg < 0) ? ax : pts[leg][0], ty = (leg < 0) ? ay : pts[leg][1];
        for (;;)
        {
            for (int o = -(w - 1) / 2; o <= w / 2; o++)
            {
                const TbBool horiz = (ty == cy) && (tx != cx);
                const int x = horiz ? cx : cx + o, y = horiz ? cy + o : cy;
                if ((x < 1) || (y < 1) || (x > p->n - 2) || (y > p->n - 2))
                {
                    memcpy(p->cell, keep, sizeof(keep));
                    return false;
                }
                const uint8_t c = p->cell[OPI(p, x, y)];
                if ((c == OpC_Hall) || (c == OpC_Door))
                    continue;
                if ((c != OpC_None) && (c != OpC_Water) && (c != OpC_Lava))
                {
                    memcpy(p->cell, keep, sizeof(keep));
                    return false;
                }
                /* not right next to a room or a court (their doors are the way in) */
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        const uint8_t nc = sh_at(p, x + dx, y + dy);
                        if ((nc == OpC_Room) || (nc == OpC_Floor) || (nc == OpC_Heart))
                        {
                            memcpy(p->cell, keep, sizeof(keep));
                            return false;
                        }
                    }
                p->cell[OPI(p, x, y)] = OpC_Hall;
            }
            if ((cx == tx) && (cy == ty))
                break;
            if (cx != tx)
                cx += (tx > cx) ? 1 : -1;
            else
                cy += (ty > cy) ? 1 : -1;
        }
    }
    return true;
}

/** A hall from (x, y) out in direction d for len cells, then a room of the given shape at its end. */
static TbBool sh_branch(struct OpPlan *p, struct OpRng *rng, int x, int y, int d, int len, int shape, int r, int w, int h,
    uint8_t cell, uint8_t gflags, struct ShRoom *out)
{
    static uint8_t keep[SH_N_MAX * SH_N_MAX];
    static uint8_t keep_room[SH_N_MAX * SH_N_MAX];
    memcpy(keep, p->cell, sizeof(keep));
    memcpy(keep_room, p->room, sizeof(keep_room));
    const int ngates = p->ngates, nitems = p->nitems;
    const int ex = x + sh_dx[d] * len, ey = y + sh_dy[d] * len;
    if (!sh_hall(p, x + sh_dx[d], y + sh_dy[d], ex, ey, 1, true))
        return false;
    /* the room beyond the door: its door side faces back along the hall */
    if (sh_add_room(p, rng, shape, r, w, h, (d + 2) & 3, ex + sh_dx[d], ey + sh_dy[d], cell, gflags, out))
        return true;
    memcpy(p->cell, keep, sizeof(keep));
    memcpy(p->room, keep_room, sizeof(keep_room));
    p->ngates = ngates;
    p->nitems = nitems;
    return false;
}

/** The places a branch can leave from: cells of the given kind (and, for a court, inside it) with the land's own
 *  cell next to them towards d (and that cell's sides free of floor). Shuffled. Returns how many (x, y, d). */
#define SH_ATTACH_MAX 1024
static int sh_attach_points(const struct OpPlan *p, struct OpRng *rng, uint8_t kind, const struct ShRoom *court, int skip_d,
    int *ax, int *ay, int *ad)
{
    int n = 0;
    for (int y = 1; y < p->n - 1; y++)
        for (int x = 1; x < p->n - 1; x++)
        {
            if (sh_at(p, x, y) != kind)
                continue;
            if ((court != NULL) && !sh_room_cell(court, x, y))
                continue;
            for (int d = 0; (d < 4) && (n < SH_ATTACH_MAX); d++)
            {
                if (d == skip_d)
                    continue;
                const int wx = x + sh_dx[d], wy = y + sh_dy[d];
                const int px = sh_dx[(d + 1) & 3], py = sh_dy[(d + 1) & 3];
                if (sh_at(p, wx, wy) != OpC_None)
                    continue;
                const uint8_t s1 = sh_at(p, wx + px, wy + py), s2 = sh_at(p, wx - px, wy - py);
                if ((s1 == OpC_Floor) || (s2 == OpC_Floor) || (s1 == OpC_Door) || (s2 == OpC_Door) || (s1 == OpC_Room) || (s2 == OpC_Room))
                    continue;
                ax[n] = x;
                ay[n] = y;
                ad[n] = d;
                n++;
            }
        }
    for (int i = n - 1; i > 0; i--)
    {
        const int j = op_rand(rng, i + 1);
        int t = ax[i]; ax[i] = ax[j]; ax[j] = t;
        t = ay[i]; ay[i] = ay[j]; ay[j] = t;
        t = ad[i]; ad[i] = ad[j]; ad[j] = t;
    }
    return n;
}

/** Side rooms along the halls: up to want of them, each down a short branch. */
static int sh_side_rooms(struct OpPlan *p, struct OpRng *rng, int x1, int y1, int x2, int y2, int want, int rmax,
    uint8_t gflags)
{
    static int ax[SH_ATTACH_MAX], ay[SH_ATTACH_MAX], ad[SH_ATTACH_MAX];
    const int na = sh_attach_points(p, rng, OpC_Hall, NULL, -1, ax, ay, ad);
    int made = 0;
    (void)x1; (void)y1; (void)x2; (void)y2;
    for (int k = 0; (k < na) && (k < want * 16) && (made < want); k++)
    {
        const int x = ax[k], y = ay[k], d = ad[k];
        if ((sh_at(p, x, y) != OpC_Hall) || (sh_at(p, x + sh_dx[d], y + sh_dy[d]) != OpC_None))
            continue;
        int w, h;
        const int r = 2 + op_rand(rng, max(1, rmax - 1));
        const int shape = sh_pick_shape(rng, r, &w, &h, false);
        if (sh_branch(p, rng, x, y, d, 1 + op_rand(rng, 3), shape, r, w, h, OpC_Room, gflags, NULL))
            made++;
    }
    return made;
}

/** A wing: a long hall from a hall cell out to one side, a court of some shape at its end, rooms off the court. */
static int sh_wing(struct OpPlan *p, struct OpRng *rng, int x, int y, int d, int len, int rooms, int rmax, uint8_t gflags)
{
    int w, h;
    const int cr = 2 + op_rand(rng, 2);
    int shape = sh_pick_shape(rng, cr, &w, &h, true);
    struct ShRoom court;
    if (!sh_branch(p, rng, x, y, d, len, shape, cr, w, h, OpC_Floor, 0xFF, &court))
        return 0;
    /* rooms off the court: from its edge, out in a direction other than back down the hall */
    int made = 0;
    static int ax[SH_ATTACH_MAX], ay[SH_ATTACH_MAX], ad[SH_ATTACH_MAX];
    const int na = sh_attach_points(p, rng, OpC_Floor, &court, (d + 2) & 3, ax, ay, ad);
    for (int k = 0; (k < na) && (made < rooms); k++)
    {
        const int cx = ax[k], cy = ay[k], dd = ad[k];
        if ((sh_at(p, cx, cy) != OpC_Floor) || (sh_at(p, cx + sh_dx[dd], cy + sh_dy[dd]) != OpC_None))
            continue;
        /* a door in the court's wall, a short hall, the room */
        const int wx = cx + sh_dx[dd], wy = cy + sh_dy[dd];
        static uint8_t keep[SH_N_MAX * SH_N_MAX], keep_room[SH_N_MAX * SH_N_MAX];
        memcpy(keep, p->cell, sizeof(keep));
        memcpy(keep_room, p->room, sizeof(keep_room));
        const int ngates = p->ngates, nitems = p->nitems;
        /* the court's side cells of the door must be walls-to-be: not floor */
        const int px = sh_dx[(dd + 1) & 3], py = sh_dy[(dd + 1) & 3];
        if ((sh_at(p, wx + px, wy + py) != OpC_None) || (sh_at(p, wx - px, wy - py) != OpC_None))
            continue;
        p->cell[OPI(p, wx, wy)] = OpC_Door;
        const int r = 2 + op_rand(rng, max(1, rmax - 1));
        int rw, rh;
        const int rshape = sh_pick_shape(rng, r, &rw, &rh, false);
        const int len2 = 1 + op_rand(rng, 3);
        const int ex = wx + sh_dx[dd] * len2, ey = wy + sh_dy[dd] * len2;
        if (sh_hall(p, wx + sh_dx[dd], wy + sh_dy[dd], ex, ey, 1, true) &&
            sh_add_room(p, rng, rshape, r, rw, rh, (dd + 2) & 3, ex + sh_dx[dd], ey + sh_dy[dd], OpC_Room, gflags, NULL))
        {
            made++;
            continue;
        }
        memcpy(p->cell, keep, sizeof(keep));
        memcpy(p->room, keep_room, sizeof(keep_room));
        p->ngates = ngates;
        p->nitems = nitems;
    }
    return 1 + made;
}

/** A moat of the biome's liquid round a placed room, at a gap of 1 (it hugs its walls), on the land's own cells. */
static void sh_moat(struct OpPlan *p, const struct ShRoom *rm, uint8_t liquid)
{
    for (int y = rm->y1 - 2; y <= rm->y2 + 2; y++)
        for (int x = rm->x1 - 2; x <= rm->x2 + 2; x++)
        {
            if (!sh_in(p, x, y) || (sh_at(p, x, y) != OpC_None) || (x < 1) || (y < 1) || (x > p->n - 2) || (y > p->n - 2))
                continue;
            int dmin = 9;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                    if (sh_room_cell(rm, x + dx, y + dy))
                        dmin = min(dmin, max(abs(dx), abs(dy)));
            if (dmin == 2)
                p->cell[OPI(p, x, y)] = liquid;
        }
}

/** An exit: from (x, y) at the box's edge out in direction d, the way the land needs dug (counted in sh_exits). */
static void sh_add_exit(struct OpPlan *p, int x, int y, int d)
{
    if (p->nexits >= 4)
        return;
    p->exx[p->nexits] = (uint8_t)x;
    p->exy[p->nexits] = (uint8_t)y;
    p->exdir[p->nexits] = (uint8_t)d;
    p->exlen[p->nexits] = 0;
    p->nexits++;
}

static void sh_gen_outpost(struct OpRng *rng, struct OpPlan *p)
{
    const int n = p->n, c = n / 2, t = p->tier;
    const uint8_t liquid = sh_biome_liquid(p->biome);
    const uint8_t breach = (t >= ShT_Medium) ? ShG_Breach : 0;
    /* the heart's room at the far end: a circle, an octagon, a diamond, a cross or a square, somewhere across */
    static const int core_r[ShT_COUNT] = {3, 3, 4, 5, 6};
    const int cr = core_r[t];
    int w, h;
    const int cshape = sh_pick_shape(rng, cr, &w, &h, true);
    const TbBool asym = (op_rand(rng, 100) < 45) && (t >= ShT_Moderate);
    const int ccx = asym ? (op_rand(rng, 2) ? c - n / 5 : c + n / 5) : c;
    struct ShRoom core;
    memset(&core, 0, sizeof(core));
    int core_door_y = 0;
    for (int tries = 0; tries < 6; tries++)
    {
        const int depth = (cshape == ShS_Rect) ? h : 2 * cr + 1;
        core_door_y = n - 3 - depth - tries;
        if (sh_add_room(p, rng, (tries < 3) ? cshape : ShS_Octagon, cr, w, h, 0, ccx, core_door_y, OpC_Floor, 0xFF, &core))
            break;
        core.r = 0;
    }
    if (core.r == 0)
        return;
    /* the heart in its middle, its portal between the heart and the door */
    const int hcx = core.mx, hcy = min(core.my + 1, core.y2 - 2);
    sh_fill(p, hcx - 1, hcy - 1, hcx + 1, hcy + 1, OpC_Heart, 0);
    p->hx = hcx;
    p->hy = hcy;
    sh_gate(p, ccx, core.y1 + 1, ShG_Heart | breach, core.x1, core.y1, core.x2, core.y2);
    if ((t >= ShT_Moderate) && (op_rand(rng, 100) < 60))
        sh_moat(p, &core, liquid);
    /* the entrance and its hall: a gatehouse of some shape just inside */
    p->ex = c;
    p->ey = 0;
    sh_set(p, c, 0, OpC_Door);
    sh_add_exit(p, c, 0, 0);
    const int gr = (t >= ShT_Large) ? 2 : 1;
    int gw, gh;
    int gshape = (op_rand(rng, 2) == 0) ? ShS_Semi : ShS_Rect;
    gw = 2 * gr + 3;
    gh = gr + 2;
    struct ShRoom gate;
    if (!sh_add_room(p, rng, gshape, gr + 1, gw, gh, 0, c, 0, OpC_Floor, 0xFF, &gate))
        sh_add_room(p, rng, ShS_Rect, 1, 3, 2, 0, c, 0, OpC_Floor, 0xFF, &gate);
    /* the gatehouse is a court: the main hall leaves it through a door at its far end */
    const int gdoor_y = gate.y2 + 1;
    sh_set(p, c, gdoor_y, OpC_Door);
    /* the main hall: down from the gatehouse, across to the heart's room, down to its door */
    static const int hall_w[ShT_COUNT] = {1, 1, 2, 2, 3};
    const int bend = gdoor_y + 2 + op_rand(rng, max(1, core.y1 - gdoor_y - 6));
    sh_hall(p, c, gdoor_y + 1, c, bend, hall_w[t], false);
    sh_hall(p, c, bend, ccx, bend, hall_w[t], true);
    sh_hall(p, ccx, bend, ccx, core_door_y - 1, 1, false);
    /* wings: long halls from the main hall out to both sides, a court and rooms at each end */
    static const int wings[ShT_COUNT] = {0, 0, 1, 2, 3};
    for (int k = 0; k < wings[t]; k++)
    {
        for (int tries = 0; tries < 12; tries++)
        {
            const int y = gdoor_y + 2 + op_rand(rng, max(1, bend - gdoor_y - 1));
            const int x = c;
            const int d = op_rand(rng, 2) ? 1 : 3;
            if (sh_at(p, x, y) != OpC_Hall)
                continue;
            /* from the hall's side edge */
            int sx = x;
            while (sh_at(p, sx + sh_dx[d], y) == OpC_Hall)
                sx += sh_dx[d];
            if (sh_wing(p, rng, sx, y, d, 3 + op_rand(rng, max(1, n / 5)), 1 + op_rand(rng, 2), 2 + t / 2, breach))
                break;
        }
    }
    /* rooms off the halls' sides */
    static const int side[ShT_COUNT] = {1, 3, 3, 4, 5};
    sh_side_rooms(p, rng, 1, 1, n - 2, n - 2, side[t], 2 + t / 2, breach);
}

/** The courtyard's outermost cell towards d on the line through (lx, ly), found from the box's edge inwards (a keep
 *  in the middle leaves holes); false when the line misses it. */
static TbBool sh_court_edge(const struct OpPlan *p, int lx, int ly, int d, int *ox, int *oy)
{
    int x = (d == 1) ? p->n - 1 : ((d == 3) ? 0 : lx), y = (d == 2) ? p->n - 1 : ((d == 0) ? 0 : ly);
    while (sh_in(p, x, y))
    {
        if (sh_at(p, x, y) == OpC_Floor)
        {
            *ox = x;
            *oy = y;
            return true;
        }
        x -= sh_dx[d];
        y -= sh_dy[d];
    }
    return false;
}

static void sh_gen_fortress(struct OpRng *rng, struct OpPlan *p)
{
    const int n = p->n, c = n / 2, t = p->tier;
    const uint8_t liquid = sh_biome_liquid(p->biome);
    const uint8_t breach = (t >= ShT_Medium) ? ShG_Breach : 0;
    /* the courtyard in the middle: a circle, an octagon, a square, a diamond or a cross */
    static const int court_r[ShT_COUNT] = {4, 5, 5, 6, 7};
    const int cr = court_r[t];
    int w, h;
    int cshape = sh_pick_shape(rng, cr, &w, &h, true);
    if (cshape == ShS_Rect)
    {
        w = 2 * cr + 1;
        h = 2 * cr + 1;
    }
    struct ShRoom court;
    memset(&court, 0, sizeof(court));
    const int depth = (cshape == ShS_Rect) ? h : 2 * cr + 1;
    if (!sh_add_room(p, rng, cshape, cr, w, h, 0, c, c - depth / 2 - 1, OpC_Floor, 0xFF, &court) &&
        !sh_add_room(p, rng, ShS_Octagon, cr, 0, 0, 0, c, c - cr - 1, OpC_Floor, 0xFF, &court))
        return;
    sh_set(p, court.ax + sh_dx[0], court.ay + sh_dy[0], OpC_None); /* no door there unless a gate goes there */
    p->hx = court.mx;
    p->hy = court.my;
    /* the keep in the middle (Large and Massive): a round or eight-sided room with the hero gate */
    if (t >= ShT_Large)
    {
        const int kr = 2 + (t == ShT_Massive ? 1 : 0);
        const int kshape = op_rand(rng, 2) ? ShS_Circle : ShS_Octagon;
        /* clear a ring round it back to the land's own: its walls go there */
        struct ShRoom keep;
        memset(&keep, 0, sizeof(keep));
        keep.shape = kshape;
        keep.r = kr + 2;
        keep.face = 0;
        keep.ax = court.mx;
        keep.ay = court.my - kr - 2;
        sh_room_frame(&keep);
        for (int y = keep.y1; y <= keep.y2; y++)
            for (int x = keep.x1; x <= keep.x2; x++)
                if (sh_room_cell(&keep, x, y))
                    sh_set(p, x, y, OpC_None);
        struct ShRoom inner;
        const int kd = op_rand(rng, 4);
        const int doorx = court.mx + sh_dx[kd] * (kr + 1), doory = court.my + sh_dy[kd] * (kr + 1);
        if (sh_add_room(p, rng, kshape, kr, 0, 0, kd, doorx, doory, OpC_Floor, 0xFF, &inner))
        {
            sh_set(p, doorx + sh_dx[kd], doory + sh_dy[kd], OpC_Hall); /* out through the cleared ring */
            sh_gate(p, court.mx, court.my, ShG_Breach, inner.x1, inner.y1, inner.x2, inner.y2);
            p->hx = court.mx;
            p->hy = court.my;
        }
    } else
        sh_gate(p, court.mx, court.my, ShG_Decor, court.mx, court.my, court.mx, court.my);
    if (op_rand(rng, 100) < ((t == ShT_Massive) ? 100 : 40 + 10 * t))
        sh_moat(p, &court, liquid);
    /* gates out: a door in the courtyard's wall on a free side, a dug way out to the box's edge */
    static const int gates[ShT_COUNT] = {1, 2, 2, 2, 3};
    int dirs[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; i--)
    {
        const int j = op_rand(rng, i + 1);
        const int tmp = dirs[i];
        dirs[i] = dirs[j];
        dirs[j] = tmp;
    }
    int made_gates = 0;
    for (int k = 0; (k < 4) && (made_gates < gates[t]); k++)
    {
        const int d = dirs[k];
        /* the courtyard's outermost cell that way */
        int x, y;
        if (!sh_court_edge(p, court.mx, court.my, d, &x, &y))
            continue;
        const int wx = x + sh_dx[d], wy = y + sh_dy[d];
        if ((sh_at(p, wx, wy) != OpC_None) && (sh_at(p, wx, wy) != OpC_Water) && (sh_at(p, wx, wy) != OpC_Lava))
            continue;
        sh_set(p, wx, wy, OpC_Door);
        int px = wx + sh_dx[d], py = wy + sh_dy[d];
        while (sh_in(p, px, py))
        {
            const uint8_t cc = sh_at(p, px, py);
            if ((cc != OpC_None) && (cc != OpC_Water) && (cc != OpC_Lava) && (cc != OpC_Path))
                break;
            sh_set(p, px, py, OpC_Path);
            if ((px == 0) || (py == 0) || (px == n - 1) || (py == n - 1))
            {
                sh_add_exit(p, px, py, d);
                break;
            }
            px += sh_dx[d];
            py += sh_dy[d];
        }
        made_gates++;
    }
    /* towers round the courtyard: rooms of many shapes down short halls from doors in its wall */
    static const int towers[ShT_COUNT] = {0, 0, 3, 4, 5};
    int made = 0;
    static int tax[SH_ATTACH_MAX], tay[SH_ATTACH_MAX], tad[SH_ATTACH_MAX];
    const int nta = sh_attach_points(p, rng, OpC_Floor, &court, -1, tax, tay, tad);
    for (int k = 0; (k < nta) && (made < towers[t]); k++)
    {
        const int d = tad[k], x = tax[k], y = tay[k];
        const int wx = x + sh_dx[d], wy = y + sh_dy[d];
        const int px = sh_dx[(d + 1) & 3], py = sh_dy[(d + 1) & 3];
        if ((sh_at(p, wx, wy) != OpC_None) || (sh_at(p, wx + px, wy + py) == OpC_Door) || (sh_at(p, wx - px, wy - py) == OpC_Door) ||
            (sh_at(p, wx + px, wy + py) == OpC_Floor) || (sh_at(p, wx - px, wy - py) == OpC_Floor))
            continue;
        static uint8_t keep[SH_N_MAX * SH_N_MAX], keep_room[SH_N_MAX * SH_N_MAX];
        memcpy(keep, p->cell, sizeof(keep));
        memcpy(keep_room, p->room, sizeof(keep_room));
        const int ngates = p->ngates, nitems = p->nitems;
        sh_set(p, wx, wy, OpC_Door);
        const int tr = 2 + op_rand(rng, 2);
        static const int tower_shapes[] = {ShS_Circle, ShS_Circle, ShS_Octagon, ShS_Semi, ShS_HalfOct, ShS_Diamond, ShS_TriTip};
        const int tshape = tower_shapes[op_rand(rng, 7)];
        const int len = 1 + op_rand(rng, 3);
        const int ex = wx + sh_dx[d] * len, ey = wy + sh_dy[d] * len;
        if (sh_hall(p, wx + sh_dx[d], wy + sh_dy[d], ex, ey, 1, true) &&
            sh_add_room(p, rng, tshape, tr, 0, 0, (d + 2) & 3, ex + sh_dx[d], ey + sh_dy[d], OpC_Room, breach, NULL))
        {
            made++;
            continue;
        }
        memcpy(p->cell, keep, sizeof(keep));
        memcpy(p->room, keep_room, sizeof(keep_room));
        p->ngates = ngates;
        p->nitems = nitems;
    }
    /* wings (Large and Massive): long halls from a courtyard door out to a court with rooms */
    static const int wings[ShT_COUNT] = {0, 0, 0, 1, 2};
    int made_wings = 0;
    const int nwa = sh_attach_points(p, rng, OpC_Floor, &court, -1, tax, tay, tad);
    for (int k = 0; (k < nwa) && (made_wings < wings[t]); k++)
    {
        const int d = tad[k], x = tax[k], y = tay[k];
        const int wx = x + sh_dx[d], wy = y + sh_dy[d];
        const int px = sh_dx[(d + 1) & 3], py = sh_dy[(d + 1) & 3];
        if ((sh_at(p, wx, wy) != OpC_None) || (sh_at(p, wx + px, wy + py) == OpC_Door) || (sh_at(p, wx - px, wy - py) == OpC_Door) ||
            (sh_at(p, wx + px, wy + py) == OpC_Floor) || (sh_at(p, wx - px, wy - py) == OpC_Floor))
            continue;
        static uint8_t keep[SH_N_MAX * SH_N_MAX], keep_room[SH_N_MAX * SH_N_MAX];
        memcpy(keep, p->cell, sizeof(keep));
        memcpy(keep_room, p->room, sizeof(keep_room));
        const int ngates = p->ngates, nitems = p->nitems;
        sh_set(p, wx, wy, OpC_Door);
        if (sh_wing(p, rng, wx, wy, d, 4 + op_rand(rng, n / 5), 2, 3, breach))
        {
            made_wings++;
            continue;
        }
        memcpy(p->cell, keep, sizeof(keep));
        memcpy(p->room, keep_room, sizeof(keep_room));
        p->ngates = ngates;
        p->nitems = nitems;
    }
}

/** Walls round what the stronghold uses: on the land's own cells next to its rooms, courts and doors always; next to
 *  its halls only where the land isn't lava or water (a hall over the biome's liquid is a causeway). In Ancient Ruins
 *  some outer walls have crumbled to earth. */
static void sh_walls_blend(struct OpPlan *p, struct OpRng *rng)
{
    static uint8_t mark[SH_N_MAX * SH_N_MAX];
    memset(mark, 0, sizeof(mark));
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
        {
            if (p->cell[OPI(p, x, y)] != OpC_None)
                continue;
            TbBool room = false, hall = false;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (!sh_in(p, x + dx, y + dy))
                        continue;
                    const uint8_t c = p->cell[OPI(p, x + dx, y + dy)];
                    room = room || (c == OpC_Floor) || (c == OpC_Room) || (c == OpC_Heart) || (c == OpC_Door);
                    hall = hall || (c == OpC_Hall);
                }
            if (room || (hall && !sh_liquid_ground(p->ground[OPI(p, x, y)])))
                mark[OPI(p, x, y)] = 1;
        }
    for (int i = 0; i < p->n * p->n; i++)
        if (mark[i])
            p->cell[i] = OpC_Wall;
    if (p->biome != WgB_Ruins)
        return;
    for (int y = 1; y < p->n - 1; y++)
        for (int x = 1; x < p->n - 1; x++)
        {
            if (p->cell[OPI(p, x, y)] != OpC_Wall)
                continue;
            TbBool outside = false;
            for (int k = 0; k < 4; k++)
                outside = outside || (sh_at(p, x + sh_dx[k], y + sh_dy[k]) == OpC_None);
            if (outside && (op_rand(rng, 100) < 12))
                p->cell[OPI(p, x, y)] = OpC_Earth;
        }
}

/** The ways out at the box's edge: how far the land beyond needs digging (rock) to reach ground a keeper can get to. */
static void sh_exits(struct OpPlan *p, int64_t wx0, int64_t wy0)
{
    struct WgWorld *gen = world_generator();
    for (int e = 0; e < p->nexits; e++)
    {
        int len = 0;
        for (int k = 1; k <= 24; k++)
        {
            const int64_t x = wx0 + p->exx[e] + (int64_t)sh_dx[p->exdir[e] & 3] * k;
            const int64_t y = wy0 + p->exy[e] + (int64_t)sh_dy[p->exdir[e] & 3] * k;
            if ((gen == NULL) || (wg_terrain_at(gen, x, y) != WgS_Rock))
                break;
            len = k;
        }
        p->exlen[e] = (uint8_t)max(len, (p->family == ShF_Outpost) ? 1 : 0);
    }
}

/******************************************************************************/
/* turning */

static void op_rot(int n, int dir, int x, int y, int *rx, int *ry)
{
    switch (dir & 3)
    {
    case 1: *rx = n - 1 - y; *ry = x; break;
    case 2: *rx = n - 1 - x; *ry = n - 1 - y; break;
    case 3: *rx = y; *ry = n - 1 - x; break;
    default: *rx = x; *ry = y; break;
    }
}

/** Turns a canonical plan so its entrance faces dir; mirrors it left to right first if asked. */
static void op_turn(struct OpPlan *p, int dir, TbBool mirror)
{
    static uint8_t cell[SH_N_MAX * SH_N_MAX], room[SH_N_MAX * SH_N_MAX], ground[SH_N_MAX * SH_N_MAX];
    const int n = p->n;
    memcpy(cell, p->cell, sizeof(cell));
    memcpy(room, p->room, sizeof(room));
    memcpy(ground, p->ground, sizeof(ground));
#define TURN(xx, yy, ox, oy) do { int mx_ = mirror ? n - 1 - (xx) : (xx); op_rot(n, dir, mx_, (yy), &(ox), &(oy)); } while (0)
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
        {
            int X, Y;
            TURN(x, y, X, Y);
            p->cell[Y * n + X] = cell[y * n + x];
            p->room[Y * n + X] = room[y * n + x];
            p->ground[Y * n + X] = ground[y * n + x];
        }
    for (int e = 0; e < p->nexits; e++)
    {
        int EX, EY;
        TURN(p->exx[e], p->exy[e], EX, EY);
        p->exx[e] = (uint8_t)EX;
        p->exy[e] = (uint8_t)EY;
        int ed = p->exdir[e] & 3;
        if (mirror && (ed & 1))
            ed ^= 2;
        p->exdir[e] = (uint8_t)((ed + dir) & 3);
    }
    int X, Y;
    TURN(p->ex, p->ey, X, Y);
    p->ex = X;
    p->ey = Y;
    static const int odx[4] = {0, 1, 0, -1}, ody[4] = {-1, 0, 1, 0};
    p->odx = odx[dir & 3];
    p->ody = ody[dir & 3];
    TURN(p->hx, p->hy, X, Y);
    p->hx = X;
    p->hy = Y;
    for (int g = 0; g < p->ngates; g++)
    {
        TURN(p->gx[g], p->gy[g], X, Y);
        p->gx[g] = (uint8_t)X;
        p->gy[g] = (uint8_t)Y;
        int X1, Y1, X2, Y2;
        TURN(p->grx1[g], p->gry1[g], X1, Y1);
        TURN(p->grx2[g], p->gry2[g], X2, Y2);
        p->grx1[g] = (uint8_t)min(X1, X2);
        p->gry1[g] = (uint8_t)min(Y1, Y2);
        p->grx2[g] = (uint8_t)max(X1, X2);
        p->gry2[g] = (uint8_t)max(Y1, Y2);
    }
    for (int t = 0; t < p->ntraps; t++)
    {
        TURN(p->tx[t], p->ty[t], X, Y);
        p->tx[t] = (uint8_t)X;
        p->ty[t] = (uint8_t)Y;
    }
    for (int i = 0; i < p->nitems; i++)
    {
        TURN(p->items[i].x, p->items[i].y, X, Y);
        p->items[i].x = (uint8_t)X;
        p->items[i].y = (uint8_t)Y;
    }
#undef TURN
    p->dir = (uint8_t)(dir & 3);
}

/******************************************************************************/
/* sizes, levels, books */

static int sh_tier_for_level(int level)
{
    if (level >= 20)
        return ShT_Massive;
    if (level >= 12)
        return ShT_Large;
    if (level >= 8)
        return ShT_Medium;
    if (level >= 4)
        return ShT_Moderate;
    return ShT_Small;
}

/** Books by size: Small and Moderate 1, Medium 1-2, Large 2-3, Massive 2-4. */
static int sh_books_for(int tier, uint64_t roll)
{
    switch (tier)
    {
    case ShT_Medium: return 1 + (int)(roll % 2);
    case ShT_Large: return 2 + (int)(roll % 2);
    case ShT_Massive: return 2 + (int)(roll % 3);
    default: return 1;
    }
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

/** The best creature level of the human keeper whose heart is nearest a world slab (all of them when no heart is
 *  on the map). */
static int sh_finder_level(int64_t wx, int64_t wy)
{
    /* test hook: KFX_WORLD_FINDERLEVEL=N decides every new stronghold as if its finder's best creature were level N */
    static int forced = -2;
    if (forced == -2)
    {
        const char *v = getenv("KFX_WORLD_FINDERLEVEL");
        forced = (v != NULL) ? atoi(v) : -1;
    }
    if (forced > 0)
        return forced;
    int best_level = 1, top = 1;
    int64_t best_d = -1;
    for (PlayerNumber pl = 0; pl < PLAYERS_COUNT; pl++)
    {
        if (!world_player_is_human(pl) || !player_exists(get_player(pl)))
            continue;
        const int lv = world_keeper_top_level(pl);
        top = max(top, lv);
        const struct Thing *heart = find_players_dungeon_heart(pl);
        if (!thing_exists(heart))
            continue;
        int64_t hx, hy;
        world_slab_to_world(subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), &hx, &hy);
        const int64_t d = (hx - wx) * (hx - wx) + (hy - wy) * (hy - wy);
        if ((best_d < 0) || (d < best_d))
        {
            best_d = d;
            best_level = lv;
        }
    }
    return (best_d >= 0) ? best_level : top;
}

/** Any chunk under a box of world slabs already made (stored or loaded), other than the given one? */
static TbBool sh_box_made(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int32_t skip_cx, int32_t skip_cy)
{
    for (int64_t cy = op_floor_div(y1, WG_CHUNK); cy <= op_floor_div(y2, WG_CHUNK); cy++)
        for (int64_t cx = op_floor_div(x1, WG_CHUNK); cx <= op_floor_div(x2, WG_CHUNK); cx++)
        {
            if ((cx == skip_cx) && (cy == skip_cy))
                continue;
            if (world_chunk_made((int32_t)cx, (int32_t)cy))
                return true;
        }
    return false;
}

/******************************************************************************/
/* what happened to them */
static struct OpState *op_state(uint8_t family, int64_t rx, int64_t ry, TbBool add)
{
    for (int i = 0; i < op_state_count; i++)
        if ((op_states[i].family == family) && (op_states[i].rx == rx) && (op_states[i].ry == ry))
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
    st->family = family;
    st->rx = rx;
    st->ry = ry;
    st->tier = ShT_None;
    return st;
}

/******************************************************************************/
/* the outposts' places */

/** An outpost region's seeded start: the chance, then where (legacy: as the old outposts were placed). */
static TbBool op_region_rng(int64_t rx, int64_t ry, struct OpRng *rng)
{
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return false;
    rng->s = wg_world_seed(gen) ^ op_mix((uint64_t)rx * 0x51ED27F1ULL) ^ op_mix((uint64_t)ry * 0x2545F491ULL + 0x4F555450ULL);
    return op_rand(rng, 100) < OP_CHANCE;
}

/** The box an outpost takes with its way in (world slabs). */
static void op_box(const struct OpPlan *p, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    *x1 = p->x0;
    *y1 = p->y0;
    *x2 = p->x0 + p->n - 1;
    *y2 = p->y0 + p->n - 1;
    if (p->tier == ShT_Legacy)
    {
        const int64_t px = p->x0 + p->ex + (int64_t)p->odx * OP_PATH, py = p->y0 + p->ey + (int64_t)p->ody * OP_PATH;
        if (px < *x1) *x1 = px;
        if (py < *y1) *y1 = py;
        if (px > *x2) *x2 = px;
        if (py > *y2) *y2 = py;
        return;
    }
    for (int e = 0; e < p->nexits; e++)
    {
        static const int edx[4] = {0, 1, 0, -1}, edy[4] = {-1, 0, 1, 0};
        const int64_t px = p->x0 + p->exx[e] + (int64_t)edx[p->exdir[e] & 3] * p->exlen[e];
        const int64_t py = p->y0 + p->exy[e] + (int64_t)edy[p->exdir[e] & 3] * p->exlen[e];
        if (px < *x1) *x1 = px;
        if (py < *y1) *y1 = py;
        if (px > *x2) *x2 = px;
        if (py > *y2) *y2 = py;
    }
}

/** The land under a plan, read in its own frame (before it's turned): ground[] of the canonical cells. */
static void sh_read_ground(struct OpPlan *p, int dir, TbBool mirror)
{
    struct WgWorld *gen = world_generator();
    const int n = p->n;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
        {
            const int mx = mirror ? n - 1 - x : x;
            int X, Y;
            op_rot(n, dir, mx, y, &X, &Y);
            p->ground[OPI(p, x, y)] = (uint8_t)((gen != NULL) ? wg_terrain_at(gen, p->x0 + X, p->y0 + Y) : WgS_Rock);
        }
    p->biome = (uint8_t)((gen != NULL) ? wg_biome_at(gen, p->x0 + n / 2, p->y0 + n / 2) : WgB_Earthen);
}

static TbBool op_place_clear(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    struct WgWorld *gen = world_generator();
    struct WgStructRef refs[2];
    const unsigned mask = (1u << WgT_Keeper) | (1u << WgT_Fortress) | (1u << WgT_Portal) | (1u << WgT_Reward);
    if (wg_structures_in(gen, x1 - OP_MARGIN, y1 - OP_MARGIN, x2 + OP_MARGIN, y2 + OP_MARGIN, mask, refs, 2) > 0)
        return false;
    if (world_stamps_overlap(x1 - OP_MARGIN, y1 - OP_MARGIN, x2 + OP_MARGIN, y2 + OP_MARGIN))
        return false;
    return true;
}

/** The old outpost of a region, if it had one: built exactly as before. */
static TbBool op_plan_make_legacy(int64_t rx, int64_t ry, struct OpPlan *p)
{
    struct OpRng rng;
    if (!op_region_rng(rx, ry, &rng))
        return false;
    memset(p, 0, sizeof(*p));
    p->family = ShF_Outpost;
    p->tier = ShT_Legacy;
    p->rx = rx;
    p->ry = ry;
    p->n = LEGACY_N;
    const int span = OP_REGION - LEGACY_N - 2 * (OP_PATH + 2);
    p->x0 = rx * OP_REGION + OP_PATH + 2 + op_rand(&rng, span);
    p->y0 = ry * OP_REGION + OP_PATH + 2 + op_rand(&rng, span);
    const int64_t cx = p->x0 + LEGACY_N / 2 - 8, cy = p->y0 + LEGACY_N / 2 - 8;
    if (cx * cx + cy * cy < (int64_t)OP_START_DIST * OP_START_DIST)
        return false;
    op_build_legacy(&rng, p);
    p->ex = 10;
    p->ey = 0;
    op_turn(p, op_rand(&rng, 4), false);
    int64_t x1, y1, x2, y2;
    op_box(p, &x1, &y1, &x2, &y2);
    return op_place_clear(x1, y1, x2, y2);
}

/** Where a region's sized outpost goes: the largest box (Massive) and its way in; a smaller one sits in its middle.
 *  Returns false when the region has none. *seed is the seed of its layout. */
static TbBool op_site(int64_t rx, int64_t ry, int64_t *mx0, int64_t *my0, uint64_t *seed)
{
    struct OpRng rng;
    if (!op_region_rng(rx, ry, &rng))
        return false;
    rng.s ^= 0x53495A4553ULL; /* "SIZES": a stream of its own */
    const int span = OP_REGION - SH_N_MAX - 2 * (OP_PATH + 2);
    *mx0 = rx * OP_REGION + OP_PATH + 2 + op_rand(&rng, span);
    *my0 = ry * OP_REGION + OP_PATH + 2 + op_rand(&rng, span);
    const int64_t cx = *mx0 + SH_N_MAX / 2 - 8, cy = *my0 + SH_N_MAX / 2 - 8;
    if (cx * cx + cy * cy < (int64_t)OP_START_DIST * OP_START_DIST)
        return false;
    *seed = op_mix(rng.s);
    return true;
}

/** The world box a plan really builds on (its cells other than the land's own, and its ways out); the old outposts:
 *  their box. */
static void sh_plan_extent(const struct OpPlan *p, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    op_box(p, x1, y1, x2, y2);
    if (p->tier >= ShT_COUNT)
        return;
    int mnx = p->n, mny = p->n, mxx = -1, mxy = -1;
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
            if (p->cell[OPI(p, x, y)] != OpC_None)
            {
                mnx = min(mnx, x);
                mny = min(mny, y);
                mxx = max(mxx, x);
                mxy = max(mxy, y);
            }
    if (mxx < 0)
        return;
    int64_t bx1 = p->x0 + mnx, by1 = p->y0 + mny, bx2 = p->x0 + mxx, by2 = p->y0 + mxy;
    for (int e = 0; e < p->nexits; e++)
    {
        static const int edx[4] = {0, 1, 0, -1}, edy[4] = {-1, 0, 1, 0};
        const int64_t px = p->x0 + p->exx[e] + (int64_t)edx[p->exdir[e] & 3] * p->exlen[e];
        const int64_t py = p->y0 + p->exy[e] + (int64_t)edy[p->exdir[e] & 3] * p->exlen[e];
        bx1 = min(bx1, px); by1 = min(by1, py); bx2 = max(bx2, px); by2 = max(by2, py);
    }
    *x1 = bx1;
    *y1 = by1;
    *x2 = bx2;
    *y2 = by2;
}

static struct OpState *op_state(uint8_t family, int64_t rx, int64_t ry, TbBool add);
static const struct OpPlan *op_plan_of(const struct OpState *st);

/** Does a decided sized fortress build on this box? */
static TbBool sh_fortress_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    for (int k = 0; k < op_state_count; k++)
    {
        if ((op_states[k].family != ShF_Fortress) || (op_states[k].tier >= ShT_COUNT))
            continue;
        const struct OpPlan *p = op_plan_of(&op_states[k]);
        if (p == NULL)
            continue;
        int64_t bx1, by1, bx2, by2;
        sh_plan_extent(p, &bx1, &by1, &bx2, &by2);
        if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
            return true;
    }
    return false;
}

/** May a sized outpost plan go where it is: nothing of the generator's, no start, no fortress under what it builds? */
static TbBool op_plan_clear(const struct OpPlan *p)
{
    int64_t x1, y1, x2, y2;
    sh_plan_extent(p, &x1, &y1, &x2, &y2);
    if (!op_place_clear(x1, y1, x2, y2))
        return false;
    return !sh_fortress_in_box(x1 - OP_MARGIN, y1 - OP_MARGIN, x2 + OP_MARGIN, y2 + OP_MARGIN);
}

static void sh_finish(struct OpPlan *p, struct OpRng *rng, int found_level);

static TbBool op_plan_make_sized(int64_t rx, int64_t ry, int tier, int found_level, struct OpPlan *p)
{
    int64_t mx0, my0;
    uint64_t seed;
    if ((tier < 0) || (tier >= ShT_COUNT) || !op_site(rx, ry, &mx0, &my0, &seed))
        return false;
    memset(p, 0, sizeof(*p));
    p->family = ShF_Outpost;
    p->tier = (uint8_t)tier;
    p->rx = rx;
    p->ry = ry;
    p->n = sh_outpost_n[tier];
    p->x0 = mx0 + (SH_N_MAX - p->n) / 2;
    p->y0 = my0 + (SH_N_MAX - p->n) / 2;
    struct OpRng rng = {seed ^ ((uint64_t)tier << 40)};
    /* which way it faces first: the land is read in the plan's own frame */
    const int dir = op_rand(&rng, 4);
    const TbBool mirror = (op_rand(&rng, 2) != 0);
    sh_read_ground(p, dir, mirror);
    memset(p->cell, OpC_None, sizeof(p->cell));
    sh_gen_outpost(&rng, p);
    sh_walls_blend(p, &rng);
    sh_finish(p, &rng, found_level);
    op_turn(p, dir, mirror);
    sh_exits(p, p->x0, p->y0);
    return true;
}

/******************************************************************************/
/* the fortresses' places */

static TbBool ft_find_ref(int64_t start_cx, int64_t start_cy, struct WgStructRef *out)
{
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return false;
    struct WgStructRef refs[8];
    /* the structure starts in its start chunk: its origin is in or next to it */
    const int64_t x1 = start_cx * WG_CHUNK - 16, y1 = start_cy * WG_CHUNK - 16;
    const int n = wg_structures_in(gen, x1, y1, x1 + WG_CHUNK + 31, y1 + WG_CHUNK + 31, 1u << WgT_Fortress, refs, 8);
    for (int i = 0; (i < n) && (i < 8); i++)
        if ((refs[i].id.start_cx == start_cx) && (refs[i].id.start_cy == start_cy))
        {
            *out = refs[i];
            return true;
        }
    return false;
}

static void sh_plan_extent(const struct OpPlan *p, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2);
static const struct OpPlan *op_plan_of(const struct OpState *st);

/** Does an outpost already decided build on this box? (One decided later keeps clear of the fortresses itself.) */
static TbBool sh_outpost_built_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    for (int k = 0; k < op_state_count; k++)
    {
        if ((op_states[k].family != ShF_Outpost) || (op_states[k].tier == ShT_None))
            continue;
        const struct OpPlan *pl = op_plan_of(&op_states[k]);
        if (pl == NULL)
            continue;
        int64_t bx1, by1, bx2, by2;
        sh_plan_extent(pl, &bx1, &by1, &bx2, &by2);
        if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
            return true;
    }
    return false;
}

/** Does a fortress plan fit where the generator put its fortress: no keeper, portal, reward cave, outpost or start
 *  under what it builds? */
static TbBool ft_fits(const struct OpPlan *p)
{
    int64_t x1, y1, x2, y2;
    sh_plan_extent(p, &x1, &y1, &x2, &y2);
    x1--;
    y1--;
    x2++;
    y2++;
    struct WgStructRef refs[2];
    const unsigned mask = (1u << WgT_Keeper) | (1u << WgT_Portal) | (1u << WgT_Reward);
    if (wg_structures_in(world_generator(), x1, y1, x2, y2, mask, refs, 2) > 0)
        return false;
    if (sh_outpost_built_in_box(x1, y1, x2, y2) || world_stamps_overlap(x1, y1, x2, y2))
        return false;
    return true;
}

static TbBool ft_plan_make(const struct WgStructRef *ref, int tier, int found_level, struct OpPlan *p)
{
    if ((tier < 0) || (tier >= ShT_COUNT))
        return false;
    memset(p, 0, sizeof(*p));
    p->family = ShF_Fortress;
    p->tier = (uint8_t)tier;
    p->rx = ref->id.start_cx;
    p->ry = ref->id.start_cy;
    p->n = sh_fortress_n[tier];
    p->x0 = ref->origin_x - p->n / 2;
    p->y0 = ref->origin_y - p->n / 2;
    /* the generator's fortress goes: its ground (all it changed, within its box) is the land's own again where this
       one doesn't build */
    p->fill = (int)max((int64_t)p->n / 2, max(max(ref->origin_x - ref->bbox_x1, ref->bbox_x2 - ref->origin_x),
        max(ref->origin_y - ref->bbox_y1, ref->bbox_y2 - ref->origin_y)));
    struct OpRng rng = {op_mix(world_seed() ^ op_mix((uint64_t)ref->id.start_cx * 0x7F4A7C15ULL) ^
        op_mix((uint64_t)ref->id.start_cy * 0x94D049BBULL + 0x46525453ULL) ^ ((uint64_t)tier << 40))};
    const int dir = op_rand(&rng, 4);
    const TbBool mirror = (op_rand(&rng, 2) != 0);
    sh_read_ground(p, dir, mirror);
    memset(p->cell, OpC_None, sizeof(p->cell));
    sh_gen_fortress(&rng, p);
    sh_walls_blend(p, &rng);
    sh_finish(p, &rng, found_level);
    /* research books by the hero gate (fortresses' own: their grade by the distance to the nearest start) */
    if (world_progress_active() && (world_progress_book_model() > 0))
    {
        struct WgStructRef graded = *ref;
        graded.distance = world_dist_to_nearest_start(ref->origin_x, ref->origin_y);
        uint8_t kind = 0;
        world_progress_books_of(&graded, world_seed(), false, &kind);
        const int nb = sh_books_for(tier, op_mix(rng.s ^ 0x424F4F4BULL));
        int bx[8], by[8];
        const int got = sh_free_floor(p, &rng, p->hx - 2, p->hy - 2, p->hx + 2, p->hy + 2, bx, by, min(nb, 8));
        for (int b = 0; b < got; b++)
            sh_item(p, bx[b], by[b], ShI_Book, (uint16_t)world_progress_book_model(), 0, kind);
    }
    op_turn(p, dir, mirror);
    sh_exits(p, p->x0, p->y0);
    return true;
}

/******************************************************************************/
/* traps, things, guards: what the sizes add */

/* the heroes a fortress's guards are, by the finder's level (Small and Moderate fortresses) */
static const struct { const char *name; int weight; int min_level; } ft_guards[] = {
    {"ARCHER", 4, 1}, {"DWARFA", 3, 1}, {"THIEF", 2, 1}, {"BARBARIAN", 3, 3}, {"MONK", 2, 4}, {"WIZARD", 2, 5},
    {"WITCH", 1, 6}, {"FAIRY", 1, 6}, {"SAMURAI", 1, 7},
};
#define FT_GUARD_KINDS ((int)(sizeof(ft_guards) / sizeof(ft_guards[0])))

static void sh_finish(struct OpPlan *p, struct OpRng *rng, int found_level)
{
    const int tier = p->tier;
    const int lv = max(1, found_level);
    /* loot */
    static const char *const loot[3] = {"GOLD_CHEST", "GOLD", "GOLDL"};
    static const int loot_min[ShT_COUNT] = {2, 3, 3, 4, 5}, loot_span[ShT_COUNT] = {2, 1, 2, 2, 2};
    const int nloot = loot_min[tier] + op_rand(rng, loot_span[tier]);
    int lx[8], ly[8];
    const int c = p->n / 2;
    int got = (p->family == ShF_Fortress) ?
        sh_free_floor(p, rng, p->hx - 4, p->hy - 4, p->hx + 4, p->hy + 4, lx, ly, min(nloot, 8)) :
        sh_free_floor(p, rng, p->hx - 3, p->hy - 3, p->hx + 3, p->hy + 3, lx, ly, min(nloot, 8));
    for (int i = 0; i < got; i++)
        sh_item(p, lx[i], ly[i], ShI_Object, (uint16_t)sh_object_model(loot[op_rand(rng, 3)]), 0, 0);
    /* guards: Small and Moderate fortresses' heroes wait inside */
    if ((p->family == ShF_Fortress) && (tier <= ShT_Moderate))
    {
        const int count = min(12, ((tier == ShT_Small) ? 3 + op_rand(rng, 2) + lv / 8 : 5 + op_rand(rng, 3) + lv / 6));
        int names[FT_GUARD_KINDS], w[FT_GUARD_KINDS], nk = 0, wsum = 0;
        for (int k = 0; k < FT_GUARD_KINDS; k++)
            if (lv >= ft_guards[k].min_level)
            {
                names[nk] = k;
                w[nk] = ft_guards[k].weight;
                wsum += w[nk];
                nk++;
            }
        int gx[16], gy[16];
        got = sh_free_floor(p, rng, 1, 1, p->n - 2, p->n - 2, gx, gy, count);
        const int cap = op_level_cap();
        for (int i = 0; (i < got) && (nk > 0); i++)
        {
            int r = op_rand(rng, wsum), k = 0;
            while ((k < nk - 1) && (r >= w[k]))
                r -= w[k++];
            const long model = get_id(creature_desc, ft_guards[names[k]].name);
            const int level = max(1, min(lv - 1 + op_rand(rng, 3), min(cap, 255)));
            if (model > 0)
                sh_item(p, gx[i], gy[i], ShI_Guard, (uint16_t)model, (uint8_t)level, CHeroTsk_DefendRooms);
        }
    }
    (void)c;
    /* traps by size, a few more for a stronger finder */
    static const int trap_min[ShT_COUNT] = {6, 10, 14, 20, 28}, trap_span[ShT_COUNT] = {4, 5, 7, 9, 11};
    static const int trap_level0[ShT_COUNT] = {1, 4, 8, 12, 20};
    const int extra = min(6, max(0, lv - trap_level0[tier]) / 2);
    const int fortress_less = (p->family == ShF_Fortress) ? 2 : 0;
    sh_traps(p, rng, max(2, trap_min[tier] - fortress_less) + op_rand(rng, trap_span[tier]) + extra, false);
    /* decorations */
    static const int decor[ShT_COUNT] = {4, 6, 8, 12, 16};
    sh_decorate(p, rng, decor[tier]);
}

/******************************************************************************/
/* plans: decided, cached */

static TbBool op_plan_build(const struct OpState *st, struct OpPlan *p)
{
    if (st->family == ShF_Outpost)
    {
        if (st->tier == ShT_Legacy)
            return op_plan_make_legacy(st->rx, st->ry, p);
        return op_plan_make_sized(st->rx, st->ry, st->tier, st->found_level, p);
    }
    if ((st->tier >= ShT_COUNT))
        return false;
    struct WgStructRef ref;
    if (!ft_find_ref(st->rx, st->ry, &ref))
        return false;
    return ft_plan_make(&ref, st->tier, st->found_level, p);
}

static const struct OpPlan *op_plan_of(const struct OpState *st)
{
    if ((st == NULL) || (st->tier == ShT_None) || ((st->family == ShF_Fortress) && (st->tier == ShT_Legacy)))
        return NULL;
    const int stamps = world_stamp_count();
    const int i = (int)(op_mix((uint64_t)st->rx * 0x9E3779B1ULL ^ ((uint64_t)st->ry << 32) ^ ((uint64_t)st->family << 62)) %
        OP_CACHE);
    if (op_cache[i].valid && (op_cache[i].family == st->family) && (op_cache[i].rx == st->rx) && (op_cache[i].ry == st->ry) &&
        (op_cache[i].tier == st->tier) && (op_cache[i].found_level == st->found_level) && (op_cache[i].stamps == stamps))
        return op_cache[i].has ? &op_cache[i].plan : NULL;
    op_cache[i].valid = true;
    op_cache[i].family = st->family;
    op_cache[i].rx = st->rx;
    op_cache[i].ry = st->ry;
    op_cache[i].tier = st->tier;
    op_cache[i].found_level = st->found_level;
    op_cache[i].stamps = stamps;
    op_cache[i].has = op_plan_build(st, &op_cache[i].plan);
    return op_cache[i].has ? &op_cache[i].plan : NULL;
}

/** The world box an undecided outpost region may take (the largest outpost, or the old one): false when none. */
static TbBool op_region_reach(int64_t rx, int64_t ry, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    int64_t mx0, my0;
    uint64_t seed;
    if (!op_site(rx, ry, &mx0, &my0, &seed))
        return false;
    *x1 = mx0 - OP_PATH;
    *y1 = my0 - OP_PATH;
    *x2 = mx0 + SH_N_MAX - 1 + OP_PATH;
    *y2 = my0 + SH_N_MAX - 1 + OP_PATH;
    return true;
}

/** Decides an outpost region the first time a chunk it may cover is generated. */
static struct OpState *op_decide_outpost(int64_t rx, int64_t ry, int32_t gen_cx, int32_t gen_cy)
{
    struct OpState *st = op_state(ShF_Outpost, rx, ry, false);
    if (st != NULL)
        return st;
    static struct OpPlan legacy;
    int64_t x1, y1, x2, y2;
    if (op_plan_make_legacy(rx, ry, &legacy))
    {
        op_box(&legacy, &x1, &y1, &x2, &y2);
        if (sh_box_made(x1, y1, x2, y2, gen_cx, gen_cy))
        {
            st = op_state(ShF_Outpost, rx, ry, true);
            if (st != NULL)
            {
                st->tier = ShT_Legacy;
                st->cx = legacy.x0 + LEGACY_N / 2;
                st->cy = legacy.y0 + LEGACY_N / 2;
                SYNCLOG("World: the heroes outpost of region (%lld,%lld) stays as it was made (legacy)", (long long)rx, (long long)ry);
            }
            return st;
        }
    }
    if (!op_region_reach(rx, ry, &x1, &y1, &x2, &y2))
        return NULL;
    st = op_state(ShF_Outpost, rx, ry, true);
    if (st == NULL)
        return NULL;
    st->cx = (x1 + x2) / 2;
    st->cy = (y1 + y2) / 2;
    if (sh_box_made(x1, y1, x2, y2, gen_cx, gen_cy))
    {
        st->tier = ShT_None; /* its ground was made without it */
        return st;
    }
    const int lv = sh_finder_level(st->cx, st->cy);
    st->found_level = (uint8_t)max(1, min(lv, 255));
    static struct OpPlan trial;
    int tier = sh_tier_for_level(lv);
    while ((tier >= ShT_Small) && !(op_plan_make_sized(rx, ry, tier, st->found_level, &trial) && op_plan_clear(&trial)))
        tier--;
    st = op_state(ShF_Outpost, rx, ry, false); /* the trials may have moved the table */
    if (st == NULL)
        return NULL;
    if (tier < ShT_Small)
    {
        st->tier = ShT_None; /* not even the smallest fits between the generator's structures */
        return st;
    }
    st->tier = (uint8_t)tier;
    SYNCLOG("World: a %s heroes outpost at world slab (%lld,%lld) (finder's best level %d)", sh_tier_names[st->tier],
        (long long)st->cx, (long long)st->cy, lv);
    return st;
}

static struct OpState *ft_decide(const struct WgStructRef *ref, int32_t gen_cx, int32_t gen_cy)
{
    struct OpState *st = op_state(ShF_Fortress, ref->id.start_cx, ref->id.start_cy, false);
    if (st != NULL)
        return st;
    st = op_state(ShF_Fortress, ref->id.start_cx, ref->id.start_cy, true);
    if (st == NULL)
        return NULL;
    st->cx = ref->origin_x;
    st->cy = ref->origin_y;
    const int64_t r = max((int64_t)FT_REACH, (int64_t)max(ref->origin_x - ref->bbox_x1, ref->bbox_x2 - ref->origin_x));
    if (sh_box_made(ref->origin_x - r, ref->origin_y - r, ref->origin_x + r, ref->origin_y + r, gen_cx, gen_cy))
    {
        st->tier = ShT_Legacy; /* the generator's fortress is there already */
        return st;
    }
    const int lv = sh_finder_level(ref->origin_x, ref->origin_y);
    int tier = sh_tier_for_level(lv);
    static struct OpPlan trial;
    while ((tier > ShT_Small) && !(ft_plan_make(ref, tier, max(1, min(lv, 255)), &trial) && ft_fits(&trial)))
        tier--;
    st = op_state(ShF_Fortress, ref->id.start_cx, ref->id.start_cy, false);
    if (st == NULL)
        return NULL;
    st->found_level = (uint8_t)max(1, min(lv, 255));
    st->tier = (uint8_t)tier;
    SYNCLOG("World: a %s hero fortress at world slab (%lld,%lld) (finder's best level %d)", sh_tier_names[tier],
        (long long)ref->origin_x, (long long)ref->origin_y, lv);
    return st;
}

/******************************************************************************/
/* the chunks */
static void op_overlay_thing(struct OutpostOverlay *ov, int32_t cx, int32_t cy, int64_t wx, int64_t wy, uint8_t oclass,
    uint16_t model, uint8_t heroes, uint16_t extra, uint8_t level)
{
    if ((wx < (int64_t)cx * WG_CHUNK) || (wx >= (int64_t)cx * WG_CHUNK + WG_CHUNK) || (wy < (int64_t)cy * WG_CHUNK) ||
        (wy >= (int64_t)cy * WG_CHUNK + WG_CHUNK) || (ov->thing_count >= OUTPOST_OVERLAY_THINGS) || (model == 0))
        return;
    struct OutpostOverlayThing *t = &ov->things[ov->thing_count++];
    t->oclass = oclass;
    t->heroes = heroes;
    t->model = model;
    t->level = level;
    t->x = (int32_t)(((wx - (int64_t)cx * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    t->y = (int32_t)(((wy - (int64_t)cy * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    t->extra = extra;
}

static int sh_overlay_plan(const struct OpPlan *p, int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    const ThingModel steel = (ThingModel)get_id(door_desc, "STEEL");
    const ThingModel braced = (ThingModel)get_id(door_desc, "BRACED");
    const ThingModel gate = (ThingModel)get_id(object_desc, "HERO_GATE");
    const TbBool fortress = (p->family == ShF_Fortress);
    const TbBool sized = (p->tier < ShT_COUNT);
    struct WgWorld *gen = world_generator();
    int decided = 0;
    /* fortresses: the box round the middle the old fortress took (the land's own ground again where this one has
       nothing); outposts: their box */
    const int64_t mcx = p->x0 + p->n / 2, mcy = p->y0 + p->n / 2;
    const int64_t fx1 = fortress ? min(mcx - p->fill, p->x0) : p->x0, fy1 = fortress ? min(mcy - p->fill, p->y0) : p->y0;
    const int64_t fx2 = fortress ? max(mcx + p->fill, p->x0 + p->n - 1) : p->x0 + p->n - 1;
    const int64_t fy2 = fortress ? max(mcy + p->fill, p->y0 + p->n - 1) : p->y0 + p->n - 1;
    for (int64_t gy = max(fy1, wy1); gy <= min(fy2, wy2); gy++)
        for (int64_t gx = max(fx1, wx1); gx <= min(fx2, wx2); gx++)
        {
            const int x = (int)(gx - p->x0), y = (int)(gy - p->y0);
            uint8_t c = sh_in(p, x, y) ? p->cell[OPI(p, x, y)] : (sized ? OpC_None : OpC_Earth);
            const TbBool in_old = fortress && (llabs(gx - mcx) <= p->fill) && (llabs(gy - mcy) <= p->fill);
            if (c == OpC_None)
            {
                if (!in_old)
                    continue; /* the land's own, as the generator made it */
                /* the old fortress's ground: the land's own again */
                const uint8_t g = sh_in(p, x, y) ? p->ground[OPI(p, x, y)] : (uint8_t)((gen != NULL) ? wg_terrain_at(gen, gx, gy) : WgS_Earth);
                static const SlabKind gk[] = {[WgS_Rock] = SlbT_ROCK, [WgS_Gold] = SlbT_GOLD, [WgS_DenseGold] = SlbT_DENSEGOLD,
                    [WgS_Gems] = SlbT_GEMS, [WgS_Earth] = SlbT_EARTH, [WgS_Path] = SlbT_PATH, [WgS_Claimed] = SlbT_PATH,
                    [WgS_Lava] = SlbT_LAVA, [WgS_Water] = SlbT_WATER};
                const int i = (int)((gy - wy1) * WG_CHUNK + (gx - wx1));
                ov->set[i] = 1;
                ov->heroes[i] = 0;
                ov->wall[i] = 0;
                ov->door[i] = 0;
                ov->kind[i] = (g < sizeof(gk) / sizeof(gk[0]) && gk[g] != 0) ? gk[g] : ((g == WgS_Rock) ? SlbT_ROCK : SlbT_EARTH);
                decided++;
                continue;
            }
            const int i = (int)((gy - wy1) * WG_CHUNK + (gx - wx1));
            ov->set[i] = 1;
            ov->heroes[i] = 1;
            ov->wall[i] = 0;
            ov->door[i] = 0;
            switch (c)
            {
            case OpC_Wall: ov->kind[i] = SlbT_WALLDRAPE; ov->wall[i] = 1; break;
            case OpC_Floor: case OpC_Causeway: case OpC_Hall: ov->kind[i] = SlbT_CLAIMED; break;
            case OpC_Lava: ov->kind[i] = SlbT_LAVA; ov->heroes[i] = 0; break;
            case OpC_Water: ov->kind[i] = SlbT_WATER; ov->heroes[i] = 0; break;
            case OpC_Earth: ov->kind[i] = SlbT_EARTH; ov->heroes[i] = 0; break;
            case OpC_Path: ov->kind[i] = SlbT_PATH; ov->heroes[i] = 0; break;
            case OpC_Heart: ov->kind[i] = SlbT_DUNGHEART; break;
            case OpC_Room: ov->kind[i] = p->room[OPI(p, x, y)]; break;
            case OpC_Door:
                ov->kind[i] = SlbT_CLAIMED;
                ov->door[i] = (uint8_t)(fortress ? ((braced > 0) ? braced : 2) : ((steel > 0) ? steel : 3));
                break;
            default: ov->kind[i] = SlbT_ROCK; ov->heroes[i] = 0; break;
            }
            decided++;
        }
    /* the sized ones' ways out: the rock beyond the box's edge dug to earth, to the nearest ground a keeper can reach */
    for (int e = 0; sized && (e < p->nexits); e++)
    {
        static const int edx[4] = {0, 1, 0, -1}, edy[4] = {-1, 0, 1, 0};
        for (int k = 1; k <= p->exlen[e]; k++)
        {
            const int64_t gx = p->x0 + p->exx[e] + (int64_t)edx[p->exdir[e] & 3] * k;
            const int64_t gy = p->y0 + p->exy[e] + (int64_t)edy[p->exdir[e] & 3] * k;
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
    }
    if ((p->family == ShF_Outpost) && !sized)
    {
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
    }
    for (int g = 0; g < p->ngates; g++)
        op_overlay_thing(ov, cx, cy, p->x0 + p->gx[g], p->y0 + p->gy[g], TCls_Object, (uint16_t)max(gate, 0), 1, 0, 0);
    for (int t = 0; t < p->ntraps; t++)
    {
        const struct TrapConfigStats *trapst = get_trap_model_stats(p->tmodel[t]);
        op_overlay_thing(ov, cx, cy, p->x0 + p->tx[t], p->y0 + p->ty[t], TCls_Trap, p->tmodel[t], 1,
            (uint16_t)max(trapst->shots, 1), 0);
    }
    for (int k = 0; k < p->nitems; k++)
    {
        const struct ShItem *it = &p->items[k];
        if (it->kind == ShI_Guard)
            op_overlay_thing(ov, cx, cy, p->x0 + it->x, p->y0 + it->y, TCls_Creature, it->model, 1, it->extra, it->level);
        else
            op_overlay_thing(ov, cx, cy, p->x0 + it->x, p->y0 + it->y, TCls_Object, it->model, 0, it->extra, 0);
    }
    return decided;
}

int world_outpost_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    memset(ov, 0, sizeof(*ov));
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return 0;
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    int decided = 0;
    /* the fortresses first: an outpost never overlaps one (the fortress keeps a smaller size) */
    {
        struct WgStructRef refs[8];
        const int n = wg_structures_in(gen, wx1 - FT_REACH, wy1 - FT_REACH, wx2 + FT_REACH, wy2 + FT_REACH,
            1u << WgT_Fortress, refs, 8);
        for (int s = 0; (s < n) && (s < 8); s++)
        {
            const struct OpState *st = ft_decide(&refs[s], cx, cy);
            const struct OpPlan *p = op_plan_of(st);
            if (p == NULL)
                continue;
            decided += sh_overlay_plan(p, cx, cy, ov);
        }
    }
    if (!op_on)
        return decided;
    const int64_t reach = SH_N_MAX + OP_PATH;
    for (int64_t ry = op_floor_div(wy1 - reach, OP_REGION); ry <= op_floor_div(wy2 + reach, OP_REGION); ry++)
    {
        for (int64_t rx = op_floor_div(wx1 - reach, OP_REGION); rx <= op_floor_div(wx2 + reach, OP_REGION); rx++)
        {
            /* decide only the regions whose outpost (old or new) may reach this chunk */
            int64_t bx1, by1, bx2, by2;
            TbBool may = op_region_reach(rx, ry, &bx1, &by1, &bx2, &by2) && (bx2 >= wx1) && (bx1 <= wx2) && (by2 >= wy1) &&
                (by1 <= wy2);
            static struct OpPlan legacy;
            if (!may && op_plan_make_legacy(rx, ry, &legacy))
            {
                op_box(&legacy, &bx1, &by1, &bx2, &by2);
                may = (bx2 >= wx1) && (bx1 <= wx2) && (by2 >= wy1) && (by1 <= wy2);
            }
            if (!may)
                continue;
            const struct OpPlan *p = op_plan_of(op_decide_outpost(rx, ry, cx, cy));
            if (p == NULL)
                continue;
            op_box(p, &bx1, &by1, &bx2, &by2);
            if ((bx2 < wx1) || (bx1 > wx2) || (by2 < wy1) || (by1 > wy2))
                continue;
            decided += sh_overlay_plan(p, cx, cy, ov);
        }
    }
    return decided;
}

TbBool world_outpost_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    if ((world_generator() == NULL) || !op_on)
        return false;
    const int64_t reach = SH_N_MAX + OP_PATH;
    for (int64_t ry = op_floor_div(y1 - reach, OP_REGION); ry <= op_floor_div(y2 + reach, OP_REGION); ry++)
        for (int64_t rx = op_floor_div(x1 - reach, OP_REGION); rx <= op_floor_div(x2 + reach, OP_REGION); rx++)
        {
            int64_t bx1, by1, bx2, by2;
            const struct OpState *st = op_state(ShF_Outpost, rx, ry, false);
            if (st != NULL)
            {
                const struct OpPlan *p = op_plan_of(st);
                if (p == NULL)
                    continue;
                sh_plan_extent(p, &bx1, &by1, &bx2, &by2);
            } else
            if (!op_region_reach(rx, ry, &bx1, &by1, &bx2, &by2))
                continue;
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
            const struct OpState *st = op_state(ShF_Outpost, rx, ry, false);
            if (st == NULL)
            {
                int64_t bx1, by1, bx2, by2;
                if (!op_region_reach(rx, ry, &bx1, &by1, &bx2, &by2))
                    continue;
                st = op_decide_outpost(rx, ry, INT32_MIN, INT32_MIN); /* a test asks: decide it now */
            }
            const struct OpPlan *p = op_plan_of(st);
            if (p == NULL)
                continue;
            const int64_t dx = p->x0 + p->n / 2 - wx, dy = p->y0 + p->n / 2 - wy;
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
    SYNCLOG("World test: %s outpost at slab (%lld,%lld), %dx%d, entrance side %d, %d portals, %d traps",
        sh_tier_names[best->tier], (long long)best->x0, (long long)best->y0, best->n, best->n, (int)best->dir,
        best->ngates, best->ntraps);
    return true;
}

/******************************************************************************/
/* the heroes they send */

void world_outposts_reset(void)
{
    free(op_states);
    op_states = NULL;
    op_state_count = op_state_cap = 0;
    memset(op_cache, 0, sizeof(op_cache));
    op_on = true;
}

void world_outposts_set_enabled(TbBool on)
{
    op_on = on;
}

static TbBool op_keeper_owner(PlayerNumber owner)
{
    return (owner >= 0) && (owner < PLAYERS_COUNT) && (owner != PLAYER_GOOD) && (owner != PLAYER_NEUTRAL) &&
        (owner != game.neutral_player_num) && player_is_keeper(owner);
}

static int op_spawn_model(MapSlabCoord sx, MapSlabCoord sy, long model, int level, struct OpRng *rng)
{
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
    level += world_coop_level_bonus_at_slab(sx, sy); /* co-op: a level higher by a party's start */
    init_creature_level(thing, (CrtrExpLevel)(max(1, min(level, cap)) - 1));
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (!creature_control_invalid(cctrl))
    {
        cctrl->party.objective = CHeroTsk_DefendRooms;
        cctrl->party.original_objective = CHeroTsk_DefendRooms; /* not wild: they stay through the day; they roam */
        cctrl->party.target_plyr_idx = -1;
    }
    return 1;
}

static int op_spawn(MapSlabCoord sx, MapSlabCoord sy, const char *name, int level, struct OpRng *rng)
{
    return op_spawn_model(sx, sy, get_id(creature_desc, name), level, rng);
}

static const char *const op_hero_names[4] = {"BARBARIAN", "ARCHER", "MONK", "THIEF"};

/* From Medium up a portal can send any hero (a Knight comes on top) */
static const struct { const char *name; int weight; } sh_any_heroes[] = {
    {"ARCHER", 3}, {"BARBARIAN", 3}, {"DWARFA", 3}, {"THIEF", 3}, {"MONK", 3}, {"WIZARD", 2}, {"WITCH", 2},
    {"FAIRY", 2}, {"SAMURAI", 2}, {"GIANT", 1}, {"KNIGHT", 1},
};
#define SH_ANY_HEROES ((int)(sizeof(sh_any_heroes) / sizeof(sh_any_heroes[0])))

/** A portal sends n heroes (the four old kinds, or any kind) at levels round the given one, and a Knight if asked. */
static int op_gate_spawn(const struct OpPlan *p, int g, int n, TbBool knight, int level, TbBool any, int knight_bonus,
    struct OpRng *rng)
{
    MapSlabCoord sx, sy;
    if (!world_engine_slab(p->x0 + p->gx[g], p->y0 + p->gy[g], &sx, &sy))
        return -1;
    /* circles (world_circles.c): the heroes a portal sends are as strong as the circle it stands in */
    level = world_circles_level_at_slab(sx, sy, world_circles_hero_floor_at_slab(sx, sy), "an outpost's heroes");
    int made = 0;
    int wsum = 0;
    for (int k = 0; k < SH_ANY_HEROES; k++)
        wsum += sh_any_heroes[k].weight;
    for (int i = 0; i < n; i++)
    {
        const int lv = level - 1 + op_rand(rng, 3);
        if (any)
        {
            int r = op_rand(rng, wsum), k = 0;
            while ((k < SH_ANY_HEROES - 1) && (r >= sh_any_heroes[k].weight))
                r -= sh_any_heroes[k++].weight;
            made += op_spawn(sx, sy, sh_any_heroes[k].name, lv, rng);
        } else
            made += op_spawn(sx, sy, op_hero_names[op_rand(rng, 4)], lv, rng);
    }
    if (knight)
        made += op_spawn(sx, sy, "KNIGHT", level + knight_bonus, rng);
    return made;
}

/** The first keeper creature on a box of engine slabs, or NULL. */
static struct Thing *op_keeper_creature_in(MapSlabCoord sx1, MapSlabCoord sy1, MapSlabCoord sx2, MapSlabCoord sy2)
{
    const MapSubtlCoord x1 = sx1 * STL_PER_SLB, y1 = sy1 * STL_PER_SLB;
    const MapSubtlCoord x2 = sx2 * STL_PER_SLB + STL_PER_SLB - 1, y2 = sy2 * STL_PER_SLB + STL_PER_SLB - 1;
    for (MapSubtlCoord y = max(y1, 0); y <= min(y2, (MapSubtlCoord)game.map_subtiles_y - 1); y++)
        for (MapSubtlCoord x = max(x1, 0); x <= min(x2, (MapSubtlCoord)game.map_subtiles_x - 1); x++)
        {
            long i = get_mapwho_thing_index(get_map_block_at(x, y));
            long guard = 0;
            while ((i > 0) && (guard++ < THINGS_COUNT))
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if (thing_is_creature(thing) && op_keeper_owner(thing->owner) && !creature_is_being_unconscious(thing))
                    return thing;
            }
        }
    return NULL;
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

/** Small, Moderate and the old outposts: the entrance wakes the portals, the heart's portal its own. */
static void op_runtime_entrance(const struct OpPlan *p, struct OpState *st, struct OpRng *rng)
{
    uint8_t flags = st->flags;
    if (!(flags & OPF_Opened))
    {
        MapSlabCoord sx, sy;
        if (world_engine_slab(p->x0 + p->ex, p->y0 + p->ey, &sx, &sy))
        {
            const struct Thing *door = get_door_for_position(slab_subtile_center(sx), slab_subtile_center(sy));
            const TbBool standing = thing_exists(door) && (door->owner == PLAYER_GOOD);
            if (standing && !(flags & OPF_DoorSeen))
                st->flags |= OPF_DoorSeen;
            if (!standing && (flags & OPF_DoorSeen))
            {
                st->flags |= OPF_Opened;
                st->level = (uint8_t)max(1, min(op_breaker_level(sx, sy), 255));
                SYNCLOG("World: the heroes outpost at slab (%lld,%lld) is broken into; its portals wake (heroes level %d)",
                    (long long)p->x0, (long long)p->y0, (int)st->level);
            }
            flags = st->flags;
        }
    }
    /* the portals (not the heart's) send their heroes, each once, when its ground is on the map */
    if (flags & OPF_Opened)
    {
        for (int g = 1; g < p->ngates; g++)
        {
            if (st->gates_done & (1u << g))
                continue;
            const int n = (p->tier == ShT_Small) ? 2 + op_rand(rng, 2) : ((p->tier == ShT_Moderate) ? 3 + op_rand(rng, 3) :
                4 + op_rand(rng, 3));
            const int made = op_gate_spawn(p, g, n, false, st->level, false, 0, rng);
            if (made < 0)
                continue;
            st->gates_done |= (uint16_t)(1u << g);
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
                st->flags |= OPF_HeartWoken;
                st->gates_done |= 1u;
                const int level = world_keeper_top_level(near->owner);
                const int n = (p->tier == ShT_Small) ? 4 + op_rand(rng, 2) : ((p->tier == ShT_Moderate) ? 6 + op_rand(rng, 3) :
                    7 + op_rand(rng, 3));
                const int made = op_gate_spawn(p, 0, n, true, level, false, 2, rng);
                SYNCLOG("World: player %d's creatures reach the outpost heart's portal: %d heroes and a Knight (level %d)",
                    (int)near->owner, max(made - 1, 0), level);
            }
        }
    }
}

/** Medium and up (outposts and fortresses): each portal room sends its heroes and a Knight when broken into. */
static void sh_runtime_rooms(const struct OpPlan *p, struct OpState *st, struct OpRng *rng)
{
    for (int g = 0; g < p->ngates; g++)
    {
        if (!(p->gflags[g] & ShG_Breach) || (st->gates_done & (1u << g)))
            continue;
        MapSlabCoord sx1, sy1, sx2, sy2;
        if (!world_engine_slab(p->x0 + p->grx1[g], p->y0 + p->gry1[g], &sx1, &sy1) ||
            !world_engine_slab(p->x0 + p->grx2[g], p->y0 + p->gry2[g], &sx2, &sy2))
            continue;
        const struct Thing *in = op_keeper_creature_in(min(sx1, sx2), min(sy1, sy2), max(sx1, sx2), max(sy1, sy2));
        if (in == NULL)
            continue;
        const int level = world_keeper_top_level(in->owner);
        const int n = max(3, min(2 + level / 3, 10)) + op_rand(rng, 2) + (p->tier - ShT_Medium);
        const int made = op_gate_spawn(p, g, n, true, level, true, 1, rng);
        if (made < 0)
            continue;
        st->gates_done |= (uint16_t)(1u << g);
        st->flags |= OPF_DoorSeen | OPF_Opened;
        SYNCLOG("World: player %d breaks into a %s %s's portal room %d: %d heroes and a Knight (level %d)", (int)in->owner,
            sh_tier_names[p->tier], (p->family == ShF_Fortress) ? "fortress" : "outpost", g, max(made - 1, 0), level);
    }
}

static void op_runtime(const struct OpPlan *p, struct OpState *st)
{
    struct OpRng rng = {op_mix(world_seed() ^ ((uint64_t)game.play_gameturn << 20) ^ (uint64_t)p->rx * 31 ^ (uint64_t)p->ry ^
        ((uint64_t)p->family << 60))};
    if (p->tier >= ShT_Medium && p->tier < ShT_COUNT)
        sh_runtime_rooms(p, st, &rng);
    else if (p->family == ShF_Outpost)
        op_runtime_entrance(p, st, &rng);
}

#define OP_LOADED_MAX 16384  /* loaded chunks looked at (more than any world loads) */
#define OP_SEEN_SIZE 4096    /* the regions of a pass, an open-addressing set */
void world_outposts_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % 8) != 3))
        return;
    /* the fortresses whose portal rooms may still wake, while their middle is on the map */
    for (int k = 0; k < op_state_count; k++)
    {
        struct OpState *st = &op_states[k];
        if ((st->family != ShF_Fortress) || (st->tier < ShT_Medium) || (st->tier >= ShT_COUNT))
            continue;
        MapSlabCoord sx, sy;
        if (!world_engine_slab(st->cx, st->cy, &sx, &sy))
            continue;
        const struct OpPlan *p = op_plan_of(st);
        if (p != NULL)
        {
            st = op_state(ShF_Fortress, p->rx, p->ry, false); /* the plan's making may have moved the table */
            if (st != NULL)
                op_runtime(p, st);
        }
    }
    if (!op_on)
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
        struct OpState *st = op_state(ShF_Outpost, order_x[k], order_y[k], false);
        if (st == NULL)
        {
            /* an older world: its outpost was made before the sizes (and before what happened to it was saved) */
            static struct OpPlan legacy;
            if (!op_plan_make_legacy(order_x[k], order_y[k], &legacy))
                continue;
            int64_t x1, y1, x2, y2;
            op_box(&legacy, &x1, &y1, &x2, &y2);
            if (!sh_box_made(x1, y1, x2, y2, INT32_MIN, INT32_MIN))
                continue;
            st = op_state(ShF_Outpost, order_x[k], order_y[k], true);
            if (st == NULL)
                continue;
            st->tier = ShT_Legacy;
            st->cx = legacy.x0 + LEGACY_N / 2;
            st->cy = legacy.y0 + LEGACY_N / 2;
        }
        const struct OpPlan *p = op_plan_of(st);
        if (p == NULL)
            continue;
        st = op_state(ShF_Outpost, order_x[k], order_y[k], false);
        if (st != NULL)
            op_runtime(p, st);
    }
}

void world_outpost_heart_destroyed(const struct Thing *heartng)
{
    if (!world_mode_active() || !thing_exists(heartng) || (heartng->owner != PLAYER_GOOD))
        return;
    int64_t wx, wy;
    world_slab_to_world(subtile_slab(heartng->mappos.x.stl.num), subtile_slab(heartng->mappos.y.stl.num), &wx, &wy);
    const int64_t rx0 = op_floor_div(wx, OP_REGION), ry0 = op_floor_div(wy, OP_REGION);
    for (int64_t ry = ry0 - 1; ry <= ry0 + 1; ry++)
        for (int64_t rx = rx0 - 1; rx <= rx0 + 1; rx++)
        {
            struct OpState *st = op_state(ShF_Outpost, rx, ry, false);
            const struct OpPlan *p = op_plan_of(st);
            if ((p == NULL) || (llabs(wx - (p->x0 + p->hx)) > 1) || (llabs(wy - (p->y0 + p->hy)) > 1))
                continue;
            st = op_state(ShF_Outpost, rx, ry, false);
            if ((st == NULL) || (st->flags & OPF_Looted))
                return;
            st->flags |= OPF_Looted;
            const int books = sh_books_for(p->tier, op_mix(world_seed() ^ (uint64_t)rx * 0x2545F491ULL ^ (uint64_t)ry ^ 0x48525442ULL));
            SYNCLOG("World: the %s heroes outpost at slab (%lld,%lld) falls: its heart drops %d heroes' spellbook%s",
                sh_tier_names[p->tier], (long long)p->x0, (long long)p->y0, books, (books == 1) ? "" : "s");
            static const int bdx[4] = {0, 3, -3, 0}, bdy[4] = {0, 0, 0, 3};
            for (int b = 0; b < books; b++)
                world_progress_drop_outpost_book(heartng->mappos.x.stl.num + bdx[b & 3], heartng->mappos.y.stl.num + bdy[b & 3]);
            return;
        }
}

/******************************************************************************/
/* the heroes roam */

TbBool world_stronghold_hero_wanders(struct Thing *creatng)
{
    if (!world_mode_active() || (creatng->owner != PLAYER_GOOD) || !thing_is_creature(creatng))
        return false;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return false;
    const unsigned char orig = cctrl->party.original_objective;
    /* outposts' and fortresses' heroes (and the generator's fortress guards); not raids, not wild creatures */
    if ((orig != CHeroTsk_DefendRooms) && (orig != CHeroTsk_Default))
        return false;
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    for (int tries = 0; tries < 16; tries++)
    {
        /* far first, nearer when far fails (a hero walled in with earth all round finds only its own floor) */
        const int lo = (tries < 8) ? SH_WANDER_MIN : 2;
        const long dist = lo + (long)THING_RANDOM(creatng, SH_WANDER_MAX - lo + 1);
        const long angle = (long)THING_RANDOM(creatng, 2048);
        const MapSlabCoord sx = cx + (MapSlabCoord)((LbSinL(angle) * dist) >> 16);
        const MapSlabCoord sy = cy - (MapSlabCoord)((LbCosL(angle) * dist) >> 16);
        if ((sx < 1) || (sy < 1) || (sx >= game.map_tiles_x - 1) || (sy >= game.map_tiles_y - 1))
            continue;
        struct SlabMap *slb = get_slabmap_block(sx, sy);
        const struct SlabConfigStats *slabst = get_slab_stats(slb);
        if (((slabst->block_flags & (SlbAtFlg_Filled | SlbAtFlg_Blocking)) != 0) || slab_kind_is_liquid(slb->kind))
            continue;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(sx));
        pos.y.val = subtile_coord_center(slab_subtile_center(sy));
        pos.z.val = 0;
        if (setup_person_move_to_coord(creatng, &pos, NavRtF_Default))
        {
            creatng->continue_state = CrSt_GoodDoingNothing;
            cctrl->wait_to_turn = game.play_gameturn + 100 + THING_RANDOM(creatng, 300);
            return true;
        }
    }
    /* nowhere to go: amble about here for a while (rather than the hero AI's own idea of defending rooms it hasn't
       got or attacking whoever it finds) */
    cctrl->wait_to_turn = game.play_gameturn + 100 + THING_RANDOM(creatng, 200);
    if (creature_choose_random_destination_on_valid_adjacent_slab(creatng))
        creatng->continue_state = CrSt_GoodDoingNothing;
    return true;
}

/******************************************************************************/
/* test support */

/** One plan as text in the log, and whether every floor, room and portal of it can be walked to from its way in. */
static int sh_dump_plan(const struct OpPlan *p, const char *what)
{
    static char row[SH_N_MAX + 1];
    static const char glyph[] = {'#', 'W', '.', 'L', '=', 'H', 'R', 'D', '~', ',', ':', ' ', '+'};
    static const char land[] = {'#', '$', '$', '*', ' ', '_', '_', 'l', 'w'};
    SYNCLOG("World test: %s %s %s %dx%d at (%lld,%lld), %d portals, %d traps, %d things", what, sh_tier_names[p->tier],
        (p->family == ShF_Fortress) ? "fortress" : "outpost", p->n, p->n, (long long)p->x0, (long long)p->y0, p->ngates,
        p->ntraps, p->nitems);
    for (int y = 0; y < p->n; y++)
    {
        for (int x = 0; x < p->n; x++)
        {
            const uint8_t c = p->cell[OPI(p, x, y)];
            char ch = (c < sizeof(glyph)) ? glyph[c] : '?';
            if (c == OpC_None)
            {
                const uint8_t g = p->ground[OPI(p, x, y)];
                ch = (p->tier < ShT_COUNT) ? ((g < sizeof(land)) ? land[g] : '?') : '#';
            }
            if (sh_trap_at(p, x, y))
                ch = 'T';
            if (sh_item_at(p, x, y))
                ch = 'o';
            if (sh_gate_at(p, x, y))
                ch = 'G';
            row[x] = ch;
        }
        row[p->n] = 0;
        SYNCLOG("World test:   |%s|", row);
    }
    /* the walk: from the entrance (outposts) or every door's path at the box's edge (fortresses), over floors,
       rooms, doors, causeways, the heart, paths, water and lava */
    static uint8_t seen[SH_N_MAX * SH_N_MAX];
    static int stack[SH_N_MAX * SH_N_MAX];
    memset(seen, 0, sizeof(seen));
    int top = 0;
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
        {
            const TbBool edge = (x == 0) || (y == 0) || (x == p->n - 1) || (y == p->n - 1);
            const uint8_t c = p->cell[OPI(p, x, y)];
            TbBool start = (p->family == ShF_Outpost) ? ((x == p->ex) && (y == p->ey)) : (edge && (c == OpC_Path));
            for (int e = 0; e < p->nexits; e++)
                start = start || ((p->exx[e] == x) && (p->exy[e] == y));
            if (start)
            {
                seen[OPI(p, x, y)] = 1;
                stack[top++] = OPI(p, x, y);
            }
        }
    while (top > 0)
    {
        const int i = stack[--top];
        const int x = i % p->n, y = i / p->n;
        static const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++)
        {
            const int nx = x + ddx[k], ny = y + ddy[k];
            if (!sh_in(p, nx, ny) || seen[OPI(p, nx, ny)])
                continue;
            const uint8_t c = p->cell[OPI(p, nx, ny)];
            if ((c == OpC_Rock) || (c == OpC_Wall) || (c == OpC_Earth) || (c == OpC_None))
                continue;
            seen[OPI(p, nx, ny)] = 1;
            stack[top++] = OPI(p, nx, ny);
        }
    }
    int lost = 0, lost_gates = 0;
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
        {
            const uint8_t c = p->cell[OPI(p, x, y)];
            if (((c == OpC_Floor) || (c == OpC_Room) || (c == OpC_Heart) || (c == OpC_Door)) && !seen[OPI(p, x, y)])
                lost++;
        }
    for (int g = 0; g < p->ngates; g++)
        if (!seen[OPI(p, p->gx[g], p->gy[g])])
            lost_gates++;
    /* doors: walls on two opposite sides, open on the other two */
    int bad_doors = 0;
    for (int y = 0; y < p->n; y++)
        for (int x = 0; x < p->n; x++)
        {
            if (p->cell[OPI(p, x, y)] != OpC_Door)
                continue;
#define SOLID(xx, yy) (sh_in(p, xx, yy) && ((sh_at(p, xx, yy) == OpC_Wall) || (sh_at(p, xx, yy) == OpC_Rock) || \
            ((sh_at(p, xx, yy) == OpC_None) && (p->ground[OPI(p, xx, yy)] <= WgS_Earth))))
            const TbBool n = SOLID(x, y - 1), s = SOLID(x, y + 1), e = SOLID(x + 1, y), w = SOLID(x - 1, y);
#undef SOLID
            if (!((e && w && !n && !s) || (n && s && !e && !w)))
            {
                if (bad_doors < 4)
                    SYNCLOG("World test:   door at (%d,%d) is not between two walls (n%d e%d s%d w%d)", x, y, n, e, s, w);
                bad_doors++;
            }
        }
    SYNCLOG("World test: %s walk: %d open cells and %d portals can't be reached, %d doors badly placed", what, lost,
        lost_gates, bad_doors);
    return lost + lost_gates + bad_doors;
}

void world_strongholds_test_dump(void)
{
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return;
    static struct OpPlan p;
    int problems = 0;
    /* outposts: the first regions out from the start that have a site, one per size */
    int tier = 0;
    for (int r = 1; (r < 12) && (tier < ShT_COUNT); r++)
        for (int64_t ry = -r; (ry <= r) && (tier < ShT_COUNT); ry++)
            for (int64_t rx = -r; (rx <= r) && (tier < ShT_COUNT); rx++)
            {
                if ((llabs(rx) != r) && (llabs(ry) != r))
                    continue;
                if (op_plan_make_sized(rx, ry, tier, 1 + tier * 5, &p))
                {
                    problems += sh_dump_plan(&p, "dump");
                    tier++;
                }
            }
    /* fortresses: the generator's nearest ones, one per size (built whether or not the size would fit there) */
    struct WgStructRef refs[16];
    const int n = wg_structures_in(gen, -1500, -1500, 1500, 1500, 1u << WgT_Fortress, refs, 16);
    for (int t = 0; (t < ShT_COUNT) && (t < n); t++)
        if (ft_plan_make(&refs[t], t, 1 + t * 5, &p))
            problems += sh_dump_plan(&p, "dump");
    SYNCLOG("World test: strongholds dumped: %d problems", problems);
}

TbBool world_stronghold_test_target(int family, int64_t wx, int64_t wy, int64_t *x0, int64_t *y0, int *n, int64_t *gx,
    int64_t *gy, int64_t *ex, int64_t *ey, int *tier)
{
    static struct OpPlan found;
    if (family == ShF_Outpost)
    {
        int64_t hx, hy, ggx, ggy;
        int odx, ody;
        if (!world_outpost_nearest(wx, wy, x0, y0, ex, ey, &odx, &ody, &hx, &hy, &ggx, &ggy))
            return false;
        const struct OpState *st = op_state(ShF_Outpost, op_floor_div(*x0 + 17, OP_REGION), op_floor_div(*y0 + 17, OP_REGION), false);
        (void)st;
        /* find the plan again for its size and first breach portal */
        const int64_t rx0 = op_floor_div(wx, OP_REGION), ry0 = op_floor_div(wy, OP_REGION);
        for (int64_t ry = ry0 - 3; ry <= ry0 + 3; ry++)
            for (int64_t rx = rx0 - 3; rx <= rx0 + 3; rx++)
            {
                const struct OpPlan *p = op_plan_of(op_state(ShF_Outpost, rx, ry, false));
                if ((p == NULL) || (p->x0 != *x0) || (p->y0 != *y0))
                    continue;
                found = *p;
            }
    } else
    {
        struct WgStructRef refs[16];
        const int nr = wg_structures_in(world_generator(), wx - 1500, wy - 1500, wx + 1500, wy + 1500, 1u << WgT_Fortress,
            refs, 16);
        int best = -1;
        int64_t best_d = 0;
        for (int i = 0; (i < nr) && (i < 16); i++)
        {
            const int64_t dx = refs[i].origin_x - wx, dy = refs[i].origin_y - wy;
            if ((best < 0) || (dx * dx + dy * dy < best_d))
            {
                best = i;
                best_d = dx * dx + dy * dy;
            }
        }
        if (best < 0)
            return false;
        const struct OpPlan *p = op_plan_of(ft_decide(&refs[best], INT32_MIN, INT32_MIN));
        if (p == NULL)
            return false;
        found = *p;
        *x0 = p->x0;
        *y0 = p->y0;
        *ex = p->x0;
        *ey = p->y0;
    }
    *n = found.n;
    *tier = found.tier;
    *gx = found.x0 + found.gx[0];
    *gy = found.y0 + found.gy[0];
    for (int g = 0; g < found.ngates; g++)
        if (found.gflags[g] & ShG_Breach)
        {
            *gx = found.x0 + found.gx[g];
            *gy = found.y0 + found.gy[g];
            break;
        }
    sh_dump_plan(&found, "target");
    return true;
}

/******************************************************************************/
/* saving: version 1 (outposts only, before the sizes) or 2 (count with the top bit set) */
#pragma pack(1)
struct OpSaved {
    int64_t rx, ry;
    uint8_t flags, level, gates_done;
};
struct OpSaved2 {
    int64_t rx, ry, cx, cy;
    uint8_t family, tier, flags, level, found_level;
    uint16_t gates_done;
};
#pragma pack()
#define OP_SAVE_V2 0x80000000u
#define OP_SAVE_OUTPOSTS_OFF 0x40000000u /* a world made before the outposts: none (its fortresses still count) */

size_t world_outposts_save_size(void)
{
    return 4 + (size_t)op_state_count * sizeof(struct OpSaved2);
}

void world_outposts_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)op_state_count | OP_SAVE_V2 | (op_on ? 0u : OP_SAVE_OUTPOSTS_OFF);
    memcpy(buf, &n, 4);
    buf += 4;
    for (int i = 0; i < op_state_count; i++)
    {
        const struct OpState *st = &op_states[i];
        struct OpSaved2 s = {st->rx, st->ry, st->cx, st->cy, st->family, st->tier, st->flags, st->level, st->found_level,
            st->gates_done};
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
    if (n & OP_SAVE_V2)
    {
        const TbBool on = !(n & OP_SAVE_OUTPOSTS_OFF);
        n &= ~(OP_SAVE_V2 | OP_SAVE_OUTPOSTS_OFF);
        if (n > (len - 4) / sizeof(struct OpSaved2))
            return false;
        for (uint32_t i = 0; i < n; i++)
        {
            struct OpSaved2 s;
            memcpy(&s, buf + 4 + (size_t)i * sizeof(s), sizeof(s));
            struct OpState *st = op_state(s.family, s.rx, s.ry, true);
            if (st == NULL)
                return false;
            st->cx = s.cx;
            st->cy = s.cy;
            st->tier = s.tier;
            st->flags = s.flags;
            st->level = s.level;
            st->found_level = s.found_level;
            st->gates_done = s.gates_done;
        }
        op_on = on;
        return true;
    }
    if (n > (len - 4) / sizeof(struct OpSaved))
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        struct OpSaved s;
        memcpy(&s, buf + 4 + (size_t)i * sizeof(s), sizeof(s));
        struct OpState *st = op_state(ShF_Outpost, s.rx, s.ry, true);
        if (st == NULL)
            return false;
        st->tier = ShT_Legacy; /* saved before the sizes: the old outpost */
        st->flags = s.flags;
        st->level = s.level;
        st->gates_done = s.gates_done;
        static struct OpPlan legacy;
        if (op_plan_make_legacy(s.rx, s.ry, &legacy))
        {
            st->cx = legacy.x0 + LEGACY_N / 2;
            st->cy = legacy.y0 + LEGACY_N / 2;
        }
    }
    op_on = true;
    return true;
}

#ifdef __cplusplus
}
#endif

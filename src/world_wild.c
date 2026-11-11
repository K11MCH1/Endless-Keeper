/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wild.c
 *     Endless world: wild creatures, spawned and despawned the way Minecraft spawns monsters.
 *
 * Spiders, Skeletons and Beetles appear on unclaimed dirt paths and Flies over water, in packs whose kind and size
 * follow Minecraft's spawn weights and group sizes, with a few biome changes (Minecraft's desert husks become
 * more Beetles, snowy strays more Skeletons, ...).
 * - Every second, WILD_CHUNK_TRIES random loaded chunks (of the window, or of the chunked world) get one pack
 *   try. A pack starts on a random slab; its kind is picked by weight from the biome's list for that ground; each
 *   member gets up to 4 tries, each moved up to 5 slabs (triangular, as Minecraft) from the last.
 * - Not within WILD_MIN_DIST slabs of the player's view or heart, never on claimed ground.
 * - Caps: Minecraft's 70 monsters (and 20 water ambient) for 289 chunks, scaled to the loaded chunks (100 in the
 *   window).
 * - Despawn: when their chunk is frozen (they aren't frozen with it, like Minecraft's 128 blocks); and,
 *   more than WILD_KEEP_DIST slabs from the view and the heart, not fighting and alive for 30 seconds, one in
 *   WILD_DESPAWN_ONE_IN each second (Minecraft's 1 in 800 per tick).
 * They belong to the heroes (hostile to every keeper), patrol where they are and fight what comes near. They are
 * marked by the DEFEND_LOCATION hero objective.
 * Burrows (2026-11-11): see below; packs also break out of the earth round a keeper and dig towards their imps.
 * Night (2026-11-07): they are the night. Packs spawn only at night; each creature's level is H-2..H (at
 * least 1), H the level of the strongest creature of the nearest human keeper; at dawn the ones not fighting fade
 * out over half a minute (Minecraft's monsters burning in daylight).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_wild.h"
#include "world_stream.h"
#include "world_gen.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "config.h"
#include "config_creature.h"
#include "creature_control.h"
#include "creature_states_hero.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "world_pages.h"
#include "world_daynight.h"
#include "world_circles.h"
#include "net_game.h"
#include "creature_states.h"
#include "creature_groups.h"
#include "thing_navigate.h"
#include "map_blocks.h"
#include "slab_data.h"
#include "thing_stats.h"
#include <limits.h>
#include "post_inc.h"

#define WILD_CYCLE_TURNS 20     /* one spawn cycle a second */
#define WILD_GOLD_PER_LEVEL 25  /* what a wild creature carries, and drops when it dies (2026-11-11) */
#define WILD_CHUNK_TRIES 2      /* pack tries per cycle */
#define WILD_MEMBER_TRIES 4     /* Minecraft: up to 4 tries per pack member */
#define WILD_JITTER 6           /* moves of rand(6) - rand(6), up to 5 slabs (Minecraft) */
#define WILD_MIN_DIST 24        /* Minecraft: no spawns within 24 blocks of a player */
#define WILD_KEEP_DIST 32       /* Minecraft: no random despawn within 32 blocks */
#define WILD_MIN_AGE (30 * 20)  /* Minecraft: 30 seconds without a player near */
#define WILD_DESPAWN_ONE_IN 40  /* per second: Minecraft's 1 in 800 per tick */
#define WILD_LEVEL_SPREAD 2     /* levels H-2..H, H: the nearest human keeper's strongest creature */
#define WILD_DAWN_ONE_IN 10     /* per second after dawn: the ones not fighting fade out over about half a minute */

enum WildGround {
    WiG_Path = 0, /* unclaimed dirt paths */
    WiG_Water,
    WiG_COUNT
};

/* Minecraft: 70 monsters and 20 water ambient creatures for 289 chunks (scaled to the loaded chunks: the window
 * has 100, the chunked world as many as are loaded) */
static const int wild_caps_289[WiG_COUNT] = {70, 20};
#define WILD_CAP_CHUNKS 289

struct WildSpawn {
    const char *name;
    int weight;
    int min_group, max_group;
};

#define WILD_KINDS 5
struct WildBiome {
    struct WildSpawn on[WiG_COUNT][WILD_KINDS]; /* unused entries have no name */
};

/* Minecraft's monster list (spider 100, skeleton 100, zombie 95 in groups of 4; endermen and witches rare) with
 * Beetles for zombies; water: Flies. In WgBiome order. */
#define WILD_BASE {"SPIDER", 100, 4, 4}, {"SKELETON", 100, 4, 4}, {"BUG", 95, 4, 4}
#define WILD_FLIES {"FLY", 100, 2, 4}
/* Zombies (a creature kind a mod adds; without it the entry is skipped): packs of 4 to 6 in every biome,
 * about two thirds as common as Spiders, and more common where the undead already are. */
#define WILD_ZOMBIES {"ZOMBIE", 65, 4, 6}
#define WILD_ZOMBIES_MANY {"ZOMBIE", 110, 4, 6}
static const struct WildBiome wild_biomes[WgB_COUNT] = {
    {{{WILD_BASE, {NULL, 0, 0, 0}, WILD_ZOMBIES}, {WILD_FLIES}}},                                        /* Earthen Caverns */
    {{{WILD_BASE, {NULL, 0, 0, 0}, WILD_ZOMBIES}, {WILD_FLIES}}},                                        /* Rocky Depths */
    {{{WILD_BASE, {"HELL_HOUND", 10, 1, 2}, WILD_ZOMBIES}, {WILD_FLIES}}},                               /* Volcanic Rifts */
    {{{WILD_BASE, {NULL, 0, 0, 0}, WILD_ZOMBIES}, {{"FLY", 150, 2, 4}, {"TENTACLE", 10, 1, 1}}}},        /* Flooded Grottos */
    {{{{"SKELETON", 180, 4, 4}, {"SPIDER", 100, 4, 4}, {"BUG", 95, 4, 4}, {"GHOST", 10, 1, 1}, WILD_ZOMBIES_MANY},
      {WILD_FLIES}}},                                                                      /* Frozen Hollows */
    {{{{"BUG", 175, 4, 4}, {"SPIDER", 100, 4, 4}, {"SKELETON", 100, 4, 4}, {NULL, 0, 0, 0}, WILD_ZOMBIES},
      {WILD_FLIES}}},                                                                      /* Sandstone Wastes */
    {{{WILD_BASE, {NULL, 0, 0, 0}, WILD_ZOMBIES}, {WILD_FLIES}}},                                        /* Crystal Caverns */
    {{{WILD_BASE, {"GHOST", 10, 1, 1}, WILD_ZOMBIES_MANY}, {WILD_FLIES}}},                                    /* Ancient Ruins */
};

/** The weight of a list entry: none for an empty entry or a creature kind that is not loaded. */
static int wild_weight(const struct WildSpawn *spawn)
{
    if ((spawn->name == NULL) || (get_id(creature_desc, spawn->name) <= 0))
        return 0;
    const char *only = getenv("KFX_WORLD_WILDONLY"); /* test switch: wild packs are of this one kind */
    if ((only != NULL) && (only[0] != 0) && (strcasecmp(spawn->name, only) != 0))
        return 0;
    return spawn->weight;
}

static uint64_t wild_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct WildRng {
    uint64_t s;
};

static int wild_rand(struct WildRng *r, int n)
{
    r->s = wild_mix(r->s);
    return (n > 0) ? (int)(r->s % (uint64_t)n) : 0;
}

/* A wild creature of a hunting pack (out of a burrow): its own mark, which the hero AI never writes. (It was "has a
 * human target": the hero AI clears and sets a creature's target as it pleases, so a digging Beetle could stop being
 * a hunter and fade, and an ordinary wild creature could become one; audit 2026-11-11.) */
#define WILD_HUNTER_MARK CHeroTsk_DefendHeart

TbBool world_wild_creature(const struct Thing *thing)
{
    if (!world_mode_active() || (thing->class_id != TCls_Creature) || (thing->owner != PLAYER_GOOD))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    return !creature_control_invalid(cctrl) && ((cctrl->party.original_objective == CHeroTsk_DefendSpawn) ||
        (cctrl->party.original_objective == WILD_HUNTER_MARK));
}

/** Which ground a wild creature counts against: the water kinds against the water cap. */
static int wild_ground_of(const struct Thing *thing)
{
    const char *name = creature_code_name(thing->model);
    for (int b = 0; b < WgB_COUNT; b++)
        for (int k = 0; k < WILD_KINDS; k++)
            if ((wild_biomes[b].on[WiG_Water][k].name != NULL) && (strcmp(wild_biomes[b].on[WiG_Water][k].name, name) == 0))
                return WiG_Water;
    return WiG_Path;
}

/** The ground a slab offers wild creatures, or -1. */
static int wild_ground_at(MapSlabCoord x, MapSlabCoord y)
{
    if (!world_paged && ((x < 1) || (y < 1) || (x >= game.map_tiles_x - 1) || (y >= game.map_tiles_y - 1)))
        return -1;
    struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return -1; /* paged mode: a chunk that isn't loaded */
    if ((slb->kind == SlbT_PATH) && (slabmap_owner(slb) == game.neutral_player_num))
        return WiG_Path;
    if (slb->kind == SlbT_WATER)
        return WiG_Water;
    return -1;
}

static long wild_dist2(MapSlabCoord ax, MapSlabCoord ay, MapSlabCoord bx, MapSlabCoord by)
{
    return (long)(ax - bx) * (ax - bx) + (long)(ay - by) * (ay - by);
}

/** Where the human players are: every human user's view and every human heart (game state, the same on every
 *  machine; a single-player run has its view and its heart). */
#define WILD_MAX_ANCHORS 224 /* every user's view and every human heart (it was 16: on a busy server the views filled
                                it and no heart was left to be an anchor; audit 2026-11-11) */
struct WildPlayer {
    int count;
    MapSlabCoord ax[WILD_MAX_ANCHORS], ay[WILD_MAX_ANCHORS];
    PlayerNumber plyr[WILD_MAX_ANCHORS];  /* whose view or heart */
    uint8_t heart[WILD_MAX_ANCHORS];      /* a heart (not a view) */
    int top_level[WILD_MAX_ANCHORS];      /* that keeper's strongest creature (1-based level) */
};

static TbBool wild_near_player(const struct WildPlayer *pl, MapSlabCoord x, MapSlabCoord y, long dist)
{
    for (int i = 0; i < pl->count; i++)
        if (wild_dist2(x, y, pl->ax[i], pl->ay[i]) < dist * dist)
            return true;
    return false;
}

/** The level (1-based) of a keeper's strongest creature; 1 for none. */
int world_keeper_top_level(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return 1;
    int top = 0;
    long guard = 0;
    for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        if ((int)cctrl->exp_level + 1 > top)
            top = (int)cctrl->exp_level + 1;
        i = cctrl->players_next_creature_idx;
    }
    return (top > 0) ? top : 1;
}

static struct Thing *wild_spawn(struct WildRng *rng, ThingModel model, MapSlabCoord x, MapSlabCoord y, int level)
{
    if (!creature_count_below_map_limit(0))
        return INVALID_THING;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x)) + (MapCoord)(wild_rand(rng, 193) - 96);
    pos.y.val = subtile_coord_center(slab_subtile_center(y)) + (MapCoord)(wild_rand(rng, 193) - 96);
    pos.z.val = 0;
    struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, model,
        PLAYER_GOOD);
    if (thing_is_invalid(thing))
        return INVALID_THING;
    level += world_coop_level_bonus_at_slab(x, y); /* co-op: a level higher by a party's start */
    init_creature_level(thing, (CrtrExpLevel)(level - 1));
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    cctrl->party.objective = CHeroTsk_DefendSpawn;
    cctrl->party.original_objective = CHeroTsk_DefendSpawn; /* marks wild creatures */
    cctrl->party.target_plyr_idx = -1;
    thing->creature.gold_carried = WILD_GOLD_PER_LEVEL * level; /* dropped when it dies (2026-11-11) */
    return thing;
}

/** One pack try in a random loaded chunk (of the window, or a page of the chunked world); returns how many
 *  spawned. */
static int wild_pack(struct WildRng *rng, const struct WildPlayer *pl, int *counts, const int *wild_caps)
{
    MapSlabCoord x, y;
    if (world_is_chunked())
    {
        const int n = wp_count();
        if (n <= 0)
            return 0;
        int k = wild_rand(rng, n);
        struct WorldPage *pg = wp_first();
        while ((k-- > 0) && (pg != NULL))
            pg = pg->next;
        if (pg == NULL)
            return 0;
        x = (MapSlabCoord)pg->cx * WORLD_CHUNK_SLABS + wild_rand(rng, WORLD_CHUNK_SLABS);
        y = (MapSlabCoord)pg->cy * WORLD_CHUNK_SLABS + wild_rand(rng, WORLD_CHUNK_SLABS);
    } else
    {
        const int lcx = wild_rand(rng, WORLD_WINDOW_CHUNKS), lcy = wild_rand(rng, WORLD_WINDOW_CHUNKS);
        x = 1 + lcx * WORLD_CHUNK_SLABS + wild_rand(rng, WORLD_CHUNK_SLABS);
        y = 1 + lcy * WORLD_CHUNK_SLABS + wild_rand(rng, WORLD_CHUNK_SLABS);
    }
    const int ground = wild_ground_at(x, y);
    if ((ground < 0) || (counts[ground] >= wild_caps[ground]))
        return 0;
    int biome = world_biome_at_engine_slab(x, y);
    if ((biome < 0) || (biome >= WgB_COUNT))
        biome = 0;
    const struct WildSpawn *list = wild_biomes[biome].on[ground];
    int total = 0;
    for (int k = 0; k < WILD_KINDS; k++)
        total += wild_weight(&list[k]);
    if (total <= 0)
        return 0;
    int r = wild_rand(rng, total), k = 0;
    while ((k < WILD_KINDS - 1) && (r >= wild_weight(&list[k])))
    {
        r -= wild_weight(&list[k]);
        k++;
    }
    if (list[k].name == NULL)
        return 0;
    const long model = get_id(creature_desc, list[k].name);
    if (model <= 0)
        return 0;
    const int group = list[k].min_group + wild_rand(rng, list[k].max_group - list[k].min_group + 1);
    const MapSlabCoord px = x, py = y;
    /* circles (world_circles.c): a pack is as strong as the circle it appears in */
    const int top = world_circles_level_at_slab(x, y, 1, "a wild pack");
    const int low = (top - WILD_LEVEL_SPREAD > 1) ? top - WILD_LEVEL_SPREAD : 1;
    int made = 0;
    for (int m = 0; (m < group) && (counts[ground] < wild_caps[ground]); m++)
    {
        for (int t = 0; t < WILD_MEMBER_TRIES; t++)
        {
            x += wild_rand(rng, WILD_JITTER) - wild_rand(rng, WILD_JITTER);
            y += wild_rand(rng, WILD_JITTER) - wild_rand(rng, WILD_JITTER);
            if ((wild_ground_at(x, y) != ground) || wild_near_player(pl, x, y, WILD_MIN_DIST))
                continue;
            if (!thing_is_invalid(wild_spawn(rng, (ThingModel)model, x, y, low + wild_rand(rng, top - low + 1))))
            {
                counts[ground]++;
                made++;
            }
            break;
        }
    }
    if (made > 0)
        SYNCLOG("World: %d wild %s (levels %d-%d) round engine slab (%d,%d), %s", made, list[k].name, low, top, (int)px,
            (int)py, wg_biome_name(biome));
    return made;
}

/******************************************************************************/
/* Burrows and digging Beetles (2026-11-11). Packs needed open, unclaimed ground to appear on, which a keeper who
 * hasn't dug far doesn't have, and then stood where they appeared. Now, at night, a pack also breaks out of the earth
 * in a band round each keeper's heart: the game opens a small burrow and the pack appears in it, led by a Beetle or
 * two that dig a tunnel towards imps that are digging (else towards any of the keeper's creatures outside their own
 * ground); the rest follow. Beetles dig earth only: not rock, not walls, not claimed ground. At dawn the burrows and
 * tunnels that are still unclaimed and empty fall in again (they are kept in the save: WBT1). */
#define WILD_BURROW_MIN 24           /* slabs from the heart, at least */
#define WILD_BURROW_MAX 60
#define WILD_BURROW_ONE_IN 90        /* per second and keeper */
#define WILD_BURROW_ONE_IN_EARLY 150 /* the first nights */
#define WILD_BURROW_EARLY_NIGHTS 4   /* no burrows on night 1; packs of two to night 4, then of four */
#define WILD_HUNT_MAX 8              /* wild creatures hunting one keeper at most */
#define WILD_DIG_TURNS 60            /* a Beetle digs a slab of earth in 3 seconds */
#define WILD_WALK_TURNS 30
#define WILD_HUNT_REACH 90           /* slabs: how far a Beetle looks for its keeper's imps and creatures */
#define WILD_MAX_TUNNEL 4096
#define WILD_MAX_DIGGERS 48
#define WILD_DIG_PHASES 5            /* a digger gets its turn every fifth pass (a pass every fourth turn: once a second) */
#define WILD_COLLAPSE_A_PASS 5       /* slabs that fall in in one pass (about 25 a second) */

struct WildTunnel {
    int64_t x, y;                    /* world slabs */
};
static struct WildTunnel wild_tunnel[WILD_MAX_TUNNEL];
static int wild_tunnels = 0;

struct WildDigger {
    ThingIndex idx;                  /* 0: free */
    GameTurn made;                   /* the Beetle's creation turn (the index may be used again) */
    PlayerNumber plyr;               /* the keeper it hunts */
    uint32_t pack;
    GameTurn next;                   /* its next step */
    uint8_t blocked;
    uint8_t walking;                 /* it found a way to walk (logged once) */
    uint16_t steps;
    uint8_t has_head;
    uint8_t stall;                   /* steps without getting nearer */
    int32_t best;                    /* the nearest it got to what it hunts (slabs; 0: not yet) */
    int64_t head_x, head_y;          /* the tunnel's head, world slabs: it digs on from there, wherever it wandered */
    int64_t trail_x[8], trail_y[8];  /* the head's last places: not stepped on again (no going round in circles) */
    int8_t last_dx, last_dy;
};
static struct WildDigger wild_digger[WILD_MAX_DIGGERS];
static uint32_t wild_pack_serial = 0;

void world_wild_reset(void)
{
    wild_tunnels = 0;
    memset(wild_digger, 0, sizeof(wild_digger));
}

static int wild_diggers_used(void)
{
    int n = 0;
    for (int k = 0; k < WILD_MAX_DIGGERS; k++)
        if (wild_digger[k].idx != 0)
            n++;
    return n;
}

/* WBT1: the tunnels (count, world slabs); then the Beetles that dig (count, the size of one, the records): a load, a
 * server's restart or a resync left packs hunting with nobody digging (audit 2026-11-11) */
size_t world_wild_save_size(void)
{
    const int diggers = wild_diggers_used();
    if ((wild_tunnels <= 0) && (diggers == 0))
        return 0;
    return 4 + (size_t)wild_tunnels * sizeof(struct WildTunnel) + 8 + (size_t)diggers * sizeof(struct WildDigger);
}

void world_wild_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)wild_tunnels;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, wild_tunnel, (size_t)n * sizeof(struct WildTunnel));
    buf += 4 + (size_t)n * sizeof(struct WildTunnel);
    const uint32_t head[2] = {(uint32_t)wild_diggers_used(), (uint32_t)sizeof(struct WildDigger)};
    memcpy(buf, head, 8);
    buf += 8;
    for (int k = 0; k < WILD_MAX_DIGGERS; k++)
    {
        if (wild_digger[k].idx == 0)
            continue;
        memcpy(buf, &wild_digger[k], sizeof(struct WildDigger));
        buf += sizeof(struct WildDigger);
    }
}

TbBool world_wild_load(const uint8_t *buf, size_t len)
{
    world_wild_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > WILD_MAX_TUNNEL) || (len < 4 + (size_t)n * sizeof(struct WildTunnel)))
        return false;
    memcpy(wild_tunnel, buf + 4, (size_t)n * sizeof(struct WildTunnel));
    wild_tunnels = (int)n;
    /* the diggers, when the save has them and their record is this build's */
    const size_t at = 4 + (size_t)n * sizeof(struct WildTunnel);
    if (len >= at + 8)
    {
        uint32_t head[2];
        memcpy(head, buf + at, 8);
        if ((head[1] == sizeof(struct WildDigger)) && (head[0] <= WILD_MAX_DIGGERS) &&
            (len >= at + 8 + (size_t)head[0] * sizeof(struct WildDigger)))
        {
            memcpy(wild_digger, buf + at + 8, (size_t)head[0] * sizeof(struct WildDigger));
            for (uint32_t k = 0; k < head[0]; k++)
                if (wild_digger[k].pack > wild_pack_serial)
                    wild_pack_serial = wild_digger[k].pack;
        }
    }
    return true;
}

/** Earth a burrow can open in and a Beetle can dig: nobody's, and not rock, gold, a wall or a room. */
static TbBool wild_earth_at(MapSlabCoord x, MapSlabCoord y)
{
    if (!world_paged && ((x < 2) || (y < 2) || (x >= game.map_tiles_x - 2) || (y >= game.map_tiles_y - 2)))
        return false;
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return false;
    return ((slb->kind == SlbT_EARTH) || (slb->kind == SlbT_TORCHDIRT)) && (slabmap_owner(slb) == game.neutral_player_num);
}

/** Ground a wild creature may walk over on its way: nobody's open path. */
static TbBool wild_open_at(MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && (slb->kind == SlbT_PATH) && (slabmap_owner(slb) == game.neutral_player_num);
}

/** Opens a slab of earth and remembers it, to fall in again at dawn. */
static void wild_open_slab(MapSlabCoord x, MapSlabCoord y)
{
    place_slab_type_on_map(SlbT_PATH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
    if (wild_tunnels < WILD_MAX_TUNNEL)
    {
        world_slab_to_world(x, y, &wild_tunnel[wild_tunnels].x, &wild_tunnel[wild_tunnels].y);
        wild_tunnels++;
    }
}

static TbBool wild_creature_on_slab(MapSlabCoord x, MapSlabCoord y)
{
    for (int dy = 0; dy < STL_PER_SLB; dy++)
    {
        for (int dx = 0; dx < STL_PER_SLB; dx++)
        {
            const struct Map *mapblk = get_map_block_at(slab_subtile(x, dx), slab_subtile(y, dy));
            long guard = 0;
            for (ThingIndex i = get_mapwho_thing_index(mapblk); (i != 0) && (guard++ < THINGS_COUNT);)
            {
                const struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                /* a creature, or what one left there: its gold, a raid's book, a body (audit 2026-11-11) */
                if ((thing->class_id == TCls_Creature) || (thing->class_id == TCls_Object) ||
                    (thing->class_id == TCls_DeadCreature))
                    return true;
                i = thing->next_on_mapblk;
            }
        }
    }
    return false;
}

/** By day, once a second: the burrows and tunnels that are still nobody's, open and empty fall in again; the ones
 *  somebody claimed or built on are theirs and are forgotten; the ones in ground that isn't loaded wait. */
static void wild_collapse_turn(void)
{
    int fell = 0, kept = 0;
    for (int i = 0; i < wild_tunnels; i++)
    {
        MapSlabCoord x, y;
        TbBool keep = true;
        if (world_engine_slab(wild_tunnel[i].x, wild_tunnel[i].y, &x, &y))
        {
            const struct SlabMap *slb = get_slabmap_block(x, y);
            if (!slabmap_block_invalid(slb))
            {
                if (!wild_open_at(x, y))
                    keep = false; /* claimed, built on or dug into something else: no longer a wild tunnel */
                else if ((fell < WILD_COLLAPSE_A_PASS) && !wild_creature_on_slab(x, y))
                {
                    place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(x), slab_subtile_center(y),
                        game.neutral_player_num, 0);
                    fell++;
                    keep = false;
                }
            }
        }
        if (keep)
            wild_tunnel[kept++] = wild_tunnel[i];
    }
    wild_tunnels = kept;
    if (fell > 0)
        SYNCLOG("World: %d slabs of wild burrows and tunnels fell in (%d left)", fell, wild_tunnels);
}

/** What a hunting Beetle digs towards: the nearest of the keeper's imps that is digging; else the nearest of the
 *  keeper's creatures or imps that stands outside the keeper's own ground; else the keeper's heart. */
static TbBool wild_hunt_target(PlayerNumber plyr_idx, MapSlabCoord bx, MapSlabCoord by, MapSlabCoord *tx, MapSlabCoord *ty)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return false;
    long best_dig = LONG_MAX, best_out = LONG_MAX;
    MapSlabCoord dig_x = 0, dig_y = 0, out_x = 0, out_y = 0;
    const ThingIndex lists[2] = {dungeon->digger_list_start, dungeon->creatr_list_start};
    for (int l = 0; l < 2; l++)
    {
        long guard = 0;
        for (ThingIndex i = lists[l]; (i > 0) && (guard++ < CREATURES_COUNT);)
        {
            struct Thing *thing = thing_get(i);
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (thing_is_picked_up(thing) || creature_is_dying(thing) || creature_is_kept_in_custody(thing))
                continue;
            const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num), y = subtile_slab(thing->mappos.y.stl.num);
            const long d = wild_dist2(x, y, bx, by);
            if (d > (long)WILD_HUNT_REACH * WILD_HUNT_REACH)
                continue;
            const CrtrStateId st = get_creature_state_besides_interruptions(thing);
            if ((l == 0) && ((st == CrSt_ImpDigsDirt) || (st == CrSt_ImpMinesGold) || (st == CrSt_ImpArrivesAtDigDirt) ||
                (st == CrSt_ImpArrivesAtMineGold)))
            {
                if (d < best_dig)
                {
                    best_dig = d;
                    dig_x = x;
                    dig_y = y;
                }
                continue;
            }
            const struct SlabMap *slb = get_slabmap_block(x, y);
            if (!slabmap_block_invalid(slb) && (slabmap_owner(slb) != plyr_idx) && (d < best_out))
            {
                best_out = d;
                out_x = x;
                out_y = y;
            }
        }
    }
    if (best_dig != LONG_MAX)
    {
        *tx = dig_x;
        *ty = dig_y;
        return true;
    }
    if (best_out != LONG_MAX)
    {
        *tx = out_x;
        *ty = out_y;
        return true;
    }
    /* nobody out: towards the dungeon itself, as far as its walls and claimed ground let it (the pack then prowls
       outside, and walks in where the keeper left a way open) */
    struct Thing *heartng = find_players_dungeon_heart(plyr_idx);
    if (thing_exists(heartng) && world_human_heart_loaded(plyr_idx))
    {
        *tx = subtile_slab(heartng->mappos.x.stl.num);
        *ty = subtile_slab(heartng->mappos.y.stl.num);
        return true;
    }
    return false;
}

static struct Thing *wild_digger_thing(const struct WildDigger *dg)
{
    if (dg->idx == 0)
        return INVALID_THING;
    struct Thing *thing = thing_get(dg->idx);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->creation_turn != dg->made) ||
        !world_wild_creature(thing) || creature_is_dying(thing) || creature_is_being_unconscious(thing) ||
        creature_is_kept_in_custody(thing) || thing_is_picked_up(thing) || thing_is_dragged_or_pulled(thing))
        return INVALID_THING;
    return thing;
}

static void wild_send(struct Thing *thing, MapSlabCoord x, MapSlabCoord y)
{
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x));
    pos.y.val = subtile_coord_center(slab_subtile_center(y));
    pos.z.val = 0;
    cleanup_current_thing_state(thing);
    if (setup_person_move_to_coord(thing, &pos, NavRtF_Default))
        thing->continue_state = CrSt_GoodDoingNothing;
}

/** Once a second: each pack's first living Beetle takes a step of its tunnel. */
static void wild_diggers_turn(int phase)
{
    for (int k = 0; k < WILD_MAX_DIGGERS; k++)
    {
        struct WildDigger *dg = &wild_digger[k];
        if ((dg->idx == 0) || ((k % WILD_DIG_PHASES) != phase))
            continue;
        struct Thing *thing = wild_digger_thing(dg);
        if (thing_is_invalid(thing))
        {
            dg->idx = 0;
            continue;
        }
        /* an earlier Beetle of the same pack that is still alive does the digging */
        TbBool second = false;
        for (int j = 0; j < k; j++)
            if ((wild_digger[j].idx != 0) && (wild_digger[j].pack == dg->pack))
                second = true;
        if (second || (game.play_gameturn < dg->next))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (cctrl->combat_flags != 0)
            continue; /* a fight first */
        /* the pack follows the Beetle that digs */
        if (creature_is_group_member(thing) && !creature_is_group_leader(thing))
            make_group_member_leader(thing);
        /* and stays a hunting pack (the hero AI looks for a dungeon of its own choice now and then) */
        cctrl->party.target_plyr_idx = dg->plyr;
        {
            long guard = 0;
            for (struct Thing *memb = get_first_follower_creature_in_group(thing); thing_exists(memb) && (guard++ < 32);)
            {
                struct CreatureControl *mctrl = creature_control_get_from_thing(memb);
                if (creature_control_invalid(mctrl))
                    break;
                if (world_wild_creature(memb))
                    mctrl->party.target_plyr_idx = dg->plyr;
                memb = (mctrl->next_in_group > 0) ? thing_get(mctrl->next_in_group) : INVALID_THING;
            }
        }
        MapSlabCoord bx = subtile_slab(thing->mappos.x.stl.num), by = subtile_slab(thing->mappos.y.stl.num);
        {
            /* the tunnel goes on from its head; a Beetle that strayed from it walks back there first */
            MapSlabCoord hx, hy;
            if (dg->has_head && world_engine_slab(dg->head_x, dg->head_y, &hx, &hy) &&
                (wild_open_at(hx, hy) || wild_earth_at(hx, hy)))
            {
                if ((labs((long)hx - bx) > 2) || (labs((long)hy - by) > 2))
                {
                    if (get_creature_state_besides_interruptions(thing) != CrSt_MoveToPosition)
                        wild_send(thing, hx, hy);
                    dg->next = game.play_gameturn + WILD_WALK_TURNS;
                    continue;
                }
                bx = hx;
                by = hy;
            } else
            {
                dg->has_head = 1;
                world_slab_to_world(bx, by, &dg->head_x, &dg->head_y);
                for (int t = 0; t < 8; t++)
                {
                    dg->trail_x[t] = dg->head_x;
                    dg->trail_y[t] = dg->head_y;
                }
            }
        }
        MapSlabCoord tx, ty;
        if (!wild_hunt_target(dg->plyr, bx, by, &tx, &ty))
        {
            dg->next = game.play_gameturn + 100; /* nobody to dig towards: the pack prowls, and looks again */
            continue;
        }
        const long adx = labs((long)tx - bx), ady = labs((long)ty - by);
        if ((adx <= 1) && (ady <= 1))
        {
            dg->next = game.play_gameturn + WILD_WALK_TURNS;
            continue; /* there: the fight is the pack's own business */
        }
        struct Coord3d tpos;
        tpos.x.val = subtile_coord_center(slab_subtile_center(tx));
        tpos.y.val = subtile_coord_center(slab_subtile_center(ty));
        tpos.z.val = 0;
        if (creature_can_navigate_to(thing, &tpos, NavRtF_Default))
        {
            /* a way there already: no digging; it walks, the pack with it */
            if (!dg->walking)
                SYNCLOG("World: a wild Beetle at engine slab (%d,%d) has a way to (%d,%d): its pack walks in on player %d",
                    (int)bx, (int)by, (int)tx, (int)ty, (int)dg->plyr);
            dg->walking = 1;
            if (get_creature_state_besides_interruptions(thing) != CrSt_MoveToPosition)
                wild_send(thing, tx, ty);
            dg->next = game.play_gameturn + 100;
            continue;
        }
        /* the four steps, nearest to the target first; never straight back */
        static const int8_t ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
        int order[4] = {0, 1, 2, 3};
        long dist[4];
        for (int d = 0; d < 4; d++)
            dist[d] = wild_dist2(bx + ddx[d], by + ddy[d], tx, ty);
        for (int a = 0; a < 3; a++)
            for (int b = a + 1; b < 4; b++)
                if (dist[order[b]] < dist[order[a]])
                {
                    const int t = order[a];
                    order[a] = order[b];
                    order[b] = t;
                }
        TbBool stepped = false;
        for (int o = 0; (o < 4) && !stepped; o++)
        {
            const int d = order[o];
            if ((ddx[d] == -dg->last_dx) && (ddy[d] == -dg->last_dy) && ((dg->last_dx != 0) || (dg->last_dy != 0)))
                continue;
            const MapSlabCoord nx = bx + ddx[d], ny = by + ddy[d];
            int64_t nwx, nwy;
            world_slab_to_world(nx, ny, &nwx, &nwy);
            TbBool been = false;
            for (int t = 0; t < 8; t++)
                if ((dg->trail_x[t] == nwx) && (dg->trail_y[t] == nwy))
                    been = true;
            if (been)
                continue;
            if (wild_earth_at(nx, ny))
            {
                if ((dg->last_dx == 0) && (dg->last_dy == 0) && (dg->blocked == 0))
                    SYNCLOG("World: a wild Beetle at engine slab (%d,%d) digs towards (%d,%d), hunting player %d", (int)bx,
                        (int)by, (int)tx, (int)ty, (int)dg->plyr);
                wild_open_slab(nx, ny);
                dg->next = game.play_gameturn + WILD_DIG_TURNS;
            } else if (wild_open_at(nx, ny))
                dg->next = game.play_gameturn + WILD_WALK_TURNS;
            else
                continue;
            wild_send(thing, nx, ny);
            dg->steps++;
            dg->head_x = nwx;
            dg->head_y = nwy;
            {
                /* walls it can't dig turn it aside: when it has gone round them for a while without getting nearer,
                   it stops, and its pack prowls there */
                const long togo = max(labs((long)tx - nx), labs((long)ty - ny));
                if ((dg->best == 0) || (togo < dg->best))
                {
                    dg->best = (int32_t)togo;
                    dg->stall = 0;
                } else if (++dg->stall > 12)
                {
                    SYNCLOG("World: a wild Beetle hunting player %d gets no nearer than %d slabs (walls or rock): its pack"
                        " prowls there", (int)dg->plyr, (int)dg->best);
                    dg->idx = 0;
                }
            }
            dg->trail_x[dg->steps % 8] = nwx;
            dg->trail_y[dg->steps % 8] = nwy;
            if (((dg->steps % 10) == 0) && (getenv("KFX_WORLD_BURROWS") != NULL))
                SYNCLOG("World test: a wild Beetle hunting player %d: %d steps, at (%d,%d), %ld slabs to go, state %d",
                    (int)dg->plyr, (int)dg->steps, (int)nx, (int)ny, max(adx, ady), (int)get_creature_state_besides_interruptions(thing));
            dg->last_dx = ddx[d];
            dg->last_dy = ddy[d];
            dg->blocked = 0;
            stepped = true;
        }
        if (!stepped)
        {
            dg->last_dx = dg->last_dy = 0;
            for (int t = 0; t < 8; t++)
            {
                dg->trail_x[t] = dg->head_x; /* a dead end: it may go back the way it came */
                dg->trail_y[t] = dg->head_y;
            }
            dg->next = game.play_gameturn + WILD_DIG_TURNS;
            if (++dg->blocked > 6)
            {
                SYNCLOG("World: a wild Beetle hunting player %d can dig no further (rock, walls or claimed ground): its pack"
                    " prowls", (int)dg->plyr);
                dg->idx = 0;
            }
        }
    }
}

/** A burrow try for one keeper: a pack breaks out of the earth WILD_BURROW_MIN..MAX slabs from the heart. */
static int wild_burrow(struct WildRng *rng, const struct WildPlayer *pl, int a, int *counts, const int *wild_caps, int reach)
{
    const long night = world_day_number();
    if ((night <= 1) && (getenv("KFX_WORLD_BURROWS") == NULL))
        return 0; /* the first night is quiet (not under the test hook) */
    const int size = (night <= WILD_BURROW_EARLY_NIGHTS) ? 2 : 4;
    /* (burrow packs don't wait for room under the cap of the ordinary packs: they have a limit of their own,
       WILD_HUNT_MAX a keeper. Once raiders had left open tunnels, ordinary packs filled the cap there and no burrow
       opened any more; seen 2026-11-11) */
    (void)wild_caps;
    const int r = WILD_BURROW_MIN + wild_rand(rng, max(reach, WILD_BURROW_MIN) - WILD_BURROW_MIN + 1);
    const int dx = wild_rand(rng, 2 * r + 1) - r;
    int dy = (int)floor(sqrt((double)(r * r - dx * dx)) + 0.5);
    if (wild_rand(rng, 2) == 0)
        dy = -dy;
    const MapSlabCoord x = pl->ax[a] + dx, y = pl->ay[a] + dy;
    if (!wild_earth_at(x, y) || wild_near_player(pl, x, y, WILD_MIN_DIST))
        return 0;
    /* the burrow: this slab and the earth beside it, three to five slabs */
    static const int8_t ndx[4] = {1, -1, 0, 0}, ndy[4] = {0, 0, 1, -1};
    MapSlabCoord sx[5], sy[5];
    int ns = 0;
    sx[ns] = x;
    sy[ns++] = y;
    const int first = wild_rand(rng, 4);
    for (int d = 0; d < 4; d++)
    {
        const int e = (first + d) % 4;
        if (wild_earth_at(x + ndx[e], y + ndy[e]))
        {
            sx[ns] = x + ndx[e];
            sy[ns++] = y + ndy[e];
        }
    }
    if (ns < 3)
        return 0;
    int biome = world_biome_at_engine_slab(x, y);
    if ((biome < 0) || (biome >= WgB_COUNT))
        biome = 0;
    const struct WildSpawn *list = wild_biomes[biome].on[WiG_Path];
    int total = 0;
    for (int k = 0; k < WILD_KINDS; k++)
        total += wild_weight(&list[k]);
    if (total <= 0)
        return 0;
    int pick = wild_rand(rng, total), k = 0;
    while ((k < WILD_KINDS - 1) && (pick >= wild_weight(&list[k])))
    {
        pick -= wild_weight(&list[k]);
        k++;
    }
    const long model = (list[k].name != NULL) ? get_id(creature_desc, list[k].name) : 0;
    const long beetle = get_id(creature_desc, "BUG");
    if ((model <= 0) || (beetle <= 0))
        return 0;
    for (int s = 0; s < ns; s++)
        wild_open_slab(sx[s], sy[s]);
    /* circles (world_circles.c): a pack is as strong as the circle it appears in */
    const int top = world_circles_level_at_slab(sx[0], sy[0], 1, "a burrowing wild pack");
    const int low = (top - WILD_LEVEL_SPREAD > 1) ? top - WILD_LEVEL_SPREAD : 1;
    const int beetles = (size >= 4) ? 2 : 1; /* the Beetles are part of the pack, not on top of it */
    const uint32_t pack = ++wild_pack_serial;
    struct Thing *leader = INVALID_THING;
    int made = 0;
    for (int m = 0; m < size; m++)
    {
        struct Thing *thing = wild_spawn(rng, (ThingModel)((m < beetles) ? beetle : model), sx[m % ns], sy[m % ns],
            low + wild_rand(rng, top - low + 1));
        if (thing_is_invalid(thing))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        cctrl->party.target_plyr_idx = pl->plyr[a]; /* a hunting pack (world_wild_turn keeps it attacking) */
        cctrl->party.original_objective = WILD_HUNTER_MARK;
        cctrl->party.objective = CHeroTsk_AttackEnemies;
        if (thing_is_invalid(leader))
            leader = thing;
        else
            add_creature_to_group(thing, leader);
        if (m < beetles)
        {
            for (int j = 0; j < WILD_MAX_DIGGERS; j++)
            {
                if (wild_digger[j].idx != 0)
                    continue;
                memset(&wild_digger[j], 0, sizeof(wild_digger[j]));
                wild_digger[j].idx = thing->index;
                wild_digger[j].made = thing->creation_turn;
                wild_digger[j].plyr = pl->plyr[a];
                wild_digger[j].pack = pack;
                wild_digger[j].next = game.play_gameturn + WILD_DIG_TURNS;
                break;
            }
        }
        counts[WiG_Path]++;
        made++;
    }
    if (made > 0)
        SYNCLOG("World: a pack of %d (%d Beetles, %s; levels %d-%d) breaks out of the earth at engine slab (%d,%d), %d slabs"
            " from player %d's heart, %s (night %ld, a burrow of %d slabs)", made, min(beetles, made), list[k].name, low, top,
            (int)x, (int)y, r, (int)pl->plyr[a], wg_biome_name(biome), night, ns);
    return made;
}

void world_wild_turn(void)
{
    if (!world_mode_active())
        return;
    /* the Beetles' steps and the falling-in of tunnels are spread over the second, a few each fourth turn, so that
       they are no spike on one turn (route searches, slab changes; audit 2026-11-11) */
    if ((game.play_gameturn % 4) == 1)
    {
        wild_diggers_turn((int)((game.play_gameturn / 4) % WILD_DIG_PHASES));
        if (world_is_day())
            wild_collapse_turn();
    }
    if ((game.play_gameturn % WILD_CYCLE_TURNS) != 3)
        return;
    struct WildRng rng = {wild_mix(world_seed() ^ ((uint64_t)game.play_gameturn * 0xD1B54A32D192ED03ULL))};
    struct WildPlayer pl;
    memset(&pl, 0, sizeof(pl));
    for (int user = 0; (user < MAX_NET_USERS) && (pl.count < WILD_MAX_ANCHORS); user++)
    {
        MapSlabCoord vx, vy;
        const PlayerNumber vp = get_net_user_player_number((NetUserId)user);
        if (world_player_is_human(vp) && world_user_view_slab(user, &vx, &vy))
        {
            pl.ax[pl.count] = vx;
            pl.ay[pl.count] = vy;
            pl.plyr[pl.count] = vp;
            pl.count++;
        }
    }
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (pl.count < WILD_MAX_ANCHORS); p++)
    {
        struct Thing *heartng = (world_player_is_human(p) && !world_coop_seat_away(p)) ? find_players_dungeon_heart(p) : INVALID_THING;
        if (thing_exists(heartng) && world_human_heart_loaded(p))
        {
            pl.ax[pl.count] = subtile_slab(heartng->mappos.x.stl.num);
            pl.ay[pl.count] = subtile_slab(heartng->mappos.y.stl.num);
            pl.plyr[pl.count] = p;
            pl.heart[pl.count] = 1;
            pl.count++;
        }
    }
    for (int a = 0; a < pl.count; a++)
        pl.top_level[a] = world_keeper_top_level(pl.plyr[a]);
    const TbBool day = world_is_day();
    /* despawning, and counting against the caps */
    int counts[WiG_COUNT] = {0, 0};
    int despawned = 0;
    static uint8_t hunting[PLAYERS_MAX]; /* wild creatures hunting each keeper */
    memset(hunting, 0, sizeof(hunting));
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long i = slist->index;
    long guard = 0;
    while ((i != 0) && (guard++ <= (long)slist->count))
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!world_wild_creature(thing) || thing_is_picked_up(thing))
            continue;
        /* a captive (in a prison or a torture room, unconscious, carried off) is its captor's: it stays */
        if (creature_is_kept_in_prison(thing) || creature_is_being_tortured(thing) || creature_is_being_unconscious(thing) ||
            thing_is_dragged_or_pulled(thing))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num), y = subtile_slab(thing->mappos.y.stl.num);
        const TbBool fighting = (cctrl->combat_flags != 0);
        if (day && !fighting && (wild_rand(&rng, WILD_DAWN_ONE_IN) == 0))
        {
            /* the night is over: they fade out (the ones in a fight stay until it's done) */
            world_remove_thing_quietly(thing);
            despawned++;
            continue;
        }
        const TbBool hunter = (cctrl->party.original_objective == WILD_HUNTER_MARK);
        PlayerNumber hunted = -1;
        if (hunter)
        {
            /* whom: the keeper it was sent for while the hero AI leaves that be, else the nearest heart's */
            hunted = cctrl->party.target_plyr_idx;
            if ((hunted < 0) || (hunted >= PLAYERS_COUNT) || !world_player_is_human(hunted))
            {
                long best_d = LONG_MAX;
                hunted = -1;
                for (int a = 0; a < pl.count; a++)
                {
                    const long d = wild_dist2(x, y, pl.ax[a], pl.ay[a]);
                    if (pl.heart[a] && (d < best_d))
                    {
                        best_d = d;
                        hunted = pl.plyr[a];
                    }
                }
            }
        }
        /* (a hunting pack doesn't fade by night: it is on its way) */
        if (!fighting && !hunter && !wild_near_player(&pl, x, y, WILD_KEEP_DIST) &&
            ((long)(game.play_gameturn - thing->creation_turn) > WILD_MIN_AGE) && (wild_rand(&rng, WILD_DESPAWN_ONE_IN) == 0))
        {
            world_remove_thing_quietly(thing);
            despawned++;
            continue;
        }
        if (hunter)
        {
            /* a hunting pack (out of a burrow): it goes for its keeper's creatures */
            if ((hunted >= 0) && (hunted < PLAYERS_MAX))
            {
                if (hunting[hunted] < 255)
                    hunting[hunted]++;
                cctrl->party.target_plyr_idx = hunted;
            }
            if (!fighting && (cctrl->party.objective != CHeroTsk_AttackEnemies))
                cctrl->party.objective = CHeroTsk_AttackEnemies;
        } else if (!fighting && (cctrl->party.objective != CHeroTsk_DefendSpawn))
            cctrl->party.objective = CHeroTsk_DefendSpawn; /* they stay where they are: the hero AI drifts a creature
                                                              that can't patrol over to attacking */
        counts[wild_ground_of(thing)]++;
    }
    if (despawned > 0)
        SYNCLOG("World: %d wild creatures despawned", despawned);
    const int chunks = world_is_chunked() ? wp_count() : WORLD_WINDOW_CHUNKS * WORLD_WINDOW_CHUNKS;
    int wild_caps[WiG_COUNT];
    for (int g = 0; g < WiG_COUNT; g++)
        wild_caps[g] = wild_caps_289[g] * chunks / WILD_CAP_CHUNKS;
    if (day)
        return; /* they come out at night */
    for (int t = 0; t < WILD_CHUNK_TRIES; t++)
        wild_pack(&rng, &pl, counts, wild_caps);
    /* burrows: a pack out of the earth round each keeper's heart */
    {
        static int one_in_test = -1;
        if (one_in_test < 0)
        {
            const char *v = getenv("KFX_WORLD_BURROWS"); /* test hook: one burrow try in <n> a second and keeper */
            one_in_test = (v != NULL) ? max(1, atoi(v)) : 0;
        }
        const long night = world_day_number();
        const int one_in = (one_in_test > 0) ? one_in_test : (night <= WILD_BURROW_EARLY_NIGHTS) ? WILD_BURROW_ONE_IN_EARLY :
            WILD_BURROW_ONE_IN;
        if ((one_in_test > 0) && ((game.play_gameturn % 400) == 3))
        {
            int hearts = 0;
            for (int a = 0; a < pl.count; a++)
                hearts += pl.heart[a];
            SYNCLOG("World test: burrows: %d anchors, %d hearts, hunting player 0: %d, night %ld, one in %d", pl.count, hearts,
                (int)hunting[0], night, one_in);
        }
        for (int a = 0; a < pl.count; a++)
        {
            if (!pl.heart[a] || (hunting[pl.plyr[a]] >= WILD_HUNT_MAX) || (wild_rand(&rng, one_in) != 0))
                continue;
            /* a few tries for a spot of earth in the band: anywhere in it first, then nearer in, since the far part
               of the band is often ground that isn't loaded (round a small dungeon, or just after a load), where no
               burrow can open (seen 2026-11-11: whole nights without a pack) */
            for (int t = 0; t < 12; t++)
            {
                const int reach = (t < 4) ? WILD_BURROW_MAX : (t < 8) ? (WILD_BURROW_MIN + WILD_BURROW_MAX) / 2 : WILD_BURROW_MIN + 8;
                const int made = wild_burrow(&rng, &pl, a, counts, wild_caps, reach);
                if (made > 0)
                {
                    hunting[pl.plyr[a]] = (uint8_t)min(255, hunting[pl.plyr[a]] + made);
                    break;
                }
            }
        }
    }
}

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
#include "net_game.h"
#include "creature_states.h"
#include "post_inc.h"

#define WILD_CYCLE_TURNS 20     /* one spawn cycle a second */
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

#define WILD_KINDS 4
struct WildBiome {
    struct WildSpawn on[WiG_COUNT][WILD_KINDS]; /* unused entries have no name */
};

/* Minecraft's monster list (spider 100, skeleton 100, zombie 95 in groups of 4; endermen and witches rare) with
 * Beetles for zombies; water: Flies. In WgBiome order. */
#define WILD_BASE {"SPIDER", 100, 4, 4}, {"SKELETON", 100, 4, 4}, {"BUG", 95, 4, 4}
#define WILD_FLIES {"FLY", 100, 2, 4}
static const struct WildBiome wild_biomes[WgB_COUNT] = {
    {{{WILD_BASE, {NULL, 0, 0, 0}}, {WILD_FLIES}}},                                        /* Earthen Caverns */
    {{{WILD_BASE, {NULL, 0, 0, 0}}, {WILD_FLIES}}},                                        /* Rocky Depths */
    {{{WILD_BASE, {"HELL_HOUND", 10, 1, 2}}, {WILD_FLIES}}},                               /* Volcanic Rifts */
    {{{WILD_BASE, {NULL, 0, 0, 0}}, {{"FLY", 150, 2, 4}, {"TENTACLE", 10, 1, 1}}}},        /* Flooded Grottos */
    {{{{"SKELETON", 180, 4, 4}, {"SPIDER", 100, 4, 4}, {"BUG", 95, 4, 4}, {"GHOST", 10, 1, 1}},
      {WILD_FLIES}}},                                                                      /* Frozen Hollows */
    {{{{"BUG", 175, 4, 4}, {"SPIDER", 100, 4, 4}, {"SKELETON", 100, 4, 4}, {NULL, 0, 0, 0}},
      {WILD_FLIES}}},                                                                      /* Sandstone Wastes */
    {{{WILD_BASE, {NULL, 0, 0, 0}}, {WILD_FLIES}}},                                        /* Crystal Caverns */
    {{{WILD_BASE, {"GHOST", 10, 1, 1}}, {WILD_FLIES}}},                                    /* Ancient Ruins */
};

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

TbBool world_wild_creature(const struct Thing *thing)
{
    if (!world_mode_active() || (thing->class_id != TCls_Creature) || (thing->owner != PLAYER_GOOD))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    return !creature_control_invalid(cctrl) && (cctrl->party.original_objective == CHeroTsk_DefendSpawn);
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
#define WILD_MAX_ANCHORS 16
struct WildPlayer {
    int count;
    MapSlabCoord ax[WILD_MAX_ANCHORS], ay[WILD_MAX_ANCHORS];
    PlayerNumber plyr[WILD_MAX_ANCHORS];  /* whose view or heart */
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

/** The strongest creature's level of the human keeper nearest to a slab (1 with no human keeper). */
static int wild_top_level_near(const struct WildPlayer *pl, MapSlabCoord x, MapSlabCoord y)
{
    int best = -1;
    long best_d = 0;
    for (int i = 0; i < pl->count; i++)
    {
        const long d = wild_dist2(x, y, pl->ax[i], pl->ay[i]);
        if ((best < 0) || (d < best_d))
        {
            best = i;
            best_d = d;
        }
    }
    return (best >= 0) ? pl->top_level[best] : 1;
}

static TbBool wild_spawn(struct WildRng *rng, ThingModel model, MapSlabCoord x, MapSlabCoord y, int level)
{
    if (!creature_count_below_map_limit(0))
        return false;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x)) + (MapCoord)(wild_rand(rng, 193) - 96);
    pos.y.val = subtile_coord_center(slab_subtile_center(y)) + (MapCoord)(wild_rand(rng, 193) - 96);
    pos.z.val = 0;
    struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, model,
        PLAYER_GOOD);
    if (thing_is_invalid(thing))
        return false;
    init_creature_level(thing, (CrtrExpLevel)(level - 1));
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    cctrl->party.objective = CHeroTsk_DefendSpawn;
    cctrl->party.original_objective = CHeroTsk_DefendSpawn; /* marks wild creatures */
    cctrl->party.target_plyr_idx = -1;
    return true;
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
        if (list[k].name != NULL)
            total += list[k].weight;
    if (total <= 0)
        return 0;
    int r = wild_rand(rng, total), k = 0;
    while ((k < WILD_KINDS - 1) && ((list[k].name == NULL) || (r >= list[k].weight)))
    {
        if (list[k].name != NULL)
            r -= list[k].weight;
        k++;
    }
    if (list[k].name == NULL)
        return 0;
    const long model = get_id(creature_desc, list[k].name);
    if (model <= 0)
        return 0;
    const int group = list[k].min_group + wild_rand(rng, list[k].max_group - list[k].min_group + 1);
    const MapSlabCoord px = x, py = y;
    const int top = wild_top_level_near(pl, x, y);
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
            if (wild_spawn(rng, (ThingModel)model, x, y, low + wild_rand(rng, top - low + 1)))
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

void world_wild_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % WILD_CYCLE_TURNS) != 3))
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
        struct Thing *heartng = world_player_is_human(p) ? find_players_dungeon_heart(p) : INVALID_THING;
        if (thing_exists(heartng) && world_human_heart_loaded(p))
        {
            pl.ax[pl.count] = subtile_slab(heartng->mappos.x.stl.num);
            pl.ay[pl.count] = subtile_slab(heartng->mappos.y.stl.num);
            pl.plyr[pl.count] = p;
            pl.count++;
        }
    }
    for (int a = 0; a < pl.count; a++)
        pl.top_level[a] = world_keeper_top_level(pl.plyr[a]);
    const TbBool day = world_is_day();
    /* despawning, and counting against the caps */
    int counts[WiG_COUNT] = {0, 0};
    int despawned = 0;
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
        if (!fighting && !wild_near_player(&pl, x, y, WILD_KEEP_DIST) &&
            ((long)(game.play_gameturn - thing->creation_turn) > WILD_MIN_AGE) && (wild_rand(&rng, WILD_DESPAWN_ONE_IN) == 0))
        {
            world_remove_thing_quietly(thing);
            despawned++;
            continue;
        }
        /* they stay where they are: the hero AI drifts a creature that can't patrol over to attacking */
        if (!fighting && (cctrl->party.objective != CHeroTsk_DefendSpawn))
            cctrl->party.objective = CHeroTsk_DefendSpawn;
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
}

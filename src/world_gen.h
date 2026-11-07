/******************************************************************************/
/** @file world_gen.h
 *     Endless-world chunk generator.
 * @par Purpose:
 *     C port of the generation part of the Python reference generator (worldgen.py)
 *     (GENERATOR_VERSION 2). A chunk is a pure function of (seed, chunk x, chunk y):
 *     climate and biomes, terrain, caves, liquids, resources, then the structures (player start, enemy
 *     keepers, hero fortresses, neutral portals, rewards) with their things.
 *     The Python generator is the reference; tests/world_gen/ compares the two.
 * @par Comment:
 *     No engine dependencies: this file and world_gen.c also build natively for
 *     the reference test. Doubles must be IEEE (SSE2, no x87, no FMA contraction).
 */
/******************************************************************************/
#ifndef WORLD_GEN_H
#define WORLD_GEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WG_GENERATOR_VERSION 6 /* worldgen.GENERATOR_VERSION (2: biomes; 3: no gold round the start dungeon;
                                  4: the start portal lines up with the heart, 10-30 slabs out;
                                  5: no dirt path round the start dungeon; gems rare, 1-2 slabs;
                                  6: gold seams of hand-made-level shapes, across liquids and through rock;
                                  only reachable gold, gems and structures; the start portal 8-14 out) */
/* Version 7 (C only while it is tuned; worldgen.py follows when it is settled): biomes at several sizes that
 * shift with the distance from the start, ground shaped by each biome, ridges along borders, liquids that belong
 * to their biomes, landmarks (great rivers, mountain ranges, giant caverns), open and solid stretches, gold by
 * biome, and five new biomes: lava and flooded oceans (islands), lava and flooded rivers (braided, with fords)
 * and Dead Man's Land (plain earth). A world keeps the version it was made with. */
#define WG_GENERATOR_LATEST 7
#define WG_V6_BIOMES 8 /* the biomes of versions 2-6: points of the climate */
#define WG_CHUNK 16
#define WG_CHUNK_CELLS (WG_CHUNK * WG_CHUNK)
#define WG_MAX_CHUNK_THINGS 96
#define WG_MAX_CHUNK_STRUCTS 16
#define WG_NAME_LEN 24

/** Slabs the generator makes. Names (wg_slab_name) are fxdata/terrain.cfg names, as in worldgen.py. */
enum WgSlab {
    WgS_Rock = 0,   /* HARD */
    WgS_Gold,       /* GOLD */
    WgS_DenseGold,  /* DENSE_GOLD */
    WgS_Gems,       /* GEMS */
    WgS_Earth,      /* DIRT */
    WgS_Path,       /* PATH */
    WgS_Claimed,    /* PRETTY_PATH */
    WgS_Lava,       /* LAVA */
    WgS_Water,      /* WATER */
    WgS_Wall,       /* WALL: automatic reinforced wall */
    WgS_Portal,     /* ENTRANCE_ZONE */
    WgS_Heart,      /* HEART_PEDESTAL */
    WgS_Treasury,   /* TREASURY_AREA */
    WgS_Lair,       /* LAIR_AREA */
    WgS_Hatchery,   /* HATCHERY */
    WgS_Training,   /* TRAINING_AREA */
    WgS_Library,    /* BOOK_SHELVES */
    WgS_DoorWood,   /* DOOR_WOODEN */
    WgS_DoorBrace,  /* DOOR_BRACE */
    WgS_COUNT
};

/** Biomes (worldgen.py BIOMES, in that order). wg_biome_name gives worldgen.py's names. */
enum WgBiome {
    WgB_Earthen = 0,
    WgB_Rocky,
    WgB_Volcanic,
    WgB_Flooded,
    WgB_Frozen,
    WgB_Sandstone,
    WgB_Crystal,
    WgB_Ruins,
    /* generator version 7 (provinces and cells of their own, not points of the climate) */
    WgB_LavaOcean,
    WgB_FloodedOcean,
    WgB_LavaRivers,
    WgB_FloodedRivers,
    WgB_DeadMans,
    WgB_COUNT
};

/** Owner of a slab or thing. worldgen.py uses tokens: None, "player", "heroes", "keeper:keeper@cx,cy". */
enum WgOwner {
    WgO_Neutral = 0,
    WgO_Player  = 1,
    WgO_Heroes  = 2,
    WgO_Keeper  = 3, /* the keeper named by WgChunk.keeper (cells) or WgThing.keeper (things) */
};

/** Structure types, in worldgen.py's names: "start", "keeper", "fortress", "portal", "reward". */
enum WgStructType {
    WgT_Start = 0,
    WgT_Keeper,
    WgT_Fortress,
    WgT_Portal,
    WgT_Reward,
    WgT_COUNT
};

/** Identifies a structure, like worldgen.py's Structure.id "<type>@<start_cx>,<start_cy>". */
struct WgStructId {
    uint8_t type;      /* WgStructType */
    int32_t start_cx;
    int32_t start_cy;
};

/** Reward kinds (WgStructRef.reward), worldgen.py: "special_box", "neutral_creatures", "gold_hoard". */
enum WgRewardKind { WgR_None = 0, WgR_SpecialBox, WgR_NeutralCreatures, WgR_GoldHoard };

#define WG_MAX_REF_CREATURES 12

/** A name and how many of it, for WgStructRef.creatures. */
struct WgNameCount {
    char name[WG_NAME_LEN];
    uint8_t count;
};

/** A structure reference, like worldgen.py's Structure.ref(). Coordinates are world slabs. */
struct WgStructRef {
    struct WgStructId id;
    int64_t origin_x, origin_y;
    int64_t bbox_x1, bbox_y1, bbox_x2, bbox_y2;
    double distance;                 /* from the player start, unrounded */
    /* per type (zero when the type has no such field) */
    int64_t heart_x, heart_y;        /* start, keeper */
    int64_t portal_x, portal_y;      /* start, keeper, portal */
    int64_t gold_x, gold_y;          /* keeper */
    int64_t gate_x, gate_y;          /* fortress */
    int32_t threat_radius;           /* keeper: slabs */
    int32_t money;                   /* keeper: starting gold */
    int32_t size;                    /* fortress */
    uint8_t moat;                    /* fortress */
    uint8_t reward;                  /* reward: WgRewardKind */
    char box[WG_NAME_LEN];           /* reward special_box: object name */
    uint8_t rooms[4];                /* keeper: WgSlab of the room to the north, east, south, west */
    int32_t creature_kinds;          /* entries used in creatures[] */
    struct WgNameCount creatures[WG_MAX_REF_CREATURES]; /* keeper "creatures", fortress "heroes", reward
                                                           "creatures": count per name, in worldgen.py's order */
    uint8_t biome;                   /* WgBiome at the origin (ref()["biome"]) */
};

/** A thing placed by a structure (worldgen.py's thing dicts). */
struct WgThing {
    uint8_t is_creature;             /* 1: kind "creature", 0: kind "object" */
    char name[WG_NAME_LEN];          /* creature.cfg / objects.cfg name */
    uint8_t owner;                   /* WgOwner */
    struct WgStructId keeper;        /* when owner == WgO_Keeper */
    double x, y;                     /* world subtiles (slab = floor(x / 3)) */
    uint8_t level;                   /* creatures: 1..10; objects: 0 */
    uint8_t has_gate;                /* HERO_GATE: gate = its fortress */
    struct WgStructId gate;
};

/** One chunk: world slabs 16*cx .. 16*cx+15 (same for y). */
struct WgChunk {
    uint64_t seed;
    int32_t cx, cy;
    uint8_t slabs[WG_CHUNK_CELLS];   /* WgSlab, index ly*16+lx */
    uint8_t owners[WG_CHUNK_CELLS];  /* WgOwner */
    uint8_t biomes[WG_CHUNK_CELLS];  /* WgBiome, index ly*16+lx */
    uint8_t has_keeper;              /* 1 when any cell or thing is owned by a keeper */
    struct WgStructId keeper;        /* that keeper (at most one keeper can touch a chunk) */
    int thing_count;
    struct WgThing things[WG_MAX_CHUNK_THINGS];        /* in worldgen.py's order */
    int struct_count;
    struct WgStructRef structs[WG_MAX_CHUNK_STRUCTS];  /* structures touching the chunk, worldgen.py's order */
};

struct WgWorld;

/** worldgen.parse_seed for text: digit strings ("-12", "105") are integers masked to 64 bits; any other
 *  text is hashed (sha256 of "seed:" + text, first 8 bytes little endian). Returns 0 and sets *seed. */
int wg_parse_seed(const char *text, uint64_t *seed);

/** A world for a seed. Caches structure starts and layouts; free it with wg_world_destroy. */
struct WgWorld *wg_world_create(uint64_t seed);
/** A world of a given generator version (6 or 7; wg_world_create is version WG_GENERATOR_VERSION). */
struct WgWorld *wg_world_create_version(uint64_t seed, int version);
int wg_world_version(const struct WgWorld *world);
void wg_world_destroy(struct WgWorld *world);
uint64_t wg_world_seed(const struct WgWorld *world);

/** worldgen.World.generate_chunk. Returns 0, or -1 if a limit (things, structs) was exceeded. */
int wg_generate_chunk(struct WgWorld *world, int32_t cx, int32_t cy, struct WgChunk *out);

/** worldgen.World.structures_in(box, specs): structures of the types in type_mask (bit 1<<WgStructType;
 *  WgT_Start is never returned, as in Python) whose bbox overlaps the box, in Python's order. Returns how
 *  many were found (may exceed max; only max are written). A mask of 0 finds nothing (Python's specs=None
 *  is all four types: 0x1E). */
int wg_structures_in(struct WgWorld *world, int64_t x1, int64_t y1, int64_t x2, int64_t y2,
                     unsigned type_mask, struct WgStructRef *out, int max);

/** The player's start structure (heart at world slab 8,8). */
void wg_start_ref(struct WgWorld *world, struct WgStructRef *out);

const char *wg_slab_name(int slab);
const char *wg_struct_type_name(int type);
const char *wg_biome_name(int biome);
/** The KeeperFX texture pack of a biome (texture_pack_desc id: STANDARD 1, ANCIENT 2, WINTER 3, ...). */
int wg_biome_texture(int biome);
/** Biome at world slab (x, y): worldgen.World.biome_at. */
int wg_biome_at(const struct WgWorld *w, int64_t x, int64_t y);

#ifdef __cplusplus
}
#endif
#endif

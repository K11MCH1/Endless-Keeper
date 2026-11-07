/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_gen.c
 *     Endless-world chunk generator.
 * @par Purpose:
 *     C port of the generation part of the Python reference generator (worldgen.py)
 *     (GENERATOR_VERSION 2): hashing, random numbers, noise, climate and biomes,
 *     terrain, gems, structure placement (random spread) and structure layouts.
 *     A chunk is a pure function of (seed, chunk x, chunk y).
 * @par Comment:
 *     C standard library only, so tests/world_gen/ can build it natively and
 *     compare it with the Python reference chunk by chunk.
 *     Every floating point operation is one IEEE double operation in the same
 *     order as in Python: SSE2 math on 32-bit x86, no x87, no FMA contraction.
 *     Python ints are emulated with uint64_t wrap-around (hashes) and int64_t
 *     (coordinates), with Python's floor division, floor shifts and round().
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "world_gen.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Python floats: IEEE doubles, every operation rounded on its own. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("fp-contract=off")
#if defined(__i386__)
#pragma GCC target ("sse2,fpmath=sse")
#endif
#elif defined(__clang__)
#pragma clang fp contract(off)
#endif

/* (GCC does not update __SSE2_MATH__ for the pragma, but it does switch the code generation) */
#if defined(__i386__) && !defined(__SSE2_MATH__) && !(defined(__GNUC__) && !defined(__clang__))
#error "world_gen.c must use SSE2 double math (-msse2 -mfpmath=sse), not x87, to match worldgen.py"
#endif

/******************************************************************************/
#define WG_GOLDEN 0x9E3779B97F4A7C15ULL
#define WG_P1 0x9E3779B97F4A7C15ULL
#define WG_P2 0xC2B2AE3D27D4EB4FULL
#define WG_P3 0xD6E8FEB86659FD93ULL
#define WG_OCTAVE 0x632BE59BD9B4E019ULL
#define WG_I32_MIN (-2147483647LL - 1)
#define WG_I32_MAX 2147483647LL
#define START_X 8 /* worldgen.PLAYER_START */
#define START_Y 8
/* worldgen.START_GOLD_FREE: no gold this close (square distance) to the player's heart; the heart room and its
 * walls reach 4 slabs out, so 15 slabs of space are left between the dungeon and the nearest gold */
#define START_GOLD_FREE (4 + 15)
/* worldgen.START_PATH_FREE: no dirt path (unclaimed path) this close to the heart (square distance): 30 slabs
 * round the dungeon. Other structures never reach in this far. */
#define START_PATH_FREE (4 + 30)
/* worldgen.SEAM_*: gold seams (generator version 6). One possible seam in every SEAM_CELL x SEAM_CELL square: a
 * bent ellipse with a lumpy edge, half of them with a second lobe, a third with a dense core. They fill earth,
 * liquids and rock; caves cut them; only the pieces you can reach are kept. */
#define SEAM_CELL 24
#define SEAM_CHANCE 0.45
#define SEAM_HALF_LENGTH_MIN 3.5
#define SEAM_HALF_LENGTH_EXTRA 6.5
#define SEAM_ASPECT_MIN 1.3
#define SEAM_ASPECT_EXTRA 3.2
#define SEAM_MIN_HALF_WIDTH 1.4
#define SEAM_LOBE_CHANCE 0.5
#define SEAM_DENSE_CHANCE 0.35
#define SEAM_EDGE 0.6
/* worldgen.REACH_CELLS: reachable ground is open (not rock) and a flood through open ground from it reaches this
 * many slabs (the pockets inside rock masses are far smaller) */
#define REACH_CELLS 400
#define REACH_SET_CAP 4096 /* the flood's hash set: up to REACH_CELLS open slabs and their rock neighbours */
#define GEMS_TRIES 4       /* worldgen.GEMS_TRIES */
#define STRUCTURE_TRIES 8  /* worldgen.STRUCTURE_TRIES: the first where it always was, the others anywhere in the chunk */
/* Structures only exist in chunks inside the int32 range, so boxes can be clamped to this. */
#define BOX_LIMIT (1LL << 40)

/* Caches (results never depend on them). Cleared at the next call once over the CLEAR size;
 * past MAX_FILL (a single huge wg_structures_in), results are computed without being cached.
 * CAPs are powers of two. The test overrides them with tiny ones (WG_SMALL_CACHES). */
#ifdef WG_SMALL_CACHES
#define CAND_CAP 64
#define CAND_CLEAR 8
#define CAND_MAX_FILL 16
#define LAY_CAP 16
#define LAY_CLEAR 2
#define LAY_MAX_FILL 3
#define SEAM_CAP 32 /* holds the 3x3 squares round a slab; emptied every 16 seams */
#else
#define CAND_CAP 16384
#define CAND_CLEAR 6144
#define CAND_MAX_FILL 12288
#define LAY_CAP 4096
#define LAY_CLEAR 1536
#define LAY_MAX_FILL 3072
#define SEAM_CAP 1024
#endif
#define LAY_MAX_BYTES ((size_t)24 << 20)

#define MAX_LTHINGS 32  /* things of one structure: a keeper has at most 6 imps and 12 creatures */
#define MAX_FOUND 64    /* structures touching one chunk, before the WG_MAX_CHUNK_STRUCTS check */

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { D_N = 0, D_E, D_S, D_W }; /* worldgen.DIRS order */
static const int dir_x[4] = {0, 1, 0, -1};
static const int dir_y[4] = {-1, 0, 1, 0};

static const char *const slab_names[WgS_COUNT] = {
    "HARD", "GOLD", "DENSE_GOLD", "GEMS", "DIRT", "PATH", "PRETTY_PATH", "LAVA", "WATER", "WALL",
    "ENTRANCE_ZONE", "HEART_PEDESTAL", "TREASURY_AREA", "LAIR_AREA", "HATCHERY", "TRAINING_AREA",
    "BOOK_SHELVES", "DOOR_WOODEN", "DOOR_BRACE"};

static const char *const type_names[WgT_COUNT] = {"start", "keeper", "fortress", "portal", "reward"};

enum {
    N_WX, N_WY, N_HARD, N_ROCK, N_CAVE, N_CAVEM, N_TUN, N_TUNM, N_LAVA, N_LRIV, N_LRIVM, N_WATER,
    N_GOLD, N_GOLDB,
    N_HEAT, N_MOIST, N_HARDC, N_WEIRD, N_BWX, N_BWY, N_WRIV, N_WRIVM, N_COUNT
};
static const char *const noise_names[N_COUNT] = {
    "wx", "wy", "hard", "rock", "cave", "cavem", "tun", "tunm", "lava", "lriv", "lrivm", "water",
    "gold", "goldb",
    "heat", "moist", "hardness", "weird", "bwx", "bwy", "wriv", "wrivm"};

/* worldgen.BIOMES: name, climate point (heat, moisture, hardness, weirdness), texture pack id */
static const char *const biome_names[WgB_COUNT] = {
    "earthen", "rocky", "volcanic", "flooded", "frozen", "sandstone", "crystal", "ruins",
    "lava_ocean", "flooded_ocean", "lava_rivers", "flooded_rivers", "deadmans_land"};
static const double biome_point[WG_V6_BIOMES][4] = {
    {0.00, 0.00, 0.00, 0.00},   /* earthen */
    {0.00, 0.00, 0.34, 0.00},   /* rocky */
    {0.34, -0.07, 0.09, 0.00},  /* volcanic */
    {-0.04, 0.34, -0.09, 0.00}, /* flooded */
    {-0.34, 0.09, 0.07, 0.00},  /* frozen */
    {0.23, -0.32, -0.20, 0.00}, /* sandstone */
    {0.00, 0.00, 0.14, 0.62},   /* crystal */
    {0.00, 0.00, -0.14, -0.62}, /* ruins */
};
static const int biome_texture[WgB_COUNT] = {1, 7, 13, 12, 3, 9, 10, 2, 13, 12, 13, 12, 1};

/** worldgen.BIOME_CREATURES: weight multipliers (1 when not listed) */
struct BiomeMult {
    uint8_t biome;
    const char *name;
    int mult;
};
static const struct BiomeMult biome_creatures[] = {
    {WgB_Volcanic, "HELL_HOUND", 3}, {WgB_Volcanic, "DEMONSPAWN", 2}, {WgB_Volcanic, "DRAGON", 2},
    {WgB_Flooded, "TENTACLE", 3}, {WgB_Flooded, "BUG", 2}, {WgB_Flooded, "FLY", 2}, {WgB_Flooded, "SPIDER", 2},
    {WgB_Frozen, "GHOST", 3}, {WgB_Frozen, "SKELETON", 3}, {WgB_Frozen, "VAMPIRE", 2},
    {WgB_Sandstone, "ORC", 2}, {WgB_Sandstone, "TROLL", 2}, {WgB_Sandstone, "BILE_DEMON", 2}};

/* worldgen._GX / _GY (math.cos / math.sin of i*pi/8), bit for bit */
static const double grad_x[16] = {
    0x1.0000000000000p+0, 0x1.d906bcf328d46p-1, 0x1.6a09e667f3bcdp-1, 0x1.87de2a6aea964p-2,
    0x1.1a62633145c07p-54, -0x1.87de2a6aea962p-2, -0x1.6a09e667f3bccp-1, -0x1.d906bcf328d46p-1,
    -0x1.0000000000000p+0, -0x1.d906bcf328d47p-1, -0x1.6a09e667f3bcep-1, -0x1.87de2a6aea96dp-2,
    -0x1.a79394c9e8a0ap-53, 0x1.87de2a6aea967p-2, 0x1.6a09e667f3bcbp-1, 0x1.d906bcf328d44p-1};
static const double grad_y[16] = {
    0x0.0p+0, 0x1.87de2a6aea963p-2, 0x1.6a09e667f3bccp-1, 0x1.d906bcf328d46p-1,
    0x1.0000000000000p+0, 0x1.d906bcf328d46p-1, 0x1.6a09e667f3bcdp-1, 0x1.87de2a6aea965p-2,
    0x1.1a62633145c07p-53, -0x1.87de2a6aea961p-2, -0x1.6a09e667f3bccp-1, -0x1.d906bcf328d44p-1,
    -0x1.0000000000000p+0, -0x1.d906bcf328d45p-1, -0x1.6a09e667f3bcep-1, -0x1.87de2a6aea96ep-2};

/* math.radians(x) is x * (pi / 180.0) in CPython */
static const double deg_to_rad = 0x1.1df46a2529d39p-6;

struct Weighted {
    const char *name;
    int weight;
    int min_dist;
};

static const struct Weighted keeper_creatures[] = {
    {"FLY", 4, 0}, {"BUG", 4, 0}, {"SPIDER", 3, 0}, {"DEMONSPAWN", 3, 0}, {"TROLL", 3, 0}, {"ORC", 3, 0},
    {"SORCEROR", 2, 200}, {"HELL_HOUND", 2, 200}, {"DARK_MISTRESS", 2, 260}, {"BILE_DEMON", 2, 260},
    {"TENTACLE", 1, 260}, {"VAMPIRE", 1, 400}, {"DRAGON", 1, 450}};
static const struct Weighted hero_types[] = {
    {"ARCHER", 4, 0}, {"DWARFA", 3, 0}, {"THIEF", 2, 0}, {"BARBARIAN", 3, 120}, {"MONK", 2, 160},
    {"WIZARD", 2, 200}, {"WITCH", 1, 260}, {"SAMURAI", 1, 320}, {"GIANT", 1, 320}, {"KNIGHT", 1, 400}};
static const struct Weighted special_boxes[] = {
    {"SPECBOX_REVMAP", 3, 0}, {"SPECBOX_RESURCT", 2, 0}, {"SPECBOX_TRANSFR", 2, 0}, {"SPECBOX_STEALHR", 2, 0},
    {"SPECBOX_MULTPLY", 1, 0}, {"SPECBOX_INCLEV", 3, 0}, {"SPECBOX_MKSAFE", 1, 0}, {"SPECBOX_HIDNWRL", 2, 0},
    {"SPECBOX_HEALALL", 2, 0}, {"SPECBOX_GETGOLD", 3, 0}};
static const struct Weighted neutral_creatures[] = {
    {"FLY", 3, 0}, {"BUG", 3, 0}, {"SPIDER", 2, 0}, {"DEMONSPAWN", 2, 0}, {"TROLL", 2, 60}, {"ORC", 2, 60},
    {"SORCEROR", 1, 150}, {"DARK_MISTRESS", 1, 200}, {"BILE_DEMON", 1, 200}, {"HELL_HOUND", 1, 150},
    {"VAMPIRE", 1, 350}, {"DRAGON", 1, 400}};

/** worldgen.BIOME_EXTRA_CREATURES: appended to a biome's keeper and neutral creature pools */
struct BiomeExtra {
    uint8_t biome;
    struct Weighted w;
};
static const struct BiomeExtra biome_extra_creatures[] = {
    {WgB_Frozen, {"GHOST", 1, 0}}, {WgB_Frozen, {"SKELETON", 1, 0}}};

/** worldgen.BIOME_GEMS / GEMS_DEFAULT: gem cluster chance per chunk and max extra cells (default 0.01, 1):
 *  gems are very rare and come as 1-2 slabs */
static const double biome_gem_chance[WgB_COUNT] = {0.01, 0.01, 0.015, 0.01, 0.01, 0.01, 0.04, 0.01, 0.01, 0.01, 0.01, 0.01, 0.0};
static const int biome_gem_extra[WgB_COUNT] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
#define MAX_GEM_CELLS 2 /* 1 + the largest biome_gem_extra */

/* version 7's noises (salted "v7:<name>") */
enum {
    V7_BWX, V7_BWY, V7_RHEAT, V7_RMOIST, V7_RHARD, V7_FINE, V7_HEAT, V7_MOIST, V7_HARD, V7_WEIRD, V7_OCEAN,
    V7_RIVERP, V7_DMLW, V7_DMLCELL, V7_DITHER, V7_RIB, V7_RIDGE, V7_RIDGEON, V7_RIDGEP, V7_OPEN, V7_LLAKEM,
    V7_LLAKE, V7_LRIV, V7_LRIVM, V7_WLAKEM, V7_WLAKE, V7_WRIV, V7_WRIVM, V7_RUINS, V7_CAVERN, V7_PILLAR,
    V7_MOUNT, V7_MOUNTM, V7_MOUNTW, V7_PASS, V7_GRIVER, V7_GRIVERM, V7_GFORD, V7_ISLAND, V7_ISLET, V7_RIV1,
    V7_RIV2, V7_RIV3, V7_RIVM, V7_FORD, V7_SEAM, V7_DMLSTART, V7_LIQTYPE, V7_ARCH, V7_ISLET2, V7_ISLET3, V7_COUNT
};
static const char *const v7_names[V7_COUNT] = {
    "bwx", "bwy", "rheat", "rmoist", "rhard", "fine", "heat", "moist", "hard", "weird", "ocean",
    "riverp", "dmlw", "dmlcell", "dither", "rib", "ridge", "ridgeon", "ridgep", "open", "llakem",
    "llake", "lriv", "lrivm", "wlakem", "wlake", "wriv", "wrivm", "ruins", "cavern", "pillar",
    "mount", "mountm", "mountw", "pass", "griver", "griverm", "gford", "island", "islet", "riv1",
    "riv2", "riv3", "rivm", "ford", "seam", "dmlstart", "liqtype", "arch", "islet2", "islet3"};

/** worldgen.Spec (random spread placement); indexed by WgStructType. */
struct Spec {
    int spacing, separation;
    double chance;
    int min_dist, radius, priority, margin;
};
static const struct Spec specs[WgT_COUNT] = {
    {1, 0, 1.0, 0, 32, 99, 3},     /* start: the portal's path ring reaches 32 slabs out */
    {24, 10, 0.45, 150, 24, 4, 8}, /* keeper */
    {14, 5, 0.6, 90, 10, 3, 3},    /* fortress */
    {7, 2, 0.75, 40, 6, 2, 3},     /* portal */
    {4, 1, 0.55, 26, 6, 1, 3},     /* reward */
};
/* worldgen.SPECS (highest priority first) is keeper, fortress, portal, reward: the enum order */
#define PLACED_MASK ((1u << WgT_Keeper) | (1u << WgT_Fortress) | (1u << WgT_Portal) | (1u << WgT_Reward))

enum { OP_NONE = 0, OP_SOFT, OP_SET };

/** A structure start: worldgen.Structure without its pieces. */
struct Cand {
    uint8_t type;
    int32_t ccx, ccy;        /* start chunk */
    int64_t ox, oy;          /* origin slab */
    int64_t x1, y1, x2, y2;  /* bbox */
    uint64_t seed;
    double distance;
    uint8_t biome;           /* Structure.biome: the biome at the origin */
};

struct LThing {
    uint8_t is_creature, owner, level, has_gate;
    const char *name;
    int64_t sx, sy;          /* slab */
    double x, y;             /* subtiles */
};

/** A built structure: worldgen.Structure.pieces() as a grid over the bbox, plus its ref. */
struct Layout {
    struct Cand st;
    struct WgStructRef ref;
    int side;
    int nthings;
    int overflow;
    size_t bytes;
    struct LThing things[MAX_LTHINGS];
    uint8_t *op, *slab, *own;
    uint8_t cells[];
};

/** A gold seam's slabs, kept in a direct-mapped cache: the grid over their bounding box (0, WgS_Gold or
 *  WgS_DenseGold); w = 0 when the square has none. */
struct SeamEntry {
    uint8_t valid;
    int64_t cx, cy;
    int64_t x0, y0;
    int w, h;
    uint8_t *cells;
};

/** The reachability flood's scratch: an open-addressing set of slabs (0 empty, 1 rock, 2 open, 3 open and seen). */
struct ReachSet {
    int64_t x[REACH_SET_CAP], y[REACH_SET_CAP];
    uint8_t st[REACH_SET_CAP];
    int64_t stack[REACH_CELLS + 8][2];
};

struct CandEntry {
    uint32_t gen;
    uint8_t type;
    uint8_t exists;
    int64_t rx, ry;
    struct Cand c;
};

struct WgWorld {
    uint64_t seed;
    uint64_t s_noise[N_COUNT];
    uint64_t s_gems;
    uint64_t s_seam;
    int version;                     /* the generator version (6 or 7) */
    uint64_t s7[V7_COUNT];           /* version 7's noises */
    int dml_start;                   /* version 7: Dead Man's Land round the start (1 seed in 100) */
    double dml_cx, dml_cy, dml_r;
    uint64_t spec_salt[WgT_COUNT];
    struct SeamEntry *seam;
    int seam_count;
    struct ReachSet *reach;
    struct Cand start;
    struct Layout *start_layout;
    struct CandEntry *cand;
    uint32_t cand_gen;
    int cand_count;
    struct Layout **lay;
    int lay_count;
    size_t lay_bytes;
};

/******************************************************************************/
/* Hashing                                                                    */
/******************************************************************************/

static uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/** worldgen.hash64: integers hash as their low 64 bits (two's complement). */
static uint64_t hash_vals(const uint64_t *v, int n)
{
    uint64_t h = 0x243F6A8885A308D3ULL;
    int i;
    for (i = 0; i < n; i++)
        h = mix64((h ^ v[i]) + WG_GOLDEN);
    return h;
}

static uint64_t hash2(uint64_t a, uint64_t b)
{
    uint64_t v[2];
    v[0] = a; v[1] = b;
    return hash_vals(v, 2);
}

static uint64_t hash3(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t v[3];
    v[0] = a; v[1] = b; v[2] = c;
    return hash_vals(v, 3);
}

static uint64_t hash4(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t v[4];
    v[0] = a; v[1] = b; v[2] = c; v[3] = d;
    return hash_vals(v, 4);
}

static uint64_t hash5(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    uint64_t v[5];
    v[0] = a; v[1] = b; v[2] = c; v[3] = d; v[4] = e;
    return hash_vals(v, 5);
}

/******************************************************************************/
/* SHA-256 (for worldgen.salt and text seeds)                                 */
/******************************************************************************/

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    size_t used;
    uint64_t total;
};

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(v, n) (((v) >> (n)) | ((v) << (32 - (n))))

static void sha256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        t1 = k + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256_init(struct Sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->used = 0;
    s->total = 0;
}

static void sha256_update(struct Sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->total += len;
    while (len > 0) {
        size_t n = 64 - s->used;
        if (n > len)
            n = len;
        memcpy(s->buf + s->used, p, n);
        s->used += n;
        p += n;
        len -= n;
        if (s->used == 64) {
            sha256_block(s->h, s->buf);
            s->used = 0;
        }
    }
}

/** First 8 bytes of the digest, little endian (worldgen.salt). */
static uint64_t sha256_final64(struct Sha256 *s)
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    uint64_t out = 0;
    int i;
    sha256_update(s, &pad, 1);
    while (s->used != 56)
        sha256_update(s, &zero, 1);
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, len, 8);
    /* digest bytes 0..7 are h[0] and h[1], big endian; read them as a little endian number */
    for (i = 0; i < 4; i++)
        out |= (uint64_t)((s->h[0] >> (24 - 8 * i)) & 0xFF) << (8 * i);
    for (i = 0; i < 4; i++)
        out |= (uint64_t)((s->h[1] >> (24 - 8 * i)) & 0xFF) << (8 * (i + 4));
    return out;
}

static uint64_t salt2(const char *prefix, const char *text, size_t text_len)
{
    struct Sha256 s;
    sha256_init(&s);
    sha256_update(&s, prefix, strlen(prefix));
    sha256_update(&s, text, text_len);
    return sha256_final64(&s);
}

/******************************************************************************/
/* Seeds                                                                      */
/******************************************************************************/

/** Code point of the UTF-8 sequence at p (n bytes), or -1. */
static long utf8_at(const unsigned char *p, size_t n)
{
    if (n == 1 && p[0] < 0x80)
        return p[0];
    if (n == 2 && (p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
        return ((long)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (n == 3 && (p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        return ((long)(p[0] & 0x0F) << 12) | ((long)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return -1;
}

/** Python's str.isspace() (what str.strip() removes). */
static int py_space(long c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

static size_t utf8_len(unsigned char b)
{
    if (b < 0x80)
        return 1;
    if ((b & 0xE0) == 0xC0)
        return 2;
    if ((b & 0xF0) == 0xE0)
        return 3;
    return 4;
}

int wg_parse_seed(const char *text, uint64_t *seed)
{
    const unsigned char *b, *e, *p;
    uint64_t v = 0;
    int neg = 0, digits = 1;
    if (text == NULL || seed == NULL)
        return -1;
    b = (const unsigned char *)text;
    e = b + strlen(text);
    /* str(seed).strip() */
    while (b < e) {
        size_t n = utf8_len(*b);
        if (n > (size_t)(e - b) || !py_space(utf8_at(b, n)))
            break;
        b += n;
    }
    while (e > b) {
        size_t n = 1;
        while (n < 4 && n < (size_t)(e - b) && (e[-(ptrdiff_t)n] & 0xC0) == 0x80)
            n++;
        if (!py_space(utf8_at(e - n, n)))
            break;
        e -= n;
    }
    if (b == e)
        return -1; /* MapError("seed is empty") */
    /* re.fullmatch(r"-?\d+", s): int(s) & M64 (ASCII digits) */
    p = b;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p == e)
        digits = 0;
    for (; p < e && digits; p++) {
        if (*p < '0' || *p > '9')
            digits = 0;
        else
            v = v * 10 + (uint64_t)(*p - '0');
    }
    if (digits) {
        *seed = neg ? (uint64_t)0 - v : v;
        return 0;
    }
    *seed = salt2("seed:", (const char *)b, (size_t)(e - b));
    return 0;
}

/******************************************************************************/
/* Random numbers, Python arithmetic                                          */
/******************************************************************************/

/** worldgen.Rng (splitmix64). */
struct Rng {
    uint64_t s;
};

static uint64_t rng_next(struct Rng *r)
{
    r->s += WG_GOLDEN;
    return mix64(r->s);
}

static double rng_random(struct Rng *r)
{
    return (double)(int64_t)(rng_next(r) >> 11) / 9007199254740992.0;
}

static int64_t rng_randint(struct Rng *r, int64_t a, int64_t b)
{
    return a + (int64_t)(rng_next(r) % (uint64_t)(b - a + 1));
}

static int rng_chance(struct Rng *r, double p)
{
    return rng_random(r) < p;
}

/** Rng.weighted over integer weights: returns the index. */
static int rng_weighted(struct Rng *r, const int *weights, int n)
{
    int total = 0, i;
    double x;
    for (i = 0; i < n; i++)
        total += weights[i];
    x = rng_random(r) * (double)total;
    for (i = 0; i < n; i++) {
        x -= (double)weights[i];
        if (x < 0)
            return i;
    }
    return n - 1;
}

static void rng_shuffle(struct Rng *r, void *base, int n, size_t size)
{
    unsigned char *p = (unsigned char *)base, tmp[32];
    int i, j;
    for (i = n - 1; i > 0; i--) {
        j = (int)rng_randint(r, 0, i);
        if (j != i) {
            memcpy(tmp, p + (size_t)i * size, size);
            memcpy(p + (size_t)i * size, p + (size_t)j * size, size);
            memcpy(p + (size_t)j * size, tmp, size);
        }
    }
}

/** Python's a // b for b > 0 (also a >> 4 for b = 16). */
static int64_t fdiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (a % b != 0 && a < 0)
        q--;
    return q;
}

/** Python's round(x) for a float: nearest integer, ties to even. */
static int64_t py_round(double v)
{
    double f = floor(v), diff = v - f;
    int64_t i = (int64_t)f;
    if (diff > 0.5 || (diff == 0.5 && (i & 1) != 0))
        i++;
    return i;
}

static int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

/* -- math.hypot of integers: correctly rounded sqrt(a*a + b*b), as CPython's hypot is -- */

struct U128 {
    uint64_t hi, lo;
};

static struct U128 mul_64x64(uint64_t a, uint64_t b)
{
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    struct U128 r;
    r.lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

static struct U128 add_128(struct U128 a, struct U128 b)
{
    struct U128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static struct U128 shl_128(struct U128 a, int k)
{
    struct U128 r;
    if (k == 0)
        return a;
    if (k >= 64) {
        r.hi = a.lo << (k - 64);
        r.lo = 0;
    } else {
        r.hi = (a.hi << k) | (a.lo >> (64 - k));
        r.lo = a.lo << k;
    }
    return r;
}

static int cmp_128(struct U128 a, struct U128 b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

static uint64_t dbl_bits(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static double bits_dbl(uint64_t u)
{
    double v;
    memcpy(&v, &u, sizeof v);
    return v;
}

/** Compares n with ((lo + hi) / 2)^2 for neighbouring positive normal doubles lo < hi. */
static int cmp_mid_square(struct U128 n, double lo, double hi)
{
    uint64_t blo = dbl_bits(lo), bhi = dbl_bits(hi);
    int elo = (int)((blo >> 52) & 0x7FF) - 1075, ehi = (int)((bhi >> 52) & 0x7FF) - 1075;
    uint64_t mlo = (blo & 0xFFFFFFFFFFFFFULL) | (1ULL << 52), mhi = (bhi & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
    int e = elo < ehi ? elo : ehi, k;
    uint64_t m = (mlo << (elo - e)) + (mhi << (ehi - e)); /* lo + hi = m * 2^e, the midpoint m * 2^(e-1) */
    struct U128 msq = mul_64x64(m, m);
    k = 2 - 2 * e; /* compare n * 2^k with m^2 */
    if (k >= 0)
        return k > 126 ? 1 : cmp_128(shl_128(n, k), msq);
    return -k > 126 ? -1 : cmp_128(n, shl_128(msq, -k));
}

static double hypot_int(int64_t a, int64_t b)
{
    uint64_t ua = (uint64_t)abs64(a), ub = (uint64_t)abs64(b), bits;
    struct U128 n = add_128(mul_64x64(ua, ua), mul_64x64(ub, ub));
    double d;
    int i;
    if (n.hi == 0 && n.lo <= (1ULL << 53))
        return sqrt((double)(int64_t)n.lo); /* exact square, correctly rounded root */
    d = sqrt((double)n.hi * 18446744073709551616.0 + (double)(int64_t)(n.lo >> 1) * 2.0);
    for (i = 0; i < 16; i++) {
        bits = dbl_bits(d);
        if (cmp_mid_square(n, d, bits_dbl(bits + 1)) > 0)
            d = bits_dbl(bits + 1);
        else if (cmp_mid_square(n, bits_dbl(bits - 1), d) < 0)
            d = bits_dbl(bits - 1);
        else
            break;
    }
    return d;
}

/** Python's float(n) for an int 0 <= n < 2^128: correctly rounded, ties to even. Below 2^63 it is the
 *  int64 conversion (one rounding, to nearest); above, it rounds in integer arithmetic. */
static double u128_to_double(struct U128 n)
{
    uint64_t top = n.hi, m;
    int bits = 128, shift, c;
    struct U128 rest, half;
    if (n.hi == 0 && (n.lo >> 63) == 0)
        return (double)(int64_t)n.lo;
    if (top == 0) {
        top = n.lo;
        bits = 64;
    }
    while ((top >> 63) == 0) {
        top <<= 1;
        bits--;
    }
    shift = bits - 53; /* >= 11: keep the top 53 bits */
    if (shift >= 64)
        m = n.hi >> (shift - 64);
    else
        m = (n.hi << (64 - shift)) | (n.lo >> shift);
    /* the bits shifted out, against half of the last kept bit */
    if (shift >= 64) {
        rest.hi = n.hi & ((1ULL << (shift - 64)) - 1);
        rest.lo = n.lo;
    } else {
        rest.hi = 0;
        rest.lo = n.lo & ((1ULL << shift) - 1);
    }
    half.hi = 0;
    half.lo = 1;
    half = shl_128(half, shift - 1);
    c = cmp_128(rest, half);
    if (c > 0 || (c == 0 && (m & 1) != 0))
        m++;
    if ((m >> 53) != 0) {
        m >>= 1;
        shift++;
    }
    return bits_dbl(((uint64_t)(shift + 52 + 1023) << 52) | (m & 0xFFFFFFFFFFFFFULL));
}

/******************************************************************************/
/* Noise                                                                      */
/******************************************************************************/

static unsigned lattice(uint64_t h)
{
    return (unsigned)(((h ^ (h >> 32)) * WG_P3) >> 60);
}

/** math.floor(v) as a double and as an integer. Python's floor returns an int, so the double is never -0.0.
 *  For |v| < 2^50 (every noise input) it is done in SSE2 arithmetic, without libm or x87 conversions:
 *  adding 1.5 * 2^52 rounds to an integer (round to nearest, as all of this code needs), whose low
 *  mantissa bits are then that integer + 2^51. */
static double floor_int(double v, int64_t *iv)
{
    const double magic = 6755399441055744.0; /* 0x1.8p52 */
    double t;
    if (!(fabs(v) < 1125899906842624.0)) { /* 2^50 */
        t = floor(v);
        *iv = (int64_t)t;
        return t + 0.0;
    }
    t = (v + magic) - magic;
    if (t > v)
        t -= 1.0;
    *iv = (int64_t)(dbl_bits(t + magic) & 0xFFFFFFFFFFFFFULL) - (1LL << 51);
    return t;
}

/** worldgen.perlin */
static double perlin(uint64_t seed, double x, double y)
{
    int64_t xi, yi;
    double fx0 = floor_int(x, &xi), fy0 = floor_int(y, &yi);
    double fx = x - fx0, fy = y - fy0;
    double u = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0);
    double v = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0);
    uint64_t ax = (uint64_t)xi * WG_P1, bx = ((uint64_t)xi + 1) * WG_P1;
    uint64_t ay = (uint64_t)yi * WG_P2, by = ((uint64_t)yi + 1) * WG_P2;
    unsigned h00 = lattice(seed ^ ax ^ ay), h10 = lattice(seed ^ bx ^ ay);
    unsigned h01 = lattice(seed ^ ax ^ by), h11 = lattice(seed ^ bx ^ by);
    double fx1 = fx - 1.0, fy1 = fy - 1.0;
    double n00 = grad_x[h00] * fx + grad_y[h00] * fy;
    double n10 = grad_x[h10] * fx1 + grad_y[h10] * fy;
    double n01 = grad_x[h01] * fx + grad_y[h01] * fy1;
    double n11 = grad_x[h11] * fx1 + grad_y[h11] * fy1;
    double a = n00 + u * (n10 - n00);
    double b = n01 + u * (n11 - n01);
    return (a + v * (b - a)) * 1.4142;
}

/** worldgen.fbm */
static double fbm(uint64_t seed, double x, double y, int octaves)
{
    double total = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
    int o;
    for (o = 0; o < octaves; o++) {
        total += amp * perlin(seed + (uint64_t)o * WG_OCTAVE, x * f, y * f);
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return total / norm;
}

/** worldgen.World.climate: (heat, moisture, hardness, weirdness) at world slab (xi, yi). */
static void climate(const struct WgWorld *w, int64_t xi, int64_t yi, double *clim)
{
    const uint64_t *s = w->s_noise;
    double x = (double)xi, y = (double)yi, bx, by, heat, moist, hard, weird, d, f;
    int64_t dx = xi - START_X, dy = yi - START_Y;
    struct U128 dd = add_128(mul_64x64((uint64_t)abs64(dx), (uint64_t)abs64(dx)),
                             mul_64x64((uint64_t)abs64(dy), (uint64_t)abs64(dy)));
    d = sqrt(u128_to_double(dd)); /* math.sqrt(float(dx * dx + dy * dy)) */
    f = (d - 48.0) / 144.0;
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;
    if (f == 0.0) {
        /* Python gets +-0.0 from noise * 0.0: the same biome and terrain as +0.0 (only compared, squared,
         * or added to non-zero values) */
        clim[0] = clim[1] = clim[2] = clim[3] = 0.0;
        return;
    }
    bx = x + 24.0 * fbm(s[N_BWX], x / 80.0, y / 80.0, 2);
    by = y + 24.0 * fbm(s[N_BWY], x / 80.0, y / 80.0, 2);
    heat = fbm(s[N_HEAT], bx / 420.0, by / 420.0, 2);
    moist = fbm(s[N_MOIST], bx / 380.0, by / 380.0, 2);
    hard = fbm(s[N_HARDC], bx / 340.0, by / 340.0, 2);
    weird = fbm(s[N_WEIRD], bx / 300.0, by / 300.0, 2);
    clim[0] = heat * f;
    clim[1] = moist * f;
    clim[2] = hard * f;
    clim[3] = weird * f;
}

/** worldgen.biome_of: the biome whose climate point is nearest (the first one on a tie). */
static int biome_of(const double *clim)
{
    int best = 0, i;
    double best_d = 0.0;
    for (i = 0; i < WG_V6_BIOMES; i++) {
        const double *p = biome_point[i];
        double t0 = clim[0] - p[0], t1 = clim[1] - p[1], t2 = clim[2] - p[2], t3 = clim[3] - p[3];
        double d = t0 * t0 + t1 * t1 + t2 * t2 + t3 * t3; /* ((a + b) + c) + d, as in Python */
        if (i == 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

static int biome_at(const struct WgWorld *w, int64_t x, int64_t y)
{
    double clim[4];
    climate(w, x, y, clim);
    return biome_of(clim);
}

/** worldgen._climate_part: the positive part of a climate value, at most 0.4. */
static double climate_part(double v)
{
    if (v <= 0.0)
        return 0.0;
    return v < 0.4 ? v : 0.4;
}

/** Python's max(a, b) of floats (b only when it is greater). */
static double py_max(double a, double b)
{
    return b > a ? b : a;
}

static double clamp_hard(double hard)
{
    if (hard > 0.4)
        hard = 0.4;
    if (hard < -0.4)
        hard = -0.4;
    return hard;
}

/** worldgen.World._shape: stage 1, the warped position and whether the slab is impenetrable rock. */
static int shape_rock(const struct WgWorld *w, int64_t xi, int64_t yi, double hard, double *pwx, double *pwy)
{
    const uint64_t *s = w->s_noise;
    double x = (double)xi, y = (double)yi, wx, wy, rock_limit, rock;
    wx = x + 7.0 * fbm(s[N_WX], x / 48.0, y / 48.0, 2);
    wy = y + 7.0 * fbm(s[N_WY], x / 48.0, y / 48.0, 2);
    rock_limit = 0.17 - 0.12 * perlin(s[N_HARD], x / 300.0, y / 300.0) - 0.30 * hard;
    rock = fbm(s[N_ROCK], wx / 34.0, wy / 34.0, 3);
    *pwx = wx;
    *pwy = wy;
    return rock > rock_limit;
}

/** climate(...)[2] alone: the hardness, computed the same way. */
static double climate_hard(const struct WgWorld *w, int64_t xi, int64_t yi)
{
    const uint64_t *s = w->s_noise;
    double x = (double)xi, y = (double)yi, bx, by, d, f;
    int64_t dx = xi - START_X, dy = yi - START_Y;
    struct U128 dd = add_128(mul_64x64((uint64_t)abs64(dx), (uint64_t)abs64(dx)),
                             mul_64x64((uint64_t)abs64(dy), (uint64_t)abs64(dy)));
    d = sqrt(u128_to_double(dd));
    f = (d - 48.0) / 144.0;
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;
    if (f == 0.0)
        return 0.0;
    bx = x + 24.0 * fbm(s[N_BWX], x / 80.0, y / 80.0, 2);
    by = y + 24.0 * fbm(s[N_BWY], x / 80.0, y / 80.0, 2);
    return fbm(s[N_HARDC], bx / 340.0, by / 340.0, 2) * f;
}

/** worldgen.World.is_rock: impenetrable rock in the terrain (before features and structures)? */
static int is_rock(const struct WgWorld *w, int64_t x, int64_t y)
{
    double wx, wy;
    return shape_rock(w, x, y, clamp_hard(climate_hard(w, x, y)), &wx, &wy);
}

/** worldgen.World.terrain_base(x, y, clim): stages 1 and 2, without gold and structures. */
static uint8_t terrain_base(const struct WgWorld *w, int64_t xi, int64_t yi, const double *clim)
{
    const uint64_t *s = w->s_noise;
    double x = (double)xi, y = (double)yi;
    double heat = clim[0], moist = clim[1], hard = clamp_hard(clim[2]), hot, cold, wet, dry;
    double wx, wy, cave_limit, lava_limit, lava, water_limit, water;
    int cave;
    hot = climate_part(heat);
    cold = climate_part(-heat);
    wet = climate_part(moist);
    dry = climate_part(-moist);
    /* stage 1: terrain shape */
    if (shape_rock(w, xi, yi, hard, &wx, &wy))
        return WgS_Rock;
    /* stage 2: caves and liquids */
    cave_limit = 0.40 - 0.08 * perlin(s[N_CAVEM], x / 200.0, y / 200.0) - 0.25 * wet + 0.20 * cold;
    cave = fbm(s[N_CAVE], wx / 15.0, wy / 15.0, 2) > cave_limit;
    if (!cave && fabs(perlin(s[N_TUN], wx / 38.0, wy / 38.0)) < 0.025 &&
        perlin(s[N_TUNM], x / 110.0, y / 110.0) > 0.4)
        cave = 1;
    lava_limit = 0.40 - 0.35 * hot + 0.80 * cold + 0.50 * wet;
    if (lava_limit < 0.34)
        lava_limit = 0.34;
    lava = fbm(s[N_LAVA], x / 64.0, y / 64.0, 2);
    if (lava > lava_limit || (cave && lava > py_max(lava_limit - 0.2, 0.24)))
        return WgS_Lava;
    if (fabs(perlin(s[N_LRIV], wx / 110.0, wy / 110.0)) < 0.03 + 0.025 * hot &&
        perlin(s[N_LRIVM], x / 220.0, y / 220.0) > 0.3 - 0.6 * hot + 1.0 * cold + 0.8 * wet)
        return WgS_Lava;
    water_limit = 0.38 - 0.30 * wet - 0.12 * cold + 0.80 * hot + 0.40 * dry;
    if (water_limit < 0.31)
        water_limit = 0.31;
    water = fbm(s[N_WATER], x / 72.0, y / 72.0, 2);
    if (water > water_limit || (cave && water > py_max(water_limit - 0.23, 0.2)))
        return WgS_Water;
    if (fabs(perlin(s[N_WRIV], wx / 100.0, wy / 100.0)) < 0.02 + 0.03 * wet &&
        perlin(s[N_WRIVM], x / 200.0, y / 200.0) > 0.55 - 0.9 * wet + 1.0 * hot + 0.6 * dry)
        return WgS_Water;
    if (cave)
        return WgS_Path;
    return WgS_Earth;
}

/******************************************************************************/
/* Generator version 7 (C only while it is tuned; worldgen.py follows)        */
/******************************************************************************/
/* Only +, -, *, /, sqrt and floor on doubles (no libm curves), so a Python port can match it bit for bit. */

static double smooth7(double a, double b, double v)
{
    double t = (v - a) / (b - a);
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

static double unit7(uint64_t h)
{
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

/** The climate of a slab in version 7: the normal biomes' climate (several sizes, shifted by the distance from the
 *  start), and the province or cell biome it is in, if any. */
struct Clim7 {
    double heat, moist, hard, weird; /* the normal biomes' climate, in version 6's ranges */
    double rheat;                    /* the region's heat: lava or water for the provinces and great rivers */
    double d;                        /* distance from the start */
    double wx, wy;                   /* the biome-warped position */
    int base, second;                /* the nearest normal biome, and the next */
    double margin;                   /* how far inside base: score(second) - score(base) */
    int special;                     /* -1, or WgB_LavaOcean .. WgB_DeadMans */
    double strength;                 /* how deep inside the province: 0 at its edge, 1 well inside */
    double divide;                   /* oceans and rivers: near 0 where lava meets water (a strip of land there) */
};

/* each normal biome's distance band: 0 near the start, 1 in the middle distance, 2 far out (idea 6) */
static const int band7[WG_V6_BIOMES] = {0, 0, 2, 1, 1, 0, 2, 2};
#define V7_BAND_PULL 0.030

/** Dead Man's Land: 2% of the 3,200-slab cells hold one, 1,000-2,500 slabs across; a world whose start is in one
 *  (1 seed in 100) has one round it. */
#define V7_DML_CELL 3200
static int dml_inside7(const struct WgWorld *w, int64_t xi, int64_t yi)
{
    const double x = (double)xi, y = (double)yi;
    const double edge = 1.0 + 0.16 * fbm(w->s7[V7_DMLW], x / 260.0, y / 260.0, 2);
    int64_t gx = fdiv(xi, V7_DML_CELL), gy = fdiv(yi, V7_DML_CELL), cx, cy;
    if (w->dml_start) {
        const double dx = x - w->dml_cx, dy = y - w->dml_cy;
        if (dx * dx + dy * dy < (w->dml_r * edge) * (w->dml_r * edge))
            return 1;
    }
    for (cy = gy - 1; cy <= gy + 1; cy++)
        for (cx = gx - 1; cx <= gx + 1; cx++) {
            uint64_t h = hash3(w->s7[V7_DMLCELL], (uint64_t)cx, (uint64_t)cy);
            double r, mx, my, dx, dy;
            if (unit7(h) >= 0.02)
                continue;
            h = mix64(h + WG_GOLDEN);
            r = 500.0 + unit7(h) * 750.0;
            h = mix64(h + WG_GOLDEN);
            mx = (double)(cx * V7_DML_CELL) + 800.0 + unit7(h) * 1600.0;
            h = mix64(h + WG_GOLDEN);
            my = (double)(cy * V7_DML_CELL) + 800.0 + unit7(h) * 1600.0;
            dx = x - mx;
            dy = y - my;
            if (dx * dx + dy * dy < (r * edge) * (r * edge))
                return 1;
        }
    return 0;
}

static void climate7(const struct WgWorld *w, int64_t xi, int64_t yi, struct Clim7 *c)
{
    const uint64_t *s = w->s7;
    const double x = (double)xi, y = (double)yi;
    const double ddx = (double)(xi - START_X), ddy = (double)(yi - START_Y);
    double bx, by, rh, rm, rd, fine, f, near, far, best = 1e30, next = 1e30, ocean, river;
    int i;
    c->d = sqrt(ddx * ddx + ddy * ddy);
    /* a broad warp and version 6's fine one: tongues and bands instead of round blobs (idea 10) */
    bx = x + 110.0 * fbm(s[V7_BWX], x / 460.0, y / 460.0, 2) + 24.0 * fbm(w->s_noise[N_BWX], x / 80.0, y / 80.0, 2);
    by = y + 110.0 * fbm(s[V7_BWY], x / 460.0, y / 460.0, 2) + 24.0 * fbm(w->s_noise[N_BWY], x / 80.0, y / 80.0, 2);
    c->wx = bx;
    c->wy = by;
    /* several sizes (idea 1): regions of 2,000 slabs, the biomes with more detail, small pockets */
    rh = fbm(s[V7_RHEAT], bx / 2200.0 + 0.41, by / 2200.0 + 0.73, 2);
    rm = fbm(s[V7_RMOIST], bx / 2000.0 + 0.67, by / 2000.0 + 0.19, 2);
    rd = fbm(s[V7_RHARD], bx / 1800.0 + 0.23, by / 1800.0 + 0.47, 2);
    fine = fbm(s[V7_FINE], bx / 140.0, by / 140.0, 2);
    c->rheat = rh;
    c->heat = 0.70 * fbm(s[V7_HEAT], bx / 420.0, by / 420.0, 4) + 0.55 * rh + 0.10 * fine;
    c->moist = 0.70 * fbm(s[V7_MOIST], bx / 380.0, by / 380.0, 4) + 0.55 * rm - 0.10 * fine;
    c->hard = 0.70 * fbm(s[V7_HARD], bx / 340.0, by / 340.0, 4) + 0.45 * rd + 0.08 * fine;
    c->weird = fbm(s[V7_WEIRD], bx / 300.0, by / 300.0, 3) + 0.12 * fine;
    /* the start's home is earthen: the full climate from 160 slabs out */
    f = smooth7(40.0, 160.0, c->d);
    c->heat *= f;
    c->moist *= f;
    c->hard *= f;
    c->weird *= f;
    /* the nearest normal biome, pulled toward the ones of this distance (idea 6) */
    near = 1.0 - smooth7(150.0, 500.0, c->d);
    far = smooth7(650.0, 1100.0, c->d);
    c->base = 0;
    c->second = 1;
    for (i = 0; i < WG_V6_BIOMES; i++) {
        const double *p = biome_point[i];
        double t0 = c->heat - p[0], t1 = c->moist - p[1], t2 = c->hard - p[2], t3 = c->weird - p[3];
        double sc = t0 * t0 + t1 * t1 + t2 * t2 + t3 * t3;
        double pull = band7[i] == 0 ? near : (band7[i] == 2 ? far : 1.0 - near - far);
        sc -= V7_BAND_PULL * pull * f;
        if (sc < best) {
            next = best;
            c->second = c->base;
            best = sc;
            c->base = i;
        } else if (sc < next) {
            next = sc;
            c->second = i;
        }
    }
    c->margin = next - best;
    /* the provinces and cells of the new biomes */
    c->special = -1;
    c->divide = 1.0;
    c->strength = 0.0;
    if (dml_inside7(w, xi, yi)) {
        c->special = WgB_DeadMans;
        c->strength = 1.0;
        return;
    }
    /* (off the noise grid: perlin is 0 at whole numbers, which would keep the provinces away from the start) */
    ocean = fbm(s[V7_OCEAN], bx / 2600.0 + 0.37, by / 2600.0 + 0.61, 2);
    river = fbm(s[V7_RIVERP], bx / 2300.0 + 0.53, by / 2300.0 + 0.29, 2);
    if (ocean > 0.30 || river > 0.31) {
        /* lava or water: a broad noise of its own, so a province is mostly one or the other; where they meet, a
           strip of land divides them */
        const double lt = fbm(s[V7_LIQTYPE], bx / 5200.0 + 0.13, by / 5200.0 + 0.91, 2) + 0.4 * rh - 0.05;
        const int lava = lt > 0.0;
        c->divide = lt < 0.0 ? -lt : lt;
        if (ocean > 0.30) {
            c->special = lava ? WgB_LavaOcean : WgB_FloodedOcean;
            c->strength = smooth7(0.30, 0.40, ocean);
        } else {
            c->special = lava ? WgB_LavaRivers : WgB_FloodedRivers;
            c->strength = smooth7(0.31, 0.38, river);
        }
    }
}

/* each normal biome's ground (idea 2) */
struct Profile7 {
    double rock_add;   /* to the rock limit: negative is more rock */
    double cave_add;   /* to the cave limit: negative is more cave */
    double cave_scale; /* the caves' size */
    double tun_thr;    /* tunnels: how wide a band of the tunnel noise */
    double tun_mask;   /* tunnels: how much of the land has them (higher is less) */
    double lava;       /* 1: the biome's own lava lakes and rivers (version 6's) */
    double water;      /* 1: the biome's own lakes and rivers */
    double ribs;       /* 1: thin ribs of rock */
    double grid;       /* 1: straight corridors and square caves */
    double crust;      /* 1: a crust of rock round the lava */
};
static const struct Profile7 profiles7[WG_V6_BIOMES] = {
    /* earthen   */ {0.00, 0.00, 1.0, 0.025, 0.40, 0, 0, 0, 0, 0},
    /* rocky     */ {-0.16, 0.08, 0.8, 0.060, 0.05, 0, 0, 0, 0, 0},
    /* volcanic  */ {-0.03, 0.02, 1.0, 0.025, 0.40, 1, 0, 0, 0, 1},
    /* flooded   */ {0.06, -0.02, 1.2, 0.025, 0.40, 0, 1, 0, 0, 0},
    /* frozen    */ {0.02, 0.10, 2.6, 0.012, 0.60, 0, 1, 0, 0, 0},
    /* sandstone */ {0.20, -0.06, 1.8, 0.040, 0.20, 0, 0, 0, 0, 0},
    /* crystal   */ {0.04, 0.04, 0.7, 0.020, 0.40, 0, 0, 1, 0, 0},
    /* ruins     */ {0.04, 0.10, 1.0, 0.000, 1.00, 0, 0, 0, 1, 0},
};

/** Ruins: corridors along a 13-slab grid (some of each segment) and square caves in some of its cells. */
static int ruins_open7(const struct WgWorld *w, int64_t xi, int64_t yi)
{
    const int64_t g = 13, gx = fdiv(xi, g), gy = fdiv(yi, g), lx = xi - gx * g, ly = yi - gy * g;
    uint64_t h = hash3(w->s7[V7_RUINS], (uint64_t)gx, (uint64_t)gy);
    if (lx <= 1 && unit7(mix64(h ^ 1)) < 0.55)
        return 1; /* a corridor north-south */
    if (ly <= 1 && unit7(mix64(h ^ 2)) < 0.55)
        return 1; /* east-west */
    if (unit7(h) < 0.20) {
        const int64_t x1 = 3 + (int64_t)(unit7(mix64(h ^ 3)) * 3.0), y1 = 3 + (int64_t)(unit7(mix64(h ^ 4)) * 3.0);
        const int64_t x2 = 8 + (int64_t)(unit7(mix64(h ^ 5)) * 3.0), y2 = 8 + (int64_t)(unit7(mix64(h ^ 6)) * 3.0);
        return lx >= x1 && lx <= x2 && ly >= y1 && ly <= y2;
    }
    return 0;
}

/** A giant cavern (idea 7): 12% of the 1,500-slab cells hold an open hall 100-260 slabs across, with pillars. */
static int cavern7(const struct WgWorld *w, int64_t xi, int64_t yi, int *pillar)
{
    const int64_t cell = 1500;
    int64_t gx = fdiv(xi, cell), gy = fdiv(yi, cell), cx, cy;
    for (cy = gy - 1; cy <= gy + 1; cy++)
        for (cx = gx - 1; cx <= gx + 1; cx++) {
            uint64_t h = hash3(w->s7[V7_CAVERN], (uint64_t)cx, (uint64_t)cy);
            double rx, ry, mx, my, dx, dy, e;
            if (unit7(h) >= 0.12)
                continue;
            h = mix64(h + WG_GOLDEN);
            rx = 50.0 + unit7(h) * 80.0;
            h = mix64(h + WG_GOLDEN);
            ry = 40.0 + unit7(h) * 60.0;
            h = mix64(h + WG_GOLDEN);
            mx = (double)(cx * cell) + 200.0 + unit7(h) * 1100.0;
            h = mix64(h + WG_GOLDEN);
            my = (double)(cy * cell) + 200.0 + unit7(h) * 1100.0;
            dx = ((double)xi - mx) / rx;
            dy = ((double)yi - my) / ry;
            e = dx * dx + dy * dy;
            if (e < 1.0 + 0.35 * fbm(w->s7[V7_CAVERN], (double)xi / 24.0, (double)yi / 24.0, 2)) {
                *pillar = fbm(w->s7[V7_PILLAR], (double)xi / 5.0, (double)yi / 5.0, 2) > 0.42;
                return 1;
            }
        }
    return 0;
}

/** The ground of land (normal biomes, islands, the land between rivers): each biome's shape, blended at borders;
 *  dry: no lakes or rivers of its own. */
static uint8_t land7(const struct WgWorld *w, int64_t xi, int64_t yi, const struct Clim7 *c, int dry)
{
    const uint64_t *s = w->s_noise, *s7 = w->s7;
    const double x = (double)xi, y = (double)yi;
    const struct Profile7 *pa = &profiles7[c->base], *pb = &profiles7[c->second];
    double wt = 0.5 + 0.5 * (c->margin / 0.006 > 1.0 ? 1.0 : c->margin / 0.006);
    const double dither = 0.5 + 0.5 * perlin(s7[V7_DITHER], x / 7.0, y / 7.0);
    struct Profile7 p;
    double hard = clamp_hard(c->hard), hot = climate_part(c->heat), cold = climate_part(-c->heat);
    double wet = climate_part(c->moist), dry_c = climate_part(-c->moist);
    double wx, wy, open, cave_limit, cs, lava_limit, lava, water_limit, water;
    int cave;
    p.rock_add = wt * pa->rock_add + (1.0 - wt) * pb->rock_add;
    p.cave_add = wt * pa->cave_add + (1.0 - wt) * pb->cave_add;
    p.cave_scale = wt * pa->cave_scale + (1.0 - wt) * pb->cave_scale;
    p.tun_thr = wt * pa->tun_thr + (1.0 - wt) * pb->tun_thr;
    p.tun_mask = wt * pa->tun_mask + (1.0 - wt) * pb->tun_mask;
    p.lava = wt * pa->lava + (1.0 - wt) * pb->lava;
    p.water = wt * pa->water + (1.0 - wt) * pb->water;
    p.ribs = wt * pa->ribs + (1.0 - wt) * pb->ribs;
    p.grid = wt * pa->grid + (1.0 - wt) * pb->grid;
    p.crust = wt * pa->crust + (1.0 - wt) * pb->crust;
    /* rock (version 6's shape, the biome's amount) */
    wx = x + 7.0 * fbm(s[N_WX], x / 48.0, y / 48.0, 2);
    wy = y + 7.0 * fbm(s[N_WY], x / 48.0, y / 48.0, 2);
    if (fbm(s[N_ROCK], wx / 34.0, wy / 34.0, 3) > 0.17 - 0.12 * perlin(s[N_HARD], x / 300.0, y / 300.0) - 0.30 * hard + p.rock_add)
        return WgS_Rock;
    if (p.ribs > dither && fabs(perlin(s7[V7_RIB], wx / 40.0, wy / 40.0)) < 0.022)
        return WgS_Rock;
    /* ridges along the borders between biomes, with passes (idea 3) */
    if (c->d > 140.0 && c->margin < 0.0035 * (0.6 + 0.4 * perlin(s7[V7_RIDGE], x / 60.0, y / 60.0)) &&
            perlin(s7[V7_RIDGEON], x / 520.0, y / 520.0) > -0.15 && perlin(s7[V7_RIDGEP], x / 40.0, y / 40.0) < 0.38)
        return WgS_Rock;
    /* caves: the biome's size and amount, and open or solid stretches (idea 9) */
    open = fbm(s7[V7_OPEN], x / 650.0, y / 650.0, 2);
    open = 0.09 * smooth7(0.22, 0.50, open) - 0.07 * smooth7(0.22, 0.50, -open);
    cave_limit = 0.40 - 0.08 * perlin(s[N_CAVEM], x / 200.0, y / 200.0) - 0.25 * wet + 0.20 * cold + p.cave_add - open;
    cs = 15.0 * p.cave_scale;
    cave = fbm(s[N_CAVE], wx / cs, wy / cs, 2) > cave_limit;
    if (!cave && p.tun_thr > 0.0 && fabs(perlin(s[N_TUN], wx / 38.0, wy / 38.0)) < p.tun_thr &&
            perlin(s[N_TUNM], x / 110.0, y / 110.0) > p.tun_mask)
        cave = 1;
    if (!cave && p.grid > dither && ruins_open7(w, xi, yi))
        cave = 1;
    if (!dry) {
        /* lava: the hot biomes' own, elsewhere a rare ragged lake or a thin river (idea 5) */
        if (p.lava > dither) {
            lava_limit = 0.40 - 0.35 * hot + 0.80 * cold + 0.50 * wet;
            if (lava_limit < 0.34)
                lava_limit = 0.34;
            lava = fbm(s[N_LAVA], x / 64.0, y / 64.0, 2);
            if (lava > lava_limit || (cave && lava > py_max(lava_limit - 0.2, 0.24)))
                return WgS_Lava;
            if (p.crust > dither && lava > lava_limit - 0.045)
                return WgS_Rock;
            if (fabs(perlin(s[N_LRIV], wx / 110.0, wy / 110.0)) < 0.03 + 0.025 * hot &&
                    perlin(s[N_LRIVM], x / 220.0, y / 220.0) > 0.3 - 0.6 * hot + 1.0 * cold + 0.8 * wet)
                return WgS_Lava;
        } else {
            if (perlin(s7[V7_LLAKEM], x / 520.0, y / 520.0) > 0.50 && fbm(s7[V7_LLAKE], wx / 36.0, wy / 36.0, 4) > 0.20)
                return WgS_Lava;
            if (fabs(perlin(s7[V7_LRIV], wx / 95.0, wy / 95.0)) < 0.010 && perlin(s7[V7_LRIVM], x / 420.0, y / 420.0) > 0.52)
                return WgS_Lava;
        }
        /* water: the wet and cold biomes' own, elsewhere ragged lakes and thin rivers */
        if (p.water > dither) {
            water_limit = 0.38 - 0.30 * wet - 0.12 * cold + 0.80 * hot + 0.40 * dry_c;
            if (water_limit < 0.31)
                water_limit = 0.31;
            water = fbm(s[N_WATER], x / 72.0, y / 72.0, 2);
            if (water > water_limit || (cave && water > py_max(water_limit - 0.23, 0.2)))
                return WgS_Water;
            if (fabs(perlin(s[N_WRIV], wx / 100.0, wy / 100.0)) < 0.02 + 0.03 * wet &&
                    perlin(s[N_WRIVM], x / 200.0, y / 200.0) > 0.55 - 0.9 * wet + 1.0 * hot + 0.6 * dry_c)
                return WgS_Water;
        } else {
            if (perlin(s7[V7_WLAKEM], x / 480.0, y / 480.0) > 0.40 && fbm(s7[V7_WLAKE], wx / 32.0, wy / 32.0, 4) > 0.18)
                return WgS_Water;
            if (fabs(perlin(s7[V7_WRIV], wx / 100.0, wy / 100.0)) < 0.012 && perlin(s7[V7_WRIVM], x / 400.0, y / 400.0) > 0.45)
                return WgS_Water;
        }
    }
    return cave ? WgS_Path : WgS_Earth;
}

/** Version 7's ground before the gold seams. */
static uint8_t ground7(const struct WgWorld *w, int64_t xi, int64_t yi, const struct Clim7 *c)
{
    const uint64_t *s7 = w->s7;
    const double x = (double)xi, y = (double)yi;
    int pillar = 0;
    if (c->special == WgB_DeadMans)
        return WgS_Earth; /* nothing: plain diggable earth */
    /* landmarks across the biomes (idea 7): mountain ranges with passes, great rivers with fords, giant caverns */
    if (c->d > 120.0 && c->special != WgB_LavaOcean && c->special != WgB_FloodedOcean &&
            perlin(s7[V7_MOUNTM], x / 3000.0, y / 3000.0) > -0.1 &&
            fabs(perlin(s7[V7_MOUNT], c->wx / 1300.0, c->wy / 1300.0)) < 0.0065 + 0.0035 * perlin(s7[V7_MOUNTW], x / 220.0, y / 220.0) &&
            perlin(s7[V7_PASS], x / 70.0, y / 70.0) < 0.40)
        return WgS_Rock;
    if (c->d > 90.0 && perlin(s7[V7_GRIVERM], x / 3200.0, y / 3200.0) > -0.25 &&
            fabs(perlin(s7[V7_GRIVER], c->wx / 1700.0, c->wy / 1700.0)) < 0.0042 &&
            perlin(s7[V7_GFORD], x / 26.0, y / 26.0) < 0.5)
        return c->rheat > 0.12 ? WgS_Lava : WgS_Water;
    if (c->d > 200.0 && cavern7(w, xi, yi, &pillar))
        return pillar ? WgS_Rock : WgS_Path;
    if (c->special == WgB_LavaOcean || c->special == WgB_FloodedOcean) {
        /* islands of 200-500 slabs and islets; more land near the coast; the start always has its island */
        double isl = fbm(s7[V7_ISLAND], c->wx / 520.0, c->wy / 520.0, 3) + 0.30 * fbm(s7[V7_ISLET], c->wx / 70.0, c->wy / 70.0, 2);
        /* islands everywhere, a little more land along the coast */
        double t = 0.06 - 0.10 * (1.0 - c->strength);
        if (c->d < 95.0)
            t -= (95.0 - c->d) / 95.0 * 0.6;
        if (c->divide < 0.012)
            t -= 1.0; /* the land between lava and water */
        if (isl < t) {
            /* near shores and the border: islands of 15-80 slabs, many where the coast is an archipelago, none
               along some stretches of open water */
            const double edge = 1.0 - smooth7(0.0, 0.65, c->strength); /* the province border */
            const double shore = 1.0 - smooth7(0.0, 0.32, t - isl);       /* close to any island or coast */
            const double coast = edge > shore ? edge : shore;
            const double arch = smooth7(-0.3, 0.1, perlin(s7[V7_ARCH], x / 650.0 + 0.17, y / 650.0 + 0.83));
            if (coast * arch > 0.0 && 0.5 * fbm(s7[V7_ISLET2], c->wx / 60.0, c->wy / 60.0, 3) +
                    0.5 * fbm(s7[V7_ISLET3], c->wx / 180.0, c->wy / 180.0, 2) > 0.26 - 0.32 * coast * arch)
                return land7(w, xi, yi, c, 1);
            return c->special == WgB_LavaOcean ? WgS_Lava : WgS_Water;
        }
        return land7(w, xi, yi, c, 1);
    }
    if (c->special == WgB_LavaRivers || c->special == WgB_FloodedRivers) {
        /* braided rivers 3-8 slabs wide that split, rejoin and turn, with fords; none in the start's home */
        const double k = 0.4 + 0.6 * c->strength;
        int river = fabs(perlin(s7[V7_RIV1], c->wx / 160.0, c->wy / 160.0)) < 0.026 * k ||
            fabs(perlin(s7[V7_RIV2], c->wx / 115.0, c->wy / 115.0)) < 0.026 * k ||
            (fabs(perlin(s7[V7_RIV3], c->wx / 240.0, c->wy / 240.0)) < 0.018 * k && perlin(s7[V7_RIVM], x / 400.0, y / 400.0) > -0.2);
        if (river && c->d >= 45.0 && c->divide >= 0.012 && perlin(s7[V7_FORD], x / 13.0, y / 13.0) < 0.48)
            return c->special == WgB_LavaRivers ? WgS_Lava : WgS_Water;
        return land7(w, xi, yi, c, 1);
    }
    return land7(w, xi, yi, c, 0);
}

static int biome7_of(const struct Clim7 *c)
{
    return c->special >= 0 ? c->special : c->base;
}

static int is_rock7(const struct WgWorld *w, int64_t x, int64_t y)
{
    struct Clim7 c;
    climate7(w, x, y, &c);
    return ground7(w, x, y, &c) == WgS_Rock;
}

/** Version 7: does a structure of this type fit here? Never in Dead Man's Land; in an ocean or a river province
 *  only on land, with room round it to build (idea: any structure, as long as it fits on its island). */
static int fits7(const struct WgWorld *w, int type, int64_t ox, int64_t oy)
{
    const int64_t r = specs[type].radius + (type == WgT_Keeper ? 8 : 2);
    int i, j;
    for (j = 0; j <= 6; j++)
        for (i = 0; i <= 6; i++) {
            const int64_t x = ox - r + (2 * r * i) / 6, y = oy - r + (2 * r * j) / 6;
            struct Clim7 c;
            uint8_t g;
            climate7(w, x, y, &c);
            if (c.special == WgB_DeadMans)
                return 0;
            if (c.special < 0)
                continue;
            g = ground7(w, x, y, &c);
            if (g == WgS_Lava || g == WgS_Water)
                return 0;
        }
    return 1;
}

/* the version switches: version 6's own code, unchanged, or version 7's */
static uint8_t ground_base_v(const struct WgWorld *w, int64_t x, int64_t y)
{
    if (w->version >= 7) {
        struct Clim7 c;
        climate7(w, x, y, &c);
        return ground7(w, x, y, &c);
    } else {
        double clim[4];
        climate(w, x, y, clim);
        return terrain_base(w, x, y, clim);
    }
}

static int is_rock_v(const struct WgWorld *w, int64_t x, int64_t y)
{
    return w->version >= 7 ? is_rock7(w, x, y) : is_rock(w, x, y);
}

static int biome_at_v(const struct WgWorld *w, int64_t x, int64_t y)
{
    if (w->version >= 7) {
        struct Clim7 c;
        climate7(w, x, y, &c);
        return biome7_of(&c);
    }
    return biome_at(w, x, y);
}

/** The reachability flood's set: the slot of (x, y), claimed (st 0 -> caller sets it) when it is new. */
static uint8_t *reach_slot(struct ReachSet *r, int64_t x, int64_t y)
{
    uint32_t i = (uint32_t)(mix64(((uint64_t)x * WG_P1) ^ ((uint64_t)y * WG_P2)) >> 40) & (REACH_SET_CAP - 1);
    while (r->st[i] != 0 && (r->x[i] != x || r->y[i] != y))
        i = (i + 1) & (REACH_SET_CAP - 1);
    if (r->st[i] == 0) {
        r->x[i] = x;
        r->y[i] = y;
    }
    return &r->st[i];
}

/** worldgen.World.reachable: open ground (not rock) that rock doesn't seal off: a flood through open ground from
 *  it reaches REACH_CELLS slabs. */
static int reachable(const struct WgWorld *w, int64_t x, int64_t y)
{
    static const int nx4[4] = {1, -1, 0, 0}, ny4[4] = {0, 0, 1, -1};
    struct ReachSet *r = w->reach;
    int sp = 0, seen = 1, d;
    if (is_rock_v(w, x, y))
        return 0;
    memset(r->st, 0, sizeof r->st);
    *reach_slot(r, x, y) = 3;
    r->stack[sp][0] = x;
    r->stack[sp][1] = y;
    sp++;
    while (sp > 0 && seen < REACH_CELLS) {
        int64_t px, py;
        sp--;
        px = r->stack[sp][0];
        py = r->stack[sp][1];
        for (d = 0; d < 4; d++) {
            int64_t qx = px + nx4[d], qy = py + ny4[d];
            uint8_t *st = reach_slot(r, qx, qy);
            if (*st == 3)
                continue;
            if (*st == 0)
                *st = is_rock_v(w, qx, qy) ? 1 : 2;
            if (*st == 1)
                continue;
            *st = 3;
            seen++;
            r->stack[sp][0] = qx;
            r->stack[sp][1] = qy;
            sp++;
        }
    }
    return seen >= REACH_CELLS;
}

/** One lobe of a gold seam: worldgen.World._seam_lobe with its centre. */
struct SeamLobe {
    int64_t sx, sy;
    double a, b, ux, uy, bend;
};

/** worldgen.World._seam_lobe: half width, a direction (a normalised integer vector: no trigonometry) and a bend. */
static void seam_lobe(struct Rng *rng, double a, struct SeamLobe *l)
{
    int64_t dx, dy;
    double n;
    l->a = a;
    l->b = a / (SEAM_ASPECT_MIN + rng_random(rng) * SEAM_ASPECT_EXTRA);
    if (l->b < SEAM_MIN_HALF_WIDTH)
        l->b = SEAM_MIN_HALF_WIDTH;
    for (;;) {
        dx = rng_randint(rng, 0, 16) - 8;
        dy = rng_randint(rng, 0, 16) - 8;
        if (dx * dx + dy * dy >= 16)
            break;
    }
    n = sqrt((double)(dx * dx + dy * dy));
    l->bend = (rng_random(rng) - 0.5) * 1.2;
    l->ux = (double)dx / n;
    l->uy = (double)dy / n;
}

/** worldgen.World._seam_cells: the seam's shape (lumpy bent ellipses) minus caves, the start's gold-free square and
 *  the pieces you can't reach, into e (x0, y0, w, h, cells). */
static void seam_cells(const struct WgWorld *w, const struct SeamLobe *lobes, int nl, double rich, int dense,
                       uint64_t ws, struct SeamEntry *e)
{
    static const int nx4[4] = {1, -1, 0, 0}, ny4[4] = {0, 0, 1, -1};
    int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, x, y;
    int k, W, H, i, sp;
    uint8_t *shape, *base;
    int32_t *stack, *piece;
    for (k = 0; k < nl; k++) {
        int64_t r = (int64_t)(lobes[k].a * rich) + 5;
        if (k == 0 || lobes[k].sx - r < x0) x0 = lobes[k].sx - r;
        if (k == 0 || lobes[k].sy - r < y0) y0 = lobes[k].sy - r;
        if (k == 0 || lobes[k].sx + r > x1) x1 = lobes[k].sx + r;
        if (k == 0 || lobes[k].sy + r > y1) y1 = lobes[k].sy + r;
    }
    W = (int)(x1 - x0 + 1);
    H = (int)(y1 - y0 + 1);
    shape = (uint8_t *)calloc((size_t)W * H, 1);
    base = (uint8_t *)calloc((size_t)W * H, 1);
    stack = (int32_t *)malloc(sizeof(int32_t) * (size_t)W * H);
    piece = (int32_t *)malloc(sizeof(int32_t) * (size_t)W * H);
    if (shape == NULL || base == NULL || stack == NULL || piece == NULL) {
        free(shape);
        free(base);
        free(stack);
        free(piece);
        return;
    }
    for (k = 0; k < nl; k++) {
        const struct SeamLobe *l = &lobes[k];
        double a = l->a * rich, b = l->b * rich;
        int64_t r = (int64_t)a + 5;
        for (y = l->sy - r; y <= l->sy + r; y++)
            for (x = l->sx - r; x <= l->sx + r; x++) {
                int64_t rx = x - l->sx, ry = y - l->sy, hdx = x - START_X, hdy = y - START_Y;
                double u, v, f;
                uint8_t *cell;
                if (fabs((double)rx) > a + 4.0 || fabs((double)ry) > a + 4.0)
                    continue;
                if (max64(abs64(hdx), abs64(hdy)) <= START_GOLD_FREE)
                    continue;
                u = ((double)rx * l->ux + (double)ry * l->uy) / a;
                v = ((double)ry * l->ux - (double)rx * l->uy) / b - l->bend * u * u;
                f = u * u + v * v;
                if (f > 1.0 + 2.0 * SEAM_EDGE)
                    continue;
                if (!(f <= 1.0 + SEAM_EDGE * fbm(ws, (double)x / 4.5, (double)y / 4.5, 2)))
                    continue;
                cell = &shape[(y - y0) * W + (x - x0)];
                if (dense && f <= 0.18 + 0.25 * perlin(ws ^ 0x5DEECE66DULL, (double)x / 2.9, (double)y / 2.9))
                    *cell = WgS_DenseGold;
                else if (*cell == 0)
                    *cell = WgS_Gold;
            }
    }
    /* caves cut seams */
    for (i = 0; i < W * H; i++)
        if (shape[i]) {
            base[i] = ground_base_v(w, x0 + i % W, y0 + i / W);
            /* version 7: no gold in lava or water either */
            if (base[i] == WgS_Path || (w->version >= 7 && (base[i] == WgS_Lava || base[i] == WgS_Water)))
                shape[i] = 0;
        }
    /* the pieces (4-connected): kept if one of their slabs, or an open slab next to them, is reachable */
    for (i = 0; i < W * H; i++) {
        int n = 0, ok = 0, j, d;
        if (shape[i] == 0 || (shape[i] & 0x80))
            continue;
        sp = 0;
        stack[sp++] = i;
        shape[i] |= 0x80;
        while (sp > 0) {
            int c = stack[--sp], cx = c % W, cy = c / W;
            piece[n++] = c;
            for (d = 0; d < 4; d++) {
                int qx = cx + nx4[d], qy = cy + ny4[d], q;
                if (qx < 0 || qy < 0 || qx >= W || qy >= H)
                    continue;
                q = qy * W + qx;
                if (shape[q] != 0 && !(shape[q] & 0x80)) {
                    shape[q] |= 0x80;
                    stack[sp++] = q;
                }
            }
        }
        for (j = 0; j < n && !ok; j++)
            if (base[piece[j]] != WgS_Rock && reachable(w, x0 + piece[j] % W, y0 + piece[j] / W))
                ok = 1;
        for (j = 0; j < n && !ok; j++)
            for (d = 0; d < 4 && !ok; d++) {
                int qx = piece[j] % W + nx4[d], qy = piece[j] / W + ny4[d];
                if (qx >= 0 && qy >= 0 && qx < W && qy < H && shape[qy * W + qx] != 0)
                    continue; /* in the seam */
                if (!is_rock_v(w, x0 + qx, y0 + qy) && reachable(w, x0 + qx, y0 + qy))
                    ok = 1;
            }
        for (j = 0; j < n; j++)
            shape[piece[j]] = ok ? (uint8_t)(shape[piece[j]] | 0x40) : (uint8_t)0x80;
    }
    for (i = 0; i < W * H; i++)
        shape[i] = (shape[i] & 0x40) ? (uint8_t)(shape[i] & 0x3F) : (uint8_t)0;
    free(base);
    free(stack);
    free(piece);
    e->x0 = x0;
    e->y0 = y0;
    e->w = W;
    e->h = H;
    e->cells = shape;
}

/* version 7: each biome's seams (idea 4): chance and richness multipliers, the chance of a dense core */
static const double seam7_biome[WgB_COUNT][3] = {
    {1.00, 1.0, 0.35}, /* earthen */
    {1.00, 0.9, 0.60}, /* rocky: dense pockets */
    {1.20, 1.0, 0.40}, /* volcanic */
    {0.60, 0.9, 0.30}, /* flooded: little */
    {0.80, 1.0, 0.30}, /* frozen */
    {1.25, 1.4, 0.35}, /* sandstone: long rich veins */
    {1.60, 1.2, 0.60}, /* crystal: more seams and dense gold */
    {1.00, 1.0, 0.35}, /* ruins */
    {1.10, 1.1, 0.40}, /* lava ocean: on the islands */
    {1.10, 1.1, 0.40}, /* flooded ocean: on the islands */
    {1.00, 1.0, 0.35}, /* lava rivers */
    {1.00, 1.0, 0.35}, /* flooded rivers */
    {0.00, 0.0, 0.00}, /* Dead Man's Land: nothing */
};

/* version 7's gold: deposits drawn from a library of shapes (circles, ovals, polygons of 3 to 10 sides, stars,
 * crosses, arrows, crescents, hearts, rings, ticks, ...), each stretched, turned, skewed, bent and lumpy, now and then
 * with limbs; some are version 6's lenses or clusters of nuggets, a few river-like veins. Dense gold as a core, a rim,
 * patches, or all of a small one. A deposit stays within SEAM7_REACH slabs of its square (seam_gold looks 2 squares
 * out in version 7). */
#define SEAM7_REACH 46

/* regular polygons on the unit circle, the first corner at the top (math.cos/math.sin, bit for bit) */
static const double poly7_3[3][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.bb67ae8584cabp-1, 0x1.ffffffffffffdp-2}, {-0x1.bb67ae8584ca9p-1, 0x1.0000000000003p-1}};
static const double poly7_4[4][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.0000000000000p+0, 0x0.0p+0}, {0x1.1a62633145c07p-54, 0x1.0000000000000p+0}, {-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}};
static const double poly7_5[5][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.e6f0e134454ffp-1, -0x1.3c6ef372fe94fp-2}, {0x1.2cf2304755a5ep-1, 0x1.9e3779b97f4a8p-1}, {-0x1.2cf2304755a5dp-1, 0x1.9e3779b97f4a8p-1}, {-0x1.e6f0e13445500p-1, -0x1.3c6ef372fe94dp-2}};
static const double poly7_6[6][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.bb67ae8584caap-1, -0x1.0000000000000p-1}, {0x1.bb67ae8584cabp-1, 0x1.ffffffffffffdp-2}, {0x1.1a62633145c07p-54, 0x1.0000000000000p+0}, {-0x1.bb67ae8584ca9p-1, 0x1.0000000000003p-1}, {-0x1.bb67ae8584caap-1, -0x1.0000000000001p-1}};
static const double poly7_7[7][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.904c37505de4bp-1, -0x1.3f3a0e28bedd1p-1}, {0x1.f329c0558e969p-1, 0x1.c7b90e3024582p-3}, {0x1.bc4c04d71abc2p-2, 0x1.cd4bca9cb5c71p-1}, {-0x1.bc4c04d71abc0p-2, 0x1.cd4bca9cb5c71p-1}, {-0x1.f329c0558e969p-1, 0x1.c7b90e3024586p-3}, {-0x1.904c37505de4cp-1, -0x1.3f3a0e28bedd0p-1}};
static const double poly7_8[8][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.6a09e667f3bcdp-1, -0x1.6a09e667f3bccp-1}, {0x1.0000000000000p+0, 0x0.0p+0}, {0x1.6a09e667f3bcdp-1, 0x1.6a09e667f3bccp-1}, {0x1.1a62633145c07p-54, 0x1.0000000000000p+0}, {-0x1.6a09e667f3bccp-1, 0x1.6a09e667f3bcdp-1}, {-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {-0x1.6a09e667f3bcep-1, -0x1.6a09e667f3bccp-1}};
static const double poly7_9[9][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.491b7523c161dp-1, -0x1.8836fa2cf5039p-1}, {0x1.f838b8c811c17p-1, -0x1.63a1a7e0b738ap-3}, {0x1.bb67ae8584cabp-1, 0x1.ffffffffffffdp-2}, {0x1.5e3a8748a0bf7p-2, 0x1.e11f642522d1bp-1}, {-0x1.5e3a8748a0bf5p-2, 0x1.e11f642522d1cp-1}, {-0x1.bb67ae8584ca9p-1, 0x1.0000000000003p-1}, {-0x1.f838b8c811c18p-1, -0x1.63a1a7e0b737ep-3}, {-0x1.491b7523c161ep-1, -0x1.8836fa2cf5038p-1}};
static const double poly7_10[10][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.2cf2304755a5ep-1, -0x1.9e3779b97f4a8p-1}, {0x1.e6f0e134454ffp-1, -0x1.3c6ef372fe94fp-2}, {0x1.e6f0e134454ffp-1, 0x1.3c6ef372fe94fp-2}, {0x1.2cf2304755a5ep-1, 0x1.9e3779b97f4a8p-1}, {0x1.1a62633145c07p-54, 0x1.0000000000000p+0}, {-0x1.2cf2304755a5dp-1, 0x1.9e3779b97f4a8p-1}, {-0x1.e6f0e134454ffp-1, 0x1.3c6ef372fe951p-2}, {-0x1.e6f0e13445500p-1, -0x1.3c6ef372fe94dp-2}, {-0x1.2cf2304755a5fp-1, -0x1.9e3779b97f4a7p-1}};
static const double star7[10][2] = {{0x1.1a62633145c07p-54, -0x1.0000000000000p+0}, {0x1.0ed9f840337bbp-2, -0x1.74cb8726f28fep-2}, {0x1.e6f0e134454ffp-1, -0x1.3c6ef372fe94fp-2}, {0x1.b63f31157194cp-2, 0x1.1cca41811852ep-3}, {0x1.2cf2304755a5ep-1, 0x1.9e3779b97f4a8p-1}, {0x1.fc4ab28be3f40p-56, 0x1.ccccccccccccdp-2}, {-0x1.2cf2304755a5dp-1, 0x1.9e3779b97f4a8p-1}, {-0x1.b63f31157194cp-2, 0x1.1cca41811852fp-3}, {-0x1.e6f0e13445500p-1, -0x1.3c6ef372fe94dp-2}, {-0x1.0ed9f840337bcp-2, -0x1.74cb8726f28fdp-2}};

struct Grid7 {
    int64_t x0, y0;
    int W, H;
    uint8_t *c;
    uint64_t ws; /* the lumps */
};

static void g7_set(struct Grid7 *g, int64_t x, int64_t y, int dense)
{
    uint8_t *cell;
    if (x < g->x0 || y < g->y0 || x >= g->x0 + g->W || y >= g->y0 + g->H)
        return;
    if (max64(abs64(x - START_X), abs64(y - START_Y)) <= START_GOLD_FREE)
        return;
    cell = &g->c[(y - g->y0) * g->W + (x - g->x0)];
    if (dense)
        *cell = WgS_DenseGold;
    else if (*cell == 0)
        *cell = WgS_Gold;
}

static void g7_dir(struct Rng *rng, double *ux, double *uy)
{
    int64_t dx, dy;
    double n;
    for (;;) {
        dx = rng_randint(rng, 0, 16) - 8;
        dy = rng_randint(rng, 0, 16) - 8;
        if (dx * dx + dy * dy >= 16)
            break;
    }
    n = sqrt((double)(dx * dx + dy * dy));
    *ux = (double)dx / n;
    *uy = (double)dy / n;
}

/** A lumpy disc. */
static void g7_disc(struct Grid7 *g, double cx, double cy, double r, int dense)
{
    int64_t x, y;
    for (y = (int64_t)floor(cy - r - 2.0); y <= (int64_t)floor(cy + r + 2.0); y++)
        for (x = (int64_t)floor(cx - r - 2.0); x <= (int64_t)floor(cx + r + 2.0); x++) {
            const double dx = (double)x + 0.5 - cx, dy = (double)y + 0.5 - cy;
            const double rr = r + 0.6 * fbm(g->ws, (double)x / 4.5, (double)y / 4.5, 2);
            if (rr > 0.0 && dx * dx + dy * dy <= rr * rr)
                g7_set(g, x, y, dense);
        }
}

/** A stroke from a to b whose radius goes from ra to rb (to a point when it gets small), lumpy. */
static void g7_stroke(struct Grid7 *g, double ax, double ay, double bx, double by, double ra, double rb, int dense)
{
    const double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    const double rm = (ra > rb ? ra : rb) + 1.5;
    const double lo_x = ax < bx ? ax : bx, hi_x = ax < bx ? bx : ax, lo_y = ay < by ? ay : by, hi_y = ay < by ? by : ay;
    int64_t x, y;
    for (y = (int64_t)floor(lo_y - rm); y <= (int64_t)floor(hi_y + rm); y++)
        for (x = (int64_t)floor(lo_x - rm); x <= (int64_t)floor(hi_x + rm); x++) {
            const double px = (double)x + 0.5 - ax, py = (double)y + 0.5 - ay;
            double t = l2 > 0.0 ? (px * vx + py * vy) / l2 : 0.0, dx, dy, r;
            if (t < 0.0)
                t = 0.0;
            if (t > 1.0)
                t = 1.0;
            dx = px - t * vx;
            dy = py - t * vy;
            r = ra + (rb - ra) * t + 0.45 * fbm(g->ws, (double)x / 4.0, (double)y / 4.0, 2);
            if (r > 0.0 && dx * dx + dy * dy <= r * r)
                g7_set(g, x, y, dense);
        }
}

/** A bent path: steps of `step` slabs from (x, y) along (ux, uy), turning a little at each, the radius going from r0
 *  to r1; with branch > 0, now and then a side branch (that can't branch again). */
static void g7_path(struct Grid7 *g, struct Rng *rng, double x, double y, double ux, double uy, double len, double step,
                    double r0, double r1, double wiggle, int dense, int branch)
{
    const int n = (int)(len / step) + 1;
    int i;
    for (i = 0; i < n; i++) {
        const double t0 = (double)i / (double)n, t1 = (double)(i + 1) / (double)n;
        const double ra = r0 + (r1 - r0) * t0, rb = r0 + (r1 - r0) * t1;
        double k = (rng_random(rng) - 0.5) * wiggle, nx, ny, nn, x2, y2;
        nx = ux - uy * k;
        ny = uy + ux * k;
        nn = sqrt(nx * nx + ny * ny);
        ux = nx / nn;
        uy = ny / nn;
        x2 = x + ux * step;
        y2 = y + uy * step;
        g7_stroke(g, x, y, x2, y2, ra, rb, dense && rng_random(rng) < 0.12);
        if (branch > 0 && i > 0 && i < n - 2 && rng_random(rng) < 0.10) {
            /* a side branch, off to one side and onward */
            const double side = rng_random(rng) < 0.5 ? -1.0 : 1.0;
            double sx = ux * 0.45 - uy * side, sy = uy * 0.45 + ux * side, sn = sqrt(sx * sx + sy * sy);
            g7_path(g, rng, x2, y2, sx / sn, sy / sn, len * (0.25 + 0.35 * rng_random(rng)), step, rb * 0.8, 0.3, wiggle,
                dense, 0);
        }
        x = x2;
        y = y2;
    }
}

/** Version 7's post-processing of a seam's shape (as version 6's seam_cells): caves, lava and water cut it, only the
 *  pieces you can reach stay; into e, cropped to its cells. */
static void seam_finish7(const struct WgWorld *w, struct Grid7 *g, struct SeamEntry *e)
{
    static const int nx4[4] = {1, -1, 0, 0}, ny4[4] = {0, 0, 1, -1};
    const int W = g->W, H = g->H;
    uint8_t *shape = g->c, *base = (uint8_t *)calloc((size_t)W * H, 1);
    int32_t *stack = (int32_t *)malloc(sizeof(int32_t) * (size_t)W * H);
    int32_t *piece = (int32_t *)malloc(sizeof(int32_t) * (size_t)W * H);
    int i, sp, minx = W, miny = H, maxx = -1, maxy = -1, x, y;
    if (base == NULL || stack == NULL || piece == NULL) {
        free(base);
        free(stack);
        free(piece);
        free(shape);
        return;
    }
    for (i = 0; i < W * H; i++)
        if (shape[i]) {
            base[i] = ground_base_v(w, g->x0 + i % W, g->y0 + i / W);
            if (base[i] == WgS_Path || base[i] == WgS_Lava || base[i] == WgS_Water)
                shape[i] = 0;
        }
    for (i = 0; i < W * H; i++) {
        int n = 0, ok = 0, j, d;
        if (shape[i] == 0 || (shape[i] & 0x80))
            continue;
        sp = 0;
        stack[sp++] = i;
        shape[i] |= 0x80;
        while (sp > 0) {
            int c = stack[--sp], cx = c % W, cy = c / W;
            piece[n++] = c;
            for (d = 0; d < 4; d++) {
                int qx = cx + nx4[d], qy = cy + ny4[d], q;
                if (qx < 0 || qy < 0 || qx >= W || qy >= H)
                    continue;
                q = qy * W + qx;
                if (shape[q] != 0 && !(shape[q] & 0x80)) {
                    shape[q] |= 0x80;
                    stack[sp++] = q;
                }
            }
        }
        for (j = 0; j < n && !ok; j++)
            if (base[piece[j]] != WgS_Rock && reachable(w, g->x0 + piece[j] % W, g->y0 + piece[j] / W))
                ok = 1;
        for (j = 0; j < n && !ok; j++)
            for (d = 0; d < 4 && !ok; d++) {
                int qx = piece[j] % W + nx4[d], qy = piece[j] / W + ny4[d];
                if (qx >= 0 && qy >= 0 && qx < W && qy < H && shape[qy * W + qx] != 0)
                    continue;
                if (!is_rock_v(w, g->x0 + qx, g->y0 + qy) && reachable(w, g->x0 + qx, g->y0 + qy))
                    ok = 1;
            }
        for (j = 0; j < n; j++)
            shape[piece[j]] = ok ? (uint8_t)(shape[piece[j]] | 0x40) : (uint8_t)0x80;
    }
    for (i = 0; i < W * H; i++) {
        shape[i] = (shape[i] & 0x40) ? (uint8_t)(shape[i] & 0x3F) : (uint8_t)0;
        if (shape[i]) {
            x = i % W;
            y = i / W;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    free(base);
    free(stack);
    free(piece);
    if (maxx < 0) {
        free(shape);
        return; /* nothing left */
    }
    e->w = maxx - minx + 1;
    e->h = maxy - miny + 1;
    e->x0 = g->x0 + minx;
    e->y0 = g->y0 + miny;
    e->cells = (uint8_t *)malloc((size_t)e->w * e->h);
    if (e->cells == NULL) {
        e->w = 0;
        free(shape);
        return;
    }
    for (y = 0; y < e->h; y++)
        memcpy(e->cells + (size_t)y * e->w, shape + (size_t)(miny + y) * W + minx, (size_t)e->w);
    free(shape);
}

/* the other polygons, in unit coordinates (y down) */
static const double arrow7[7][2] = {{-1.0, -0.28}, {0.2, -0.28}, {0.2, -0.7}, {1.0, 0.0}, {0.2, 0.7}, {0.2, 0.28}, {-1.0, 0.28}};
static const double tri_right7[3][2] = {{-0.8, -0.9}, {-0.8, 0.9}, {0.9, 0.9}};
static const double tri_scalene7[3][2] = {{-1.0, 0.7}, {1.0, 0.85}, {-0.35, -0.9}};
static const double tri_iso7[3][2] = {{0.0, -1.0}, {0.55, 1.0}, {-0.55, 1.0}};
static const double para7[4][2] = {{-0.6, -0.6}, {1.0, -0.6}, {0.6, 0.6}, {-1.0, 0.6}};
static const double trap7[4][2] = {{-0.5, -0.6}, {0.5, -0.6}, {1.0, 0.6}, {-1.0, 0.6}};
static const double kite7[4][2] = {{0.0, -1.0}, {0.6, -0.25}, {0.0, 1.0}, {-0.6, -0.25}};
static const double heart_tip7[3][2] = {{-0.98, -0.1}, {0.98, -0.1}, {0.0, 1.0}};

/** Point in polygon (even-odd). */
static int in_poly7(const double (*p)[2], int n, double u, double v)
{
    int i, j, in = 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        const double xi = p[i][0], yi = p[i][1], xj = p[j][0], yj = p[j][1];
        if (((yi > v) != (yj > v)) && (u < (xj - xi) * (v - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

static double seg_d2_7(double u, double v, double ax, double ay, double bx, double by)
{
    const double vx = bx - ax, vy = by - ay, px = u - ax, py = v - ay;
    double t = (px * vx + py * vy) / (vx * vx + vy * vy), dx, dy;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    dx = px - t * vx;
    dy = py - t * vy;
    return dx * dx + dy * dy;
}

/* the shapes (the picture's thirty), and version 6's lens, clusters of nuggets and veins */
enum {
    SK_CIRCLE, SK_OVAL, SK_CRESCENT, SK_RING, SK_SEMI, SK_QUADRANT, SK_PIE, SK_TREFOIL, SK_HEART, SK_CURVI,
    SK_SQUARE, SK_RECT, SK_DIAMOND, SK_PENT, SK_HEX, SK_HEPT, SK_OCT, SK_NON, SK_DEC, SK_ISO, SK_RIGHT, SK_SCALENE,
    SK_EQUI, SK_PARA, SK_TRAP, SK_KITE,
    SK_CROSS, SK_STAR, SK_ARROW, SK_TICK,
    SK_LENS, SK_CLUSTER,
    SK_VEIN,
    SK_COUNT
};
/* their families: round, angular, pointy, organic, vein */
static const int sk7_first[6] = {SK_CIRCLE, SK_SQUARE, SK_CROSS, SK_LENS, SK_VEIN, SK_COUNT};
/* each biome's mix of the families, in percent */
static const int seam7_family[WgB_COUNT][5] = {
    {30, 28, 14, 22, 6},  /* earthen */
    {18, 40, 12, 26, 4},  /* rocky: blocks and pockets */
    {30, 22, 22, 18, 8},  /* volcanic */
    {38, 18, 10, 28, 6},  /* flooded: rounded */
    {16, 34, 34, 12, 4},  /* frozen: shards and points */
    {36, 20, 10, 22, 12}, /* sandstone: rounded, more veins */
    {10, 44, 38, 6, 2},   /* crystal: facets and points */
    {14, 52, 14, 16, 4},  /* ruins: blocks */
    {30, 28, 14, 22, 6},  /* lava ocean */
    {30, 28, 14, 22, 6},  /* flooded ocean */
    {26, 24, 14, 20, 16}, /* lava rivers */
    {26, 24, 14, 20, 16}, /* flooded rivers */
    {30, 28, 14, 22, 6},  /* Dead Man's Land (never has gold) */
};

/** Inside the shape (radius 1, y down)? */
static int shape_in7(int k, double u, double v)
{
    const double r2 = u * u + v * v;
    switch (k) {
    case SK_CIRCLE:
    case SK_OVAL: return r2 <= 1.0;
    case SK_CRESCENT: return r2 <= 1.0 && (u - 0.45) * (u - 0.45) + v * v > 0.62;
    case SK_RING: return r2 <= 1.0 && r2 >= 0.36;
    case SK_SEMI: return r2 <= 1.0 && v <= 0.15;
    case SK_QUADRANT: return (u + 0.5) * (u + 0.5) + (v - 0.5) * (v - 0.5) <= 1.44 && u >= -0.5 && v <= 0.5;
    case SK_PIE: return r2 <= 1.0 && !(u > 0.0 && (v < 0.0 ? -v : v) < u * 0.8);
    case SK_TREFOIL:
        return u * u + (v + 0.48) * (v + 0.48) <= 0.25 || (u - 0.42) * (u - 0.42) + (v - 0.24) * (v - 0.24) <= 0.25 ||
            (u + 0.42) * (u + 0.42) + (v - 0.24) * (v - 0.24) <= 0.25;
    case SK_HEART:
        return (u + 0.45) * (u + 0.45) + (v + 0.25) * (v + 0.25) <= 0.30 || (u - 0.45) * (u - 0.45) + (v + 0.25) * (v + 0.25) <= 0.30 ||
            in_poly7(heart_tip7, 3, u, v);
    case SK_CURVI: {
        /* Reuleaux triangle: within sqrt(3) of each corner of the triangle */
        int i;
        for (i = 0; i < 3; i++) {
            const double dx = u - poly7_3[i][0], dy = v - poly7_3[i][1];
            if (dx * dx + dy * dy > 3.0)
                return 0;
        }
        return 1;
    }
    case SK_SQUARE:
    case SK_RECT: return (u < 0.0 ? -u : u) <= 0.85 && (v < 0.0 ? -v : v) <= 0.85;
    case SK_DIAMOND: return in_poly7(poly7_4, 4, u * 0.9, v * 0.9);
    case SK_PENT: return in_poly7(poly7_5, 5, u, v);
    case SK_HEX: return in_poly7(poly7_6, 6, u, v);
    case SK_HEPT: return in_poly7(poly7_7, 7, u, v);
    case SK_OCT: return in_poly7(poly7_8, 8, u, v);
    case SK_NON: return in_poly7(poly7_9, 9, u, v);
    case SK_DEC: return in_poly7(poly7_10, 10, u, v);
    case SK_ISO: return in_poly7(tri_iso7, 3, u, v);
    case SK_RIGHT: return in_poly7(tri_right7, 3, u, v);
    case SK_SCALENE: return in_poly7(tri_scalene7, 3, u, v);
    case SK_EQUI: return in_poly7(poly7_3, 3, u * 0.9, v * 0.9);
    case SK_PARA: return in_poly7(para7, 4, u, v);
    case SK_TRAP: return in_poly7(trap7, 4, u, v);
    case SK_KITE: return in_poly7(kite7, 4, u, v);
    case SK_CROSS: {
        const double au = u < 0.0 ? -u : u, av = v < 0.0 ? -v : v;
        return (au <= 1.0 && av <= 0.34) || (au <= 0.34 && av <= 1.0);
    }
    case SK_STAR: return in_poly7(star7, 10, u, v);
    case SK_ARROW: return in_poly7(arrow7, 7, u, v);
    case SK_TICK: return seg_d2_7(u, v, -0.8, 0.0, -0.25, 0.55) <= 0.05 || seg_d2_7(u, v, -0.25, 0.55, 0.9, -0.75) <= 0.05;
    default: return r2 <= 1.0;
    }
}

/** A deposit's form: one of the library's shapes, stretched, turned, skewed, curved like a banana, wider at one
 *  end, bent at its own scale, rippled at half of it and lumpy at a few slabs. */
struct Form7 {
    int kind;
    double cx, cy, r, aspect, ux, uy, shear, bend, taper, wbig, wmid, wlb, wlm;
    uint64_t s1, s2, s3, s4;
};

static void form7_make(struct Rng *rng, struct Form7 *f, int kind, double cx, double cy, double r, uint64_t ws)
{
    double t;
    f->kind = kind;
    f->cx = cx;
    f->cy = cy;
    f->r = r;
    g7_dir(rng, &f->ux, &f->uy);
    t = rng_random(rng);
    f->aspect = (kind == SK_OVAL || kind == SK_RECT) ? 1.6 + t * 1.2 : 1.0 + t * t * 1.8;
    t = rng_random(rng);
    f->shear = t < 0.5 ? (rng_random(rng) - 0.5) * 1.4 : 0.0;
    f->bend = (rng_random(rng) - 0.5) * 1.4;
    f->taper = (rng_random(rng) - 0.5) * 1.0;
    f->wbig = 0.25 + rng_random(rng) * 0.40;
    f->wmid = 0.12 + rng_random(rng) * 0.16;
    f->wlb = r * 1.1 + 1.5;
    f->wlm = r * 0.45 + 1.0;
    f->s1 = mix64(ws + 1);
    f->s2 = mix64(ws + 2);
    f->s3 = mix64(ws + 3);
    f->s4 = mix64(ws + 4);
}

static void form7_local(const struct Form7 *f, int64_t xi, int64_t yi, double *uo, double *vo)
{
    const double x = (double)xi, y = (double)yi, dx = x + 0.5 - f->cx, dy = y + 0.5 - f->cy;
    double p = dx * f->ux + dy * f->uy, q = dy * f->ux - dx * f->uy, u, v, k;
    p += f->shear * q;
    u = p / (f->r * f->aspect);
    v = q / f->r;
    v -= f->bend * u * u;
    k = 1.0 + f->taper * u;
    if (k < 0.25)
        k = 0.25;
    v /= k;
    u += f->wbig * fbm(f->s1, x / f->wlb, y / f->wlb, 2) + f->wmid * fbm(f->s3, x / f->wlm, y / f->wlm, 2) +
        0.16 * fbm(f->s2, x / 4.5, y / 4.5, 2);
    v += f->wbig * fbm(f->s2, x / f->wlb, y / f->wlb, 2) + f->wmid * fbm(f->s4, x / f->wlm, y / f->wlm, 2) +
        0.16 * fbm(f->s3, x / 4.5, y / 4.5, 2);
    *uo = u;
    *vo = v;
}

static void seam_build7(const struct WgWorld *w, int64_t cx, int64_t cy, struct SeamEntry *e)
{
    struct Rng rng;
    struct Clim7 c;
    struct Grid7 g;
    int64_t sx, sy;
    int b, fam, kind, dense, dstyle, roll, acc, i;
    double chance_roll, rich, ux, uy, size_roll, r;
    rng.s = hash3(w->s7[V7_SEAM], (uint64_t)cx, (uint64_t)cy);
    chance_roll = rng_random(&rng);
    sx = cx * SEAM_CELL + rng_randint(&rng, 0, SEAM_CELL - 1);
    sy = cy * SEAM_CELL + rng_randint(&rng, 0, SEAM_CELL - 1);
    climate7(w, sx, sy, &c);
    b = biome7_of(&c);
    if (!(chance_roll < SEAM_CHANCE * seam7_biome[b][0]))
        return;
    rich = seam7_biome[b][1] + 0.4 * climate_part(-c.moist);
    dense = rng_random(&rng) < seam7_biome[b][2];
    /* the family by the biome, then one of its shapes */
    roll = (int)rng_randint(&rng, 0, 99);
    for (fam = 0, acc = 0; fam < 4; fam++) {
        acc += seam7_family[b][fam];
        if (roll < acc)
            break;
    }
    kind = sk7_first[fam] + (int)rng_randint(&rng, 0, sk7_first[fam + 1] - sk7_first[fam] - 1);
    /* the size: small, medium or large */
    size_roll = rng_random(&rng);
    r = size_roll < 0.4 ? 2.0 + rng_random(&rng) * 1.5 : (size_roll < 0.8 ? 3.5 + rng_random(&rng) * 3.0 : 6.5 + rng_random(&rng) * 4.5);
    r *= 0.75 + 0.22 * rich;
    /* the dense gold: a core, a rim, patches, or all of a small one */
    dstyle = (int)rng_randint(&rng, 0, 99);
    dstyle = dstyle < 45 ? 0 : (dstyle < 60 ? 1 : (dstyle < 85 ? 2 : 3));
    if (dstyle == 3 && r > 3.5)
        dstyle = 0;
    g.x0 = cx * SEAM_CELL - SEAM7_REACH;
    g.y0 = cy * SEAM_CELL - SEAM7_REACH;
    g.W = g.H = SEAM_CELL + 2 * SEAM7_REACH;
    g.ws = rng_next(&rng);
    g.c = (uint8_t *)calloc((size_t)g.W * g.H, 1);
    if (g.c == NULL)
        return;
    g7_dir(&rng, &ux, &uy);
    if (kind == SK_VEIN) {
        /* a river-like vein: meanders both ways from its middle, thin, pointy ends, a branch now and then */
        const double len = (18.0 + rng_random(&rng) * 40.0) * rich, wdt = 0.9 + rng_random(&rng) * 0.6;
        const double wig = 0.45 + rng_random(&rng) * 0.5;
        g7_path(&g, &rng, (double)sx + 0.5, (double)sy + 0.5, ux, uy, len * 0.5, 2.0, wdt, 0.3, wig, dense, 1);
        g7_path(&g, &rng, (double)sx + 0.5, (double)sy + 0.5, -ux, -uy, len * 0.5, 2.0, wdt, 0.3, wig, dense, 1);
    } else if (kind == SK_CLUSTER) {
        /* nuggets round a spot */
        const int n = (int)rng_randint(&rng, 3, 7);
        for (i = 0; i < n; i++) {
            double ox, oy, d;
            g7_dir(&rng, &ox, &oy);
            d = (i == 0) ? 0.0 : (2.5 + rng_random(&rng) * 0.9 * r);
            g7_disc(&g, (double)sx + 0.5 + ox * d, (double)sy + 0.5 + oy * d, 0.9 + rng_random(&rng) * 1.1, dense && i == 0);
        }
    } else if (kind == SK_LENS) {
        /* version 6's lens: a bent ellipse with a lumpy edge, perhaps a second lobe */
        struct SeamLobe lobes[2];
        int nl = 1, k;
        lobes[0].sx = sx;
        lobes[0].sy = sy;
        seam_lobe(&rng, SEAM_HALF_LENGTH_MIN + rng_random(&rng) * SEAM_HALF_LENGTH_EXTRA, &lobes[0]);
        if (rng_random(&rng) < SEAM_LOBE_CHANCE) {
            double a2 = lobes[0].a * (0.45 + rng_random(&rng) * 0.35), t = lobes[0].a * (0.5 + rng_random(&rng) * 0.5);
            if (rng_random(&rng) < 0.5)
                t = -t;
            lobes[1].sx = sx + py_round(lobes[0].ux * t);
            lobes[1].sy = sy + py_round(lobes[0].uy * t);
            seam_lobe(&rng, a2, &lobes[1]);
            nl = 2;
        }
        for (k = 0; k < nl; k++) {
            const struct SeamLobe *l = &lobes[k];
            const double a = l->a * rich, bb = l->b * rich;
            const int64_t rr = (int64_t)a + 5;
            int64_t x, y;
            for (y = l->sy - rr; y <= l->sy + rr; y++)
                for (x = l->sx - rr; x <= l->sx + rr; x++) {
                    const double rx = (double)(x - l->sx), ry = (double)(y - l->sy);
                    const double u = (rx * l->ux + ry * l->uy) / a, v = (ry * l->ux - rx * l->uy) / bb - l->bend * u * u;
                    const double f = u * u + v * v;
                    if (f <= 1.0 + SEAM_EDGE * fbm(g.ws, (double)x / 4.5, (double)y / 4.5, 2))
                        g7_set(&g, x, y, dense && f <= 0.18 + 0.25 * perlin(g.ws ^ 0x5DEECE66DULL, (double)x / 2.9, (double)y / 2.9));
                }
        }
    } else {
        /* a shape of the library, deformed (Form7), sometimes fused with a second smaller one, with bites out of
           its edge */
        struct Form7 fm, f2;
        double bites[3][3];
        int has2 = 0, nbites, bi;
        const uint64_t ws_p = mix64(g.ws + 5);
        int64_t x, y, reach;
        form7_make(&rng, &fm, kind, (double)sx + 0.5, (double)sy + 0.5, r, g.ws);
        reach = (int64_t)(r * fm.aspect * 2.3) + 6;
        if (rng_random(&rng) < 0.25) {
            double ox, oy, d;
            const int k2 = SK_CIRCLE + (int)rng_randint(&rng, 0, SK_LENS - 1 - SK_CIRCLE);
            g7_dir(&rng, &ox, &oy);
            d = r * (0.6 + rng_random(&rng) * 0.6);
            form7_make(&rng, &f2, k2, fm.cx + ox * d, fm.cy + oy * d, r * (0.4 + rng_random(&rng) * 0.4), mix64(g.ws + 9));
            has2 = 1;
            if ((int64_t)(d + f2.r * f2.aspect * 2.3) + 6 > reach)
                reach = (int64_t)(d + f2.r * f2.aspect * 2.3) + 6;
        }
        nbites = (int)rng_randint(&rng, 0, 3);
        for (bi = 0; bi < nbites; bi++) {
            double ox, oy, d;
            g7_dir(&rng, &ox, &oy);
            d = r * (0.7 + rng_random(&rng) * 0.5);
            bites[bi][0] = fm.cx + ox * d;
            bites[bi][1] = fm.cy + oy * d;
            bites[bi][2] = r * (0.25 + rng_random(&rng) * 0.3);
        }
        for (y = sy - reach; y <= sy + reach; y++)
            for (x = sx - reach; x <= sx + reach; x++) {
                double u, v;
                int in = 0, core = 0, rim = 0;
                form7_local(&fm, x, y, &u, &v);
                if (shape_in7(fm.kind, u, v)) {
                    in = 1;
                    core = shape_in7(fm.kind, u * 2.0, v * 2.0);
                    rim = !shape_in7(fm.kind, u * 1.35, v * 1.35);
                } else if (has2) {
                    double u2, v2;
                    form7_local(&f2, x, y, &u2, &v2);
                    if (shape_in7(f2.kind, u2, v2)) {
                        in = 1;
                        rim = !shape_in7(f2.kind, u2 * 1.35, v2 * 1.35);
                    }
                }
                for (bi = 0; bi < nbites && in; bi++) {
                    const double bx = (double)x + 0.5 - bites[bi][0], by = (double)y + 0.5 - bites[bi][1];
                    if (bx * bx + by * by < bites[bi][2] * bites[bi][2])
                        in = 0;
                }
                if (!in)
                    continue;
                switch (dstyle) {
                case 0: g7_set(&g, x, y, dense && core); break;                                         /* a core */
                case 1: g7_set(&g, x, y, dense && rim); break;                                          /* a rim */
                case 2: g7_set(&g, x, y, dense && perlin(ws_p, (double)x / 2.5, (double)y / 2.5) > 0.3); break; /* patches */
                default: g7_set(&g, x, y, dense); break;                                                /* all of it */
                }
            }
        /* now and then limbs that stretch out and taper */
        if (rng_random(&rng) < (fam == 2 ? 0.12 : (fam == 3 ? 0.15 : 0.28))) {
            const int limbs = (int)rng_randint(&rng, 1, 4);
            for (i = 0; i < limbs; i++) {
                double lx, ly;
                g7_dir(&rng, &lx, &ly);
                g7_path(&g, &rng, (double)sx + 0.5 + lx * r * 0.7, (double)sy + 0.5 + ly * r * 0.7, lx, ly,
                    (3.0 + rng_random(&rng) * 7.0) * rich, 1.5, 0.4 + r * 0.12, 0.3, 0.5, 0, 0);
            }
        }
    }
    seam_finish7(w, &g, e);
}

/** worldgen.World.seam: builds the seam of square (cx, cy) into e. */
static void seam_build(const struct WgWorld *w, int64_t cx, int64_t cy, struct SeamEntry *e)
{
    struct Rng rng;
    struct SeamLobe lobes[2];
    int64_t sx, sy;
    int nl = 1, dense;
    uint64_t ws;
    double clim[4], crys;
    if (w->version >= 7) {
        seam_build7(w, cx, cy, e);
        return;
    }
    rng.s = hash3(w->s_seam, (uint64_t)cx, (uint64_t)cy);
    if (!(rng_random(&rng) < SEAM_CHANCE))
        return;
    sx = cx * SEAM_CELL + rng_randint(&rng, 0, SEAM_CELL - 1);
    sy = cy * SEAM_CELL + rng_randint(&rng, 0, SEAM_CELL - 1);
    lobes[0].sx = sx;
    lobes[0].sy = sy;
    seam_lobe(&rng, SEAM_HALF_LENGTH_MIN + rng_random(&rng) * SEAM_HALF_LENGTH_EXTRA, &lobes[0]);
    if (rng_random(&rng) < SEAM_LOBE_CHANCE) {
        double a2 = lobes[0].a * (0.45 + rng_random(&rng) * 0.35);
        double t = lobes[0].a * (0.5 + rng_random(&rng) * 0.5);
        if (rng_random(&rng) < 0.5)
            t = -t;
        lobes[1].sx = sx + py_round(lobes[0].ux * t);
        lobes[1].sy = sy + py_round(lobes[0].uy * t);
        seam_lobe(&rng, a2, &lobes[1]);
        nl = 2;
    }
    dense = rng_random(&rng) < SEAM_DENSE_CHANCE;
    ws = rng_next(&rng);
    climate(w, sx, sy, clim);
    crys = (clim[3] - 0.31) / 0.31;
    if (crys < 0.0)
        crys = 0.0;
    if (crys > 1.0)
        crys = 1.0;
    seam_cells(w, lobes, nl, 1.0 + 0.4 * climate_part(-clim[1]) + 0.3 * crys, dense, ws, e);
}

static uint32_t key_hash(int type, int64_t a, int64_t b);

/** The seam of square (cx, cy), from the cache (open addressing, emptied once half full) or built now. */
static const struct SeamEntry *seam_get(struct WgWorld *w, int64_t cx, int64_t cy)
{
    uint32_t i = key_hash(WgT_COUNT, cx, cy) & (SEAM_CAP - 1);
    struct SeamEntry *e;
    while (w->seam[i].valid) {
        if (w->seam[i].cx == cx && w->seam[i].cy == cy)
            return &w->seam[i];
        i = (i + 1) & (SEAM_CAP - 1);
    }
    if (w->seam_count >= SEAM_CAP / 2) {
        int k;
        for (k = 0; k < SEAM_CAP; k++)
            free(w->seam[k].cells);
        memset(w->seam, 0, sizeof(struct SeamEntry) * SEAM_CAP);
        w->seam_count = 0;
        i = key_hash(WgT_COUNT, cx, cy) & (SEAM_CAP - 1);
    }
    e = &w->seam[i];
    seam_build(w, cx, cy, e);
    e->valid = 1;
    e->cx = cx;
    e->cy = cy;
    w->seam_count++;
    return e;
}

/** worldgen.World.seam_gold: WgS_Gold, WgS_DenseGold or 0 at a world slab, from the seams of the 3x3 squares round
 *  it. */
static int seam_gold(struct WgWorld *w, int64_t x, int64_t y)
{
    int64_t gx = fdiv(x, SEAM_CELL), gy = fdiv(y, SEAM_CELL), cx, cy;
    const int64_t reach = w->version >= 7 ? 2 : 1; /* version 7's seams can be longer (SEAM7_REACH) */
    int found = 0;
    for (cy = gy - reach; cy <= gy + reach; cy++)
        for (cx = gx - reach; cx <= gx + reach; cx++) {
            const struct SeamEntry *e = seam_get(w, cx, cy);
            int k;
            if (e->w == 0 || x < e->x0 || y < e->y0 || x >= e->x0 + e->w || y >= e->y0 + e->h)
                continue;
            k = e->cells[(y - e->y0) * e->w + (x - e->x0)];
            if (k == WgS_DenseGold)
                return WgS_DenseGold;
            if (k != 0)
                found = WgS_Gold;
        }
    return found;
}

/** worldgen.World.terrain(x, y, clim): the gold seams (stage 3) over the base terrain. */
static uint8_t terrain(struct WgWorld *w, int64_t xi, int64_t yi, const double *clim)
{
    int g = seam_gold(w, xi, yi);
    if (g != 0)
        return (uint8_t)g;
    return terrain_base(w, xi, yi, clim);
}

/** Version 7's slab: the gold seams over the ground; none in Dead Man's Land. */
static uint8_t terrain7(struct WgWorld *w, int64_t xi, int64_t yi, const struct Clim7 *c)
{
    int g;
    if (c->special == WgB_DeadMans)
        return WgS_Earth;
    g = seam_gold(w, xi, yi);
    if (g != 0)
        return (uint8_t)g;
    return ground7(w, xi, yi, c);
}

/** Python's list[i] = v for a list of 256: a negative index counts from the end; past the end Python raises
 *  IndexError (only possible for the gem cells of a Crystal chunk), so it is skipped here. */
/** worldgen.World.place_gems */
static void place_gems(const struct WgWorld *w, struct WgChunk *ch, int64_t cx0, int64_t cy0)
{
    struct Rng rng;
    double roll;
    int64_t lx, ly, extra, i;
    int cells[MAX_GEM_CELLS][2], n = 1, d, k, tries = 1, biome = ch->biomes[8 * WG_CHUNK + 8];
    rng.s = hash3(w->s_gems, (uint64_t)(int64_t)ch->cx, (uint64_t)(int64_t)ch->cy);
    roll = rng_random(&rng);
    lx = rng_randint(&rng, 3, 12);
    ly = rng_randint(&rng, 3, 12);
    if (roll >= biome_gem_chance[biome])
        return;
    if (hypot_int(cx0 + 8 - START_X, cy0 + 8 - START_Y) < 48)
        return;
    /* on open earth you can reach only (never inside rock) */
    while (ch->slabs[ly * WG_CHUNK + lx] != WgS_Earth || !reachable(w, cx0 + lx, cy0 + ly)) {
        if (tries >= GEMS_TRIES)
            return;
        lx = rng_randint(&rng, 3, 12);
        ly = rng_randint(&rng, 3, 12);
        tries++;
    }
    cells[0][0] = (int)lx;
    cells[0][1] = (int)ly;
    extra = rng_randint(&rng, 0, biome_gem_extra[biome]);
    for (i = 0; i < extra; i++) {
        int c = (int)rng_randint(&rng, 0, n - 1), nx, ny, found = 0;
        d = (int)rng_randint(&rng, 0, 3);
        nx = cells[c][0] + dir_x[d];
        ny = cells[c][1] + dir_y[d];
        if ((nx < 0) || (nx >= WG_CHUNK) || (ny < 0) || (ny >= WG_CHUNK))
            continue; /* the cluster stays in the chunk */
        for (k = 0; k < n; k++)
            if (cells[k][0] == nx && cells[k][1] == ny)
                found = 1;
        if (!found && ch->slabs[ny * WG_CHUNK + nx] == WgS_Earth) { /* and on earth */
            cells[n][0] = nx;
            cells[n][1] = ny;
            n++;
        }
    }
    for (k = 0; k < n; k++)
        ch->slabs[cells[k][1] * WG_CHUNK + cells[k][0]] = WgS_Gems;
}

/******************************************************************************/
/* Structure layouts (worldgen.Builder and the build_* functions)             */
/******************************************************************************/

struct Builder {
    struct Layout *lay;
    int64_t ox, oy;
    int64_t bx1, by1, bx2, by2;
    int side;
    int owner;               /* Structure.owner as a WgOwner */
    struct Rng rng;
    double distance;
    uint64_t seed;
    int biome;               /* Builder.biome */
};

static int is_owned_floor(int slab)
{
    switch (slab) {
    case WgS_Claimed: case WgS_Heart: case WgS_Portal: case WgS_Treasury: case WgS_Lair: case WgS_Hatchery:
    case WgS_Training: case WgS_Library:
        return 1;
    default:
        return 0;
    }
}

static int keeps_owner(int slab)
{
    return is_owned_floor(slab) || slab == WgS_Wall || slab == WgS_DoorWood || slab == WgS_DoorBrace;
}

static int b_inside(const struct Builder *b, int64_t x, int64_t y)
{
    return b->bx1 <= x && x <= b->bx2 && b->by1 <= y && y <= b->by2;
}

static size_t b_index(const struct Builder *b, int64_t x, int64_t y)
{
    return (size_t)((y - b->by1) * b->side + (x - b->bx1));
}

static void b_set(struct Builder *b, int64_t x, int64_t y, int slab, int owner)
{
    size_t i;
    if (!b_inside(b, x, y))
        return; /* Builder.clipped */
    i = b_index(b, x, y);
    b->lay->op[i] = OP_SET;
    b->lay->slab[i] = (uint8_t)slab;
    b->lay->own[i] = (uint8_t)(keeps_owner(slab) ? owner : WgO_Neutral);
}

static int b_is_set(const struct Builder *b, int64_t x, int64_t y)
{
    return b_inside(b, x, y) && b->lay->op[b_index(b, x, y)] == OP_SET;
}

static void b_soft(struct Builder *b, int64_t x, int64_t y)
{
    if (b_inside(b, x, y)) {
        size_t i = b_index(b, x, y);
        if (b->lay->op[i] == OP_NONE)
            b->lay->op[i] = OP_SOFT;
    }
}

static void b_rect(struct Builder *b, int64_t xa, int64_t ya, int64_t xb, int64_t yb, int slab, int owner)
{
    int64_t x, y;
    for (y = min64(ya, yb); y <= max64(ya, yb); y++)
        for (x = min64(xa, xb); x <= max64(xa, xb); x++)
            b_set(b, x, y, slab, owner);
}

enum { BLOB_SOFT, BLOB_SEAM, BLOB_PATH, BLOB_RING };

/** Builder.blob with the callbacks used by worldgen.py: soft, gold_seam's put, set PATH, the hoard ring. */
static void b_blob(struct Builder *b, int64_t cx, int64_t cy, double r, double jitter, int mode, double dense)
{
    uint64_t s = hash4(b->seed, (uint64_t)cx, (uint64_t)cy, (uint64_t)(int64_t)(r * 16.0));
    double reach = r + fabs(jitter);
    int64_t rr = (int64_t)ceil(reach) + 1, x, y;
    for (y = cy - rr; y <= cy + rr; y++)
        for (x = cx - rr; x <= cx + rr; x++) {
            int64_t dx = x - cx, dy = y - cy;
            double d = sqrt((double)(dx * dx + dy * dy)); /* math.hypot, exact for small integers */
            double a, lim;
            int slab;
            if (d > reach)
                continue;
            a = atan2((double)dy, (double)dx);
            lim = r + jitter * perlin(s, cos(a) * 1.3 + 3.1, sin(a) * 1.3 + 7.7);
            if (!(d <= lim))
                continue;
            switch (mode) {
            case BLOB_SOFT:
                b_soft(b, x, y);
                break;
            case BLOB_SEAM:
                if (!b_is_set(b, x, y)) {
                    slab = (d < r * 0.45 && rng_chance(&b->rng, dense)) ? WgS_DenseGold : WgS_Gold;
                    b_set(b, x, y, slab, WgO_Neutral);
                }
                break;
            case BLOB_PATH:
                b_set(b, x, y, WgS_Path, WgO_Neutral);
                break;
            default: /* BLOB_RING */
                if (!b_is_set(b, x, y)) {
                    slab = rng_chance(&b->rng, 0.2) ? WgS_DenseGold : WgS_Gold;
                    b_set(b, x, y, slab, WgO_Neutral);
                }
                break;
            }
        }
}

static void b_soft_line(struct Builder *b, int64_t xa, int64_t ya, int64_t xb, int64_t yb, int hw)
{
    int64_t n = max64(max64(abs64(xb - xa), abs64(yb - ya)), 1), i, x, y, dx, dy;
    for (i = 0; i <= n; i++) {
        x = py_round((double)xa + (double)((xb - xa) * i) / (double)n);
        y = py_round((double)ya + (double)((yb - ya) * i) / (double)n);
        for (dy = -hw; dy <= hw; dy++)
            for (dx = -hw; dx <= hw; dx++)
                b_soft(b, x + dx, y + dy);
    }
}

static int in_keep(int64_t (*keep)[2], int nkeep, int64_t x, int64_t y)
{
    int k;
    for (k = 0; k < nkeep; k++)
        if (keep[k][0] == x && keep[k][1] == y)
            return 1;
    return 0;
}

/** Builder.fortify. The result does not depend on the order the floor cells are visited in:
 *  walls only go on cells that are not set, and walls are not floor. */
static void b_fortify(struct Builder *b, int owner, int64_t (*keep)[2], int nkeep)
{
    struct Layout *L = b->lay;
    int gx, gy, k;
    int64_t dx, dy;
    for (gy = 0; gy < b->side; gy++)
        for (gx = 0; gx < b->side; gx++) {
            size_t i = (size_t)gy * (size_t)b->side + (size_t)gx;
            int64_t x = b->bx1 + gx, y = b->by1 + gy;
            if (L->op[i] != OP_SET || L->own[i] != owner || !is_owned_floor(L->slab[i]))
                continue;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                    if (!in_keep(keep, nkeep, x + dx, y + dy) && !b_is_set(b, x + dx, y + dy))
                        b_set(b, x + dx, y + dy, WgS_Wall, owner);
        }
    for (k = 0; k < nkeep; k++)
        b_soft(b, keep[k][0], keep[k][1]);
}

static void b_thing(struct Builder *b, int is_creature, const char *name, int64_t x, int64_t y, int owner,
                    int64_t level, int has_gate)
{
    struct LThing *t;
    if (!b_inside(b, x, y))
        return; /* Builder.clipped */
    if (b->lay->nthings >= MAX_LTHINGS) {
        b->lay->overflow = 1;
        return;
    }
    t = &b->lay->things[b->lay->nthings++];
    t->is_creature = (uint8_t)is_creature;
    t->name = name;
    t->owner = (uint8_t)owner;
    t->level = (uint8_t)(is_creature ? max64(1, min64(10, level)) : 0);
    t->has_gate = (uint8_t)has_gate;
    t->sx = x;
    t->sy = y;
    t->x = (double)(x * 3) + 1.5;
    t->y = (double)(y * 3) + 1.5;
}

static void b_creature(struct Builder *b, const char *name, int64_t x, int64_t y, int owner, int64_t level)
{
    b_thing(b, 1, name, x, y, owner, level, 0);
}

static void b_obj(struct Builder *b, const char *name, int64_t x, int64_t y, int owner)
{
    b_thing(b, 0, name, x, y, owner, 0, 0);
}

static void b_heart_room(struct Builder *b, int owner)
{
    b_rect(b, b->ox - 2, b->oy - 2, b->ox + 2, b->oy + 2, WgS_Claimed, owner);
    b_rect(b, b->ox - 1, b->oy - 1, b->ox + 1, b->oy + 1, WgS_Heart, owner);
}

static void b_corridor(struct Builder *b, int d, int64_t a1, int64_t a2, int owner)
{
    int64_t a;
    for (a = a1; a <= a2; a++)
        b_set(b, b->ox + dir_x[d] * a, b->oy + dir_y[d] * a, WgS_Claimed, owner);
}

/** Builder.arm_room; the cells go to cells[] when it is not NULL (at most depth * width). */
static int b_arm_room(struct Builder *b, int d, int64_t start, int depth, int width, int slab, int owner,
                      int64_t (*cells)[2])
{
    int64_t px = -dir_y[d], py = dir_x[d], a, p;
    int n = 0;
    for (a = start; a < start + depth; a++)
        for (p = -(width / 2); p < width - width / 2; p++) {
            int64_t x = b->ox + dir_x[d] * a + px * p, y = b->oy + dir_y[d] * a + py * p;
            b_set(b, x, y, slab, owner);
            if (cells != NULL) {
                cells[n][0] = x;
                cells[n][1] = y;
            }
            n++;
        }
    return n;
}

static void b_gold_seam(struct Builder *b, int64_t cx, int64_t cy, double r, double dense)
{
    b_blob(b, cx, cy, r, r * 0.35, BLOB_SEAM, dense);
}

static int64_t level_for(double distance, int per, int cap)
{
    return max64(1, min64(cap, 1 + (int64_t)(distance / (double)per)));
}

static int make_pool(const struct Weighted *src, int n, double distance, const char **names, int *weights)
{
    int i, k = 0;
    for (i = 0; i < n; i++)
        if (distance >= (double)src[i].min_dist) {
            names[k] = src[i].name;
            weights[k] = src[i].weight;
            k++;
        }
    return k;
}

static int biome_mult(int biome, const char *name)
{
    int i;
    for (i = 0; i < COUNT_OF(biome_creatures); i++)
        if (biome_creatures[i].biome == biome && strcmp(biome_creatures[i].name, name) == 0)
            return biome_creatures[i].mult;
    return 1;
}

/** worldgen._biome_pool: weights times the biome's multipliers, then the biome's extra creatures. */
static int make_biome_pool(const struct Weighted *src, int n, double distance, int biome, const char **names,
                           int *weights)
{
    int i, k = make_pool(src, n, distance, names, weights);
    for (i = 0; i < k; i++)
        weights[i] *= biome_mult(biome, names[i]);
    for (i = 0; i < COUNT_OF(biome_extra_creatures); i++) {
        const struct Weighted *e = &biome_extra_creatures[i].w;
        if (biome_extra_creatures[i].biome == biome && distance >= (double)e->min_dist) {
            names[k] = e->name;
            weights[k] = e->weight * biome_mult(biome, e->name);
            k++;
        }
    }
    return k;
}

/** Counter(): one more of name, keeping the order names first came in. */
static void ref_count(struct WgStructRef *ref, const char *name)
{
    int i;
    for (i = 0; i < ref->creature_kinds; i++)
        if (strcmp(ref->creatures[i].name, name) == 0) {
            ref->creatures[i].count++;
            return;
        }
    if (ref->creature_kinds < WG_MAX_REF_CREATURES) {
        strncpy(ref->creatures[i].name, name, WG_NAME_LEN - 1);
        ref->creatures[i].count = 1;
        ref->creature_kinds++;
    }
}

static void build_start(struct Builder *b)
{
    static const int64_t imps[4][2] = {{-2, -2}, {2, -2}, {-2, 2}, {2, 2}};
    /* worldgen.build_start: the seams stay in the +x/+y quarter; the one whose sector the portal's arm
     * crosses (east or south) moves to the diagonal */
    int los[2] = {-15, 65}, spans[2] = {40, 40}, pd;
    struct Rng *rng = &b->rng;
    const int64_t ox = b->ox, oy = b->oy;
    const int me = WgO_Player;
    int64_t keep[4][2], dist, px, py, gc, gx, gy;
    double ga, gr, ca, sa, gd;
    int d, i;
    b_heart_room(b, me);
    for (d = 0; d < 4; d++) {
        int64_t x = ox + dir_x[d] * 3, y = oy + dir_y[d] * 3;
        b_set(b, x, y, WgS_Claimed, me);
        keep[d][0] = x + dir_x[d];
        keep[d][1] = y + dir_y[d];
    }
    b_fortify(b, me, keep, 4);
    for (i = 0; i < 4; i++)
        b_creature(b, "IMP", ox + imps[i][0], oy + imps[i][1], me, 1);
    /* the portal: straight out from the heart in one of the four directions, 8-14 slabs away (3-9 slabs of earth
     * between the dungeon's wall and the portal) */
    pd = (int)rng_randint(rng, 0, 3);
    dist = rng_randint(rng, 8, 14);
    px = ox + dir_x[pd] * dist;
    py = oy + dir_y[pd] * dist;
    b_rect(b, px - 2, py - 2, px + 2, py + 2, WgS_Path, WgO_Neutral);
    b_rect(b, px - 1, py - 1, px + 1, py + 1, WgS_Portal, WgO_Neutral);
    b_soft_line(b, ox, oy, px, py, 1);
    if (pd == 1) {        /* east */
        los[0] = 30;
        spans[0] = 20;
    } else if (pd == 2) { /* south */
        los[1] = 40;
        spans[1] = 20;
    }
    for (i = 0; i < 2 && b->biome != WgB_DeadMans; i++) {
        ga = ((double)los[i] + rng_random(rng) * (double)spans[i]) * deg_to_rad;
        gc = rng_randint(rng, START_GOLD_FREE + 5, START_GOLD_FREE + 6);
        ca = cos(ga);
        sa = sin(ga);
        gd = (double)gc / (fabs(ca) > fabs(sa) ? fabs(ca) : fabs(sa));
        gx = ox + py_round(ca * gd);
        gy = oy + py_round(sa * gd);
        gr = 2.8 + rng_random(rng) * 0.6;
        b_gold_seam(b, gx, gy, gr, 0.0);
        b_soft_line(b, ox, oy, gx, gy, 1);
    }
    b_blob(b, ox, oy, 14.0, 3.0, BLOB_SOFT, 0.0);
    b->lay->ref.heart_x = ox;
    b->lay->ref.heart_y = oy;
    b->lay->ref.portal_x = px;
    b->lay->ref.portal_y = py;
}

static void room_size(int room, int *depth, int *width)
{
    switch (room) {
    case WgS_Lair: *depth = 4; *width = 5; break;
    case WgS_Library: *depth = 3; *width = 3; break;
    default: *depth = 3; *width = 4; break; /* hatchery, treasury, training */
    }
}

static void build_keeper(struct Builder *b)
{
    static const int64_t imp_spots[6][2] = {{-2, -2}, {2, -2}, {-2, 2}, {2, 2}, {0, -2}, {0, 2}};
    struct Rng *rng = &b->rng;
    struct WgStructRef *ref = &b->lay->ref;
    const int64_t ox = b->ox, oy = b->oy;
    const int me = b->owner;
    int rooms[4], dirs[4] = {D_N, D_E, D_S, D_W}, arm_room[4], others[3], nothers = 0;
    int64_t arm_far[4], lair_cells[4 * 5][2], keep[2][2], spots[6][2];
    int64_t far, plen, pcx, pcy, gcx, gcy, count, lvl, nimps;
    const char *pool_names[16];
    int pool_w[16], npool, nlair = 0, k, i;

    b_heart_room(b, me);
    rooms[0] = WgS_Lair;
    rooms[1] = WgS_Hatchery;
    rooms[2] = WgS_Treasury;
    rooms[3] = rng_randint(rng, 0, 1) == 0 ? WgS_Training : WgS_Library;
    rng_shuffle(rng, dirs, 4, sizeof dirs[0]);
    for (k = 0; k < 4; k++) {
        int d = dirs[k], room = rooms[k], depth, width;
        int64_t length = rng_randint(rng, 3, 4), start;
        b_corridor(b, d, 3, 2 + length, me);
        room_size(room, &depth, &width);
        if (room == WgS_Lair && b->distance > 400)
            width = 5;
        start = 3 + length;
        if (room == WgS_Lair) {
            nlair = b_arm_room(b, d, start, depth, width, room, me, lair_cells);
        } else {
            b_arm_room(b, d, start, depth, width, room, me, NULL);
        }
        arm_room[d] = room;
        arm_far[d] = start + depth;
    }
    for (k = 0; k < 4; k++)
        if (arm_room[dirs[k]] != WgS_Lair)
            others[nothers++] = dirs[k];
    /* portal */
    far = arm_far[others[0]];
    plen = rng_randint(rng, 2, 4);
    b_corridor(b, others[0], far, far + plen - 1, me);
    pcx = ox + dir_x[others[0]] * (far + plen + 1);
    pcy = oy + dir_y[others[0]] * (far + plen + 1);
    b_rect(b, pcx - 1, pcy - 1, pcx + 1, pcy + 1, WgS_Portal, me);
    /* gold */
    far = arm_far[others[1]];
    b_corridor(b, others[1], far, far, me);
    keep[0][0] = ox + dir_x[others[1]] * (far + 1);
    keep[0][1] = oy + dir_y[others[1]] * (far + 1);
    gcx = ox + dir_x[others[1]] * (far + 4);
    gcy = oy + dir_y[others[1]] * (far + 4);
    b_gold_seam(b, gcx, gcy, 2.3, 0.3);
    b_soft_line(b, keep[0][0], keep[0][1], gcx, gcy, 1);
    /* second way out */
    far = arm_far[others[2]];
    b_corridor(b, others[2], far, far, me);
    keep[1][0] = ox + dir_x[others[2]] * (far + 1);
    keep[1][1] = oy + dir_y[others[2]] * (far + 1);
    b_fortify(b, me, keep, 2);
    /* imps on the heart ring */
    memcpy(spots, imp_spots, sizeof spots);
    rng_shuffle(rng, spots, 6, sizeof spots[0]);
    nimps = rng_randint(rng, 4, 6);
    for (i = 0; i < nimps; i++)
        b_creature(b, "IMP", ox + spots[i][0], oy + spots[i][1], me, 1);
    /* creatures in the lair */
    rng_shuffle(rng, lair_cells, nlair, sizeof lair_cells[0]);
    count = min64(min64(nlair, 4 + (int64_t)(b->distance / 150.0) + rng_randint(rng, 0, 2)), 12);
    npool = make_biome_pool(keeper_creatures, COUNT_OF(keeper_creatures), b->distance, b->biome, pool_names,
                            pool_w);
    lvl = level_for(b->distance, 200, 6);
    for (i = 0; i < count; i++) {
        const char *name = pool_names[rng_weighted(rng, pool_w, npool)];
        ref_count(ref, name);
        b_creature(b, name, lair_cells[i][0], lair_cells[i][1], me, lvl + rng_randint(rng, 0, 1));
    }
    b_blob(b, ox, oy, 17.0, 3.0, BLOB_SOFT, 0.0);
    ref->heart_x = ox;
    ref->heart_y = oy;
    ref->portal_x = pcx;
    ref->portal_y = pcy;
    ref->gold_x = gcx;
    ref->gold_y = gcy;
    for (k = 0; k < 4; k++)
        ref->rooms[k] = (uint8_t)arm_room[k];
    ref->threat_radius = (int32_t)rng_randint(rng, 36, 56);
    ref->money = (int32_t)(2000 + 500 * min64(6, (int64_t)(b->distance / 150.0)));
}

static void build_fortress(struct Builder *b)
{
    static const char *const loot[3] = {"GOLD_CHEST", "GOLD", "GOLDL"};
    struct Rng *rng = &b->rng;
    struct WgStructRef *ref = &b->lay->ref;
    const int64_t ox = b->ox, oy = b->oy;
    const int me = WgO_Heroes;
    int dirs[4] = {D_N, D_E, D_S, D_W}, door, moat, k, nfloor = 0, npool, pool_w[16];
    int64_t r, p, ndoors, x, y, count, lvl, nloot, i, a;
    int64_t floor_cells[13 * 13][2];
    const char *pool_names[16];

    r = rng_randint(rng, 4, 6);
    b_rect(b, ox - r, oy - r, ox + r, oy + r, WgS_Claimed, me);
    if (r >= 5) {
        p = (r + 1) / 2;
        b_set(b, ox - p, oy - p, WgS_Wall, me);
        b_set(b, ox - p, oy + p, WgS_Wall, me);
        b_set(b, ox + p, oy - p, WgS_Wall, me);
        b_set(b, ox + p, oy + p, WgS_Wall, me);
    }
    rng_shuffle(rng, dirs, 4, sizeof dirs[0]);
    ndoors = rng_randint(rng, 1, 2);
    door = rng_randint(rng, 0, 1) == 0 ? WgS_DoorWood : WgS_DoorBrace;
    moat = rng_chance(rng, 0.4);
    for (k = 0; k < ndoors; k++) {
        int d = dirs[k];
        b_set(b, ox + dir_x[d] * (r + 1), oy + dir_y[d] * (r + 1), door, me);
        for (a = 2; a <= 3; a++)
            b_set(b, ox + dir_x[d] * (r + a), oy + dir_y[d] * (r + a), WgS_Path, WgO_Neutral);
    }
    for (y = oy - r - 1; y <= oy + r + 1; y++)
        for (x = ox - r - 1; x <= ox + r + 1; x++)
            if (max64(abs64(x - ox), abs64(y - oy)) == r + 1 && !b_is_set(b, x, y))
                b_set(b, x, y, WgS_Wall, me);
    if (moat)
        for (y = oy - r - 2; y <= oy + r + 2; y++)
            for (x = ox - r - 2; x <= ox + r + 2; x++)
                if (max64(abs64(x - ox), abs64(y - oy)) == r + 2 && !b_is_set(b, x, y))
                    b_set(b, x, y, WgS_Water, WgO_Neutral);
    b_thing(b, 0, "HERO_GATE", ox, oy, me, 0, 1);
    for (y = oy - r; y <= oy + r; y++)
        for (x = ox - r; x <= ox + r; x++)
            if ((x != ox || y != oy) && b_is_set(b, x, y) && b->lay->slab[b_index(b, x, y)] == WgS_Claimed) {
                floor_cells[nfloor][0] = x;
                floor_cells[nfloor][1] = y;
                nfloor++;
            }
    rng_shuffle(rng, floor_cells, nfloor, sizeof floor_cells[0]);
    count = min64(3 + (int64_t)(b->distance / 100.0) + rng_randint(rng, 0, 1) + (b->biome == WgB_Ruins ? 2 : 0),
                  11);
    npool = make_pool(hero_types, COUNT_OF(hero_types), b->distance, pool_names, pool_w);
    lvl = level_for(b->distance, 120, 8);
    for (i = 0; i < count && i < nfloor; i++) {
        const char *name = pool_names[rng_weighted(rng, pool_w, npool)];
        ref_count(ref, name);
        b_creature(b, name, floor_cells[i][0], floor_cells[i][1], me, lvl + rng_randint(rng, 0, 1));
    }
    nloot = rng_randint(rng, 2, 3);
    for (i = count; i < count + nloot && i < nfloor; i++)
        b_obj(b, loot[rng_randint(rng, 0, 2)], floor_cells[i][0], floor_cells[i][1], WgO_Neutral);
    b_blob(b, ox, oy, (double)r + 3.5, 1.0, BLOB_SOFT, 0.0);
    ref->gate_x = ox;
    ref->gate_y = oy;
    ref->size = (int32_t)(2 * r + 3);
    ref->moat = (uint8_t)moat;
}

static void build_portal(struct Builder *b)
{
    struct Rng *rng = &b->rng;
    const int64_t ox = b->ox, oy = b->oy;
    int64_t n, i, a, len;
    b_rect(b, ox - 2, oy - 2, ox + 2, oy + 2, WgS_Path, WgO_Neutral);
    b_rect(b, ox - 1, oy - 1, ox + 1, oy + 1, WgS_Portal, WgO_Neutral);
    n = rng_randint(rng, 1, 2);
    for (i = 0; i < n; i++) {
        int d = (int)rng_randint(rng, 0, 3);
        len = rng_randint(rng, 1, 3);
        for (a = 3; a < 3 + len; a++)
            b_set(b, ox + dir_x[d] * a, oy + dir_y[d] * a, WgS_Path, WgO_Neutral);
    }
    b_blob(b, ox, oy, 4.5, 1.0, BLOB_SOFT, 0.0);
    b->lay->ref.portal_x = ox;
    b->lay->ref.portal_y = oy;
}

static void build_reward(struct Builder *b)
{
    static const int kind_w[3] = {3, 3, 4};
    static const char *const hoard[4] = {"GOLD_CHEST", "GOLD", "GOLDL", "GOLD_BAG"};
    struct Rng *rng = &b->rng;
    struct WgStructRef *ref = &b->lay->ref;
    const int64_t ox = b->ox, oy = b->oy;
    int64_t cave[64][2], spots[64][2], n, i;
    int ncave = 0, kind, gx, gy, has_origin = 0, npool, pool_w[16], k;
    const char *pool_names[16];
    double r;

    kind = rng_weighted(rng, kind_w, 3);
    r = 1.6 + rng_random(rng) * 0.8;
    b_blob(b, ox, oy, r, 0.5, BLOB_PATH, 0.0);
    /* sorted(p for p, op in cells.items() if op[1] == PATH): x, then y */
    for (gx = 0; gx < b->side; gx++)
        for (gy = 0; gy < b->side; gy++) {
            size_t li = (size_t)gy * (size_t)b->side + (size_t)gx;
            if (b->lay->op[li] == OP_SET && b->lay->slab[li] == WgS_Path && ncave < 63) {
                cave[ncave][0] = b->bx1 + gx;
                cave[ncave][1] = b->by1 + gy;
                if (cave[ncave][0] == ox && cave[ncave][1] == oy)
                    has_origin = 1;
                ncave++;
            }
        }
    b_set(b, ox, oy, WgS_Path, WgO_Neutral);
    if (!has_origin) {
        cave[ncave][0] = ox;
        cave[ncave][1] = oy;
        ncave++;
    }
    if (kind == 0) { /* special_box */
        const char *box;
        npool = make_pool(special_boxes, COUNT_OF(special_boxes), b->distance, pool_names, pool_w);
        box = pool_names[rng_weighted(rng, pool_w, npool)];
        b_obj(b, box, ox, oy, WgO_Neutral);
        strncpy(ref->box, box, WG_NAME_LEN - 1);
    } else if (kind == 1) { /* neutral_creatures */
        npool = make_biome_pool(neutral_creatures, COUNT_OF(neutral_creatures), b->distance, b->biome, pool_names,
                                pool_w);
        memcpy(spots, cave, sizeof cave[0] * (size_t)ncave);
        rng_shuffle(rng, spots, ncave, sizeof spots[0]);
        n = rng_randint(rng, 1, 3);
        for (i = 0; i < n && i < ncave; i++) {
            const char *name = pool_names[rng_weighted(rng, pool_w, npool)];
            ref_count(ref, name);
            b_creature(b, name, spots[i][0], spots[i][1], WgO_Neutral,
                       level_for(b->distance, 100, 7) + rng_randint(rng, 0, 1));
        }
    } else { /* gold_hoard */
        b_blob(b, ox, oy, r + 1.3, 0.4, BLOB_RING, 0.0);
        memcpy(spots, cave, sizeof cave[0] * (size_t)ncave);
        rng_shuffle(rng, spots, ncave, sizeof spots[0]);
        n = rng_randint(rng, 2, 4);
        for (i = 0; i < n && i < ncave; i++) {
            k = (int)rng_randint(rng, 0, 3);
            b_obj(b, hoard[k], spots[i][0], spots[i][1], WgO_Neutral);
        }
    }
    b_blob(b, ox, oy, r + 2.6, 0.6, BLOB_SOFT, 0.0);
    ref->reward = (uint8_t)(kind + 1);
}

/******************************************************************************/
/* Structures: placement and caches                                           */
/******************************************************************************/

static int overlap(int64_t ax1, int64_t ay1, int64_t ax2, int64_t ay2, int64_t bx1, int64_t by1, int64_t bx2,
                   int64_t by2)
{
    return ax1 <= bx2 && bx1 <= ax2 && ay1 <= by2 && by1 <= ay2;
}

static void make_cand(const struct WgWorld *w, struct Cand *c, int type, int64_t ccx, int64_t ccy, int64_t ox,
                      int64_t oy, double distance, int biome)
{
    int r = specs[type].radius;
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)type;
    c->biome = (uint8_t)biome;
    c->ccx = (int32_t)ccx;
    c->ccy = (int32_t)ccy;
    c->ox = ox;
    c->oy = oy;
    c->x1 = ox - r;
    c->y1 = oy - r;
    c->x2 = ox + r;
    c->y2 = oy + r;
    c->distance = distance;
    if (type == WgT_Start)
        c->seed = hash2(w->seed, w->spec_salt[type]);
    else
        c->seed = hash5(w->seed, w->spec_salt[type], (uint64_t)ccx, (uint64_t)ccy, 1);
}

static struct Layout *build_layout(const struct Cand *c)
{
    int side = 2 * specs[c->type].radius + 1;
    size_t ncells = (size_t)side * (size_t)side;
    size_t bytes = sizeof(struct Layout) + 3 * ncells;
    struct Layout *L = (struct Layout *)malloc(bytes);
    struct Builder b;
    if (L == NULL)
        return NULL;
    memset(L, 0, sizeof *L);
    L->st = *c;
    L->side = side;
    L->bytes = bytes;
    L->op = L->cells;
    L->slab = L->cells + ncells;
    L->own = L->cells + 2 * ncells;
    memset(L->cells, 0, 3 * ncells);
    L->ref.id.type = c->type;
    L->ref.id.start_cx = c->ccx;
    L->ref.id.start_cy = c->ccy;
    L->ref.origin_x = c->ox;
    L->ref.origin_y = c->oy;
    L->ref.bbox_x1 = c->x1;
    L->ref.bbox_y1 = c->y1;
    L->ref.bbox_x2 = c->x2;
    L->ref.bbox_y2 = c->y2;
    L->ref.distance = c->distance;
    L->ref.biome = c->biome;
    memset(&b, 0, sizeof b);
    b.lay = L;
    b.ox = c->ox;
    b.oy = c->oy;
    b.bx1 = c->x1;
    b.by1 = c->y1;
    b.bx2 = c->x2;
    b.by2 = c->y2;
    b.side = side;
    b.rng.s = c->seed;
    b.distance = c->distance;
    b.seed = c->seed;
    b.biome = c->biome;
    switch (c->type) {
    case WgT_Start: b.owner = WgO_Player; build_start(&b); break;
    case WgT_Keeper: b.owner = WgO_Keeper; build_keeper(&b); break;
    case WgT_Fortress: b.owner = WgO_Heroes; build_fortress(&b); break;
    case WgT_Portal: b.owner = WgO_Neutral; build_portal(&b); break;
    default: b.owner = WgO_Neutral; build_reward(&b); break;
    }
    return L;
}

static uint32_t key_hash(int type, int64_t a, int64_t b)
{
    return (uint32_t)(mix64(((uint64_t)type << 58) ^ ((uint64_t)a * WG_P1) ^ ((uint64_t)b * WG_P2)) >> 32);
}

static void maintain_caches(struct WgWorld *w)
{
    int i;
    if (w->cand_count > CAND_CLEAR) {
        w->cand_gen++;
        if (w->cand_gen == 0) {
            memset(w->cand, 0, sizeof(struct CandEntry) * CAND_CAP);
            w->cand_gen = 1;
        }
        w->cand_count = 0;
    }
    if (w->lay_count > LAY_CLEAR || w->lay_bytes > LAY_MAX_BYTES) {
        for (i = 0; i < LAY_CAP; i++) {
            free(w->lay[i]);
            w->lay[i] = NULL;
        }
        w->lay_count = 0;
        w->lay_bytes = 0;
    }
}

static int structs_in(struct WgWorld *w, int64_t bx1, int64_t by1, int64_t bx2, int64_t by2, unsigned mask,
                      struct Cand *out, int max, int any_only);

/** World._blocked */
static int blocked(struct WgWorld *w, const struct Cand *c)
{
    int64_t m = specs[c->type].margin;
    const struct Cand *s = &w->start;
    unsigned mask = 0;
    int t;
    if (overlap(c->x1 - m, c->y1 - m, c->x2 + m, c->y2 + m, s->x1, s->y1, s->x2, s->y2))
        return 1;
    for (t = WgT_Keeper; t < WgT_COUNT; t++)
        if (specs[t].priority > specs[c->type].priority)
            mask |= 1u << t;
    if (mask == 0)
        return 0;
    return structs_in(w, c->x1 - m, c->y1 - m, c->x2 + m, c->y2 + m, mask, NULL, 0, 1) > 0;
}

/** worldgen.BIOME_STRUCTURES: structure chance multiplier in percent */
static int biome_struct_pct(int biome, int type)
{
    if (biome == WgB_Ruins)
        return type == WgT_Fortress ? 200 : type == WgT_Reward ? 160 : 100;
    if (biome == WgB_Crystal)
        return type == WgT_Fortress ? 50 : type == WgT_Reward ? 140 : 100;
    return 100;
}

/** Spec.chance_bound: the highest chance any biome gives the type (a roll at or above it never places one). */
static double chance_bound(int type)
{
    int pct = 100, b;
    double bound;
    for (b = 0; b < WgB_COUNT; b++)
        if (biome_struct_pct(b, type) > pct)
            pct = biome_struct_pct(b, type);
    bound = specs[type].chance * (double)pct / 100.0;
    return bound < 1.0 ? bound : 1.0;
}

/** World.candidate: the structure start of one region, if any. */
static int candidate(struct WgWorld *w, int type, int64_t rx, int64_t ry, struct Cand *out)
{
    const struct Spec *sp = &specs[type];
    uint32_t i = key_hash(type, rx, ry) & (CAND_CAP - 1);
    struct CandEntry *e;
    struct Rng rng;
    struct Cand c;
    int64_t span, ccx, ccy, ox, oy;
    double roll, dist, chance;
    int exists = 0, in_range, biome = -1, rock = 0;
    for (;;) {
        e = &w->cand[i];
        if (e->gen != w->cand_gen)
            break;
        if (e->type == type && e->rx == rx && e->ry == ry) {
            if (e->exists)
                *out = e->c;
            return e->exists;
        }
        i = (i + 1) & (CAND_CAP - 1);
    }
    rng.s = hash4(w->seed, w->spec_salt[type], (uint64_t)rx, (uint64_t)ry);
    span = sp->spacing - sp->separation;
    ccx = rx * sp->spacing + rng_randint(&rng, 0, span - 1);
    ccy = ry * sp->spacing + rng_randint(&rng, 0, span - 1);
    roll = rng_random(&rng);
    ox = ccx * WG_CHUNK + rng_randint(&rng, 4, 11);
    oy = ccy * WG_CHUNK + rng_randint(&rng, 4, 11);
    memset(&c, 0, sizeof c);
    in_range = ccx >= WG_I32_MIN && ccx <= WG_I32_MAX && ccy >= WG_I32_MIN && ccy <= WG_I32_MAX;
    chance = sp->chance;
    if (in_range && roll < chance_bound(type)) {
        /* never inside impenetrable rock or sealed off by it: other places in the same chunk, else no structure */
        int tries = 1;
        rock = !reachable(w, ox, oy) || (w->version >= 7 && !fits7(w, type, ox, oy));
        while (rock && tries < STRUCTURE_TRIES) {
            ox = ccx * WG_CHUNK + rng_randint(&rng, 0, WG_CHUNK - 1);
            oy = ccy * WG_CHUNK + rng_randint(&rng, 0, WG_CHUNK - 1);
            rock = !reachable(w, ox, oy) || (w->version >= 7 && !fits7(w, type, ox, oy));
            tries++;
        }
    }
    if (in_range && roll < chance_bound(type)) {
        /* chance = min(1.0, spec.chance * pct / 100.0); the biome is a pure function of the origin, so it is
         * only computed where it can matter (BIOME_STRUCTURES has fortress and reward) or is kept */
        int pct = 100;
        if (type == WgT_Fortress || type == WgT_Reward) {
            biome = biome_at_v(w, ox, oy);
            pct = biome_struct_pct(biome, type);
        }
        chance = sp->chance * (double)pct / 100.0;
        if (!(chance < 1.0))
            chance = 1.0;
    }
    if (roll < chance && in_range && !rock) {
        dist = hypot_int(ox - START_X, oy - START_Y);
        if (dist >= (double)sp->min_dist) {
            if (biome < 0)
                biome = biome_at_v(w, ox, oy);
            make_cand(w, &c, type, ccx, ccy, ox, oy, dist, biome);
            exists = !blocked(w, &c);
        }
    }
    /* insert (blocked() may have added entries, so probe again) */
    if (w->cand_count < CAND_MAX_FILL) {
        i = key_hash(type, rx, ry) & (CAND_CAP - 1);
        while (w->cand[i].gen == w->cand_gen &&
               !(w->cand[i].type == type && w->cand[i].rx == rx && w->cand[i].ry == ry))
            i = (i + 1) & (CAND_CAP - 1);
        e = &w->cand[i];
        if (e->gen != w->cand_gen)
            w->cand_count++;
        e->gen = w->cand_gen;
        e->type = (uint8_t)type;
        e->rx = rx;
        e->ry = ry;
        e->exists = (uint8_t)exists;
        e->c = c;
    }
    if (exists)
        *out = c;
    return exists;
}

/** World.structures_in: in SPECS order, then region x, then region y. Returns the count (all of them);
 *  writes at most max. With any_only, returns 1 at the first one found. */
static int structs_in(struct WgWorld *w, int64_t bx1, int64_t by1, int64_t bx2, int64_t by2, unsigned mask,
                      struct Cand *out, int max, int any_only)
{
    int n = 0, t;
    for (t = WgT_Keeper; t < WgT_COUNT; t++) {
        const struct Spec *sp = &specs[t];
        int64_t reach = sp->radius, rx, ry, r1x, r1y, r2x, r2y;
        if (!(mask & (1u << t)))
            continue;
        r1x = fdiv(fdiv(bx1 - reach, WG_CHUNK), sp->spacing);
        r1y = fdiv(fdiv(by1 - reach, WG_CHUNK), sp->spacing);
        r2x = fdiv(fdiv(bx2 + reach, WG_CHUNK), sp->spacing);
        r2y = fdiv(fdiv(by2 + reach, WG_CHUNK), sp->spacing);
        for (rx = r1x; rx <= r2x; rx++)
            for (ry = r1y; ry <= r2y; ry++) {
                struct Cand c;
                if (candidate(w, t, rx, ry, &c) && overlap(c.x1, c.y1, c.x2, c.y2, bx1, by1, bx2, by2)) {
                    if (any_only)
                        return 1;
                    if (n < max)
                        out[n] = c;
                    n++;
                }
            }
    }
    return n;
}

/** The built layout of a structure; *temp is set when the caller has to free it. */
static struct Layout *get_layout(struct WgWorld *w, const struct Cand *c, int *temp)
{
    uint32_t i;
    struct Layout *L;
    *temp = 0;
    if (c->type == WgT_Start)
        return w->start_layout;
    i = key_hash(c->type, c->ccx, c->ccy) & (LAY_CAP - 1);
    while ((L = w->lay[i]) != NULL) {
        if (L->st.type == c->type && L->st.ccx == c->ccx && L->st.ccy == c->ccy)
            return L;
        i = (i + 1) & (LAY_CAP - 1);
    }
    L = build_layout(c);
    if (L == NULL)
        return NULL;
    if (w->lay_count < LAY_MAX_FILL) {
        w->lay[i] = L;
        w->lay_count++;
        w->lay_bytes += L->bytes;
    } else {
        *temp = 1;
    }
    return L;
}

static int fmt_int(char *p, int64_t v)
{
    char tmp[24];
    int n = 0, k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    if (v < 0)
        p[k++] = '-';
    do {
        tmp[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    while (n > 0)
        p[k++] = tmp[--n];
    p[k] = '\0';
    return k;
}

/** Structure.id: "<type>@<cx>,<cy>" */
static void make_id(char *buf, const struct Cand *c)
{
    size_t n = strlen(type_names[c->type]);
    memcpy(buf, type_names[c->type], n);
    buf[n++] = '@';
    n += (size_t)fmt_int(buf + n, c->ccx);
    buf[n++] = ',';
    fmt_int(buf + n, c->ccy);
}

static int same_id(const struct WgStructId *a, const struct WgStructId *b)
{
    return a->type == b->type && a->start_cx == b->start_cx && a->start_cy == b->start_cy;
}

/******************************************************************************/
/* API                                                                        */
/******************************************************************************/

struct WgWorld *wg_world_create(uint64_t seed)
{
    return wg_world_create_version(seed, WG_GENERATOR_VERSION);
}

int wg_world_version(const struct WgWorld *w)
{
    return w->version;
}

struct WgWorld *wg_world_create_version(uint64_t seed, int version)
{
    struct WgWorld *w = (struct WgWorld *)calloc(1, sizeof(struct WgWorld));
    int i;
    if (w == NULL)
        return NULL;
    w->cand = (struct CandEntry *)calloc(CAND_CAP, sizeof(struct CandEntry));
    w->lay = (struct Layout **)calloc(LAY_CAP, sizeof(struct Layout *));
    w->seam = (struct SeamEntry *)calloc(SEAM_CAP, sizeof(struct SeamEntry));
    w->reach = (struct ReachSet *)calloc(1, sizeof(struct ReachSet));
    if (w->cand == NULL || w->lay == NULL || w->seam == NULL || w->reach == NULL) {
        wg_world_destroy(w);
        return NULL;
    }
    w->seed = seed;
    w->cand_gen = 1;
    for (i = 0; i < N_COUNT; i++)
        w->s_noise[i] = hash2(seed, salt2("noise:", noise_names[i], strlen(noise_names[i])));
    w->s_gems = hash2(seed, salt2("feature:", "gems", 4));
    w->s_seam = hash2(seed, salt2("feature:", "goldseam", 8));
    for (i = 0; i < WgT_COUNT; i++)
        w->spec_salt[i] = salt2("structure:", type_names[i], strlen(type_names[i]));
    w->version = version >= 7 ? 7 : 6;
    if (w->version >= 7) {
        uint64_t h;
        for (i = 0; i < V7_COUNT; i++)
            w->s7[i] = hash2(seed, salt2("v7:", v7_names[i], strlen(v7_names[i])));
        /* Dead Man's Land round the start: 1 seed in 100 */
        h = w->s7[V7_DMLSTART];
        if (unit7(h) < 0.01) {
            w->dml_start = 1;
            h = mix64(h + WG_GOLDEN);
            w->dml_r = 500.0 + unit7(h) * 750.0;
            h = mix64(h + WG_GOLDEN);
            w->dml_cx = (double)START_X + (unit7(h) - 0.5) * w->dml_r;
            h = mix64(h + WG_GOLDEN);
            w->dml_cy = (double)START_Y + (unit7(h) - 0.5) * w->dml_r;
        }
    }
    make_cand(w, &w->start, WgT_Start, 0, 0, START_X, START_Y, hypot_int(0, 0), biome_at_v(w, START_X, START_Y));
    w->start_layout = build_layout(&w->start);
    if (w->start_layout == NULL) {
        wg_world_destroy(w);
        return NULL;
    }
    return w;
}

void wg_world_destroy(struct WgWorld *w)
{
    int i;
    if (w == NULL)
        return;
    if (w->lay != NULL)
        for (i = 0; i < LAY_CAP; i++)
            free(w->lay[i]);
    free(w->lay);
    free(w->cand);
    if (w->seam != NULL)
        for (i = 0; i < SEAM_CAP; i++)
            free(w->seam[i].cells);
    free(w->seam);
    free(w->reach);
    free(w->start_layout);
    free(w);
}

uint64_t wg_world_seed(const struct WgWorld *w)
{
    return w->seed;
}

int wg_generate_chunk(struct WgWorld *w, int32_t cx, int32_t cy, struct WgChunk *out)
{
    struct Cand found[MAX_FOUND];
    char ids[MAX_FOUND][40];
    int order[MAX_FOUND];
    uint8_t kref[WG_CHUNK_CELLS]; /* 1 + index in out->structs of the keeper whose cell it is */
    const struct Cand *s = &w->start;
    int64_t cx0 = (int64_t)cx * WG_CHUNK, cy0 = (int64_t)cy * WG_CHUNK, x, y;
    int n, i, j, k, err = 0, have_keeper = 0;
    struct WgStructId keeper;

    maintain_caches(w);
    memset(out, 0, sizeof *out);
    out->seed = w->seed;
    out->cx = cx;
    out->cy = cy;
    /* stages 0-3: climate and biome, terrain */
    for (y = 0; y < WG_CHUNK; y++)
        for (x = 0; x < WG_CHUNK; x++) {
            double clim[4];
            int biome;
            if (w->version >= 7) {
                struct Clim7 c7;
                climate7(w, cx0 + x, cy0 + y, &c7);
                out->biomes[y * WG_CHUNK + x] = (uint8_t)biome7_of(&c7);
                out->slabs[y * WG_CHUNK + x] = terrain7(w, cx0 + x, cy0 + y, &c7);
                continue;
            }
            climate(w, cx0 + x, cy0 + y, clim);
            biome = biome_of(clim);
            out->biomes[y * WG_CHUNK + x] = (uint8_t)biome;
            out->slabs[y * WG_CHUNK + x] = terrain(w, cx0 + x, cy0 + y, clim);
        }
    place_gems(w, out, cx0, cy0);
    /* stage 4: World.structures_touching, sorted by (priority, id) */
    n = structs_in(w, cx0, cy0, cx0 + WG_CHUNK - 1, cy0 + WG_CHUNK - 1, PLACED_MASK, found, MAX_FOUND - 1, 0);
    if (n > MAX_FOUND - 1)
        return -1;
    if (overlap(s->x1, s->y1, s->x2, s->y2, cx0, cy0, cx0 + WG_CHUNK - 1, cy0 + WG_CHUNK - 1))
        found[n++] = *s;
    if (n > WG_MAX_CHUNK_STRUCTS)
        return -1;
    for (i = 0; i < n; i++) {
        make_id(ids[i], &found[i]);
        order[i] = i;
    }
    for (i = 1; i < n; i++) {
        int v = order[i];
        for (j = i; j > 0; j--) {
            int u = order[j - 1], pu = specs[found[u].type].priority, pv = specs[found[v].type].priority;
            if (pu < pv || (pu == pv && strcmp(ids[u], ids[v]) <= 0))
                break;
            order[j] = u;
        }
        order[j] = v;
    }
    memset(kref, 0, sizeof kref);
    memset(&keeper, 0, sizeof keeper);
    for (k = 0; k < n; k++) {
        const struct Cand *c = &found[order[k]];
        int temp;
        struct Layout *L = get_layout(w, c, &temp);
        int64_t ix1, iy1, ix2, iy2;
        if (L == NULL)
            return -1;
        if (L->overflow)
            err = 1;
        ix1 = max64(cx0, c->x1);
        iy1 = max64(cy0, c->y1);
        ix2 = min64(cx0 + WG_CHUNK - 1, c->x2);
        iy2 = min64(cy0 + WG_CHUNK - 1, c->y2);
        for (y = iy1; y <= iy2; y++)
            for (x = ix1; x <= ix2; x++) {
                size_t li = (size_t)((y - c->y1) * L->side + (x - c->x1));
                int ci = (int)((y - cy0) * WG_CHUNK + (x - cx0));
                if (L->op[li] == OP_SET) {
                    if ((L->slab[li] == WgS_Gold || L->slab[li] == WgS_DenseGold) && out->slabs[ci] == WgS_Rock)
                        continue; /* a structure's gold never goes into impenetrable rock */
                    out->slabs[ci] = L->slab[li];
                    out->owners[ci] = L->own[li];
                    kref[ci] = (uint8_t)(L->own[li] == WgO_Keeper ? k + 1 : 0);
                } else if (L->op[li] == OP_SOFT) {
                    uint8_t sl = out->slabs[ci];
                    if (sl == WgS_Rock || sl == WgS_Lava || sl == WgS_Water) {
                        out->slabs[ci] = WgS_Earth;
                        out->owners[ci] = WgO_Neutral;
                        kref[ci] = 0;
                    }
                }
            }
        for (i = 0; i < L->nthings; i++) {
            const struct LThing *t = &L->things[i];
            struct WgThing *o;
            if (fdiv(t->sx, WG_CHUNK) != cx || fdiv(t->sy, WG_CHUNK) != cy)
                continue;
            if (out->thing_count >= WG_MAX_CHUNK_THINGS) {
                err = 1;
                break;
            }
            o = &out->things[out->thing_count++];
            o->is_creature = t->is_creature;
            strncpy(o->name, t->name, WG_NAME_LEN - 1);
            o->owner = t->owner;
            if (t->owner == WgO_Keeper) {
                o->keeper = L->ref.id;
                if (have_keeper && !same_id(&keeper, &o->keeper))
                    err = 1;
                keeper = o->keeper;
                have_keeper = 1;
            }
            o->x = t->x;
            o->y = t->y;
            o->level = t->level;
            o->has_gate = t->has_gate;
            if (t->has_gate)
                o->gate = L->ref.id;
        }
        out->structs[k] = L->ref;
        if (temp)
            free(L);
    }
    out->struct_count = n;
    /* space round the start dungeon, whatever put it there: no gold or dense gold within START_GOLD_FREE and no
     * dirt path within START_PATH_FREE (both become earth) */
    if (overlap(cx0, cy0, cx0 + WG_CHUNK - 1, cy0 + WG_CHUNK - 1, START_X - START_PATH_FREE, START_Y - START_PATH_FREE,
            START_X + START_PATH_FREE, START_Y + START_PATH_FREE))
        for (i = 0; i < WG_CHUNK_CELLS; i++) {
            int64_t dx = cx0 + i % WG_CHUNK - START_X, dy = cy0 + i / WG_CHUNK - START_Y, d;
            uint8_t slab = out->slabs[i];
            if (dx < 0)
                dx = -dx;
            if (dy < 0)
                dy = -dy;
            d = (dx > dy) ? dx : dy;
            if (((slab == WgS_Gold || slab == WgS_DenseGold) && d <= START_GOLD_FREE) ||
                    (slab == WgS_Path && d <= START_PATH_FREE))
                out->slabs[i] = WgS_Earth;
        }
    for (i = 0; i < WG_CHUNK_CELLS; i++)
        if (out->owners[i] == WgO_Keeper) {
            const struct WgStructId *id = &out->structs[kref[i] - 1].id;
            if (have_keeper && !same_id(&keeper, id))
                err = 1;
            keeper = *id;
            have_keeper = 1;
        }
    out->has_keeper = (uint8_t)have_keeper;
    out->keeper = keeper;
    return err ? -1 : 0;
}

int wg_structures_in(struct WgWorld *w, int64_t x1, int64_t y1, int64_t x2, int64_t y2, unsigned type_mask,
                     struct WgStructRef *out, int max)
{
    struct Cand *buf = NULL;
    int n, i;
    maintain_caches(w);
    x1 = max64(-BOX_LIMIT, min64(BOX_LIMIT, x1));
    y1 = max64(-BOX_LIMIT, min64(BOX_LIMIT, y1));
    x2 = max64(-BOX_LIMIT, min64(BOX_LIMIT, x2));
    y2 = max64(-BOX_LIMIT, min64(BOX_LIMIT, y2));
    if (out == NULL || max < 0)
        max = 0;
    if (max > 0) {
        buf = (struct Cand *)malloc(sizeof(struct Cand) * (size_t)max);
        if (buf == NULL)
            return -1;
    }
    n = structs_in(w, x1, y1, x2, y2, type_mask & PLACED_MASK, buf, max, 0);
    for (i = 0; i < n && i < max; i++) {
        int temp;
        struct Layout *L = get_layout(w, &buf[i], &temp);
        if (L == NULL) {
            free(buf);
            return -1;
        }
        out[i] = L->ref;
        if (temp)
            free(L);
    }
    free(buf);
    return n;
}

void wg_start_ref(struct WgWorld *w, struct WgStructRef *out)
{
    *out = w->start_layout->ref;
}

const char *wg_slab_name(int slab)
{
    return (slab >= 0 && slab < WgS_COUNT) ? slab_names[slab] : "?";
}

const char *wg_struct_type_name(int type)
{
    return (type >= 0 && type < WgT_COUNT) ? type_names[type] : "?";
}

int wg_biome_at(const struct WgWorld *w, int64_t x, int64_t y)
{
    return biome_at_v(w, x, y);
}

const char *wg_biome_name(int biome)
{
    return (biome >= 0 && biome < WgB_COUNT) ? biome_names[biome] : "?";
}

int wg_biome_texture(int biome)
{
    return (biome >= 0 && biome < WgB_COUNT) ? biome_texture[biome] : biome_texture[WgB_Earthen];
}

/******************************************************************************/
/** @file world_stream.c
 *     Endless world: a fixed-size map window that slides over a generated world.
 * @par Purpose:
 *     See world_stream.h. The window is WORLD_WINDOW_CHUNKS x WORLD_WINDOW_CHUNKS chunks of 16x16 slabs
 *     plus a one-slab rock frame: engine slab (ex, ey) is world slab (16*origin_cx + ex - 1, ...).
 *     Chunks that leave the window are frozen into the chunk store (every chunk that was ever loaded is
 *     stored; only never-loaded chunks come from the generator). The minimap draws dropped chunks from the
 *     map memory: the panel colour codes the chunk had when it left.
 */
/******************************************************************************/
#include "pre_inc.h"
#include <limits.h>
#include "world_stream.h"
#include "world_shift.h"
#include "world_gen.h"
#include "world_daynight.h"
#include "world_wild.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <zlib.h>

#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "game_legacy.h"
#include "globals.h"
#include "map_data.h"
#include "map_blocks.h"
#include "map_columns.h"
#include "slab_data.h"
#include "room_data.h"
#include "room_util.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_creature.h"
#include "thing_doors.h"
#include "thing_traps.h"
#include "thing_factory.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_trapdoor.h"
#include "config_terrain.h"
#include "config_rules.h"
#include "dungeon_data.h"
#include "player_data.h"
#include "player_utils.h"
#include "player_computer.h"
#include "player_instances.h"
#include "player_complookup.h"
#include "power_hand.h"
#include "frontmenu_ingame_map.h"
#include "kfx/save/SaveManager.h"
#include "local_camera.h"
#include "gui_parchment.h"
#include "engine_camera.h"
#include "packets.h"
#include "tasks_list.h"
#include "world_pages.h"
#include "world_menu.h"
#include "replay.h"
#include "kfx/save/core/save_codec.h"
#include "kfx/save/core/save_schema.h"
#include "kfx/save/core/schema/save_tables_decl.h"
#include "map_ceiling.h"
#include "ariadne_update.h"
#include "ariadne.h"
#include "ariadne_tringls.h"
#include "ariadne_points.h"
#include "light_data.h"
#include "keeperfx.hpp"
#include "thing_navigate.h"
#include "room_lair.h"
#include "net_game.h"
#include "world_colours.h"
#include "scrcapt.h"
#include "room_workshop.h"
#include "config_magic.h"
#include "creature_states_lair.h"
#include "net_state.h"
#include "world_progress.h"
#include "world_outpost.h"
#include "net_spectator.h"
#include "roomspace_input.h"
#include "frontmenu_ingame_tabs.h"
#include "frontend.h"
#include "gui_frontmenu.h"
#include "creature_instances.h"
#include "thing_corpses.h"
#include "creature_jobs.h"
#include "room_jobs.h"
#include "creature_states_hero.h"
#include "server_admin.h"
#include "net_exchange_common.h"
#include "post_inc.h"

/******************************************************************************/
#define WORLD_MFLG_DEADBACKTOPOOL 0x08 /* MFlg_DeadBackToPool, keeperfx.hpp */

/* map_blocks.c, not in its header */
void remove_block_from_map_element(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
void place_single_slab_type_on_map(SlabKind slbkind, MapSlabCoord slb_x, MapSlabCoord slb_y, PlayerNumber plyr_idx);
void delete_attached_lights_on_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);
#define WORLD_MAX_KEEPERS 256 /* keepers on record (met, woken or destroyed); a world of 100 players meets many */
#define CHUNK_STL WORLD_CHUNK_SUBTILES
#define CHUNK_STL_CELLS (CHUNK_STL * CHUNK_STL)
#define WORLD_REC_MAGIC_V1 0x314B4357u /* "WCK1": no slab textures */
#define WORLD_REC_MAGIC_V2 0x324B4357u /* "WCK2": with slab textures */
#define WORLD_REC_MAGIC_V3 0x334B4357u /* "WCK3": with slab textures and dig orders */
#define WORLD_REC_MAGIC_V4 0x344B4357u /* "WCK4": revealed flags and dig orders, 16 bits (players 0-15) */
#define WORLD_REC_MAGIC 0x354B4357u /* "WCK5": revealed flags and dig orders as player sets of n 32-bit words */
#define WORLD_BLOB_MAGIC 0x31444C57u /* "WLD1" */
#define WORLD_PORTALS_MAGIC 0x31545250u /* "PRT1": optional section after the chunk tables */
#define WORLD_PAGES_MAGIC 0x31534750u /* "PGS1": paged mode only, after the portals: the loaded chunk pages (raw) */
#define WORLD_PAGES2_MAGIC 0x32534750u /* "PGS2": the same, field by field through the save schema */
#define WORLD_PROGRESS_MAGIC 0x31475250u /* "PRG1": optional: the research-book progression */
#define WORLD_STAMPS_MAGIC 0x314D5453u /* "STM1": optional: the start dungeons of the other human seats */
#define WORLD_EXTRA_MAGIC 0x31525458u /* "XTR1": optional, last: lair waits and keepers' last-near turns */
#define WORLD_KEYS_MAGIC 0x31594B50u /* "PKY1": optional, last: each seat's owner (its reconnect token), the server's */
#define WORLD_HEARTS_MAGIC 0x315A4648u /* "HFZ1": optional, after the progression: heart-frozen bits of players 8-15 */
#define WORLD_HEARTS2_MAGIC 0x325A4648u /* "HFZ2": in its place: the players with frozen hearts (u8 words, the words) */
#define WORLD_LEVELS_MAGIC 0x31564C50u /* "PLV1": optional, before the seats' owners: the players' XP (player levels) */
#define WORLD_OUTPOSTS_MAGIC 0x3153504Fu /* "OPS1": optional, after PLV1: what happened to the heroes outposts */
#define WORLD_STARTGOLD_MAGIC 0x31594D53u /* "SMY1": optional, after OPS1: the gold seats that join get */
#define WORLD_STATS_MAGIC 0x31545350u /* "PST1": optional, after PKY1 (last): the players' records (server console) */
#define WORLD_MAX_PORTALS 65536
/* The player's creature limit (imps don't count, as in stock): this many, plus this many per owned portal. */
#define WORLD_BASE_CREATURES 10
/* Chunked world: how far a keeper's threat radius (at most 56 slabs from its heart) reaches beyond its box (24) */
#define WORLD_KEEPER_REACH 40
#define WORLD_CREATURES_PER_PORTAL 5

/** World owner ids, as stored in chunk records (colours are only assigned while loaded). */
enum WorldOwner {
    WOwn_Neutral = 0,
    WOwn_Player  = 1, /* PLAYER0, the level's own player */
    WOwn_Heroes  = 2,
    WOwn_Keeper0 = 3, /* WOwn_Keeper0 + index in the keeper list */
    WOwn_HumanBase = 0xF0, /* WOwn_HumanBase + player number: other human players (network games) */
};

enum WorldKeeperState {
    WKSt_Waiting = 0,  /* known, no colour (none was free) */
    WKSt_Passive,      /* has a colour; its AI doesn't run */
    WKSt_Active,       /* the player came within its threat radius */
    WKSt_Destroyed,
};

struct WorldKeeper {
    struct WgStructId id;
    int64_t heart_x, heart_y;          /* world slabs */
    int64_t bbox_x1, bbox_y1, bbox_x2, bbox_y2;
    int32_t threat_radius;
    int32_t money;
    int8_t slot;                       /* player number, -1 when none */
    uint8_t state;
    uint8_t set_up;                    /* made a computer player */
    uint8_t heart_frozen;
};

struct ChunkSlot {
    int32_t cx, cy;
    uint8_t used;
    uint32_t len;
    uint8_t *data;
};

struct ChunkTable {
    struct ChunkSlot *slots;
    uint32_t cap;
    uint32_t count;
};

/** A portal the player has seen: who owned it when last in the window (it keeps that owner while frozen). */
struct WorldPortal {
    int64_t x, y;                      /* world slab of the room's centre */
    int8_t owner;                      /* engine player number, -1: nobody's */
    uint8_t seen;                      /* scratch for update_portals */
    int8_t last_owner;                 /* the last player who owned it (not saved): a capture is a new one */
};

/** A creature whose lair is on ground that isn't loaded (paged mode): it gets the lair back when the ground loads. */
struct WorldLairWait {
    int32_t thing_idx;
    int32_t creation_turn;             /* tells the creature from a later thing in the same slot */
    MapSubtlCoord stl_x, stl_y;        /* engine subtile of the lair */
};
#define WORLD_LAIR_WAITS_MAX 256

/** A copy of the start structure for another human seat (network games): world slabs moved by (dx, dy). */
struct WorldStamp {
    int32_t plyr;
    int64_t dx, dy;
};
#define WORLD_MAX_STAMPS 16
/* each copy takes the ground within this many slabs of the start's heart (the rooms, the portal and the gold-free
   ground round them) */
#define WORLD_STAMP_RADIUS 24

struct WorldState {
    TbBool pending;
    TbBool active;
    char seed_text[64];
    uint64_t seed;
    struct WgWorld *gen;
    int32_t origin_cx, origin_cy;      /* world chunk at window chunk (0,0) */
    struct ChunkTable store;           /* frozen chunk records (zlib) */
    struct ChunkTable memory;          /* map memory: CHUNK_STL_CELLS u16 panel codes, raw */
    struct WorldKeeper keepers[WORLD_MAX_KEEPERS];
    int keeper_count;
    PlayerBitFlags player_heart_frozen; /* the human players whose heart is in a frozen chunk */
    GameTurn last_shift_turn;
    long shifts;
    int64_t home_x, home_y;            /* world slab of the player's heart */
    struct WorldPortal *portals;
    int portal_count, portal_cap;
    TbBool rebind_hearts;              /* set after loading a save: bind_hearts on the next turn */
    struct WorldLairWait lair_waits[WORLD_LAIR_WAITS_MAX]; /* not saved: a save drops lairs still waiting */
    GameTurn keeper_last_near[WORLD_MAX_KEEPERS]; /* last turn a human was near an awake keeper (not saved) */
    struct WorldStamp stamps[WORLD_MAX_STAMPS];
    int stamp_count;
    GoldAmount start_money;              /* PLAYER0's gold at the first turn: seats that join later get it */
    int lair_wait_count;
};

static struct WorldState W;
/* each seat's owner: its player's reconnect token (0: none yet, or a world from before). The server's own: not in W
   or any streamed memory, so no client can read another player's token */
static uint64_t seat_tokens[PLAYERS_MAX];

uint64_t world_seat_token(PlayerNumber plyr_idx)
{
    return ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX)) ? seat_tokens[plyr_idx] : 0;
}

void world_set_seat_token(PlayerNumber plyr_idx, uint64_t token)
{
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        seat_tokens[plyr_idx] = token;
}
/* bumped whenever a map memory record is written, replaced or freed (world_minimap_code caches a record pointer) */
static uint32_t world_memory_gen = 0;

/******************************************************************************/
/* Chunk tables (open addressing, keyed by world chunk) */

static uint32_t chunk_hash(int32_t cx, int32_t cy)
{
    uint64_t h = ((uint64_t)(uint32_t)cx * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)(uint32_t)cy * 0xC2B2AE3D27D4EB4FULL);
    h ^= h >> 29;
    return (uint32_t)h;
}

static void table_free(struct ChunkTable *t)
{
    for (uint32_t i = 0; i < t->cap; i++)
        free(t->slots[i].data);
    free(t->slots);
    memset(t, 0, sizeof(*t));
}

static struct ChunkSlot *table_find(const struct ChunkTable *t, int32_t cx, int32_t cy)
{
    if (t->cap == 0)
        return NULL;
    uint32_t i = chunk_hash(cx, cy) & (t->cap - 1);
    while (t->slots[i].used)
    {
        if ((t->slots[i].cx == cx) && (t->slots[i].cy == cy))
            return &t->slots[i];
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}

static struct ChunkSlot *table_insert(struct ChunkTable *t, int32_t cx, int32_t cy);

static TbBool table_grow(struct ChunkTable *t)
{
    struct ChunkTable n;
    n.cap = (t->cap == 0) ? 256 : t->cap * 2;
    n.count = 0;
    n.slots = (struct ChunkSlot *)calloc(n.cap, sizeof(struct ChunkSlot));
    if (n.slots == NULL)
        return false;
    for (uint32_t i = 0; i < t->cap; i++)
    {
        if (!t->slots[i].used)
            continue;
        struct ChunkSlot *s = table_insert(&n, t->slots[i].cx, t->slots[i].cy);
        s->len = t->slots[i].len;
        s->data = t->slots[i].data;
    }
    free(t->slots);
    *t = n;
    return true;
}

static struct ChunkSlot *table_insert(struct ChunkTable *t, int32_t cx, int32_t cy)
{
    struct ChunkSlot *s = table_find(t, cx, cy);
    if (s != NULL)
        return s;
    if ((t->count + 1) * 2 > t->cap)
    {
        if (!table_grow(t))
            return NULL;
    }
    uint32_t i = chunk_hash(cx, cy) & (t->cap - 1);
    while (t->slots[i].used)
        i = (i + 1) & (t->cap - 1);
    s = &t->slots[i];
    s->used = 1;
    s->cx = cx;
    s->cy = cy;
    s->len = 0;
    s->data = NULL;
    t->count++;
    return s;
}

/******************************************************************************/
/* Coordinates */

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/** Engine slab of the first slab of a window chunk. */
static MapSlabCoord window_chunk_slab(int lc)
{
    if (world_paged) /* chunks at their world positions; the origin is the same on both axes */
        return (MapSlabCoord)(WP_ORIGIN_SLB + (int64_t)(W.origin_cx + lc) * WORLD_CHUNK_SLABS);
    return (MapSlabCoord)(1 + lc * WORLD_CHUNK_SLABS);
}

static int64_t engine_slab_x(int64_t wx) { return world_paged ? wx + WP_ORIGIN_SLB : wx - (int64_t)W.origin_cx * WORLD_CHUNK_SLABS + 1; }
static int64_t world_slab_x_of(int64_t ex) { return world_paged ? ex - WP_ORIGIN_SLB : (int64_t)W.origin_cx * WORLD_CHUNK_SLABS + ex - 1; }
static int64_t world_slab_y_of(int64_t ey) { return world_paged ? ey - WP_ORIGIN_SLB : (int64_t)W.origin_cy * WORLD_CHUNK_SLABS + ey - 1; }
static int64_t engine_slab_y(int64_t wy) { return world_paged ? wy + WP_ORIGIN_SLB : wy - (int64_t)W.origin_cy * WORLD_CHUNK_SLABS + 1; }

/** New runs play the chunked world. A replay plays the world it was
 *  recorded on (replays from before have no flag: the sliding window). Test hook: KFX_WORLD_PAGED=0 starts new
 *  runs in the sliding window instead, as saves from before the chunked world still play. */
static TbBool world_paged_requested(void)
{
    if (replay.load_enable)
        return flag_is_set(replay.head.flags, PSHF_ChunkedWorld);
    const char *env = getenv("KFX_WORLD_PAGED");
    return (env == NULL) || (atoi(env) != 0);
}

/******************************************************************************/
/* Owners */

static int keeper_index(const struct WgStructId *id)
{
    for (int i = 0; i < W.keeper_count; i++)
    {
        if ((W.keepers[i].id.start_cx == id->start_cx) && (W.keepers[i].id.start_cy == id->start_cy))
            return i;
    }
    return -1;
}

static TbBool slot_in_use(PlayerNumber plyr_idx)
{
    for (int i = 0; i < W.keeper_count; i++)
        if ((W.keepers[i].slot == plyr_idx) && (W.keepers[i].state != WKSt_Destroyed))
            return true;
    return false;
}

/* The players the world's keepers take, in order. A single-player run: 1-3 and 6-8 as before, then 10-15. A network
   world leaves 1-3 to human seats and starts at 8, which no seat takes, then the high numbers, so the seats (given
   out from the low end) and the keepers meet in the middle. Player 9 is never a player (ALL_PLAYERS). Worlds without
   players 10-15 (older saves and replays) keep the lists they had. */
static const PlayerNumber keeper_slots_9[] = {1, 2, 3, 6, 7, 8};
/* with more players: 1-3, 6-8, then 10 up (single player); 8, then the highest down to 10, then 7 and 6 (network) */
static PlayerNumber keeper_slots_ext[PLAYERS_MAX];
static PlayerNumber keeper_slots_mp_ext[PLAYERS_MAX];

static size_t keeper_slots_extended(TbBool network, const PlayerNumber **slots)
{
    const int count = players_in_use();
    size_t n = 0;
    if (network)
    {
        keeper_slots_mp_ext[n++] = 8;
        for (int p = count - 1; p >= 10; p--)
            keeper_slots_mp_ext[n++] = (PlayerNumber)p;
        keeper_slots_mp_ext[n++] = 7;
        keeper_slots_mp_ext[n++] = 6;
        *slots = keeper_slots_mp_ext;
    } else
    {
        static const PlayerNumber first[] = {1, 2, 3, 6, 7, 8};
        for (size_t i = 0; i < sizeof(first) / sizeof(first[0]); i++)
            keeper_slots_ext[n++] = first[i];
        for (int p = 10; p < count; p++)
            keeper_slots_ext[n++] = (PlayerNumber)p;
        *slots = keeper_slots_ext;
    }
    return n;
}

/* The players human seats take in a network world, in order: 0-3, then 6 and 7, then 10 up (when in use), each when
   no world keeper holds it. Player 8 is left to the world's keepers, so a keeper that wakes finds a colour. */
static const PlayerNumber world_seats_9[] = {0, 1, 2, 3, 6, 7};
static PlayerNumber world_seats_ext[PLAYERS_MAX];

int world_seat_players(const PlayerNumber **seats)
{
    if (game.players_extended)
    {
        int n = 0;
        for (size_t i = 0; i < sizeof(world_seats_9) / sizeof(world_seats_9[0]); i++)
            world_seats_ext[n++] = world_seats_9[i];
        for (int p = 10; p < players_in_use(); p++)
            world_seats_ext[n++] = (PlayerNumber)p;
        /* test hook KFX_WORLD_SEATSHIGH=1: seats after the first from the highest player down (players 16+ in a
           test with a few windows) */
        static int high = -1;
        if (high < 0)
            high = (getenv("KFX_WORLD_SEATSHIGH") != NULL) ? 1 : 0;
        if (high)
        {
            for (int i = 1, j = n - 1; i < j; i++, j--)
            {
                const PlayerNumber t = world_seats_ext[i];
                world_seats_ext[i] = world_seats_ext[j];
                world_seats_ext[j] = t;
            }
        }
        *seats = world_seats_ext;
        return n;
    }
    *seats = world_seats_9;
    return (int)(sizeof(world_seats_9) / sizeof(world_seats_9[0]));
}

/** Every player a human seat can take. */
static PlayerBitFlags world_seat_mask(void)
{
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    PlayerBitFlags mask = pmask_none();
    for (int i = 0; i < n; i++)
        pmask_add(&mask, seats[i]);
    return mask;
}

/** Gives the keeper a colour if it has none. Returns its player number, or neutral. */
static PlayerNumber keeper_take_slot(int k)
{
    struct WorldKeeper *wk = &W.keepers[k];
    if (wk->slot >= 0)
        return wk->slot;
    if (wk->state == WKSt_Destroyed)
        return game.neutral_player_num;
    const PlayerNumber *slots = keeper_slots_9;
    size_t nslots = sizeof(keeper_slots_9) / sizeof(keeper_slots_9[0]);
    if (game.players_extended)
        nslots = keeper_slots_extended(game.game_kind == GKind_MultiGame, &slots);
    for (size_t i = 0; i < nslots; i++)
    {
        PlayerNumber p = slots[i];
        if (slot_in_use(p))
            continue;
        /* a network game: players 1-3 are human seats (the seats are settled after the first chunks are filled) */
        if ((game.game_kind == GKind_MultiGame) && (p <= 3))
            continue;
        struct PlayerInfo *player = get_player(p);
        if (player_exists(player))
            continue;
        wk->slot = p;
        wk->state = WKSt_Passive;
        SYNCLOG("World: keeper@%d,%d takes player %d", (int)wk->id.start_cx, (int)wk->id.start_cy, (int)p);
        return p;
    }
    WARNLOG("World: no free colour for keeper@%d,%d, its things are neutral", (int)wk->id.start_cx, (int)wk->id.start_cy);
    return game.neutral_player_num;
}

static int keeper_register(const struct WgStructRef *ref)
{
    int k = keeper_index(&ref->id);
    if (k >= 0)
        return k;
    if (W.keeper_count >= WORLD_MAX_KEEPERS)
        return -1;
    k = W.keeper_count++;
    struct WorldKeeper *wk = &W.keepers[k];
    memset(wk, 0, sizeof(*wk));
    wk->id = ref->id;
    wk->heart_x = ref->heart_x;
    wk->heart_y = ref->heart_y;
    wk->bbox_x1 = ref->bbox_x1;
    wk->bbox_y1 = ref->bbox_y1;
    wk->bbox_x2 = ref->bbox_x2;
    wk->bbox_y2 = ref->bbox_y2;
    wk->threat_radius = ref->threat_radius;
    wk->money = ref->money;
    wk->slot = -1;
    wk->state = WKSt_Waiting;
    return k;
}

/******************************************************************************/
/* Human players. A single-player run has one, PLAYER0; a network game has one per user. The world treats each of
 * them as "the player": their hearts, creatures and ground load chunks, and raids come for each of them. */

TbBool world_player_is_human(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx == PLAYER_GOOD) || (plyr_idx == game.neutral_player_num))
        return false;
    for (int k = 0; k < W.keeper_count; k++)
        if (W.keepers[k].slot == plyr_idx)
            return false;
    if (plyr_idx == PLAYER0)
        return true;
    const struct PlayerInfo *player = get_player(plyr_idx);
    /* a dropped human's seat, played by the computer for now (a placeholder), is still theirs */
    return player_exists(player) && (((player->allocflags & PlaF_CompCtrl) == 0) || player_is_placeholder(player));
}

TbBool world_player_is_keeper(PlayerNumber plyr_idx)
{
    for (int k = 0; k < W.keeper_count; k++)
        if ((W.keepers[k].slot == plyr_idx) && (plyr_idx >= 0))
            return true;
    return false;
}

int32_t world_player_identity(PlayerNumber plyr_idx)
{
    for (int k = 0; k < W.keeper_count; k++)
        if ((W.keepers[k].slot == plyr_idx) && (plyr_idx >= 0))
            return 1000 + k;
    return plyr_idx;
}

/** Every human player. */
static PlayerBitFlags world_human_mask(void)
{
    PlayerBitFlags mask = pmask_none();
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        if (world_player_is_human(p))
            pmask_add(&mask, p);
    return mask;
}

static TbBool owner_in_mask(PlayerNumber owner, PlayerBitFlags mask)
{
    return (owner >= 0) && (owner < PLAYERS_COUNT) && pmask_has(mask, owner);
}

static PlayerNumber owner_to_engine(uint8_t wown)
{
    switch (wown)
    {
    case WOwn_Neutral: return game.neutral_player_num;
    case WOwn_Player: return PLAYER0;
    case WOwn_Heroes: return PLAYER_GOOD;
    default:
        if ((wown > WOwn_HumanBase) && (wown < WOwn_HumanBase + PLAYERS_COUNT))
            return (PlayerNumber)(wown - WOwn_HumanBase);
        if (wown - WOwn_Keeper0 < W.keeper_count)
            return keeper_take_slot(wown - WOwn_Keeper0);
        return game.neutral_player_num;
    }
}

static uint8_t owner_to_world(PlayerNumber plyr_idx)
{
    if (plyr_idx == PLAYER0)
        return WOwn_Player;
    if (plyr_idx == PLAYER_GOOD)
        return WOwn_Heroes;
    for (int i = 0; i < W.keeper_count; i++)
        if ((W.keepers[i].slot == plyr_idx) && (W.keepers[i].state != WKSt_Destroyed))
            return (uint8_t)(WOwn_Keeper0 + i);
    if (world_player_is_human(plyr_idx))
        return (uint8_t)(WOwn_HumanBase + plyr_idx);
    return WOwn_Neutral;
}

/******************************************************************************/
/* Chunk contents in engine terms: what a frozen record or a generated chunk holds. */

enum WorldThingFlags {
    WTF_Locked = 0x01,
    WTF_Lair = 0x02,       /* creature: z holds its lair's subtile relative to the chunk (x low 16 bits, y high) */
    WTF_LaidToRest = 0x04, /* a dead body in a graveyard */
    WTF_Prisoner = 0x08,   /* a creature kept in its captor's prison */
    WTF_Tortured = 0x10,   /* a creature on its captor's torture room */
};

/** What a frozen creature keeps besides its WorldThing: its mood, hunger, wages and kills (the CEX1 block). */
#define WORLD_CREXT_MAGIC 0x31584543u /* "CEX1": optional, after the dig flags */
#pragma pack(1)
struct WorldCreatureExt {
    uint16_t ordinal;           /* its index in the chunk's things */
    uint8_t hunger_amount;
    uint8_t hunger_loss;
    int32_t hunger_level;
    int32_t annoyance_level[5];
    uint8_t paydays_owed;
    int8_t paydays_advanced;
    uint8_t mood_flags;
    uint8_t joining_age;
    int16_t kills_num;
    int16_t kills_num_allied;
    int16_t kills_num_enemy;
    uint8_t blood_type;
    uint8_t spare[3];
};
#pragma pack()

struct WorldThing {
    uint8_t oclass;
    uint8_t owner;          /* WorldOwner */
    uint8_t level;          /* creature level 1..10 */
    uint8_t flags;
    uint16_t model;
    int32_t x, y, z;        /* map coordinates relative to the chunk's first subtile */
    int32_t health;
    uint32_t exp_points;
    uint32_t gold;
    uint16_t extra;         /* hero gate number, trap shots, a creature's original hero objective */
    char name[CREATURE_NAME_MAX];
};

#define WORLD_MAX_THINGS_PER_CHUNK 384

struct WorldChunkData {
    int32_t cx, cy;
    uint8_t fresh;                      /* from the generator, never loaded before */
    uint8_t kind[WG_CHUNK_CELLS];       /* engine slab kinds */
    uint8_t owner[WG_CHUNK_CELLS];      /* WorldOwner */
    uint16_t health[WG_CHUNK_CELLS];    /* 0: default for the kind */
    uint8_t texture[WG_CHUNK_CELLS];    /* slab texture pack (slab_ext_data); 0: the level's own */
    uint8_t texture_initial[WG_CHUNK_CELLS]; /* what it goes back to when nobody owns it (biome texture) */
    uint8_t wall[WG_CHUNK_CELLS];       /* generated: automatic wall, kind chosen on placement */
    uint8_t door[WG_CHUNK_CELLS];       /* generated: door kind (door model) to place */
    PlayerBitFlags revealed[CHUNK_STL_CELLS]; /* the players each subtile is revealed to */
    PlayerBitFlags dig[WG_CHUNK_CELLS];       /* the players that had the slab marked for digging */
    int thing_count;
    struct WorldThing things[WORLD_MAX_THINGS_PER_CHUNK];
    int ext_count;
    struct WorldCreatureExt ext[WORLD_MAX_THINGS_PER_CHUNK];
};

static struct WorldChunkData chunk_buf;

static const SlabKind wg_kinds[WgS_COUNT] = {
    [WgS_Rock] = SlbT_ROCK, [WgS_Gold] = SlbT_GOLD, [WgS_DenseGold] = SlbT_DENSEGOLD, [WgS_Gems] = SlbT_GEMS,
    [WgS_Earth] = SlbT_EARTH, [WgS_Path] = SlbT_PATH, [WgS_Claimed] = SlbT_CLAIMED, [WgS_Lava] = SlbT_LAVA,
    [WgS_Water] = SlbT_WATER, [WgS_Wall] = SlbT_WALLDRAPE, [WgS_Portal] = SlbT_ENTRANCE,
    [WgS_Heart] = SlbT_DUNGHEART, [WgS_Treasury] = SlbT_TREASURE, [WgS_Lair] = SlbT_LAIR,
    [WgS_Hatchery] = SlbT_GARDEN, [WgS_Training] = SlbT_TRAINING, [WgS_Library] = SlbT_LIBRARY,
    [WgS_DoorWood] = SlbT_CLAIMED, [WgS_DoorBrace] = SlbT_CLAIMED,
};

static uint8_t wg_owner_to_world(uint8_t wgown, const struct WgStructId *keeper)
{
    switch (wgown)
    {
    case WgO_Player: return WOwn_Player;
    case WgO_Heroes: return WOwn_Heroes;
    case WgO_Keeper:
    {
        int k = keeper_index(keeper);
        return (k >= 0) ? (uint8_t)(WOwn_Keeper0 + k) : WOwn_Neutral;
    }
    default: return WOwn_Neutral;
    }
}

/** The world slabs a stamp covers: those within WORLD_STAMP_RADIUS of the start's heart, moved. */
static void stamp_rect(const struct WorldStamp *st, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    struct WgStructRef start;
    wg_start_ref(W.gen, &start);
    *x1 = start.heart_x - WORLD_STAMP_RADIUS + st->dx;
    *y1 = start.heart_y - WORLD_STAMP_RADIUS + st->dy;
    *x2 = start.heart_x + WORLD_STAMP_RADIUS + st->dx;
    *y2 = start.heart_y + WORLD_STAMP_RADIUS + st->dy;
}

/** A slab of the start structure itself (the player's dungeon and its portal), as opposed to the ground round it. */
static TbBool stamp_structure_cell(uint8_t wg_slab, uint8_t wg_owner)
{
    if (wg_owner == WgO_Player)
        return true;
    switch (wg_slab)
    {
    case WgS_Wall: case WgS_Portal: case WgS_Heart: case WgS_Treasury: case WgS_Lair: case WgS_Hatchery:
    case WgS_Training: case WgS_Library: case WgS_DoorWood: case WgS_DoorBrace: case WgS_Claimed:
        return true;
    default:
        return false;
    }
}

/* the ground round a copied start follows the start's rules: no gold or gems this close to the heart, no rock,
   lava or water this close (nothing seals the dungeon or its portal in), no dug paths at all */
#define STAMP_NO_GOLD_DIST 21
#define STAMP_CLEAR_DIST 14
#define WTF_StampCopy 0x80 /* generation only: a thing copied by a stamp (never stored) */

/** The heroes outposts in a freshly generated chunk: their slabs, iron doors, traps, portals and loot; whatever the
 *  generator put on their cells goes. */
static void apply_outposts(int32_t cx, int32_t cy, struct WorldChunkData *cd)
{
    static struct OutpostOverlay ov;
    if (world_outpost_overlay(cx, cy, &ov) <= 0)
        return;
    for (int i = 0; i < WG_CHUNK_CELLS; i++)
    {
        if (!ov.set[i])
            continue;
        cd->kind[i] = ov.kind[i];
        cd->owner[i] = ov.heroes[i] ? WOwn_Heroes : WOwn_Neutral;
        cd->wall[i] = ov.wall[i];
        cd->door[i] = ov.door[i];
        if (slab_kind_has_no_ownership(cd->kind[i]))
            cd->owner[i] = WOwn_Neutral;
    }
    int kept = 0;
    for (int t = 0; t < cd->thing_count; t++)
    {
        const struct WorldThing *th = &cd->things[t];
        const int sx = th->x / (COORD_PER_STL * STL_PER_SLB), sy = th->y / (COORD_PER_STL * STL_PER_SLB);
        if ((sx >= 0) && (sx < WG_CHUNK) && (sy >= 0) && (sy < WG_CHUNK) && ov.set[sy * WG_CHUNK + sx])
            continue;
        if (kept != t)
            cd->things[kept] = *th;
        kept++;
    }
    cd->thing_count = kept;
    for (int t = 0; (t < ov.thing_count) && (cd->thing_count < WORLD_MAX_THINGS_PER_CHUNK); t++)
    {
        struct WorldThing *th = &cd->things[cd->thing_count++];
        memset(th, 0, sizeof(*th));
        th->oclass = ov.things[t].oclass;
        th->model = ov.things[t].model;
        th->owner = ov.things[t].heroes ? WOwn_Heroes : WOwn_Neutral;
        th->x = ov.things[t].x;
        th->y = ov.things[t].y;
        th->z = -1;
        th->health = -1;
        th->extra = ov.things[t].extra;
    }
}

/** Copies the start structure into a freshly generated chunk wherever a stamp covers it: the dungeon's slabs, walls,
 *  doors and things (the player's become the seat's) and its portal. The ground round them stays the chunk's own,
 *  cleared as round the first start. */
static void apply_stamps(int32_t cx, int32_t cy, struct WorldChunkData *cd)
{
    static struct WgChunk src;
    ThingModel wood = get_id(door_desc, "WOOD");
    ThingModel brace = get_id(door_desc, "BRACED");
    struct WgStructRef start;
    if (W.stamp_count > 0)
        wg_start_ref(W.gen, &start);
    for (int n = 0; n < W.stamp_count; n++)
    {
        const struct WorldStamp *st = &W.stamps[n];
        int64_t x1, y1, x2, y2;
        stamp_rect(st, &x1, &y1, &x2, &y2);
        const int64_t cx1 = (int64_t)cx * WG_CHUNK, cy1 = (int64_t)cy * WG_CHUNK;
        const int64_t ox1 = max(x1, cx1), oy1 = max(y1, cy1);
        const int64_t ox2 = min(x2, cx1 + WG_CHUNK - 1), oy2 = min(y2, cy1 + WG_CHUNK - 1);
        if ((ox1 > ox2) || (oy1 > oy2))
            continue;
        const uint8_t wown = (uint8_t)((st->plyr == PLAYER0) ? WOwn_Player : WOwn_HumanBase + st->plyr);
        const int64_t hx = start.heart_x + st->dx, hy = start.heart_y + st->dy;
        uint8_t structure[WG_CHUNK_CELLS];
        memset(structure, 0, sizeof(structure));
        /* the dungeon's slabs and things, one source chunk at a time */
        for (int64_t scy = floor_div(oy1 - st->dy, WG_CHUNK); scy <= floor_div(oy2 - st->dy, WG_CHUNK); scy++)
        {
            for (int64_t scx = floor_div(ox1 - st->dx, WG_CHUNK); scx <= floor_div(ox2 - st->dx, WG_CHUNK); scx++)
            {
                if (wg_generate_chunk(W.gen, (int32_t)scx, (int32_t)scy, &src) != 0)
                    continue;
                for (int j = 0; j < WG_CHUNK_CELLS; j++)
                {
                    const int64_t x = scx * WG_CHUNK + j % WG_CHUNK + st->dx, y = scy * WG_CHUNK + j / WG_CHUNK + st->dy;
                    if ((x < ox1) || (x > ox2) || (y < oy1) || (y > oy2) || !stamp_structure_cell(src.slabs[j], src.owners[j]))
                        continue;
                    const int i = (int)((y - cy1) * WG_CHUNK + (x - cx1));
                    const uint8_t sl = src.slabs[j];
                    structure[i] = 1;
                    cd->kind[i] = (sl < WgS_COUNT) ? wg_kinds[sl] : SlbT_ROCK;
                    cd->owner[i] = (src.owners[j] == WgO_Player) ? wown : WOwn_Neutral;
                    cd->wall[i] = (sl == WgS_Wall);
                    cd->door[i] = 0;
                    if (sl == WgS_DoorWood)
                        cd->door[i] = (wood > 0) ? (uint8_t)wood : 1;
                    else if (sl == WgS_DoorBrace)
                        cd->door[i] = (brace > 0) ? (uint8_t)brace : 2;
                    if (slab_kind_has_no_ownership(cd->kind[i]))
                        cd->owner[i] = WOwn_Neutral;
                }
                for (int t = 0; (t < src.thing_count) && (cd->thing_count < WORLD_MAX_THINGS_PER_CHUNK); t++)
                {
                    const struct WgThing *wt = &src.things[t];
                    const int64_t ssx = (int64_t)floor(wt->x / STL_PER_SLB), ssy = (int64_t)floor(wt->y / STL_PER_SLB);
                    if ((ssx < scx * WG_CHUNK) || (ssx >= scx * WG_CHUNK + WG_CHUNK) || (ssy < scy * WG_CHUNK) ||
                        (ssy >= scy * WG_CHUNK + WG_CHUNK))
                        continue;
                    const int j = (int)((ssy - scy * WG_CHUNK) * WG_CHUNK + (ssx - scx * WG_CHUNK));
                    const int64_t sx = ssx + st->dx, sy = ssy + st->dy;
                    if ((sx < ox1) || (sx > ox2) || (sy < oy1) || (sy > oy2) || !stamp_structure_cell(src.slabs[j], src.owners[j]))
                        continue;
                    long model = get_id(wt->is_creature ? creature_desc : object_desc, wt->name);
                    if (model <= 0)
                        continue;
                    const double tx = wt->x + (double)st->dx * STL_PER_SLB, ty = wt->y + (double)st->dy * STL_PER_SLB;
                    struct WorldThing *th = &cd->things[cd->thing_count];
                    memset(th, 0, sizeof(*th));
                    th->oclass = wt->is_creature ? TCls_Creature : TCls_Object;
                    th->model = (uint16_t)model;
                    th->owner = (wt->owner == WgO_Player) ? wown : WOwn_Neutral;
                    th->level = wt->is_creature ? wt->level : 0;
                    th->x = (int32_t)llround((tx - (double)cx1 * STL_PER_SLB) * COORD_PER_STL);
                    th->y = (int32_t)llround((ty - (double)cy1 * STL_PER_SLB) * COORD_PER_STL);
                    th->z = -1;
                    th->health = -1;
                    th->flags |= WTF_StampCopy;
                    cd->thing_count++;
                }
            }
        }
        /* the chunk's own ground round the dungeon, cleared as round the first start */
        for (int64_t y = oy1; y <= oy2; y++)
        {
            for (int64_t x = ox1; x <= ox2; x++)
            {
                const int i = (int)((y - cy1) * WG_CHUNK + (x - cx1));
                if (structure[i])
                    continue;
                const int64_t d = max(llabs(x - hx), llabs(y - hy));
                const SlabKind k = cd->kind[i];
                if ((d <= STAMP_NO_GOLD_DIST) && ((k == SlbT_GOLD) || (k == SlbT_DENSEGOLD) || (k == SlbT_GEMS)))
                    cd->kind[i] = SlbT_EARTH;
                if ((d <= STAMP_CLEAR_DIST) && ((k == SlbT_ROCK) || (k == SlbT_LAVA) || (k == SlbT_WATER)))
                    cd->kind[i] = SlbT_EARTH;
                if (k == SlbT_PATH)
                    cd->kind[i] = SlbT_EARTH;
                if (cd->kind[i] != k)
                {
                    cd->owner[i] = WOwn_Neutral;
                    cd->wall[i] = 0;
                    cd->door[i] = 0;
                }
            }
        }
        /* the chunk's own things on the dungeon's slabs, or objects on cleared ground, go */
        int kept = 0;
        for (int t = 0; t < cd->thing_count; t++)
        {
            struct WorldThing *th = &cd->things[t];
            const int64_t tx = cx1 + floor_div(th->x / COORD_PER_STL, STL_PER_SLB);
            const int64_t ty = cy1 + floor_div(th->y / COORD_PER_STL, STL_PER_SLB);
            const TbBool copied = (th->flags & WTF_StampCopy) != 0;
            th->flags &= (uint8_t)~WTF_StampCopy;
            if (!copied && (tx >= ox1) && (tx <= ox2) && (ty >= oy1) && (ty <= oy2))
            {
                const int i = (int)((ty - cy1) * WG_CHUNK + (tx - cx1));
                if (structure[i] || ((cd->kind[i] == SlbT_EARTH) && (th->oclass == TCls_Object)))
                    continue;
            }
            cd->things[kept++] = *th;
        }
        cd->thing_count = kept;
    }
}

/** The player starts (world slabs): the first player's, then every seat's start dungeon. */
static int world_starts(int64_t *xs, int64_t *ys, int max_starts)
{
    int n = 0;
    xs[n] = W.home_x;
    ys[n] = W.home_y;
    n++;
    struct WgStructRef start;
    wg_start_ref(W.gen, &start);
    for (int k = 0; (k < W.stamp_count) && (n < max_starts); k++, n++)
    {
        xs[n] = start.heart_x + W.stamps[k].dx;
        ys[n] = start.heart_y + W.stamps[k].dy;
    }
    return n;
}

/** Slabs from a world slab to the nearest player start: research books grade by it, so every player's books grow
 *  from their own start outward. */
static double dist_to_nearest_start(int64_t x, int64_t y)
{
    int64_t xs[WORLD_MAX_STAMPS + 1], ys[WORLD_MAX_STAMPS + 1];
    const int n = world_starts(xs, ys, WORLD_MAX_STAMPS + 1);
    double best = 1e30;
    for (int i = 0; i < n; i++)
    {
        const double dx = (double)(x - xs[i]), dy = (double)(y - ys[i]);
        const double d = sqrt(dx * dx + dy * dy);
        if (d < best)
            best = d;
    }
    return best;
}

/** Research books: the two reward caves nearest each player start always hold one. */
static TbBool book_cave_guaranteed(const struct WgStructRef *ref)
{
    int64_t xs[WORLD_MAX_STAMPS + 1], ys[WORLD_MAX_STAMPS + 1];
    const int starts = world_starts(xs, ys, WORLD_MAX_STAMPS + 1);
    for (int s = 0; s < starts; s++)
    {
        struct WgStructRef near[32];
        const int n = min(wg_structures_in(W.gen, xs[s] - 200, ys[s] - 200, xs[s] + 200, ys[s] + 200,
            1u << WgT_Reward, near, 32), 32);
        double best[2] = {1e30, 1e30};
        int64_t cave[2][2] = {{INT64_MIN, INT64_MIN}, {INT64_MIN, INT64_MIN}};
        for (int i = 0; i < n; i++)
        {
            const double dx = (double)(near[i].origin_x - xs[s]), dy = (double)(near[i].origin_y - ys[s]);
            const double d = sqrt(dx * dx + dy * dy);
            const int slot = (d < best[0]) ? 0 : ((d < best[1]) ? 1 : -1);
            if (slot < 0)
                continue;
            if (slot == 0)
            {
                best[1] = best[0];
                cave[1][0] = cave[0][0];
                cave[1][1] = cave[0][1];
            }
            best[slot] = d;
            cave[slot][0] = near[i].origin_x;
            cave[slot][1] = near[i].origin_y;
        }
        for (int k = 0; k < 2; k++)
        {
            if ((cave[k][0] == ref->origin_x) && (cave[k][1] == ref->origin_y))
                return true;
        }
    }
    return false;
}

static TbBool book_floor(SlabKind kind)
{
    return (kind != SlbT_ROCK) && (kind != SlbT_GOLD) && (kind != SlbT_DENSEGOLD) && (kind != SlbT_GEMS) &&
        (kind != SlbT_EARTH) && (kind != SlbT_TORCHDIRT) && (kind != SlbT_LAVA) && (kind != SlbT_WATER) &&
        !slab_kind_is_door(kind) && (kind < SlbT_WALLDRAPE || kind > SlbT_WALLPAIRSHR);
}

/** Books of the reward caves and fortresses in a fresh chunk: in the cave's middle, or inside the fortress by its
 *  gate. Only the chunk that holds a book's spot places it. */
static void place_books(const struct WgChunk *ch, int32_t cx, int32_t cy, struct WorldChunkData *cd)
{
    const ThingModel model = world_progress_book_model();
    if (!world_progress_active() || (model <= 0))
        return;
    for (int s = 0; s < ch->struct_count; s++)
    {
        const struct WgStructRef *ref = &ch->structs[s];
        if ((ref->id.type != WgT_Reward) && (ref->id.type != WgT_Fortress))
            continue;
        uint8_t kind = 0;
        struct WgStructRef graded = *ref;
        graded.distance = dist_to_nearest_start(ref->origin_x, ref->origin_y);
        const int n = world_progress_books_of(&graded, W.seed, (ref->id.type == WgT_Reward) && book_cave_guaranteed(ref), &kind);
        const int64_t mx = (ref->bbox_x1 + ref->bbox_x2) / 2, my = (ref->bbox_y1 + ref->bbox_y2) / 2;
        for (int k = 0; k < n; k++)
        {
            int64_t bx = mx + k, by = my;
            if (ref->id.type == WgT_Fortress)
            {
                /* one slab inside the gate, toward the middle */
                bx = ref->gate_x + ((mx > ref->gate_x) ? 1 : ((mx < ref->gate_x) ? -1 : 0)) + k;
                by = ref->gate_y + ((my > ref->gate_y) ? 1 : ((my < ref->gate_y) ? -1 : 0));
            }
            const int64_t lx = bx - (int64_t)cx * WG_CHUNK, ly = by - (int64_t)cy * WG_CHUNK;
            if ((lx < 0) || (ly < 0) || (lx >= WG_CHUNK) || (ly >= WG_CHUNK) || (cd->thing_count >= WORLD_MAX_THINGS_PER_CHUNK))
                continue;
            if (!book_floor(cd->kind[ly * WG_CHUNK + lx]))
                continue;
            struct WorldThing *th = &cd->things[cd->thing_count++];
            memset(th, 0, sizeof(*th));
            th->oclass = TCls_Object;
            th->model = (uint16_t)model;
            th->owner = WOwn_Neutral;
            th->x = (int32_t)(lx * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2;
            th->y = (int32_t)(ly * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2;
            th->z = -1;
            th->health = -1;
            th->extra = kind;
            SYNCLOG("World progress: chunk (%d,%d) holds a research book (kind %d) at world slab (%ld,%ld) in a %s",
                (int)cx, (int)cy, (int)kind, (long)bx, (long)by, wg_struct_type_name(ref->id.type));
        }
    }
}

int world_biome_at_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!W.active || (W.gen == NULL))
        return WgB_Earthen;
    return wg_biome_at(W.gen, world_slab_x_of(subtile_slab(stl_x)), world_slab_y_of(subtile_slab(stl_y)));
}

/* worldgen.py HERO_TYPES: name, weight, the distance from which it guards fortresses */
static const struct { const char *name; int weight; int from; } hero_table[] = {
    {"ARCHER", 4, 0}, {"DWARFA", 3, 0}, {"THIEF", 2, 0}, {"BARBARIAN", 3, 120}, {"MONK", 2, 160},
    {"WIZARD", 2, 200}, {"WITCH", 1, 260}, {"SAMURAI", 1, 320}, {"GIANT", 1, 320}, {"KNIGHT", 1, 400},
};
#define HERO_TABLE_LEN ((int)(sizeof(hero_table) / sizeof(hero_table[0])))

static uint64_t hero_hash(uint64_t seed, int64_t x, int64_t y)
{
    uint64_t h = seed ^ ((uint64_t)x * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)y * 0xC2B2AE3D27D4EB4FULL) ^ 0x4845524FULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static int hero_level_at(double d) /* worldgen.py _level_for(distance, 120, 8) */
{
    return max(1, min(8, 1 + (int)(d / 120.0)));
}

/** A fortress guard of a fresh chunk, re-graded for the nearest player start: false when it isn't there at all (a
 *  fortress near a start has fewer guards). Deterministic from the guard's position. */
static TbBool regrade_hero(int64_t wx, int64_t wy, long *model, uint8_t *level)
{
    const double dx = (double)(wx - W.home_x), dy = (double)(wy - W.home_y);
    const double d_first = sqrt(dx * dx + dy * dy);
    const double d = dist_to_nearest_start(wx, wy);
    if (d + 1.0 >= d_first)
        return true; /* the first player's start is the nearest: as generated */
    const uint64_t h = hero_hash(W.seed, wx, wy);
    /* fewer guards: as many as the generator puts there (3 + d / 100, at most 11) */
    const double keep = (3.0 + d / 100.0) / min(11.0, 3.0 + d_first / 100.0);
    if ((double)(h % 1000) >= keep * 1000.0)
        return false;
    /* the level, with the generator's random step kept */
    const int step = max(0, min(1, (int)*level - hero_level_at(d_first)));
    *level = (uint8_t)(hero_level_at(d) + step);
    /* a hero too strong for here becomes one of those that guard at this distance */
    const char *name = NULL;
    for (int i = 0; i < HERO_TABLE_LEN; i++)
    {
        if (*model == get_id(creature_desc, hero_table[i].name))
            name = hero_table[i].name;
    }
    int from = 0;
    for (int i = 0; (i < HERO_TABLE_LEN) && (name != NULL); i++)
    {
        if (strcmp(name, hero_table[i].name) == 0)
            from = hero_table[i].from;
    }
    if ((name == NULL) || (from <= d))
        return true;
    int total = 0;
    for (int i = 0; i < HERO_TABLE_LEN; i++)
        total += (hero_table[i].from <= d) ? hero_table[i].weight : 0;
    int pick = (int)((h >> 20) % (uint64_t)max(total, 1));
    for (int i = 0; i < HERO_TABLE_LEN; i++)
    {
        if (hero_table[i].from > d)
            continue;
        if (pick < hero_table[i].weight)
        {
            const long m = get_id(creature_desc, hero_table[i].name);
            if (m > 0)
                *model = m;
            break;
        }
        pick -= hero_table[i].weight;
    }
    return true;
}

static TbBool generate_chunk_data(int32_t cx, int32_t cy, struct WorldChunkData *cd)
{
    static struct WgChunk ch;
    memset(cd, 0, sizeof(*cd));
    cd->cx = cx;
    cd->cy = cy;
    cd->fresh = 1;
    if (wg_generate_chunk(W.gen, cx, cy, &ch) != 0)
    {
        ERRORLOG("World: chunk (%d,%d) could not be generated", (int)cx, (int)cy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
            cd->kind[i] = SlbT_ROCK;
        return false;
    }
    for (int s = 0; s < ch.struct_count; s++)
    {
        if (ch.structs[s].id.type == WgT_Keeper)
            keeper_register(&ch.structs[s]);
    }
    if (world_paged)
    {
        /* the chunked world: also the keepers whose threat radius may reach this chunk, so that the player's
           creatures coming near load their base (cm_collect_wanted) */
        struct WgStructRef near[4];
        const int64_t x1 = (int64_t)cx * WG_CHUNK - WORLD_KEEPER_REACH, y1 = (int64_t)cy * WG_CHUNK - WORLD_KEEPER_REACH;
        const int n = wg_structures_in(W.gen, x1, y1, x1 + WG_CHUNK - 1 + 2 * WORLD_KEEPER_REACH,
            y1 + WG_CHUNK - 1 + 2 * WORLD_KEEPER_REACH, 1u << WgT_Keeper, near, 4);
        for (int s = 0; (s < n) && (s < 4); s++)
            keeper_register(&near[s]);
    }
    ThingModel wood = get_id(door_desc, "WOOD");
    ThingModel brace = get_id(door_desc, "BRACED");
    for (int i = 0; i < WG_CHUNK_CELLS; i++)
    {
        uint8_t s = ch.slabs[i];
        cd->kind[i] = (s < WgS_COUNT) ? wg_kinds[s] : SlbT_ROCK;
        cd->texture[i] = (uint8_t)wg_biome_texture(ch.biomes[i]);
        cd->texture_initial[i] = cd->texture[i];
        cd->owner[i] = wg_owner_to_world(ch.owners[i], &ch.keeper);
        cd->wall[i] = (s == WgS_Wall);
        if (s == WgS_DoorWood)
            cd->door[i] = (wood > 0) ? (uint8_t)wood : 1;
        else if (s == WgS_DoorBrace)
            cd->door[i] = (brace > 0) ? (uint8_t)brace : 2;
        if (slab_kind_has_no_ownership(cd->kind[i]))
            cd->owner[i] = WOwn_Neutral;
    }
    const int64_t ox = (int64_t)cx * WG_CHUNK * STL_PER_SLB, oy = (int64_t)cy * WG_CHUNK * STL_PER_SLB;
    int heroes_regraded = 0, heroes_dropped = 0;
    for (int t = 0; (t < ch.thing_count) && (cd->thing_count < WORLD_MAX_THINGS_PER_CHUNK); t++)
    {
        const struct WgThing *wt = &ch.things[t];
        struct WorldThing *th = &cd->things[cd->thing_count];
        memset(th, 0, sizeof(*th));
        th->oclass = wt->is_creature ? TCls_Creature : TCls_Object;
        long model = get_id(wt->is_creature ? creature_desc : object_desc, wt->name);
        if (model <= 0)
        {
            WARNLOG("World: unknown %s %s", wt->is_creature ? "creature" : "object", wt->name);
            continue;
        }
        th->owner = wg_owner_to_world(wt->owner, &wt->keeper);
        th->level = wt->is_creature ? wt->level : 0;
        if (wt->is_creature && (th->owner == WOwn_Heroes) && (W.stamp_count > 0))
        {
            /* fortress guards follow the nearest player start */
            const long was_model = model;
            const uint8_t was_level = th->level;
            if (!regrade_hero((int64_t)floor(wt->x / STL_PER_SLB), (int64_t)floor(wt->y / STL_PER_SLB), &model, &th->level))
            {
                heroes_dropped++;
                continue;
            }
            if ((model != was_model) || (th->level != was_level))
                heroes_regraded++;
        }
        th->model = (uint16_t)model;
        th->x = (int32_t)llround((wt->x - (double)ox) * COORD_PER_STL);
        th->y = (int32_t)llround((wt->y - (double)oy) * COORD_PER_STL);
        th->z = -1; /* on the floor */
        th->health = -1;
        if (wt->has_gate)
            th->extra = 1;
        cd->thing_count++;
    }
    if (heroes_regraded + heroes_dropped > 0)
        SYNCLOG("World: chunk (%d,%d): %d fortress guards re-graded and %d left out for the nearest player start",
            (int)cx, (int)cy, heroes_regraded, heroes_dropped);
    /* no heart objects: placing the heart pedestal makes the heart (a slab object) */
    place_books(&ch, cx, cy, cd);
    apply_outposts(cx, cy, cd);
    apply_stamps(cx, cy, cd);
    return true;
}

/******************************************************************************/
/* Chunk records */

struct RecWriter {
    uint8_t *buf;
    uint32_t len, cap;
    TbBool err;                        /* an allocation failed: the record is incomplete */
};

static void rec_put(struct RecWriter *w, const void *p, uint32_t n)
{
    if (w->len + n > w->cap)
    {
        uint32_t ncap = (w->cap == 0) ? 4096 : w->cap;
        while (w->len + n > ncap)
            ncap *= 2;
        uint8_t *nb = (uint8_t *)realloc(w->buf, ncap);
        if (nb == NULL)
        {
            w->err = true;
            return;
        }
        w->buf = nb;
        w->cap = ncap;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void rec_u32(struct RecWriter *w, uint32_t v) { rec_put(w, &v, 4); }

struct RecReader {
    const uint8_t *buf;
    uint32_t len, pos;
    TbBool bad;
};

static void rec_get(struct RecReader *r, void *p, uint32_t n)
{
    if (r->pos + n > r->len)
    {
        r->bad = true;
        memset(p, 0, n);
        return;
    }
    memcpy(p, r->buf + r->pos, n);
    r->pos += n;
}

static uint32_t rec_get_u32(struct RecReader *r) { uint32_t v; rec_get(r, &v, 4); return v; }
/** The next u32 without reading it (0 when there are fewer than 4 bytes left). */
static uint32_t rec_peek_u32(const struct RecReader *r)
{
    uint32_t v = 0;
    if (!r->bad && (r->len - r->pos >= 4))
        memcpy(&v, r->buf + r->pos, 4);
    return v;
}

/** Compressed blob: u32 raw length, then the zlib stream. */
static TbBool pack(const uint8_t *raw, uint32_t raw_len, uint8_t **out, uint32_t *out_len)
{
    uLongf zlen = compressBound(raw_len);
    uint8_t *z = (uint8_t *)malloc(zlen + 4);
    if (z == NULL)
        return false;
    memcpy(z, &raw_len, 4);
    if (compress2(z + 4, &zlen, raw, raw_len, 1) != Z_OK)
    {
        free(z);
        return false;
    }
    *out = z;
    *out_len = (uint32_t)zlen + 4;
    return true;
}

static TbBool unpack(const uint8_t *z, uint32_t zlen, uint8_t **out, uint32_t *out_len)
{
    if (zlen < 4)
        return false;
    uint32_t raw_len;
    memcpy(&raw_len, z, 4);
    if (raw_len > 64u * 1024u * 1024u)
        return false;
    uint8_t *raw = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (raw == NULL)
        return false;
    uLongf rl = raw_len;
    if ((uncompress(raw, &rl, z + 4, zlen - 4) != Z_OK) || (rl != raw_len))
    {
        free(raw);
        return false;
    }
    *out = raw;
    *out_len = raw_len;
    return true;
}

/* What world_read_slab_box read of chunks that weren't loaded: slab kinds and the player's slabs, by world chunk
 * (direct-mapped). An entry is forgotten when its chunk's record is written; the cache is dropped at level end and
 * when a save is loaded. */
#define PEEK_CACHE_BITS 12
#define PEEK_CACHE_SIZE (1 << PEEK_CACHE_BITS)
struct PeekEntry {
    int32_t cx, cy;
    uint8_t valid;
    uint8_t kind[WG_CHUNK_CELLS];
    uint8_t owner[WG_CHUNK_CELLS];     /* WorldOwner (generated chunks: only the start's player and neutral) */
};
static struct PeekEntry *peek_cache = NULL;

static struct PeekEntry *peek_cache_slot(int64_t cx, int64_t cy)
{
    const uint64_t key = ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy;
    return &peek_cache[(key * 0x9E3779B97F4A7C15ull) >> (64 - PEEK_CACHE_BITS)];
}

static void peek_cache_forget(int64_t cx, int64_t cy)
{
    if (peek_cache == NULL)
        return;
    struct PeekEntry *e = peek_cache_slot(cx, cy);
    if (e->valid && (e->cx == cx) && (e->cy == cy))
        e->valid = 0;
}

static void peek_cache_drop(void)
{
    free(peek_cache);
    peek_cache = NULL;
}

/** The 32-bit words of a player set a chunk record keeps: enough for the players in use. */
static uint32_t rec_mask_words(void)
{
    const uint32_t n = (uint32_t)(PLAYERS_COUNT + 31) / 32;
    return (n < 1) ? 1 : (n > PLAYER_MASK_WORDS) ? PLAYER_MASK_WORDS : n;
}

static void rec_put_masks(struct RecWriter *w, const PlayerBitFlags *masks, int count, uint32_t words)
{
    for (int i = 0; i < count; i++)
        rec_put(w, masks[i].w, 4 * words);
}

/** Player sets of `words` words a cell; the words past this build's are dropped. */
static void rec_get_masks(struct RecReader *r, PlayerBitFlags *masks, int count, uint32_t words)
{
    for (int i = 0; i < count; i++)
    {
        for (uint32_t k = 0; k < words; k++)
        {
            uint32_t v = rec_get_u32(r);
            if (k < PLAYER_MASK_WORDS)
                masks[i].w[k] = v;
        }
    }
}

/** 16-bit player sets (records before WCK5). */
static void rec_get_masks16(struct RecReader *r, PlayerBitFlags *masks, int count)
{
    for (int i = 0; i < count; i++)
    {
        uint16_t v = 0;
        rec_get(r, &v, 2);
        masks[i] = pmask_from_u32(v);
    }
}

/** Stores a chunk's record; false when it couldn't be (out of memory). */
static TbBool store_chunk_record(const struct WorldChunkData *cd)
{
    peek_cache_forget(cd->cx, cd->cy);
    struct RecWriter w;
    memset(&w, 0, sizeof(w));
    rec_u32(&w, WORLD_REC_MAGIC);
    rec_put(&w, &cd->cx, 4);
    rec_put(&w, &cd->cy, 4);
    rec_put(&w, cd->kind, sizeof(cd->kind));
    rec_put(&w, cd->owner, sizeof(cd->owner));
    rec_put(&w, cd->health, sizeof(cd->health));
    rec_put(&w, cd->texture, sizeof(cd->texture));
    rec_put(&w, cd->texture_initial, sizeof(cd->texture_initial));
    const uint32_t words = rec_mask_words();
    rec_u32(&w, words);
    rec_put_masks(&w, cd->revealed, CHUNK_STL_CELLS, words);
    rec_u32(&w, (uint32_t)cd->thing_count);
    rec_put(&w, cd->things, (uint32_t)(sizeof(struct WorldThing) * cd->thing_count));
    rec_put_masks(&w, cd->dig, WG_CHUNK_CELLS, words);
    if (cd->ext_count > 0)
    {
        rec_u32(&w, WORLD_CREXT_MAGIC);
        rec_u32(&w, (uint32_t)cd->ext_count);
        rec_put(&w, cd->ext, (uint32_t)(sizeof(struct WorldCreatureExt) * cd->ext_count));
    }
    if ((w.buf == NULL) || w.err)
    {
        free(w.buf);
        return false;
    }
    uint8_t *z;
    uint32_t zlen;
    TbBool stored = false;
    if (pack(w.buf, w.len, &z, &zlen))
    {
        struct ChunkSlot *s = table_insert(&W.store, cd->cx, cd->cy);
        if (s != NULL)
        {
            free(s->data);
            s->data = z;
            s->len = zlen;
            stored = true;
        } else
        {
            free(z);
        }
    }
    free(w.buf);
    return stored;
}

static TbBool load_chunk_record(int32_t cx, int32_t cy, struct WorldChunkData *cd)
{
    struct ChunkSlot *s = table_find(&W.store, cx, cy);
    if ((s == NULL) || (s->data == NULL))
        return false;
    uint8_t *raw;
    uint32_t raw_len;
    if (!unpack(s->data, s->len, &raw, &raw_len))
    {
        ERRORLOG("World: stored chunk (%d,%d) is damaged", (int)cx, (int)cy);
        return false;
    }
    struct RecReader r = {raw, raw_len, 0, false};
    memset(cd, 0, sizeof(*cd));
    uint32_t magic = rec_get_u32(&r);
    if ((magic != WORLD_REC_MAGIC) && (magic != WORLD_REC_MAGIC_V4) && (magic != WORLD_REC_MAGIC_V3) &&
        (magic != WORLD_REC_MAGIC_V2) && (magic != WORLD_REC_MAGIC_V1))
        r.bad = true;
    rec_get(&r, &cd->cx, 4);
    rec_get(&r, &cd->cy, 4);
    rec_get(&r, cd->kind, sizeof(cd->kind));
    rec_get(&r, cd->owner, sizeof(cd->owner));
    rec_get(&r, cd->health, sizeof(cd->health));
    if (magic != WORLD_REC_MAGIC_V1)
    {
        rec_get(&r, cd->texture, sizeof(cd->texture));
        rec_get(&r, cd->texture_initial, sizeof(cd->texture_initial));
    }
    uint32_t words = 0;
    if (magic == WORLD_REC_MAGIC)
    {
        words = rec_get_u32(&r);
        if ((words < 1) || (words > 64))
            r.bad = true;
        else
            rec_get_masks(&r, cd->revealed, CHUNK_STL_CELLS, words);
    } else
    if (magic == WORLD_REC_MAGIC_V4)
        rec_get_masks16(&r, cd->revealed, CHUNK_STL_CELLS);
    else
    {
        /* older records hold one flag a subtile: the local player's, PLAYER0 in a single-player run */
        uint8_t old_revealed[CHUNK_STL_CELLS];
        rec_get(&r, old_revealed, sizeof(old_revealed));
        for (int i = 0; i < CHUNK_STL_CELLS; i++)
            cd->revealed[i] = old_revealed[i] ? pmask_of(PLAYER0) : pmask_none();
    }
    uint32_t n = rec_get_u32(&r);
    if (n > WORLD_MAX_THINGS_PER_CHUNK)
        r.bad = true;
    else
        rec_get(&r, cd->things, (uint32_t)(sizeof(struct WorldThing) * n));
    if ((magic == WORLD_REC_MAGIC) && !r.bad)
        rec_get_masks(&r, cd->dig, WG_CHUNK_CELLS, words);
    else
    if ((magic == WORLD_REC_MAGIC_V4) || (magic == WORLD_REC_MAGIC_V3))
        rec_get_masks16(&r, cd->dig, WG_CHUNK_CELLS);
    cd->thing_count = r.bad ? 0 : (int)n;
    if (!r.bad && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_CREXT_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t ne = rec_get_u32(&r);
        if ((ne <= WORLD_MAX_THINGS_PER_CHUNK) && (r.len - r.pos >= sizeof(struct WorldCreatureExt) * ne))
        {
            rec_get(&r, cd->ext, (uint32_t)(sizeof(struct WorldCreatureExt) * ne));
            cd->ext_count = r.bad ? 0 : (int)ne;
        } else
            WARNLOG("World: chunk (%d,%d): its creatures' moods are damaged; they are left out", (int)cx, (int)cy);
    }
    free(raw);
    if (r.bad || (cd->cx != cx) || (cd->cy != cy))
    {
        ERRORLOG("World: stored chunk (%d,%d) is damaged", (int)cx, (int)cy);
        return false;
    }
    return true;
}

/******************************************************************************/
/* Freezing a chunk of the window */

static TbBool thing_is_slab_object(const struct Thing *thing)
{
    if ((thing->class_id != TCls_Object) && (thing->class_id != TCls_EffectGen))
        return false;
    long p = thing->parent_idx;
    long sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
    if (world_paged)
    {
        /* paged slab numbers don't fit parent_idx: it holds slab_parent_key of the slab, on or next to the thing */
        if (p < 0)
            return false;
        for (long dy = -1; dy <= 1; dy++)
            for (long dx = -1; dx <= 1; dx++)
                if ((sx + dx >= 0) && (sy + dy >= 0) && (p == slab_parent_key(get_slab_number(sx + dx, sy + dy))))
                    return true;
        return false;
    }
    if ((p <= 0) || (p >= (int64_t)game.map_tiles_x * game.map_tiles_y))
        return false;
    long px = p % game.map_tiles_x, py = p / game.map_tiles_x;
    return (labs(px - sx) <= 1) && (labs(py - sy) <= 1);
}

static TbBool thing_is_frozen_kind(const struct Thing *thing)
{
    switch (thing->class_id)
    {
    case TCls_Creature:
        return !world_wild_creature(thing); /* wild creatures despawn when their chunk leaves the window */
    case TCls_Trap:
    case TCls_Door:
    case TCls_DeadCreature:
        return true;
    case TCls_Object:
        if (thing_is_dungeon_heart(thing))
            return true; /* a slab object, but its health is kept while the chunk is frozen */
        if (thing_is_slab_object(thing))
            return false;
        if ((thing->model == ObjMdl_RoomFlag) || object_is_mature_food(thing) || thing_is_lair_totem(thing))
            return (thing->model != ObjMdl_RoomFlag) && !thing_is_lair_totem(thing);
        return true;
    default:
        return false;
    }
}

/******************************************************************************/
/* Lairs across frozen chunks */

static int lair_wait_find(const struct Thing *creatng)
{
    for (int i = 0; i < W.lair_wait_count; i++)
        if ((W.lair_waits[i].thing_idx == creatng->index) && (W.lair_waits[i].creation_turn == creatng->creation_turn))
            return i;
    return -1;
}

static void lair_wait_drop(int i)
{
    W.lair_waits[i] = W.lair_waits[--W.lair_wait_count];
}

static void lair_wait_add(const struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_paged)
        return;
    int i = lair_wait_find(creatng);
    if (i < 0)
    {
        if (W.lair_wait_count >= WORLD_LAIR_WAITS_MAX)
            lair_wait_drop(0);
        i = W.lair_wait_count++;
    }
    W.lair_waits[i].thing_idx = creatng->index;
    W.lair_waits[i].creation_turn = creatng->creation_turn;
    W.lair_waits[i].stl_x = stl_x;
    W.lair_waits[i].stl_y = stl_y;
}

/** Gives a creature its lair back at a subtile: 1 done, 0 not possible any more, -1 the ground isn't loaded yet. */
static int lair_try_attach(struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (world_paged)
    {
        if ((stl_x < 0) || (stl_y < 0) || (wp_page_of_subtile(stl_x, stl_y) == NULL))
            return -1;
    }
    else if ((stl_x < 0) || (stl_y < 0) || (stl_x >= game.map_subtiles_x) || (stl_y >= game.map_subtiles_y))
        return 0;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl) || (cctrl->lairtng_idx > 0))
        return 0;
    struct Room *room = subtile_room_get(stl_x, stl_y);
    if (room_is_invalid(room) || (room->owner != creatng->owner) || !room_role_matches(room->kind, RoRoF_LairStorage))
        return 0;
    if (!thing_is_invalid(find_lair_totem_at(stl_x, stl_y)))
        return 0;
    if (!room_has_enough_free_capacity_for_creature_job(room, creatng, Job_TAKE_SLEEP))
        return 0;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(stl_x);
    pos.y.val = subtile_coord_center(stl_y);
    pos.z.val = 0;
    return creature_place_lair_totem(creatng, room, &pos) ? 1 : 0;
}

/** After chunks load: the creatures waiting for a lair on them get it. */
static void lair_waits_process(void)
{
    for (int i = 0; i < W.lair_wait_count; )
    {
        const struct WorldLairWait *lw = &W.lair_waits[i];
        struct Thing *creatng = thing_get(lw->thing_idx);
        int res = 0;
        if (thing_exists(creatng) && (creatng->class_id == TCls_Creature) && (creatng->creation_turn == lw->creation_turn))
            res = lair_try_attach(creatng, lw->stl_x, lw->stl_y);
        if (res > 0)
            SYNCLOG("World: %s index %d: lair at (%d,%d) back", thing_model_name(creatng), (int)creatng->index,
                (int)lw->stl_x, (int)lw->stl_y);
        if (res < 0)
            i++;
        else
            lair_wait_drop(i);
    }
}

static void freeze_thing(const struct Thing *thing, MapSubtlCoord stl_x0, MapSubtlCoord stl_y0, struct WorldChunkData *cd)
{
    if (cd->thing_count >= WORLD_MAX_THINGS_PER_CHUNK)
        return;
    struct WorldThing *th = &cd->things[cd->thing_count];
    memset(th, 0, sizeof(*th));
    th->oclass = thing->class_id;
    th->model = thing->model;
    th->owner = owner_to_world(thing->owner);
    th->x = thing->mappos.x.val - stl_x0 * COORD_PER_STL;
    th->y = thing->mappos.y.val - stl_y0 * COORD_PER_STL;
    th->z = thing->mappos.z.val;
    th->health = thing->health;
    switch (thing->class_id)
    {
    case TCls_Creature:
    {
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl))
        {
            th->level = (uint8_t)(cctrl->exp_level + 1);
            th->exp_points = (uint32_t)cctrl->exp_points;
            th->extra = cctrl->party.original_objective; /* day-raid heroes: CHeroTsk_AttackEnemies */
            if ((thing->owner == PLAYER_GOOD) && (th->extra > 0) && (cctrl->party.target_plyr_idx > PLAYER0) &&
                (cctrl->party.target_plyr_idx < PLAYERS_COUNT))
                th->extra |= (uint16_t)((cctrl->party.target_plyr_idx + 1) << 8); /* raid target other than PLAYER0 */
            memcpy(th->name, cctrl->creature_name, sizeof(th->name));
            th->name[sizeof(th->name) - 1] = 0;
            /* its mood, hunger, wages and kills */
            if (cd->ext_count < WORLD_MAX_THINGS_PER_CHUNK)
            {
                struct WorldCreatureExt *e = &cd->ext[cd->ext_count++];
                memset(e, 0, sizeof(*e));
                e->ordinal = (uint16_t)cd->thing_count;
                e->hunger_amount = cctrl->hunger_amount;
                e->hunger_loss = cctrl->hunger_loss;
                e->hunger_level = cctrl->hunger_level;
                for (int k = 0; k < 5; k++)
                    e->annoyance_level[k] = cctrl->annoyance_level[k];
                e->paydays_owed = cctrl->paydays_owed;
                e->paydays_advanced = cctrl->paydays_advanced;
                e->mood_flags = cctrl->mood_flags;
                e->joining_age = cctrl->joining_age;
                e->kills_num = cctrl->kills_num;
                e->kills_num_allied = cctrl->kills_num_allied;
                e->kills_num_enemy = cctrl->kills_num_enemy;
                e->blood_type = cctrl->blood_type;
            }
            if (creature_is_kept_in_prison(thing))
                th->flags |= WTF_Prisoner;
            else if (creature_is_being_tortured(thing))
                th->flags |= WTF_Tortured;
        }
        th->gold = (uint32_t)thing->creature.gold_carried;
        th->z = -1;
        /* its lair (standing, or waiting for its ground to load), relative to the chunk */
        MapSubtlCoord lx = 0, ly = 0;
        TbBool has_lair = false;
        if (!creature_control_invalid(cctrl) && (cctrl->lairtng_idx > 0))
        {
            const struct Thing *lairtng = thing_get(cctrl->lairtng_idx);
            if (thing_exists(lairtng))
            {
                lx = lairtng->mappos.x.stl.num;
                ly = lairtng->mappos.y.stl.num;
                has_lair = true;
            }
        }
        const int wait = lair_wait_find(thing);
        if (wait >= 0)
        {
            if (!has_lair)
            {
                lx = W.lair_waits[wait].stl_x;
                ly = W.lair_waits[wait].stl_y;
                has_lair = true;
            }
            lair_wait_drop(wait);
        }
        const int32_t ox = lx - stl_x0, oy = ly - stl_y0;
        if (has_lair && (ox >= -32768) && (ox <= 32767) && (oy >= -32768) && (oy <= 32767))
        {
            th->flags |= WTF_Lair;
            th->z = (int32_t)(((uint32_t)(uint16_t)(int16_t)oy << 16) | (uint32_t)(uint16_t)(int16_t)ox);
        }
        break;
    }
    case TCls_Object:
        if (object_is_gold_pile(thing) || thing_is_gold_hoard(thing))
            th->gold = (uint32_t)thing->valuable.gold_stored;
        if (object_is_hero_gate(thing))
            th->extra = thing->hero_gate.number;
        else if (thing_is_custom_special_box(thing))
            th->extra = thing->custom_box.box_kind;
        break;
    case TCls_Door:
        if (thing->door.is_locked)
            th->flags |= WTF_Locked;
        break;
    case TCls_Trap:
        th->extra = (uint16_t)thing->trap.num_shots;
        break;
    case TCls_DeadCreature:
        th->level = (uint8_t)(thing->corpse.exp_level + 1);
        if (thing->corpse.laid_to_rest)
            th->flags |= WTF_LaidToRest;
        th->z = -1;
        break;
    default:
        break;
    }
    cd->thing_count++;
}

/** Removes a thing from the map without any of the effects of dying or being sold. */
static void remove_thing_quietly(struct Thing *thing)
{
    if (thing->class_id == TCls_Creature)
    {
        thing->creature.gold_carried = 0;
        unsigned char mode = game.mode_flags;
        game.mode_flags &= ~WORLD_MFLG_DEADBACKTOPOOL;
        kill_creature(thing, INVALID_THING, -1, CrDed_NoEffects | CrDed_NotReallyDying);
        game.mode_flags = mode;
        return;
    }
    if (thing->class_id == TCls_Object && thing_is_dungeon_heart(thing))
    {
        delete_thing_structure(thing, 0);
        return;
    }
    if (thing->class_id == TCls_Object && thing_is_gold_hoard(thing))
    {
        struct Room *room = get_room_thing_is_on(thing);
        if (!room_is_invalid(room) && (thing->valuable.gold_stored > 0))
        {
            remove_gold_from_hoarde(thing, room, thing->valuable.gold_stored);
            return;
        }
    }
    if (thing->class_id == TCls_Object && thing_is_lair_totem(thing))
    {
        struct Thing *crtng = thing_get(thing->lair.belongs_to);
        if (thing_exists(crtng) && (crtng->class_id == TCls_Creature))
        {
            remove_creature_lair(crtng);
            return;
        }
    }
    delete_thing_structure(thing, 0);
}

/* Re-placing a slab deletes the dungeon heart attached to it (sets its health to 0) and places a new one. The slabs
 * round a chunk that loads are re-placed (walls and columns meet the new ground), and deleting a freezing chunk's
 * room slabs re-places the room slabs next to them; a keeper's heart room can straddle a chunk border. A heart on
 * those slabs is detached while that happens, and a heart placed meanwhile on its slab is removed. */
struct HeartGuard {
    struct Thing *heart;
    long parent_idx;
};
#define HEART_GUARDS_MAX 16

/** Detaches the hearts standing on slabs of the box [x1,x2]x[y1,y2] outside [ex1,ex2]x[ey1,ey2]. */
static int guard_hearts(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2, MapSlabCoord ex1,
    MapSlabCoord ey1, MapSlabCoord ex2, MapSlabCoord ey2, struct HeartGuard *guards)
{
    int n = 0;
    for (MapSlabCoord y = max(y1, 0); y <= y2; y++)
    {
        for (MapSlabCoord x = max(x1, 0); x <= x2; x++)
        {
            if ((x >= ex1) && (x <= ex2) && (y >= ey1) && (y <= ey2))
                continue;
            for (int k = 0; k < STL_PER_SLB * STL_PER_SLB; k++)
            {
                struct Map *mapblk = get_map_block_at(x * STL_PER_SLB + k % STL_PER_SLB, y * STL_PER_SLB + k / STL_PER_SLB);
                if (map_block_invalid(mapblk))
                    continue;
                unsigned long guard_steps = 0;
                for (struct Thing *thing = thing_get(get_mapwho_thing_index(mapblk)); !thing_is_invalid(thing);
                     thing = thing_get(thing->next_on_mapblk))
                {
                    if (++guard_steps > THINGS_COUNT)
                        break;
                    if ((thing->class_id != TCls_Object) || !thing_is_dungeon_heart(thing) || (thing->parent_idx < 0) ||
                        (n >= HEART_GUARDS_MAX))
                        continue;
                    guards[n].heart = thing;
                    guards[n].parent_idx = thing->parent_idx;
                    thing->parent_idx = -1;
                    n++;
                }
            }
        }
    }
    return n;
}

static struct Thing *find_heart_near(MapSubtlCoord stl_x, MapSubtlCoord stl_y, const struct Thing *except);

static void unguard_hearts(const struct HeartGuard *guards, int n)
{
    for (int i = 0; i < n; i++)
    {
        struct Thing *heart = guards[i].heart;
        heart->parent_idx = guards[i].parent_idx;
        for (int tries = 0; tries < 4; tries++)
        {
            struct Thing *copy = find_heart_near(heart->mappos.x.stl.num, heart->mappos.y.stl.num, heart);
            if (!thing_exists(copy) || (copy->owner != heart->owner))
                break;
            struct Dungeon *dungeon = get_players_num_dungeon(copy->owner);
            if (!dungeon_invalid(dungeon))
            {
                if (dungeon->backup_heart_idx == copy->index)
                    dungeon->backup_heart_idx = 0;
                if (dungeon->dnheart_idx == copy->index)
                    dungeon->dnheart_idx = heart->index;
            }
            SYNCLOG("World: kept the heart of player %d (index %d) through a chunk border, removed its copy %d",
                (int)heart->owner, (int)heart->index, (int)copy->index);
            delete_thing_structure(copy, 0);
        }
    }
}

/** Freezes a chunk of the window into the store; false when its record couldn't be stored (chunk_buf still holds it). */
static TbBool freeze_window_chunk(int lcx, int lcy)
{
    struct WorldChunkData *cd = &chunk_buf;
    memset(cd, 0, sizeof(*cd));
    cd->cx = W.origin_cx + lcx;
    cd->cy = W.origin_cy + lcy;
    const MapSlabCoord sx0 = window_chunk_slab(lcx), sy0 = window_chunk_slab(lcy);
    const MapSubtlCoord stl_x0 = sx0 * STL_PER_SLB, stl_y0 = sy0 * STL_PER_SLB;
    /* slabs */
    for (int ly = 0; ly < WG_CHUNK; ly++)
    {
        for (int lx = 0; lx < WG_CHUNK; lx++)
        {
            struct SlabMap *slb = get_slabmap_block(sx0 + lx, sy0 + ly);
            int i = ly * WG_CHUNK + lx;
            SlabKind kind = slb->kind;
            if (slab_kind_is_door(kind))
                kind = SlbT_CLAIMED; /* the door thing is stored, and placed again over claimed ground */
            cd->kind[i] = (uint8_t)kind;
            cd->owner[i] = owner_to_world(slabmap_owner(slb));
            cd->health[i] = (uint16_t)max(0, min(65535, (long)slb->health));
            SlabCodedCoords slb_num = get_slab_number(sx0 + lx, sy0 + ly);
            cd->texture[i] = (*slab_ext_ptr(slb_num));
            cd->texture_initial[i] = (*slab_ext_initial_ptr(slb_num));
            /* dig orders (taken off the map with the slabs in clear_band, marked again when the chunk returns) */
            SubtlCodedCoords tsk = get_subtile_number_at_slab_center(sx0 + lx, sy0 + ly);
            for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
                if (player_is_keeper(p) && (find_from_task_list(p, tsk) != -1))
                    pmask_add(&cd->dig[i], p);
        }
    }
    /* revealed flags and the map memory */
    TbBool any_revealed = false;
    uint16_t *codes = (uint16_t *)calloc(CHUNK_STL_CELLS, sizeof(uint16_t));
    for (int y = 0; y < CHUNK_STL; y++)
    {
        for (int x = 0; x < CHUNK_STL; x++)
        {
            struct Map *mapblk = get_map_block_at(stl_x0 + x, stl_y0 + y);
            cd->revealed[y * CHUNK_STL + x] = mapblk->revealed;
            if (codes != NULL)
            {
                codes[y * CHUNK_STL + x] = panel_map_code_at(stl_x0 + x, stl_y0 + y);
                if (codes[y * CHUNK_STL + x] != 0)
                    any_revealed = true;
            }
        }
    }
    if (codes != NULL)
    {
        if (any_revealed)
        {
            struct ChunkSlot *s = table_insert(&W.memory, cd->cx, cd->cy);
            world_memory_gen++;
            if (s != NULL)
            {
                free(s->data);
                s->data = (uint8_t *)codes;
                s->len = CHUNK_STL_CELLS * sizeof(uint16_t);
                codes = NULL;
            }
        }
        free(codes);
    }
    /* things: everything standing in the chunk, except what is held in a hand */
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || thing_is_picked_up(thing))
            continue;
        MapSubtlCoord tx = thing->mappos.x.stl.num, ty = thing->mappos.y.stl.num;
        if ((tx < stl_x0) || (tx >= stl_x0 + CHUNK_STL) || (ty < stl_y0) || (ty >= stl_y0 + CHUNK_STL))
            continue;
        if ((thing->class_id == TCls_Object) && thing_is_lair_totem(thing))
        {
            /* a creature that isn't frozen with this chunk gets this lair back when the chunk loads again */
            struct Thing *crtng = thing_get(thing->lair.belongs_to);
            if (thing_exists(crtng) && (crtng->class_id == TCls_Creature) && (thing_is_picked_up(crtng) ||
                (crtng->mappos.x.stl.num < stl_x0) || (crtng->mappos.x.stl.num >= stl_x0 + CHUNK_STL) ||
                (crtng->mappos.y.stl.num < stl_y0) || (crtng->mappos.y.stl.num >= stl_y0 + CHUNK_STL)))
                lair_wait_add(crtng, tx, ty);
        }
        if (thing_is_frozen_kind(thing))
        {
            freeze_thing(thing, stl_x0, stl_y0, cd);
            if (thing_is_dungeon_heart(thing))
            {
                /* while its heart is frozen, a player doesn't need one */
                game.conf.rules[thing->owner].gameplay.heart_required = 0;
                if (world_player_is_human(thing->owner))
                    pmask_add(&W.player_heart_frozen, thing->owner);
                for (int k = 0; k < W.keeper_count; k++)
                    if (W.keepers[k].slot == thing->owner)
                        W.keepers[k].heart_frozen = 1;
            }
        }
    }
    /* spellbooks and workshop crates in the chunk's rooms: off the map now, out of their owners' totals (the thaw
       assimilates them again). Deleting the room slabs would move them onto room slabs outside the chunk, where they
       would stay while the frozen record also holds them. */
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Object) || thing_is_picked_up(thing) ||
            thing_is_dragged_or_pulled(thing) || (!thing_is_spellbook(thing) && !thing_is_workshop_crate(thing)))
            continue;
        MapSubtlCoord tx = thing->mappos.x.stl.num, ty = thing->mappos.y.stl.num;
        if ((tx < stl_x0) || (tx >= stl_x0 + CHUNK_STL) || (ty < stl_y0) || (ty >= stl_y0 + CHUNK_STL))
            continue;
        struct Room *room = get_room_thing_is_on(thing);
        if (!room_is_invalid(room) && (thing->owner == room->owner) && !is_neutral_thing(thing))
        {
            if (thing_is_spellbook(thing) && room_role_matches(room->kind, RoRoF_PowersStorage))
            {
                remove_power_from_player(book_thing_to_power_kind(thing), thing->owner);
            } else
            if (thing_is_workshop_crate(thing) && room_role_matches(room->kind, RoRoF_CratesStorage))
            {
                const ThingClass tngclass = crate_thing_to_workshop_item_class(thing);
                const ThingModel tngmodel = crate_thing_to_workshop_item_model(thing);
                if (remove_workshop_item_from_amount_stored(thing->owner, tngclass, tngmodel, WrkCrtF_NoOffmap) > WrkCrtS_None)
                    remove_workshop_item_from_amount_placeable(thing->owner, tngclass, tngmodel);
            }
        }
        delete_thing_structure(thing, 0);
    }
    /* rooms first: the engine takes their contents (treasury gold, books, boxes) out of their owners' totals; the
       room slabs next to the chunk are re-placed meanwhile, so a heart there is guarded */
    struct HeartGuard guards[HEART_GUARDS_MAX];
    const int nguards = guard_hearts(sx0 - 1, sy0 - 1, sx0 + WG_CHUNK, sy0 + WG_CHUNK, sx0, sy0, sx0 + WG_CHUNK - 1,
        sy0 + WG_CHUNK - 1, guards);
    for (int ly = 0; ly < WG_CHUNK; ly++)
    {
        for (int lx = 0; lx < WG_CHUNK; lx++)
        {
            struct SlabMap *slb = get_slabmap_block(sx0 + lx, sy0 + ly);
            if (slb->room_index > 0)
                delete_room_slab(sx0 + lx, sy0 + ly, 0);
        }
    }
    unguard_hearts(guards, nguards);
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || thing_is_picked_up(thing))
            continue;
        MapSubtlCoord tx = thing->mappos.x.stl.num, ty = thing->mappos.y.stl.num;
        if ((tx < stl_x0) || (tx >= stl_x0 + CHUNK_STL) || (ty < stl_y0) || (ty >= stl_y0 + CHUNK_STL))
            continue;
        if (thing_is_slab_object(thing))
            continue; /* goes with its slab */
        remove_thing_quietly(thing);
    }
    if (!store_chunk_record(cd))
    {
        ERRORLOG("World: chunk (%d,%d) could not be stored", (int)cd->cx, (int)cd->cy);
        return false;
    }
    return true;
}

/** A slab's dig orders, for every keeper. Unlike all_players_untag_blocks_for_digging_in_area this also takes
 *  orders on slabs the player hasn't explored: left in the task list, the shift would clamp them onto the
 *  window's edge, and imps would dig there. */
static void untag_slab_for_all_keepers(MapSlabCoord x, MapSlabCoord y)
{
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        if (player_is_keeper(p) && (find_from_task_list(p, get_subtile_number_at_slab_center(x, y)) != -1))
            untag_blocks_for_digging_in_area(slab_subtile(x, 0), slab_subtile(y, 0), p);
}

/** Takes the room slabs and dig tags of a band of slabs out of the game, and releases their columns. */
static void clear_band(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2)
{
    x1 = max(x1, 0); y1 = max(y1, 0);
    x2 = min(x2, game.map_tiles_x - 1); y2 = min(y2, game.map_tiles_y - 1);
    if ((x2 < x1) || (y2 < y1))
        return;
    for (MapSlabCoord y = y1; y <= y2; y++)
    {
        for (MapSlabCoord x = x1; x <= x2; x++)
        {
            struct SlabMap *slb = get_slabmap_block(x, y);
            if (slabmap_block_invalid(slb))
                continue;
            if (slb->room_index > 0)
                delete_room_slab(x, y, 0);
            untag_slab_for_all_keepers(x, y);
            delete_attached_lights_on_slab(x, y);
        }
    }
    /* slab objects and lights left on those slabs */
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || thing_is_picked_up(thing))
            continue;
        MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
        if ((sx >= x1) && (sx <= x2) && (sy >= y1) && (sy <= y2))
            remove_thing_quietly(thing);
    }
    for (MapSubtlCoord y = y1 * STL_PER_SLB; y < (y2 + 1) * STL_PER_SLB; y++)
        for (MapSubtlCoord x = x1 * STL_PER_SLB; x < (x2 + 1) * STL_PER_SLB; x++)
            remove_block_from_map_element(x, y);
}

/******************************************************************************/
/* Filling chunks into the window */

/** A dungeon heart other than `except` within a subtile of (stl_x, stl_y). */
static struct Thing *find_heart_near(MapSubtlCoord stl_x, MapSubtlCoord stl_y, const struct Thing *except)
{
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing == except) || (thing->class_id != TCls_Object) || !thing_is_dungeon_heart(thing))
            continue;
        if ((abs(thing->mappos.x.stl.num - stl_x) <= 1) && (abs(thing->mappos.y.stl.num - stl_y) <= 1))
            return thing;
    }
    return INVALID_THING;
}

static void place_world_thing(const struct WorldThing *th, const struct WorldCreatureExt *ext, MapSubtlCoord stl_x0,
    MapSubtlCoord stl_y0)
{
    struct Coord3d pos;
    pos.x.val = stl_x0 * COORD_PER_STL + th->x;
    pos.y.val = stl_y0 * COORD_PER_STL + th->y;
    pos.z.val = 0;
    if ((pos.x.stl.num < stl_x0) || (pos.x.stl.num >= stl_x0 + CHUNK_STL) ||
        (pos.y.stl.num < stl_y0) || (pos.y.stl.num >= stl_y0 + CHUNK_STL))
        return;
    pos.z.val = get_floor_height_at(&pos);
    if ((th->z > 0) && (th->oclass != TCls_Creature))
        pos.z.val = max(pos.z.val, th->z);
    PlayerNumber owner = owner_to_engine(th->owner);
    struct Thing *thing = INVALID_THING;
    switch (th->oclass)
    {
    case TCls_Creature:
    {
        thing = create_creature(&pos, th->model, owner);
        if (thing_is_invalid(thing))
            return;
        init_creature_level(thing, (th->level > 0) ? th->level - 1 : 0);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl) && (th->name[0] != 0))
        {
            memcpy(cctrl->creature_name, th->name, sizeof(cctrl->creature_name));
            cctrl->creature_name[sizeof(cctrl->creature_name) - 1] = 0;
        }
        if (!creature_control_invalid(cctrl))
            cctrl->exp_points = th->exp_points;
        if (!creature_control_invalid(cctrl) && (th->extra > 0) && (owner == PLAYER_GOOD))
        {
            cctrl->party.original_objective = (unsigned char)(th->extra & 0xFF);
            cctrl->party.objective = (unsigned char)(th->extra & 0xFF);
            cctrl->party.target_plyr_idx = (th->extra >> 8) ? (PlayerNumber)((th->extra >> 8) - 1) : PLAYER0;
        }
        if (th->health > 0)
            thing->health = th->health;
        thing->creature.gold_carried = th->gold;
        if ((ext != NULL) && !creature_control_invalid(cctrl))
        {
            cctrl->hunger_amount = ext->hunger_amount;
            cctrl->hunger_loss = ext->hunger_loss;
            cctrl->hunger_level = ext->hunger_level;
            for (int k = 0; k < 5; k++)
                cctrl->annoyance_level[k] = ext->annoyance_level[k];
            cctrl->paydays_owed = ext->paydays_owed;
            cctrl->paydays_advanced = ext->paydays_advanced;
            cctrl->mood_flags = ext->mood_flags;
            cctrl->joining_age = ext->joining_age;
            cctrl->kills_num = ext->kills_num;
            cctrl->kills_num_allied = ext->kills_num_allied;
            cctrl->kills_num_enemy = ext->kills_num_enemy;
            cctrl->blood_type = ext->blood_type;
        }
        if ((th->flags & (WTF_Prisoner | WTF_Tortured)) && !creature_control_invalid(cctrl))
        {
            /* back into the captor's prison or torture room it was frozen in */
            const TbBool prisoner = (th->flags & WTF_Prisoner) != 0;
            struct Room *room = get_room_thing_is_on(thing);
            if (!room_is_invalid(room) && (room->owner != thing->owner) &&
                room_role_matches(room->kind, get_room_role_for_job(prisoner ? Job_CAPTIVITY : Job_PAINFUL_TORTURE)))
            {
                cleanup_current_thing_state(thing);
                internal_set_thing_state(thing, prisoner ? CrSt_CreatureArrivedAtPrison : CrSt_AtTortureRoom);
            }
        }
        if ((th->flags & WTF_Lair) && !creature_control_invalid(cctrl))
        {
            const MapSubtlCoord lx = stl_x0 + (int16_t)(uint16_t)((uint32_t)th->z & 0xFFFFu);
            const MapSubtlCoord ly = stl_y0 + (int16_t)(uint16_t)((uint32_t)th->z >> 16);
            const int res = lair_try_attach(thing, lx, ly);
            if (res < 0)
                lair_wait_add(thing, lx, ly);
            SYNCLOG("World: %s index %d: lair at (%d,%d) %s", thing_model_name(thing), (int)thing->index,
                (int)lx, (int)ly, (res > 0) ? "back" : ((res < 0) ? "waits for its ground" : "gone"));
        }
        break;
    }
    case TCls_Object:
    {
        if (th->model == ObjMdl_SoulCountainer)
        {
            struct Thing *heart = find_heart_near(pos.x.stl.num, pos.y.stl.num, INVALID_THING);
            if (thing_exists(heart))
            {
                if (th->health > 0)
                    heart->health = th->health;
                break;
            }
        }
        thing = create_object(&pos, th->model, owner, -1);
        if (thing_is_invalid(thing))
            return;
        if (object_is_hero_gate(thing))
            thing->hero_gate.number = (unsigned char)th->extra;
        else if (thing_is_custom_special_box(thing))
            thing->custom_box.box_kind = (unsigned char)th->extra;
        if ((th->gold > 0) && object_is_gold_pile(thing))
        {
            thing->valuable.gold_stored = 0;
            add_gold_to_pile(thing, th->gold);
        }
        else if ((th->gold > 0) && thing_is_gold_hoard(thing))
        {
            thing->valuable.gold_stored = th->gold; /* the treasury takes it in below */
        }
        if (th->health > 0)
            thing->health = th->health;
        check_and_asimilate_thing_by_room(thing);
        break;
    }
    case TCls_Trap:
        thing = create_trap(&pos, th->model, owner);
        if (!thing_is_invalid(thing))
            thing->trap.num_shots = th->extra;
        break;
    case TCls_DeadCreature:
    {
        /* a body; one laid to rest goes back into its graveyard, with its time to rise */
        const TbBool laid = (th->flags & WTF_LaidToRest) != 0;
        thing = create_dead_creature(&pos, th->model, laid ? DCrSt_Dying : DCrSt_Dead, owner,
            (CrtrExpLevel)((th->level > 0) ? th->level - 1 : 0));
        if (thing_is_invalid(thing))
            return;
        if (laid)
        {
            struct Room *room = get_room_thing_is_on(thing);
            if (!room_is_invalid(room) && room_role_matches(room->kind, RoRoF_DeadStorage) &&
                (room->used_capacity < room->total_capacity))
            {
                room->used_capacity++;
                thing->corpse.laid_to_rest = 1;
            }
        }
        if (th->health > 0)
            thing->health = th->health;
        break;
    }
    case TCls_Door:
        if (player_place_door_without_check_at(pos.x.stl.num, pos.y.stl.num, owner, th->model, true))
        {
            thing = get_door_for_position(pos.x.stl.num, pos.y.stl.num);
            if (!thing_is_invalid(thing))
            {
                if (th->flags & WTF_Locked)
                    lock_door(thing);
                if (th->health > 0)
                    thing->health = th->health;
            }
        }
        break;
    default:
        break;
    }
}

struct FillChunk {
    int lcx, lcy;
    struct WorldChunkData *cd;
};

/* player_utils.c, not in a header: what init_keeper_map_exploration_by_terrain shows a keeper at the level start */
TbBool map_position_initially_explored_for_player(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);

/** What a level start shows every keeper (init_keeper_map_exploration_by_terrain): all rock, gold and gems,
 *  neutral portals and the ground round them, and their own ground, in the chunk at engine slab (sx0, sy0). The
 *  level start only covers the chunks there are then; chunks that come in later get it here, so the world's
 *  rock masses and gold show on the maps as the player's creatures load new ground. */
static int explore_like_level_start(MapSlabCoord sx0, MapSlabCoord sy0)
{
    int explored = 0;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!player_exists(get_player(p)) || !player_is_keeper(p))
            continue;
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            MapSlabCoord x = sx0 + i % WG_CHUNK, y = sy0 + i / WG_CHUNK;
            if (map_position_initially_explored_for_player(p, x, y) && set_slab_explored(p, x, y))
                explored++;
        }
    }
    return explored;
}

/** Writes chunks into the window. The band's columns must have been released (clear_band). */
static void fill_chunks(struct FillChunk *list, int count)
{
    /* 1. slab kinds and owners, raw */
    for (int c = 0; c < count; c++)
    {
        struct WorldChunkData *cd = list[c].cd;
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            struct SlabMap *slb = get_slabmap_block(sx0 + i % WG_CHUNK, sy0 + i / WG_CHUNK);
            slb->kind = cd->kind[i];
            slb->room_index = 0;
            slb->next_in_room = 0;
            SlabCodedCoords slb_num = get_slab_number(sx0 + i % WG_CHUNK, sy0 + i / WG_CHUNK);
            (*slab_ext_initial_ptr(slb_num)) = cd->texture_initial[i];
            set_slab_owner(sx0 + i % WG_CHUNK, sy0 + i / WG_CHUNK, owner_to_engine(cd->owner[i]));
            (*slab_ext_ptr(slb_num)) = cd->texture[i];
        }
    }
    /* 2. automatic walls and earth next to claimed ground */
    for (int c = 0; c < count; c++)
    {
        struct WorldChunkData *cd = list[c].cd;
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            MapSlabCoord x = sx0 + i % WG_CHUNK, y = sy0 + i / WG_CHUNK;
            struct SlabMap *slb = get_slabmap_block(x, y);
            if (cd->wall[i])
                slb->kind = choose_pretty_type(slabmap_owner(slb), x, y);
            else if (slb->kind == SlbT_EARTH)
                slb->kind = alter_rock_style(SlbT_EARTH, x, y, slabmap_owner(slb));
        }
    }
    /* 3. columns and slab objects, the band and the ring of slabs around it */
    MapSlabCoord bx1 = 0x7FFFFFFF, by1 = 0x7FFFFFFF, bx2 = -1, by2 = -1; /* paged slabs are about a million */
    for (int c = 0; c < count; c++)
    {
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        bx1 = min(bx1, sx0); by1 = min(by1, sy0);
        bx2 = max(bx2, sx0 + WG_CHUNK - 1); by2 = max(by2, sy0 + WG_CHUNK - 1);
    }
    struct HeartGuard guards[HEART_GUARDS_MAX]; /* a heart on the ring (a neighbour chunk's) is kept as it is */
    const int nguards = guard_hearts(bx1 - 1, by1 - 1, min(bx2 + 1, game.map_tiles_x - 1), min(by2 + 1, game.map_tiles_y - 1),
        bx1, by1, bx2, by2, guards);
    for (MapSlabCoord y = max(by1 - 1, 0); y <= min(by2 + 1, game.map_tiles_y - 1); y++)
    {
        for (MapSlabCoord x = max(bx1 - 1, 0); x <= min(bx2 + 1, game.map_tiles_x - 1); x++)
        {
            struct SlabMap *slb = get_slabmap_block(x, y);
            if (slabmap_block_invalid(slb))
                continue; /* paged mode: a chunk that isn't loaded */
            TbBool in_band = (x >= bx1) && (x <= bx2) && (y >= by1) && (y <= by2);
            HitPoints health = slb->health;
            place_single_slab_type_on_map(slb->kind, x, y, slabmap_owner(slb));
            if (!in_band)
                slb->health = health;
        }
    }
    unguard_hearts(guards, nguards);
    /* 4. health, rooms, doors, things, revealed flags */
    for (int c = 0; c < count; c++)
    {
        struct WorldChunkData *cd = list[c].cd;
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            struct SlabMap *slb = get_slabmap_block(sx0 + i % WG_CHUNK, sy0 + i / WG_CHUNK);
            if (cd->health[i] > 0)
                slb->health = cd->health[i];
        }
    }
    for (int c = 0; c < count; c++)
    {
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            MapSlabCoord x = sx0 + i % WG_CHUNK, y = sy0 + i / WG_CHUNK;
            struct SlabMap *slb = get_slabmap_block(x, y);
            RoomKind rkind = slab_corresponding_room(slb->kind);
            if ((rkind > 0) && (slb->room_index == 0))
            {
                struct Room *room = create_room(slabmap_owner(slb), rkind, slab_subtile_center(x), slab_subtile_center(y));
                if (!room_is_invalid(room))
                    do_room_integration(room);
            }
        }
    }
    for (int c = 0; c < count; c++)
    {
        struct WorldChunkData *cd = list[c].cd;
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            if (cd->door[i] == 0)
                continue;
            MapSlabCoord x = sx0 + i % WG_CHUNK, y = sy0 + i / WG_CHUNK;
            PlayerNumber owner = owner_to_engine(cd->owner[i]);
            player_place_door_without_check_at(slab_subtile_center(x), slab_subtile_center(y), owner, cd->door[i], true);
        }
    }
    for (int c = 0; c < count; c++)
    {
        struct WorldChunkData *cd = list[c].cd;
        const MapSubtlCoord stl_x0 = window_chunk_slab(list[c].lcx) * STL_PER_SLB;
        const MapSubtlCoord stl_y0 = window_chunk_slab(list[c].lcy) * STL_PER_SLB;
        static int16_t ext_of[WORLD_MAX_THINGS_PER_CHUNK];
        for (int t = 0; t < cd->thing_count; t++)
            ext_of[t] = -1;
        for (int e = 0; e < cd->ext_count; e++)
            if (cd->ext[e].ordinal < cd->thing_count)
                ext_of[cd->ext[e].ordinal] = (int16_t)e;
        for (int t = 0; t < cd->thing_count; t++)
            place_world_thing(&cd->things[t], (ext_of[t] >= 0) ? &cd->ext[ext_of[t]] : NULL, stl_x0, stl_y0);
        /* test hook: KFX_WORLD_REVEAL=1 shows every chunk that comes in (only read in world levels) */
        static int reveal_all = -1;
        if (reveal_all < 0)
            reveal_all = (getenv("KFX_WORLD_REVEAL") != NULL) ? 1 : 0;
        PlayerBitFlags everyone = pmask_none();
        if (reveal_all) /* every seat, even one seated after the chunk came in */
            everyone = pmask_or(world_human_mask(), (game.game_kind == GKind_MultiGame) ? world_seat_mask() : pmask_none());
        for (int y = 0; y < CHUNK_STL; y++)
        {
            for (int x = 0; x < CHUNK_STL; x++)
            {
                const PlayerBitFlags bits = pmask_or(cd->revealed[y * CHUNK_STL + x], everyone);
                for (int k = 0; k < PLAYER_MASK_WORDS; k++)
                {
                    for (uint32_t v = bits.w[k]; v != 0; v &= v - 1)
                    {
                        const int pl = k * 32 + __builtin_ctz(v);
                        if (pl < PLAYERS_COUNT)
                            reveal_map_subtile(stl_x0 + x, stl_y0 + y, (PlayerNumber)pl);
                    }
                }
            }
        }
        explore_like_level_start(window_chunk_slab(list[c].lcx), window_chunk_slab(list[c].lcy));
    }
    /* 5. dig orders stored with the chunk, as the hand marks them */
    for (int c = 0; c < count; c++)
    {
        struct WorldChunkData *cd = list[c].cd;
        MapSlabCoord sx0 = window_chunk_slab(list[c].lcx), sy0 = window_chunk_slab(list[c].lcy);
        for (int i = 0; i < WG_CHUNK_CELLS; i++)
        {
            if (!pmask_any(cd->dig[i]))
                continue;
            for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
                if (pmask_has(cd->dig[i], p) && player_is_keeper(p))
                    tag_blocks_for_digging_in_area(slab_subtile(sx0 + i % WG_CHUNK, 0), slab_subtile(sy0 + i / WG_CHUNK, 0), p);
        }
    }
}

/** The rock frame round the window. */
static void place_frame(void)
{
    if (world_paged)
        return; /* no frame: chunks that aren't loaded are the border */
    for (MapSlabCoord y = 0; y < game.map_tiles_y; y++)
    {
        for (MapSlabCoord x = 0; x < game.map_tiles_x; x++)
        {
            if ((x != 0) && (y != 0) && (x != game.map_tiles_x - 1) && (y != game.map_tiles_y - 1))
                continue;
            struct SlabMap *slb = get_slabmap_block(x, y);
            if (slb->room_index > 0)
                delete_room_slab(x, y, 0);
            slb->kind = SlbT_ROCK;
            (*slab_ext_initial_ptr(get_slab_number(x, y))) = 0;
            set_slab_owner(x, y, game.neutral_player_num);
            (*slab_ext_ptr(get_slab_number(x, y))) = 0;
            place_single_slab_type_on_map(SlbT_ROCK, x, y, game.neutral_player_num);
        }
    }
}

static struct WorldChunkData *chunk_data_for(int32_t cx, int32_t cy)
{
    struct WorldChunkData *cd = (struct WorldChunkData *)malloc(sizeof(struct WorldChunkData));
    if (cd == NULL)
        return NULL;
    if (!load_chunk_record(cx, cy, cd))
        generate_chunk_data(cx, cy, cd);
    return cd;
}

/******************************************************************************/
/* Keepers and hearts */

/** Removes a second heart standing on a heart: older versions placed one over the heart the pedestal makes. */
static void remove_duplicate_hearts(void)
{
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *heart = thing_get(i);
        if (!thing_exists(heart) || (heart->class_id != TCls_Object) || !thing_is_dungeon_heart(heart))
            continue;
        struct Thing *other = find_heart_near(heart->mappos.x.stl.num, heart->mappos.y.stl.num, heart);
        if (!thing_exists(other) || (other->owner != heart->owner))
            continue;
        struct Thing *keep = thing_is_slab_object(other) ? other : heart;
        struct Thing *drop = (keep == other) ? heart : other;
        if ((keep->health <= 0) && (drop->health > 0))
        {
            /* a live heart wins over a dead one; it takes over the slab attachment */
            struct Thing *live = drop;
            drop = keep;
            keep = live;
            if (thing_is_slab_object(drop) && !thing_is_slab_object(keep))
                keep->parent_idx = drop->parent_idx;
        }
        if ((drop->health > 0) && (drop->health < keep->health))
            keep->health = drop->health;
        SYNCLOG("World: removed a second dungeon heart of player %d (index %d, keeping %d)", (int)drop->owner,
            (int)drop->index, (int)keep->index);
        struct Dungeon *dungeon = get_players_num_dungeon(drop->owner);
        if (!dungeon_invalid(dungeon) && (dungeon->dnheart_idx == drop->index))
            dungeon->dnheart_idx = keep->index;
        if (!dungeon_invalid(dungeon) && (dungeon->backup_heart_idx == drop->index))
            dungeon->backup_heart_idx = 0;
        delete_thing_structure(drop, 0);
    }
}

static void bind_hearts(void)
{
    remove_duplicate_hearts();
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (p == game.neutral_player_num)
            continue;
        if (p == PLAYER_GOOD)
        {
            /* the heroes' hearts (outposts) stay unbound: one destroyed only explodes with its room, it doesn't start
               the heroes' "dungeon destroyed" sweep over every loaded hero door and floor */
            struct Dungeon *gd = get_players_num_dungeon(p);
            if (!dungeon_invalid(gd) && (gd->heart_destroy_state == 0))
            {
                gd->dnheart_idx = 0;
                gd->backup_heart_idx = 0;
            }
            continue;
        }
        struct Thing *heartng = find_players_dungeon_heart(p);
        if (!thing_exists(heartng))
            continue;
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        if (dungeon->dnheart_idx != heartng->index)
        {
            init_player_start(get_player(p), true);
        }
        /* a backup heart must be another heart of the player's */
        struct Thing *backup = thing_get(dungeon->backup_heart_idx);
        if ((dungeon->backup_heart_idx == dungeon->dnheart_idx) || !thing_exists(backup) || !thing_is_dungeon_heart(backup) ||
            (backup->owner != p))
            dungeon->backup_heart_idx = 0;
        game.conf.rules[p].gameplay.heart_required = 1;
        pmask_del(&W.player_heart_frozen, p);
        for (int k = 0; k < W.keeper_count; k++)
            if (W.keepers[k].slot == p)
                W.keepers[k].heart_frozen = 0;
    }
}

static void setup_keepers(void)
{
    for (int k = 0; k < W.keeper_count; k++)
    {
        struct WorldKeeper *wk = &W.keepers[k];
        if ((wk->slot < 0) || wk->set_up || (wk->state == WKSt_Destroyed))
            continue;
        struct Thing *heartng = find_players_dungeon_heart(wk->slot);
        if (!thing_exists(heartng))
            continue;
        PlayerNumber p = wk->slot;
        if (!script_support_setup_player_as_computer_keeper(p, 1))
        {
            WARNLOG("World: keeper@%d,%d could not be set up as player %d", (int)wk->id.start_cx, (int)wk->id.start_cy, (int)p);
            continue;
        }
        struct PlayerInfo *player = get_player(p);
        post_init_player(player);
        init_creature_states_for_player(p);
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (!dungeon_invalid(dungeon))
        {
            dungeon->offmap_money_owned += wk->money;
            dungeon->total_money_owned += wk->money;
            dungeon->max_creatures_attracted = 25;
        }
        game.conf.rules[p].gameplay.heart_required = 1;
        wk->set_up = 1;
        if (wk->state < WKSt_Passive)
            wk->state = WKSt_Passive;
        SYNCLOG("World: keeper@%d,%d set up as player %d (passive, threat radius %d)", (int)wk->id.start_cx,
            (int)wk->id.start_cy, (int)p, (int)wk->threat_radius);
    }
}

static TbBool human_creature_near_keeper(const struct WorldKeeper *wk, PlayerNumber plyr_idx);

/** Is one of a human player's creatures within the keeper's threat radius of its heart? */
static TbBool player_creature_near_keeper(const struct WorldKeeper *wk)
{
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (world_player_is_human(p) && human_creature_near_keeper(wk, p))
            return true;
    }
    return false;
}

/** Is one of the player's creatures within the keeper's threat radius of its heart? */
static TbBool human_creature_near_keeper(const struct WorldKeeper *wk, PlayerNumber plyr_idx)
{
    int64_t hx = engine_slab_x(wk->heart_x), hy = engine_slab_y(wk->heart_y);
    int64_t r = wk->threat_radius;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return false;
    ThingIndex lists[2] = {dungeon->creatr_list_start, dungeon->digger_list_start};
    for (int l = 0; l < 2; l++)
    {
        long i = lists[l];
        long guard = 0;
        while ((i > 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing))
                break;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            i = creature_control_invalid(cctrl) ? 0 : cctrl->players_next_creature_idx;
            int64_t dx = subtile_slab(thing->mappos.x.stl.num) - hx, dy = subtile_slab(thing->mappos.y.stl.num) - hy;
            if (dx * dx + dy * dy <= r * r)
                return true;
        }
    }
    return false;
}

/** Is anything of a human player's (a creature, or a slab) within the keeper's threat radius of its heart? */
static TbBool player_near_keeper(const struct WorldKeeper *wk)
{
    if (player_creature_near_keeper(wk))
        return true;
    const PlayerBitFlags humans = world_human_mask();
    int64_t hx = engine_slab_x(wk->heart_x), hy = engine_slab_y(wk->heart_y);
    int64_t r = wk->threat_radius;
    for (int64_t y = max(hy - r, 0); y <= min(hy + r, (int64_t)game.map_tiles_y - 1); y++)
    {
        for (int64_t x = max(hx - r, 0); x <= min(hx + r, (int64_t)game.map_tiles_x - 1); x++)
        {
            if ((x - hx) * (x - hx) + (y - hy) * (y - hy) > r * r)
                continue;
            struct SlabMap *slb = get_slabmap_block((MapSlabCoord)x, (MapSlabCoord)y);
            if (owner_in_mask(slabmap_owner(slb), humans))
                return true;
        }
    }
    return false;
}

static int32_t engine_chunk_of(int64_t wc);

/* An awake keeper with no human near for this long goes back to sleep (10 minutes). */
#define WORLD_KEEPER_SLEEP_TURNS (20 * 60 * 10)

/** Nothing of a keeper's is loaded: no chunk of its box, no thing and no slab of its player. */
static TbBool keeper_slot_unused(const struct WorldKeeper *wk)
{
    if (!world_paged || (wk->slot < 0))
        return false;
    const PlayerNumber p = wk->slot;
    for (int64_t cy = floor_div(wk->bbox_y1, WG_CHUNK); cy <= floor_div(wk->bbox_y2, WG_CHUNK); cy++)
        for (int64_t cx = floor_div(wk->bbox_x1, WG_CHUNK); cx <= floor_div(wk->bbox_x2, WG_CHUNK); cx++)
            if (wp_page(engine_chunk_of(cx), engine_chunk_of(cy)) != NULL)
                return false;
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        const struct Thing *thing = thing_get(i);
        if (thing_index_synced(i) && thing_exists(thing) && (thing->owner == p))
            return false;
    }
    for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
        for (int i = 0; i < WP_CHUNK_SLAB_CELLS; i++)
            if (slabmap_owner(&pg->slab[i]) == p)
                return false;
    return true;
}

/** Gives a keeper's player slot back. The player and its dungeon are emptied of what was the keeper's own (the
 *  level's settings for the slot, such as what can be built, stay); its gold outside treasuries is kept for when it
 *  comes back. */
static void keeper_release_slot(int k)
{
    struct WorldKeeper *wk = &W.keepers[k];
    const PlayerNumber p = wk->slot;
    struct PlayerInfo *player = get_player(p);
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (wk->set_up && !dungeon_invalid(dungeon) && (wk->state != WKSt_Destroyed))
        wk->money = (int32_t)max((GoldAmount)0, dungeon->offmap_money_owned);
    struct Computer2 *comp = get_computer_player(p);
    if (!computer_player_invalid(comp))
    {
        for (int guard = 0; (comp->task_idx > 0) && (guard < COMPUTER_TASKS_COUNT); guard++)
        {
            struct ComputerTask *ctask = get_computer_task(comp->task_idx);
            if (computer_task_invalid(ctask) || !remove_task(comp, ctask))
                break;
        }
    }
    if (!dungeon_invalid(dungeon))
    {
        for (long t = min((long)dungeon->highest_task_number, (long)MAPTASKS_COUNT) - 1; t >= 0; t--)
        {
            const struct MapTask *mtask = &dungeon->task_list[t];
            if (mtask->kind != 0)
                untag_blocks_for_digging_in_area(stl_num_decode_x(mtask->coords), stl_num_decode_y(mtask->coords), p);
        }
        memset(dungeon->task_list, 0, sizeof(dungeon->task_list));
        dungeon->highest_task_number = 0;
        dungeon->total_money_owned = 0;
        dungeon->offmap_money_owned = 0;
        dungeon->dnheart_idx = 0;
        dungeon->backup_heart_idx = 0;
        dungeon->heart_destroy_state = 0;
        dungeon->heart_destroy_turn = 0;
        dungeon->devastation_turn = 0;
        dungeon->num_active_diggers = 0;
        dungeon->num_active_creatrs = 0;
        dungeon->creatr_list_start = 0;
        dungeon->digger_list_start = 0;
    }
    if (!player_invalid(player))
    {
        player->allocflags &= ~(PlaF_Allocated | PlaF_CompCtrl);
        player->victory_state = VicS_Undecided;
        player->allied_players = pmask_none();
    }
    for (PlayerNumber q = 0; q < PLAYERS_COUNT; q++)
        pmask_del(&get_player(q)->allied_players, p);
    /* the next keeper in this slot doesn't know the ground this one had seen */
    for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
        for (int i = 0; i < WP_CHUNK_STL_CELLS; i++)
            pmask_del(&pg->map[i].revealed, p);
    for (int i = 0; i < W.portal_count; i++)
        if (W.portals[i].owner == p)
            W.portals[i].owner = -1; /* its frozen portals don't count for the next holder of the slot */
    SYNCLOG("World: keeper@%d,%d gives player %d back (%s, %d gold kept)", (int)wk->id.start_cx, (int)wk->id.start_cy,
        (int)p, (wk->state == WKSt_Destroyed) ? "destroyed" : "asleep", (int)wk->money);
    wk->slot = -1;
    wk->set_up = 0;
    wk->heart_frozen = 0;
    if (wk->state != WKSt_Destroyed)
        wk->state = WKSt_Waiting;
}

static void update_keepers(void)
{
    for (int k = 0; k < W.keeper_count; k++)
    {
        struct WorldKeeper *wk = &W.keepers[k];
        if (wk->slot < 0)
            continue;
        struct PlayerInfo *player = get_player(wk->slot);
        if (wk->set_up && (player->victory_state == VicS_LostLevel) && (wk->state != WKSt_Destroyed))
        {
            wk->state = WKSt_Destroyed;
            SYNCLOG("World: keeper@%d,%d (player %d) is destroyed", (int)wk->id.start_cx, (int)wk->id.start_cy, (int)wk->slot);
            {
                /* its grimoires: 3 research books a grade above where it lived, at its heart */
                const int grade = min(5, world_progress_grade_at(dist_to_nearest_start(wk->heart_x, wk->heart_y)) + 1);
                world_progress_drop_books(slab_subtile_center(engine_slab_x(wk->heart_x)),
                    slab_subtile_center(engine_slab_y(wk->heart_y)), grade, wg_biome_at(W.gen, wk->heart_x, wk->heart_y), 3);
            }
            continue;
        }
        /* test hook: KFX_WORLD_WAKE=1 wakes keepers as soon as they are set up (only read in world levels) */
        static int wake_all = -1;
        if (wake_all < 0)
            wake_all = (getenv("KFX_WORLD_WAKE") != NULL) ? 1 : 0;
        if ((wk->state == WKSt_Passive) && wk->set_up && !wk->heart_frozen && (wake_all || player_near_keeper(wk)))
        {
            wk->state = WKSt_Active;
            W.keeper_last_near[k] = game.play_gameturn;
            SYNCLOG("World: keeper@%d,%d (player %d) wakes up", (int)wk->id.start_cx, (int)wk->id.start_cy, (int)wk->slot);
        } else
        if ((wk->state == WKSt_Active) && world_paged && !wake_all)
        {
            /* no human near for a long time: back to sleep, so its ground can freeze and its slot be reused */
            if ((W.keeper_last_near[k] == 0) || player_near_keeper(wk))
                W.keeper_last_near[k] = game.play_gameturn;
            else if (game.play_gameturn - W.keeper_last_near[k] > WORLD_KEEPER_SLEEP_TURNS)
            {
                wk->state = WKSt_Passive;
                SYNCLOG("World: keeper@%d,%d (player %d) goes back to sleep", (int)wk->id.start_cx, (int)wk->id.start_cy,
                    (int)wk->slot);
            }
        }
        if (((wk->state == WKSt_Passive) || (wk->state == WKSt_Destroyed)) && keeper_slot_unused(wk))
            keeper_release_slot(k);
    }
}

TbBool world_keeper_is_passive(PlayerNumber plyr_idx)
{
    if (!W.active)
        return false;
    for (int k = 0; k < W.keeper_count; k++)
        if ((W.keepers[k].slot == plyr_idx) && (W.keepers[k].state == WKSt_Passive))
            return true;
    return false;
}

uint64_t world_seed(void)
{
    return W.seed;
}

int world_biome_at_engine_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!W.active || (W.gen == NULL))
        return 0;
    return wg_biome_at(W.gen, world_slab_x_of(slb_x), world_slab_y_of(slb_y));
}

void world_slab_to_world(MapSlabCoord slb_x, MapSlabCoord slb_y, int64_t *wx, int64_t *wy)
{
    *wx = world_slab_x_of(slb_x);
    *wy = world_slab_y_of(slb_y);
}

TbBool world_player_heart_loaded(void)
{
    return world_human_heart_loaded(PLAYER0);
}

TbBool world_human_heart_loaded(PlayerNumber plyr_idx)
{
    return W.active && (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && !pmask_has(W.player_heart_frozen, plyr_idx);
}

TbBool world_pays_at_nearest_treasury(PlayerNumber plyr_idx)
{
    if (!W.active)
        return false;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    return !dungeon_invalid(dungeon) && (dungeon->total_money_owned > 0);
}

/******************************************************************************/
/* Portals and the creature limit */

static struct WorldPortal *portal_at(int64_t wx, int64_t wy)
{
    for (int i = 0; i < W.portal_count; i++)
    {
        int64_t dx = W.portals[i].x - wx, dy = W.portals[i].y - wy;
        if ((dx >= -2) && (dx <= 2) && (dy >= -2) && (dy <= 2))
            return &W.portals[i];
    }
    return NULL;
}

static struct WorldPortal *portal_add(int64_t wx, int64_t wy)
{
    if (W.portal_count >= W.portal_cap)
    {
        if (W.portal_cap >= WORLD_MAX_PORTALS)
            return NULL;
        int cap = (W.portal_cap > 0) ? W.portal_cap * 2 : 32;
        struct WorldPortal *grown = (struct WorldPortal *)realloc(W.portals, sizeof(struct WorldPortal) * (size_t)cap);
        if (grown == NULL)
            return NULL;
        W.portals = grown;
        W.portal_cap = cap;
    }
    struct WorldPortal *p = &W.portals[W.portal_count++];
    p->x = wx;
    p->y = wy;
    p->owner = -1;
    p->seen = 0;
    p->last_owner = -1;
    return p;
}

/** The player's creature limit: WORLD_BASE_CREATURES, plus WORLD_CREATURES_PER_PORTAL for each portal they own,
 *  loaded or frozen. */
static void update_creature_limit(void)
{
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!world_player_is_human(p))
            continue;
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        int owned = 0;
        for (int i = 0; i < W.portal_count; i++)
            if (W.portals[i].owner == p)
                owned++;
        int limit = WORLD_BASE_CREATURES + WORLD_CREATURES_PER_PORTAL * owned;
        if (dungeon->max_creatures_attracted != limit)
        {
            if (p == PLAYER0)
                SYNCLOG("World: the player owns %d portals, creature limit %d", owned, limit);
            else
                SYNCLOG("World: player %d owns %d portals, creature limit %d", (int)p, owned, limit);
            dungeon->max_creatures_attracted = limit;
        }
    }
}

/** Who owns each portal in the window now. Portals outside it keep the owner they had when their chunk left. */
static void update_portals(void)
{
    for (int i = 0; i < W.portal_count; i++)
        W.portals[i].seen = 0;
    for (long ri = 1; ri < ROOMS_COUNT; ri++)
    {
        struct Room *room = room_get(ri);
        if (!room_exists(room) || !room_role_matches(room->kind, RoRoF_CrPoolSpawn))
            continue;
        int64_t wx = world_slab_x_of(subtile_slab(room->central_stl_x));
        int64_t wy = world_slab_y_of(subtile_slab(room->central_stl_y));
        struct WorldPortal *p = portal_at(wx, wy);
        const TbBool known = (p != NULL);
        if (p == NULL)
            p = portal_add(wx, wy);
        if (p == NULL)
            continue;
        p->seen = 1;
        const int before = p->owner;
        p->owner = ((room->owner == game.neutral_player_num) || (room->owner >= PLAYERS_COUNT)) ? -1 : (int8_t)room->owner;
        /* a known portal changing hands to a player: captured (the server console's, and the player's record) */
        if (known && (p->owner >= 0) && (p->owner != before) && (p->owner != p->last_owner))
            world_player_portal_captured(p->owner, world_biome_at_subtile(room->central_stl_x, room->central_stl_y));
        if (p->owner >= 0)
            p->last_owner = p->owner;
        /* its biome's creatures come to its owner, for good */
        if (p->owner >= 0)
            world_progress_portal_owned(p->owner, world_biome_at_subtile(room->central_stl_x, room->central_stl_y));
    }
    for (int i = 0; i < W.portal_count; i++)
    {
        struct WorldPortal *p = &W.portals[i];
        int64_t ex = engine_slab_x(p->x), ey = engine_slab_y(p->y);
        /* gone from the loaded map without being frozen (destroyed): the window, or a loaded chunk */
        const TbBool loaded = world_paged ? (wp_page_of_slab((MapSlabCoord)ex, (MapSlabCoord)ey) != NULL)
            : ((ex >= 1) && (ex <= WORLD_WINDOW_SLABS - 2) && (ey >= 1) && (ey <= WORLD_WINDOW_SLABS - 2));
        if (!p->seen && loaded)
            p->owner = -1;
    }
    update_creature_limit();
}

/** Window chunks that must stay loaded: the boxes of awake keepers. */
static TbBool window_chunk_pinned(int lcx, int lcy)
{
    int64_t cx = W.origin_cx + lcx, cy = W.origin_cy + lcy;
    for (int k = 0; k < W.keeper_count; k++)
    {
        const struct WorldKeeper *wk = &W.keepers[k];
        if (wk->state != WKSt_Active)
            continue;
        if ((cx >= floor_div(wk->bbox_x1, WG_CHUNK)) && (cx <= floor_div(wk->bbox_x2, WG_CHUNK)) &&
            (cy >= floor_div(wk->bbox_y1, WG_CHUNK)) && (cy <= floor_div(wk->bbox_y2, WG_CHUNK)))
            return true;
    }
    return false;
}

/******************************************************************************/
/* Shifting */

static void world_log_dig_tasks(const char *when);

/** Before a shift: any task (of any player) on a slab that won't be in the window's chunks afterwards is
 *  removed, because the shift would otherwise clamp it onto the edge. clear_band has normally taken them all. */
static void drop_tasks_leaving_window(MapSlabCoord dslb_x, MapSlabCoord dslb_y)
{
    int dropped = 0;
    for (PlayerNumber p = 0; p < DUNGEONS_COUNT; p++)
    {
        struct Dungeon *dungeon = get_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        for (long i = min((long)dungeon->highest_task_number, (long)MAPTASKS_COUNT) - 1; i >= 0; i--)
        {
            struct MapTask *mtask = &dungeon->task_list[i];
            if (mtask->kind == 0)
                continue;
            MapSlabCoord sx = subtile_slab(stl_num_decode_x(mtask->coords)) - dslb_x;
            MapSlabCoord sy = subtile_slab(stl_num_decode_y(mtask->coords)) - dslb_y;
            if ((sx >= 1) && (sy >= 1) && (sx < game.map_tiles_x - 1) && (sy < game.map_tiles_y - 1))
                continue;
            remove_from_task_list(p, i);
            dropped++;
        }
    }
    if (dropped > 0)
        WARNLOG("World: dropped %d tasks that the shift would have put on the window's edge", dropped);
}

static TbBool shift_window(int dcx, int dcy)
{
    if (world_paged)
        return false;
    if ((dcx == 0) && (dcy == 0))
        return false;
    update_portals();
    const TbClockMSec started = LbTimerClock();
    /* which window chunks leave */
    for (int lcy = 0; lcy < WORLD_WINDOW_CHUNKS; lcy++)
    {
        for (int lcx = 0; lcx < WORLD_WINDOW_CHUNKS; lcx++)
        {
            int nlcx = lcx - dcx, nlcy = lcy - dcy;
            TbBool stays = (nlcx >= 0) && (nlcx < WORLD_WINDOW_CHUNKS) && (nlcy >= 0) && (nlcy < WORLD_WINDOW_CHUNKS);
            if (!stays && window_chunk_pinned(lcx, lcy))
                return false;
        }
    }
    for (int lcy = 0; lcy < WORLD_WINDOW_CHUNKS; lcy++)
    {
        for (int lcx = 0; lcx < WORLD_WINDOW_CHUNKS; lcx++)
        {
            int nlcx = lcx - dcx, nlcy = lcy - dcy;
            TbBool stays = (nlcx >= 0) && (nlcx < WORLD_WINDOW_CHUNKS) && (nlcy >= 0) && (nlcy < WORLD_WINDOW_CHUNKS);
            if (!stays)
                freeze_window_chunk(lcx, lcy);
        }
    }
    /* the leaving band (and the frame next to it) loses rooms, tags, leftover things and columns */
    MapSlabCoord lx1 = 0, lx2 = game.map_tiles_x - 1, ly1 = 0, ly2 = game.map_tiles_y - 1;
    if (dcx > 0) lx2 = window_chunk_slab(min(dcx, WORLD_WINDOW_CHUNKS)) - 1;
    if (dcx < 0) lx1 = window_chunk_slab(max(WORLD_WINDOW_CHUNKS + dcx, 0));
    if (dcy > 0) ly2 = window_chunk_slab(min(dcy, WORLD_WINDOW_CHUNKS)) - 1;
    if (dcy < 0) ly1 = window_chunk_slab(max(WORLD_WINDOW_CHUNKS + dcy, 0));
    if (dcx != 0)
        clear_band(lx1, 0, lx2, game.map_tiles_y - 1);
    if (dcy != 0)
        clear_band(0, ly1, game.map_tiles_x - 1, ly2);
    /* the frame on the far side holds world slabs of the chunks coming in: release it too */
    const TbClockMSec t_frozen = LbTimerClock();
    if (dcx > 0) clear_band(game.map_tiles_x - 1, 0, game.map_tiles_x - 1, game.map_tiles_y - 1);
    if (dcx < 0) clear_band(0, 0, 0, game.map_tiles_y - 1);
    if (dcy > 0) clear_band(0, game.map_tiles_y - 1, game.map_tiles_x - 1, game.map_tiles_y - 1);
    if (dcy < 0) clear_band(0, 0, game.map_tiles_x - 1, 0);

    const TbClockMSec t_cleared = LbTimerClock();
    drop_tasks_leaving_window((MapSlabCoord)(dcx * WORLD_CHUNK_SLABS), (MapSlabCoord)(dcy * WORLD_CHUNK_SLABS));
    world_shift_state((MapSlabCoord)(dcx * WORLD_CHUNK_SLABS), (MapSlabCoord)(dcy * WORLD_CHUNK_SLABS));
    const TbClockMSec t_shifted = LbTimerClock();
    W.origin_cx += dcx;
    W.origin_cy += dcy;

    /* fill the chunks that came in */
    struct FillChunk list[WORLD_WINDOW_CHUNKS * WORLD_WINDOW_CHUNKS];
    int count = 0;
    for (int lcy = 0; lcy < WORLD_WINDOW_CHUNKS; lcy++)
    {
        for (int lcx = 0; lcx < WORLD_WINDOW_CHUNKS; lcx++)
        {
            int olcx = lcx + dcx, olcy = lcy + dcy;
            TbBool was_in = (olcx >= 0) && (olcx < WORLD_WINDOW_CHUNKS) && (olcy >= 0) && (olcy < WORLD_WINDOW_CHUNKS);
            if (was_in)
                continue;
            list[count].lcx = lcx;
            list[count].lcy = lcy;
            list[count].cd = chunk_data_for(W.origin_cx + lcx, W.origin_cy + lcy);
            if (list[count].cd != NULL)
                count++;
        }
    }
    const TbClockMSec t_loaded = LbTimerClock();
    fill_chunks(list, count);
    for (int c = 0; c < count; c++)
        free(list[c].cd);
    place_frame();
    const TbClockMSec t_filled = LbTimerClock();
    world_log_dig_tasks("after a shift");
    bind_hearts();
    setup_keepers();
    calculate_dungeon_area_scores();
    /* diggers' task stacks and the computers' gold lookups are rebuilt from the new map */
    for (PlayerNumber p = 0; p < DUNGEONS_COUNT; p++)
    {
        struct Dungeon *dungeon = get_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        dungeon->digger_stack_length = 0;
        dungeon->digger_stack_update_turn = 0;
    }
    {
        MapSubtlCoord bsx = game.map_subtiles_x, bsy = game.map_subtiles_y, bex = 0, bey = 0;
        for (int c = 0; c < count; c++)
        {
            MapSubtlCoord sx = window_chunk_slab(list[c].lcx) * STL_PER_SLB, sy = window_chunk_slab(list[c].lcy) * STL_PER_SLB;
            bsx = min(bsx, sx); bsy = min(bsy, sy);
            bex = max(bex, sx + CHUNK_STL - 1); bey = max(bey, sy + CHUNK_STL - 1);
        }
        if (count == 0) { bsx = bsy = bex = bey = 0; }
        world_shift_rebuild(bsx, bsy, bex, bey);
    }
    check_map_for_gold();
    const TbClockMSec t_rebuilt = LbTimerClock();
    SYNCLOG("World: shift timing: freeze %ld, clear %ld, move %ld, load/generate %ld, fill %ld, rebuild %ld ms",
        (long)(t_frozen - started), (long)(t_cleared - t_frozen), (long)(t_shifted - t_cleared),
        (long)(t_loaded - t_shifted), (long)(t_filled - t_loaded), (long)(t_rebuilt - t_filled));
    W.shifts++;
    W.last_shift_turn = game.play_gameturn;
    SYNCLOG("World: window moved by (%d,%d) chunks to origin (%d,%d) in %ld ms, %d chunks filled, %lu stored, %lu remembered",
        dcx, dcy, (int)W.origin_cx, (int)W.origin_cy, (long)(LbTimerClock() - started), count,
        (unsigned long)W.store.count, (unsigned long)W.memory.count);
    return true;
}

/** Where a user is looking: the possessed creature, or the dungeon camera. */
static TbBool user_view_slab(NetUserId user, MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    struct UserState *ustate = get_user_state(user);
    if (ustate == NULL)
        return false;
    if ((get_user_view_type(user) == PVT_CreatureContrl) || (get_user_view_type(user) == PVT_CreaturePasngr))
    {
        struct Thing *thing = thing_get(ustate->controlled_thing_idx);
        if (thing_exists(thing))
        {
            *slb_x = subtile_slab(thing->mappos.x.stl.num);
            *slb_y = subtile_slab(thing->mappos.y.stl.num);
            return true;
        }
    }
    *slb_x = coord_slab(ustate->dungeon_camera.x);
    *slb_y = coord_slab(ustate->dungeon_camera.y);
    return true;
}

/** Where the local player is looking (the window follows it). */
static TbBool view_slab(MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    return user_view_slab(get_my_user(), slb_x, slb_y);
}

/******************************************************************************/
/* The chunk manager (paged mode) */

#define CM_PERIOD 4              /* turns between passes */
#define CM_HEART_RADIUS 2        /* chunks round the heart (5x5): the start area and its portal stay reachable */
#define CM_CREATURE_RADIUS 1     /* round each of the player's creatures and each raid hero (3x3) */
#define CM_TERRITORY_RADIUS 1    /* round each chunk holding a slab the player owns: the ground ahead can be seen and dug */
#define CM_UNLOAD_TURNS 200      /* a chunk nobody has wanted for this long is frozen */
#define CM_LOAD_BUDGET 3         /* chunks loaded per pass */
#define CM_UNLOAD_BUDGET 2       /* chunks frozen per pass, and only in passes that loaded none (unloading can wait) */
#define CM_MARGIN_STL 6          /* navigation, ceilings and the minimap are refreshed this far round a chunk */
#define CM_LIGHT_MARGIN_STL 16   /* static light this far (the reach of the longest static light) */

static struct ChunkTable cm_wanted;  /* the world chunks wanted in this pass (keys only) */

struct CmCandidate {
    int64_t cx, cy, d;
};

static void table_clear(struct ChunkTable *t)
{
    for (uint32_t i = 0; i < t->cap; i++)
    {
        free(t->slots[i].data);
        memset(&t->slots[i], 0, sizeof(t->slots[i]));
    }
    t->count = 0;
}

static int64_t page_world_cx(const struct WorldPage *pg) { return (int64_t)pg->cx - WP_ORIGIN_SLB / WORLD_CHUNK_SLABS; }
static int64_t page_world_cy(const struct WorldPage *pg) { return (int64_t)pg->cy - WP_ORIGIN_SLB / WORLD_CHUNK_SLABS; }
static int32_t engine_chunk_of(int64_t wc) { return (int32_t)(wc + WP_ORIGIN_SLB / WORLD_CHUNK_SLABS); }

static void cm_want(int64_t wcx, int64_t wcy, int r)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            table_insert(&cm_wanted, (int32_t)(wcx + dx), (int32_t)(wcy + dy));
}

static void cm_want_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y, int r)
{
    cm_want(floor_div(world_slab_x_of(subtile_slab(stl_x)), WORLD_CHUNK_SLABS),
            floor_div(world_slab_y_of(subtile_slab(stl_y)), WORLD_CHUNK_SLABS), r);
}

/** The chunks that must be loaded now. Only game state is read, so every machine and replay agrees. The camera
 *  isn't one of them: like players in Minecraft, the player's creatures load (and generate) the world. */
static void cm_collect_wanted(int64_t *home_cx, int64_t *home_cy)
{
    table_clear(&cm_wanted);
    *home_cx = floor_div(W.home_x, WORLD_CHUNK_SLABS);
    *home_cy = floor_div(W.home_y, WORLD_CHUNK_SLABS);
    cm_want(*home_cx, *home_cy, CM_HEART_RADIUS);
    const PlayerBitFlags humans = world_human_mask();
    /* the other human players' hearts (network games) */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if ((p == PLAYER0) || !owner_in_mask(p, humans))
            continue;
        struct Thing *heartng = find_players_dungeon_heart(p);
        if (thing_exists(heartng))
            cm_want_subtile(heartng->mappos.x.stl.num, heartng->mappos.y.stl.num, CM_HEART_RADIUS);
    }
    /* the human players' creatures and imps, and raid heroes */
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || thing_is_picked_up(thing))
            continue;
        if (owner_in_mask(thing->owner, humans) || world_raid_hero(thing))
            cm_want_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num, CM_CREATURE_RADIUS);
    }
    /* awake keepers keep their whole box; one of the player's creatures within a keeper's threat radius loads its
       box, so that it is set up and wakes (update_keepers) */
    for (int k = 0; k < W.keeper_count; k++)
    {
        const struct WorldKeeper *wk = &W.keepers[k];
        if (wk->state == WKSt_Destroyed)
            continue;
        if ((wk->state != WKSt_Active) && !player_creature_near_keeper(wk))
            continue;
        for (int64_t cy = floor_div(wk->bbox_y1, WG_CHUNK); cy <= floor_div(wk->bbox_y2, WG_CHUNK); cy++)
            for (int64_t cx = floor_div(wk->bbox_x1, WG_CHUNK); cx <= floor_div(wk->bbox_x2, WG_CHUNK); cx++)
                table_insert(&cm_wanted, (int32_t)cx, (int32_t)cy);
    }
    /* the player's territory (any loaded chunk with a slab they own) and the chunks next to it, so that the ground
       just past the claimed area can always be seen and marked for digging, wherever the imps are */
    for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
    {
        for (int i = 0; i < WP_CHUNK_SLAB_CELLS; i++)
        {
            if (owner_in_mask(slabmap_owner(&pg->slab[i]), humans))
            {
                cm_want(page_world_cx(pg), page_world_cy(pg), CM_TERRITORY_RADIUS);
                break;
            }
        }
    }
    /* the human players' dig orders: their chunks stay until they are dug */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!owner_in_mask(p, humans))
            continue;
        struct Dungeon *dungeon = get_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        for (long t = min((long)dungeon->highest_task_number, (long)MAPTASKS_COUNT) - 1; t >= 0; t--)
        {
            const struct MapTask *mtask = &dungeon->task_list[t];
            if (mtask->kind != 0)
                cm_want_subtile(stl_num_decode_x(mtask->coords), stl_num_decode_y(mtask->coords), 0);
        }
    }
}

/** Navigation, ceilings, static light and the minimap round a chunk that was loaded or freed. */
static void cm_refresh_chunk(int32_t ecx, int32_t ecy)
{
    const MapSubtlCoord x1 = (MapSubtlCoord)ecx * CHUNK_STL, y1 = (MapSubtlCoord)ecy * CHUNK_STL;
    const MapSubtlCoord x2 = x1 + CHUNK_STL - 1, y2 = y1 + CHUNK_STL - 1;
    WORLD_PROF(WPS_RfNav, nav_refresh_area(x1 - CM_MARGIN_STL, y1 - CM_MARGIN_STL, x2 + CM_MARGIN_STL, y2 + CM_MARGIN_STL));
    WORLD_PROF(WPS_RfCeil, ceiling_partially_recompute_heights(x1 - CM_MARGIN_STL, y1 - CM_MARGIN_STL, x2 + CM_MARGIN_STL, y2 + CM_MARGIN_STL));
    WORLD_PROF(WPS_RfLight, light_stat_refresh_area(x1 - CM_LIGHT_MARGIN_STL, y1 - CM_LIGHT_MARGIN_STL, x2 + CM_LIGHT_MARGIN_STL, y2 + CM_LIGHT_MARGIN_STL));
    WORLD_PROF(WPS_RfPanel, panel_map_update(x1 - CM_MARGIN_STL, y1 - CM_MARGIN_STL, CHUNK_STL + 2 * CM_MARGIN_STL, CHUNK_STL + 2 * CM_MARGIN_STL));
}

/** After chunks came or went: what the window's shift rebuilt for the whole map, minus what cm_refresh_chunk did. */
static void cm_after_changes(void)
{
    bind_hearts();
    setup_keepers();
    WORLD_PROF(WPS_CmScores, calculate_dungeon_area_scores());
    for (PlayerNumber p = 0; p < DUNGEONS_COUNT; p++)
    {
        struct Dungeon *dungeon = get_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        dungeon->digger_stack_length = 0;
        dungeon->digger_stack_update_turn = 0;
    }
    WORLD_PROF(WPS_CmGold, check_map_for_gold());
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl))
            cctrl->arid.may_need_reroute = 1;
    }
    game.map_changed_for_navigation = 1;
    me_pointed_at = INVALID_MAP_BLOCK; /* it may point into a freed page; the next frame finds it again */
}

static int cm_candidate_cmp(const void *a, const void *b)
{
    const struct CmCandidate *ca = (const struct CmCandidate *)a, *cb = (const struct CmCandidate *)b;
    if (ca->d != cb->d) return (ca->d < cb->d) ? -1 : 1;
    if (ca->cy != cb->cy) return (ca->cy < cb->cy) ? -1 : 1;
    if (ca->cx != cb->cx) return (ca->cx < cb->cx) ? -1 : 1;
    return 0;
}

/** Loads one world chunk into a new page. */
static TbBool cm_load_chunk(int64_t cx, int64_t cy, uint32_t now)
{
    const int32_t ecx = engine_chunk_of(cx), ecy = engine_chunk_of(cy);
    struct WorldPage *pg = wp_create(ecx, ecy);
    if (pg == NULL)
        return false;
    pg->wanted_turn = now;
    struct FillChunk fc;
    fc.lcx = (int)(cx - W.origin_cx);
    fc.lcy = (int)(cy - W.origin_cy);
    WORLD_PROF(WPS_LdData, fc.cd = chunk_data_for((int32_t)cx, (int32_t)cy));
    if (fc.cd == NULL)
    {
        wp_free(ecx, ecy);
        return false;
    }
    WORLD_PROF(WPS_LdFill, fill_chunks(&fc, 1));
    free(fc.cd);
    cm_refresh_chunk(ecx, ecy);
    if (W.lair_wait_count > 0)
        lair_waits_process();
    return true;
}

/** Freezes one loaded chunk into the store and frees its page. */
static void cm_unload_chunk(int64_t cx, int64_t cy)
{
    const int32_t ecx = engine_chunk_of(cx), ecy = engine_chunk_of(cy);
    TbBool stored;
    WORLD_PROF(WPS_FzRecord, stored = freeze_window_chunk((int)(cx - W.origin_cx), (int)(cy - W.origin_cy)));
    const MapSlabCoord sx0 = (MapSlabCoord)ecx * WORLD_CHUNK_SLABS, sy0 = (MapSlabCoord)ecy * WORLD_CHUNK_SLABS;
    WORLD_PROF(WPS_FzClear, clear_band(sx0, sy0, sx0 + WORLD_CHUNK_SLABS - 1, sy0 + WORLD_CHUNK_SLABS - 1));
    if (!stored)
    {
        /* out of memory: the chunk goes back as it was (its record data is still in chunk_buf) and stays loaded */
        struct FillChunk fc;
        fc.lcx = (int)(cx - W.origin_cx);
        fc.lcy = (int)(cy - W.origin_cy);
        fc.cd = &chunk_buf;
        fill_chunks(&fc, 1);
        struct WorldPage *pg = wp_page(ecx, ecy);
        if (pg != NULL)
            pg->wanted_turn = (uint32_t)max((long)game.play_gameturn, 1L);
        cm_refresh_chunk(ecx, ecy);
        return;
    }
    wp_free(ecx, ecy);
    cm_refresh_chunk(ecx, ecy);
}

static int count_creatures_with_lairs(void)
{
    int n = 0;
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl) && (cctrl->lairtng_idx > 0))
            n++;
    }
    return n;
}

/** Test hook KFX_WORLD_LAIRTEST="<turn>[,1]": at that turn (or the first one after it) every chunk holding a lair or a
 *  creature with one (",1": only the lairs, so creatures elsewhere wait for them) but no dungeon heart freezes; the chunk manager loads the wanted ones again. Creatures with a lair are counted then and
 *  200 turns later. Only read in paged world levels. */
static void world_lair_test(void)
{
    static long test_turn = -2;
    static int totems_only = 0;
    if (test_turn == -2)
    {
        const char *v = getenv("KFX_WORLD_LAIRTEST");
        test_turn = (v != NULL) ? atol(v) : -1;
        totems_only = (v != NULL) && (strchr(v, ',') != NULL) && (atoi(strchr(v, ',') + 1) == 1);
    }
    static GameTurn fired_at = 0;
    if (test_turn < 0)
        return;
    if ((fired_at > 0) && (fired_at + 200 == game.play_gameturn))
        SYNCLOG("World lairtest: %d creatures have a lair 200 turns later, %d still waiting", count_creatures_with_lairs(),
            W.lair_wait_count);
    /* once, at the first turn from test_turn on (after a loaded save is bound) */
    if ((fired_at > 0) || (game.play_gameturn < (GameTurn)test_turn) || W.rebind_hearts || !world_paged)
        return;
    fired_at = game.play_gameturn;
    const int before = count_creatures_with_lairs();
    int64_t list[2 * 64];
    int n = 0;
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || thing_is_picked_up(thing))
            continue;
        TbBool pick = (thing->class_id == TCls_Object) && thing_is_lair_totem(thing);
        if ((thing->class_id == TCls_Creature) && !totems_only)
        {
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            pick = !creature_control_invalid(cctrl) && (cctrl->lairtng_idx > 0);
        }
        if (!pick)
            continue;
        const int64_t cx = floor_div(world_slab_x_of(subtile_slab(thing->mappos.x.stl.num)), WORLD_CHUNK_SLABS);
        const int64_t cy = floor_div(world_slab_y_of(subtile_slab(thing->mappos.y.stl.num)), WORLD_CHUNK_SLABS);
        TbBool have = false;
        for (int k = 0; k < n; k++)
            have = have || ((list[2 * k] == cx) && (list[2 * k + 1] == cy));
        if (!have && (n < 64))
        {
            list[2 * n] = cx;
            list[2 * n + 1] = cy;
            n++;
        }
    }
    /* not the chunks of hearts */
    int frozen = 0;
    for (int k = 0; k < n; k++)
    {
        TbBool heart = false;
        for (long i = 1; (i < THINGS_COUNT) && !heart; i++)
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing) || (thing->class_id != TCls_Object) || !thing_is_dungeon_heart(thing))
                continue;
            heart = (floor_div(world_slab_x_of(subtile_slab(thing->mappos.x.stl.num)), WORLD_CHUNK_SLABS) == list[2 * k]) &&
                    (floor_div(world_slab_y_of(subtile_slab(thing->mappos.y.stl.num)), WORLD_CHUNK_SLABS) == list[2 * k + 1]);
        }
        if (heart)
            continue;
        cm_unload_chunk(list[2 * k], list[2 * k + 1]);
        frozen++;
    }
    if (frozen > 0)
        cm_after_changes();
    SYNCLOG("World lairtest: %d creatures had a lair; %d of %d chunks with lairs frozen; %d now, %d waiting",
        before, frozen, n, count_creatures_with_lairs(), W.lair_wait_count);
}

static void chunk_manager_turn(void)
{
    world_lair_test();
    if ((game.play_gameturn % CM_PERIOD) != 0)
        return;
    const TbClockMSec started = LbTimerClock();
    const uint32_t now = (uint32_t)max((long)game.play_gameturn, 1L);
    int64_t hcx, hcy;
    WORLD_PROF(WPS_CmCollect, cm_collect_wanted(&hcx, &hcy));
    /* wanted chunks: loaded ones are kept, the others are candidates, nearest the heart first */
    struct CmCandidate *cand = (struct CmCandidate *)malloc(sizeof(struct CmCandidate) * (cm_wanted.count + 1));
    int ncand = 0;
    for (uint32_t i = 0; (cand != NULL) && (i < cm_wanted.cap); i++)
    {
        const struct ChunkSlot *s = &cm_wanted.slots[i];
        if (!s->used)
            continue;
        struct WorldPage *pg = wp_page(engine_chunk_of(s->cx), engine_chunk_of(s->cy));
        if (pg != NULL)
        {
            pg->wanted_turn = now;
            continue;
        }
        cand[ncand].cx = s->cx;
        cand[ncand].cy = s->cy;
        cand[ncand].d = (s->cx - hcx) * (s->cx - hcx) + (s->cy - hcy) * (s->cy - hcy);
        ncand++;
    }
    int loaded = 0;
    /* room for another chunk: up to WORLD_MAX_THINGS_PER_CHUNK things, and about 100 triangles and 50 points of the
       navigation mesh (both fixed pools). Short of room, nothing loads (creation would fail silently and those
       things would be gone at the next freeze); freezing goes on and makes room. */
    const TbBool room_for_more = (game.synced_free_things_count > 2 * WORLD_MAX_THINGS_PER_CHUNK) &&
        (count_Triangles < TRIANLGLES_COUNT - 4000) && (count_Points < POINTS_COUNT - 2000);
    if ((ncand > 0) && !room_for_more)
    {
        static GameTurn last_warned = 0;
        if ((last_warned == 0) || (game.play_gameturn >= last_warned + 1000) || (game.play_gameturn < last_warned))
        {
            WARNLOG("World: no room to load more chunks: %d free things, %ld/%d triangles, %ld/%d points",
                (int)game.synced_free_things_count, (long)count_Triangles, TRIANLGLES_COUNT, (long)count_Points,
                POINTS_COUNT);
            last_warned = game.play_gameturn;
        }
    }
    if ((ncand > 0) && room_for_more)
    {
        qsort(cand, (size_t)ncand, sizeof(struct CmCandidate), cm_candidate_cmp);
        for (int i = 0; (i < ncand) && (loaded < CM_LOAD_BUDGET); i++)
        {
            TbBool ok;
            WORLD_PROF(WPS_CmLoad, ok = cm_load_chunk(cand[i].cx, cand[i].cy, now));
            if (!ok)
                break;
            loaded++;
        }
    }
    free(cand);
    /* chunks nobody has wanted for CM_UNLOAD_TURNS: freeze them (pages from a save or the level start count as
       wanted now) */
    struct CmCandidate gone[CM_UNLOAD_BUDGET];
    int ngone = 0;
    for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
    {
        if (pg->wanted_turn == 0)
            pg->wanted_turn = now;
        if ((loaded == 0) && (ngone < CM_UNLOAD_BUDGET) && (now - pg->wanted_turn >= CM_UNLOAD_TURNS))
        {
            gone[ngone].cx = page_world_cx(pg);
            gone[ngone].cy = page_world_cy(pg);
            ngone++;
        }
    }
    if (ngone > 0)
        update_portals(); /* portal owners are remembered when their chunk freezes */
    for (int i = 0; i < ngone; i++)
        WORLD_PROF(WPS_CmFreeze, cm_unload_chunk(gone[i].cx, gone[i].cy));
    if (loaded + ngone == 0)
        return;
    WORLD_PROF(WPS_CmAfter, cm_after_changes());
    long objects = -1, effgens = -1; /* counted with the profiler on (a scan of every thing slot) */
    if (world_prof_now() != 0)
    {
        objects = effgens = 0;
        for (long i = 1; i < THINGS_COUNT; i++)
        {
            const struct Thing *thing = thing_get(i);
            if (thing_exists(thing) && (thing->class_id == TCls_Object)) objects++;
            if (thing_exists(thing) && (thing->class_id == TCls_EffectGen)) effgens++;
        }
    }
    SYNCLOG("World: chunks: %d loaded, %d frozen (%d wanted, %d to load), %d in memory, %lu stored, %ld ms; mesh %ld/%d triangles, %ld/%d points; %ld objects, %ld effect generators",
        loaded, ngone, (int)cm_wanted.count, ncand - loaded, wp_count(), (unsigned long)W.store.count,
        (long)(LbTimerClock() - started), (long)count_Triangles, TRIANLGLES_COUNT, (long)count_Points, POINTS_COUNT,
        objects, effgens);
}

int world_load_chunks_round(MapSlabCoord slb_x, MapSlabCoord slb_y, int r)
{
    if (!W.active || !world_paged)
        return 0;
    const int64_t cx = floor_div(world_slab_x_of(slb_x), WORLD_CHUNK_SLABS);
    const int64_t cy = floor_div(world_slab_y_of(slb_y), WORLD_CHUNK_SLABS);
    const uint32_t now = (uint32_t)max((long)game.play_gameturn, 1L);
    int loaded = 0;
    for (int dy = -r; dy <= r; dy++)
    {
        for (int dx = -r; dx <= r; dx++)
        {
            if (wp_page(engine_chunk_of(cx + dx), engine_chunk_of(cy + dy)) != NULL)
                continue;
            if ((game.synced_free_things_count <= 2 * WORLD_MAX_THINGS_PER_CHUNK) ||
                (count_Triangles >= TRIANLGLES_COUNT - 4000) || (count_Points >= POINTS_COUNT - 2000))
            {
                WARNLOG("World: no room for more chunks (things or navigation): %d left unloaded round (%d,%d)",
                    (2 * r + 1) * (2 * r + 1) - loaded, (int)cx, (int)cy);
                dy = r + 1;
                break;
            }
            if (cm_load_chunk(cx + dx, cy + dy, now))
                loaded++;
        }
    }
    if (loaded > 0)
        cm_after_changes();
    return loaded;
}

/******************************************************************************/
/* Reading slabs that may not be loaded (the day raids' landing search) */

static struct WgChunk peek_gen;

/** Is an engine slab on the live map: a loaded page, or inside the window's frame? */
static TbBool slab_is_live(int64_t x, int64_t y)
{
    if (world_paged)
        return wp_page_of_slab((MapSlabCoord)x, (MapSlabCoord)y) != NULL;
    return (x >= 1) && (y >= 1) && (x <= game.map_tiles_x - 2) && (y <= game.map_tiles_y - 2);
}

/** A chunk that isn't loaded, as world_read_slab_box sees it: from the cache, else the chunk store, else the
 *  generator (and then cached). *cd is a scratch record, allocated on first use (the caller frees it). */
static struct PeekEntry *peek_get(int64_t wcx, int64_t wcy, struct WorldChunkData **cd, int *stored, int *generated)
{
    static struct PeekEntry peek_scratch;
    if (peek_cache == NULL)
        peek_cache = (struct PeekEntry *)calloc(PEEK_CACHE_SIZE, sizeof(struct PeekEntry));
    struct PeekEntry *pe = (peek_cache != NULL) ? peek_cache_slot(wcx, wcy) : &peek_scratch;
    if (pe->valid && (pe->cx == wcx) && (pe->cy == wcy))
        return pe;
    pe->valid = 0;
    pe->cx = (int32_t)wcx;
    pe->cy = (int32_t)wcy;
    if (*cd == NULL)
        *cd = (struct WorldChunkData *)malloc(sizeof(struct WorldChunkData));
    if ((*cd != NULL) && load_chunk_record((int32_t)wcx, (int32_t)wcy, *cd))
    {
        for (int k = 0; k < WG_CHUNK_CELLS; k++)
        {
            pe->kind[k] = (*cd)->kind[k];
            pe->owner[k] = (*cd)->owner[k];
        }
        pe->valid = 1;
        (*stored)++;
    } else if (wg_generate_chunk(W.gen, (int32_t)wcx, (int32_t)wcy, &peek_gen) == 0)
    {
        for (int k = 0; k < WG_CHUNK_CELLS; k++)
        {
            const uint8_t s = peek_gen.slabs[k];
            pe->kind[k] = (uint8_t)((s < WgS_COUNT) ? wg_kinds[s] : SlbT_ROCK);
            pe->owner[k] = (peek_gen.owners[k] == WgO_Player) ? WOwn_Player : WOwn_Neutral;
        }
        pe->valid = 1;
        (*generated)++;
    }
    return pe;
}

int world_peek_warm(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2, int budget)
{
    if (!W.active || (W.gen == NULL) || !world_paged || (x2 < x1) || (y2 < y1) || (budget <= 0))
        return 0;
    const int64_t wcx1 = floor_div(world_slab_x_of(x1), WG_CHUNK), wcx2 = floor_div(world_slab_x_of(x2), WG_CHUNK);
    const int64_t wcy1 = floor_div(world_slab_y_of(y1), WG_CHUNK), wcy2 = floor_div(world_slab_y_of(y2), WG_CHUNK);
    struct WorldChunkData *cd = NULL;
    int stored = 0, generated = 0, done = 0;
    for (int64_t wcy = wcy1; (wcy <= wcy2) && (done < budget); wcy++)
    {
        for (int64_t wcx = wcx1; (wcx <= wcx2) && (done < budget); wcx++)
        {
            if (wp_page(engine_chunk_of(wcx), engine_chunk_of(wcy)) != NULL)
                continue; /* loaded: read live */
            if ((peek_cache != NULL) && peek_cache_slot(wcx, wcy)->valid && (peek_cache_slot(wcx, wcy)->cx == wcx) &&
                (peek_cache_slot(wcx, wcy)->cy == wcy))
                continue;
            peek_get(wcx, wcy, &cd, &stored, &generated);
            done++;
        }
    }
    free(cd);
    return done;
}

TbBool world_read_slab_box(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2, uint8_t *kinds,
    uint8_t *player_owned, PlayerNumber plyr_idx)
{
    const uint8_t wown = owner_to_world(plyr_idx);
    if (!W.active || (W.gen == NULL) || (x2 < x1) || (y2 < y1))
        return false;
    const int64_t w = (int64_t)x2 - x1 + 1;
    const int64_t wcx1 = floor_div(world_slab_x_of(x1), WG_CHUNK), wcx2 = floor_div(world_slab_x_of(x2), WG_CHUNK);
    const int64_t wcy1 = floor_div(world_slab_y_of(y1), WG_CHUNK), wcy2 = floor_div(world_slab_y_of(y2), WG_CHUNK);
    struct WorldChunkData *cd = NULL;
    int stored = 0, generated = 0;
    for (int64_t wcy = wcy1; wcy <= wcy2; wcy++)
    {
        for (int64_t wcx = wcx1; wcx <= wcx2; wcx++)
        {
            const int64_t ex0 = engine_slab_x(wcx * WG_CHUNK), ey0 = engine_slab_y(wcy * WG_CHUNK);
            int src = 0; /* 0: not read yet, 1: read (pe: cache entry), -1: unreadable */
            struct PeekEntry *pe = NULL;
            for (int ly = 0; ly < WG_CHUNK; ly++)
            {
                const int64_t y = ey0 + ly;
                if ((y < y1) || (y > y2))
                    continue;
                for (int lx = 0; lx < WG_CHUNK; lx++)
                {
                    const int64_t x = ex0 + lx;
                    if ((x < x1) || (x > x2))
                        continue;
                    const size_t o = (size_t)((y - y1) * w + (x - x1));
                    if (slab_is_live(x, y))
                    {
                        struct SlabMap *slb = get_slabmap_block((MapSlabCoord)x, (MapSlabCoord)y);
                        kinds[o] = (uint8_t)slb->kind;
                        player_owned[o] = (slabmap_owner(slb) == plyr_idx);
                        continue;
                    }
                    if (src == 0)
                    {
                        pe = peek_get(wcx, wcy, &cd, &stored, &generated);
                        src = pe->valid ? 1 : -1;
                    }
                    const int i = ly * WG_CHUNK + lx;
                    if (src == 1)
                    {
                        kinds[o] = pe->kind[i];
                        player_owned[o] = (pe->owner[i] == wown);
                    } else
                    {
                        kinds[o] = SlbT_ROCK;
                        player_owned[o] = 0;
                    }
                }
            }
        }
    }
    free(cd);
    if (stored + generated > 0)
        SYNCLOG("World: read %ldx%ld slabs, %d chunks from the store and %d from the generator", (long)w,
            (long)((int64_t)y2 - y1 + 1), stored, generated);
    return true;
}

/******************************************************************************/
/* Lifecycle */

TbBool world_mode_active(void)
{
    return W.active;
}

/******************************************************************************/
/* Development timer (KFX_WORLD_PROFILE=1) */

static const char *const prof_names[WPS_COUNT] = {
    "turn", "packets", "things", "rooms", "dungeons", "research", "events", "script", "computers", "players",
    "lighting", "world", "chunks", "frame", "cm_collect", "cm_load", "cm_freeze", "cm_after", "cm_scores", "cm_gold",
    "fz_record", "fz_clear", "ld_data", "ld_fill", "rf_nav", "rf_ceil", "rf_light", "rf_panel",
    "pk_checksum", "pk_write", "pk_users",
    "t_creatures", "t_traps", "t_shots", "t_objects", "t_effects", "t_other", "path"};
static int64_t prof_sum[WPS_COUNT], prof_max[WPS_COUNT];
static long prof_count[WPS_COUNT];
static GameTurn prof_last_report;

int64_t world_prof_now(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = (getenv("KFX_WORLD_PROFILE") != NULL) ? 1 : 0;
    if (!enabled)
        return 0;
    const int64_t t = get_time_tick_ns();
    return (t != 0) ? t : 1;
}

static int64_t prof_turn_sum[WPS_COUNT]; /* world_prof_sum: this turn's total so far */
static TbBool prof_per_turn[WPS_COUNT];

void world_prof_sum(int section, int64_t t0)
{
    if ((t0 == 0) || (section < 0) || (section >= WPS_COUNT))
        return;
    prof_turn_sum[section] += get_time_tick_ns() - t0;
    prof_per_turn[section] = true;
}

void world_prof_add(int section, int64_t t0)
{
    if ((section == WPS_Turn) && (t0 != 0))
    {
        /* the per-turn sums become one entry each */
        for (int i = 0; i < WPS_COUNT; i++)
        {
            if (!prof_per_turn[i])
                continue;
            prof_sum[i] += prof_turn_sum[i];
            prof_count[i]++;
            if (prof_turn_sum[i] > prof_max[i])
                prof_max[i] = prof_turn_sum[i];
            prof_turn_sum[i] = 0;
        }
    }
    if ((t0 == 0) || (section < 0) || (section >= WPS_COUNT))
        return;
    const int64_t d = get_time_tick_ns() - t0;
    prof_sum[section] += d;
    prof_count[section]++;
    if (d > prof_max[section])
        prof_max[section] = d;
    if ((section == WPS_Turn) && (game.play_gameturn >= prof_last_report + 200))
    {
        char line[2048];
        int n = 0;
        for (int i = 0; (i < WPS_COUNT) && (n < (int)sizeof(line) - 48); i++)
        {
            if (prof_count[i] == 0)
                continue;
            n += snprintf(line + n, sizeof(line) - n, " %s %.2f/%.1f", prof_names[i],
                (double)prof_sum[i] / prof_count[i] / 1e6, (double)prof_max[i] / 1e6);
        }
        SYNCLOG("World profile (ms, average/max over %ld turns, %d chunks loaded):%s",
            prof_count[WPS_Turn], world_paged ? wp_count() : 0, line);
        /* columns: how many are in use and the most shared one (a column's use counter is 16 bits) */
        long used = 0, negative = 0, negative_inactive = 0, max_use = 0, max_idx = 0, max_active = 0;
        for (long i = 1; i < COLUMNS_COUNT; i++)
        {
            const struct Column *col = &game.columns_data[i];
            if ((col->use != 0) || (col->bitfields & CLF_ACTIVE))
                used++;
            if (col->use < 0)
            {
                negative++;
                if ((col->bitfields & CLF_ACTIVE) == 0)
                    negative_inactive++;
            }
            if (col->use > max_use)
            {
                max_use = col->use;
                max_idx = i;
                max_active = (col->bitfields & CLF_ACTIVE) ? 1 : 0;
            }
        }
        SYNCLOG("World profile: %ld columns in use, most used %ld (%ld uses, %s), %ld with a negative count (%ld not active)",
            used, max_idx, max_use, max_active ? "active" : "not active", negative, negative_inactive);
        long creatures = 0, things = 0;
        long per_owner[PLAYERS_MAX] = {0};
        for (long i = 1; i < THINGS_COUNT; i++)
        {
            const struct Thing *thing = thing_get(i);
            if (!thing_index_synced(i) || !thing_exists(thing))
                continue;
            things++;
            if (thing->class_id == TCls_Creature)
            {
                creatures++;
                if (thing->owner < PLAYERS_COUNT)
                    per_owner[thing->owner]++;
            }
        }
        SYNCLOG("World profile: %ld synced things, %ld creatures (by player: %ld %ld %ld %ld %ld %ld %ld %ld %ld)", things,
            creatures, per_owner[0], per_owner[1], per_owner[2], per_owner[3], per_owner[4], per_owner[5], per_owner[6],
            per_owner[7], per_owner[8]);
        memset(prof_sum, 0, sizeof(prof_sum));
        memset(prof_max, 0, sizeof(prof_max));
        memset(prof_count, 0, sizeof(prof_count));
        prof_last_report = game.play_gameturn;
    }
}

TbBool world_is_chunked(void)
{
    return W.active && world_paged;
}

static char world_next_seed[64];

void world_set_next_seed(const char *seed_text)
{
    snprintf(world_next_seed, sizeof(world_next_seed), "%s", (seed_text != NULL) ? seed_text : "");
}

const char *world_next_seed_text(void)
{
    return world_next_seed;
}

const char *world_seed_text(void)
{
    return W.seed_text;
}

void world_after_turn(void)
{
    if (W.active)
        world_daynight_after_turn();
}

TbBool world_request(const char *seed_text)
{
    uint64_t seed;
    char next_seed[sizeof(world_next_seed)];
    if (world_next_seed[0] != '\0')
    {
        snprintf(next_seed, sizeof(next_seed), "%s", world_next_seed);
        world_next_seed[0] = '\0';
        SYNCLOG("World: seed %s instead of the script's %s", next_seed, seed_text);
        seed_text = next_seed;
    }
    if (wg_parse_seed(seed_text, &seed) != 0)
    {
        ERRORLOG("ENDLESS_WORLD: bad seed \"%s\"", seed_text);
        return false;
    }
    world_level_end();
    W.pending = true;
    W.seed = seed;
    snprintf(W.seed_text, sizeof(W.seed_text), "%s", seed_text);
    /* players 10 and up for keepers and seats: every slot (PLAYERS_MAX) in a new world; a replay keeps what it was
       made with (16 before more were possible, nine before that). Test hooks: KFX_WORLD_PLAYERS9=1 keeps nine,
       KFX_WORLD_PLAYERS=n uses n. */
    if (replay.load_enable)
    {
        if (replay.head.players_in_use > 16)
            players_extend(replay.head.players_in_use);
        else if (flag_is_set(replay.head.flags, PSHF_Players16))
            players_extend(16);
    } else
    if (getenv("KFX_WORLD_PLAYERS9") == NULL)
    {
        const char *n = getenv("KFX_WORLD_PLAYERS");
        players_extend(((n != NULL) && (atoi(n) > STOCK_PLAYERS_COUNT)) ? atoi(n) : PLAYERS_MAX);
    }
    return true;
}

static TbBool world_init_state(void)
{
    W.gen = wg_world_create(W.seed);
    if (W.gen == NULL)
        return false;
    W.active = true;
    {
        struct WgStructRef start;
        wg_start_ref(W.gen, &start);
        W.home_x = start.heart_x;
        W.home_y = start.heart_y;
    }
    int missing = world_shift_audit();
    if (missing > 0)
        WARNLOG("World: %d position fields are not covered by the shift table", missing);
    return true;
}

void world_level_start(void)
{
    if (!W.pending)
        return;
    W.pending = false;
    netstate_level_start();
    world_progress_level_start();
    if ((game.map_tiles_x != WORLD_WINDOW_SLABS) || (game.map_tiles_y != WORLD_WINDOW_SLABS))
    {
        ERRORLOG("ENDLESS_WORLD needs a %dx%d map, this one is %dx%d", WORLD_WINDOW_SLABS, WORLD_WINDOW_SLABS,
            (int)game.map_tiles_x, (int)game.map_tiles_y);
        return;
    }
    if (!world_init_state())
        return;
    netstate_take_all_users();
    /* a network game: the other seats get their start dungeon at the first world turn; until their heart is bound
       they must not lose for having none */
    if (game.game_kind == GKind_MultiGame)
    {
        const PlayerNumber *seats = NULL;
        const int nseats = world_seat_players(&seats);
        for (int i = 1; i < nseats; i++)
            game.conf.rules[seats[i]].gameplay.heart_required = 0; /* a world keeper set up on one needs one again */
    }
    /* the start (chunk 0,0) goes in the middle of the window */
    W.origin_cx = -(WORLD_WINDOW_CHUNKS / 2);
    W.origin_cy = -(WORLD_WINDOW_CHUNKS / 2);
    /* whatever the stub map held goes */
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (thing_exists(thing) && !thing_is_picked_up(thing))
            remove_thing_quietly(thing);
    }
    /* players get their numbers only after the level files are loaded; things and rooms made now need them */
    PlayerNumber saved_ids[PLAYERS_MAX];
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        saved_ids[p] = get_player(p)->id_number;
        get_player(p)->id_number = p;
    }
    clear_band(0, 0, game.map_tiles_x - 1, game.map_tiles_y - 1);
    struct FillChunk list[WORLD_WINDOW_CHUNKS * WORLD_WINDOW_CHUNKS];
    int count = 0;
    if (world_paged_requested())
    {
        /* the chunked world: the map becomes pages at world positions; the stub's flat map is left behind.
           The chunks round the heart come now, the chunk manager loads the rest as they are wanted. */
        wp_set_paged(true);
        const int64_t hcx = floor_div(W.home_x, WORLD_CHUNK_SLABS), hcy = floor_div(W.home_y, WORLD_CHUNK_SLABS);
        for (int64_t cy = hcy - CM_HEART_RADIUS; cy <= hcy + CM_HEART_RADIUS; cy++)
        {
            for (int64_t cx = hcx - CM_HEART_RADIUS; cx <= hcx + CM_HEART_RADIUS; cx++)
            {
                if (wp_create(engine_chunk_of(cx), engine_chunk_of(cy)) == NULL)
                    continue;
                list[count].lcx = (int)(cx - W.origin_cx);
                list[count].lcy = (int)(cy - W.origin_cy);
                list[count].cd = chunk_data_for((int32_t)cx, (int32_t)cy);
                if (list[count].cd != NULL)
                    count++;
            }
        }
        SYNCLOG("World: paged mode, %d chunk pages, map %ldx%ld slabs", wp_count(), (long)game.map_tiles_x, (long)game.map_tiles_y);
    } else
    {
        for (int lcy = 0; lcy < WORLD_WINDOW_CHUNKS; lcy++)
        {
            for (int lcx = 0; lcx < WORLD_WINDOW_CHUNKS; lcx++)
            {
                list[count].lcx = lcx;
                list[count].lcy = lcy;
                list[count].cd = chunk_data_for(W.origin_cx + lcx, W.origin_cy + lcy);
                if (list[count].cd != NULL)
                    count++;
            }
        }
    }
    fill_chunks(list, count);
    for (int c = 0; c < count; c++)
        free(list[c].cd);
    place_frame();
    if (world_paged)
        ceiling_init(); /* the level loader computed ceilings for the stub, not for the pages */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        get_player(p)->id_number = saved_ids[p];
    W.last_shift_turn = 0;
    SYNCLOG("World: seed %s, %d chunks generated around the start, %d keepers known", W.seed_text, count, W.keeper_count);
    server_admin_log("world", "New world, seed %s: %d chunks generated around the start, %d keepers known", W.seed_text, count,
        W.keeper_count);
}

/** A place for seat plyr's copy of the start: their own direction, 160-260 slabs out, where no structure lies and
 *  no other start is near. */
#define SPAWN_FIRST_RADIUS 500      /* slabs from the first player's start to the second player's */
#define SPAWN_RADIUS_STEP 100       /* each later spawn this much further out */
#define SPAWN_GOLDEN_ANGLE 782      /* the golden angle in the engine's 2048 steps: spawns spread all round */

/** Where the n-th player after the first starts (n = 1, 2, ...): on a widening spiral round the first player's start.
 *  Integer sine tables, so every machine places it the same. Nudged out and sideways until no keeper, fortress or
 *  portal is near and no other start is close. */
static TbBool choose_seat_offset(PlayerNumber plyr, int64_t *dx, int64_t *dy)
{
    const int n = W.stamp_count + 1;
    const long radius = SPAWN_FIRST_RADIUS + (long)SPAWN_RADIUS_STEP * (n - 1);
    const long angle = (ANGLE_EAST + (long)(n - 1) * SPAWN_GOLDEN_ANGLE) & ANGLE_MASK;
    static const int outs[] = {0, 30, 60, 90, 120};
    static const int turns[] = {0, 40, -40, 80, -80, 120, -120};
    struct WgStructRef refs[2];
    for (size_t o = 0; o < sizeof(outs) / sizeof(outs[0]); o++)
    {
        for (size_t t = 0; t < sizeof(turns) / sizeof(turns[0]); t++)
        {
            const long r = radius + outs[o];
            const long a = (angle + turns[t]) & ANGLE_MASK;
            struct WorldStamp st = {plyr, ((int64_t)LbSinL(a) * r) / 65536, -((int64_t)LbCosL(a) * r) / 65536};
            int64_t x1, y1, x2, y2;
            stamp_rect(&st, &x1, &y1, &x2, &y2);
            /* no keeper, fortress or portal near (a reward cave under it is just covered) */
            const unsigned mask = (1u << WgT_Keeper) | (1u << WgT_Fortress) | (1u << WgT_Portal);
            if (wg_structures_in(W.gen, x1 - 8, y1 - 8, x2 + 8, y2 + 8, mask, refs, 2) > 0)
                continue;
            if (world_outpost_in_box(x1 - 8, y1 - 8, x2 + 8, y2 + 8))
                continue; /* nor a heroes outpost */
            /* nor ground already made (stored or loaded): the copy goes only into freshly generated chunks */
            TbBool made = false;
            for (int64_t cy = floor_div(y1 - 8, WG_CHUNK); (cy <= floor_div(y2 + 8, WG_CHUNK)) && !made; cy++)
                for (int64_t cx = floor_div(x1 - 8, WG_CHUNK); (cx <= floor_div(x2 + 8, WG_CHUNK)) && !made; cx++)
                    made = (table_find(&W.store, (int32_t)cx, (int32_t)cy) != NULL) ||
                        (world_paged && (wp_page(engine_chunk_of(cx), engine_chunk_of(cy)) != NULL));
            if (made)
                continue;
            TbBool clash = false;
            for (int k = 0; (k < W.stamp_count) && !clash; k++)
            {
                int64_t ax1, ay1, ax2, ay2;
                stamp_rect(&W.stamps[k], &ax1, &ay1, &ax2, &ay2);
                clash = (x1 <= ax2 + 32) && (ax1 <= x2 + 32) && (y1 <= ay2 + 32) && (ay1 <= y2 + 32);
            }
            if (clash)
                continue;
            *dx = st.dx;
            *dy = st.dy;
            return true;
        }
    }
    return false;
}

/** Network games: every human seat other than PLAYER0 without a heart gets a copy of the start structure, and the
 *  chunks round its heart load now. Returns the seats that got one (bit p). */
static void cameras_to_hearts(PlayerNumber only);

static PlayerBitFlags make_seat_starts(void)
{
    PlayerBitFlags made = pmask_none();
    if (!world_paged || (W.gen == NULL))
        return made;
    struct WgStructRef start;
    wg_start_ref(W.gen, &start);
    for (PlayerNumber p = 1; p < PLAYERS_COUNT; p++)
    {
        if (!world_player_is_human(p) || thing_exists(find_players_dungeon_heart(p)) || (W.stamp_count >= WORLD_MAX_STAMPS))
            continue;
        TbBool have = false;
        for (int n = 0; n < W.stamp_count; n++)
            have = have || (W.stamps[n].plyr == p);
        if (have)
            continue;
        int64_t dx, dy;
        if (!choose_seat_offset(p, &dx, &dy))
        {
            struct WorldStamp probe = {p, 180, 0};
            int64_t x1, y1, x2, y2;
            stamp_rect(&probe, &x1, &y1, &x2, &y2);
            struct WgStructRef refs[8];
            const int found = wg_structures_in(W.gen, x1 - 8, y1 - 8, x2 + 8, y2 + 8,
                (1u << WgT_Keeper) | (1u << WgT_Fortress) | (1u << WgT_Portal), refs, 8);
            WARNLOG("World: no place for player %d's start dungeon (start box %ldx%ld; at 180 east %d structures, first type %d)",
                (int)p, (long)(x2 - x1 + 1), (long)(y2 - y1 + 1), found, (found > 0) ? (int)refs[0].id.type : -1);
            continue;
        }
        struct WorldStamp *st = &W.stamps[W.stamp_count++];
        st->plyr = p;
        st->dx = dx;
        st->dy = dy;
        const int loaded = world_load_chunks_round((MapSlabCoord)engine_slab_x(start.heart_x + dx),
            (MapSlabCoord)engine_slab_y(start.heart_y + dy), 2);
        SYNCLOG("World: player %d starts at world slab (%ld,%ld), %d chunks loaded", (int)p, (long)(start.heart_x + dx),
            (long)(start.heart_y + dy), loaded);
        pmask_add(&made, p);
    }
    return made;
}

/** After the level's players are set up: hearts, keepers. Called from the first world turn. */
/** Levels to 100 (2026-11-07): a creature's abilities in the order it learns them; the 1st and 2nd keep
 *  their levels, the 3rd comes at level 10, the 4th at 20, the 5th at 30 ... Every model, heroes too; it gives the
 *  same result when run again (a load), and every creature's abilities follow at once. */
static TbBool abilities_spread = false;
static void world_spread_abilities(void)
{
    if (abilities_spread)
        return;
    abilities_spread = true;
#ifdef CREATURE_MAX_LEVEL_LIMIT
    const int level_cap = (int)creature_max_levels_for_owner(PLAYER0);
#else
    const int level_cap = CREATURE_MAX_LEVEL;
#endif
    if (level_cap <= CREATURE_MAX_LEVEL)
        return; /* creatures stop at level 10 here: the stock levels stay */
    int changed = 0;
    for (ThingModel model = 1; model < game.conf.crtr_conf.model_count; model++)
    {
        struct CreatureModelConfig *crconf = creature_stats_get(model);
        int order[LEARNED_INSTANCES_COUNT];
        int n = 0;
        for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
            if (crconf->learned_instance_id[i] > 0)
                order[n++] = i;
        /* by the level it was learnt at, the slot order breaking ties */
        for (int a = 1; a < n; a++)
            for (int b = a; (b > 0) && (crconf->learned_instance_level[order[b - 1]] > crconf->learned_instance_level[order[b]]); b--)
            {
                const int t = order[b - 1];
                order[b - 1] = order[b];
                order[b] = t;
            }
        for (int r = 2; r < n; r++)
        {
            const unsigned char level = (unsigned char)((r - 1) * 10); /* the 3rd (r = 2) at 10, the 4th at 20 ... */
            if (crconf->learned_instance_level[order[r]] != level)
            {
                crconf->learned_instance_level[order[r]] = level;
                changed++;
            }
        }
    }
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        creature_increase_available_instances(thing);
    }
    if (changed > 0)
    {
        /* for example, the Wizard's */
        char levels[64] = "";
        const long wizard = get_id(creature_desc, "WIZARD");
        if (wizard > 0)
        {
            const struct CreatureModelConfig *crconf = creature_stats_get((ThingModel)wizard);
            for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
                if (crconf->learned_instance_id[i] > 0)
                    snprintf(levels + strlen(levels), sizeof(levels) - strlen(levels), "%s%d", levels[0] ? "," : "", (int)crconf->learned_instance_level[i]);
        }
        SYNCLOG("World: creatures learn their 3rd ability at level 10, then every 10 levels (%d moved; the Wizard's at %s)", changed, levels);
    }
}

/** Endless worlds (2026-11-07): an imp learns from mining gold or dense gold. A turn of mining gives what a
 *  turn in a full training room gives it (its training work value at full efficiency) on the way to levels 2 and 3;
 *  from level 3 on that is divided by the level less one, so each level after 3 comes slower. */
void world_imp_mined_gold(struct Thing *creatng)
{
    if (!world_mode_active() || !thing_is_creature(creatng))
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl) || cctrl->exp_level_up)
        return; /* a level-up is pending: it comes first */
    long gain = compute_creature_work_value_for_room_role(creatng, RoRoF_CrTrainExp, ROOM_EFFICIENCY_MAX);
    if (gain < 1)
        gain = 1;
    if (cctrl->exp_level >= 2)
        gain /= (long)cctrl->exp_level; /* level 3 (exp_level 2): half; level 4: a third ... */
    cctrl->exp_points += gain;
    if (check_experience_upgrade(creatng) && cctrl->exp_level_up)
        SYNCLOG("World: %s %d of player %d reaches level %d mining gold", thing_model_name(creatng), (int)creatng->index,
            (int)creatng->owner, (int)cctrl->exp_level + 2);
}

static void world_first_turn(void)
{
    const PlayerBitFlags new_seats = make_seat_starts();
    bind_hearts();
    world_progress_first_turn();
    /* a new seat starts with the gold the level gives PLAYER0 (the script's START_MONEY names PLAYER0 only) */
    const struct Dungeon *first = get_players_num_dungeon(PLAYER0);
    if (!dungeon_invalid(first))
        W.start_money = first->total_money_owned;
    for (PlayerNumber p = 1; (p < PLAYERS_COUNT) && !dungeon_invalid(first); p++)
    {
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (pmask_has(new_seats, p) && !dungeon_invalid(dungeon) && (dungeon->total_money_owned == 0))
        {
            dungeon->offmap_money_owned = first->offmap_money_owned;
            dungeon->total_money_owned = first->total_money_owned;
        }
    }
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!world_player_is_human(p))
            continue;
        struct Thing *heartng = find_players_dungeon_heart(p);
        if (thing_exists(heartng))
            SYNCLOG("World: human player %d, heart %d at world slab (%ld,%ld)", (int)p, (int)heartng->index,
                (long)world_slab_x_of(subtile_slab(heartng->mappos.x.stl.num)),
                (long)world_slab_y_of(subtile_slab(heartng->mappos.y.stl.num)));
        else
            WARNLOG("World: human player %d has no heart", (int)p);
    }
    if (world_paged)
        cameras_to_hearts(PLAYER_NONE);
    setup_keepers();
    update_portals();
}

/** The users of a player (or of every human player: PLAYER_NONE) look at their heart. The cameras are game state,
 *  so on every machine all of them are set. */
static void cameras_to_hearts(PlayerNumber only)
{
    {
        for (NetUserId user = 0; user < MAX_NET_USERS; user++)
        {
            const PlayerNumber plyr_idx = get_net_user_player_number(user);
            struct UserState *ustate = get_user_state(user);
            if ((ustate == NULL) || !world_player_is_human(plyr_idx) || ((only != PLAYER_NONE) && (plyr_idx != only)))
                continue;
            struct DungeonCamera pose = ustate->dungeon_camera;
            if (plyr_idx == PLAYER0)
            {
                pose.x = (MapCoord)(slab_subtile_center(engine_slab_x(W.home_x)) * COORD_PER_STL + COORD_PER_STL / 2);
                pose.y = (MapCoord)(slab_subtile_center(engine_slab_y(W.home_y)) * COORD_PER_STL + COORD_PER_STL / 2);
            } else
            {
                struct Thing *heartng = find_players_dungeon_heart(plyr_idx);
                if (!thing_exists(heartng))
                    continue;
                pose.x = heartng->mappos.x.val;
                pose.y = heartng->mappos.y.val;
            }
            ustate->dungeon_camera = pose;
            if (user == get_my_user())
                sync_local_camera_pose(user, &pose);
        }
    }
}

void world_seat_joined(PlayerNumber plyr_idx)
{
    if (!W.active || !world_paged)
        return;
    const PlayerBitFlags made = make_seat_starts();
    bind_hearts();
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (pmask_has(made, plyr_idx) && !dungeon_invalid(dungeon) && (dungeon->total_money_owned == 0))
    {
        dungeon->offmap_money_owned = W.start_money;
        dungeon->total_money_owned = W.start_money;
    }
    /* the keeper's start (exploration, cameras, allies) now that it has a heart */
    init_player(get_player(plyr_idx), 0);
    world_progress_new_player(plyr_idx);
    cameras_to_hearts(plyr_idx);
    const struct Thing *heartng = find_players_dungeon_heart(plyr_idx);
    SYNCLOG("World: player %d joined the running world%s, heart %d", (int)plyr_idx,
        pmask_has(made, plyr_idx) ? " with a new start dungeon" : "", thing_exists(heartng) ? (int)heartng->index : 0);
}

/** Test hook KFX_WORLD_TAGDIG: reads the variable once. */
static const char *world_tagdig_env(void)
{
    static const char *env = NULL;
    static TbBool read = false;
    if (!read)
    {
        env = getenv("KFX_WORLD_TAGDIG");
        read = true;
    }
    return ((env != NULL) && (env[0] != '\0')) ? env : NULL;
}

/** Test hook: logs the player's dig tasks (count, world bounding box, how many lie on the window's outer ring). */
static void world_log_dig_tasks(const char *when)
{
    if (world_tagdig_env() == NULL)
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    if (dungeon_invalid(dungeon))
        return;
    int n = 0, on_edge = 0;
    int64_t wx1 = 0, wy1 = 0, wx2 = 0, wy2 = 0;
    long imax = min((long)dungeon->highest_task_number, (long)MAPTASKS_COUNT);
    for (long i = 0; i < imax; i++)
    {
        const struct MapTask *mtask = &dungeon->task_list[i];
        if (mtask->kind == 0)
            continue;
        MapSlabCoord sx = subtile_slab(stl_num_decode_x(mtask->coords)), sy = subtile_slab(stl_num_decode_y(mtask->coords));
        int64_t wx, wy;
        world_slab_to_world(sx, sy, &wx, &wy);
        if (n == 0)
        {
            wx1 = wx2 = wx;
            wy1 = wy2 = wy;
        }
        wx1 = min(wx1, wx); wx2 = max(wx2, wx); wy1 = min(wy1, wy); wy2 = max(wy2, wy);
        if ((sx <= 1) || (sy <= 1) || (sx >= game.map_tiles_x - 2) || (sy >= game.map_tiles_y - 2))
            on_edge++;
        n++;
    }
    SYNCLOG("World tagdig %s: %d dig tasks, world slabs x %lld..%lld y %lld..%lld, %d on the window's outer ring",
        when, n, (long long)wx1, (long long)wx2, (long long)wy1, (long long)wy2, on_edge);
}

/** Test hook: KFX_WORLD_TAGDIG="dx,dy,w,h[,1]" marks that rectangle of slabs (relative to the player's heart) for
 *  digging as the player at turn 30, the way a drag with the hand does; with ,1 it reveals the rectangle first.
 *  Only read in world levels. */
/** Test hook KFX_WORLD_TESTGOLD="dx,dy,w,h": at turn 25 the earth there (slabs from player 0's heart) is gold. */
static void world_testgold(void)
{
    const char *env = getenv("KFX_WORLD_TESTGOLD");
    if ((env == NULL) || (game.play_gameturn != 25))
        return;
    int dx = 0, dy = 0, w = 0, h = 0;
    if (sscanf(env, "%d,%d,%d,%d", &dx, &dy, &w, &h) < 4)
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    struct Thing *heart = dungeon_invalid(dungeon) ? INVALID_THING : thing_get(dungeon->dnheart_idx);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    int made = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const MapSlabCoord sx = hx + dx + x, sy = hy + dy + y;
            if (slab_coords_invalid(sx, sy) || (get_slabmap_block(sx, sy)->kind != SlbT_EARTH))
                continue;
            place_slab_type_on_map(SlbT_GOLD, slab_subtile_center(sx), slab_subtile_center(sy), game.neutral_player_num, 0);
            made++;
        }
    SYNCLOG("World test: %d earth slabs at heart%+d,%+d (%dx%d) are gold", made, dx, dy, w, h);
}

static void world_tagdig(void)
{
    const char *env = world_tagdig_env();
    if ((env == NULL) || (game.play_gameturn != 30))
        return;
    int dx = 0, dy = 0, w = 0, h = 0, reveal = 0;
    if (sscanf(env, "%d,%d,%d,%d,%d", &dx, &dy, &w, &h, &reveal) < 4)
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    struct Thing *heart = dungeon_invalid(dungeon) ? INVALID_THING : thing_get(dungeon->dnheart_idx);
    if (!thing_exists(heart))
        return;
    MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    int tagged = 0, unrevealed = 0;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            MapSlabCoord sx = hx + dx + x, sy = hy + dy + y;
            if ((sx < 1) || (sy < 1) || (sx >= game.map_tiles_x - 1) || (sy >= game.map_tiles_y - 1))
                continue;
            if (reveal)
                for (int k = 0; k < STL_PER_SLB * STL_PER_SLB; k++)
                    reveal_map_subtile(slab_subtile(sx, k % STL_PER_SLB), slab_subtile(sy, k / STL_PER_SLB), PLAYER0);
            if (!map_block_revealed(get_map_block_at(slab_subtile_center(sx), slab_subtile_center(sy)), PLAYER0))
                unrevealed++;
            if (tag_blocks_for_digging_in_area(slab_subtile(sx, 0), slab_subtile(sy, 0), PLAYER0))
                tagged++;
        }
    }
    SYNCLOG("World tagdig: marked %d slabs (%d of them unexplored) at heart%+d,%+d, %dx%d", tagged, unrevealed, dx, dy, w, h);
    world_log_dig_tasks("after marking");
}

/** Test hook: KFX_WORLD_AUTOSHIFT="E2,S1,W3,N1" moves the window by itself (3 chunks per step, one step every
 *  50 turns), so streaming can be checked without anyone at the controls. V<n> saves in slot n, L<n> loads
 *  it, Q quits to the menu, other letters wait. Only read in world levels. */
static TbBool world_autoshift(void)
{
    static const char *route = NULL;
    static int route_read = 0;
    static int step = 0, done_in_step = 0;
    if (!route_read)
    {
        route = getenv("KFX_WORLD_AUTOSHIFT");
        route_read = 1;
    }
    if ((route == NULL) || (route[0] == 0))
        return false;
    if ((game.play_gameturn < 100) || (game.play_gameturn < W.last_shift_turn + 50))
        return true;
    /* find step number `step` in the route */
    const char *p = route;
    for (int i = 0; (i < step) && (p != NULL); i++)
    {
        p = strchr(p, ',');
        if (p != NULL) p++;
    }
    if ((p == NULL) || (*p == 0))
        return true;
    char dir = *p;
    int n = atoi(p + 1);
    if (n <= 0) n = 1;
    if (dir == 'Q')
    {
        /* Q: quit to the menu, as the options panel does */
        W.last_shift_turn = game.play_gameturn;
        step++;
        done_in_step = 0;
        SYNCLOG("World autoshift: quit to the menu");
        set_players_packet_action(get_my_player(), PckA_QuitToMainMenu, 0, 0, 0, 0);
        return true;
    }
    if ((dir == 'V') || (dir == 'L'))
    {
        /* V<slot>: save the game in that slot; L<slot>: load it again */
        W.last_shift_turn = game.play_gameturn;
        step++;
        done_in_step = 0;
        if (dir == 'V')
        {
            SaveManager_FillSlot(n, "World test");
            int ok = SaveManager_Save(n);
            SYNCLOG("World autoshift: saved slot %d (%s)", n, ok ? "ok" : "failed");
        } else
        {
            enum SaveCheckResult why;
            enum SaveLoadOutcome outcome = SaveManager_Load(n, &why);
            SYNCLOG("World autoshift: loaded slot %d, outcome %d, check %d", n, (int)outcome, (int)why);
            W.last_shift_turn = game.play_gameturn;
        }
        return true;
    }
    int dcx = (dir == 'E') ? 1 : (dir == 'W') ? -1 : 0;
    int dcy = (dir == 'S') ? 1 : (dir == 'N') ? -1 : 0;
    if (!shift_window(dcx * WORLD_SHIFT_CHUNKS, dcy * WORLD_SHIFT_CHUNKS))
        SYNCLOG("World autoshift: step %c%d refused (pinned chunks)", dir, n);
    W.last_shift_turn = game.play_gameturn;
    if (++done_in_step >= n)
    {
        step++;
        done_in_step = 0;
    }
    return true;
}

/** Test hook (paged mode): KFX_WORLD_SENDIMP="dx,dy,period,count" makes the player's first imp hop dx,dy slabs onto
 *  open, loaded ground every `period` turns from turn 200, `count` times and then `count` times back, to see
 *  creatures load the world and chunks come back from the store. A hop over 32 slabs loads the target's chunks
 *  first, so one hop can go far (a big box of loaded ground). */
/** Test hook (chunked world): KFX_WORLD_PROFKEEPERS="n[,turn]" loads the bases of the n keepers nearest the start at
 *  turn 100 (and again at `turn`; with KFX_WORLD_WAKE=1 they wake at once), to measure what active keepers cost and
 *  to see keeper slots reused. Only read in world levels. */
/* Test hook KFX_WORLD_RNGTRACE=1: logs every draw from the synced random seeds (desync hunting in network tests) */
static void world_rng_trace(const uint32_t *seed, unsigned long range, const char *func_name, unsigned long place)
{
    const char *which = (seed == &game.player_random_seed) ? "player" : (seed == &game.action_random_seed) ? "action" :
        (seed == &game.ai_random_seed) ? "ai" : NULL;
    if (which != NULL)
        SYNCLOG("RNG %s %s:%lu range %lu seed %08x", which, func_name, place, range, (unsigned)*seed);
}

static void world_profkeepers(void)
{
    static int rng_trace = -1;
    if (rng_trace < 0)
    {
        rng_trace = (getenv("KFX_WORLD_RNGTRACE") != NULL) ? 1 : 0;
        if (rng_trace)
            lb_random_trace = world_rng_trace;
    }
    static int want = -1;
    static long again = 0;
    if (want < 0)
    {
        const char *v = getenv("KFX_WORLD_PROFKEEPERS");
        want = (v != NULL) ? atoi(v) : 0;
        again = ((v != NULL) && (strchr(v, ',') != NULL)) ? atol(strchr(v, ',') + 1) : 0;
    }
    if ((want <= 0) || !world_paged || (W.gen == NULL) ||
        ((game.play_gameturn != 100) && ((again <= 0) || (game.play_gameturn != (GameTurn)again))))
        return;
    static struct WgStructRef refs[WORLD_MAX_KEEPERS];
    const int found = wg_structures_in(W.gen, -3000, -3000, 3000, 3000, 1u << WgT_Keeper, refs, WORLD_MAX_KEEPERS);
    const int n = min(found, WORLD_MAX_KEEPERS);
    /* nearest first (selection by distance; ties keep the generator's order) */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (refs[j].distance < refs[i].distance)
            {
                struct WgStructRef t = refs[i];
                refs[i] = refs[j];
                refs[j] = t;
            }
    int loaded = 0;
    for (int i = 0; (i < n) && (i < want); i++)
    {
        const int64_t hx = engine_slab_x(refs[i].heart_x), hy = engine_slab_y(refs[i].heart_y);
        loaded += world_load_chunks_round((MapSlabCoord)hx, (MapSlabCoord)hy, 2);
        SYNCLOG("World profkeepers: keeper %d at world slab (%ld,%ld), %.0f slabs out", i + 1, (long)refs[i].heart_x,
            (long)refs[i].heart_y, refs[i].distance);
    }
    SYNCLOG("World profkeepers: %d chunks loaded for %d keepers (%d found)", loaded, min(n, want), found);
}

static void world_sendimp(void)
{
    static const char *env = NULL;
    static int env_read = 0;
    static int hops = 0;
    if (!env_read)
    {
        env = getenv("KFX_WORLD_SENDIMP");
        env_read = 1;
    }
    int dx = 0, dy = 0, period = 0, count = 0;
    if (!world_paged || (env == NULL) || (sscanf(env, "%d,%d,%d,%d", &dx, &dy, &period, &count) < 4) || (period <= 0))
        return;
    if ((game.play_gameturn < 200) || (((game.play_gameturn - 200) % period) != 0) || (hops >= 2 * count))
        return;
    if (hops >= count)
    {
        /* then the same way back, over the chunks that froze behind it */
        dx = -dx;
        dy = -dy;
    }
    static ThingIndex imp_idx = 0; /* the same imp every hop */
    struct Thing *imp = thing_get(imp_idx);
    if ((imp_idx == 0) || !thing_exists(imp) || (imp->class_id != TCls_Creature))
    {
        struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
        imp = dungeon_invalid(dungeon) ? INVALID_THING : thing_get(dungeon->digger_list_start);
        if (!thing_exists(imp))
            return;
        imp_idx = imp->index;
    }
    hops++;
    const MapSlabCoord tx = subtile_slab(imp->mappos.x.stl.num) + dx, ty = subtile_slab(imp->mappos.y.stl.num) + dy;
    /* a long jump (over 32 slabs) loads the ground round the target first */
    if ((abs(dx) > 32) || (abs(dy) > 32))
        world_load_chunks_round(tx, ty, 1);
    for (int r = 0; r <= 16; r++)
    {
        for (int oy = -r; oy <= r; oy++)
        {
            for (int ox = -r; ox <= r; ox++)
            {
                if ((abs(ox) != r) && (abs(oy) != r))
                    continue;
                const MapSlabCoord sx = tx + ox, sy = ty + oy;
                struct Map *mapblk = get_map_block_at(slab_subtile_center(sx), slab_subtile_center(sy));
                if (map_block_invalid(mapblk))
                    continue;
                /* plain earth is dug out as the imp lands (a hole of one slab); lava, water and rock are skipped */
                const SlabKind kind = get_slabmap_block(sx, sy)->kind;
                if ((kind == SlbT_EARTH) || (kind == SlbT_TORCHDIRT))
                    dig_out_block(slab_subtile_center(sx), slab_subtile_center(sy), PLAYER0);
                if (((mapblk->flags & SlbAtFlg_Blocking) != 0) || !slab_is_safe_land(PLAYER0, sx, sy))
                    continue;
                struct Coord3d pos;
                pos.x.val = subtile_coord_center(slab_subtile_center(sx));
                pos.y.val = subtile_coord_center(slab_subtile_center(sy));
                pos.z.val = 0;
                pos.z.val = get_floor_height_at(&pos);
                move_thing_in_map(imp, &pos);
                reset_interpolation_of_thing(imp);
                set_start_state(imp);
                SYNCLOG("World sendimp: hop %d, imp %d to world slab (%ld,%ld), slab kind %d", hops, (int)imp->index,
                    (long)world_slab_x_of(sx), (long)world_slab_y_of(sy), (int)get_slabmap_block(sx, sy)->kind);
                return;
            }
        }
    }
    SYNCLOG("World sendimp: hop %d, no open loaded ground near world slab (%ld,%ld)", hops,
        (long)world_slab_x_of(tx), (long)world_slab_y_of(ty));
}

/** Test hook helper (KFX_WORLD_SHOTS): the heart of the keeper nearest the local player's heart, or NULL. */
static const struct Thing *shots_nearest_other_heart(void)
{
    const struct Thing *mine = get_player_soul_container(my_player_number);
    const struct Thing *best = NULL;
    long best_d = LONG_MAX;
    for (PlayerNumber pl = 0; (pl < PLAYERS_COUNT) && thing_exists(mine); pl++)
    {
        if ((pl == my_player_number) || (pl == PLAYER_GOOD) || (pl == PLAYER_NEUTRAL))
            continue;
        const struct Thing *heart = get_player_soul_container(pl);
        if (!thing_exists(heart))
            continue;
        const long d = labs((long)heart->mappos.x.val - mine->mappos.x.val) +
            labs((long)heart->mappos.y.val - mine->mappos.y.val);
        if (d < best_d)
        {
            best_d = d;
            best = heart;
        }
    }
    return best;
}
static void world_parchment_look_at(int64_t wx, int64_t wy);
static void local_home_slab(int64_t *wx, int64_t *wy);

/** Test hook: KFX_WORLD_AUTOPAN="E300,S120,W400" scrolls the view by itself, one slab per turn (after turn
 *  100), so the window follows through the normal trigger. Only read in world levels; thin clients too. */
static void world_autopan(void)
{
    static const char *route = NULL;
    static int route_read = 0;
    static int step = 0, done_in_step = 0;
    if (!route_read)
    {
        route = getenv("KFX_WORLD_AUTOPAN");
        route_read = 1;
    }
    if ((route == NULL) || (route[0] == 0))
        return;
    const char *p = route;
    for (int i = 0; (i < step) && (p != NULL); i++)
    {
        p = strchr(p, ',');
        if (p != NULL) p++;
    }
    /* before turn 100 the view is held still; after the route it is left alone */
    if ((p == NULL) || (*p == 0))
        return;
    char dir = (game.play_gameturn < 100) ? 'H' : *p;
    int n = (dir == 'H') ? 1 : atoi(p + 1);
    int dx = (dir == 'E') ? 1 : (dir == 'W') ? -1 : 0;
    int dy = (dir == 'S') ? 1 : (dir == 'N') ? -1 : 0;
    struct UserState *ustate = get_user_state(get_my_user());
    if (ustate == NULL)
        return;
    /* the view is kept in world coordinates, so shifts and edge scrolling don't move it */
    static int64_t view_wx = 0, view_wy = 0;
    static int view_set = 0;
    const int64_t org_x = world_paged ? -(int64_t)WP_ORIGIN_SLB * STL_PER_SLB * COORD_PER_STL
                                      : ((int64_t)W.origin_cx * WORLD_CHUNK_SLABS - 1) * STL_PER_SLB * COORD_PER_STL;
    const int64_t org_y = world_paged ? -(int64_t)WP_ORIGIN_SLB * STL_PER_SLB * COORD_PER_STL
                                      : ((int64_t)W.origin_cy * WORLD_CHUNK_SLABS - 1) * STL_PER_SLB * COORD_PER_STL;
    if (!view_set)
    {
        view_wx = ustate->dungeon_camera.x + org_x;
        view_wy = ustate->dungeon_camera.y + org_y;
        view_set = 1;
    }
    view_wx += dx * STL_PER_SLB * COORD_PER_STL;
    view_wy += dy * STL_PER_SLB * COORD_PER_STL;
    struct DungeonCamera pose = ustate->dungeon_camera;
    pose.x = (MapCoord)max(STL_PER_SLB * COORD_PER_STL, min(view_wx - org_x, (int64_t)(game.map_subtiles_x - STL_PER_SLB) * COORD_PER_STL));
    pose.y = (MapCoord)max(STL_PER_SLB * COORD_PER_STL, min(view_wy - org_y, (int64_t)(game.map_subtiles_y - STL_PER_SLB) * COORD_PER_STL));
    ustate->dungeon_camera = pose;
    sync_local_camera_pose(get_my_user(), &pose);
    if (dir == 'H')
        return;
    if (++done_in_step >= n)
    {
        step++;
        done_in_step = 0;
        SYNCLOG("World autopan: step %d done, view at world slab (%ld,%ld)", step,
            (long)world_slab_x_of(coord_slab(pose.x)), (long)world_slab_y_of(coord_slab(pose.y)));
    }
}

static void sync_camera_after_zoom(void);

/** Test hook KFX_WORLD_SHOTS (local only). */
static void world_shots_hook(void)
{
    {
        /* test hook: KFX_WORLD_SHOTS="600,900k" saves the game's own screenshot at those turns as
           scrshots/world_p<player>_t<turn>.png: the frame as drawn, whatever the desktop shows (local only); "k"
           puts the view on the nearest other keeper's heart 40 turns before (single player only); "m" opens
           the parchment map on that heart (also in network games: the map view is local) */
        static const char *shots = NULL;
        static int shots_read = 0;
        if (!shots_read)
        {
            shots = getenv("KFX_WORLD_SHOTS");
            shots_read = 1;
        }
        for (const char *sp = shots; sp != NULL; sp = strchr(sp, ','))
        {
            if (*sp == ',')
                sp++;
            const char *end = sp;
            while ((*end >= '0') && (*end <= '9'))
                end++;
            if ((*end == 'm') && (atol(sp) - 40 == (long)game.play_gameturn))
                zoom_to_parchment_map();
            if ((*end == 'm') && (atol(sp) - 20 == (long)game.play_gameturn))
            {
                const struct Thing *best = shots_nearest_other_heart();
                if (best != NULL)
                {
                    world_parchment_look_at(world_slab_x_of(coord_slab(best->mappos.x.val)),
                        world_slab_y_of(coord_slab(best->mappos.y.val)));
                    SYNCLOG("World shots: map on %s's heart", player_code_name(best->owner));
                }
            }
            if ((*end == 'k') && (atol(sp) - 40 == (long)game.play_gameturn) && (game.game_kind != GKind_MultiGame))
            {
                /* "900k": look at the nearest other keeper's heart first */
                const struct Thing *best = shots_nearest_other_heart();
                struct UserState *ustate = get_user_state(get_my_user());
                if ((best != NULL) && (ustate != NULL))
                {
                    struct DungeonCamera pose = ustate->dungeon_camera;
                    pose.x = best->mappos.x.val;
                    pose.y = best->mappos.y.val;
                    ustate->dungeon_camera = pose;
                    sync_local_camera_pose(get_my_user(), &pose);
                    SYNCLOG("World shots: view on %s's heart", player_code_name(best->owner));
                }
            }
            if (atol(sp) == (long)game.play_gameturn)
            {
                char fname[64];
                if (network_is_active() && (netstate.my_id >= MAX_NET_USERS)) /* a spectator: no player of its own */
                    snprintf(fname, sizeof(fname), "scrshots/world_pS%d_t%lu.png", (int)netstate.my_id,
                        (unsigned long)game.play_gameturn);
                else
                    snprintf(fname, sizeof(fname), "scrshots/world_p%d_t%lu.png", (int)my_player_number,
                        (unsigned long)game.play_gameturn);
                take_screenshot(fname);
            }
        }
    }
}

/** Test hook KFX_WORLD_ASSIST (network tests). */
static void world_assist_hook(void)
{
    {
        /* test hook (network tests): KFX_WORLD_ASSIST="kind@turn" hands the local player's seat to the in-game
           assistant at that turn, through packets like the panel button, so the seat plays by itself and stays
           human (simulated players; the host must allow the assistant) */
        static int assist_kind = -1;
        static long assist_turn = 0;
        if (assist_kind < 0)
        {
            const char *v = getenv("KFX_WORLD_ASSIST");
            assist_kind = 0;
            if ((v != NULL) && (strchr(v, '@') != NULL))
            {
                assist_kind = atoi(v);
                assist_turn = atol(strchr(v, '@') + 1);
            }
        }
        /* once each, at the first turn seen at or after its time (a thin client may skip turns while catching up) */
        static GameTurn kind_sent = 0, toggle_sent = 0;
        if (game.play_gameturn < kind_sent)
            kind_sent = toggle_sent = 0; /* a new level */
        if ((assist_kind > 0) && (kind_sent == 0) && (game.play_gameturn >= (GameTurn)assist_turn))
        {
            kind_sent = game.play_gameturn;
            set_players_packet_action(get_my_player(), PckA_SetComputerKind, assist_kind, 0, 0, 0);
        }
        else if ((assist_kind > 0) && (kind_sent != 0) && (toggle_sent == 0) && (game.play_gameturn >= kind_sent + 4))
        {
            toggle_sent = game.play_gameturn;
            set_players_packet_action(get_my_player(), PckA_ToggleComputer, 0, 0, 0, 0);
            SYNCLOG("World assist: player %d asks for the computer assistant (kind %d)", (int)my_player_number, assist_kind);
        }
        if ((assist_kind > 0) && (game.play_gameturn > (GameTurn)assist_turn) && (((game.play_gameturn - assist_turn) % 600) == 100))
        {
            for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
            {
                const struct Dungeon *dungeon = get_players_num_dungeon(p);
                if (!world_player_is_human(p) || dungeon_invalid(dungeon))
                    continue;
                const struct Computer2 *comp = get_computer_player(p);
                SYNCLOG("World assist: player %d computer_enabled %d model %d, %d imps, %d creatures, %ld gold, area %ld",
                    (int)p, (int)dungeon->computer_enabled, computer_player_invalid(comp) ? -1 : (int)comp->model,
                    (int)dungeon->num_active_diggers, (int)dungeon->num_active_creatrs, (long)dungeon->total_money_owned,
                    (long)dungeon->total_area);
            }
        }
    }
}

/** Opening to the network by itself: a dedicated server at once (the host then watches), or the test hook
 *  KFX_WORLD_OPEN=turn at that turn. */
static void world_open_hook(void)
{
    static int open_turn = -1;
    static GameTurn tried_turn = 0;
    if (open_turn < 0)
        open_turn = (getenv("KFX_WORLD_OPEN") != NULL) ? atoi(getenv("KFX_WORLD_OPEN")) : 0;
    if (network_is_active() || (game.play_gameturn < 2) || (tried_turn == game.play_gameturn))
        return;
    if (netstate_dedicated())
    {
        if (tried_turn != 0)
            return; /* it failed once: the log says why */
        tried_turn = game.play_gameturn;
        if (network_open_running_game())
            network_host_become_observer();
        return;
    }
    if (world_menu_open_on_start)
    {
        world_menu_open_on_start = false; /* Host on the Endless Mode menu */
        tried_turn = game.play_gameturn;
        if (!network_open_running_game())
            message_add(MsgType_Blank, 0, "The world couldn't be opened to the network (is port 5556 in use?)");
        return;
    }
    if ((open_turn > 0) && (game.play_gameturn == (GameTurn)open_turn))
    {
        tried_turn = game.play_gameturn;
        network_open_running_game();
    }
}

/******************************************************************************/
/* engine_render.h's (its globals clash with names here): where a thing is drawn between turns */
struct ThingInterpolateResult
{
    struct Coord3d mappos;
    int32_t floor_height;
};
struct ThingInterpolateResult interpolate_thing(struct Thing *thing);

/* Test hook KFX_WORLD_CLIENTTEST="E300,D500,I600,Q1100" (thin clients, through the real input paths) */
static struct {
    TbBool read;
    int pan_turn, dig_turn, quit_turn, info_turn, possess_turn, walk_turn;
    int chat_turn[4], nchat;
    ThingIndex possessed;
    long eye_max;
    TbBool dig_on, dig_found;
    int dig_phase;
    unsigned long dig_seq;
    MapSlabCoord dig_sx, dig_sy;
} ct = {0};
#define CT_DIG_SLABS 3

static void client_test_read(void)
{
    if (ct.read)
        return;
    ct.read = true;
    ct.pan_turn = ct.dig_turn = ct.quit_turn = ct.info_turn = ct.possess_turn = ct.walk_turn = -1;
    const char *spec = getenv("KFX_WORLD_CLIENTTEST");
    for (const char *p = spec; (p != NULL) && (*p != '\0');)
    {
        const int t = atoi(p + 1);
        if (*p == 'E') ct.pan_turn = t;
        if (*p == 'D') ct.dig_turn = t;
        if (*p == 'Q') ct.quit_turn = t;
        if (*p == 'I') ct.info_turn = t;
        if (*p == 'C') ct.possess_turn = t;
        if (*p == 'W') ct.walk_turn = t;
        if ((*p == 'T') && (ct.nchat < 4)) ct.chat_turn[ct.nchat++] = t;
        p = strchr(p, ',');
        if (p != NULL) p++;
    }
}

/** Earth slabs in a row of CT_DIG_SLABS, 4-12 slabs from this player's heart. */
static TbBool client_test_find_dig(void)
{
    struct Dungeon *dungeon = get_players_num_dungeon(my_player_number);
    struct Thing *heart = thing_get(dungeon->dnheart_idx);
    if (!thing_exists(heart))
        return false;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    for (int r = 4; r <= 12; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
            {
                if ((abs(dx) != r) && (abs(dy) != r))
                    continue;
                TbBool row = true;
                for (int k = 0; (k < CT_DIG_SLABS) && row; k++)
                {
                    const MapSlabCoord x = hx + dx + k, y = hy + dy;
                    if (slab_coords_invalid(x, y) || (get_slabmap_block(x, y)->kind != SlbT_EARTH))
                        row = false;
                }
                if (row)
                {
                    ct.dig_sx = hx + dx;
                    ct.dig_sy = hy + dy;
                    return true;
                }
            }
    return false;
}

/** Each frame, after input(): the dig drag's mouse, the possession and the walk, in the local packet. */
void world_client_test_input(void)
{
    client_test_read();
    const GameTurn turn = game.play_gameturn;
    for (int i = 0; i < ct.nchat; i++)
    {
        if ((ct.chat_turn[i] < 0) || (turn < (GameTurn)ct.chat_turn[i]))
            continue;
        ct.chat_turn[i] = -1;
        char msg[64];
        snprintf(msg, sizeof(msg), "hello %d from %s", i + 1, netstate.users[netstate.my_id].name);
        send_network_chat_message_at(netstate.my_id, msg, -1, -1);
        SYNCLOG("Client test: chat \"%s\"", msg);
    }
    if ((ct.possessed != 0) && (get_user_view_type(get_my_user()) == PVT_CreatureContrl))
    {
        /* as the last frame drew them: the eye and the creature, both between their turns */
        struct Thing *crtng = thing_get(get_local_user_state()->controlled_thing_idx);
        const struct Camera *eye = get_local_active_camera();
        if (thing_exists(crtng) && (eye != NULL))
        {
            const struct ThingInterpolateResult drawn = interpolate_thing(crtng);
            const double dx = (double)eye->mappos.x.val - (double)drawn.mappos.x.val;
            const double dy = (double)eye->mappos.y.val - (double)drawn.mappos.y.val;
            const long d = (long)sqrt(dx * dx + dy * dy);
            if (d > ct.eye_max)
                ct.eye_max = d;
        }
    }
    if ((ct.possess_turn >= 0) && (turn >= (GameTurn)ct.possess_turn) && (ct.possessed == 0))
    {
        /* one of this player's creatures (its first imp) */
        struct Dungeon *dungeon = get_players_num_dungeon(my_player_number);
        struct Thing *crtng = thing_get(dungeon->digger_list_start);
        if (thing_is_creature(crtng))
        {
            ct.possessed = crtng->index;
            set_packet_power_on_thing(get_local_packet(), PwrK_POSSESS, crtng->index);
            SYNCLOG("Client test: possessing creature %d", (int)crtng->index);
        }
    }
    if ((ct.walk_turn >= 0) && (turn >= (GameTurn)ct.walk_turn) && (turn < (GameTurn)ct.walk_turn + 320))
    {
        /* as holding a move key: forward, left, back, right, 40 turns each (one way is open ground) */
        static const unsigned long keys[4] = {PCtr_MoveUp, PCtr_MoveLeft, PCtr_MoveDown, PCtr_MoveRight};
        get_local_packet()->control_flags |= keys[((turn - ct.walk_turn) / 40) % 4];
    }
    if (!ct.dig_on)
        return;
    const struct Packet *last = netstate_client_last_input();
    const unsigned long seq = (last != NULL) ? last->turn : 0;
    if (seq != ct.dig_seq)
    {
        ct.dig_seq = seq;
        ct.dig_phase++;
    }
    if (ct.dig_phase > CT_DIG_SLABS + 1)
    {
        ct.dig_on = false;
        return;
    }
    struct Packet *pckt = get_local_packet();
    const int k = min(max(ct.dig_phase - 1, 0), CT_DIG_SLABS - 1);
    pckt->pos_x = subtile_coord_center(slab_subtile_center(ct.dig_sx + k));
    pckt->pos_y = subtile_coord_center(slab_subtile_center(ct.dig_sy));
    pckt->control_flags &= ~(PCtr_Gui | PCtr_LBtnAnyAction | PCtr_RBtnClick);
    pckt->control_flags |= PCtr_MapCoordsValid;
    pckt->additional_packet_values = (pckt->additional_packet_values & ~PCAdV_ContextMask) | ((CSt_PickAxe << 1) & PCAdV_ContextMask);
    if (ct.dig_phase <= 1)
        pckt->control_flags |= PCtr_LBtnClick | PCtr_LBtnHeld;
    else if (ct.dig_phase <= CT_DIG_SLABS)
        pckt->control_flags |= PCtr_LBtnHeld;
    else
        pckt->control_flags |= PCtr_LBtnRelease;
}

/** Each thin client turn: the camera's push, the dig drag's start, the quit, and the logs. */
static void world_client_test_turn(void)
{
    client_test_read();
    const GameTurn t = game.play_gameturn;
    const struct DungeonCamera *cam = &get_user_state(get_my_user())->dungeon_camera;
    if ((ct.pan_turn >= 0) && ((t == (GameTurn)ct.pan_turn) || (t == (GameTurn)ct.pan_turn + 80)))
        SYNCLOG("Client test: the server has this player's camera at slab (%d,%d)", (int)coord_slab(cam->x), (int)coord_slab(cam->y));
    if ((ct.pan_turn >= 0) && (t >= (GameTurn)ct.pan_turn) && (t < (GameTurn)ct.pan_turn + 40))
        local_state.camera_movement_x = 1.0f; /* as holding the 'move right' key */
    if ((ct.dig_turn >= 0) && (t == (GameTurn)ct.dig_turn))
    {
        ct.dig_found = client_test_find_dig();
        if (ct.dig_found)
        {
            ct.dig_on = true;
            ct.dig_phase = 0;
            ct.dig_seq = 0;
            SYNCLOG("Client test: dragging a dig mark over slabs (%d..%d,%d)", (int)ct.dig_sx, (int)ct.dig_sx + CT_DIG_SLABS - 1, (int)ct.dig_sy);
        } else
            SYNCLOG("Client test: no earth by the heart to dig");
    }
    if (ct.dig_found && (t >= (GameTurn)ct.dig_turn) && (t <= (GameTurn)ct.dig_turn + 600) && (((t - ct.dig_turn) % 10) == 0))
    {
        int tagged = 0, dug = 0;
        for (int k = 0; k < CT_DIG_SLABS; k++)
        {
            if (find_from_task_list_by_slab(my_player_number, ct.dig_sx + k, ct.dig_sy) != -1)
                tagged++;
            if (get_slabmap_block(ct.dig_sx + k, ct.dig_sy)->kind != SlbT_EARTH)
                dug++;
        }
        SYNCLOG("Client test: dig marks predicted here %d, tagged on the server %d, dug %d", dig_prediction_count(), tagged, dug);
    }
    static int info_tries = 0;
    if ((ct.info_turn >= 0) && (t >= (GameTurn)ct.info_turn) && (info_tries < 10) && !menu_is_active(GMnu_QUERY) && (++info_tries > 0))
        go_to_adjacent_menu_tab(-1); /* the '?' tab: the player list */
    if ((ct.possessed != 0) && ((t % 10) == 0) && (get_user_view_type(get_my_user()) == PVT_CreatureContrl))
    {
        const struct Thing *crtng = thing_get(get_local_user_state()->controlled_thing_idx);
        const long moved = thing_exists(crtng) ? (labs((long)crtng->mappos.x.val - (long)crtng->previous_mappos.x.val)
            + labs((long)crtng->mappos.y.val - (long)crtng->previous_mappos.y.val)) : 0;
        SYNCLOG("Client test: possessed %d, the eye as drawn up to %ld from the creature as drawn; it moved %ld this turn",
            (int)get_local_user_state()->controlled_thing_idx, ct.eye_max, moved);
        ct.eye_max = 0;
    }
    if ((ct.quit_turn >= 0) && (t == (GameTurn)ct.quit_turn))
    {
        SYNCLOG("Client test: Quit");
        set_players_packet_action(get_my_player(), PckA_QuitToMainMenu, 0, 0, 0, 0);
    }
}

/** Test hook KFX_WORLD_TESTHEART="turn,turn,...": at each turn a heroes heart is made beside player 0's heart, hit by
 *  player 0 and destroyed: player 0 gets the XP (player levels). */
static void world_testheart_hook(void)
{
    const char *env = getenv("KFX_WORLD_TESTHEART");
    if (env == NULL)
        return;
    TbBool now = false;
    for (const char *c = env; (c != NULL) && (*c != '\0'); c = strchr(c, ','), c = (c != NULL) ? c + 1 : NULL)
        if ((GameTurn)atol(c) == game.play_gameturn)
            now = true;
    if (!now)
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    struct Thing *heart = dungeon_invalid(dungeon) ? INVALID_THING : thing_get(dungeon->dnheart_idx);
    if (!thing_exists(heart))
        return;
    struct Coord3d pos = heart->mappos;
    pos.x.val += 30 * COORD_PER_STL; /* well away: its fall breaks hearts near it, and the room under it */
    struct Thing *made = create_object(&pos, heart->model, PLAYER_GOOD, -1);
    if (!thing_exists(made))
    {
        SYNCLOG("World test: no heroes heart could be made");
        return;
    }
    world_heart_hit(made, PLAYER0);
    made->health = 0;
    SYNCLOG("World test: heroes heart %d made and hit by player 0", (int)made->index);
}

/** Test hook KFX_WORLD_TESTPORTAL="turn": at that turn the portal nearest player 0's heart becomes player 0's. */
static void world_testportal_hook(void)
{
    const char *env = getenv("KFX_WORLD_TESTPORTAL");
    if ((env == NULL) || ((GameTurn)atol(env) != game.play_gameturn))
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    struct Thing *heart = dungeon_invalid(dungeon) ? INVALID_THING : thing_get(dungeon->dnheart_idx);
    if (!thing_exists(heart))
        return;
    struct Room *best = NULL;
    long best_d = LONG_MAX;
    for (long ri = 1; ri < ROOMS_COUNT; ri++)
    {
        struct Room *room = room_get(ri);
        if (!room_exists(room) || !room_role_matches(room->kind, RoRoF_CrPoolSpawn) || (room->owner == PLAYER0))
            continue;
        const long d = labs((long)room->central_stl_x - heart->mappos.x.stl.num) + labs((long)room->central_stl_y - heart->mappos.y.stl.num);
        if (d < best_d)
        {
            best_d = d;
            best = room;
        }
    }
    if (best == NULL)
    {
        SYNCLOG("World test: no portal to take");
        return;
    }
    SYNCLOG("World test: player 0 takes the portal at subtile (%d,%d), %s", (int)best->central_stl_x, (int)best->central_stl_y,
        wg_biome_name(world_biome_at_subtile(best->central_stl_x, best->central_stl_y)));
    take_over_room(best, PLAYER0);
}

static struct Thing *test_find_outpost_book(void)
{
    const struct StructureList *slist = get_list_for_thing_class(TCls_Object);
    struct Thing *book = INVALID_THING;
    long guard = 0;
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if ((thing->model == world_progress_book_model()) && (thing->custom_box.box_kind == WORLD_OUTPOST_BOOK_KIND))
            book = thing;
    }
    return book;
}

/** Test hook KFX_WORLD_TESTOUTPOST="T": the heroes outpost nearest the start. T: its chunks load (a slab of player 0's
 *  on its way in keeps them, as territory) and the view goes there; T+100: its entrance door breaks; T+200: a creature
 *  of player 0's stands by the heart's portal; T+300: its heart falls to player 0; T+340: the spellbook is clicked
 *  (no Library yet: it teaches the Library), and a second one is shelved (it opens a research). */
static void world_testoutpost_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_TESTOUTPOST");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || (W.gen == NULL))
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t != 0) && (t != 100) && (t != 200) && (t != 300) && (t != 340))
        return;
    struct WgStructRef start;
    wg_start_ref(W.gen, &start);
    int64_t x0, y0, ex, ey, hx, hy, gx, gy;
    int odx, ody;
    if (!world_outpost_nearest(start.heart_x, start.heart_y, &x0, &y0, &ex, &ey, &odx, &ody, &hx, &hy, &gx, &gy))
    {
        SYNCLOG("World test: no outpost near the start");
        return;
    }
    const MapSlabCoord sex = (MapSlabCoord)engine_slab_x(ex), sey = (MapSlabCoord)engine_slab_y(ey);
    if (t == 0)
    {
        const int loaded = world_load_chunks_round((MapSlabCoord)engine_slab_x(x0 + 10), (MapSlabCoord)engine_slab_y(y0 + 10), 1);
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(sex + 3 * odx), slab_subtile_center(sey + 3 * ody), PLAYER0, 0);
        struct UserState *ustate = get_user_state(get_my_user());
        if (ustate != NULL)
        {
            struct DungeonCamera pose = ustate->dungeon_camera;
            pose.x = subtile_coord_center(slab_subtile_center((MapSlabCoord)engine_slab_x(x0 + 10)));
            pose.y = subtile_coord_center(slab_subtile_center((MapSlabCoord)engine_slab_y(y0 + 10)));
            ustate->dungeon_camera = pose;
            sync_local_camera_pose(get_my_user(), &pose);
        }
        SYNCLOG("World test: %d chunks loaded round the outpost; view on it", loaded);
    } else if (t == 100)
    {
        struct Thing *door = get_door_for_position(slab_subtile_center(sex), slab_subtile_center(sey));
        SYNCLOG("World test: the outpost's entrance door %s", thing_exists(door) ? "breaks" : "is not there");
        if (thing_exists(door))
            destroy_door(door);
    } else if (t == 200)
    {
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center((MapSlabCoord)engine_slab_x(gx)));
        pos.y.val = subtile_coord_center(slab_subtile_center((MapSlabCoord)engine_slab_y(gy)));
        pos.z.val = 0;
        struct Thing *crtng = create_creature(&pos, (ThingModel)get_id(creature_desc, "BILE_DEMON"), PLAYER0);
        if (thing_exists(crtng))
            init_creature_level(crtng, 9);
        SYNCLOG("World test: a creature of player 0's by the heart's portal: %s", thing_exists(crtng) ? "yes" : "no");
    } else if (t == 300)
    {
        struct Thing *heart = find_heart_near(slab_subtile_center((MapSlabCoord)engine_slab_x(hx)),
            slab_subtile_center((MapSlabCoord)engine_slab_y(hy)), INVALID_THING);
        SYNCLOG("World test: the outpost's heart %s", thing_exists(heart) ? "falls to player 0" : "is not there");
        if (thing_exists(heart))
        {
            world_heart_hit(heart, PLAYER0);
            heart->health = 0;
        }
    } else if (t == 340)
    {
        struct Thing *book = test_find_outpost_book();
        SYNCLOG("World test: the heroes' spellbook %s", thing_exists(book) ? "is clicked" : "is not there");
        if (thing_exists(book))
            world_progress_activate(book, get_player(PLAYER0));
        struct Thing *heart = get_player_soul_container(PLAYER0);
        struct Room *room = INVALID_ROOM;
        for (long ri = 1; ri < ROOMS_COUNT; ri++)
        {
            struct Room *r = room_get(ri);
            if (room_exists(r) && (r->owner == PLAYER0))
                room = r;
        }
        if (thing_exists(heart) && !room_is_invalid(room))
        {
            world_progress_drop_outpost_book(heart->mappos.x.stl.num + 3, heart->mappos.y.stl.num + 3);
            book = test_find_outpost_book();
            SYNCLOG("World test: a second heroes' spellbook %s", thing_exists(book) ? "is shelved" : "is not there");
            if (thing_exists(book))
                world_progress_book_shelved(book, room);
        }
    }
}

/** Test hook KFX_WORLD_TESTFREEZE="T": at T, 7 chunks east of the start, a heroes' Knight with a set mood (hunger 1234,
 *  annoyance 777, 5 kills, 2 paydays owed), a dead Archer, and a heroes' prison holding a neutral Fly; the chunk
 *  freezes when nothing keeps it; at T+700 it loads again and what came back is logged. */
static void world_testfreeze_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_TESTFREEZE");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || (W.gen == NULL))
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t != 0) && (t != 30) && (t != 700) && (t != 760))
        return;
    struct WgStructRef start;
    wg_start_ref(W.gen, &start);
    const int64_t wx = start.heart_x + 7 * WG_CHUNK, wy = start.heart_y;
    const MapSlabCoord sx = (MapSlabCoord)engine_slab_x(wx), sy = (MapSlabCoord)engine_slab_y(wy);
    if (t == 0)
    {
        world_load_chunks_round(sx, sy, 0);
        /* a heroes' prison, 3x3, and the things */
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                place_slab_type_on_map(SlbT_PRISON, slab_subtile_center(sx + 4 + dx), slab_subtile_center(sy + dy), PLAYER_GOOD, 0);
        struct Room *room = create_room(PLAYER_GOOD, (RoomKind)get_id(room_desc, "PRISON"), slab_subtile_center(sx + 4),
            slab_subtile_center(sy));
        if (!room_is_invalid(room))
            do_room_integration(room);
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(sx));
        pos.y.val = subtile_coord_center(slab_subtile_center(sy));
        pos.z.val = 0;
        struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
        if (thing_exists(knight))
        {
            struct CreatureControl *cctrl = creature_control_get_from_thing(knight);
            cctrl->hunger_level = 1234;
            cctrl->annoyance_level[1] = 777;
            cctrl->kills_num = 5;
            cctrl->paydays_owed = 2;
            cctrl->party.objective = CHeroTsk_DefendRooms;
            cctrl->party.original_objective = CHeroTsk_DefendRooms;
        }
        pos.x.val += 3 * COORD_PER_STL;
        struct Thing *body = create_dead_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), DCrSt_Dead, PLAYER_GOOD, 3);
        pos.x.val = subtile_coord_center(slab_subtile_center(sx + 4));
        struct Thing *fly = create_creature(&pos, (ThingModel)get_id(creature_desc, "FLY"), game.neutral_player_num);
        if (thing_exists(fly))
        {
            cleanup_current_thing_state(fly);
            internal_set_thing_state(fly, CrSt_CreatureArrivedAtPrison);
        }
        SYNCLOG("World test: freeze: knight %d, body %d, prison %d, fly %d at engine slab (%d,%d)",
            thing_exists(knight) ? (int)knight->index : -1, thing_exists(body) ? (int)body->index : -1,
            room_is_invalid(room) ? -1 : (int)room->index, thing_exists(fly) ? (int)fly->index : -1, (int)sx, (int)sy);
        return;
    }
    if (t == 700)
    {
        SYNCLOG("World test: freeze: the chunk is %s", (wp_page_of_slab(sx, sy) != NULL) ? "still loaded" : "frozen; it loads");
        world_load_chunks_round(sx, sy, 0);
    }
    /* T+30 (before it freezes) and T+700/760 (after): what is there */
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if ((labs((long)subtile_slab(thing->mappos.x.stl.num) - sx) > 8) || (labs((long)subtile_slab(thing->mappos.y.stl.num) - sy) > 8))
            continue;
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if ((thing->model != get_id(creature_desc, "KNIGHT")) && (thing->model != get_id(creature_desc, "FLY")))
            continue;
        SYNCLOG("World test: freeze: %s %d owner %d: hunger %d, annoyance %d, kills %d, owed %d, in prison %d",
            thing_model_name(thing), (int)thing->index, (int)thing->owner, (int)cctrl->hunger_level,
            (int)cctrl->annoyance_level[1], (int)cctrl->kills_num, (int)cctrl->paydays_owed, (int)creature_is_kept_in_prison(thing));
    }
    slist = get_list_for_thing_class(TCls_DeadCreature);
    guard = 0;
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if ((labs((long)subtile_slab(thing->mappos.x.stl.num) - sx) <= 8) && (labs((long)subtile_slab(thing->mappos.y.stl.num) - sy) <= 8))
            SYNCLOG("World test: freeze: dead %s %d, level %d", thing_model_name(thing), (int)thing->index,
                (int)thing->corpse.exp_level + 1);
    }
}

/** Test hook KFX_WORLD_TESTLOSE="p@turn": player p loses at that turn, as when its heart falls. */
static void world_testlose_hook(void)
{
    static int read = 0, plyr = -1, turn = -1;
    if (!read)
    {
        read = 1;
        const char *spec = getenv("KFX_WORLD_TESTLOSE");
        if ((spec != NULL) && (strchr(spec, '@') != NULL))
        {
            plyr = atoi(spec);
            turn = atoi(strchr(spec, '@') + 1);
        }
    }
    if ((plyr < 0) || (plyr >= PLAYERS_COUNT) || (game.play_gameturn != (GameTurn)turn))
        return;
    struct PlayerInfo *player = get_player(plyr);
    if (player_exists(player))
    {
        SYNCLOG("World test: player %d loses", plyr);
        set_player_as_lost_level(player);
    }
}

/** A thin client's world turn (network play): nothing is simulated here, only this machine's presentation. */
void world_client_turn(void)
{
    if (!W.active)
        return;
    world_colours_turn();
    world_progress_present();
    world_shots_hook();
    world_assist_hook();
    world_progress_client_turn();
    /* test hook KFX_WORLD_AUTOPAN: the packets take the moved view to the server (from turn 100, when the first
       keyframe has put this machine's camera where the server has the player) */
    if (game.play_gameturn >= 100)
        world_autopan();
    world_client_test_turn();
}

void world_process_turn(void)
{
    if (!W.active)
        return;
    world_spread_abilities();
    sync_camera_after_zoom();
    if (!world_paged)
        world_autopan();
    if (game.play_gameturn <= 1)
        world_first_turn();
    if (world_paged)
        world_autopan(); /* after the first turn has put the camera on the heart */
    if (W.rebind_hearts)
    {
        W.rebind_hearts = false;
        bind_hearts();
        /* saves from before chunks that come in were explored as at a level start: the loaded chunks get it now */
        int explored = 0;
        if (world_paged)
        {
            for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
            {
                explored += explore_like_level_start((MapSlabCoord)pg->cx * WORLD_CHUNK_SLABS, (MapSlabCoord)pg->cy * WORLD_CHUNK_SLABS);
                panel_map_update((MapSubtlCoord)pg->cx * CHUNK_STL, (MapSubtlCoord)pg->cy * CHUNK_STL, CHUNK_STL, CHUNK_STL);
            }
        } else
        {
            for (int lcy = 0; lcy < WORLD_WINDOW_CHUNKS; lcy++)
                for (int lcx = 0; lcx < WORLD_WINDOW_CHUNKS; lcx++)
                    explored += explore_like_level_start(window_chunk_slab(lcx), window_chunk_slab(lcy));
            panel_map_update(0, 0, game.map_subtiles_x, game.map_subtiles_y);
        }
        if (explored > 0)
            SYNCLOG("World: %d slabs of the loaded chunks explored as at a level start (rock, gold, gems, portals)", explored);
    }
    if ((game.play_gameturn % 16) == 0)
    {
        update_keepers();
        update_portals();
    }
    world_colours_turn(); /* local presentation only */
    world_progress_present();
    world_shots_hook();
    world_progress_turn();
    server_admin_turn(); /* a dedicated server's console: play time, status */
    world_open_hook();
    world_testlose_hook();
    world_testheart_hook();
    world_testportal_hook();
    world_testoutpost_hook();
    world_testfreeze_hook();
    world_daynight_turn();
    world_wild_turn();
    world_outposts_turn();
    world_testgold();
    world_tagdig();
    world_sendimp();
    world_profkeepers();
    world_assist_hook();
    {
        /* test hook: KFX_WORLD_FASTFORWARD=n runs n turns per drawn frame, as fast as the machine allows (pacing
           only, nothing synced) - a measure of how many turns a second the simulation alone can do */
        static int fast = -1;
        if (fast < 0)
        {
            const char *v = getenv("KFX_WORLD_FASTFORWARD");
            fast = (v != NULL) ? max(0, min(atoi(v), GAME_FAST_FORWARD_MAX)) : 0;
        }
        if ((fast > 0) && (local_state.fast_forward != fast))
            local_state.fast_forward = fast;
    }
    if (world_paged)
    {
        world_autoshift(); /* test hook: only its save, load and quit steps do anything here */
        WORLD_PROF(WPS_Chunks, chunk_manager_turn()); /* no window to shift: chunks come and go */
        return;
    }
    if (world_autoshift())
        return;
    if (game.play_gameturn < W.last_shift_turn + 8)
        return;
    MapSlabCoord vx, vy;
    if (!view_slab(&vx, &vy))
        return;
    int lcx = (int)floor_div(vx - 1, WORLD_CHUNK_SLABS), lcy = (int)floor_div(vy - 1, WORLD_CHUNK_SLABS);
    int dcx = 0, dcy = 0;
    if (lcx < WORLD_SHIFT_MARGIN_CHUNKS) dcx = -WORLD_SHIFT_CHUNKS;
    else if (lcx >= WORLD_WINDOW_CHUNKS - WORLD_SHIFT_MARGIN_CHUNKS) dcx = WORLD_SHIFT_CHUNKS;
    if (lcy < WORLD_SHIFT_MARGIN_CHUNKS) dcy = -WORLD_SHIFT_CHUNKS;
    else if (lcy >= WORLD_WINDOW_CHUNKS - WORLD_SHIFT_MARGIN_CHUNKS) dcy = WORLD_SHIFT_CHUNKS;
    if ((dcx != 0) || (dcy != 0))
        SYNCLOG("World: view at engine slab (%d,%d), window chunk (%d,%d)", (int)vx, (int)vy, lcx, lcy);
    /* one axis per shift; the other one follows a few turns later */
    if (dcx != 0)
    {
        if (shift_window(dcx, 0))
            return;
    }
    if (dcy != 0)
        shift_window(0, dcy);
}

void world_level_end(void)
{
    abilities_spread = false; /* the next world (or a load) spreads them again */
    memset(seat_tokens, 0, sizeof(seat_tokens)); /* a new world's seats have no owners yet (a load sets them after) */
    netstate_level_end();
    world_progress_level_end();
    world_levels_reset();
    world_outposts_reset();
    world_daynight_level_end();
    world_colours_reset();
    peek_cache_drop();
    if (world_paged)
        wp_set_paged(false);
    if (W.gen != NULL)
        wg_world_destroy(W.gen);
    table_free(&W.store);
    table_free(&W.memory);
    world_memory_gen++;
    table_free(&cm_wanted);
    free(W.portals);
    memset(&W, 0, sizeof(W));
}

/******************************************************************************/
/* Minimap */

unsigned short world_minimap_code(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!W.active)
        return 0;
    int64_t wsx = world_paged ? (int64_t)stl_x - (int64_t)WP_ORIGIN_SLB * STL_PER_SLB
                              : (int64_t)stl_x + ((int64_t)W.origin_cx * WORLD_CHUNK_SLABS - 1) * STL_PER_SLB;
    int64_t wsy = world_paged ? (int64_t)stl_y - (int64_t)WP_ORIGIN_SLB * STL_PER_SLB
                              : (int64_t)stl_y + ((int64_t)W.origin_cy * WORLD_CHUNK_SLABS - 1) * STL_PER_SLB;
    int64_t cx = floor_div(wsx, CHUNK_STL), cy = floor_div(wsy, CHUNK_STL);
    static int32_t last_cx = 0x7FFFFFFF, last_cy = 0x7FFFFFFF;
    static const uint16_t *last = NULL;
    static uint32_t last_gen = 0xFFFFFFFF;
    if ((cx != last_cx) || (cy != last_cy) || (last_gen != world_memory_gen))
    {
        struct ChunkSlot *s = table_find(&W.memory, (int32_t)cx, (int32_t)cy);
        last = (s != NULL) ? (const uint16_t *)s->data : NULL;
        last_cx = (int32_t)cx;
        last_cy = (int32_t)cy;
        last_gen = world_memory_gen;
    }
    if (last == NULL)
        return 0;
    int lx = (int)(wsx - cx * CHUNK_STL), ly = (int)(wsy - cy * CHUNK_STL);
    return last[ly * CHUNK_STL + lx];
}

/******************************************************************************/
/* Full-screen map */

static int64_t pview_wx, pview_wy;     /* world slab at the centre of the full-screen map */
static TbBool pview_valid;
static TbClockMSec pview_last_used;
static TbBool zoom_jumped;

static MapSlabCoord clamp_slab16(int64_t v)
{
    /* slab coordinates are 32-bit now (they were 16-bit); paged positions are about a million */
    return (MapSlabCoord)max(-0x3FFFFFFF, min(v, 0x3FFFFFFF));
}

void world_parchment_view_origin(MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    const TbClockMSec now = LbTimerClock();
    if (!pview_valid || (now - pview_last_used > 400))
    {
        /* the map was just opened: start on the camera */
        struct UserState *ustate = get_user_state(get_my_user());
        int64_t hx, hy;
        local_home_slab(&hx, &hy);
        MapSlabCoord ex = engine_slab_x(hx), ey = engine_slab_y(hy);
        if (!world_paged)
        {
            ex = game.map_tiles_x / 2;
            ey = game.map_tiles_y / 2;
        }
        if (ustate != NULL)
        {
            ex = coord_slab(ustate->dungeon_camera.x);
            ey = coord_slab(ustate->dungeon_camera.y);
        }
        pview_wx = world_slab_x_of(ex);
        pview_wy = world_slab_y_of(ey);
        pview_valid = true;
    }
    pview_last_used = now;
    /* the view is window-sized (in window mode the map is exactly that size) */
    *slb_x = clamp_slab16(engine_slab_x(pview_wx) - WORLD_WINDOW_SLABS / 2);
    *slb_y = clamp_slab16(engine_slab_y(pview_wy) - WORLD_WINDOW_SLABS / 2);
}

/** Test hook (KFX_WORLD_SHOTS "m"): the open parchment map shows this world slab (local view only). */
static void world_parchment_look_at(int64_t wx, int64_t wy)
{
    pview_wx = wx;
    pview_wy = wy;
    pview_valid = true;
    pview_last_used = LbTimerClock();
}

/** The local player's heart in world slabs: player 0's is the world home; a network seat's heart is kept loaded (the
 *  chunks round every human heart are wanted), so its thing gives it. Local presentation only. */
static void local_home_slab(int64_t *wx, int64_t *wy)
{
    *wx = W.home_x;
    *wy = W.home_y;
    if ((my_player_number == PLAYER0) || (my_player_number >= PLAYERS_COUNT) ||
        (my_player_number == PLAYER_GOOD) || (my_player_number == PLAYER_NEUTRAL))
        return; /* player 0, or a spectator */
    const struct Thing *heartng = find_players_dungeon_heart(my_player_number);
    if (!thing_exists(heartng))
        return;
    *wx = world_slab_x_of(coord_slab(heartng->mappos.x.val));
    *wy = world_slab_y_of(coord_slab(heartng->mappos.y.val));
}

void world_parchment_scroll(int dx_slabs, int dy_slabs)
{
    pview_wx += dx_slabs;
    pview_wy += dy_slabs;
    pview_last_used = LbTimerClock();
}

void world_parchment_home(void)
{
    local_home_slab(&pview_wx, &pview_wy);
    pview_valid = true;
    pview_last_used = LbTimerClock();
}

TbBool world_parchment_heart(MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    if (!W.active)
        return false;
    int64_t hx, hy;
    local_home_slab(&hx, &hy);
    *slb_x = clamp_slab16(engine_slab_x(hx));
    *slb_y = clamp_slab16(engine_slab_y(hy));
    return true;
}

TbBool world_zoom_from_map(int user, int32_t *stl_x, int32_t *stl_y)
{
    zoom_jumped = false;
    if (!W.active)
        return false;
    if (world_paged)
        return false; /* no window to move: the camera goes anywhere (only creatures load chunks) */
    const int32_t lo = STL_PER_SLB, hix = game.map_subtiles_x - STL_PER_SLB, hiy = game.map_subtiles_y - STL_PER_SLB;
    if ((*stl_x >= lo) && (*stl_x < hix) && (*stl_y >= lo) && (*stl_y < hiy))
        return false;
    /* centre the window on the chunk that was clicked */
    int64_t wx = world_slab_x_of(floor_div(*stl_x, STL_PER_SLB));
    int64_t wy = world_slab_y_of(floor_div(*stl_y, STL_PER_SLB));
    int dcx = (int)(floor_div(wx, WORLD_CHUNK_SLABS) - WORLD_WINDOW_CHUNKS / 2 - W.origin_cx);
    int dcy = (int)(floor_div(wy, WORLD_CHUNK_SLABS) - WORLD_WINDOW_CHUNKS / 2 - W.origin_cy);
    if (!shift_window(dcx, dcy))
    {
        SYNCLOG("World: the map jump to world slab (%ld,%ld) is blocked by an awake keeper", (long)wx, (long)wy);
        return false;
    }
    *stl_x -= dcx * WORLD_CHUNK_SUBTILES;
    *stl_y -= dcy * WORLD_CHUNK_SUBTILES;
    zoom_jumped = true;
    SYNCLOG("World: jumped to world slab (%ld,%ld) from the map", (long)wx, (long)wy);
    return true;
}

static TbBool zoom_camera_sync;

void world_after_zoom_from_map(int user)
{
    if (!zoom_jumped)
        return;
    zoom_jumped = false;
    /* The local camera reads its own copy of the packet (the packet history, also what replays keep), with
     * the subtile from before the jump; it is put right at the end of this game turn. */
    if (user == get_my_user())
        zoom_camera_sync = true;
}

static void sync_camera_after_zoom(void)
{
    if (!zoom_camera_sync)
        return;
    zoom_camera_sync = false;
    struct UserState *ustate = get_user_state(get_my_user());
    if (ustate != NULL)
        sync_local_camera_pose(get_my_user(), &ustate->dungeon_camera);
}

/******************************************************************************/
/* Saves */

static void blob_table(struct RecWriter *w, const struct ChunkTable *t)
{
    rec_u32(w, t->count);
    for (uint32_t i = 0; i < t->cap; i++)
    {
        const struct ChunkSlot *s = &t->slots[i];
        if (!s->used)
            continue;
        rec_put(w, &s->cx, 4);
        rec_put(w, &s->cy, 4);
        rec_u32(w, s->len);
        rec_put(w, s->data, s->len);
    }
}

static TbBool unblob_table(struct RecReader *r, struct ChunkTable *t)
{
    uint32_t n = rec_get_u32(r);
    for (uint32_t i = 0; (i < n) && !r->bad; i++)
    {
        int32_t cx, cy;
        rec_get(r, &cx, 4);
        rec_get(r, &cy, 4);
        uint32_t len = rec_get_u32(r);
        if ((len > r->len - r->pos) || r->bad)
        {
            r->bad = true;
            break;
        }
        uint8_t *data = (uint8_t *)malloc(len ? len : 1);
        if (data == NULL)
        {
            r->bad = true;
            break;
        }
        rec_get(r, data, len);
        struct ChunkSlot *s = table_insert(t, cx, cy);
        if (s == NULL)
        {
            free(data);
            r->bad = true;
            break;
        }
        free(s->data);
        s->data = data;
        s->len = len;
    }
    return !r->bad;
}

TbBool world_save_blob(unsigned char **data, uint32_t *len)
{
    *data = NULL;
    *len = 0;
    if (!W.active)
        return false;
    struct RecWriter w;
    memset(&w, 0, sizeof(w));
    rec_u32(&w, WORLD_BLOB_MAGIC);
    rec_u32(&w, WG_GENERATOR_VERSION);
    rec_put(&w, W.seed_text, sizeof(W.seed_text));
    rec_put(&w, &W.seed, 8);
    rec_put(&w, &W.origin_cx, 4);
    rec_put(&w, &W.origin_cy, 4);
    rec_u32(&w, (uint32_t)W.keeper_count);
    rec_put(&w, W.keepers, (uint32_t)(sizeof(struct WorldKeeper) * W.keeper_count));
    {
        const uint8_t low = (uint8_t)(pmask_low32(W.player_heart_frozen) & 0xFFu);
        rec_put(&w, &low, 1); /* players 0-7; the others in the HFZ2 section */
    }
    blob_table(&w, &W.store);
    blob_table(&w, &W.memory);
    rec_u32(&w, WORLD_PORTALS_MAGIC);
    rec_u32(&w, (uint32_t)W.portal_count);
    for (int i = 0; i < W.portal_count; i++)
    {
        rec_put(&w, &W.portals[i].x, 8);
        rec_put(&w, &W.portals[i].y, 8);
        rec_put(&w, &W.portals[i].owner, 1);
    }
    if (world_paged)
    {
        /* the loaded chunks' map: blocks (with their thing lists) and slabs field by field through the save schema
         * (struct Map, struct SlabMap; a load matches the fields by name, so it survives a KeeperFX update that
         * changes them), and the textures as bytes; the rest of a page is rebuilt after loading */
        struct SaveBuffer schema;
        struct SaveError err;
        memset(&schema, 0, sizeof(schema));
        memset(&err, 0, sizeof(err));
        const struct SaveStructDesc *roots[2] = {&save_desc_Map, &save_desc_SlabMap};
        if (save_schema_encode(&schema, roots, 2, SVM_Save, &err) != SVR_Ok)
        {
            ERRORLOG("World: the chunk pages could not be saved: %s", err.message);
            save_buf_free(&schema);
            free(w.buf);
            return false;
        }
        rec_u32(&w, WORLD_PAGES2_MAGIC);
        rec_u32(&w, schema.len);
        rec_put(&w, schema.data, schema.len);
        save_buf_free(&schema);
        rec_u32(&w, (uint32_t)wp_count());
        for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
        {
            struct SaveBuffer page;
            memset(&page, 0, sizeof(page));
            TbBool page_ok = true;
            for (int i = 0; page_ok && (i < WP_CHUNK_STL_CELLS); i++)
                page_ok = (save_encode_record(&page, &save_desc_Map, &pg->map[i], SVM_Save, &err) == SVR_Ok);
            for (int i = 0; page_ok && (i < WP_CHUNK_SLAB_CELLS); i++)
                page_ok = (save_encode_record(&page, &save_desc_SlabMap, &pg->slab[i], SVM_Save, &err) == SVR_Ok);
            if (!page_ok)
            {
                ERRORLOG("World: chunk page (%d,%d) could not be saved: %s", (int)pg->cx, (int)pg->cy, err.message);
                save_buf_free(&page);
                free(w.buf);
                return false;
            }
            rec_put(&w, &pg->cx, 4);
            rec_put(&w, &pg->cy, 4);
            rec_u32(&w, page.len);
            rec_put(&w, page.data, page.len);
            rec_put(&w, pg->ext, sizeof(pg->ext));
            rec_put(&w, pg->ext_initial, sizeof(pg->ext_initial));
            save_buf_free(&page);
        }
    }
    /* lair waits and the keepers' last-near turns (they steer the game; a network resync must carry them) */
    rec_u32(&w, WORLD_EXTRA_MAGIC);
    rec_u32(&w, (uint32_t)W.lair_wait_count);
    rec_put(&w, W.lair_waits, (uint32_t)(sizeof(struct WorldLairWait) * W.lair_wait_count));
    rec_u32(&w, (uint32_t)W.keeper_count);
    rec_put(&w, W.keeper_last_near, (uint32_t)(sizeof(GameTurn) * W.keeper_count));
    if (W.stamp_count > 0)
    {
        rec_u32(&w, WORLD_STAMPS_MAGIC);
        rec_u32(&w, (uint32_t)W.stamp_count);
        for (int n = 0; n < W.stamp_count; n++)
        {
            rec_put(&w, &W.stamps[n].plyr, 4);
            rec_put(&w, &W.stamps[n].dx, 8);
            rec_put(&w, &W.stamps[n].dy, 8);
        }
    }
    {
        /* the research-book progression */
        const size_t prg = world_progress_save_size();
        if (prg > 0)
        {
            uint8_t *pb = (uint8_t *)malloc(prg);
            if (pb != NULL)
            {
                world_progress_save(pb);
                rec_u32(&w, WORLD_PROGRESS_MAGIC);
                rec_u32(&w, (uint32_t)prg);
                rec_put(&w, pb, (uint32_t)prg);
                free(pb);
            }
        }
    }
    {
        PlayerBitFlags high = W.player_heart_frozen;
        high.w[0] &= ~0xFFu;
        if (pmask_any(high))
        {
            const uint8_t words = PLAYER_MASK_WORDS;
            rec_u32(&w, WORLD_HEARTS2_MAGIC);
            rec_put(&w, &words, 1);
            rec_put(&w, W.player_heart_frozen.w, 4 * PLAYER_MASK_WORDS);
        }
    }
    {
        /* the players' XP (player levels) */
        uint32_t nxp = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            if (world_player_xp(p) != 0)
                nxp++;
        if (nxp > 0)
        {
            rec_u32(&w, WORLD_LEVELS_MAGIC);
            rec_u32(&w, nxp);
            for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            {
                const uint32_t xp = world_player_xp(p);
                if (xp == 0)
                    continue;
                const uint32_t plyr = (uint32_t)p;
                rec_put(&w, &plyr, 4);
                rec_put(&w, &xp, 4);
            }
        }
    }
    {
        /* what happened to the heroes outposts */
        const size_t ops = world_outposts_save_size();
        uint8_t *ob = (ops > 0) ? (uint8_t *)malloc(ops) : NULL;
        if (ob != NULL)
        {
            world_outposts_save(ob);
            rec_u32(&w, WORLD_OUTPOSTS_MAGIC);
            rec_u32(&w, (uint32_t)ops);
            rec_put(&w, ob, (uint32_t)ops);
            free(ob);
        }
    }
    {
        const int64_t gold = (int64_t)W.start_money;
        rec_u32(&w, WORLD_STARTGOLD_MAGIC);
        rec_put(&w, &gold, 8);
    }
    /* the seats' owners: a save (single player, or a server); never a lockstep resync to the other machines */
    if (!network_is_active() || netstate_is_server())
    {
        uint32_t nkeys = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            if (seat_tokens[p] != 0)
                nkeys++;
        if (nkeys > 0)
        {
            rec_u32(&w, WORLD_KEYS_MAGIC);
            rec_u32(&w, nkeys);
            for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            {
                if (seat_tokens[p] == 0)
                    continue;
                const uint32_t plyr = (uint32_t)p;
                rec_put(&w, &plyr, 4);
                rec_put(&w, &seat_tokens[p], 8);
            }
        }
        /* the players' records: last, so an older game reads all it knows and leaves this */
        uint32_t nst = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        {
            const struct WorldPlayerStats *st = world_player_stats(p);
            if ((st->hearts | st->outposts | st->portals | st->play_turns) != 0)
                nst++;
        }
        if (nst > 0)
        {
            rec_u32(&w, WORLD_STATS_MAGIC);
            rec_u32(&w, nst);
            for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            {
                const struct WorldPlayerStats *st = world_player_stats(p);
                if ((st->hearts | st->outposts | st->portals | st->play_turns) == 0)
                    continue;
                const uint32_t v[5] = {(uint32_t)p, st->hearts, st->outposts, st->portals, st->play_turns};
                rec_put(&w, v, sizeof(v));
            }
        }
    }
    if ((w.buf == NULL) || w.err)
    {
        free(w.buf);
        return false;
    }
    TbBool ok = pack(w.buf, w.len, data, len);
    free(w.buf);
    return ok;
}

static void rec_skip(struct RecReader *r, uint32_t n)
{
    if (r->bad || (n > r->len - r->pos))
    {
        r->bad = true;
        return;
    }
    r->pos += n;
}

static enum WorldBlobCheck wbc_fail(enum WorldBlobCheck res, char *why, size_t why_len, const char *fmt, ...)
{
    if ((why != NULL) && (why_len > 0))
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, why_len, fmt, ap);
        va_end(ap);
    }
    return res;
}

/* Follows world_load_blob as far as the chunk pages; keep the two in step. */
enum WorldBlobCheck world_blob_check(const unsigned char *data, uint32_t len, char *why, size_t why_len)
{
    if ((why != NULL) && (why_len > 0))
        why[0] = 0;
    uint8_t *raw;
    uint32_t raw_len;
    if (!unpack(data, len, &raw, &raw_len))
        return wbc_fail(WBC_Damaged, why, why_len, "the saved world can't be unpacked");
    struct RecReader r = {raw, raw_len, 0, false};
    enum WorldBlobCheck res = WBC_Ok;
    if (rec_get_u32(&r) != WORLD_BLOB_MAGIC)
        res = wbc_fail(WBC_Damaged, why, why_len, "the world data is not a saved world");
    rec_get_u32(&r); /* the generator version: an older one still loads */
    rec_skip(&r, (uint32_t)(sizeof(W.seed_text) + 8 + 4 + 4));
    const uint32_t nk = rec_get_u32(&r);
    if ((res == WBC_Ok) && (nk > WORLD_MAX_KEEPERS))
        res = wbc_fail(WBC_Damaged, why, why_len, "the saved world lists %lu keepers", (unsigned long)nk);
    else
        rec_skip(&r, (uint32_t)(sizeof(struct WorldKeeper) * nk) + 1);
    for (int t = 0; (t < 2) && (res == WBC_Ok) && !r.bad; t++)
    {
        const uint32_t n = rec_get_u32(&r);
        for (uint32_t i = 0; (i < n) && !r.bad; i++)
        {
            rec_skip(&r, 8);
            rec_skip(&r, rec_get_u32(&r));
        }
    }
    if ((res == WBC_Ok) && !r.bad && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_PORTALS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t np = rec_get_u32(&r);
        if (np > WORLD_MAX_PORTALS)
            res = wbc_fail(WBC_Damaged, why, why_len, "the saved world lists %lu portals", (unsigned long)np);
        else
            rec_skip(&r, np * 17);
    }
    if ((res == WBC_Ok) && !r.bad && (r.len - r.pos >= 12) && (rec_peek_u32(&r) == WORLD_PAGES2_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t schema_len = rec_get_u32(&r);
        struct SaveFileSchema schema;
        struct SaveError err;
        memset(&schema, 0, sizeof(schema));
        memset(&err, 0, sizeof(err));
        if (r.bad || (schema_len > r.len - r.pos) || (save_schema_decode(r.buf + r.pos, schema_len, &schema, &err) != SVR_Ok))
            res = wbc_fail(WBC_Damaged, why, why_len, "the saved chunk pages' layout can't be read: %s", err.message);
        save_schema_free(&schema);
    }
    else if ((res == WBC_Ok) && !r.bad && (r.len - r.pos >= 16) && (rec_peek_u32(&r) == WORLD_PAGES_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t map_size = rec_get_u32(&r), slab_size = rec_get_u32(&r);
        if ((map_size != sizeof(struct Map)) || (slab_size != sizeof(struct SlabMap)))
            res = wbc_fail(WBC_Incompatible, why, why_len, "the world was saved by an older build: its chunk pages "
                "hold map cells of %lu and %lu bytes, this build's are %lu and %lu", (unsigned long)map_size,
                (unsigned long)slab_size, (unsigned long)sizeof(struct Map), (unsigned long)sizeof(struct SlabMap));
    }
    if ((res == WBC_Ok) && r.bad)
        res = wbc_fail(WBC_Damaged, why, why_len, "the saved world is cut short");
    free(raw);
    return res;
}

TbBool world_load_blob(const unsigned char *data, uint32_t len)
{
    world_level_end();
    uint8_t *raw;
    uint32_t raw_len;
    if (!unpack(data, len, &raw, &raw_len))
    {
        ERRORLOG("World: saved world is damaged");
        return false;
    }
    struct RecReader r = {raw, raw_len, 0, false};
    TbBool ok = (rec_get_u32(&r) == WORLD_BLOB_MAGIC);
    uint32_t ver = rec_get_u32(&r);
    if (ver != WG_GENERATOR_VERSION)
    {
        WARNLOG("World: saved with generator version %u, this game has %u", (unsigned)ver, (unsigned)WG_GENERATOR_VERSION);
    }
    rec_get(&r, W.seed_text, sizeof(W.seed_text));
    W.seed_text[sizeof(W.seed_text) - 1] = 0;
    rec_get(&r, &W.seed, 8);
    rec_get(&r, &W.origin_cx, 4);
    rec_get(&r, &W.origin_cy, 4);
    uint32_t nk = rec_get_u32(&r);
    if (nk > WORLD_MAX_KEEPERS)
        ok = false;
    else
    {
        rec_get(&r, W.keepers, (uint32_t)(sizeof(struct WorldKeeper) * nk));
        W.keeper_count = (int)nk;
    }
    {
        uint8_t low = 0;
        rec_get(&r, &low, 1);
        W.player_heart_frozen = pmask_from_u32(low); /* the others: the HFZ1 or HFZ2 section, if any */
    }
    ok = ok && unblob_table(&r, &W.store) && unblob_table(&r, &W.memory) && !r.bad;
    world_memory_gen++;
    peek_cache_drop();
    /* portals (saves from before the creature limit have none: portals are learnt again as they come in) */
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_PORTALS_MAGIC))
    {
        rec_get_u32(&r);
        uint32_t np = rec_get_u32(&r);
        if (np > WORLD_MAX_PORTALS)
            ok = false;
        for (uint32_t i = 0; ok && (i < np); i++)
        {
            int64_t x, y;
            int8_t owner;
            rec_get(&r, &x, 8);
            rec_get(&r, &y, 8);
            rec_get(&r, &owner, 1);
            struct WorldPortal *p = r.bad ? NULL : portal_add(x, y);
            if (p == NULL)
                ok = false;
            else
            {
                p->owner = owner;
                p->last_owner = owner;
            }
        }
    }
    /* a paged world's chunk pages: field by field (PGS2), or raw memory of the build that wrote them (PGS1) */
    if (ok && (r.len - r.pos >= 12) && (rec_peek_u32(&r) == WORLD_PAGES2_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t schema_len = rec_get_u32(&r);
        struct SaveFileSchema schema;
        struct SaveDecoder dec;
        struct SaveError err;
        memset(&schema, 0, sizeof(schema));
        memset(&err, 0, sizeof(err));
        if (r.bad || (schema_len > r.len - r.pos) || (save_schema_decode(r.buf + r.pos, schema_len, &schema, &err) != SVR_Ok))
        {
            ERRORLOG("World: the saved chunk pages' layout can't be read: %s", err.message);
            ok = false;
        }
        else
        {
            r.pos += schema_len;
            save_decoder_init(&dec, &schema, SVM_Save);
            const uint32_t npages = rec_get_u32(&r);
            if (npages > 1000000)
                ok = false;
            wp_set_paged(true);
            for (uint32_t n = 0; ok && (n < npages); n++)
            {
                int32_t cx, cy;
                rec_get(&r, &cx, 4);
                rec_get(&r, &cy, 4);
                const uint32_t plen = rec_get_u32(&r);
                struct WorldPage *pg = (r.bad || (plen > r.len - r.pos)) ? NULL : wp_create(cx, cy);
                if (pg == NULL)
                {
                    ok = false;
                    break;
                }
                const uint8_t *pdata = r.buf + r.pos;
                uint32_t pos = 0;
                for (int i = 0; ok && (i < WP_CHUNK_STL_CELLS); i++)
                {
                    uint32_t used = 0;
                    ok = (save_decode_record(&dec, &save_desc_Map, pdata + pos, plen - pos, &used, &pg->map[i], &err) == SVR_Ok);
                    pos += used;
                }
                for (int i = 0; ok && (i < WP_CHUNK_SLAB_CELLS); i++)
                {
                    uint32_t used = 0;
                    ok = (save_decode_record(&dec, &save_desc_SlabMap, pdata + pos, plen - pos, &used, &pg->slab[i], &err) == SVR_Ok);
                    pos += used;
                }
                if (!ok)
                    ERRORLOG("World: saved chunk page (%d,%d) is damaged: %s", (int)cx, (int)cy, err.message);
                r.pos += plen;
                rec_get(&r, pg->ext, sizeof(pg->ext));
                rec_get(&r, pg->ext_initial, sizeof(pg->ext_initial));
                ok = ok && !r.bad;
            }
            save_decoder_free(&dec);
            save_schema_free(&schema);
            SYNCLOG("World: %d chunk pages loaded", wp_count());
        }
    }
    else if (ok && (r.len - r.pos >= 16) && (rec_peek_u32(&r) == WORLD_PAGES_MAGIC))
    {
        rec_get_u32(&r);
        uint32_t map_size = rec_get_u32(&r), slab_size = rec_get_u32(&r), npages = rec_get_u32(&r);
        if ((map_size != sizeof(struct Map)) || (slab_size != sizeof(struct SlabMap)) || (npages > 1000000))
        {
            ERRORLOG("World: the saved chunk pages are from another build");
            ok = false;
        }
        else
        {
            wp_set_paged(true);
            for (uint32_t i = 0; ok && (i < npages); i++)
            {
                int32_t cx, cy;
                rec_get(&r, &cx, 4);
                rec_get(&r, &cy, 4);
                struct WorldPage *pg = r.bad ? NULL : wp_create(cx, cy);
                if (pg == NULL)
                {
                    ok = false;
                    break;
                }
                rec_get(&r, pg->map, sizeof(pg->map));
                rec_get(&r, pg->slab, sizeof(pg->slab));
                rec_get(&r, pg->ext, sizeof(pg->ext));
                rec_get(&r, pg->ext_initial, sizeof(pg->ext_initial));
                ok = !r.bad;
            }
            SYNCLOG("World: %d chunk pages loaded", wp_count());
        }
    }
    if (ok && (r.len - r.pos >= 12) && (rec_peek_u32(&r) == WORLD_EXTRA_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t nw = rec_get_u32(&r);
        if (nw > WORLD_LAIR_WAITS_MAX)
            r.bad = true;
        else
        {
            rec_get(&r, W.lair_waits, (uint32_t)(sizeof(struct WorldLairWait) * nw));
            W.lair_wait_count = (int)nw;
        }
        const uint32_t nkeepers = rec_get_u32(&r);
        if ((nkeepers > WORLD_MAX_KEEPERS) || (nkeepers != (uint32_t)W.keeper_count))
            r.bad = true;
        else
            rec_get(&r, W.keeper_last_near, (uint32_t)(sizeof(GameTurn) * nkeepers));
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_STAMPS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t n = rec_get_u32(&r);
        for (uint32_t k = 0; (k < n) && !r.bad; k++)
        {
            struct WorldStamp st;
            rec_get(&r, &st.plyr, 4);
            rec_get(&r, &st.dx, 8);
            rec_get(&r, &st.dy, 8);
            if (!r.bad && (W.stamp_count < WORLD_MAX_STAMPS))
                W.stamps[W.stamp_count++] = st;
        }
        ok = !r.bad;
    }
    world_progress_level_end(); /* a save without the section plays without the progression */
    world_outposts_set_enabled(false); /* and one without OPS1 was made before the outposts: it has none */
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_PROGRESS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t n = rec_get_u32(&r);
        if (!r.bad && (n <= r.len - r.pos))
        {
            world_progress_load(r.buf + r.pos, n);
            r.pos += n;
        } else
            r.bad = true;
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 5) && (rec_peek_u32(&r) == WORLD_HEARTS_MAGIC))
    {
        rec_get_u32(&r);
        uint8_t high = 0;
        rec_get(&r, &high, 1);
        W.player_heart_frozen.w[0] |= (uint32_t)high << 8;
        ok = !r.bad;
    } else
    if (ok && (r.len - r.pos >= 5) && (rec_peek_u32(&r) == WORLD_HEARTS2_MAGIC))
    {
        rec_get_u32(&r);
        uint8_t words = 0;
        rec_get(&r, &words, 1);
        for (int k = 0; (k < words) && !r.bad; k++)
        {
            uint32_t v = rec_get_u32(&r);
            if (k < PLAYER_MASK_WORDS)
                W.player_heart_frozen.w[k] = v;
        }
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_LEVELS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t n = rec_get_u32(&r);
        for (uint32_t k = 0; (k < n) && !r.bad; k++)
        {
            uint32_t plyr = 0, xp = 0;
            rec_get(&r, &plyr, 4);
            rec_get(&r, &xp, 4);
            if (!r.bad && (plyr < PLAYERS_MAX))
                world_set_player_xp((PlayerNumber)plyr, xp);
        }
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_OUTPOSTS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t n = rec_get_u32(&r);
        if (!r.bad && (n <= r.len - r.pos))
        {
            world_outposts_load(r.buf + r.pos, n);
            r.pos += n;
        } else
            r.bad = true;
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 12) && (rec_peek_u32(&r) == WORLD_STARTGOLD_MAGIC))
    {
        rec_get_u32(&r);
        int64_t gold = 0;
        rec_get(&r, &gold, 8);
        if (!r.bad && (gold >= 0))
            W.start_money = (GoldAmount)gold;
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_KEYS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t n = rec_get_u32(&r);
        for (uint32_t k = 0; (k < n) && !r.bad; k++)
        {
            uint32_t plyr = 0;
            uint64_t token = 0;
            rec_get(&r, &plyr, 4);
            rec_get(&r, &token, 8);
            if (!r.bad && (plyr < PLAYERS_MAX))
                seat_tokens[plyr] = token;
        }
        ok = !r.bad;
    }
    if (ok && (r.len - r.pos >= 8) && (rec_peek_u32(&r) == WORLD_STATS_MAGIC))
    {
        rec_get_u32(&r);
        const uint32_t n = rec_get_u32(&r);
        for (uint32_t k = 0; (k < n) && !r.bad; k++)
        {
            uint32_t v[5] = {0, 0, 0, 0, 0};
            rec_get(&r, v, sizeof(v));
            if (!r.bad && (v[0] < PLAYERS_MAX))
            {
                const struct WorldPlayerStats st = {v[1], v[2], v[3], v[4]};
                world_set_player_stats((PlayerNumber)v[0], &st);
            }
        }
        ok = !r.bad;
    }
    free(raw);
    if (!ok || !world_init_state())
    {
        ERRORLOG("World: saved world is damaged");
        world_level_end();
        return false;
    }
    W.last_shift_turn = game.play_gameturn;
    W.rebind_hearts = true;
    server_admin_log("world", "Loaded world seed %s: %lu chunks explored, %d keepers, %d portals", W.seed_text,
        (unsigned long)W.store.count, W.keeper_count, W.portal_count);
    SYNCLOG("World: loaded seed %s at origin (%d,%d), %lu chunks stored, %d keepers, %d portals", W.seed_text,
        (int)W.origin_cx, (int)W.origin_cy, (unsigned long)W.store.count, W.keeper_count, W.portal_count);
    SYNCLOG("World: loaded player 0 at %u XP (level %d), outposts %s (%u bytes of their state), start gold %ld",
        (unsigned)world_player_xp(PLAYER0), world_player_level(PLAYER0), (world_outposts_save_size() > 0) ? "on" : "off",
        (unsigned)world_outposts_save_size(), (long)W.start_money);
    return true;
}

TbBool world_load_blob_for_resync(const unsigned char *data, uint32_t len)
{
    if (!world_load_blob(data, len))
        return false;
    /* the host's game state already has the hearts bound and the ground explored */
    W.rebind_hearts = false;
    ceiling_init();
    return true;
}

void world_after_resync(void)
{
    if (!W.active)
        return;
    navigation_colours_refresh();
    light_stat_refresh();
    if (world_paged)
    {
        for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
            panel_map_update((MapSubtlCoord)pg->cx * CHUNK_STL, (MapSubtlCoord)pg->cy * CHUNK_STL, CHUNK_STL, CHUNK_STL);
    } else
        panel_map_update(0, 0, game.map_subtiles_x, game.map_subtiles_y);
}

TbBool world_view_slab(MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    return W.active && view_slab(slb_x, slb_y);
}

TbBool world_user_view_slab(int user, MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    return W.active && (user >= 0) && (user < MAX_NET_USERS) && user_view_slab((NetUserId)user, slb_x, slb_y);
}

void world_remove_thing_quietly(struct Thing *thing)
{
    remove_thing_quietly(thing);
}

void world_load_none(void)
{
    world_level_end();
}

/******************************************************************************/
/* for the heroes outposts (world_outpost.c) */
struct WgWorld *world_generator(void)
{
    return W.active ? W.gen : NULL;
}

int world_stamp_count(void)
{
    return W.active ? W.stamp_count : 0;
}

TbBool world_stamps_overlap(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    if (!W.active || (W.gen == NULL))
        return false;
    for (int k = 0; k < W.stamp_count; k++)
    {
        int64_t ax1, ay1, ax2, ay2;
        stamp_rect(&W.stamps[k], &ax1, &ay1, &ax2, &ay2);
        if ((x1 <= ax2) && (ax1 <= x2) && (y1 <= ay2) && (ay1 <= y2))
            return true;
    }
    return false;
}

TbBool world_engine_slab(int64_t wx, int64_t wy, MapSlabCoord *ex, MapSlabCoord *ey)
{
    if (!W.active)
        return false;
    const int64_t x = engine_slab_x(wx), y = engine_slab_y(wy);
    const TbBool loaded = world_paged ? (wp_page_of_slab((MapSlabCoord)x, (MapSlabCoord)y) != NULL)
        : ((x >= 1) && (x <= WORLD_WINDOW_SLABS - 2) && (y >= 1) && (y <= WORLD_WINDOW_SLABS - 2));
    if (!loaded)
        return false;
    *ex = (MapSlabCoord)x;
    *ey = (MapSlabCoord)y;
    return true;
}

int world_loaded_chunk_count(void)
{
    if (!W.active)
        return 0;
    return world_paged ? wp_count() : (WORLD_WINDOW_CHUNKS * WORLD_WINDOW_CHUNKS);
}

long world_stored_chunk_count(void)
{
    return W.active ? (long)W.store.count : 0;
}

int world_loaded_chunks(int32_t *wcx, int32_t *wcy, int max)
{
    int n = 0;
    if (!W.active)
        return 0;
    if (world_paged)
    {
        for (struct WorldPage *pg = wp_first(); (pg != NULL) && (n < max); pg = pg->next)
        {
            wcx[n] = (int32_t)floor_div(world_slab_x_of((int64_t)pg->cx * WORLD_CHUNK_SLABS), WG_CHUNK);
            wcy[n] = (int32_t)floor_div(world_slab_y_of((int64_t)pg->cy * WORLD_CHUNK_SLABS), WG_CHUNK);
            n++;
        }
    } else
    {
        for (int lcy = 0; lcy < WORLD_WINDOW_CHUNKS; lcy++)
            for (int lcx = 0; (lcx < WORLD_WINDOW_CHUNKS) && (n < max); lcx++)
            {
                wcx[n] = W.origin_cx + lcx;
                wcy[n] = W.origin_cy + lcy;
                n++;
            }
    }
    return n;
}

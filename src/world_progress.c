/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_progress.c
 *     Endless world: progression by research books (see world_progress.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_progress.h"

#include <string.h>
#include <stdlib.h>

#include "world_stream.h"
#include "net_secure.h"
#include "world_gen.h"
#include "world_daynight.h"
#include "world_misdeeds.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "config_terrain.h"
#include "config_magic.h"
#include "config_trapdoor.h"
#include "config_objects.h"
#include "config_creature.h"
#include "thing_objects.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_effects.h"
#include "creature_control.h"
#include "map_events.h"
#include "map_data.h"
#include "slab_data.h"
#include "gui_soundmsgs.h"
#include "power_specials.h"
#include "packets.h"
#include "keeperfx.hpp"
#include "gui_msgs.h"
#include "replay_rewind.h"
#include "world_outpost.h"
#include "room_data.h"
#include "server_admin.h"
#include "room_library.h"
#include "creature_states_rsrch.h"
#include "magic_powers.h"
#include "thing_stats.h"
#include "gui_frontmenu.h"
#include "world_hand.h"
#include "room_workshop.h"
#include "world_circles.h"
#include "creature_senses.h"
#include "world_storage.h"
#include "world_seasons.h"
#include "world_watchfire.h"
#include "world_dark.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
enum WprKind {
    WprK_Room,
    WprK_Power,
    WprK_Trap,
    WprK_Door,
};

struct WprItem {
    const char *name;      /* config name */
    const char *label;     /* shown in tooltips */
    uint8_t kind;
    uint8_t tier;          /* 1-5; 6 is Armageddon */
    uint8_t biome;         /* WgBiome the item's lore belongs to */
};

/* 47 items. The order is saved (bits of the unlocked mask): only append. */
static const struct WprItem wpr_items[] = {
    {"RESEARCH", "Library", WprK_Room, 1, WgB_Ruins},
    {"TRAINING", "Training Room", WprK_Room, 1, WgB_Earthen},
    {"WORKSHOP", "Workshop", WprK_Room, 1, WgB_Rocky},
    {"BRIDGE", "Bridge", WprK_Room, 1, WgB_Flooded},
    {"POWER_SIGHT", "Sight of Evil", WprK_Power, 1, WgB_Crystal},
    {"POWER_SPEED", "Speed Monster", WprK_Power, 1, WgB_Sandstone},
    {"GUARD_POST", "Guard Post", WprK_Room, 2, WgB_Earthen},
    {"BARRACKS", "Barracks", WprK_Room, 2, WgB_Sandstone},
    {"WOOD", "Wooden Door", WprK_Door, 2, WgB_Earthen},
    {"ALARM", "Alarm Trap", WprK_Trap, 2, WgB_Rocky},
    {"POISON_GAS", "Poison Gas Trap", WprK_Trap, 2, WgB_Flooded},
    {"POWER_OBEY", "Must Obey", WprK_Power, 2, WgB_Sandstone},
    {"POWER_CALL_TO_ARMS", "Call to Arms", WprK_Power, 2, WgB_Earthen},
    {"POWER_TUNNELLER", "Tunneller", WprK_Power, 2, WgB_Rocky},
    {"PRISON", "Prison", WprK_Room, 3, WgB_Frozen},
    {"TORTURE", "Torture Chamber", WprK_Room, 3, WgB_Volcanic},
    {"BRACED", "Braced Door", WprK_Door, 3, WgB_Rocky},
    {"LIGHTNING", "Lightning Trap", WprK_Trap, 3, WgB_Crystal},
    {"POWER_HEAL_CREATURE", "Heal", WprK_Power, 3, WgB_Ruins},
    {"POWER_CONCEAL", "Conceal Monster", WprK_Power, 3, WgB_Frozen},
    {"POWER_HOLD_AUDIENCE", "Hold Audience", WprK_Power, 3, WgB_Ruins},
    {"POWER_SLOW", "Slow", WprK_Power, 3, WgB_Flooded},
    {"POWER_FLIGHT", "Flight", WprK_Power, 3, WgB_Volcanic},
    {"POWER_VISION", "Vision", WprK_Power, 3, WgB_Crystal},
    {"POWER_FREEZE", "Freeze", WprK_Power, 3, WgB_Frozen},
    {"TEMPLE", "Temple", WprK_Room, 4, WgB_Flooded},
    {"GRAVEYARD", "Graveyard", WprK_Room, 4, WgB_Frozen},
    {"SCAVENGER", "Scavenger Room", WprK_Room, 4, WgB_Volcanic},
    {"STEEL", "Steel Door", WprK_Door, 4, WgB_Rocky},
    {"BOULDER", "Boulder Trap", WprK_Trap, 4, WgB_Rocky},
    {"LAVA", "Lava Trap", WprK_Trap, 4, WgB_Volcanic},
    {"SENTRY", "Sentry Trap", WprK_Trap, 4, WgB_Sandstone},
    {"POWER_LIGHTNING", "Lightning Strike", WprK_Power, 4, WgB_Crystal},
    {"POWER_PROTECT", "Protect Monster", WprK_Power, 4, WgB_Ruins},
    {"POWER_REBOUND", "Rebound", WprK_Power, 4, WgB_Crystal},
    {"POWER_CAVE_IN", "Cave-In", WprK_Power, 4, WgB_Rocky},
    {"POWER_TIME_BOMB", "Time Bomb", WprK_Power, 4, WgB_Volcanic},
    {"MAGIC", "Magic Door", WprK_Door, 5, WgB_Crystal},
    {"SECRET", "Secret Door", WprK_Door, 5, WgB_Ruins},
    {"MIDAS", "Midas Door", WprK_Door, 5, WgB_Sandstone},
    {"WORD_OF_POWER", "Word of Power Trap", WprK_Trap, 5, WgB_Ruins},
    {"TNT", "TNT", WprK_Trap, 5, WgB_Volcanic},
    {"BALLISTA", "Ballista", WprK_Trap, 5, WgB_Sandstone},
    {"POWER_CHICKEN", "Chicken", WprK_Power, 5, WgB_Earthen},
    {"POWER_DISEASE", "Disease", WprK_Power, 5, WgB_Flooded},
    {"POWER_DESTROY_WALLS", "Destroy Walls", WprK_Power, 5, WgB_Rocky},
    {"POWER_ARMAGEDDON", "Armageddon", WprK_Power, 6, WgB_Ruins},
};
#define WPR_ITEMS ((int)(sizeof(wpr_items) / sizeof(wpr_items[0])))
#define WPR_LIBRARY 0
#define WPR_PRISON 14
#define WPR_TORTURE 15
#define WPR_ARMAGEDDON (WPR_ITEMS - 1)

/** Destroy Walls and Armageddon stay off in endless worlds (2026-11-07): no book teaches them. */
static TbBool wpr_item_disabled(int i)
{
    return (i == WPR_ARMAGEDDON) || ((i >= 0) && (i < WPR_ITEMS) && ((strcmp(wpr_items[i].name, "POWER_DESTROY_WALLS") == 0) ||
        (strcmp(wpr_items[i].name, "POWER_DISEASE") == 0) || /* Disease: off since 2026-11-09 */
        (strcmp(wpr_items[i].name, "SCAVENGER") == 0))); /* the Scavenger Room: off since 2026-11-12 */
}

/** A human keeper's progression. */
struct WprPlayer {
    uint32_t lvl_pending_since;
    uint32_t lvl_done_turn; /* the turn a level was last finished (not saved): a "?" item goes next, then a level */ /* the turn a room's next level began waiting (it waits for the research in progress) */
    uint64_t unlocked;          /* bit per item */
    uint32_t books_read;
    uint32_t boons;
    uint8_t best_grade;
    uint16_t raid_pending;      /* books carried by raids that came for this player, dropped by the next hero to die */
    uint8_t offer_count;
    uint8_t offer_items[3];
    uint16_t offer_things[3];   /* the lore books on the map */
};

static int wpr_requested;
static int wpr_mode;            /* 0 off, 1 draft, 2 automatic */
static TbBool wpr_kit_given;
static PlayerBitFlags wpr_kit_players; /* seats that have had the kit */
static struct WprPlayer wpr_players[PLAYERS_MAX];
static int wpr_ids[WPR_ITEMS];  /* the item's room, power, trap or door kind (<= 0: not in this game's configs) */
static ThingModel book_model, lore_model;

#define WPR_SAVE_MAGIC 0x31475250u /* "PRG1" */
#define WPR_SAVE_VERSION 5 /* 2: the seats that had the kit, after the players (16 bits); 3: as a player set (u8 words, the
                              words); 4: then the room levels (count, then per player 4 levels, 4 allowed, the room
                              researched now, the turn its level began waiting); 5: rooms and spells (players, entries,
                              then per player the levels, the allowed levels, the entry researched now, the turn) */

/* rooms with levels: a level-up among a book's choices is item WPR_ITEMS + room */
#define WPR_TRAINING 1
/* Amenity Mastery (2026-11-11): the keeper's rooms, spells and traps with levels. Entries 0-3 rooms, 4-10 spells
 * (keeper powers), 11-16 more rooms, 17-20 traps. The order is saved: only append. A duplicate book no longer allows
 * the next level outright: it gives its entry XP, more the deeper it was found, and a level is allowed when the
 * entry's bar is full; the Library then researches it (the Workshop builds a trap's). */
#define WRL_ROOMS 21
#define WRL_FIRST_POWER 4
#define WRL_FIRST_ROOM2 11
#define WRL_FIRST_TRAP 17
#define WRL_IS_POWER(r) (((r) >= WRL_FIRST_POWER) && ((r) < WRL_FIRST_ROOM2))
#define WRL_IS_TRAP(r) ((r) >= WRL_FIRST_TRAP)
#define WRL_MAX 5 /* room for the levels of any entry */
#define WRL_ITEM(r) (WPR_ITEMS + (r))
#define WPR_OPTIONS (WPR_ITEMS + WRL_ROOMS)
static const char *const wrl_names[WRL_ROOMS] = {"LAIR", "GARDEN", "RESEARCH", "TRAINING", "POWER_IMP", "POWER_SIGHT",
    "POWER_SPEED", "POWER_CONCEAL", "POWER_HEAL_CREATURE", "POWER_LIGHTNING", "POWER_PROTECT",
    "TREASURE", "WORKSHOP", "PRISON", "TORTURE", "BARRACKS", "GRAVEYARD",
    "POISON_GAS", "LIGHTNING", "WORD_OF_POWER", "LAVA"};
static const char *const wrl_labels[WRL_ROOMS] = {"Lair", "Hatchery", "Library", "Training Room", "Create Imp",
    "Sight of Evil", "Speed Monster", "Conceal Monster", "Heal", "Lightning Strike", "Protect Monster",
    "Treasury", "Workshop", "Prison", "Torture Chamber", "Barracks", "Graveyard",
    "Poison Gas Trap", "Lightning Trap", "Word of Power Trap", "Lava Trap"};
/* Amenity Mastery's numbers (how many levels an entry has, what a level takes and what it gives) are kept encoded:
 * keepers are meant to find them by playing. Decoded on first use into struct WrlNumbers. */
static const uint8_t wrl_data[] = {
    0x5E,0x82,0xA4,0xCC,0xF2,0x16,0x38,0x60,0x93,0xB5,0xD9,0xFF,0x27,0x49,0x6D,0x93,0xC6,0xE8,0x09,0x32,0x53,0x7C,0xA2,0xC6,
    0xF5,0x1B,0x43,0x65,0x89,0xAF,0xD7,0xF9,0x2A,0x4E,0x76,0x98,0xBE,0xE2,0x04,0x2C,0x5E,0x81,0xC2,0xCB,0x60,0x14,0x3A,0x5F,
    0x8F,0xB4,0xF5,0xFF,0x7B,0x4A,0x6D,0x92,0xC2,0xE7,0xC4,0x31,0xA2,0x7A,0xA0,0xC5,0xF5,0x1A,0x13,0x65,0xD1,0xAC,0xD3,0xF8,
    0x28,0x4D,0xE2,0x96,0x9C,0xE2,0x06,0x2B,0x5B,0x80,0x89,0xCB,0xB7,0x16,0x39,0x5E,0x8E,0xB3,0x2C,0xFC,0xCA,0x44,0x6C,0x91,
    0xC1,0xE6,0x9B,0x31,0x0D,0x78,0x9F,0xC4,0xF4,0x19,0xAE,0x62,0xD0,0xAF,0xF2,0xF4,0xCF,0x4F,0xE1,0x97,0x9B,0xE3,0xED,0x29,
    0x5A,0x7F,0x34,0xC8,0xCE,0x10,0xD0,0x5E,0x8D,0xB2,0x1F,0xFC,0xD5,0x47,0x6B,0x90,0xC0,0xE5,0xC2,0x2F,0xA0,0x78,0x9E,0xC3,
    0xF3,0x18,0xAD,0x63,0xA7,0xAF,0xD1,0xF6,0x26,0x4B,0xE0,0x94,0x9A,0xDC,0x04,0x29,0x59,0x7E,0x6B,0xC8,0x7D,0x13,0x37,0x5C,
    0x8C,0xB1,0x46,0xFA,0x00,0x46,0x6A,0x8F,0xBF,0xE4,0xC1,0x2E,0xA7,0x79,0x75,0xC1,0x22,0x10,0xC8,0x60,0x6E,0xA8,0xD0,0xF5,
    0x25,0x4A,0x9B,0x95,0x51,0xDD,0x03,0x28,0x58,0x7D,0x1E,0xC5,0xEC,0x11,0x36,0x5B,0x8B,0xB0,0xD5,0xFA,0x1F,0x44,0x6C,0x8E,
    0xB6,0xE3,0x08,0x2D,0x52,0x77,0x96,0xC1,0xE5,0x16,0x3B,0x60,0x85,0xAA,0xCA,0xF4,0x2E,0x49,0x6E,0x93,0xB8,0xDD,0x07,0x27,
    0x5F,0x7C,0xA1,0xC6,0xEB,0x10,0x30,0x5A,0x80,0xAF,0xD4,0xF9,0x1E,0x43,0x6D,0x8D,0xB7,0xE2,0x07,0x2C,0x51,0x76,0x9E,0xC0,
    0xFA,0x15,0x3A,0x5F,0x84,0xA9,0xCB,0xF3,0x29,0x48,0x6D,0x92,0xB7,0xDC,0x04,0x26,0x5C,0x7B,0xA0,0xC5,0xEA,0x0F,0x35,0x59,
    0x8B,0xAE,0xD3,0xF8,0x1D,0x42,0x66,0x8C,0xBE,0xE1,0x06,0x2B,0x50,0x75,0x9B,0xBF,0xED,0x14,0x3B,0x5E,0x81,0xA8,0xCD,0xF2,
    0x22,0x47,0x6F,0x91,0xB0,0xDB,0x06,0x25,0x53,0x7A,0x9F,0xC4,0xE9,0x0E,0x30,0x58,0x81,0xAD,0xDB,0xF7,0x15,0x41,0x66,0x8B,
    0xBB,0xE0,0xCD,0x2A,0xDF,0x75,0x09,0xBF,0x7E,0x12,0x38,0x5D,0x82,0xA7,0xB2,0xF5,0x59,0x43,0xBD,0x96,0x65,0xDD,0xFF,0x24,
    0x54,0x79,0x9C,0xC3,0xEF,0x0D,0x38,0x57,0x8D,0xAC,0xD1,0xF6,0x1B,0x40,0x66,0x8A,0xBC,0xDF,0x0D,0x29,0x47,0x73,0x98,0xBD,
    0xED,0x12,0x37,0x5C,0x11,0xA7,0x93,0xF2,0x78,0x47,0x6A,0x8F,0xB4,0xD9,0xFC,0x23,0x51,0x78,0x99,0xC2,0xE3,0x0C,0x31,0x56,
    0x86,0xAB,0xD0,0xF5,0x18,0x3F,0x66,0x89,0xBB,0xDE,0x03,0x28,0x4D,0x72,0x97,0xBC,0xEC,0x11,0x36,0x5B,0x80,0xA5,0xCA,0xEF,
    0x1F,0x44,0x69,0x8E,0xB3,0xD8,0xFD,0x22,0x53,0x77,0x9C,0xC1,0xE6,0x0B,0x30,0x55,0x85,0xAA,0xCF,0xF4,0x19,0x3E,0x63,0x88,
    0xB8,0xDD,0x02,0x27,0x4C,0x71,0x96,0xBB,0xEB,0x10,0x35,0x5A,0x7F,0xA4,0xC9,0xEE,0x1E,0x43,0x68,0x8D,0xB2,0xD7,0xFC,0x21,
    0x51,0x76,0x9A,0xC0,0xE7,0x0A,0x2D,0x54,0x86,0xA9,0xCE,0xF3,0x18,0x3D,0x62,0x87,0xB7,0xDC,0x01,0x26,0x4B,0x70,0x95,0xBA,
    0xEA,0x0F,0x34,0x59,0x7E,0xA3,0xC8,0xED,0x1D,0x42,0x67,0x8C,0xB1,0xD6,0xF8,0x20,0x56,0x75,0x9C,0xBF,0xE2,0x09,0x2E,0x53,
    0x83,0xA8,0xCD,0xF2,0x17,0x3C,0x61,0x86,0xB6,0xDB,0x00,0x25,0x4A,0x6F,0x94,0xB9,0xE9,0x0E,0x33,0x58,0x7D,0xA2,0xC7,0xEC,
    0x1C,0x41,0x66,0x8B,0xB0,0xD5,0xFA,0x1F,0x4F,0x74,0x99,0xBE,0xE3,0x08,0x2D,0x52,0x82,0xA7,0xCC,0xF1,0x16,0x3B,0x60,0x85,
    0xB5,0xDA,0x0B,0x25,0xBD,0x6F,0x67,0xB9,0x1C,0x0C,0x32,0x57,0x7C,0xA1,0xC4,0xEB,0x18,0x40,0x61,0x8A,0xAA,0xD4,0xF9,0x1E,
    0x4E,0x73,0x9A,0xBD,0xE6,0x07,0x2A,0x51,0x89,0xA6,0xCB,0xF0,0x15,0x3A,0x5D,0x84,0xB7,0xD9,0xFA,0x23,0x4D,0x6D,0x98,0xB7,
    0x8E,0x0C,0xF9,0x56,0x47,0xA0,0xC7,0xEA,0x18,0x3F,0x64,0x89,0xAE,0xD3,
};
struct WrlNumbers {
    int16_t top[WRL_ROOMS];                 /* the top level of each entry */
    int16_t need[WRL_ROOMS][4];             /* the XP a level takes (the bar starts again after a level) */
    int16_t room[WRB_COUNT][4];             /* enum WorldRoomBonus, by level */
    int16_t power[WPB_COUNT][WRL_MAX + 1];  /* enum WorldPowerBonus, by level */
    int16_t trap[4][4][WRL_MAX + 1];        /* trap entry, enum WorldTrapBonus, by level */
    int16_t times[3][WRL_MAX + 1];          /* what a level costs, times the first: rooms, spells, traps */
    int16_t misc[8];
};
enum { WRLN_XpPerCircle = 0, WRLN_CostPercent, WRLN_PoisonTurns, WRLN_PoisonEvery, WRLN_ChainSlabs, WRLN_ChainDivide };
typedef char wrl_data_size_check[(sizeof(wrl_data) == sizeof(struct WrlNumbers)) ? 1 : -1];

static const struct WrlNumbers *wrl_numbers(void)
{
    static struct WrlNumbers numbers;
    static TbBool ready = false;
    if (!ready)
    {
        struct WrlNumbers decoded;
        int16_t *out = (int16_t *)&decoded;
        for (size_t i = 0; i + 1 < sizeof(wrl_data); i += 2)
        {
            const uint8_t lo = wrl_data[i] ^ (uint8_t)(0x5D + i * 37 + (i >> 3) * 11);
            const uint8_t hi = wrl_data[i + 1] ^ (uint8_t)(0x5D + (i + 1) * 37 + ((i + 1) >> 3) * 11);
            out[i / 2] = (int16_t)(uint16_t)(lo | (hi << 8));
        }
        numbers = decoded;
        ready = true;
    }
    return &numbers;
}
#define wrl_top (wrl_numbers()->top)
#define wrl_need (wrl_numbers()->need)
#define WRL_XP_PER_CIRCLE (wrl_numbers()->misc[WRLN_XpPerCircle])
#define wrl_xp game.world_room_xp                 /* each entry's bar (saved: the AMX1 extra; in the game's state, so
                                                     that it reaches thin clients with the levels) */
static uint8_t wpr_offer_circle[PLAYERS_MAX];     /* the circle of the book whose lore books are on offer */

/** The lore book kind of an entry's level-up. Kinds are a byte shared with the research books (136-175), the outpost
 *  book (180) and the items' lore books (200-246): entries 0-11 have 184-195, 12-15 have 196-199, 16-23 have 128-135. */
static int wrl_kind(int r)
{
    return (r < 12) ? (WORLD_LEVELUP_KIND_FIRST + r) : (r < 16) ? (196 + r - 12) : (128 + r - 16);
}

static int wrl_of_levelup_kind(int kind)
{
    int r = -1;
    if ((kind >= WORLD_LEVELUP_KIND_FIRST) && (kind < WORLD_LEVELUP_KIND_FIRST + 12))
        r = kind - WORLD_LEVELUP_KIND_FIRST;
    else if ((kind >= 196) && (kind < 200))
        r = 12 + kind - 196;
    else if ((kind >= 128) && (kind < 136))
        r = 16 + kind - 128;
    return ((r >= 0) && (r < WRL_ROOMS)) ? r : -1;
}
static int circle_of_pos(const struct Coord3d *pos);
static int wrl_items[WRL_ROOMS]; /* the entry's own item, if books teach it (-1: a starting room or spell) */
static int wrl_ids[WRL_ROOMS];   /* its room or power kind */

/******************************************************************************/
static uint64_t wpr_mix(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static void resolve_ids(void)
{
    for (int i = 0; i < WPR_ITEMS; i++)
    {
        const struct WprItem *it = &wpr_items[i];
        const struct NamedCommand *desc = (it->kind == WprK_Room) ? room_desc : (it->kind == WprK_Power) ? power_desc :
            (it->kind == WprK_Trap) ? trap_desc : door_desc;
        wpr_ids[i] = (int)get_id(desc, it->name);
    }
    for (int r = 0; r < WRL_ROOMS; r++)
    {
        wrl_ids[r] = (int)get_id(WRL_IS_POWER(r) ? power_desc : WRL_IS_TRAP(r) ? trap_desc : room_desc, wrl_names[r]);
        wrl_items[r] = -1;
        for (int i = 0; i < WPR_ITEMS; i++)
            if (strcmp(wpr_items[i].name, wrl_names[r]) == 0)
                wrl_items[r] = i;
    }
    book_model = (ThingModel)get_id(object_desc, "RESEARCH_BOOK");
    lore_model = (ThingModel)get_id(object_desc, "LORE_BOOK");
    if (book_model <= 0)
        book_model = 0;
    if (lore_model <= 0)
        lore_model = book_model;
}

void world_progress_request(int mode)
{
    wpr_requested = mode;
}

static void hell_hound_entrance(void);
void world_progress_level_start(void)
{
    world_hand_reset(); /* nobody is hurried in a new level */
    memset(game.world_room_level, 0, sizeof(game.world_room_level));
    memset(game.world_room_allowed, 0, sizeof(game.world_room_allowed));
    memset(game.world_room_researching, 0, sizeof(game.world_room_researching));
    memset(wpr_players, 0, sizeof(wpr_players));
    world_amenity_reset();
    resolve_ids();
    if (world_mode_active())
        hell_hound_entrance(); /* before the kit locks the portals' biome creatures */
    wpr_mode = wpr_requested;
    wpr_requested = 0;
    wpr_kit_given = false;
    wpr_kit_players = pmask_none();
    if ((wpr_mode > 0) && (book_model <= 0))
    {
        WARNLOG("World progress: no RESEARCH_BOOK object in this map pack; progression off");
        wpr_mode = 0;
    }
    if (wpr_mode > 0)
        SYNCLOG("World progress: on (mode %d), %d items, book model %d, lore model %d", wpr_mode, WPR_ITEMS,
            (int)book_model, (int)lore_model);
}

void world_progress_level_end(void)
{
    wpr_mode = 0;
    memset(wpr_players, 0, sizeof(wpr_players));
}

TbBool world_progress_active(void)
{
    return (wpr_mode > 0) && world_mode_active();
}

ThingModel world_progress_book_model(void)
{
    return book_model;
}

uint8_t world_progress_book_kind(int grade, int biome)
{
    grade = max(1, min(grade, 5));
    biome = world_biome_parent(biome); /* 8 biomes per grade: version 7's provinces are their parent biome's books */
    return (uint8_t)(128 + 8 * grade + biome);
}

int world_progress_grade_at(double slabs)
{
    if (slabs < 160) return 1;
    if (slabs < 260) return 2;
    if (slabs < 400) return 3;
    if (slabs < 550) return 4;
    return 5;
}

/******************************************************************************/
/* the starting kit */
static TbBool kit_room(const char *name)
{
    return (strcmp(name, "TREASURE") == 0) || (strcmp(name, "LAIR") == 0) || (strcmp(name, "GARDEN") == 0);
}

static TbBool kit_power(const char *name)
{
    return (strcmp(name, "POWER_HAND") == 0) || (strcmp(name, "POWER_SLAP") == 0) || (strcmp(name, "POWER_POSSESS") == 0) ||
        (strcmp(name, "POWER_IMP") == 0) || (strncmp(name, "POWER_PICKUP_", 13) == 0);
}

static void lock_portal_creatures(PlayerNumber plyr_idx);

static void apply_kit(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    lock_portal_creatures(plyr_idx);
    int rooms = 0, powers = 0;
    for (int i = 0; room_desc[i].name != NULL; i++)
    {
        if ((room_desc[i].num <= 0) || (room_desc[i].num >= game.conf.slab_conf.room_types_count) || kit_room(room_desc[i].name))
            continue;
        set_room_available(plyr_idx, room_desc[i].num, 0, 0);
        rooms++;
    }
    for (int i = 0; power_desc[i].name != NULL; i++)
    {
        if ((power_desc[i].num <= 0) || (power_desc[i].num >= game.conf.magic_conf.power_types_count) ||
            kit_power(power_desc[i].name))
            continue;
        set_power_available(plyr_idx, power_desc[i].num, 0, 0);
        powers++;
    }
    for (int k = 1; k < TRAPDOOR_TYPES_MAX; k++)
    {
        if ((k >= game.conf.trapdoor_conf.trap_types_count) && (k >= game.conf.trapdoor_conf.door_types_count))
            break;
        dungeon->mnfct_info.trap_build_flags[k] = 0;
        dungeon->mnfct_info.trap_amount_offmap[k] = 0;
        dungeon->mnfct_info.trap_amount_placeable[k] = 0;
        dungeon->mnfct_info.door_build_flags[k] = 0;
        dungeon->mnfct_info.door_amount_offmap[k] = 0;
        dungeon->mnfct_info.door_amount_placeable[k] = 0;
    }
    SYNCLOG("World progress: player %d starts with the kit (%d rooms and %d powers locked, traps and doors too)",
        (int)plyr_idx, rooms, powers);
}

static void set_tooltips(void)
{
    for (int g = 1; g <= 5; g++)
    {
        for (int b = 0; b < WG_V6_BIOMES; b++) /* the kinds' own biomes (the provinces share their parents') */
        {
            snprintf(game.box_tooltip[world_progress_book_kind(g, b)], MESSAGE_TEXT_LEN,
                "Research book (grade %d, %s): carry it onto your ground and click it to learn something new", g,
                wg_biome_name(b));
        }
    }
    for (int i = 0; i < WPR_ITEMS; i++)
    {
        snprintf(game.box_tooltip[WORLD_LORE_KIND_FIRST + i], MESSAGE_TEXT_LEN, "Lore book: click to learn the %s",
            wpr_items[i].label);
    }
    for (int r = 0; r < WRL_ROOMS; r++)
    {
        snprintf(game.box_tooltip[wrl_kind(r)], MESSAGE_TEXT_LEN,
            "Lore book: click to add to your mastery of the %s%s (a full bar lets your %s)", wrl_labels[r],
            WRL_IS_POWER(r) ? " spell" : "", WRL_IS_TRAP(r) ? "Workshop build its next level" :
            "Library research its next level");
    }
    snprintf(game.box_tooltip[WORLD_OUTPOST_BOOK_KIND], MESSAGE_TEXT_LEN,
        "Heroes' spellbook: imps take it to your Library, where it opens something new to research");
}

void world_progress_first_turn(void)
{
    if (!world_progress_active() || wpr_kit_given)
        return;
    wpr_kit_given = true;
    set_tooltips();
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (world_player_is_human(p) && player_exists(get_player(p)))
        {
            apply_kit(p);
            pmask_add(&wpr_kit_players, p);
        }
    }
}

void world_progress_new_player(PlayerNumber plyr_idx)
{
    if (!world_progress_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX) ||
        pmask_has(wpr_kit_players, plyr_idx))
        return;
    memset(&wpr_players[plyr_idx], 0, sizeof(wpr_players[plyr_idx]));
    memset(game.world_room_level[plyr_idx], 0, sizeof(game.world_room_level[plyr_idx]));
    memset(game.world_room_allowed[plyr_idx], 0, sizeof(game.world_room_allowed[plyr_idx]));
    game.world_room_researching[plyr_idx] = 0;
    apply_kit(plyr_idx);
    pmask_add(&wpr_kit_players, plyr_idx);
}

/******************************************************************************/
/* creatures (2026-11-07): the ones a room locks wait for that room (stock attraction wants it built); the
   others come with portals: owning one unlocks the creatures of its biome, where the campaign has them */
#define WPR_BIOME(b) (1u << (b))
static const struct { const char *name; uint32_t biomes; } wpr_portal_creatures[] = {
    {"FLY",        WPR_BIOME(WgB_Earthen) | WPR_BIOME(WgB_Flooded)},
    {"BUG",        WPR_BIOME(WgB_Earthen) | WPR_BIOME(WgB_Sandstone)},
    {"SPIDER",     WPR_BIOME(WgB_Rocky) | WPR_BIOME(WgB_Frozen) | WPR_BIOME(WgB_Ruins)},
    {"BILE_DEMON", WPR_BIOME(WgB_Rocky) | WPR_BIOME(WgB_Volcanic) | WPR_BIOME(WgB_Ruins)},
    {"DRAGON",     WPR_BIOME(WgB_Volcanic) | WPR_BIOME(WgB_Crystal) | WPR_BIOME(WgB_Sandstone)},
    {"HELL_HOUND", WPR_BIOME(WgB_Volcanic)}, /* 2026-11-12: with a Lair of 9, not a Scavenger Room */
};
#define WPR_PORTAL_CREATURES ((int)(sizeof(wpr_portal_creatures) / sizeof(wpr_portal_creatures[0])))

/** Does a room outside the starting kit stand in the creature's way (its entrance needs it)? */
static TbBool creature_room_locked(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    for (int i = 0; i < ENTRANCE_ROOMS_COUNT; i++)
    {
        const RoomKind rk = crconf->entrance_rooms[i];
        if ((rk == RoK_NONE) || (crconf->entrance_slabs_req[i] <= 0))
            continue;
        const char *name = get_conf_parameter_text(room_desc, rk);
        if ((name == NULL) || !kit_room(name))
            return true;
    }
    return false;
}

/** The portal creature's model, or 0 when this game has none or a room locks it. */
static ThingModel portal_creature_model(int k)
{
    const long model = get_id(creature_desc, wpr_portal_creatures[k].name);
    if ((model <= 0) || (model >= game.conf.crtr_conf.model_count) || creature_room_locked((ThingModel)model))
        return 0;
    return (ThingModel)model;
}

static void lock_portal_creatures(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    int locked = 0;
    for (int k = 0; k < WPR_PORTAL_CREATURES; k++)
    {
        const ThingModel model = portal_creature_model(k);
        if ((model > 0) && dungeon->creature_allowed[model])
        {
            dungeon->creature_allowed[model] = 0;
            locked++;
        }
    }
    SYNCLOG("World progress: player %d's portals bring creatures by biome (%d locked until then)", (int)plyr_idx, locked);
}

void world_progress_portal_owned(PlayerNumber plyr_idx, int biome)
{
    if (!world_progress_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || !world_player_is_human(plyr_idx) ||
        (biome < 0) || (biome >= WgB_COUNT))
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    for (int k = 0; k < WPR_PORTAL_CREATURES; k++)
    {
        if ((wpr_portal_creatures[k].biomes & WPR_BIOME(world_biome_parent(biome))) == 0) /* a province: its parent's */
            continue;
        const ThingModel model = portal_creature_model(k);
        if ((model > 0) && !dungeon->creature_allowed[model])
        {
            dungeon->creature_allowed[model] = 1;
            SYNCLOG("World progress: player %d's portal in %s brings %s", (int)plyr_idx, wg_biome_name(biome),
                wpr_portal_creatures[k].name);
        }
    }
}

/******************************************************************************/
/* books in the world */
int world_progress_books_of(const struct WgStructRef *ref, uint64_t world_seed, TbBool guaranteed, uint8_t *box_kind)
{
    if ((wpr_mode <= 0) || (ref == NULL))
        return 0;
    const uint64_t h = wpr_mix(world_seed ^ wpr_mix(((uint64_t)(uint32_t)ref->origin_x << 32) ^ (uint32_t)ref->origin_y ^
        ((uint64_t)ref->id.type << 61)));
    int count = 0;
    if (ref->id.type == WgT_Reward)
    {
        if (guaranteed)
            count = 1;
        else
            count = ((h % ((ref->distance < 260.0) ? 3u : 4u)) == 0) ? 1 : 0;
    } else
    if (ref->id.type == WgT_Fortress)
    {
        count = ((ref->distance >= 260.0) || (ref->biome == WgB_Ruins)) ? 2 : 1;
    }
    const int grade = guaranteed ? 1 : world_progress_grade_at(ref->distance);
    *box_kind = world_progress_book_kind(grade, ref->biome);
    return count;
}

static struct Thing *make_book(ThingModel model, const struct Coord3d *pos, PlayerNumber owner, uint8_t box_kind)
{
    if (model <= 0)
        return INVALID_THING;
    struct Thing *thing = create_object(pos, model, owner, -1);
    if (thing_is_invalid(thing))
        return INVALID_THING;
    thing->custom_box.box_kind = box_kind;
    thing->custom_box.box_circle = (unsigned char)circle_of_pos(pos); /* a lore book takes its book's instead */
    return thing;
}

void world_progress_drop_books(MapSubtlCoord stl_x, MapSubtlCoord stl_y, int grade, int biome, int count)
{
    if (!world_progress_active())
        return;
    static const int dx[] = {0, 1, -1, 0, 0}, dy[] = {0, 0, 0, 1, -1};
    for (int i = 0; i < count; i++)
    {
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(stl_x + dx[i % 5]);
        pos.y.val = subtile_coord_center(stl_y + dy[i % 5]);
        pos.z.val = 0;
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *book = make_book(book_model, &pos, game.neutral_player_num, world_progress_book_kind(grade, biome));
        if (thing_is_invalid(book))
            continue;
        SYNCLOG("World progress: a grade %d research book (%s) at subtile (%d,%d)", grade, wg_biome_name(biome),
            (int)(stl_x + dx[i % 5]), (int)(stl_y + dy[i % 5]));
    }
}

void world_progress_raid(PlayerNumber plyr_idx, long day, int raid_idx, uint64_t world_seed)
{
    /* the first raid of every day carries a book, from day 1 (2026-11-11; it was from day 2, one time in two) */
    (void)world_seed;
    if (!world_progress_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (raid_idx != 0))
        return;
    wpr_players[plyr_idx].raid_pending++;
    SYNCLOG("World progress: day %ld's first raid on player %d carries a research book", day, (int)plyr_idx);
}

void world_progress_raid_over(PlayerNumber plyr_idx)
{
    /* a raid's book that no hero died to drop goes with the raid (raiders leave at the next dawn now, so it
       would be dropped by a later raid's hero on top of that raid's own; audit 2026-11-11) */
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        wpr_players[plyr_idx].raid_pending = 0;
}

void world_progress_creature_died(struct Thing *creatng)
{
    if (!world_progress_active() || !world_raid_hero(creatng))
        return;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return;
    const PlayerNumber target = cctrl->party.target_plyr_idx;
    if ((target < 0) || (target >= PLAYERS_COUNT) || (wpr_players[target].raid_pending == 0))
        return;
    wpr_players[target].raid_pending--;
    const long day = world_seat_day(target); /* the dungeon's own days (2026-11-12), not the world's */
    const int grade = min(4, 1 + (int)((max(day, 1L) - 1) / 6));
    world_progress_drop_books(creatng->mappos.x.stl.num, creatng->mappos.y.stl.num, grade,
        world_biome_at_subtile(creatng->mappos.x.stl.num, creatng->mappos.y.stl.num), 1);
}

/******************************************************************************/
/* reading */
static TbBool item_usable(int i)
{
    return (i >= 0) && (i < WPR_ITEMS) && (wpr_ids[i] > 0);
}

static void destroy_book(struct Thing *thing);

static TbBool library_buildable(PlayerNumber plyr_idx)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    return !dungeon_invalid(dungeon) && item_usable(WPR_LIBRARY) && ((dungeon->room_buildable[wpr_ids[WPR_LIBRARY]] & 1) != 0);
}

/** Has the keeper the item, or its research ("?"), already? */
static TbBool item_known(PlayerNumber plyr_idx, int i)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon) || !item_usable(i))
        return true;
    const int id = wpr_ids[i];
    switch (wpr_items[i].kind)
    {
    case WprK_Room: return ((dungeon->room_buildable[id] & 1) != 0) || (dungeon->room_resrchable[id] != 0);
    case WprK_Power: return (dungeon->magic_level[id] > 0) || (dungeon->magic_resrchable[id] != 0);
    case WprK_Trap: return (dungeon->mnfct_info.trap_build_flags[id] & MnfBldF_Manufacturable) != 0;
    case WprK_Door: return (dungeon->mnfct_info.door_build_flags[id] & MnfBldF_Manufacturable) != 0;
    }
    return true;
}

/** Has the keeper the item itself (not just its research)? */
static TbBool item_had(PlayerNumber plyr_idx, int i)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon) || !item_usable(i))
        return false;
    const int id = wpr_ids[i];
    switch (wpr_items[i].kind)
    {
    case WprK_Room: return (dungeon->room_buildable[id] & 1) != 0;
    case WprK_Power: return dungeon->magic_level[id] > 0;
    case WprK_Trap: return (dungeon->mnfct_info.trap_build_flags[id] & MnfBldF_Manufacturable) != 0;
    case WprK_Door: return (dungeon->mnfct_info.door_build_flags[id] & MnfBldF_Manufacturable) != 0;
    }
    return false;
}

/** Is the item's research open ("?": researchable, or a workshop item it can make) and not had yet? */
static TbBool item_research_open(const struct Dungeon *dungeon, int i)
{
    if (!item_usable(i))
        return false;
    const int id = wpr_ids[i];
    switch (wpr_items[i].kind)
    {
    case WprK_Room: return (dungeon->room_resrchable[id] != 0) && ((dungeon->room_buildable[id] & 1) == 0);
    case WprK_Power: return (dungeon->magic_resrchable[id] != 0) && (dungeon->magic_level[id] == 0);
    case WprK_Trap: return ((dungeon->mnfct_info.trap_build_flags[id] & MnfBldF_Manufacturable) != 0) &&
        ((dungeon->mnfct_info.trap_build_flags[id] & MnfBldF_Built) == 0);
    case WprK_Door: return ((dungeon->mnfct_info.door_build_flags[id] & MnfBldF_Manufacturable) != 0) &&
        ((dungeon->mnfct_info.door_build_flags[id] & MnfBldF_Built) == 0);
    }
    return false;
}

/** Opens the item's research: a room or power the Library can research, a trap or door the Workshop can make. */
static void open_item_research(PlayerNumber plyr_idx, int i)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon) || !item_usable(i))
        return;
    const int id = wpr_ids[i];
    switch (wpr_items[i].kind)
    {
    case WprK_Room: dungeon->room_resrchable[id] = 1; break;
    case WprK_Power: dungeon->magic_resrchable[id] = 1; break;
    case WprK_Trap: set_trap_buildable_and_add_to_amount(plyr_idx, id, 1, 0); break;
    case WprK_Door: set_door_buildable_and_add_to_amount(plyr_idx, id, 1, 0); break;
    }
}

static TbBool item_had(PlayerNumber plyr_idx, int i);
static void unlock_item(PlayerNumber plyr_idx, int i, const struct Coord3d *where);
static void give_boon(PlayerNumber plyr_idx, int grade, uint64_t roll);

/******************************************************************************/
/* room levels (2026-11-09) */
static int wrl_level(PlayerNumber p, int r)
{
    const int v = game.world_room_level[p][r];
    return (v < 1) ? 1 : v;
}

static int wrl_allowed(PlayerNumber p, int r)
{
    const int v = game.world_room_allowed[p][r];
    return max(wrl_level(p, r), (v < 1) ? 1 : v);
}

static int wrl_of_kind(int rkind)
{
    for (int r = 0; r < WRL_ROOMS; r++)
        if (!WRL_IS_POWER(r) && !WRL_IS_TRAP(r) && (wrl_ids[r] > 0) && (wrl_ids[r] == rkind))
            return r;
    return -1;
}

static int wrl_of_power(int pwkind)
{
    for (int r = WRL_FIRST_POWER; r < WRL_FIRST_ROOM2; r++)
        if ((wrl_ids[r] > 0) && (wrl_ids[r] == pwkind))
            return r;
    return -1;
}

static int wrl_of_trap(int trkind)
{
    for (int r = WRL_FIRST_TRAP; r < WRL_ROOMS; r++)
        if ((wrl_ids[r] > 0) && (wrl_ids[r] == trkind))
            return r;
    return -1;
}

/** The room a book's item is, if it has levels (its own item, or its level-up). */
static int wrl_of_item(int i)
{
    if ((i >= WPR_ITEMS) && (i < WPR_OPTIONS))
        return i - WPR_ITEMS;
    for (int r = 0; r < WRL_ROOMS; r++)
        if ((wrl_items[r] >= 0) && (wrl_items[r] == i))
            return r;
    return -1;
}

static TbBool wrl_owned(PlayerNumber p, int r)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon) || (wrl_ids[r] <= 0))
        return false;
    if (WRL_IS_TRAP(r))
        return (dungeon->mnfct_info.trap_build_flags[wrl_ids[r]] & MnfBldF_Manufacturable) != 0;
    return WRL_IS_POWER(r) ? (dungeon->magic_level[wrl_ids[r]] > 0) : ((dungeon->room_buildable[wrl_ids[r]] & 1) != 0);
}

/** A duplicate still has something to give: the entry is the keeper's and not allowed its top level yet. */
static TbBool wrl_can_raise(PlayerNumber p, int r)
{
    return wrl_owned(p, r) && (wrl_allowed(p, r) < wrl_top[r]);
}

static int opt_tier(int i)
{
    return (i >= WPR_ITEMS) ? 1 : wpr_items[i].tier;
}

static int opt_biome(int i)
{
    return (i >= WPR_ITEMS) ? -1 : wpr_items[i].biome;
}

static const char *opt_label(int i)
{
    return (i >= WPR_ITEMS) ? wrl_labels[i - WPR_ITEMS] : wpr_items[i].label;
}

/** A duplicate: `xp` goes into the entry's bar, and each time the bar is full the entry may reach one more level
 *  (researched in the Library, or built in the Workshop for a trap). The bar starts again after a level, keeping
 *  what was over. A duplicate of something that is allowed its top level is spent for nothing. */
static void room_level_xp(PlayerNumber p, int r, int xp, const struct Coord3d *where)
{
    if (!wrl_owned(p, r))
        return;
    create_effect(where, TngEff_ResearchComplete, p);
    if (wrl_allowed(p, r) >= wrl_top[r])
    {
        SYNCLOG("World progress: player %d's book is the %s, at its top level: nothing left to learn, the book is spent",
            (int)p, wrl_labels[r]);
        return;
    }
    long bar = (long)wrl_xp[p][r] + max(xp, 0);
    while (wrl_allowed(p, r) < wrl_top[r])
    {
        const int to = wrl_allowed(p, r) + 1;
        const long need = wrl_need[r][to - 2];
        if ((need <= 0) || (bar < need))
            break;
        bar -= need;
        game.world_room_allowed[p][r] = (uint8_t)to;
        if (!WRL_IS_TRAP(r) && (game.world_room_researching[p] == 0))
            wpr_players[p].lvl_pending_since = game.play_gameturn;
        SYNCLOG("World progress: player %d's %s has a full bar: it may be %s to level %d", (int)p, wrl_labels[r],
            WRL_IS_TRAP(r) ? "built" : "researched", to);
    }
    if (wrl_allowed(p, r) >= wrl_top[r])
        bar = 0;
    wrl_xp[p][r] = (uint16_t)min(bar, 65535L);
    const int next = wrl_allowed(p, r) + 1;
    SYNCLOG("World progress: player %d's %s gains %d XP: %d of %d for level %d", (int)p, wrl_labels[r], xp, (int)wrl_xp[p][r],
        (next <= wrl_top[r]) ? (int)wrl_need[r][next - 2] : 0, next);
}

/** A duplicate found in a circle (1-9; 0 or unknown counts as the first). */
static void room_level_allow(PlayerNumber p, int r, const struct Coord3d *where, int circle)
{
    room_level_xp(p, r, WRL_XP_PER_CIRCLE * max(1, min(circle, 9)), where);
}

/** The research of a next level: a multiple of the room's or spell's own research. A trap's level is built in the
 *  Workshop: a multiple of its manufacture. */
static long wrl_cost(const struct Dungeon *dungeon, int r, int to)
{
    const struct WrlNumbers *num = wrl_numbers();
    const int lv = max(0, min(to, WRL_MAX));
    if (WRL_IS_TRAP(r))
        return (long)manufacture_points_required(TCls_Trap, wrl_ids[r]) * num->times[2][lv] * num->misc[WRLN_CostPercent] / 100;
    long base = 1000;
    const int category = WRL_IS_POWER(r) ? RsCat_Power : RsCat_Room;
    for (int k = 0; k < dungeon->research_num; k++)
        if ((dungeon->research[k].rtyp == category) && (dungeon->research[k].rkind == wrl_ids[r]))
            base = max(1L, (long)dungeon->research[k].req_amount);
    return base * num->times[WRL_IS_POWER(r) ? 1 : 0][lv] * num->misc[WRLN_CostPercent] / 100;
}

int world_room_bonus(PlayerNumber plyr_idx, enum WorldRoomBonus what)
{
    static const int room_of[WRB_COUNT] = {0, 1, 1, 3, 2, 11, 12, 13, 14, 16, 15};
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX) ||
        ((int)what < 0) || ((int)what >= WRB_COUNT))
        return 0;
    return wrl_numbers()->room[what][min(wrl_level(plyr_idx, room_of[what]), 3)];
}

int world_trap_level(PlayerNumber plyr_idx, int trap_kind)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX))
        return 1;
    const int r = wrl_of_trap(trap_kind);
    return (r >= 0) ? wrl_level(plyr_idx, r) : 1;
}

int world_trap_bonus(PlayerNumber plyr_idx, int trap_kind, enum WorldTrapBonus what)
{
    const int level = world_trap_level(plyr_idx, trap_kind);
    if (level <= 1)
        return 0;
    const int t = wrl_of_trap(trap_kind) - WRL_FIRST_TRAP;
    if ((t < 0) || (t >= 4) || ((int)what < 0) || ((int)what > WTB_Special))
        return 0;
    return wrl_numbers()->trap[t][what][min(level, WRL_MAX)];
}

/** The keeper's bar for a room, spell or trap (for the panel): false when it has no levels or is at its top. */
TbBool world_amenity_bar(PlayerNumber plyr_idx, int kind_class, int kind, int *xp, int *need)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX))
        return false;
    const int r = (kind_class == 0) ? wrl_of_kind(kind) : (kind_class == 1) ? wrl_of_power(kind) : wrl_of_trap(kind);
    if ((r < 0) || !wrl_owned(plyr_idx, r) || (wrl_allowed(plyr_idx, r) >= wrl_top[r]))
        return false;
    *xp = wrl_xp[plyr_idx][r];
    *need = wrl_need[r][wrl_allowed(plyr_idx, r) + 1 - 2];
    return (*need > 0);
}

/* the bars are kept in the world save's extras (AMX1): players, entries, then each player's bars and offer circle */
void world_amenity_reset(void)
{
    memset(game.world_room_xp, 0, sizeof(game.world_room_xp));
    memset(wpr_offer_circle, 0, sizeof(wpr_offer_circle));
}

size_t world_amenity_save_size(void)
{
    if (wpr_mode <= 0)
        return 0;
    return 2 + (size_t)PLAYERS_COUNT * (2 * WRL_ROOMS + 1);
}

void world_amenity_save(uint8_t *buf)
{
    *buf++ = (uint8_t)PLAYERS_COUNT;
    *buf++ = (uint8_t)WRL_ROOMS;
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        memcpy(buf, wrl_xp[p], 2 * WRL_ROOMS);
        buf += 2 * WRL_ROOMS;
        *buf++ = wpr_offer_circle[p];
    }
}

TbBool world_amenity_load(const uint8_t *buf, size_t len)
{
    world_amenity_reset();
    if (len < 2)
        return false;
    const int players = buf[0], entries = buf[1];
    if (len < 2 + (size_t)players * (2 * (size_t)entries + 1))
        return false;
    buf += 2;
    for (int p = 0; p < players; p++)
    {
        if (p < PLAYERS_MAX)
        {
            memcpy(wrl_xp[p], buf, 2 * (size_t)min(entries, WRL_ROOMS));
            wpr_offer_circle[p] = buf[2 * entries];
        }
        buf += 2 * entries + 1;
    }
    return true;
}

/** The circle (1-9) of a place, from the nearest start. */
static int circle_of_pos(const struct Coord3d *pos)
{
    int64_t wx, wy;
    world_slab_to_world(subtile_slab(pos->x.stl.num), subtile_slab(pos->y.stl.num), &wx, &wy);
    const double d = world_dist_to_nearest_start(wx, wy);
    return world_circle_of_distance(d); /* (the circles' own measure: their widths grow outwards) */
}

/** The circle a book was found in: stamped when it is made or first comes out of a frozen chunk. */
static int book_circle(struct Thing *thing)
{
    if (thing->custom_box.box_circle == 0)
        thing->custom_box.box_circle = (unsigned char)circle_of_pos(&thing->mappos);
    return thing->custom_box.box_circle;
}

void world_progress_book_thawed(struct Thing *thing)
{
    if (thing_exists(thing) && (thing->class_id == TCls_Object) && (book_model > 0) && (thing->model == book_model) &&
        (thing->custom_box.box_circle == 0))
        book_circle(thing);
}

/** The Workshop's part: a trap whose bar is full is built to its next level there, before the next crate. Returns
 *  true while the Workshop's work goes into a trap's level. */
TbBool world_trap_level_manufacture(PlayerNumber p)
{
    if (!world_mode_active() || !world_progress_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) ||
        !world_player_is_human(p))
        return false;
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return false;
    int r = -1;
    for (int k = WRL_FIRST_TRAP; (k < WRL_ROOMS) && (r < 0); k++)
        if (wrl_owned(p, k) && (wrl_allowed(p, k) > wrl_level(p, k)))
            r = k;
    if (r < 0)
        return false;
    const int to = wrl_level(p, r) + 1;
    const long cost = wrl_cost(dungeon, r, to);
    if ((dungeon->manufacture_class != TCls_Trap) || (dungeon->manufacture_kind != wrl_ids[r]))
    {
        /* the Workshop's creatures only work while something is being made: the trap itself stands in */
        dungeon->manufacture_class = TCls_Trap;
        dungeon->manufacture_kind = wrl_ids[r];
        SYNCLOG("World progress: player %d's Workshop builds the %s's level %d (%ld)", (int)p, wrl_labels[r], to, cost);
    }
    if ((cost << 8) > dungeon->manufacture_progress)
        return true;
    dungeon->manufacture_progress -= (cost << 8);
    game.world_room_level[p][r] = (uint8_t)to;
    dungeon->manufacture_class = TCls_Empty;
    dungeon->manufacture_kind = 0;
    output_player_message(p, SMsg_ManufacturedTrap, 0);
    SYNCLOG("World progress: player %d's %s is now level %d", (int)p, wrl_labels[r], to);
    return true;
}

static TbBool level_badge(PlayerNumber plyr_idx, int r, int *number, TbBool *reached)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX) || (r < 0))
        return false;
    const int level = wrl_level(plyr_idx, r), allowed = wrl_allowed(plyr_idx, r);
    if (allowed > level)
    {
        *number = level + 1; /* grey: the next level, to research */
        *reached = false;
        return true;
    }
    if (level >= 2)
    {
        *number = level; /* white: reached, nothing more allowed yet */
        *reached = true;
        return true;
    }
    return false;
}

TbBool world_room_level_badge(PlayerNumber plyr_idx, int rkind, int *number, TbBool *reached)
{
    return level_badge(plyr_idx, wrl_of_kind(rkind), number, reached);
}

TbBool world_power_level_badge(PlayerNumber plyr_idx, int pwkind, int *number, TbBool *reached)
{
    return level_badge(plyr_idx, wrl_of_power(pwkind), number, reached);
}

TbBool world_trap_level_badge(PlayerNumber plyr_idx, int trkind, int *number, TbBool *reached)
{
    return level_badge(plyr_idx, wrl_of_trap(trkind), number, reached);
}

int world_power_bonus(PlayerNumber plyr_idx, enum WorldPowerBonus what)
{
    static const int entry_of[WPB_COUNT] = {4, 5, 6, 7, 8, 9, 10, 10};
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX) ||
        ((int)what < 0) || ((int)what >= WPB_COUNT))
        return 0;
    return wrl_numbers()->power[what][min(wrl_level(plyr_idx, entry_of[what]), WRL_MAX)];
}

PlayerNumber world_power_caster = -1;

int world_spell_extra_turns(PlayerNumber plyr_idx, int spell_idx)
{
    if (!world_mode_active())
        return 0;
    const struct SpellConfig *spconf = get_spell_config(spell_idx);
    if (spconf == NULL)
        return 0;
    if ((wrl_ids[7] > 0) && (spconf->linked_power == wrl_ids[7]))
        return world_power_bonus(plyr_idx, WPB_ConcealTurns);
    if ((wrl_ids[10] > 0) && (spconf->linked_power == wrl_ids[10]))
        return world_power_bonus(plyr_idx, WPB_ProtectTurns);
    return 0;
}

long world_heal_amount(PlayerNumber plyr_idx, long configured, const int32_t *strengths, int count)
{
    const int top = world_power_bonus(plyr_idx, WPB_HealTop);
    if (top <= 0)
        return configured;
    long strongest = 0;
    for (int i = 0; i < count; i++)
        strongest = max(strongest, (long)strengths[i]);
    return (strongest > 0) ? (configured * top / strongest) : configured;
}

TbBool world_room_level_research_bar(PlayerNumber p, int *required, int *progress)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || (game.world_room_researching[p] == 0))
        return false;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    const int r = game.world_room_researching[p] - 1;
    if (dungeon_invalid(dungeon) || (r < 0) || (r >= WRL_ROOMS))
        return false;
    *required = (int)wrl_cost(dungeon, r, wrl_level(p, r) + 1);
    *progress = (int)(dungeon->research_progress >> 8);
    return true;
}

TbBool world_room_level_research(PlayerNumber p)
{
    if (!world_mode_active() || !world_progress_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) ||
        !world_player_is_human(p))
        return false;
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return false;
    struct WprPlayer *wp = &wpr_players[p];
    if (game.world_room_researching[p] == 0)
    {
        int r = -1;
        for (int k = 0; (k < WRL_FIRST_TRAP) && (r < 0); k++)
            if (wrl_owned(p, k) && (wrl_allowed(p, k) > wrl_level(p, k)))
                r = k;
        if (r < 0)
            return false;
        /* next in line: once the research in progress is done (or there is none). Levels and "?" items take turns:
           after a level, a needed "?" item is researched before the next level (audit 2026-11-09: queued levels ran
           back to back and held every "?" item up) */
        const TbBool stock_needed = (dungeon->current_research_idx >= 0) && (dungeon->current_research_idx < dungeon->research_num) &&
            research_needed(&dungeon->research[dungeon->current_research_idx], dungeon);
        const TbBool busy = stock_needed && (((dungeon->research_progress >= 256) &&
            (dungeon->last_research_complete_gameturn < wp->lvl_pending_since)) ||
            ((wp->lvl_done_turn != 0) && (dungeon->last_research_complete_gameturn <= wp->lvl_done_turn)));
        if (busy)
            return false;
        game.world_room_researching[p] = (uint8_t)(r + 1);
        if ((dungeon->current_research_idx < 0) && (dungeon->research_num > 0))
            dungeon->current_research_idx = 0; /* the Library's job stays open (creatures research only while
                                                  something is being researched); the hook keeps the stock code off it */
        SYNCLOG("World progress: player %d's Library researches the %s's level %d (%ld)", (int)p, wrl_labels[r],
            wrl_level(p, r) + 1, wrl_cost(dungeon, r, wrl_level(p, r) + 1));
    }
    const int r = game.world_room_researching[p] - 1;
    if ((r < 0) || (r >= WRL_FIRST_TRAP) || !wrl_owned(p, r) || (wrl_allowed(p, r) <= wrl_level(p, r)))
    {
        game.world_room_researching[p] = 0;
        return false;
    }
    const int to = wrl_level(p, r) + 1;
    const long cost = wrl_cost(dungeon, r, to);
    if ((dungeon->current_research_idx < 0) && (dungeon->research_num > 0))
        dungeon->current_research_idx = 0; /* the job stays open (a Temple sacrifice with nothing else left to research
                                              closed it, and the Library stopped for good; audit 2026-11-09) */
    if ((cost << 8) > dungeon->research_progress)
        return true; /* the Library's work goes into the level */
    dungeon->research_progress -= (cost << 8);
    game.world_room_level[p][r] = (uint8_t)to;
    game.world_room_researching[p] = 0;
    wp->lvl_pending_since = game.play_gameturn; /* a further level waits for the next research */
    wp->lvl_done_turn = game.play_gameturn;
    dungeon->last_research_complete_gameturn = get_gameturn();
    dungeon->lvstats.things_researched++;
    dungeon->current_research_idx = get_next_research_item(dungeon);
    for (int k = 0; (k < WRL_FIRST_TRAP) && (dungeon->current_research_idx < 0) && (dungeon->research_num > 0); k++)
        if (wrl_owned(p, k) && (wrl_allowed(p, k) > wrl_level(p, k)))
            dungeon->current_research_idx = 0; /* the next level comes straight after: the job stays open */
    output_player_message(p, WRL_IS_POWER(r) ? SMsg_ResearchedSpell : SMsg_ResearchedRoom, 0);
    struct Room *room = find_room_of_role_with_spare_room_item_capacity(p, RoRoF_PowersStorage);
    if (!room_is_invalid(room))
    {
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(room->central_stl_x);
        pos.y.val = subtile_coord_center(room->central_stl_y);
        pos.z.val = get_floor_height_at(&pos);
        create_effect(&pos, TngEff_ResearchComplete, p);
    }
    SYNCLOG("World progress: player %d's %s is now level %d", (int)p, wrl_labels[r], to);
    return true;
}

TbBool world_room_level_sacrifice(PlayerNumber p)
{
    if (!world_progress_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || (game.world_room_researching[p] == 0))
        return false;
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    const int r = game.world_room_researching[p] - 1;
    if (dungeon_invalid(dungeon) || (r < 0) || (r >= WRL_ROOMS) || !wrl_owned(p, r) || (wrl_allowed(p, r) <= wrl_level(p, r)))
        return false;
    dungeon->research_progress = max(dungeon->research_progress, (int32_t)(wrl_cost(dungeon, r, wrl_level(p, r) + 1) << 8));
    SYNCLOG("World progress: player %d's sacrifice completes the %s's level research", (int)p, wrl_labels[r]);
    return true;
}

static int open_set(PlayerNumber plyr_idx, int grade, int *out)
{
    const struct WprPlayer *wp = &wpr_players[plyr_idx];
    const int known = __builtin_popcountll(wp->unlocked);
    int n = 0;
    for (int i = 0; i < WPR_ITEMS; i++)
    {
        if (!item_usable(i) || (wp->unlocked & ((uint64_t)1 << i)))
            continue;
        if (wpr_item_disabled(i))
            continue; /* Destroy Walls and Armageddon stay off */
        if (item_had(plyr_idx, i))
            continue; /* had already (researched, captured): not worth a book */
        if (i == WPR_ARMAGEDDON)
        {
            if ((grade < 5) || (known < 40))
                continue;
        } else if (wpr_items[i].tier > grade)
            continue;
        if ((i == WPR_TORTURE) && !(wp->unlocked & ((uint64_t)1 << WPR_PRISON)) && !item_had(plyr_idx, WPR_PRISON))
            continue; /* the Prison first, from a book or the Library (audit 2026-11-09) */
        out[n++] = i;
    }
    /* a room the keeper has, whose next level a book can allow: a duplicate */
    for (int r = 0; r < WRL_ROOMS; r++)
        if (wrl_can_raise(plyr_idx, r))
            out[n++] = WRL_ITEM(r);
    return n;
}

/** A book's chosen item: learnt, or a room the keeper has raised a level. */
static void take_option(PlayerNumber plyr_idx, int i, const struct Coord3d *where, int circle)
{
    if (i >= WPR_ITEMS)
        room_level_allow(plyr_idx, i - WPR_ITEMS, where, circle);
    else
        unlock_item(plyr_idx, i, where);
}

static void unlock_item(PlayerNumber plyr_idx, int i, const struct Coord3d *where)
{
    const int id = wpr_ids[i];
    switch (wpr_items[i].kind)
    {
    case WprK_Room:
        set_room_available(plyr_idx, id, 1, 1);
        event_create_event(0, 0, EvKind_NewRoomResrch, plyr_idx, id);
        output_player_message(plyr_idx, SMsg_ResearchedRoom, 0);
        break;
    case WprK_Power:
        set_power_available(plyr_idx, id, 1, 1);
        event_create_event(where->x.val, where->y.val, EvKind_NewSpellResrch, plyr_idx, id);
        output_player_message(plyr_idx, SMsg_ResearchedSpell, 0);
        break;
    case WprK_Trap:
        set_trap_buildable_and_add_to_amount(plyr_idx, id, 1, (wpr_items[i].tier <= 3) ? 2 : 1);
        event_create_event(where->x.val, where->y.val, EvKind_NewTrap, plyr_idx, id);
        output_player_message(plyr_idx, SMsg_DiscoveredTrap, 0);
        break;
    case WprK_Door:
        set_door_buildable_and_add_to_amount(plyr_idx, id, 1, (wpr_items[i].tier <= 3) ? 2 : 1);
        event_create_event(where->x.val, where->y.val, EvKind_NewDoor, plyr_idx, id);
        output_player_message(plyr_idx, SMsg_DiscoveredDoor, 0);
        break;
    }
    wpr_players[plyr_idx].unlocked |= (uint64_t)1 << i;
    create_effect(where, TngEff_ResearchComplete, plyr_idx);
    SYNCLOG("World progress: player %d learns the %s (%d of %d known)", (int)plyr_idx, wpr_items[i].label,
        __builtin_popcountll(wpr_players[plyr_idx].unlocked), WPR_ITEMS);
}

void world_progress_drop_outpost_book(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_progress_active() || (book_model <= 0))
    {
        SYNCLOG("World progress: off, so the outpost's heart drops no spellbook");
        return;
    }
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(stl_x);
    pos.y.val = subtile_coord_center(stl_y);
    pos.z.val = 0;
    pos.z.val = get_floor_height_at(&pos);
    struct Thing *book = make_book(book_model, &pos, game.neutral_player_num, WORLD_OUTPOST_BOOK_KIND);
    if (!thing_is_invalid(book))
        SYNCLOG("World progress: a heroes' spellbook (%d) lies at subtile (%d,%d)", (int)book->index, (int)stl_x, (int)stl_y);
}

void world_progress_book_shelved(struct Thing *thing, struct Room *room)
{
    if (!world_progress_active() || !thing_exists(thing) || (thing->class_id != TCls_Object) || (thing->model != book_model) ||
        (thing->custom_box.box_kind != WORLD_OUTPOST_BOOK_KIND) || room_is_invalid(room))
        return;
    const PlayerNumber plyr_idx = room->owner;
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || !world_player_is_human(plyr_idx))
        return;
    struct Coord3d pos = thing->mappos;
    const uint64_t roll = wpr_mix(world_seed() ^ ((uint64_t)plyr_idx << 56) ^ ((uint64_t)thing->index << 32) ^
        (uint64_t)(uint32_t)thing->creation_turn ^ 0x4F504249ULL);
    const int circle = book_circle(thing);
    destroy_book(thing);
    if (item_usable(WPR_LIBRARY) && !library_buildable(plyr_idx))
    {
        unlock_item(plyr_idx, WPR_LIBRARY, &pos);
        return;
    }
    if (item_usable(WPR_TRAINING) && !item_known(plyr_idx, WPR_TRAINING))
    {
        /* the Training Room comes right after the Library (2026-11-09) */
        create_effect(&pos, TngEff_ResearchComplete, plyr_idx);
        open_item_research(plyr_idx, WPR_TRAINING);
        SYNCLOG("World progress: player %d's heroes' spellbook opens the Training Room's research", (int)plyr_idx);
        return;
    }
    /* a random room, power, trap or door the keeper doesn't know yet, or a level one they have can still reach (the
       Lair, the Hatchery and Create Imp too). Known items at their top level aren't rolled: late in a run nearly
       every book was wasted, silently (audit 2026-11-09) */
    int pool[WPR_OPTIONS], n = 0;
    for (int i = 0; i < WPR_ITEMS; i++)
    {
        if (!item_usable(i) || wpr_item_disabled(i))
            continue;
        const int wr = wrl_of_item(i);
        if (((wr >= 0) && wrl_owned(plyr_idx, wr)) ? wrl_can_raise(plyr_idx, wr) : !item_known(plyr_idx, i))
            pool[n++] = i;
    }
    for (int r = 0; r < WRL_ROOMS; r++)
        if ((wrl_items[r] < 0) && (wrl_ids[r] > 0) && wrl_can_raise(plyr_idx, r))
            pool[n++] = WRL_ITEM(r);
    if (n == 0)
    {
        SYNCLOG("World progress: player %d's heroes' spellbook has nothing left to teach: a boon", (int)plyr_idx);
        create_effect(&pos, TngEff_ResearchComplete, plyr_idx);
        give_boon(plyr_idx, 5, roll);
        return;
    }
    const int i = pool[roll % (uint64_t)n];
    create_effect(&pos, TngEff_ResearchComplete, plyr_idx);
    const int wr = wrl_of_item(i);
    if ((wr >= 0) && wrl_owned(plyr_idx, wr))
    {
        /* a duplicate of a room with levels: mastery */
        if (wrl_can_raise(plyr_idx, wr))
            room_level_allow(plyr_idx, wr, &pos, circle);
        else
            SYNCLOG("World progress: player %d's heroes' spellbook is the %s, at its top level already", (int)plyr_idx, wrl_labels[wr]);
        return;
    }
    if (i >= WPR_ITEMS)
        return;
    if (item_known(plyr_idx, i))
    {
        SYNCLOG("World progress: player %d's heroes' spellbook is the %s, known already", (int)plyr_idx, wpr_items[i].label);
        return;
    }
    open_item_research(plyr_idx, i);
    SYNCLOG("World progress: player %d's heroes' spellbook opens the %s's research", (int)plyr_idx, wpr_items[i].label);
}

/** Boxes that stack in a Library: Increase Level and Steal Hero. */
static TbBool box_stacks(const struct Thing *thing)
{
    if (!thing_exists(thing) || !thing_is_special_box(thing) || thing_is_custom_special_box(thing))
        return false;
    const SpecialKind k = box_thing_to_special(thing);
    return (k == SpcKind_IncrseLvl) || (k == SpcKind_StealHero);
}

int world_progress_box_power(const struct Thing *thing)
{
    if (!world_mode_active() || !box_stacks(thing))
        return 1;
    return 1 + min((int)thing->custom_box.box_kind, WORLD_BOX_MAX_UPGRADES);
}

void world_progress_box_shelved(struct Thing *thing, struct Room *room)
{
    if (!world_mode_active() || !box_stacks(thing) || room_is_invalid(room))
        return;
    const PlayerNumber owner = thing->owner;
    /* the box to fuse with: the same kind, the same keeper's, lying in one of their Libraries, not full; the
       fullest first (one fills up to the top before the next starts), then the one in this room */
    struct Thing *best = INVALID_THING;
    int best_score = -1;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Object);
    long guard = 0;
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *other = thing_get(i);
        if (thing_is_invalid(other))
            break;
        i = other->next_of_class;
        if ((other == thing) || (other->model != thing->model) || (other->owner != owner) || !box_stacks(other) ||
            thing_is_dragged_or_pulled(other) || thing_is_picked_up(other) ||
            (other->custom_box.box_kind >= WORLD_BOX_MAX_UPGRADES))
            continue;
        struct Room *oroom = get_room_thing_is_on(other);
        if (room_is_invalid(oroom) || (oroom->owner != owner) || !room_role_matches(oroom->kind, RoRoF_PowersStorage))
            continue;
        const int score = other->custom_box.box_kind * 2 + ((oroom == room) ? 1 : 0);
        if (score > best_score)
        {
            best_score = score;
            best = other;
        }
    }
    if (thing_is_invalid(best))
        return;
    /* the boxes' powers add up: one box takes what fits, the rest stays in the one brought (none: it goes) */
    const int total = (1 + best->custom_box.box_kind) + (1 + min((int)thing->custom_box.box_kind, WORLD_BOX_MAX_UPGRADES));
    const int top = 1 + WORLD_BOX_MAX_UPGRADES;
    best->custom_box.box_kind = (unsigned char)(min(total, top) - 1);
    struct SpecialConfigStats *specst = get_special_model_stats(box_thing_to_special(thing));
    create_used_effect_or_element(&thing->mappos, specst->effect_id, owner, thing->index);
    create_effect(&best->mappos, TngEff_ResearchComplete, owner);
    SYNCLOG("World: player %d's %s box fuses with the one in the Library: it now works %d times", (int)owner,
        thing_model_name(best), 1 + (int)best->custom_box.box_kind);
    if (total > top)
    {
        thing->custom_box.box_kind = (unsigned char)(total - top - 1);
        return;
    }
    remove_events_thing_is_attached_to(thing);
    destroy_object(thing);
}

static void give_boon(PlayerNumber plyr_idx, int grade, uint64_t roll)
{
    struct WprPlayer *wp = &wpr_players[plyr_idx];
    wp->boons++;
    if ((roll % 3) == 0)
    {
        increase_level(get_player(plyr_idx), 1);
        SYNCLOG("World progress: player %d's book gives a boon: every creature gains a level", (int)plyr_idx);
    } else
    {
        player_add_offmap_gold(plyr_idx, 500 * grade);
        SYNCLOG("World progress: player %d's book gives a boon: %d gold", (int)plyr_idx, 500 * grade);
    }
}

/** The options of a book: one from the highest open tier, one of the book's biome, one more; distinct. */
static int choose_options(const int *open, int n, int biome, uint64_t roll, int *opt)
{
    int count = 0;
    int top = 0;
    for (int k = 0; k < n; k++)
        top = max(top, opt_tier(open[k]));
    for (int pass = 0; (pass < 3) && (count < 3); pass++)
    {
        int cand[WPR_OPTIONS];
        int m = 0;
        for (int k = 0; k < n; k++)
        {
            const int i = open[k];
            TbBool taken = false;
            for (int c = 0; c < count; c++)
                taken = taken || (opt[c] == i);
            if (taken)
                continue;
            if ((pass == 0) && (opt_tier(i) != top))
                continue;
            if ((pass == 1) && (opt_biome(i) != biome))
                continue;
            cand[m++] = i;
        }
        if (m == 0)
            continue;
        roll = wpr_mix(roll + pass);
        opt[count++] = cand[roll % (uint64_t)m];
    }
    return count;
}

static void destroy_book(struct Thing *thing)
{
    remove_events_thing_is_attached_to(thing);
    destroy_object(thing);
}

TbBool world_progress_activate(struct Thing *thing, struct PlayerInfo *player)
{
    if ((wpr_mode <= 0) || !thing_exists(thing) || (thing->class_id != TCls_Object) || (book_model <= 0) ||
        ((thing->model != book_model) && (thing->model != lore_model)))
        return false;
    const uint8_t kind = thing->custom_box.box_kind;
    const TbBool is_levelup = (thing->model == lore_model) && (wrl_of_levelup_kind(kind) >= 0);
    const TbBool is_lore = (kind >= WORLD_LORE_KIND_FIRST) || is_levelup;
    const PlayerNumber plyr_idx = player->id_number;
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || !world_player_is_human(plyr_idx))
        return true; /* books are for human keepers */
    struct WprPlayer *wp = &wpr_players[plyr_idx];
    struct Coord3d pos = thing->mappos;
    if (kind == WORLD_OUTPOST_BOOK_KIND)
    {
        /* the first one teaches the Library (there is none to take it to); after that the imps take them there */
        if (item_usable(WPR_LIBRARY) && !library_buildable(plyr_idx))
        {
            destroy_book(thing);
            unlock_item(plyr_idx, WPR_LIBRARY, &pos);
        }
        return true;
    }
    if (is_lore)
    {
        TbBool offered = false;
        for (int c = 0; c < wp->offer_count; c++)
            offered = offered || (wp->offer_things[c] == thing->index);
        if (!offered)
        {
            SYNCLOG("World progress: player %d's click on lore book %d: not their offer, nothing learnt", (int)plyr_idx,
                (int)thing->index);
            return true;
        }
        const int item = is_levelup ? WRL_ITEM(wrl_of_levelup_kind(kind)) : (kind - WORLD_LORE_KIND_FIRST);
        if ((item >= WPR_ITEMS) && (item < WPR_OPTIONS))
            room_level_allow(plyr_idx, item - WPR_ITEMS, &pos, wpr_offer_circle[plyr_idx]);
        else
        if (item_usable(item) && !wpr_item_disabled(item) && !(wp->unlocked & ((uint64_t)1 << item)))
            unlock_item(plyr_idx, item, &pos);
        for (int c = 0; c < wp->offer_count; c++)
        {
            struct Thing *other = thing_get(wp->offer_things[c]);
            if ((other != thing) && thing_exists(other) && (other->class_id == TCls_Object) && (other->model == lore_model))
                destroy_book(other);
        }
        wp->offer_count = 0;
        destroy_book(thing);
        return true;
    }
    if (wp->offer_count > 0)
    {
        /* an offer is open while its lore books are on the map: this book waits */
        TbBool open = false;
        for (int c = 0; c < wp->offer_count; c++)
        {
            const struct Thing *lore = thing_get(wp->offer_things[c]);
            open = open || (thing_exists(lore) && (lore->class_id == TCls_Object) && (lore->model == lore_model));
        }
        if (open)
            return true;
        wp->offer_count = 0;
    }
    const int grade = max(1, min((kind - 128) / 8, 5));
    const int biome = (kind - 128) % 8;
    const int circle = book_circle(thing); /* where it was found: what a duplicate in it is worth */
    wp->books_read++;
    wp->best_grade = (uint8_t)max((int)wp->best_grade, grade);
    const uint64_t roll = wpr_mix(world_seed() ^ ((uint64_t)plyr_idx << 56) ^ ((uint64_t)wp->books_read << 24) ^ 0x424F4F4BULL);
    int open[WPR_OPTIONS];
    const int n = open_set(plyr_idx, grade, open);
    SYNCLOG("World progress: player %d reads a grade %d book (%s), %d things it could teach", (int)plyr_idx, grade,
        wg_biome_name(biome), n);
    destroy_book(thing);
    if (n == 0)
    {
        give_boon(plyr_idx, grade, roll);
        return true;
    }
    if (item_usable(WPR_LIBRARY) && !(wp->unlocked & ((uint64_t)1 << WPR_LIBRARY)))
    {
        /* the first book teaches the Library */
        unlock_item(plyr_idx, WPR_LIBRARY, &pos);
        return true;
    }
    if (item_usable(WPR_TRAINING) && !(wp->unlocked & ((uint64_t)1 << WPR_TRAINING)) && !item_had(plyr_idx, WPR_TRAINING))
    {
        /* and the second the Training Room (2026-11-09) */
        unlock_item(plyr_idx, WPR_TRAINING, &pos);
        return true;
    }
    int opt[3];
    const int k = choose_options(open, n, biome, roll, opt);
    static int automatic = -1;
    if (automatic < 0)
        automatic = ((getenv("KFX_WORLD_PROGRESS") != NULL) && (strcmp(getenv("KFX_WORLD_PROGRESS"), "auto") == 0)) ? 1 : 0;
    if ((wpr_mode == 2) || automatic || (k == 1) || (lore_model <= 0))
    {
        take_option(plyr_idx, opt[0], &pos, circle);
        return true;
    }
    wpr_offer_circle[plyr_idx] = (uint8_t)circle;
    /* the offer: lore books beside where the book lay, on the reader's ground */
    const MapSubtlCoord sx = pos.x.stl.num, sy = pos.y.stl.num;
    static const int ox[] = {-1, 1, 0}, oy[] = {0, 0, 1};
    wp->offer_count = 0;
    for (int c = 0; c < k; c++)
    {
        struct Coord3d lp;
        lp.x.val = subtile_coord_center(sx + ox[c]);
        lp.y.val = subtile_coord_center(sy + oy[c]);
        lp.z.val = 0;
        lp.z.val = get_floor_height_at(&lp);
        const int lore_kind = (opt[c] >= WPR_ITEMS) ? wrl_kind(opt[c] - WPR_ITEMS) : (WORLD_LORE_KIND_FIRST + opt[c]);
        struct Thing *lore = make_book(lore_model, &lp, plyr_idx, (uint8_t)lore_kind);
        if (thing_is_invalid(lore))
            continue;
        lore->custom_box.box_circle = (unsigned char)circle;
        wp->offer_items[wp->offer_count] = (uint8_t)opt[c];
        wp->offer_things[wp->offer_count] = lore->index;
        wp->offer_count++;
    }
    SYNCLOG("World progress: player %d is offered %s%s%s%s%s", (int)plyr_idx, opt_label(opt[0]),
        (k > 1) ? ", " : "", (k > 1) ? opt_label(opt[1]) : "", (k > 2) ? ", " : "", (k > 2) ? opt_label(opt[2]) : "");
    if (wp->offer_count == 0)
        take_option(plyr_idx, opt[0], &pos, circle);
    return true;
}

/******************************************************************************/
/* test hooks */

/** KFX_WORLD_BOOKS="n[,grade]": at turn 30, n research books beside every human keeper's heart. */
static void test_books(void)
{
    static int want = -1, grade = 1;
    if (want < 0)
    {
        const char *v = getenv("KFX_WORLD_BOOKS");
        want = (v != NULL) ? atoi(v) : 0;
        if ((v != NULL) && (strchr(v, ',') != NULL))
            grade = atoi(strchr(v, ',') + 1);
    }
    if ((want <= 0) || (game.play_gameturn != 30))
        return;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!world_player_is_human(p))
            continue;
        const struct Thing *heart = get_player_soul_container(p);
        if (!thing_exists(heart))
            continue;
        /* the heart room is the player's ground: 3 subtiles south and east of the heart's centre and on */
        for (int i = 0; i < want; i++)
        {
            world_progress_drop_books(heart->mappos.x.stl.num + 3 + (i % 3), heart->mappos.y.stl.num + 3 + (i / 3), grade,
                i % WgB_COUNT, 1);
        }
    }
}

/** KFX_WORLD_READBOOKS=1: every 20 turns the local player clicks a research or lore book on its ground, through a
 *  packet as the Hand does. */
static void test_read_books(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("KFX_WORLD_READBOOKS") != NULL) ? 1 : 0;
    if (!on || ((game.play_gameturn % 20) != 0) || (book_model <= 0))
        return;
    const struct StructureList *list = get_list_for_thing_class(TCls_Object);
    long i = list->index;
    long guard = 0;
    while ((i > 0) && (guard++ < THINGS_COUNT))
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing))
            break;
        i = thing->next_of_class;
        if ((thing->model != book_model) && (thing->model != lore_model))
            continue;
        if (slabmap_owner(get_slabmap_thing_is_on(thing)) != my_player_number)
            continue;
        set_players_packet_action(get_my_player(), PckA_UseSpecialBox, thing->index, 0, 0, 0);
        SYNCLOG("World progress test: player %d clicks %s %d (kind %d)", (int)my_player_number,
            (thing->model == lore_model) && (thing->custom_box.box_kind >= WORLD_LORE_KIND_FIRST) ? "lore book" : "book",
            (int)thing->index, (int)thing->custom_box.box_kind);
        return;
    }
}

/** The spells' effects as cast on one of the keeper's imps (the room levels test): speed and armour before and after
 *  Speed and Protect, the time Conceal and Protect last, what a strongest Heal heals, the level of a new imp. */
static void test_spell_effects(PlayerNumber p, const char *when)
{
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    struct Thing *crtng = INVALID_THING;
    for (ThingIndex i = dungeon->digger_list_start, guard = 0; (i > 0) && (guard < 500); guard++)
    {
        struct Thing *t = thing_get(i);
        struct CreatureControl *c = creature_control_get_from_thing(t);
        if (!thing_is_creature(t) || creature_control_invalid(c))
            break;
        if (!creature_under_spell_effect(t, CSAfF_Speed) && !creature_under_spell_effect(t, CSAfF_Armour) &&
            !creature_under_spell_effect(t, CSAfF_Invisibility) && !thing_is_picked_up(t))
        {
            crtng = t;
            break;
        }
        i = c->players_next_creature_idx;
    }
    if (!thing_is_creature(crtng))
    {
        SYNCLOG("Spell levels test (%s): no imp free of spells", when);
        return;
    }
    const MapSubtlCoord sx = crtng->mappos.x.stl.num, sy = crtng->mappos.y.stl.num;
    const long speed0 = calculate_correct_creature_maxspeed(crtng), armour0 = calculate_correct_creature_armour(crtng);
    const TbResult rs = magic_use_power_on_thing(p, PwrK_SPEEDCRTR, 0, sx, sy, crtng, PwMod_CastForFree);
    const TbResult rp = magic_use_power_on_thing(p, PwrK_PROTECT, 0, sx, sy, crtng, PwMod_CastForFree);
    const TbResult rc = magic_use_power_on_thing(p, PwrK_CONCEAL, 0, sx, sy, crtng, PwMod_CastForFree);
    const long speed1 = calculate_correct_creature_maxspeed(crtng), armour1 = calculate_correct_creature_armour(crtng);
    struct CreatureControl *cctrl = creature_control_get_from_thing(crtng);
    char slots[120] = "";
    for (int k = 0; k < CREATURE_MAX_SPELLS_CASTED_AT; k++)
        if (cctrl->casted_spells[k].spkind != 0)
            snprintf(slots + strlen(slots), sizeof(slots) - strlen(slots), " %s=%d", spell_code_name(cctrl->casted_spells[k].spkind),
                (int)cctrl->casted_spells[k].duration);
    SYNCLOG("Spell levels test (%s): imp %d speed %ld -> %ld (cast %d), armour %ld -> %ld (cast %d), conceal cast %d,"
        " spells%s", when, (int)crtng->index, speed0, speed1, (int)rs, armour0, armour1, (int)rp, (int)rc, slots);
    struct Thing *big = INVALID_THING;
    for (ThingIndex i = dungeon->creatr_list_start, guard = 0; (i > 0) && (guard < 500); guard++)
    {
        struct Thing *t = thing_get(i);
        struct CreatureControl *c = creature_control_get_from_thing(t);
        if (!thing_is_creature(t) || creature_control_invalid(c))
            break;
        if (!creature_under_spell_effect(t, CSAfF_Armour) && !thing_is_picked_up(t) &&
            (!thing_is_creature(big) || (c->max_health > creature_control_get_from_thing(big)->max_health)))
            big = t;
        i = c->players_next_creature_idx;
    }
    if (thing_is_creature(big))
    {
        struct CreatureControl *bc = creature_control_get_from_thing(big);
        const MapSubtlCoord bx = big->mappos.x.stl.num, by = big->mappos.y.stl.num;
        const long barmour0 = calculate_correct_creature_armour(big);
        const TbResult rbp = magic_use_power_on_thing(p, PwrK_PROTECT, 0, bx, by, big, PwMod_CastForFree);
        const long barmour1 = calculate_correct_creature_armour(big);
        /* a big max health for the measure only, so the heal is not cut at the creature's own */
        const HitPoints health0 = big->health, max0 = bc->max_health;
        bc->max_health = 5000;
        big->health = 1;
        const TbResult rh = magic_use_power_on_thing(p, PwrK_HEALCRTR, 8, bx, by, big, PwMod_CastForFree);
        const HitPoints healed = big->health - 1;
        bc->max_health = max0;
        big->health = health0;
        SYNCLOG("Spell levels test (%s): %s %d (max health %ld): armour %ld -> %ld (cast %d); heal at power 8 healed %ld"
            " (cast %d)", when, thing_model_name(big), (int)big->index, (long)bc->max_health, barmour0, barmour1, (int)rbp,
            (long)healed, (int)rh);
    }
    const TbResult ri = magic_use_available_power_on_subtile(p, PwrK_MKDIGGER, 0, sx, sy, PwCast_None, PwMod_CastForFree);
    const struct Thing *imp = thing_get(dungeon->digger_list_start);
    const struct CreatureControl *ic = creature_control_get_from_thing(imp);
    SYNCLOG("Spell levels test (%s): Create Imp cast %d at %d,%d, the newest imp %d is level %d", when, (int)ri, (int)sx,
        (int)sy, (int)imp->index, creature_control_invalid(ic) ? -1 : (int)ic->exp_level + 1);
}

/** KFX_WORLD_TESTROOMLVL=<turns>[,fast][,spells]: that many turns after this machine's world started, every room and
 *  spell with levels the local keeper owns gets two duplicate books (",spells": the keeper gets the seven spells
 *  first, and their effects are measured then and once all are level 3); then every 600 turns the levels, the
 *  bonuses and the research are logged. */
static void test_room_levels(void)
{
    static int delay = -2;
    static TbBool spells = false, spells_measured = false, traps = false;
    static int fast = 0;
    static GameTurn first = 0;
    if (delay == -2)
    {
        const char *v = getenv("KFX_WORLD_TESTROOMLVL");
        delay = (v != NULL) ? atoi(v) : -1;
        /* ",fast": research points as if 40 a turn came in (",fast=N": N a turn) */
        const char *f = (v != NULL) ? strstr(v, "fast") : NULL;
        fast = (f == NULL) ? 0 : ((f[4] == '=') ? atoi(f + 5) : 40);
        spells = (v != NULL) && (strstr(v, "spells") != NULL);
        /* ",traps": the four traps with levels are given; the Lightning Trap's bar is left part full, the trap
           tab is opened for the screenshots */
        traps = (v != NULL) && (strstr(v, "traps") != NULL);
    }
    if (delay < 0)
        return;
    if (first == 0)
        first = game.play_gameturn;
    const PlayerNumber p = my_player_number;
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX))
        return;
    if (game.play_gameturn == first + (GameTurn)delay)
    {
        const struct Thing *heart = get_player_soul_container(p);
        struct Coord3d pos;
        memset(&pos, 0, sizeof(pos));
        if (thing_exists(heart))
            pos = heart->mappos;
        if (spells)
        {
            for (int r = WRL_FIRST_POWER; r < WRL_FIRST_ROOM2; r++)
                if (!wrl_owned(p, r) && (wrl_ids[r] > 0))
                    add_power_to_player(wrl_ids[r], p);
            test_spell_effects(p, "level 1");
        }
        if (traps)
            for (int r = WRL_FIRST_TRAP; r < WRL_ROOMS; r++)
                if (wrl_ids[r] > 0)
                    set_trap_buildable_and_add_to_amount(p, wrl_ids[r], 1, 1);
        for (int r = 0; r < WRL_ROOMS; r++)
        {
            if (!wrl_owned(p, r))
            {
                SYNCLOG("Room levels test: player %d has no %s", (int)p, wrl_labels[r]);
                continue;
            }
            /* every bar full, to the top (with ",traps", the Lightning Trap's only a little) */
            room_level_xp(p, r, (traps && (r == WRL_FIRST_TRAP + 1)) ? 10 : 30000, &pos);
        }
        if (traps)
            set_menu_mode(GMnu_TRAP);
    }
    if (fast && (game.play_gameturn > first + (GameTurn)delay) && (game.world_room_researching[p] != 0))
    {
        struct Dungeon *fd = get_players_num_dungeon(p);
        if (!dungeon_invalid(fd))
            fd->research_progress += fast << 8;
    }
    if (fast && (game.play_gameturn > first + (GameTurn)delay) && (getenv("KFX_WORLD_TESTAMENITY") != NULL))
    {
        /* the Library's and the Workshop's parts without the rooms (a test run builds none) */
        struct Dungeon *fd = get_players_num_dungeon(p);
        if (!dungeon_invalid(fd))
        {
            fd->manufacture_progress += fast << 8;
            world_trap_level_manufacture(p);
            if (game.world_room_researching[p] == 0)
                fd->research_progress += fast << 8;
            world_room_level_research(p);
        }
    }
    if (spells && !spells_measured && (game.play_gameturn > first + (GameTurn)delay))
    {
        TbBool all = true;
        for (int r = WRL_FIRST_POWER; r < WRL_FIRST_ROOM2; r++)
            all = all && (wrl_level(p, r) >= wrl_top[r]);
        if (all)
        {
            spells_measured = true;
            test_spell_effects(p, "level 3");
            set_menu_mode(GMnu_SPELL); /* the spells tab, for the screenshots */
        }
    }
    if ((game.play_gameturn > first + (GameTurn)delay) && (((game.play_gameturn - first) % 600) == 0))
    {
        const struct Dungeon *dungeon = get_players_num_dungeon(p);
        SYNCLOG("Room levels test: current research %d (job open %d)", dungeon_invalid(dungeon) ? -9 : (int)dungeon->current_research_idx,
            dungeon_invalid(dungeon) ? 0 : (dungeon->current_research_idx >= 0));
        char levels[400] = "";
        for (int r = 0; r < WRL_ROOMS; r++)
            snprintf(levels + strlen(levels), sizeof(levels) - strlen(levels), "%s%s %d/%d", r ? ", " : "", wrl_labels[r],
                wrl_level(p, r), wrl_allowed(p, r));
        SYNCLOG("Room levels test: %s; researching %d, progress %ld", levels, (int)game.world_room_researching[p] - 1,
            dungeon_invalid(dungeon) ? 0L : (long)(dungeon->research_progress >> 8));
        SYNCLOG("Room levels test: bonuses lair %d hunger %d food %d train %d research %d; imp +%d sight %d speed %d "
            "conceal +%d heal top %d lightning %d protect %d +%d", world_room_bonus(p, WRB_LairHeal),
            world_room_bonus(p, WRB_HatcheryHunger), world_room_bonus(p, WRB_HatcheryHeal), world_room_bonus(p, WRB_TrainingXp),
            world_room_bonus(p, WRB_LibraryResearch), world_power_bonus(p, WPB_ImpLevels), world_power_bonus(p, WPB_SightRadius),
            world_power_bonus(p, WPB_SpeedEffect), world_power_bonus(p, WPB_ConcealTurns), world_power_bonus(p, WPB_HealTop),
            world_power_bonus(p, WPB_LightningDamage), world_power_bonus(p, WPB_ProtectArmour), world_power_bonus(p, WPB_ProtectTurns));
    }
}

/** Hell Hounds were drawn only by a Scavenger Room, which is off in endless worlds (2026-11-12): they come to a Lair
 *  of 9 slabs instead, once the keeper owns a portal in volcanic ground (they are one of the portals' biome
 *  creatures). The level's config is this machine's, read again with each level: set on every machine. */
static void hell_hound_entrance(void)
{
    /* and the world's creature cap (2026-11-12): MapCreatureLimit (1000) stopped portals, Create Imp, wild packs and
       raids long before 100 keepers of 50 creatures; in endless worlds it is the engine's own table (8192), so 5,000
       players' creatures leave room for imps, heroes and wild packs */
    if (game.conf.rules[0].gameplay.creatures_count != CREATURES_COUNT - 2)
    {
        game.conf.rules[0].gameplay.creatures_count = CREATURES_COUNT - 2;
        SYNCLOG("World progress: the world's creature cap is %d", CREATURES_COUNT - 2);
    }
    const long model = get_id(creature_desc, "HELL_HOUND");
    const long lair = get_id(room_desc, "LAIR");
    if ((model <= 0) || (lair <= 0) || (model >= game.conf.crtr_conf.model_count))
        return;
    struct CreatureModelConfig *crconf = creature_stats_get((ThingModel)model);
    if ((crconf->entrance_rooms[0] == (RoomKind)lair) && (crconf->entrance_slabs_req[0] == 9))
        return;
    for (int k = 0; k < ENTRANCE_ROOMS_COUNT; k++)
    {
        crconf->entrance_rooms[k] = RoK_NONE;
        crconf->entrance_slabs_req[k] = 0;
    }
    crconf->entrance_rooms[0] = (RoomKind)lair;
    crconf->entrance_slabs_req[0] = 9;
    SYNCLOG("World progress: Hell Hounds come to a Lair of 9 slabs (no Scavenger Room in endless worlds)");
}

/** Destroy Walls, Armageddon and Disease stay off in endless worlds for every keeper, AI ones too: neither
 *  researchable nor usable. */
static void hell_hound_entrance(void);
static void ban_powers(void)
{
    hell_hound_entrance();
    for (int i = 0; i < WPR_ITEMS; i++)
    {
        if (!wpr_item_disabled(i) || !item_usable(i) || (wpr_items[i].kind != WprK_Room))
            continue;
        /* a room that stays off (the Scavenger Room): nobody can build it, a computer keeper neither */
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            if (dungeon_invalid(dungeon) || !player_exists(get_player(p)))
                continue;
            if ((dungeon->room_buildable[wpr_ids[i]] != 0) || (dungeon->room_resrchable[wpr_ids[i]] != 0))
                set_room_available(p, wpr_ids[i], 0, 0);
        }
    }
    for (int i = 0; i < WPR_ITEMS; i++)
    {
        if (!wpr_item_disabled(i) || !item_usable(i) || (wpr_items[i].kind != WprK_Power))
            continue;
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            if (dungeon_invalid(dungeon) || !player_exists(get_player(p)))
                continue;
            if ((dungeon->magic_resrchable[wpr_ids[i]] != 0) || (dungeon->magic_level[wpr_ids[i]] > 0))
                set_power_available(p, wpr_ids[i], 0, 0);
        }
    }
}

/******************************************************************************/
/* Amenity Mastery: the Barracks, the Poison Gas Trap's lasting poison, the Lightning Trap's second bolt and chain */

TbBool world_barracks_may_group(const struct Thing *a, const struct Thing *b)
{
    if (!world_mode_active() || !world_player_is_human(a->owner))
        return true;
    static long horny = -1;
    if (horny < 0)
        horny = max(get_id(creature_desc, "HORNY"), 0L);
    const int mixed = world_room_bonus(a->owner, WRB_BarracksMixed);
    if ((horny > 0) && ((a->model == horny) || (b->model == horny)))
        return (mixed >= 2) || (a->model == b->model);
    if (mixed >= 1)
        return true;
    /* lair enemies (Fly and Spider, Bile Demon and Skeleton, ...) */
    const struct CreatureModelConfig *ca = creature_stats_get_from_thing(a), *cb = creature_stats_get_from_thing(b);
    for (int i = 0; i < CREATURE_TYPES_MAX; i++)
        if (((ca->lair_enemy[i] != 0) && (ca->lair_enemy[i] == b->model)) || ((cb->lair_enemy[i] != 0) && (cb->lair_enemy[i] == a->model)))
            return false;
    return true;
}

#define WP_POISON_MAX 96
#define WP_POISON_TURNS (wrl_numbers()->misc[WRLN_PoisonTurns])
#define WP_POISON_EVERY (wrl_numbers()->misc[WRLN_PoisonEvery])
static struct WpPoison { ThingIndex idx; int16_t damage; int8_t owner; uint32_t made, until, next; } wp_poison[WP_POISON_MAX];
static int wp_poison_count = 0;
static int wp_trap_log = 40; /* log lines per run */

void world_trap_gas_hit(const struct Thing *efftng, struct Thing *creatng, HitPoints damage)
{
    if ((efftng->class_id != TCls_Effect) || (efftng->shot_effect.parent_class_id != TCls_Trap) || (damage <= 0) ||
        (world_trap_bonus(efftng->owner, efftng->shot_effect.parent_model, WTB_Special) <= 0))
        return;
    struct WpPoison *slot = NULL;
    for (int i = 0; i < WP_POISON_MAX; i++)
    {
        if ((wp_poison[i].idx == creatng->index) && (wp_poison[i].made == (uint32_t)creatng->creation_turn))
        {
            slot = &wp_poison[i];
            break;
        }
        if ((slot == NULL) && (wp_poison[i].idx == 0))
            slot = &wp_poison[i];
    }
    if (slot == NULL)
        return;
    if (slot->idx == 0)
    {
        wp_poison_count++;
        slot->idx = creatng->index;
        slot->made = (uint32_t)creatng->creation_turn;
        slot->next = (uint32_t)game.play_gameturn + WP_POISON_EVERY;
        slot->damage = 0;
        if (wp_trap_log > 0)
        {
            wp_trap_log--;
            SYNCLOG("Amenity: Poison Gas Trap of player %d poisons %s index %d", (int)efftng->owner,
                thing_model_name(creatng), (int)creatng->index);
        }
    }
    slot->owner = (int8_t)efftng->owner;
    slot->damage = (int16_t)max((HitPoints)slot->damage, min(damage, (HitPoints)30000));
    slot->until = (uint32_t)game.play_gameturn + WP_POISON_TURNS; /* counted from its last breath of the gas */
}

static void wp_poison_turn(void)
{
    if (wp_poison_count == 0)
        return;
    const uint32_t now = (uint32_t)game.play_gameturn;
    for (int i = 0; i < WP_POISON_MAX; i++)
    {
        struct WpPoison *p = &wp_poison[i];
        if (p->idx == 0)
            continue;
        struct Thing *thing = thing_get(p->idx);
        const TbBool alive = thing_exists(thing) && (thing->class_id == TCls_Creature) &&
            ((uint32_t)thing->creation_turn == p->made) && (thing->health >= 0) && !creature_is_dying(thing);
        if (!alive || (now > p->until))
        {
            memset(p, 0, sizeof(*p));
            wp_poison_count--;
            continue;
        }
        if (now < p->next)
            continue;
        p->next = now + WP_POISON_EVERY;
        if (wp_trap_log > 0)
        {
            wp_trap_log--;
            SYNCLOG("Amenity: the lasting poison does %d to %s index %d (health %d)", (int)p->damage, thing_model_name(thing),
                (int)thing->index, (int)thing->health);
        }
        apply_damage_to_thing_and_display_health(thing, p->damage, p->owner);
    }
}

/** The nearest creature a trap of that owner strikes, within range of pos and in its sight, other than the given ones. */
static struct Thing *wp_bolt_target(PlayerNumber owner, const struct Coord3d *pos, long range, const struct Thing *const *skip, int skips)
{
    long best_dist = range;
    struct Thing *best = INVALID_THING;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long i = slist->index;
    for (unsigned long k = 0; (i != 0) && (k <= slist->count); k++)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (players_are_mutual_allies(owner, thing->owner) || thing_is_picked_up(thing) || creature_is_being_unconscious(thing) ||
            creature_is_dying(thing))
            continue;
        TbBool skipped = false;
        for (int n = 0; n < skips; n++)
            if (skip[n] == thing)
                skipped = true;
        if (skipped)
            continue;
        const long dist = get_2d_distance(pos, &thing->mappos);
        if ((dist < best_dist) && line_of_sight_2d(pos, &thing->mappos))
        {
            best_dist = dist;
            best = thing;
        }
    }
    return best;
}

void world_trap_lightning_extras(struct Thing *shotng, struct Thing *first, HitPoints damage, long range, long spacing, long effect_id)
{
    struct Thing *traptng = thing_get(shotng->parent_idx);
    if (!thing_exists(traptng) || (traptng->class_id != TCls_Trap) || (damage <= 0))
        return;
    const int level_extra = world_trap_bonus(traptng->owner, traptng->model, WTB_Special);
    if (level_extra <= 0)
        return;
    const struct Thing *struck[4] = {first, NULL, NULL, NULL};
    int count = 1;
    /* a second creature at once; the first second bolt of an activation takes one more charge
       (kept in the bolt's unused "dexterity": 1 once the charge is taken) */
    struct Thing *second = wp_bolt_target(shotng->owner, &shotng->mappos, range, struck, count);
    if (!thing_is_invalid(second))
    {
        TbBool paid = (shotng->shot.dexterity == 1);
        if (!paid && ((traptng->trap.num_shots == INFINITE_CHARGES) || (traptng->trap.num_shots > 0)))
        {
            if (traptng->trap.num_shots != INFINITE_CHARGES)
            {
                traptng->trap.num_shots--;
                if (traptng->trap.num_shots <= 0)
                    traptng->health = -1;
            }
            shotng->shot.dexterity = 1;
            paid = true;
        }
        if (paid)
        {
            draw_lightning(&shotng->mappos, &second->mappos, spacing, effect_id);
            if (wp_trap_log > 0)
            {
                wp_trap_log--;
                SYNCLOG("Amenity: Lightning Trap of player %d strikes a second creature, %s index %d, for %d (charges left %d)",
                    (int)traptng->owner, thing_model_name(second), (int)second->index, (int)damage, (int)traptng->trap.num_shots);
            }
            struck[count++] = second;
            apply_damage_to_thing_and_display_health(second, damage, shotng->owner);
        }
    }
    if (level_extra < 2)
        return;
    /* the chain: each creature struck passes a part on to one more nearby */
    const int direct = count;
    for (int n = 0; n < direct; n++)
    {
        const struct Thing *from = struck[n];
        if (!thing_exists(from))
            continue;
        struct Thing *next = wp_bolt_target(shotng->owner, &from->mappos, wrl_numbers()->misc[WRLN_ChainSlabs] * COORD_PER_SLB, struck, count);
        if (thing_is_invalid(next))
            continue;
        draw_lightning(&from->mappos, &next->mappos, spacing, effect_id);
        if (wp_trap_log > 0)
        {
            wp_trap_log--;
            SYNCLOG("Amenity: Lightning Trap chain: from %s index %d to %s index %d for %d", thing_model_name(from), (int)from->index,
                thing_model_name(next), (int)next->index, (int)(damage / wrl_numbers()->misc[WRLN_ChainDivide]));
        }
        struck[count++] = next;
        apply_damage_to_thing_and_display_health(next, damage / wrl_numbers()->misc[WRLN_ChainDivide], shotng->owner);
    }
}

/** Test hook KFX_WORLD_TRAPTEST="T": at T the local keeper's Poison Gas Trap and Lightning Trap are at their top level;
 *  one of each is put south of the heart with three hero Archers on the Lightning Trap and two on the gas trap; and
 *  the Barracks' party rule is logged for the room's three levels. */
static void test_traps(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_TRAPTEST");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || ((long)game.play_gameturn != base))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !thing_exists(heart))
        return;
    /* the Barracks' rule */
    static const char *const names[4] = {"ORC", "HORNY", "FLY", "SPIDER"};
    struct Thing *crt[4];
    struct Coord3d pos = heart->mappos;
    pos.y.val += 6 * COORD_PER_STL;
    TbBool all = true;
    for (int i = 0; i < 4; i++)
    {
        crt[i] = create_creature(&pos, (ThingModel)get_id(creature_desc, names[i]), p);
        all = all && !thing_is_invalid(crt[i]);
    }
    const uint8_t barracks_was = game.world_room_level[p][15];
    for (int level = 1; all && (level <= 3); level++)
    {
        game.world_room_level[p][15] = (uint8_t)level;
        SYNCLOG("Amenity test: Barracks level %d: Fly with Spider %s, Orc with Horned Reaper %s, Orc with Fly %s", level,
            world_barracks_may_group(crt[2], crt[3]) ? "yes" : "no", world_barracks_may_group(crt[0], crt[1]) ? "yes" : "no",
            world_barracks_may_group(crt[0], crt[2]) ? "yes" : "no");
    }
    game.world_room_level[p][15] = barracks_was;
    for (int i = 0; i < 4; i++)
        if (!thing_is_invalid(crt[i]))
            delete_thing_structure(crt[i], 0);
    /* the traps */
    game.world_room_level[p][WRL_FIRST_TRAP + 0] = (uint8_t)wrl_top[WRL_FIRST_TRAP + 0];
    game.world_room_level[p][WRL_FIRST_TRAP + 1] = (uint8_t)wrl_top[WRL_FIRST_TRAP + 1];
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    static const char *const traps[2] = {"LIGHTNING", "POISON_GAS"};
    for (int t = 0; t < 2; t++)
    {
        struct Coord3d tpos;
        tpos.x.val = subtile_coord_center(slab_subtile_center(hx + (t ? -1 : 1)));
        tpos.y.val = subtile_coord_center(slab_subtile_center(hy + 2));
        tpos.z.val = 0;
        tpos.z.val = get_floor_height_at(&tpos);
        struct Thing *traptng = create_trap(&tpos, (ThingModel)get_id(trap_desc, traps[t]), p);
        if (!thing_is_invalid(traptng))
            rearm_trap(traptng);
        int heroes = 0;
        for (int i = 0; i < (t ? 2 : 3); i++)
        {
            struct Coord3d cpos = tpos;
            cpos.x.val += (i - 1) * COORD_PER_STL;
            const struct Thing *hero = create_creature(&cpos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
            if (!thing_is_invalid(hero))
                heroes++;
        }
        SYNCLOG("Amenity test: %s trap %s (level %d, %d charges) with %d Archers on it", traps[t],
            thing_is_invalid(traptng) ? "MISSING" : "placed", world_trap_level(p, traptng->model),
            thing_is_invalid(traptng) ? 0 : (int)traptng->trap.num_shots, heroes);
    }
}

void world_progress_turn(void)
{
    if (world_mode_active() && ((game.play_gameturn % 64) == 2))
        ban_powers();
    wp_poison_turn();
    test_traps();
    world_storage_turn();
    world_hand_test_turn();
    world_watchfire_turn();
    world_dark_turn();
    if (!world_progress_active())
        return;
    test_books();
    test_read_books();
    test_room_levels();
}

void world_progress_client_turn(void)
{
    test_read_books();
}

/******************************************************************************/
/* saving */
#pragma pack(1)
struct WprSaved {
    uint64_t unlocked;
    uint32_t books_read, boons;
    uint8_t best_grade;
    uint16_t raid_pending;
    uint8_t offer_count;
    uint8_t offer_items[3];
    uint16_t offer_things[3];
};
#pragma pack()

/** The words of the kit set a save keeps: enough for the players in use. */
static int wpr_mask_words(void)
{
    const int n = (PLAYERS_COUNT + 31) / 32;
    return (n < 1) ? 1 : (n > PLAYER_MASK_WORDS) ? PLAYER_MASK_WORDS : n;
}

size_t world_progress_save_size(void)
{
    if (wpr_mode <= 0)
        return 0;
    return 4 + 4 + 1 + 1 + PLAYERS_COUNT * sizeof(struct WprSaved) + 1 + 4 * wpr_mask_words() + 2 + PLAYERS_COUNT * (2 * WRL_ROOMS + 5);
}

void world_progress_save(uint8_t *buf)
{
    const uint32_t magic = WPR_SAVE_MAGIC, version = WPR_SAVE_VERSION;
    memcpy(buf, &magic, 4); buf += 4;
    memcpy(buf, &version, 4); buf += 4;
    *buf++ = (uint8_t)wpr_mode;
    *buf++ = (uint8_t)PLAYERS_COUNT;
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        const struct WprPlayer *wp = &wpr_players[p];
        struct WprSaved s;
        memset(&s, 0, sizeof(s));
        s.unlocked = wp->unlocked;
        s.books_read = wp->books_read;
        s.boons = wp->boons;
        s.best_grade = wp->best_grade;
        s.raid_pending = wp->raid_pending;
        s.offer_count = wp->offer_count;
        memcpy(s.offer_items, wp->offer_items, sizeof(s.offer_items));
        memcpy(s.offer_things, wp->offer_things, sizeof(s.offer_things));
        memcpy(buf, &s, sizeof(s));
        buf += sizeof(s);
    }
    const int words = wpr_mask_words();
    *buf++ = (uint8_t)words;
    memcpy(buf, wpr_kit_players.w, 4 * (size_t)words);
    buf += 4 * (size_t)words;
    *buf++ = (uint8_t)PLAYERS_COUNT;
    *buf++ = (uint8_t)WRL_ROOMS;
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        memcpy(buf, game.world_room_level[p], WRL_ROOMS); buf += WRL_ROOMS;
        memcpy(buf, game.world_room_allowed[p], WRL_ROOMS); buf += WRL_ROOMS;
        *buf++ = game.world_room_researching[p];
        memcpy(buf, &wpr_players[p].lvl_pending_since, 4); buf += 4;
    }
}

TbBool world_progress_load(const uint8_t *buf, size_t len)
{
    memset(wpr_players, 0, sizeof(wpr_players));
    memset(game.world_room_level, 0, sizeof(game.world_room_level));
    memset(game.world_room_allowed, 0, sizeof(game.world_room_allowed));
    memset(game.world_room_researching, 0, sizeof(game.world_room_researching));
    resolve_ids();
    wpr_mode = 0;
    uint32_t magic, version;
    if (len < 10)
        return false;
    memcpy(&magic, buf, 4);
    memcpy(&version, buf + 4, 4);
    if ((magic != WPR_SAVE_MAGIC) || (version < 1) || (version > WPR_SAVE_VERSION))
        return false;
    const int mode = buf[8], count = buf[9];
    wpr_kit_given = true; /* a loaded run had its kit long ago */
    wpr_kit_players = pmask_none();
    if (len < 10 + (size_t)count * sizeof(struct WprSaved))
        return false;
    const uint8_t *p = buf + 10;
    for (int i = 0; i < count; i++, p += sizeof(struct WprSaved))
    {
        if (i >= PLAYERS_COUNT)
            continue;
        struct WprSaved s;
        memcpy(&s, p, sizeof(s));
        struct WprPlayer *wp = &wpr_players[i];
        wp->unlocked = s.unlocked;
        wp->books_read = s.books_read;
        wp->boons = s.boons;
        wp->best_grade = s.best_grade;
        wp->raid_pending = s.raid_pending;
        wp->offer_count = (uint8_t)min((int)s.offer_count, 3);
        memcpy(wp->offer_items, s.offer_items, sizeof(wp->offer_items));
        memcpy(wp->offer_things, s.offer_things, sizeof(wp->offer_things));
    }
    wpr_mode = mode;
    const uint8_t *kit = buf + 10 + (size_t)count * sizeof(struct WprSaved);
    const size_t kit_len = len - (10 + (size_t)count * sizeof(struct WprSaved));
    if ((version >= 3) && (kit_len >= 1) && (kit[0] >= 1) && (kit_len >= 1 + 4 * (size_t)kit[0]))
    {
        for (int k = 0; (k < kit[0]) && (k < PLAYER_MASK_WORDS); k++)
            memcpy(&wpr_kit_players.w[k], kit + 1 + 4 * k, 4);
        const uint8_t *lv = kit + 1 + 4 * (size_t)kit[0];
        const size_t lv_len = kit_len - (1 + 4 * (size_t)kit[0]);
        if ((version >= 5) && (lv_len >= 2) && (lv[1] <= WRL_ROOMS) && (lv_len >= 2 + (size_t)lv[0] * (2 * lv[1] + 5)))
        {
            const int n = lv[1], stride = 2 * n + 5;
            for (int pl = 0; pl < lv[0]; pl++)
            {
                if (pl >= PLAYERS_COUNT)
                    continue;
                const uint8_t *e = lv + 2 + (size_t)pl * stride;
                for (int r = 0; r < n; r++)
                {
                    game.world_room_level[pl][r] = (uint8_t)min((int)e[r], WRL_MAX);
                    game.world_room_allowed[pl][r] = (uint8_t)min((int)e[n + r], WRL_MAX);
                }
                game.world_room_researching[pl] = (e[2 * n] <= WRL_ROOMS) ? e[2 * n] : 0;
                memcpy(&wpr_players[pl].lvl_pending_since, e + 2 * n + 1, 4);
            }
        } else
        if ((version == 4) && (lv_len >= 1) && (lv_len >= 1 + (size_t)lv[0] * 13))
        {
            for (int pl = 0; pl < lv[0]; pl++)
            {
                if (pl >= PLAYERS_COUNT)
                    continue;
                const uint8_t *e = lv + 1 + (size_t)pl * 13;
                for (int r = 0; r < 4; r++)
                {
                    game.world_room_level[pl][r] = (uint8_t)min((int)e[r], WRL_MAX);
                    game.world_room_allowed[pl][r] = (uint8_t)min((int)e[4 + r], WRL_MAX);
                }
                game.world_room_researching[pl] = (e[8] <= 4) ? e[8] : 0;
                memcpy(&wpr_players[pl].lvl_pending_since, e + 9, 4);
            }
        }
    } else
    if ((version == 2) && (kit_len >= 2))
    {
        uint16_t v;
        memcpy(&v, kit, 2);
        wpr_kit_players = pmask_from_u32(v);
    } else
    {
        /* version 1: the seats that have read or been offered anything had their kit; a seat first used after this
           gets it */
        for (int i = 0; i < PLAYERS_COUNT; i++)
        {
            if ((wpr_players[i].unlocked != 0) || (wpr_players[i].books_read != 0) || (i == PLAYER0))
                pmask_add(&wpr_kit_players, i);
        }
    }
    set_tooltips(); /* a run started before a new kind of book has its tooltip too */
    SYNCLOG("World progress: loaded (mode %d)", wpr_mode);
    return true;
}

/******************************************************************************/
/* player levels (design, 2026-11-07: "for every Keeper/Heroes Heart destroyed, Players receive 1000 XP. Max level
   is 10 for now"). Level L+1 needs 1000 x L more: level 2 at 1000 XP, 3 at 3000, 4 at 6000 ... 10 at 45000. The XP
   is with the rewindable statics: the thin clients' stream carries those, and a replay's rewind keeps it. */
#define WPL_XP_PER_HEART 1000
#define WPL_HITS 32
#define WPL_WATCH_TURNS 64 /* what a machine sees in its first turns of a world (a load, a join) is not news */
#define wpl_xp game.world_player_xp /* in struct Game: replays rewind it, thin clients on any platform get it */

/* who hit each heart last (the simulating machine) */
static struct {
    ThingIndex heart;
    int32_t created;
    PlayerNumber by;
} wpl_hits[WPL_HITS];
static int wpl_hits_next;

/* this machine's presentation */
static uint8_t wpl_seen[PLAYERS_MAX];
static int wpl_watch;
static uint8_t crtr_seen[CREATURE_TYPES_MAX]; /* this machine's keeper's creature_allowed, as last seen */
static uint8_t research_seen[WPR_ITEMS];      /* this machine's keeper's open research, as last seen */
static uint8_t wrl_seen_level[WRL_ROOMS], wrl_seen_allowed[WRL_ROOMS]; /* its room levels, as last seen */

static int wpl_level_of(uint32_t xp)
{
    int level = 1;
    while ((level < WORLD_PLAYER_LEVEL_MAX) && (xp >= (uint32_t)WPL_XP_PER_HEART / 2 * (uint32_t)level * (uint32_t)(level + 1)))
        level++;
    return level;
}

uint32_t world_player_xp(PlayerNumber plyr_idx)
{
    return ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX)) ? wpl_xp[plyr_idx] : 0;
}

void world_set_player_xp(PlayerNumber plyr_idx, uint32_t xp)
{
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        wpl_xp[plyr_idx] = xp;
}

int world_player_level(PlayerNumber plyr_idx)
{
    return wpl_level_of(world_player_xp(plyr_idx));
}

/* the players' records for a dedicated server's console (the simulating machine's, kept in the save) */
static struct WorldPlayerStats wps[PLAYERS_MAX];

const struct WorldPlayerStats *world_player_stats(PlayerNumber plyr_idx)
{
    static const struct WorldPlayerStats none;
    return ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX)) ? &wps[plyr_idx] : &none;
}

void world_set_player_stats(PlayerNumber plyr_idx, const struct WorldPlayerStats *st)
{
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && (st != NULL))
        wps[plyr_idx] = *st;
}

void world_player_played_turn(PlayerNumber plyr_idx)
{
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        wps[plyr_idx].play_turns++;
}

void world_add_player_xp(PlayerNumber plyr_idx, uint32_t xp)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    const int before = wpl_level_of(wpl_xp[plyr_idx]);
    wpl_xp[plyr_idx] = (wpl_xp[plyr_idx] > UINT32_MAX - xp) ? UINT32_MAX : (wpl_xp[plyr_idx] + xp);
    const int after = wpl_level_of(wpl_xp[plyr_idx]);
    SYNCLOG("World: player %d was given %u XP: %u XP, level %d", (int)plyr_idx, (unsigned)xp, (unsigned)wpl_xp[plyr_idx], after);
    if (after > before)
    {
        server_admin_player_event(plyr_idx, "%s reached level %d", server_admin_player_name(plyr_idx), after);
        world_perks_level_reached(plyr_idx, after);
    }
}

void world_player_portal_captured(PlayerNumber plyr_idx, int biome)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || !world_player_is_human(plyr_idx))
        return;
    /* a player who isn't here captures nothing (their frozen dungeon coming back is not a capture) */
    if (network_is_active() && (get_player_primary_user(get_player(plyr_idx)) < 0))
        return;
    wps[plyr_idx].portals++;
    SYNCLOG("World: player %d captured a portal (%d so far)", (int)plyr_idx, (int)wps[plyr_idx].portals);
    server_admin_player_event(plyr_idx, "%s captured a portal (%s)", server_admin_player_name(plyr_idx),
        ((biome >= 0) && (biome < WgB_COUNT)) ? wg_biome_name(biome) : "?");
}

void world_progress_seat_reset(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    wpl_xp[plyr_idx] = 0;
    memset(&wps[plyr_idx], 0, sizeof(wps[plyr_idx]));
    memset(&wpr_players[plyr_idx], 0, sizeof(wpr_players[plyr_idx]));
    memset(game.world_room_level[plyr_idx], 0, sizeof(game.world_room_level[plyr_idx]));
    memset(game.world_room_allowed[plyr_idx], 0, sizeof(game.world_room_allowed[plyr_idx]));
    game.world_room_researching[plyr_idx] = 0;
    /* and the mastery bars: the next keeper of the seat began with the last one's (audit 2026-11-11) */
    memset(game.world_room_xp[plyr_idx], 0, sizeof(game.world_room_xp[plyr_idx]));
    wpr_offer_circle[plyr_idx] = 0;
    pmask_del(&wpr_kit_players, plyr_idx); /* the kit is given again when it starts */
    world_misdeeds_strays_seat_reset(plyr_idx); /* Strays: the old keeper's strays go wild again */
    world_watchfire_seat_reset(plyr_idx); /* and the watchfires */
    SYNCLOG("World: player %d's level, XP, records and research progress start again from zero", (int)plyr_idx);
}

/******************************************************************************/
/* passive upgrades, kept by player (see world_progress.h) */
#define WORLD_PERKS_MAX 256
static struct WorldPerks wpk[WORLD_PERKS_MAX];
static int wpk_count;

int world_perks_tokens_to_key(void)
{
    for (int i = 0; i < wpk_count; i++)
        wpk[i].token = netsec_world_token(wpk[i].token);
    return wpk_count;
}

struct WorldPerks *world_perks_of(uint64_t token, TbBool create)
{
    for (int i = 0; i < wpk_count; i++)
        if (wpk[i].token == token)
            return &wpk[i];
    if (!create || (wpk_count >= WORLD_PERKS_MAX))
        return NULL;
    struct WorldPerks *pk = &wpk[wpk_count++];
    memset(pk, 0, sizeof(*pk));
    pk->token = token;
    return pk;
}

void world_perks_seat_started(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    const struct WorldPerks *pk = world_perks_of(world_seat_token(plyr_idx), false);
    if (pk == NULL)
        return;
    int had = 0;
    for (int s = 0; s < WORLD_PERK_SLOTS; s++)
        had += (pk->level[s] != 0) ? 1 : 0;
    /* no upgrade exists yet: each one, when made, takes effect on the seat's dungeon here */
    SYNCLOG("World: player %d keeps %d passive upgrade%s", (int)plyr_idx, had, (had == 1) ? "" : "s");
}

void world_perks_level_reached(PlayerNumber plyr_idx, int level)
{
    (void)plyr_idx;
    (void)level; /* no upgrade is given for a level yet */
}

int world_perks_count(void)
{
    return wpk_count;
}

const struct WorldPerks *world_perks_entry(int i)
{
    return ((i >= 0) && (i < wpk_count)) ? &wpk[i] : NULL;
}

void world_perks_reset(void)
{
    memset(wpk, 0, sizeof(wpk));
    wpk_count = 0;
}

void world_levels_reset(void)
{
    memset(wpl_xp, 0, sizeof(wpl_xp));
    memset(wps, 0, sizeof(wps));
    memset(wpl_hits, 0, sizeof(wpl_hits));
    wpl_hits_next = 0;
    memset(wpl_seen, 0, sizeof(wpl_seen));
    memset(crtr_seen, 0, sizeof(crtr_seen));
    memset(research_seen, 0, sizeof(research_seen));
    memset(wrl_seen_level, 0, sizeof(wrl_seen_level));
    memset(wrl_seen_allowed, 0, sizeof(wrl_seen_allowed));
    wpl_watch = 0;
}

static int wpl_hit_slot(const struct Thing *heartng)
{
    for (int i = 0; i < WPL_HITS; i++)
        if ((wpl_hits[i].heart == heartng->index) && (wpl_hits[i].created == heartng->creation_turn))
            return i;
    return -1;
}

void world_heart_hit(const struct Thing *heartng, PlayerNumber by)
{
    if (!world_mode_active() || !thing_exists(heartng) || (by < 0) || (by >= PLAYERS_COUNT) || (by == heartng->owner))
        return;
    int i = wpl_hit_slot(heartng);
    if (i < 0)
    {
        i = wpl_hits_next;
        wpl_hits_next = (wpl_hits_next + 1) % WPL_HITS;
        wpl_hits[i].heart = heartng->index;
        wpl_hits[i].created = heartng->creation_turn;
    }
    wpl_hits[i].by = by;
}

static const char *wpl_name(PlayerNumber plyr_idx, char *buf, size_t size)
{
    struct PlayerInfo *player = get_player(plyr_idx);
    if (player_exists(player) && (player->player_name[0] != '\0'))
        snprintf(buf, size, "%s", player->player_name);
    else
        snprintf(buf, size, "Keeper %d", (int)plyr_idx + 1);
    return buf;
}

void world_heart_destroyed(const struct Thing *heartng)
{
    if (!world_mode_active() || !thing_exists(heartng))
        return;
    world_outpost_heart_destroyed(heartng); /* an outpost's heart drops its spellbook */
    world_misdeeds_heart_destroyed(heartng);
    const int i = wpl_hit_slot(heartng);
    const PlayerNumber victim = heartng->owner;
    if (i < 0)
    {
        SYNCLOG("World: heart %d of player %d destroyed, by no player", (int)heartng->index, (int)heartng->owner);
        if (world_player_is_human(victim))
            server_admin_player_event(victim, "%s died: their heart was destroyed", server_admin_player_name(victim));
        return;
    }
    const PlayerNumber by = wpl_hits[i].by;
    memset(&wpl_hits[i], 0, sizeof(wpl_hits[i]));
    if (world_player_is_human(victim))
        server_admin_player_event(victim, "%s died: heart destroyed by %s", server_admin_player_name(victim), server_admin_player_name(by));
    /* a keeper's XP: not the heroes' (their creatures, the wild ones) or the neutral player's */
    if ((by < 0) || (by >= PLAYERS_COUNT) || (by == PLAYER_GOOD) || (by == PLAYER_NEUTRAL) || (by == game.neutral_player_num)
        || !player_exists(get_player(by)) || (get_player(by)->victory_state == VicS_LostLevel))
        return;
    const int before = wpl_level_of(wpl_xp[by]);
    wpl_xp[by] += WPL_XP_PER_HEART;
    const int after = wpl_level_of(wpl_xp[by]);
    SYNCLOG("World: player %d destroyed the heart of player %d: %u XP, level %d%s", (int)by, (int)heartng->owner,
        (unsigned)wpl_xp[by], after, (after > before) ? " (up)" : "");
    if (victim == PLAYER_GOOD)
    {
        wps[by].outposts++;
        server_admin_player_event(by, "%s defeated a heroes outpost", server_admin_player_name(by));
    } else
    {
        wps[by].hearts++;
        if (world_player_is_human(victim))
            server_admin_player_event(by, "%s defeated %s", server_admin_player_name(by), server_admin_player_name(victim));
        else
            server_admin_player_event(by, "%s defeated the enemy keeper %s", server_admin_player_name(by), server_admin_player_name(victim));
    }
    if (after > before)
    {
        server_admin_player_event(by, "%s reached level %d", server_admin_player_name(by), after);
        world_perks_level_reached(by, after);
    }
}

void world_progress_present(void)
{
    if (!world_mode_active())
        return;
    static PlayerNumber watched_player = -1;
    if (watched_player != my_player_number)
    {
        watched_player = my_player_number; /* seated (or reseated): what it has now is not news */
        wpl_watch = 0;
    }
    const TbBool news = (wpl_watch >= WPL_WATCH_TURNS);
    if (!news)
        wpl_watch++;
    /* creatures that come to this machine's keeper now (a portal of their biome) */
    struct Dungeon *mine = (my_player_number < PLAYERS_COUNT) ? get_players_num_dungeon(my_player_number) : INVALID_DUNGEON;
    if (!dungeon_invalid(mine))
    {
        for (int k = 0; k < WPR_PORTAL_CREATURES; k++)
        {
            const ThingModel model = portal_creature_model(k);
            if (model <= 0)
                continue;
            const uint8_t now = mine->creature_allowed[model] ? 1 : 0;
            if (news && now && !crtr_seen[model])
            {
                const struct CreatureModelConfig *crconf = creature_stats_get(model);
                message_add_fmt(MsgType_Creature, model, "%s will come to your portals", get_string(crconf->namestr_idx));
            }
            crtr_seen[model] = now;
        }
        /* research opened (a heroes' spellbook) */
        for (int i = 0; i < WPR_ITEMS; i++)
        {
            const uint8_t open = item_research_open(mine, i) ? 1 : 0;
            if (news && open && !research_seen[i])
            {
                switch (wpr_items[i].kind)
                {
                case WprK_Room:
                    message_add_fmt(MsgType_Room, wpr_ids[i], "New research: %s", wpr_items[i].label);
                    break;
                case WprK_Power:
                    message_add_fmt(MsgType_KeeperSpell, wpr_ids[i], "New research: %s", wpr_items[i].label);
                    break;
                default:
                    message_add_fmt(MsgType_Blank, 0, "Your Workshop can make the %s", wpr_items[i].label);
                    break;
                }
            }
            research_seen[i] = open;
        }
        for (int r = 0; (r < WRL_ROOMS) && (my_player_number < PLAYERS_MAX); r++)
        {
            const int level = wrl_level(my_player_number, r), allowed = wrl_allowed(my_player_number, r);
            const int msg_type = WRL_IS_POWER(r) ? MsgType_KeeperSpell : WRL_IS_TRAP(r) ? MsgType_Blank : MsgType_Room;
            const char *what = WRL_IS_POWER(r) ? " spell" : "";
            if (news && (wrl_seen_allowed[r] != 0) && (allowed > wrl_seen_allowed[r]))
                message_add_fmt(msg_type, WRL_IS_TRAP(r) ? 0 : wrl_ids[r], "Your %s%s can be %s to level %d", wrl_labels[r], what,
                    WRL_IS_TRAP(r) ? "built" : "researched", allowed);
            if (news && (wrl_seen_level[r] != 0) && (level > wrl_seen_level[r]))
                message_add_fmt(msg_type, WRL_IS_TRAP(r) ? 0 : wrl_ids[r], "Your %s%s is now level %d", wrl_labels[r], what, level);
            wrl_seen_level[r] = (uint8_t)level;
            wrl_seen_allowed[r] = (uint8_t)allowed;
        }
    }
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        const int level = wpl_level_of(wpl_xp[p]);
        if (news && (wpl_seen[p] != 0) && (level > wpl_seen[p]))
        {
            if (p == my_player_number)
            {
                message_add_fmt(MsgType_Player, p, "Player level %d!", level);
                world_hand_level_news(wpl_seen[p], level);
            }
            else
            {
                char name[32];
                message_add_fmt(MsgType_Player, p, "%s reached player level %d", wpl_name(p, name, sizeof(name)), level);
            }
        }
        wpl_seen[p] = (uint8_t)level;
    }
    world_seasons_turn(); /* every machine: the season's change, told to its keeper */
}

#ifdef __cplusplus
}
#endif

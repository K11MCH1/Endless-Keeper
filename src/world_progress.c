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
#include <math.h>

#include "world_stream.h"
#include "bflib_datetm.h"
#include "bflib_dernc.h"
#include "sprite_recolour.h"
#include "config.h"
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
#include "thing_traps.h"
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
#include "world_wallbreak.h"
#include "net_state.h"
#include "net_game.h"
#include "player_utils.h"
#include "config_magic.h"
#include "bflib_math.h"
#include "world_migrate.h"
#include "world_fill.h"
#include "world_expedition.h"
#include "world_caretaker.h"
#include "creature_states.h"
#include "front_simple.h"
#include "vidmode.h"
#include "engine_render.h"
#include "bflib_video.h"
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
    uint8_t kind_w[4];          /* the book kinds' chances (rooms, spells, traps and doors, duplicates); all 0: the start */
    uint8_t kind_streak;        /* rooms, spells, traps or doors learnt one after another (no duplicate between) */
};

static int wpr_requested;
static int wpr_mode;            /* 0 off, 1 draft, 2 automatic */
static TbBool wpr_kit_given;
static PlayerBitFlags wpr_kit_players; /* seats that have had the kit */
static struct WprPlayer wpr_players[PLAYERS_MAX];
static int wpr_ids[WPR_ITEMS];  /* the item's room, power, trap or door kind (<= 0: not in this game's configs) */
static ThingModel book_model, lore_model;

#define WPR_SAVE_MAGIC 0x31475250u /* "PRG1" */
#define WPR_SAVE_VERSION 7 /* 7: levels by work (2026-11-22): the allowed levels' bytes hold the bars' top bits, the entry
                              researched now is the chosen one; */ /* 2: the seats that had the kit, after the players (16 bits); 3: as a player set (u8 words, the
                              words); 4: then the room levels (count, then per player 4 levels, 4 allowed, the room
                              researched now, the turn its level began waiting); 5: rooms and spells (players, entries,
                              then per player the levels, the allowed levels, the entry researched now, the turn); 6: the book kinds' chances and row, after the levels */

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
/* Levels by work (the user, 2026-11-22): the Library's work fills the bar of the room or spell the keeper chose (the
   Library's spellbook window, world_level_window.c), the Workshop's the bar of the trap it makes, by time (wrl_need_of);
   duplicate books give no levels any more. The Training Room has 20 levels: each opens 5 more creature levels (5 x its
   level, up to 100). */
#define WRL_TRAINING_ENTRY 3
#define WRL_TRAINING_TOP 20
#define WRL_TOP_MAX WRL_TRAINING_TOP
#define WRL_BAR_MAX 0xFFFFFFL /* a bar is 24 bits: world_room_xp (16) and world_room_allowed (8, unused since levels
                                 come by work) together, both in the game's state so that thin clients get it */
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
static TbBool wpr_hound_check; /* (defined with relock_hell_hounds) */
static TbBool wpr_trap_check; /* (defined with repair_trap_looks) */
void world_progress_level_start(void)
{
    world_hand_reset(); /* nobody is hurried in a new level */
    world_wallbreak_reset();
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
    wpr_hound_check = true;
    wpr_trap_check = true;
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
        (strcmp(name, "POWER_IMP") == 0) || (strncmp(name, "POWER_PICKUP_", 13) == 0) ||
        (strcmp(name, "POWER_EXPEDITION") == 0); /* the Expedition spell from the start (world_expedition.c) */
}

static void lock_portal_creatures(PlayerNumber plyr_idx);
static void hell_hound_entrance(void);

static void apply_kit(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    /* The Hell Hound's entrance first. The portal creatures are locked only when no room holds them back, and the
       Hell Hound still asked for a Scavenger Room when the kit was given (its Lair rule came two turns later, with
       ban_powers): it was left out of the lock, and then came from any portal to a Lair of 9 (2026-11-12 to 11-14). */
    hell_hound_entrance();
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
   others come with portals: owning one unlocks the creatures of its biome, where the campaign has them.
   Flies, Beetles and Spiders (the user, 2026-11-15) come from any portal: no biome holds them back, only the rooms
   their entrance asks for (none, a Lair of 1, a Hatchery and a Lair of 9) - wpr_free_creatures */
#define WPR_BIOME(b) (1u << (b))
static const char *const wpr_free_creatures[] = {"FLY", "BUG", "SPIDER"};
#define WPR_FREE_CREATURES ((int)(sizeof(wpr_free_creatures) / sizeof(wpr_free_creatures[0])))
static const struct { const char *name; uint32_t biomes; } wpr_portal_creatures[] = {
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
    /* every raid carries a book (the user, 2026-11-20: raids bring fewer heroes now; it was the first raid of each
       day only, from day 1 - 2026-11-11 - and before that from day 2, one time in two) */
    (void)world_seed;
    if (!world_progress_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    wpr_players[plyr_idx].raid_pending++;
    SYNCLOG("World progress: day %ld's raid %d on player %d carries a research book", day, raid_idx + 1, (int)plyr_idx);
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
/* the books' colours by grade */

static unsigned char wbc_tab[5][256];
/* The tables with the light on them: 4 levels, not the game's 32 - each table drawn is a row of the OpenGL renderer's
   colour table, which holds 128 and is only emptied when the palette changes (5 grades x 32 levels would fill it, and
   sprites needing a new row would lose their colours and shading). Made once each, kept. */
#define WBC_LIGHTS 4 /* dark, dim, lit, bright (2026-11-21: a book in the dark was drawn at a quarter, a faint glow) */
static unsigned char wbc_lit[5][WBC_LIGHTS][256];
static uint8_t wbc_lit_made[5];
static uint32_t wbc_pal_sum = 0;
static TbBool wbc_ready = false;
static unsigned char wbc_mask[256]; /* the cover's colours: what is recoloured */
/* The colours the OpenGL and Vulkan renderers draw (sprite_recolour.c; the user, 2026-11-21: "blue still looks like
   lavender" - the palette has no blue): true white, green, blue, red, purple, from each pixel's own shaded colour. The
   software renderer draws the tables above, the palette's nearest. */
static const struct SpriteRecolour wbc_true[5] = {
    {  0.0f, 0.00f, 0.03f, 0.50f, 0.50f}, /* white: its lines kept */
    {135.0f, 0.42f, 0.04f, 0.62f, 0.36f}, /* green (pastel: the user, 2026-11-21 "make the colors softer, pastel-like") */
    {210.0f, 0.42f, 0.06f, 0.62f, 0.37f}, /* blue */
    {355.0f, 0.40f, 0.05f, 0.62f, 0.37f}, /* red */
    {285.0f, 0.40f, 0.05f, 0.62f, 0.36f}, /* purple */
};

static const unsigned char *wbc_identity(void)
{
    static unsigned char id[256];
    static TbBool made = false;
    if (!made)
    {
        for (int i = 0; i < 256; i++)
            id[i] = (unsigned char)i;
        made = true;
    }
    return id;
}

/** The five tables: the cover's colours (saturated, not the gold trim) take the grade's hue with their own saturation
 *  and brightness, as the nearest colour of the palette; pages, greys and the gold stay (wbc_build). */
/* The palette the tables are made from: the game's own palette file, not the palette in use - a gamma setting
   brightens that one, and tables made from it picked other colours (2026-11-21: with gamma 2 the white books came out
   lilac); the gamma brightens a book's colours as it does all others. The palette in use if the file can't be read. */
static unsigned char wbc_file_pal[768];
static const unsigned char *wbc_P = NULL;
static const unsigned char *wbc_palette(void)
{
    static int loaded = -1;
    if (loaded < 0)
        loaded = (LbFileLoadAt(prepare_file_path(FGrp_StdData, "palette.dat"), wbc_file_pal) == 768) ? 1 : 0;
    return loaded ? wbc_file_pal : engine_palette;
}

static void wbc_hsv(int i, float *hue, float *sat, float *lum)
{
    const float r = wbc_P[3 * i] / 63.0f, g = wbc_P[3 * i + 1] / 63.0f, b = wbc_P[3 * i + 2] / 63.0f;
    const float cmax = max(r, max(g, b)), cmin = min(r, min(g, b));
    *sat = (cmax > 0.0f) ? (cmax - cmin) / cmax : 0.0f;
    float h = 0.0f;
    if (cmax > cmin)
    {
        if (cmax == r)
            h = 60.0f * ((g - b) / (cmax - cmin));
        else if (cmax == g)
            h = 60.0f * (2.0f + (b - r) / (cmax - cmin));
        else
            h = 60.0f * (4.0f + (r - g) / (cmax - cmin));
        if (h < 0.0f)
            h += 360.0f;
    }
    *hue = h;
    *lum = 0.30f * r + 0.59f * g + 0.11f * b;
}

/** A colour from hue (degrees), saturation and value (0-1). */
static void wbc_rgb(float hue, float sat, float val, float *r, float *g, float *b)
{
    const float c = val * sat, hp = hue / 60.0f;
    const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (hp < 1.0f) { r1 = c; g1 = x; }
    else if (hp < 2.0f) { r1 = x; g1 = c; }
    else if (hp < 3.0f) { g1 = c; b1 = x; }
    else if (hp < 4.0f) { g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; b1 = c; }
    else { r1 = c; b1 = x; }
    const float m = val - c;
    *r = r1 + m;
    *g = g1 + m;
    *b = b1 + m;
}

static void wbc_build(void)
{
    wbc_P = wbc_palette();
    if (wbc_P == NULL)
        return;
    /* the palette looked at no more than every 100 ms (2026-11-21: it was hashed for every book drawn, twice a book in
       the 3D view) */
    static TbClockMSec looked = 0;
    const TbClockMSec now = LbTimerClock();
    if (wbc_ready && (now - looked < 100))
        return;
    looked = now;
    uint32_t sum = 2166136261u;
    for (int i = 0; i < 768; i++)
        sum = (sum ^ wbc_P[i]) * 16777619u;
    if (wbc_ready && (sum == wbc_pal_sum))
        return;
    float hue[256], sat[256], lum[256];
    for (int i = 0; i < 256; i++)
        wbc_hsv(i, &hue[i], &sat[i], &lum[i]);
    /* Each cover colour keeps its own saturation and brightness and takes the grade's hue; the nearest colour of the
       whole palette is drawn (2026-11-21, the user: "All the colors are suffering from the same issue as white": each
       grade was matched to one hue family's few shades, many cover shades became one and the book's lines were gone) */
    /* (the palette has no true blue and no mid greys: white is ivory - its whites and the warm beige ramp - and blue
       is the lilac ramp, so each keeps a light-to-dark range for the lines) */
    static const float grade_hue[5] = {45.0f, 120.0f, 268.0f, 2.0f, 300.0f};
    for (int k = 0; k < 5; k++)
    {
        for (int i = 0; i < 256; i++)
        {
            const TbBool keep = (i == 0) || (sat[i] < 0.22f) || ((hue[i] >= 38.0f) && (hue[i] <= 70.0f)); /* (pages, greys, gold) */
            wbc_mask[i] = keep ? 0 : 1;
            if (keep)
            {
                wbc_tab[k][i] = (unsigned char)i;
                continue;
            }
            const float v = max(wbc_P[3 * i], max(wbc_P[3 * i + 1], wbc_P[3 * i + 2])) / 63.0f;
            float s2 = sat[i], v2 = v;
            if (k == 0)
            {
                s2 = 0.12f; /* white: ivory, its light and dark kept */
                v2 = min(1.0f, v * 0.55f + 0.45f);
            } else if (k == 2)
            {
                s2 = 0.24f; /* blue: the lilac ramp (the palette's saturated blues are near black) */
                v2 = min(1.0f, v * 0.65f + 0.33f);
            } else if (k == 3)
                v2 = min(1.0f, v * 1.15f + 0.04f); /* (red: the dark reds read as brown) */
            float r, g, b;
            wbc_rgb(grade_hue[k], s2, v2, &r, &g, &b);
            int best = i;
            float bestd = 1e9f;
            for (int j = 1; j < 256; j++)
            {
                const float dr = wbc_P[3 * j] / 63.0f - r, dg = wbc_P[3 * j + 1] / 63.0f - g,
                    db = wbc_P[3 * j + 2] / 63.0f - b;
                const float ds = sat[j] - s2; /* (and as grey or as coloured: the palette's greys are lilac) */
                const float d = 3.0f * dr * dr + 4.0f * dg * dg + 2.0f * db * db + 1.5f * ds * ds;
                if (d < bestd)
                {
                    bestd = d;
                    best = j;
                }
            }
            wbc_tab[k][i] = (unsigned char)best;
        }
    }
    /* the true colours the OpenGL and Vulkan renderers draw instead (sprite_recolour.c) */
    for (int k = 0; k < 5; k++)
        sprite_recolour_register(wbc_tab[k], wbc_identity(), wbc_mask, &wbc_true[k]);
    memset(wbc_lit_made, 0, sizeof(wbc_lit_made));
    wbc_pal_sum = sum;
    wbc_ready = true;
}

const unsigned char *world_book_draw_remap(const struct Thing *thing, int shade)
{
    if (!world_mode_active() || (book_model <= 0) || !thing_exists(thing) || (thing->class_id != TCls_Object) ||
        ((thing->model != book_model) && (thing->model != lore_model)))
        return NULL;
    const int kind = thing->custom_box.box_kind;
    int grade = 0;
    if ((thing->model == lore_model) && (wrl_of_levelup_kind(kind) >= 0))
    {
        const int it = wrl_items[wrl_of_levelup_kind(kind)];
        grade = (it >= 0) ? wpr_items[it].tier : 1; /* a level's lore book: its item's tier (a starting room's: 1) */
    } else if (kind >= WORLD_LORE_KIND_FIRST)
    {
        const int it = kind - WORLD_LORE_KIND_FIRST;
        grade = ((it >= 0) && (it < WPR_ITEMS)) ? wpr_items[it].tier : 0;
    } else if ((kind >= WORLD_BOOK_KIND_FIRST) && (kind < WORLD_OUTPOST_BOOK_KIND))
        grade = (kind - 128) / 8;
    if (grade <= 0)
        return NULL; /* (a heroes' spellbook keeps the ordinary look) */
    grade = min(grade, 5);
    wbc_build();
    if (!wbc_ready)
        return NULL;
    /* the light, to the nearest of 0, 8, 16, 24 (and 32: none) */
    const int level = (shade < 0) ? WBC_LIGHTS : min(WBC_LIGHTS, max(0, (shade + 4) / 8));
    if (level >= WBC_LIGHTS)
        return wbc_tab[grade - 1];
    const int g = grade - 1;
    if (!(wbc_lit_made[g] & (1u << level)))
    {
        const unsigned char *fade = &pixmap.fade_tables[256 * (8 * level)];
        for (int i = 0; i < 256; i++)
            wbc_lit[g][level][i] = wbc_tab[g][fade[i]]; /* darkened first, then coloured (2026-11-21: the other way round
                                                         the darkening took a pale cover to the palette's lilac greys) */
        sprite_recolour_register(wbc_lit[g][level], fade, wbc_mask, &wbc_true[g]);
        wbc_lit_made[g] |= (uint8_t)(1u << level);
    }
    return wbc_lit[g][level];
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

/** (Levels used to be allowed by books first, then researched; since 2026-11-22 a full bar is the level itself.) */
static int wrl_allowed(PlayerNumber p, int r)
{
    return wrl_level(p, r);
}

static int wrl_top_of(int r)
{
    return (r == WRL_TRAINING_ENTRY) ? WRL_TRAINING_TOP : wrl_top[r];
}

static long wrl_bar(PlayerNumber p, int r)
{
    return (long)wrl_xp[p][r] | ((long)game.world_room_allowed[p][r] << 16);
}

static void wrl_set_bar(PlayerNumber p, int r, long v)
{
    v = max(0L, min(v, WRL_BAR_MAX));
    wrl_xp[p][r] = (uint16_t)(v & 0xFFFF);
    game.world_room_allowed[p][r] = (uint8_t)(v >> 16);
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


static const char *opt_label(int i)
{
    return (i >= WPR_ITEMS) ? wrl_labels[i - WPR_ITEMS] : wpr_items[i].label;
}

/* Levels by time (the user, 2026-11-22, after research points made a late Library level the Training Room in a minute):
   level n takes n x 29 minutes of the Library's work (a trap's: the Workshop making it), at 1x; each creature working
   there makes it faster: +0.2x a Warlock, Vampire or Ghost, +0.5x a Dragon, Wizard, Monk or Fairy, +0.1x any other.
   Nobody working: it waits. (The user: all 20 Training Room levels in about 3.5 days with one Warlock, 1.5 with ten.)
   A bar counts tenths of a turn at 1x (level 20: under 7 million, the bar holds 16.7). */
#define WRL_MINUTES_PER_LEVEL 29
#define WRL_UNITS_PER_MINUTE (60 * 20 * 10)

/** The time level `to` takes, in bar units. */
static long wrl_need_of(PlayerNumber p, int r, int to)
{
    (void)p; (void)r;
    if (to < 2)
        return 0;
    return min((long)to * WRL_MINUTES_PER_LEVEL * WRL_UNITS_PER_MINUTE, WRL_BAR_MAX);
}

/** Time goes into an entry's bar; each full bar is a level, the bar starts again with what was over. */
static void wrl_add_xp(PlayerNumber p, int r, long xp)
{
    if ((xp <= 0) || !wrl_owned(p, r))
        return;
    const int top = wrl_top_of(r);
    if (wrl_level(p, r) >= top)
        return;
    long bar = wrl_bar(p, r) + xp;
    while (wrl_level(p, r) < top)
    {
        const int to = wrl_level(p, r) + 1;
        const long need = wrl_need_of(p, r, to);
        if ((need <= 0) || (bar < need))
            break;
        bar -= need;
        game.world_room_level[p][r] = (uint8_t)to;
        output_player_message(p, WRL_IS_TRAP(r) ? SMsg_ManufacturedTrap : WRL_IS_POWER(r) ? SMsg_ResearchedSpell :
            SMsg_ResearchedRoom, 0);
        SYNCLOG("World progress: player %d's %s is now level %d", (int)p, wrl_labels[r], to);
    }
    wrl_set_bar(p, r, (wrl_level(p, r) >= top) ? 0 : bar);
}

static TbBool wrl_levels_for(PlayerNumber p);
static ThingModel wrl_fast_kinds[8];  /* +0.5x */
static ThingModel wrl_quick_kinds[8]; /* +0.2x */

static void wrl_resolve_workers(void)
{
    static const char *const fast[] = {"DRAGON", "WIZARD", "MONK", "FAIRY"};
    static const char *const quick[] = {"SORCEROR", "VAMPIRE", "GHOST"};
    memset(wrl_fast_kinds, 0, sizeof(wrl_fast_kinds));
    memset(wrl_quick_kinds, 0, sizeof(wrl_quick_kinds));
    for (int k = 0; k < 4; k++)
        wrl_fast_kinds[k] = (ThingModel)max(0L, (long)get_id(creature_desc, fast[k]));
    for (int k = 0; k < 3; k++)
        wrl_quick_kinds[k] = (ThingModel)max(0L, (long)get_id(creature_desc, quick[k]));
}

/** What a creature working adds to the speed, in hundredths. */
static int wrl_worker_bonus(ThingModel model)
{
    for (int k = 0; k < 8; k++)
        if ((wrl_fast_kinds[k] > 0) && (wrl_fast_kinds[k] == model))
            return 50;
    for (int k = 0; k < 8; k++)
        if ((wrl_quick_kinds[k] > 0) && (wrl_quick_kinds[k] == model))
            return 20;
    return 10;
}

int world_level_speed(PlayerNumber p, int where, int *workers)
{
    int n = 0, speed = 0;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if ((p >= 0) && (p < PLAYERS_COUNT) && !dungeon_invalid(dungeon))
    {
        if (wrl_fast_kinds[0] == 0)
            wrl_resolve_workers();
        const CrtrStateId want = (where == 0) ? CrSt_Researching : CrSt_Manufacturing;
        for (ThingIndex i = dungeon->creatr_list_start, guard = 0; (i > 0) && (guard < CREATURES_COUNT); guard++)
        {
            const struct Thing *t = thing_get(i);
            const struct CreatureControl *c = creature_control_get_from_thing(t);
            if (thing_is_invalid(t) || creature_control_invalid(c))
                break;
            i = c->players_next_creature_idx;
            if (get_creature_state_besides_interruptions(t) != want)
                continue;
            n++;
            speed += wrl_worker_bonus(t->model);
        }
    }
    if (workers != NULL)
        *workers = n;
    return (n > 0) ? (100 + speed) : 0;
}

/** Every 4 turns (the simulation): the time the Library and the Workshop worked goes into their bars. */
static void wrl_time_turn(void)
{
    if ((game.play_gameturn % 4) != 1)
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!wrl_levels_for(p) || world_caretaker_active(p) || world_coop_seat_away(p))
            continue;
        const struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon))
            continue;
        const int r = world_level_focus(p);
        if (r >= 0)
        {
            const int sp = world_level_speed(p, 0, NULL);
            if (sp > 0)
                wrl_add_xp(p, r, 4L * sp / 10); /* (speeds are whole tenths) */
        }
        if (dungeon->manufacture_class == TCls_Trap)
        {
            const int t = wrl_of_trap(dungeon->manufacture_kind);
            const int sp = (t >= 0) ? world_level_speed(p, 1, NULL) : 0;
            if (sp > 0)
                wrl_add_xp(p, t, 4L * sp / 10);
        }
    }
}

/** Whose rooms and spells level by time: human keepers and computer keepers alike (the same rules, 2026-11-23). */
static TbBool wrl_levels_for(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || (p == PLAYER_GOOD) || (p == game.neutral_player_num))
        return false;
    return world_player_is_human(p) || player_is_keeper(p);
}

TbBool world_level_set_focus(PlayerNumber p, int entry)
{
    if (!world_mode_active() || !wrl_levels_for(p) || (entry < 0) || (entry >= WRL_FIRST_TRAP) || !wrl_owned(p, entry) ||
        (wrl_level(p, entry) >= wrl_top_of(entry)))
        return false;
    if (game.world_room_researching[p] != (uint8_t)(entry + 1))
    {
        game.world_room_researching[p] = (uint8_t)(entry + 1);
        SYNCLOG("World progress: player %d's Library now works on the %s's levels", (int)p, wrl_labels[entry]);
    }
    return true;
}

void world_level_test_time(PlayerNumber p, long seconds)
{
    const int r = world_level_focus(p);
    if (r >= 0)
        wrl_add_xp(p, r, seconds * 20 * 10);
}

int world_level_focus(PlayerNumber p)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX))
        return -1;
    const int chosen = (int)game.world_room_researching[p] - 1;
    if ((chosen >= 0) && (chosen < WRL_FIRST_TRAP) && wrl_owned(p, chosen) && (wrl_level(p, chosen) < wrl_top_of(chosen)))
        return chosen;
    /* none chosen (or it is at its top): the Training Room, then the first room or spell that can still grow */
    if (wrl_owned(p, WRL_TRAINING_ENTRY) && (wrl_level(p, WRL_TRAINING_ENTRY) < WRL_TRAINING_TOP))
        return WRL_TRAINING_ENTRY;
    for (int r = 0; r < WRL_FIRST_TRAP; r++)
        if (wrl_owned(p, r) && (wrl_level(p, r) < wrl_top_of(r)))
            return r;
    return -1;
}

void world_level_focus_packet(PlayerNumber p, int r)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || (r < 0) || (r >= WRL_FIRST_TRAP) ||
        !wrl_owned(p, r))
        return;
    game.world_room_researching[p] = (uint8_t)(r + 1);
    SYNCLOG("World progress: player %d's Library now works on the %s's levels", (int)p, wrl_labels[r]);
}

/* The Library's leveling window (world_level_window.c): the rooms, then the spells, in the window's order */
static const int wrl_window_rooms[] = {0, 1, 2, 3, 11, 12, 13, 14, 15, 16};
static const int wrl_window_spells[] = {4, 5, 6, 7, 8, 9, 10};

int world_level_entries(PlayerNumber p, int which, struct WorldLevelEntry *out, int max_entries)
{
    const int *list = (which == 0) ? wrl_window_rooms : wrl_window_spells;
    const int n = (which == 0) ? (int)(sizeof(wrl_window_rooms) / sizeof(wrl_window_rooms[0])) :
        (int)(sizeof(wrl_window_spells) / sizeof(wrl_window_spells[0]));
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX))
        return 0;
    const int focus = world_level_focus(p);
    int k = 0;
    for (int i = 0; (i < n) && (k < max_entries); i++)
    {
        const int r = list[i];
        if (wrl_ids[r] <= 0)
            continue;
        struct WorldLevelEntry *e = &out[k++];
        e->entry = r;
        e->kind_class = WRL_IS_POWER(r) ? 1 : 0;
        e->kind = wrl_ids[r];
        e->label = wrl_labels[r];
        e->owned = wrl_owned(p, r);
        e->level = wrl_level(p, r);
        e->top = wrl_top_of(r);
        e->bar = wrl_bar(p, r);
        e->need = (e->level < e->top) ? wrl_need_of(p, r, e->level + 1) : 0;
        e->focus = (r == focus);
        e->chosen = ((int)game.world_room_researching[p] - 1 == r);
    }
    return k;
}

/** What level n of entry r gives, in words (the numbers read from Amenity Mastery's table). */
static void wrl_effect_text(int r, int n, char *out, size_t size)
{
    const struct WrlNumbers *num = wrl_numbers();
    const int rl = max(0, min(n, 3)), pl = max(0, min(n, WRL_MAX));
    switch (r)
    {
    case 0: snprintf(out, size, "creatures heal %d%% faster in the Lair", num->room[WRB_LairHeal][rl]); break;
    case 1: snprintf(out, size, "creatures stay fed %d%% longer, chickens heal %d%% more", num->room[WRB_HatcheryHunger][rl],
        num->room[WRB_HatcheryHeal][rl]); break;
    case 2: snprintf(out, size, "research %d%% faster", num->room[WRB_LibraryResearch][rl]); break;
    case 11: snprintf(out, size, "creatures' pay %d%% lower", num->room[WRB_TreasuryPay][rl]); break;
    case 12: snprintf(out, size, "manufacture %d%% faster", num->room[WRB_WorkshopSpeed][rl]); break;
    case 13: snprintf(out, size, "prisoners starve and turn to skeletons %d%% sooner", num->room[WRB_PrisonSkeleton][rl]); break;
    case 14: snprintf(out, size, "tortured creatures convert %d%% sooner", num->room[WRB_TortureConvert][rl]); break;
    case 15:
        snprintf(out, size, "%s", (num->room[WRB_BarracksMixed][rl] >= 2) ? "the Horned Reaper groups with anyone" :
            (num->room[WRB_BarracksMixed][rl] >= 1) ? "creatures that dislike each other group together" : "no change");
        break;
    case 16: snprintf(out, size, "Vampires need %d corpse%s fewer", num->room[WRB_GraveyardCorpses][rl],
        (num->room[WRB_GraveyardCorpses][rl] == 1) ? "" : "s"); break;
    case 4: snprintf(out, size, "new Imps come %d level%s higher", num->power[WPB_ImpLevels][pl],
        (num->power[WPB_ImpLevels][pl] == 1) ? "" : "s"); break;
    case 5: snprintf(out, size, "sees %d%% wider", num->power[WPB_SightRadius][pl]); break;
    case 6: snprintf(out, size, "creatures move %d%% faster", num->power[WPB_SpeedEffect][pl]); break;
    case 7: snprintf(out, size, "lasts %d seconds longer", num->power[WPB_ConcealTurns][pl] / 20); break;
    case 8: snprintf(out, size, "heals up to %d health", num->power[WPB_HealTop][pl]); break;
    case 9: snprintf(out, size, "%d%% more damage", num->power[WPB_LightningDamage][pl]); break;
    case 10:
        if (num->power[WPB_ProtectTurns][pl] > 0)
            snprintf(out, size, "%d%% more armour, lasts %d seconds longer", num->power[WPB_ProtectArmour][pl],
                num->power[WPB_ProtectTurns][pl] / 20);
        else
            snprintf(out, size, "%d%% more armour", num->power[WPB_ProtectArmour][pl]);
        break;
    default: snprintf(out, size, "stronger"); break;
    }
}

int world_level_entry_lines(PlayerNumber p, int r, char (*lines)[WORLD_LEVEL_LINE], int max_lines)
{
    if ((r < 0) || (r >= WRL_ROOMS) || (max_lines <= 0))
        return 0;
    if (r == WRL_TRAINING_ENTRY)
    {
        int k = 0;
        snprintf(lines[k++], WORLD_LEVEL_LINE, "Each level increases the max Level cap of creatures by 5");
        if (k < max_lines)
            snprintf(lines[k++], WORLD_LEVEL_LINE, "Your creatures can reach level %d now", world_creature_level_cap(p));
        return k;
    }
    int k = 0;
    for (int n = 2; (n <= wrl_top_of(r)) && (k < max_lines); n++)
    {
        char what[WORLD_LEVEL_LINE];
        wrl_effect_text(r, n, what, sizeof(what));
        snprintf(lines[k++], WORLD_LEVEL_LINE, "Level %d: %s", n, what);
    }
    return k;
}

int world_creature_level_cap(PlayerNumber p)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX))
        return CREATURE_MAX_LEVEL_LIMIT;
    return min(CREATURE_MAX_LEVEL_LIMIT, 5 * max(1, wrl_level(p, WRL_TRAINING_ENTRY)));
}

int world_room_bonus(PlayerNumber plyr_idx, enum WorldRoomBonus what)
{
    static const int room_of[WRB_COUNT] = {0, 1, 1, 3, 2, 11, 12, 13, 14, 16, 15};
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX) ||
        ((int)what < 0) || ((int)what >= WRB_COUNT))
        return 0;
    if (what == WRB_TrainingXp)
        return 0; /* the Training Room's levels open creature levels instead (2026-11-22) */
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
    if ((r < 0) || !wrl_owned(plyr_idx, r) || (wrl_level(plyr_idx, r) >= wrl_top_of(r)))
        return false;
    *xp = (int)wrl_bar(plyr_idx, r);
    *need = (int)wrl_need_of(plyr_idx, r, wrl_level(plyr_idx, r) + 1);
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
    *buf++ = (uint8_t)(WRL_ROOMS | 0x80); /* 0x80: bars of levels by work (2026-11-22) */
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
    const int players = buf[0], entries = buf[1] & 0x7F;
    const TbBool by_work = (buf[1] & 0x80) != 0;
    if (len < 2 + (size_t)players * (2 * (size_t)entries + 1))
        return false;
    buf += 2;
    for (int p = 0; p < players; p++)
    {
        if (p < PLAYERS_MAX)
        {
            if (by_work)
                memcpy(wrl_xp[p], buf, 2 * (size_t)min(entries, WRL_ROOMS)); /* (an older run's book bars start again) */
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
    (void)p; /* trap levels come from the Workshop's work on the trap itself (wrl_time_turn, 2026-11-22) */
    return false;
}

static TbBool level_badge(PlayerNumber plyr_idx, int r, int *number, TbBool *reached)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX) || (r < 0))
        return false;
    const int level = wrl_level(plyr_idx, r);
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
    (void)p; (void)required; (void)progress; /* (levels have their own bars since 2026-11-22) */
    return false;
}

/** The Library's work fills the chosen entry's bar by time alongside the "?" research (wrl_time_turn). With no "?"
 *  research left, the Library keeps working for the levels: true then, and the stock code stays off its job. */
TbBool world_room_level_research(PlayerNumber p)
{
    if (!world_mode_active() || !world_progress_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) ||
        !world_player_is_human(p))
        return false;
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon) || (dungeon->research_num <= 0))
        return false;
    const struct ResearchVal *rsrchval = get_players_current_research_val(p);
    if (((rsrchval != NULL) && research_needed(rsrchval, dungeon)) || (get_next_research_item(dungeon) >= 0))
        return false; /* "?" research to do: the stock code works on it */
    if (world_level_focus(p) < 0)
        return false;
    dungeon->current_research_idx = 0; /* the job stays open (creatures research only while something is researched) */
    dungeon->research_progress = 0;
    return true;
}

TbBool world_room_level_sacrifice(PlayerNumber p)
{
    (void)p;
    return false;
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
        if (item_known(plyr_idx, i))
            continue; /* had already (researched, captured), or its research ("?") is open: not worth a book */
        if (i == WPR_ARMAGEDDON)
        {
            if ((grade < 5) || (known < 40))
                continue;
        }
        /* any tier (the user, 2026-11-22: "The Spellbook system was never supposed to grant Players Rooms, Spells and
           Traps in order"): a keeper may get a Prison or Lightning Strike early */
        if ((i == WPR_TORTURE) && !(wp->unlocked & ((uint64_t)1 << WPR_PRISON)) && !item_known(plyr_idx, WPR_PRISON))
            continue; /* the Prison first, from a book or the Library (audit 2026-11-09) */
        out[n++] = i;
    }
    return n; /* (no duplicates: levels come from the Library's and the Workshop's work, 2026-11-22) */
}

/** A book's chosen item after the Library and Training Room (the user, 2026-11-22): not learnt outright, its research
 *  opens ("?"): the Library's creatures research a room or spell, the Workshop's make the first of a trap or door. */
static void research_item(PlayerNumber plyr_idx, int i, const struct Coord3d *where)
{
    open_item_research(plyr_idx, i);
    create_effect(where, TngEff_ResearchComplete, plyr_idx);
    SYNCLOG("World progress: player %d's book opens the %s's research (%s)", (int)plyr_idx, wpr_items[i].label,
        ((wpr_items[i].kind == WprK_Trap) || (wpr_items[i].kind == WprK_Door)) ? "the Workshop makes the first" : "the Library researches it");
}

/** A book's chosen item: its research opened, or a room the keeper has raised a level. */
static void take_option(PlayerNumber plyr_idx, int i, const struct Coord3d *where, int circle)
{
    (void)circle;
    if (i >= WPR_ITEMS)
        SYNCLOG("World progress: player %d's %s level-up book: levels come from work now", (int)plyr_idx, opt_label(i));
    else
        research_item(plyr_idx, i, where);
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
    book_circle(thing); /* (stamped; what a duplicate was worth, before 2026-11-22) */
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
        if (!((wr >= 0) && wrl_owned(plyr_idx, wr)) && !item_known(plyr_idx, i))
            pool[n++] = i; /* (no duplicates since 2026-11-22) */
    }
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
        SYNCLOG("World progress: player %d's heroes' spellbook is the %s, theirs already", (int)plyr_idx, wrl_labels[wr]);
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

/* A book's kind (the user, 2026-11-15): drawn first, by chance, then the book's choices are all of that kind. Rooms,
   spells (keeper powers), traps (with doors: both are the Workshop's), duplicates; spells only once the keeper has the
   Library, traps and doors only once it has the Workshop. A kind with nothing left to teach gives its chance to the
   others. */
enum WprBookKind { WBK_Room, WBK_Spell, WBK_Trap, WBK_Duplicate, WBK_COUNT };
/* The kinds' chances move (the user, 2026-11-20): they start at these; learning a room, spell or trap/door (after the
   Library and Training Room) takes WBK_STEP from its own kind and gives half each to the other two, and each one learnt
   in a row (no duplicate between) adds WBK_DUP_STEP times the row's length to the duplicates; a duplicate learnt sets
   the duplicates back to their start and ends the row, the rest gaining what they lose. No kind below WBK_MIN; the
   four always add up to 100. */
static const int wbk_chance[WBK_COUNT] = {40, 30, 20, 10};
#define WBK_STEP 6
#define WBK_DUP_STEP 2
#define WBK_MIN 5
static const char *const wbk_names[WBK_COUNT] = {"rooms", "spells", "traps and doors", "duplicates"};
#define WPR_WORKSHOP 2

static void wbk_defaults(struct WprPlayer *wp)
{
    if ((wp->kind_w[0] | wp->kind_w[1] | wp->kind_w[2] | wp->kind_w[3]) != 0)
        return;
    for (int b = 0; b < WBK_COUNT; b++)
        wp->kind_w[b] = (uint8_t)wbk_chance[b];
}

/** Floors at WBK_MIN, then scales to 100 (the rounding's rest to the largest). */
static void wbk_normalize(int *w)
{
    for (int b = 0; b < WBK_COUNT; b++)
        w[b] = max(w[b], WBK_MIN);
    int sum = 0;
    for (int b = 0; b < WBK_COUNT; b++)
        sum += w[b];
    int out = 0, big = 0;
    for (int b = 0; b < WBK_COUNT; b++)
    {
        w[b] = max(WBK_MIN, (w[b] * 100 + sum / 2) / sum);
        out += w[b];
        if (w[b] > w[big])
            big = b;
    }
    w[big] += 100 - out;
}

static int wbk_of(int i);

/** A book taught item i (chosen from its lore books, or at once): the kinds' chances move. */
static void wbk_learned(PlayerNumber plyr_idx, int i)
{
    struct WprPlayer *wp = &wpr_players[plyr_idx];
    wbk_defaults(wp);
    int w[WBK_COUNT];
    for (int b = 0; b < WBK_COUNT; b++)
        w[b] = wp->kind_w[b];
    const int k = wbk_of(i);
    if (k == WBK_Duplicate)
    {
        wp->kind_streak = 0;
        w[WBK_Duplicate] = wbk_chance[WBK_Duplicate];
    } else
    {
        w[k] -= WBK_STEP;
        for (int b = 0; b < WBK_Duplicate; b++)
            if (b != k)
                w[b] += WBK_STEP / 2;
        if (wp->kind_streak < 255)
            wp->kind_streak++;
        w[WBK_Duplicate] += WBK_DUP_STEP * wp->kind_streak;
    }
    wbk_normalize(w);
    for (int b = 0; b < WBK_COUNT; b++)
        wp->kind_w[b] = (uint8_t)w[b];
    SYNCLOG("World progress: player %d learnt %s from a book: the kinds' chances now rooms %d, spells %d, traps and doors %d, "
        "duplicates %d (%d in a row)", (int)plyr_idx, opt_label(i), w[0], w[1], w[2], w[3], (int)wp->kind_streak);
}

static int wbk_of(int i)
{
    if (i >= WPR_ITEMS)
        return WBK_Duplicate;
    switch (wpr_items[i].kind)
    {
    case WprK_Power: return WBK_Spell;
    case WprK_Trap:
    case WprK_Door: return WBK_Trap;
    default: return WBK_Room;
    }
}

/** Keeps only the open items of one kind, drawn by wbk_chance among the kinds the keeper may get; returns how many
 *  are left (0: no kind it may get has anything). */
static int book_kind_filter(PlayerNumber plyr_idx, int *open, int n, uint64_t roll)
{
    struct WprPlayer *wp = &wpr_players[plyr_idx];
    wbk_defaults(wp);
    const TbBool library = (wp->unlocked & ((uint64_t)1 << WPR_LIBRARY)) || item_had(plyr_idx, WPR_LIBRARY);
    const TbBool workshop = (wp->unlocked & ((uint64_t)1 << WPR_WORKSHOP)) || item_had(plyr_idx, WPR_WORKSHOP);
    int count[WBK_COUNT] = {0};
    for (int k = 0; k < n; k++)
        count[wbk_of(open[k])]++;
    if (!library)
        count[WBK_Spell] = 0;
    if (!workshop)
        count[WBK_Trap] = 0;
    int total = 0;
    for (int b = 0; b < WBK_COUNT; b++)
        total += (count[b] > 0) ? wp->kind_w[b] : 0;
    if (total <= 0)
        return 0;
    int pick = (int)(wpr_mix(roll ^ 0x4B494E44ULL) % (uint64_t)total), kind = 0;
    for (int b = 0; b < WBK_COUNT; b++)
    {
        if (count[b] <= 0)
            continue;
        kind = b;
        if (pick < wp->kind_w[b])
            break;
        pick -= wp->kind_w[b];
    }
    int m = 0;
    for (int k = 0; k < n; k++)
        if (wbk_of(open[k]) == kind)
            open[m++] = open[k];
    SYNCLOG("World progress: player %d's book is of %s (%d of them open; library %d, workshop %d; chances %d/%d/%d/%d)",
        (int)plyr_idx, wbk_names[kind], m, (int)library, (int)workshop, wp->kind_w[0], wp->kind_w[1], wp->kind_w[2], wp->kind_w[3]);
    return m;
}

/** A book's choices (the user, 2026-11-22): two of its kind, drawn at random, any tier. */
#define WPR_CHOICES 2
static int choose_options(const int *open, int n, int biome, uint64_t roll, int *opt)
{
    (void)biome;
    int count = 0;
    while ((count < WPR_CHOICES) && (count < n))
    {
        roll = wpr_mix(roll + (uint64_t)count + 1);
        int cand[WPR_OPTIONS];
        int m = 0;
        for (int k = 0; k < n; k++)
        {
            TbBool taken = false;
            for (int c = 0; c < count; c++)
                taken = taken || (opt[c] == open[k]);
            if (!taken)
                cand[m++] = open[k];
        }
        if (m == 0)
            break;
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
        {
            SYNCLOG("World progress: player %d's %s level-up book: levels come from work now", (int)plyr_idx,
                opt_label(item));
        } else
        if (item_usable(item) && !wpr_item_disabled(item) && !(wp->unlocked & ((uint64_t)1 << item)) && !item_known(plyr_idx, item))
        {
            research_item(plyr_idx, item, &pos);
            wbk_learned(plyr_idx, item);
        }
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
    const int nk = book_kind_filter(plyr_idx, open, n, roll);
    if (nk == 0)
    {
        give_boon(plyr_idx, grade, roll); /* nothing of a kind it may get yet */
        return true;
    }
    int opt[3] = {0};
    const int k = choose_options(open, nk, biome, roll, opt);
    static int automatic = -1;
    if (automatic < 0)
        automatic = ((getenv("KFX_WORLD_PROGRESS") != NULL) && (strcmp(getenv("KFX_WORLD_PROGRESS"), "auto") == 0)) ? 1 : 0;
    if ((wpr_mode == 2) || automatic || (k == 1) || (lore_model <= 0))
    {
        take_option(plyr_idx, opt[0], &pos, circle);
        wbk_learned(plyr_idx, opt[0]);
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
    {
        take_option(plyr_idx, opt[0], &pos, circle);
        wbk_learned(plyr_idx, opt[0]);
    }
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
            /* (grade 0: grades 1 to 5 in turn, to see their colours side by side) */
            world_progress_drop_books(heart->mappos.x.stl.num + 3 + (i % 5), heart->mappos.y.stl.num + 3 + (i / 5),
                (grade > 0) ? grade : 1 + (i % 5), i % WgB_COUNT, 1);
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
            wrl_add_xp(p, r, (traps && (r == WRL_FIRST_TRAP + 1)) ? 10 : WRL_BAR_MAX);
            (void)pos;
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
            all = all && (wrl_level(p, r) >= wrl_top_of(r));
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
    game.world_room_level[p][WRL_FIRST_TRAP + 0] = (uint8_t)wrl_top_of(WRL_FIRST_TRAP + 0);
    game.world_room_level[p][WRL_FIRST_TRAP + 1] = (uint8_t)wrl_top_of(WRL_FIRST_TRAP + 1);
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

/** Worlds begun before the fix above have Hell Hounds allowed for keepers who own no portal in volcanic ground: they
 *  are locked again (the Hell Hounds a keeper has stay). Looked at once after each start or load of a world, when the
 *  world's portals are known. A keeper who once owned such a portal and lost it to another keeper is locked too:
 *  the save doesn't say who owned what before. */
static TbBool wpr_hound_check = true;
static void relock_hell_hounds(void)
{
    const long model = get_id(creature_desc, "HELL_HOUND");
    if ((model <= 0) || (model >= game.conf.crtr_conf.model_count) || creature_room_locked((ThingModel)model))
        return;
    uint32_t biomes = 0;
    for (int k = 0; k < WPR_PORTAL_CREATURES; k++)
        if (strcmp(wpr_portal_creatures[k].name, "HELL_HOUND") == 0)
            biomes = wpr_portal_creatures[k].biomes;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!pmask_has(wpr_kit_players, p) || !world_player_is_human(p) || !player_exists(get_player(p)))
            continue;
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon) || !dungeon->creature_allowed[model] || world_player_owns_portal_in(p, biomes))
            continue;
        dungeon->creature_allowed[model] = 0;
        SYNCLOG("World progress: player %d's Hell Hounds wait for a portal in volcanic ground again (they had been left out of the lock)", (int)p);
    }
}

/** No computer assistant in endless worlds (2026-11-15, computer_assistant_allowed): one a human keeper had switched
 *  on before goes off, once after each start or load. */
static void assistants_off(void)
{
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!world_player_is_human(p) || !player_exists(get_player(p)) ||
            flag_is_set(get_player(p)->allocflags, PlaF_CompCtrl))
            continue; /* (an away keeper's caretaker is a computer keeper of its own: world_caretaker.c) */
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon) || ((dungeon->computer_enabled & 0x01) == 0))
            continue;
        dungeon->computer_enabled &= ~0x01;
        SYNCLOG("World progress: player %d's computer assistant is switched off (none in endless worlds)", (int)p);
    }
}

/** Worlds begun before 2026-11-15 locked Flies, Beetles and Spiders until a portal of their biomes: they are allowed
 *  again for every keeper who got the kit (wpr_free_creatures). Once after each start or load, with the Hell Hounds. */
static void unlock_free_creatures(void)
{
    for (int k = 0; k < WPR_FREE_CREATURES; k++)
    {
        const long model = get_id(creature_desc, wpr_free_creatures[k]);
        if ((model <= 0) || (model >= game.conf.crtr_conf.model_count))
            continue;
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            if (!pmask_has(wpr_kit_players, p) || !world_player_is_human(p) || !player_exists(get_player(p)))
                continue;
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            if (dungeon_invalid(dungeon) || dungeon->creature_allowed[model])
                continue;
            dungeon->creature_allowed[model] = 1;
            SYNCLOG("World progress: player %d's portals bring %s in any biome now", (int)p, wpr_free_creatures[k]);
        }
    }
}

/** Until 2026-11-15 the world gave a trap it brought back its charges without the look of an armed trap, so the
 *  outposts' traps (and a keeper's own, once its ground had been put away and brought back) were drawn see-through,
 *  as empty traps are. New ones are right from the start (world_stream.c); the ones a save still holds are put right
 *  here, once after each start or load: a trap with charges gets the look the game gives it when it is armed. */
static TbBool wpr_trap_check = true;
static void repair_trap_looks(void)
{
    const struct StructureList *slist = get_list_for_thing_class(TCls_Trap);
    long i = slist->index, guard = 0;
    int charged = 0, mended = 0;
    while ((i != 0) && (guard++ <= THINGS_COUNT))
    {
        struct Thing *traptng = thing_get(i);
        if (thing_is_invalid(traptng))
            break;
        i = traptng->next_of_class;
        if (traptng->trap.num_shots == 0)
            continue; /* empty: see-through is its look */
        charged++;
        const struct TrapConfigStats *trapst = get_trap_model_stats(traptng->model);
        if ((traptng->rendering_flags & TRF_Transpar_Flags) == (trapst->transparency_flag & TRF_Transpar_Flags))
            continue;
        set_trap_shots(traptng, traptng->trap.num_shots);
        mended++;
    }
    if (charged > 0)
        SYNCLOG("World: %d traps with charges on the loaded ground; %d of them looked empty and look armed again", charged,
            mended);
}

static void wimp_test_turn(void);
TbBool world_progress_book_teaches(PlayerNumber plyr_idx, PowerKind pwkind)
{
    if (!world_progress_active() || !world_player_is_human(plyr_idx))
        return true;
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    return dungeon_invalid(dungeon) || (dungeon->magic_level[pwkind] > 0);
}

/** Computer keepers know what the human keeper nearest them knows (the user, 2026-11-18: "Enemy Keepers are currently a
 *  massive source for unlocking rooms, traps, spells. Make sure they work within the same rules as the Player. When
 *  Enemy Keepers spawn, they just spawn with the same unlocked rooms, spells and traps as the Player and also
 *  automatically unlock them as Player progresses"). The level script gave them everything: a keeper's Library held
 *  the books of every spell and its Workshop made every trap, for the player to take. Each computer keeper's rooms,
 *  spells, traps and doors are set to those of the human keeper whose heart is nearest its own, now and whenever that
 *  keeper learns something; its creatures are its own (pools and biomes). */
static void mirror_computer_keepers(void)
{
    for (PlayerNumber c = 0; c < PLAYERS_COUNT; c++)
    {
        if ((c == PLAYER_GOOD) || (c == game.neutral_player_num) || world_player_is_human(c) || !player_exists(get_player(c)))
            continue;
        struct Dungeon *cd = get_players_num_dungeon(c);
        struct Thing *cheart = find_players_dungeon_heart(c);
        if (dungeon_invalid(cd) || !thing_exists(cheart))
            continue;
        /* the human keeper nearest it */
        PlayerNumber h = -1;
        long best = LONG_MAX;
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            if (!world_player_is_human(p) || !player_exists(get_player(p)))
                continue;
            struct Thing *hh = find_players_dungeon_heart(p);
            if (!thing_exists(hh))
                continue;
            const long dx = (long)hh->mappos.x.stl.num - cheart->mappos.x.stl.num, dy = (long)hh->mappos.y.stl.num - cheart->mappos.y.stl.num;
            const long d = dx * dx + dy * dy;
            if (d < best)
            {
                best = d;
                h = p;
            }
        }
        if (h < 0)
            continue;
        const struct Dungeon *hd = get_players_num_dungeon(h);
        if (dungeon_invalid(hd))
            continue;
        int rooms = 0, powers = 0, traps = 0;
        for (RoomKind r = 1; r < game.conf.slab_conf.room_types_count; r++)
        {
            const long want_res = hd->room_resrchable[r];
            const long want_av = hd->room_buildable[r] & 1;
            if ((cd->room_resrchable[r] != want_res) || ((cd->room_buildable[r] & 1) != want_av))
            {
                set_room_available(c, r, want_res, want_av);
                rooms++;
            }
        }
        for (PowerKind pw = 1; pw < game.conf.magic_conf.power_types_count; pw++)
        {
            const TbBool want_av = (hd->magic_level[pw] > 0), have_av = (cd->magic_level[pw] > 0);
            if ((cd->magic_resrchable[pw] != hd->magic_resrchable[pw]) || (want_av != have_av))
            {
                set_power_available(c, pw, hd->magic_resrchable[pw], want_av ? 1 : 0);
                powers++;
            }
        }
        for (int k = 1; k < TRAPDOOR_TYPES_MAX; k++)
        {
            const unsigned char tw = hd->mnfct_info.trap_build_flags[k] & MnfBldF_Manufacturable;
            if ((cd->mnfct_info.trap_build_flags[k] & MnfBldF_Manufacturable) != tw)
            {
                cd->mnfct_info.trap_build_flags[k] = (unsigned char)((cd->mnfct_info.trap_build_flags[k] & ~MnfBldF_Manufacturable) | tw);
                traps++;
            }
            const unsigned char dw = hd->mnfct_info.door_build_flags[k] & MnfBldF_Manufacturable;
            if ((cd->mnfct_info.door_build_flags[k] & MnfBldF_Manufacturable) != dw)
            {
                cd->mnfct_info.door_build_flags[k] = (unsigned char)((cd->mnfct_info.door_build_flags[k] & ~MnfBldF_Manufacturable) | dw);
                traps++;
            }
        }
        if (rooms + powers + traps > 0)
            SYNCLOG("World progress: computer keeper %d knows what player %d knows: %d rooms, %d spells, %d traps and"
                " doors changed", (int)c, (int)h, rooms, powers, traps);
    }
}

void world_progress_turn(void)
{
    wimp_test_turn();
    if (world_progress_active() && ((game.play_gameturn % 40) == 11))
        mirror_computer_keepers();
    if (world_mode_active() && ((game.play_gameturn % 64) == 2))
    {
        ban_powers();
        if (wpr_trap_check)
        {
            wpr_trap_check = false;
            repair_trap_looks();
        }
        if (wpr_hound_check && world_progress_active())
        {
            wpr_hound_check = false;
            relock_hell_hounds();
            unlock_free_creatures();
            assistants_off();
        }
    }
    wp_poison_turn();
    test_traps();
    world_storage_turn();
    world_hand_test_turn();
    world_watchfire_turn();
    world_dark_turn();
    world_record_turn(); /* the Keeper panel's Time Played */
    world_migrate_turn(); /* Move The Heart */
    world_fill_turn(); /* the Fill room */
    world_expedition_turn(); /* the Expedition spell */
    world_wallbreak_turn(); /* Beetles and raid Tunnellers break fortified walls */
    if (!world_progress_active())
        return;
    test_books();
    test_read_books();
    test_room_levels();
    wrl_time_turn(); /* levels by time (2026-11-22) */
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
    return 4 + 4 + 1 + 1 + PLAYERS_COUNT * sizeof(struct WprSaved) + 1 + 4 * wpr_mask_words() + 2 + PLAYERS_COUNT * (2 * WRL_ROOMS + 5) +
        1 + PLAYERS_COUNT * 5;
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
    /* version 6: the book kinds' chances and row */
    *buf++ = (uint8_t)PLAYERS_COUNT;
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        memcpy(buf, wpr_players[p].kind_w, 4); buf += 4;
        *buf++ = wpr_players[p].kind_streak;
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
    wpr_hound_check = true; /* (see relock_hell_hounds) */
    wpr_trap_check = true; /* (see repair_trap_looks) */
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
                    game.world_room_level[pl][r] = (uint8_t)min((int)e[r], WRL_TOP_MAX);
                    game.world_room_allowed[pl][r] = (version >= 7) ? e[n + r] : 0; /* (an older run's bars start again) */
                }
                game.world_room_researching[pl] = ((version >= 7) && (e[2 * n] <= WRL_FIRST_TRAP)) ? e[2 * n] : 0;
                memcpy(&wpr_players[pl].lvl_pending_since, e + 2 * n + 1, 4);
            }
            const uint8_t *kw = lv + 2 + (size_t)lv[0] * stride;
            const size_t kw_len = lv_len - (2 + (size_t)lv[0] * stride);
            if ((version >= 6) && (kw_len >= 1) && (kw_len >= 1 + (size_t)kw[0] * 5))
            {
                for (int pl = 0; (pl < kw[0]) && (pl < PLAYERS_COUNT); pl++)
                {
                    memcpy(wpr_players[pl].kind_w, kw + 1 + (size_t)pl * 5, 4);
                    wpr_players[pl].kind_streak = kw[1 + (size_t)pl * 5 + 4];
                }
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
                    game.world_room_allowed[pl][r] = 0;
                }
                game.world_room_researching[pl] = 0;
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
#define wps game.world_player_record /* in struct Game: a thin client's Keeper panel shows it */

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

/* The Keeper panel's statistics (2026-11-19), counted on the simulating machine and seen everywhere (struct Game). */
static ThingIndex wrec_lost_heart[PLAYERS_MAX];   /* the last Heart counted as lost (one Heart is reported twice) */
static GameTurn wrec_lost_made[PLAYERS_MAX];

static TbBool wrec_counts(PlayerNumber p)
{
    return world_mode_active() && !netstate_is_client() && (p >= 0) && (p < PLAYERS_COUNT) && (p < PLAYERS_MAX) &&
        world_player_is_human(p);
}

uint32_t world_player_level_xp(int level)
{
    if (level <= 1)
        return 0;
    return (uint32_t)WPL_XP_PER_HEART / 2 * (uint32_t)(level - 1) * (uint32_t)level;
}

void world_record_heart_lost(const struct Thing *heartng)
{
    if (!thing_exists(heartng))
        return;
    const PlayerNumber p = heartng->owner;
    if (!wrec_counts(p) || ((wrec_lost_heart[p] == heartng->index) && (wrec_lost_made[p] == heartng->creation_turn)))
        return;
    wrec_lost_heart[p] = heartng->index;
    wrec_lost_made[p] = heartng->creation_turn;
    wps[p].hearts_lost++;
    SYNCLOG("World: player %d lost its Heart (%u lost in all)", (int)p, (unsigned)wps[p].hearts_lost);
}

void world_record_kill(const struct Thing *victim, PlayerNumber killer)
{
    if (!wrec_counts(killer) || !thing_exists(victim) || (victim->owner == killer))
        return;
    if (victim->owner == PLAYER_GOOD)
        wps[killer].heroes_killed++;
    else if (!players_creatures_tolerate_each_other(killer, victim->owner))
        wps[killer].creatures_killed++;
}

void world_record_dug(PlayerNumber p, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!wrec_counts(p))
        return;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const long dx = (long)slb_x - (long)subtile_slab(heart->mappos.x.stl.num);
    const long dy = (long)slb_y - (long)subtile_slab(heart->mappos.y.stl.num);
    const uint32_t d = (uint32_t)LbSqrL(dx * dx + dy * dy);
    if (d > wps[p].dug_farthest)
        wps[p].dug_farthest = d;
}

void world_record_portrait(PlayerNumber p, ThingModel model)
{
    if ((p < 0) || (p >= PLAYERS_MAX) || (model <= 0) || (model >= game.conf.crtr_conf.model_count))
        return;
    wps[p].portrait = (uint16_t)model;
    SYNCLOG("World: player %d's portrait is the %s", (int)p, creature_code_name(model));
}

/** Time Played: every turn a keeper is here (single player: this machine's; a server: the seats a user holds). */
static void wrec_play_time(void)
{
    if (!world_mode_active() || netstate_is_client())
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        const struct PlayerInfo *player = get_player(p);
        if (!world_player_is_human(p) || !player_exists(player) || player_is_placeholder(player) ||
            (player->victory_state == VicS_LostLevel))
            continue;
        if (network_is_active())
        {
            const NetUserId u = get_player_primary_user(player);
            if ((u < 0) || (u >= MAX_NET_USERS) || (netstate_dedicated() && (u == SERVER_ID)))
                continue;
        } else if (p != my_player_number)
            continue;
        wps[p].play_turns++;
    }
}

void world_record_turn(void)
{
    wrec_play_time();
}

/* REC2 (an extra of the world save): the record's fields that came with the Keeper panel (PST1 has the first four) */
size_t world_record_save_size(void)
{
    uint32_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if ((wps[p].hearts_lost | wps[p].heroes_killed | wps[p].creatures_killed | wps[p].dug_farthest | wps[p].portrait) != 0)
            n++;
    return (n == 0) ? 0 : (4 + n * 6 * 4);
}

void world_record_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *at = buf + 4;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        const struct WorldPlayerStats *st = &wps[p];
        if ((st->hearts_lost | st->heroes_killed | st->creatures_killed | st->dug_farthest | st->portrait) == 0)
            continue;
        const uint32_t v[6] = {(uint32_t)p, st->hearts_lost, st->heroes_killed, st->creatures_killed, st->dug_farthest,
            st->portrait};
        memcpy(at, v, sizeof(v));
        at += sizeof(v);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_record_load(const uint8_t *buf, size_t len)
{
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * 24 + 4 > len)
        return false;
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t v[6];
        memcpy(v, buf + 4 + k * 24, sizeof(v));
        if (v[0] >= PLAYERS_MAX)
            continue;
        struct WorldPlayerStats *st = &wps[v[0]];
        st->hearts_lost = v[1];
        st->heroes_killed = v[2];
        st->creatures_killed = v[3];
        st->dug_farthest = v[4];
        st->portrait = (uint16_t)v[5];
    }
    return true;
}

void world_record_reset(void)
{
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        wps[p].hearts_lost = 0;
        wps[p].heroes_killed = 0;
        wps[p].creatures_killed = 0;
        wps[p].dug_farthest = 0;
        wps[p].portrait = 0;
    }
    memset(wrec_lost_heart, 0, sizeof(wrec_lost_heart));
    memset(wrec_lost_made, 0, sizeof(wrec_lost_made));
}

void world_progress_seat_reset(PlayerNumber plyr_idx, TbBool keep_record)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    /* the user, 2026-11-19: level, XP and the Profile's statistics are a lifetime record ("Hearts Lost keeps
       counting"): a keeper starting again keeps them; another player taking the seat starts from zero */
    if (!keep_record)
    {
        wpl_xp[plyr_idx] = 0;
        memset(&wps[plyr_idx], 0, sizeof(wps[plyr_idx]));
    }
    memset(&wpr_players[plyr_idx], 0, sizeof(wpr_players[plyr_idx]));
    memset(game.world_room_level[plyr_idx], 0, sizeof(game.world_room_level[plyr_idx]));
    memset(game.world_room_allowed[plyr_idx], 0, sizeof(game.world_room_allowed[plyr_idx]));
    game.world_room_researching[plyr_idx] = 0;
    /* and the mastery bars: the next keeper of the seat began with the last one's (audit 2026-11-11) */
    memset(game.world_room_xp[plyr_idx], 0, sizeof(game.world_room_xp[plyr_idx]));
    wpr_offer_circle[plyr_idx] = 0;
    world_migrate_seat_reset(plyr_idx); /* no migration going on, and no moved Heart */
    world_fill_seat_reset(plyr_idx); /* and no Fill room marks */
    world_expedition_seat_reset(plyr_idx); /* its expeditions' Imps back, and no question marks */
    world_spawn_portal_reset(plyr_idx); /* its first portal is chosen again */
    pmask_del(&wpr_kit_players, plyr_idx); /* the kit is given again when it starts */
    world_misdeeds_strays_seat_reset(plyr_idx); /* Strays: the old keeper's strays go wild again */
    world_watchfire_seat_reset(plyr_idx); /* and the watchfires */
    SYNCLOG("World: player %d's research progress starts again from zero; level, XP and records %s", (int)plyr_idx,
        keep_record ? "kept" : "too");
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
    world_record_heart_lost(heartng); /* the Keeper panel's Hearts Lost */
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

/******************************************************************************/
/* Imps (the user, 2026-11-15): at most WORLD_MAX_IMPS a keeper; Create Imp at that many is refused with a word in the
   notifications. */

#define WIMP_TEXT "You have reached the max allowed imps."
#define WIMP_NOTE_MARK "\x01IC"

int world_imp_count(PlayerNumber plyr_idx)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    return dungeon_invalid(dungeon) ? 0 : (int)dungeon->num_active_diggers;
}

/** On this machine: the refusal in the notifications, once (clicking again keeps the one there, for longer). */
static void wimp_tell_here(int power_kind)
{
    for (int i = 0; i < game.active_messages_count; i++)
        if ((game.messages[i].target_idx == my_player_number) && (strcmp(game.messages[i].text, WIMP_TEXT) == 0))
        {
            game.messages[i].expiration_turn = get_gameturn() + GUI_MESSAGES_DELAY;
            return;
        }
    targeted_message_add(MsgType_KeeperSpell, power_kind, my_player_number, GUI_MESSAGES_DELAY, "%s", WIMP_TEXT);
}

TbBool world_imp_cap_refuses(PlayerNumber plyr_idx, int power_kind)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return false;
    const struct PowerConfigStats *powerst = get_power_model_stats(power_kind);
    if ((powerst == NULL) || (powerst->creature_model != get_players_special_digger_model(plyr_idx)))
        return false;
    const int imps = world_imp_count(plyr_idx);
    if (imps < WORLD_MAX_IMPS)
        return false;
    SYNCLOG("World: player %d has %d Imps (at most %d): Create Imp refused", (int)plyr_idx, imps, WORLD_MAX_IMPS);
    if (plyr_idx == my_player_number)
        wimp_tell_here(power_kind);
    else if (netstate_is_server())
    {
        /* the keeper plays on another machine: the server tells it (world_imp_net_notice there) */
        const NetUserId u = get_player_primary_user(get_player(plyr_idx));
        if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        {
            char text[16];
            snprintf(text, sizeof(text), WIMP_NOTE_MARK "%d", (int)power_kind);
            server_admin_notice(u, text);
        }
    }
    return true;
}

/** Test hook KFX_WORLD_IMPTEST=T: from turn T, every 20 turns, player 0 casts Create Imp (free) beside its heart, ten
 *  times; each try is logged with the Imp count before and after. */
static void wimp_test_turn(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_IMPTEST");
        at = (v != NULL) ? atol(v) : -1;
    }
    const long t = (long)game.play_gameturn - at;
    if ((at < 0) || (t < 0) || (t > 200) || ((t % 20) != 0))
        return;
    struct Thing *heartng = get_player_soul_container(PLAYER0);
    if (!thing_exists(heartng))
        return;
    const int before = world_imp_count(PLAYER0);
    get_player(PLAYER0)->power_of_cooldown_turn = 0;
    const TbResult r = magic_use_power_on_subtile(PLAYER0, PwrK_MKDIGGER, 0, heartng->mappos.x.stl.num,
        heartng->mappos.y.stl.num + 2, PwCast_None, PwMod_CastForFree);
    SYNCLOG("World imp test: try %ld: %d Imps before, %d after (%s)", t / 20 + 1, before, world_imp_count(PLAYER0),
        (r == Lb_FAIL) ? "refused" : "made");
}

TbBool world_imp_net_notice(const char *text)
{
    if ((text == NULL) || (strncmp(text, WIMP_NOTE_MARK, 3) != 0))
        return false;
    wimp_tell_here(atoi(text + 3));
    return true;
}

GoldAmount world_level_scaled_gold(GoldAmount base, int exp_level)
{
    int64_t v = (int64_t)base * 10000; /* in ten-thousandths, so that small wages grow too */
    for (int i = 0; i < exp_level; i++)
        v = v * 102 / 100;
    v = (v + 5000) / 10000;
    return (GoldAmount)((v > 32767) ? 32767 : v); /* (as the game's own formula saturates it) */
}

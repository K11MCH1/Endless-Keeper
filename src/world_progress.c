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
#include "world_gen.h"
#include "world_daynight.h"
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
    return (i == WPR_ARMAGEDDON) || ((i >= 0) && (i < WPR_ITEMS) && (strcmp(wpr_items[i].name, "POWER_DESTROY_WALLS") == 0));
}

/** A human keeper's progression. */
struct WprPlayer {
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
#define WPR_SAVE_VERSION 3 /* 2: the seats that had the kit, after the players (16 bits); 3: as a player set (u8 words, the words) */

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

void world_progress_level_start(void)
{
    memset(wpr_players, 0, sizeof(wpr_players));
    resolve_ids();
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
    biome = max(0, min(biome, WgB_COUNT - 1));
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
        for (int b = 0; b < WgB_COUNT; b++)
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
        if ((wpr_portal_creatures[k].biomes & WPR_BIOME(biome)) == 0)
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
    if (!world_progress_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (day < 2) || (raid_idx != 0))
        return;
    if ((wpr_mix(world_seed ^ ((uint64_t)day << 8) ^ (uint64_t)plyr_idx ^ 0x5241494442ULL) & 1u) == 0)
        return;
    wpr_players[plyr_idx].raid_pending++;
    SYNCLOG("World progress: day %ld's first raid on player %d carries a research book", day, (int)plyr_idx);
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
    const long day = world_day_number();
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
        if ((i == WPR_TORTURE) && !(wp->unlocked & ((uint64_t)1 << WPR_PRISON)))
            continue;
        out[n++] = i;
    }
    return n;
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
    destroy_book(thing);
    if (item_usable(WPR_LIBRARY) && !library_buildable(plyr_idx))
    {
        unlock_item(plyr_idx, WPR_LIBRARY, &pos);
        return;
    }
    /* a random room, power, trap or door: one the keeper has already is possible */
    int pool[WPR_ITEMS], n = 0;
    for (int i = 0; i < WPR_ITEMS; i++)
        if (item_usable(i) && !wpr_item_disabled(i))
            pool[n++] = i;
    if (n == 0)
        return;
    const int i = pool[roll % (uint64_t)n];
    create_effect(&pos, TngEff_ResearchComplete, plyr_idx);
    if (item_known(plyr_idx, i))
    {
        SYNCLOG("World progress: player %d's heroes' spellbook is the %s, known already", (int)plyr_idx, wpr_items[i].label);
        return;
    }
    open_item_research(plyr_idx, i);
    SYNCLOG("World progress: player %d's heroes' spellbook opens the %s's research", (int)plyr_idx, wpr_items[i].label);
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
        top = max(top, (int)wpr_items[open[k]].tier);
    for (int pass = 0; (pass < 3) && (count < 3); pass++)
    {
        int cand[WPR_ITEMS];
        int m = 0;
        for (int k = 0; k < n; k++)
        {
            const int i = open[k];
            TbBool taken = false;
            for (int c = 0; c < count; c++)
                taken = taken || (opt[c] == i);
            if (taken)
                continue;
            if ((pass == 0) && (wpr_items[i].tier != top))
                continue;
            if ((pass == 1) && (wpr_items[i].biome != biome))
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
    const TbBool is_lore = (kind >= WORLD_LORE_KIND_FIRST);
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
        const int item = kind - WORLD_LORE_KIND_FIRST;
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
    wp->books_read++;
    wp->best_grade = (uint8_t)max((int)wp->best_grade, grade);
    const uint64_t roll = wpr_mix(world_seed() ^ ((uint64_t)plyr_idx << 56) ^ ((uint64_t)wp->books_read << 24) ^ 0x424F4F4BULL);
    int open[WPR_ITEMS];
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
    int opt[3];
    const int k = choose_options(open, n, biome, roll, opt);
    static int automatic = -1;
    if (automatic < 0)
        automatic = ((getenv("KFX_WORLD_PROGRESS") != NULL) && (strcmp(getenv("KFX_WORLD_PROGRESS"), "auto") == 0)) ? 1 : 0;
    if ((wpr_mode == 2) || automatic || (k == 1) || (lore_model <= 0))
    {
        unlock_item(plyr_idx, opt[0], &pos);
        return true;
    }
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
        struct Thing *lore = make_book(lore_model, &lp, plyr_idx, (uint8_t)(WORLD_LORE_KIND_FIRST + opt[c]));
        if (thing_is_invalid(lore))
            continue;
        wp->offer_items[wp->offer_count] = (uint8_t)opt[c];
        wp->offer_things[wp->offer_count] = lore->index;
        wp->offer_count++;
    }
    SYNCLOG("World progress: player %d is offered %s%s%s%s%s", (int)plyr_idx, wpr_items[opt[0]].label,
        (k > 1) ? ", " : "", (k > 1) ? wpr_items[opt[1]].label : "", (k > 2) ? ", " : "", (k > 2) ? wpr_items[opt[2]].label : "");
    if (wp->offer_count == 0)
        unlock_item(plyr_idx, opt[0], &pos);
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

/** Destroy Walls and Armageddon stay off in endless worlds for every keeper, AI ones too: neither
 *  researchable nor usable. */
static void ban_powers(void)
{
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

void world_progress_turn(void)
{
    if (world_mode_active() && ((game.play_gameturn % 64) == 2))
        ban_powers();
    if (!world_progress_active())
        return;
    test_books();
    test_read_books();
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
    return 4 + 4 + 1 + 1 + PLAYERS_COUNT * sizeof(struct WprSaved) + 1 + 4 * wpr_mask_words();
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
}

TbBool world_progress_load(const uint8_t *buf, size_t len)
{
    memset(wpr_players, 0, sizeof(wpr_players));
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
kfx_rewindable static uint32_t wpl_xp[PLAYERS_MAX];

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
        server_admin_log("level", "%s reached level %d", server_admin_player_name(plyr_idx), after);
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
    server_admin_log("portal", "%s captured a portal (%s)", server_admin_player_name(plyr_idx),
        ((biome >= 0) && (biome < WgB_COUNT)) ? wg_biome_name(biome) : "?");
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
    const int i = wpl_hit_slot(heartng);
    const PlayerNumber victim = heartng->owner;
    if (i < 0)
    {
        SYNCLOG("World: heart %d of player %d destroyed, by no player", (int)heartng->index, (int)heartng->owner);
        if (world_player_is_human(victim))
            server_admin_log("death", "%s died: their heart was destroyed", server_admin_player_name(victim));
        return;
    }
    const PlayerNumber by = wpl_hits[i].by;
    memset(&wpl_hits[i], 0, sizeof(wpl_hits[i]));
    if (world_player_is_human(victim))
        server_admin_log("death", "%s died: heart destroyed by %s", server_admin_player_name(victim), server_admin_player_name(by));
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
        server_admin_log("defeat", "%s defeated a heroes outpost", server_admin_player_name(by));
    } else
    {
        wps[by].hearts++;
        if (world_player_is_human(victim))
            server_admin_log("defeat", "%s defeated %s", server_admin_player_name(by), server_admin_player_name(victim));
        else
            server_admin_log("defeat", "%s defeated the enemy keeper %s", server_admin_player_name(by), server_admin_player_name(victim));
    }
    if (after > before)
        server_admin_log("level", "%s reached level %d", server_admin_player_name(by), after);
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
    }
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        const int level = wpl_level_of(wpl_xp[p]);
        if (news && (wpl_seen[p] != 0) && (level > wpl_seen[p]))
        {
            if (p == my_player_number)
                message_add_fmt(MsgType_Player, p, "Player level %d!", level);
            else
            {
                char name[32];
                message_add_fmt(MsgType_Player, p, "%s reached player level %d", wpl_name(p, name, sizeof(name)), level);
            }
        }
        wpl_seen[p] = (uint8_t)level;
    }
}

#ifdef __cplusplus
}
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_unions.c
 *     Endless world: unholy unions.
 * @par Purpose:
 *     See world_unions.h. Whether an amenity is at its top mastery is asked of world_progress (its numbers stay there).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_unions.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "world_stream.h"
#include "world_progress.h"
#include "world_daynight.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "creature_control.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "room_util.h"
#include "map_blocks.h"
#include "player_utils.h"
#include "world_caretaker.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct UnionDef {
    const char *name;
    const char *room_word;      /* what "!union <room>" names */
    const char *amenity[2];     /* the two amenities (Amenity Mastery's entry names) */
    RoomKind room;
    const char *line;
};
static const struct UnionDef unions[WUn_COUNT] = {
    {"the Scrying Room", "library", {"RESEARCH", "POWER_SIGHT"}, RoK_LIBRARY, "the ground round your Libraries is always shown"},
    {"the Chair", "prison", {"PRISON", "LIGHTNING"}, RoK_PRISON, "each captive talks: gold every day"},
    {"the poisoned Hatchery", "hatchery", {"GARDEN", "POISON_GAS"}, RoK_GARDEN, "enemies in your Hatcheries are poisoned"},
    {"the Infirmary", "lair", {"LAIR", "POWER_HEAL_CREATURE"}, RoK_LAIR, "sleep in your Lairs heals 50% faster"},
};

static uint8_t un[PLAYERS_MAX];          /* bit per union */
static uint32_t un_paid_day[PLAYERS_MAX]; /* the Chair: the day last paid */

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: unions, to player %d: %s", (int)p, text);
    if (is_my_player_number(p) && !netstate_dedicated())
    {
        if (chat_box_active())
            chat_box_add(-1, text);
        else
            message_add(MsgType_Blank, 0, text);
        return;
    }
    if (!network_is_active() || !netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

TbBool world_union_has(PlayerNumber p, int u)
{
    return world_mode_active() && (p >= 0) && (p < PLAYERS_MAX) && (u >= 0) && (u < WUn_COUNT) && (un[p] & (1u << u))
        && !world_caretaker_active(p); /* away: the unions rest, the caretaker keeps the usual rules */
}

int world_union_heal_pct(PlayerNumber p)
{
    return world_union_has(p, WUn_Infirmary) ? 50 : 0;
}

static TbBool test_masteries(void)
{
    return getenv("KFX_WORLD_UNIONMASTERED") != NULL; /* test switch: every amenity counts as mastered */
}

void world_union_packet(PlayerNumber p, int what)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    char text[200];
    if (what == 0)
    {
        int n = snprintf(text, sizeof(text), "Unions:");
        for (int u = 0; u < WUn_COUNT; u++)
            n += snprintf(text + n, sizeof(text) - n, " %s%s;", unions[u].name, world_union_has(p, u) ? " [yours]" : "");
        tell_keeper(p, text);
        return;
    }
    const int u = what - 1;
    if ((u < 0) || (u >= WUn_COUNT))
        return;
    const struct UnionDef *d = &unions[u];
    if (world_union_has(p, u))
        return;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon) || (dungeon->room_list_start[RoK_TEMPLE] == 0))
    {
        tell_keeper(p, "Unions are made at the Temple: you have none.");
        return;
    }
    if (!test_masteries() && (!world_amenity_mastered(p, d->amenity[0]) || !world_amenity_mastered(p, d->amenity[1])))
    {
        snprintf(text, sizeof(text), "%s needs both of its amenities at their top mastery.", d->name);
        tell_keeper(p, text);
        return;
    }
    un[p] |= (uint8_t)(1u << u);
    snprintf(text, sizeof(text), "Your Temple joins them: %s (%s).", d->name, d->line);
    tell_keeper(p, text);
}

TbBool world_union_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strncasecmp(text + 1, "union", 5) != 0)
        return false;
    const char *arg = text + 6;
    if ((*arg != '\0') && (*arg != ' '))
        return false;
    while (*arg == ' ')
        arg++;
    int what = 0;
    for (int u = 0; (u < WUn_COUNT) && (*arg != '\0'); u++)
        if (strcasecmp(arg, unions[u].room_word) == 0)
            what = u + 1;
    set_players_packet_action(get_my_player(), PckA_WorldUnion, (unsigned long)what, 0, 0, 0);
    return true;
}

static void scry(PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    long guard = 0;
    for (RoomIndex i = dungeon->room_list_start[RoK_LIBRARY]; (i != 0) && (guard++ < ROOMS_COUNT);)
    {
        const struct Room *room = room_get(i);
        if (room_is_invalid(room))
            break;
        i = room->next_of_owner;
        reveal_map_area(p, room->central_stl_x - 30, room->central_stl_x + 30, room->central_stl_y - 30, room->central_stl_y + 30);
    }
}

static int chair(PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    int captives = 0;
    long guard = 0;
    for (RoomIndex i = dungeon->room_list_start[RoK_PRISON]; (i != 0) && (guard++ < ROOMS_COUNT);)
    {
        const struct Room *room = room_get(i);
        if (room_is_invalid(room))
            break;
        i = room->next_of_owner;
        captives += room->used_capacity;
    }
    if (captives > 0)
        player_add_offmap_gold(p, 150 * captives);
    return captives;
}

static void poison_hatcheries(PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    long guard = 0;
    for (RoomIndex r = dungeon->room_list_start[RoK_GARDEN]; (r != 0) && (guard++ < ROOMS_COUNT);)
    {
        const struct Room *room = room_get(r);
        if (room_is_invalid(room))
            break;
        r = room->next_of_owner;
        long g2 = 0;
        for (SlabCodedCoords s = room->slabs_list; (s != 0) && (g2++ < 4096); s = get_next_slab_number_in_room(s))
        {
            const MapSlabCoord x = slb_num_decode_x(s), y = slb_num_decode_y(s);
            for (MapSubtlCoord sy = slab_subtile(y, 0); sy <= slab_subtile(y, 2); sy++)
                for (MapSubtlCoord sx = slab_subtile(x, 0); sx <= slab_subtile(x, 2); sx++)
                {
                    long i = get_mapwho_thing_index(get_map_block_at(sx, sy));
                    unsigned long g3 = 0;
                    while ((i != 0) && (g3++ < THINGS_COUNT))
                    {
                        struct Thing *t = thing_get(i);
                        if (thing_is_invalid(t))
                            break;
                        i = t->next_on_mapblk;
                        if (!thing_is_creature(t) || (t->owner == p) || !players_are_enemies(p, t->owner))
                            continue;
                        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
                        const HitPoints hurt = max(1, cctrl->max_health * 3 / 100);
                        t->health = max(1, t->health - hurt);
                    }
                }
        }
    }
}

/** KFX_WORLD_UNIONTEST=turn (test switch): this machine's keeper asks for the Scrying Room without a Temple (refused),
 *  then, with a Temple laid below its Heart, for all four (with KFX_WORLD_UNIONMASTERED all amenities count as
 *  mastered); the list is logged. */
static void union_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_UNIONTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_UNIONTEST=%ld", at);
    }
    if ((at < 0) || ((long)game.play_gameturn != at))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    world_union_packet(p, WUn_Scrying + 1);
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    for (int dy = 0; dy < 3; dy++)
        for (int dx = 0; dx < 3; dx++)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1 + dx), slab_subtile_center(hy + 6 + dy), p, 0);
            replace_slab_from_script(hx - 1 + dx, hy + 6 + dy, SlbT_TEMPLE);
        }
    for (int u = 0; u < WUn_COUNT; u++)
        world_union_packet(p, u + 1);
    world_union_packet(p, 0);
    SYNCLOG("World: union test: unions %02x, Infirmary healing %+d%%", (unsigned)un[p], world_union_heal_pct(p));
}

void world_unions_turn(void)
{
    if (!simulating())
        return;
    union_test();
    if ((game.play_gameturn % 32) != 17)
        return;
    const uint32_t day = (uint32_t)world_day_number();
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if ((un[p] == 0) || !world_player_is_human(p) || dungeon_invalid(get_players_num_dungeon(p)))
            continue;
        if (world_union_has(p, WUn_PoisonHatchery))
            poison_hatcheries(p);
        if (world_union_has(p, WUn_Scrying) && ((game.play_gameturn % 128) == 17))
            scry(p);
        if (world_union_has(p, WUn_Chair) && (un_paid_day[p] != day))
        {
            un_paid_day[p] = day;
            const int n = chair(p);
            if (n > 0)
                SYNCLOG("World: player %d's captives talk in the Chair: %d gold", (int)p, 150 * n);
        }
    }
}

/******************************************************************************/
void world_unions_reset(void)
{
    memset(un, 0, sizeof(un));
    memset(un_paid_day, 0, sizeof(un_paid_day));
}

size_t world_unions_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (un[p] != 0)
            return 4 + sizeof(un) + sizeof(un_paid_day);
    return 0;
}

void world_unions_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, un, sizeof(un));
    memcpy(buf + 4 + sizeof(un), un_paid_day, sizeof(un_paid_day));
}

TbBool world_unions_load(const uint8_t *buf, size_t len)
{
    world_unions_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (4 + sizeof(un) + sizeof(un_paid_day) > len))
        return false;
    memcpy(un, buf + 4, sizeof(un));
    memcpy(un_paid_day, buf + 4 + sizeof(un), sizeof(un_paid_day));
    return true;
}

void world_unions_seat_reset(PlayerNumber p)
{
    if ((p >= 0) && (p < PLAYERS_MAX))
    {
        un[p] = 0;
        un_paid_day[p] = 0;
    }
}

#ifdef __cplusplus
}
#endif

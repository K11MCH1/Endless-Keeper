/******************************************************************************/
// Endless Keeper: A Prisoner's Fate.
/******************************************************************************/
/** @file world_prisoners.c
 *     A captive hero gets a real choice beside converting it, starving it into a Skeleton or sacrificing it. Its
 *     jailer picks it up and drops it:
 *     - on their own Treasury Room: Ransom. Gold now (three times its wage, at least 100), and the hero comes back
 *       with that keeper's next raid, a level higher;
 *     - on their own Library: Interrogation. It gives up the nearest hero stronghold (direction and distance; its
 *       heart and portal are revealed when they are on the map), then dies under the questioning.
 *     The heroes waiting to come back are the world save's "PRF1" extra.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_prisoners.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_groups.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "map_data.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "room_data.h"
#include "server_admin.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "player_instances.h"
#include "thing_factory.h"
#include "creature_states_hero.h"
#include "map_blocks.h"
#include "thing_navigate.h"
#include "world_outpost.h"
#include "world_stream.h"
#include "world_wild.h"
#include "creature_states_prisn.h"
#include "power_hand.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#define PF_QUEUE_MAX 64
#define PF_RANSOM_WAGES 3
#define PF_RANSOM_MIN 100
#define PF_RETURN_PER_RAID 4
#define PF_LEVEL_MAX 100
#define PF_SAVE_MAGIC 0x31465250u /* "PRF1" */

#pragma pack(1)
struct PfReturn {
    uint16_t model;     /* 0: free */
    uint8_t owner;      /* the keeper it was ransomed by: it comes back with that keeper's raid */
    uint8_t level;      /* the level it comes back at (1-based) */
};
#pragma pack()

static struct PfReturn pf_queue[PF_QUEUE_MAX];

static void pf_tell(PlayerNumber plyr_idx, const char *text)
{
    if (is_my_player_number(plyr_idx) && !netstate_dedicated()) /* (a dedicated server's own number is a joined player's) */
    {
        message_add(MsgType_Blank, 0, text);
        return;
    }
    if (!netstate_is_server() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    const NetUserId u = get_player_primary_user(get_player(plyr_idx));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static const char *pf_direction(int64_t dx, int64_t dy)
{
    static const char *const names[8] = {"east", "south-east", "south", "south-west", "west", "north-west", "north",
        "north-east"};
    const double a = atan2((double)dy, (double)dx); /* y grows southwards */
    int k = (int)floor(a / (M_PI / 4) + 0.5);
    return names[(k + 8) % 8];
}

static void pf_ransom(PlayerNumber jailer, struct Thing *thing)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const int level = cctrl->exp_level + 1;
    const GoldAmount wage = compute_creature_max_pay(crconf->pay, cctrl->exp_level);
    const GoldAmount gold = max(wage * PF_RANSOM_WAGES, (GoldAmount)PF_RANSOM_MIN);
    player_add_offmap_gold(jailer, gold);
    int slot = -1;
    for (int i = 0; (i < PF_QUEUE_MAX) && (slot < 0); i++)
        if (pf_queue[i].model == 0)
            slot = i;
    if (slot >= 0)
    {
        pf_queue[slot].model = (uint16_t)thing->model;
        pf_queue[slot].owner = (uint8_t)jailer;
        pf_queue[slot].level = (uint8_t)min(level + 1, PF_LEVEL_MAX);
    }
    SYNCLOG("Prisoners: player %d ransomed a %s of level %d for %ld gold; %s", (int)jailer, thing_model_name(thing), level,
        (long)gold, (slot >= 0) ? "it comes back with the next raid, a level higher" : "the queue is full: it won't come back");
    char text[160];
    snprintf(text, sizeof(text), "The %s is ransomed for %ld gold. It will come back with a raid, a level higher.",
        creature_code_name(thing->model), (long)gold);
    pf_tell(jailer, text);
    create_effect(&thing->mappos, TngEff_Explosion3, jailer);
    delete_thing_structure(thing, 0);
}

static void pf_interrogate(PlayerNumber jailer, struct Thing *thing)
{
    int64_t wx = 0, wy = 0;
    world_slab_to_world(coord_slab(thing->mappos.x.val), coord_slab(thing->mappos.y.val), &wx, &wy);
    int64_t x0, y0, ex, ey, hx, hy, gx, gy;
    int odx, ody;
    char text[200];
    if (world_outpost_nearest(wx, wy, &x0, &y0, &ex, &ey, &odx, &ody, &hx, &hy, &gx, &gy))
    {
        const long dist = (long)floor(sqrt((double)(hx - wx) * (hx - wx) + (double)(hy - wy) * (hy - wy)) + 0.5);
        int revealed = 0;
        const int64_t spots[2][2] = {{hx, hy}, {gx, gy}};
        for (int s = 0; s < 2; s++)
        {
            MapSlabCoord sx, sy;
            if (world_engine_slab(spots[s][0], spots[s][1], &sx, &sy))
            {
                const int r = (s == 0) ? 4 : 2;
                reveal_map_area(jailer, slab_subtile(max(sx - r, 0), 0), slab_subtile(sx + r, 2), slab_subtile(max(sy - r, 0), 0),
                    slab_subtile(sy + r, 2));
                revealed++;
            }
        }
        SYNCLOG("Prisoners: player %d interrogated a %s: the nearest stronghold's heart is %ld slabs %s at world (%lld,%lld); "
            "%d of its heart and portal revealed", (int)jailer, thing_model_name(thing), dist, pf_direction(hx - wx, hy - wy),
            (long long)hx, (long long)hy, revealed);
        snprintf(text, sizeof(text), "Under questioning, the %s gives up a hero stronghold %ld slabs to the %s%s.",
            creature_code_name(thing->model), dist, pf_direction(hx - wx, hy - wy), (revealed > 0) ? ": see your map" : "");
    } else
    {
        SYNCLOG("Prisoners: player %d interrogated a %s: no stronghold near", (int)jailer, thing_model_name(thing));
        snprintf(text, sizeof(text), "Under questioning, the %s knows of nothing you haven't found.", creature_code_name(thing->model));
    }
    pf_tell(jailer, text);
    kill_creature(thing, INVALID_THING, jailer, CrDed_NoUnconscious); /* (it does not survive the questioning) */
}

TbBool world_prisoners_dropped(struct Dungeon *dungeon, struct Thing *thing)
{
    if (!world_mode_active() || dungeon_invalid(dungeon) || !thing_is_creature(thing) || (thing->owner != PLAYER_GOOD) ||
        world_wild_creature(thing) || thing_is_creature_digger(thing) || (thing->health < 0))
        return false;
    const PlayerNumber jailer = dungeon->owner;
    if (!world_player_is_human(jailer))
        return false;
    const struct Room *room = get_room_thing_is_on(thing);
    if (room_is_invalid(room) || (room->owner != jailer))
        return false;
    if (room_role_matches(room->kind, RoRoF_GoldStorage))
        pf_ransom(jailer, thing);
    else if (room_role_matches(room->kind, RoRoF_Research))
        pf_interrogate(jailer, thing);
    else
        return false;
    return true;
}

int world_prisoners_join_raid(PlayerNumber plyr_idx, struct Thing *leader, MapSlabCoord sx, MapSlabCoord sy)
{
    if (!thing_exists(leader))
        return 0;
    int made = 0;
    for (int i = 0; (i < PF_QUEUE_MAX) && (made < PF_RETURN_PER_RAID); i++)
    {
        struct PfReturn *ret = &pf_queue[i];
        if ((ret->model == 0) || (ret->owner != plyr_idx))
            continue;
        if (!creature_count_below_map_limit(0))
            break;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(sx)) + (MapCoord)(((made % 3) - 1) * 64);
        pos.y.val = subtile_coord_center(slab_subtile_center(sy)) + (MapCoord)(((made / 3) - 1) * 64);
        pos.z.val = 0;
        struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, ret->model, PLAYER_GOOD);
        if (thing_is_invalid(thing))
            continue;
        /* as the raid's own heroes come: through the ceiling, then after the leader */
        thing->mappos.z.val = get_ceiling_height(&thing->mappos);
        create_effect(&thing->mappos, TngEff_CeilingBreach, thing->owner);
        initialise_thing_state(thing, CrSt_CreatureHeroEntering);
        set_flag(thing->rendering_flags, TRF_Invisible);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        cctrl->countdown = 40 + 8 * made;
        init_creature_level(thing, (CrtrExpLevel)(ret->level - 1));
        cctrl->party.original_objective = CHeroTsk_AttackEnemies; /* (marks raid heroes) */
        cctrl->party.objective = CHeroTsk_AttackEnemies;
        cctrl->party.target_plyr_idx = plyr_idx;
        add_creature_to_group(thing, leader);
        SYNCLOG("Prisoners: a ransomed %s comes back with player %d's raid at level %d", thing_model_name(thing), (int)plyr_idx,
            (int)ret->level);
        memset(ret, 0, sizeof(*ret));
        made++;
    }
    return made;
}

/** Test hook KFX_WORLD_PRISONTEST="T": at T a one-slab Treasury Room and a one-slab Library beside the local keeper's
 *  heart, and two Knights of level 5 as if carried out of the Prison: one dropped on each (the hand's drop calls the
 *  same function). Then the next raid should bring the first one back at level 6. "T,1": the rooms and a Prison are
 *  built the stock way, the Knights are put in the Prison (the stock arrival there), and at T+20 and T+30 each is
 *  taken in the keeper's hand and dropped on its room (the hand's own pickup and drop). */
static void pf_test_by_hand(long t)
{
    static ThingIndex knights[2];
    static MapSlabCoord rx[2], ry[2];
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 0)
    {
        static const char *const rooms[3] = {"TREASURE", "RESEARCH", "PRISON"};
        struct Room *made[3] = {NULL, NULL, NULL};
        for (int k = 0; k < 3; k++)
        {
            const MapSlabCoord x = hx - 2 + 2 * k, y = hy + 5;
            for (int dy = 3; dy <= 4; dy++)
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(hy + dy), my_player_number, 0);
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(y), my_player_number, 0);
            made[k] = place_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(x), slab_subtile_center(y));
            reveal_map_area(my_player_number, slab_subtile(x, 0), slab_subtile(x, 2), slab_subtile(hy + 3, 0), slab_subtile(y, 2));
            if (k < 2)
            {
                rx[k] = x;
                ry[k] = y;
            } else
            {
                /* a second Prison slab: one captive a slab */
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x + 1), slab_subtile_center(y), my_player_number, 0);
                place_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(x + 1), slab_subtile_center(y));
                reveal_map_area(my_player_number, slab_subtile(x + 1, 0), slab_subtile(x + 1, 2), slab_subtile(y, 0), slab_subtile(y, 2));
            }
        }
        for (int k = 0; k < 1; k++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(hx + 2));
            pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
            pos.z.val = get_floor_height_at(&pos);
            struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
            if (thing_is_invalid(knight))
                continue;
            init_creature_level(knight, 4);
            move_thing_in_map(knight, &pos);
            creature_arrived_at_prison(knight); /* (as when a captive is carried in) */
            knights[k] = knight->index;
        }
        SYNCLOG("Prisoners test: by hand: Treasury %s, Library %s, Prison %s; the Knights kept in the Prison: %d %d",
            room_is_invalid(made[0]) ? "MISSING" : "built", room_is_invalid(made[1]) ? "MISSING" : "built",
            room_is_invalid(made[2]) ? "MISSING" : "built", (int)creature_is_kept_in_prison(thing_get(knights[0])),
            (int)creature_is_kept_in_prison(thing_get(knights[1])));
        return;
    }
    if (t == 25)
    {
        /* the second captive, once the first has left the Prison */
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx + 2));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
        if (!thing_is_invalid(knight))
        {
            init_creature_level(knight, 4);
            move_thing_in_map(knight, &pos);
            creature_arrived_at_prison(knight);
            knights[1] = knight->index;
            SYNCLOG("Prisoners test: by hand: the second Knight kept in the Prison: %d", (int)creature_is_kept_in_prison(knight));
        }
        return;
    }
    if ((t != 20) && (t != 30))
        return;
    const int k = (t == 20) ? 0 : 1;
    struct Thing *knight = thing_get(knights[k]);
    if (!thing_exists(knight))
        return;
    struct PlayerInfo *player = get_player(my_player_number);
    const TbBool may = thing_is_pickable_by_hand(player, knight);
    if (!place_thing_in_power_hand(knight, my_player_number))
    {
        SYNCLOG("Prisoners test: by hand: the Knight can't be taken in the hand (pickable %d)", (int)may);
        return;
    }
    get_player_primary_user_state(player)->thing_under_hand = 0;
    const GoldAmount before = get_dungeon(my_player_number)->total_money_owned;
    const short res = dump_first_held_thing_on_map(my_player_number, slab_subtile_center(rx[k]), slab_subtile_center(ry[k]), true);
    SYNCLOG("Prisoners test: by hand: a Knight from the Prison (pickable by the hand %d) dropped on the %s: %d; it is %s; treasury %ld -> %ld",
        (int)may, (k == 0) ? "Treasury" : "Library", (int)res, (thing_exists(knight) && (knight->class_id == TCls_Creature) &&
        (knight->model == (ThingModel)get_id(creature_desc, "KNIGHT"))) ? "still here" : "gone", (long)before,
        (long)get_dungeon(my_player_number)->total_money_owned);
}
static void pf_test_hook(void)
{
    static long base = -2;
    static int by_hand = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_PRISONTEST");
        base = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%d", &base, &by_hand) >= 1))
            SYNCLOG("Prisoners test: KFX_WORLD_PRISONTEST at turn %ld%s", base, by_hand ? ", by hand" : "");
        else
            base = -1;
    }
    if (by_hand && (base >= 0) && ((long)game.play_gameturn >= base))
    {
        pf_test_by_hand((long)game.play_gameturn - base);
        return;
    }
    if ((base < 0) || ((long)game.play_gameturn != base))
        return;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    static const char *const rooms[2] = {"TREASURE", "RESEARCH"};
    static const SlabKind slabs[2] = {SlbT_TREASURE, SlbT_LIBRARY};
    for (int k = 0; k < 2; k++)
    {
        const MapSlabCoord rx = hx + ((k == 0) ? -1 : 1), ry = hy + 5;
        for (int dy = 3; dy <= 4; dy++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(rx), slab_subtile_center(hy + dy), my_player_number, 0);
        place_slab_type_on_map(slabs[k], slab_subtile_center(rx), slab_subtile_center(ry), my_player_number, 0);
        struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(rx),
            slab_subtile_center(ry));
        if (!room_is_invalid(room))
            do_room_integration(room);
        reveal_map_area(my_player_number, slab_subtile(rx, 0), slab_subtile(rx, 2), slab_subtile(hy + 3, 0), slab_subtile(ry, 2));
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(rx));
        pos.y.val = subtile_coord_center(slab_subtile_center(ry));
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
        if (thing_is_invalid(knight))
            continue;
        init_creature_level(knight, 4);
        move_thing_in_map(knight, &pos); /* (as a drop: on the room's slab, not moved to a free spot beside it) */
        const GoldAmount before = get_dungeon(my_player_number)->total_money_owned;
        const TbBool taken = world_prisoners_dropped(get_dungeon(my_player_number), knight);
        SYNCLOG("Prisoners test: %s room %s; a Knight dropped on it: %s; treasury %ld -> %ld", rooms[k],
            room_is_invalid(room) ? "MISSING" : "built", taken ? "taken" : "NOT taken", (long)before,
            (long)get_dungeon(my_player_number)->total_money_owned);
    }
}

void world_prisoners_turn(void)
{
    pf_test_hook();
}

void world_prisoners_reset(void)
{
    memset(pf_queue, 0, sizeof(pf_queue));
}

static int pf_count(void)
{
    int n = 0;
    for (int i = 0; i < PF_QUEUE_MAX; i++)
        if (pf_queue[i].model != 0)
            n++;
    return n;
}

size_t world_prisoners_save_size(void)
{
    return (pf_count() > 0) ? 8 + sizeof(pf_queue) : 0;
}

void world_prisoners_save(uint8_t *buf)
{
    const uint32_t head[2] = {PF_SAVE_MAGIC, (uint32_t)sizeof(pf_queue)};
    memcpy(buf, head, 8);
    memcpy(buf + 8, pf_queue, sizeof(pf_queue));
}

TbBool world_prisoners_load(const uint8_t *buf, size_t len)
{
    world_prisoners_reset();
    uint32_t head[2];
    if (len < 8)
        return false;
    memcpy(head, buf, 8);
    if ((head[0] != PF_SAVE_MAGIC) || (len < 8 + (size_t)head[1]))
        return false;
    memcpy(pf_queue, buf + 8, min((size_t)head[1], sizeof(pf_queue)));
    SYNCLOG("Prisoners: %d ransomed heroes waiting to come back", pf_count());
    return true;
}

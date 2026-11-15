/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_scars.c
 *     Endless world: Scars (2026-11-13; see world_scars.h).
 * @par Purpose:
 *     Decisions (the user was away; the recommended choice each time):
 *     - Wrecked: a hero tears out a slab of a human keeper's room. The room is desecrated if the keeper has fewer than
 *       two scars.
 *     - Four fifths: a room is desecrated only if its slabs are at most two fifths of all the keeper's room slabs (half
 *       of two fifths is a fifth); a crack takes a tenth. A new crack needs the Heart to have been above half since.
 *     - Relaid and prayed over: the room has its slabs back (as many as when it was desecrated) and then one of the
 *       keeper's creatures prays in a Temple for half a minute. A room torn out entirely takes its scar with it.
 *     - A crack: the Heart below a quarter of its health; it then can't heal above nine tenths. Mended by a hero's
 *       heart-shard (a keeper gets one when a heroes heart falls with one of their creatures nearest it; used at once)
 *       or by gold dropped on the Heart: 10,000, plus 5,000 for each time it was mended with gold before.
 *     Saved: SCR1.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_scars.h"
#include "world_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_util.h"
#include "creature_instances.h"
#include "thing_navigate.h"
/* creature_instances.c, not in a header */
long instf_attack_room_slab(struct Thing *creatng, int32_t *param);
#include "thing_creature.h"
#include "config_creature.h"
#include "slab_data.h"
#include "room_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "post_inc.h"

#define SC_MAX_ROOMS 2
#define SC_PRAY_TURNS (30 * 20)
#define SC_MEND_GOLD 10000
#define SC_MEND_GOLD_MORE 5000

#pragma pack(1)
struct ScKeeper {
    uint8_t cracked, shards, mends, armed; /* armed: the Heart was above half since its last crack (a new crack may come) */
    uint16_t room[SC_MAX_ROOMS];      /* 1: a desecrated room (0 none); it is found by its slab below (a room torn into is
                                         made again under another index) */
    int32_t room_wx[SC_MAX_ROOMS], room_wy[SC_MAX_ROOMS]; /* a slab of it, world slabs */
    uint8_t room_kind[SC_MAX_ROOMS];
    uint16_t room_size[SC_MAX_ROOMS]; /* its slabs when it was desecrated */
    uint8_t relaid[SC_MAX_ROOMS];     /* its slabs are back: waiting for a prayer */
    uint16_t prayed[SC_MAX_ROOMS];    /* turns of prayer since */
};
#pragma pack()
static struct ScKeeper sc[PLAYERS_MAX];

static int sc_count(PlayerNumber p)
{
    int n = sc[p].cracked ? 1 : 0;
    for (int k = 0; k < SC_MAX_ROOMS; k++)
        n += (sc[p].room[k] != 0) ? 1 : 0;
    return n;
}

/** The room of scar k: the keeper's room of its kind on its slab or within two slabs of it (NULL: gone). */
static struct Room *sc_room_of(PlayerNumber p, int k)
{
    MapSlabCoord ex, ey;
    if ((sc[p].room[k] == 0) || !world_engine_slab(sc[p].room_wx[k], sc[p].room_wy[k], &ex, &ey))
        return NULL;
    for (int d = 0; d <= 2; d++)
        for (int dy = -d; dy <= d; dy++)
            for (int dx = -d; dx <= d; dx++)
            {
                struct Room *room = slab_room_get(ex + dx, ey + dy);
                if (room_exists(room) && (room->owner == p) && (room->kind == sc[p].room_kind[k]))
                    return room;
            }
    return NULL;
}

static TbBool sc_room_desecrated(const struct Room *room, int *k_out)
{
    const PlayerNumber p = room->owner;
    if (p >= PLAYERS_MAX)
        return false;
    for (int k = 0; k < SC_MAX_ROOMS; k++)
        if ((sc[p].room[k] != 0) && (sc[p].room_kind[k] == room->kind) && (sc_room_of(p, k) == room))
        {
            if (k_out != NULL)
                *k_out = k;
            return true;
        }
    return false;
}

int world_scars_room_percent(const struct Room *room)
{
    if (!world_mode_active() || room_is_invalid(room))
        return 100;
    int k;
    if (!sc_room_desecrated(room, &k))
        return 100;
    return sc[room->owner].relaid[k] ? 75 : 50; /* (relaid, waiting for its prayer: three quarters) */
}

void world_scars_room_hit(struct Room *room, const struct Thing *attacker)
{
    if (!world_mode_active() || room_is_invalid(room) || !thing_exists(attacker) || (attacker->owner != PLAYER_GOOD))
        return;
    const PlayerNumber p = room->owner;
    if ((p >= PLAYERS_MAX) || !world_player_is_human(p) || sc_room_desecrated(room, NULL) || (sc_count(p) >= 2))
        return;
    /* four fifths: the room's slabs at most two fifths of all the keeper's room slabs (half of two fifths is a fifth) */
    long all = 0;
    for (int i = 1; i < ROOMS_COUNT; i++)
    {
        const struct Room *r2 = room_get(i);
        if (room_exists(r2) && (r2->owner == p))
            all += r2->slabs_count;
    }
    if ((all <= 0) || ((long)room->slabs_count * 5 > all * 2))
    {
        SYNCLOG("World: raiders wreck player %d's %s, but it is too much of the dungeon to be desecrated (%d of %ld slabs)",
            (int)p, room_code_name(room->kind), (int)room->slabs_count, all);
        return;
    }
    for (int k = 0; k < SC_MAX_ROOMS; k++)
        if (sc[p].room[k] == 0)
        {
            sc[p].room[k] = 1;
            int64_t wx, wy;
            world_slab_to_world(subtile_slab(room->central_stl_x), subtile_slab(room->central_stl_y), &wx, &wy);
            sc[p].room_wx[k] = (int32_t)wx;
            sc[p].room_wy[k] = (int32_t)wy;
            sc[p].room_kind[k] = (uint8_t)room->kind;
            sc[p].room_size[k] = room->slabs_count;
            sc[p].relaid[k] = 0;
            sc[p].prayed[k] = 0;
            room->efficiency = calculate_room_efficiency(room);
            SYNCLOG("World: raiders desecrate player %d's %s (room %d, %d slabs): half efficiency until it is relaid and"
                " prayed over", (int)p, room_code_name(room->kind), (int)room->index, (int)room->slabs_count);
            return;
        }
}

void world_scars_heroes_heart_fell(const struct Thing *heartng)
{
    if (!world_mode_active() || !thing_exists(heartng))
        return;
    PlayerNumber best = PLAYER_NONE;
    long best_d = 20 * STL_PER_SLB;
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *c = thing_get(i);
        if (thing_is_invalid(c))
            break;
        i = c->next_of_class;
        if ((c->owner >= PLAYERS_MAX) || !world_player_is_human(c->owner))
            continue;
        const long d = max(labs((long)c->mappos.x.stl.num - heartng->mappos.x.stl.num), labs((long)c->mappos.y.stl.num -
            heartng->mappos.y.stl.num));
        if (d < best_d)
        {
            best_d = d;
            best = c->owner;
        }
    }
    if (best == PLAYER_NONE)
        return;
    if (sc[best].shards < 255)
        sc[best].shards++;
    SYNCLOG("World: player %d takes a hero's heart-shard from the fallen heart (%d now)", (int)best, (int)sc[best].shards);
}

/** Gold piles lying on the cracked Heart's slabs (within one slab of it): mended when they come to the price. */
static TbBool sc_mend_with_gold(PlayerNumber p, struct Thing *heart)
{
    const long price = SC_MEND_GOLD + SC_MEND_GOLD_MORE * (long)sc[p].mends;
    long total = 0;
    const struct StructureList *olist = get_list_for_thing_class(TCls_Object);
    long guard = 0;
    for (ThingIndex i = olist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *o = thing_get(i);
        if (thing_is_invalid(o))
            break;
        i = o->next_of_class;
        if (!object_is_gold_pile(o) || thing_is_picked_up(o))
            continue;
        if ((labs((long)o->mappos.x.stl.num - heart->mappos.x.stl.num) > 4) || (labs((long)o->mappos.y.stl.num -
            heart->mappos.y.stl.num) > 4))
            continue;
        total += o->valuable.gold_stored;
    }
    if (total < price)
        return false;
    /* the price is taken from the piles, the rest stays */
    long left = price;
    guard = 0;
    for (ThingIndex i = olist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT) && (left > 0);)
    {
        struct Thing *o = thing_get(i);
        if (thing_is_invalid(o))
            break;
        i = o->next_of_class;
        if (!object_is_gold_pile(o) || thing_is_picked_up(o) ||
            (labs((long)o->mappos.x.stl.num - heart->mappos.x.stl.num) > 4) ||
            (labs((long)o->mappos.y.stl.num - heart->mappos.y.stl.num) > 4))
            continue;
        if (o->valuable.gold_stored <= left)
        {
            left -= o->valuable.gold_stored;
            delete_thing_structure(o, 0);
        } else
        {
            o->valuable.gold_stored -= left;
            add_gold_to_pile(o, 0);
            left = 0;
        }
    }
    sc[p].mends++;
    SYNCLOG("World: player %d mends the Heart's crack with %ld gold", (int)p, price);
    return true;
}

/** Test hook KFX_WORLD_SCARTEST=T: at turn T player 0's Heart is beaten to a fifth of its health; at T+400 a pile of
 *  10,000 gold is dropped on it (as a keeper drops gold with the Hand); at T+800 a hero tears out a slab of player 0's
 *  smallest room (the real instance, by a Thief made beside it). */
static void sc_test(void)
{
    static long at = -1;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_SCARTEST");
        at = (v != NULL) ? atol(v) : 0;
    }
    if (at <= 0)
        return;
    struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heart))
        return;
    if (game.play_gameturn == (GameTurn)at)
    {
        heart->health = get_object_model_stats(heart->model)->health / 5;
        SYNCLOG("World test: scars: player 0's Heart beaten to %ld", (long)heart->health);
    }
    if (game.play_gameturn == (GameTurn)(at + 400))
    {
        struct Coord3d pos = heart->mappos;
        pos.x.val += COORD_PER_STL;
        pos.z.val = 0;
        drop_gold_pile(10000, &pos);
        SYNCLOG("World test: scars: 10,000 gold dropped on player 0's Heart");
    }
    if (game.play_gameturn == (GameTurn)(at + 800))
    {
        struct Room *best = NULL;
        for (int i = 1; i < ROOMS_COUNT; i++)
        {
            struct Room *room = room_get(i);
            if (!room_exists(room) || (room->owner != PLAYER0) || room_cannot_vandalise(room->kind) ||
                room_role_matches(room->kind, RoRoF_KeeperStorage) || (room->slabs_count < 2))
                continue;
            if ((best == NULL) || (room->slabs_count < best->slabs_count))
                best = room;
        }
        if (best == NULL)
        {
            SYNCLOG("World test: scars: player 0 has no room to wreck");
            return;
        }
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(slb_num_decode_x(best->slabs_list)));
        pos.y.val = subtile_coord_center(slab_subtile_center(slb_num_decode_y(best->slabs_list)));
        pos.z.val = 0;
        struct Thing *thief = create_creature(&pos, (ThingModel)get_id(creature_desc, "THIEF"), PLAYER_GOOD);
        if (thing_exists(thief))
        {
            move_thing_in_map(thief, &pos);
            struct SlabMap *slb = get_slabmap_thing_is_on(thief);
            slb->health = 1; /* (one blow takes the slab) */
            int32_t param = 0;
            instf_attack_room_slab(thief, &param);
            SYNCLOG("World test: scars: a Thief tears out a slab of player 0's room %d (kind %d, %d slabs)", (int)best->index,
                (int)best->kind, (int)best->slabs_count); /* (the pointer is the room as it was before the slab went) */
        }
    }
}

void world_scars_turn(void)
{
    if (!world_mode_active())
        return;
    sc_test();
    if ((game.play_gameturn % 16) != 13)
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!world_player_is_human(p))
            continue;
        struct Thing *heart = find_players_dungeon_heart(p);
        /* the Heart's crack */
        if (thing_exists(heart))
        {
            const struct ObjectConfigStats *objst = get_object_model_stats(heart->model);
            const HitPoints max_hp = objst->health;
            if (heart->health * 2 >= max_hp)
                sc[p].armed = 1;
            if (!sc[p].cracked && sc[p].armed && (max_hp > 0) && (heart->health > 0) && (heart->health * 4 < max_hp) &&
                (sc_count(p) < 2))
            {
                sc[p].cracked = 1;
                sc[p].armed = 0;
                SYNCLOG("World: player %d's Heart is beaten below a quarter of its health: it keeps a crack", (int)p);
            }
            if (sc[p].cracked)
            {
                if (sc[p].shards > 0)
                {
                    sc[p].shards--;
                    sc[p].cracked = 0;
                    SYNCLOG("World: player %d mends the Heart's crack with a hero's heart-shard", (int)p);
                } else if (sc_mend_with_gold(p, heart))
                    sc[p].cracked = 0;
                else if (heart->health > max_hp * 9 / 10)
                    heart->health = max_hp * 9 / 10; /* a tenth off its health */
            }
        }
        /* desecrated rooms: relaid, then prayed over */
        for (int k = 0; k < SC_MAX_ROOMS; k++)
        {
            if (sc[p].room[k] == 0)
                continue;
            MapSlabCoord ex, ey;
            if (!world_engine_slab(sc[p].room_wx[k], sc[p].room_wy[k], &ex, &ey))
                continue; /* frozen: as it was */
            struct Room *room = sc_room_of(p, k);
            if (room == NULL)
            {
                SYNCLOG("World: player %d's desecrated room is gone: its scar goes with it", (int)p);
                sc[p].room[k] = 0;
                continue;
            }
            if (!sc[p].relaid[k] && (room->slabs_count >= sc[p].room_size[k]))
            {
                sc[p].relaid[k] = 1;
                room->efficiency = calculate_room_efficiency(room);
                SYNCLOG("World: player %d's desecrated %s is relaid: it waits for a creature to pray", (int)p,
                    room_code_name(room->kind));
            }
            if (sc[p].relaid[k])
            {
                /* a creature of the keeper's praying in a Temple */
                TbBool praying = false;
                struct Dungeon *dungeon = get_players_num_dungeon(p);
                long guard = 0;
                for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT) && !praying;)
                {
                    struct Thing *c = thing_get(i);
                    struct CreatureControl *cc = creature_control_get_from_thing(c);
                    if (!thing_exists(c) || creature_control_invalid(cc))
                        break;
                    i = cc->players_next_creature_idx;
                    const CrtrStateId st = get_creature_state_besides_interruptions(c);
                    praying = (st == CrSt_PrayingInTemple) || (st == CrSt_AtTemple);
                }
                if (praying)
                    sc[p].prayed[k] += 16;
                if (sc[p].prayed[k] >= SC_PRAY_TURNS)
                {
                    sc[p].room[k] = 0;
                    room->efficiency = calculate_room_efficiency(room);
                    SYNCLOG("World: player %d's %s is prayed over: it is clean again", (int)p, room_code_name(room->kind));
                }
            }
        }
    }
}

void world_scars_reset(void)
{
    memset(sc, 0, sizeof(sc));
}

size_t world_scars_save_size(void)
{
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if ((sc_count(p) > 0) || (sc[p].shards > 0) || (sc[p].mends > 0))
            return sizeof(sc);
    return 0;
}

void world_scars_save(uint8_t *buf)
{
    memcpy(buf, sc, sizeof(sc));
}

TbBool world_scars_load(const uint8_t *buf, size_t len)
{
    world_scars_reset();
    if (len < sizeof(sc))
        return false;
    memcpy(sc, buf, sizeof(sc));
    return true;
}

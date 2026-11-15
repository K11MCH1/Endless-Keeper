/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rooms.c
 *     Endless world: what a room gains from where and how it is built.
 * @par Purpose:
 *     See world_rooms.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_rooms.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "world_stream.h"
#include "game_legacy.h"
#include "player_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_stats.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "keeperfx.hpp"
#include "room_util.h"
#include "map_blocks.h"
#include "thing_list.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* Neighbours (the night of 2026-11-13): six pairs. Each pair gives one room an effect when it shares a wall with the
   other: two pull against the rest (a Graveyard beside a Lair, a Training Room beside a Library). */
struct NeighbourPair {
    RoomKind room;          /* the room that gains */
    RoomKind beside;        /* ... when it touches this one */
    enum WorldRoomMod what;
    int pct;
    TbBool not_vampires;    /* a Vampire doesn't mind it */
    const char *line;
};
static const struct NeighbourPair pairs[] = {
    {RoK_TORTURE,  RoK_PRISON,    WRM_Torture,  25, false, "a Torture Chamber beside the Prison breaks captives sooner"},
    {RoK_LIBRARY,  RoK_TEMPLE,    WRM_Work,     20, false, "a Library beside the Temple reads forbidden things sooner"},
    {RoK_LAIR,     RoK_GARDEN,    WRM_LairHeal, 25, false, "a Lair beside a Hatchery: sleepers wake fed and mend faster"},
    {RoK_LAIR,     RoK_GRAVEYARD, WRM_LairHeal,-25, true,  "a Lair beside a Graveyard: everyone but Vampires sleeps badly"},
    {RoK_WORKSHOP, RoK_TREASURE,  WRM_Work,     15, false, "a Workshop beside the Treasury: gold at hand, work goes faster"},
    {RoK_LIBRARY,  RoK_TRAINING,  WRM_Work,    -15, false, "a Library beside a Training Room: the din slows the reading"},
};
#define NEIGHBOUR_PAIRS ((int)(sizeof(pairs) / sizeof(pairs[0])))
#define NEIGHBOUR_TURNS 128

static uint32_t nb_mask[ROOMS_COUNT];
static uint32_t nb_turn[ROOMS_COUNT]; /* 1 + the turn the mask was worked out (0: never) */

void world_rooms_reset(void)
{
    memset(nb_mask, 0, sizeof(nb_mask));
    memset(nb_turn, 0, sizeof(nb_turn));
}

static uint32_t work_out_neighbours(const struct Room *room)
{
    uint32_t mask = 0;
    static const signed char dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    long guard = 0;
    for (SlabCodedCoords i = room->slabs_list; (i != 0) && (guard++ < 4096); i = get_next_slab_number_in_room(i))
    {
        const MapSlabCoord x = slb_num_decode_x(i), y = slb_num_decode_y(i);
        for (int d = 0; d < 4; d++)
        {
            const struct Room *other = slab_room_get(x + dirs[d][0], y + dirs[d][1]);
            if (room_is_invalid(other) || (other == room) || (other->owner != room->owner) || (other->kind >= 32))
                continue;
            mask |= 1u << other->kind;
        }
    }
    return mask;
}

uint32_t world_room_neighbour_kinds(const struct Room *room)
{
    if (room_is_invalid(room) || (room->index <= 0) || (room->index >= ROOMS_COUNT))
        return 0;
    const RoomIndex r = room->index;
    const uint32_t now = (uint32_t)game.play_gameturn;
    if ((nb_turn[r] == 0) || (nb_turn[r] - 1 > now) || (now - (nb_turn[r] - 1) >= NEIGHBOUR_TURNS))
    {
        nb_mask[r] = work_out_neighbours(room);
        nb_turn[r] = now + 1;
    }
    return nb_mask[r];
}

static int neighbour_pct(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng, int *malus)
{
    int bonus = 0;
    uint32_t mask = 0;
    TbBool mask_known = false;
    for (int k = 0; k < NEIGHBOUR_PAIRS; k++)
    {
        const struct NeighbourPair *pr = &pairs[k];
        if ((pr->room != room->kind) || (pr->what != what))
            continue;
        if (!mask_known)
        {
            mask = world_room_neighbour_kinds(room);
            mask_known = true;
        }
        if (!(mask & (1u << pr->beside)))
            continue;
        static ThingModel vampire = -1;
        if (vampire < 0)
            vampire = (ThingModel)get_id(creature_desc, "VAMPIRE");
        if (pr->not_vampires && (creatng != NULL) && thing_exists(creatng) && (creatng->model == vampire))
            continue;
        if (pr->pct >= 0)
            bonus += pr->pct;
        else
            *malus += pr->pct;
    }
    return bonus;
}

int world_room_mod(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng)
{
    if (!world_mode_active() || room_is_invalid(room) || (room->owner >= PLAYERS_MAX) || !world_player_is_human(room->owner))
        return 0;
    int malus = 0;
    int bonus = neighbour_pct(room, what, creatng, &malus);
    /* (The Shape of a Room and Lie of the Land add their bonuses here.) */
    if (bonus > WORLD_ROOM_BONUS_CAP)
        bonus = WORLD_ROOM_BONUS_CAP;
    return bonus + malus;
}

long world_room_apply(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng, long value)
{
    const int pct = world_room_mod(room, what, creatng);
    return (pct == 0) ? value : value * (100 + pct) / 100;
}

long world_room_apply_here(const struct Thing *creatng, enum WorldRoomMod what, long value)
{
    if (!world_mode_active())
        return value;
    const struct Room *room = get_room_thing_is_on(creatng);
    if (room_is_invalid(room) || (room->owner != creatng->owner))
        return value;
    return world_room_apply(room, what, creatng, value);
}

/** KFX_WORLD_ROOMTEST=turn (test switch): at that turn, every room of this machine's keeper is logged with the rooms
 *  it touches and its bonuses. */
void world_rooms_turn(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_ROOMTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_ROOMTEST=%ld", at);
    }
    if ((at > 100) && ((long)game.play_gameturn == at - 100) && world_mode_active() && (getenv("KFX_WORLD_ROOMLAYOUT") != NULL))
    {
        /* KFX_WORLD_ROOMLAYOUT=1 (test switch): 100 turns before, two strips of six 3x3 rooms below this machine's Heart,
           laid so that every pair touches: Prison|Torture|Lair|Graveyard|Library|Temple and
           Hatchery|Lair|Workshop|Treasury|Library|Training */
        const PlayerNumber me = my_player_number;
        const struct Thing *heart = get_player_soul_container(me);
        if (thing_exists(heart))
        {
            static const unsigned char strips[2][6] = {
                {SlbT_PRISON, SlbT_TORTURE, SlbT_LAIR, SlbT_GRAVEYARD, SlbT_LIBRARY, SlbT_TEMPLE},
                {SlbT_GARDEN, SlbT_LAIR, SlbT_WORKSHOP, SlbT_TREASURE, SlbT_LIBRARY, SlbT_TRAINING}};
            const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
            for (int s = 0; s < 2; s++)
                for (int r = 0; r < 6; r++)
                    for (int dy = 0; dy < 3; dy++)
                        for (int dx = 0; dx < 3; dx++)
                        {
                            const MapSlabCoord x = hx - 9 + r * 3 + dx, y = hy + 6 + s * 4 + dy;
                            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(y), me, 0);
                            replace_slab_from_script(x, y, strips[s][r]);
                        }
            SYNCLOG("World: room test: two strips of rooms laid below player %d's Heart", (int)me);
        }
        return;
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    const PlayerNumber p = my_player_number;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    int n = 0;
    for (RoomKind kind = 1; kind < RoK_TYPES_COUNT; kind++)
    {
        long guard = 0;
        for (RoomIndex i = dungeon->room_list_start[kind]; (i != 0) && (guard++ < ROOMS_COUNT);)
        {
            const struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            const uint32_t mask = world_room_neighbour_kinds(room);
            char touches[160];
            int t = 0;
            touches[0] = '\0';
            for (RoomKind k = 1; (k < RoK_TYPES_COUNT) && (t < (int)sizeof(touches) - 24); k++)
                if (mask & (1u << k))
                    t += snprintf(touches + t, sizeof(touches) - t, "%s%s", (t > 0) ? "," : "", room_code_name(k));
            SYNCLOG("World: room test: %s #%d (%d slabs) touches [%s]: work %+d%%, torture %+d%%, lair heal %+d%%",
                room_code_name(room->kind), (int)room->index, (int)room->slabs_count, touches,
                world_room_mod(room, WRM_Work, NULL), world_room_mod(room, WRM_Torture, NULL), world_room_mod(room, WRM_LairHeal, NULL));
            if (room->kind == RoK_LAIR)
            {
                /* the Vampire exception: a Vampire and an Orc, made for the moment at the Heart, asked the same */
                const struct Thing *heart = get_player_soul_container(p);
                struct Thing *vamp = thing_exists(heart) ? create_creature((struct Coord3d *)&heart->mappos,
                    (ThingModel)get_id(creature_desc, "VAMPIRE"), p) : NULL;
                struct Thing *orc = thing_exists(heart) ? create_creature((struct Coord3d *)&heart->mappos,
                    (ThingModel)get_id(creature_desc, "ORC"), p) : NULL;
                SYNCLOG("World: room test: LAIR #%d lair heal for a Vampire %+d%%, for an Orc %+d%%", (int)room->index,
                    thing_is_invalid(vamp) ? -999 : world_room_mod(room, WRM_LairHeal, vamp),
                    thing_is_invalid(orc) ? -999 : world_room_mod(room, WRM_LairHeal, orc));
                if (!thing_is_invalid(vamp))
                    delete_thing_structure(vamp, 0);
                if (!thing_is_invalid(orc))
                    delete_thing_structure(orc, 0);
            }
            n++;
        }
    }
    SYNCLOG("World: room test: %d rooms of player %d", n, (int)p);
}

#ifdef __cplusplus
}
#endif

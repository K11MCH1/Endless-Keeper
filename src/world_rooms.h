/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rooms.h
 *     Endless world: what a room gains from where and how it is built.
 * @par Purpose:
 *     One place for the per-room bonuses of three roadmap pages, which add up under one cap so that small bonuses
 *     never stack into a large one:
 *     - Neighbours: a room's bonus from the rooms of the same keeper it shares a wall with (six pairs, some of
 *       which pull against each other);
 *     - (later) The Shape of a Room and Lie of the Land add their sources here.
 *     Everything is derived from the map (nothing saved); a room's neighbours are worked out again every 128 turns.
 *     Endless worlds and human keepers only.
 */
/******************************************************************************/
#ifndef DK_WORLD_ROOMS_H
#define DK_WORLD_ROOMS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Room;
struct Thing;

/** What a bonus applies to. */
enum WorldRoomMod {
    WRM_Work = 0,      /* research, manufacture, training, scavenging done in the room */
    WRM_Torture,       /* torture progress */
    WRM_LairHeal,      /* healing while asleep in a Lair */
    WRM_Hatch,         /* how soon a Hatchery hatches its next chicken */
    WRM_GraveKeep,     /* how long a body laid in a Graveyard keeps (not capped: not a speed) */
    WRM_COUNT
};
/** The Shape of a Room: what a room's cut makes it. */
enum WorldRoomShape {
    WRS_None = 0,
    WRS_GreatHall,     /* a Lair of 25 slabs or more: full (3/4), its sleepers heal 15% faster */
    WRS_Cells,         /* a Lair of 6 slabs or fewer: enemies in it don't brawl */
    WRS_Gallery,       /* a Training Room 1-2 slabs wide and 5 long or more: shooters train 20% faster */
    WRS_Ring,          /* a square Training Room of 4x4 or more: melee fighters train 20% faster */
    WRS_Alcoves,       /* a Library with two dead-end slabs or more: mastery (level) research 20% faster */
    WRS_OpenFloor,     /* a filled Library of 9 slabs or more with no dead end: new research 20% faster */
    WRS_Vault,         /* a Treasury with one door and no other way in (named; more gold a slab is not built yet) */
    WRS_COUNT
};
/** The most all sources together may add (percent); maluses are not capped. */
#define WORLD_ROOM_BONUS_CAP 30

/** A room's bonus in percent for one effect, all sources added and capped (0 outside endless worlds). `creatng`
 *  may be NULL; some bonuses depend on the creature (a Vampire minds no Graveyard). */
int world_room_mod(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng);
/** Apply it: value * (100 + bonus) / 100. */
long world_room_apply(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng, long value);
/** The room a creature works in (the one it stands on), for the work hooks. */
long world_room_apply_here(const struct Thing *creatng, enum WorldRoomMod what, long value);
/** Neighbours: the room kinds (bit per RoomKind) of the same keeper's rooms sharing a wall with this room. */
uint32_t world_room_neighbour_kinds(const struct Room *room);
/** Lie of the Land: the biome under a room (-1: none), and what it does there (NULL: nothing). */
int world_room_biome(const struct Room *room);
const char *world_room_land_line(const struct Room *room);
/** A room's shape (worked out from the map every 128 turns) and its name ("a gallery"). */
enum WorldRoomShape world_room_shape(const struct Room *room);
const char *world_room_shape_name(enum WorldRoomShape shape);
/** Each machine, each turn: this machine's keeper hears when a room of theirs takes a shape. */
void world_rooms_present(void);
/** A world ends or a save is loaded: nothing worked out is kept. */
void world_rooms_reset(void);
/** Each world turn (server or single player): the test hook. */
void world_rooms_turn(void);

#ifdef __cplusplus
}
#endif
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_migrate.h
 *     Endless worlds: Move The Heart (the dungeon's migration).
 * @par Purpose:
 *     The user, 2026-11-19: from the Keeper panel, "Migrate Dungeon" turns the Hand into the Soul Container with a
 *     5x5 room highlight; it is set down on the keeper's own claimed, empty ground. Four Imps, one at each corner,
 *     carry it there, attackable on the way; one Imp frightened off and it drops (the task stays). "Cancel Migration"
 *     carries it back. At the new place it gets a new pedestal and flames; the old pedestal stays with its flames
 *     doused. The keeper's creatures follow it, and from then on use only rooms near it (until new ones are built).
 */
/******************************************************************************/
#ifndef DK_WORLD_MIGRATE_H
#define DK_WORLD_MIGRATE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Room;
struct Thing;

enum WorldMigrateState {
    WMig_None = 0,
    WMig_Going,     /* the Imps carry the Heart to its new home */
    WMig_Returning, /* called off: they carry it back to the old one */
};

#define WORLD_MIGRATE_ROOM_SIZE 5     /* the new home: 5x5 slabs */
#define WORLD_MIGRATE_ROOM_REACH 40   /* slabs from the Heart: farther rooms are left alone after a migration */

/** The keeper's migration: WMig_* (on every machine: struct Game). */
int world_migrate_state(PlayerNumber plyr_idx);
/** Imps holding the Heart now (0-4), and whether it is up on them. */
int world_migrate_carriers(PlayerNumber plyr_idx);

/* This machine's placing mode (the panel's Migrate Dungeon). */
void world_migrate_arm(void);
TbBool world_migrate_armed(void);
void world_migrate_disarm(void);
/** Called first among the dungeon view's inputs: an armed placing takes the clicks (left sets it down, right
 *  calls it off). True when it took them. */
TbBool world_migrate_inputs(void);
/** Before the 3D view draws the cursor's box: the 5x5 highlight under the pointer while placing. */
void world_migrate_cursor_box(void);
/** Draws the Soul Container where the Hand would be while placing; false when the Hand is drawn as usual. */
TbBool world_migrate_draw_hand(long cursor_x, long cursor_y);
/** Every turn, on the machine that shows the game: the arrival's sounds when the camera is near. */
void world_migrate_local_turn(void);
/** The panel's Cancel Migration. */
void world_migrate_request_cancel(void);

/** Game state (the simulating machine): a keeper's packet. what 1: carry the Heart to (stl_x, stl_y); 2: call it off. */
void world_migrate_packet(PlayerNumber plyr_idx, int what, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
void world_migrate_turn(void);
/** After a migration (or during one), a room of the keeper's farther than WORLD_MIGRATE_ROOM_REACH slabs from its
 *  Heart: its creatures (not its Imps) do not go there. */
TbBool world_migrate_room_too_far(const struct Thing *creatng, const struct Room *room);
/** Is "your creature cannot reach the room it needs" (the event, its text and voice) for this kind of room kept quiet:
 *  while the keeper's Soul Container is carried (the user, 2026-11-19: "disabled until the Soul Container reaches its
 *  destination"), and after a move while every room of the kind is out of reach (its creatures leave them alone on
 *  purpose)? */
TbBool world_migrate_quiet_room(PlayerNumber plyr_idx, RoomKind rkind);
/** A seat starts again or is taken by another player: no migration. */
void world_migrate_seat_reset(PlayerNumber plyr_idx);

size_t world_migrate_save_size(void);
void world_migrate_save(uint8_t *buf);
TbBool world_migrate_load(const uint8_t *buf, size_t len);
void world_migrate_reset(void);

#ifdef __cplusplus
}
#endif
#endif

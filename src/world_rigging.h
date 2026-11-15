/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rigging.h
 *     Endless world: rigging, devices linked to each other.
 * @par Purpose:
 *     A keeper links one of its Alarm traps to its other devices: "!rig" in the chat line with the cursor on the Alarm
 *     picks it, "!rig" on one of the keeper's doors or traps links it (again on the same one unlinks it); up to eight
 *     devices an Alarm. When the Alarm goes off, its linked doors lock and its linked traps fire. Only the keeper's own
 *     devices. Saved as the XTR2 record "RIG1". (The trigger plate, the door that counts heroes and the purse for a
 *     chain of three are not built: the plate needs its own art.)
 */
/******************************************************************************/
#ifndef DK_WORLD_RIGGING_H
#define DK_WORLD_RIGGING_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define RIGGING_LINKS_MAX 1024
#define RIGGING_PER_TRIGGER 8
#define RIGGING_COUNT_MAX 20 /* a counting door locks after at most this many heroes */

/** "!rig" (also with '/'): true when it was. */
TbBool world_rigging_command(const char *text, MapCoord cursor_x, MapCoord cursor_y);
/** The packet (simulating machine): the device at that subtile. */
void world_rigging_packet(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** A trap is set off by a creature: if it is a linked Alarm, its devices act. */
void world_rigging_triggered(struct Thing *traptng, struct Thing *creatng);
/** A counting door: it lets heroes through while unlocked (to count them), then locks behind them. */
TbBool world_rigging_counting_door(const struct Thing *doortng);
/** Each world turn (server or single player): the test hook; links to gone devices dropped. */
void world_rigging_turn(void);

void world_rigging_reset(void);
size_t world_rigging_save_size(void);
void world_rigging_save(uint8_t *buf);
TbBool world_rigging_load(const uint8_t *buf, size_t len);

/** Counting doors ("!count N" with the cursor on a door of the keeper's): saved as their own XTR2 record. */
size_t world_rigging_counters_save_size(void);
void world_rigging_counters_save(uint8_t *buf);
TbBool world_rigging_counters_load(const uint8_t *buf, size_t len);
void world_rigging_counters_reset(void);

#ifdef __cplusplus
}
#endif
#endif

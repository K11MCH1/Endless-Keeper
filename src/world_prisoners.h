/******************************************************************************/
// Endless Keeper: A Prisoner's Fate.
/******************************************************************************/
#ifndef WORLD_PRISONERS_H
#define WORLD_PRISONERS_H

#include <stddef.h>
#include <stdint.h>
#include "globals.h"
#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Dungeon;

/** A keeper drops a creature from the hand: a captive hero dropped on its jailer's Treasury Room is ransomed, on
 *  their Library interrogated. True when it was (the creature is gone). */
TbBool world_prisoners_dropped(struct Dungeon *dungeon, struct Thing *thing);
/** A raid's first party has landed for this keeper: ransomed heroes join it, a level higher. Returns how many. */
int world_prisoners_join_raid(PlayerNumber plyr_idx, struct Thing *leader, MapSlabCoord sx, MapSlabCoord sy);
void world_prisoners_turn(void);

void world_prisoners_reset(void);
size_t world_prisoners_save_size(void);
void world_prisoners_save(uint8_t *buf);
TbBool world_prisoners_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

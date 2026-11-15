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
/** Chain Gangs: how many captives are in irons (the checks below cost nothing while it is 0). */
extern int world_prisoners_gang_size;
/** Chain Gangs: is this creature in irons (owned by its jailer, working as a digger)? */
TbBool world_prisoners_chained(const struct Thing *thing);
/** Chain Gangs: an unguarded captive slacks (a quarter of its speed). */
long world_prisoners_speed(const struct Thing *thing, long speed);
/** Chain Gangs: a creature's chunk freezes: its irons for the frozen record (0: not in irons). */
uint8_t world_prisoners_freeze(const struct Thing *thing);
/** Chain Gangs: a frozen creature comes back with the irons it froze in. */
void world_prisoners_thaw(struct Thing *thing, uint8_t flags);
/** Chain Gangs: a gang takes no wage. */
GoldAmount world_prisoners_pay(const struct Thing *thing, GoldAmount pay);

void world_prisoners_reset(void);
size_t world_prisoners_save_size(void);
void world_prisoners_save(uint8_t *buf);
TbBool world_prisoners_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

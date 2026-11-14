/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_crusade.h
 *     Endless world: the Crusade Road (2026-11-13).
 * @par Purpose:
 *     A hero fortress a keeper has disturbed and left standing founds a new heroes outpost one region nearer to that
 *     keeper every two days, up to three. Outposts on the same road answer each other's alarms. Razing the fortress
 *     ends its road. A road never grows towards a keeper who is away or still new, nor closer than 100 slabs to a
 *     heart.
 */
/******************************************************************************/
#ifndef DK_WORLD_CRUSADE_H
#define DK_WORLD_CRUSADE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Has a road claimed this outpost region (it gets an outpost when its ground is made)? */
TbBool world_crusade_forced(int64_t rx, int64_t ry);
/** An outpost was broken into: the others on its road answer. */
void world_crusade_outpost_alarm(int64_t rx, int64_t ry, int level);
/** A keeper's creature disturbed this fortress (generator start chunk): its road will grow towards that keeper. */
void world_crusade_fortress_disturbed(int32_t fcx, int32_t fcy, PlayerNumber plyr_idx);
/** Each world turn (the simulation). */
void world_crusade_turn(void);

void world_crusade_reset(void);
size_t world_crusade_save_size(void);
void world_crusade_save(uint8_t *buf);
TbBool world_crusade_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

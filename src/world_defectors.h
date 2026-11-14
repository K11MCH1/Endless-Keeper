/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_defectors.h
 *     Endless world: Defectors (2026-11-13).
 * @par Purpose:
 *     An angry creature that walks out of its keeper's portal no longer just disappears: if another keeper on the
 *     server offers what it went without (the wages it was owed, food, a lair of its own), it comes out of one of
 *     that keeper's portals with its level (no higher than the new keeper's best creature), its name and its Rap
 *     Sheet, and a line on the sheet naming the keeper it left and why.
 */
/******************************************************************************/
#ifndef DK_WORLD_DEFECTORS_H
#define DK_WORLD_DEFECTORS_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;

#ifdef __cplusplus
extern "C" {
#endif

/** An angry creature steps into its keeper's portal (creature_leaves, before it goes): it may defect. */
/* true when another keeper took it in (a copy came out of its portal): the old one must not go back to its pool */
TbBool world_defectors_leaving(struct Thing *creatng);
/** Each world turn (the simulation): the Rap Sheet lines of the creatures each player looks at. */
void world_defectors_turn(void);
/** The Rap Sheet's line for the creature this machine's player looks at (NULL: none). */
const char *world_defectors_rap_line(ThingIndex thing_idx);

void world_defectors_reset(void);
size_t world_defectors_save_size(void);
void world_defectors_save(uint8_t *buf);
TbBool world_defectors_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

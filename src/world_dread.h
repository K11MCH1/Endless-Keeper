/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dread.h
 *     Endless world: the Ledger of Dread (2026-11-13).
 * @par Purpose:
 *     Raids are sized from a budget, the keeper's dread, instead of from their single best creature: the levels of
 *     the whole army, the gold held, the traps and doors standing, and how the last raids went. A keeper who has
 *     just lost most of their army gets a floor of mercy. Each keeper's dread shows as a skull meter under the dial.
 */
/******************************************************************************/
#ifndef DK_WORLD_DREAD_H
#define DK_WORLD_DREAD_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;

#ifdef __cplusplus
extern "C" {
#endif

/** The level a raid on this keeper is sized from (1-based, in place of the best creature's level), worked out now
 *  and logged. *mercy: the keeper has just lost most of their army (the raid is as small as its tier allows). */
int world_dread_raid_level(PlayerNumber plyr_idx, TbBool *mercy);
/** A raid's heroes: n were made for this keeper; one left the world alive; the raid is over. */
void world_dread_raid_made(PlayerNumber plyr_idx, int n);
void world_dread_raider_left(PlayerNumber plyr_idx);
void world_dread_raid_over(PlayerNumber plyr_idx);
/** A creature died: a human keeper's creature lost while a raid is on counts against the raid's outcome. */
void world_dread_creature_died(const struct Thing *thing);
/** A keeper's dungeon (re)starts: a clean ledger. */
void world_dread_seat_started(PlayerNumber plyr_idx);
/** Each turn of an endless world (the simulation): every 64 turns the meters are brought up to date. */
void world_dread_turn(void);
/** The skull meter, under the dial: at (x, y), w wide (screen pixels). */
void world_dread_draw(int x, int y, int w);

void world_dread_reset(void);
size_t world_dread_save_size(void);
void world_dread_save(uint8_t *buf);
TbBool world_dread_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

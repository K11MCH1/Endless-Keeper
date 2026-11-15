/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pacts.h
 *     Endless world: Hostage Pacts (2026-11-13).
 * @par Purpose:
 *     Two keepers living close together make a pact (a truce, an open border or an alliance), sealed by an exchange
 *     of hostages held in each other's Prison, well fed and untouched, while it lasts. Whoever breaks it leaves their
 *     hostage to the other.
 */
/******************************************************************************/
#ifndef DK_WORLD_PACTS_H
#define DK_WORLD_PACTS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** Do these two keepers have a pact (any kind): neither side's creatures harm the other's Heart or rooms. */
TbBool world_pact_spares(PlayerNumber a, PlayerNumber b);
/** Is this creature a hostage of a pact that holds (no Hand may take it, nothing harms it)? */
TbBool world_pact_hostage(const struct Thing *creatng);
/** A seated player's "!pact ..." line on a dedicated server: 1 (handled). */
TbBool world_pact_command(int user, PlayerNumber plyr_idx, const char *args);
/** The simulation: hostages kept fed and in place, alliances kept, pacts of gone keepers ended. */
void world_pacts_turn(void);

/** Saved in the world save (the PCT1 extra). */
/** A seat gets a new dungeon (starts again, or a new player): its pacts end. */
void world_pacts_player_reset(PlayerNumber plyr_idx);
void world_pacts_reset(void);
size_t world_pacts_save_size(void);
void world_pacts_save(uint8_t *buf);
TbBool world_pacts_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

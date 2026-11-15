/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wars.h
 *     Endless world: Portal Wars (2026-11-13).
 * @par Purpose:
 *     A war two keepers arrange for a stake: after a day to prepare, up to twelve of the attacker's creatures step
 *     out of the defender's portal at dusk and must carry the flame of the defender's Heart back into it before they
 *     all fall or dawn comes. The stake goes to the winner; nothing else is lost and no Heart is destroyed.
 */
/******************************************************************************/
#ifndef DK_WORLD_WARS_H
#define DK_WORLD_WARS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** A war's attacker is felled: sent home hurt instead (true: the kill is off). */
TbBool world_war_spare_fallen(struct Thing *creatng);
/** May this keeper's creatures harm that keeper's Heart? false while a war of theirs is on (no Heart is destroyed). */
TbBool world_war_spares_heart(PlayerNumber dealer, PlayerNumber heart_owner);
/** A seated player's "!war ..." line on a dedicated server: 1 (handled). */
TbBool world_war_command(int user, PlayerNumber plyr_idx, const char *args);
void world_wars_turn(void);
/** This machine: the flame on its carrier. */
void world_wars_local_turn(void);

/** A seat gets a new dungeon (starts again, or a new player): its wars are called off. */
void world_wars_player_reset(PlayerNumber plyr_idx);
void world_wars_reset(void);
size_t world_wars_save_size(void);
void world_wars_save(uint8_t *buf);
TbBool world_wars_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

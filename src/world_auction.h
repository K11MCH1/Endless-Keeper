/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_auction.h
 *     Endless world: The Auction House (2026-11-13).
 * @par Purpose:
 *     Keepers on one server sell each other their own creatures and the heroes they hold captive, for gold, within
 *     price limits and a daily limit on buying; the portals deliver what was bought.
 */
/******************************************************************************/
#ifndef DK_WORLD_AUCTION_H
#define DK_WORLD_AUCTION_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A seated player's "!sell", "!buy", "!market" or "!unsell" line on a dedicated server: 1 (handled). */
TbBool world_auction_command(int user, PlayerNumber plyr_idx, const char *word, const char *args);
void world_auction_turn(void);

/** A seat gets a new dungeon (starts again, or a new player): its offers come down. */
void world_auction_player_reset(PlayerNumber plyr_idx);
void world_auction_reset(void);
size_t world_auction_save_size(void);
void world_auction_save(uint8_t *buf);
TbBool world_auction_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

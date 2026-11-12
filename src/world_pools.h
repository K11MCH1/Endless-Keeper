/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pools.h
 *     Endless world: a creature pool for each keeper (2026-11-12).
 * @par Purpose:
 *     The level script fills one pool for the whole world, and every keeper's portal drew from it, so on a server the
 *     first keepers took the scarce kinds. Each human keeper now has their own copy of the script's pool, made when
 *     their dungeon starts (again after Start again or a reset); their portals draw from it and their own creatures'
 *     deaths and dismissals return to it. Computer keepers and heroes keep the world's pool.
 */
/******************************************************************************/
#ifndef DK_WORLD_POOLS_H
#define DK_WORLD_POOLS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The keeper's own pool (counts by creature model), or NULL: not an endless world, or a keeper without one (it uses
 *  the world's). A human keeper of a world saved before the pools gets one the first time it is asked for. */
int32_t *world_pool_for(PlayerNumber plyr_idx);
/** The script's pool, as the world began (taken on the first world turn). */
void world_pools_first_turn(void);
/** A human keeper's dungeon (re)starts: its pool is a fresh copy of the script's. */
void world_pools_seat_started(PlayerNumber plyr_idx);

void world_pools_reset(void);
/** Test hook KFX_POOL_TEST=1 (each turn on a server). */
void world_pools_test(void);
size_t world_pools_save_size(void);
void world_pools_save(uint8_t *buf);
TbBool world_pools_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

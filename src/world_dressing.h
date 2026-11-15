/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dressing.h
 *     Endless world: dressing the dungeon, a biome's look for a keeper's dungeon.
 * @par Purpose:
 *     A keeper unlocks a biome's look (its walls and floors: the biome's texture pack) by destroying five heroes'
 *     strongholds in that biome. "!look <biome>" in the chat line dresses the keeper's own slabs in it, "!look off"
 *     takes it off, "!look" lists what is unlocked (a packet; the answer reaches a joined keeper too). It is only for
 *     show; the look is each slab's texture, which everyone sees. The keeper's slabs in the loaded world are dressed
 *     (or undressed) one chunk a turn. Saved as the XTR2 record "DRS1".
 */
/******************************************************************************/
#ifndef DK_WORLD_DRESSING_H
#define DK_WORLD_DRESSING_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRESSING_STRONGHOLDS 5

/** "!look ..." (also with '/'): true when it was one. */
TbBool world_dressing_command(const char *text);
/** The packet (simulating machine): biome + 1 to wear (0: off, 255: list). */
void world_dressing_packet(PlayerNumber plyr_idx, int what);
/** A heroes' stronghold fell to a keeper at this subtile (its heart). */
void world_dressing_stronghold_destroyed(PlayerNumber by, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Each world turn (server or single player): one chunk dressed; the test hook. */
void world_dressing_turn(void);

void world_dressing_reset(void);
size_t world_dressing_save_size(void);
void world_dressing_save(uint8_t *buf);
TbBool world_dressing_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): no looks unlocked but the Earthen one, none worn. */
void world_dressing_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

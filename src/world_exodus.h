/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_exodus.h
 *     Endless world: moving the Heart.
 * @par Purpose:
 *     "!exodus" in the chat line orders the Exodus at a Heart above three quarters of its health. "!settle", with the
 *     cursor on open ground the keeper has seen at least 40 slabs from the Heart, sets the Heart down there: a new
 *     pedestal rises round it (the keeper's claimed floor), the Heart arrives at half its health, and the old pedestal
 *     is left as neutral ruins. Not within 20 slabs of another keeper's land (an ally's is fine). "!exodus" again
 *     calls it off. (The road between, the ember carried by imps, is not
 *     built.) Saved as the XTR2 record "EXO1".
 */
/******************************************************************************/
#ifndef DK_WORLD_EXODUS_H
#define DK_WORLD_EXODUS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXODUS_MIN_SLABS 40
#define EXODUS_NEIGHBOUR_SLABS 20 /* no other keeper's land (an ally's is fine) this near the new Heart */

/** "!exodus" or "!settle" (also with '/'). */
TbBool world_exodus_command(const char *text, MapCoord cursor_x, MapCoord cursor_y);
/** The packet (simulating machine): what 1 = order or call off the Exodus, 2 = settle at subtile (x, y). */
void world_exodus_packet(PlayerNumber plyr_idx, int what, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Each world turn (server or single player): the test hook. */
void world_exodus_turn(void);

void world_exodus_reset(void);
size_t world_exodus_save_size(void);
void world_exodus_save(uint8_t *buf);
TbBool world_exodus_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): no Exodus ordered. */
void world_exodus_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

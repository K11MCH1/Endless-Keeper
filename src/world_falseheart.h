/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_falseheart.h
 *     Endless world: the false Heart, a decoy for raids.
 * @par Purpose:
 *     A keeper with a Workshop sets one decoy Heart on open ground it has seen ("!falseheart" in the chat line, at the
 *     cursor; 500 gold; one at a time). A raid on that keeper whose landing is closer to the decoy than to the real
 *     Heart sends its leader digging to the decoy instead, unless the raid sees through it (one raid in four). When the
 *     leader reaches the decoy it smashes it, and the raid sets off for the real Heart. Saved as the XTR2 record
 *     "FHT1" (the decoys and the leaders on their way to one).
 */
/******************************************************************************/
#ifndef DK_WORLD_FALSEHEART_H
#define DK_WORLD_FALSEHEART_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define FALSEHEART_PRICE 500
#define FALSEHEART_SEE_THROUGH 25   /* percent of raids */
#define FALSEHEART_REACH 2          /* slabs: the leader smashes it this close */
#define FALSEHEART_LURES 2          /* raids a false Heart can draw at most, then it fools no more */

/** "!falseheart" (or /falseheart): asks for the decoy at the cursor's subtile. True when it was one. */
TbBool world_falseheart_command(const char *text, MapCoord cursor_x, MapCoord cursor_y);
/** The packet (simulating machine). */
void world_falseheart_place(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** A raid's leader is sent at a keeper: true when it was sent to the keeper's decoy instead of the Heart.
 *  landing: where the party landed (engine slabs). */
TbBool world_falseheart_lure(struct Thing *leader, PlayerNumber plyr_idx, MapSlabCoord land_x, MapSlabCoord land_y, uint32_t salt);
/** Each world turn (server or single player): leaders at a decoy smash it; the test hook. */
void world_falseheart_turn(void);

void world_falseheart_reset(void);
size_t world_falseheart_save_size(void);
void world_falseheart_save(uint8_t *buf);
TbBool world_falseheart_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): the keeper's false Heart is taken away, no raid is lured to it. */
void world_falseheart_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

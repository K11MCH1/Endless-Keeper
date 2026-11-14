/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_watchfire.h
 *     Endless world: watchfires.
 * @par Purpose:
 *     A keeper with a Workshop sets a watchfire down on ground it has seen ("!watchfire" in the chat line, at the
 *     cursor): 300 gold, at most two burning for each Workshop, and 100 gold every night it burns (unpaid, it goes
 *     out). It burns for one day. Within its light (6 slabs):
 *     - no wild pack appears and no burrow opens: one that would comes up at the light's edge instead, one bigger;
 *     - the ground stays revealed to its keeper;
 *     - the keeper's creatures keep their nerve. At night a creature more than 40 slabs from its keeper's Heart and
 *       outside its keeper's lit fires loses its nerve: it flees when wounded at half as much health again as it
 *       otherwise would (a creature with the vice Afraid of Nothing in the Dark never does).
 *     Saved as the XTR2 record "WFR1".
 */
/******************************************************************************/
#ifndef DK_WORLD_WATCHFIRE_H
#define DK_WORLD_WATCHFIRE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define WATCHFIRE_REACH 6          /* slabs */
#define WATCHFIRE_PER_WORKSHOP 2
#define WATCHFIRE_PRICE 300
#define WATCHFIRE_NIGHT_PRICE 100
#define WATCHFIRE_NERVE_SLABS 40   /* "far from home" */
#define WATCHFIRES_MAX 256

/** The chat line "!watchfire" (or /watchfire): asks for a watchfire at the cursor's subtile. True when it was one. */
TbBool world_watchfire_command(const char *text, MapCoord cursor_x, MapCoord cursor_y);
/** The packet (simulating machine): player plyr_idx sets a watchfire at that subtile. */
void world_watchfire_place(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Is this engine slab within a lit watchfire's light (any keeper's)? */
TbBool world_watchfire_covers(MapSlabCoord x, MapSlabCoord y);
/** ... within the light of one of this keeper's lit watchfires? */
TbBool world_watchfire_covers_for(PlayerNumber owner, MapSlabCoord x, MapSlabCoord y);
/** Wild packs and burrows: a spot within a fire's light is moved to its edge; true when it was moved. */
TbBool world_watchfire_push(MapSlabCoord *x, MapSlabCoord *y);
/** Has this creature lost its nerve (night, far from home, no fire, no vice)? */
TbBool world_watchfire_unnerved(const struct Thing *creatng);
/** Each world turn (server or single player): night prices, fires going out, the revealed ground, the test hook. */
void world_watchfire_turn(void);

void world_watchfire_reset(void);
size_t world_watchfire_save_size(void);
void world_watchfire_save(uint8_t *buf);
TbBool world_watchfire_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): the keeper's watchfires go out (no more nights to pay). */
void world_watchfire_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

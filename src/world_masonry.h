/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_masonry.h
 *     Endless world: backfill and masonry, imps putting ground back.
 * @par Purpose:
 *     Two orders, given in the chat line with the cursor over a slab ("!backfill", "!masonry"; a packet to the
 *     server):
 *     - Backfill: open floor (bare path, or the keeper's own claimed floor) is filled in with earth.
 *     - Masonry: on the keeper's own claimed floor, the keeper's reinforced wall is built (150 gold a slab, paid when
 *       it is built; it waits while the keeper can't pay).
 *     An imp of the keeper is sent to stand beside the slab; once imps have worked beside it for 96 turns (two imps,
 *     half the time) and no creature stands on it, it is done. Neither order works on another keeper's land or right
 *     beside it (eight slabs round), nor while another keeper's creature stands on the slab, so no keeper can wall in
 *     another's creature. Orders are saved (XTR2 record "MSN1").
 */
/******************************************************************************/
#ifndef DK_WORLD_MASONRY_H
#define DK_WORLD_MASONRY_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

enum WorldMasonryOrder { WMo_Backfill = 1, WMo_Masonry = 2 };
#define MASONRY_PRICE 150
#define MASONRY_WORK_TURNS 96
#define MASONRY_ORDERS_MAX 512
#define MASONRY_ORDERS_PER_KEEPER 32

/** "!backfill" or "!masonry" (also with '/'): asks for the order at the cursor's subtile. True when it was one. */
TbBool world_masonry_command(const char *text, MapCoord cursor_x, MapCoord cursor_y);
/** The packet (simulating machine). */
void world_masonry_order(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y, int kind);
/** Each world turn (server or single player): imps sent, work done; the test hook. */
void world_masonry_turn(void);

void world_masonry_reset(void);
size_t world_masonry_save_size(void);
void world_masonry_save(uint8_t *buf);
TbBool world_masonry_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

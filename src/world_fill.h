/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_fill.h
 *     Endless worlds: the Fill room (Imps fill marked floor with earth).
 * @par Purpose:
 *     The user, 2026-11-19: a room button, FILL, in the Rooms tab. Placed like a room (the same highlight, green on
 *     empty floor that is no water, lava or room and has no trap, object or creature on it), it costs 30 gold a slab
 *     (the price shown as the Training Room shows its pay) and leaves a wireframe on the slab. An Imp goes to it, digs
 *     at it, and instead of digging it out fills it with an earth wall. A fill that would shut a creature (or the Imp
 *     itself) in waits. Sell Room takes marks away (no gold back). See world_fill.c.
 */
/******************************************************************************/
#ifndef DK_WORLD_FILL_H
#define DK_WORLD_FILL_H

#include "globals.h"
#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Dungeon;
struct DiggerStack;
struct Room;

/** The FILL room kind of the loaded config, or -1 when it has none. */
int world_fill_room_kind(void);
TbBool world_fill_is_kind(RoomKind rkind);
/** can_build_room_at_slab for FILL: empty floor (own claimed, or unclaimed path), not marked yet. */
TbBool world_fill_can_mark(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** keeper_build_room for FILL: pays and marks the slab. True when it was marked. */
TbBool world_fill_place(int user, PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** Sell Room over a mark of the player's: it is taken away (no gold back). True when there was one. */
TbBool world_fill_unmark(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);
TbBool world_fill_marked_by(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** Imps do not reinforce from here: a marked slab, or a pocket a waiting fill would close. */
TbBool world_fill_keep_out(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** The panel's meter: fills done of the marks placed since the last were all done, 0-256; -1: no marks. */
int world_fill_meter(PlayerNumber plyr_idx);
/** The 3D view: wireframes over the local player's marks (isometric: camera at x,y,z; front view: frontview=1). */
void world_fill_draw_marks(long cam_x, long cam_y, long cam_z, int frontview, void *cam, unsigned char stl_width);

/** The digger stack (spdigger_stack.c): the keeper's marks as DigTsk_FillSlab tasks. */
int world_fill_add_to_imp_stack(struct Dungeon *dungeon, int max_tasks);
long world_fill_check_out_worker(struct Thing *thing, struct DiggerStack *dstack);
/** The Imp's states (crstates.cfg ImpArrivesAtFill, ImpFills). */
short imp_arrives_at_fill(struct Thing *thing);
short imp_fills(struct Thing *thing);
/** instf_dig: a stroke of a filling Imp. True when it was one (the dig itself is left out). */
TbBool world_fill_dig_hit(struct Thing *creatng);
/** crstates.cfg without the two states (an older file): they are made here. */
void world_fill_states_fallback(void);

/** custom_sprites.c, after the fxdata zips: the icons ENDLESS_FILL_BIG, _BUTTON and _CURSOR (built into the program). */
void world_fill_register_icons(void);

void world_fill_turn(void);
void world_fill_seat_reset(PlayerNumber plyr_idx);
size_t world_fill_save_size(void);
void world_fill_save(uint8_t *buf);
TbBool world_fill_load(const uint8_t *buf, size_t len);
void world_fill_reset(void);

#ifdef __cplusplus
}
#endif
#endif

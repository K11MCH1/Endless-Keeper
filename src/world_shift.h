/******************************************************************************/
/** @file world_shift.h
 *     Endless world: moving the whole game state by whole chunks.
 */
/******************************************************************************/
#ifndef DK_WORLD_SHIFT_H
#define DK_WORLD_SHIFT_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Moves every position in the game state by (-dslb_x, -dslb_y) slabs: the map arrays (map, slabmap,
 *  slab extension data) and every coordinate field that the save tables describe and the shift table marks,
 *  plus a few things the save tables don't cover (raw creature job data, slab object parents, local
 *  cameras). Positions that end up outside the map are clamped into the rock frame. The uncovered band is
 *  left empty (no columns, no things): the caller fills it, then calls world_shift_rebuild(). */
void world_shift_state(MapSlabCoord dslb_x, MapSlabCoord dslb_y);
/** Rebuilds what is derived from the map, as loading a saved game does (navigation, lights, minimap cache),
 *  recomputes the ceiling over the filled band and the frame (it moved with the map elsewhere), and makes
 *  creatures re-plan their routes. */
void world_shift_rebuild(MapSubtlCoord band_sx, MapSubtlCoord band_sy, MapSubtlCoord band_ex, MapSubtlCoord band_ey);
/** Checks the shift table against the save tables: logs every position-like field that the table doesn't
 *  mention. Returns how many it found. */
int world_shift_audit(void);

#ifdef __cplusplus
}
#endif
#endif

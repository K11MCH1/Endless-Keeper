/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file ariadne_update.h
 *     Header file for ariadne_update.c.
 * @par Purpose:
 *     map updates for Ariadne system support.
 * @par Comment:
 *     Just a header file - #defines, typedefs, function prototypes etc.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef DK_ARIADNE_UPDATE_H
#define DK_ARIADNE_UPDATE_H

#include "bflib_basics.h"
#include "globals.h"

enum NavigationStateOperation {
    NavigationState_Store,
    NavigationState_Restore,
};

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/

/******************************************************************************/
long update_navigation_triangulation(long start_x, long start_y, long end_x, long end_y);
/** Endless worlds: navigation updates wait (colours kept) until nav_defer_end, which triangulates what they touched
 *  outside the area the caller refreshes next (start/end as nav_triangulate_paged takes them). */
void nav_defer_begin(void);
void nav_defer_end(long refresh_sx, long refresh_sy, long refresh_ex, long refresh_ey);
int32_t init_navigation(void);
void rebuild_navigation(void);
size_t transfer_navigation_state(void *buffer, enum NavigationStateOperation operation);
/** Recomputes the navigation colours from the map, leaving the mesh as it is (after a network resync). */
void navigation_colours_refresh(void);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

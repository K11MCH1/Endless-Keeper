/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_screen.h
 *     Endless worlds: The Pentagram's own screen (see world_pentagram_screen.c).
 */
/******************************************************************************/
#ifndef DK_WORLD_PENTAGRAM_SCREEN_H
#define DK_WORLD_PENTAGRAM_SCREEN_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

TbBool world_pentagram_screen_open(void);
void world_pentagram_screen_show(void);
void world_pentagram_screen_close(void);
/** Android (2026-11-28): a pinch zooms by e^log_scale round a screen point (game pixels), following the fingers. */
void world_pentagram_screen_pinch(float log_scale, long screen_x, long screen_y);
int world_pentagram_screen_test_all(TbBool reverse);
void world_pentagram_screen_test_motion(long step); /* test hook: pan and zoom */ /* test hook: learn everything through the screen */
/** Each frame (GameUI, over everything but the pointer). */
void world_pentagram_screen_draw(void);
/** The input loop, first: true while it is open (it takes everything). */
TbBool world_pentagram_screen_inputs(void);

#ifdef __cplusplus
}
#endif
#endif

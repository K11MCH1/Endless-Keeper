/******************************************************************************/
// Endless Keeper - screen safe area for the UI
/******************************************************************************/
/** @file ui_safe_area.h
 *     Phones with rounded corners and camera hole punches cut into the screen's edges (2026-11-24). The 3D world
 *     still fills the whole screen; the UI (the side panel with the minimap, the dial and World Boss strip, messages,
 *     timers, dialogs, the menus) keeps inside the safe area: the screen less these insets.
 *     The insets come from the window system (on Android: the camera cutout and the corners' radii, or the margin
 *     the player set in the launcher); elsewhere they are 0 and nothing moves.
 */
/******************************************************************************/
#ifndef UI_SAFE_AREA_H
#define UI_SAFE_AREA_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Insets from each edge of the game's screen, in its pixels (MyScreenWidth x MyScreenHeight). */
extern int ui_safe_left;
extern int ui_safe_top;
extern int ui_safe_right;
extern int ui_safe_bottom;

/** Reads the insets again (after a screen mode change, or when the window system reports new ones).
 *  True when they changed. */
TbBool ui_safe_area_refresh(void);
/** The same, and lays the in-game menus out again if they changed while a level is shown. */
void ui_safe_area_changed(void);

/** The safe area's right and bottom edges (exclusive), in screen pixels. */
int ui_safe_right_edge(void);
int ui_safe_bottom_edge(void);

#ifdef __cplusplus
}
#endif

#endif

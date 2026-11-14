/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_creature_window.h
 *     Endless worlds: the creature window.
 * @par Purpose:
 *     What the Information tool shows about a creature, in a window of its own in the middle of the view: the
 *     portrait, mood, level and name, its ten abilities, its twenty statistics, and the Rap Sheet (the record,
 *     contracts and vices). The side panel keeps its own tab.
 */
/******************************************************************************/
#ifndef DK_WORLD_CREATURE_WINDOW_H
#define DK_WORLD_CREATURE_WINDOW_H

#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct GuiMenu world_creature_window_menu;

/** Is the creature window the place a queried creature is shown in (an endless world)? */
TbBool world_creature_window_used(void);
/** Is it on the screen now? (What the view draws over itself - messages, the day's dial - goes under it then.) */
TbBool world_creature_window_open(void);
/** The side panel's menu to show for a queried creature: the given page of the Information Panel, or, where the
 *  creature window is used, the Information tab itself (the window has what the pages had). */
int world_creature_window_panel_menu(int menu);
/** Every turn, on the machine that shows the game: the window is open exactly while this player looks at a creature
 *  with the Information tool. */
void world_creature_window_turn(void);

/* The window's lettering, for what is drawn into it from elsewhere (the Rap Sheet, world_misdeeds.c).
 * Places are in pixels: (x, y) is the top left corner of the capitals themselves, `cap` their height, `w` the room
 * the line has; align 0 left, 1 centre, 2 right. A line too long for its room is drawn smaller, never cut. */
enum WorldWindowInk { WwI_Bright, WwI_Dim, WwI_Gold, WwI_Rule };
#define WORLD_WINDOW_LINE 128
/** A colour of the window's lettering. */
unsigned char world_window_ink(int ink);
/** Text to be read: the game's plain font in one colour over a dark shadow, at whole pixels where that is near. */
void world_window_plain(int x, int y, int w, int cap, int align, unsigned char colour, const char *text);
/** How wide world_window_plain draws the text with room enough. */
int world_window_plain_width(const char *text, int cap);
/** The plain font's own capitals, in pixels: smaller than this its letters lose rows and columns. */
int world_window_plain_cap(void);
/** Splits text at its spaces into lines no wider than w: fills `lines`, returns how many (at most `max`; what is
 *  left over stays in the last, which is then drawn smaller). */
int world_window_plain_wrap(const char *text, int cap, int w, char lines[][WORLD_WINDOW_LINE], int max);
/** A heading or a button's word: the game's own lettering. */
void world_window_display(int x, int y, int w, int cap, int align, const char *text);
int world_window_display_width(const char *text, int cap);

#ifdef __cplusplus
}
#endif
#endif

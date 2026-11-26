/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_level_window.h
 *     Endless worlds: the Library's spellbook and the Rooms & Spells To Level window (world_level_window.c).
 */
/******************************************************************************/
#ifndef DK_WORLD_LEVEL_WINDOW_H
#define DK_WORLD_LEVEL_WINDOW_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuiMenu;
extern struct GuiMenu world_level_window_menu;

TbBool world_level_window_open(void);
void world_level_window_show(void);
void world_level_window_close(void);
/** Drawn after every other menu, over the panels (GameUI.cpp), with its tooltip. */
void world_level_window_draw_last(void);
/** The spellbook over the Library the Hand is on: on the Hand's layer, just before the Hand (GameUI.cpp). */
void world_level_book_draw(void);
/** A click on the book opens the window (front_input.c); true when the click was the book's. */
TbBool world_level_window_inputs(void);
/** Each turn on every machine but a dedicated server: the test hook KFX_WORLD_LEVELTEST. */
void world_level_window_turn(void);

/** The pointer is on the floating spellbook (drawn this frame or the last). */
TbBool world_level_book_hovered(void);

#ifdef __cplusplus
}
#endif
#endif

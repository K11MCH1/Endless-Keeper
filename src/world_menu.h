/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_menu.h
 *     Endless Mode: the main-menu button, its menu (Continue, New Run, Load Game), the list of endless saves,
 *     and autosaving endless runs.
 */
/******************************************************************************/
#ifndef DK_WORLD_MENU_H
#define DK_WORLD_MENU_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuiButton;
struct GuiMenu;

/** The map pack Endless Mode plays (levels/endless.cfg); its saves are the endless saves. */
#define ENDLESS_MAPPACK_FNAME "endless.cfg"

extern struct GuiMenu world_endless_menu;
extern struct GuiMenu world_endless_load_menu;

/** Draws the main menu's "Endless Mode" button. */
void world_menu_draw_endless_button(struct GuiButton *gbtn);
/** The main menu's "Endless Mode" button, every frame (runs the KFX_WORLD_MENU test hook). */
void world_menu_endless_maintain(struct GuiButton *gbtn);

/** Frontend state set-up for the two Endless Mode screens. */
void world_menu_setup(void);
/** The player chose Host Network Game in the Endless Mode menu (cleared at the main menu). */
extern TbBool world_menu_mp_host;
/** The lobby's Start: when hosting an endless run, starts it (the start chat) and returns true. */
TbBool world_menu_lobby_start(void);
/** A seed nobody has played (1..99999999). */
void world_menu_fresh_seed(char *seed, size_t len);
/** The newest endless save's slot, or -1. */
int world_menu_newest_endless_slot(void);
/** Test hook KFX_WORLD_MENU: one step every 2.5 s (also ticked by the network screens). */
void world_menu_test_tick(void);
void world_menu_load_setup(void);
extern struct GuiMenu world_list_menu;
void world_list_menu_setup(void);
void world_list_menu_close(void);
/** Host: the new run opens to the network at its second turn. */
extern TbBool world_menu_open_on_start;
/** A thin client left its world: the next menu is the World List, showing the message (NULL: none). */
void world_menu_return_to_list(const char *message);
TbBool world_menu_take_return_to_list(void);

/** Is the loaded campaign the Endless Mode map pack? (Leaving such a level returns to the Endless Mode menu.) */
TbBool world_menu_endless_campaign_loaded(void);

/** Autosave of an endless run, in its own slot: at dawn and when the player quits to the menu. */
void world_autosave(const char *why);
int world_menu_slot_for_seed(const char *seed);
int world_menu_new_world_state(void);
void world_menu_write_index(void);

#ifdef __cplusplus
}
#endif
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file chat_box.h
 *     Endless Keeper: the chat box of a network endless game (2026-11-10). Chat and the server's lines go into a box
 *     in the style of the game's message dialogs (not to the top of the screen); the chat icon at the bottom of the
 *     event column opens and closes it, and its eye leaves only the lines, which fade.
 */
/******************************************************************************/
#ifndef DK_CHAT_BOX_H
#define DK_CHAT_BOX_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct GuiButton;
struct GuiMenu;

enum ChatBoxMode {
    CBM_Open = 0, /* the box */
    CBM_Faded,    /* the eye: the lines alone where the box was, fading after a while */
    CBM_Closed,   /* closed by the chat icon: the icon blinks for new lines */
    CBM_Count
};

extern struct GuiMenu chat_box_menu;

/** A network endless game on a player's machine (not a dedicated server, not single player). */
TbBool chat_box_active(void);
/** A chat line (plyr_idx: its keeper) or the server's line (plyr_idx -1) into the box. */
void chat_box_add(PlayerNumber plyr_idx, const char *text);
/** Opens, closes and places the box (each frame and each turn). */
void chat_box_turn(void);
/** The lines alone, where the box would be: the eye, or a view without the box (possessing a creature). */
void chat_box_draw_faded(void);
/** The typing line is in the box (or where it would be), not at the top of the screen. */
TbBool chat_box_draws_typing(void);
/** A new session: the last server's lines go. */
void chat_box_reset(void);

/* The chat icon: the event column's bottom button; the events show one slot up. */
TbBool chat_box_icon_button(const struct GuiButton *gbtn);
/** The event slot an event button shows. */
long event_button_slot(const struct GuiButton *gbtn);
void chat_box_icon_draw(struct GuiButton *gbtn);
void chat_box_icon_click(struct GuiButton *gbtn);
void chat_box_icon_maintain(struct GuiButton *gbtn);

/* settings.toml [network] chat_box */
int chat_box_mode_setting(void);
void chat_box_mode_load(int mode);
/** Test hook (KFX_NET_TESTSAY "#cb <what>"). */
void chat_box_test(const char *what);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

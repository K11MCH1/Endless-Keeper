/******************************************************************************/
// Endless Keeper - the side panel in the Molten look
/******************************************************************************/
/** @file world_side_panel.h
 *     The game's side panel (the minimap, the gold, the five tabs and their icons) in the launcher's Molten look on
 *     endless worlds with WINDOW_STYLE=MODERN (the user, 2026-11-26: "its time to modernize these. Same Molten
 *     design."). The game's own buttons stay - their clicks, tooltips and upkeep - moved to the new layout; what they
 *     drew is painted in true colour under the game's UI (the minimap, tooltips and the Hand stay over it).
 */
/******************************************************************************/
#ifndef WORLD_SIDE_PANEL_H
#define WORLD_SIDE_PANEL_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuiButton;

/** Whether the modern side panel is in use (endless world, modern style, the game's panel up). */
TbBool world_side_panel_on(void);
/** Moves the game's panel buttons to the modern layout (before the GUI's input and its drawing, each frame). */
void world_side_panel_place(void);
/** Paints and shows the panel under the game's UI (instead of the panel art and the gold). */
void world_side_panel_draw(void);
/** The compass letters round the minimap, over it (where the game drew its compass). */
void world_side_panel_draw_over(void);
/** True when the modern panel paints this button itself (the game's drawing of it is left out). */
TbBool world_side_panel_paints(const struct GuiButton *gbtn);

struct TbSprite;
/** Whether the view's own pieces (the day's dial, the World Boss strip) are in the modern look (endless, modern). */
TbBool world_side_hud_modern(void);
/** The modern side panel slides away (2026-11-26): its tab under the event buttons (the mouse over it; its click), TAB
 *  (true: done, the classic toggle left alone), and the game's minimap left out while the panel is away. */
/** The message box at the foot of the view in the Molten look (2026-11-27): its drawing (true: done) and its words'
 *  button's maintain (the wheel, the scroll bar). */
struct GuiMenu;
TbBool world_ui_textbox_draw(struct GuiMenu *gmnu);
void world_ui_textbox_maintain(struct GuiButton *gbtn);
TbBool world_side_tab_over(void);
void world_side_tab_click(void);
TbBool world_side_hud_toggle(void);
TbBool world_side_minimap_away(void);
/** The day's dial at the top right in a gold ring, the year's days round it, "Day N" (or "Night N") under it: `disc`
 *  is the dial's picture as the game turns it. Gives the y under it (for the World Boss strip). */
int world_side_dial_draw(const struct TbSprite *disc, int frame);
/** The World Boss strip under the dial: square and against the screen's right edge, `left` dim, `right` in ember.
 *  Gives its box (x, y, w, h) for its button. */
struct GuiMenu;
/** A menu with the game's ornate frame (Options, Quit, Graphics, Sound, the assistant, Load, Save) in the modern look:
 *  true when drawn here (gui_pretty_background draws nothing more). */
TbBool world_ui_menu_draw(struct GuiMenu *gmnu);
/** True when such a menu's button is painted with it (the game's drawing of it is left out). */
TbBool world_ui_menu_paints(const struct GuiButton *gbtn);
/** The main menu in the modern look: a button (frontend_draw_button) gathered to be painted with the others (true:
 *  the game draws nothing more), the version lines the same, and all of them painted after the menu's drawing. */
TbBool world_ui_front_button(const struct GuiButton *gbtn, const char *text);
TbBool world_ui_front_version(int y, int h, const char *line1, const char *line2);
void world_ui_front_flush(void);
/** An edited icon (a PNG at its place in fxdata/ui_icons/ or new ui/Modern UI icons/) for an icon the modern look draws:
 *  `kind` 0 a panel icon, 1 a framed button's, 2 an activity's, 3 an event sign, 4/5 a menu icon, 10 a tab (spr its
 *  number). NULL: none (the caller cuts the game's sprite); else malloc'd straight RGBA, the caller keeps it. */
unsigned char *world_ui_icon_override(int kind, long spr, int *w, int *h);
/** The "Paused" box in the modern look (frontmenu_ingame_evnt.c): a button at the game's place for it. True: drawn. */
TbBool world_ui_paused(const char *text, int x, int y, int w, int h, TbBool hot);
/** The pointer's tooltip in the modern look (gui_tooltips.c): true when shown here (the game draws nothing more). */
TbBool world_ui_tooltip(const char *text, long pos_x, long pos_y);
void world_side_boss_strip(int y, const char *left, const char *right, TbBool clickable, TbBool hover, int *box);

#ifdef __cplusplus
}
#endif
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_tools.h
 *     Endless world: the Keeper's Tools panel.
 * @par Purpose:
 *     One window listing the keeper's chat-command tools as buttons with their cost: the Rap Sheet, Attunement, the
 *     Cookbook, Heresies, Unions, the dungeon's looks, Watchfire, Backfill, Masonry, the False Heart, the Champion
 *     (make and possess), Rigging, the Exodus and Settle. Opened by the K key or the small "Tools" button at the top
 *     right of the screen; K or Close shuts it. A button does what its chat command does. A tool that acts at a place
 *     arms the cursor: the next click on the map uses it there (Backfill, Masonry and Rigging stay armed for more
 *     clicks), a right click cancels. Each tool's own command makes the packet, so a joined player's clicks go to the
 *     server as typed commands do. The chat commands stay.
 */
/******************************************************************************/
#ifndef DK_WORLD_TOOLS_H
#define DK_WORLD_TOOLS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuiMenu;
extern struct GuiMenu world_tools_menu;

/** The dungeon view's inputs (before the stock ones): the K key, the on-screen button, an armed tool's map click or
 *  right click. True when the input was taken. */
TbBool world_tools_inputs(void);
/** Drawn over the dungeon view each frame: the "Tools" button and an armed tool's hint line. */
void world_tools_draw_overlay(void);
/** Each frame (local): the test hook. */
void world_tools_frame(void);
/** "!tools" in the chat line opens or shuts the panel. */
TbBool world_tools_command(const char *text);

#ifdef __cplusplus
}
#endif
#endif

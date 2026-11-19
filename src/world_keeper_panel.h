/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keeper_panel.h
 *     Endless worlds: the Keeper panel (Profile, Upgrades, Social), opened by clicking the Soul Container.
 * @par Purpose:
 *     The user's mockup, 2026-11-19: three tabs (Profile, Upgrades, Social). Profile: the keeper's name, a portrait,
 *     its level with the XP bar, its statistics (Time Played, Keepers Defeated, Hearts Lost, Heroes Killed, Creatures
 *     Killed, Longest Distance Dug), Migrate Dungeon, Reset Dungeon and its rank among the keepers of the world.
 *     Upgrades and Social are for later.
 */
/******************************************************************************/
#ifndef DK_WORLD_KEEPER_PANEL_H
#define DK_WORLD_KEEPER_PANEL_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
extern struct GuiMenu world_keeper_panel_menu;

/** Is the panel on the screen now? */
TbBool world_keeper_panel_open(void);
void world_keeper_panel_show(void);
void world_keeper_panel_close(void);
/** The panel over everything else on the screen but the Hand (the user, 2026-11-19: "The panel needs to be drawn
 *  above everything"): the menus' own drawing leaves it out, the frame's drawing ends with it. */
void world_keeper_panel_draw_last(void);
/** First among the dungeon view's inputs: placing the Soul Container (world_migrate.c), and a click on this keeper's
 *  own Soul Container (the CLICKABLE object under the Hand) opens the panel. True when it took the click. */
TbBool world_keeper_panel_inputs(void);
/** Game state (the simulating machine): the panel's packet (PckA_WorldKeeperPanel). */
void world_keeper_panel_packet(PlayerNumber plyr_idx, unsigned long par1, unsigned long par2);
/** Every turn, on the machine that shows the game (test hook KFX_WORLD_PANELTEST). */
void world_keeper_panel_turn(void);

#ifdef __cplusplus
}
#endif
#endif

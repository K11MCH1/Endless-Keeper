/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_reaper.h
 *     Endless worlds: the Heart of the Pentagram's Reaper (2026-11-27).
 * @par Purpose:
 *     The user: once a day a keeper with the Heart unlocked can summon a level 100 Horned Reaper for 20 seconds (as
 *     in Dungeon Keeper 2). He can't be picked up, makes no lair, never eats or takes pay, and seeks out every enemy
 *     nearby; slapped, he grows very angry and kills anything in his path, the keeper's own creatures too. He rises
 *     out of the floor slowly turning (as creatures dropped in the Temple), with Word of Power blasts (no harm, a
 *     4-slab knock-back) and lava fireballs (no harm) round him, and sinks back the same way. A Molten icon at the top
 *     right of the game screen shows Ready, his 20 seconds, or the time to the next dawn (greyed).
 */
/******************************************************************************/
#ifndef DK_WORLD_REAPER_H
#define DK_WORLD_REAPER_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** Game state (packets.c): the keeper summons the Reaper at a subtile of its own claimed ground. */
void world_reaper_summon_packet(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Each world turn (the simulating machine): the Reaper's rising, blasts, hunting, sinking. */
void world_reaper_turn(void);
/** Is this creature a summoned Reaper (no Hand, lair, food, pay, prison or fleeing)? */
TbBool world_reaper_is(const struct Thing *thing);
/** Is this effect one of the Reaper's (it harms nothing and changes no slab)? */
TbBool world_reaper_effect(const struct Thing *efftng);
/** The keeper slapped its Reaper: he grows angry. */
void world_reaper_slapped(struct Thing *thing);
/** May the keeper summon now (the Heart unlocked, today's summon unused, not at a World Boss)? */
TbBool world_reaper_ready(PlayerNumber plyr_idx);
/** This machine: the HUD icon (top right): drawn, and clicked (true when it took the click). */
void world_reaper_hud_draw(void);
TbBool world_reaper_hud_click(int mx, int my);
/** This machine: the icon was clicked, the next click on the map summons him (false: not choosing a place). */
TbBool world_reaper_placing(void);
/** This machine: a click on the map while placing (true: taken). */
TbBool world_reaper_place_click(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** front_input.c (the dungeon view): the placing (left click summons, right click calls it off); true when taken. */
TbBool world_reaper_inputs(void);
/** front_input.c (the GUI's own pass): the pointer is over the Reaper's icon or the World Boss strip - the HUD's, not
 *  the dungeon's (no click goes through to the ground); the icon's click is taken here. */
TbBool world_reaper_hud_mouse(void);
/** engine_render.c: the placing's box on the slab under the pointer (green where he can be summoned, red elsewhere). */
void world_reaper_cursor_box(void);
/** power_hand.c: while placing, the Reaper's icon is the hand (true: drawn). */
TbBool world_reaper_draw_hand(long cursor_x, long cursor_y);
/** custom_sprites.c: the pointer's icon. */
void world_reaper_register_icons(void);

#ifdef __cplusplus
}
#endif
#endif

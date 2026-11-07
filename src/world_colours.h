/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_colours.h
 *     Endless world: the colours each machine shows its player.
 * @par Purpose:
 *     Presentation only, never synced. The local player always sees themselves as red. Every other keeper, human or
 *     computer, takes a colour in the order the local player meets them: blue, green, yellow, purple, black, orange,
 *     then blue again for the 7th, and so on. Heroes stay white and neutral stays neutral. A keeper is met when any
 *     of its slabs or creatures is first revealed to the local player. Outside endless levels every function gives
 *     the stock colours.
 */
/******************************************************************************/
#ifndef DK_WORLD_COLOURS_H
#define DK_WORLD_COLOURS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called each game turn in world levels: looks for keepers the local player has just met (local state only). */
void world_colours_turn(void);
/** Forgets who was met (a new level, a loaded save). */
void world_colours_reset(void);
/** The colour index this machine shows for a player (get_player_color_idx outside endless levels). */
unsigned char world_display_colour(PlayerNumber plyr_idx);
TbBool world_player_met(PlayerNumber plyr_idx);
TbBool world_player_listed(PlayerNumber plyr_idx);
/** A true colour index (dungeon->color_idx) to the colour this machine shows for its owner. */
unsigned char world_display_colour_of_colour(unsigned char colour_idx);
/** A cube as this machine shows it: an owned cube (wall drapes, claimed floor marks, the heart's top...) in the
 *  display colour of its owner. Other cubes are unchanged. */
unsigned short world_display_cube(unsigned short cube_id);
struct Thing;
/** The sprite this machine draws a thing with: a coloured object (guard flag, heart flame) in its owner's display
 *  colour. Other things keep their sprite. */
unsigned short world_display_thing_sprite(const struct Thing *thing);
/** The cube's look as this machine shows it (draw code only; building columns keeps the true cube). */
#define get_display_cube_stats(cube) get_cube_model_stats(world_display_cube(cube))

#ifdef __cplusplus
}
#endif
#endif

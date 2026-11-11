/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_circles.h
 *     Endless world: the nine circles round every dungeon (2026-11-11).
 * @par Purpose:
 *     The land round every dungeon start is cut into nine circles, the n-th n * 250 slabs wide, measured from the NEAREST
 *     start. What a keeper travels to (heroes of outposts and fortresses, wild creatures, rival keepers' creatures)
 *     is as strong as its circle says; a keeper whose imps dig through a circle's line is told so, once.
 */
/******************************************************************************/
#ifndef DK_WORLD_CIRCLES_H
#define DK_WORLD_CIRCLES_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC_CIRCLES      9
#define WC_CIRCLE_SLABS 250 /* the first circle's width; the n-th is n times as wide */

/** The circle (1..9) of a distance in slabs from the nearest start. */
int world_circle_of_distance(double distance);
/** The circle (1..9) a world slab lies in. */
int world_circle_at_world(int64_t wx, int64_t wy);
/** The circle (1..9) an engine slab lies in. */
int world_circle_at_engine_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);

/** The level (1-based) of an enemy standing on a world slab: circles 1-4 a share (50, 75, 100, 125%) of the nearest
 *  keeper's best creature level and never below floor_level; circles 5-9 fixed (30, 45, 60, 80, 100). Outside an
 *  endless world: floor_level. circle_out (may be NULL) gets the circle. */
int world_circles_level_at_world(int64_t wx, int64_t wy, int floor_level, int *circle_out);
/** The same for an engine slab, logged ("what": who is levelled; NULL: no log line). */
int world_circles_level_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y, int floor_level, const char *what);
/** The level the generator gives heroes at a place today (by the distance to the nearest start): the floor of the
 *  heroes outposts and fortresses send. */
int world_circles_hero_floor_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** The strength of the first circle round a keeper's start (the level existing enemies near a new start come down
 *  to). */
int world_circles_welcome_level(PlayerNumber plyr_idx);

/** A keeper's imp dug a slab out: past the line of a circle it hasn't breached, the keeper is told. */
void world_circles_dug(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** The highest circle a keeper has breached (1..9). */
int world_circles_breached(PlayerNumber plyr_idx);
/** A keeper gets a new start: it has breached the first circle only. */
void world_circles_player_reset(PlayerNumber plyr_idx);
/** Once per game turn (from world_process_turn). */
void world_circles_turn(void);
/** The banner, drawn over the game view. */
void world_circles_draw(void);
/** A thin client: a server notice that is a circle message (not shown as chat). */
TbBool world_circles_net_notice(const char *text);
/** The full-screen map: a circle's name when the keeper looking has breached it, else NULL. */
const char *world_circles_map_name(int circle);

/** Which circles each keeper has breached: kept in the world save (the CIR1 extra). */
void world_circles_reset(void);
size_t world_circles_save_size(void);
void world_circles_save(uint8_t *buf);
TbBool world_circles_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

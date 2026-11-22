/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wild.h
 *     Endless world: wild creatures, spawned and despawned the way Minecraft spawns monsters.
 */
/******************************************************************************/
#ifndef DK_WORLD_WILD_H
#define DK_WORLD_WILD_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** Once per game turn in endless levels (from world_process_turn): spawning and despawning. */
void world_wild_turn(void);

/** Is this a wild creature? (Those aren't frozen with their chunk: they despawn when it leaves the window.) */
TbBool world_wild_creature(const struct Thing *thing);
/** A wild creature that came before the dawn of the day now going (by night: before the last dawn). The ones that
 *  came by day don't fade at dawn and don't leave their walls (2026-11-15: packs come by day too). */
TbBool world_wild_from_last_night(const struct Thing *thing);
/** The burrows and tunnels wild packs dug, to fall in again at dawn: kept in the world save (the WBT1 extra). */
/** A slab dug open as part of a wild tunnel (a wall a Beetle broke through, world_wallbreak.c): it falls in with the
 *  tunnel after dawn. */
/** A raid Tunneller dug this subtile's slab out: it falls in again like a wild tunnel (by day, nobody's, empty, no raider near). */
void world_wild_raid_dug(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
void world_wild_tunnel_slab(MapSlabCoord x, MapSlabCoord y);
void world_wild_reset(void);
size_t world_wild_save_size(void);
void world_wild_save(uint8_t *buf);
TbBool world_wild_load(const uint8_t *buf, size_t len);
/** The level (1-based) of a keeper's strongest creature; 1 for none. */
int world_keeper_top_level(PlayerNumber plyr_idx);

#ifdef __cplusplus
}
#endif
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_temperament.h
 *     Endless world: world temperaments (three traits and one wonder dealt to a new world).
 */
/******************************************************************************/
#ifndef DK_WORLD_TEMPERAMENT_H
#define DK_WORLD_TEMPERAMENT_H

#include "bflib_basics.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void world_temper_reset(void);
void world_temper_new_world(int64_t home_x, int64_t home_y);
void world_temper_first_turn(void);
void world_temper_turn(void);
TbBool world_temper_text(char *buf, size_t len);
/** The outposts' chance (percent of regions with one) with the temperament. */
int world_temper_outpost_chance(int base);
/** A wild pack cap with the temperament. */
int world_temper_wild_cap(int cap);
/** -1 poor in gold, +1 rich in gold, 0 neither. */
int world_temper_gold(void);
/** The wonder's middle (world slab), if the world has one. */
TbBool world_temper_wonder(int64_t *wx, int64_t *wy);
int world_temper_nights_wire(void);
void world_temper_client_nights(int pct);
size_t world_temper_save_size(void);
void world_temper_save(uint8_t *buf);
TbBool world_temper_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

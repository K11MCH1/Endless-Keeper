/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_risen.h
 *     Endless world: Vampires, Skeletons and Ghosts rise within limits (world_risen.c).
 */
/******************************************************************************/
#ifndef WORLD_RISEN_H
#define WORLD_RISEN_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

enum WorldRisenKind {
    WRis_Vampire = 0, /* from corpses rotted in the Graveyard */
    WRis_Skeleton,    /* from prisoners starved to death */
    WRis_Ghost,       /* from creatures dead under torture */
    WRis_Count
};

/** An endless world's event of that kind for the keeper (a corpse rotted, a prisoner or tortured creature died): kept for
 *  a world day and its night. True when one of that kind (model) rises now: the keeper has fewer than 4 and the events
 *  kept reach (the ones it has + 1) x base_needed, which are used up. */
TbBool world_risen_counts(PlayerNumber plyr_idx, int kind, int base_needed, ThingModel model);

/** Test switch KFX_WORLD_RISENTEST (world_risen.c); called every world turn. */
void world_risen_test_turn(void);
void world_risen_reset(void);
size_t world_risen_save_size(void);
void world_risen_save(uint8_t *buf);
TbBool world_risen_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif

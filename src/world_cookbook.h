/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_cookbook.h
 *     Endless world: the Unholy Cookbook, a world's hidden Temple recipes.
 * @par Purpose:
 *     Each world hides twelve recipes of three creature kinds, drawn from the world's seed (so they differ from world to
 *     world and are never written down in the code). Three creatures sacrificed at a keeper's Temple within a day make
 *     a mix: a recipe's mix gives its result, a wrong one does nothing or, one time in three, bites (one of the keeper's
 *     creatures is hurt). The creatures sacrificed are the price of guessing. A keeper's cookbook page ("!cookbook" in
 *     the chat line) lists the recipes it has made. The known recipes are in struct Game (game.world_cookbook_known) so
 *     a joined player's page shows them; saved as the XTR2 record "CKB1". The recipes and their kinds stay out of every
 *     log line.
 */
/******************************************************************************/
#ifndef DK_WORLD_COOKBOOK_H
#define DK_WORLD_COOKBOOK_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Coord3d;

#define COOKBOOK_RECIPES 12

/** A creature of this kind was sacrificed at a Temple of this keeper. */
void world_cookbook_sacrificed(PlayerNumber plyr_idx, ThingModel model, const struct Coord3d *pos);
/** "!cookbook" (or /cookbook): this machine's keeper's page, in its chat box or messages. */
TbBool world_cookbook_command(const char *text);
/** Each world turn (server or single player): the test hook. */
void world_cookbook_turn(void);

void world_cookbook_reset(void);
size_t world_cookbook_save_size(void);
void world_cookbook_save(uint8_t *buf);
TbBool world_cookbook_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): an empty cookbook page. */
void world_cookbook_seat_reset(PlayerNumber p);

/** A digest of this world's recipes (tests: two machines compared without a recipe written down). */
uint32_t world_cookbook_digest(void);

struct Coord3d;
/** A heroes' stronghold fell to keeper `by` at pos: in the Ancient Ruins it leaves a lore page (one ingredient). */
void world_cookbook_stronghold_destroyed(PlayerNumber by, const struct Coord3d *pos);

#ifdef __cplusplus
}
#endif
#endif

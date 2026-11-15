/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_champion.h
 *     Endless world: the keeper's Champion.
 * @par Purpose:
 *     A keeper makes one creature its Champion at the Heart ("!champion" in the chat line with the cursor on one of
 *     its creatures within 3 slabs of its Heart; the title passes at most once in 7 days). "!possess" puts the keeper
 *     inside its Champion wherever it is. The Champion fights at its level plus its keeper's player level, the added
 *     levels capped at the dungeon's age in days; they are not its real level, so they never count as the keeper's
 *     best creature level (raids, circles). A fallen Champion comes back to the Heart a day later, one level lower,
 *     with its kind and name. Its name label says "(Champion)". Saved as the XTR2 record "CHM1".
 */
/******************************************************************************/
#ifndef DK_WORLD_CHAMPION_H
#define DK_WORLD_CHAMPION_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define CHAMPION_HEART_SLABS 3
#define CHAMPION_CHANGE_DAYS 7

/** "!champion" and "!possess" (also with '/'): true when it was one of them. */
TbBool world_champion_command(const char *text, MapCoord cursor_x, MapCoord cursor_y);
/** The dungeon view's inputs: J possesses the Champion. True when taken. */
TbBool world_champion_inputs(void);
/** The packet (simulating machine): what 1 = make the creature at subtile (x, y) Champion, 2 = possess it. */
void world_champion_packet(PlayerNumber plyr_idx, int what, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** The level a creature fights at (its own, plus its keeper's levels if it is the Champion). */
CrtrExpLevel world_champion_level(const struct Thing *creatng, CrtrExpLevel exp_level);
/** Is this creature a Champion? */
TbBool world_champion_is(const struct Thing *creatng);
/** For its name label: " (Champion)" or "". */
const char *world_champion_label(const struct Thing *creatng);
/** A creature is about to die: a Champion is pulled back to the Heart a day later. */
void world_champion_falls(struct Thing *creatng);
/** Each world turn (server or single player): Champions coming back; the test hook. */
void world_champion_turn(void);

void world_champion_reset(void);
size_t world_champion_save_size(void);
void world_champion_save(uint8_t *buf);
TbBool world_champion_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): no Champion, no title waiting, no fallen one coming back. */
void world_champion_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

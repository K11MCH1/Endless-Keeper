/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_attune.h
 *     Endless world: Attunement, the keeper's skill tree of powers.
 * @par Purpose:
 *     Each player level after the first gives one point (nine at level 10). Three branches of four ranks:
 *     - Keeping an army alive: Heal, Protect Monster, Speed Monster;
 *     - Fighting at home: Freeze, Slow, Lightning Strike;
 *     - Leading: Possession, Must Obey, Call to Arms.
 *     Each rank makes its branch's powers 12% cheaper (48% at rank 4). Twelve ranks, nine points: a keeper chooses.
 *     The window ("!attune" in the chat line) shows the branches and spends points (a packet). The ranks are in struct
 *     Game for joined players; "Start again" clears them. Saved as the XTR2 record "ATN1".
 */
/******************************************************************************/
#ifndef DK_WORLD_ATTUNE_H
#define DK_WORLD_ATTUNE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuiMenu;

#define ATTUNE_BRANCHES 3
#define ATTUNE_RANKS 4
#define ATTUNE_PCT_PER_RANK 12

/** A power's price for a keeper after its Attunement. */
GoldAmount world_attune_price(PlayerNumber plyr_idx, PowerKind pwkind, GoldAmount price);
/** Points a keeper has to spend. */
int world_attune_points_left(PlayerNumber plyr_idx);
/** The packet (simulating machine): one more rank in a branch. */
void world_attune_spend(PlayerNumber plyr_idx, int branch);
/** "!attune" (or /attune): opens or closes the window. */
TbBool world_attune_command(const char *text);
extern struct GuiMenu world_attune_menu;
/** "Start again": the seat's ranks from nothing. */
void world_attune_seat_reset(PlayerNumber plyr_idx);
/** Each world turn (server or single player): the test hook. */
void world_attune_turn(void);

void world_attune_reset(void);
size_t world_attune_save_size(void);
void world_attune_save(uint8_t *buf);
TbBool world_attune_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

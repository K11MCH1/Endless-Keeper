/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_unions.h
 *     Endless world: unholy unions, two mastered amenities joined at the Temple.
 * @par Purpose:
 *     When both amenities of a union are at their top mastery and the keeper has a Temple, "!union <room>" in the chat
 *     line joins them (a packet); each room takes one union. The first four:
 *     - Library and Sight of Evil: the Scrying Room, the ground round each Library always shown (10 slabs);
 *     - Prison and the Lightning Trap: the Chair, each captive talks: 150 gold a day each;
 *     - Hatchery and the Poison Gas Trap: the poisoned Hatchery, an enemy creature on it is hurt (3% of its health
 *       every 32 turns);
 *     - Lair and Heal: the Infirmary, sleep heals 50% faster.
 *     "!union" lists them. Saved as the XTR2 record "UNI1".
 */
/******************************************************************************/
#ifndef DK_WORLD_UNIONS_H
#define DK_WORLD_UNIONS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

enum WorldUnion { WUn_Scrying = 0, WUn_Chair, WUn_PoisonHatchery, WUn_Infirmary, WUn_COUNT };

TbBool world_union_has(PlayerNumber plyr_idx, int u);
/** The Infirmary's sleep healing, in percent more. */
int world_union_heal_pct(PlayerNumber plyr_idx);
/** "!union ..." (also with '/'). */
TbBool world_union_command(const char *text);
/** The packet (simulating machine): u + 1 to join (0: list). */
void world_union_packet(PlayerNumber plyr_idx, int what);
/** Each world turn (server or single player): the unions at work; the test hook. */
void world_unions_turn(void);

void world_unions_reset(void);
size_t world_unions_save_size(void);
void world_unions_save(uint8_t *buf);
TbBool world_unions_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): no unions (the mastery behind them starts again too). */
void world_unions_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

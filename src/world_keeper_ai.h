/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keeper_ai.h
 *     Endless worlds: the enemy keepers' brain over KeeperFX's computer player (world_keeper_ai.c).
 */
/******************************************************************************/
#ifndef DK_WORLD_KEEPER_AI_H
#define DK_WORLD_KEEPER_AI_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Each turn of an endless world (the simulation): awake computer keepers think now and then. */
void world_keeper_ai_turn(void);
/** KeeperFX's attack plans ask before they march: true when this keeper's brain allows an assault on the victim now
 *  (always true outside endless worlds and for human keepers' assistants). */
TbBool world_keeper_ai_may_attack(PlayerNumber plyr_idx, PlayerNumber victim);
/** A keeper wakes after being left alone so many world days: what it did meanwhile. */
void world_keeper_ai_woke(PlayerNumber plyr_idx, long days_alone);
/** Its personality's name ("the Warlord"), "" before it first thinks. */
const char *world_keeper_ai_personality_name(PlayerNumber plyr_idx);
/** The gold the endgame-size room a rival pair's keeper wants still costs (0: none wanted; world_rivals.c tops it up). */
long world_keeper_ai_room_gold(PlayerNumber plyr_idx);

void world_keeper_ai_reset(void);
size_t world_keeper_ai_save_size(void);
void world_keeper_ai_save(uint8_t *buf);
TbBool world_keeper_ai_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

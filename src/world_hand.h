/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_hand.h
 *     Endless world: the Hand of Evil grows with the keeper's player level.
 * @par Purpose:
 *     A bigger grip (the Hand holds one more creature per player level, from nine at level 2 up to sixteen) and a
 *     wider slap (from level 4, a slap also hurries the keeper's own creatures standing beside the slapped one,
 *     without hurting or angering them). Both follow the player level alone, so they need no saved state and
 *     every machine (a joined thin client too, which gets the levels in the world stream) agrees on them.
 *     Outside endless worlds nothing changes.
 */
/******************************************************************************/
#ifndef DK_WORLD_HAND_H
#define DK_WORLD_HAND_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct PlayerInfo;

#define WORLD_HAND_GRIP_MAX 16        /* the most the Hand can hold */
#define WORLD_HAND_SLAP_LEVEL 4       /* the player level from which a slap hurries the creatures beside it */
#define WORLD_HAND_SLAP_REACH 2       /* subtiles around the slapped creature */
#define WORLD_HAND_SLAP_MAX 8         /* creatures hurried by one slap at most */
#define WORLD_HAND_HURRY_TURNS 100    /* how long a bystander is hurried (5 seconds) */
#define WORLD_HAND_HURRY_PERCENT 150  /* its walking speed meanwhile */

/** How many creatures a keeper's Hand can hold: `stock` (the rules' MaxThingsInHand) outside endless worlds. */
int world_hand_capacity(PlayerNumber plyr_idx, int stock);
/** Whether a keeper's slap also hurries the creatures beside the slapped one. */
TbBool world_hand_wide_slap(PlayerNumber plyr_idx);
/** A keeper slapped one of its creatures: hurry the ones beside it (endless worlds, from WORLD_HAND_SLAP_LEVEL). */
void world_hand_slapped(struct PlayerInfo *player, struct Thing *slapped);
/** The player level at which the Hand holds `grip` creatures (0 when it never does). For messages and pages. */
int world_hand_level_for_grip(int grip);
/** This machine's keeper went from player level `before` to `after`: what the Hand gained, as messages. */
void world_hand_level_news(int before, int after);
/** Each world turn (server or single player): the KFX_WORLD_HANDTEST test hook. */
void world_hand_test_turn(void);
/** A creature's walking speed (get_creature_speed): faster while hurried by a slap beside it. */
long world_hand_speed(const struct Thing *thing, long speed);
/** A world starts or ends (a save loaded too): no one is hurried. */
void world_hand_reset(void);

#ifdef __cplusplus
}
#endif
#endif

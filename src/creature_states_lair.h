/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file creature_states_lair.h
 *     Header file for creature_states_lair.c.
 * @par Purpose:
 *     Creature state machine functions for their job in various rooms.
 * @par Comment:
 *     Just a header file - #defines, typedefs, function prototypes etc.
 * @author   KeeperFX Team
 * @date     23 Sep 2009 - 05 Jan 2011
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef DK_CRTRSTATELAIR_H
#define DK_CRTRSTATELAIR_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
#pragma pack(1)

struct Thing;
struct Room;

#pragma pack()
/******************************************************************************/
TbBool creature_can_do_healing_sleep(const struct Thing *creatng);
TbBool creature_is_doing_lair_activity(const struct Thing *thing);
TbBool creature_is_sleeping(const struct Thing *thing);
TbBool creature_is_doing_toking(const struct Thing *thing);
TbBool creature_requires_healing(const struct Thing *thing);
TbBool creature_place_lair_totem(struct Thing *creatng, struct Room *room, const struct Coord3d *pos);

CrStateRet creature_at_changed_lair(struct Thing *thing);
CrStateRet creature_at_new_lair(struct Thing *thing);
short creature_change_lair(struct Thing *thing);
short creature_choose_room_for_lair_site(struct Thing *thing);
short creature_drop_unconscious_in_lair(struct Thing *thing);
short at_lair_to_sleep(struct Thing *thing);
short cleanup_sleep(struct Thing *thing);
TbBool setup_head_for_random_unused_lair_subtile(struct Thing *creatng, struct Room *room);
TbBool creature_move_to_home_lair(struct Thing *creatng);
short creature_going_home_to_sleep(struct Thing *thing);
short creature_sleep(struct Thing *thing);
long process_lair_enemy(struct Thing *thing, struct Room *room);

/* Lair animations (creature property LAIR_ANIMATIONS). */
#define LAIR_ANIM_SLEEP_SPEED  96  /* animation speed of going to sleep; 256 = one frame a turn */
#define LAIR_ANIM_WAKE_SPEED  128  /* animation speed of coming out; doubled with an enemy near */
#define LAIR_ANIM_BREATHE_SPEED 48 /* animation speed while asleep (the stock speed is 128) */
enum LairAnimationPhases { LairAnim_None = 0, LairAnim_GoingToSleep, LairAnim_WakingUp, };
TbBool creature_sleeps_with_lair_animations(const struct Thing *thing);
int creature_lair_animation_phase(const struct Thing *thing);
TbBool creature_lair_wake_is_hurried(const struct Thing *thing);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

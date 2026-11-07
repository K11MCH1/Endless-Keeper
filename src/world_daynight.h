/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_daynight.h
 *     Endless world: day and night, the day/night icon, and the heroes who break in by day.
 */
/******************************************************************************/
#ifndef DK_WORLD_DAYNIGHT_H
#define DK_WORLD_DAYNIGHT_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** Day number (1 = the first day) and whether it is day now; both follow from the game turn. */
long world_day_number(void);
TbBool world_is_day(void);

/** Once per game turn in endless levels (from world_process_turn): day/night changes and the day's raids. */
void world_daynight_turn(void);
/** Between turns (world_after_turn): the dawn autosave the last turn asked for. */
void world_daynight_after_turn(void);
/** Clears what must not carry over into the next run. */
void world_daynight_level_end(void);

/** Is this one of the heroes of a day raid? (They show on the maps like tunnelling hero parties.) */
TbBool world_raid_hero(const struct Thing *thing);

/** Draws the day/night icon in the top right corner (endless levels only; from the in-game UI). */
void world_daynight_draw(void);

#ifdef __cplusplus
}
#endif
#endif

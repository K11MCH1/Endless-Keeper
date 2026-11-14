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
/** The days a keeper's dungeon has lived (1 = its first day): the world's clock, counted from the day it began. */
long world_seat_day(PlayerNumber plyr_idx);
/** A keeper's dungeon (re)starts: its days count from today. */
void world_daynight_seat_started(PlayerNumber plyr_idx);
void world_seatdays_reset(void);
size_t world_seatdays_save_size(void);
void world_seatdays_save(uint8_t *buf);
TbBool world_seatdays_load(const uint8_t *buf, size_t len);
TbBool world_is_day(void);
/** A day and its night, in game turns. */
long world_day_turns(void);

/** Once per game turn in endless levels (from world_process_turn): day/night changes and the day's raids. */
void world_daynight_turn(void);
/** Between turns (world_after_turn): the dawn autosave the last turn asked for. */
void world_daynight_after_turn(void);
/** Clears what must not carry over into the next run. */
void world_daynight_level_end(void);

/** Is this one of the heroes of a day raid? (They show on the maps like tunnelling hero parties.) */
TbBool world_raid_hero(const struct Thing *thing);
/** Heroes are raiding this keeper now. */
TbBool world_raid_on(PlayerNumber plyr_idx);
/** A raid hero whose raid is over (after its third night) walks back to its landing and leaves; true when this one
 *  was seen to (the hero AI then leaves it alone). */
TbBool world_raid_hero_goes_home(struct Thing *thing);
/** The RDX1 extra of the world save: the day's owed first raid and the fleeing parties' clock. */
void world_raids_extra_reset(void);
size_t world_raids_extra_save_size(void);
void world_raids_extra_save(uint8_t *buf);
TbBool world_raids_extra_load(const uint8_t *buf, size_t len);
/** A creature died: when it led a raiding party, the rest of the party goes home at once (2026-11-11). */
void world_raid_creature_died(struct Thing *thing);
/** The raids going on (saved with the world, RDS1). */
size_t world_raids_save_size(void);
void world_raids_save(uint8_t *buf);
TbBool world_raids_load(const uint8_t *buf, size_t len);
void world_raids_reset(void);
/** Start again: the raid on that player (if any) goes home now. */
void world_raids_player_retreat(PlayerNumber plyr_idx);

/** Draws the day/night icon in the top right corner (endless levels only; from the in-game UI). */
void world_daynight_draw(void);

/** The chunk manager: the chunks the day's raids keep loaded (retreating heroes' landings), as world slabs. */
void world_daynight_want_chunks(void (*want)(int64_t wx, int64_t wy, int r));

#ifdef __cplusplus
}
#endif
#endif

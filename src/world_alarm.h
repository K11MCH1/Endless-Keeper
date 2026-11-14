/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_alarm.h
 *     Endless world: Alarm Bells (2026-11-13).
 * @par Purpose:
 *     Every hero fortress has an alarm bell. Gold a keeper's imps mine within its reach fills that keeper's alarm for
 *     that fortress (each keeper has their own). The alarm grows in three stages: a warning (the heroes' white flag
 *     and horn on the seam, and a few heroes come for it), a raiding party sent for the keeper's gold, and a column
 *     that marches on the heart. Only razing the fortress (a keeper claiming its middle) silences its bell, for good.
 */
/******************************************************************************/
#ifndef DK_WORLD_ALARM_H
#define DK_WORLD_ALARM_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;

#ifdef __cplusplus
extern "C" {
#endif

/** A keeper's imp got gold out of the slab at (stl_x, stl_y). */
void world_alarm_gold_mined(const struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y, long gold);
/** Each turn of an endless world (the simulation): stages that are due, flags that have flown long enough, fortresses
 *  razed. */
void world_alarm_turn(void);
/** A hero an alarm sent (its own mark): it keeps the chunks round it loaded, as raid heroes do. */
TbBool world_alarm_hero(const struct Thing *thing);
/** A keeper's dungeon (re)starts: their alarms go quiet. */
void world_alarm_seat_started(PlayerNumber plyr_idx);

void world_alarm_reset(void);
size_t world_alarm_save_size(void);
void world_alarm_save(uint8_t *buf);
TbBool world_alarm_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

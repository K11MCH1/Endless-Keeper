/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_seasons.h
 *     Endless world: seasons below.
 * @par Purpose:
 *     The world has a year of 28 days, four seasons of 7, the same for everyone (by the world's day number):
 *     - The Thaw: Hatcheries hatch a quarter sooner; wild packs by the water come as bigger swarms of Flies.
 *     - The Swelter: the heroes send one more raid each day; fire does a quarter more harm.
 *     - The Rot: Hatcheries hatch half as often; a body laid in a Graveyard counts as two (it rises in half the time).
 *     - The Long Night: day and night swap lengths; wild packs are one bigger; portals bring creatures more slowly.
 *     A dungeon in its first 7 days feels only the kinder half of each season (its Thaw, none of the rest that can be
 *     told apart by keeper). The year is drawn as a ring of 28 marks around the sun and moon dial. Nothing is saved:
 *     the season follows from the world's day.
 */
/******************************************************************************/
#ifndef DK_WORLD_SEASONS_H
#define DK_WORLD_SEASONS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

enum WorldSeason { WSe_Thaw = 0, WSe_Swelter, WSe_Rot, WSe_LongNight, WSe_COUNT };
#define SEASON_DAYS 7
#define SEASON_YEAR_DAYS (SEASON_DAYS * WSe_COUNT)
#define SEASON_SPARED_DAYS 7   /* a new dungeon's days with only the kinder half */

/** The season of a world day (1 = the first). */
int world_season_of_day(long day);
/** The season now. */
int world_season(void);
const char *world_season_name(int season);
/** Whether a keeper feels the harsh half of the seasons (not in its first 7 days; any non-keeper does). */
TbBool world_season_harsh_for(PlayerNumber plyr_idx);
/** The Long Night on that world day? (day and night swap lengths) */
TbBool world_season_long_night(long day);
/** A Hatchery's hatching, in percent more (+25 in the Thaw, -50 in the Rot), for its keeper. */
int world_season_hatch_pct(PlayerNumber plyr_idx);
/** The extra raids a keeper gets today (1 in the Swelter). */
int world_season_extra_raids(PlayerNumber plyr_idx);
/** Fire's harm to a creature, in percent more (+25 in the Swelter). */
int world_season_fire_pct(const struct Thing *victim);
/** How much sooner a Graveyard body rises (percent of its time: 50 in the Rot, else 100). */
int world_season_grave_pct(PlayerNumber plyr_idx);
/** Wild packs: the extra creatures of a pack (Long Night +1; by water in the Thaw +2), and whether it is Flies. */
int world_season_pack_extra(TbBool by_water, TbBool *flies);
/** Portals: percent longer between creatures (50 in the Long Night) for a keeper. */
int world_season_portal_pct(PlayerNumber plyr_idx);
/** Each machine, each frame where the dial is drawn: the year's ring. */
void world_seasons_draw_ring(int x, int y, int size);
/** Each world turn: the season's change is told; the test hook. */
void world_seasons_turn(void);

#ifdef __cplusplus
}
#endif
#endif

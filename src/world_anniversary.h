/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_anniversary.h
 *     Endless world: anniversaries of the Heart.
 * @par Purpose:
 *     Each dungeon counts its own days (world_seat_day). Days 7, 30, 100 and 365 are marked with a gift (special
 *     boxes beside the Heart), one more flame on the Heart, and a line in the chat for everyone. A dungeon that
 *     starts again (its Heart lost) starts its age, its marks and its extra flames from nothing.
 *     Saved as the XTR2 record "ANV1".
 */
/******************************************************************************/
#ifndef DK_WORLD_ANNIVERSARY_H
#define DK_WORLD_ANNIVERSARY_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WORLD_ANNIVERSARIES 4

/** The day of anniversary i (0-3). */
long world_anniversary_day(int i);
/** How many anniversaries a keeper's dungeon has marked. */
int world_anniversaries_marked(PlayerNumber plyr_idx);
/** A keeper's dungeon (re)starts: no marks, and the flames it was given go out. */
void world_anniversary_seat_started(PlayerNumber plyr_idx);
/** Each world turn (server or single player). */
void world_anniversary_turn(void);

void world_anniversary_reset(void);
size_t world_anniversary_save_size(void);
void world_anniversary_save(uint8_t *buf);
TbBool world_anniversary_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif

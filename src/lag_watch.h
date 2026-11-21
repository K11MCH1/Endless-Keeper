/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file lag_watch.h
 *     Header file for lag_watch.c.
 * @par Purpose:
 *     Lag watch: slow turns, slow frames and a game running below speed, written into the log.
 */
/******************************************************************************/
#ifndef DK_LAG_WATCH_H
#define DK_LAG_WATCH_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A game turn was worked out in ns nanoseconds; counted false while paused or fast-forwarding. */
void lag_watch_turn(int64_t ns, TbBool counted);
/** A frame was drawn: draw_ns building it, present_ns handing it to the graphics driver. */
void lag_watch_frame(int64_t draw_ns, int64_t present_ns);
/** The game waited on purpose (its window was not in front): this stretch of time is not judged. */
void lag_watch_idle(void);

#ifdef __cplusplus
}
#endif
#endif

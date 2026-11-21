/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file lag_watch.c
 *     Lag watch: slow turns, slow frames and a game running below speed, written into the log.
 * @par Purpose:
 *     Always on, in every game (the user, 2026-11-21, after a migration that "lagged more and crashed": "Lag check
 *     would be useful in general"). It costs two clock reads a turn and two a frame. Three kinds of line, "Lag: ...":
 *     - a turn that took more than LW_SLOW_TURN_MS to work out (the whole game stood still that long);
 *     - a frame that took more than LW_SLOW_FRAME_MS to draw and hand to the graphics driver;
 *     - every LW_WINDOW_SECONDS, when the game made fewer than LW_SPEED_SHARE of the turns it should have (or a slow
 *       turn or frame happened in that stretch): its speed with where the time went (turns, drawing, the driver).
 *     The first lag also switches on the detailed turn timer (KFX_WORLD_PROFILE's "World profile" lines every 200
 *     turns, and the creature states'), so the log after it shows which part of the game was slow. Lines are kept to
 *     one a second at most. A paused or fast-forwarded game, and time spent waiting for the window to come back to
 *     the front, are not judged.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "lag_watch.h"

#include <string.h>
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "game_legacy.h"
#include "world_stream.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int32_t turns_per_second;

#define LW_SLOW_TURN_MS 100
#define LW_SLOW_FRAME_MS 150
#define LW_WINDOW_SECONDS 5
#define LW_SPEED_SHARE 0.8
#define NS_PER_MS 1000000LL

static struct {
    int64_t start;          /* this stretch's start (0: none yet) */
    long turns, frames;
    int64_t turn_sum, turn_max, draw_sum, present_sum, frame_max;
    long slow;              /* slow turns and frames in this stretch */
    TbBool void_window;     /* paused, fast-forwarding or waiting for the window: not judged */
    int64_t last_line;
    long held_back;         /* lines not written (one a second) */
    TbBool profiled;
} lw;

static TbBool lw_may_write(int64_t now)
{
    if ((lw.last_line != 0) && (now - lw.last_line < 1000 * NS_PER_MS))
    {
        lw.held_back++;
        return false;
    }
    lw.last_line = now;
    return true;
}

static void lw_lagged(void)
{
    if (lw.profiled)
        return;
    lw.profiled = true;
    world_prof_force_on();
    SYNCLOG("Lag: the detailed turn timer is on from now (\"World profile\" lines every 200 turns show the slow part)");
}

static void lw_window(int64_t now)
{
    if (lw.start == 0)
    {
        lw.start = now;
        return;
    }
    const int64_t span = now - lw.start;
    if (span < LW_WINDOW_SECONDS * 1000 * NS_PER_MS)
        return;
    const double seconds = (double)span / 1e9;
    const double speed = (double)lw.turns / seconds;
    const TbBool slow_speed = (turns_per_second > 0) && (speed < LW_SPEED_SHARE * turns_per_second);
    if (!lw.void_window && (slow_speed || (lw.slow > 0)) && (lw.turns > 0))
    {
        lw_lagged();
        SYNCLOG("Lag: %.1f turns a second of %d over %.0f s (turn %lu): a turn %.1f ms on average (most %.0f), "
            "%ld frames %.1f ms each to draw and %.1f ms to hand over (most %.0f), %ld slow turns or frames%s",
            speed, (int)turns_per_second, seconds, (unsigned long)game.play_gameturn,
            (double)lw.turn_sum / lw.turns / NS_PER_MS, (double)lw.turn_max / NS_PER_MS, lw.frames,
            (lw.frames > 0) ? (double)lw.draw_sum / lw.frames / NS_PER_MS : 0.0,
            (lw.frames > 0) ? (double)lw.present_sum / lw.frames / NS_PER_MS : 0.0, (double)lw.frame_max / NS_PER_MS,
            lw.slow, (lw.held_back > 0) ? " (some lines held back)" : "");
        lw.held_back = 0;
    }
    const int64_t last_line = lw.last_line;
    const long held = lw.held_back;
    const TbBool profiled = lw.profiled;
    memset(&lw, 0, sizeof(lw));
    lw.start = now;
    lw.last_line = last_line;
    lw.held_back = held;
    lw.profiled = profiled;
}

void lag_watch_turn(int64_t ns, TbBool counted)
{
    const int64_t now = get_time_tick_ns();
    if (!counted || ((game.operation_flags & GOF_Paused) != 0) || (game.play_gameturn < 40))
        lw.void_window = true;
    else
    {
        lw.turns++;
        lw.turn_sum += ns;
        if (ns > lw.turn_max)
            lw.turn_max = ns;
        if (ns > LW_SLOW_TURN_MS * NS_PER_MS)
        {
            lw.slow++;
            lw_lagged();
            if (lw_may_write(now))
                SYNCLOG("Lag: turn %lu took %.0f ms to work out (the game stood still that long)",
                    (unsigned long)game.play_gameturn, (double)ns / NS_PER_MS);
        }
    }
    lw_window(now);
}

void lag_watch_frame(int64_t draw_ns, int64_t present_ns)
{
    const int64_t now = get_time_tick_ns();
    lw.frames++;
    lw.draw_sum += draw_ns;
    lw.present_sum += present_ns;
    const int64_t all = draw_ns + present_ns;
    if (all > lw.frame_max)
        lw.frame_max = all;
    if (!lw.void_window && (all > LW_SLOW_FRAME_MS * NS_PER_MS))
    {
        lw.slow++;
        lw_lagged();
        if (lw_may_write(now))
            SYNCLOG("Lag: a frame took %.0f ms (%.0f to draw, %.0f to hand to the graphics driver), turn %lu",
                (double)all / NS_PER_MS, (double)draw_ns / NS_PER_MS, (double)present_ns / NS_PER_MS,
                (unsigned long)game.play_gameturn);
    }
}

void lag_watch_idle(void)
{
    lw.void_window = true;
}

#ifdef __cplusplus
}
#endif

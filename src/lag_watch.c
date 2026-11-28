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
 *     When memory runs short (lag_watch_memory_short) a line says so, at most once a minute, lag or not; the crash
 *     report names it too (bflib_crash.c).
 *     Each lag line ends with the memory then (2026-11-23, GitHub issue 3: a Linux game slowed down 100 times all at
 *     once, as when memory runs out and the computer swaps): the game's own, and the computer's free and swap.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "lag_watch.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN /* (as bflib_crash.c: globals.h's NOMSG macro would leave out the message types ole2.h needs) */
#define PSAPI_VERSION 2 /* GetProcessMemoryInfo is kernel32's K32GetProcessMemoryInfo (Windows 7 and later): no psapi.dll */
#include <windows.h>
#include <psapi.h>
#endif
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
    int64_t low_memory_line; /* when memory running out was last said */
} lw;

#if !defined(_WIN32)
/** A "Key:   123 kB" value of a /proc file, in kB (-1: not there). */
static long long lw_proc_kb(const char *fname, const char *key)
{
    FILE *f = fopen(fname, "r");
    if (f == NULL)
        return -1;
    char line[256];
    long long kb = -1;
    const size_t n = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if ((strncmp(line, key, n) == 0) && (line[n] == ':'))
        {
            kb = atoll(line + n + 1);
            break;
        }
    }
    fclose(f);
    return kb;
}
#endif

/** The memory now, in kB (-1: not readable here). */
struct LwMem {
    long long game_kb, free_kb, total_kb, swap_kb, commit_kb, limit_kb, space_kb, space_total_kb;
};

static void lw_read(struct LwMem *m)
{
    m->game_kb = m->free_kb = m->total_kb = m->swap_kb = m->commit_kb = m->limit_kb = m->space_kb = m->space_total_kb = -1;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        m->game_kb = (long long)(pmc.WorkingSetSize / 1024);
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
    {
        m->free_kb = (long long)(ms.ullAvailPhys / 1024);
        m->total_kb = (long long)(ms.ullTotalPhys / 1024);
        if (ms.ullTotalPageFile >= ms.ullAvailPageFile)
        {
            m->commit_kb = (long long)((ms.ullTotalPageFile - ms.ullAvailPageFile) / 1024);
            m->limit_kb = (long long)(ms.ullTotalPageFile / 1024);
        }
        /* the game's own address space: 2 or 4 GB for the 32-bit game, which can fill it with RAM to spare */
        if (ms.ullTotalVirtual <= (8ULL << 30))
        {
            m->space_kb = (long long)(ms.ullAvailVirtual / 1024);
            m->space_total_kb = (long long)(ms.ullTotalVirtual / 1024);
        }
    }
#else
    m->game_kb = lw_proc_kb("/proc/self/status", "VmRSS");
    m->free_kb = lw_proc_kb("/proc/meminfo", "MemAvailable");
    m->total_kb = lw_proc_kb("/proc/meminfo", "MemTotal");
    const long long swap_total = lw_proc_kb("/proc/meminfo", "SwapTotal");
    const long long swap_free = lw_proc_kb("/proc/meminfo", "SwapFree");
    if ((swap_total >= 0) && (swap_free >= 0))
        m->swap_kb = swap_total - swap_free;
#endif
}

void lag_watch_memory_text(char *out, size_t len)
{
    struct LwMem m;
    lw_read(&m);
    int n = snprintf(out, len, "; memory:");
    if ((m.game_kb >= 0) && (n > 0) && ((size_t)n < len))
        n += snprintf(out + n, len - n, " the game %lld MB,", m.game_kb / 1024);
    if ((m.space_kb >= 0) && (m.space_total_kb > 0) && (n > 0) && ((size_t)n < len))
        n += snprintf(out + n, len - n, " its address space %lld of %lld MB left,", m.space_kb / 1024, m.space_total_kb / 1024);
    if ((m.free_kb >= 0) && (m.total_kb > 0) && (n > 0) && ((size_t)n < len))
        n += snprintf(out + n, len - n, " the computer %lld of %lld MB free,", m.free_kb / 1024, m.total_kb / 1024);
    if ((m.swap_kb >= 0) && (n > 0) && ((size_t)n < len))
        n += snprintf(out + n, len - n, " swap %lld MB in use,", m.swap_kb / 1024);
    if ((m.commit_kb >= 0) && (m.limit_kb > 0) && (n > 0) && ((size_t)n < len))
        n += snprintf(out + n, len - n, " committed %lld of %lld MB", m.commit_kb / 1024, m.limit_kb / 1024);
    if ((n > 0) && ((size_t)n < len) && (out[n - 1] == ','))
        out[n - 1] = '\0';
    if (strcmp(out, "; memory:") == 0)
        out[0] = '\0';
}

/* Short of memory: less than this much free (or this share of the RAM, whichever is more), Windows' commit limit
   within this share of being reached, or the 32-bit game's address space this close to full. */
#define LW_LOW_FREE_MB 256
#define LW_LOW_FREE_SHARE 0.03
#define LW_LOW_COMMIT_SHARE 0.97
#define LW_LOW_SPACE_MB 256

TbBool lag_watch_memory_short(char *why, size_t len)
{
    struct LwMem m;
    lw_read(&m);
    if ((m.space_kb >= 0) && (m.space_kb < LW_LOW_SPACE_MB * 1024LL))
    {
        snprintf(why, len, "the game has used up nearly all the memory a 32-bit program can have (%lld MB of %lld left)",
            m.space_kb / 1024, m.space_total_kb / 1024);
        return true;
    }
    if ((m.free_kb >= 0) && (m.total_kb > 0))
    {
        /* test switch KFX_MEM_LOW_MB=n: memory counts as short below n MB free (to try the warning and the crash
           report's words without running out) */
        static long long test_mb = -2;
        if (test_mb == -2)
            test_mb = (getenv("KFX_MEM_LOW_MB") != NULL) ? atoll(getenv("KFX_MEM_LOW_MB")) : -1;
        long long low = ((test_mb > 0) ? test_mb : LW_LOW_FREE_MB) * 1024LL;
        if ((test_mb <= 0) && (m.total_kb * LW_LOW_FREE_SHARE > low))
            low = (long long)(m.total_kb * LW_LOW_FREE_SHARE);
        if (m.free_kb < low)
        {
            snprintf(why, len, "the computer has almost no memory free (%lld MB of %lld)", m.free_kb / 1024, m.total_kb / 1024);
            return true;
        }
    }
    if ((m.commit_kb >= 0) && (m.limit_kb > 0) && (m.commit_kb > m.limit_kb * LW_LOW_COMMIT_SHARE))
    {
        snprintf(why, len, "the computer's memory and page file are nearly full (%lld of %lld MB committed)",
            m.commit_kb / 1024, m.limit_kb / 1024);
        return true;
    }
    if (len > 0)
        why[0] = '\0';
    return false;
}

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
    char mem[192];
    lag_watch_memory_text(mem, sizeof(mem));
    SYNCLOG("Lag: the detailed turn timer is on from now (\"World profile\" lines every 200 turns show the slow part)%s", mem);
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
    /* memory running out: said before the computer swaps the game to a crawl or the system ends it (Linux's and
       Android's out-of-memory killer leaves no trace in the game's log), at most once a minute */
    char why[160];
    if (lag_watch_memory_short(why, sizeof(why)) && ((lw.low_memory_line == 0) || (now - lw.low_memory_line > 60000 * NS_PER_MS)))
    {
        char mem[192];
        lag_watch_memory_text(mem, sizeof(mem));
        lw_lagged();
        SYNCLOG("Lag: memory is running out: %s%s", why, mem);
        lw.low_memory_line = now;
    }
    const double seconds = (double)span / 1e9;
    const double speed = (double)lw.turns / seconds;
    const TbBool slow_speed = (turns_per_second > 0) && (speed < LW_SPEED_SHARE * turns_per_second);
    static int always = -1; /* test switch KFX_LAG_ALWAYS=1 (singleplayer pass, 2026-11-28): the speed line every
                               stretch, lag or not (frame rate and drawing cost of a normal game); no detailed timer */
    if (always < 0)
        always = (getenv("KFX_LAG_ALWAYS") != NULL) ? 1 : 0;
    if (!lw.void_window && (slow_speed || (lw.slow > 0) || always) && (lw.turns > 0))
    {
        if (slow_speed || (lw.slow > 0))
            lw_lagged();
        char mem[192];
        lag_watch_memory_text(mem, sizeof(mem));
        SYNCLOG("Lag: %.1f turns a second of %d over %.0f s (turn %lu): a turn %.1f ms on average (most %.0f), "
            "%ld frames %.1f ms each to draw and %.1f ms to hand over (most %.0f), %ld slow turns or frames%s%s",
            speed, (int)turns_per_second, seconds, (unsigned long)game.play_gameturn,
            (double)lw.turn_sum / lw.turns / NS_PER_MS, (double)lw.turn_max / NS_PER_MS, lw.frames,
            (lw.frames > 0) ? (double)lw.draw_sum / lw.frames / NS_PER_MS : 0.0,
            (lw.frames > 0) ? (double)lw.present_sum / lw.frames / NS_PER_MS : 0.0, (double)lw.frame_max / NS_PER_MS,
            lw.slow, (lw.held_back > 0) ? " (some lines held back)" : "", mem);
        lw.held_back = 0;
    }
    const int64_t last_line = lw.last_line;
    const long held = lw.held_back;
    const TbBool profiled = lw.profiled;
    const int64_t low_memory_line = lw.low_memory_line;
    memset(&lw, 0, sizeof(lw));
    lw.low_memory_line = low_memory_line;
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

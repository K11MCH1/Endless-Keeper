/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_seasons.c
 *     Endless world: seasons below.
 * @par Purpose:
 *     See world_seasons.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_seasons.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "game_legacy.h"
#include "player_data.h"
#include "thing_data.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "world_caretaker.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static const char *const season_names[WSe_COUNT] = {"the Thaw", "the Swelter", "the Rot", "the Long Night"};
static const char *const season_lines[WSe_COUNT] = {
    "Hatcheries are quick, and the lakes are thick with Flies.",
    "The heroes come more often, and fire burns hotter.",
    "Hatcheries falter, and the Graveyard fills twice as fast.",
    "Night is long, wild packs are bigger, and portals are slow."};

/** KFX_WORLD_SEASON=n (test switch): it is always season n. */
static int forced_season(void)
{
    static int forced = -2;
    if (forced == -2)
    {
        const char *v = getenv("KFX_WORLD_SEASON");
        forced = (v != NULL) ? atoi(v) : -1;
        if ((forced < 0) || (forced >= WSe_COUNT))
            forced = -1;
        else
            SYNCLOG("World: test switch KFX_WORLD_SEASON=%d (%s)", forced, season_names[forced]);
    }
    return forced;
}

int world_season_of_day(long day)
{
    const int f = forced_season();
    if (f >= 0)
        return f;
    if (day < 1)
        day = 1;
    return (int)(((day - 1) / SEASON_DAYS) % WSe_COUNT);
}

int world_season(void)
{
    return world_season_of_day(world_day_number());
}

const char *world_season_name(int season)
{
    return ((season >= 0) && (season < WSe_COUNT)) ? season_names[season] : "";
}

TbBool world_season_harsh_for(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_COUNT) || !world_player_is_human(p))
        return true;
    if (world_caretaker_active(p))
        return false; /* away: the caretaker's dungeon must not get harder */
    return world_seat_day(p) > SEASON_SPARED_DAYS;
}

TbBool world_season_long_night(long day)
{
    return world_mode_active() && (world_season_of_day(day) == WSe_LongNight);
}

int world_season_hatch_pct(PlayerNumber p)
{
    if (!world_mode_active())
        return 0;
    switch (world_season())
    {
    case WSe_Thaw:
        return 25;
    case WSe_Rot:
        return world_season_harsh_for(p) ? -50 : 0;
    default:
        return 0;
    }
}

int world_season_extra_raids(PlayerNumber p)
{
    return (world_mode_active() && (world_season() == WSe_Swelter) && world_season_harsh_for(p)) ? 1 : 0;
}

int world_season_fire_pct(const struct Thing *victim)
{
    if (!world_mode_active() || (world_season() != WSe_Swelter) || !thing_exists(victim))
        return 0;
    return world_season_harsh_for(victim->owner) ? 25 : 0;
}

int world_season_grave_pct(PlayerNumber p)
{
    return (world_mode_active() && (world_season() == WSe_Rot) && world_season_harsh_for(p)) ? 50 : 100;
}

int world_season_pack_extra(TbBool by_water, TbBool *flies)
{
    *flies = false;
    if (!world_mode_active())
        return 0;
    const int s = world_season();
    if ((s == WSe_Thaw) && by_water)
    {
        *flies = true;
        return 2;
    }
    return (s == WSe_LongNight) ? 1 : 0;
}

int world_season_portal_pct(PlayerNumber p)
{
    return (world_mode_active() && (world_season() == WSe_LongNight) && world_season_harsh_for(p)) ? 50 : 0;
}

/******************************************************************************/
/** The year's ring: 28 marks round the dial, coloured by season, today's bright. */
void world_seasons_draw_ring(int x, int y, int size)
{
    if (!world_mode_active())
        return;
    static TbPixel col[WSe_COUNT][2];
    static TbBool made = false;
    if (!made)
    {
        static const unsigned char rgb[WSe_COUNT][3] = {{20, 48, 24}, {60, 40, 10}, {34, 30, 14}, {18, 22, 50}};
        for (int s = 0; s < WSe_COUNT; s++)
        {
            col[s][0] = LbPaletteFindColour(engine_palette, rgb[s][0] / 2, rgb[s][1] / 2, rgb[s][2] / 2);
            col[s][1] = LbPaletteFindColour(engine_palette, (unsigned char)min(63, rgb[s][0] + 10), (unsigned char)min(63, rgb[s][1] + 10),
                (unsigned char)min(63, rgb[s][2] + 10));
        }
        made = true;
    }
    const long today = world_day_number();
    const int today_i = (int)((max(today, 1L) - 1) % SEASON_YEAR_DAYS);
    const int dot = max(2, size / 22);
    /* on the dial own rim: the dial sits against the screen edge, so the ring stays inside it */
    const double cx = x + size / 2.0, cy = y + size / 2.0, r = size / 2.0 - dot;
    for (int i = 0; i < SEASON_YEAR_DAYS; i++)
    {
        /* the year runs clockwise from the top */
        const double a = -M_PI / 2 + 2.0 * M_PI * i / SEASON_YEAR_DAYS;
        const int px = (int)floor(cx + r * cos(a) + 0.5), py = (int)floor(cy + r * sin(a) + 0.5);
        const int s = i / SEASON_DAYS;
        const int d = (i == today_i) ? dot + 2 : dot;
        LbDrawBox(px - d / 2, py - d / 2, d, d, col[s][(i == today_i) ? 1 : 0]);
    }
}

/** KFX_WORLD_SEASONTEST=1 (test switch): every day's dawn logs the season and what it does for this machine's keeper. */
void world_seasons_turn(void)
{
    if (!world_mode_active())
        return;
    static int last = -1;
    const int s = world_season();
    if (s != last)
    {
        if (last >= 0)
        {
            char text[160];
            snprintf(text, sizeof(text), "The season turns: %s. %s", season_names[s], season_lines[s]);
            SYNCLOG("World: %s", text);
            /* every machine knows the day: each tells its own keeper (a dedicated server tells nobody) */
            if (!netstate_dedicated())
            {
                if (chat_box_active())
                    chat_box_add(-1, text);
                else
                    message_add(MsgType_Blank, 0, text);
            }
        }
        last = s;
    }
    static int test = -1;
    if (test < 0)
        test = (getenv("KFX_WORLD_SEASONTEST") != NULL) ? 1 : 0;
    if (test && ((game.play_gameturn % (long)world_day_turns()) == 1))
    {
        /* this machine's keeper, or on a dedicated server the first human keeper (a joined player) */
        PlayerNumber p = my_player_number;
        for (PlayerNumber q = 0; netstate_dedicated() && (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
            if (world_player_is_human(q))
            {
                p = q;
                break;
            }
        TbBool flies;
        const int extra = world_season_pack_extra(true, &flies);
        SYNCLOG("World: season test: day %ld, %s, long night %d, day lit %d; for player %d (day %ld%s): hatch %+d%%, raids +%d, "
            "fire %+d%%, grave %d%%, portals +%d%%, packs by water +%d%s, packs +%d", world_day_number(), season_names[s],
            (int)world_season_long_night(world_day_number()), (int)world_is_day(), (int)p, world_seat_day(p),
            world_season_harsh_for(p) ? "" : ", spared", world_season_hatch_pct(p), world_season_extra_raids(p),
            (world_season() == WSe_Swelter && world_season_harsh_for(p)) ? 25 : 0, world_season_grave_pct(p),
            world_season_portal_pct(p), extra, flies ? " (Flies)" : "", world_season_pack_extra(false, &flies));
    }
}

#ifdef __cplusplus
}
#endif

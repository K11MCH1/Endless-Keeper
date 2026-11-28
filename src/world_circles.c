/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_circles.c
 *     Endless world: the nine circles round every dungeon (2026-11-11).
 * @par Purpose:
 *     The circle of a place, by its distance in slabs to the nearest dungeon start: the n-th circle is n * 250
 *     slabs wide (they end at 250, 750, 1500, 2500, 3750, 5250, 7000, 9000); the ninth is everything beyond.
 *     Enemies a keeper travels to follow their circle: in circles 1-4 a share (50, 75, 100, 125%) of the nearest
 *     keeper's best creature level (of a party: the better of the two), never below what the generator gives;
 *     in circles 5-9 fixed levels (30, 45, 60, 80, 100). Hero raids that come to a dungeon don't follow circles.
 *     A keeper whose imps dig a slab out beyond the circles it has breached sees that circle's name and line at the
 *     top of the screen, once per circle, ever (kept in the world save: the CIR1 extra).
 *     The names and lines are not written out here: they are for the players to find. The tables below hold them
 *     encoded (made by a script that is not part of the source).
 * Test hook: KFX_WORLD_CIRCLETEST="far=N,top=L,breach=C@T,turns": every place counts as N slabs farther from the
 *     nearest start; the nearest keeper's best level counts as L; player 0 breaches circle C at turn T; the banner
 *     runs on game turns (20 a second) instead of the clock, so a screenshot at a turn shows a known moment.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_circles.h"
#include "world_stream.h"
#include "world_wild.h"
#include "world_attract.h"
#include "world_threat.h"
#include "kfx/renderer/RendererManager.h"
#include "ui_molten.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL3/SDL.h>
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_fileio.h"
#include "bflib_dernc.h"
#include "bflib_sprite.h"
#include "config.h"
#include "bflib_sound.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "config_sounds.h"
#include "front_simple.h"
#include "frontend.h"
#include "game_legacy.h"
#include "globals.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "kfx/renderer/RendererManager.h"
#include "server_admin.h"
#include "gui_ttf.h"
#include "thing_stats.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_spdig.h"
#include "dungeon_data.h"
#include "map_blocks.h"
#include "map_columns.h"
#include "map_data.h"
#include "power_hand.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "tasks_list.h"
#include "post_inc.h"

/* circles 1-4: the share of the nearest keeper's best creature level, in percent; 5-9: the fixed level */
static const int wc_share[4] = {50, 75, 100, 125};
static const int wc_fixed[5] = {30, 45, 60, 80, 100};

static const uint8_t wc_t0_1[] = {0x88,0xB8,0xB3,0x89,0x97,0x05};
static const uint8_t wc_t0_2[] = {0xB2,0x86,0x9E,0x67,0x79,0x22};
static const uint8_t wc_t0_3[] = {0xBB,0x79,0x7D,0x47,0x47,0x4C,0x4C};
static const uint8_t wc_t0_4[] = {0x5A,0x4B,0x5D,0x27,0x3D,0x33,0x07,0x76};
static const uint8_t wc_t0_5[] = {0x7F,0x20,0x3A,0x3A,0x02,0x17,0xE7,0x93};
static const uint8_t wc_t0_6[] = {0x01,0x03,0x1D,0x08,0xE8,0xE4,0xD6,0xC3,0xBD};
static const uint8_t wc_t0_7[] = {0x3B,0x0D,0xE7,0xF8,0xCA,0xDF,0xA1,0xCD};
static const uint8_t wc_t0_8[] = {0xCE,0xFE,0xC8,0xD2,0xA7,0xBF,0xB3,0xEA};
static const uint8_t wc_t0_9[] = {0xF8,0xDC,0xAE,0xBC,0x8D,0xED};
static const uint8_t wc_t1_1[] = {0xDA,0xCE,0xDA,0xD3,0xA1,0xBB,0x85,0xCF,0x88,0x66,0x36,0x45,0x55,0x5C,0x38,0x79,0x44,0x28,0x1B,0xFF,0xB6,0xA5};
static const uint8_t wc_t1_2[] = {0xE5,0xD6,0xAE,0xF8,0x94,0x87,0x96,0x69,0x6D,0x06,0x51,0x25,0x2B,0x35,0x15,0x11,0x81};
static const uint8_t wc_t1_3[] = {0x9A,0xB3,0x8D,0xD5,0x66,0x6E,0x6E,0x42,0x16,0x2F,0x3F,0x32,0x01,0x04,0xA4,0xF3,0xFF,0xC8,0xD3,0xC5};
static const uint8_t wc_t1_4[] = {0xA5,0x97,0x25,0x7D,0x71,0x49,0x1E,0x35,0x73,0x0C,0x0C,0x14,0xE3,0xB8,0x81,0xCB,0xCD,0xAD,0xA7,0x9B,0x80,0x92,0x6C,0x31,0x50,0x10,0x5A,0x38,0x36,0x12,0x14,0x7E};
static const uint8_t wc_t1_5[] = {0x46,0x7A,0x56,0x47,0x55,0x27,0x31,0x43,0x12,0x08,0xFE,0xB7,0xD4,0xD0,0xD7,0xA5,0xD8};
static const uint8_t wc_t1_6[] = {0x69,0x5D,0x5C,0x27,0x3C,0x02,0x53,0xE1,0xFA,0xFB,0xDE,0x94,0xA7,0xA1,0xA9,0xC8,0x94,0x22,0x7D,0x79,0x48,0x45,0x2C,0x3E,0x5D};
static const uint8_t wc_t1_7[] = {0x0C,0x20,0x32,0x0C,0x56,0xF1,0xF5,0xE9,0xDF,0xC5,0xAA,0xD1};
static const uint8_t wc_t1_8[] = {0x0D,0x19,0x10,0xE8,0xB3,0xC8,0xCC,0xC9,0xE7,0xB5,0xC1,0x86,0x94,0x65,0x70,0x22};
static const uint8_t wc_t1_9[] = {0x3E,0xEC,0xF0,0xCC,0xC2,0xD8,0xEA,0xA3,0x8C,0x94,0xDE,0x6F,0x79,0x57,0x59,0x13,0x6C,0x2D,0x0E,0x16,0xF2,0xE8,0xBA,0xD0,0xD5,0xB2,0xEE,0xAF,0x80,0x9C,0x71,0x0F};
static const uint8_t *const wc_t0[WC_CIRCLES] = {wc_t0_1, wc_t0_2, wc_t0_3, wc_t0_4, wc_t0_5, wc_t0_6, wc_t0_7, wc_t0_8, wc_t0_9};
static const uint8_t *const wc_t1[WC_CIRCLES] = {wc_t1_1, wc_t1_2, wc_t1_3, wc_t1_4, wc_t1_5, wc_t1_6, wc_t1_7, wc_t1_8, wc_t1_9};

#define WC_TEXT_MAX 64
/** A circle's name (kind 0) or line (kind 1), decoded into buf. */
static const char *wc_text(int kind, int circle, char *buf)
{
    buf[0] = '\0';
    if ((circle < 1) || (circle > WC_CIRCLES))
        return buf;
    const uint8_t *src = (kind == 0) ? wc_t0[circle - 1] : wc_t1[circle - 1];
    for (int i = 0; i < WC_TEXT_MAX - 1; i++)
    {
        buf[i] = (char)(src[i] ^ (uint8_t)(0xA7 + i * 13 + (circle + kind * 16) * 29));
        if (buf[i] == '\0')
            break;
    }
    buf[WC_TEXT_MAX - 1] = '\0';
    return buf;
}

/******************************************************************************/
/* The test hook */

static struct {
    TbBool read;
    long far;           /* slabs added to every distance */
    int top;            /* the nearest keeper's best level (0: the real one) */
    int breach;         /* player 0 breaches this circle ... */
    long breach_turn;   /* ... at this turn */
    TbBool turns;       /* the banner runs on game turns */
} wc_test;

static void wc_test_read(void)
{
    if (wc_test.read)
        return;
    wc_test.read = true;
    const char *v = getenv("KFX_WORLD_CIRCLETEST");
    if (v == NULL)
        return;
    const char *s;
    if ((s = strstr(v, "far=")) != NULL)
        wc_test.far = atol(s + 4);
    if ((s = strstr(v, "top=")) != NULL)
        wc_test.top = atoi(s + 4);
    if ((s = strstr(v, "breach=")) != NULL)
    {
        wc_test.breach = atoi(s + 7);
        const char *at = strchr(s, '@');
        wc_test.breach_turn = (at != NULL) ? atol(at + 1) : 100;
    }
    wc_test.turns = (strstr(v, "turns") != NULL);
    SYNCLOG("World circles test: far %ld, top %d, breach %d at turn %ld%s", wc_test.far, wc_test.top, wc_test.breach,
        wc_test.breach_turn, wc_test.turns ? ", banner on game turns" : "");
}

/******************************************************************************/
/* Circles and levels */

int world_circle_of_distance(double distance)
{
    if (!(distance > 0.0))
        return 1;
    /* growing widths (2026-11-11; every circle was 250 wide, which the user found too short): the n-th circle is
       n * 250 slabs wide, so they end at 250, 750, 1500, 2500, 3750, 5250, 7000 and 9000 slabs; the ninth is
       everything beyond */
    double end = 0.0;
    for (int circle = 1; circle < WC_CIRCLES; circle++)
    {
        end += (double)circle * WC_CIRCLE_SLABS;
        if (distance < end)
            return circle;
    }
    return WC_CIRCLES;
}

static double wc_distance(int64_t wx, int64_t wy)
{
    wc_test_read();
    return world_dist_to_nearest_start(wx, wy) + (double)wc_test.far;
}

double world_circles_distance(int64_t wx, int64_t wy)
{
    return wc_distance(wx, wy);
}

int world_circle_at_world(int64_t wx, int64_t wy)
{
    if (!world_mode_active() || (world_generator() == NULL))
        return 1;
    return world_circle_of_distance(wc_distance(wx, wy));
}

int world_circle_at_engine_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    int64_t wx, wy;
    world_slab_to_world(slb_x, slb_y, &wx, &wy);
    return world_circle_at_world(wx, wy);
}

/** A keeper's best creature level, of a party the better of the two. */
static int wc_keeper_top(PlayerNumber plyr_idx)
{
    wc_test_read();
    if (wc_test.top > 0)
        return wc_test.top;
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return 1;
    int top = world_keeper_top_level(plyr_idx);
    const PlayerNumber mate = world_coop_partner(plyr_idx);
    if ((mate != PLAYER_NONE) && (mate >= 0) && (mate < PLAYERS_COUNT))
        top = max(top, world_keeper_top_level(mate));
    return max(top, 1);
}

static int wc_level_for(int circle, int top, int floor_level)
{
    int level;
    if (circle >= 5)
        level = wc_fixed[min(circle, WC_CIRCLES) - 5];
    else
        level = max(floor_level, (top * wc_share[max(circle, 1) - 1] + 50) / 100);
    return max(1, min(level, CREATURE_MAX_LEVEL_LIMIT));
}

int world_circles_level_at_world(int64_t wx, int64_t wy, int floor_level, int *circle_out)
{
    if (circle_out != NULL)
        *circle_out = 1;
    if (!world_mode_active() || world_attract_active() || (world_generator() == NULL))
        return floor_level;
    const int circle = world_circle_at_world(wx, wy);
    if (circle_out != NULL)
        *circle_out = circle;
    const int top = (circle >= 5) ? 0 : wc_keeper_top(world_nearest_start_player(wx, wy));
    return wc_level_for(circle, top, floor_level);
}

int world_circles_level_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y, int floor_level, const char *what)
{
    int64_t wx, wy;
    world_slab_to_world(slb_x, slb_y, &wx, &wy);
    int circle = 1;
    const int level = world_circles_level_at_world(wx, wy, floor_level, &circle);
    if ((what != NULL) && world_mode_active() && !world_attract_active())
    {
        const PlayerNumber near = world_nearest_start_player(wx, wy);
        SYNCLOG("World circles: %s at world slab (%ld,%ld), circle %d: level %d (nearest keeper %d's best %d, floor %d)",
            what, (long)wx, (long)wy, circle, level, (int)near, wc_keeper_top(near), floor_level);
    }
    return level;
}

int world_circles_wild_level_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y, int floor_level, const char *what)
{
    int64_t wx, wy;
    world_slab_to_world(slb_x, slb_y, &wx, &wy);
    if (!world_mode_active() || world_attract_active() || (world_generator() == NULL))
        return floor_level;
    const int circle = world_circle_at_world(wx, wy);
    const PlayerNumber near = world_nearest_start_player(wx, wy);
    /* the army's upper quarter, not its best creature (the threat budget, 2026-11-18: one strong creature brought
       packs and raids far above the rest of the army) */
    const int top = (circle >= 5) ? 0 : world_threat_upper_level(near);
    /* wild packs (the user, 2026-11-22: level 8 creatures met packs of level 3 at home): the army's upper quarter in
       circles 1 and 2, a level above in circle 3, two in circle 4 (it was the circles' 50, 75, 100 and 125%); circles
       5-9 their fixed levels */
    const int level = (circle >= 5) ? wc_level_for(circle, top, floor_level) :
        max(1, min(max(floor_level, top + ((circle <= 2) ? 0 : circle - 2)), CREATURE_MAX_LEVEL_LIMIT));
    if (what != NULL)
        SYNCLOG("World circles: %s at world slab (%ld,%ld), circle %d: level %d (nearest keeper %d's army's upper quarter %d,"
            " floor %d)", what, (long)wx, (long)wy, circle, level, (int)near, top, floor_level);
    return level;
}

int world_circles_hero_floor_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!world_mode_active() || (world_generator() == NULL))
        return 1;
    int64_t wx, wy;
    world_slab_to_world(slb_x, slb_y, &wx, &wy);
    /* the generator's hero levels: 1 + distance / 120, at most 8 */
    return max(1, min(8, 1 + (int)(world_dist_to_nearest_start(wx, wy) / 120.0)));
}

int world_circles_welcome_level(PlayerNumber plyr_idx)
{
    return wc_level_for(1, wc_keeper_top(plyr_idx), 1);
}

/******************************************************************************/
/* Breaches and the banner */

static uint8_t wc_breached[PLAYERS_MAX];  /* the highest circle each keeper's imps have dug into (0: the first) */
static int wc_client_breached;            /* a thin client: what the server says of this player (0: not told) */

static int wc_banner_circle;              /* 0: none */
static TbClockMSec wc_banner_from;
static long wc_banner_from_turn;

#define WC_FADE_MS   1000   /* each text fades in over this, and both fade out over it */
#define WC_LINE_MS    700   /* the line starts this long after the name */
#define WC_HOLD_MS   3000   /* both stay for this once the line is in full */
#define WC_FADE_STEPS  10   /* 0 full .. 9 faintest */
#define WC_NOTE_MARK "\x01WC"

int world_circles_breached(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return 1;
    return max(1, min((int)wc_breached[plyr_idx], WC_CIRCLES));
}

void world_circles_player_reset(PlayerNumber plyr_idx)
{
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        wc_breached[plyr_idx] = 1;
}

static void wc_banner_start(int circle)
{
    wc_banner_circle = circle;
    wc_banner_from = LbTimerClock();
    wc_banner_from_turn = (long)game.play_gameturn;
    if (snd_larg_tile_down > 0)
        play_non_3d_sample(snd_larg_tile_down);
}

/** The player's machine is another one: the server's word to its thin client ('B': show the banner; 'Q': quietly,
 *  what it has breached, for its map). */
static void wc_tell_client(PlayerNumber plyr_idx, char how, int circle)
{
    if (!netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(plyr_idx));
    if ((u <= SERVER_ID) || (u >= MAX_NET_USERS))
        return;
    char text[16];
    snprintf(text, sizeof(text), WC_NOTE_MARK "%c%d", how, circle);
    server_admin_notice(u, text);
}

static void wc_breach(PlayerNumber plyr_idx, int circle, const char *how)
{
    wc_breached[plyr_idx] = (uint8_t)circle;
    const TbBool local = is_my_player_number(plyr_idx) && !netstate_dedicated();
    SYNCLOG("World circles: player %d breaches circle %d (%s)%s", (int)plyr_idx, circle, how,
        local ? ": banner" : "");
    if (local)
        wc_banner_start(circle);
    else
        wc_tell_client(plyr_idx, 'B', circle);
}

void world_circles_dug(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_mode_active() || world_attract_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    if (!world_player_is_human(plyr_idx))
        return;
    const int circle = world_circle_at_engine_slab(subtile_slab(stl_x), subtile_slab(stl_y));
    {
        static int logged;
        if ((wc_test.far != 0) && (logged++ < 3))
            SYNCLOG("World circles test: player %d's imp dug a slab in circle %d (breached so far: %d)", (int)plyr_idx,
                circle, world_circles_breached(plyr_idx));
    }
    if (circle <= world_circles_breached(plyr_idx))
        return;
    wc_breach(plyr_idx, circle, "its imps dug through the line");
}

TbBool world_circles_net_notice(const char *text)
{
    if ((text == NULL) || (strncmp(text, WC_NOTE_MARK, 3) != 0))
        return false;
    const int circle = atoi(text + 4);
    if ((circle >= 1) && (circle <= WC_CIRCLES))
    {
        wc_client_breached = max(wc_client_breached, circle);
        if (text[3] == 'B')
            wc_banner_start(circle);
    }
    return true;
}


/******************************************************************************/
/* Test hook KFX_WORLD_DIGRUN="imps[,drops[,stop_circle]]" (local, single player): from turn 400 player 0 digs a
   1-wide tunnel east from its Heart for ever, the way a player keeps marking slabs ahead of the dig: a few slabs past
   the dig front are always marked, round impenetrable rock, gems, lava and walls it cannot dig. The keeper keeps that
   many Imps (more are made when some die). drops=1 (the default): every 10 s each free Imp more than 25 slabs from
   the front is put down at the claimed slab nearest the front, as a player drops them with the Hand, and the gold it
   carries goes to the treasury (as a player who drops it there first). "DIGRUN" lines: every game minute the front's
   distance from the start and its circle, each circle reached with its time, "DIGRUN: done" at stop_circle (9). */

#define DR_AHEAD 6
#define DR_BX0 24   /* the search box: this far behind the anchor (west) */
#define DR_BX1 56   /* this far ahead (east) */
#define DR_BY 40    /* this far to each side */
#define DR_BW (DR_BX0 + DR_BX1 + 1)
#define DR_BH (2 * DR_BY + 1)
#define DR_N (DR_BW * DR_BH)
static struct {
    int imps, drops, stop;
    TbBool on, done;
    long started;
    int64_t row, ax, ay; /* the start's row; the anchor: the farthest dug slab on the way, connected to the dungeon */
    int circle, marked;
    long dropped, made, gold;
    double best;
    int32_t cost[DR_N], prev[DR_N];
    int8_t kind[DR_N];
    int32_t heap[DR_N * 4];
    int path[DR_N], plen;
} dr;

/** 0 open (dug), 1 to dig, 2 cannot be dug, -1 not loaded. */
static int dr_slab(int64_t wx, int64_t wy)
{
    MapSlabCoord ex, ey;
    if (!world_engine_slab(wx, wy, &ex, &ey))
        return -1;
    const struct SlabMap *slb = get_slabmap_block(ex, ey);
    if (slabmap_block_invalid(slb))
        return -1;
    const SlabKind k = slb->kind;
    if ((k == SlbT_ROCK) || (k == SlbT_GEMS) || (k == SlbT_LAVA))
        return 2;
    const struct SlabConfigStats *st = get_slab_kind_stats(k);
    if (((st->block_flags & SlbAtFlg_IsRoom) != 0) && (slabmap_owner(slb) == PLAYER0))
        return 0; /* (the keeper's own rooms, the Heart's middle too) */
    if ((st->block_flags & SlbAtFlg_IsDoor) != 0)
        return (slabmap_owner(slb) == PLAYER0) ? 0 : 2;
    if ((st->block_flags & SlbAtFlg_Blocking) != 0)
        return (((st->block_flags & SlbAtFlg_Digable) != 0) || (slabmap_owner(slb) == PLAYER0)) ? 1 : 2; /* (its own
            walls a keeper can dig too) */
    return 0;
}

static void dr_box_xy(int i, int64_t *wx, int64_t *wy)
{
    *wx = dr.ax - DR_BX0 + i % DR_BW;
    *wy = dr.ay - DR_BY + i / DR_BW;
}

/** The anchor moves only over ground the keeper holds or one of its Imps has reached (within 3 slabs): an open cave
 *  ahead is walked, not skipped. */
static TbBool dr_reached(int i)
{
    int64_t wx, wy;
    MapSlabCoord ex, ey;
    dr_box_xy(i, &wx, &wy);
    if (!world_engine_slab(wx, wy, &ex, &ey))
        return false;
    if (slabmap_owner(get_slabmap_block(ex, ey)) == PLAYER0)
        return true;
    const struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    long guard = 0;
    for (ThingIndex t = dungeon->digger_list_start; (t > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        const struct Thing *thing = thing_get(t);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        t = cctrl->players_next_creature_idx;
        if ((abs((int)subtile_slab(thing->mappos.x.stl.num) - ex) <= 3) && (abs((int)subtile_slab(thing->mappos.y.stl.num) - ey) <= 3))
            return true;
    }
    return false;
}

/** Plans the way on from the anchor: the cheapest route (walking 1 a slab, digging 5) to the place in the box that
 *  is best for going east (cost less 6 a slab east, plus 1 a slab off the start's row; ground that isn't loaded yet
 *  counts as to dig, so the way goes round the rock it knows towards it); the anchor moves on along it
 *  over dug ground; dr.path gets the route from the anchor. */
static void dr_plan(void)
{
    for (int i = 0; i < DR_N; i++)
    {
        int64_t wx, wy;
        dr_box_xy(i, &wx, &wy);
        dr.kind[i] = (int8_t)dr_slab(wx, wy);
        dr.cost[i] = INT32_MAX;
        dr.prev[i] = -1;
    }
    const int start = DR_BY * DR_BW + DR_BX0;
    int hn = 0;
    dr.cost[start] = 0;
    dr.heap[hn++] = start;
    while (hn > 0)
    {
        /* (a binary heap on cost) */
        const int u = dr.heap[0];
        dr.heap[0] = dr.heap[--hn];
        for (int c = 0;;)
        {
            int l = 2 * c + 1, r = l + 1, m = c;
            if ((l < hn) && (dr.cost[dr.heap[l]] < dr.cost[dr.heap[m]])) m = l;
            if ((r < hn) && (dr.cost[dr.heap[r]] < dr.cost[dr.heap[m]])) m = r;
            if (m == c) break;
            const int t = dr.heap[c]; dr.heap[c] = dr.heap[m]; dr.heap[m] = t; c = m;
        }
        const int ux = u % DR_BW, uy = u / DR_BW;
        static const int ddx[4] = {1, 0, 0, -1}, ddy[4] = {0, 1, -1, 0};
        for (int d = 0; d < 4; d++)
        {
            const int vx = ux + ddx[d], vy = uy + ddy[d];
            if ((vx < 0) || (vy < 0) || (vx >= DR_BW) || (vy >= DR_BH))
                continue;
            const int v = vy * DR_BW + vx;
            if (dr.kind[v] == 2)
                continue;
            const int32_t nc = dr.cost[u] + ((dr.kind[v] != 0) ? 5 : 1); /* (ground not loaded yet: as if to dig) */
            if (nc >= dr.cost[v] || (hn >= DR_N * 4))
                continue;
            dr.cost[v] = nc;
            dr.prev[v] = u;
            int c = hn;
            dr.heap[hn++] = v;
            while (c > 0)
            {
                const int pa = (c - 1) / 2;
                if (dr.cost[dr.heap[pa]] <= dr.cost[dr.heap[c]]) break;
                const int t = dr.heap[c]; dr.heap[c] = dr.heap[pa]; dr.heap[pa] = t; c = pa;
            }
        }
    }
    /* the goal: the best place at least 8 slabs further east (if none can be reached, the farthest east there is) */
    int best = -1, far_x = -1;
    long best_score = 0;
    for (int i = 0; i < DR_N; i++)
        if ((dr.cost[i] != INT32_MAX) && (i % DR_BW > far_x))
            far_x = i % DR_BW;
    const int min_x = min(DR_BX0 + 8, far_x);
    for (int i = 0; i < DR_N; i++)
    {
        if ((dr.cost[i] == INT32_MAX) || (i % DR_BW < min_x))
            continue;
        int64_t wx, wy;
        dr_box_xy(i, &wx, &wy);
        const long score = (long)dr.cost[i] - 6L * (long)(wx - dr.ax) + (long)llabs((long long)(wy - dr.row));
        if ((best < 0) || (score < best_score))
        {
            best = i;
            best_score = score;
        }
    }
    dr.plen = 0;
    for (int i = best; (i >= 0) && (dr.plen < DR_N); i = dr.prev[i])
        dr.path[dr.plen++] = i;
    for (int i = 0; i < dr.plen / 2; i++)
    {
        const int t = dr.path[i]; dr.path[i] = dr.path[dr.plen - 1 - i]; dr.path[dr.plen - 1 - i] = t;
    }
    /* the anchor moves on over the dug part of the way (kept a few slabs back: the box stays round the dig) */
    int k = 0;
    while ((k + 1 < dr.plen) && (dr.kind[dr.path[k + 1]] == 0) && dr_reached(dr.path[k + 1]))
        k++;
    if (k > 0)
    {
        int64_t wx, wy;
        dr_box_xy(dr.path[k], &wx, &wy);
        dr.ax = wx;
        dr.ay = wy;
        memmove(dr.path, dr.path + k, (size_t)(dr.plen - k) * sizeof(dr.path[0]));
        dr.plen -= k;
    }
}

void world_circles_want_chunks(void (*want)(int64_t wx, int64_t wy, int r))
{
    if (dr.on && !dr.done && (dr.started != 0))
        want(dr.ax, dr.ay, 1); /* (the dig front's ground, as the Imps working there keep it) */
}

static TbBool dr_imp_free(const struct Thing *thing)
{
    if (!thing_exists(thing) || (thing->owner != PLAYER0) || !thing_is_creature_special_digger(thing))
        return false;
    if (thing_is_picked_up(thing) || ((thing->alloc_flags & TAlF_IsControlled) != 0) || (thing->health <= 0))
        return false;
    if (creature_is_being_unconscious(thing) || creature_is_kept_in_custody(thing) ||
        creature_is_dragging_or_being_dragged(thing) || creature_is_dying(thing))
        return false;
    return get_creature_state_type(thing) != CrStTyp_FightCrtr;
}

static void dr_imps(const struct Coord3d *drop, MapSlabCoord fx, MapSlabCoord fy, int drop_d)
{
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    int have = 0;
    long guard = 0;
    for (ThingIndex i = dungeon->digger_list_start; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (!thing_is_creature_special_digger(thing))
            continue;
        have++;
        if ((drop == NULL) || !dr.drops || !dr_imp_free(thing))
            continue;
        const int d = abs((int)subtile_slab(thing->mappos.x.stl.num) - fx) + abs((int)subtile_slab(thing->mappos.y.stl.num) - fy);
        if (d <= drop_d + 10)
            continue; /* (put down only when it saves the Imp a walk) */
        if (thing->creature.gold_carried > 0)
        {
            player_add_offmap_gold(PLAYER0, thing->creature.gold_carried);
            dr.gold += thing->creature.gold_carried;
            thing->creature.gold_carried = 0;
        }
        struct Coord3d pos = *drop;
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(thing, &pos);
        set_start_state(thing);
        dr.dropped++;
    }
    for (; have < dr.imps; have++)
    {
        struct Thing *heart = get_player_soul_container(PLAYER0);
        const struct Coord3d *at = (drop != NULL) ? drop : (thing_exists(heart) ? &heart->mappos : NULL);
        if ((at == NULL) || thing_is_invalid(create_owned_special_digger(at->x.val, at->y.val, PLAYER0)))
            break;
        dr.made++;
    }
}

static void dr_turn(void)
{
    static TbBool read = false;
    if (!read)
    {
        read = true;
        const char *v = getenv("KFX_WORLD_DIGRUN");
        dr.drops = 1;
        dr.stop = WC_CIRCLES;
        if ((v != NULL) && (sscanf(v, "%d,%d,%d", &dr.imps, &dr.drops, &dr.stop) >= 1) && (dr.imps > 0))
        {
            dr.on = true;
            SYNCLOG("World: test switch KFX_WORLD_DIGRUN=%s", v);
        }
    }
    if (!dr.on || dr.done || (game.game_kind == GKind_MultiGame) || (game.play_gameturn < 400))
        return;
    const long t = (long)game.play_gameturn;
    if (dr.started == 0)
    {
        struct Thing *heart = get_player_soul_container(PLAYER0);
        if (!thing_exists(heart))
            return;
        int64_t hx, hy;
        world_slab_to_world(subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), &hx, &hy);
        dr.ax = hx;
        dr.ay = hy;
        dr.row = hy;
        dr.started = t;
        dr.circle = 1;
        dr_imps(NULL, 0, 0, 0);
        SYNCLOG("World: DIGRUN: from world slab (%ld,%ld), %d Imps, drops %s", (long)hx, (long)hy, dr.imps, dr.drops ? "on" : "off");
    }
    if ((t % 20) != 0)
        return;
    dr_plan();
    int dug = 0;
    int64_t tx = dr.ax, ty = dr.ay; /* the slab being dug: the first to dig on the way */
    for (int i = 1; (i < dr.plen) && (dug < DR_AHEAD); i++)
    {
        if (dr.kind[dr.path[i]] != 1)
            continue;
        int64_t wx, wy;
        MapSlabCoord ex, ey;
        dr_box_xy(dr.path[i], &wx, &wy);
        if (dug++ == 0)
        {
            tx = wx;
            ty = wy;
        }
        if (!world_engine_slab(wx, wy, &ex, &ey))
            continue;
        for (int k = 0; k < STL_PER_SLB * STL_PER_SLB; k++)
            reveal_map_subtile(slab_subtile(ex, k % STL_PER_SLB), slab_subtile(ey, k / STL_PER_SLB), PLAYER0);
        if (tag_blocks_for_digging_in_area(slab_subtile(ex, 0), slab_subtile(ey, 0), PLAYER0))
            dr.marked++;
    }
    const double d = world_dist_to_nearest_start(dr.ax, dr.ay);
    if (d > dr.best)
        dr.best = d;
    const int circle = world_circle_of_distance(dr.best);
    const long el = t - dr.started;
    if (circle > dr.circle)
    {
        dr.circle = circle;
        SYNCLOG("World: DIGRUN: circle %d reached at %ld turns (%ld:%02ld:%02ld game time), front %.0f slabs out, "
            "%d slabs marked", circle, el, el / 72000, (el / 1200) % 60, (el / 20) % 60, dr.best, dr.marked);
    }
    if ((t % 200) == 0)
    {
        /* a player puts free Imps down on the keeper's ground nearest the slab being dug */
        struct Coord3d drop, *dp = NULL;
        int bestd = 1 << 30;
        for (int dy = -16; dy <= 16; dy++)
            for (int dx = -24; dx <= 24; dx++)
            {
                MapSlabCoord ex, ey;
                if ((abs(dx) + abs(dy) >= bestd) || !world_engine_slab(tx + dx, ty + dy, &ex, &ey))
                    continue;
                const struct SlabMap *slb = get_slabmap_block(ex, ey);
                if ((slabmap_owner(slb) != PLAYER0) || (dr_slab(tx + dx, ty + dy) != 0))
                    continue;
                bestd = abs(dx) + abs(dy);
                drop.x.val = subtile_coord_center(slab_subtile_center(ex));
                drop.y.val = subtile_coord_center(slab_subtile_center(ey));
                drop.z.val = 0;
                dp = &drop;
            }
        MapSlabCoord gx = 0, gy = 0;
        if (!world_engine_slab(tx, ty, &gx, &gy))
            dp = NULL;
        dr_imps(dp, gx, gy, bestd);
    }
    if ((el % 1200) == 0)
    {
        const struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
        SYNCLOG("World: DIGRUN: %ld min: front at world slab (%ld,%ld), %.0f slabs out (best %.0f), circle %d, %d "
            "slabs marked, Imps %d, made %ld, dropped %ld, gold carried back %ld, treasury %ld", el / 1200, (long)dr.ax,
            (long)dr.ay, d, dr.best, circle, dr.marked, (int)dungeon->num_active_diggers, dr.made, dr.dropped,
            dr.gold, (long)dungeon->total_money_owned);
        if (el == 12000)
        {
            for (int i = 0; (i < dr.plen) && (i < 14); i++)
            {
                int64_t wx, wy;
                MapSlabCoord ex = 0, ey = 0;
                dr_box_xy(dr.path[i], &wx, &wy);
                const TbBool here = world_engine_slab(wx, wy, &ex, &ey);
                const struct SlabMap *slb = here ? get_slabmap_block(ex, ey) : NULL;
                const struct Map *mapblk = here ? get_map_block_at(slab_subtile_center(ex), slab_subtile_center(ey)) : NULL;
                SYNCLOG("World: DIGRUN:   way %d: world (%ld,%ld) engine (%d,%d) kind %d slab %d owner %d tagged %d reached %d",
                    i, (long)wx, (long)wy, (int)ex, (int)ey, dr.kind[dr.path[i]], (slb != NULL) ? (int)slb->kind : -1,
                    (slb != NULL) ? (int)slabmap_owner(slb) : -1,
                    (mapblk != NULL) ? (int)(find_from_task_list_by_slab(PLAYER0, ex, ey) >= 0) : -1,
                    dr_reached(dr.path[i]));
            }
            const struct Dungeon *dg = get_players_num_dungeon(PLAYER0);
            long guard = 0;
            for (ThingIndex ti = dg->digger_list_start; (ti > 0) && (guard++ <= (long)THINGS_COUNT);)
            {
                const struct Thing *thing = thing_get(ti);
                const struct CreatureControl *cc = creature_control_get_from_thing(thing);
                if (thing_is_invalid(thing) || creature_control_invalid(cc))
                    break;
                ti = cc->players_next_creature_idx;
                int64_t iwx, iwy;
                world_slab_to_world(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num), &iwx, &iwy);
                SYNCLOG("World: DIGRUN:   Imp %d at world (%ld,%ld), state %d, continue %d, tasks in the list %d", (int)thing->index,
                    (long)iwx, (long)iwy, (int)thing->active_state, (int)thing->continue_state, (int)dg->task_count);
            }
        }
        int counts[4] = {0, 0, 0, 0};
        for (int i = 0; i < DR_N; i++)
            counts[(dr.kind[i] < 0) ? 3 : dr.kind[i]]++;
        SYNCLOG("World: DIGRUN:   way planned %d slabs on from the anchor; the box: %d dug, %d to dig, %d not, %d unloaded; "
            "round the anchor E %d S %d N %d W %d", dr.plen, counts[0], counts[1], counts[2], counts[3],
            dr_slab(dr.ax + 1, dr.ay), dr_slab(dr.ax, dr.ay + 1), dr_slab(dr.ax, dr.ay - 1), dr_slab(dr.ax - 1, dr.ay));
        {
            MapSlabCoord ex, ey;
            if (world_engine_slab(dr.ax + 1, dr.ay, &ex, &ey))
            {
                const struct SlabMap *slb = get_slabmap_block(ex, ey);
                SYNCLOG("World: DIGRUN:   east of the anchor: kind %d owner %d flags %lx", (int)slb->kind, (int)slabmap_owner(slb),
                    (unsigned long)get_slab_kind_stats(slb->kind)->block_flags);
            }
        }
    }
    if (circle >= dr.stop)
    {
        dr.done = true;
        SYNCLOG("World: DIGRUN: done, circle %d at %ld turns (%ld:%02ld:%02ld game time)", circle, el, el / 72000,
            (el / 1200) % 60, (el / 20) % 60);
    }
}

void world_circles_turn(void)
{
    if (!world_mode_active() || world_attract_active())
        return;
    wc_test_read();
    dr_turn();
    if ((wc_test.breach > 1) && ((long)game.play_gameturn == wc_test.breach_turn)
      && (wc_test.breach > world_circles_breached(PLAYER0)))
        wc_breach(PLAYER0, min(wc_test.breach, WC_CIRCLES), "test hook");
    /* thin clients: what each has breached, now and then (a player who joined a moment ago; its map's names) */
    if (netstate_is_server() && ((game.play_gameturn % 600) == 300))
    {
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
            if (world_player_is_human(p))
                wc_tell_client(p, 'Q', world_circles_breached(p));
    }
}

/** The circles the keeper at this machine has breached. */
static int wc_local_breached(void)
{
    if (netstate_is_client())
        return max(1, wc_client_breached);
    return world_circles_breached(my_player_number);
}

const char *world_circles_map_name(int circle)
{
    static char names[WC_CIRCLES + 1][WC_TEXT_MAX];
    if ((circle < 1) || (circle > WC_CIRCLES) || (circle > wc_local_breached()))
        return NULL;
    return wc_text(0, circle, names[circle]);
}

/* The banner's own font: the menus' large one (the game's small font scaled up four times was blocky). It is drawn in
   the menus' palette, which the game doesn't use while playing, so its colours go through that palette. Loaded at
   the first banner; NULL: the small font as before. */
static struct TbSpriteSheet *wc_font = NULL;
static unsigned char wc_font_pal[PALETTE_SIZE];

static struct TbSpriteSheet *wc_big_font(void)
{
    static TbBool tried = false;
    if (tried)
        return wc_font;
    tried = true;
    if (LbFileLoadAt(prepare_file_path(FGrp_LoData, "front.pal"), wc_font_pal) != PALETTE_SIZE)
        return NULL;
#ifdef SPRITE_FORMAT_V2
    wc_font = load_font("ldata/frontft2-64.dat", "ldata/frontft2-64.tab");
#else
    wc_font = load_font("ldata/frontft2.dat", "ldata/frontft2.tab");
#endif
    if (wc_font != NULL)
        LbTextInvalidateFontGeneration();
    return wc_font;
}

/** The font's colours a fade step fainter (0 as is); kind 1: in the red of the name; 2: the game's own colours (the
 *  TrueType text, made in them). Kept: the renderer reads the remap when it draws the frame, later. */
static const unsigned char *wc_remap(int step, int kind)
{
    static unsigned char map[3][WC_FADE_STEPS][256];
    static TbBool made[3][WC_FADE_STEPS];
    if ((step < 0) || (step >= WC_FADE_STEPS) || (engine_palette == NULL) || (kind < 0) || (kind > 2))
        return NULL;
    const int t = kind;
    const TbBool tinted = (kind == 1);
    const unsigned char *font_pal = ((wc_font != NULL) && (kind != 2)) ? wc_font_pal : engine_palette;
    if (!made[t][step])
    {
        const int f = 64 - step * 54 / (WC_FADE_STEPS - 1); /* 64 (as is) down to 10 */
        for (int i = 0; i < 256; i++)
        {
            const unsigned char *c = &font_pal[3 * i];
            int r = c[0], g = c[1], b = c[2];
            if (tinted)
            {
                /* the font's light and dark, in red */
                const int lum = max(r, max(g, b));
                r = lum;
                g = lum * 14 / 64;
                b = lum * 8 / 64;
            }
            map[t][step][i] = LbPaletteFindColour(engine_palette, (unsigned char)(r * f / 64), (unsigned char)(g * f / 64),
                (unsigned char)(b * f / 64));
        }
        made[t][step] = true;
    }
    return map[t][step];
}

/** Text centred at the top of the screen at a strength (0..1000). */
static void wc_draw_text(const char *text, int y, int units, int strength, TbBool tinted)
{
    const int step = (1000 - strength) * WC_FADE_STEPS / 1000;
    if (step >= WC_FADE_STEPS)
        return;
    const int w = LbTextStringWidthM(text, units);
    const unsigned char *old_remap = LbTextGetRemap();
    /* the faintest steps are see-through too */
    RendererSetDrawFlags((step >= WC_FADE_STEPS - 4) ? Lb_SPRITE_TRANSPAR4 : 0);
    LbTextSetRemap(wc_remap(step, tinted));
    LbTextDrawResized((MyScreenWidth - w) / 2, y, units, text);
    LbTextSetRemap(old_remap);
    RendererSetDrawFlags(0);
}

/* The banner in a TrueType font (gui_ttf.c), made at the size it is shown: 0 the name, 1 the line. NULL: no such font
   on this system, the bitmap font as before. Remade when the circle or the size changes. */
static struct TtfText *wc_ttf[2];
static int wc_ttf_circle[2], wc_ttf_px[2];

/** A text of the banner (k 0 the name, 1 the line) at px, smaller where it would be wider than room (measured first:
 *  it is made once). Also run on the thread that makes the next circle's banner ahead: it touches no shared state. */
static struct TtfText *wc_make_fitted(int k, const char *text, int px, int room)
{
    const enum TtfStyle style = (k == 0) ? TtfS_Red : TtfS_Parchment;
    const int tracking = (k == 0) ? 90 : 15;
    const int wide = ttf_text_width(text, px, tracking);
    const int fit = (wide > room) ? (int)((long long)px * room / wide) : px;
    return ttf_text_make(text, fit, style, tracking);
}

/* The next circle's banner, made ahead on a thread of its own while nothing is shown (the user, 2026-11-28: crossing
   a circle stood the game still while its banner was made): when the keeper crosses into it, it only has to be shown.
   Made again when the screen's size changes; taken only for the circle and the sizes it was made for. */
struct WcPre {
    int circle;              /* 0: none */
    int px[2], room;
    TbBool gpu;              /* made in true colour (OpenGL/Vulkan), else the palette version */
    char text[2][WC_TEXT_MAX];
    struct TtfText *made[2];
    struct TtfRgba *rgba[2];
};
static struct WcPre wc_pre;
static SDL_Thread *wc_pre_thread = NULL;

/** The true-colour version of wc_make_fitted. */
static struct TtfRgba *wc_make_fitted_rgba(int k, const char *text, int px, int room)
{
    const enum TtfStyle style = (k == 0) ? TtfS_Red : TtfS_Parchment;
    const int tracking = (k == 0) ? 90 : 15;
    const int wide = ttf_text_width(text, px, tracking);
    const int fit = (wide > room) ? (int)((long long)px * room / wide) : px;
    return ttf_text_make_rgba(text, fit, style, tracking);
}

static int SDLCALL wc_pre_run(void *data)
{
    struct WcPre *pre = (struct WcPre *)data;
    for (int k = 0; k < 2; k++)
    {
        if (pre->gpu)
            pre->rgba[k] = wc_make_fitted_rgba(k, pre->text[k], pre->px[k], pre->room);
        else
            pre->made[k] = wc_make_fitted(k, pre->text[k], pre->px[k], pre->room);
    }
    return 0;
}

/** The thread's making finished (waits for it: by the time it is asked, it has long been done). */
static void wc_pre_join(void)
{
    if (wc_pre_thread == NULL)
        return;
    SDL_WaitThread(wc_pre_thread, NULL);
    wc_pre_thread = NULL;
}

static void wc_pre_drop(void)
{
    wc_pre_join();
    for (int k = 0; k < 2; k++)
    {
        ttf_text_free(wc_pre.made[k]);
        wc_pre.made[k] = NULL;
        ttf_rgba_free(wc_pre.rgba[k]);
        wc_pre.rgba[k] = NULL;
    }
    wc_pre.circle = 0;
}

static const struct TtfText *wc_ttf_get(int k, int circle, const char *text, int px)
{
    if ((wc_ttf_circle[k] != circle) || (wc_ttf_px[k] != px))
    {
        const TbClockMSec made_from = LbTimerClock();
        ttf_text_free(wc_ttf[k]);
        wc_ttf[k] = NULL;
        const int room = MyScreenWidth * 94 / 100;
        wc_pre_join();
        const TbBool ahead = (wc_pre.circle == circle) && (wc_pre.px[k] == px) && (wc_pre.room == room) &&
            (wc_pre.made[k] != NULL) && (strcmp(wc_pre.text[k], text) == 0);
        if (ahead)
        {
            wc_ttf[k] = wc_pre.made[k];
            wc_pre.made[k] = NULL;
        } else
            wc_ttf[k] = wc_make_fitted(k, text, px, room);
        wc_ttf_circle[k] = circle;
        wc_ttf_px[k] = px;
        /* (the log names the circle by its number only: the names are the players' to find) */
        SYNCLOG("World circles: banner %s of circle %d %s at %d px in %ld ms (%dx%d)", (k == 0) ? "name" : "line", circle,
            ahead ? "made ahead, shown" : "made", px, (long)(LbTimerClock() - made_from),
            (wc_ttf[k] != NULL) ? wc_ttf[k]->w : 0, (wc_ttf[k] != NULL) ? wc_ttf[k]->h : 0);
    }
    return wc_ttf[k];
}

/** The banner's sizes on this screen: the bitmap font's units, and the letters' heights in pixels. */
static void wc_banner_sizes(int *name_units, int *line_units, int *name_h, int *line_h)
{
    LbTextSetFont(winfont);
    /* (40% smaller than the first banner's four and two times: the user, 2026-11-28: "too big") */
    *name_units = units_per_pixel * 24 / 10;
    *line_units = units_per_pixel * 12 / 10;
    *name_h = LbTextLineHeight() * *name_units / 16;
    *line_h = LbTextLineHeight() * *line_units / 16;
}

/** While no banner is shown: the next circle's banner made ahead, when it isn't yet (or the screen's size changed). */
static void wc_pre_make_next(void)
{
    if (wc_pre_thread != NULL)
        return;
    const int next = wc_local_breached() + 1;
    if ((next > WC_CIRCLES) || (engine_palette == NULL) || !ttf_available()) /* (ttf_available loads the font here) */
        return;
    int name_units, line_units, name_h, line_h;
    wc_banner_sizes(&name_units, &line_units, &name_h, &line_h);
    const int px0 = name_h * 11 / 10, px1 = line_h * 11 / 10, room = MyScreenWidth * 94 / 100;
    const TbBool gpu = RendererIsGPUBackend();
    if ((wc_pre.circle == next) && (wc_pre.px[0] == px0) && (wc_pre.px[1] == px1) && (wc_pre.room == room) && (wc_pre.gpu == gpu))
        return;
    wc_pre_drop();
    wc_pre.circle = next;
    wc_pre.px[0] = px0;
    wc_pre.px[1] = px1;
    wc_pre.room = room;
    wc_pre.gpu = gpu;
    wc_text(0, next, wc_pre.text[0]);
    wc_text(1, next, wc_pre.text[1]);
    for (char *c = wc_pre.text[0]; *c != 0; c++)
        *c = (char)toupper((unsigned char)*c);
    wc_pre_thread = SDL_CreateThread(wc_pre_run, "circle_banner", &wc_pre);
    if (wc_pre_thread == NULL)
        wc_pre.circle = 0; /* (no thread: it is made when it is shown, as before) */
}

/** TrueType text centred at the top of the screen, its letters' top at y, at a strength (0..1000) */
static void wc_draw_ttf(const struct TtfText *t, int y, int strength)
{
    const int step = (1000 - strength) * WC_FADE_STEPS / 1000;
    if (step >= WC_FADE_STEPS)
        return;
    ttf_text_draw(t, (MyScreenWidth - t->w) / 2, y - t->pad, (step > 0) ? wc_remap(step, 2) : NULL,
        (step >= WC_FADE_STEPS - 4));
}

/* The banner in true colour on OpenGL and Vulkan (the user, 2026-11-28: "the text transition is rough"): its two texts
   as pictures with no palette steps, put together into one picture (the main menu's second slot: free in a game) with
   each text's strength as its opacity, in 64 steps - the palette version fades in 10 colour steps and its last ones
   are see-through at a stroke. The picture is put together again only when a step changes; in between the renderer
   keeps its copy. */
static struct TtfRgba *wc_rgba[2];
static int wc_rgba_circle[2], wc_rgba_px[2];
static unsigned char *wc_pic = NULL;
static size_t wc_pic_cap = 0;
static uint32_t wc_pic_gen = 0;
static int wc_pic_key[7];

static const struct TtfRgba *wc_rgba_get(int k, int circle, const char *text, int px)
{
    if ((wc_rgba_circle[k] != circle) || (wc_rgba_px[k] != px))
    {
        const TbClockMSec made_from = LbTimerClock();
        ttf_rgba_free(wc_rgba[k]);
        wc_rgba[k] = NULL;
        const int room = MyScreenWidth * 94 / 100;
        wc_pre_join();
        const TbBool ahead = wc_pre.gpu && (wc_pre.circle == circle) && (wc_pre.px[k] == px) && (wc_pre.room == room) &&
            (wc_pre.rgba[k] != NULL) && (strcmp(wc_pre.text[k], text) == 0);
        if (ahead)
        {
            wc_rgba[k] = wc_pre.rgba[k];
            wc_pre.rgba[k] = NULL;
        } else
            wc_rgba[k] = wc_make_fitted_rgba(k, text, px, room);
        wc_rgba_circle[k] = circle;
        wc_rgba_px[k] = px;
        SYNCLOG("World circles: banner %s of circle %d in true colour %s at %d px in %ld ms (%dx%d)", (k == 0) ? "name" : "line",
            circle, ahead ? "made ahead, shown" : "made", px, (long)(LbTimerClock() - made_from),
            (wc_rgba[k] != NULL) ? wc_rgba[k]->w : 0, (wc_rgba[k] != NULL) ? wc_rgba[k]->h : 0);
    }
    return wc_rgba[k];
}

/** src at q/64 of its strength over dst (premultiplied, dw wide), its top-left at dx, dy. */
static void wc_pic_over(unsigned char *dst, int dw, int dx, int dy, const struct TtfRgba *src, int q)
{
    for (int y = 0; y < src->h; y++)
    {
        unsigned char *d = dst + ((size_t)(dy + y) * dw + dx) * 4;
        const unsigned char *sp = src->px + (size_t)y * src->w * 4;
        for (int x = 0; x < src->w; x++, d += 4, sp += 4)
        {
            const int sa = sp[3] * q / 64;
            if (sa == 0)
                continue;
            const int inv = 255 - sa;
            for (int k = 0; k < 3; k++)
                d[k] = (unsigned char)min(255, sp[k] * q / 64 + d[k] * inv / 255);
            d[3] = (unsigned char)min(255, sa + d[3] * inv / 255);
        }
    }
}

/** The banner as one true-colour picture; false: no such font or no GPU renderer (the palette version is drawn). */
static TbBool wc_draw_true_colour(int circle, const char *name, const char *line, int name_h, int line_h, int y,
    int name_s, int line_s)
{
    const struct TtfRgba *a = wc_rgba_get(0, circle, name, name_h * 11 / 10);
    const struct TtfRgba *b = wc_rgba_get(1, circle, line, line_h * 11 / 10);
    if ((a == NULL) || (b == NULL))
        return false;
    const int q0 = max(0, min(64, name_s * 64 / 1000)), q1 = max(0, min(64, line_s * 64 / 1000));
    if ((q0 == 0) && (q1 == 0))
        return true;
    /* where the palette version puts them: each centred, the line under the name's letters */
    const int top0 = y - a->pad;
    const int top1 = y + (a->h - 2 * a->pad) + name_h / 10 - b->pad;
    const int top = min(top0, top1), bottom = max(top0 + a->h, top1 + b->h);
    const int pw = max(a->w, b->w), ph = bottom - top;
    const int key[7] = {circle, q0, q1, pw, ph, a->w * 4096 + a->h, b->w * 4096 + b->h};
    if ((wc_pic == NULL) || (memcmp(key, wc_pic_key, sizeof(key)) != 0))
    {
        const size_t need = (size_t)pw * ph * 4;
        if (need > wc_pic_cap)
        {
            unsigned char *pic = (unsigned char *)realloc(wc_pic, need);
            if (pic == NULL)
                return false;
            wc_pic = pic;
            wc_pic_cap = need;
        }
        memset(wc_pic, 0, need);
        if (q0 > 0)
            wc_pic_over(wc_pic, pw, (pw - a->w) / 2, top0 - top, a, q0);
        if (q1 > 0)
            wc_pic_over(wc_pic, pw, (pw - b->w) / 2, top1 - top, b, q1);
        memcpy(wc_pic_key, key, sizeof(key));
        wc_pic_gen++;
    }
    return RendererSubmitUIImage(UmSl_Front2, (MyScreenWidth - pw) / 2, top, pw, ph, wc_pic, wc_pic_gen);
}

void world_circles_draw(void)
{
    if (!world_mode_active() || world_attract_active())
    {
        wc_banner_circle = 0;
        return;
    }
    if (wc_banner_circle == 0)
    {
        wc_pre_make_next();
        return;
    }
    wc_test_read();
    const long t = wc_test.turns ? ((long)game.play_gameturn - wc_banner_from_turn) * 50
        : (long)(LbTimerClock() - wc_banner_from);
    const long full = WC_LINE_MS + WC_FADE_MS;         /* both in full */
    const long out = full + WC_HOLD_MS;                /* the fade out begins */
    if ((t < 0) || (t >= out + WC_FADE_MS))
    {
        wc_banner_circle = 0;
        return;
    }
    int name_s = (int)min(1000L, t * 1000 / WC_FADE_MS);
    int line_s = (int)max(0L, min(1000L, (t - WC_LINE_MS) * 1000 / WC_FADE_MS));
    if (t > out)
    {
        const int left = (int)(1000 - (t - out) * 1000 / WC_FADE_MS);
        name_s = min(name_s, left);
        line_s = min(line_s, left);
    }
    char name[WC_TEXT_MAX], line[WC_TEXT_MAX];
    wc_text(0, wc_banner_circle, name);
    wc_text(1, wc_banner_circle, line);
    for (char *c = name; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);
    /* the sizes are those of the small font at 2.4 and 1.2 times; the large font is scaled to the same heights */
    int name_units, line_units, name_h, line_h;
    wc_banner_sizes(&name_units, &line_units, &name_h, &line_h);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    const int y = MyScreenHeight * 7 / 100;
    if (RendererIsGPUBackend() && wc_draw_true_colour(wc_banner_circle, name, line, name_h, line_h, y, name_s, line_s))
        return;
    const struct TtfText *name_t = wc_ttf_get(0, wc_banner_circle, name, name_h * 11 / 10);
    const struct TtfText *line_t = wc_ttf_get(1, wc_banner_circle, line, line_h * 11 / 10);
    if ((name_t != NULL) && (line_t != NULL))
    {
        wc_draw_ttf(name_t, y, name_s);
        wc_draw_ttf(line_t, y + (name_t->h - 2 * name_t->pad) + name_h / 10, line_s);
        return;
    }
    if (wc_big_font() != NULL)
    {
        const int small_h = max(LbTextLineHeight(), 1);
        LbTextSetFont(wc_font);
        const int big_h = max(LbTextLineHeight(), 1);
        name_units = max(name_units * small_h / big_h, 1);
        line_units = max(line_units * small_h / big_h, 1);
    }
    wc_draw_text(name, y, name_units, name_s, true);
    wc_draw_text(line, y + name_h + name_h / 8, line_units, line_s, false);
}

/******************************************************************************/
/* The save (the CIR1 extra): a count, then the highest circle breached by each player */

void world_circles_reset(void)
{
    wc_pre_drop();
    memset(wc_breached, 0, sizeof(wc_breached));
    wc_client_breached = 0;
    wc_banner_circle = 0;
}

size_t world_circles_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (wc_breached[p] > 1)
            return 1 + (size_t)PLAYERS_MAX;
    return 0;
}

void world_circles_save(uint8_t *buf)
{
    buf[0] = (uint8_t)PLAYERS_MAX;
    memcpy(buf + 1, wc_breached, PLAYERS_MAX);
}

TbBool world_circles_load(const uint8_t *buf, size_t len)
{
    world_circles_reset();
    if (len < 1)
        return true;
    const size_t n = min((size_t)buf[0], min(len - 1, (size_t)PLAYERS_MAX));
    for (size_t p = 0; p < n; p++)
        wc_breached[p] = (uint8_t)min((int)buf[1 + p], WC_CIRCLES);
    for (size_t p = 0; p < n; p++)
        if (wc_breached[p] > 1)
            SYNCLOG("World circles: loaded: player %d has breached circle %d", (int)p, (int)wc_breached[p]);
    return true;
}

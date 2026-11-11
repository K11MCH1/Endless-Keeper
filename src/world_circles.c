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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
#include "thing_stats.h"
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

void world_circles_turn(void)
{
    if (!world_mode_active() || world_attract_active())
        return;
    wc_test_read();
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

/** The font's colours a fade step fainter (0 as is); tinted: in the red of the name. Kept: the renderer reads the
 *  remap when it draws the frame, later. */
static const unsigned char *wc_remap(int step, TbBool tinted)
{
    static unsigned char map[2][WC_FADE_STEPS][256];
    static TbBool made[2][WC_FADE_STEPS];
    if ((step < 0) || (step >= WC_FADE_STEPS) || (engine_palette == NULL))
        return NULL;
    const int t = tinted ? 1 : 0;
    const unsigned char *font_pal = (wc_font != NULL) ? wc_font_pal : engine_palette;
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

void world_circles_draw(void)
{
    if (wc_banner_circle == 0)
        return;
    if (!world_mode_active() || world_attract_active())
    {
        wc_banner_circle = 0;
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
    /* the sizes are those of the small font at four and two times; the large font is scaled to the same heights */
    LbTextSetFont(winfont);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    int name_units = units_per_pixel * 4;
    int line_units = units_per_pixel * 2;
    const int y = MyScreenHeight * 7 / 100;
    const int name_h = LbTextLineHeight() * name_units / 16;
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

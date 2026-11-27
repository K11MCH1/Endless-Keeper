/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_daynight.c
 *     Endless world: day and night, the day/night icon, and the heroes who break in by day.
 *
 * Time follows the game turn: a 20-minute cycle (at 20 turns a second) of 12 minutes of day and 8 of night,
 * starting at sunrise. Nothing needs saving: the day number, the time of day and the day's raid times all come
 * from game.play_gameturn and the world seed.
 *
 * By day, heroes break in through the ceiling (the FALL spawn of scripts) at open spots about 60 slabs from the
 * player's heart (2026-11-14, the player: it was 100; never nearer than 40): parties of a Tunneller that digs its way to the heart and its followers (2-6 in all). The first
 * raid of a day always comes, the day's others one time in two (2026-11-11), at least four minutes apart, up to the
 * day's end (the last two minutes were kept free 2026-11-18 to 2026-11-19). A raid is bought with a threat budget (2026-11-18; it was the player's best creature):
 * the army's fighting power (world_threat.c) x a curve of the dungeon's days x an adjustment from how the last raids
 * went, growing at most 25% a raid; each hero costs its fighting power. At most 2 parties before day 8, then 3, the
 * third landing 45-90 seconds after the others. The army's upper quarter of levels sets the leader's level and who may
 * follow (Thieves and Tunnellers, then Dwarves, Archers, Barbarians, Monks, and from level 9 the heroes of the landing's
 * biome); the followers come between its middle and upper quarter.
 * A raid's heroes stay until the next dawn, then walk back to where they landed and leave, and a party whose leader
 * dies goes home at once (2026-11-11; it was the dawn after their third night); no new raid comes for a player while
 * the last one's heroes are still there (the invasions are saved: RDS1).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_daynight.h"
#include "world_attract.h"
#include "world_daynight_art.h"
#include "world_stream.h"
#include "world_threat.h"
#include "gui_draw.h"
#include "custom_sprites.h"
#include "sprites.h"
#include "world_gen.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "bflib_basics.h"
#include "bflib_math.h"
#include "bflib_planar.h"
#include "bflib_sprite.h"
#include "bflib_video.h"
#include "config.h"
#include "config_creature.h"
#include "config_crtrstates.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "creature_control.h"
#include "creature_groups.h"
#include "creature_states.h"
#include "creature_states_hero.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "map_blocks.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_doors.h"
#include "thing_effects.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "world_menu.h"
#include "world_pages.h"
#include "net_game.h"
#include "world_boss.h"
#include "world_truce.h"
#include "world_defectors.h"
#include "world_crusade.h"
#include "world_alarm.h"
#include "world_progress.h"
#include "server_admin.h"
#include "world_seasons.h"
#include "world_creature_window.h"
#include "vidfade.h"
#include "world_wallbreak.h"
#include "ui_safe_area.h"
#include "world_side_panel.h"
#include "world_pentagram.h"
#include "world_caretaker.h"
#include "post_inc.h"

/******************************************************************************/
/* The clock */

#define DN_DAY_TURNS_DEFAULT   (12 * 60 * 20)
#define DN_NIGHT_TURNS_DEFAULT (8 * 60 * 20)

static GameTurnDelta dn_day_turns = DN_DAY_TURNS_DEFAULT;
static GameTurnDelta dn_night_turns = DN_NIGHT_TURNS_DEFAULT;
/* each possible raid of a day happens with this chance, in percent (KFX_WORLD_RAIDCHANCE changes it for tests) */
#define DN_RAID_CHANCE 50
static int dn_raid_chance = DN_RAID_CHANCE;
/** Set on the first turn of each day after the first: autosave once the turn is over. */
static TbBool dn_dawn_save;

/** Test hook: KFX_WORLD_DAYNIGHT="600,400" sets the day and night lengths in turns (only read in world levels). */
static void dn_read_lengths(void)
{
    static int read = 0;
    if (read)
        return;
    read = 1;
    const char *e = getenv("KFX_WORLD_DAYNIGHT");
    long d = 0, n = 0;
    if ((e != NULL) && (sscanf(e, "%ld,%ld", &d, &n) == 2) && (d > 0) && (n > 0))
    {
        dn_day_turns = d;
        dn_night_turns = n;
        SYNCLOG("World: test day/night lengths %ld and %ld turns", d, n);
    }
    const char *c = getenv("KFX_WORLD_RAIDCHANCE");
    if ((c != NULL) && (atoi(c) >= 0) && (atoi(c) <= 100))
    {
        dn_raid_chance = atoi(c);
        SYNCLOG("World: test raid chance %d%%", dn_raid_chance);
    }
}

static void dn_time(GameTurn turn, long *day, GameTurnDelta *t)
{
    const GameTurnDelta cycle = dn_day_turns + dn_night_turns;
    *day = (long)(turn / cycle) + 1;
    *t = (GameTurnDelta)(turn % cycle);
}

long world_day_number(void)
{
    long day;
    GameTurnDelta t;
    dn_read_lengths();
    dn_time(game.play_gameturn, &day, &t);
    return day;
}

/* Every third day, and its night, nothing comes: no raid, no wild pack (the user, 2026-11-19: "Every 3 days a night and
   a day with no enemy spawns. So no Wild packs, no heroes. To give player a breather"). The world's day as the dial
   shows it (days 3, 6, 9 ...), the same for every keeper. Test switch KFX_WORLD_SAFEEVERY=n: every n-th day (0: never). */
#define DN_SAFE_EVERY 3
static TbBool dn_safe_day_at(long day);
TbBool world_safe_day(void)
{
    return dn_safe_day_at(world_day_number());
}

/** Is that world day a safe day (every third)? */
static TbBool dn_safe_day_at(long day)
{
    static long every = -2;
    if (every == -2)
    {
        const char *v = getenv("KFX_WORLD_SAFEEVERY");
        every = (v != NULL) ? max(0L, atol(v)) : DN_SAFE_EVERY;
        if (v != NULL)
            SYNCLOG("World: test switch KFX_WORLD_SAFEEVERY=%ld", every);
    }
    if ((every <= 0) || !world_mode_active())
        return false;
    return (day % every) == 0;
}

GameTurn world_daylight_began(void)
{
    dn_read_lengths();
    const GameTurnDelta cycle = dn_day_turns + dn_night_turns;
    return (GameTurn)((game.play_gameturn / cycle) * cycle);
}

long world_day_turns(void)
{
    dn_read_lengths();
    return (long)(dn_day_turns + dn_night_turns);
}

/** Seasons Below: a day's daylight (the Long Night swaps the lengths of day and night). */
static GameTurnDelta dn_light(long day)
{
    return world_season_long_night(day) ? dn_night_turns : dn_day_turns;
}

TbBool world_is_day(void)
{
    long day;
    GameTurnDelta t;
    dn_read_lengths();
    dn_time(game.play_gameturn, &day, &t);
    return t < dn_light(day);
}

/** The disc's turn, clockwise in radians: -pi/2 at sunrise (sun on the left), 0 at noon (sun on top),
 *  pi/2 at sunset, pi at midnight (moon on top). */
static double dn_disc_angle(void)
{
    long day;
    GameTurnDelta t;
    dn_time(game.play_gameturn, &day, &t);
    const GameTurnDelta light = dn_light(day), dark = dn_day_turns + dn_night_turns - light;
    if (t < light)
        return M_PI * (-0.5 + (double)t / (double)light);
    return M_PI * (0.5 + (double)(t - light) / (double)dark);
}

/******************************************************************************/
/* The icon: a disc (day half with the sun, night half with the moon) turning inside a fixed rim */

#define DN_FRAMES 128

static unsigned char dn_src[DAYNIGHT_SRC * DAYNIGHT_SRC];
static unsigned char dn_rim_px[DAYNIGHT_RIM * DAYNIGHT_RIM];
static TbBool dn_art_ready = false;
/* frames are made when first needed and kept for the whole run: the renderer keeps a handle per sprite */
static struct TbSprite dn_frame_spr[DN_FRAMES];
static unsigned char *dn_frame_data[DN_FRAMES];
static struct TbSprite dn_rim_spr;
static unsigned char *dn_rim_data = NULL;

static void dn_unrle(const unsigned char *rle, size_t len, unsigned char *out, int side)
{
    size_t p = 0, total = (size_t)side * side;
    for (size_t i = 0; i + 1 < len && p < total; i += 2)
        for (int k = 0; (k < rle[i]) && (p < total); k++)
            out[p++] = rle[i + 1];
    while (p < total)
        out[p++] = 0;
}

/** Palette-index image (0 transparent) to the sprite format: per row, n>0 then n pixels, -n to skip, 0 ends it. */
static unsigned char *dn_encode_sprite(const unsigned char *px, int w, int h)
{
    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 2 + h + 16);
    if (buf == NULL)
        return NULL;
    size_t o = 0;
    for (int y = 0; y < h; y++)
    {
        const unsigned char *row = px + y * w;
        int x = 0;
        while (x < w)
        {
            int n = 0;
            if (row[x] == 0)
            {
                while ((x + n < w) && (row[x + n] == 0) && (n < 127))
                    n++;
                if (x + n < w)
                    buf[o++] = (unsigned char)(-n);
            } else
            {
                while ((x + n < w) && (row[x + n] != 0) && (n < 127))
                    n++;
                buf[o++] = (unsigned char)n;
                memcpy(buf + o, row + x, (size_t)n);
                o += (size_t)n;
            }
            x += n;
        }
        buf[o++] = 0;
    }
    return buf;
}

static void dn_prepare_art(void)
{
    if (dn_art_ready)
        return;
    dn_unrle(daynight_disc_rle, sizeof(daynight_disc_rle), dn_src, DAYNIGHT_SRC);
    dn_unrle(daynight_rim_rle, sizeof(daynight_rim_rle), dn_rim_px, DAYNIGHT_RIM);
    dn_rim_data = dn_encode_sprite(dn_rim_px, DAYNIGHT_RIM, DAYNIGHT_RIM);
    dn_rim_spr.Data = dn_rim_data;
    dn_rim_spr.SWidth = DAYNIGHT_RIM;
    dn_rim_spr.SHeight = DAYNIGHT_RIM;
    dn_art_ready = true;
}

/** Frame k: the disc turned clockwise by 2*pi*k/DN_FRAMES. Each shown pixel takes the most common colour of
 *  4x4 samples of the 4x source, so edges stay clean pixel art (as daynight_icon.py's preview). */
static void dn_make_frame(int k)
{
    unsigned char px[DAYNIGHT_DISC * DAYNIGHT_DISC];
    const double theta = 2.0 * M_PI * k / DN_FRAMES;
    const double c = cos(theta), s = sin(theta);
    const double half = DAYNIGHT_DISC / 2.0;
    const double scale = (double)DAYNIGHT_SRC / DAYNIGHT_DISC;
    memset(px, 0, sizeof(px));
    for (int y = 0; y < DAYNIGHT_DISC; y++)
    {
        for (int x = 0; x < DAYNIGHT_DISC; x++)
        {
            const double cx = x + 0.5 - half, cy = y + 0.5 - half;
            if (cx * cx + cy * cy > half * half)
                continue;
            unsigned char cols[16];
            int counts[16];
            int ncols = 0;
            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    const double dx = x + (i + 0.5) / 4.0 - half, dy = y + (j + 0.5) / 4.0 - half;
                    const double sx = c * dx + s * dy, sy = -s * dx + c * dy;
                    const int ix = (int)((sx + half) * scale), iy = (int)((sy + half) * scale);
                    if ((ix < 0) || (iy < 0) || (ix >= DAYNIGHT_SRC) || (iy >= DAYNIGHT_SRC))
                        continue;
                    const unsigned char col = dn_src[iy * DAYNIGHT_SRC + ix];
                    if (col == 0)
                        continue;
                    int m = 0;
                    while ((m < ncols) && (cols[m] != col))
                        m++;
                    if (m == ncols)
                    {
                        cols[ncols] = col;
                        counts[ncols++] = 0;
                    }
                    counts[m]++;
                }
            }
            int best = -1;
            for (int m = 0; m < ncols; m++)
                if ((best < 0) || (counts[m] > counts[best]))
                    best = m;
            if (best >= 0)
                px[y * DAYNIGHT_DISC + x] = cols[best];
        }
    }
    dn_frame_data[k] = dn_encode_sprite(px, DAYNIGHT_DISC, DAYNIGHT_DISC);
    dn_frame_spr[k].Data = dn_frame_data[k];
    dn_frame_spr[k].SWidth = DAYNIGHT_DISC;
    dn_frame_spr[k].SHeight = DAYNIGHT_DISC;
}

/** The day's number under the dial (the user, 2026-11-15: "a day counter under the dial like "Day 1""). It is the
 *  world's day, which the clock raises at each dawn; while the dial shows the night it says "Night X", the night
 *  after day X. The game's
 *  plain font, light with a dark edge so that it reads over any ground, centred on the dial. Its letters are a whole
 *  multiple of the font's own (even strokes on every screen) and never smaller than the font: a number too long to
 *  stand centred (a server's world reaches four figures in two weeks) moves left, off the screen's edge, instead.
 *  (x, y): the dial's left end and its lower edge. Returns how far below y the next thing can begin: the letters and
 *  the room the 'y' hangs into. */
static int dn_draw_day(int x, int y, int size)
{
    const int upp = units_per_pixel;
    /* test hook: KFX_WORLD_DAYLABEL=12345 shows that number */
    static long shown = -2;
    if (shown == -2)
    {
        const char *e = getenv("KFX_WORLD_DAYLABEL");
        shown = (e != NULL) ? atol(e) : -1;
    }
    char text[64];
    /* as the dial shows it (the user, 2026-11-15): "Day X" while the sun is up, "Night X" while the moon is */
    snprintf(text, sizeof(text), "%s %ld", world_is_day() ? "Day" : "Night", (shown > 0) ? shown : world_day_number());
    {
        /* endless (2026-11-27): the Pentagram's Omen: by night, tomorrow's raids */
        char omen[32];
        const size_t len = strlen(text);
        if (world_pentagram_omen_text(my_player_number, omen, sizeof(omen)))
            snprintf(text + len, sizeof(text) - len, " - %s", omen);
    }
    const int own = max(world_window_plain_cap(), 1);
    const int cap = own * max((6 * upp / 16 + own / 2) / own, 1);
    const int gap = max(3 * upp / 16, 2);
    const int edge = max((cap + 8) / 16, 1);
    const int wide = world_window_plain_width(text, cap);
    const int air = edge + max(upp / 16, 1); /* the dark edge, and a unit between it and the screen's edge */
    const int tx = min(x + size / 2 - wide / 2, ui_safe_right_edge() - air - wide), ty = y + gap;
    const unsigned char dark = colours[0][0][0];
    /* (the room given is the text's own width and a little: it is never made smaller to fit) */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if ((dx != 0) || (dy != 0))
                world_window_plain(tx + dx * edge, ty + dy * edge, wide + 4, cap, 0, dark, text);
    /* a safe day and its night (no raids, no wild packs): the text is green (the user, 2026-11-19) */
    world_window_plain(tx, ty, wide + 4, cap, 0, world_safe_day() ? colours[4][15][4] : world_window_ink(WwI_Bright), text);
    return gap + cap + (3 * cap + 7) / 8;
}

void world_daynight_draw(void)
{
    {
        /* test switch KFX_WORLD_FLOWERTEST=1: a row of the level marks the Hand's list draws (levels 1, 4, 10, 11, 12,
           25, 100) under the dial, for a screenshot (2026-11-19) */
        static int on = -1;
        if (on < 0)
            on = (getenv("KFX_WORLD_FLOWERTEST") != NULL) ? 1 : 0;
        if (on)
        {
            static const int lv[7] = {1, 4, 10, 11, 12, 25, 100};
            const int upp = max(16, units_per_pixel_ui);
            for (int k = 0; k < 7; k++)
                draw_level_flower_left(MyScreenWidth / 3 + k * 40 * upp / 16, MyScreenHeight / 5, upp, lv[k] - 1);
        }
    }
    if (world_attract_active())
        return; /* the launcher's background: the picture alone */
    if (!world_mode_active())
        return;
    /* (not while a creature is taken over: the user, 2026-11-27; world_boss_draw is still asked, so the strip's
       button goes with it) */
    const unsigned char view = get_user_view_type(get_my_user());
    if ((view == PVT_CreatureContrl) || (view == PVT_CreaturePasngr))
    {
        world_boss_draw(0, -1, -1);
        return;
    }
    dn_read_lengths();
    dn_prepare_art();
    if (dn_rim_data == NULL)
        return;
    double a = dn_disc_angle() / (2.0 * M_PI);
    a -= floor(a);
    const int k = ((int)floor(a * DN_FRAMES + 0.5)) % DN_FRAMES;
    if (dn_frame_data[k] == NULL)
        dn_make_frame(k);
    if (dn_frame_data[k] == NULL)
        return;
    if (world_side_hud_modern())
    {
        /* the modern look: the dial in a gold ring with the year's days on it, the World Boss strip under it */
        world_boss_draw(0, world_side_dial_draw(&dn_frame_spr[k], k), 0);
        return;
    }
    const int upp = units_per_pixel;
    const int size = DAYNIGHT_RIM * upp / 16;
    const int margin = 6 * upp / 16;
    const int x = ui_safe_right_edge() - size - margin; /* (inside the safe area: rounded corners, cutouts) */
    const int y = ui_safe_top + margin;
    const int off = ((DAYNIGHT_RIM - DAYNIGHT_DISC) / 2) * upp / 16;
    UIRenderer_SubmitPanelSpriteRaw(x + off, y + off, upp, &dn_frame_spr[k], 0);
    UIRenderer_SubmitPanelSpriteRaw(x, y, upp, &dn_rim_spr, 0);
    world_seasons_draw_ring(x, y, size); /* the year round the dial */
    const int day_h = dn_draw_day(x, y + size, size);
    /* the World Boss timer, under the day's number: three units clear of it, as a button under a line is */
    world_boss_draw(x, y + size + day_h + 3 * upp / 16, size);
}

/******************************************************************************/
/* Raids by day */


struct DnHero {
    const char *name;
    int weight;
};

/* Raid tiers by the player's best creature level (2026-11-08): how many parties land at once, and who follows each
 * party's Tunneller (Tunnellers dig, so the others can follow anywhere). Each tier adds a hero to the ones before;
 * from level 9 the heroes of the landing's biome come along too. */
struct DnTier {
    int min_level;
    int parties_min, parties_max;
    struct DnHero add;          /* the hero this tier adds to the followers */
    int biome_weight;           /* the weight of the landing biome's heroes (0: not in this tier) */
};
static const struct DnTier dn_tiers[] = {
    {1, 1, 1, {"THIEF", 3}, 0},
    {4, 1, 2, {"DWARFA", 4}, 0},
    {6, 1, 3, {"ARCHER", 3}, 0},
    {9, 2, 4, {"BARBARIAN", 3}, 2},
    {13, 3, 5, {"MONK", 2}, 3},
};
#define DN_TIERS ((int)(sizeof(dn_tiers) / sizeof(dn_tiers[0])))
/* Tunnellers follow in every tier */
static const struct DnHero dn_tunneller_follower = {"TUNNELLER", 2};

/* The heroes each biome adds in the last two tiers */
static const char *const dn_biome_heroes[WgB_COUNT][2] = {
    [WgB_Earthen] = {"KNIGHT", NULL},
    [WgB_Rocky] = {"GIANT", NULL},
    [WgB_Volcanic] = {"WIZARD", NULL},
    [WgB_Flooded] = {"FAIRY", NULL},
    [WgB_Frozen] = {"WITCH", NULL},
    [WgB_Sandstone] = {"SAMURAI", NULL},
    [WgB_Crystal] = {"WIZARD", "FAIRY"},
    [WgB_Ruins] = {"KNIGHT", "SAMURAI"},
    [WgB_LavaOcean] = {"WIZARD", NULL},
    [WgB_FloodedOcean] = {"FAIRY", NULL},
    [WgB_LavaRivers] = {"WIZARD", NULL},
    [WgB_FloodedRivers] = {"FAIRY", NULL},
    [WgB_DeadMans] = {"KNIGHT", "GIANT"},
};

#define DN_SPAWN_DIST 60             /* slabs from the heart a raid lands at, about (2026-11-14, the player: "about 60
                                        slabs from the dungeon heart (still never nearer than 40)"; it was 100) */
#define DN_SPAWN_SLACK 5
#define DN_SPAWN_MIN_DIST 40
#define DN_SPAWN_REACH 105           /* the ground looked at for a landing, round the heart: as far as it always was, so
                                        a dungeon with no open ground near still gets its raid from further out */
#define DN_PARTY_GAP 8               /* slabs at least between two parties' landings */
#define DN_MAX_PARTIES 5
#define DN_RETREAT_NIGHTS 1          /* raiders go home at the next dawn (2026-11-11; it was after their third night) */
#define DN_NIGHT_CLEAR 0 /* (the last two minutes of a day were kept free of raids till 2026-11-19: dn_night_clear) */
#define DN_RAID_GAP (4 * 60 * 20)    /* turns at least between two raids of a day on a player, where the day has room */
#define DN_DAY1_CHANCE_FROM (4 * 60 * 20) /* a dungeon's first day: its chance raids not before minute 4 (the user,
                                             2026-11-22; they came from 36 seconds, the first twentieth of the day) */
/* A raider whose party's leader died: it goes home at once (the mark is saved and streamed with the creature) */
#define DN_FLEEING_MARK CHeroTsk_DefendParty
#define DN_HOME_DIST 2               /* slabs from a landing spot a raider going home leaves at */
#define DN_RETREAT_TURNS (2 * 60 * 20) /* after this long going home, the raiders left leave wherever they are */

#ifdef CREATURE_MAX_LEVEL_LIMIT
#define DN_LEVEL_CAP CREATURE_MAX_LEVEL_LIMIT /* with the max-level patch; set_creature_level keeps to the rule */
#else
#define DN_LEVEL_CAP CREATURE_MAX_LEVEL
#endif

static uint64_t dn_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct DnRng {
    uint64_t s;
};

static uint32_t dn_rand(struct DnRng *r, uint32_t n)
{
    r->s = dn_mix(r->s);
    return (n > 0) ? (uint32_t)(r->s % n) : 0;
}

/** Possible tunnel-ins on a day: 2 to day 4, 3 to day 8, then 4. The first always comes, the others one time in
 *  two. (2026-11-11: 1 on day 1; 2026-11-14, the player: "1 extra possible raid for the 1st day".) */
static int dn_raids_on_day(long day)
{
    return (day < 5) ? 2 : (day < 9) ? 3 : 4;
}

/** Time of day of raid i of n: one in each part of the day, at a time fixed by the seed. No raid starts in the first
 *  or last twentieth of a day. A dungeon's first day (the user, 2026-11-15): its first raid (the one that always
 *  comes) waits for the second half, and the others (each one time in two) come between minute 4 (2026-11-22) and
 *  midday. */
static uint64_t dn_player_salt(PlayerNumber plyr_idx);

/** Time of raid i for a player: each player's at its own time in the slot (all of them landed in one turn, each
 *  loading its landing chunks there; audit 2026-11-09). Player 0 keeps the time it always had. */
static GameTurnDelta dn_raid_time_for(long day, int i, int n, PlayerNumber plyr_idx);

/** The end of a day no raid lands in: none any more (the user, 2026-11-19: "remove the last-two-minutes restriction
 *  for the Raid parties"; it was DN_NIGHT_CLEAR, two minutes, from 2026-11-18). */
static GameTurnDelta dn_night_clear(long day)
{
    (void)day;
    return 0;
}

static GameTurnDelta dn_raid_time_for(long day, int i, int n, PlayerNumber plyr_idx)
{
    const GameTurnDelta edge = dn_light(day) / 20;
    GameTurnDelta start = edge, end = dn_light(day) - dn_night_clear(day); /* (it was the last twentieth: 36 seconds) */
    int slots = n, j = i; /* raid i is in slot j of the part of the day it comes in */
    if (world_seat_day(plyr_idx) <= 1)
    {
        const GameTurnDelta midday = dn_light(day) / 2;
        if ((i == 0) || (n < 2))
        {
            start = midday; /* the raid that always comes: the second half, as before */
            slots = 1;
            j = 0;
        } else
        {
            end = midday; /* the others: between minute 4 and midday (in a short day, its first third and midday) */
            start = min(DN_DAY1_CHANCE_FROM, midday * 2 / 3);
            if (start < edge)
                start = edge;
            slots = n - 1;
            j = i - 1;
        }
    }
    const GameTurnDelta span = end - start;
    /* breathing room (2026-11-11): DN_RAID_GAP between two raids, or as much as the day has room for (four raids
       don't fit four minutes apart in a 12-minute day); what is left over is shared out as each raid's leeway */
    GameTurnDelta gap = 0;
    if (slots > 1)
    {
        gap = DN_RAID_GAP;
        const GameTurnDelta most = span * 4 / 5 / (slots - 1);
        if (gap > most)
            gap = most;
    }
    GameTurnDelta slot = (span - gap * (slots - 1)) / slots;
    if (slot < 1)
        slot = 1;
    uint64_t h = dn_mix(world_seed() ^ dn_mix((uint64_t)day * 0x632BE59BD9B4E019ULL + (uint64_t)i));
    if (plyr_idx != PLAYER0)
        h = dn_mix(h ^ dn_player_salt(plyr_idx));
    return start + (gap + slot) * j + (GameTurnDelta)(h % (uint64_t)slot);
}

/** The slabs the landing search reads: a box of engine slabs (world_read_slab_box), loaded or not. */
struct DnArea {
    MapSlabCoord x1, y1;
    int w, h;
    uint8_t *kind;
    uint8_t *mine; /* the player's slab */
    uint8_t *reach; /* a Tunneller can get from here to the heart (dn_reach); NULL: not worked out */
};

/** Slabs a raid's Tunneller gets through on its way to the heart: not rock, gold, gems or lava, which it can't dig. It
 *  digs earth, walks floors and water, and breaks or goes round a keeper's walls and doors. */
static TbBool dn_tunnel_passable(SlabKind k)
{
    return (k != SlbT_ROCK) && (k != SlbT_GOLD) && (k != SlbT_DENSEGOLD) && (k != SlbT_GEMS) && (k != SlbT_LAVA);
}

/** The area's slabs from which a Tunneller can get to the heart: a flood from the heart's slab over the passable ones
 *  (the user, 2026-11-19: "heroes can spawn in a single empty slab surrounded by Gold ... If the location does not have
 *  a way out towards the Player's heart, then it should not spawn there"). */
static void dn_reach(struct DnArea *a, MapSlabCoord hx, MapSlabCoord hy)
{
    const size_t n = (size_t)a->w * a->h;
    a->reach = (uint8_t *)calloc(n, 1);
    int32_t *queue = (int32_t *)malloc(sizeof(int32_t) * n);
    if ((a->reach == NULL) || (queue == NULL) || (hx < a->x1) || (hy < a->y1) || (hx >= a->x1 + a->w) || (hy >= a->y1 + a->h))
    {
        free(a->reach);
        a->reach = NULL;
        free(queue);
        return;
    }
    size_t head = 0, tail = 0;
    const int32_t start = (int32_t)((hy - a->y1) * a->w + (hx - a->x1));
    a->reach[start] = 1;
    queue[tail++] = start;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while (head < tail)
    {
        const int32_t c = queue[head++];
        const int cx = c % a->w, cy = c / a->w;
        for (int d = 0; d < 4; d++)
        {
            const int nx = cx + dx[d], ny = cy + dy[d];
            if ((nx < 0) || (ny < 0) || (nx >= a->w) || (ny >= a->h))
                continue;
            const int32_t nc = ny * a->w + nx;
            if (a->reach[nc] || !dn_tunnel_passable((SlabKind)a->kind[nc]))
                continue;
            a->reach[nc] = 1;
            queue[tail++] = nc;
        }
    }
    free(queue);
}

/** Can a raid's Tunneller get from this landing to the heart? */
static TbBool dn_reachable(const struct DnArea *a, MapSlabCoord x, MapSlabCoord y)
{
    return (a->reach == NULL) || (a->reach[(size_t)(y - a->y1) * a->w + (size_t)(x - a->x1)] != 0);
}

static SlabKind dn_kind(const struct DnArea *a, MapSlabCoord x, MapSlabCoord y)
{
    return a->kind[(size_t)(y - a->y1) * a->w + (size_t)(x - a->x1)];
}

/** Open ground to land on: a dug or claimed floor that isn't the player's. */
static TbBool dn_landing(const struct DnArea *a, MapSlabCoord x, MapSlabCoord y)
{
    const SlabKind kind = dn_kind(a, x, y);
    if ((kind != SlbT_PATH) && (kind != SlbT_CLAIMED))
        return false;
    return !a->mine[(size_t)(y - a->y1) * a->w + (size_t)(x - a->x1)];
}

/** Slabs a Tunneller can't dig through (rock, gems) or must not cross (lava) on the straight line from (x0,y0)
 *  to (x1,y1). */
static int dn_line_blockers(const struct DnArea *a, MapSlabCoord x0, MapSlabCoord y0, MapSlabCoord x1, MapSlabCoord y1)
{
    const int dx = abs(x1 - x0), dy = abs(y1 - y0);
    const int n = (dx > dy) ? dx : dy;
    int blockers = 0;
    for (int i = 1; i < n; i++)
    {
        const MapSlabCoord x = x0 + (MapSlabCoord)floor((double)(x1 - x0) * i / n + 0.5);
        const MapSlabCoord y = y0 + (MapSlabCoord)floor((double)(y1 - y0) * i / n + 0.5);
        const SlabKind kind = dn_kind(a, x, y);
        if ((kind == SlbT_ROCK) || (kind == SlbT_GEMS) || (kind == SlbT_LAVA) || (kind == SlbT_GOLD) ||
            (kind == SlbT_DENSEGOLD)) /* (gold too, 2026-11-13: a Tunneller cannot dig it either) */
            blockers++;
    }
    return blockers;
}

/* The threat budget (the user, 2026-11-18: "Raiders are overperforming. Especially when 3 at a time start to spawn. We
 * need to move away from the based-on-highest-creature-level rule"). A raid is bought with a budget: the army's
 * fighting power (world_threat.c) times a curve of the dungeon's days times an adjustment from how the last raids went
 * (it grew at most 25% a raid until 2026-11-22). Each hero costs its own fighting power. */
#define DN_CURVE_DAY1 0.5            /* the budget against the army on a dungeon's first day ... */
#define DN_CURVE_STEP 0.05           /* ... and each day after ... */
#define DN_CURVE_TOP 1.2             /* ... up to this (day 15) */
#define DN_ADJUST_MIN 0.6            /* the adjustment from the last raids: between these */
#define DN_ADJUST_MAX 1.3
#define DN_ADJUST_UP 1.1             /* a raid crushed with almost no losses: the next one this much stronger */
#define DN_ADJUST_DOWN 0.8           /* a raid that hurt: the next one this much weaker */
#define DN_ADJUST_RESTART 0.7        /* a dungeon that fell starts again this low */
#define DN_LOSS_SMALL 0.10           /* army lost: "almost none" up to this share ... */
#define DN_LOSS_BIG 0.40             /* ... "it hurt" from this share */
#define DN_KILLED_CRUSHED 0.6        /* "crushed": at least this share of the raiders killed */
#define DN_PARTIES_EARLY 2           /* at most this many parties a raid before day DN_PARTIES_LATE_DAY ... */
#define DN_PARTIES_LATE 3            /* ... and this many after */
#define DN_PARTIES_LATE_DAY 8
#define DN_LATE_TURNS (45 * 20)      /* a third party lands 45 to 90 seconds after the others, from another side */
#define DN_LEADER_DAY_STEP 5         /* the leader: a level more for every this many days ... */
#define DN_LEADER_DAY_MAX 3          /* ... at most this many */

/** The threat state of each player (saved: THR1). */
struct DnThreat {
    float adjust;       /* 0: not set yet (1.0) */
    float last_budget;  /* the last raid's budget; 0: none yet (no growth cap) */
    float raid_army;    /* the keeper's army power when the raid on now landed; 0: not tracking one */
    float raid_power;   /* that raid's power */
    int32_t heart_hp;   /* the heart's health then */
    int32_t heart_min;  /* its lowest health during the raid */
    int32_t heart_idx;  /* its index (another heart now: it fell) */
    int16_t made, killed; /* the raid's heroes, landed and killed */
};
static struct DnThreat dn_thr[PLAYERS_MAX];

/** A raid's third party, landing later (dn_late_turn). Not saved: a load before it lands drops it. */
struct DnLate {
    uint8_t active;
    GameTurn at;
    int64_t wx, wy;             /* where, world slab */
    int32_t day;
    int idx, n;
    ThingModel models[6];
    int levels[6];
    double cost;
    uint64_t seed;
};
static struct DnLate dn_late[PLAYERS_MAX];

static double dn_adjust(PlayerNumber p)
{
    return ((p >= 0) && (p < PLAYERS_MAX) && (dn_thr[p].adjust > 0.0f)) ? (double)dn_thr[p].adjust : 1.0;
}

static ThingModel dn_model(const char *name)
{
    const long model = get_id(creature_desc, name);
    return (model > 0) ? (ThingModel)model : 0;
}

static int dn_tier_for(int best)
{
    int t = 0;
    for (int i = 0; i < DN_TIERS; i++)
        if (best >= dn_tiers[i].min_level)
            t = i;
    return t;
}

/** The followers of a tier's parties landing in a biome. */
struct DnPool {
    ThingModel model[16];
    int weight[16];
    int n, total;
};

static void dn_pool_add(struct DnPool *pool, const char *name, int weight)
{
    const ThingModel model = dn_model(name);
    if ((model <= 0) || (weight <= 0) || (pool->n >= 16))
        return;
    pool->model[pool->n] = model;
    pool->weight[pool->n] = weight;
    pool->total += weight;
    pool->n++;
}

static void dn_make_pool(int tier, int biome, struct DnPool *pool)
{
    memset(pool, 0, sizeof(*pool));
    dn_pool_add(pool, dn_tunneller_follower.name, dn_tunneller_follower.weight);
    for (int t = 0; t <= tier; t++)
        dn_pool_add(pool, dn_tiers[t].add.name, dn_tiers[t].add.weight);
    if ((dn_tiers[tier].biome_weight > 0) && (biome >= 0) && (biome < WgB_COUNT))
        for (int k = 0; k < 2; k++)
            if (dn_biome_heroes[biome][k] != NULL)
                dn_pool_add(pool, dn_biome_heroes[biome][k], dn_tiers[tier].biome_weight);
}

static ThingModel dn_pick_follower(struct DnRng *rng, const struct DnPool *pool)
{
    if (pool->n <= 0)
        return dn_model("THIEF");
    int r = (int)dn_rand(rng, (uint32_t)pool->total), i = 0;
    while ((i < pool->n - 1) && (r >= pool->weight[i]))
    {
        r -= pool->weight[i];
        i++;
    }
    return pool->model[i];
}

/** Does possible raid i of a day come? Fixed by the seed: DN_RAID_CHANCE percent of them do. */
/** Each human player gets their own raids; PLAYER0's dice are those of a single-player run. */
static uint64_t dn_player_salt(PlayerNumber plyr_idx)
{
    return (uint64_t)plyr_idx * 0x9E3779B97F4A7C15ULL;
}

static TbBool dn_raid_happens(long day, int i, PlayerNumber plyr_idx)
{
    if (i == 0)
        return (dn_raid_chance > 0); /* the first raid of a day always comes (2026-11-11; a test's 0% still means none) */
    const uint64_t h = dn_mix(world_seed() ^ dn_mix((uint64_t)day * 0x2545F4914F6CDD1DULL + (uint64_t)i + 77) ^
        dn_player_salt(plyr_idx));
    return (int)(h % 100) < dn_raid_chance;
}

/* creature_states_hero.c, not in a header */
TbBool script_support_send_tunneller_to_dungeon_heart(struct Thing *creatng, PlayerNumber plyr_idx);

/** Levels for a party of n (2026-11-18, the threat budget): the leader a level above the army's upper quarter, and a
 *  level more for every DN_LEADER_DAY_STEP days (at most DN_LEADER_DAY_MAX); the others spread evenly from the upper
 *  quarter down to the army's middle level; each moved by -1, 0 or +1. (It was: the leader at the best creature + 1 +
 *  1 per 3 days, the others from just under the best down to half of it.) */
static void dn_party_levels(struct DnRng *rng, int n, int upper, int median, long day, int *levels)
{
    levels[0] = upper + 1 + min((int)((day - 1) / DN_LEADER_DAY_STEP), DN_LEADER_DAY_MAX);
    const int m = n - 1;
    const double low = (median < upper) ? median : upper;
    for (int j = 1; j <= m; j++)
        levels[j] = (int)floor(low + (upper - low) * (double)(m - j) / (double)((m > 1) ? m - 1 : 1) + 0.5);
    for (int j = 0; j < n; j++)
    {
        levels[j] += (int)dn_rand(rng, 3) - 1;
        if (levels[j] < 1)
            levels[j] = 1;
        if (levels[j] > DN_LEVEL_CAP)
            levels[j] = DN_LEVEL_CAP;
    }
}

/******************************************************************************/
/* Invasions: the parties of one raid on one player (2026-11-08). A new raid on a player waits until the last one is
 * over (every raider gone); at the dawn after their third night the raiders walk back to where they landed and leave
 * through the ceiling. Saved with the world (world_raids_save). */
struct DnInvasion {
    uint8_t active, retreating, nspots;
    int32_t day;                       /* the day it landed */
    GameTurn retreat_turn;
    GameTurn flee_turn;                /* when a party's leader last died (not saved: a load starts it again) */
    int64_t spot_x[DN_MAX_PARTIES], spot_y[DN_MAX_PARTIES]; /* the parties' landings, world slabs */
};
static struct DnInvasion dn_inv[PLAYERS_MAX];
/** The day whose first raid a player is still owed: it couldn't come while the last raid's heroes were on their way
 *  home, and comes when they are gone (that day only; not saved). 0: none. */
static int32_t dn_owed_day[PLAYERS_MAX];
/** Raids that waited because the dungeon had no creatures but Imps (the user, 2026-11-22: a raid came at 36 seconds
 *  on a dungeon of Imps only): they come once it has a creature, the same day, one at a time DN_RAID_GAP apart; at
 *  nightfall the ones still waiting are dropped. Not saved (a load drops them). */
struct DnArmyWait {
    int32_t day;
    uint32_t mask;      /* the day's raids waiting (bit = raid index) */
    GameTurn last;      /* when the last of them came */
};
static struct DnArmyWait dn_army_wait[PLAYERS_MAX];

TbBool world_raid_hero(const struct Thing *thing);

TbBool world_raid_on(PlayerNumber plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && dn_inv[plyr_idx].active && !dn_inv[plyr_idx].retreating;
}

TbBool world_raid_present(PlayerNumber plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && (dn_inv[plyr_idx].active || dn_late[plyr_idx].active);
}

static int dn_raider_invasion(const struct Thing *thing);

TbBool world_raid_at_dungeon(PlayerNumber plyr_idx, int slabs)
{
    if (!world_raid_present(plyr_idx))
        return false;
    struct Thing *heart = find_players_dungeon_heart(plyr_idx);
    struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (!thing_exists(heart) || dungeon_invalid(heroes))
        return false;
    const long hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    long guard = 0;
    for (ThingIndex i = heroes->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (!world_raid_hero(thing) || (dn_raider_invasion(thing) != plyr_idx) || creature_is_dying(thing))
            continue;
        if ((labs((long)subtile_slab(thing->mappos.x.stl.num) - hx) <= slabs) && (labs((long)subtile_slab(thing->mappos.y.stl.num) - hy) <= slabs))
            return true;
    }
    return false;
}

/** The invasion a raider belongs to: its target's, else the one whose landing is nearest; -1 none. */
static int dn_raider_invasion(const struct Thing *thing)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const PlayerNumber t = cctrl->party.target_plyr_idx;
    if ((t >= 0) && (t < PLAYERS_COUNT) && dn_inv[t].active)
        return t;
    int64_t wx, wy;
    world_slab_to_world(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num), &wx, &wy);
    int best = -1;
    int64_t best_d = 0;
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!dn_inv[p].active)
            continue;
        for (int s = 0; s < dn_inv[p].nspots; s++)
        {
            const int64_t dx = dn_inv[p].spot_x[s] - wx, dy = dn_inv[p].spot_y[s] - wy;
            if ((best < 0) || (dx * dx + dy * dy < best_d))
            {
                best = p;
                best_d = dx * dx + dy * dy;
            }
        }
    }
    return best;
}

static TbBool dn_invasion_on(PlayerNumber plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_COUNT) && dn_inv[plyr_idx].active;
}

/** A raider leaves the world: back up through the ceiling. */
static void dn_raider_leaves(struct Thing *thing)
{
    create_effect(&thing->mappos, TngEff_CeilingBreach, thing->owner);
    world_remove_thing_quietly(thing);
}

/** A raider of a retreating invasion: on its way to the nearest landing, or gone when it is there (or can't get
 *  there, or has taken too long). Returns true when it was dealt with (it goes home or has left). */
static TbBool dn_raider_go_home(struct Thing *thing, struct DnInvasion *inv, TbBool in_update)
{
    if (creature_is_being_unconscious(thing) || creature_is_dying(thing) || thing_is_picked_up(thing) ||
        thing_is_dragged_or_pulled(thing) || creature_is_kept_in_custody(thing))
        return false;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const MapSlabCoord tx = subtile_slab(thing->mappos.x.stl.num), ty = subtile_slab(thing->mappos.y.stl.num);
    TbBool found = false;
    MapSlabCoord bx = 0, by = 0;
    long best_d = LONG_MAX;
    for (int s = 0; s < inv->nspots; s++)
    {
        MapSlabCoord sx, sy;
        if (!world_engine_slab(inv->spot_x[s], inv->spot_y[s], &sx, &sy))
            continue;
        const long d = max(labs((long)sx - tx), labs((long)sy - ty));
        if (d < best_d)
        {
            best_d = d;
            bx = sx;
            by = sy;
            found = true;
        }
    }
    /* how long it has been going home: since the raid turned back, or since its party's leader died */
    const GameTurn since = inv->retreating ? inv->retreat_turn : inv->flee_turn;
    const TbBool late = ((GameTurnDelta)(game.play_gameturn - since) > DN_RETREAT_TURNS);
    if (!found && !late)
    {
        /* its landing's ground went: it comes back, and the raider goes there next time (the landing's own chunk now,
           the ones round it in the next turns: nine at once were a 40 to 70 ms turn at every dawn) */
        for (int s = 0; s < inv->nspots; s++)
            world_load_chunks_landing_world(inv->spot_x[s], inv->spot_y[s]);
        return true;
    }
    if (!found || (best_d <= DN_HOME_DIST) || late)
    {
        if (in_update)
            return true; /* called from the creature's own update: it leaves on the invasions' turn, not inside it */
        SYNCLOG("World: a raiding %s leaves %s", thing_model_name(thing),
            !found ? "(its landing is off the map)" : (best_d <= DN_HOME_DIST) ? "where it landed" : "where it is (too long on its way)");
        dn_raider_leaves(thing);
        return true;
    }
    if ((cctrl->combat_flags != 0) || (get_creature_state_besides_interruptions(thing) == CrSt_MoveToPosition))
        return true; /* a fight first; or on its way already */
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(bx));
    pos.y.val = subtile_coord_center(slab_subtile_center(by));
    pos.z.val = 0;
    if (!creature_can_navigate_to(thing, &pos, NavRtF_Default))
        return true; /* no way there yet (not loaded, or dug shut): it waits, and leaves when the time is up */
    cleanup_current_thing_state(thing);
    if (setup_person_move_to_coord(thing, &pos, NavRtF_Default))
        thing->continue_state = CrSt_GoodDoingNothing;
    return true;
}

/** A raider whose party lost its leader. */
static TbBool dn_raider_fleeing(const struct Thing *thing)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    return !creature_control_invalid(cctrl) && (cctrl->party.original_objective == DN_FLEEING_MARK);
}

/******************************************************************************/
/* A raid leader's way to the heart (2026-11-13).
 *
 * The stock Tunneller digs a straight line and follows whatever it cannot dig (rock, gold, gems, a keeper's
 * reinforced walls) by "wall hugging". That froze where two such faces meet at an inside corner, and where its line
 * clipped the corner of a seam: it walks the middle of its own one-slab tunnel, so the step towards the wall it hugs
 * always looks free, and it turned on the spot in four small steps for ever (seen at a dungeon's corner and beside a
 * gold seam: no raid of five arrived). A raid's leader is led slab by slab instead:
 * - straight for the heart, a slab at a time along the line (earth is dug, open ground walked);
 * - where the slab ahead is something it cannot dig it goes round it, keeping it on one hand, as the campaigns'
 *   Tunnellers go round a dungeon's walls. It leaves the obstacle where it is nearer the heart than where it met it
 *   and the way on is free;
 * - a dungeon closed by reinforced walls is such an obstacle: the leader goes round and round it. A hole in the walls
 *   (a slab that is not reinforced, a corridor's mouth) lets it in as it comes by, it steps onto the keeper's open
 *   ground wherever that lies beside it, and whenever it can walk to the heart it does (through a door too: heroes
 *   break doors). Walls are never dug: a dungeon with no hole and no door is not entered;
 * - a dungeon with long walled corridors can take longer than a day to go round, so after RG_SCOUT_SLABS slabs round
 *   without a way in the leader makes for the nearest hole there is: the nearest ground the heart can be walked to
 *   from, by the cheapest way over open ground and earth, never through a wall. With no hole it goes on round.
 * Every leg is one slab, to the slab beside it, so the stock code only ever walks or digs straight ahead and its wall
 * hugging never starts. Kept per creature and not saved: after a load the leader starts again from where it stands. */
#define RG_WALK_CHECK 128   /* turns between two looks for a way to walk to the heart */
#define RG_WALK_SOON 16     /* the same beside one of the keeper's doors, on the keeper's ground, at a way's end */
#define RG_ARRIVED 48       /* this near the middle of the slab it was sent to, it is sent on */
#define RG_LEG_TURNS 400    /* a slab not reached in this long is left alone for a while (a dug slab takes about 50) */
#define RG_AVOID_TURNS 2000
#define RG_NO_ENTRY_TURNS 600 /* the keeper's ground it stood on led nowhere: it doesn't step aside for more that long */
#define RG_SCOUT_SLABS 120  /* slabs round without a way in before it looks for the nearest hole (and again after) */
#define RG_PATHS 8          /* ways to a hole kept at once (one a leader) */
#define RG_PATH_MAX 1024    /* slabs of such a way at the most */
enum { RG_Direct = 0, RG_Round, RG_Path };
enum { RG_HandNone = 0, RG_HandRight, RG_HandLeft }; /* the side the obstacle is kept on */

struct RaidGuide {
    ThingIndex thing;        /* whose: the stamp, with born */
    GameTurn born;
    uint8_t started, mode, hand, dir;
    uint8_t leg_set, path, hit_first_set, hit_first_dir; /* path: its way to a hole, slot + 1 (0: none) */
    int32_t laps, round_steps;
    int32_t round_total, scout_at; /* slabs gone round in all; when it next looks for a hole */
    int64_t goal_x, goal_y;  /* the heart it is going for (world slabs, as all below) */
    int64_t line_x, line_y;  /* where its straight line began */
    int64_t leg_x, leg_y;    /* the slab it was sent to last */
    int64_t hit_x, hit_y;    /* where it met what it is going round */
    int64_t hit_d2;          /* how far from the heart that was (squared) */
    int64_t avoid_x, avoid_y; /* a slab it could not get onto */
    GameTurn avoid_until;
    GameTurn leg_turn;       /* when it was sent to the slab */
    GameTurn walk_turn;      /* when it last looked for a way to walk in */
    GameTurn no_entry_until;
};
struct RaidPath {
    ThingIndex thing;        /* whose (0: free) */
    GameTurn made;
    uint16_t len, at;        /* its steps, and the next one */
    int64_t x, y;            /* the slab the next step leaves from */
    uint8_t dir[RG_PATH_MAX];
};
static struct RaidGuide rg_table[CREATURES_COUNT];
static struct RaidPath rg_paths[RG_PATHS];
static const int8_t rg_dx[4] = {0, 1, 0, -1}, rg_dy[4] = {-1, 0, 1, 0}; /* north, east, south, west */
static int rg_log_budget = 400;

static void rg_reset(void)
{
    memset(rg_table, 0, sizeof(rg_table));
    memset(rg_paths, 0, sizeof(rg_paths));
    rg_log_budget = 400;
}

static void rg_drop_path(struct RaidGuide *g)
{
    if ((g->path > 0) && (g->path <= RG_PATHS) && (rg_paths[g->path - 1].thing == g->thing))
        rg_paths[g->path - 1].thing = 0;
    g->path = 0;
}

static struct RaidGuide *rg_of(const struct Thing *creatng)
{
    const long i = creatng->ccontrol_idx;
    if ((i <= 0) || (i >= CREATURES_COUNT))
        return NULL;
    struct RaidGuide *g = &rg_table[i];
    if ((g->thing != creatng->index) || (g->born != (GameTurn)creatng->creation_turn))
    {
        rg_drop_path(g);
        memset(g, 0, sizeof(*g));
        g->thing = creatng->index;
        g->born = (GameTurn)creatng->creation_turn;
    }
    return g;
}

/** A slab a raid's Tunneller can go onto: open ground it can walk, or earth it can dig. Not rock, gold, gems, lava,
 *  a wall, a door, or ground that isn't loaded. */
static TbBool rg_free(MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return false;
    const SlabKind k = slb->kind;
    if ((k == SlbT_EARTH) || (k == SlbT_TORCHDIRT))
        return true;
    if ((k == SlbT_ROCK) || (k == SlbT_LAVA))
        return false;
    const struct SlabConfigStats *st = get_slab_kind_stats(k);
    if ((st->block_flags & (SlbAtFlg_IsDoor | SlbAtFlg_Filled | SlbAtFlg_Valuable | SlbAtFlg_Digable)) != 0)
        return false; /* doors, walls, gold, gems, anything else to be dug */
    /* open ground: its middle is clear (a room's floor is; a kind's own flags say "blocking" for every room, which
       is only true of its pillars) */
    const struct Map *mapblk = get_map_block_at(slab_subtile_center(x), slab_subtile_center(y));
    return !map_block_invalid(mapblk) && ((mapblk->flags & SlbAtFlg_Blocking) == 0);
}

static TbBool rg_avoided(const struct RaidGuide *g, MapSlabCoord x, MapSlabCoord y)
{
    if (game.play_gameturn >= g->avoid_until)
        return false;
    int64_t wx, wy;
    world_slab_to_world(x, y, &wx, &wy);
    return (wx == g->avoid_x) && (wy == g->avoid_y);
}

/** The same, for this leader now: not the slab it could not get onto a while ago. */
static TbBool rg_open(const struct RaidGuide *g, MapSlabCoord x, MapSlabCoord y)
{
    return rg_free(x, y) && !rg_avoided(g, x, y);
}

static TbBool rg_keepers(MapSlabCoord x, MapSlabCoord y, PlayerNumber target)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == target);
}

static TbBool rg_earth(MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && ((slb->kind == SlbT_EARTH) || (slb->kind == SlbT_TORCHDIRT));
}

/** The keeper's own open ground (claimed floor or a room's). */
static TbBool rg_keepers_ground(MapSlabCoord x, MapSlabCoord y, PlayerNumber target)
{
    return rg_keepers(x, y, target) && rg_free(x, y) && !rg_earth(x, y);
}

static TbBool rg_keepers_door(MapSlabCoord x, MapSlabCoord y, PlayerNumber target)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return rg_keepers(x, y, target) && slab_kind_is_door(slb->kind);
}

/** The steps of a straight line from (lx,ly) to (gx,gy) that leave (cx,cy): the one that keeps nearest the line, and
 *  the other one that also brings it nearer the goal (-1: none). */
static void rg_line_steps(int64_t cx, int64_t cy, int64_t lx, int64_t ly, int64_t gx, int64_t gy, int *first, int *second)
{
    const int64_t dx = gx - cx, dy = gy - cy;
    const int dir_x = (dx > 0) ? 1 : 3, dir_y = (dy > 0) ? 2 : 0;
    *first = -1;
    *second = -1;
    if ((dx == 0) && (dy == 0))
        return;
    if (dx == 0)
    {
        *first = dir_y;
        return;
    }
    if (dy == 0)
    {
        *first = dir_x;
        return;
    }
    /* which of the two slabs lies nearer the line (twice the area of the triangle it makes with the line) */
    const int64_t ax = gx - lx, ay = gy - ly;
    const int64_t off_x = llabs(ax * (cy - ly) - ay * (cx + rg_dx[dir_x] - lx));
    const int64_t off_y = llabs(ax * (cy + rg_dy[dir_y] - ly) - ay * (cx - lx));
    if ((off_x < off_y) || ((off_x == off_y) && (llabs(dx) >= llabs(dy))))
    {
        *first = dir_x;
        *second = dir_y;
    } else
    {
        *first = dir_y;
        *second = dir_x;
    }
}

/** Is this a raid's leader that is led: a raiding Tunneller at the head of its party (or alone) whose raid is on?
 *  Gives the keeper it raids and the slab of that keeper's heart. */
static TbBool rg_led(const struct Thing *creatng, PlayerNumber *target, MapSlabCoord *hx, MapSlabCoord *hy)
{
    if (!world_raid_hero(creatng) || (creatng->model != get_players_special_digger_model(creatng->owner)))
        return false;
    if (creature_is_group_member(creatng) && !creature_is_group_leader(creatng))
        return false;
    if (dn_raider_fleeing(creatng))
        return false;
    const int p = dn_raider_invasion(creatng);
    if ((p < 0) || dn_inv[p].retreating || !world_human_heart_loaded((PlayerNumber)p))
        return false;
    const struct Thing *heartng = find_players_dungeon_heart((PlayerNumber)p);
    if (!thing_exists(heartng))
        return false;
    *target = (PlayerNumber)p;
    *hx = subtile_slab(heartng->mappos.x.stl.num);
    *hy = subtile_slab(heartng->mappos.y.stl.num);
    return true;
}

/** Can the leader walk to the heart now (an open way, or one through a door, which heroes break)? */
static TbBool rg_can_walk_in(struct Thing *creatng, PlayerNumber target)
{
    struct Thing *heartng = find_players_dungeon_heart(target);
    if (!thing_exists(heartng) || player_cannot_win(target))
        return false;
    return creature_can_navigate_to(creatng, &heartng->mappos, NavRtF_Default);
}

/** What a slab of ground that isn't loaded is to the search, by its kind: 0 barred, 1 open, 2 earth, 3 the keeper's door. */
static uint8_t rg_kind_class(SlabKind k, TbBool keepers)
{
    if ((k == SlbT_EARTH) || (k == SlbT_TORCHDIRT))
        return 2;
    if (slab_kind_is_door(k))
        return keepers ? 3 : 0;
    if ((k == SlbT_ROCK) || (k == SlbT_LAVA))
        return 0;
    const struct SlabConfigStats *st = get_slab_kind_stats(k);
    return ((st->block_flags & (SlbAtFlg_Filled | SlbAtFlg_Valuable | SlbAtFlg_Digable)) != 0) ? 0 : 1;
}

/** The nearest hole: the cheapest way from the leader, over open ground (1 a slab) and earth it can dig (3), never
 *  through a wall, to ground the heart can be walked to from (through the keeper's doors too: heroes break them).
 *  The ground looked at is the box a raid's landing is chosen in (as far round the heart as raids land): what is
 *  loaded, and what the landing search remembers of the rest; nothing is read for it, and ground that is not known
 *  counts as rock (*unknown: how many chunks of it). Returns the way's slot + 1, or 0: no hole (or the leader is
 *  outside that box). */
static int rg_scout(const struct Thing *creatng, const struct RaidGuide *g, PlayerNumber target, MapSlabCoord hx, MapSlabCoord hy,
    int *dug, int *unknown)
{
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    const MapSlabCoord reach = DN_SPAWN_DIST + DN_SPAWN_SLACK;
    const MapSlabCoord x1 = hx - reach, y1 = hy - reach;
    const int w = 2 * reach + 1, h = 2 * reach + 1;
    *unknown = 0;
    if ((cx < x1) || (cy < y1) || (cx >= x1 + w) || (cy >= y1 + h))
        return 0;
    const int n = w * h;
    uint8_t *cls = (uint8_t *)malloc((size_t)n);   /* 0 barred, 1 open, 2 earth, 3 the keeper's door; +16 joined to the heart */
    uint8_t *from = (uint8_t *)malloc((size_t)n);  /* the step that reached it; 0xFF not reached */
    uint8_t *kinds = (uint8_t *)malloc((size_t)n), *mine = (uint8_t *)malloc((size_t)n);
    int32_t *queue = (int32_t *)malloc(sizeof(int32_t) * 3 * (size_t)n);
    int slot = 0;
    if ((cls != NULL) && (from != NULL) && (queue != NULL) && (kinds != NULL) && (mine != NULL))
    {
        const TbBool known = world_read_slab_box_known(x1, y1, x1 + w - 1, y1 + h - 1, kinds, mine, target, unknown);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const MapSlabCoord sx = x1 + x, sy = y1 + y;
                uint8_t c = 0;
                if (rg_avoided(g, sx, sy))
                    c = 0;
                else if (!slabmap_block_invalid(get_slabmap_block(sx, sy)))
                {
                    /* loaded: as the leader's own steps see it */
                    if (rg_keepers_door(sx, sy, target))
                        c = 3;
                    else if (rg_earth(sx, sy))
                        c = 2;
                    else if (rg_free(sx, sy))
                        c = 1;
                } else if (known)
                    c = rg_kind_class((SlabKind)kinds[y * w + x], mine[y * w + x] != 0);
                cls[y * w + x] = c;
            }
        const int heart = (hy - y1) * w + (hx - x1), leader = (cy - y1) * w + (cx - x1);
        cls[heart] = 1;  /* (the heart's own slab, whatever stands on it) */
        if ((cls[leader] & 15) != 3)
            cls[leader] = 1;
        /* the ground the heart can be walked to from */
        int head = 0, tail = 0;
        queue[tail++] = heart;
        cls[heart] |= 16;
        while (head < tail)
        {
            const int i = queue[head++];
            const int x = i % w, y = i / w;
            for (int d = 0; d < 4; d++)
            {
                const int nx = x + rg_dx[d], ny = y + rg_dy[d];
                if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h))
                    continue;
                const int k = ny * w + nx;
                if (((cls[k] & 16) != 0) || (((cls[k] & 15) != 1) && ((cls[k] & 15) != 3)))
                    continue;
                cls[k] |= 16;
                queue[tail++] = k;
            }
        }
        /* the cheapest way from the leader to it: every queue entry is a slab and the turns of digging left on it
           (an earth slab waits two entries more than an open one), so the entries come out in the order of cost */
        int found = -1;
        if ((cls[leader] & 16) == 0)
        {
            memset(from, 0xFF, (size_t)n);
            head = 0;
            tail = 0;
            from[leader] = 4;
            queue[tail++] = leader * 4;
            while ((head < tail) && (found < 0))
            {
                const int e = queue[head++];
                const int i = e >> 2;
                if ((e & 3) != 0)
                {
                    queue[tail++] = e - 1;
                    continue;
                }
                const int x = i % w, y = i / w;
                for (int d = 0; (d < 4) && (found < 0); d++)
                {
                    const int nx = x + rg_dx[d], ny = y + rg_dy[d];
                    if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h))
                        continue;
                    const int k = ny * w + nx;
                    if ((cls[k] & 16) != 0)
                        found = i; /* beside ground joined to the heart (or one of the keeper's doors on a way to it) */
                }
                for (int d = 0; (d < 4) && (found < 0); d++)
                {
                    const int nx = x + rg_dx[d], ny = y + rg_dy[d];
                    if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h))
                        continue;
                    const int k = ny * w + nx;
                    if ((from[k] != 0xFF) || (((cls[k] & 15) != 1) && ((cls[k] & 15) != 2)))
                        continue;
                    from[k] = (uint8_t)d;
                    queue[tail++] = k * 4 + (((cls[k] & 15) == 2) ? 2 : 0);
                }
            }
        }
        if (found >= 0)
        {
            /* back along the steps that reached it */
            int len = 0, earth = 0;
            for (int i = found; (i != leader) && (len <= RG_PATH_MAX); len++)
            {
                if ((cls[i] & 15) == 2)
                    earth++;
                const int d = from[i];
                i = i - rg_dy[d] * w - rg_dx[d];
            }
            if ((len > 0) && (len <= RG_PATH_MAX))
            {
                int s = -1;
                for (int k = 0; k < RG_PATHS; k++)
                    if (rg_paths[k].thing == 0)
                        s = k;
                for (int k = 0; (s < 0) && (k < RG_PATHS); k++)
                {
                    TbBool oldest = true;
                    for (int j = 0; j < RG_PATHS; j++)
                        oldest = oldest && (rg_paths[k].made <= rg_paths[j].made);
                    if (oldest)
                        s = k;
                }
                struct RaidPath *p = &rg_paths[s];
                p->thing = creatng->index;
                p->made = game.play_gameturn;
                p->len = (uint16_t)len;
                p->at = 0;
                world_slab_to_world(cx, cy, &p->x, &p->y);
                int at = len;
                for (int i = found; i != leader;)
                {
                    const int d = from[i];
                    p->dir[--at] = (uint8_t)d;
                    i = i - rg_dy[d] * w - rg_dx[d];
                }
                slot = s + 1;
                *dug = earth;
            }
        }
    }
    free(cls);
    free(from);
    free(queue);
    free(kinds);
    free(mine);
    return slot;
}

/** The leader's next slab from where it stands. False: walled in on every side. */
static TbBool rg_step(const struct Thing *creatng, struct RaidGuide *g, PlayerNumber target, MapSlabCoord hx, MapSlabCoord hy,
    MapSlabCoord *nx, MapSlabCoord *ny)
{
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    int64_t wcx, wcy, wgx, wgy;
    world_slab_to_world(cx, cy, &wcx, &wcy);
    world_slab_to_world(hx, hy, &wgx, &wgy);
    /* a new heart, or the leader isn't where its way left it (a fight, the hand, a push): its line starts here */
    if (!g->started || (g->goal_x != wgx) || (g->goal_y != wgy) ||
        (g->leg_set && (llabs(wcx - g->leg_x) + llabs(wcy - g->leg_y) > 1)))
    {
        rg_drop_path(g);
        g->started = 1;
        g->mode = RG_Direct;
        g->goal_x = wgx;
        g->goal_y = wgy;
        g->line_x = wcx;
        g->line_y = wcy;
    }
    int dir = -1;
    /* the keeper's open ground beside it: in it goes (not from the keeper's ground itself, nor soon after such
       ground led nowhere) */
    if (!rg_keepers(cx, cy, target) && (game.play_gameturn >= g->no_entry_until))
        for (int d = 0; (d < 4) && (dir < 0); d++)
            if (rg_keepers_ground(cx + rg_dx[d], cy + rg_dy[d], target) && rg_open(g, cx + rg_dx[d], cy + rg_dy[d]))
                dir = d;
    if ((dir < 0) && (g->mode == RG_Path))
    {
        /* on its way to a hole: the next step of it, while the way is still there */
        struct RaidPath *p = ((g->path > 0) && (g->path <= RG_PATHS)) ? &rg_paths[g->path - 1] : NULL;
        if ((p != NULL) && (p->thing == creatng->index) && (p->at < p->len) && (p->x == wcx) && (p->y == wcy) &&
            rg_open(g, cx + rg_dx[p->dir[p->at]], cy + rg_dy[p->dir[p->at]]))
        {
            dir = p->dir[p->at];
            p->at++;
            world_slab_to_world(cx + rg_dx[dir], cy + rg_dy[dir], &p->x, &p->y);
        } else
        {
            /* the way is gone (its hole was walled up meanwhile), or it led nowhere: straight for the heart again, and
               another look for a hole after a few slabs round what bars it then */
            const TbBool at_its_end = (p != NULL) && (p->thing == creatng->index) && (p->at >= p->len);
            rg_drop_path(g);
            g->mode = RG_Direct;
            g->line_x = wcx;
            g->line_y = wcy;
            g->scout_at = g->round_total + (at_its_end ? RG_SCOUT_SLABS : 8);
        }
    }
    const int64_t d2 = (wgx - wcx) * (wgx - wcx) + (wgy - wcy) * (wgy - wcy);
    if ((dir < 0) && (g->mode == RG_Round) && (d2 < g->hit_d2))
    {
        /* nearer the heart than where it met the obstacle, and the way on is free: it leaves it */
        int a, b;
        rg_line_steps(wcx, wcy, wcx, wcy, wgx, wgy, &a, &b);
        if (((a >= 0) && rg_open(g, cx + rg_dx[a], cy + rg_dy[a])) || ((b >= 0) && rg_open(g, cx + rg_dx[b], cy + rg_dy[b])))
        {
            g->mode = RG_Direct;
            g->line_x = wcx;
            g->line_y = wcy;
            if (rg_log_budget > 0)
            {
                rg_log_budget--;
                SYNCLOG("World: the leader (index %d) raiding player %d is past what barred its way after %d slabs round it",
                    (int)creatng->index, (int)target, (int)g->round_steps);
            }
        }
    }
    if ((dir < 0) && (g->mode == RG_Direct))
    {
        int a, b;
        rg_line_steps(wcx, wcy, g->line_x, g->line_y, wgx, wgy, &a, &b);
        if (a < 0)
            return false; /* (on the heart's own slab: the keeper's ground, the stock code has it by now) */
        if (rg_open(g, cx + rg_dx[a], cy + rg_dy[a]))
            dir = a;
        else if ((b >= 0) && rg_open(g, cx + rg_dx[b], cy + rg_dy[b]))
            dir = b;
        else
        {
            /* barred: it goes round, the obstacle on one hand. The first time the hand is the heart's side (straight
               ahead of the heart: by the creature's number); after that it keeps the hand it has. */
            g->mode = RG_Round;
            g->hit_x = wcx;
            g->hit_y = wcy;
            g->hit_d2 = d2;
            g->round_steps = 0;
            g->laps = 0;
            g->hit_first_set = 0;
            if (g->scout_at == 0)
                g->scout_at = g->round_total + RG_SCOUT_SLABS;
            if (g->hand == RG_HandNone)
            {
                const TbBool turn_left = (b >= 0) ? (b == ((a + 3) & 3)) : ((creatng->index & 1) != 0);
                g->hand = turn_left ? RG_HandRight : RG_HandLeft;
            }
            g->dir = (uint8_t)((g->hand == RG_HandRight) ? ((a + 3) & 3) : ((a + 1) & 3));
            if (rg_log_budget > 0)
            {
                rg_log_budget--;
                const struct SlabMap *slb = get_slabmap_block(cx + rg_dx[a], cy + rg_dy[a]);
                SYNCLOG("World: the leader (index %d) raiding player %d meets %s of player %ld, %ld,%ld slabs from the heart:"
                    " it goes round it, keeping it on its %s", (int)creatng->index, (int)target,
                    slabmap_block_invalid(slb) ? "ground not loaded" : slab_code_name(slb->kind),
                    slabmap_block_invalid(slb) ? -1L : slabmap_owner(slb), (long)(wcx - wgx), (long)(wcy - wgy),
                    (g->hand == RG_HandRight) ? "right" : "left");
            }
        }
    }
    if ((dir < 0) && (g->mode == RG_Round))
    {
        /* the hand's side first, then straight on, then away from it, then back */
        const int h = g->dir;
        const int toward = (g->hand == RG_HandRight) ? ((h + 1) & 3) : ((h + 3) & 3);
        const int away = (g->hand == RG_HandRight) ? ((h + 3) & 3) : ((h + 1) & 3);
        const int order[4] = {toward, h, away, (h + 2) & 3};
        for (int k = 0; (k < 4) && (dir < 0); k++)
            if (rg_open(g, cx + rg_dx[order[k]], cy + rg_dy[order[k]]))
                dir = order[k];
        if (dir < 0)
            return false;
        if ((wcx == g->hit_x) && (wcy == g->hit_y))
        {
            if (!g->hit_first_set)
            {
                g->hit_first_set = 1;
                g->hit_first_dir = (uint8_t)dir;
            } else if ((dir == g->hit_first_dir) && (g->round_steps > 3))
            {
                /* once round, and no way in: a dungeon closed by its walls. Round again: a hole may come. */
                g->laps++;
                if ((rg_log_budget > 0) && ((g->laps <= 3) || ((g->laps % 10) == 0)))
                {
                    rg_log_budget--;
                    SYNCLOG("World: the leader (index %d) raiding player %d has been round what bars its way %d time%s"
                        " (%d slabs) and found no way in: it goes round again", (int)creatng->index, (int)target,
                        (int)g->laps, (g->laps == 1) ? "" : "s", (int)g->round_steps);
                }
                g->round_steps = 0;
            }
        }
        g->dir = (uint8_t)dir;
        g->round_steps++;
        g->round_total++;
        if (g->round_total >= g->scout_at)
        {
            /* a long time round and no way in: is there a hole anywhere near? */
            g->scout_at = g->round_total + RG_SCOUT_SLABS;
            int dug = 0, unknown = 0;
            const int64_t t0 = world_prof_now();
            const int slot = rg_scout(creatng, g, target, hx, hy, &dug, &unknown);
            if (t0 != 0)
                SYNCLOG("World profile: a raid leader's look for a hole took %.2f ms", (double)(world_prof_now() - t0) / 1e6);
            if (slot > 0)
            {
                struct RaidPath *p = &rg_paths[slot - 1];
                rg_drop_path(g);
                g->path = (uint8_t)slot;
                g->mode = RG_Path;
                dir = p->dir[0];
                p->at = 1;
                world_slab_to_world(cx + rg_dx[dir], cy + rg_dy[dir], &p->x, &p->y);
            }
            if (rg_log_budget > 0)
            {
                rg_log_budget--;
                if (slot > 0)
                    SYNCLOG("World: the leader (index %d) raiding player %d has gone round %d slabs without a way in: it makes"
                        " for the nearest hole, %d slabs on (%d of them earth to dig)", (int)creatng->index, (int)target,
                        (int)g->round_total, (int)rg_paths[slot - 1].len, dug);
                else
                    SYNCLOG("World: the leader (index %d) raiding player %d has gone round %d slabs without a way in, and"
                        " there is no hole in the ground known round the heart (%d chunks of it not known): it goes on round",
                        (int)creatng->index, (int)target, (int)g->round_total, unknown);
            }
        }
    }
    if (dir < 0)
        return false;
    *nx = cx + rg_dx[dir];
    *ny = cy + rg_dy[dir];
    world_slab_to_world(*nx, *ny, &g->leg_x, &g->leg_y);
    g->leg_set = 1;
    g->leg_turn = game.play_gameturn;
    return true;
}

static void rg_leg_pos(struct Thing *creatng, MapSlabCoord nx, MapSlabCoord ny, struct Coord3d *pos)
{
    pos->x.val = subtile_coord_center(slab_subtile_center(nx));
    pos->y.val = subtile_coord_center(slab_subtile_center(ny));
    pos->z.val = get_thing_height_at(creatng, pos);
}

/** The leader goes in on foot: the stock hero code has it from here. */
static void rg_walk_in(struct Thing *creatng, struct RaidGuide *g, PlayerNumber target, const char *how)
{
    if (rg_log_budget > 0)
    {
        rg_log_budget--;
        SYNCLOG("World: the leader (index %d) raiding player %d %s: it goes in on foot", (int)creatng->index, (int)target, how);
    }
    rg_drop_path(g);
    g->started = 0; /* (if it ever digs on, its line starts where it stands then; it keeps its hand) */
    g->leg_set = 0;
    internal_set_thing_state(creatng, CrSt_GoodDoingNothing);
}

/** Has the leader walked its way to a hole to the end? (It looks for the way in there at once.) */
static TbBool rg_at_end(const struct Thing *creatng, const struct RaidGuide *g, const struct CreatureControl *cctrl)
{
    if ((g->mode != RG_Path) || (g->path <= 0) || (g->path > RG_PATHS))
        return false;
    const struct RaidPath *p = &rg_paths[g->path - 1];
    return (p->thing == creatng->index) && (p->at >= p->len) &&
        (get_chessboard_distance(&creatng->mappos, &cctrl->moveto_pos) <= RG_ARRIVED);
}

/** The slab the leader was sent to and has not reached: where it is, if it is still its way (true). */
static TbBool rg_leg_pending(const struct Thing *creatng, struct RaidGuide *g, MapSlabCoord *lx, MapSlabCoord *ly)
{
    if (!g->leg_set || !world_engine_slab(g->leg_x, g->leg_y, lx, ly))
        return false;
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    if (abs(cx - *lx) + abs(cy - *ly) > 1)
        return false; /* it isn't beside it any more */
    if ((GameTurnDelta)(game.play_gameturn - g->leg_turn) > RG_LEG_TURNS)
    {
        /* it never got there: that slab is left alone for a while */
        g->avoid_x = g->leg_x;
        g->avoid_y = g->leg_y;
        g->avoid_until = game.play_gameturn + RG_AVOID_TURNS;
        if (rg_log_budget > 0)
        {
            rg_log_budget--;
            SYNCLOG("World: the leader (index %d) of a raid could not get onto a slab in %d turns: it leaves that slab alone",
                (int)creatng->index, RG_LEG_TURNS);
        }
        return false;
    }
    return rg_open(g, *lx, *ly);
}

short world_raid_leader_leg(struct Thing *creatng)
{
    PlayerNumber target;
    MapSlabCoord hx, hy;
    if (!rg_led(creatng, &target, &hx, &hy))
        return 0;
    struct RaidGuide *g = rg_of(creatng);
    if (g == NULL)
        return 0;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    cctrl->party.target_plyr_idx = target; /* (the hero AI may have looked elsewhere) */
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    /* a way to walk to the heart: looked for now and then; sooner beside one of the keeper's doors, on the keeper's
       ground and at the end of its way to a hole */
    const TbBool on_keepers = rg_keepers(cx, cy, target);
    TbBool by_door = false;
    for (int d = 0; d < 4; d++)
        by_door = by_door || rg_keepers_door(cx + rg_dx[d], cy + rg_dy[d], target);
    const TbBool at_end = rg_at_end(creatng, g, cctrl);
    const GameTurnDelta since = (GameTurnDelta)(game.play_gameturn - g->walk_turn);
    if ((g->walk_turn == 0) || (since >= RG_WALK_CHECK) || at_end || ((by_door || on_keepers) && (since >= RG_WALK_SOON)))
    {
        g->walk_turn = game.play_gameturn;
        if (rg_can_walk_in(creatng, target))
        {
            rg_walk_in(creatng, g, target, on_keepers ? "stands on the keeper's ground" : by_door ?
                "stands at a door on a way to the heart" : at_end ? "is at the hole" : "can walk to the heart");
            return 1;
        }
        if (on_keepers)
            g->no_entry_until = game.play_gameturn + RG_NO_ENTRY_TURNS;
    }
    /* on its way to the slab it was sent to: nothing to do until it stands there */
    MapSlabCoord lx, ly;
    if (rg_leg_pending(creatng, g, &lx, &ly))
    {
        const TbBool same = (subtile_slab(cctrl->moveto_pos.x.stl.num) == lx) && (subtile_slab(cctrl->moveto_pos.y.stl.num) == ly);
        if (same && (get_chessboard_distance(&creatng->mappos, &cctrl->moveto_pos) > RG_ARRIVED))
            return 2;
        if (!same && ((cx != lx) || (cy != ly)))
        {
            rg_leg_pos(creatng, lx, ly, &cctrl->moveto_pos); /* (sent elsewhere meanwhile: back on its way) */
            return 2;
        }
    }
    MapSlabCoord nx, ny;
    if (!rg_step(creatng, g, target, hx, hy, &nx, &ny))
        return 0;
    rg_leg_pos(creatng, nx, ny, &cctrl->moveto_pos);
    return 2;
}

/** Sends the leader on its way from an idle state (true: it tunnels again). */
static TbBool rg_send(struct Thing *creatng, struct RaidGuide *g, PlayerNumber target, MapSlabCoord hx, MapSlabCoord hy)
{
    MapSlabCoord nx, ny;
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    if (!rg_leg_pending(creatng, g, &nx, &ny) || ((cx == nx) && (cy == ny)))
    {
        if (!rg_step(creatng, g, target, hx, hy, &nx, &ny))
            return false;
    }
    struct Coord3d pos;
    rg_leg_pos(creatng, nx, ny, &pos);
    return send_tunneller_to_point_in_dungeon(creatng, target, &pos);
}

/** The leader and a fortified wall (2026-11-15, the user: "Tunnellers can break through fortified walls but only the
 *  ones outside of the rings of anchors against the dark"): when the next slab of its straight line to the heart is
 *  such a wall (and the line's other slab is not open), it breaks it (a minute, world_wallbreak.c) instead of going
 *  round. True while it works; when the wall is dug out its line starts again from where it stands and it goes on. */
static TbBool rg_break_wall(struct Thing *creatng, struct RaidGuide *g, MapSlabCoord hx, MapSlabCoord hy)
{
    if (!world_wallbreak_raids())
        return false; /* (off since 2026-11-22: a raid goes round to a hole or a door) */
    const MapSlabCoord cx = subtile_slab(creatng->mappos.x.stl.num), cy = subtile_slab(creatng->mappos.y.stl.num);
    int64_t wcx, wcy, wgx, wgy;
    world_slab_to_world(cx, cy, &wcx, &wcy);
    world_slab_to_world(hx, hy, &wgx, &wgy);
    int a, b;
    rg_line_steps(wcx, wcy, wcx, wcy, wgx, wgy, &a, &b);
    if (a < 0)
        return false;
    const TbBool a_open = rg_open(g, cx + rg_dx[a], cy + rg_dy[a]);
    const TbBool b_open = (b >= 0) && rg_open(g, cx + rg_dx[b], cy + rg_dy[b]);
    int d = -1;
    if (!a_open && world_wallbreak_can(cx + rg_dx[a], cy + rg_dy[a]))
        d = a;
    else if (!a_open && !b_open && (b >= 0) && world_wallbreak_can(cx + rg_dx[b], cy + rg_dy[b]))
        d = b;
    if (d < 0)
        return false;
    const int r = world_wallbreak_work(creatng, cx + rg_dx[d], cy + rg_dy[d]);
    if (r == 1)
        return true;
    if (r == 2)
    {
        rg_drop_path(g);
        g->mode = RG_Direct;
        g->line_x = wcx;
        g->line_y = wcy;
    }
    return false;
}

TbBool world_raid_leader_send(struct Thing *creatng)
{
    PlayerNumber target;
    MapSlabCoord hx, hy;
    if (!rg_led(creatng, &target, &hx, &hy))
        return false;
    struct RaidGuide *g = rg_of(creatng);
    if (g == NULL)
        return false;
    if (world_wallbreak_busy(creatng) || rg_break_wall(creatng, g, hx, hy) || world_wallbreak_busy(creatng))
        return true; /* at a wall it breaks: it stays (the last: it broke one just now and steps into the hole) */
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    cctrl->party.target_plyr_idx = target;
    if ((g->walk_turn == 0) || ((GameTurnDelta)(game.play_gameturn - g->walk_turn) >= RG_WALK_SOON) || rg_at_end(creatng, g, cctrl))
    {
        g->walk_turn = game.play_gameturn;
        if (rg_can_walk_in(creatng, target))
        {
            rg_walk_in(creatng, g, target, "can walk to the heart");
            return true;
        }
    }
    return rg_send(creatng, g, target, hx, hy);
}

TbBool world_raid_leader_resume(struct Thing *creatng)
{
    PlayerNumber target;
    MapSlabCoord hx, hy;
    if (!rg_led(creatng, &target, &hx, &hy))
        return false;
    struct RaidGuide *g = rg_of(creatng);
    if ((g != NULL) && world_wallbreak_busy(creatng))
        return true; /* at a wall it breaks: it stays */
    if ((g == NULL) || ((GameTurnDelta)(game.play_gameturn - g->walk_turn) < RG_WALK_CHECK))
        return false;
    g->walk_turn = game.play_gameturn;
    if (rg_can_walk_in(creatng, target))
        return false;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    cctrl->party.target_plyr_idx = target;
    if (!rg_send(creatng, g, target, hx, hy))
        return false;
    if (rg_log_budget > 0)
    {
        rg_log_budget--;
        SYNCLOG("World: the leader (index %d) raiding player %d has no way to walk to the heart: it digs on",
            (int)creatng->index, (int)target);
    }
    return true;
}

void world_raid_creature_died(struct Thing *thing)
{
    if (world_raid_hero(thing))
    {
        /* the raid's toll (dn_threat_outcome: a raid crushed makes the next one stronger) */
        const int tp = dn_raider_invasion(thing);
        if ((tp >= 0) && (tp < PLAYERS_MAX) && (dn_thr[tp].made > 0))
            dn_thr[tp].killed++;
    }
    if (!world_raid_hero(thing) || !creature_is_group_leader(thing))
        return;
    /* the leader of a raiding party died: the rest of the party goes home at once (2026-11-11) */
    const int p = dn_raider_invasion(thing);
    if (p < 0)
        return;
    int fled = 0;
    long guard = 0;
    struct Thing *memb = get_first_follower_creature_in_group(thing);
    while (thing_exists(memb) && (guard++ < CREATURES_COUNT))
    {
        struct CreatureControl *mctrl = creature_control_get_from_thing(memb);
        if (creature_control_invalid(mctrl))
            break;
        struct Thing *next = (mctrl->next_in_group > 0) ? thing_get(mctrl->next_in_group) : INVALID_THING;
        if ((memb != thing) && world_raid_hero(memb))
        {
            mctrl->party.original_objective = DN_FLEEING_MARK;
            mctrl->party.objective = CHeroTsk_DefendParty;
            fled++;
        }
        memb = next;
    }
    if (fled == 0)
        return;
    dn_inv[p].flee_turn = game.play_gameturn; /* (the chunk manager wants their landings from now on) */
    SYNCLOG("World: the leader (%s) of a party raiding player %d died: its %d heroes go home", thing_model_name(thing), p,
        fled);
}

TbBool world_raid_hero_retreating(const struct Thing *thing)
{
    if (!world_raid_hero(thing))
        return false;
    const int p = dn_raider_invasion(thing);
    return (p >= 0) && (dn_inv[p].retreating || dn_raider_fleeing(thing));
}

TbBool world_raid_hero_goes_home(struct Thing *thing)
{
    if (!world_raid_hero(thing))
        return false;
    const int p = dn_raider_invasion(thing);
    if ((p < 0) || (!dn_inv[p].retreating && !dn_raider_fleeing(thing)))
        return false;
    return dn_raider_go_home(thing, &dn_inv[p], true);
}

/** Every 16 turns: invasions end when their raiders are gone, turn back at the next dawn; raiders of an older save
 *  (no invasion yet) start one. */
static void dn_threat_outcome(PlayerNumber p);

static void dn_invasions_turn(long day, GameTurnDelta t)
{
    if ((game.play_gameturn % 16) != 5)
        return;
    static int count[PLAYERS_MAX];
    memset(count, 0, sizeof(count));
    /* turn back at the next dawn */
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        struct DnInvasion *inv = &dn_inv[p];
        if (inv->active && !inv->retreating && (day >= inv->day + DN_RETREAT_NIGHTS) && (t < dn_light(day)))
        {
            inv->retreating = 1;
            inv->retreat_turn = game.play_gameturn;
            /* (their landings are loaded by the chunk manager, which wants them from now on - see
               world_daynight_want_chunks - and no longer all at once here: every raid turns back on the same
               turn after dawn, which was up to nine chunks a landing in one turn; audit 2026-11-11) */
            SYNCLOG("World: the heroes raiding player %d since day %d go home (day %ld)", p, (int)inv->day, day);
        }
    }
    struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (dungeon_invalid(heroes))
        return;
    ThingIndex lists[2] = {heroes->creatr_list_start, heroes->digger_list_start};
    for (int l = 0; l < 2; l++)
    {
        long i = lists[l];
        long guard = 0;
        while ((i > 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing))
                break;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (!world_raid_hero(thing))
                continue;
            if (creature_is_kept_in_custody(thing) || creature_is_being_unconscious(thing))
                continue; /* a captive (or one lying senseless) doesn't keep the raid going */
            int p = dn_raider_invasion(thing);
            if (p < 0)
            {
                /* a raider from before the invasions were kept: it starts one for its target, landed here today */
                const PlayerNumber tp = cctrl->party.target_plyr_idx;
                if ((tp < 0) || (tp >= PLAYERS_COUNT) || !(world_player_is_human(tp) || world_player_is_keeper(tp)))
                    continue;
                struct DnInvasion *inv = &dn_inv[tp];
                memset(inv, 0, sizeof(*inv));
                inv->active = 1;
                inv->day = (int32_t)day;
                inv->nspots = 1;
                world_slab_to_world(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num),
                    &inv->spot_x[0], &inv->spot_y[0]);
                p = tp;
            }
            count[p]++;
            if (dn_inv[p].retreating || dn_raider_fleeing(thing))
                dn_raider_go_home(thing, &dn_inv[p], false);
        }
    }
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        if (dn_inv[p].active && (count[p] == 0) && !dn_late[p].active)
        {
            SYNCLOG("World: the raid on player %d (day %d) is over", p, (int)dn_inv[p].day);
            memset(&dn_inv[p], 0, sizeof(dn_inv[p]));
            dn_threat_outcome(p);
            world_progress_raid_over(p);
        } else if (dn_inv[p].active && (dn_thr[p].raid_army > 0.0f))
        {
            /* the heart's lowest health while the raid is on (it heals: at the end it could look untouched) */
            struct Thing *heart = find_players_dungeon_heart(p);
            if (thing_exists(heart) && ((int32_t)heart->index == dn_thr[p].heart_idx) && (heart->health < dn_thr[p].heart_min))
                dn_thr[p].heart_min = (int32_t)heart->health;
        }
    }
}

/** A raid's party lands at engine slab (sx, sy) for a player: n heroes (the first a Tunneller, its leader) of those
 *  models and levels, through the ceiling (the FALL spawn of scripts). Returns how many came; desc: "KIND level, ...". */
static int dn_land_party(PlayerNumber plyr_idx, MapSlabCoord sx, MapSlabCoord sy, int n, const ThingModel *models,
    const int *levels, struct DnRng *rng, char *desc, size_t desc_size)
{
    struct Thing *leader = INVALID_THING;
    int made = 0, dlen = 0;
    desc[0] = 0;
    for (int k = 0; k < n; k++)
    {
        if (!creature_count_below_map_limit(0))
        {
            WARNLOG("World: raid stopped at the creature limit");
            break;
        }
        const ThingModel model = thing_is_invalid(leader) ? dn_model("TUNNELLER") : models[k];
        if (model <= 0)
            continue;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(sx)) + (MapCoord)((int)dn_rand(rng, 193) - 96);
        pos.y.val = subtile_coord_center(slab_subtile_center(sy)) + (MapCoord)((int)dn_rand(rng, 193) - 96);
        pos.z.val = 0;
        struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, model,
            PLAYER_GOOD);
        if (thing_is_invalid(thing))
            continue;
        /* the FALL spawn of script commands: through the ceiling */
        thing->mappos.z.val = get_ceiling_height(&thing->mappos);
        create_effect(&thing->mappos, TngEff_CeilingBreach, thing->owner);
        initialise_thing_state(thing, CrSt_CreatureHeroEntering);
        set_flag(thing->rendering_flags, TRF_Invisible);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        cctrl->countdown = 24 + 8 * k;
        init_creature_level(thing, (CrtrExpLevel)(levels[k] - 1));
        cctrl->party.original_objective = CHeroTsk_AttackEnemies; /* marks raid heroes (world_raid_hero) */
        if (thing_is_invalid(leader))
        {
            /* the Tunneller leads, digging its way to the heart, like ADD_TUNNELLER_PARTY_TO_LEVEL */
            leader = thing;
            if (!script_support_send_tunneller_to_dungeon_heart(thing, plyr_idx))
                cctrl->party.objective = CHeroTsk_AttackEnemies;
            cctrl->party.target_plyr_idx = plyr_idx;
        } else
        {
            cctrl->party.objective = CHeroTsk_AttackEnemies;
            cctrl->party.target_plyr_idx = plyr_idx;
            add_creature_to_group(thing, leader);
        }
        made++;
        if (dlen < (int)desc_size - 24)
            dlen += snprintf(desc + dlen, desc_size - dlen, "%s%s %d", (made > 1) ? ", " : "", creature_code_name(model),
                (int)cctrl->exp_level + 1);
    }
    return made;
}

/** A party's price: each hero's fighting power (world_threat_power), as the army is measured. */
static double dn_party_cost(int n, const ThingModel *models, const int *levels)
{
    double c = 0.0;
    for (int k = 0; k < n; k++)
        c += world_threat_power(models[k], levels[k]);
    return c;
}


/** Each turn: the third parties whose time has come land (when their raid is still on). */
static void dn_late_turn(void)
{
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        struct DnLate *late = &dn_late[p];
        if (!late->active || (game.play_gameturn < late->at))
            continue;
        late->active = 0;
        if (world_safe_day() || world_pentagram_rest_on(p, world_day_number()))
        {
            SYNCLOG("World: a raid's late party doesn't land for player %d: a safe day", (int)p);
            continue;
        }
        if (dn_thr[p].raid_power > (float)late->cost)
            dn_thr[p].raid_power -= (float)late->cost; /* (planned with it: put back when it lands) */
        MapSlabCoord sx, sy;
        if (!dn_inv[p].active || dn_inv[p].retreating)
        {
            SYNCLOG("World: day %d raid %d's late party doesn't come for player %d: the raid is over", (int)late->day,
                late->idx + 1, (int)p);
            continue;
        }
        if (!world_engine_slab(late->wx, late->wy, &sx, &sy) && !world_engine_slab_unloaded(late->wx, late->wy, &sx, &sy))
        {
            SYNCLOG("World: day %d raid %d's late party doesn't come for player %d: its landing is out of the map",
                (int)late->day, late->idx + 1, (int)p);
            continue;
        }
        world_load_chunks_landing_world(late->wx, late->wy); /* (its chunk went out with nobody there meanwhile) */
        struct DnRng rng = {late->seed};
        char desc[200];
        const int made = dn_land_party(p, sx, sy, late->n, late->models, late->levels, &rng, desc, sizeof(desc));
        if (made == 0)
            continue;
        if (dn_inv[p].nspots < DN_MAX_PARTIES)
        {
            dn_inv[p].spot_x[dn_inv[p].nspots] = late->wx;
            dn_inv[p].spot_y[dn_inv[p].nspots] = late->wy;
            dn_inv[p].nspots++;
        }
        dn_thr[p].made = (int16_t)(dn_thr[p].made + made);
        dn_thr[p].raid_power += (float)late->cost;
        SYNCLOG("World: day %d raid %d's late party: %d heroes tunnel in at engine slab (%d,%d) for player %d: %s"
            " (power %.0f)", (int)late->day, late->idx + 1, made, (int)sx, (int)sy, (int)p, desc, late->cost);
    }
}

/** A raid on a player is over (its heroes all dead or gone): how it went sets the next raids' adjustment. */
static void dn_threat_outcome(PlayerNumber p)
{
    struct DnThreat *th = &dn_thr[p];
    if (th->raid_army <= 0.0f)
        return;
    struct WorldArmy army;
    world_threat_army(p, &army);
    double loss = ((double)th->raid_army - army.power) / (double)th->raid_army;
    if (loss < 0.0)
        loss = 0.0;
    if (loss > 1.0)
        loss = 1.0;
    struct Thing *heart = find_players_dungeon_heart(p);
    const TbBool fell = !thing_exists(heart) || ((int32_t)heart->index != th->heart_idx);
    const TbBool hurt = !fell && (th->heart_hp > 0) && (th->heart_min < th->heart_hp * 9 / 10);
    const double killed = (th->made > 0) ? (double)th->killed / (double)th->made : 0.0;
    const double before = dn_adjust(p);
    double a = before;
    const char *how;
    if (fell)
    {
        a = DN_ADJUST_RESTART;
        how = "the heart fell";
    } else if (hurt || (loss >= DN_LOSS_BIG))
    {
        a *= DN_ADJUST_DOWN;
        how = hurt ? "the heart was hurt" : "the army lost much";
    } else if ((loss <= DN_LOSS_SMALL) && (killed >= DN_KILLED_CRUSHED))
    {
        a *= DN_ADJUST_UP;
        how = "crushed";
    } else
        how = "held";
    if (a < DN_ADJUST_MIN)
        a = DN_ADJUST_MIN;
    if (a > DN_ADJUST_MAX)
        a = DN_ADJUST_MAX;
    world_pentagram_raid_over(p, (int)th->made, (int)th->killed, (double)th->raid_power, (double)th->raid_army); /* XP */
    SYNCLOG("World: the raid on player %d (power %.0f, %d heroes, %d killed) is over: army %.0f -> %.0f (%.0f%% lost), %s:"
        " adjustment %.2f -> %.2f", (int)p, (double)th->raid_power, (int)th->made, (int)th->killed, (double)th->raid_army,
        army.power, loss * 100.0, how, before, a);
    th->adjust = (float)a;
    th->raid_army = 0.0f;
    th->raid_power = 0.0f;
    th->heart_hp = th->heart_min = th->heart_idx = 0;
    th->made = th->killed = 0;
}

/** Whom raids come for: a human player's dungeon (not a co-op seat whose player is away, nor one at a World Boss
 *  fight), and an awake computer keeper's (the user, 2026-11-25: "Hero raids should work for Enemy keepers as well";
 *  a keeper asleep is not loaded, and its ground freezes). */
static TbBool dn_raid_target(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX))
        return false;
    if (world_player_is_human(p))
        return !world_coop_seat_away(p) && !world_boss_player_away(p);
    return world_player_is_keeper(p) && !world_keeper_is_passive(p) && thing_exists(find_players_dungeon_heart(p));
}

void world_raids_forget_player(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    memset(&dn_inv[p], 0, sizeof(dn_inv[p]));
    dn_owed_day[p] = 0;
    memset(&dn_army_wait[p], 0, sizeof(dn_army_wait[p]));
    memset(&dn_late[p], 0, sizeof(dn_late[p]));
    memset(&dn_thr[p], 0, sizeof(dn_thr[p]));
}

static void dn_raid(long day, int idx, PlayerNumber plyr_idx)
{
    if (world_safe_day())
    {
        SYNCLOG("World: day %ld raid %d doesn't come for player %d: a safe day", day, idx + 1, (int)plyr_idx);
        if (idx == 0)
            dn_owed_day[plyr_idx] = 0; /* (and isn't owed after it) */
        return;
    }
    if (world_pentagram_rest_on(plyr_idx, day))
    {
        SYNCLOG("World: day %ld raid %d doesn't come for player %d: its Day of Rest", day, idx + 1, (int)plyr_idx);
        if (idx == 0)
            dn_owed_day[plyr_idx] = 0;
        return;
    }
    if (dn_invasion_on(plyr_idx))
    {
        SYNCLOG("World: day %ld raid %d doesn't come for player %d: the last raid's heroes are still here%s", day, idx + 1,
            (int)plyr_idx, (idx == 0) ? " (it comes when they are gone)" : "");
        if (idx == 0)
            dn_owed_day[plyr_idx] = (int32_t)day; /* the day's first raid always comes (2026-11-11) */
        return;
    }
    {
        struct WorldArmy army;
        world_threat_army(plyr_idx, &army);
        if (army.count == 0)
        {
            SYNCLOG("World: day %ld raid %d waits for player %d: no creatures but Imps yet (it comes when one is there, today)",
                day, idx + 1, (int)plyr_idx);
            if (dn_army_wait[plyr_idx].day != (int32_t)day)
            {
                dn_army_wait[plyr_idx].day = (int32_t)day;
                dn_army_wait[plyr_idx].mask = 0;
            }
            dn_army_wait[plyr_idx].mask |= 1u << (idx & 31);
            if (idx == 0)
                dn_owed_day[plyr_idx] = 0;
            return;
        }
    }
    if (idx == 0)
        dn_owed_day[plyr_idx] = 0;
    struct DnRng rng = {dn_mix(world_seed() ^ ((uint64_t)day << 8) ^ (uint64_t)idx ^ 0x5A1D5A1D5A1DULL ^
        dn_player_salt(plyr_idx))};
    struct Thing *heartng = find_players_dungeon_heart(plyr_idx);
    if (!thing_exists(heartng) || (world_player_is_human(plyr_idx) && !world_human_heart_loaded(plyr_idx)))
    {
        SYNCLOG("World: day %ld raid %d skipped: the player's heart isn't loaded", day, idx + 1);
        return;
    }
    const MapSlabCoord hx = subtile_slab(heartng->mappos.x.stl.num), hy = subtile_slab(heartng->mappos.y.stl.num);
    /* the slabs to search: inside the window's frame, or in the chunked world everything within reach of the
     * heart, loaded or not (unloaded slabs are read from the chunk store or the generator) */
    MapSlabCoord bx1 = 1, by1 = 1, bx2 = game.map_tiles_x - 2, by2 = game.map_tiles_y - 2;
    if (world_is_chunked())
    {
        const MapSlabCoord reach = DN_SPAWN_REACH;
        bx1 = hx - reach;
        by1 = hy - reach;
        bx2 = hx + reach;
        by2 = hy + reach;
    }
    const int w = bx2 - bx1 + 1, h = by2 - by1 + 1;
    struct DnArea area = {bx1, by1, w, h, (uint8_t *)malloc((size_t)w * h), (uint8_t *)malloc((size_t)w * h), NULL};
    /* where to land: open ground at least DN_SPAWN_MIN_DIST out whose straight line to the heart crosses the
     * fewest slabs a Tunneller can't dig (big rock masses leave Tunnellers stuck: they can only dig round them),
     * and of those the closest to DN_SPAWN_DIST slabs (all within DN_SPAWN_SLACK of it count as equal) */
    int32_t *cand = (int32_t *)malloc(sizeof(int32_t) * (size_t)w * h);
    if ((cand == NULL) || (area.kind == NULL) || (area.mine == NULL) ||
        !world_read_slab_box(bx1, by1, bx2, by2, area.kind, area.mine, plyr_idx))
    {
        free(cand);
        free(area.kind);
        free(area.mine);
        free(area.reach);
        return;
    }
    /* another human keeper's floor is no landing either (audit 2026-11-21: a co-op partner's corridors 11 slabs from
       the heart were open ground, and the raid landed in the partner's dungeon). Loaded ground only: the slabs read
       from the store or the generator say just whose they are not. */
    for (MapSlabCoord y = by1; y <= by2; y++)
        for (MapSlabCoord x = bx1; x <= bx2; x++)
        {
            const size_t c = (size_t)(y - by1) * w + (size_t)(x - bx1);
            if ((area.kind[c] != SlbT_CLAIMED) || area.mine[c] || (world_is_chunked() && (wp_page_of_slab(x, y) == NULL)))
                continue;
            const struct SlabMap *slb = get_slabmap_block(x, y);
            const PlayerNumber o = slabmap_block_invalid(slb) ? PLAYER_NEUTRAL : slabmap_owner(slb);
            if ((o >= 0) && (o < PLAYERS_COUNT) && (o != plyr_idx) && (world_player_is_human(o) || world_player_is_keeper(o)))
                area.mine[c] = 1; /* (a computer keeper's floor too, 2026-11-25: raids come for keepers as well) */
        }
    dn_reach(&area, hx, hy); /* only where a Tunneller can get to the heart from */
    int ncand = 0, best_blockers = -1, best_off = 0;
    for (MapSlabCoord y = by1; y <= by2; y++)
    {
        for (MapSlabCoord x = bx1; x <= bx2; x++)
        {
            if (!dn_landing(&area, x, y) || !dn_reachable(&area, x, y))
                continue;
            const double d = sqrt((double)(x - hx) * (x - hx) + (double)(y - hy) * (y - hy));
            if (d < DN_SPAWN_MIN_DIST)
                continue;
            const int off = (int)max(0.0, fabs(d - DN_SPAWN_DIST) - DN_SPAWN_SLACK);
            if ((best_blockers == 0) && (off > best_off))
                continue; /* can't beat what we have: skip the line */
            const int b = dn_line_blockers(&area, x, y, hx, hy);
            if ((best_blockers < 0) || (b < best_blockers) || ((b == best_blockers) && (off < best_off)))
            {
                best_blockers = b;
                best_off = off;
                ncand = 0;
            }
            if ((b == best_blockers) && (off == best_off))
                cand[ncand++] = (y - by1) * w + (x - bx1);
        }
    }
    if (ncand <= 0)
    {
        free(cand);
        free(area.kind);
        free(area.mine);
        free(area.reach);
        SYNCLOG("World: day %ld raid %d skipped: no open ground at least %d slabs from the heart", day, idx + 1,
            DN_SPAWN_MIN_DIST);
        return;
    }
    /* the threat budget (2026-11-18): the whole army, not its best creature */
    struct WorldArmy army;
    world_threat_army(plyr_idx, &army);
    double power = army.power;
    int upper = army.upper, median = army.median;
    const PlayerNumber partner = world_coop_partner(plyr_idx);
    TbBool pair = false;
    if ((partner >= 0) && (partner < PLAYERS_COUNT) && (partner != plyr_idx) && world_coop_allies(plyr_idx, partner))
    {
        /* co-op: the party's two armies measured together and the budget shared between the two dungeons (it was: the
           heroes a level higher) */
        struct WorldArmy pa;
        world_threat_army(partner, &pa);
        power = (power + pa.power) / 2.0;
        upper = (upper + pa.upper + 1) / 2;
        median = (median + pa.median + 1) / 2;
        pair = true;
    }
    const long sday = world_seat_day(plyr_idx); /* the dungeon's own days */
    double curve = DN_CURVE_DAY1 + DN_CURVE_STEP * (double)(sday - 1);
    if (curve > DN_CURVE_TOP)
        curve = DN_CURVE_TOP;
    const double adjust = dn_adjust(plyr_idx);
    double budget = power * curve * adjust;
    /* endless (2026-11-27): the Pentagram. Thin the Ranks: raids sized from 95/90/85% of the army; Vigil: 25% smaller
       while the caretaker keeps the dungeon */
    budget = budget * (100 + world_pentagram_fx(plyr_idx, WFX_RAID_SIZE_PCT)) / 100.0;
    if (world_caretaker_active(plyr_idx))
        budget = budget * (100 - world_pentagram_fx(plyr_idx, WFX_VIGIL)) / 100.0;
    /* (no growth cap any more, 2026-11-22: "at most 25% more than the last raid" kept a fast-growing army's raids
       far below it - level 3 raiders against level 8 creatures; the curve and the adjustment set the budget) */
    const TbBool capped = false;
    const int tier = dn_tier_for(upper); /* which heroes may come: by the army's upper quarter */
    int want = (sday < DN_PARTIES_LATE_DAY) ? DN_PARTIES_EARLY : DN_PARTIES_LATE; /* landing spots: the most */
    {
        const char *v = getenv("KFX_WORLD_RAIDPARTIES"); /* test switch: this many at most, whatever the day */
        if ((v != NULL) && (atoi(v) >= 1) && (atoi(v) <= DN_PARTIES_LATE))
            want = atoi(v);
    }
    MapSlabCoord spot_x[DN_MAX_PARTIES], spot_y[DN_MAX_PARTIES];
    int nspots = 0;
    {
        /* the first party: one of the best spots, as raids always chose */
        const int k = (int)dn_rand(&rng, (uint32_t)ncand);
        spot_x[0] = bx1 + cand[k] % w;
        spot_y[0] = by1 + cand[k] / w;
        nspots = 1;
    }
    if (want > 1)
    {
        /* the others: the next best spots (fewest blockers on the way, then nearest DN_SPAWN_DIST out, then by
           chance), each DN_PARTY_GAP slabs from the spots taken */
        struct DnSpot { int32_t cell, score; uint32_t tie; };
        struct DnSpot *all = (struct DnSpot *)malloc(sizeof(struct DnSpot) * (size_t)w * h);
        int nall = 0;
        for (MapSlabCoord y = by1; (all != NULL) && (y <= by2); y++)
            for (MapSlabCoord x = bx1; x <= bx2; x++)
            {
                if (!dn_landing(&area, x, y) || !dn_reachable(&area, x, y))
                    continue;
                const double d = sqrt((double)(x - hx) * (x - hx) + (double)(y - hy) * (y - hy));
                if (d < DN_SPAWN_MIN_DIST)
                    continue;
                const int off = (int)max(0.0, fabs(d - DN_SPAWN_DIST) - DN_SPAWN_SLACK);
                all[nall].cell = (y - by1) * w + (x - bx1);
                all[nall].score = dn_line_blockers(&area, x, y, hx, hy) * 1000 + min(off, 999);
                all[nall].tie = dn_rand(&rng, 0x7FFFFFFF);
                nall++;
            }
        while ((all != NULL) && (nspots < want) && (nall > 0))
        {
            int bi = -1;
            for (int k = 0; k < nall; k++)
            {
                const MapSlabCoord sx = bx1 + all[k].cell % w, sy = by1 + all[k].cell / w;
                TbBool close = false;
                for (int s = 0; s < nspots; s++)
                    close = close || ((abs(sx - spot_x[s]) < DN_PARTY_GAP) && (abs(sy - spot_y[s]) < DN_PARTY_GAP));
                if (close)
                    continue;
                if ((bi < 0) || (all[k].score < all[bi].score) || ((all[k].score == all[bi].score) && (all[k].tie < all[bi].tie)))
                    bi = k;
            }
            if (bi < 0)
                break;
            spot_x[nspots] = bx1 + all[bi].cell % w;
            spot_y[nspots] = by1 + all[bi].cell / w;
            nspots++;
            all[bi] = all[--nall];
        }
        free(all);
    }
    free(cand);
    free(area.kind);
    free(area.mine);
    free(area.reach);
    struct DnInvasion inv;
    memset(&inv, 0, sizeof(inv));
    int made_all = 0, parties = 0;
    double spent = 0.0;
    dn_late[plyr_idx].active = 0;
    for (int s = 0; s < nspots; s++)
    {
        const MapSlabCoord sx = spot_x[s], sy = spot_y[s];
        const int biome = world_biome_at_engine_slab(sx, sy);
        struct DnPool pool;
        dn_make_pool(tier, biome, &pool);
        /* the party: as big as its share of what is left of the budget buys (2 to 6 heroes), a size smaller now and
           then for variety; its heroes and levels as the army's (dn_party_levels) */
        int levels[6];
        ThingModel models[6];
        dn_party_levels(&rng, 6, upper, median, sday, levels);
        models[0] = dn_model("TUNNELLER");
        for (int k = 1; k < 6; k++)
            models[k] = dn_pick_follower(&rng, &pool);
        const double left = budget - spent;
        const double share = left / (double)(nspots - s);
        int n = 6;
        double cost = dn_party_cost(n, models, levels);
        while ((n > 2) && (cost > share))
            cost = dn_party_cost(--n, models, levels);
        if ((n > 2) && (dn_rand(&rng, 4) == 0))
            cost = dn_party_cost(--n, models, levels);
        if ((cost > left) && (s == 0))
        {
            /* the first party always comes (a raid is never nothing): weaker until it fits, or all at level 1 */
            TbBool lowered = true;
            while ((cost > left) && lowered)
            {
                lowered = false;
                for (int k = 0; k < n; k++)
                    if (levels[k] > 1)
                    {
                        levels[k]--;
                        lowered = true;
                    }
                cost = dn_party_cost(n, models, levels);
            }
        } else if (cost > left)
            break; /* the budget is spent */
        spent += cost;
        parties++;
        if (s >= 2)
        {
            /* a third party comes later, from another side (the user, 2026-11-18: "Especially when 3 at a time start
               to spawn") */
            struct DnLate *late = &dn_late[plyr_idx];
            memset(late, 0, sizeof(*late));
            late->active = 1;
            late->at = game.play_gameturn + DN_LATE_TURNS + (GameTurn)dn_rand(&rng, DN_LATE_TURNS);
            world_slab_to_world(sx, sy, &late->wx, &late->wy);
            late->day = (int32_t)day;
            late->idx = idx;
            late->n = n;
            late->cost = cost;
            late->seed = rng.s ^ 0x1A7E1A7EULL;
            memcpy(late->models, models, sizeof(models));
            memcpy(late->levels, levels, sizeof(levels));
            SYNCLOG("World: day %ld raid %d party %d: %d heroes (power %.0f) land in %ld seconds", day, idx + 1, s + 1, n,
                cost, (long)((late->at - game.play_gameturn) / 20));
            continue;
        }
        /* the chunked world: the landing's own chunk comes in now and the eight round it in the next turns, first in
           the chunk manager's queue (all nine at once were a 30 to 70 ms turn; the heroes stand still for their first
           24 turns and more, and keep chunks loaded round them from then on) */
        const int landing_chunks = world_load_chunks_landing(sx, sy);
        if (landing_chunks > 0)
            SYNCLOG("World: day %ld raid %d: the chunk of landing %d loaded, the ones round it follow", day, idx + 1, s + 1);
        char desc[200];
        const int made = dn_land_party(plyr_idx, sx, sy, n, models, levels, &rng, desc, sizeof(desc));
        if (made == 0)
            continue;
        world_slab_to_world(sx, sy, &inv.spot_x[inv.nspots], &inv.spot_y[inv.nspots]);
        world_pentagram_raid_landed(plyr_idx, inv.spot_x[inv.nspots], inv.spot_y[inv.nspots]); /* endless: Night Watch */
        inv.nspots++;
        made_all += made;
        const long dist = (long)floor(sqrt((double)(sx - hx) * (sx - hx) + (double)(sy - hy) * (sy - hy)) + 0.5);
        SYNCLOG("World: day %ld raid %d party %d: %d heroes tunnel in at engine slab (%d,%d), %ld slabs from the heart,"
            " %s: %s (power %.0f)", day, idx + 1, s + 1, made, (int)sx, (int)sy, dist, wg_biome_name(biome), desc, cost);
    }
    SYNCLOG("World: day %ld raid %d on player %d: army %.0f (%d creatures; levels: middle %d, upper quarter %d, best %d)%s;"
        " the dungeon's day %ld: curve %.2f x adjustment %.2f = budget %.0f%s; %d parties, power %.0f", day, idx + 1,
        (int)plyr_idx, army.power, army.count, army.median, army.upper, army.best,
        pair ? ", measured with its party partner's" : "", sday, curve, adjust, budget,
        capped ? " (capped: at most 25% more than the last raid)" : "", parties, spent);
    if (made_all == 0)
    {
        dn_late[plyr_idx].active = 0;
        return;
    }
    {
        struct DnThreat *th = &dn_thr[plyr_idx];
        th->last_budget = (float)budget;
        th->raid_army = (float)((army.power > 1.0) ? army.power : 1.0);
        th->raid_power = (float)spent;
        th->heart_hp = (int32_t)heartng->health;
        th->heart_min = th->heart_hp;
        th->heart_idx = (int32_t)heartng->index;
        th->made = (int16_t)made_all;
        th->killed = 0;
    }
    if (world_player_is_human(plyr_idx))
        world_progress_raid(plyr_idx, day, idx, world_seed()); /* its book: only for a raid that came (audit 2026-11-09); a
                                                                   computer keeper's raid carries none */
    inv.active = 1;
    inv.day = (int32_t)day;
    dn_inv[plyr_idx] = inv;
    /* no message and no event: a raid is meant to be a surprise */
}

TbBool world_raid_hero(const struct Thing *thing)
{
    if (!world_mode_active() || (thing->class_id != TCls_Creature) || (thing->owner != PLAYER_GOOD))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    /* only day raids set an original objective in endless levels: fortress heroes keep the default */
    return !creature_control_invalid(cctrl) && ((cctrl->party.original_objective == CHeroTsk_AttackEnemies) ||
        (cctrl->party.original_objective == DN_FLEEING_MARK));
}

/* saving: the invasions on */
#pragma pack(1)
struct DnSaved {
    uint32_t plyr;
    int32_t day;
    uint8_t retreating, nspots;
    uint32_t retreat_age;              /* turns since it turned back */
    int64_t spot_x[DN_MAX_PARTIES], spot_y[DN_MAX_PARTIES];
};
#pragma pack()

size_t world_raids_save_size(void)
{
    size_t n = 0;
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (dn_inv[p].active)
            n++;
    return 4 + n * sizeof(struct DnSaved);
}

void world_raids_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *out = buf + 4;
    for (int p = 0; p < PLAYERS_MAX; p++)
    {
        const struct DnInvasion *inv = &dn_inv[p];
        if (!inv->active)
            continue;
        struct DnSaved s;
        memset(&s, 0, sizeof(s));
        s.plyr = (uint32_t)p;
        s.day = inv->day;
        s.retreating = inv->retreating;
        s.nspots = inv->nspots;
        s.retreat_age = inv->retreating ? (uint32_t)(game.play_gameturn - inv->retreat_turn) : 0;
        memcpy(s.spot_x, inv->spot_x, sizeof(s.spot_x));
        memcpy(s.spot_y, inv->spot_y, sizeof(s.spot_y));
        memcpy(out, &s, sizeof(s));
        out += sizeof(s);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_raids_load(const uint8_t *buf, size_t len)
{
    memset(dn_inv, 0, sizeof(dn_inv));
    uint32_t n;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if (n > (len - 4) / sizeof(struct DnSaved))
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        struct DnSaved s;
        memcpy(&s, buf + 4 + (size_t)i * sizeof(s), sizeof(s));
        if ((s.plyr >= PLAYERS_MAX) || (s.nspots > DN_MAX_PARTIES))
            continue;
        struct DnInvasion *inv = &dn_inv[s.plyr];
        inv->active = 1;
        inv->day = s.day;
        inv->retreating = s.retreating;
        inv->nspots = s.nspots;
        inv->retreat_turn = game.play_gameturn - s.retreat_age;
        inv->flee_turn = game.play_gameturn;
        memcpy(inv->spot_x, s.spot_x, sizeof(s.spot_x));
        memcpy(inv->spot_y, s.spot_y, sizeof(s.spot_y));
    }
    return true;
}

void world_raids_reset(void)
{
    rg_reset();
    memset(dn_inv, 0, sizeof(dn_inv));
    memset(dn_owed_day, 0, sizeof(dn_owed_day));
    memset(dn_army_wait, 0, sizeof(dn_army_wait));
}

/* The RDX1 extra of the world save: what RDS1 has no room for (audit 2026-11-11: after a load or a resync the day's
 * owed first raid never came, and fleeing raiders got their two minutes again). Per player with something to keep:
 * the player, the day whose first raid is owed (0 none), the turns since a party's leader died (UINT32_MAX none). */
#pragma pack(1)
struct DnExtraSaved {
    uint32_t plyr;
    int32_t owed_day;
    uint32_t flee_age;
};
#pragma pack()

static TbBool dn_extra_wanted(PlayerNumber p)
{
    return (dn_owed_day[p] != 0) || (dn_inv[p].active && !dn_inv[p].retreating && (dn_inv[p].flee_turn != 0));
}

void world_raids_extra_reset(void)
{
    memset(dn_owed_day, 0, sizeof(dn_owed_day)); /* (the raids themselves were read from RDS1 before this) */
    memset(dn_army_wait, 0, sizeof(dn_army_wait));
}

size_t world_raids_extra_save_size(void)
{
    size_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (dn_extra_wanted(p))
            n++;
    return (n > 0) ? 4 + n * sizeof(struct DnExtraSaved) : 0;
}

void world_raids_extra_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *at = buf + 4;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!dn_extra_wanted(p))
            continue;
        struct DnExtraSaved s;
        s.plyr = (uint32_t)p;
        s.owed_day = dn_owed_day[p];
        s.flee_age = (dn_inv[p].active && (dn_inv[p].flee_turn != 0)) ? (uint32_t)(game.play_gameturn - dn_inv[p].flee_turn) :
            UINT32_MAX;
        memcpy(at, &s, sizeof(s));
        at += sizeof(s);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_raids_extra_load(const uint8_t *buf, size_t len)
{
    world_raids_extra_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > PLAYERS_MAX) || (len < 4 + (size_t)n * sizeof(struct DnExtraSaved)))
        return false;
    for (uint32_t k = 0; k < n; k++)
    {
        struct DnExtraSaved s;
        memcpy(&s, buf + 4 + (size_t)k * sizeof(s), sizeof(s));
        if (s.plyr >= PLAYERS_MAX)
            continue;
        dn_owed_day[s.plyr] = s.owed_day;
        if ((s.flee_age != UINT32_MAX) && dn_inv[s.plyr].active)
            dn_inv[s.plyr].flee_turn = game.play_gameturn - s.flee_age;
    }
    return true;
}

/* The THR1 extra of the world save (2026-11-18): the threat budget's state per player (the adjustment from the last
 * raids, the last raid's budget for the growth cap, and the raid on now as it landed). A late party isn't saved. */
#pragma pack(1)
struct DnThreatSaved {
    uint32_t plyr;
    float adjust, last_budget, raid_army, raid_power;
    int32_t heart_hp, heart_min, heart_idx;
    int16_t made, killed;
};
#pragma pack()

static TbBool dn_threat_wanted(PlayerNumber p)
{
    return (dn_thr[p].adjust > 0.0f) || (dn_thr[p].last_budget > 0.0f) || (dn_thr[p].raid_army > 0.0f);
}

void world_raids_threat_reset(void)
{
    memset(dn_thr, 0, sizeof(dn_thr));
    memset(dn_late, 0, sizeof(dn_late));
}

size_t world_raids_threat_save_size(void)
{
    size_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (dn_threat_wanted(p))
            n++;
    return (n > 0) ? 4 + n * sizeof(struct DnThreatSaved) : 0;
}

void world_raids_threat_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *at = buf + 4;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!dn_threat_wanted(p))
            continue;
        const struct DnThreat *th = &dn_thr[p];
        struct DnThreatSaved s;
        memset(&s, 0, sizeof(s));
        s.plyr = (uint32_t)p;
        s.adjust = th->adjust;
        s.last_budget = th->last_budget;
        s.raid_army = th->raid_army;
        s.raid_power = th->raid_power;
        s.heart_hp = th->heart_hp;
        s.heart_min = th->heart_min;
        s.heart_idx = th->heart_idx;
        s.made = th->made;
        s.killed = th->killed;
        memcpy(at, &s, sizeof(s));
        at += sizeof(s);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_raids_threat_load(const uint8_t *buf, size_t len)
{
    world_raids_threat_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > PLAYERS_MAX) || (len < 4 + (size_t)n * sizeof(struct DnThreatSaved)))
        return false;
    for (uint32_t k = 0; k < n; k++)
    {
        struct DnThreatSaved s;
        memcpy(&s, buf + 4 + (size_t)k * sizeof(s), sizeof(s));
        if (s.plyr >= PLAYERS_MAX)
            continue;
        struct DnThreat *th = &dn_thr[s.plyr];
        th->adjust = s.adjust;
        th->last_budget = s.last_budget;
        th->raid_army = s.raid_army;
        th->raid_power = s.raid_power;
        th->heart_hp = s.heart_hp;
        th->heart_min = s.heart_min;
        th->heart_idx = s.heart_idx;
        th->made = s.made;
        th->killed = s.killed;
    }
    return true;
}

/** Test hook KFX_WORLD_THREATTEST=kill|hurt (the threat budget's adjustment): 200 turns after a raid on player 0 landed,
 *  kill: all its heroes die (the raid is crushed: the next one is stronger); hurt: the heart loses a fifth of its
 *  health (the next one is weaker). */
static void dn_threattest_hook(void)
{
    static int mode = -1; /* 0 off, 1 kill, 2 hurt */
    static int32_t seen_day = -1;
    static GameTurn landed = 0;
    if (mode < 0)
    {
        const char *v = getenv("KFX_WORLD_THREATTEST");
        mode = (v == NULL) ? 0 : (strcmp(v, "kill") == 0) ? 1 : (strcmp(v, "hurt") == 0) ? 2 : 0;
    }
    if ((mode == 0) || !dn_inv[PLAYER0].active || (dn_thr[PLAYER0].raid_army <= 0.0f))
        return;
    if (dn_inv[PLAYER0].day != seen_day)
    {
        seen_day = dn_inv[PLAYER0].day; /* a new raid (one a day at most while the last one's heroes stay) */
        landed = game.play_gameturn;
    }
    if (game.play_gameturn != landed + 200)
        return;
    if (mode == 2)
    {
        struct Thing *heart = find_players_dungeon_heart(PLAYER0);
        if (thing_exists(heart))
        {
            heart->health -= heart->health / 5;
            if (heart->health < dn_thr[PLAYER0].heart_min)
                dn_thr[PLAYER0].heart_min = (int32_t)heart->health;
            SYNCLOG("World threat test: player 0's heart hurt to %ld", (long)heart->health);
        }
        return;
    }
    struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (dungeon_invalid(heroes))
        return;
    int killed = 0;
    long guard = 0;
    for (ThingIndex i = heroes->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (world_raid_hero(thing) && (dn_raider_invasion(thing) == PLAYER0) && !creature_is_dying(thing))
        {
            kill_creature(thing, INVALID_THING, -1, CrDed_NoUnconscious);
            killed++;
        }
    }
    SYNCLOG("World threat test: %d of player 0's raiders killed", killed);
}

/** Test hook KFX_WORLD_KILLLEADER=<turns>: that long after it was made, the leader of a raiding party is killed (the
 *  party must go home). */
static void dn_killleader_hook(void)
{
    static long after = -1;
    if (after < 0)
    {
        const char *v = getenv("KFX_WORLD_KILLLEADER");
        after = (v != NULL) ? atol(v) : 0;
    }
    if ((after <= 0) || ((game.play_gameturn % 16) != 9))
        return;
    struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (dungeon_invalid(heroes))
        return;
    long i = heroes->creatr_list_start;
    TbBool diggers = false;
    long guard = 0;
    while (guard++ < CREATURES_COUNT)
    {
        if (i <= 0)
        {
            if (diggers)
                break;
            diggers = true;
            i = heroes->digger_list_start;
            continue;
        }
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing))
            break;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (world_raid_hero(thing) && creature_is_group_leader(thing) && !creature_is_dying(thing) &&
            ((long)(game.play_gameturn - thing->creation_turn) > after))
        {
            SYNCLOG("World test: killing the raid leader %s (index %d)", thing_model_name(thing), (int)thing->index);
            kill_creature(thing, INVALID_THING, -1, CrDed_NoUnconscious);
            return;
        }
    }
}

/** Test hook KFX_WORLD_RAIDWATCH="<every>[,<map every>[,<radius>]]": every <every> turns the log says where each
 *  raiding party's leader is and what it is doing; every <map every> turns it draws the ground round each leader,
 *  <radius> slabs each way (# rock, . earth, , torch earth, W the raided keeper's reinforced wall, w another wall,
 *  x a damaged wall, D/d a door, o the keeper's claimed floor, c other claimed floor, R/r a room, space a path,
 *  ~ water, ^ lava, g gold, G gems, + other diggable ground, % other solid ground; T the leader, h a raider,
 *  k one of the keeper's creatures, i one of its imps, H its heart). */
static char dn_watch_slab_char(MapSlabCoord x, MapSlabCoord y, PlayerNumber target)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return '?';
    const SlabKind k = slb->kind;
    const long own = slabmap_owner(slb);
    if (k == SlbT_ROCK)
        return '#';
    if (k == SlbT_GOLD)
        return 'g';
    if (k == SlbT_GEMS)
        return 'G';
    if (k == SlbT_EARTH)
        return '.';
    if (k == SlbT_TORCHDIRT)
        return ',';
    if (k == SlbT_DAMAGEDWALL)
        return 'x';
    if (slab_kind_is_fortified_wall(k))
        return (own == target) ? 'W' : 'w';
    if (slab_kind_is_door(k))
        return (own == target) ? 'D' : 'd';
    if (k == SlbT_LAVA)
        return '^';
    if (k == SlbT_WATER)
        return '~';
    if (k == SlbT_PATH)
        return ' ';
    if (k == SlbT_CLAIMED)
        return (own == target) ? 'o' : 'c';
    if (slab_kind_is_room(k))
        return (own == target) ? 'R' : 'r';
    const struct SlabConfigStats *st = get_slab_kind_stats(k);
    if ((st->block_flags & SlbAtFlg_Blocking) != 0)
        return ((st->block_flags & SlbAtFlg_Digable) != 0) ? '+' : '%';
    return '_';
}

static void dn_watch_mark(char *grid, int side, MapSlabCoord x0, MapSlabCoord y0, const struct Thing *thing, char c)
{
    const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num) - x0, y = subtile_slab(thing->mappos.y.stl.num) - y0;
    if ((x < 0) || (y < 0) || (x >= side) || (y >= side))
        return;
    char *at = &grid[y * (side + 1) + x];
    if ((*at != 'T') && ((*at != 'H') || (c == 'T')))
        *at = c;
}

static void dn_watch_mark_dungeon(char *grid, int side, MapSlabCoord x0, MapSlabCoord y0, PlayerNumber plyr_idx,
    char fighter, char digger, TbBool raiders_only)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    for (int list = 0; list < 2; list++)
    {
        long i = (list == 0) ? dungeon->creatr_list_start : dungeon->digger_list_start;
        long guard = 0;
        while ((i > 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing))
                break;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (raiders_only && !world_raid_hero(thing))
                continue;
            dn_watch_mark(grid, side, x0, y0, thing, (list == 0) ? fighter : digger);
        }
    }
}

/** Test hook KFX_WORLD_RAIDDOOR="<turn>[,<slabs>]": at that turn player 0's start gets a doorway in its west wall,
 *  two slabs north of the heart's row: the wall slab becomes claimed floor with a wooden door on it, and <slabs>
 *  slabs outside it become open path (0 when a raid has dug its ring there already: imps wall up a short path that
 *  leads nowhere within minutes). A raid going round the walls should stop at the door and break it. */
static void dn_raiddoor_hook(void)
{
    static long at = -2, stub = 0;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_RAIDDOOR");
        at = -1;
        if (v != NULL)
            sscanf(v, "%ld,%ld", &at, &stub);
    }
    if ((at < 0) || ((long)game.play_gameturn != at))
        return;
    const struct Thing *heartng = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heartng))
        return;
    const MapSlabCoord hx = subtile_slab(heartng->mappos.x.stl.num), hy = subtile_slab(heartng->mappos.y.stl.num);
    /* the first reinforced wall west of the heart on that row */
    MapSlabCoord wx = hx;
    const MapSlabCoord wy = hy - 2;
    for (int k = 1; k < 12; k++)
    {
        const struct SlabMap *slb = get_slabmap_block(hx - k, wy);
        if (!slabmap_block_invalid(slb) && slab_kind_is_fortified_wall(slb->kind))
        {
            wx = hx - k;
            break;
        }
    }
    if (wx == hx)
    {
        SYNCLOG("World test: raid door: no wall found west of the heart");
        return;
    }
    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(wx), slab_subtile_center(wy), PLAYER0, 0);
    for (long k = 1; k <= stub; k++)
        place_slab_type_on_map(SlbT_PATH, slab_subtile_center(wx - k), slab_subtile_center(wy), game.neutral_player_num, 0);
    script_place_door(PLAYER0, (ThingModel)get_id(door_desc, "WOOD"), wx, wy, false, true);
    const struct SlabMap *slb = get_slabmap_block(wx, wy);
    SYNCLOG("World test: raid door: a doorway %ld slabs west of the heart, 2 north: the slab is now %s of player %ld",
        (long)(hx - wx), slab_code_name(slb->kind), slabmap_owner(slb));
}

static void dn_raidwatch_hook(void)
{
    static long every = -1, map_every = 0, radius = 10;
    static int32_t last_x[CREATURES_COUNT], last_y[CREATURES_COUNT];
    static GameTurn since[CREATURES_COUNT];
    if (every < 0)
    {
        const char *v = getenv("KFX_WORLD_RAIDWATCH");
        every = 0;
        if (v != NULL)
            sscanf(v, "%ld,%ld,%ld", &every, &map_every, &radius);
        radius = max(2L, min(radius, 30L));
        if (every > 0)
            SYNCLOG("World test: watching raid leaders every %ld turns (a map every %ld, %ld slabs each way)", every,
                map_every, radius);
    }
    if ((every <= 0) || ((game.play_gameturn % every) != 0))
        return;
    struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (dungeon_invalid(heroes))
        return;
    for (int list = 0; list < 2; list++)
    {
        long i = (list == 0) ? heroes->creatr_list_start : heroes->digger_list_start;
        long guard = 0;
        while ((i > 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing))
                break;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (!world_raid_hero(thing) || creature_is_dying(thing))
                continue;
            /* a party's leader, or a raider on its own */
            if (!creature_is_group_leader(thing) && creature_is_group_member(thing))
                continue;
            const PlayerNumber target = cctrl->party.target_plyr_idx;
            const MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
            int64_t wx, wy;
            world_slab_to_world(sx, sy, &wx, &wy);
            long hdx = 0, hdy = 0;
            struct Thing *heartng = (target >= 0) ? find_players_dungeon_heart(target) : INVALID_THING;
            if (thing_exists(heartng))
            {
                hdx = sx - subtile_slab(heartng->mappos.x.stl.num);
                hdy = sy - subtile_slab(heartng->mappos.y.stl.num);
            }
            const int c = thing->index % CREATURES_COUNT;
            if ((last_x[c] != (int32_t)wx) || (last_y[c] != (int32_t)wy) || (since[c] == 0))
            {
                last_x[c] = (int32_t)wx;
                last_y[c] = (int32_t)wy;
                since[c] = game.play_gameturn;
            }
            const struct SlabMap *here = get_slabmap_block(sx, sy);
            const MapSubtlCoord bx = stl_num_decode_x(cctrl->navi.first_colliding_block),
                by = stl_num_decode_y(cctrl->navi.first_colliding_block);
            const struct SlabMap *blk = get_slabmap_for_subtile(bx, by);
            int followers = 0;
            {
                long g2 = 0;
                struct Thing *memb = creature_is_group_leader(thing) ? get_first_follower_creature_in_group(thing) :
                    INVALID_THING;
                while (thing_exists(memb) && (g2++ < CREATURES_COUNT))
                {
                    struct CreatureControl *mctrl = creature_control_get_from_thing(memb);
                    if (creature_control_invalid(mctrl))
                        break;
                    if (memb != thing)
                        followers++;
                    memb = (mctrl->next_in_group > 0) ? thing_get(mctrl->next_in_group) : INVALID_THING;
                }
            }
            SYNCLOG("World raidwatch: %s %d (level %d, %d followers) raiding player %d: %ld,%ld slabs from its heart, on"
                " %s of player %ld, on this slab for %ld turns; state %s then %s, instance %s, objective %d; goal %ld,%ld"
                " slabs from here; wallhug state %d side %d angle %d push %d retry %d; next step %ld,%ld from here;"
                " block to dig %ld,%ld subtiles from here: %s of player %ld",
                thing_model_name(thing), (int)thing->index, (int)cctrl->exp_level + 1, followers, (int)target, hdx, hdy,
                slab_code_name(here->kind), slabmap_owner(here), (long)(game.play_gameturn - since[c]),
                creature_state_code_name(thing->active_state), creature_state_code_name(thing->continue_state),
                creature_instance_code_name(cctrl->instance_id), (int)cctrl->party.objective,
                (long)subtile_slab(cctrl->moveto_pos.x.stl.num) - sx, (long)subtile_slab(cctrl->moveto_pos.y.stl.num) - sy,
                (int)cctrl->navi.navstate, (int)cctrl->navi.side, (int)cctrl->navi.angle, (int)cctrl->navi.push_counter,
                (int)cctrl->navi.wallhug_retry_counter,
                (long)cctrl->navi.pos_next.x.val - (long)thing->mappos.x.val,
                (long)cctrl->navi.pos_next.y.val - (long)thing->mappos.y.val,
                (long)bx - thing->mappos.x.stl.num, (long)by - thing->mappos.y.stl.num,
                slabmap_block_invalid(blk) ? "nothing" : slab_code_name(blk->kind),
                slabmap_block_invalid(blk) ? -1L : slabmap_owner(blk));
            {
                const long gi = thing->ccontrol_idx;
                const struct RaidGuide *g = ((gi > 0) && (gi < CREATURES_COUNT)) ? &rg_table[gi] : NULL;
                /* how much of the ground round it its keeper has seen (does a raid show the ground it crosses?) */
                int seen = 0;
                for (int ry = -3; ry <= 3; ry++)
                    for (int rx = -3; rx <= 3; rx++)
                    {
                        const struct Map *mb = get_map_block_at(slab_subtile_center(sx + rx), slab_subtile_center(sy + ry));
                        if (!map_block_invalid(mb) && (target >= 0) && map_block_revealed(mb, target))
                            seen++;
                    }
                if ((g != NULL) && (g->thing == thing->index) && g->started)
                    SYNCLOG("World raidled: %d %s, hand %s, %d slabs round, %d times round; its keeper has seen %d of the 49 slabs round it",
                        (int)thing->index, (g->mode == RG_Round) ? "going round" : (g->mode == RG_Path) ? "on its way to a hole" :
                        "straight for the heart", (g->hand == RG_HandRight) ? "right" : (g->hand == RG_HandLeft) ? "left" : "none",
                        (int)g->round_steps, (int)g->laps, seen);
            }
            if ((map_every <= 0) || ((game.play_gameturn % map_every) != 0))
                continue;
            const int side = (int)(2 * radius + 1);
            char *grid = (char *)malloc((size_t)(side + 1) * side);
            if (grid == NULL)
                continue;
            const MapSlabCoord x0 = sx - radius, y0 = sy - radius;
            for (int y = 0; y < side; y++)
            {
                for (int x = 0; x < side; x++)
                    grid[y * (side + 1) + x] = dn_watch_slab_char(x0 + x, y0 + y, target);
                grid[y * (side + 1) + side] = 0;
            }
            if (target >= 0)
                dn_watch_mark_dungeon(grid, side, x0, y0, target, 'k', 'i', false);
            dn_watch_mark_dungeon(grid, side, x0, y0, PLAYER_GOOD, 'h', 'h', true);
            if (thing_exists(heartng))
                dn_watch_mark(grid, side, x0, y0, heartng, 'H');
            dn_watch_mark(grid, side, x0, y0, thing, 'T');
            for (int y = 0; y < side; y++)
                SYNCLOG("World raidmap %d: |%s|", (int)thing->index, &grid[y * (side + 1)]);
            free(grid);
        }
    }
}

void world_raids_player_retreat(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    struct DnInvasion *inv = &dn_inv[plyr_idx];
    if (!inv->active || inv->retreating)
        return;
    inv->retreating = 1;
    inv->retreat_turn = game.play_gameturn;
    for (int s = 0; s < inv->nspots; s++)
        world_load_chunks_landing_world(inv->spot_x[s], inv->spot_y[s]);
    SYNCLOG("World: the heroes raiding player %d go home (its keeper starts again)", (int)plyr_idx);
}

/******************************************************************************/
/* Days per dungeon (2026-11-12): the clock is the world's, but each dungeon counts its own days from the day it
   began (again after Start again or a reset). The raids read them: how many may come in a day, the leader's
   level, the grade of the raid's book. A newcomer on the world's 31st day meets day-1 raids. */
static int32_t dn_seat_first_day[PLAYERS_MAX]; /* the world's day the dungeon began (0: the first) */

void world_daynight_seat_started(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    dn_seat_first_day[plyr_idx] = (int32_t)world_day_number();
    world_alarm_seat_started(plyr_idx);
    /* the threat budget starts again: no cap from the last dungeon's raids, and a dungeon here before (it fell, or was
       started again) begins gently */
    const TbBool had_raids = (dn_thr[plyr_idx].last_budget > 0.0f) || (dn_thr[plyr_idx].adjust > 0.0f);
    memset(&dn_thr[plyr_idx], 0, sizeof(dn_thr[plyr_idx]));
    dn_late[plyr_idx].active = 0;
    if (had_raids)
        dn_thr[plyr_idx].adjust = (float)DN_ADJUST_RESTART;
    SYNCLOG("World: player %d's dungeon counts its days from the world's day %d", (int)plyr_idx, (int)dn_seat_first_day[plyr_idx]);
}

long world_seat_day(PlayerNumber plyr_idx)
{
    const long day = world_day_number();
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (dn_seat_first_day[plyr_idx] <= 0))
        return day; /* a dungeon of a world saved before: as old as the world */
    return max(1L, day - (long)dn_seat_first_day[plyr_idx] + 1);
}

void world_seatdays_reset(void)
{
    memset(dn_seat_first_day, 0, sizeof(dn_seat_first_day));
}

size_t world_seatdays_save_size(void)
{
    return 4 + sizeof(dn_seat_first_day);
}

void world_seatdays_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, dn_seat_first_day, sizeof(dn_seat_first_day));
}

TbBool world_seatdays_load(const uint8_t *buf, size_t len)
{
    world_seatdays_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * 4 + 4 > len)
        return false;
    memcpy(dn_seat_first_day, buf + 4, 4 * (size_t)min((uint32_t)PLAYERS_MAX, n));
    return true;
}

void world_daynight_level_end(void)
{
    dn_dawn_save = false;
    world_raids_reset();
}

void world_daynight_after_turn(void)
{
    if (!dn_dawn_save)
        return;
    dn_dawn_save = false;
    world_autosave("dawn");
}

/** endless (2026-11-27): the Pentagram's Omen. At nightfall, tomorrow's raids for each keeper who has it (counted
 *  here, on the machine that has the world's seed; the dial of every machine reads them from struct Game). The first
 *  raid of a day always comes; the others one time in two. */
static void dn_omen(long day)
{
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        struct WorldPentagram *s = world_pentagram_state(p);
        if ((s == NULL) || (world_pentagram_fx(p, WFX_OMEN) <= 0))
            continue;
        int raids = 0;
        if (!dn_safe_day_at(day + 1) && !world_pentagram_rest_on(p, day + 1) && dn_raid_target(p))
        {
            const int n = dn_raids_on_day(world_seat_day(p) + 1) + world_season_extra_raids(p);
            for (int i = 0; i < n; i++)
                raids += dn_raid_happens(day + 1, i, p) ? 1 : 0;
        }
        s->omen = (uint16_t)((((uint32_t)(day + 1) & 0xFF) << 8) | (uint32_t)min(raids + 1, 255));
        SYNCLOG("World: Omen for player %d: %d raid%s on day %ld", (int)p, raids, (raids == 1) ? "" : "s", day + 1);
    }
}

void world_daynight_turn(void)
{
    dn_read_lengths();
    dn_raidwatch_hook(); /* test hooks */
    dn_raiddoor_hook();
    long day;
    GameTurnDelta t;
    dn_time(game.play_gameturn, &day, &t);
    if (t == 0)
    {
        SYNCLOG("World: day %ld begins", day);
        server_admin_log("world", "Day %ld begins", day);
        if (day > 1)
            dn_dawn_save = true;
    }
    else if (t == dn_light(day))
    {
        SYNCLOG("World: night %ld begins", day);
        server_admin_log("world", "Night %ld begins", day);
        dn_omen(day);
    }
    dn_invasions_turn(day, t);
    dn_late_turn();
    dn_killleader_hook();
    dn_threattest_hook();
    world_boss_turn();
    world_truce_turn();
    world_defectors_turn();
    world_crusade_turn();
    world_alarm_turn();
    if (t >= dn_light(day))
    {
        for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
        {
            if (dn_army_wait[p].mask != 0)
                SYNCLOG("World: day %ld's waiting raids don't come for player %d: night fell before it had a creature", day, (int)p);
            dn_army_wait[p].mask = 0;
        }
        return;
    }
    /* a first raid that waited for the last raid's heroes to go: it comes now, if its time has passed */
    if ((game.play_gameturn % 16) == 13)
    {
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            if ((dn_owed_day[p] != (int32_t)day) || dn_invasion_on(p))
                continue;
            if (!dn_raid_target(p))
            {
                dn_owed_day[p] = 0;
                continue;
            }
            if (t > dn_light(day) - dn_night_clear(day))
            {
                /* too near nightfall: the night's packs come then (the user, 2026-11-18: "Raiders can spawn on the last
                   minute before night falls") */
                SYNCLOG("World: day %ld's first raid doesn't come for player %d: too near nightfall", day, (int)p);
                dn_owed_day[p] = 0;
                continue;
            }
            SYNCLOG("World: day %ld's first raid comes for player %d now: the last raid's heroes are gone", day, (int)p);
            dn_raid(day, 0, p);
            if (!dn_invasion_on(p))
                dn_owed_day[p] = 0; /* it couldn't land: not again today */
        }
    }
    /* raids that waited for the dungeon's first creature: one now, if it has one (and no raid is on) */
    if ((game.play_gameturn % 16) == 9)
    {
        for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
        {
            struct DnArmyWait *aw = &dn_army_wait[p];
            if (aw->mask == 0)
                continue;
            if ((aw->day != (int32_t)day) || !dn_raid_target(p))
            {
                aw->mask = 0;
                continue;
            }
            if (dn_invasion_on(p) || ((aw->last != 0) && ((GameTurnDelta)(game.play_gameturn - aw->last) < DN_RAID_GAP)))
                continue;
            struct WorldArmy army;
            world_threat_army(p, &army);
            if (army.count == 0)
                continue;
            int idx = 0;
            while ((idx < 31) && ((aw->mask & (1u << idx)) == 0))
                idx++;
            aw->mask &= ~(1u << idx);
            aw->last = game.play_gameturn;
            SYNCLOG("World: day %ld raid %d comes for player %d now: the dungeon has %d creature%s", day, idx + 1, (int)p,
                army.count, (army.count == 1) ? "" : "s");
            dn_raid(day, idx, p);
        }
    }
    /* by day, read the raid search box ahead a few chunks at a time (the first raid of a run would otherwise
       generate about 200 chunks in one turn); it changes nothing in the game */
    if (world_is_chunked() && ((game.play_gameturn % 4) == 2))
    {
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            struct Thing *heartng = dn_raid_target(p) ? find_players_dungeon_heart(p) : INVALID_THING;
            if (!thing_exists(heartng))
                continue;
            const MapSlabCoord hx = subtile_slab(heartng->mappos.x.stl.num), hy = subtile_slab(heartng->mappos.y.stl.num);
            const MapSlabCoord reach = DN_SPAWN_REACH;
            world_peek_warm(hx - reach, hy - reach, hx + reach, hy + reach, 2);
        }
    }
    /* every human player gets its day's raids (as many as its dungeon's own days allow), each at its own time in the
       raid's part of the day, rolled on its own; and every awake computer keeper (2026-11-25) */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!dn_raid_target(p))
            continue; /* (a co-op dungeon whose player is away is frozen: no raid comes for it; nor for a keeper at a
                         World Boss fight, nor for a computer keeper asleep) */
        const int n = dn_raids_on_day(world_seat_day(p)) + world_season_extra_raids(p); /* the Swelter: one more */
        for (int i = 0; i < n; i++)
        {
            if (dn_raid_time_for(day, i, n, p) != t)
                continue;
            if (dn_raid_happens(day, i, p))
                dn_raid(day, i, p);
            else if (p == PLAYER0)
                SYNCLOG("World: day %ld raid %d doesn't come (%d%% chance)", day, i + 1, dn_raid_chance);
            else
                SYNCLOG("World: day %ld raid %d doesn't come for player %d (%d%% chance)", day, i + 1, (int)p,
                    dn_raid_chance);
        }
    }
}

/** The chunks the day's raids keep loaded: the landings of raids whose heroes walk back to them. */
void world_daynight_want_chunks(void (*want)(int64_t wx, int64_t wy, int r))
{
    if (!world_mode_active())
        return;
    world_boss_want_chunks(want); /* a World Boss's arena while a fight is on */
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        /* a late party's landing: in, ten seconds before it lands */
        if (dn_late[p].active && ((GameTurnDelta)(dn_late[p].at - game.play_gameturn) <= 10 * 20))
            want(dn_late[p].wx, dn_late[p].wy, 1);
        const struct DnInvasion *inv = &dn_inv[p];
        /* going home: the whole raid after dawn, or a party whose leader died not long ago */
        const TbBool fleeing = (inv->flee_turn != 0) && ((GameTurnDelta)(game.play_gameturn - inv->flee_turn) <= DN_RETREAT_TURNS);
        if (!inv->active || (!inv->retreating && !fleeing))
            continue;
        for (int s = 0; s < inv->nspots; s++)
            want(inv->spot_x[s], inv->spot_y[s], 1);
    }
}

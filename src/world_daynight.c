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
 * By day, heroes break in through the ceiling (the FALL spawn of scripts) at open spots about 100 slabs from the
 * player's heart: a Tunneller that digs its way to the heart, followed by Dwarves, Thieves and Tunnellers (2-6
 * in all). Each possible raid comes one time in three. Their levels follow the player's best creature.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_daynight.h"
#include "world_daynight_art.h"
#include "world_stream.h"
#include "world_gen.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_math.h"
#include "bflib_sprite.h"
#include "bflib_video.h"
#include "config.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_groups.h"
#include "creature_states.h"
#include "creature_states_hero.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "world_menu.h"
#include "world_pages.h"
#include "net_game.h"
#include "world_progress.h"
#include "server_admin.h"
#include "post_inc.h"

/******************************************************************************/
/* The clock */

#define DN_DAY_TURNS_DEFAULT   (12 * 60 * 20)
#define DN_NIGHT_TURNS_DEFAULT (8 * 60 * 20)

static GameTurnDelta dn_day_turns = DN_DAY_TURNS_DEFAULT;
static GameTurnDelta dn_night_turns = DN_NIGHT_TURNS_DEFAULT;
/* each possible raid of a day happens with this chance, in percent (KFX_WORLD_RAIDCHANCE changes it for tests) */
#define DN_RAID_CHANCE 33
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

TbBool world_is_day(void)
{
    long day;
    GameTurnDelta t;
    dn_read_lengths();
    dn_time(game.play_gameturn, &day, &t);
    return t < dn_day_turns;
}

/** The disc's turn, clockwise in radians: -pi/2 at sunrise (sun on the left), 0 at noon (sun on top),
 *  pi/2 at sunset, pi at midnight (moon on top). */
static double dn_disc_angle(void)
{
    long day;
    GameTurnDelta t;
    dn_time(game.play_gameturn, &day, &t);
    if (t < dn_day_turns)
        return M_PI * (-0.5 + (double)t / (double)dn_day_turns);
    return M_PI * (0.5 + (double)(t - dn_day_turns) / (double)dn_night_turns);
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

void world_daynight_draw(void)
{
    if (!world_mode_active())
        return;
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
    const int upp = units_per_pixel;
    const int size = DAYNIGHT_RIM * upp / 16;
    const int margin = 6 * upp / 16;
    const int x = MyScreenWidth - size - margin;
    const int y = margin;
    const int off = ((DAYNIGHT_RIM - DAYNIGHT_DISC) / 2) * upp / 16;
    UIRenderer_SubmitPanelSpriteRaw(x + off, y + off, upp, &dn_frame_spr[k], 0);
    UIRenderer_SubmitPanelSpriteRaw(x, y, upp, &dn_rim_spr, 0);
}

/******************************************************************************/
/* Raids by day */

/* party size 2..6: the bigger, the rarer */
static const int dn_party_weights[5] = {30, 30, 22, 12, 6};

struct DnHero {
    const char *name;
    int weight;
};

/* a raid is a Tunneller digging its way in, followed by these (Tunnellers dig, so they can follow anywhere) */
static const struct DnHero dn_followers[3] = {{"DWARFA", 4}, {"THIEF", 3}, {"TUNNELLER", 2}};


#define DN_SPAWN_DIST 100
#define DN_SPAWN_SLACK 5
#define DN_SPAWN_MIN_DIST 40

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

/** Possible tunnel-ins on a day: 1 on day 1, one more every 2 days, at most 4 (each comes one time in three). */
static int dn_raids_on_day(long day)
{
    long n = 1 + (day - 1) / 2;
    return (n > 4) ? 4 : (int)n;
}

/** Time of day of raid i of n: one in each part of the day, at a time fixed by the seed. The first day's raid
 *  waits for its second half; no raid starts in the first or last twentieth of a day. */
static GameTurnDelta dn_raid_time(long day, int i, int n)
{
    const GameTurnDelta edge = dn_day_turns / 20;
    const GameTurnDelta start = (day == 1) ? dn_day_turns / 2 : edge;
    GameTurnDelta slot = (dn_day_turns - edge - start) / n;
    if (slot < 1)
        slot = 1;
    const uint64_t h = dn_mix(world_seed() ^ dn_mix((uint64_t)day * 0x632BE59BD9B4E019ULL + (uint64_t)i));
    return start + slot * i + (GameTurnDelta)(h % (uint64_t)slot);
}

/** The slabs the landing search reads: a box of engine slabs (world_read_slab_box), loaded or not. */
struct DnArea {
    MapSlabCoord x1, y1;
    int w, h;
    uint8_t *kind;
    uint8_t *mine; /* the player's slab */
};

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
        if ((kind == SlbT_ROCK) || (kind == SlbT_GEMS) || (kind == SlbT_LAVA))
            blockers++;
    }
    return blockers;
}

/** A player's best creature level (1-based), imps included. */
static int dn_keeper_best_level(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    int best = 1;
    if (dungeon_invalid(dungeon))
        return best;
    ThingIndex lists[2] = {dungeon->creatr_list_start, dungeon->digger_list_start};
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
            if (cctrl->exp_level + 1 > best)
                best = cctrl->exp_level + 1;
        }
    }
    return best;
}

static ThingModel dn_model(const char *name)
{
    const long model = get_id(creature_desc, name);
    return (model > 0) ? (ThingModel)model : 0;
}

static ThingModel dn_pick_follower(struct DnRng *rng)
{
    int total = 0;
    for (int i = 0; i < 3; i++)
        total += dn_followers[i].weight;
    int r = (int)dn_rand(rng, (uint32_t)total), i = 0;
    while ((i < 2) && (r >= dn_followers[i].weight))
    {
        r -= dn_followers[i].weight;
        i++;
    }
    return dn_model(dn_followers[i].name);
}

/** Does possible raid i of a day come? Fixed by the seed: DN_RAID_CHANCE percent of them do. */
/** Each human player gets their own raids; PLAYER0's dice are those of a single-player run. */
static uint64_t dn_player_salt(PlayerNumber plyr_idx)
{
    return (uint64_t)plyr_idx * 0x9E3779B97F4A7C15ULL;
}

static TbBool dn_raid_happens(long day, int i, PlayerNumber plyr_idx)
{
    const uint64_t h = dn_mix(world_seed() ^ dn_mix((uint64_t)day * 0x2545F4914F6CDD1DULL + (uint64_t)i + 77) ^
        dn_player_salt(plyr_idx));
    return (int)(h % 100) < dn_raid_chance;
}

/* creature_states_hero.c, not in a header */
TbBool script_support_send_tunneller_to_dungeon_heart(struct Thing *creatng, PlayerNumber plyr_idx);

/** Levels for a party of n: the leader at best + 1 + 1 per 3 days survived, the others spread evenly from just
 *  under best down to half of it, each moved by -1, 0 or +1. */
static void dn_party_levels(struct DnRng *rng, int n, int best, long day, int *levels)
{
    levels[0] = best + 1 + (int)((day - 1) / 3);
    const int m = n - 1;
    const double low = best / 2.0;
    for (int j = 1; j <= m; j++)
        levels[j] = (int)floor(low + (best - low) * (double)(m - j) / (double)m + 0.5);
    for (int j = 0; j < n; j++)
    {
        levels[j] += (int)dn_rand(rng, 3) - 1;
        if (levels[j] < 1)
            levels[j] = 1;
        if (levels[j] > DN_LEVEL_CAP)
            levels[j] = DN_LEVEL_CAP;
    }
}

static void dn_raid(long day, int idx, PlayerNumber plyr_idx)
{
    world_progress_raid(plyr_idx, day, idx, world_seed());
    struct DnRng rng = {dn_mix(world_seed() ^ ((uint64_t)day << 8) ^ (uint64_t)idx ^ 0x5A1D5A1D5A1DULL ^
        dn_player_salt(plyr_idx))};
    struct Thing *heartng = find_players_dungeon_heart(plyr_idx);
    if (!thing_exists(heartng) || !world_human_heart_loaded(plyr_idx))
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
        const MapSlabCoord reach = DN_SPAWN_DIST + DN_SPAWN_SLACK;
        bx1 = hx - reach;
        by1 = hy - reach;
        bx2 = hx + reach;
        by2 = hy + reach;
    }
    const int w = bx2 - bx1 + 1, h = by2 - by1 + 1;
    struct DnArea area = {bx1, by1, w, h, (uint8_t *)malloc((size_t)w * h), (uint8_t *)malloc((size_t)w * h)};
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
        return;
    }
    int ncand = 0, best_blockers = -1, best_off = 0;
    for (MapSlabCoord y = by1; y <= by2; y++)
    {
        for (MapSlabCoord x = bx1; x <= bx2; x++)
        {
            if (!dn_landing(&area, x, y))
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
    const int32_t spot = (ncand > 0) ? cand[dn_rand(&rng, (uint32_t)ncand)] : -1;
    free(cand);
    free(area.kind);
    free(area.mine);
    if (spot < 0)
    {
        SYNCLOG("World: day %ld raid %d skipped: no open ground at least %d slabs from the heart", day, idx + 1,
            DN_SPAWN_MIN_DIST);
        return;
    }
    const MapSlabCoord sx = bx1 + spot % w, sy = by1 + spot / w;
    /* the chunked world: the landing chunks come in now; the heroes keep chunks loaded round them from then on */
    const int landing_chunks = world_load_chunks_round(sx, sy, 1);
    if (landing_chunks > 0)
        SYNCLOG("World: day %ld raid %d: %d chunks loaded round the landing", day, idx + 1, landing_chunks);
    const int biome = world_biome_at_engine_slab(sx, sy);
    /* the party */
    int total = 0;
    for (int i = 0; i < 5; i++)
        total += dn_party_weights[i];
    int r = (int)dn_rand(&rng, (uint32_t)total), n = 2;
    for (int i = 0; i < 5; i++)
    {
        if (r < dn_party_weights[i])
        {
            n = 2 + i;
            break;
        }
        r -= dn_party_weights[i];
    }
    int levels[6];
    const int best = dn_keeper_best_level(plyr_idx);
    dn_party_levels(&rng, n, best, day, levels);
    struct Thing *leader = INVALID_THING;
    int made = 0;
    char desc[160];
    int dlen = 0;
    desc[0] = 0;
    for (int k = 0; k < n; k++)
    {
        if (!creature_count_below_map_limit(0))
        {
            WARNLOG("World: raid stopped at the creature limit");
            break;
        }
        const ThingModel model = thing_is_invalid(leader) ? dn_model("TUNNELLER") : dn_pick_follower(&rng);
        if (model <= 0)
            continue;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(sx)) + (MapCoord)((int)dn_rand(&rng, 193) - 96);
        pos.y.val = subtile_coord_center(slab_subtile_center(sy)) + (MapCoord)((int)dn_rand(&rng, 193) - 96);
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
        } else
        {
            cctrl->party.objective = CHeroTsk_AttackEnemies;
            cctrl->party.target_plyr_idx = plyr_idx;
            add_creature_to_group(thing, leader);
        }
        made++;
        if (dlen < (int)sizeof(desc) - 24)
            dlen += snprintf(desc + dlen, sizeof(desc) - dlen, "%s%s %d", (made > 1) ? ", " : "",
                creature_code_name(model), (int)cctrl->exp_level + 1);
    }
    if (made == 0)
        return;
    const long dist = (long)floor(sqrt((double)(sx - hx) * (sx - hx) + (double)(sy - hy) * (sy - hy)) + 0.5);
    SYNCLOG("World: day %ld raid %d: %d heroes tunnel in at engine slab (%d,%d), %ld slabs from the heart, %s: %s"
        " (player's best level %d)", day, idx + 1, made, (int)sx, (int)sy, dist, wg_biome_name(biome), desc, best);
    /* no message and no event: a raid is meant to be a surprise */
}

TbBool world_raid_hero(const struct Thing *thing)
{
    if (!world_mode_active() || (thing->class_id != TCls_Creature) || (thing->owner != PLAYER_GOOD))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    /* only day raids set an original objective in endless levels: fortress heroes keep the default */
    return !creature_control_invalid(cctrl) && (cctrl->party.original_objective == CHeroTsk_AttackEnemies);
}

void world_daynight_level_end(void)
{
    dn_dawn_save = false;
}

void world_daynight_after_turn(void)
{
    if (!dn_dawn_save)
        return;
    dn_dawn_save = false;
    world_autosave("dawn");
}

void world_daynight_turn(void)
{
    dn_read_lengths();
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
    else if (t == dn_day_turns)
    {
        SYNCLOG("World: night %ld begins", day);
        server_admin_log("world", "Night %ld begins", day);
    }
    if (t >= dn_day_turns)
        return;
    /* by day, read the raid search box ahead a few chunks at a time (the first raid of a run would otherwise
       generate about 200 chunks in one turn); it changes nothing in the game */
    if (world_is_chunked() && ((game.play_gameturn % 4) == 2))
    {
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            struct Thing *heartng = world_player_is_human(p) ? find_players_dungeon_heart(p) : INVALID_THING;
            if (!thing_exists(heartng))
                continue;
            const MapSlabCoord hx = subtile_slab(heartng->mappos.x.stl.num), hy = subtile_slab(heartng->mappos.y.stl.num);
            const MapSlabCoord reach = DN_SPAWN_DIST + DN_SPAWN_SLACK;
            world_peek_warm(hx - reach, hy - reach, hx + reach, hy + reach, 2);
        }
    }
    const int n = dn_raids_on_day(day);
    for (int i = 0; i < n; i++)
    {
        if (dn_raid_time(day, i, n) != t)
            continue;
        /* every human player gets the day's raids at the same times, each rolled on its own */
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            if (!world_player_is_human(p))
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

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
 * player's heart: parties of a Tunneller that digs its way to the heart and its followers (2-6 in all). The first
 * raid of a day always comes, the day's others one time in two (2026-11-11), at least four minutes apart. The player's best creature sets their levels, how many parties land at once and who
 * follows (2026-11-08: 1 party up to level 3, 1-2 to level 5, 1-3 to level 8, 2-4 to level 12, then 3-5; Thieves
 * and Tunnellers, then Dwarves, Archers, Barbarians, Monks, and from level 9 the heroes of the landing's biome).
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
#include "world_gen.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
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
#include "thing_navigate.h"
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

void world_daynight_draw(void)
{
    if (world_attract_active())
        return; /* the launcher's background: the picture alone */
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
    world_seasons_draw_ring(x, y, size); /* the year round the dial */
    world_boss_draw(x, y + size + 2 * upp / 16, size); /* the World Boss timer, under the dial */
}

/******************************************************************************/
/* Raids by day */

/* party size 2..6: the bigger, the rarer */
static const int dn_party_weights[5] = {30, 30, 22, 12, 6};

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

#define DN_SPAWN_DIST 100
#define DN_SPAWN_SLACK 5
#define DN_SPAWN_MIN_DIST 40
#define DN_PARTY_GAP 8               /* slabs at least between two parties' landings */
#define DN_MAX_PARTIES 5
#define DN_RETREAT_NIGHTS 1          /* raiders go home at the next dawn (2026-11-11; it was after their third night) */
#define DN_RAID_GAP (4 * 60 * 20)    /* turns at least between two raids of a day on a player, where the day has room */
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

/** Possible tunnel-ins on a day (2026-11-11): 1 on day 1, 2 to day 4, 3 to day 8, then 4. The first always comes,
 *  the others one time in two. */
static int dn_raids_on_day(long day)
{
    return (day <= 1) ? 1 : (day < 5) ? 2 : (day < 9) ? 3 : 4;
}

/** Time of day of raid i of n: one in each part of the day, at a time fixed by the seed. The first day's raid
 *  waits for its second half; no raid starts in the first or last twentieth of a day. */
static uint64_t dn_player_salt(PlayerNumber plyr_idx);

/** Time of raid i for a player: each player's at its own time in the slot (all of them landed in one turn, each
 *  loading its landing chunks there; audit 2026-11-09). Player 0 keeps the time it always had. */
static GameTurnDelta dn_raid_time_for(long day, int i, int n, PlayerNumber plyr_idx);

static GameTurnDelta dn_raid_time_for(long day, int i, int n, PlayerNumber plyr_idx)
{
    const GameTurnDelta edge = dn_light(day) / 20;
    const GameTurnDelta start = (day == 1) ? dn_light(day) / 2 : edge;
    const GameTurnDelta span = dn_light(day) - edge - start;
    /* breathing room (2026-11-11): DN_RAID_GAP between two raids, or as much as the day has room for (four raids
       don't fit four minutes apart in a 12-minute day); what is left over is shared out as each raid's leeway */
    GameTurnDelta gap = 0;
    if (n > 1)
    {
        gap = DN_RAID_GAP;
        const GameTurnDelta most = span * 4 / 5 / (n - 1);
        if (gap > most)
            gap = most;
    }
    GameTurnDelta slot = (span - gap * (n - 1)) / n;
    if (slot < 1)
        slot = 1;
    uint64_t h = dn_mix(world_seed() ^ dn_mix((uint64_t)day * 0x632BE59BD9B4E019ULL + (uint64_t)i));
    if (plyr_idx != PLAYER0)
        h = dn_mix(h ^ dn_player_salt(plyr_idx));
    return start + (gap + slot) * i + (GameTurnDelta)(h % (uint64_t)slot);
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

/** A player's best creature level (1-based), imps not counted: mining imps and Create Imp's level 3 imps outgrew the
 *  army and brought raids far above it (audit 2026-11-09). */
static int dn_keeper_best_level(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    int best = 1;
    if (dungeon_invalid(dungeon))
        return best;
    ThingIndex lists[1] = {dungeon->creatr_list_start};
    for (int l = 0; l < 1; l++)
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

TbBool world_raid_hero(const struct Thing *thing);

TbBool world_raid_on(PlayerNumber plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && dn_inv[plyr_idx].active && !dn_inv[plyr_idx].retreating;
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
        /* its landing's ground went: it comes back, and the raider goes there next time */
        for (int s = 0; s < inv->nspots; s++)
            world_load_chunks_round_world(inv->spot_x[s], inv->spot_y[s], 1);
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

void world_raid_creature_died(struct Thing *thing)
{
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
                if ((tp < 0) || (tp >= PLAYERS_COUNT) || !world_player_is_human(tp))
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
        if (dn_inv[p].active && (count[p] == 0))
        {
            SYNCLOG("World: the raid on player %d (day %d) is over", p, (int)dn_inv[p].day);
            memset(&dn_inv[p], 0, sizeof(dn_inv[p]));
            world_progress_raid_over(p);
        }
    }
}

static void dn_raid(long day, int idx, PlayerNumber plyr_idx)
{
    if (dn_invasion_on(plyr_idx))
    {
        SYNCLOG("World: day %ld raid %d doesn't come for player %d: the last raid's heroes are still here%s", day, idx + 1,
            (int)plyr_idx, (idx == 0) ? " (it comes when they are gone)" : "");
        if (idx == 0)
            dn_owed_day[plyr_idx] = (int32_t)day; /* the day's first raid always comes (2026-11-11) */
        return;
    }
    if (idx == 0)
        dn_owed_day[plyr_idx] = 0;
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
    if (ncand <= 0)
    {
        free(cand);
        free(area.kind);
        free(area.mine);
        SYNCLOG("World: day %ld raid %d skipped: no open ground at least %d slabs from the heart", day, idx + 1,
            DN_SPAWN_MIN_DIST);
        return;
    }
    /* how many parties: by the player's best creature, each party landing on its own spot */
    const int best = dn_keeper_best_level(plyr_idx);
    const int coop_bonus = world_coop_level_bonus(plyr_idx);
    const int tier = dn_tier_for(best);
    const int want = dn_tiers[tier].parties_min +
        (int)dn_rand(&rng, (uint32_t)(dn_tiers[tier].parties_max - dn_tiers[tier].parties_min + 1));
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
                if (!dn_landing(&area, x, y))
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
    struct DnInvasion inv;
    memset(&inv, 0, sizeof(inv));
    int made_all = 0;
    for (int s = 0; s < nspots; s++)
    {
        const MapSlabCoord sx = spot_x[s], sy = spot_y[s];
        /* the chunked world: the landing chunks come in now; the heroes keep chunks loaded round them from then on */
        const int landing_chunks = world_load_chunks_round(sx, sy, 1);
        if (landing_chunks > 0)
            SYNCLOG("World: day %ld raid %d: %d chunks loaded round landing %d", day, idx + 1, landing_chunks, s + 1);
        const int biome = world_biome_at_engine_slab(sx, sy);
        struct DnPool pool;
        dn_make_pool(tier, biome, &pool);
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
        dn_party_levels(&rng, n, best, world_seat_day(plyr_idx), levels); /* the dungeon's own days */
        /* co-op: the heroes that come for a keeper in a party are a level higher */
        for (int j = 0; (j < n) && (coop_bonus > 0); j++)
            levels[j] = min(levels[j] + coop_bonus, DN_LEVEL_CAP);
        struct Thing *leader = INVALID_THING;
        int made = 0;
        char desc[200];
        int dlen = 0;
        desc[0] = 0;
        for (int k = 0; k < n; k++)
        {
            if (!creature_count_below_map_limit(0))
            {
                WARNLOG("World: raid stopped at the creature limit");
                break;
            }
            const ThingModel model = thing_is_invalid(leader) ? dn_model("TUNNELLER") : dn_pick_follower(&rng, &pool);
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
                cctrl->party.target_plyr_idx = plyr_idx;
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
            continue;
        world_slab_to_world(sx, sy, &inv.spot_x[inv.nspots], &inv.spot_y[inv.nspots]);
        inv.nspots++;
        made_all += made;
        const long dist = (long)floor(sqrt((double)(sx - hx) * (sx - hx) + (double)(sy - hy) * (sy - hy)) + 0.5);
        SYNCLOG("World: day %ld raid %d party %d of %d: %d heroes tunnel in at engine slab (%d,%d), %ld slabs from the"
            " heart, %s: %s (player's best level %d, the dungeon's day %ld%s)", day, idx + 1, s + 1, nspots, made, (int)sx,
            (int)sy, dist, wg_biome_name(biome), desc, best, world_seat_day(plyr_idx),
            (coop_bonus > 0) ? "; a level higher: the keeper is in a party" : "");
    }
    if (made_all == 0)
        return;
    world_progress_raid(plyr_idx, day, idx, world_seed()); /* its book: only for a raid that came (audit 2026-11-09) */
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
    memset(dn_inv, 0, sizeof(dn_inv));
    memset(dn_owed_day, 0, sizeof(dn_owed_day));
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
        world_load_chunks_round_world(inv->spot_x[s], inv->spot_y[s], 1);
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
    else if (t == dn_light(day))
    {
        SYNCLOG("World: night %ld begins", day);
        server_admin_log("world", "Night %ld begins", day);
    }
    dn_invasions_turn(day, t);
    dn_killleader_hook();
    world_boss_turn();
    world_truce_turn();
    world_defectors_turn();
    world_crusade_turn();
    world_alarm_turn();
    if (t >= dn_light(day))
        return;
    /* a first raid that waited for the last raid's heroes to go: it comes now, if its time has passed */
    if ((game.play_gameturn % 16) == 13)
    {
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            if ((dn_owed_day[p] != (int32_t)day) || dn_invasion_on(p))
                continue;
            if (!world_player_is_human(p) || world_coop_seat_away(p))
            {
                dn_owed_day[p] = 0;
                continue;
            }
            SYNCLOG("World: day %ld's first raid comes for player %d now: the last raid's heroes are gone", day, (int)p);
            dn_raid(day, 0, p);
            if (!dn_invasion_on(p))
                dn_owed_day[p] = 0; /* it couldn't land: not again today */
        }
    }
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
    /* every human player gets its day's raids (as many as its dungeon's own days allow), each at its own time in the
       raid's part of the day, rolled on its own */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!world_player_is_human(p) || world_coop_seat_away(p) || world_boss_player_away(p))
            continue; /* (a co-op dungeon whose player is away is frozen: no raid comes for it; nor for a keeper at a
                         World Boss fight) */
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
        const struct DnInvasion *inv = &dn_inv[p];
        /* going home: the whole raid after dawn, or a party whose leader died not long ago */
        const TbBool fleeing = (inv->flee_turn != 0) && ((GameTurnDelta)(game.play_gameturn - inv->flee_turn) <= DN_RETREAT_TURNS);
        if (!inv->active || (!inv->retreating && !fleeing))
            continue;
        for (int s = 0; s < inv->nspots; s++)
            want(inv->spot_x[s], inv->spot_y[s], 1);
    }
}

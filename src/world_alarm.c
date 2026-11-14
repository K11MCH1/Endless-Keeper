/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_alarm.c
 *     Endless world: Alarm Bells (2026-11-13; see world_alarm.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time; reworked after the
 *     audit of 2026-11-13):
 *     - Reach: 200 slabs from the fortress's middle; gold mined where several bells reach fills the nearest one's.
 *       Stages at 25,000, then 50,000 more, then 100,000 more gold mined in reach since the stage before, at least a
 *       minute apart; after a column the bell rests a day. A dungeon's first two days ring no bell. (2,500, 6,000
 *       and 12,000 destroyed a new keeper in eight minutes.)
 *     - Heroes don't answer a Call to Arms: the flag and horn are shown on their own over the seam (the heroes'
 *       white banner), and the heroes are sent by their party's orders.
 *     - The heroes come from the fortress's side: they land through the ceiling on dug, unclaimed ground 30-80 slabs
 *       from the seam towards the fortress, with no claimed ground of anyone within 6 slabs (never in a keeper's
 *       dungeon, nor a neighbour's), choosing the landing with the fewest rock, gem and lava slabs on the line to the
 *       seam, as day raids do. A Tunneller leads every party and digs towards the keeper's dungeon. No such ground:
 *       the stage waits.
 *     - As strong as the fortress: levels from the circle the fortress stands in (as strongholds' heroes), plus the
 *       co-op level; the kinds those of its guards by its depth (Barbarians from 120 slabs, Monks 160, Wizards 200,
 *       Witches 260, Samurai and Giants 320, Knights 400). Warning: a Tunneller and two; raiding party: a Tunneller
 *       and four Thieves (StealGold); column: a Tunneller and seven (AttackDnHeart).
 *     - The alarm's heroes carry a mark of their own (original objective SabotageRooms): the strongholds' mark made
 *       them roam. They keep the chunks round them loaded, as raid heroes do, and leave through the ceiling after
 *       eight minutes when not fighting.
 *     - Razed: a keeper has claimed ground at the fortress's middle. Then the bell is silent for everyone, for good.
 *     - A dungeon kept by the caretaker (its player is away) fills no alarm, nor does a co-op dungeon whose player
 *       is away.
 *     Saved: ALM1 (every keeper's alarms, the razed fortresses).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_alarm.h"
#include "world_stream.h"
#include "world_gen.h"
#include "world_caretaker.h"
#include "world_circles.h"
#include "world_daynight.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_math.h"
#include "config_creature.h"
#include "config_magic.h"
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
#include "slab_data.h"
#include "sounds.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_objects.h"
#include "thing_physics.h"
#include "map_utils.h"
#include "bflib_planar.h"
#include "post_inc.h"

static int al_reach = 200;           /* slabs from a fortress's middle its bell hears */
#define AL_REACH al_reach
#define AL_MAX 1024                  /* alarms kept (keeper, fortress) */
#define AL_RAZED_MAX 2048
#define AL_CACHE 32                  /* fortresses a keeper's mining is checked against */
#define AL_REQUERY_DIST 256          /* the cache is looked up again when the mining moves this far from where it was
                                        looked up (a keeper mines in several places at once) */
#define AL_STAGE_GAP (60 * 20)       /* turns at least between two stages of one alarm */
#define AL_FLAG_TURNS (45 * 20)      /* how long the heroes' flag flies */
#define AL_LAND_AHEAD 18             /* slabs from the seam towards the fortress the heroes land */

static uint32_t al_stage_gold[3] = {25000, 50000, 100000}; /* (2,500/6,000/12,000 destroyed a new keeper in eight
                                                               minutes: audit 2026-11-13) */
#define AL_QUIET_DAYS 2              /* a dungeon's first days: its mining rings no bell */
#define AL_REST_TURNS (24 * 60 * 20) /* after a column, the bell rests a day before it can ring again */
#define AL_HERO_LIFE (8 * 60 * 20)   /* an alarm's heroes leave through the ceiling after this, when not fighting */
/* The alarm's heroes' own mark (party.original_objective): not a raid's (AttackEnemies, DefendParty), the wild
   packs' (DefendSpawn, DefendHeart) nor the strongholds' (DefendRooms, which makes them roam; audit 2026-11-13) */
#define AL_HERO_MARK CHeroTsk_SabotageRooms
#define AL_LAND_MIN 30               /* slabs from the seam, towards the fortress, the heroes land at least */
#define AL_LAND_MAX 80
#define AL_LAND_CLEAR 6              /* no claimed ground of anyone within this many slabs of a landing */
static int al_test_read = 0;
static TbBool al_log_on = false;
static TbBool al_test_numbers = false; /* KFX_WORLD_ALARMGOLD: no quiet days, no rest (tests) */

/** Test switches: KFX_WORLD_ALARMGOLD="a,b,c" (the stages' gold), KFX_WORLD_ALARMLOG=1 (the fortresses a keeper's
 *  mining is checked against, each time they are looked up), KFX_WORLD_ALARMREACH=slabs (the bells' reach). */
static void al_read_test(void)
{
    if (al_test_read)
        return;
    al_test_read = 1;
    const char *v = getenv("KFX_WORLD_ALARMGOLD");
    if (v != NULL)
    {
        unsigned a = 0, b = 0, c = 0;
        if (sscanf(v, "%u,%u,%u", &a, &b, &c) == 3)
        {
            al_stage_gold[0] = a;
            al_stage_gold[1] = b;
            al_stage_gold[2] = c;
        }
        al_test_numbers = true;
        SYNCLOG("World alarm test: stages at %u, %u and %u gold", al_stage_gold[0], al_stage_gold[1], al_stage_gold[2]);
    }
    al_log_on = (getenv("KFX_WORLD_ALARMLOG") != NULL);
    const char *rv = getenv("KFX_WORLD_ALARMREACH");
    if ((rv != NULL) && (atoi(rv) > 0))
    {
        al_reach = atoi(rv);
        SYNCLOG("World alarm test: bells reach %d slabs", al_reach);
    }
}
static const char *const al_stage_names[3] = {"a warning", "a raiding party", "a column"};

struct AlEntry {
    uint8_t used, plyr, stage, pad;
    int32_t fcx, fcy;                /* the fortress: its generator structure's start chunk */
    int64_t fx, fy;                  /* its middle, world slabs */
    int64_t seam_x, seam_y;          /* the last seam mined in its reach, world slabs */
    uint32_t gold;                   /* gold mined in reach since the last stage */
    GameTurn last_stage;
};
static struct AlEntry al[AL_MAX];
struct AlRazed {
    int32_t fcx, fcy;
};
static struct AlRazed al_razed[AL_RAZED_MAX];
static int al_razed_n;

/* Each keeper's fortresses near where they mine (not saved: looked up again) */
struct AlCache {
    TbBool valid;
    int64_t qx, qy;
    GameTurn qturn;
    int n;
    int32_t fcx[AL_CACHE], fcy[AL_CACHE];
    int64_t fx[AL_CACHE], fy[AL_CACHE];
};
static struct AlCache al_cache[PLAYERS_MAX];

/* The heroes' flag flying now (one: the heroes have one Call to Arms banner) */
static ThingIndex al_flag_idx;
static GameTurn al_flag_until;

static TbBool al_razed_has(int32_t fcx, int32_t fcy)
{
    for (int i = 0; i < al_razed_n; i++)
        if ((al_razed[i].fcx == fcx) && (al_razed[i].fcy == fcy))
            return true;
    return false;
}

static void al_cache_fill(PlayerNumber p, int64_t sx, int64_t sy)
{
    struct AlCache *c = &al_cache[p];
    static struct WgStructRef refs[AL_CACHE];
    c->valid = true;
    c->qx = sx;
    c->qy = sy;
    c->qturn = game.play_gameturn;
    c->n = 0;
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return;
    const int64_t r = AL_REACH + AL_REQUERY_DIST + 16;
    int n = wg_structures_in(gen, sx - r, sy - r, sx + r, sy + r, 1u << WgT_Fortress, refs, AL_CACHE);
    if (n > AL_CACHE)
        n = AL_CACHE;
    for (int i = 0; i < n; i++)
    {
        c->fcx[c->n] = refs[i].id.start_cx;
        c->fcy[c->n] = refs[i].id.start_cy;
        c->fx[c->n] = (refs[i].bbox_x1 + refs[i].bbox_x2) / 2;
        c->fy[c->n] = (refs[i].bbox_y1 + refs[i].bbox_y2) / 2;
        c->n++;
    }
    if (al_log_on)
    {
        int inreach = 0;
        double nearest = -1.0;
        for (int i = 0; i < c->n; i++)
        {
            const double ddx = (double)(sx - c->fx[i]), ddy = (double)(sy - c->fy[i]);
            const double d = sqrt(ddx * ddx + ddy * ddy);
            if (d <= AL_REACH)
                inreach++;
            if ((nearest < 0) || (d < nearest))
                nearest = d;
        }
        SYNCLOG("World alarm test: player %d mines at world slab (%lld,%lld): %d fortresses near, %d of them in reach"
            " (%d slabs), the nearest %.0f slabs away", (int)p, (long long)sx, (long long)sy, c->n, inreach, AL_REACH,
            nearest);
    }
}

static struct AlEntry *al_entry(PlayerNumber p, int32_t fcx, int32_t fcy, TbBool make)
{
    struct AlEntry *free_one = NULL;
    for (int i = 0; i < AL_MAX; i++)
    {
        if (!al[i].used)
        {
            if (free_one == NULL)
                free_one = &al[i];
            continue;
        }
        if ((al[i].plyr == p) && (al[i].fcx == fcx) && (al[i].fcy == fcy))
            return &al[i];
    }
    if (!make || (free_one == NULL))
        return NULL;
    memset(free_one, 0, sizeof(*free_one));
    free_one->used = 1;
    free_one->plyr = (uint8_t)p;
    free_one->fcx = fcx;
    free_one->fcy = fcy;
    free_one->last_stage = game.play_gameturn;
    return free_one;
}

void world_alarm_gold_mined(const struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y, long gold)
{
    if (!world_mode_active() || !thing_exists(creatng) || (gold <= 0))
        return;
    al_read_test();
    const PlayerNumber p = creatng->owner;
    if ((p >= PLAYERS_MAX) || !world_player_is_human(p) || world_caretaker_active(p) || world_coop_seat_away(p))
        return;
    if (!al_test_numbers && (world_seat_day(p) <= AL_QUIET_DAYS))
        return; /* a new dungeon's first days ring no bell (audit 2026-11-13: it was destroyed in eight minutes) */
    int64_t sx, sy;
    world_slab_to_world(subtile_slab(stl_x), subtile_slab(stl_y), &sx, &sy);
    struct AlCache *c = &al_cache[p];
    if (!c->valid || (llabs(sx - c->qx) > AL_REQUERY_DIST) || (llabs(sy - c->qy) > AL_REQUERY_DIST) ||
        ((GameTurnDelta)(game.play_gameturn - c->qturn) > 8192))
        al_cache_fill(p, sx, sy);
    /* the nearest bell in reach hears it (three fortresses sending three parties for one seam was too much) */
    int near = -1;
    double near_d = 0;
    for (int i = 0; i < c->n; i++)
    {
        const double dx = (double)(sx - c->fx[i]), dy = (double)(sy - c->fy[i]);
        const double d = dx * dx + dy * dy;
        if ((d > (double)AL_REACH * AL_REACH) || al_razed_has(c->fcx[i], c->fcy[i]))
            continue;
        if ((near < 0) || (d < near_d))
        {
            near = i;
            near_d = d;
        }
    }
    for (int i = near; (i >= 0) && (i == near); i++)
    {
        struct AlEntry *e = al_entry(p, c->fcx[i], c->fcy[i], true);
        if (e == NULL)
            continue;
        e->fx = c->fx[i];
        e->fy = c->fy[i];
        e->seam_x = sx;
        e->seam_y = sy;
        e->gold += (uint32_t)gold;
    }
}

/** Where the heroes land: dug, unclaimed ground (a path) AL_LAND_MIN..AL_LAND_MAX slabs from the seam (sx, sy) on
 *  the fortress's side (direction ux, uy), with no claimed ground of anyone (no keeper's dungeon, no hero hall)
 *  within AL_LAND_CLEAR slabs, nearest to the point AL_LAND_MIN + 20 slabs out. Read whether loaded or not, as the
 *  day raids do. False: no such ground now (the stage waits). */
static TbBool al_find_landing(PlayerNumber p, MapSlabCoord sx, MapSlabCoord sy, double ux, double uy, MapSlabCoord *ox,
    MapSlabCoord *oy)
{
    const MapSlabCoord px = sx + (MapSlabCoord)floor(ux * (AL_LAND_MIN + 20) + 0.5);
    const MapSlabCoord py = sy + (MapSlabCoord)floor(uy * (AL_LAND_MIN + 20) + 0.5);
    const int half = (AL_LAND_MAX - AL_LAND_MIN) / 2 + 12 + AL_LAND_CLEAR;
    /* the box takes in the seam too: the line from a landing to it is read (rock, gems and lava a Tunneller can not
       dig through or cross), and the landing with the fewest such slabs on it wins, as a day raid's does */
    const MapSlabCoord x1 = min(px - half, sx - 2), y1 = min(py - half, sy - 2), x2 = max(px + half, sx + 2),
        y2 = max(py + half, sy + 2);
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    uint8_t *kind = (uint8_t *)malloc((size_t)w * h);
    uint8_t *mine = (uint8_t *)malloc((size_t)w * h);
    int32_t *sum = (int32_t *)calloc((size_t)(w + 1) * (h + 1), sizeof(int32_t));
    TbBool found = false;
    if ((kind != NULL) && (mine != NULL) && (sum != NULL) && world_read_slab_box(x1, y1, x2, y2, kind, mine, p))
    {
        /* claimed ground counted in a summed table: "none within AL_LAND_CLEAR" in four reads */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const uint8_t k = kind[y * w + x];
                const int claimed = (k == SlbT_CLAIMED) || (k >= SlbT_WALLDRAPE && k <= SlbT_WALLPAIRSHR) ||
                    ((k >= SlbT_ENTRANCE) && (k != SlbT_GEMS) && (k != SlbT_LAVA) && (k != SlbT_WATER) &&
                    (k != SlbT_PATH) && (k != SlbT_EARTH) && (k != SlbT_TORCHDIRT) && (k != SlbT_GOLD) && (k != SlbT_ROCK));
                sum[(y + 1) * (w + 1) + (x + 1)] = claimed + sum[y * (w + 1) + (x + 1)] + sum[(y + 1) * (w + 1) + x] -
                    sum[y * (w + 1) + x];
            }
        long best = -1;
        for (int y = AL_LAND_CLEAR; y < h - AL_LAND_CLEAR; y++)
            for (int x = AL_LAND_CLEAR; x < w - AL_LAND_CLEAR; x++)
            {
                if ((kind[y * w + x] != SlbT_PATH) || mine[y * w + x])
                    continue;
                const MapSlabCoord ex = x1 + x, ey = y1 + y;
                const double fx = (double)(ex - sx), fy = (double)(ey - sy);
                const double along = fx * ux + fy * uy;
                if ((along < AL_LAND_MIN) || (along > AL_LAND_MAX) || (fx * fx + fy * fy < (double)AL_LAND_MIN * AL_LAND_MIN))
                    continue;
                const int a = y - AL_LAND_CLEAR, b = y + AL_LAND_CLEAR + 1, c = x - AL_LAND_CLEAR, d = x + AL_LAND_CLEAR + 1;
                const int32_t n = sum[b * (w + 1) + d] - sum[a * (w + 1) + d] - sum[b * (w + 1) + c] + sum[a * (w + 1) + c];
                if (n > 0)
                    continue;
                int blockers = 0;
                const int steps = (int)max(labs((long)ex - sx), labs((long)ey - sy));
                for (int t = 1; t < steps; t++)
                {
                    const int lxx = (int)(sx - x1) + (int)floor((double)(ex - sx) * t / steps + 0.5);
                    const int lyy = (int)(sy - y1) + (int)floor((double)(ey - sy) * t / steps + 0.5);
                    const uint8_t k = kind[lyy * w + lxx];
                    if ((k == SlbT_ROCK) || (k == SlbT_GEMS) || (k == SlbT_LAVA))
                        blockers++;
                }
                const long dd = (long)blockers * 100000L + (long)(ex - px) * (ex - px) + (long)(ey - py) * (ey - py);
                if ((best < 0) || (dd < best))
                {
                    best = dd;
                    *ox = ex;
                    *oy = ey;
                    found = true;
                }
            }
    }
    free(kind);
    free(mine);
    free(sum);
    return found;
}

static ThingModel al_model(const char *name)
{
    const long model = get_id(creature_desc, name);
    return (model > 0) ? (ThingModel)model : 0;
}

/* creature_states_hero.c, not in a header */
TbBool script_support_send_tunneller_to_dungeon_heart(struct Thing *creatng, PlayerNumber plyr_idx);

/** One hero through the ceiling at (x, y). The mark CHeroTsk_DefendRooms as its original objective keeps it apart
 *  from the day's raiders (world_raid_hero). */
static struct Thing *al_hero(const char *name, MapSlabCoord x, MapSlabCoord y, int level, int k)
{
    const ThingModel model = al_model(name);
    if ((model <= 0) || !creature_count_below_map_limit(0))
        return INVALID_THING;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x)) + (MapCoord)(((k * 37) % 193) - 96);
    pos.y.val = subtile_coord_center(slab_subtile_center(y)) + (MapCoord)(((k * 71) % 193) - 96);
    pos.z.val = 0;
    struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, model, PLAYER_GOOD);
    if (thing_is_invalid(thing))
        return INVALID_THING;
    thing->mappos.z.val = get_ceiling_height(&thing->mappos);
    create_effect(&thing->mappos, TngEff_CeilingBreach, thing->owner);
    initialise_thing_state(thing, CrSt_CreatureHeroEntering);
    set_flag(thing->rendering_flags, TRF_Invisible);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    cctrl->countdown = 24 + 8 * k;
#ifdef CREATURE_MAX_LEVEL_LIMIT
    const int cap = CREATURE_MAX_LEVEL_LIMIT;
#else
    const int cap = CREATURE_MAX_LEVEL;
#endif
    init_creature_level(thing, (CrtrExpLevel)(max(1, min(level, cap)) - 1));
    cctrl->party.original_objective = AL_HERO_MARK;
    return thing;
}

/** The heroes' flag and horn on the seam. */
static void al_raise_flag(MapSlabCoord x, MapSlabCoord y)
{
    struct PlayerInfo *heroes = get_player(PLAYER_GOOD);
    if (player_invalid(heroes))
        return;
    struct Thing *old = thing_get(al_flag_idx);
    if ((al_flag_idx > 0) && thing_is_object(old) && (old->model == ObjMdl_CTAEnsign) && (old->owner == PLAYER_GOOD))
        delete_thing_structure(old, 0);
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x));
    pos.y.val = subtile_coord_center(slab_subtile_center(y));
    pos.z.val = get_floor_height_at(&pos);
    struct Thing *flag = create_object(&pos, ObjMdl_CTAEnsign, PLAYER_GOOD, -1);
    if (thing_is_invalid(flag))
        return;
    /* the flag only: the heroes' dungeon gets no Call to Arms (cta_start_turn stays 0), so no hero is pulled to it */
    heroes->cta_flag_idx = flag->index;
    flag->mappos.z.val = get_thing_height_at(flag, &flag->mappos);
    set_call_to_arms_as_birthing(flag);
    al_flag_idx = flag->index;
    al_flag_until = game.play_gameturn + AL_FLAG_TURNS;
    struct PowerConfigStats *powerst = get_power_model_stats(PwrK_CALL2ARMS);
    thing_play_sample(flag, powerst->select_sound_idx, NORMAL_PITCH, 0, 3, 0, 6, FULL_LOUDNESS);
    SYNCLOG("World alarm: the heroes' flag (thing %d) flies over the seam at engine slab (%d,%d)", (int)flag->index,
        (int)x, (int)y);
}

/* The fortress's heroes by how deep it lies (worldgen.py HERO_TYPES: the distance from the nearest start at which a
   kind guards fortresses), as its guards are chosen */
static const struct { const char *name; int from; } al_kinds[] = {
    {"ARCHER", 0}, {"DWARFA", 0}, {"THIEF", 0}, {"BARBARIAN", 120}, {"MONK", 160}, {"WIZARD", 200}, {"WITCH", 260},
    {"SAMURAI", 320}, {"GIANT", 320}, {"KNIGHT", 400},
};
#define AL_KINDS ((int)(sizeof(al_kinds) / sizeof(al_kinds[0])))

/* creature_states_hero.c, not in a header */
TbBool script_support_send_tunneller_to_dungeon_heart(struct Thing *creatng, PlayerNumber plyr_idx);

static void al_fire_stage(struct AlEntry *e)
{
    const PlayerNumber p = e->plyr;
    MapSlabCoord seam_x, seam_y;
    if (!world_engine_slab(e->seam_x, e->seam_y, &seam_x, &seam_y))
        return; /* the seam isn't on the map now: later */
    const double dx = (double)(e->fx - e->seam_x), dy = (double)(e->fy - e->seam_y);
    const double len = sqrt(dx * dx + dy * dy);
    const double ux = (len > 1.0) ? dx / len : 1.0, uy = (len > 1.0) ? dy / len : 0.0;
    MapSlabCoord lx, ly;
    if (!al_find_landing(p, seam_x, seam_y, ux, uy, &lx, &ly))
    {
        SYNCLOG("World alarm: no open ground on the side of the fortress at chunk (%d,%d) for player %d's alarm: later",
            (int)e->fcx, (int)e->fcy, (int)p);
        e->last_stage = game.play_gameturn - AL_STAGE_GAP + 512; /* again in 512 turns */
        return;
    }
    world_load_chunks_round(lx, ly, 1);
    /* as strong as the fortress: its circle's level (as strongholds' heroes), its kinds by its depth */
    const int top = world_circles_level_at_world(e->fx, e->fy, 1, NULL) + world_coop_level_bonus(p);
    const double depth = world_dist_to_nearest_start(e->fx, e->fy);
    const char *kinds[AL_KINDS];
    int nk = 0;
    for (int k = 0; k < AL_KINDS; k++)
        if (depth >= al_kinds[k].from)
            kinds[nk++] = al_kinds[k].name;
    const int stage = e->stage;
    if (stage == 0)
        al_raise_flag(seam_x, seam_y); /* the warning: flag and horn on the seam; the heroes come from far off */
    /* stage 0: a Tunneller and two; 1: a Tunneller and four Thieves for the Treasury; 2: a Tunneller and seven */
    const int n = (stage == 0) ? 3 : (stage == 1) ? 5 : 8;
    const int base = (stage == 0) ? top - 2 : (stage == 1) ? top - 1 : top;
    struct Thing *leader = INVALID_THING;
    int made = 0;
    char desc[200];
    int dlen = 0;
    desc[0] = 0;
    for (int k = 0; k < n; k++)
    {
        const char *name = (k == 0) ? "TUNNELLER" : (stage == 1) ? "THIEF" : kinds[(k * 7 + (int)e->fcx * 3 + (int)e->fcy) % max(nk, 1)];
        struct Thing *h = al_hero(name, lx, ly, (k == 0) ? base + 1 : base, k);
        if (thing_is_invalid(h))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(h);
        cctrl->party.target_plyr_idx = p;
        cctrl->party.objective = (stage == 1) ? CHeroTsk_StealGold : (stage == 2) ? CHeroTsk_AttackDnHeart : CHeroTsk_AttackEnemies;
        if (thing_is_invalid(leader))
        {
            /* the Tunneller digs from the fortress's side towards the dungeon, as a raid's does */
            leader = h;
            script_support_send_tunneller_to_dungeon_heart(h, p);
            cctrl->party.original_objective = AL_HERO_MARK;
            cctrl->party.target_plyr_idx = p;
        } else
            add_creature_to_group(h, leader);
        made++;
        if (dlen < (int)sizeof(desc) - 24)
            dlen += snprintf(desc + dlen, sizeof(desc) - dlen, "%s%s", (made > 1) ? ", " : "", name);
    }
    const long dist = (long)floor(sqrt((double)(lx - seam_x) * (lx - seam_x) + (double)(ly - seam_y) * (ly - seam_y)) + 0.5);
    SYNCLOG("World alarm: the bell of the fortress at chunk (%d,%d) (%.0f slabs deep) rings for player %d (%u gold mined"
        " in its reach): %s, %d heroes at level %d+ land %ld slabs from the seam at engine slab (%d,%d): %s",
        (int)e->fcx, (int)e->fcy, depth, (int)p, (unsigned)e->gold, al_stage_names[stage], made, max(1, base), dist,
        (int)lx, (int)ly, desc);
    e->gold = 0;
    e->last_stage = game.play_gameturn;
    if ((stage == 2) && !al_test_numbers)
        e->last_stage = game.play_gameturn + AL_REST_TURNS; /* a day's rest after a column */
    e->stage = (uint8_t)((stage + 1) % 3);
}

/** The alarm's heroes leave after AL_HERO_LIFE when they aren't fighting (through the ceiling, as raiders). */
TbBool world_alarm_hero(const struct Thing *thing)
{
    if (!world_mode_active() || !thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->owner != PLAYER_GOOD))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    return !creature_control_invalid(cctrl) && (cctrl->party.original_objective == AL_HERO_MARK);
}

static void al_heroes_leave(void)
{
    struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (dungeon_invalid(heroes))
        return;
    const ThingIndex lists[2] = {heroes->creatr_list_start, heroes->digger_list_start};
    int gone = 0;
    for (int l = 0; l < 2; l++)
    {
        long guard = 0;
        for (ThingIndex i = lists[l]; (i > 0) && (guard++ < CREATURES_COUNT);)
        {
            struct Thing *thing = thing_get(i);
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (al_log_on && world_alarm_hero(thing) && ((game.play_gameturn % 1024) == 45))
            {
                struct Thing *heart = find_players_dungeon_heart(cctrl->party.target_plyr_idx);
                SYNCLOG("World alarm test: %s %d at (%d,%d), state %d, objective %d, target %d, %ld slabs from its heart",
                    thing_model_name(thing), (int)thing->index, (int)subtile_slab(thing->mappos.x.stl.num),
                    (int)subtile_slab(thing->mappos.y.stl.num), (int)get_creature_state_besides_interruptions(thing),
                    (int)cctrl->party.objective, (int)cctrl->party.target_plyr_idx, thing_exists(heart) ?
                    (long)get_chessboard_distance(&thing->mappos, &heart->mappos) / COORD_PER_STL / 3 : -1L);
            }
            if (!world_alarm_hero(thing) || (cctrl->combat_flags != 0) || creature_is_dying(thing) ||
                creature_is_being_unconscious(thing) || creature_is_kept_in_custody(thing) || thing_is_picked_up(thing))
                continue;
            if ((GameTurnDelta)(game.play_gameturn - thing->creation_turn) < AL_HERO_LIFE)
                continue;
            create_effect(&thing->mappos, TngEff_CeilingBreach, thing->owner);
            world_remove_thing_quietly(thing);
            gone++;
        }
    }
    if (gone > 0)
        SYNCLOG("World alarm: %d of the fortresses' heroes go back through the ceiling", gone);
}

/** A fortress is razed when a keeper has claimed ground at its middle (checked while it is on the map). */
static void al_check_razed(void)
{
    for (int i = 0; i < AL_MAX; i++)
    {
        if (!al[i].used || al_razed_has(al[i].fcx, al[i].fcy))
            continue;
        MapSlabCoord mx, my;
        if (!world_engine_slab(al[i].fx, al[i].fy, &mx, &my))
            continue;
        PlayerNumber who = PLAYER_NEUTRAL;
        for (int dy = -2; (dy <= 2) && (who == PLAYER_NEUTRAL); dy++)
            for (int dx = -2; dx <= 2; dx++)
            {
                struct SlabMap *slb = get_slabmap_block(mx + dx, my + dy);
                if (slabmap_block_invalid(slb))
                    continue;
                const PlayerNumber o = slabmap_owner(slb);
                if ((o < PLAYERS_MAX) && world_player_is_human(o))
                {
                    who = o;
                    break;
                }
            }
        if (who == PLAYER_NEUTRAL)
            continue;
        if (al_razed_n < AL_RAZED_MAX)
        {
            al_razed[al_razed_n].fcx = al[i].fcx;
            al_razed[al_razed_n].fcy = al[i].fcy;
            al_razed_n++;
        }
        SYNCLOG("World alarm: player %d has razed the fortress at chunk (%d,%d): its bell is silent for good", (int)who,
            (int)al[i].fcx, (int)al[i].fcy);
        const int32_t fcx = al[i].fcx, fcy = al[i].fcy;
        for (int k = 0; k < AL_MAX; k++)
            if (al[k].used && (al[k].fcx == fcx) && (al[k].fcy == fcy))
                al[k].used = 0;
    }
}

void world_alarm_turn(void)
{
    if (!world_mode_active())
        return;
    if ((al_flag_idx > 0) && ((GameTurnDelta)(game.play_gameturn - al_flag_until) >= 0))
    {
        struct Thing *flag = thing_get(al_flag_idx);
        if (thing_is_object(flag) && (flag->model == ObjMdl_CTAEnsign) && (flag->owner == PLAYER_GOOD))
        {
            set_call_to_arms_as_dying(flag);
            SYNCLOG("World alarm: the heroes' flag (thing %d) comes down", (int)flag->index);
        }
        al_flag_idx = 0;
    } else if ((al_flag_idx == 0) && ((game.play_gameturn % 64) == 31))
    {
        /* a flag of ours from before a load (heroes have no Call to Arms of their own): it goes */
        struct PlayerInfo *heroes = get_player(PLAYER_GOOD);
        struct Thing *flag = player_invalid(heroes) ? INVALID_THING : thing_get(heroes->cta_flag_idx);
        if ((heroes->cta_flag_idx > 0) && thing_is_object(flag) && (flag->model == ObjMdl_CTAEnsign) &&
            (flag->owner == PLAYER_GOOD) && (flag->call_to_arms_flag.state == CTAOL_Alive))
            set_call_to_arms_as_dying(flag);
        else if ((heroes->cta_flag_idx > 0) && (!thing_is_object(flag) || (flag->owner != PLAYER_GOOD)))
            heroes->cta_flag_idx = 0; /* (its flag went with a frozen chunk: the index may be another's now) */
    }
    if ((game.play_gameturn % 16) == 7)
    {
        for (int i = 0; i < AL_MAX; i++)
        {
            struct AlEntry *e = &al[i];
            if (!e->used || (e->gold < al_stage_gold[e->stage]))
                continue;
            if ((GameTurnDelta)(game.play_gameturn - e->last_stage) < AL_STAGE_GAP)
                continue;
            if (!world_player_is_human(e->plyr) || world_caretaker_active(e->plyr) || world_coop_seat_away(e->plyr))
                continue;
            al_fire_stage(e);
        }
    }
    if ((game.play_gameturn % 256) == 77)
        al_check_razed();
    if ((game.play_gameturn % 64) == 45)
        al_heroes_leave();
}

void world_alarm_seat_started(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    for (int i = 0; i < AL_MAX; i++)
        if (al[i].used && (al[i].plyr == plyr_idx))
            al[i].used = 0;
    al_cache[plyr_idx].valid = false;
}

/******************************************************************************/
/* Saving (XTR2 record ALM1): the alarms (turns as ages) and the razed fortresses. */
#pragma pack(1)
struct AlSaved {
    uint8_t plyr, stage;
    int32_t fcx, fcy;
    int64_t fx, fy, seam_x, seam_y;
    uint32_t gold;
    uint32_t stage_age;
};
#pragma pack()

void world_alarm_reset(void)
{
    memset(al, 0, sizeof(al));
    memset(al_razed, 0, sizeof(al_razed));
    al_razed_n = 0;
    memset(al_cache, 0, sizeof(al_cache));
    al_flag_idx = 0;
    al_flag_until = 0;
}

static int al_count(void)
{
    int n = 0;
    for (int i = 0; i < AL_MAX; i++)
        if (al[i].used)
            n++;
    return n;
}

size_t world_alarm_save_size(void)
{
    const int n = al_count();
    if ((n == 0) && (al_razed_n == 0))
        return 0;
    return 8 + (size_t)n * sizeof(struct AlSaved) + (size_t)al_razed_n * sizeof(struct AlRazed);
}

void world_alarm_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)al_count(), r = (uint32_t)al_razed_n;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, &r, 4);
    uint8_t *at = buf + 8;
    for (int i = 0; i < AL_MAX; i++)
    {
        if (!al[i].used)
            continue;
        struct AlSaved s;
        memset(&s, 0, sizeof(s));
        s.plyr = al[i].plyr;
        s.stage = al[i].stage;
        s.fcx = al[i].fcx;
        s.fcy = al[i].fcy;
        s.fx = al[i].fx;
        s.fy = al[i].fy;
        s.seam_x = al[i].seam_x;
        s.seam_y = al[i].seam_y;
        s.gold = al[i].gold;
        s.stage_age = (uint32_t)(game.play_gameturn - al[i].last_stage);
        memcpy(at, &s, sizeof(s));
        at += sizeof(s);
    }
    memcpy(at, al_razed, (size_t)r * sizeof(struct AlRazed));
}

TbBool world_alarm_load(const uint8_t *buf, size_t len)
{
    world_alarm_reset();
    uint32_t n = 0, r = 0;
    if (len < 8)
        return false;
    memcpy(&n, buf, 4);
    memcpy(&r, buf + 4, 4);
    if ((n > AL_MAX) || (r > AL_RAZED_MAX) || (len < 8 + (size_t)n * sizeof(struct AlSaved) + (size_t)r * sizeof(struct AlRazed)))
        return false;
    const uint8_t *at = buf + 8;
    for (uint32_t i = 0; i < n; i++)
    {
        struct AlSaved s;
        memcpy(&s, at, sizeof(s));
        at += sizeof(s);
        if ((s.plyr >= PLAYERS_MAX) || (s.stage > 2))
            continue;
        struct AlEntry *e = &al[i];
        e->used = 1;
        e->plyr = s.plyr;
        e->stage = s.stage;
        e->fcx = s.fcx;
        e->fcy = s.fcy;
        e->fx = s.fx;
        e->fy = s.fy;
        e->seam_x = s.seam_x;
        e->seam_y = s.seam_y;
        e->gold = s.gold;
        e->last_stage = game.play_gameturn - s.stage_age;
    }
    memcpy(al_razed, at, (size_t)r * sizeof(struct AlRazed));
    al_razed_n = (int)r;
    return true;
}

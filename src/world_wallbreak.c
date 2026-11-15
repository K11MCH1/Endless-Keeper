/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wallbreak.c
 *     Endless worlds: wild Beetles and raid Tunnellers break fortified walls.
 * @par Purpose:
 *     See world_wallbreak.h. The work in progress is not saved: after a load a breaker starts its minute again.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_wallbreak.h"

#include <string.h>
#include <stdlib.h>

#include "world_stream.h"
#include "world_dark.h"
#include "world_daynight.h"
#include "world_wild.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "map_blocks.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "thing_navigate.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_creature.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define WB_JOBS 64
#define WB_TRACKED 64
#define WB_STALL_SECONDS 3 /* a Tunneller that gets no nearer for this long, beside a breakable wall, breaks it */

struct WbJob {
    ThingIndex idx;
    GameTurn made;          /* the breaker's creation turn (an index can be used again) */
    int64_t wx, wy;         /* the wall, world slab */
    int64_t sx, sy;         /* where the breaker stands */
    GameTurn until;         /* the wall breaks at this turn */
    GameTurn touched;       /* the last turn its breaker asked (it stops asking: the job ends) */
};
static struct WbJob wb_job[WB_JOBS];

/* raid Tunnellers watched: how near they have come to their target */
struct WbTunneller {
    ThingIndex idx;
    GameTurn made;
    long best;              /* the nearest it has been (squared slabs) */
    int stall;              /* seconds without getting nearer */
    int64_t wx, wy;         /* the wall it breaks (has_wall) */
    uint8_t has_wall;
};
static struct WbTunneller wb_tun[WB_TRACKED];

void world_wallbreak_reset(void)
{
    memset(wb_job, 0, sizeof(wb_job));
    memset(wb_tun, 0, sizeof(wb_tun));
}

static TbBool fortified_kind(SlabKind kind)
{
    return (kind >= SlbT_WALLDRAPE) && (kind <= SlbT_WALLPAIRSHR);
}

TbBool world_wallbreak_can(MapSlabCoord x, MapSlabCoord y)
{
    if (!world_mode_active())
        return false;
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || !fortified_kind(slb->kind))
        return false;
    const PlayerNumber owner = slabmap_owner(slb);
    if ((owner < 0) || (owner >= PLAYERS_COUNT) || (owner == PLAYER_GOOD) || (owner == game.neutral_player_num))
        return false;
    static int rings_off = -1; /* test switch KFX_WORLD_WALLRINGS=off: walls inside the rings break too */
    if (rings_off < 0)
        rings_off = ((getenv("KFX_WORLD_WALLRINGS") != NULL) && (strcmp(getenv("KFX_WORLD_WALLRINGS"), "off") == 0)) ? 1 : 0;
    return rings_off || !world_dark_anchored(owner, x, y); /* inside the keeper's rings it holds */
}

static struct Thing *job_thing(const struct WbJob *j)
{
    if (j->idx == 0)
        return INVALID_THING;
    struct Thing *thing = thing_get(j->idx);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->creation_turn != j->made) ||
        creature_is_dying(thing) || thing_is_picked_up(thing))
        return INVALID_THING;
    return thing;
}

static void stand_at(struct Thing *thing, MapSlabCoord x, MapSlabCoord y)
{
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x));
    pos.y.val = subtile_coord_center(slab_subtile_center(y));
    pos.z.val = 0;
    cleanup_current_thing_state(thing);
    if (setup_person_move_to_coord(thing, &pos, NavRtF_Default))
        thing->continue_state = CrSt_GoodDoingNothing;
}

/** A raid Tunneller whose wall broke, or whose job ended, goes back to its own task (the user, 2026-11-15: "After
 *  Tunneller breaks a fortified wall it needs to reset back to its original task which is heading to the Dungeon
 *  Heart"): its idle state, which walks it to the heart when it can get there and tunnels toward it when it cannot
 *  (a raid's leader goes on with its guide from there). Held at the wall it was only told to stand there and then do
 *  nothing. A raider whose raid is over walks home instead (the user, 2026-11-15: "the dawn-retreat logic should
 *  override the wall-breaking. But should not override in the middle of battles"). Not when it is in that state already (its leader's guide is asking from inside it) or in a fight; wild
 *  creatures are driven by world_wild.c each turn. */
static void back_to_task(struct Thing *thing)
{
    static long tunneller = -2;
    if (tunneller == -2)
        tunneller = get_id(creature_desc, "TUNNELLER");
    if (!thing_exists(thing) || (thing->model != tunneller) || !world_raid_hero(thing) || creature_is_dying(thing) ||
        thing_is_picked_up(thing))
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (cctrl->combat_flags != 0))
        return;
    if (get_creature_state_besides_interruptions(thing) == CrSt_TunnellerDoingNothing)
        return;
    cleanup_current_thing_state(thing);
    if (world_raid_hero_retreating(thing))
    {
        /* its raid is over (or its leader died): the hero idle state sends it back to its landing (on its next turn;
           world_raid_hero_goes_home, which leaves alone one that is walking somewhere already) */
        internal_set_thing_state(thing, CrSt_GoodDoingNothing);
        SYNCLOG("World: the %s (index %d) leaves its wall: its raid goes home", thing_model_name(thing), (int)thing->index);
        return;
    }
    internal_set_thing_state(thing, CrSt_TunnellerDoingNothing);
    SYNCLOG("World: the %s (index %d) goes back to its way to the heart", thing_model_name(thing), (int)thing->index);
}

static void break_wall(MapSlabCoord x, MapSlabCoord y, const struct Thing *breaker)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    const PlayerNumber owner = slabmap_block_invalid(slb) ? -1 : slabmap_owner(slb);
    /* dug through (the user, 2026-11-15: "They should dig through with the timing we implemented"): open floor that
       is nobody's, with rubble; the breaker walks on through. A Beetle's hole is part of its tunnel and falls in with it
       after dawn (the keeper can fortify it again then); a raid Tunneller's stays open, as its tunnels do. */
    if (world_wild_creature(breaker))
        world_wild_tunnel_slab(x, y);
    else
        place_slab_type_on_map(SlbT_PATH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
    create_dirt_rubble_for_dug_slab(x, y);
    int64_t wx = 0, wy = 0;
    world_slab_to_world(x, y, &wx, &wy);
    SYNCLOG("World: a %s breaks through player %d's fortified wall at world slab (%ld,%ld)", thing_model_name(breaker),
        (int)owner, (long)wx, (long)wy);
}

TbBool world_wallbreak_busy(const struct Thing *breaker)
{
    if (!thing_exists(breaker))
        return false;
    for (int k = 0; k < WB_JOBS; k++)
        if ((wb_job[k].idx == breaker->index) && (wb_job[k].made == breaker->creation_turn) &&
            (game.play_gameturn <= wb_job[k].touched + 60))
            return true;
    return false;
}

/** Must this breaker leave its wall: a raider whose raid goes home, or a wild creature after dawn? */
static TbBool breaker_must_go(const struct Thing *breaker)
{
    return world_raid_hero_retreating(breaker) || (world_wild_from_last_night(breaker) && world_is_day());
}

int world_wallbreak_work(struct Thing *breaker, MapSlabCoord x, MapSlabCoord y)
{
    if (!thing_exists(breaker) || !world_wallbreak_can(x, y))
        return 0;
    {
        /* only a wall it stands beside (a breaker that went off fighting kept asking about its old wall from far away,
           and its minute started again where it stood: it would have broken a wall a dozen slabs off) */
        const MapSlabCoord bsx = subtile_slab(breaker->mappos.x.stl.num), bsy = subtile_slab(breaker->mappos.y.stl.num);
        if ((abs((int)bsx - (int)x) > 1) || (abs((int)bsy - (int)y) > 1))
        {
            for (int k = 0; k < WB_JOBS; k++)
                if ((wb_job[k].idx == breaker->index) && (wb_job[k].made == breaker->creation_turn))
                {
                    wb_job[k].idx = 0;
                    back_to_task(breaker);
                }
            return 0;
        }
    }
    if (breaker_must_go(breaker))
    {
        /* dawn comes before the wall (a fight still comes first: hold_breakers lets a fighter be) */
        struct WbJob *gone = NULL;
        for (int k = 0; k < WB_JOBS; k++)
            if ((wb_job[k].idx == breaker->index) && (wb_job[k].made == breaker->creation_turn))
                gone = &wb_job[k];
        if (gone != NULL)
        {
            gone->idx = 0;
            back_to_task(breaker);
        }
        return 0;
    }
    int64_t wx, wy, bx, by;
    world_slab_to_world(x, y, &wx, &wy);
    world_slab_to_world(subtile_slab(breaker->mappos.x.stl.num), subtile_slab(breaker->mappos.y.stl.num), &bx, &by);
    struct WbJob *j = NULL, *free_job = NULL;
    for (int k = 0; k < WB_JOBS; k++)
    {
        if ((wb_job[k].idx == breaker->index) && (wb_job[k].made == breaker->creation_turn))
            j = &wb_job[k];
        else if ((free_job == NULL) && thing_is_invalid(job_thing(&wb_job[k])))
            free_job = &wb_job[k];
    }
    if ((j != NULL) && ((j->wx != wx) || (j->wy != wy) || (llabs(j->sx - bx) > 1) || (llabs(j->sy - by) > 1)))
        j = NULL; /* another wall, or it went off: the minute starts again */
    if (j == NULL)
    {
        for (int k = 0; k < WB_JOBS; k++)
            if ((wb_job[k].idx == breaker->index) && (wb_job[k].made == breaker->creation_turn))
                j = &wb_job[k];
        if (j == NULL)
            j = free_job;
        if (j == NULL)
            return 0; /* (too many at once) */
        j->idx = breaker->index;
        j->made = breaker->creation_turn;
        j->wx = wx;
        j->wy = wy;
        j->sx = bx;
        j->sy = by;
        j->until = game.play_gameturn + WALLBREAK_TURNS;
        SYNCLOG("World: a %s (index %d) starts breaking a fortified wall at world slab (%ld,%ld) (a minute)",
            thing_model_name(breaker), (int)breaker->index, (long)wx, (long)wy);
    }
    j->touched = game.play_gameturn;
    if (game.play_gameturn >= j->until)
    {
        break_wall(x, y, breaker);
        j->idx = 0;
        back_to_task(breaker);
        return 2;
    }
    return 1;
}

static struct WbJob *job_of(const struct Thing *thing)
{
    for (int k = 0; k < WB_JOBS; k++)
        if ((wb_job[k].idx == thing->index) && (wb_job[k].made == thing->creation_turn))
            return &wb_job[k];
    return NULL;
}

/** Each turn: the breakers at work stay where they stand (a fight comes first); a job its breaker stopped asking about
 *  for three seconds ends. */
static void hold_breakers(void)
{
    for (int k = 0; k < WB_JOBS; k++)
    {
        struct WbJob *j = &wb_job[k];
        if (j->idx == 0)
            continue;
        struct Thing *thing = job_thing(j);
        if (thing_is_invalid(thing))
        {
            j->idx = 0;
            continue;
        }
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (breaker_must_go(thing))
        {
            /* dawn: the raid goes home and the wall waits; one in a fight finishes it first and is let go after */
            if (!creature_control_invalid(cctrl) && (cctrl->combat_flags != 0))
                continue;
            j->idx = 0;
            back_to_task(thing);
            continue;
        }
        MapSlabCoord wx, wy;
        if ((game.play_gameturn > j->touched + 60) || !world_engine_slab(j->wx, j->wy, &wx, &wy) || !world_wallbreak_can(wx, wy))
        {
            /* its breaker stopped asking, or the wall is no longer one it can break (dug, or inside a ring now) */
            j->idx = 0;
            back_to_task(thing);
            continue;
        }
        if (creature_control_invalid(cctrl) || (cctrl->combat_flags != 0))
            continue;
        MapSlabCoord ex, ey;
        if (!world_engine_slab(j->sx, j->sy, &ex, &ey))
            continue;
        if (get_creature_state_besides_interruptions(thing) != CrSt_MoveToPosition)
            stand_at(thing, ex, ey);
    }
}

/** Once a second: the raid's Tunnellers. One that gets no nearer to its target for WB_STALL_SECONDS and stands beside a
 *  breakable wall that lies on its way (nearer the target than where it stands) breaks it. */
static void tunnellers_second(void)
{
    static long tunneller = -2;
    if (tunneller == -2)
        tunneller = get_id(creature_desc, "TUNNELLER");
    if (tunneller <= 0)
        return;
    const struct Dungeon *heroes = get_players_num_dungeon(PLAYER_GOOD);
    if (dungeon_invalid(heroes))
        return;
    long guard = 0;
    for (ThingIndex i = heroes->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if ((thing->model != tunneller) || !world_raid_hero(thing) || creature_is_dying(thing) || thing_is_picked_up(thing))
            continue;
        if (world_raid_hero_retreating(thing))
            continue; /* going home: no walls (hold_breakers let it go from one it was at) */
        const PlayerNumber target = cctrl->party.target_plyr_idx;
        if ((target < 0) || (target >= PLAYERS_COUNT))
            continue;
        struct Thing *heart = find_players_dungeon_heart(target);
        if (!thing_exists(heart))
            continue;
        /* its record */
        struct WbTunneller *t = NULL, *free_t = NULL;
        for (int k = 0; k < WB_TRACKED; k++)
        {
            if ((wb_tun[k].idx == thing->index) && (wb_tun[k].made == thing->creation_turn))
                t = &wb_tun[k];
            else if (free_t == NULL)
            {
                const struct Thing *o = (wb_tun[k].idx != 0) ? thing_get(wb_tun[k].idx) : INVALID_THING;
                if (!thing_exists(o) || (o->creation_turn != wb_tun[k].made))
                    free_t = &wb_tun[k];
            }
        }
        if (t == NULL)
        {
            if (free_t == NULL)
                continue;
            t = free_t;
            memset(t, 0, sizeof(*t));
            t->idx = thing->index;
            t->made = thing->creation_turn;
            t->best = LONG_MAX;
        }
        const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num), y = subtile_slab(thing->mappos.y.stl.num);
        const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
        const long d = (long)(x - hx) * (x - hx) + (long)(y - hy) * (y - hy);
        {
            /* at work at a wall (begun here or by its guide, world_daynight.c): on with it each second until it breaks
               (held where it stands, the leader asks its guide for nothing meanwhile) */
            const struct WbJob *jj = job_of(thing);
            MapSlabCoord ex, ey;
            if ((jj != NULL) && (cctrl->combat_flags != 0))
                continue; /* a fight first: nothing asked meanwhile (if it ends away from the wall, the job lapses) */
            if ((jj != NULL) && world_engine_slab(jj->wx, jj->wy, &ex, &ey))
            {
                if (world_wallbreak_work(thing, ex, ey) != 1)
                {
                    t->has_wall = 0;
                    t->stall = 0;
                    t->best = d;
                }
                continue;
            }
        }
        if (t->has_wall)
        {
            MapSlabCoord ex, ey;
            const int r = world_engine_slab(t->wx, t->wy, &ex, &ey) ? world_wallbreak_work(thing, ex, ey) : 0;
            if (r != 1)
            {
                t->has_wall = 0;
                t->stall = 0;
                t->best = d;
            }
            continue;
        }
        if (d < t->best)
        {
            t->best = d;
            t->stall = 0;
            continue;
        }
        if ((++t->stall < WB_STALL_SECONDS) || (cctrl->combat_flags != 0))
            continue;
        /* beside a breakable wall that is nearer the target than this slab: the nearest such one */
        static const int8_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        long best_wd = d;
        int best_k = -1;
        for (int k = 0; k < 4; k++)
        {
            const MapSlabCoord wx = x + dx[k], wy = y + dy[k];
            const long wd = (long)(wx - hx) * (wx - hx) + (long)(wy - hy) * (wy - hy);
            if ((wd < best_wd) && world_wallbreak_can(wx, wy))
            {
                best_wd = wd;
                best_k = k;
            }
        }
        if (best_k < 0)
            continue;
        world_slab_to_world(x + dx[best_k], y + dy[best_k], &t->wx, &t->wy);
        t->has_wall = (world_wallbreak_work(thing, x + dx[best_k], y + dy[best_k]) == 1) ? 1 : 0;
    }
}

/** Test hook KFX_WORLD_WALLTEST=T: at T, east of player 0's heart, two of its fortified walls 24 slabs out (outside the
 *  Heart's ring) with a neutral Beetle beside one and a heroes' Tunneller beside the other, and one 10 slabs out
 *  (inside the ring); then each second both work at their walls until those break. Logged. */
static void walltest_turn(void)
{
    static long at = -2;
    static ThingIndex bug = 0, tun = 0;
    static GameTurn bug_made, tun_made;
    static TbBool bug_done, tun_done;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_WALLTEST");
        at = (v != NULL) ? atol(v) : -1;
    }
    if (at < 0)
        return;
    const long t = (long)game.play_gameturn - at;
    if ((t < 0) || (t > WALLBREAK_TURNS + 200))
        return;
    struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 0)
    {
        const MapSlabCoord walls[3][2] = {{hx + 24, hy}, {hx + 24, hy + 3}, {hx + 10, hy + 6}};
        for (int k = 0; k < 3; k++)
        {
            place_slab_type_on_map(SlbT_WALLDRAPE, slab_subtile_center(walls[k][0]), slab_subtile_center(walls[k][1]), PLAYER0, 0);
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(walls[k][0] - 1), slab_subtile_center(walls[k][1]),
                game.neutral_player_num, 0);
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(walls[k][0] + 1), slab_subtile_center(walls[k][1]),
                game.neutral_player_num, 0);
            SYNCLOG("World wall test: player 0's fortified wall at engine slab (%d,%d), %d slabs from the heart: breakable %d",
                (int)walls[k][0], (int)walls[k][1], (int)(walls[k][0] - hx), (int)world_wallbreak_can(walls[k][0], walls[k][1]));
        }
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx + 23));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy));
        pos.z.val = 0;
        struct Thing *b = create_creature(&pos, (ThingModel)get_id(creature_desc, "BUG"), PLAYER_GOOD);
        pos.x.val = subtile_coord_center(slab_subtile_center(hx + 25));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 3));
        struct Thing *u = create_creature(&pos, (ThingModel)get_id(creature_desc, "TUNNELLER"), PLAYER_GOOD);
        if (thing_exists(b)) { bug = b->index; bug_made = b->creation_turn; }
        if (thing_exists(u)) { tun = u->index; tun_made = u->creation_turn; }
        bug_done = tun_done = false;
        return;
    }
    if ((t % 20) != 0)
        return;
    struct Thing *b = thing_get(bug), *u = thing_get(tun);
    if (!bug_done && thing_exists(b) && (b->creation_turn == bug_made))
    {
        const int r = world_wallbreak_work(b, hx + 24, hy);
        if (r != 1)
        {
            bug_done = true;
            SYNCLOG("World wall test: the Beetle's wall: %s after %ld turns; the slab is %s now", (r == 2) ? "broken" : "not breakable", t,
                slab_code_name(get_slabmap_block(hx + 24, hy)->kind));
        }
    }
    if (!tun_done && thing_exists(u) && (u->creation_turn == tun_made))
    {
        const int r = world_wallbreak_work(u, hx + 24, hy + 3);
        if (r != 1)
        {
            tun_done = true;
            SYNCLOG("World wall test: the Tunneller's wall: %s after %ld turns; the slab is %s now", (r == 2) ? "broken" : "not breakable", t,
                slab_code_name(get_slabmap_block(hx + 24, hy + 3)->kind));
        }
    }
    if ((t % 400) == 0)
        SYNCLOG("World wall test: turn %ld: the Beetle at slab (%d,%d), the Tunneller at slab (%d,%d)", t,
            thing_exists(b) ? (int)subtile_slab(b->mappos.x.stl.num) : -1, thing_exists(b) ? (int)subtile_slab(b->mappos.y.stl.num) : -1,
            thing_exists(u) ? (int)subtile_slab(u->mappos.x.stl.num) : -1, thing_exists(u) ? (int)subtile_slab(u->mappos.y.stl.num) : -1);
}

void world_wallbreak_turn(void)
{
    if (!world_mode_active())
        return;
    walltest_turn();
    hold_breakers();
    if ((game.play_gameturn % 20) == 7)
        tunnellers_second();
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

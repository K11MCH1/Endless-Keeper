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
#include "creature_instances.h"
#include "creature_groups.h"
#include "config_crtrstates.h"
#include "config_terrain.h"
#include "thing_physics.h"
#include "bflib_planar.h"
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
    uint8_t stepping;       /* it walks into the hole it broke (bx, by), to go on from there (chain_through) */
    int64_t bx, by;         /* the hole, world slab */
    int64_t fx, fy;         /* where it stood to break it, world slab */
    GameTurn step_from;
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

static long wb_tunneller_model(void)
{
    static long tunneller = -2;
    if (tunneller == -2)
        tunneller = get_id(creature_desc, "TUNNELLER");
    return tunneller;
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
    for (int k = 0; k < WB_TRACKED; k++)
        if (wb_tun[k].stepping && (wb_tun[k].idx == breaker->index) && (wb_tun[k].made == breaker->creation_turn))
            return true; /* stepping into the hole it broke, to go on through */
    return false;
}

/** Open ground a creature walks on (not earth, which a Tunneller digs; not a wall, door, rock, gold or liquid). */
static TbBool wb_walkable(MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return false;
    const SlabKind k = slb->kind;
    if ((k == SlbT_EARTH) || (k == SlbT_TORCHDIRT) || (k == SlbT_ROCK) || (k == SlbT_LAVA) || (k == SlbT_WATER))
        return false;
    const struct SlabConfigStats *st = get_slab_kind_stats(k);
    return (st->block_flags & (SlbAtFlg_IsDoor | SlbAtFlg_Filled | SlbAtFlg_Valuable | SlbAtFlg_Digable)) == 0;
}

static struct WbTunneller *tun_record(const struct Thing *thing, TbBool make)
{
    struct WbTunneller *free_t = NULL;
    for (int k = 0; k < WB_TRACKED; k++)
    {
        if ((wb_tun[k].idx == thing->index) && (wb_tun[k].made == thing->creation_turn))
            return &wb_tun[k];
        if ((free_t == NULL) && make)
        {
            const struct Thing *o = (wb_tun[k].idx != 0) ? thing_get(wb_tun[k].idx) : INVALID_THING;
            if (!thing_exists(o) || (o->creation_turn != wb_tun[k].made))
                free_t = &wb_tun[k];
        }
    }
    if (free_t != NULL)
    {
        memset(free_t, 0, sizeof(*free_t));
        free_t->idx = thing->index;
        free_t->made = thing->creation_turn;
        free_t->best = LONG_MAX;
    }
    return free_t;
}

/** A heroes' Tunneller that broke a wall goes on through it (the user, 2026-11-18: "When tunneller finds itself against
 *  a diagonal dug out tunnel that is fortified, it will only destroy corners and move on to the next bottom corner
 *  instead of destroying the wall adjacent of the corner wall, never breaching into the dungeon"). A corner of a
 *  diagonal corridor touches the corridor only at a corner, where nobody can pass; sent back to its tunnelling the
 *  Tunneller followed the walls along to the next corner. Now it steps into the hole and goes on from there
 *  (steppers_turn): into the dungeon if the hole opens on its floor, else through the next breakable wall nearer the
 *  heart. True: it steps in (not back to its task yet). */
static TbBool chain_through(struct Thing *breaker, MapSlabCoord x, MapSlabCoord y)
{
    if (!thing_exists(breaker) || (breaker->model != wb_tunneller_model()) || (breaker->owner != PLAYER_GOOD) ||
        world_wild_creature(breaker) || world_raid_hero_retreating(breaker))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(breaker);
    if (creature_control_invalid(cctrl) || (cctrl->party.target_plyr_idx < 0) || (cctrl->party.target_plyr_idx >= PLAYERS_COUNT))
        return false;
    struct WbTunneller *t = tun_record(breaker, true);
    if (t == NULL)
        return false;
    world_slab_to_world(x, y, &t->bx, &t->by);
    world_slab_to_world(subtile_slab(breaker->mappos.x.stl.num), subtile_slab(breaker->mappos.y.stl.num), &t->fx, &t->fy);
    t->stepping = 1;
    t->has_wall = 0;
    t->step_from = game.play_gameturn;
    stand_at(breaker, x, y);
    return true;
}

/** Each turn: the Tunnellers stepping into the holes they broke. In the hole: into the dungeon when a slab beside it is
 *  open floor, else on through the breakable wall beside it nearest the heart (nearer than where it stood); else back
 *  to its task. */
static void steppers_turn(void)
{
    for (int k = 0; k < WB_TRACKED; k++)
    {
        struct WbTunneller *t = &wb_tun[k];
        if (!t->stepping)
            continue;
        struct Thing *thing = thing_get(t->idx);
        if (!thing_exists(thing) || (thing->creation_turn != t->made) || creature_is_dying(thing) || thing_is_picked_up(thing))
        {
            t->stepping = 0;
            continue;
        }
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
        {
            t->stepping = 0;
            continue;
        }
        if (cctrl->combat_flags != 0)
            continue; /* a fight first */
        MapSlabCoord ex, ey, sx, sy;
        if (world_raid_hero_retreating(thing) || !world_engine_slab(t->bx, t->by, &ex, &ey) ||
            !world_engine_slab(t->fx, t->fy, &sx, &sy) || ((GameTurnDelta)(game.play_gameturn - t->step_from) > 200))
        {
            t->stepping = 0;
            back_to_task(thing);
            continue;
        }
        if ((subtile_slab(thing->mappos.x.stl.num) != ex) || (subtile_slab(thing->mappos.y.stl.num) != ey))
        {
            if (get_creature_state_besides_interruptions(thing) != CrSt_MoveToPosition)
                stand_at(thing, ex, ey);
            continue;
        }
        t->stepping = 0;
        static const int8_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        int64_t wbx, wby;
        world_slab_to_world(ex, ey, &wbx, &wby);
        TbBool opens = false;
        for (int d = 0; d < 4; d++)
            if (!((ex + dx[d] == sx) && (ey + dy[d] == sy)) && wb_walkable(ex + dx[d], ey + dy[d]))
                opens = true;
        struct Thing *heart = find_players_dungeon_heart(cctrl->party.target_plyr_idx);
        if (opens || !thing_exists(heart))
        {
            SYNCLOG("World: the %s (index %d) is through the wall at world slab (%ld,%ld)%s", thing_model_name(thing),
                (int)thing->index, (long)wbx, (long)wby, opens ? ": it opens on the dungeon" : "");
            back_to_task(thing);
            continue;
        }
        const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
        long best = (long)(sx - hx) * (sx - hx) + (long)(sy - hy) * (sy - hy);
        int best_d = -1;
        for (int d = 0; d < 4; d++)
        {
            const MapSlabCoord nx = ex + dx[d], ny = ey + dy[d];
            const long nd = (long)(nx - hx) * (nx - hx) + (long)(ny - hy) * (ny - hy);
            if ((nd < best) && world_wallbreak_can(nx, ny))
            {
                best = nd;
                best_d = d;
            }
        }
        if (best_d < 0)
        {
            SYNCLOG("World: the %s (index %d) broke the wall at world slab (%ld,%ld) but no wall it can break lies beyond",
                thing_model_name(thing), (int)thing->index, (long)wbx, (long)wby);
            back_to_task(thing);
            continue;
        }
        const MapSlabCoord nx = ex + dx[best_d], ny = ey + dy[best_d];
        world_slab_to_world(nx, ny, &t->wx, &t->wy);
        t->has_wall = (world_wallbreak_work(thing, nx, ny) == 1) ? 1 : 0;
        SYNCLOG("World: the %s (index %d) goes on through the next fortified wall at world slab (%ld,%ld)%s",
            thing_model_name(thing), (int)thing->index, (long)t->wx, (long)t->wy, t->has_wall ? "" : ": it can't");
        if (!t->has_wall)
            back_to_task(thing);
    }
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
        if (!chain_through(breaker, x, y))
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

TbBool world_wallbreak_strike_only(const struct Thing *thing, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_mode_active() || !thing_exists(thing))
        return false;
    const struct WbJob *j = job_of(thing);
    if (j == NULL)
        return false;
    int64_t wx, wy;
    world_slab_to_world(subtile_slab(stl_x), subtile_slab(stl_y), &wx, &wy);
    return (wx == j->wx) && (wy == j->wy);
}

/** A Tunneller at its wall swings at it with the tunnelling instance (its dig animation and sound, as when heroes
 *  tunnel; the user, 2026-11-18: "I want the Tunneller to attack the wall while the digging sound plays"). The wall
 *  still breaks on the timer: instf_tunnel only plays the sound for a wall job (world_wallbreak_strike_only). Held in
 *  the hero idle state with its idle clock kept at now, so the state does nothing while it works (re-sending it to its
 *  spot each turn cleared the instance). */
static void swing_at_wall(struct Thing *thing, struct CreatureControl *cctrl, MapSlabCoord wx, MapSlabCoord wy)
{
    if (get_creature_state_besides_interruptions(thing) != CrSt_GoodDoingNothing)
        internal_set_thing_state(thing, CrSt_GoodDoingNothing);
    cctrl->idle.start_gameturn = game.play_gameturn;
    struct Coord3d wall;
    wall.x.val = subtile_coord_center(slab_subtile_center(wx));
    wall.y.val = subtile_coord_center(slab_subtile_center(wy));
    wall.z.val = thing->mappos.z.val;
    creature_set_speed(thing, 0);
    creature_turn_to_face(thing, &wall);
    if (cctrl->instance_id != CrInst_NULL)
        return;
    cctrl->navi.first_colliding_block = get_subtile_number(slab_subtile_center(wx), slab_subtile_center(wy));
    set_creature_instance(thing, CrInst_TUNNEL, 0, 0);
}

TbBool world_wallbreak_follower_wanders(struct Thing *follower, const struct Thing *leader)
{
    if (!world_mode_active() || !thing_exists(leader) || !world_wallbreak_busy(leader))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(follower);
    if (creature_control_invalid(cctrl) || (cctrl->combat_flags != 0))
        return false;
    /* only near the leader: one farther off follows as usual (back to its place behind it) */
    if (get_chessboard_distance(&follower->mappos, &leader->mappos) > subtile_coord(4 * STL_PER_SLB, 0))
        return false;
    /* (the user, 2026-11-18: "Let them wander while the Tunneller is working the fortified wall"): now and then a
       step to a slab beside it, then back to following; stood at their places behind the leader, they never moved */
    if (((game.play_gameturn + (GameTurn)follower->index * 13) % 50) == 0)
    {
        if (creature_choose_random_destination_on_valid_adjacent_slab(follower))
            follower->continue_state = CrSt_CreatureFollowLeader;
    }
    return true;
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
        const CrtrStateId st = get_creature_state_besides_interruptions(thing);
        if ((thing->model == wb_tunneller_model()) && (st != CrSt_MoveToPosition) &&
            (subtile_slab(thing->mappos.x.stl.num) == ex) && (subtile_slab(thing->mappos.y.stl.num) == ey))
        {
            swing_at_wall(thing, cctrl, wx, wy); /* at its spot: it digs at the wall */
            continue;
        }
        if (st != CrSt_MoveToPosition)
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
        if (t->stepping)
            continue; /* walking into the hole it broke: steppers_turn has it */
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
    static ThingIndex fol[2];
    static GameTurn fol_made[2];
    static int swings;
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
        /* open ground behind the Tunneller's wall, for its party to wander on */
        for (int ox = 25; ox <= 27; ox++)
            for (int oy = 2; oy <= 4; oy++)
                place_slab_type_on_map(SlbT_PATH, slab_subtile_center(hx + ox), slab_subtile_center(hy + oy), game.neutral_player_num, 0);
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
        /* two followers in the Tunneller's party: they should wander while it works (world_wallbreak_follower_wanders) */
        static const char *const fol_kinds[2] = {"ARCHER", "MONK"};
        for (int k = 0; k < 2; k++)
        {
            pos.x.val = subtile_coord_center(slab_subtile_center(hx + 26 + k));
            pos.y.val = subtile_coord_center(slab_subtile_center(hy + 3));
            struct Thing *f = create_creature(&pos, (ThingModel)get_id(creature_desc, fol_kinds[k]), PLAYER_GOOD);
            fol[k] = 0;
            if (thing_exists(f) && thing_exists(u) && add_creature_to_group(f, u))
            {
                fol[k] = f->index;
                fol_made[k] = f->creation_turn;
            }
        }
        swings = 0;
        return;
    }
    {
        /* the Tunneller's swings at its wall (a tunnelling instance begun) */
        struct Thing *ut = thing_get(tun);
        if (!tun_done && thing_exists(ut) && (ut->creation_turn == tun_made))
        {
            const struct CreatureControl *uc = creature_control_get_from_thing(ut);
            if (!creature_control_invalid(uc) && (uc->instance_id == CrInst_TUNNEL) && (uc->inst_turn == 1))
                swings++;
        }
    }
    if ((t % 200) == 0)
    {
        for (int k = 0; k < 2; k++)
        {
            struct Thing *f = thing_get(fol[k]);
            if ((fol[k] == 0) || !thing_exists(f) || (f->creation_turn != fol_made[k]))
                continue;
            SYNCLOG("World wall test: turn %ld: follower %s at subtile (%d,%d), state %s; the Tunneller has swung %d times", t,
                thing_model_name(f), (int)f->mappos.x.stl.num, (int)f->mappos.y.stl.num,
                creature_state_code_name(get_creature_state_besides_interruptions(f)), swings);
        }
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

/** Test hook KFX_WORLD_WALLDIAG=T: at T, 30 slabs east and 10 south of player 0's heart, a diagonal corridor of its
 *  claimed floor with every slab round it fortified (the corners too, as imps fortify), and a heroes' Tunneller (its
 *  target player 0) on the far side against a corner, which it starts breaking at T+20. It should break the corner and
 *  then the wall behind it, and stand open on the corridor (the user's screenshot, 2026-11-18). Logged. */
static void walldiag_turn(void)
{
    static long at = -2;
    static ThingIndex tun = 0;
    static GameTurn tun_made;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_WALLDIAG");
        at = (v != NULL) ? atol(v) : -1;
    }
    if (at < 0)
        return;
    const long t = (long)game.play_gameturn - at;
    if ((t < 0) || (t > 3 * WALLBREAK_TURNS + 400))
        return;
    struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord X = subtile_slab(heart->mappos.x.stl.num) + 30, Y = subtile_slab(heart->mappos.y.stl.num) + 10;
    if (t == 0)
    {
        for (int ox = -6; ox <= 6; ox++)
            for (int oy = -6; oy <= 6; oy++)
                place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(X + ox), slab_subtile_center(Y + oy), game.neutral_player_num, 0);
        for (int i = -4; i <= 4; i++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(X + i), slab_subtile_center(Y - i), PLAYER0, 0);
        for (int i = -4; i <= 4; i++)
            for (int ox = -1; ox <= 1; ox++)
                for (int oy = -1; oy <= 1; oy++)
                {
                    const int cx = i + ox, cy = -i + oy;
                    if ((cx == -cy) && (cx >= -4) && (cx <= 4))
                        continue; /* the corridor */
                    if (get_slabmap_block(X + cx, Y + cy)->kind == SlbT_WALLDRAPE)
                        continue;
                    place_slab_type_on_map(SlbT_WALLDRAPE, slab_subtile_center(X + cx), slab_subtile_center(Y + cy), PLAYER0, 0);
                }
        place_slab_type_on_map(SlbT_PATH, slab_subtile_center(X + 1), slab_subtile_center(Y + 2), game.neutral_player_num, 0);
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(X + 1));
        pos.y.val = subtile_coord_center(slab_subtile_center(Y + 2));
        pos.z.val = 0;
        struct Thing *u = create_creature(&pos, (ThingModel)get_id(creature_desc, "TUNNELLER"), PLAYER_GOOD);
        if (thing_exists(u))
        {
            tun = u->index;
            tun_made = u->creation_turn;
            creature_control_get_from_thing(u)->party.target_plyr_idx = PLAYER0;
        }
        SYNCLOG("World diagonal wall test: corridor through engine slab (%d,%d); the corner (%d,%d) breakable %d, the wall behind it (%d,%d) breakable %d",
            (int)X, (int)Y, (int)(X + 1), (int)(Y + 1), (int)world_wallbreak_can(X + 1, Y + 1), (int)X, (int)(Y + 1),
            (int)world_wallbreak_can(X, Y + 1));
        return;
    }
    struct Thing *u = thing_get(tun);
    if (!thing_exists(u) || (u->creation_turn != tun_made))
        return;
    if (t == 20)
        world_wallbreak_work(u, X + 1, Y + 1);
    else if ((t % 20) == 0)
    {
        /* as tunnellers_second does for a raid's Tunneller: on with the wall it works at */
        const struct WbJob *j = job_of(u);
        MapSlabCoord ex, ey;
        if ((j != NULL) && world_engine_slab(j->wx, j->wy, &ex, &ey))
            world_wallbreak_work(u, ex, ey);
    }
    if (((t % 400) == 0) || (t == 3 * WALLBREAK_TURNS + 400))
    {
        const MapSlabCoord ux = subtile_slab(u->mappos.x.stl.num), uy = subtile_slab(u->mappos.y.stl.num);
        int open_beside = 0;
        static const int8_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; d++)
            if (get_slabmap_block(ux + dx[d], uy + dy[d])->kind == SlbT_CLAIMED)
                open_beside++;
        SYNCLOG("World diagonal wall test: turn %ld: the Tunneller at engine slab (%d,%d) (%+d,%+d from the corridor's middle), "
            "beside %d corridor slabs; corner %s, wall behind %s", t, (int)ux, (int)uy, (int)(ux - X), (int)(uy - Y), open_beside,
            slab_code_name(get_slabmap_block(X + 1, Y + 1)->kind), slab_code_name(get_slabmap_block(X, Y + 1)->kind));
    }
}

void world_wallbreak_turn(void)
{
    if (!world_mode_active())
        return;
    walltest_turn();
    walldiag_turn();
    hold_breakers();
    steppers_turn();
    if ((game.play_gameturn % 20) == 7)
        tunnellers_second();
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

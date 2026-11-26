/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keeper_ai.c
 *     Endless worlds: the enemy keepers' brain over KeeperFX's computer player (2026-11-23).
 * @par Purpose:
 *     The user, 2026-11-23: enemy keepers were "too simplistic": they had trouble growing their dungeon, nurturing their
 *     creatures, exploring for points of interest, attacking heroes, capturing and converting them; "Encountering
 *     Players shouldn't always be met with an instant assault, they should know how to bully the player, know when to
 *     retreat, know when to be defensive, know when to be aggressive. They should be able to know when they are dealing
 *     with too much and be able to cut themselves off [...] Basically smart ai that can have multiple personalities."
 *
 *     KeeperFX's computer player stays the keeper's hands (digging, building rooms, dropping creatures, its traps,
 *     doors and prison tendency). This brain thinks for each awake keeper every few seconds and steers those hands:
 *     - it measures its own army (world_threat.c), the enemies inside its home and near it, its wounded;
 *     - it takes a stance: Calm (nurture and explore), Probe (a small party tests a neighbour), Bully (hunting a weaker
 *       neighbour's Imps), Assault (KeeperFX's attack plans are let loose on that neighbour, and its creatures are
 *       carried to the fight), Retreat (an assault that cost too much: creatures carried home), Defend (intruders at
 *       home: Call to Arms, creatures dropped on them) and Turtle (too many intruders: it also cuts its tunnels with the
 *       Cave-In fill and turns the cut into a trap place, for the stock trap check to fill with its best trap);
 *     - KeeperFX's attack plans only run when the brain says Assault (computer_check_attack_with_filter asks);
 *     - Calm: it chooses what its Library levels (the player's rules: room levels by time, the Training Room's creature
 *       level cap), sends creatures to train, carries prisoners to torture, digs towards portals, neutral creatures and
 *       hero outposts, and raids an outpost when strong enough.
 *     A personality, the same for a keeper in every load of the world (from its record), weighs all of this: the
 *     Warlord, the Bully, the Turtle, the Collector, the Torturer, the Opportunist and the Diplomat.
 *     When a keeper wakes after being left alone it has grown: gold and more room for creatures (world_keeper_ai_woke).
 *
 *     Game state: it runs on the simulating machine (lockstep machines alike: no randomness of its own, only the game's
 *     state), and is saved (the world extra KAI1).
 *
 * Test hooks: KFX_WORLD_KAILOG=1 logs every thought; KFX_WORLD_KAIPERSONA=n gives every keeper personality n (0-6).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_keeper_ai.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bflib_basics.h"
#include "bflib_math.h"
#include "config.h"
#include "config_creature.h"
#include "config_magic.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "map_data.h"
#include "player_computer.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "power_hand.h"
#include "room_data.h"
#include "room_util.h"
#include "config_trapdoor.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "world_daynight.h"
#include "world_fill.h"
#include "world_gen.h"
#include "world_outpost.h"
#include "world_progress.h"
#include "world_rivals.h"
#include "world_stream.h"
#include "world_threat.h"
#include "world_truce.h"
#include "thing_navigate.h"
#include "bflib_planar.h"
#include "map_blocks.h"
#include "creature_states_combt.h"
#include "game_merge.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define KAI_THINK 64                 /* turns between a keeper's thoughts */
#define KAI_HOME_R 20                /* slabs from its heart: home */
#define KAI_CONTACT_R 70             /* slabs from its heart: neighbours it reacts to */
#define KAI_REACH_R 120              /* slabs: hearts it may march on */
#define KAI_EXPLORE_R 90             /* slabs: points of interest it digs to */
#define KAI_PROBE_GAP (20 * 120)     /* turns between two probes or bullying runs */
#define KAI_EXPLORE_GAP (20 * 150)
#define KAI_CUT_GAP (20 * 60)
#define KAI_RETREAT_TURNS (20 * 90)  /* a retreat lasts at least this */
#define KAI_TRAIN_GAP (20 * 120)
#define KAI_REGROUP_TURNS (20 * 180) /* after a retreat, no assault on the same keeper this long */
#define KAI_IDLE_ASSAULT (20 * 60)   /* an assault with nothing to march on ends after this */
#define KAI_FORTIFY_GAP (20 * 120)   /* turns between two looks at its defences (the Turtle: a third of it) */
#define KAI_OUTPOST_GAP (20 * 300)   /* turns between two looks for a far portal to build beside */
#define KAI_OUTPOST_FAR 25           /* slabs from its heart: a portal this far gets a Lair beside it */
#define KAI_KEEP_OUT 12
#define KAI_CTA_TURNS (20 * 25) /* how long a party's Call To Arms stays up */
#define KAI_MIN_FIGHTERS 3 /* fewer able creatures than this: no party goes out (no bullying, probing or assault) */              /* slabs: it claims nothing this near another keeper's ground (not assaulting) */
#define KAI_CATCH_UP_MAX 8           /* rooms built or grown at most when it wakes */

enum KaiPersonality { KaiP_Warlord = 0, KaiP_Bully, KaiP_Turtle, KaiP_Collector, KaiP_Torturer, KaiP_Opportunist,
    KaiP_Diplomat, KaiP_COUNT };

struct KaiTraits {
    const char *name;
    int assault;      /* % of a neighbour's army its own must be to march on it */
    int bully;        /* % (of the neighbour's army) at which it hunts its Imps; 0: never */
    int probe;        /* 0-100: how readily it tests a neighbour with a small party */
    int retreat;      /* % of its army an assault may cost before it falls back */
    int turtle;       /* intruders' power, % of its own, at which it cuts its tunnels */
    int explore;      /* 0-100: how much it digs towards points of interest */
    int outpost;      /* % of an outpost's guess it needs to raid one; 0: never */
    int needs_busy;   /* 1: only marches on a neighbour busy fighting someone else */
    int provoked;     /* 1: only marches on a neighbour that came into its home first */
    const int8_t *focus; /* what its Library levels first (Amenity Mastery entries), -1 ends */
};

/* entries: 0 Lair, 1 Hatchery, 2 Library, 3 Training Room, 4-10 spells (4 Create Imp, 8 Heal, 9 Lightning, 10 Protect),
   11 Treasury, 12 Workshop, 13 Prison, 14 Torture, 15 Barracks, 16 Graveyard */
static const int8_t kai_focus_war[] = {3, 9, 10, 0, 1, -1};
static const int8_t kai_focus_bully[] = {3, 6, 9, 1, -1};
static const int8_t kai_focus_turtle[] = {12, 3, 10, 8, 0, -1};
static const int8_t kai_focus_collect[] = {2, 3, 11, 4, -1};
static const int8_t kai_focus_torture[] = {13, 14, 3, 16, -1};
static const int8_t kai_focus_opport[] = {3, 9, 6, 2, -1};
static const int8_t kai_focus_diplo[] = {11, 2, 3, 8, -1};

static const struct KaiTraits kai_traits[KaiP_COUNT] = {
    /* name           assault bully probe retreat turtle explore outpost busy provoked focus */
    {"the Warlord",      100,   0,   60,     70,    250,     30,    90,    0,   0, kai_focus_war},
    {"the Bully",        160,  80,   80,     30,    150,     30,     0,    0,   0, kai_focus_bully},
    {"the Turtle",       260,   0,   10,     25,     80,     20,     0,    0,   0, kai_focus_turtle},
    {"the Collector",    300,   0,   20,     30,    100,     90,   120,    0,   0, kai_focus_collect},
    {"the Torturer",     140,   0,   40,     40,    140,     40,   110,    0,   0, kai_focus_torture},
    {"the Opportunist",  110,  90,   50,     35,    130,     50,   150,    1,   0, kai_focus_opport},
    {"the Diplomat",     130,   0,    0,     25,    120,     50,     0,    0,   1, kai_focus_diplo},
};

enum KaiStance { KaiS_Calm = 0, KaiS_Probe, KaiS_Bully, KaiS_Assault, KaiS_Retreat, KaiS_Defend, KaiS_Turtle, KaiS_War,
    KaiS_COUNT };
static const char *const kai_stance_names[KaiS_COUNT] = {"calm", "probing", "bullying", "assaulting", "retreating",
    "defending", "turtling", "warring"};

#pragma pack(1)
struct KaiState {
    uint8_t used;
    uint8_t stance;
    uint8_t personality;
    int8_t target;            /* the neighbour of its stance (-1 none) */
    uint32_t since;           /* the turn its stance began */
    uint32_t last_probe;
    uint32_t last_explore;
    uint32_t last_cut;
    uint32_t last_train;
    uint32_t last_outpost;
    uint32_t last_retreat;    /* no new assault on its last target for a while after falling back */
    float assault_power;      /* its army's power when its assault began */
    PlayerBitFlags provoked_by; /* neighbours that came into its home */
    uint32_t last_fortify;    /* doors and trap places towards its foes */
    uint32_t last_outpost_build; /* a Lair beside a far portal */
};
#pragma pack()

static struct KaiState kai[PLAYERS_MAX];
static int kai_log = -1;
static int kai_forced_persona = -2;

/******************************************************************************/
static TbBool kai_logging(void)
{
    if (kai_log < 0)
        kai_log = (getenv("KFX_WORLD_KAILOG") != NULL) ? 1 : 0;
    return kai_log > 0;
}

/* Test switch KFX_KAI_CLASSIC=1 (2026-11-23, the user's play-test on a small fixed map): the brain also runs the computer
   keepers of a normal level (not an Endless world). The world's own parts stay off there: the portals and structures the
   world knows, hero outposts, the Cave-In fill and growth while asleep. Without the switch normal levels are stock. */
static TbBool kai_classic(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("KFX_KAI_CLASSIC");
        on = ((v != NULL) && (atoi(v) != 0)) ? 1 : 0;
    }
    return (on > 0) && !world_mode_active();
}

/** A keeper the brain leaves alone: a human's (in an Endless world, a dropped human's seat too). */
static TbBool kai_is_human(PlayerNumber p)
{
    if (world_mode_active())
        return world_player_is_human(p);
    const struct PlayerInfo *player = get_player(p);
    return player_exists(player) && ((player->allocflags & PlaF_CompCtrl) == 0);
}

static int kai_personality_of(uint32_t id_hash)
{
    if (kai_forced_persona == -2)
    {
        const char *v = getenv("KFX_WORLD_KAIPERSONA");
        kai_forced_persona = (v != NULL) ? max(0, min(atoi(v), KaiP_COUNT - 1)) : -1;
    }
    if (kai_forced_persona >= 0)
        return kai_forced_persona;
    return (int)(((uint64_t)id_hash * KaiP_COUNT) >> 32); /* (the hash's high bits: an even spread) */
}

const char *world_keeper_ai_personality_name(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || !kai[plyr_idx].used)
        return "";
    return kai_traits[kai[plyr_idx].personality].name;
}

static long kai_dist2(const struct Coord3d *a, const struct Coord3d *b)
{
    const long dx = (long)a->x.stl.num - b->x.stl.num, dy = (long)a->y.stl.num - b->y.stl.num;
    return dx * dx + dy * dy;
}

#define KAI_SLABS2(s) ((long)(s) * STL_PER_SLB * (long)(s) * STL_PER_SLB)

static TbBool kai_enemy(PlayerNumber p, PlayerNumber o)
{
    if ((o == p) || (o < 0) || (o >= PLAYERS_COUNT) || (o == game.neutral_player_num))
        return false;
    if (world_truce_allied(p, o))
        return false;
    return (o == PLAYER_GOOD) || players_are_enemies(p, o);
}

/** What a keeper sees in one look round. */
struct KaiView {
    struct WorldArmy army;
    int own, wounded, away;              /* its creatures, those under 40% health, those far from home */
    double home_power[PLAYERS_MAX];      /* enemies inside its home */
    double near_power[PLAYERS_MAX];      /* enemies near it */
    struct Coord3d home_pos;             /* the intruder nearest its heart */
    long home_d2;
    struct Coord3d near_pos[PLAYERS_MAX];  /* the nearest creature of each neighbour */
    long near_d2[PLAYERS_MAX];
    struct Coord3d imp_pos[PLAYERS_MAX];   /* the nearest Imp of each neighbour */
    long imp_d2[PLAYERS_MAX];
    struct Coord3d neutral_pos;          /* the nearest neutral creature (to claim) */
    long neutral_d2;
    TbBool fighting[PLAYERS_MAX];        /* a neighbour's creatures are fighting someone */
};

static void kai_look(PlayerNumber p, const struct Coord3d *heart, struct KaiView *v)
{
    memset(v, 0, sizeof(*v));
    world_threat_army(p, &v->army);
    v->home_d2 = LONG_MAX;
    v->neutral_d2 = LONG_MAX;
    for (int i = 0; i < PLAYERS_MAX; i++)
        v->near_d2[i] = v->imp_d2[i] = LONG_MAX;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if (thing_is_picked_up(t) || creature_is_dying(t))
            continue;
        const PlayerNumber o = t->owner;
        const long d2 = kai_dist2(&t->mappos, heart);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (creature_control_invalid(cctrl))
            continue;
        if (o == p)
        {
            if (thing_is_creature_digger(t))
                continue;
            v->own++;
            if ((cctrl->max_health > 0) && (t->health * 100 < cctrl->max_health * 40))
                v->wounded++;
            if (d2 > KAI_SLABS2(KAI_HOME_R))
                v->away++;
            continue;
        }
        if (o == game.neutral_player_num)
        {
            if ((d2 < v->neutral_d2) && (d2 <= KAI_SLABS2(KAI_EXPLORE_R)))
            {
                v->neutral_d2 = d2;
                v->neutral_pos = t->mappos;
            }
            continue;
        }
        if (!kai_enemy(p, o) || (o >= PLAYERS_MAX) || (d2 > KAI_SLABS2(KAI_CONTACT_R)))
            continue;
        if (creature_is_kept_in_prison(t) || creature_is_being_tortured(t))
            continue;
        if (thing_is_creature_digger(t))
        {
            if (d2 < v->imp_d2[o])
            {
                v->imp_d2[o] = d2;
                v->imp_pos[o] = t->mappos;
            }
            continue;
        }
        const double pw = world_threat_power(t->model, (int)cctrl->exp_level + 1);
        v->near_power[o] += pw;
        if (d2 < v->near_d2[o])
        {
            v->near_d2[o] = d2;
            v->near_pos[o] = t->mappos;
        }
        if (cctrl->combat_flags != 0)
            v->fighting[o] = true;
        if (d2 <= KAI_SLABS2(KAI_HOME_R))
        {
            v->home_power[o] += pw;
            if (d2 < v->home_d2)
            {
                v->home_d2 = d2;
                v->home_pos = t->mappos;
            }
        }
    }
}

/******************************************************************************/
/* Its hands (KeeperFX's computer player) */

static int kai_free_fighters(const struct KaiView *v, int share)
{
    return max(1, min(12, (v->own - v->wounded) * share / 100));
}

static TbBool kai_edge_toward(PlayerNumber p, const struct Coord3d *heart, const struct Coord3d *target, struct Coord3d *out);

static void kai_defend(struct Computer2 *comp, struct KaiState *ks, const struct KaiView *v, PlayerNumber p)
{
    struct Coord3d pos = v->home_pos;
    /* the drop: on the intruders when the Hand may put creatures there, else on its own ground nearest them (a raid's
       heroes stand in their own tunnel: the drop there failed, "Could not dump", play-test round 2026-11-25) */
    struct Coord3d drop = pos;
    const struct Thing *heartng = find_players_dungeon_heart(p);
    if (!can_drop_thing_here_as(pos.x.stl.num, pos.y.stl.num, p, 0, true) && thing_exists(heartng))
        kai_edge_toward(p, &heartng->mappos, &pos, &drop);
    if (!is_task_in_progress(comp, CTT_MoveCreaturesToDefend))
        create_task_move_creatures_to_defend(comp, &drop, kai_free_fighters(v, 70), 0);
    if (!is_task_in_progress(comp, CTT_MagicCallToArms) && computer_able_to_use_power(comp, PwrK_CALL2ARMS, 0, 1))
        create_task_magic_battle_call_to_arms(comp, &pos, 20 * 30, kai_free_fighters(v, 60));
    (void)ks;
    (void)p;
}

/** Cuts the tunnel the intruders came by: the first claimed corridor slab of its own on the line from the intruder to
 *  its heart that nothing stands on is filled (the Cave-In fill, if it has it), and becomes a trap place either way. */
static TbBool kai_cut_off(struct Computer2 *comp, PlayerNumber p, const struct Coord3d *heart, const struct Coord3d *from)
{
    const MapSlabCoord hx = subtile_slab(heart->x.stl.num), hy = subtile_slab(heart->y.stl.num);
    const MapSlabCoord fx = subtile_slab(from->x.stl.num), fy = subtile_slab(from->y.stl.num);
    const int steps = max(abs(fx - hx), abs(fy - hy));
    for (int s = 0; s <= steps; s++)
    {
        const MapSlabCoord x = fx + (MapSlabCoord)((long)(hx - fx) * s / max(steps, 1));
        const MapSlabCoord y = fy + (MapSlabCoord)((long)(hy - fy) * s / max(steps, 1));
        if ((abs(x - hx) <= 3) && (abs(y - hy) <= 3))
            break; /* (never at the heart itself) */
        const struct SlabMap *slb = get_slabmap_block(x, y);
        if (slabmap_block_invalid(slb) || (slb->kind != SlbT_CLAIMED) || (slabmap_owner(slb) != p) || (slb->room_index > 0))
            continue;
        /* a corridor: at most two open sides */
        int open = 0;
        static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++)
        {
            const struct SlabMap *n = get_slabmap_block(x + dx4[k], y + dy4[k]);
            if (!slabmap_block_invalid(n) && ((n->kind == SlbT_CLAIMED) || (n->kind == SlbT_PATH) || (n->room_index > 0)))
                open++;
        }
        if (open > 2)
            continue;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(x));
        pos.y.val = subtile_coord_center(slab_subtile_center(y));
        pos.z.val = 0;
        if (world_mode_active() && world_fill_can_mark(p, x, y) && world_fill_place(-1, p, x, y))
        {
            SYNCLOG("Keeper AI: player %d cuts its tunnel at engine slab (%d,%d) with the Cave-In fill", (int)p, (int)x, (int)y);
            return true;
        }
        add_to_trap_locations(comp, &pos);
        SYNCLOG("Keeper AI: player %d makes engine slab (%d,%d) a trap place against intruders", (int)p, (int)x, (int)y);
        return true;
    }
    return false;
}

static void kai_bring_home(struct Computer2 *comp, const struct KaiView *v, const struct Coord3d *heart)
{
    struct Coord3d pos = *heart;
    if ((v->away > 0) && !is_task_in_progress(comp, CTT_MoveCreaturesToDefend))
        create_task_move_creatures_to_defend(comp, &pos, min(v->away, 12), 0);
}

/** The keeper's own ground nearest the target on the line from its heart (a keeper drops creatures on its own ground
 *  only); false when it has none that way. */
static TbBool kai_edge_toward(PlayerNumber p, const struct Coord3d *heart, const struct Coord3d *target, struct Coord3d *out)
{
    const MapSlabCoord hx = subtile_slab(heart->x.stl.num), hy = subtile_slab(heart->y.stl.num);
    const MapSlabCoord tx = subtile_slab(target->x.stl.num), ty = subtile_slab(target->y.stl.num);
    const int steps = max(abs(tx - hx), abs(ty - hy));
    TbBool found = false;
    for (int s = 0; s <= steps; s++)
    {
        const MapSlabCoord x = hx + (MapSlabCoord)((long)(tx - hx) * s / max(steps, 1));
        const MapSlabCoord y = hy + (MapSlabCoord)((long)(ty - hy) * s / max(steps, 1));
        const struct SlabMap *slb = get_slabmap_block(x, y);
        if (slabmap_block_invalid(slb) || (slabmap_owner(slb) != p) || ((slb->kind != SlbT_CLAIMED) && (slb->room_index <= 0)))
            continue;
        out->x.val = subtile_coord_center(slab_subtile_center(x));
        out->y.val = subtile_coord_center(slab_subtile_center(y));
        out->z.val = 0;
        out->z.val = get_floor_height_at(out);
        found = true;
    }
    return found;
}

/** Marks the whole tunnel from its own ground to the target for its Imps at once (as a keeper's drag does: they dig it
 *  side by side, not a slab at a time), up to KAI_TUNNEL slabs; returns how many slabs were marked. */
#define KAI_TUNNEL 90
static TbBool kai_undiggable(const struct SlabMap *slb)
{
    /* (water is waded through, not dug: it is no wall; lava is, as is what can't be dug) */
    return slabmap_block_invalid(slb) || (slb->kind == SlbT_ROCK) || (slb->kind == SlbT_LAVA) || (slb->kind == SlbT_GEMS);
}

static int kai_mark_tunnel(PlayerNumber p, const struct Coord3d *from, const struct Coord3d *to)
{
    MapSlabCoord x = subtile_slab(from->x.stl.num), y = subtile_slab(from->y.stl.num);
    const MapSlabCoord tx = subtile_slab(to->x.stl.num), ty = subtile_slab(to->y.stl.num);
    /* a step at a time towards the target, round what can't be dug (four ways only: a creature can't squeeze between
       two corners), never back onto the last few slabs it went by */
    MapSlabCoord seen_x[24], seen_y[24];
    int seen_n = 0, seen_w = 0, marked = 0;
    char trail[KAI_TUNNEL + 1];
    int tn = 0, front_left = 12;
    static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int step = 0; (step < KAI_TUNNEL) && ((x != tx) || (y != ty)); step++)
    {
        int best = -1;
        long best_d = LONG_MAX;
        for (int k = 0; k < 4; k++)
        {
            const MapSlabCoord nx = x + dx4[k], ny = y + dy4[k];
            if (kai_undiggable(get_slabmap_block(nx, ny)))
                continue;
            TbBool been = false;
            for (int j = 0; (j < seen_n) && !been; j++)
                been = (seen_x[j] == nx) && (seen_y[j] == ny);
            if (been)
                continue;
            const long d = (long)(tx - nx) * (tx - nx) + (long)(ty - ny) * (ty - ny);
            if (d < best_d)
            {
                best_d = d;
                best = k;
            }
        }
        if (best < 0)
            break; /* (walled in by rock and its own trail: give up for now) */
        seen_x[seen_w] = x;
        seen_y[seen_w] = y;
        seen_w = (seen_w + 1) % 24;
        seen_n = min(seen_n + 1, 24);
        x += dx4[best];
        y += dy4[best];
        const struct SlabMap *ns = get_slabmap_block(x, y);
        /* only the front: the next dozen slabs still to dig (its Imps dig there together, not all along the way) */
        const TbBool solid = (ns->kind != SlbT_PATH) && (ns->kind != SlbT_CLAIMED) && (ns->kind != SlbT_WATER) &&
            (ns->room_index <= 0);
        TbBool tagged = false;
        if (solid && (front_left > 0))
        {
            front_left--;
            tagged = tag_blocks_for_digging_in_area(slab_subtile(x, 0), slab_subtile(y, 0), p);
        }
        if (tagged)
            marked++;
        if (tn < KAI_TUNNEL)
            trail[tn++] = tagged ? '+' : (ns->kind == SlbT_PATH) ? 'p' : (ns->kind == SlbT_CLAIMED) ? 'c' :
                (ns->kind == SlbT_EARTH) ? 'e' : (ns->room_index > 0) ? 'r' : 'o';
    }
    trail[tn] = 0;
    if (kai_logging())
        SYNCLOG("Keeper AI: player %d tunnel: %s, ends %d,%d slabs from the target", (int)p, trail, (int)(tx - x), (int)(ty - y));
    return marked;
}

/** A party (a probe, a hunt for Imps, a raid) goes out by a player's means only (the user, play-test 2026-11-23: creatures
 *  walked to the player's dungeon with no Call To Arms up): the tunnel there is marked from its own ground nearest the
 *  target, then a Call To Arms is cast at the target (KeeperFX's own task: the banner, its gold) when the keeper has the
 *  power and the gold for it; otherwise its Hand drops them on its own ground nearest the target, and from there they go
 *  for what they see as any creature does. */
static void kai_send_party(struct Computer2 *comp, PlayerNumber p, const struct Coord3d *heart, const struct Coord3d *where,
    int count)
{
    struct Coord3d edge;
    const TbBool has_edge = kai_edge_toward(p, heart, where, &edge);
    const int marked = has_edge ? kai_mark_tunnel(p, &edge, where) : 0;
    if ((marked > 0) && !is_task_in_progress(comp, CTT_SlapDiggers))
        create_task_slap_imps(comp, 2, false); /* (as the stock AI hurries them) */
    const char *how = "waits: a Call To Arms or a drop is under way";
    const TbBool under_way = is_task_in_progress(comp, CTT_MagicCallToArms) || is_task_in_progress(comp, CTT_MoveCreaturesToDefend);
    if (under_way)
    {
        /* (the one going out now carries on: KeeperFX's computer runs one Call To Arms at a time) */
    } else if (computer_able_to_use_power(comp, PwrK_CALL2ARMS, 0, 1))
    {
        struct Coord3d pos = *where;
        if (create_task_magic_battle_call_to_arms(comp, &pos, KAI_CTA_TURNS, count))
            how = "goes under a Call To Arms";
    } else if (has_edge)
    {
        if (create_task_move_creatures_to_defend(comp, &edge, count, 0))
            how = "is dropped by the Hand on its own ground nearest the target";
    } else
        how = "stays home: no Call To Arms and none of its ground that way";
    if (kai_logging())
        SYNCLOG("Keeper AI: player %d party of %d for engine slab (%d,%d) %s; %d tunnel slabs marked", (int)p, count,
            (int)subtile_slab(where->x.stl.num), (int)subtile_slab(where->y.stl.num), how, marked);
}

/** An assault: KeeperFX's own attack (a tunnel dug from its nearest room to the target, then its creatures sent along
 *  it), through the keeper's first attack process. */
static TbBool kai_start_attack(struct Computer2 *comp, PlayerNumber victim, const struct Coord3d *where)
{
    if (is_there_an_attack_task(comp))
        return false;
    for (int i = 0; i <= COMPUTER_PROCESSES_COUNT; i++)
    {
        struct ComputerProcess *cproc = &comp->processes[i];
        if (cproc->func_complete == 14) /* completed_attack1: an attack process */
        {
            struct Coord3d pos = *where;
            return setup_computer_attack(comp, cproc, &pos, victim) == 1;
        }
        if (flag_is_set(cproc->flags, ComProc_ListEnd))
            break;
    }
    return false;
}

static TbBool kai_may_claim(PlayerNumber p, const struct Coord3d *c);
static int kai_claim_r(const struct KaiTraits *tr);

/** Calm: what its Library levels (the player's rules), training, prisoners to torture, points of interest. */
static void kai_nurture(struct Computer2 *comp, struct KaiState *ks, const struct KaiView *v, PlayerNumber p,
    const struct Coord3d *heart)
{
    const struct KaiTraits *tr = &kai_traits[ks->personality];
    struct Dungeon *dungeon = comp->dungeon;
    /* what the Library levels: the first of its choices that can still grow */
    for (int i = 0; tr->focus[i] >= 0; i++)
        if (world_level_set_focus(p, tr->focus[i]))
            break;
    /* training, while there is gold for it */
    const RoomKind train = (RoomKind)get_id(room_desc, "TRAINING");
    if ((train > 0) && (dungeon->room_list_start[train] > 0) && (dungeon->total_money_owned > 3000) &&
        ((GameTurnDelta)(game.play_gameturn - ks->last_train) > KAI_TRAIN_GAP) && !is_task_in_progress(comp, CTT_MoveCreatureToRoom))
    {
        if (create_task_move_creatures_to_room(comp, dungeon->room_list_start[train], max(1, min(4, v->own / 3))))
            ks->last_train = game.play_gameturn;
    }
    /* prisoners to the Torture Chamber, to convert them (the Torturer above all) */
    const RoomKind torture = (RoomKind)get_id(room_desc, "TORTURE");
    if ((torture > 0) && (dungeon->room_list_start[torture] > 0) && !is_task_in_progress(comp, CTT_MoveCreatureToPos))
    {
        const struct Room *room = room_get(dungeon->room_list_start[torture]);
        const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
        long guard = 0;
        for (ThingIndex i = slist->index; (i > 0) && (guard++ < CREATURES_COUNT) && !room_is_invalid(room);)
        {
            const struct Thing *t = thing_get(i);
            if (thing_is_invalid(t))
                break;
            i = t->next_of_class;
            if ((t->owner == p) || !creature_is_kept_in_prison(t))
                continue;
            const struct Room *prison = get_room_thing_is_on(t);
            if (room_is_invalid(prison) || (prison->owner != p))
                continue;
            if (create_task_move_creature_to_subtile(comp, t, room->central_stl_x, room->central_stl_y, CrSt_Torturing))
            {
                SYNCLOG("Keeper AI: player %d carries prisoner %s index %d to torture", (int)p, thing_model_name(t), (int)t->index);
                break;
            }
        }
    }
    /* capturing: imprison what it beats, once it has a Prison (the stock check sets it too; the Torturer always) */
    const RoomKind prison = (RoomKind)get_id(room_desc, "PRISON");
    if ((prison > 0) && (dungeon->room_list_start[prison] > 0) && (ks->personality == KaiP_Torturer))
        set_creature_tendencies(get_player(p), CrTend_Imprison, true);
    /* exploring: a neutral creature, a portal nobody holds, an outpost (by its liking) */
    if ((tr->explore > 0) && ((ks->last_explore == 0) ||
        ((GameTurnDelta)(game.play_gameturn - ks->last_explore) > KAI_EXPLORE_GAP * 100 / max(tr->explore, 10))) &&
        !is_task_in_progress(comp, CTT_DigToNeutral))
    {
        struct Coord3d to;
        TbBool found = false;
        if ((v->neutral_d2 != LONG_MAX) && (v->neutral_d2 <= KAI_SLABS2(kai_claim_r(tr))) && kai_may_claim(p, &v->neutral_pos))
        {
            to = v->neutral_pos;
            found = true;
        }
        if (!found && world_mode_active())
        {
            static int64_t px[256], py[256];
            static int po[256];
            const int n = world_portals_list(px, py, po, 256);
            long best = LONG_MAX;
            for (int k = 0; k < n; k++)
            {
                if (po[k] == p)
                    continue;
                MapSlabCoord ex, ey;
                if (!world_engine_slab(px[k], py[k], &ex, &ey))
                    continue;
                struct Coord3d c;
                c.x.val = subtile_coord_center(slab_subtile_center(ex));
                c.y.val = subtile_coord_center(slab_subtile_center(ey));
                c.z.val = 0;
                const long d2 = kai_dist2(&c, heart);
                if ((d2 < best) && (d2 <= KAI_SLABS2(kai_claim_r(tr))) && kai_may_claim(p, &c))
                {
                    best = d2;
                    to = c;
                    found = true;
                }
            }
        }
        if (!found && world_mode_active())
        {
            /* what the world holds (it knows its own ground): portals and reward caves on loaded ground near it */
            struct WgWorld *gen = world_generator();
            static struct WgStructRef refs[24];
            int64_t hwx, hwy;
            world_slab_to_world(subtile_slab(heart->x.stl.num), subtile_slab(heart->y.stl.num), &hwx, &hwy);
            const int n = (gen != NULL) ? wg_structures_in(gen, hwx - KAI_EXPLORE_R, hwy - KAI_EXPLORE_R, hwx + KAI_EXPLORE_R,
                hwy + KAI_EXPLORE_R, (1u << WgT_Portal) | (1u << WgT_Reward), refs, 24) : 0;
            long best = LONG_MAX;
            for (int k = 0; k < n; k++)
            {
                const int64_t sx = (refs[k].id.type == WgT_Portal) ? refs[k].portal_x : (refs[k].bbox_x1 + refs[k].bbox_x2) / 2;
                const int64_t sy = (refs[k].id.type == WgT_Portal) ? refs[k].portal_y : (refs[k].bbox_y1 + refs[k].bbox_y2) / 2;
                MapSlabCoord ex, ey;
                if (!world_engine_slab(sx, sy, &ex, &ey))
                    continue;
                const struct SlabMap *slb = get_slabmap_block(ex, ey);
                if (slabmap_block_invalid(slb) || (slabmap_owner(slb) == p))
                    continue; /* (its own already) */
                struct Coord3d c;
                c.x.val = subtile_coord_center(slab_subtile_center(ex));
                c.y.val = subtile_coord_center(slab_subtile_center(ey));
                c.z.val = 0;
                const long d2 = kai_dist2(&c, heart);
                if ((d2 < best) && (d2 <= KAI_SLABS2(kai_claim_r(tr))) && kai_may_claim(p, &c))
                {
                    best = d2;
                    to = c;
                    found = true;
                }
            }
        }
        if (!found && kai_logging())
            SYNCLOG("Keeper AI: player %d (%s) finds nothing to dig to within %d slabs", (int)p, tr->name, kai_claim_r(tr));
        if (found)
        {
            struct Coord3d from = *heart;
            if (create_task_dig_to_neutral(comp, from, to))
                SYNCLOG("Keeper AI: player %d (%s) digs towards engine slab (%d,%d)", (int)p, tr->name,
                    (int)subtile_slab(to.x.stl.num), (int)subtile_slab(to.y.stl.num));
        }
        ks->last_explore = game.play_gameturn;
    }
    /* an outpost: raided when its army is strong enough for the guess of what guards it */
    if (world_mode_active() && (tr->outpost > 0) && ((ks->last_outpost == 0) || ((GameTurnDelta)(game.play_gameturn - ks->last_outpost) > KAI_EXPLORE_GAP * 2)))
    {
        ks->last_outpost = game.play_gameturn;
        int64_t wx, wy;
        world_slab_to_world(subtile_slab(heart->x.stl.num), subtile_slab(heart->y.stl.num), &wx, &wy);
        int64_t x0, y0, exw, eyw, hxw, hyw, gx, gy;
        int odx, ody;
        if (world_outpost_nearest(wx, wy, &x0, &y0, &exw, &eyw, &odx, &ody, &hxw, &hyw, &gx, &gy))
        {
            MapSlabCoord ex, ey;
            const long dx = (long)(exw - wx), dy = (long)(eyw - wy);
            /* the guess of what guards it: six Barbarians at the level of its own army's upper quarter */
            const ThingModel guard_kind = (ThingModel)max(1L, (long)get_id(creature_desc, "BARBARIAN"));
            const double guess = 6.0 * world_threat_power(guard_kind, v->army.upper);
            if ((dx * dx + dy * dy <= (long)KAI_EXPLORE_R * KAI_EXPLORE_R) && world_engine_slab(exw, eyw, &ex, &ey) &&
                (v->army.power * 100.0 >= guess * tr->outpost))
            {
                struct Coord3d to;
                to.x.val = subtile_coord_center(slab_subtile_center(ex));
                to.y.val = subtile_coord_center(slab_subtile_center(ey));
                to.z.val = 0;
                kai_send_party(comp, p, heart, &to, kai_free_fighters(v, 50));
                SYNCLOG("Keeper AI: player %d (%s) raids the hero outpost at world slab (%ld,%ld)", (int)p, tr->name,
                    (long)exw, (long)eyw);
            }
        }
    }
}


/******************************************************************************/
/* Building: what each personality builds first and how big (KeeperFX's own room processes do the building; the brain
   sets their priorities and sizes), doors and trap places towards its foes, a Lair beside a far portal, and the rooms
   it built while it slept. */

extern Comp_Process_Func computer_process_func_list[];
long computer_check_any_room(struct Computer2 *comp, struct ComputerProcess *cproc);

enum KaiRoom { KaiR_Treasury = 0, KaiR_Lair, KaiR_Hatchery, KaiR_Library, KaiR_Training, KaiR_Workshop, KaiR_Prison,
    KaiR_Torture, KaiR_Barracks, KaiR_Graveyard, KaiR_Temple, KaiR_COUNT };
static const char *const kai_room_names[KaiR_COUNT] = {"TREASURE", "LAIR", "GARDEN", "RESEARCH", "TRAINING", "WORKSHOP",
    "PRISON", "TORTURE", "BARRACKS", "GRAVEYARD", "TEMPLE"};

struct KaiRoomPlan {
    int8_t prio[KaiR_COUNT]; /* the room process's priority (KeeperFX picks the highest of those ready) */
    int8_t size[KaiR_COUNT]; /* slabs added to (or taken from) the side its needs give */
};

static const struct KaiRoomPlan kai_room_plans[KaiP_COUNT] = {
    /*                Tres Lair Hatc Libr Trai Work Pris Tort Barr Grav Temp */
    /* Warlord */   {{ 10,   9,   8,   5,  10,   4,   3,   2,   7,   2,   3}, { 0,  1,  1,  0,  2,  0,  0,  0,  1,  0,  0}},
    /* Bully */     {{ 10,   8,   8,   5,  10,   3,   4,   2,   6,   2,   3}, { 0,  0,  1,  0,  1,  0,  0,  0,  1,  0,  0}},
    /* Turtle */    {{ 10,   8,   7,   6,   6,  11,   3,   2,   1,   2,   5}, { 0, -1, -1,  0,  0,  2,  0,  0, -1,  0,  0}},
    /* Collector */ {{ 12,   7,   7,   9,   5,   6,   3,   2,   2,   2,   4}, { 2,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0}},
    /* Torturer */  {{ 10,   8,   8,   5,   6,   4,  11,  11,   3,   6,   3}, { 0,  0,  0,  0,  0,  0,  1,  1,  0,  1,  0}},
    /* Opportunist*/{{ 10,   8,   8,   7,   7,   6,   3,   3,   4,   2,   3}, { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0}},
    /* Diplomat */  {{ 10,   8,   8,   7,   7,   6,   3,   3,   2,   2,   6}, { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0}},
};

static RoomKind kai_room_kind(int r)
{
    static RoomKind kinds[KaiR_COUNT];
    static TbBool known = false;
    if (!known)
    {
        for (int k = 0; k < KaiR_COUNT; k++)
            kinds[k] = (RoomKind)max(0L, (long)get_id(room_desc, kai_room_names[k]));
        known = true;
    }
    return kinds[r];
}

static int kai_room_of_kind(RoomKind rkind)
{
    for (int r = 0; r < KaiR_COUNT; r++)
        if ((kai_room_kind(r) > 0) && (kai_room_kind(r) == rkind))
            return r;
    return -1;
}

/** How many of its creatures research (for the Library's size). */
static int kai_researchers(const struct Dungeon *dungeon)
{
    int n = 0;
    long guard = 0;
    for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        const struct Thing *t = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (thing_is_invalid(t) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(t);
        if ((crconf != NULL) && (crconf->research_value > 0))
            n++;
    }
    return n;
}

/** The side of the next room of a kind, from what the dungeon needs and its personality's liking. */
static int kai_room_side(const struct Dungeon *dungeon, int r, int personality, int own, int researchers)
{
    int side;
    switch (r)
    {
    case KaiR_Lair: side = (int)ceil(sqrt((double)max(9, (int)dungeon->max_creatures_attracted))); break;
    case KaiR_Hatchery: side = 3 + own / 8; break;
    case KaiR_Treasury: side = 3 + (int)(dungeon->total_money_owned / 8000); break;
    case KaiR_Library: side = 3 + researchers / 2; break;
    case KaiR_Training: case KaiR_Workshop: side = 4; break;
    default: side = 3; break;
    }
    side += kai_room_plans[personality].size[r];
    return max(3, min(side, (r == KaiR_Lair) ? 9 : 7));
}

/* A rival pair's keeper builds at the endgame's size (the user, 2026-11-25, after watching a war: "they're always stuck
   with the 3x4 training room. Make sure that if they need bigger rooms, any room, that they build another one in the
   standard of endgame"): its main rooms 7x7, the others 5x5, every new room of a kind, the stock "room nearly full"
   ones too; kai_rival_rooms builds another such room of every kind it has whose rooms are smaller than that. */
static const int8_t kai_endgame_side[KaiR_COUNT] = {
    /* Tres Lair Hatc Libr Trai Work Pris Tort Barr Grav Temp */
        7,   7,   7,   7,   7,   7,   5,   5,   5,   5,   5};

/* the sizes it last gave each process (a process that found no room for them makes them smaller itself; they are
   set again only when the need changes) */
static uint8_t kai_set_side[PLAYERS_MAX][COMPUTER_PROCESSES_COUNT + 1];

static void kai_build_plan(struct Computer2 *comp, const struct KaiState *ks, const struct KaiView *v, PlayerNumber p)
{
    const struct Dungeon *dungeon = comp->dungeon;
    const int researchers = kai_researchers(dungeon);
    for (int i = 0; i <= COMPUTER_PROCESSES_COUNT; i++)
    {
        struct ComputerProcess *cproc = &comp->processes[i];
        if (flag_is_set(cproc->flags, ComProc_ListEnd))
            break;
        if (computer_process_func_list[cproc->func_check] != computer_check_any_room)
            continue;
        const int r = kai_room_of_kind((RoomKind)cproc->process_configuration_value_4);
        if (r < 0)
            continue;
        cproc->priority = kai_room_plans[ks->personality].prio[r];
        const TbBool rival = world_mode_active() && world_rivals_is_rival(p);
        const int side = rival ? kai_endgame_side[r] : kai_room_side(dungeon, r, ks->personality, v->own, researchers);
        if (kai_set_side[p][i] != side)
        {
            kai_set_side[p][i] = (uint8_t)side;
            cproc->process_configuration_value_2 = side;
            cproc->process_configuration_value_3 = ((r == KaiR_Hatchery) && !rival) ? side + 1 : side;
            if (kai_logging())
                SYNCLOG("Keeper AI: player %d (%s) plans its next %s at %dx%d, priority %d", (int)p,
                    kai_traits[ks->personality].name, kai_room_names[r], (int)cproc->process_configuration_value_2,
                    (int)cproc->process_configuration_value_3, (int)cproc->priority);
        }
    }
}

/** Is another keeper's ground (not an ally's, not the heroes') within r slabs of the slab? */
static TbBool kai_foreign_near(PlayerNumber p, MapSlabCoord x, MapSlabCoord y, int r)
{
    for (int dy = -r; dy <= r; dy += 2)
        for (int dx = -r; dx <= r; dx += 2)
        {
            const struct SlabMap *slb = get_slabmap_block(x + dx, y + dy);
            if (slabmap_block_invalid(slb))
                continue;
            const PlayerNumber o = slabmap_owner(slb);
            if ((o == p) || (o == game.neutral_player_num) || (o == PLAYER_GOOD) || (o < 0) || (o >= PLAYERS_COUNT))
                continue;
            if ((slb->kind == SlbT_CLAIMED) || (slb->room_index > 0) || (slb->kind >= SlbT_WALLDRAPE && slb->kind <= SlbT_WALLPAIRSHR))
                if (!world_truce_allied(p, o))
                    return true;
        }
    return false;
}

/** May it dig to and claim this place (its expansion rules): not near another keeper's ground unless it is assaulting. */
static TbBool kai_may_claim(PlayerNumber p, const struct Coord3d *c)
{
    if (world_mode_active() && !world_rivals_site_ok(p, subtile_slab(c->x.stl.num), subtile_slab(c->y.stl.num)))
        return false; /* (a rival pair's keeper: never towards its rival) */
    if (kai[p].stance == KaiS_Assault)
        return true;
    return !kai_foreign_near(p, subtile_slab(c->x.stl.num), subtile_slab(c->y.stl.num), KAI_KEEP_OUT);
}

/** The slabs it claims at most this far from its heart (its liking for exploring). */
static int kai_claim_r(const struct KaiTraits *tr)
{
    return min(KAI_EXPLORE_R, 40 + tr->explore / 2);
}

/** KeeperFX's own digging (gold, entrances) stays off other keepers' ground too, unless it is assaulting. */
static void kai_keep_out(struct Computer2 *comp, PlayerNumber p)
{
    if (kai[p].stance == KaiS_Assault)
        return;
    long ti = comp->task_idx;
    for (int k = 0; (ti != 0) && (k < COMPUTER_TASKS_COUNT); k++)
    {
        struct ComputerTask *ctask = get_computer_task(ti);
        if (computer_task_invalid(ctask))
            break;
        ti = ctask->next_task;
        if ((ctask->ttype != CTT_DigToGold) && (ctask->ttype != CTT_DigToEntrance))
            continue;
        const MapSlabCoord x = subtile_slab(ctask->dig.pos_dest.x.stl.num), y = subtile_slab(ctask->dig.pos_dest.y.stl.num);
        if (!kai_foreign_near(p, x, y, KAI_KEEP_OUT / 2) && (!world_mode_active() || world_rivals_site_ok(p, x, y)))
            continue;
        SYNCLOG("Keeper AI: player %d drops its digging to engine slab (%d,%d): another keeper's ground", (int)p, (int)x, (int)y);
        remove_task(comp, ctask);
    }
}

/** Is the slab a corridor of its own (claimed, no room, at most two open sides)? opposite: its open sides face each
 *  other (a door fits). */
static TbBool kai_corridor(PlayerNumber p, MapSlabCoord x, MapSlabCoord y, TbBool *opposite)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || (slb->kind != SlbT_CLAIMED) || (slabmap_owner(slb) != p) || (slb->room_index > 0))
        return false;
    static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    TbBool open[4];
    int n = 0;
    for (int k = 0; k < 4; k++)
    {
        const struct SlabMap *nb = get_slabmap_block(x + dx4[k], y + dy4[k]);
        open[k] = !slabmap_block_invalid(nb) && ((nb->kind == SlbT_CLAIMED) || (nb->kind == SlbT_PATH) || (nb->room_index > 0) ||
            slab_kind_is_door(nb->kind));
        n += open[k] ? 1 : 0;
    }
    if (opposite != NULL)
        *opposite = (n == 2) && ((open[0] && open[1]) || (open[2] && open[3]));
    return n <= 2;
}

/** Defences towards a foe: a door on the first straight corridor slab of its own on the way from its heart, and trap
 *  places on the next ones (KeeperFX's own trap check sets traps there when it has them). Its Imps reinforce the walls
 *  by themselves. */
static void kai_fortify(struct Computer2 *comp, struct KaiState *ks, PlayerNumber p, const struct Coord3d *heart,
    const struct Coord3d *foe)
{
    const GameTurnDelta gap = (ks->personality == KaiP_Turtle) ? KAI_FORTIFY_GAP / 3 : KAI_FORTIFY_GAP;
    if ((ks->last_fortify != 0) && ((GameTurnDelta)(game.play_gameturn - ks->last_fortify) < gap))
        return;
    ks->last_fortify = game.play_gameturn;
    ThingModel door = 0;
    for (ThingModel k = game.conf.trapdoor_conf.door_types_count - 1; (k > 0) && (door == 0); k--)
        if (is_door_placeable(p, k))
            door = k;
    const int traps = (ks->personality == KaiP_Turtle) ? 3 : 1;
    /* its own corridor slabs near home, the nearest the foe first (the edge of its dungeon that faces it) */
    enum { KAI_FORT_N = 12 };
    MapSlabCoord cx[KAI_FORT_N], cy[KAI_FORT_N];
    long cd[KAI_FORT_N];
    TbBool cs[KAI_FORT_N];
    int n = 0;
    const MapSlabCoord hx = subtile_slab(heart->x.stl.num), hy = subtile_slab(heart->y.stl.num);
    const MapSlabCoord fx = subtile_slab(foe->x.stl.num), fy = subtile_slab(foe->y.stl.num);
    const int r = KAI_HOME_R * 2;
    for (MapSlabCoord y = hy - r; y <= hy + r; y++)
        for (MapSlabCoord x = hx - r; x <= hx + r; x++)
        {
            if ((abs(x - hx) <= 3) && (abs(y - hy) <= 3))
                continue; /* (never at the heart itself) */
            TbBool straight = false;
            if (!kai_corridor(p, x, y, &straight))
                continue;
            const long d = (long)(x - fx) * (x - fx) + (long)(y - fy) * (y - fy);
            if ((n == KAI_FORT_N) && (d >= cd[n - 1]))
                continue;
            int at = (n < KAI_FORT_N) ? n++ : n - 1;
            while ((at > 0) && (cd[at - 1] > d))
            {
                cx[at] = cx[at - 1];
                cy[at] = cy[at - 1];
                cd[at] = cd[at - 1];
                cs[at] = cs[at - 1];
                at--;
            }
            cx[at] = x;
            cy[at] = y;
            cd[at] = d;
            cs[at] = straight;
        }
    TbBool door_done = (door == 0);
    int placed = 0, used_n = 0;
    MapSlabCoord ux[KAI_FORT_N], uy[KAI_FORT_N];
    for (int k = 0; (k < n) && (!door_done || (placed < traps)); k++)
    {
        TbBool close = false;
        for (int u = 0; u < used_n; u++)
            close |= (abs(cx[k] - ux[u]) <= 2) && (abs(cy[k] - uy[u]) <= 2);
        if (close)
            continue;
        if (!door_done && cs[k] &&
            (try_game_action(comp, p, GA_PlaceDoor, 0, slab_subtile_center(cx[k]), slab_subtile_center(cy[k]), door, 0) > Lb_OK))
        {
            SYNCLOG("Keeper AI: player %d (%s) puts a door at engine slab (%d,%d) towards its foe", (int)p,
                kai_traits[ks->personality].name, (int)cx[k], (int)cy[k]);
            door_done = true;
            ux[used_n] = cx[k];
            uy[used_n++] = cy[k];
            continue;
        }
        if (placed < traps)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(cx[k]));
            pos.y.val = subtile_coord_center(slab_subtile_center(cy[k]));
            pos.z.val = 0;
            if (add_to_trap_locations(comp, &pos))
            {
                placed++;
                ux[used_n] = cx[k];
                uy[used_n++] = cy[k];
                SYNCLOG("Keeper AI: player %d (%s) makes engine slab (%d,%d) a trap place towards its foe", (int)p,
                    kai_traits[ks->personality].name, (int)cx[k], (int)cy[k]);
            }
        }
    }
}

/** An outpost: a Lair beside a portal of its own far from its heart, so its creatures rest there and guard it. */
static void kai_outpost_build(struct Computer2 *comp, struct KaiState *ks, PlayerNumber p, const struct Coord3d *heart)
{
    if ((ks->last_outpost_build != 0) && ((GameTurnDelta)(game.play_gameturn - ks->last_outpost_build) < KAI_OUTPOST_GAP))
        return;
    ks->last_outpost_build = game.play_gameturn;
    struct Dungeon *dungeon = comp->dungeon;
    const RoomKind entrance = (RoomKind)max(0L, (long)get_id(room_desc, "ENTRANCE"));
    const RoomKind lair = kai_room_kind(KaiR_Lair);
    if ((entrance <= 0) || (lair <= 0) || !is_room_available(p, lair) || (dungeon->total_money_owned < 2000) ||
        is_task_in_progress(comp, CTT_DigRoom) || is_task_in_progress(comp, CTT_DigRoomPassage))
        return;
    long guard = 0;
    for (long i = dungeon->room_list_start[entrance]; (i != 0) && (guard++ < ROOMS_COUNT);)
    {
        const struct Room *room = room_get(i);
        if (room_is_invalid(room))
            break;
        i = room->next_of_owner;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(room->central_stl_x);
        pos.y.val = subtile_coord_center(room->central_stl_y);
        pos.z.val = subtile_coord(1, 0);
        if (kai_dist2(&pos, heart) < KAI_SLABS2(KAI_OUTPOST_FAR))
            continue;
        /* a Lair of its own near it already? */
        TbBool has = false;
        long g2 = 0;
        for (long j = dungeon->room_list_start[lair]; (j != 0) && (g2++ < ROOMS_COUNT) && !has;)
        {
            const struct Room *lr = room_get(j);
            if (room_is_invalid(lr))
                break;
            j = lr->next_of_owner;
            struct Coord3d lp;
            lp.x.val = subtile_coord_center(lr->central_stl_x);
            lp.y.val = subtile_coord_center(lr->central_stl_y);
            lp.z.val = 0;
            has = (kai_dist2(&lp, &pos) <= KAI_SLABS2(10));
        }
        if (has)
            continue;
        for (int perfect = 1; perfect >= 0; perfect--)
        {
            const struct ComputerTask *ctask = able_to_build_room(comp, &pos, lair, 3, 3, 6, perfect);
            if (!computer_task_invalid(ctask))
            {
                SYNCLOG("Keeper AI: player %d (%s) builds an outpost Lair beside its portal at engine slab (%d,%d)", (int)p,
                    kai_traits[ks->personality].name, (int)subtile_slab(room->central_stl_x), (int)subtile_slab(room->central_stl_y));
                return;
            }
        }
    }
}

/** May a room of its own go on this slab: plain earth or a wall of its own (its Imps reinforce the new edge)? */
static TbBool kai_site_slab(PlayerNumber p, const struct SlabMap *slb)
{
    if (slabmap_block_invalid(slb))
        return false;
    if ((slb->kind == SlbT_EARTH) || (slb->kind == SlbT_TORCHDIRT))
        return true;
    return (slb->kind >= SlbT_WALLDRAPE) && (slb->kind <= SlbT_WALLPAIRSHR) && (slabmap_owner(slb) == p);
}

/** A place for a w x h room: plain earth or its own walls, on loaded ground, touching its own ground (or, to grow a room, that
 *  room's kind), r_min to r_max slabs from (cx, cy); its top-left corner in *ox, *oy. */
static TbBool kai_find_site(PlayerNumber p, MapSlabCoord cx, MapSlabCoord cy, int w, int h, int r_min, int r_max,
    RoomKind touch_kind, MapSlabCoord *ox, MapSlabCoord *oy)
{
    for (int r = r_min; r <= r_max; r++)
    {
        const int ring_n = max(8, r * 6);
        for (int a = 0; a < ring_n; a++)
        {
            const double ang = a * 6.2831853 / ring_n;
            const MapSlabCoord x0 = cx + (MapSlabCoord)lround(r * cos(ang)) - w / 2;
            const MapSlabCoord y0 = cy + (MapSlabCoord)lround(r * sin(ang)) - h / 2;
            TbBool ok = true;
            for (int dy = 0; (dy < h) && ok; dy++)
                for (int dx = 0; (dx < w) && ok; dx++)
                {
                    const struct SlabMap *slb = get_slabmap_block(x0 + dx, y0 + dy);
                    ok = kai_site_slab(p, slb);
                }
            if (!ok)
                continue;
            /* touching: a slab beside the rectangle's edge (not its corners) */
            TbBool touches = false;
            for (int k = 0; (k < 2 * (w + h)) && !touches; k++)
            {
                MapSlabCoord bx, by;
                if (k < w) { bx = x0 + k; by = y0 - 1; }
                else if (k < 2 * w) { bx = x0 + k - w; by = y0 + h; }
                else if (k < 2 * w + h) { bx = x0 - 1; by = y0 + k - 2 * w; }
                else { bx = x0 + w; by = y0 + k - 2 * w - h; }
                const struct SlabMap *nb = get_slabmap_block(bx, by);
                if (slabmap_block_invalid(nb) || (slabmap_owner(nb) != p))
                    continue;
                if (touch_kind > 0)
                {
                    const struct Room *room = slab_room_get(bx, by);
                    touches = !room_is_invalid(room) && (room->kind == touch_kind) && (room->owner == p);
                } else
                    touches = (nb->kind == SlbT_CLAIMED) || (nb->room_index > 0);
            }
            if (!touches || kai_foreign_near(p, x0 + w / 2, y0 + h / 2, KAI_KEEP_OUT) ||
                (world_mode_active() && !world_rivals_site_ok(p, x0 + w / 2, y0 + h / 2)))
                continue;
            *ox = x0;
            *oy = y0;
            return true;
        }
    }
    return false;
}

/** Makes a w x h room of its own at once (the rooms it built while it slept). */
static void kai_place_room(PlayerNumber p, RoomKind rkind, MapSlabCoord x0, MapSlabCoord y0, int w, int h)
{
    const struct RoomConfigStats *roomst = get_room_kind_stats(rkind);
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x0 + dx), slab_subtile_center(y0 + dy), p, 0);
            replace_slab_from_script(x0 + dx, y0 + dy, roomst->assigned_slab);
        }
}

/** What it built while it slept: a room a day (at most KAI_CATCH_UP_MAX), the rooms it lacks first in its
 *  personality's order, then its fullest rooms grown by a strip beside them. */
static void kai_catch_up(PlayerNumber p, const struct KaiState *ks, long days_alone)
{
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    const struct Thing *heart = find_players_dungeon_heart(p);
    if (dungeon_invalid(dungeon) || !thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const struct KaiRoomPlan *plan = &kai_room_plans[ks->personality];
    int order[KaiR_COUNT];
    for (int r = 0; r < KaiR_COUNT; r++)
        order[r] = r;
    for (int a = 0; a < KaiR_COUNT; a++) /* by its priorities, highest first */
        for (int b = a + 1; b < KaiR_COUNT; b++)
            if (plan->prio[order[b]] > plan->prio[order[a]])
            {
                const int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
    int left = (int)min((long)KAI_CATCH_UP_MAX, days_alone);
    int built = 0, grown = 0;
    /* the rooms it lacks */
    for (int k = 0; (k < KaiR_COUNT) && (left > 0); k++)
    {
        const int r = order[k];
        const RoomKind rkind = kai_room_kind(r);
        if ((rkind <= 0) || dungeon_has_room(dungeon, rkind) || !is_room_available(p, rkind))
            continue;
        const int side = kai_room_side(dungeon, r, ks->personality, 0, 0);
        MapSlabCoord x0, y0;
        if (!kai_find_site(p, hx, hy, side, side, 4, KAI_HOME_R, 0, &x0, &y0))
            continue;
        kai_place_room(p, rkind, x0, y0, side, side);
        SYNCLOG("Keeper AI: player %d (%s) built a %dx%d %s while it slept", (int)p, kai_traits[ks->personality].name, side,
            side, kai_room_names[r]);
        built++;
        left--;
    }
    /* its rooms grown: each pass the best-liked rooms it has get a strip of two slabs' depth beside them */
    for (int pass = 0; (pass < 3) && (left > 0); pass++)
        for (int k = 0; (k < KaiR_COUNT) && (left > 0); k++)
        {
            const int r = order[k];
            const RoomKind rkind = kai_room_kind(r);
            if ((rkind <= 0) || !dungeon_has_room(dungeon, rkind) || (plan->prio[r] < 6))
                continue;
            const struct Room *room = room_get(dungeon->room_list_start[rkind]);
            if (room_is_invalid(room))
                continue;
            const MapSlabCoord rx = subtile_slab(room->central_stl_x), ry = subtile_slab(room->central_stl_y);
            const int side = kai_room_side(dungeon, r, ks->personality, 0, 0);
            MapSlabCoord x0, y0;
            TbBool wide = true;
            if (!kai_find_site(p, rx, ry, side, 2, 1, 7, rkind, &x0, &y0))
            {
                wide = false;
                if (!kai_find_site(p, rx, ry, 2, side, 1, 7, rkind, &x0, &y0))
                    continue;
            }
            kai_place_room(p, rkind, x0, y0, wide ? side : 2, wide ? 2 : side);
            SYNCLOG("Keeper AI: player %d (%s) grew its %s by %d slabs while it slept", (int)p, kai_traits[ks->personality].name,
                kai_room_names[r], side * 2);
            grown++;
            left--;
        }
    SYNCLOG("Keeper AI: player %d slept %ld days: %d rooms built, %d grown", (int)p, days_alone, built, grown);
}

/******************************************************************************/
/* Thinking */

static void kai_set_stance(struct KaiState *ks, int stance, int target, const struct KaiView *v, PlayerNumber p)
{
    if ((ks->stance == stance) && (ks->target == target))
        return;
    char who[24] = "";
    if (target >= 0)
        snprintf(who, sizeof(who), " player %d", target);
    SYNCLOG("Keeper AI: player %d (%s) %s -> %s%s (army %.0f, %d creatures, %d wounded)", (int)p,
        kai_traits[ks->personality].name, kai_stance_names[ks->stance], kai_stance_names[stance], who, v->army.power, v->own,
        v->wounded);
    ks->stance = (uint8_t)stance;
    ks->target = (int8_t)target;
    ks->since = game.play_gameturn;
    if (stance == KaiS_Assault)
        ks->assault_power = (float)v->army.power;
}

static void kai_init(PlayerNumber p, uint32_t id_hash)
{
    struct KaiState *ks = &kai[p];
    if (ks->used)
        return;
    memset(ks, 0, sizeof(*ks));
    ks->used = 1;
    ks->target = -1;
    ks->personality = (uint8_t)kai_personality_of(id_hash);
    memset(kai_set_side[p], 0, sizeof(kai_set_side[p]));
    SYNCLOG("Keeper AI: player %d is %s", (int)p, kai_traits[ks->personality].name);
}

static void kai_think_stance(PlayerNumber p, struct Computer2 *comp, struct KaiState *ks, const struct Coord3d *heartp,
    struct KaiView *vp);
static void kai_rival_rooms(struct Computer2 *comp, PlayerNumber p);

static void kai_think(PlayerNumber p, const struct WorldKeeperInfo *wi)
{
    kai_init(p, wi->id_hash);
    struct KaiState *ks = &kai[p];
    struct Computer2 *comp = get_computer_player(p);
    struct Thing *heartng = find_players_dungeon_heart(p);
    if (computer_player_invalid(comp) || dungeon_invalid(comp->dungeon) || !thing_exists(heartng))
        return;
    const struct Coord3d heart = heartng->mappos;
    static struct KaiView v;
    kai_look(p, &heart, &v);
    kai_think_stance(p, comp, ks, &heart, &v);
    /* building (every thought, whatever its stance): its plan, its expansion rules, defences towards a foe, outposts */
    kai_build_plan(comp, ks, &v, p);
    if (world_mode_active() && world_rivals_is_rival(p))
        kai_rival_rooms(comp, p);
    kai_keep_out(comp, p);
    {
        const struct Coord3d *foe = NULL;
        if (v.home_d2 != LONG_MAX)
            foe = &v.home_pos;
        else if ((ks->target >= 0) && (ks->target < PLAYERS_MAX) && (v.near_d2[ks->target] != LONG_MAX))
            foe = &v.near_pos[ks->target];
        else
        {
            long best = LONG_MAX;
            for (PlayerNumber o = 0; o < PLAYERS_MAX; o++)
                if ((v.near_d2[o] < best) && kai_enemy(p, o))
                {
                    best = v.near_d2[o];
                    foe = &v.near_pos[o];
                }
        }
        if (foe != NULL)
            kai_fortify(comp, ks, p, &heart, foe);
    }
    if (ks->stance == KaiS_Calm)
        kai_outpost_build(comp, ks, p, &heart);
}

/******************************************************************************/
/* A rival pair's keeper (world_rivals.c): its war with its rival, and the humans who come into its territory */
#define KAI_WAR_WAVE (20 * 40)   /* turns between two waves onto the battlefield (by its liking for probing) */
#define KAI_WAR_WAY (20 * 45)    /* turns between two marks of its Imps' way across */
#define KAI_HELP_GAP (20 * 15)   /* in a truce: turns between two parties sent to help its rival */
#define KAI_RIVAL_SIGHT 40       /* slabs from its heart: a human's creature it sees (and goes for) on a safe day's rest */
static GameTurn kai_war_wave[PLAYERS_MAX], kai_war_way[PLAYERS_MAX], kai_war_help[PLAYERS_MAX]; /* (not saved: timers) */
#define KAI_WAR_EXPLORE (20 * 30) /* turns between two marks of its exploring tunnel (its Imps' work between battles) */
#define KAI_WAR_EXPLORE_R 150     /* slabs: the world's structures it digs out to */
static GameTurn kai_war_explore[PLAYERS_MAX];
#define KAI_ROOM_GAP (20 * 20)    /* turns between two looks at its rooms' sizes */
static GameTurn kai_room_look[PLAYERS_MAX];
static long kai_room_gold[PLAYERS_MAX]; /* the gold the endgame room it wants still costs (world_rivals.c tops it up) */
#define KAI_ROOM_RETRY (20 * 180) /* turns before it looks again for a place for a kind that found none */
static GameTurn kai_room_fail[PLAYERS_MAX][KaiR_COUNT];

long count_no_room_build_tasks(const struct Computer2 *comp);

/** A place further out than KeeperFX's own search (that looks at most about 11 slabs from its rooms): a corridor of
 *  12 to 22 slabs from any of its rooms to a w x w room. */
static struct ComputerTask *kai_rival_room_far(struct Computer2 *comp, RoomKind rkind, int w)
{
    struct Dungeon *dungeon = comp->dungeon;
    for (long dist = 12; dist <= 22; dist += 2)
        for (long perfect = 1; perfect >= 0; perfect--)
            for (RoomKind look = 1; look < game.conf.slab_conf.room_types_count; look++)
            {
                long ri = dungeon->room_list_start[look];
                for (int guard = 0; (ri != 0) && (guard < ROOMS_COUNT); guard++)
                {
                    const struct Room *room = room_get(ri);
                    if (room_is_invalid(room))
                        break;
                    ri = room->next_of_owner;
                    struct Coord3d pos;
                    pos.x.val = subtile_coord_center(room->central_stl_x);
                    pos.y.val = subtile_coord_center(room->central_stl_y);
                    pos.z.val = subtile_coord(1, 0);
                    struct ComputerTask *ctask = able_to_build_room(comp, &pos, rkind, w, w, dist, perfect);
                    if (!computer_task_invalid(ctask))
                        return ctask;
                }
            }
    return INVALID_COMPUTER_TASK;
}

/** A rival pair's keeper's rooms (see kai_endgame_side): of every kind it has whose rooms together are smaller than
 *  one endgame room, it builds another at the endgame's size, one at a time, the Training Room first. Through KeeperFX's
 *  own room building (a site its Imps dig out beside its rooms; world_rivals_site_ok keeps it off the rival's side). */
static void kai_rival_rooms(struct Computer2 *comp, PlayerNumber p)
{
    static const int order[] = {KaiR_Training, KaiR_Lair, KaiR_Hatchery, KaiR_Treasury, KaiR_Library, KaiR_Workshop,
        KaiR_Barracks, KaiR_Temple, KaiR_Prison, KaiR_Torture, KaiR_Graveyard};
    const GameTurn now = game.play_gameturn;
    if ((kai_room_look[p] != 0) && ((GameTurnDelta)(now - kai_room_look[p]) >= 0) &&
        ((GameTurnDelta)(now - kai_room_look[p]) < KAI_ROOM_GAP))
        return;
    kai_room_look[p] = now;
    struct Dungeon *dungeon = comp->dungeon;
    if (count_no_room_build_tasks(comp) > 0)
        return; /* (one at a time: the gold it wants stays as it was) */
    kai_room_gold[p] = 0;
    for (int k = 0; k < (int)(sizeof(order) / sizeof(order[0])); k++)
    {
        const int r = order[k];
        const RoomKind rkind = kai_room_kind(r);
        if (kai_logging() && (rkind > 0))
            SYNCLOG("Keeper AI: rival player %d's %s: has %d, buildable %d, %ld slabs", (int)p, kai_room_names[r],
                (int)dungeon_has_room(dungeon, rkind), (int)is_room_available(p, rkind), get_room_slabs_count(p, rkind));
        if ((rkind <= 0) || !dungeon_has_room(dungeon, rkind) || !is_room_available(p, rkind))
            continue;
        const int side = kai_endgame_side[r];
        const long have = get_room_slabs_count(p, rkind);
        if (have >= side * side)
            continue;
        if ((kai_room_fail[p][r] != 0) && ((GameTurnDelta)(now - kai_room_fail[p][r]) >= 0) &&
            ((GameTurnDelta)(now - kai_room_fail[p][r]) < KAI_ROOM_RETRY))
            continue; /* (no place last time: later) */
        const struct RoomConfigStats *roomst = get_room_kind_stats(rkind);
        kai_room_gold[p] = (long)roomst->cost * side * side;
        /* (no place for the full size beside its rooms: further out, then one slab smaller each way, still a big room) */
        int got = side;
        struct ComputerTask *ctask = computer_setup_build_room(comp, rkind, side, side, -1);
        if (computer_task_invalid(ctask))
            ctask = kai_rival_room_far(comp, rkind, side);
        if (computer_task_invalid(ctask) && (side > 5))
        {
            got = side - 1;
            ctask = computer_setup_build_room(comp, rkind, got, got, -1);
            if (computer_task_invalid(ctask))
                ctask = kai_rival_room_far(comp, rkind, got);
        }
        if (computer_task_invalid(ctask))
        {
            kai_room_fail[p][r] = now;
            SYNCLOG("Keeper AI: rival player %d wants a %dx%d %s (its %s slabs: %ld), but finds no place", (int)p, side, side,
                kai_room_names[r], kai_room_names[r], have);
            continue; /* (the next kind) */
        }
        /* the task's process: the stock one of the kind (its completion wakes it), else none */
        int pi = COMPUTER_PROCESSES_COUNT;
        for (int i = 0; i < COMPUTER_PROCESSES_COUNT; i++)
        {
            const struct ComputerProcess *cproc = &comp->processes[i];
            if (flag_is_set(cproc->flags, ComProc_ListEnd))
                break;
            if ((computer_process_func_list[cproc->func_check] == computer_check_any_room) &&
                ((RoomKind)cproc->process_configuration_value_4 == rkind))
            {
                pi = i;
                break;
            }
        }
        ctask->cproc_idx = pi;
        SYNCLOG("Keeper AI: rival player %d builds another %s at the endgame's %dx%d%s (its %s slabs: %ld, gold %ld)", (int)p,
            kai_room_names[r], got, got, (got < side) ? " (no place for 7x7)" : "", kai_room_names[r], have,
            (long)dungeon->total_money_owned);
        return;
    }
}

long world_keeper_ai_room_gold(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return 0;
    return kai_room_gold[plyr_idx];
}
static uint8_t kai_war_explore_dir[PLAYERS_MAX];
/* targets whose tunnel had nothing left to dig (reached, or no way on from its ground): the next is chosen instead (a
   play-test run: half the marks went to such a target, its Imps idle meanwhile) */
#define KAI_EXPLORE_DONE 8
static int64_t kai_explore_done[PLAYERS_MAX][KAI_EXPLORE_DONE][2];
static uint8_t kai_explore_done_n[PLAYERS_MAX];

/** Exploring as a player does (the user, 2026-11-25: Imps "roaming around the battles. Give them something to do, even
 *  if their treasuries are full"; "They should explore outwards like Player to find procedurally generated
 *  structures"): the next stretch of a tunnel is marked for its Imps towards the nearest portal or reward cave of the
 *  world on its own side (away from its rival, and off other keepers' ground), else a hero fortress, else a way
 *  straight out from its rival in turn ahead, half left, half right. */
static void kai_rival_explore(PlayerNumber p, struct Computer2 *comp, const struct Coord3d *heart)
{
    struct WorldKeeperRival kr;
    struct WgWorld *gen = world_generator();
    if ((gen == NULL) || !world_keeper_rival(p, &kr))
        return;
    int64_t hwx, hwy;
    world_slab_to_world(subtile_slab(heart->x.stl.num), subtile_slab(heart->y.stl.num), &hwx, &hwy);
    double ux = (double)(hwx - kr.rival_heart_x), uy = (double)(hwy - kr.rival_heart_y);
    const double ul = sqrt(ux * ux + uy * uy);
    if (ul < 1.0)
        return;
    ux /= ul;
    uy /= ul;
    static struct WgStructRef refs[32];
    const int n = min(32, wg_structures_in(gen, hwx - KAI_WAR_EXPLORE_R, hwy - KAI_WAR_EXPLORE_R, hwx + KAI_WAR_EXPLORE_R,
        hwy + KAI_WAR_EXPLORE_R, (1u << WgT_Portal) | (1u << WgT_Reward) | (1u << WgT_Fortress), refs, 32));
    double best = 1e18;
    int64_t tx = 0, ty = 0;
    const char *what = NULL;
    for (int i = 0; i < n; i++)
    {
        const struct WgStructRef *r = &refs[i];
        const int64_t sx = (r->id.type == WgT_Portal) ? r->portal_x : (r->id.type == WgT_Fortress) ? r->gate_x : (r->bbox_x1 + r->bbox_x2) / 2;
        const int64_t sy = (r->id.type == WgT_Portal) ? r->portal_y : (r->id.type == WgT_Fortress) ? r->gate_y : (r->bbox_y1 + r->bbox_y2) / 2;
        const double dx = (double)(sx - hwx), dy = (double)(sy - hwy), d = sqrt(dx * dx + dy * dy);
        if ((d < 8.0) || (d > KAI_WAR_EXPLORE_R) || (dx * ux + dy * uy < -4.0))
            continue; /* (here already, too far, or towards its rival) */
        TbBool done = false;
        for (int k = 0; (k < KAI_EXPLORE_DONE) && !done; k++)
            done = (kai_explore_done[p][k][0] == sx) && (kai_explore_done[p][k][1] == sy);
        if (done)
            continue;
        MapSlabCoord ex, ey;
        if (world_engine_slab(sx, sy, &ex, &ey))
        {
            const struct SlabMap *slb = get_slabmap_block(ex, ey);
            if (slabmap_block_invalid(slb) || (slabmap_owner(slb) == p))
                continue; /* (reached: its own ground) */
            struct Coord3d c;
            c.x.val = subtile_coord_center(slab_subtile_center(ex));
            c.y.val = subtile_coord_center(slab_subtile_center(ey));
            c.z.val = 0;
            if ((r->id.type != WgT_Fortress) && !kai_may_claim(p, &c))
                continue; /* (another keeper's ground) */
        }
        const double score = d + ((r->id.type == WgT_Fortress) ? 60.0 : 0.0);
        if (score < best)
        {
            best = score;
            tx = sx;
            ty = sy;
            what = (r->id.type == WgT_Portal) ? "a portal" : (r->id.type == WgT_Fortress) ? "a hero fortress" : "a reward cave";
        }
    }
    if (what == NULL)
    {
        static const double turns[5] = {0.0, 0.785, -0.785, 1.571, -1.571};
        const double a = turns[kai_war_explore_dir[p]++ % 5];
        tx = hwx + (int64_t)lround(60.0 * (ux * cos(a) - uy * sin(a)));
        ty = hwy + (int64_t)lround(60.0 * (ux * sin(a) + uy * cos(a)));
        what = "the world away from its rival";
    }
    MapSlabCoord ex, ey;
    if (!world_engine_slab_unloaded(tx, ty, &ex, &ey))
        return;
    struct Coord3d to, edge;
    to.x.val = subtile_coord_center(slab_subtile_center(ex));
    to.y.val = subtile_coord_center(slab_subtile_center(ey));
    to.z.val = 0;
    if (!kai_edge_toward(p, heart, &to, &edge))
        return;
    const int marked = kai_mark_tunnel(p, &edge, &to);
    if ((marked > 0) && !is_task_in_progress(comp, CTT_SlapDiggers))
        create_task_slap_imps(comp, 1, false);
    if (marked == 0)
    {
        const int k = kai_explore_done_n[p]++ % KAI_EXPLORE_DONE;
        kai_explore_done[p][k][0] = tx;
        kai_explore_done[p][k][1] = ty;
        kai_war_explore[p] = game.play_gameturn - KAI_WAR_EXPLORE + 20 * 3; /* (the next target soon) */
    }
    SYNCLOG("Keeper AI: player %d explores towards %s at world slab (%ld,%ld): %d slabs marked for its Imps", (int)p, what,
        (long)tx, (long)ty, marked);
}

static void kai_think_rival(PlayerNumber p, struct Computer2 *comp, struct KaiState *ks, const struct Coord3d *heart,
    const struct KaiView *v, const struct WorldRivalView *rv)
{
    const struct KaiTraits *tr = &kai_traits[ks->personality];
    const GameTurn now = game.play_gameturn;
    /* its Imps' work between battles: exploring outwards, whatever else it does (but not with a human at home) */
    if (!rv->invaded && ((GameTurnDelta)(now - kai_war_explore[p]) > KAI_WAR_EXPLORE))
    {
        kai_war_explore[p] = now;
        kai_rival_explore(p, comp, heart);
    }
    /* a human in its territory: it defends there (in a truce its rival comes to help) */
    if (rv->invaded)
    {
        static struct KaiView dv;
        dv = *v;
        dv.home_pos = rv->invader_pos;
        const double human = ((rv->invader >= 0) && (rv->invader < PLAYERS_MAX)) ? v->near_power[rv->invader] : 0.0;
        const TbBool turtle = (human * 100.0 > max(v->army.power, 1.0) * tr->turtle);
        kai_set_stance(ks, turtle ? KaiS_Turtle : KaiS_Defend, rv->invader, v, p);
        kai_defend(comp, ks, &dv, p);
        if (turtle && ((GameTurnDelta)(now - ks->last_cut) > KAI_CUT_GAP))
        {
            ks->last_cut = now;
            kai_cut_off(comp, p, heart, &rv->invader_pos);
        }
        return;
    }
    /* raiding heroes at its home (world_daynight.c raids it too): it defends, whatever its war or truce */
    if (v->home_power[PLAYER_GOOD] > 0.0)
    {
        kai_set_stance(ks, KaiS_Defend, PLAYER_GOOD, v, p);
        kai_defend(comp, ks, v, p);
        return;
    }
    if (rv->truce)
    {
        if (rv->rival_invaded && (v->own - v->wounded >= 2) && ((GameTurnDelta)(now - kai_war_help[p]) > KAI_HELP_GAP))
        {
            kai_war_help[p] = now;
            kai_set_stance(ks, KaiS_Defend, rv->rival, v, p);
            /* its Hand drops them on its rival's ground nearest the intruder (truced, that ground takes them); a Call
               To Arms instead when it has one (truce run, 2026-11-25: dropped on its own ground nearest the intruder, the
               help never got there) */
            struct Coord3d spot;
            TbBool found = false;
            const MapSlabCoord ix = subtile_slab(rv->rival_invader_pos.x.stl.num), iy = subtile_slab(rv->rival_invader_pos.y.stl.num);
            for (int r = 1; (r <= 6) && !found; r++)
                for (int dy = -r; (dy <= r) && !found; dy++)
                    for (int dx = -r; (dx <= r) && !found; dx++)
                    {
                        if ((abs(dx) != r) && (abs(dy) != r))
                            continue;
                        const struct SlabMap *slb = get_slabmap_block(ix + dx, iy + dy);
                        if (slabmap_block_invalid(slb) || (slabmap_owner(slb) != rv->rival) ||
                            ((slb->kind != SlbT_CLAIMED) && (slb->room_index <= 0)))
                            continue;
                        spot.x.val = subtile_coord_center(slab_subtile_center(ix + dx));
                        spot.y.val = subtile_coord_center(slab_subtile_center(iy + dy));
                        spot.z.val = 0;
                        if (can_drop_thing_here_as(spot.x.stl.num, spot.y.stl.num, p, 0, true))
                            found = true;
                    }
            if (computer_able_to_use_power(comp, PwrK_CALL2ARMS, 0, 1) || !found)
                kai_send_party(comp, p, heart, &rv->rival_invader_pos, kai_free_fighters(v, 60));
            else if (!is_task_in_progress(comp, CTT_MoveCreaturesToDefend) &&
                create_task_move_creatures_to_defend(comp, &spot, kai_free_fighters(v, 60), 0))
                SYNCLOG("Keeper AI: player %d drops %d creatures on its rival's ground at engine slab (%d,%d), beside the intruder",
                    (int)p, kai_free_fighters(v, 60), (int)subtile_slab(spot.x.stl.num), (int)subtile_slab(spot.y.stl.num));
            SYNCLOG("Keeper AI: player %d (%s) sends its creatures to help player %d, its rival, drive an intruder away", (int)p,
                tr->name, (int)rv->rival);
            return;
        }
        if (rv->rest && !rv->rival_invaded)
        {
            /* the safe day's rest: no war, but a human's creature in sight (within KAI_RIVAL_SIGHT of its heart, on the
               battlefield too) is attacked, by a player's means (the user, 2026-11-25: "the truce is not applicable to
               Player so they will attack Player on sight") */
            long best = KAI_SLABS2(KAI_RIVAL_SIGHT);
            PlayerNumber who = -1;
            struct Coord3d at;
            memset(&at, 0, sizeof(at));
            for (PlayerNumber o = 0; o < PLAYERS_MAX; o++)
            {
                if (!kai_is_human(o) || !kai_enemy(p, o))
                    continue;
                if (v->near_d2[o] < best)
                {
                    best = v->near_d2[o];
                    at = v->near_pos[o];
                    who = o;
                }
                if (v->imp_d2[o] < best)
                {
                    best = v->imp_d2[o];
                    at = v->imp_pos[o];
                    who = o;
                }
            }
            if ((who >= 0) && (v->own - v->wounded >= 2) && ((GameTurnDelta)(now - kai_war_help[p]) > KAI_HELP_GAP))
            {
                kai_war_help[p] = now;
                kai_set_stance(ks, KaiS_Defend, who, v, p);
                kai_send_party(comp, p, heart, &at, kai_free_fighters(v, 60));
                SYNCLOG("Keeper AI: player %d (%s) rests from its war on the safe day, and goes for player %d's creature it sees at "
                    "engine slab (%d,%d)", (int)p, tr->name, (int)who, (int)subtile_slab(at.x.stl.num), (int)subtile_slab(at.y.stl.num));
                return;
            }
        }
        if (!rv->rival_invaded)
        {
            kai_set_stance(ks, KaiS_Calm, -1, v, p);
            kai_nurture(comp, ks, v, p, heart);
        }
        return;
    }
    if (rv->rival == PLAYER_NONE)
    {
        /* its rival asleep or gone: no war; and it leaves everyone else alone */
        kai_set_stance(ks, KaiS_Calm, -1, v, p);
        kai_nurture(comp, ks, v, p, heart);
        return;
    }
    /* its rival's creatures at its home: it defends (they cannot take it, but they fight there) */
    if ((rv->rival < PLAYERS_MAX) && (v->home_power[rv->rival] > 0.0))
    {
        kai_set_stance(ks, KaiS_Defend, rv->rival, v, p);
        kai_defend(comp, ks, v, p);
        return;
    }
    /* the war: waves onto the battlefield, the Imps' way across marked */
    kai_set_stance(ks, KaiS_War, rv->rival, v, p);
    if (rv->field_known)
    {
        if ((GameTurnDelta)(now - kai_war_way[p]) > KAI_WAR_WAY)
        {
            kai_war_way[p] = now;
            struct Coord3d edge;
            if (kai_edge_toward(p, heart, &rv->field_far, &edge))
            {
                const int marked = kai_mark_tunnel(p, &edge, &rv->field_far);
                if ((marked > 0) && !is_task_in_progress(comp, CTT_SlapDiggers))
                    create_task_slap_imps(comp, 2, false);
            }
        }
        const GameTurnDelta wave_gap = KAI_WAR_WAVE * 100 / max(40, min(160, tr->probe + 60));
        if ((v->own - v->wounded >= KAI_MIN_FIGHTERS) && ((GameTurnDelta)(now - kai_war_wave[p]) > wave_gap))
        {
            kai_war_wave[p] = now;
            struct WorldArmy theirs;
            world_threat_army(rv->rival, &theirs);
            const double ratio = max(v->army.power, 1.0) * 100.0 / max(theirs.power, 1.0);
            if (ratio > 150.0)
            {
                /* half again as strong: it has overreached; it pulls back and regroups while its rival's portal sends
                   reinforcements (world_rivals.c), and the war goes on even (second run: the stronger side kept 20
                   creatures on the battlefield and killed every newcomer as it came) */
                SYNCLOG("Keeper AI: player %d (%s) regroups at home: its army %.0f against player %d's %.0f", (int)p, tr->name,
                    v->army.power, (int)rv->rival, theirs.power);
                kai_bring_home(comp, v, heart);
                return;
            }
            /* weaker: it holds its own end of the battlefield; stronger: it pushes towards the far end */
            const struct Coord3d *to = (ratio < 70.0) ? &rv->field_own : (ratio > 140.0) ? &rv->field_far : &rv->field_mid;
            const int share = (ratio < 70.0) ? 40 : (ratio > 140.0) ? 80 : 60;
            SYNCLOG("Keeper AI: player %d (%s) sends a wave to the %s of %s against player %d (armies %.0f against %.0f)", (int)p,
                tr->name, (ratio < 70.0) ? "near end" : (ratio > 140.0) ? "far end" : "middle", wg_field_name(rv->field_kind),
                (int)rv->rival, v->army.power, theirs.power);
            kai_send_party(comp, p, heart, to, kai_free_fighters(v, share));
            return;
        }
    }
    kai_nurture(comp, ks, v, p, heart);
}

static void kai_think_stance(PlayerNumber p, struct Computer2 *comp, struct KaiState *ks, const struct Coord3d *heartp,
    struct KaiView *vp)
{
    const struct KaiTraits *tr = &kai_traits[ks->personality];
    const struct Coord3d heart = *heartp;
    static struct KaiView v;
    v = *vp;
    {
        struct WorldRivalView rv;
        if (world_mode_active() && world_rivals_view(p, &rv))
        {
            kai_think_rival(p, comp, ks, heartp, vp, &rv);
            return;
        }
    }
    const double own = max(v.army.power, 1.0);
    /* intruders at home */
    double home = 0.0;
    for (PlayerNumber o = 0; o < PLAYERS_MAX; o++)
    {
        home += v.home_power[o];
        if ((v.home_power[o] > 0.0) && (o != PLAYER_GOOD))
            pmask_add(&ks->provoked_by, o);
    }
    if (home > 0.0)
    {
        const TbBool turtle = (home * 100.0 > own * tr->turtle);
        kai_set_stance(ks, turtle ? KaiS_Turtle : KaiS_Defend, -1, &v, p);
        kai_defend(comp, ks, &v, p);
        if (turtle && ((GameTurnDelta)(game.play_gameturn - ks->last_cut) > KAI_CUT_GAP))
        {
            ks->last_cut = game.play_gameturn;
            kai_cut_off(comp, p, &heart, &v.home_pos);
        }
        return;
    }
    /* a much stronger force coming near (not home yet): cut the tunnels towards it and gather at home */
    {
        PlayerNumber big = -1;
        double big_power = 0.0;
        for (PlayerNumber o = 0; o < PLAYERS_MAX; o++)
            if ((v.near_power[o] > big_power) && kai_enemy(p, o))
            {
                big = o;
                big_power = v.near_power[o];
            }
        if ((big >= 0) && (big_power * 100.0 > own * tr->turtle) && (ks->stance != KaiS_Assault))
        {
            kai_set_stance(ks, KaiS_Turtle, big, &v, p);
            kai_bring_home(comp, &v, &heart);
            if ((GameTurnDelta)(game.play_gameturn - ks->last_cut) > KAI_CUT_GAP)
            {
                ks->last_cut = game.play_gameturn;
                kai_cut_off(comp, p, &heart, &v.near_pos[big]);
            }
            return;
        }
    }
    /* an assault in progress: on, or back when it cost too much */
    if (ks->stance == KaiS_Assault)
    {
        const double lost = (ks->assault_power > 0.0f) ? (ks->assault_power - v.army.power) * 100.0 / ks->assault_power : 0.0;
        if (lost >= tr->retreat)
        {
            kai_set_stance(ks, KaiS_Retreat, ks->target, &v, p);
            ks->last_retreat = game.play_gameturn;
            kai_bring_home(comp, &v, &heart);
            return;
        }
        /* nothing left to march on (the neighbour went, its heart out of reach): the assault ends */
        const struct Thing *theirs_heart = (ks->target >= 0) ? find_players_dungeon_heart(ks->target) : INVALID_THING;
        const TbBool reachable = thing_exists(theirs_heart) && (kai_dist2(&theirs_heart->mappos, &heart) <= KAI_SLABS2(KAI_REACH_R));
        if ((ks->target < 0) || ((v.near_d2[ks->target] == LONG_MAX) && !reachable && !is_there_an_attack_task(comp) &&
            ((GameTurnDelta)(game.play_gameturn - ks->since) > KAI_IDLE_ASSAULT)))
        {
            kai_set_stance(ks, KaiS_Calm, -1, &v, p);
            kai_nurture(comp, ks, &v, p, &heart);
            return;
        }
        if ((ks->target >= 0) && (v.near_d2[ks->target] != LONG_MAX))
        {
            kai_send_party(comp, p, &heart, &v.near_pos[ks->target], kai_free_fighters(&v, 70));
            if (kai_start_attack(comp, ks->target, &v.near_pos[ks->target]))
                SYNCLOG("Keeper AI: player %d (%s) marches on player %d's creatures at engine slab (%d,%d)", (int)p, tr->name,
                    (int)ks->target, (int)subtile_slab(v.near_pos[ks->target].x.stl.num), (int)subtile_slab(v.near_pos[ks->target].y.stl.num));
        } else if (ks->target >= 0)
        {
            const struct Thing *theirs = find_players_dungeon_heart(ks->target);
            if (thing_exists(theirs) && (kai_dist2(&theirs->mappos, &heart) <= KAI_SLABS2(KAI_REACH_R)) &&
                kai_start_attack(comp, ks->target, &theirs->mappos))
                SYNCLOG("Keeper AI: player %d (%s) marches on player %d's heart", (int)p, tr->name, (int)ks->target);
        }
        return;
    }
    if ((ks->stance == KaiS_Retreat) && ((GameTurnDelta)(game.play_gameturn - ks->since) < KAI_RETREAT_TURNS))
    {
        kai_bring_home(comp, &v, &heart);
        return;
    }
    /* the neighbour that matters most: the strongest presence near it */
    PlayerNumber target = -1;
    double target_near = 0.0;
    for (PlayerNumber o = 0; o < PLAYERS_MAX; o++)
    {
        if ((o == PLAYER_GOOD) || !kai_enemy(p, o))
            continue;
        const TbBool near = (v.near_d2[o] != LONG_MAX) || (v.imp_d2[o] != LONG_MAX);
        if (!near)
            continue;
        const double weight = v.near_power[o] + ((v.imp_d2[o] != LONG_MAX) ? 1.0 : 0.0);
        if ((target < 0) || (weight > target_near))
        {
            target = o;
            target_near = weight;
        }
    }
    if (target < 0)
    {
        kai_set_stance(ks, KaiS_Calm, -1, &v, p);
        kai_nurture(comp, ks, &v, p, &heart);
        return;
    }
    if (v.own - v.wounded < KAI_MIN_FIGHTERS)
    {
        /* nothing to send yet (a new dungeon whose creatures are still to come, play-test 2026-11-23: a Bully with no
           creatures marked a tunnel to the player at turn 57): it keeps to itself and grows */
        kai_set_stance(ks, KaiS_Calm, target, &v, p);
        kai_nurture(comp, ks, &v, p, &heart);
        return;
    }
    struct WorldArmy theirs;
    world_threat_army(target, &theirs);
    const double ratio = own * 100.0 / max(theirs.power, 1.0);
    const TbBool busy = v.fighting[target];
    const TbBool regrouping = (ks->last_retreat != 0) && (ks->target == target) &&
        ((GameTurnDelta)(game.play_gameturn - ks->last_retreat) < KAI_REGROUP_TURNS);
    const TbBool may_assault = (ratio >= tr->assault) && (!tr->needs_busy || busy) && !regrouping &&
        (!tr->provoked || pmask_has(ks->provoked_by, target)) && (v.wounded * 3 < max(v.own, 1));
    if (may_assault)
    {
        kai_set_stance(ks, KaiS_Assault, target, &v, p);
        if (v.near_d2[target] != LONG_MAX)
        {
            kai_send_party(comp, p, &heart, &v.near_pos[target], kai_free_fighters(&v, 70));
            kai_start_attack(comp, target, &v.near_pos[target]);
            SYNCLOG("Keeper AI: player %d (%s) marches on player %d's creatures", (int)p, tr->name, (int)target);
        }
        return;
    }
    if ((tr->bully > 0) && (ratio >= tr->bully) && (v.imp_d2[target] != LONG_MAX))
    {
        kai_set_stance(ks, KaiS_Bully, target, &v, p);
        if ((ks->last_probe == 0) || ((GameTurnDelta)(game.play_gameturn - ks->last_probe) > KAI_PROBE_GAP / 4))
        {
            ks->last_probe = game.play_gameturn;
            kai_send_party(comp, p, &heart, &v.imp_pos[target], max(2, kai_free_fighters(&v, 25)));
            SYNCLOG("Keeper AI: player %d (%s) hunts player %d's Imps", (int)p, tr->name, (int)target);
        }
        return;
    }
    if ((tr->probe > 0) && ((ks->last_probe == 0) || ((GameTurnDelta)(game.play_gameturn - ks->last_probe) > KAI_PROBE_GAP * 100 / tr->probe)) &&
        (v.near_d2[target] != LONG_MAX) && (ratio >= 50))
    {
        kai_set_stance(ks, KaiS_Probe, target, &v, p);
        ks->last_probe = game.play_gameturn;
        kai_send_party(comp, p, &heart, &v.near_pos[target], 3);
        SYNCLOG("Keeper AI: player %d (%s) tests player %d with a small party", (int)p, tr->name, (int)target);
        return;
    }
    /* otherwise it keeps to itself and grows */
    kai_set_stance(ks, KaiS_Calm, target, &v, p);
    kai_nurture(comp, ks, &v, p, &heart);
}

/******************************************************************************/
/* Test hook KFX_WORLD_KAITEST=T (with KFX_WORLD_PROFKEEPERS=1 and KFX_WORLD_WAKE=1): the first awake keeper gets an
   army of 8 at level 5 and 8,000 gold at T; at T+400 four of player 0's creatures (level 2) stand on open ground
   30-60 slabs from its heart; at T+3000 fourteen (level 9) stand 6 slabs from its heart. Its thoughts are logged. */
static PlayerNumber kai_test_keeper = -1;
static TbBool kai_classic_keeper(PlayerNumber p, struct WorldKeeperInfo *wi);

/** The test switch's rooms: every room of the build plans is known to player 0 (computer keepers know what it knows)
 *  and to the keeper, which also gets two wooden doors. */
static void kai_test_unlock(PlayerNumber p)
{
    for (int r = 0; r < KaiR_COUNT; r++)
    {
        const RoomKind rkind = kai_room_kind(r);
        if (rkind <= 0)
            continue;
        set_room_available(PLAYER0, rkind, 1, 1);
        if (p >= 0)
            set_room_available(p, rkind, 1, 1);
    }
    struct Dungeon *dungeon = (p >= 0) ? get_players_num_dungeon(p) : INVALID_DUNGEON;
    const ThingModel wood = (ThingModel)get_id(door_desc, "WOOD");
    if (!dungeon_invalid(dungeon) && (wood > 0))
        set_door_buildable_and_add_to_amount(p, wood, 1, 2);
    /* a normal level: and Call To Arms, so that its parties can go out under the banner (KFX_KAI_CLASSIC tests) */
    if (!dungeon_invalid(dungeon) && kai_classic())
        set_power_available(p, PwrK_CALL2ARMS, 1, 1);
}

static TbBool kai_test_open_spot(const struct Coord3d *heart, int r_min, int r_max, struct Coord3d *out)
{
    const MapSlabCoord hx = subtile_slab(heart->x.stl.num), hy = subtile_slab(heart->y.stl.num);
    for (int r = r_min; r <= r_max; r++)
        for (int a = 0; a < 64; a++)
        {
            const double ang = a * 6.2831853 / 64.0;
            const MapSlabCoord x = hx + (MapSlabCoord)(r * cos(ang)), y = hy + (MapSlabCoord)(r * sin(ang));
            const struct SlabMap *slb = get_slabmap_block(x, y);
            if (slabmap_block_invalid(slb) || (slb->room_index > 0) ||
                !((slb->kind == SlbT_PATH) || ((slb->kind == SlbT_CLAIMED) && ((r_min < KAI_HOME_R) ||
                (kai_classic() && (slabmap_owner(slb) == PLAYER0)))))) /* (a normal level: player 0's own ground will do) */
                continue;
            out->x.val = subtile_coord_center(slab_subtile_center(x));
            out->y.val = subtile_coord_center(slab_subtile_center(y));
            out->z.val = 0;
            out->z.val = get_floor_height_at(out);
            return true;
        }
    return false;
}

static int kai_test_spawn(PlayerNumber owner, const struct Coord3d *at, int n, int level)
{
    static const char *const kinds[] = {"ORC", "TROLL", "DARK_MISTRESS", "DEMONSPAWN", "SORCEROR", "BILE_DEMON", "SPIDER",
        "HELL_HOUND"};
    int made = 0;
    for (int k = 0; k < n; k++)
    {
        const ThingModel model = (ThingModel)get_id(creature_desc, kinds[k % 8]);
        if (model <= 0)
            continue;
        struct Coord3d pos = *at;
        pos.x.val += (k % 3) * 64;
        pos.y.val += (k / 3 % 3) * 64;
        struct Thing *c = create_creature(&pos, model, owner);
        if (!thing_exists(c))
            continue;
        move_thing_in_map(c, &pos);
        if (level > 1)
            init_creature_level(c, (CrtrExpLevel)(level - 1));
        made++;
    }
    return made;
}

static void kai_test_turn(void)
{
    static long at = -2;
    static int near_n = 4, near_level = 2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_KAITEST"); /* "T[,n,level]": the party near it */
        at = (v != NULL) ? atol(v) : -1;
        if (v != NULL)
            sscanf(v, "%ld,%d,%d", &at, &near_n, &near_level);
        if (at >= 0)
            SYNCLOG("Keeper AI: test switch KFX_WORLD_KAITEST=%ld", at);
    }
    if (at < 0)
        return;
    const long t = (long)game.play_gameturn;
    if ((t != at) && (t != at + 400) && (t != at + 3000) && (((t - at) % 1000) != 500))
        return;
    if (kai_test_keeper < 0)
        for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX) && (kai_test_keeper < 0); p++)
        {
            struct WorldKeeperInfo wi;
            if (!kai_is_human(p) && (kai_classic() ? kai_classic_keeper(p, &wi) : (world_keeper_of_player(p, &wi) && wi.active)) &&
                thing_exists(find_players_dungeon_heart(p)))
                kai_test_keeper = p;
        }
    const PlayerNumber p = kai_test_keeper;
    const struct Thing *heart = (p >= 0) ? find_players_dungeon_heart(p) : INVALID_THING;
    if (!thing_exists(heart))
    {
        if (t == at)
            SYNCLOG("Keeper AI test: no awake keeper with a heart");
        return;
    }
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (t == at)
    {
        kai_test_unlock(p);
        const int made = kai_test_spawn(p, &heart->mappos, 8, 5);
        dungeon->total_money_owned += 8000;
        dungeon->offmap_money_owned += 8000;
        SYNCLOG("Keeper AI test: player %d (%s) gets %d creatures and 8000 gold", (int)p, world_keeper_ai_personality_name(p), made);
    } else if ((t == at + 400) || (t == at + 3000))
    {
        struct Coord3d spot;
        const TbBool home = (t == at + 3000);
        if (kai_test_open_spot(&heart->mappos, home ? 4 : 30, home ? 10 : 60, &spot))
        {
            const int made = kai_test_spawn(PLAYER0, &spot, home ? 14 : near_n, home ? 9 : near_level);
            if (!home)
            {
                /* and two of its Imps beside them (for a Bully to hunt) */
                const ThingModel imp = get_players_special_digger_model(PLAYER0);
                for (int k = 0; (k < 2) && (imp > 0); k++)
                {
                    struct Coord3d ip = spot;
                    ip.x.val += 128 + k * 64;
                    struct Thing *c = create_creature(&ip, imp, PLAYER0);
                    if (thing_exists(c))
                        move_thing_in_map(c, &ip);
                }
            }
            SYNCLOG("Keeper AI test: %d of player 0's creatures at engine slab (%d,%d), %ld slabs from player %d's heart", made,
                (int)subtile_slab(spot.x.stl.num), (int)subtile_slab(spot.y.stl.num),
                (long)LbDiagonalLength(abs(spot.x.stl.num - heart->mappos.x.stl.num), abs(spot.y.stl.num - heart->mappos.y.stl.num)) / STL_PER_SLB, (int)p);
        } else
            SYNCLOG("Keeper AI test: no open ground for player 0's creatures near player %d", (int)p);
    } else
    {
        struct WorldArmy a0, ap;
        world_threat_army(PLAYER0, &a0);
        world_threat_army(p, &ap);
        SYNCLOG("Keeper AI test: turn %ld: player %d (%s) %s, army %.0f (%d), player 0 army %.0f (%d), gold %ld", t, (int)p,
            world_keeper_ai_personality_name(p), kai[p].used ? kai_stance_names[kai[p].stance] : "-", ap.power, ap.count, a0.power,
            a0.count, (long)dungeon->total_money_owned);
    }
}

/** KFX_KAI_CLASSIC: a normal level's computer keeper, as the world would describe it (its personality from the level and
 *  its player number). */
static TbBool kai_classic_keeper(PlayerNumber p, struct WorldKeeperInfo *wi)
{
    const struct PlayerInfo *player = get_player(p);
    if (!player_exists(player) || ((player->allocflags & PlaF_CompCtrl) == 0) || !thing_exists(find_players_dungeon_heart(p)))
        return false;
    memset(wi, 0, sizeof(*wi));
    wi->index = -1;
    wi->active = wi->set_up = true;
    uint64_t z = (uint64_t)get_loaded_level_number() * 0x9E3779B97F4A7C15ull + (uint64_t)p + 1;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    wi->id_hash = (uint32_t)((z ^ (z >> 31)) >> 32);
    return true;
}

void world_keeper_ai_turn(void)
{
    const TbBool classic = kai_classic();
    if (!world_mode_active() && !classic)
        return;
    if (classic)
    {
        /* a new level (or an earlier turn: a load): what the brain knew belongs to another game */
        static LevelNumber level = -1;
        static GameTurn last = 0;
        if ((level != get_loaded_level_number()) || (game.play_gameturn < last))
            world_keeper_ai_reset();
        level = get_loaded_level_number();
        last = game.play_gameturn;
    }
    kai_test_turn();
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (((game.play_gameturn + (GameTurn)p * 7) % KAI_THINK) != 0)
            continue;
        if (kai_is_human(p) || (p == PLAYER_GOOD) || (p == game.neutral_player_num))
            continue;
        struct WorldKeeperInfo wi;
        if (classic)
        {
            if (!kai_classic_keeper(p, &wi))
                continue;
        } else if (!world_keeper_of_player(p, &wi) || !wi.set_up || !wi.active || !player_exists(get_player(p)))
        {
            if (kai[p].used && !world_keeper_of_player(p, NULL))
                memset(&kai[p], 0, sizeof(kai[p])); /* the slot is another keeper's now */
            continue;
        }
        kai_think(p, &wi);
        if (kai_logging())
        {
            /* its stock tasks, for the log: type and where its digging is */
            char tasks[200] = "";
            const struct Computer2 *comp = get_computer_player(p);
            long ti = computer_player_invalid(comp) ? 0 : comp->task_idx;
            for (int k = 0; (ti != 0) && (k < 12); k++)
            {
                const struct ComputerTask *ctask = get_computer_task(ti);
                if (computer_task_invalid(ctask))
                    break;
                snprintf(tasks + strlen(tasks), sizeof(tasks) - strlen(tasks), " %d%s@%d,%d", (int)ctask->ttype,
                    flag_is_set(ctask->flags, ComTsk_Unkn0001) ? "" : "(off)", (int)subtile_slab(ctask->dig.pos_next.x.stl.num),
                    (int)subtile_slab(ctask->dig.pos_next.y.stl.num));
                ti = ctask->next_task;
            }
            SYNCLOG("Keeper AI: player %d thinks: %s, tasks:%s", (int)p, kai_stance_names[kai[p].stance], tasks);
        }
    }
}

TbBool world_keeper_ai_may_attack(PlayerNumber plyr_idx, PlayerNumber victim)
{
    if ((!world_mode_active() && !kai_classic()) || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || kai_is_human(plyr_idx))
        return true;
    if (world_mode_active() && world_rivals_is_rival(plyr_idx))
        return false; /* (a rival pair's keeper: never KeeperFX's attack plans; its war is waves onto the battlefield) */
    const struct KaiState *ks = &kai[plyr_idx];
    if (!ks->used)
        return false; /* (not thought yet: no assault) */
    return (ks->stance == KaiS_Assault) && ((ks->target < 0) || (ks->target == victim));
}

void world_keeper_ai_woke(PlayerNumber plyr_idx, long days_alone)
{
    {
        /* test hook: KFX_WORLD_KAISLEPT=<n> - a keeper that wakes without days alone slept n days */
        const char *t = getenv("KFX_WORLD_KAISLEPT");
        if ((t != NULL) && (days_alone <= 0))
            days_alone = atol(t);
    }
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (days_alone <= 0))
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    /* its life while it slept: gold its imps would have mined, room for more creatures (portals bring them) */
    const long gold = min(days_alone * 1500L, 30000L);
    dungeon->offmap_money_owned += gold;
    dungeon->total_money_owned += gold;
    dungeon->max_creatures_attracted = (unsigned char)min(60L, (long)dungeon->max_creatures_attracted + days_alone * 2);
    SYNCLOG("Keeper AI: player %d slept %ld days: %ld gold, room for %d creatures", (int)plyr_idx, days_alone, gold,
        (int)dungeon->max_creatures_attracted);
    struct WorldKeeperInfo wi;
    if (world_keeper_of_player(plyr_idx, &wi))
    {
        kai_init(plyr_idx, wi.id_hash);
        if (getenv("KFX_WORLD_KAITEST") != NULL)
            kai_test_unlock(plyr_idx); /* (the test switch: rooms it may build while it slept) */
        kai_catch_up(plyr_idx, &kai[plyr_idx], days_alone);
    }
}

/******************************************************************************/
/* Saving (world extra KAI1) */
void world_keeper_ai_reset(void)
{
    memset(kai, 0, sizeof(kai));
    memset(kai_set_side, 0, sizeof(kai_set_side));
    memset(kai_war_wave, 0, sizeof(kai_war_wave));
    memset(kai_war_way, 0, sizeof(kai_war_way));
    memset(kai_war_help, 0, sizeof(kai_war_help));
    memset(kai_war_explore, 0, sizeof(kai_war_explore));
    memset(kai_war_explore_dir, 0, sizeof(kai_war_explore_dir));
    memset(kai_room_look, 0, sizeof(kai_room_look));
    memset(kai_room_gold, 0, sizeof(kai_room_gold));
    memset(kai_room_fail, 0, sizeof(kai_room_fail));
    memset(kai_explore_done, 0, sizeof(kai_explore_done));
    memset(kai_explore_done_n, 0, sizeof(kai_explore_done_n));
}

size_t world_keeper_ai_save_size(void)
{
    return 2 + sizeof(kai);
}

void world_keeper_ai_save(uint8_t *buf)
{
    buf[0] = (uint8_t)PLAYERS_MAX;
    buf[1] = (uint8_t)sizeof(struct KaiState);
    memcpy(buf + 2, kai, sizeof(kai));
}

TbBool world_keeper_ai_load(const uint8_t *buf, size_t len)
{
    world_keeper_ai_reset();
    if ((len < 2) || (buf[1] != (uint8_t)sizeof(struct KaiState)))
        return false;
    const int n = min((int)buf[0], PLAYERS_MAX);
    if (len < 2 + (size_t)n * sizeof(struct KaiState))
        return false;
    memcpy(kai, buf + 2, (size_t)n * sizeof(struct KaiState));
    return true;
}

#ifdef __cplusplus
}
#endif

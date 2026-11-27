/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_fx.c
 *     Endless worlds: the Pentagram's skills at work (see world_pentagram.h): the helpers the game's systems call, and
 *     the skills that keep time each turn.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pentagram.h"
#include "world_pentagram_hooks.h"

#include <math.h>
#include <string.h>

#include "bflib_basics.h"
#include "bflib_math.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_utils.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "thing_creature.h"
#include "thing_doors.h"
#include "thing_traps.h"
#include "thing_physics.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "config_objects.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "power_process.h"
#include "player_instances.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_wild.h"
#include "world_boss.h"
#include "world_misdeeds.h"
#include "world_reaper.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX(p, e) world_pentagram_fx((p), (e))
#define SLAB_UNITS (COORD_PER_STL * STL_PER_SLB)

/******************************************************************************/
/* Small questions the skills ask */

static TbBool wfx_keeper(PlayerNumber p)
{
    return (p >= 0) && (p < PLAYERS_MAX) && (p != PLAYER_GOOD) && (p != game.neutral_player_num) && world_mode_active();
}

static long wfx_dist_slabs(const struct Coord3d *a, const struct Coord3d *b)
{
    return (long)(get_2d_distance(a, b) / SLAB_UNITS);
}

/** Within `slabs` of the keeper's Heart. */
static TbBool wfx_near_heart(PlayerNumber p, const struct Coord3d *pos, long slabs)
{
    struct Thing *heart = get_player_soul_container(p);
    return thing_exists(heart) && (wfx_dist_slabs(pos, &heart->mappos) <= slabs);
}

/** On the keeper's own claimed ground. */
static TbBool wfx_on_own_ground(const struct Thing *thing)
{
    const struct SlabMap *slb = get_slabmap_for_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == thing->owner);
}

static TbBool wfx_room_role_here(const struct Thing *thing, RoomRole role)
{
    struct Room *room = get_room_thing_is_on(thing);
    return !room_is_invalid(room) && (room->owner == thing->owner) && room_role_matches(room->kind, role);
}

/** Of the keeper's other creatures, how many within `slabs` (stops counting at `enough`). */
static int wfx_friends_near(const struct Thing *thing, long slabs, int enough)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(thing->owner);
    if (dungeon_invalid(dungeon))
        return 0;
    int n = 0, guard = 0;
    for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard < 2000); guard++)
    {
        struct Thing *c = thing_get(i);
        if (!thing_exists(c))
            break;
        struct CreatureControl *cc = creature_control_get_from_thing(c);
        i = cc->players_next_creature_idx;
        if ((c != thing) && (wfx_dist_slabs(&c->mappos, &thing->mappos) <= slabs) && (++n >= enough))
            break;
    }
    return n;
}

/** A door of the keeper's within `slabs`. */
static TbBool wfx_own_door_near(const struct Thing *thing, long slabs)
{
    const MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
    for (MapSlabCoord y = sy - slabs; y <= sy + slabs; y++)
        for (MapSlabCoord x = sx - slabs; x <= sx + slabs; x++)
        {
            struct SlabMap *slb = get_slabmap_block(x, y);
            if (!slabmap_block_invalid(slb) && slab_kind_is_door(slb->kind) && (slabmap_owner(slb) == thing->owner))
                return true;
        }
    return false;
}

/** A full Treasury of the keeper's within `slabs`. */
static TbBool wfx_full_treasury_near(const struct Thing *thing, long slabs)
{
    const MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
    for (MapSlabCoord y = sy - slabs; y <= sy + slabs; y++)
        for (MapSlabCoord x = sx - slabs; x <= sx + slabs; x++)
        {
            struct Room *room = subtile_room_get(slab_subtile_center(x), slab_subtile_center(y));
            if (!room_is_invalid(room) && (room->owner == thing->owner) && room_role_matches(room->kind, RoRoF_GoldStorage) &&
                (room->total_capacity > 0) && (room->used_capacity >= room->total_capacity))
                return true;
        }
    return false;
}

/** The keeper's creature under its Call To Arms. */
static TbBool wfx_under_cta(const struct Thing *thing)
{
    return creature_affected_by_call_to_arms(thing) || creature_is_called_to_arms(thing);
}

/** The keeper possesses a creature within `slabs` of this one. */
static TbBool wfx_possessed_near(const struct Thing *thing, long slabs)
{
    struct PlayerInfo *player = get_player(thing->owner);
    struct UserState *ustate = player_invalid(player) ? NULL : get_player_primary_user_state(player);
    if ((ustate == NULL) || (ustate->view_type != PVT_CreatureContrl))
        return false;
    struct Thing *held = thing_get(ustate->controlled_thing_idx);
    return thing_exists(held) && (held != thing) && (wfx_dist_slabs(&held->mappos, &thing->mappos) <= slabs);
}

TbBool world_pentagram_named(const struct Thing *thing)
{
    return world_misdeeds_shown_name(thing) != NULL; /* (named, and level 10 and up) */
}

/******************************************************************************/
/* Combat: creatures (thing_shots.c, the two places the vices adjust damage) */

HitPoints world_pentagram_adjust_damage(struct Thing *victim, struct Thing *attacker, HitPoints dmg)
{
    if (!world_mode_active() || (dmg <= 0) || !thing_exists(victim))
        return dmg;
    long pct = 100;
    /* the dealer's skills */
    if (thing_exists(attacker) && wfx_keeper(attacker->owner))
    {
        const PlayerNumber p = attacker->owner;
        if (thing_is_creature(attacker))
        {
            pct += FX(p, WFX_DAMAGE_PCT);
            if (!world_is_day())
                pct += FX(p, WFX_DMG_NIGHT_PCT);
            if (world_wild_creature(victim))
                pct += FX(p, WFX_DMG_WILD_PCT);
            if (victim->owner == PLAYER_GOOD)
                pct += FX(p, WFX_DMG_HEROES_PCT);
            if ((victim->owner != p) && (victim->owner != game.neutral_player_num))
                pct += FX(p, WFX_DMG_ENEMY_PCT);
            if (wfx_under_cta(attacker))
                pct += FX(p, WFX_CTA_DMG_PCT);
            if (FX(p, WFX_LEAD_THE_CHARGE) && wfx_possessed_near(attacker, 8))
                pct += FX(p, WFX_LEAD_THE_CHARGE);
            if (FX(p, WFX_RENOWN) && world_pentagram_named(attacker))
                pct += FX(p, WFX_RENOWN);
            if (FX(p, WFX_DRAGONS_HOARD) && wfx_full_treasury_near(attacker, 4))
                pct += FX(p, WFX_DRAGONS_HOARD);
            if (FX(p, WFX_GOLDEN_THRONE))
            {
                const struct Dungeon *dungeon = get_players_num_dungeon(p);
                if (!dungeon_invalid(dungeon))
                    pct += min(10L, (long)(dungeon->total_money_owned / 100000));
            }
        }
        else if (thing_is_deployed_trap(attacker))
            pct += world_pentagram_trap_damage_pct(attacker);
    }
    /* the defender's skills */
    if (wfx_keeper(victim->owner) && thing_is_creature(victim))
    {
        const PlayerNumber q = victim->owner;
        long taken = 0;
        if (FX(q, WFX_OWN_GROUND_TAKEN_PCT) && wfx_on_own_ground(victim))
            taken += FX(q, WFX_OWN_GROUND_TAKEN_PCT);
        if (FX(q, WFX_NEAR_HEART_TAKEN_PCT) && wfx_near_heart(q, &victim->mappos, 10))
            taken += FX(q, WFX_NEAR_HEART_TAKEN_PCT);
        if (thing_is_creature_digger(victim))
            taken += FX(q, WFX_IMP_TAKEN_PCT);
        if (FX(q, WFX_GUARD_TAKEN_PCT) && wfx_room_role_here(victim, RoRoF_CrGuard))
            taken += FX(q, WFX_GUARD_TAKEN_PCT);
        if (FX(q, WFX_DOOR_FIGHT_TAKEN_PCT) && wfx_own_door_near(victim, 3))
            taken += FX(q, WFX_DOOR_FIGHT_TAKEN_PCT);
        if (FX(q, WFX_SHIELD_WALL) && (wfx_friends_near(victim, 3, 3) >= 3))
            taken -= FX(q, WFX_SHIELD_WALL);
        if (FX(q, WFX_NO_RETREAT) && wfx_under_cta(victim))
            taken += FX(q, WFX_NO_RETREAT);
        if (FX(q, WFX_BOSS_SLAYER) && thing_exists(attacker) && world_boss_takes_all(attacker))
            taken -= 25;
        pct = pct * (100 + max(-90L, taken)) / 100;
    }
    if (pct == 100)
        return dmg;
    return (HitPoints)max(1L, (long)((int64_t)dmg * pct / 100));
}

/** A shot's dealer: the creature or trap behind it. */
static struct Thing *wfx_shot_dealer(struct Thing *shotng)
{
    if (!thing_exists(shotng) || (shotng->parent_idx == shotng->index))
        return INVALID_THING;
    struct Thing *dealer = thing_get(shotng->parent_idx);
    return thing_exists(dealer) ? dealer : INVALID_THING;
}

/** Damage to Hearts and doors: Heart Breaker, Siege Breaker, the minors, Blood Moon, Vanguard of Ruin. */
static long wfx_struct_pct(struct Thing *dealer, TbBool heart)
{
    if (!thing_exists(dealer) || !wfx_keeper(dealer->owner) || !thing_is_creature(dealer))
        return 0;
    const PlayerNumber p = dealer->owner;
    long pct = FX(p, WFX_DMG_STRUCT_PCT);
    pct += heart ? FX(p, WFX_DMG_HEART_PCT) : FX(p, WFX_DOOR_BREAK_PCT);
    if (!world_is_day())
        pct += FX(p, WFX_BLOOD_MOON);
    if (wfx_under_cta(dealer))
        pct += FX(p, WFX_VANGUARD_RUIN);
    return pct;
}

HitPoints world_pentagram_heart_damage(struct Thing *heartng, struct Thing *shotng, HitPoints dmg)
{
    if (!world_mode_active() || (dmg <= 0) || !thing_exists(heartng))
        return dmg;
    long pct = 100 + wfx_struct_pct(wfx_shot_dealer(shotng), true);
    if (wfx_keeper(heartng->owner))
        pct = pct * (100 + FX(heartng->owner, WFX_HEART_DMG_TAKEN_PCT)) / 100;
    return (HitPoints)max(1L, (long)((int64_t)dmg * pct / 100));
}

HitPoints world_pentagram_door_damage(struct Thing *doortng, struct Thing *shotng, HitPoints dmg)
{
    if (!world_mode_active() || (dmg <= 0) || !thing_exists(doortng))
        return dmg;
    struct Thing *dealer = wfx_shot_dealer(shotng);
    long pct = 100 + wfx_struct_pct(dealer, false);
    const PlayerNumber q = doortng->owner;
    if (wfx_keeper(q))
    {
        /* Fortifier: raiders break a locked door three times slower */
        if (FX(q, WFX_FORTIFIER) && doortng->door.is_locked && thing_exists(dealer) && world_raid_hero(dealer))
            pct /= 3;
        /* Spiked Doors: the dealer takes some back */
        const int thorns = FX(q, WFX_DOOR_THORNS_PCT);
        if ((thorns > 0) && thing_exists(dealer) && thing_is_creature(dealer) && (dealer->owner != q))
        {
            const HitPoints back = (HitPoints)max(1L, (long)dmg * thorns / 100);
            apply_damage_to_thing(dealer, back, q);
        }
    }
    return (HitPoints)max(1L, (long)((int64_t)dmg * pct / 100));
}

/******************************************************************************/
/* Creatures' figures (thing_stats.c) */

HitPoints world_pentagram_max_health(const struct Thing *thing, HitPoints max_health)
{
    if (!world_mode_active() || !wfx_keeper(thing->owner))
        return max_health;
    long pct = 100 + FX(thing->owner, WFX_HEALTH_PCT);
    if (FX(thing->owner, WFX_RENOWN) && world_pentagram_named(thing))
        pct += FX(thing->owner, WFX_RENOWN);
    if (pct == 100)
        return max_health;
    return (HitPoints)min((int64_t)INT32_MAX, (int64_t)max_health * pct / 100);
}

long world_pentagram_speed(const struct Thing *thing, long speed)
{
    if (!world_mode_active() || !wfx_keeper(thing->owner))
        return speed;
    const PlayerNumber p = thing->owner;
    long pct = 100 + FX(p, WFX_WALK_PCT);
    if (!world_is_day())
        pct += FX(p, WFX_WALK_NIGHT_PCT);
    if (wfx_under_cta(thing))
    {
        pct += FX(p, WFX_CTA_WALK_PCT);
        if (FX(p, WFX_BLOOD_BANNER))
            pct += 20;
        if (FX(p, WFX_WAR_HORN) && world_pentagram_cta_fresh(thing))
            pct += 10;
    }
    if (FX(p, WFX_OBEDIENCE) && player_uses_power_obey(p))
        pct += 10;
    if (thing_is_creature_digger(thing) && FX(p, WFX_IMP_WALK_OWN_PCT) && wfx_on_own_ground(thing))
        pct += FX(p, WFX_IMP_WALK_OWN_PCT);
    if (thing_is_creature_digger(thing) && FX(p, WFX_SAPPERS) && (thing->continue_state == CrSt_CreatureArmsTrap))
        pct += 100; /* Sappers: on its way to arm a trap */
    return (pct == 100) ? speed : speed * pct / 100;
}

GoldAmount world_pentagram_pay(const struct Thing *thing, GoldAmount pay)
{
    if (!world_mode_active() || !wfx_keeper(thing->owner))
        return pay;
    if (world_reaper_is(thing))
        return 0; /* the Reaper takes no pay */
    const PlayerNumber p = thing->owner;
    long pct = 100 + FX(p, WFX_WAGE_PCT);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (!creature_control_invalid(cctrl) && (cctrl->exp_level + 1 >= 10))
        pct += FX(p, WFX_WAGE_SENIOR_PCT);
    return pay * max(10L, pct) / 100;
}

GoldAmount world_pentagram_training_cost(const struct Thing *thing, GoldAmount cost)
{
    if (!world_mode_active() || !wfx_keeper(thing->owner))
        return cost;
    return cost * max(10L, 100L + FX(thing->owner, WFX_TRAINING_FEE_PCT)) / 100;
}

/** The Heart's most health: Iron Heart, the minors. */
HitPoints world_pentagram_heart_max(const struct Thing *heartng, HitPoints base)
{
    if (!world_mode_active() || !thing_exists(heartng) || !wfx_keeper(heartng->owner))
        return base;
    return (HitPoints)min((int64_t)INT32_MAX, (int64_t)base * (100 + FX(heartng->owner, WFX_HEART_HP_PCT)) / 100);
}

HitPoints world_pentagram_door_max(const struct Thing *doortng, HitPoints base)
{
    if (!world_mode_active() || !thing_exists(doortng) || !wfx_keeper(doortng->owner))
        return base;
    return (HitPoints)min((int64_t)INT32_MAX, (int64_t)base * (100 + FX(doortng->owner, WFX_DOOR_HP_PCT)) / 100);
}

/******************************************************************************/
/* What keeps time (the simulating machine, each world turn) */

static uint8_t wfx_applied_version[PLAYERS_MAX];
static uint8_t wfx_applied_valid[PLAYERS_MAX];

/** The web changed: creatures' most health follows (health keeps its share). */
static void wfx_refresh_creatures(PlayerNumber p)
{
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    for (int list = 0; list < 2; list++)
    {
        int guard = 0;
        for (ThingIndex i = list ? dungeon->digger_list_start : dungeon->creatr_list_start; (i > 0) && (guard < 4000); guard++)
        {
            struct Thing *c = thing_get(i);
            if (!thing_exists(c))
                break;
            struct CreatureControl *cc = creature_control_get_from_thing(c);
            i = cc->players_next_creature_idx;
            update_relative_creature_health(c);
        }
    }
    /* the keeper's doors keep their share of their most health (Bolted Doors) */
    static int16_t door_pct[PLAYERS_MAX];
    const int want = FX(p, WFX_DOOR_HP_PCT);
    if (want != door_pct[p])
    {
        const struct StructureList *slist = get_list_for_thing_class(TCls_Door);
        int guard = 0;
        for (ThingIndex i = slist->index; (i > 0) && (guard < 20000); guard++)
        {
            struct Thing *d = thing_get(i);
            if (!thing_exists(d))
                break;
            i = d->next_of_class;
            if ((d->owner == p) && (d->health > 0))
                d->health = (HitPoints)((int64_t)d->health * (100 + want) / (100 + door_pct[p]));
        }
        door_pct[p] = (int16_t)want;
    }
}

void world_pentagram_turn(void)
{
    if (!world_mode_active())
        return;
    world_pentagram_marks_turn(); /* Night Sentinels, Siege Maps */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!wfx_keeper(p) || !player_exists(get_player(p)))
            continue;
        struct WorldPentagram *s = world_pentagram_state(p);
        if (!wfx_applied_valid[p] || (wfx_applied_version[p] != s->version))
        {
            wfx_applied_valid[p] = 1;
            wfx_applied_version[p] = s->version;
            wfx_refresh_creatures(p);
        }
        world_pentagram_keep_time(p);
    }
}

#ifdef __cplusplus
}
#endif

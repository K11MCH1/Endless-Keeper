/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_world.c
 *     Endless worlds: the Pentagram's skills for creatures' experience, kills and the dungeon's other works (see
 *     world_pentagram_hooks.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pentagram.h"
#include "world_pentagram_hooks.h"

#include "bflib_basics.h"
#include "bflib_math.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "thing_stats.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_terrain.h"
#include "room_data.h"
#include "slab_data.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_wild.h"
#include "world_progress.h"
#include "world_reaper.h"
#include "power_process.h"
#include "creature_states_mood.h"
#include "map_data.h"
#include "room_util.h"
#include "room_list.h"
#include "bflib_planar.h"
#include "world_storage.h"
#include "world_threat.h"
#include "world_circles.h"
#include "room_treasure.h"
#include "thing_objects.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX(p, e) world_pentagram_fx((p), (e))

static TbBool wpw_keeper(PlayerNumber p)
{
    return world_mode_active() && (p >= 0) && (p < PLAYERS_MAX) && (p != PLAYER_GOOD) && (p != game.neutral_player_num);
}

static TbBool wpw_under_cta(const struct Thing *thing)
{
    return creature_affected_by_call_to_arms(thing) || creature_is_called_to_arms(thing);
}

/** The level counted from 1. */
static int wpw_level(const struct Thing *thing)
{
    const struct CreatureControl *cc = creature_control_get_from_thing(thing);
    return creature_control_invalid(cc) ? 1 : (int)cc->exp_level + 1;
}

/******************************************************************************/
/* Experience */

long world_pentagram_combat_xp(const struct Thing *shooter, const struct Thing *target, long exp_factor)
{
    if (!thing_exists(shooter) || !wpw_keeper(shooter->owner) || (exp_factor <= 0))
        return exp_factor;
    const PlayerNumber p = shooter->owner;
    long pct = 100;
    if (wpw_level(shooter) < 5)
        pct += FX(p, WFX_XP_LOW_PCT);
    if (thing_exists(target) && (target->owner == PLAYER_GOOD))
        pct += FX(p, WFX_XP_HEROES_PCT);
    if (wpw_under_cta(shooter) && FX(p, WFX_BLOOD_BANNER))
        pct += 25;
    long out = exp_factor * pct / 100;
    /* Blooding: twice from wild creatures, three times at night */
    if (thing_exists(target) && world_wild_creature(target) && FX(p, WFX_BLOODING))
        out *= world_is_day() ? 2 : 3;
    return out;
}

long world_pentagram_training_xp(const struct Thing *thing, long value)
{
    if (!thing_exists(thing) || !wpw_keeper(thing->owner) || (value <= 0))
        return value;
    long pct = 100 + FX(thing->owner, WFX_TRAINING_XP_PCT);
    if (wpw_level(thing) < 5)
        pct += FX(thing->owner, WFX_XP_LOW_PCT);
    return value * pct / 100;
}

/* XP of the world's events (the user, 2026-11-27) */
#define WPG_XP_FORTRESS 2000
#define WPG_XP_BOSS 2500
#define WPG_XP_RAID 150
#define WPG_XP_PACK_CREATURE 15
#define WPG_XP_PORTAL 500
#define WPG_XP_LEVEL 100

/******************************************************************************/
/* Kills (thing_creature.c, as a creature dies of a blow) */

void world_pentagram_on_kill(struct Thing *victim, struct Thing *killer, PlayerNumber killer_plyr)
{
    if (!world_mode_active() || !thing_exists(victim))
        return;
    /* Soul Harvest: a creature dying in a keeper's Prison or Torture Room banks gold for that keeper's next spell */
    {
        struct Room *room = get_room_thing_is_on(victim);
        if (!room_is_invalid(room) && wpw_keeper(room->owner) &&
            (room_role_matches(room->kind, RoRoF_Prison) || room_role_matches(room->kind, RoRoF_Torture)))
            world_pentagram_soul_harvest(room->owner);
    }
    const PlayerNumber p = thing_exists(killer) ? killer->owner : killer_plyr;
    if (!wpw_keeper(p) || (victim->owner == p))
        return;
    const TbBool wild = world_wild_creature(victim);
    const TbBool hero = (victim->owner == PLAYER_GOOD) && !wild;
    GoldAmount gold = 0;
    if (hero)
        gold += FX(p, WFX_BOUNTY);
    if (wild)
        gold += FX(p, WFX_TROPHIES);
    /* Spoils of the Raid, Hunter's Moon: a raider killed on the keeper's ground drops more of what it carried */
    if (world_raid_hero(victim) && (victim->creature.gold_carried > 0))
    {
        const struct SlabMap *slb = get_slabmap_for_subtile(victim->mappos.x.stl.num, victim->mappos.y.stl.num);
        long pct = 0;
        if (!slabmap_block_invalid(slb) && (slabmap_owner(slb) == p))
            pct += FX(p, WFX_RAID_GOLD_PCT);
        if (!world_is_day() && FX(p, WFX_HUNTERS_MOON))
            pct += 100;
        gold += (GoldAmount)((long)victim->creature.gold_carried * pct / 100);
    }
    if (gold > 0)
        player_add_offmap_gold(p, world_pentagram_gold_more(p, gold));
    /* XP: a wild pack's creatures (a pack of three is about the 40 XP a wiped pack gives), by their strength */
    if (wild)
        world_pentagram_xp(p, WpgXp_Pack, WPG_XP_PACK_CREATURE, wpw_level(victim), victim->owner,
            subtile_slab(victim->mappos.x.stl.num), subtile_slab(victim->mappos.y.stl.num));
    /* Apex Predator: the killer heals when it fells a wild creature or a hero */
    const int apex = FX(p, WFX_APEX);
    if ((apex > 0) && (wild || hero) && thing_exists(killer) && thing_is_creature(killer))
    {
        struct CreatureControl *kc = creature_control_get_from_thing(killer);
        if (!creature_control_invalid(kc))
            apply_health_to_thing_and_display_health(killer, (HitPoints)max(1L, (long)kc->max_health * apex / 100));
    }
}

/******************************************************************************/
/* Creatures: portals, anger, Vices, slaps, fear (room_entrance.c, thing_creature.c, world_misdeeds.c, magic_powers.c,
   creature_states.c, creature_states_combt.c) */

GameTurnDelta world_pentagram_portal_wait(PlayerNumber plyr_idx, GameTurnDelta wait)
{
    if (!wpw_keeper(plyr_idx) || (wait <= 0))
        return wait;
    return wait * max(10L, 100L - FX(plyr_idx, WFX_PORTAL_PCT)) / 100;
}

void world_pentagram_portal_arrival(struct Thing *creatng)
{
    if (!thing_exists(creatng) || !wpw_keeper(creatng->owner))
        return;
    const PlayerNumber p = creatng->owner;
    int up = 0;
    if (FX(p, WFX_LEGION))
        up += 2;
    if (FX(p, WFX_PATRON))
    {
        up += 1;
        /* "with their first payday already paid": its wage comes back to the keeper now */
        player_add_offmap_gold(p, calculate_correct_creature_pay(creatng));
    }
    if (up <= 0)
        return;
    struct CreatureControl *cc = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cc))
        return;
    const int cap = world_creature_level_cap(p);
    const int to = min((int)cc->exp_level + up, max(0, cap - 1));
    if (to > (int)cc->exp_level)
        set_creature_level(creatng, (CrtrExpLevel)to);
}

int world_pentagram_level_cap(PlayerNumber plyr_idx, int cap)
{
    return (wpw_keeper(plyr_idx) && FX(plyr_idx, WFX_LEGION)) ? min(CREATURE_MAX_LEVEL_LIMIT, cap + 10) : cap;
}

/** Of the keeper's creatures, a named one within `slabs`. */
static TbBool wpw_named_near(const struct Thing *thing, long slabs)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(thing->owner);
    int guard = 0;
    for (ThingIndex i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start; (i > 0) && (guard < 2000); guard++)
    {
        struct Thing *c = thing_get(i);
        if (!thing_exists(c))
            break;
        i = creature_control_get_from_thing(c)->players_next_creature_idx;
        if ((c != thing) && world_pentagram_named(c) &&
            (get_2d_distance(&c->mappos, &thing->mappos) <= slabs * COORD_PER_STL * STL_PER_SLB))
            return true;
    }
    return false;
}

long world_pentagram_anger(const struct Thing *creatng, long anger, int reason)
{
    if ((anger <= 0) || !thing_exists(creatng) || !wpw_keeper(creatng->owner))
        return anger;
    const PlayerNumber p = creatng->owner;
    if ((reason == AngR_NotPaid) && FX(p, WFX_IRON_CROWN))
        return 0;
    long pct = 100;
    if ((reason == AngR_NotPaid) || (reason == AngR_Hungry) || (reason == AngR_NoLair))
        pct -= FX(p, WFX_ANGER_SLOW_PCT);
    if (FX(p, WFX_CULT) && wpw_named_near(creatng, 6))
        pct -= FX(p, WFX_CULT);
    return anger * max(0L, pct) / 100;
}

/** Reign of Terror (always), Mind Shackle (while Must Obey lasts): an angry creature does not leave. */
TbBool world_pentagram_never_leaves(const struct Thing *creatng)
{
    if (!thing_exists(creatng) || !wpw_keeper(creatng->owner))
        return false;
    const PlayerNumber p = creatng->owner;
    return FX(p, WFX_REIGN_OF_TERROR) || (FX(p, WFX_MIND_SHACKLE) && player_uses_power_obey(p));
}

/** Infamy: the level a creature's Vices are counted at (their levels come sooner). */
int world_pentagram_vice_level(const struct Thing *thing, int level)
{
    if (!thing_exists(thing) || !wpw_keeper(thing->owner))
        return level;
    const int pct = FX(thing->owner, WFX_VICE_PCT);
    return (pct <= 0) ? level : (int)((long)level * 100 / max(10, 100 - pct));
}

/** A slap: how long it hurries (Heavy Hand, the minors); Reign of Terror ends the anger for a fifth of its health. */
GameTurnDelta world_pentagram_slap(struct Thing *thing, PlayerNumber slapper, GameTurnDelta turns)
{
    if (!thing_exists(thing) || !wpw_keeper(slapper) || (thing->owner != slapper))
        return turns;
    if (FX(slapper, WFX_REIGN_OF_TERROR) && thing_is_creature(thing))
    {
        struct CreatureControl *cc = creature_control_get_from_thing(thing);
        anger_set_creature_anger_all_types(thing, 0);
        if (!creature_control_invalid(cc))
            apply_damage_to_thing(thing, (HitPoints)max(1L, (long)cc->max_health / 5), slapper);
    }
    world_reaper_slapped(thing);
    return turns * (100 + FX(slapper, WFX_SLAP_TIME_PCT)) / 100;
}

/** Cruel Slap: the wide slap reaches 2 slabs further and hurries twice as long. */
int world_pentagram_slap_reach(PlayerNumber plyr_idx, int reach)
{
    return (wpw_keeper(plyr_idx) && FX(plyr_idx, WFX_CRUEL_SLAP)) ? reach + 2 * STL_PER_SLB : reach;
}

GameTurnDelta world_pentagram_slap_hurry(PlayerNumber plyr_idx, GameTurnDelta turns)
{
    if (!wpw_keeper(plyr_idx))
        return turns;
    long t = (long)turns * (100 + FX(plyr_idx, WFX_SLAP_TIME_PCT)) / 100;
    if (FX(plyr_idx, WFX_CRUEL_SLAP))
        t *= 2;
    return (GameTurnDelta)t;
}

/** Fear: -1 as the game has it, 0 never scared (No Retreat, Blood Banner), 1 scared (Dread Lord). */
int world_pentagram_scared(const struct Thing *creatng)
{
    if (!world_mode_active() || !thing_exists(creatng))
        return -1;
    if (world_reaper_is(creatng))
        return 0; /* the Reaper fears nothing */
    if (wpw_keeper(creatng->owner) && wpw_under_cta(creatng) &&
        (FX(creatng->owner, WFX_NO_RETREAT) || FX(creatng->owner, WFX_BLOOD_BANNER)))
        return 0;
    /* Dread Lord: heroes and wild creatures near such a keeper's Heart, Prison or Torture Room flee at half health */
    if (((creatng->owner == PLAYER_GOOD) || world_wild_creature(creatng)))
    {
        const struct CreatureControl *cc = creature_control_get_from_thing(creatng);
        if (creature_control_invalid(cc) || (creatng->health * 2 >= cc->max_health))
            return -1;
        const MapSlabCoord sx = subtile_slab(creatng->mappos.x.stl.num), sy = subtile_slab(creatng->mappos.y.stl.num);
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            if (!wpw_keeper(p) || !FX(p, WFX_DREAD_LORD))
                continue;
            struct Thing *heart = get_player_soul_container(p);
            if (thing_exists(heart) && (get_2d_distance(&heart->mappos, &creatng->mappos) <= 8 * COORD_PER_STL * STL_PER_SLB))
                return 1;
            for (MapSlabCoord y = sy - 8; y <= sy + 8; y += 2)
                for (MapSlabCoord x = sx - 8; x <= sx + 8; x += 2)
                {
                    struct Room *room = subtile_room_get(slab_subtile_center(x), slab_subtile_center(y));
                    if (!room_is_invalid(room) && (room->owner == p) &&
                        (room_role_matches(room->kind, RoRoF_Prison) || room_role_matches(room->kind, RoRoF_Torture)))
                        return 1;
                }
        }
    }
    return -1;
}

/******************************************************************************/
/* Rooms' work: the Library and Workshop, torture, the Prison (thing_stats.c, creature_states_tortr.c, creature_states.c,
   thing_traps.c, world_progress.c) */

long world_pentagram_work(const struct Thing *creatng, unsigned long rrole, long value)
{
    if (!thing_exists(creatng) || !wpw_keeper(creatng->owner) || (value <= 0))
        return value;
    const PlayerNumber p = creatng->owner;
    long pct = 100;
    if (rrole & RoRoF_Research)
        pct += FX(p, WFX_BOOK_RESEARCH_PCT);
    if (rrole & RoRoF_CratesManufctr)
        pct += FX(p, WFX_WORKSHOP_PCT);
    if (FX(p, WFX_MIND_SHACKLE) && player_uses_power_obey(p))
        pct += FX(p, WFX_MIND_SHACKLE);
    return value * pct / 100;
}

/** The speed the Library (where 0) or the Workshop (1) levels at, with Tutor and the minors. */
int world_pentagram_level_speed(PlayerNumber plyr_idx, int where, int speed)
{
    if (!wpw_keeper(plyr_idx) || (speed <= 0))
        return speed;
    return speed * (100 + FX(plyr_idx, (where == 0) ? WFX_LIBRARY_PCT : WFX_WORKSHOP_LEVEL_PCT)) / 100;
}

TbBool world_pentagram_academy(PlayerNumber plyr_idx)
{
    return wpw_keeper(plyr_idx) && FX(plyr_idx, WFX_ACADEMY);
}

long world_pentagram_torture(PlayerNumber torturer, long turn_val)
{
    return wpw_keeper(torturer) ? turn_val * (100 + FX(torturer, WFX_TORTURE_PCT)) / 100 : turn_val;
}

long world_pentagram_prison_hunger(PlayerNumber jailer, long every)
{
    return wpw_keeper(jailer) ? every * (100 + FX(jailer, WFX_PRISON_LIFE_PCT)) / 100 : every;
}

GameTurnDelta world_pentagram_trap_delay(PlayerNumber plyr_idx, GameTurnDelta delay)
{
    if (!wpw_keeper(plyr_idx) || (delay <= 0))
        return delay;
    return delay * max(10L, 100L - FX(plyr_idx, WFX_TRAP_REARM_PCT)) / 100;
}

/******************************************************************************/
/* Imps and digging (creature_instances.c, map_data.c) */

static long wpw_slabs_from_heart(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return 0;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(stl_x);
    pos.y.val = subtile_coord_center(stl_y);
    pos.z.val = 0;
    return (long)(get_2d_distance(&pos, &heart->mappos) / (COORD_PER_STL * STL_PER_SLB));
}

long world_pentagram_dig_damage(const struct Thing *digger, MapSubtlCoord stl_x, MapSubtlCoord stl_y, TbBool gold, long dmg)
{
    if (!thing_exists(digger) || !wpw_keeper(digger->owner) || (dmg <= 0))
        return dmg;
    const PlayerNumber p = digger->owner;
    long pct = 100 + FX(p, WFX_DIG_PCT);
    const TbBool far = (FX(p, WFX_NOMAD) || FX(p, WFX_GOLD_RUSH)) && (wpw_slabs_from_heart(p, stl_x, stl_y) > 40);
    if (far && FX(p, WFX_NOMAD))
        pct += 20;
    if (far && gold)
        pct += FX(p, WFX_GOLD_RUSH);
    /* Deep Delvers: another keeper's or the heroes' fortified wall takes three times as long */
    const struct SlabMap *slb = get_slabmap_for_subtile(stl_x, stl_y);
    if (FX(p, WFX_DEEP_DELVERS) && !slabmap_block_invalid(slb) && (slabmap_owner(slb) != p) &&
        (slabmap_owner(slb) != game.neutral_player_num) && slab_kind_is_fortified_wall(slb->kind))
        pct = pct / FX(p, WFX_DEEP_DELVERS);
    return max(1L, dmg * pct / 100);
}

/** Gold on top of what a mining hit gives (the Alarm Bells hear only the plain amount). */
long world_pentagram_mined_gold(const struct Thing *digger, long gold)
{
    if (!thing_exists(digger) || !wpw_keeper(digger->owner) || (gold <= 0))
        return 0;
    const long pct = FX(digger->owner, WFX_MINING_PCT) + FX(digger->owner, WFX_GOLD_PCT); /* (Blood Money too) */
    if (pct <= 0)
        return 0;
    const long scaled = gold * pct;
    long extra = scaled / 100;
    if ((long)((game.play_gameturn + digger->index * 13) % 100) < (scaled % 100))
        extra++;
    return extra;
}

GoldAmount world_pentagram_gold_hold(const struct Thing *digger, GoldAmount hold)
{
    if (!thing_exists(digger) || !wpw_keeper(digger->owner) || !thing_is_creature_digger(digger))
        return hold;
    return hold * (100 + FX(digger->owner, WFX_IMP_CARRY_PCT)) / 100;
}

/** Quick Claim: extra wear an Imp's blow on another's floor or room does (0 or 1). */
int world_pentagram_claim_extra(const struct Thing *digger)
{
    if (!thing_exists(digger) || !wpw_keeper(digger->owner))
        return 0;
    const int pct = FX(digger->owner, WFX_CLAIM_PCT);
    return ((pct > 0) && ((long)((game.play_gameturn + digger->index * 7) % 100) < pct)) ? 1 : 0;
}

/** Fortifier: the blows a reinforcement needs (stock: 25). */
int world_pentagram_reinforce_hits(const struct Thing *digger, int hits)
{
    if (!thing_exists(digger) || !wpw_keeper(digger->owner) || !FX(digger->owner, WFX_FORTIFIER))
        return hits;
    return hits / 2;
}

/** Deep Delvers: a keeper may dig another keeper's or the heroes' fortified wall. */
TbBool world_pentagram_digs_enemy_walls(PlayerNumber plyr_idx)
{
    return wpw_keeper(plyr_idx) && FX(plyr_idx, WFX_DEEP_DELVERS);
}

int world_pentagram_imp_cap(PlayerNumber plyr_idx, int cap)
{
    return wpw_keeper(plyr_idx) ? cap + FX(plyr_idx, WFX_IMP_CAP) : cap;
}

int world_pentagram_hand_cap(PlayerNumber plyr_idx, int cap)
{
    return wpw_keeper(plyr_idx) ? cap + FX(plyr_idx, WFX_HAND_CAP) : cap;
}

/******************************************************************************/
/* Gold: Treasuries, rooms (room_util.c, packets.c, player_instances.c, the panels) */

GoldAmount world_pentagram_gold_per_hoard(PlayerNumber plyr_idx, GoldAmount per_hoard)
{
    return wpw_keeper(plyr_idx) ? per_hoard * (100 + FX(plyr_idx, WFX_TREASURY_PCT)) / 100 : per_hoard;
}

GoldAmount world_pentagram_room_cost(PlayerNumber plyr_idx, RoomKind rkind, GoldAmount cost)
{
    if (!wpw_keeper(plyr_idx) || (cost <= 0))
        return cost;
    if (FX(plyr_idx, WFX_GILDED_HALLS))
        cost = cost * 70 / 100;
    return cost;
}

/** What selling a room slab gives back (stock: a share of its price). */
GoldAmount world_pentagram_room_sale(PlayerNumber plyr_idx, RoomKind rkind, GoldAmount cost, long percent)
{
    const GoldAmount paid = world_pentagram_room_cost(plyr_idx, rkind, cost);
    if (wpw_keeper(plyr_idx) && FX(plyr_idx, WFX_GILDED_HALLS))
        return paid; /* Gilded Halls: all it cost */
    return (GoldAmount)((long)paid * percent / 100);
}

/** A Cave-In fill's price a slab (world_fill.c): Rockfall halves it. */
int world_pentagram_fill_reach(PlayerNumber plyr_idx)
{
    return ((world_pentagram_fx(plyr_idx, WFX_ROCKFALL) > 0) ? 3 : 0) + ((world_pentagram_fx(plyr_idx, WFX_SAPPERS) > 0) ? 3 : 0);
}

GoldAmount world_pentagram_fill_cost(PlayerNumber plyr_idx, GoldAmount cost)
{
    return (wpw_keeper(plyr_idx) && FX(plyr_idx, WFX_ROCKFALL)) ? cost / 2 : cost;
}

#ifdef __cplusplus
}
#endif

/******************************************************************************/
/* the maps and sight (2026-11-27): Night Watch, Night Sentinels, Siege Maps, Dark Sight, Lantern */

#define WPG_WATCH_TURNS (30 * 20)    /* Night Watch: a landing pings 30 seconds */
#define WPG_SEEN_TURNS 64            /* Night Sentinels: what they saw stays this long (they look again every 16 turns) */
#define WPG_SIEGE_JOIN 25            /* Siege Maps: hero rooms this near (slabs) are one stronghold */

void world_pentagram_raid_landed(PlayerNumber plyr_idx, int64_t wx, int64_t wy)
{
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if ((s == NULL) || (world_pentagram_fx(plyr_idx, WFX_NIGHT_WATCH) <= 0))
        return;
    s->watch_x = (int32_t)wx;
    s->watch_y = (int32_t)wy;
    s->watch_until = (uint32_t)(game.play_gameturn + WPG_WATCH_TURNS);
    SYNCLOG("World: Night Watch: player %d's raid lands at world slab (%ld,%ld)", (int)plyr_idx, (long)wx, (long)wy);
}

static TbBool wpg_in_guard_post(const struct Thing *creatng)
{
    const struct Room *room = get_room_thing_is_on(creatng);
    return !room_is_invalid(room) && room_role_matches(room->kind, RoRoF_CrGuard) && (room->owner == creatng->owner);
}

long world_pentagram_sight_bonus(const struct Thing *creatng)
{
    if (!world_mode_active() || thing_is_invalid(creatng) || (creatng->owner >= PLAYERS_MAX) || world_is_day())
        return 0;
    long slabs = world_pentagram_fx(creatng->owner, WFX_SIGHT_NIGHT);
    if ((world_pentagram_fx(creatng->owner, WFX_NIGHT_SENTINELS) > 0) && wpg_in_guard_post(creatng))
        slabs += world_pentagram_fx(creatng->owner, WFX_NIGHT_SENTINELS);
    return slabs * STL_PER_SLB;
}

void world_pentagram_dug(const struct Thing *creatng, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!world_mode_active() || thing_is_invalid(creatng) || (creatng->owner >= PLAYERS_MAX))
        return;
    const long more = world_pentagram_fx(creatng->owner, WFX_LANTERN);
    if (more <= 0)
        return;
    const long r = 1 + more; /* (the dig reveals the slabs beside it; the Lantern this many more) */
    reveal_map_area(creatng->owner, slab_subtile(slb_x - r, 0), slab_subtile(slb_x + r, 2), slab_subtile(slb_y - r, 0),
        slab_subtile(slb_y + r, 2));
}

/** Night Sentinels: raiders within sight of the keeper's creatures in its Guard Posts. */
static void wpg_sentinels(PlayerNumber p, struct WorldPentagram *s)
{
    struct Thing *guards[32];
    int ng = 0;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if ((thing->owner == p) && (ng < 32) && thing_exists(thing) && !thing_is_picked_up(thing) && wpg_in_guard_post(thing))
            guards[ng++] = thing;
    }
    int n = 0;
    guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (ng > 0) && (n < WPG_SEEN_MAX) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!thing_exists(thing) || !world_raid_hero(thing))
            continue;
        for (int g = 0; g < ng; g++)
        {
            const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(guards[g]);
            const MapCoordDelta reach = subtile_coord(crconf->visual_range + world_pentagram_sight_bonus(guards[g]), 0);
            if (get_chessboard_distance(&guards[g]->mappos, &thing->mappos) > reach)
                continue;
            int64_t wx, wy;
            world_slab_to_world(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num), &wx, &wy);
            s->seen_x[n] = (int32_t)wx;
            s->seen_y[n] = (int32_t)wy;
            n++;
            break;
        }
    }
    if ((n > 0) || (s->seen_n > 0))
    {
        s->seen_n = (uint8_t)n;
        s->seen_until = (uint32_t)(game.play_gameturn + WPG_SEEN_TURNS);
    }
}

/** Siege Maps: the heroes' rooms the keeper has seen, joined into strongholds (boxes of world slabs); a box whose
 *  ground is loaded and seen with no hero room in it any more goes. */
static void wpg_siege(PlayerNumber p, struct WorldPentagram *s)
{
    /* first: boxes whose ground is here and empty of heroes now */
    uint8_t alive[WPG_SIEGE_MAX] = {0};
    uint8_t here[WPG_SIEGE_MAX] = {0};
    for (struct Room *room = start_rooms; room < end_rooms; room++)
    {
        if (!room_exists(room) || (room->owner != PLAYER_GOOD))
            continue;
        const MapSubtlCoord cx = room->central_stl_x, cy = room->central_stl_y;
        int64_t wx, wy;
        world_slab_to_world(subtile_slab(cx), subtile_slab(cy), &wx, &wy);
        /* the room's box: its slabs */
        int64_t x1 = wx, y1 = wy, x2 = wx, y2 = wy;
        long k = 0;
        for (SlabCodedCoords sn = room->slabs_list; (sn > 0) && (k++ < 1024); sn = get_next_slab_number_in_room(sn))
        {
            int64_t sx, sy;
            world_slab_to_world(slb_num_decode_x(sn), slb_num_decode_y(sn), &sx, &sy);
            x1 = min(x1, sx); y1 = min(y1, sy); x2 = max(x2, sx); y2 = max(y2, sy);
        }
        int at = -1;
        for (int m = 0; (m < s->siege_n) && (at < 0); m++)
            if ((wx >= s->siege[m][0] - WPG_SIEGE_JOIN) && (wx <= s->siege[m][2] + WPG_SIEGE_JOIN) &&
                (wy >= s->siege[m][1] - WPG_SIEGE_JOIN) && (wy <= s->siege[m][3] + WPG_SIEGE_JOIN))
                at = m;
        if (at >= 0)
            alive[at] = 1;
        if (!subtile_revealed(cx, cy, p))
            continue;
        if (at < 0)
        {
            if (s->siege_n >= WPG_SIEGE_MAX)
            {
                /* full: the oldest goes */
                memmove(&s->siege[0], &s->siege[1], sizeof(s->siege[0]) * (WPG_SIEGE_MAX - 1));
                memmove(&alive[0], &alive[1], WPG_SIEGE_MAX - 1);
                s->siege_n--;
            }
            at = s->siege_n++;
            s->siege[at][0] = (int32_t)x1; s->siege[at][1] = (int32_t)y1;
            s->siege[at][2] = (int32_t)x2; s->siege[at][3] = (int32_t)y2;
            alive[at] = 1;
            SYNCLOG("World: Siege Maps: player %d marks a stronghold at world slabs (%ld,%ld)-(%ld,%ld)", (int)p, (long)x1,
                (long)y1, (long)x2, (long)y2);
        } else
        {
            s->siege[at][0] = (int32_t)min((int64_t)s->siege[at][0], x1); s->siege[at][1] = (int32_t)min((int64_t)s->siege[at][1], y1);
            s->siege[at][2] = (int32_t)max((int64_t)s->siege[at][2], x2); s->siege[at][3] = (int32_t)max((int64_t)s->siege[at][3], y2);
        }
    }
    for (int m = 0; m < s->siege_n; m++)
    {
        MapSlabCoord ex, ey;
        const int64_t mx = ((int64_t)s->siege[m][0] + s->siege[m][2]) / 2, my = ((int64_t)s->siege[m][1] + s->siege[m][3]) / 2;
        here[m] = world_engine_slab(mx, my, &ex, &ey) && subtile_revealed(slab_subtile_center(ex), slab_subtile_center(ey), p);
    }
    int w = 0;
    for (int m = 0; m < s->siege_n; m++)
    {
        if (here[m] && !alive[m])
        {
            SYNCLOG("World: Siege Maps: player %d's stronghold at world slabs (%ld,%ld) is gone", (int)p, (long)s->siege[m][0],
                (long)s->siege[m][1]);
            continue;
        }
        if (w != m)
            memcpy(s->siege[w], s->siege[m], sizeof(s->siege[0]));
        w++;
    }
    s->siege_n = (uint8_t)w;
}

void world_pentagram_marks_turn(void)
{
    if (!world_mode_active())
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        struct WorldPentagram *s = world_pentagram_state(p);
        if ((s == NULL) || !player_exists(get_player(p)))
            continue;
        if (((game.play_gameturn % 16) == (GameTurn)(p % 16)) && (world_pentagram_fx(p, WFX_NIGHT_SENTINELS) > 0))
        {
            if (world_is_day())
                s->seen_n = 0;
            else
                wpg_sentinels(p, s);
        }
        if (((game.play_gameturn % 128) == (GameTurn)((p * 5 + 70) % 128)) && (world_pentagram_fx(p, WFX_SIEGE_MAPS) > 0))
            wpg_siege(p, s);
    }
}

/******************************************************************************/
/* gold and XP from the world's events (2026-11-27): Blood Money, Plunder, Pillager, Scout's Reward, Crusade's End,
   Boss Slayer; XP for fortresses razed, World Bosses beaten, raids beaten, portals, levels */

GoldAmount world_pentagram_gold_more(PlayerNumber plyr_idx, GoldAmount gold)
{
    if (!wpw_keeper(plyr_idx) || (gold <= 0))
        return gold;
    return (GoldAmount)((long)gold * (100 + FX(plyr_idx, WFX_GOLD_PCT)) / 100);
}

void world_pentagram_room_taken(struct Room *room, const struct Thing *claimer)
{
    if (!world_mode_active() || room_is_invalid(room) || thing_is_invalid(claimer) || !wpw_keeper(claimer->owner) ||
        (room->owner == claimer->owner) || !room_role_matches(room->kind, RoRoF_GoldStorage))
        return;
    const long pct = FX(claimer->owner, WFX_PLUNDER_PCT);
    const GoldAmount gold = (GoldAmount)((long)room->capacity_used_for_storage * pct / 100);
    if (gold <= 0)
        return;
    player_add_offmap_gold(claimer->owner, world_pentagram_gold_more(claimer->owner, gold));
    SYNCLOG("World: player %d plunders %ld gold more from player %d's Treasury (%ld in it)", (int)claimer->owner,
        (long)gold, (int)room->owner, (long)room->capacity_used_for_storage);
}

void world_pentagram_heart_destroyed(PlayerNumber by, const struct Thing *heartng)
{
    if (!wpw_keeper(by) || !thing_exists(heartng) || (heartng->owner != PLAYER_GOOD) || (FX(by, WFX_PILLAGER) <= 0))
        return;
    /* one more spellbook, and the heroes' Treasury gold round the Heart (40 slabs) carried to the keeper */
    world_progress_drop_outpost_book(heartng->mappos.x.stl.num - 3, heartng->mappos.y.stl.num - 3);
    const MapSlabCoord hx = subtile_slab(heartng->mappos.x.stl.num), hy = subtile_slab(heartng->mappos.y.stl.num);
    GoldAmount total = 0;
    for (struct Room *room = start_rooms; room < end_rooms; room++)
    {
        if (!room_exists(room) || (room->owner != PLAYER_GOOD) || !room_role_matches(room->kind, RoRoF_GoldStorage))
            continue;
        if ((abs((int)subtile_slab(room->central_stl_x) - (int)hx) > 40) || (abs((int)subtile_slab(room->central_stl_y) - (int)hy) > 40))
            continue;
        long k = 0;
        for (SlabCodedCoords sn = room->slabs_list; (sn > 0) && (k++ < 1024); sn = get_next_slab_number_in_room(sn))
        {
            struct Thing *hoard = find_gold_hoarde_at(slab_subtile_center(slb_num_decode_x(sn)), slab_subtile_center(slb_num_decode_y(sn)));
            if (!thing_exists(hoard) || (hoard->valuable.gold_stored <= 0))
                continue;
            total += remove_gold_from_hoarde(hoard, room, hoard->valuable.gold_stored);
        }
    }
    if (total > 0)
        player_add_offmap_gold(by, world_pentagram_gold_more(by, total));
    SYNCLOG("World: Pillager: player %d takes %ld gold from the outpost's Treasury and one more spellbook", (int)by, (long)total);
}

void world_pentagram_portal_captured(PlayerNumber plyr_idx, int biome)
{
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if (!wpw_keeper(plyr_idx) || (s == NULL) || (biome < 0) || (biome >= 32))
        return;
    if (s->biome_mask & (1u << biome))
        return;
    s->biome_mask |= (1u << biome);
    world_pentagram_xp(plyr_idx, WpgXp_Portal, WPG_XP_PORTAL, 0, -1, -1, -1);
    const long reward = FX(plyr_idx, WFX_SCOUTS_REWARD);
    if (reward > 0)
    {
        player_add_offmap_gold(plyr_idx, world_pentagram_gold_more(plyr_idx, reward));
        world_pentagram_xp(plyr_idx, WpgXp_Portal, reward, 0, -1, -1, -1);
        SYNCLOG("World: Scout's Reward: player %d's first portal in biome %d: %ld gold and XP", (int)plyr_idx, biome, reward);
    }
}

void world_pentagram_fortress_razed(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!wpw_keeper(plyr_idx))
        return;
    world_pentagram_xp(plyr_idx, WpgXp_Fortress, WPG_XP_FORTRESS, 0, PLAYER_GOOD, slb_x, slb_y);
    if (FX(plyr_idx, WFX_CRUSADES_END) <= 0)
        return;
    /* Crusade's End: a research book in the keeper's Storage, its grade by the fortress's place (deeper circles hold the
       bigger fortresses: grade 1 in the first two, 5 from the ninth) */
    const int circle = max(1, min(9, world_circle_at_engine_slab(slb_x, slb_y)));
    const int grade = min(5, 1 + (circle - 1) / 2);
    struct Thing *heartng = get_player_soul_container(plyr_idx);
    if (!thing_exists(heartng))
        return;
    const MapSubtlCoord bx = heartng->mappos.x.stl.num + 3, by = heartng->mappos.y.stl.num + 3;
    world_progress_drop_books(bx, by, grade, -1, 1);
    const ThingModel book = world_progress_book_model();
    const struct StructureList *olist = get_list_for_thing_class(TCls_Object);
    long guard = 0;
    for (ThingIndex i = olist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *obj = thing_get(i);
        if (thing_is_invalid(obj))
            break;
        i = obj->next_of_class;
        if ((obj->model == book) && (obj->creation_turn == game.play_gameturn) && (abs((int)obj->mappos.x.stl.num - (int)bx) <= 3) &&
            (abs((int)obj->mappos.y.stl.num - (int)by) <= 3))
        {
            world_storage_add_loot(obj, plyr_idx);
            break;
        }
    }
    SYNCLOG("World: Crusade's End: player %d razed a fortress (circle %d): a grade %d book for its Storage", (int)plyr_idx,
        circle, grade);
}

TbBool world_pentagram_crusade_spared(PlayerNumber plyr_idx)
{
    return wpw_keeper(plyr_idx) && (FX(plyr_idx, WFX_CRUSADES_END) > 0);
}

void world_pentagram_raid_over(PlayerNumber plyr_idx, int made, int killed, double raid_power, double army_power)
{
    /* beaten: half its heroes or more fell (the others fled); its strength next to the army's */
    if (!wpw_keeper(plyr_idx) || (made <= 0) || (killed * 2 < made))
        return;
    const int upper = world_threat_upper_level(plyr_idx);
    const double r = (army_power > 0.0) ? raid_power / army_power : 1.0;
    const int level = max(1, (int)(upper * r + 0.5));
    struct Thing *heartng = get_player_soul_container(plyr_idx);
    world_pentagram_xp(plyr_idx, WpgXp_Raid, WPG_XP_RAID, level, PLAYER_GOOD,
        thing_exists(heartng) ? subtile_slab(heartng->mappos.x.stl.num) : -1,
        thing_exists(heartng) ? subtile_slab(heartng->mappos.y.stl.num) : -1);
}

void world_pentagram_boss_won(PlayerNumber plyr_idx, int grade)
{
    if (!wpw_keeper(plyr_idx))
        return;
    world_pentagram_xp(plyr_idx, WpgXp_Boss, WPG_XP_BOSS, 0, PLAYER_GOOD, -1, -1);
    if (FX(plyr_idx, WFX_BOSS_SLAYER) <= 0)
        return;
    /* Boss Slayer: one more book, in the keeper's Storage (its gold share is half again: world_boss.c) */
    struct Thing *heartng = get_player_soul_container(plyr_idx);
    if (!thing_exists(heartng))
        return;
    const MapSubtlCoord bx = heartng->mappos.x.stl.num + 3, by = heartng->mappos.y.stl.num - 3;
    world_progress_drop_books(bx, by, grade, -1, 1);
    const ThingModel book = world_progress_book_model();
    const struct StructureList *olist = get_list_for_thing_class(TCls_Object);
    long guard = 0;
    for (ThingIndex i = olist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *obj = thing_get(i);
        if (thing_is_invalid(obj))
            break;
        i = obj->next_of_class;
        if ((obj->model == book) && (obj->creation_turn == game.play_gameturn) && (abs((int)obj->mappos.x.stl.num - (int)bx) <= 3) &&
            (abs((int)obj->mappos.y.stl.num - (int)by) <= 3))
        {
            world_storage_add_loot(obj, plyr_idx);
            break;
        }
    }
    SYNCLOG("World: Boss Slayer: player %d gets one more book (grade %d)", (int)plyr_idx, grade);
}

void world_pentagram_level_gained(PlayerNumber plyr_idx)
{
    world_pentagram_xp(plyr_idx, WpgXp_Level, WPG_XP_LEVEL, 0, -1, -1, -1);
}

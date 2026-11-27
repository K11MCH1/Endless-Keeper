/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_time.c
 *     Endless worlds: the Pentagram's skills that keep time (each world turn, per keeper), and the traps' and healing's
 *     helpers (see world_pentagram_hooks.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pentagram.h"
#include "world_pentagram_hooks.h"

#include <string.h>

#include "bflib_basics.h"
#include "bflib_math.h"
#include "game_legacy.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "thing_creature.h"
#include "thing_doors.h"
#include "thing_traps.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_terrain.h"
#include "config_objects.h"
#include "config_trapdoor.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "player_instances.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX(p, e) world_pentagram_fx((p), (e))
#define SLAB_UNITS (COORD_PER_STL * STL_PER_SLB)
#define TURNS_A_SECOND 20

static long wpt_dist_slabs(const struct Coord3d *a, const struct Coord3d *b)
{
    return (long)(get_2d_distance(a, b) / SLAB_UNITS);
}

/******************************************************************************/
/* Healing (apply_health_to_thing) */

HitPoints world_pentagram_heal(const struct Thing *thing, HitPoints amount)
{
    if (!world_mode_active() || (amount <= 0) || !thing_exists(thing) || (thing->owner >= PLAYERS_MAX))
        return amount;
    const int pct = FX(thing->owner, WFX_HEAL_OWN_PCT);
    if (pct == 0)
        return amount;
    const struct SlabMap *slb = get_slabmap_for_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
    if (slabmap_block_invalid(slb) || (slabmap_owner(slb) != thing->owner))
        return amount;
    /* the fraction of a point comes as a whole point that often (as the Lair's level does) */
    const long scaled = (long)amount * (100 + pct);
    HitPoints out = (HitPoints)(scaled / 100);
    if ((long)((game.play_gameturn + thing->index * 37) % 100) < (scaled % 100))
        out++;
    return out;
}

/******************************************************************************/
/* Traps: Artifice's minors, Killing Floor, Minefield */

long world_pentagram_trap_damage_pct(const struct Thing *traptng)
{
    if (!world_mode_active() || !thing_exists(traptng) || (traptng->owner >= PLAYERS_MAX))
        return 0;
    const PlayerNumber p = traptng->owner;
    long pct = FX(p, WFX_TRAP_DMG_PCT);
    if (FX(p, WFX_KILLING_FLOOR))
    {
        const struct SlabMap *slb = get_slabmap_for_subtile(traptng->mappos.x.stl.num, traptng->mappos.y.stl.num);
        struct Thing *heart = get_player_soul_container(p);
        if (!slabmap_block_invalid(slb) && (slabmap_owner(slb) == p) && thing_exists(heart) &&
            (wpt_dist_slabs(&traptng->mappos, &heart->mappos) <= 20))
            pct += FX(p, WFX_KILLING_FLOOR);
    }
    /* Minefield: while the day's first raid is on */
    if (FX(p, WFX_MINEFIELD) && world_raid_on(p))
    {
        struct WorldPentagram *s = world_pentagram_state(p);
        if (s->minefield_day == (uint32_t)world_day_number())
            pct += FX(p, WFX_MINEFIELD);
    }
    return pct;
}

/******************************************************************************/
/* War Horn: when each creature began answering the Call To Arms (every simulating machine keeps it alike) */

#define WPT_CTA_MAX 512
static ThingIndex wpt_cta_thing[WPT_CTA_MAX];
static GameTurn wpt_cta_since[WPT_CTA_MAX];

TbBool world_pentagram_cta_fresh(const struct Thing *thing)
{
    for (int i = 0; i < WPT_CTA_MAX; i++)
        if (wpt_cta_thing[i] == thing->index)
            return (game.play_gameturn - wpt_cta_since[i]) <= 20 * TURNS_A_SECOND;
    return false;
}

static void wpt_cta_track(struct Thing *c, TbBool answering)
{
    int free_slot = -1;
    for (int i = 0; i < WPT_CTA_MAX; i++)
    {
        if (wpt_cta_thing[i] == c->index)
        {
            if (!answering)
                wpt_cta_thing[i] = 0;
            return;
        }
        if ((free_slot < 0) && (wpt_cta_thing[i] == 0))
            free_slot = i;
    }
    if (answering && (free_slot >= 0))
    {
        wpt_cta_thing[free_slot] = c->index;
        wpt_cta_since[free_slot] = game.play_gameturn;
    }
}

/******************************************************************************/
/* Each world turn */

static TbBool wpt_enemy_near(PlayerNumber p, const struct Coord3d *pos, long slabs)
{
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    int guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard < 20000); guard++)
    {
        struct Thing *c = thing_get(i);
        if (!thing_exists(c))
            break;
        i = c->next_of_class;
        if ((c->owner != p) && (c->owner != game.neutral_player_num) && !players_are_mutual_allies(p, c->owner) &&
            (wpt_dist_slabs(&c->mappos, pos) <= slabs))
            return true;
    }
    return false;
}

static void wpt_heal_pct(struct Thing *c, long permil)
{
    struct CreatureControl *cc = creature_control_get_from_thing(c);
    if (creature_control_invalid(cc) || (c->health >= cc->max_health))
        return;
    apply_health_to_thing_and_display_health(c, (HitPoints)max(1L, (long)cc->max_health * permil / 1000));
}

void world_pentagram_keep_time(PlayerNumber p)
{
    struct WorldPentagram *s = world_pentagram_state(p);
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if ((s == NULL) || dungeon_invalid(dungeon))
        return;
    struct Thing *heart = get_player_soul_container(p);
    /* the Heart's extra health: given once as it grows, taken off as it shrinks */
    if (thing_exists(heart))
    {
        const int want = FX(p, WFX_HEART_HP_PCT);
        if (want != s->heart_pct_applied)
        {
            const struct ObjectConfigStats *objst = get_object_model_stats(heart->model);
            const long delta = (long)objst->health * (want - s->heart_pct_applied) / 100;
            heart->health = (HitPoints)max(1L, min((long)world_pentagram_heart_max(heart, objst->health), (long)heart->health + delta));
            s->heart_pct_applied = (int16_t)want;
        }
        /* Iron Heart: 1% every 10 s while no enemy is within 10 slabs */
        if (FX(p, WFX_HEART_REGEN) && ((game.play_gameturn + p) % (10 * TURNS_A_SECOND) == 0) && (heart->health > 0))
        {
            const struct ObjectConfigStats *objst = get_object_model_stats(heart->model);
            const HitPoints top = world_pentagram_heart_max(heart, objst->health);
            if ((heart->health < top) && !wpt_enemy_near(p, &heart->mappos, 10))
                heart->health = min(top, heart->health + max(1, top / 100));
        }
    }
    const TbBool second = ((game.play_gameturn + p) % TURNS_A_SECOND) == 0;
    const int fire = FX(p, WFX_HEARTFIRE), garrison = FX(p, WFX_GARRISON), horn = FX(p, WFX_WAR_HORN);
    if (second && (fire || garrison || horn))
    {
        int guard = 0;
        for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard < 4000); guard++)
        {
            struct Thing *c = thing_get(i);
            if (!thing_exists(c))
                break;
            struct CreatureControl *cc = creature_control_get_from_thing(c);
            i = cc->players_next_creature_idx;
            /* Heartfire: 1% a second within 6 slabs of the Heart */
            if (fire && thing_exists(heart) && (wpt_dist_slabs(&c->mappos, &heart->mappos) <= 6))
                wpt_heal_pct(c, 10L * fire);
            /* Garrison: 2% a second in a Guard Post within 20 slabs of the Heart */
            if (garrison && thing_exists(heart) && (wpt_dist_slabs(&c->mappos, &heart->mappos) <= 20))
            {
                struct Room *room = get_room_thing_is_on(c);
                if (!room_is_invalid(room) && (room->owner == p) && room_role_matches(room->kind, RoRoF_CrGuard))
                    wpt_heal_pct(c, 10L * garrison);
            }
            if (horn)
                wpt_cta_track(c, creature_affected_by_call_to_arms(c) || creature_is_called_to_arms(c));
        }
    }
    /* Living Fortress: the keeper's doors within 15 slabs of its Heart mend 2% a second */
    const int mend = FX(p, WFX_LIVING_FORTRESS);
    if (second && mend && thing_exists(heart))
    {
        const struct StructureList *slist = get_list_for_thing_class(TCls_Door);
        int guard = 0;
        for (ThingIndex i = slist->index; (i > 0) && (guard < 20000); guard++)
        {
            struct Thing *d = thing_get(i);
            if (!thing_exists(d))
                break;
            i = d->next_of_class;
            if ((d->owner != p) || (wpt_dist_slabs(&d->mappos, &heart->mappos) > 15))
                continue;
            const HitPoints top = get_thing_max_health(d);
            if (d->health < top)
                d->health = min(top, d->health + max(1, top * mend / 100));
        }
    }
    /* Minefield: the day's first raid is marked when it comes */
    if (FX(p, WFX_MINEFIELD) && world_raid_on(p))
    {
        const uint32_t today = (uint32_t)world_day_number();
        if ((s->minefield_day != today) && (s->minefield_day != today + 1000000u))
            s->minefield_day = today;
    }
}

#ifdef __cplusplus
}
#endif

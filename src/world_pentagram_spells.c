/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_spells.c
 *     Endless worlds: the Pentagram's skills for keeper spells (see world_pentagram_hooks.h): prices, free casts,
 *     Overchannel, Spell Echo, Resonance, durations and strength.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pentagram.h"
#include "world_pentagram_hooks.h"

#include "bflib_basics.h"
#include "bflib_math.h"
#include "game_legacy.h"
#include "game_merge.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "creature_control.h"
#include "config_magic.h"
#include "magic_powers.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_expedition.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX(p, e) world_pentagram_fx((p), (e))
#define SLAB_UNITS (COORD_PER_STL * STL_PER_SLB)

static TbBool wps_keeper(PlayerNumber p)
{
    return world_mode_active() && (p >= 0) && (p < PLAYERS_MAX) && (p != game.neutral_player_num);
}

/* while a cast made one level higher by Overchannel pays: the price is the level asked for (set round a cast) */
static int wps_overchannel_paying;
/* the casts Spell Echo and Resonance make themselves (no echo of an echo) */
static int wps_inner_cast;

GoldAmount world_pentagram_power_price(PlayerNumber plyr_idx, PowerKind pwkind, KeepPwrLevel power_level, GoldAmount price)
{
    if (!wps_keeper(plyr_idx) || (price <= 0))
        return price;
    long pct = 100 + FX(plyr_idx, WFX_SPELL_COST_PCT);
    if (power_level == 0)
        pct += FX(plyr_idx, WFX_SPELL_LOW_COST_PCT);
    if (!world_is_day())
        pct -= FX(plyr_idx, WFX_WITCHING_HOUR);
    if (pwkind == PwrK_CALL2ARMS)
        pct += FX(plyr_idx, WFX_CTA_COST_PCT);
    if ((pwkind == PwrK_OBEY) && FX(plyr_idx, WFX_OBEDIENCE))
        pct -= 50;
    long out = (long)price * max(5L, pct) / 100;
    if ((pwkind == world_expedition_power_kind()) && (pwkind > 0))
        out += FX(plyr_idx, WFX_EXPEDITION_COST); /* Cartographer: 1,500 less a point */
    return (GoldAmount)max(0L, out);
}

/** Overchannel: while paying for a cast made a level higher, the price of the level asked for. */
KeepPwrLevel world_pentagram_price_level(PlayerNumber plyr_idx, KeepPwrLevel power_level)
{
    return (wps_overchannel_paying && (power_level > 0)) ? (KeepPwrLevel)(power_level - 1) : power_level;
}

GoldAmount world_pentagram_spell_pay(PlayerNumber plyr_idx, PowerKind pwkind, GoldAmount price)
{
    if (!wps_keeper(plyr_idx) || (price <= 0))
        return price;
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    /* Eclipse: once each night, the next spell is free */
    if (FX(plyr_idx, WFX_ECLIPSE) && !world_is_day() && (s->eclipse_night != (uint32_t)world_day_number()))
    {
        s->eclipse_night = (uint32_t)world_day_number();
        SYNCLOG("World: player %d's Eclipse pays for its %s", (int)plyr_idx, power_code_name(pwkind));
        return 0;
    }
    /* Soul Harvest: what deaths in the Prison and Torture Room banked */
    if (s->soul_credit > 0)
    {
        const GoldAmount used = min((GoldAmount)s->soul_credit, price);
        s->soul_credit -= (uint32_t)used;
        return price - used;
    }
    return price;
}

void world_pentagram_spell_unpay(PlayerNumber plyr_idx, GoldAmount credit)
{
    /* (the cast failed for want of gold: the credit was spent before the gold was taken; audit 2026-11-28) */
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if ((s == NULL) || (credit <= 0))
        return;
    s->soul_credit += (uint32_t)credit;
}

/** A creature died in the keeper's Prison or Torture Room: Soul Harvest banks gold for the next spell. */
void world_pentagram_soul_harvest(PlayerNumber plyr_idx)
{
    if (!wps_keeper(plyr_idx) || !FX(plyr_idx, WFX_SOUL_HARVEST))
        return;
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    s->soul_credit = (uint32_t)min((long)FX(plyr_idx, WFX_SOUL_HARVEST), (long)s->soul_credit + FX(plyr_idx, WFX_SOUL_HARVEST));
}

KeepPwrLevel world_pentagram_cast_begin(PlayerNumber plyr_idx, PowerKind pwkind, KeepPwrLevel power_level)
{
    if (!wps_keeper(plyr_idx) || wps_inner_cast || !FX(plyr_idx, WFX_OVERCHANNEL))
        return power_level;
    const struct PowerConfigStats *powerst = get_power_model_stats(pwkind);
    if ((powerst == NULL) || (power_level >= MAGIC_OVERCHARGE_LEVELS) || (powerst->cost[power_level + 1] <= 0))
        return power_level;
    wps_overchannel_paying = 1;
    return (KeepPwrLevel)(power_level + 1);
}

void world_pentagram_cast_end(PlayerNumber plyr_idx, PowerKind pwkind, KeepPwrLevel power_level, MapSubtlCoord stl_x,
    MapSubtlCoord stl_y, struct Thing *thing, unsigned long mod_flags, TbResult ret)
{
    wps_overchannel_paying = 0;
    if ((ret != Lb_SUCCESS) || wps_inner_cast || !wps_keeper(plyr_idx))
        return;
    wps_inner_cast = 1;
    /* Resonance: Protect Monster and Speed Monster also touch the keeper's creatures beside the one cast on */
    const int res = FX(plyr_idx, WFX_RESONANCE);
    if ((res > 0) && ((pwkind == PwrK_PROTECT) || (pwkind == PwrK_SPEEDCRTR)) && thing_exists(thing) && thing_is_creature(thing))
    {
        const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
        int touched = 0, guard = 0;
        for (ThingIndex i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start; (i > 0) && (touched < res) && (guard < 2000); guard++)
        {
            struct Thing *c = thing_get(i);
            if (!thing_exists(c))
                break;
            i = creature_control_get_from_thing(c)->players_next_creature_idx;
            if ((c != thing) && (get_2d_distance(&c->mappos, &thing->mappos) <= 2 * SLAB_UNITS))
            {
                magic_use_power_direct(plyr_idx, pwkind, power_level, c->mappos.x.stl.num, c->mappos.y.stl.num, c,
                    mod_flags | PwMod_CastForFree);
                touched++;
            }
        }
    }
    /* Spell Echo: a chance the spell comes again on the same spot, free */
    const int echo = FX(plyr_idx, WFX_SPELL_ECHO);
    if ((echo > 0) && (pwkind != PwrK_HAND) && (pwkind != PwrK_SLAP) && (pwkind != PwrK_POSSESS) && ((int)GAME_RANDOM(100) < echo))
    {
        if (!thing_exists(thing) || thing_is_creature(thing) || (stl_x != 0) || (stl_y != 0))
        {
            SYNCLOG("World: player %d's %s echoes", (int)plyr_idx, power_code_name(pwkind));
            magic_use_power_direct(plyr_idx, pwkind, power_level, stl_x, stl_y, thing_exists(thing) ? thing : INVALID_THING,
                mod_flags | PwMod_CastForFree);
        }
    }
    wps_inner_cast = 0;
}

GameTurnDelta world_pentagram_spell_duration(PlayerNumber caster, SpellKind spell_idx, GameTurnDelta duration)
{
    if (!wps_keeper(caster) || (duration <= 0))
        return duration;
    const struct SpellConfig *spconf = get_spell_config(spell_idx);
    if (spconf == NULL)
        return duration;
    long pct = 100 + FX(caster, WFX_SPELL_POWER_PCT);
    if ((spconf->linked_power == PwrK_SPEEDCRTR) || (spconf->linked_power == PwrK_PROTECT))
    {
        pct += FX(caster, WFX_BUFF_TIME_PCT);
        if (!world_is_day() && FX(caster, WFX_MOON_WARD))
            pct += 100;
    }
    return (GameTurnDelta)((long)duration * pct / 100);
}

/** The spell levels' bonuses with the Pentagram's on top (world_power_bonus). */
int world_pentagram_power_bonus(PlayerNumber plyr_idx, int what, int bonus)
{
    if (!wps_keeper(plyr_idx))
        return bonus;
    const int power = FX(plyr_idx, WFX_SPELL_POWER_PCT);
    switch (what)
    {
    case 0: /* WPB_ImpLevels: Seasoned Imps */
        return bonus + FX(plyr_idx, WFX_IMP_LEVELS);
    case 1: /* WPB_SightRadius: Far Sight */
        return bonus + FX(plyr_idx, WFX_SIGHT_PCT) + power;
    case 2: /* WPB_SpeedEffect */
        return bonus + power;
    case 5: /* WPB_LightningDamage: Thunderhand */
        return bonus + FX(plyr_idx, WFX_LIGHTNING_PCT) + power;
    default:
        return bonus;
    }
}

long world_pentagram_heal_amount(PlayerNumber plyr_idx, long amount)
{
    if (!wps_keeper(plyr_idx))
        return amount;
    return amount * (100 + FX(plyr_idx, WFX_HEAL_SPELL_PCT) + FX(plyr_idx, WFX_SPELL_POWER_PCT)) / 100;
}

#ifdef __cplusplus
}
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_truce.c
 *     Endless worlds: truces and friendly fire.
 * @par Purpose:
 *     The user, 2026-11-14: "Players/AI keepers who are truced with each other, their creatures should not be able
 *     to damage each other. Basically create a 'friendly fire' variable and logic that we can use for allied
 *     players. We will implement this across Endless mode, not just the World Boss Event."
 *
 *     Measured that day in a World Boss fight of ten keepers with five creatures each, none of them allied, all
 *     truced in the arena: of the damage their creatures took, 75,100 came from the boss and 57,694 from the other
 *     keepers' creatures, most of it in the first 25 seconds (Bile Demons' gas over the whole crowd, missiles that
 *     met another keeper's creature on their way to the boss). The creatures left each other alone and still wore
 *     each other down. The game's own rules for allies have the same hole: most area attacks are set to hit every
 *     creature that is not the attacker's own.
 *
 *     Who is at peace:
 *       - allied keepers, anywhere (the game's alliance: in an endless world, the keepers of a co-op party);
 *       - the keepers at a World Boss's fight, inside its arena (world_boss_spares).
 *     A truce of a new kind is one more line in truce_covers().
 */
/******************************************************************************/
#include "pre_inc.h"
#include <stdlib.h>
#include <string.h>
#include "world_truce.h"
#include "bflib_basics.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "thing_data.h"
#include "keeperfx.hpp"
#include "world_stream.h"
#include "world_boss.h"
#include "config.h"
#include "config_creature.h"
#include "creature_control.h"
#include "thing_creature.h"
#include "thing_list.h"
#include "post_inc.h"

#define TRUCE_SAVE_VERSION 1

static uint8_t truce_friendly_fire; /* 0: allied or truced keepers cannot hurt each other's creatures */

/** Test switch KFX_WORLD_FRIENDLYFIRE=1|0: the variable's value in this run, whatever the world's save says (to see
 *  the same fight with friendly fire on and off). Returns whether the switch is set. */
static TbBool truce_test_value(uint8_t *value)
{
    static int set = -1, to = 0;
    if (set < 0)
    {
        const char *v = getenv("KFX_WORLD_FRIENDLYFIRE");
        set = (v != NULL) ? 1 : 0;
        to = ((v != NULL) && (atoi(v) > 0)) ? 1 : 0;
    }
    if (set)
        *value = (uint8_t)to;
    return (set != 0);
}

TbBool world_friendly_fire(void)
{
    return !world_mode_active() || (truce_friendly_fire != 0);
}

void world_set_friendly_fire(TbBool on)
{
    if ((truce_friendly_fire != 0) != (on != 0))
        SYNCLOG("World truce: friendly fire is %s: keepers who are allied or truced %s hurt each other's creatures",
            on ? "on" : "off", on ? "can" : "cannot");
    truce_friendly_fire = on ? 1 : 0;
}

TbBool world_truce_allied(PlayerNumber a, PlayerNumber b)
{
    return (a != b) && (a >= 0) && (b >= 0) && (a < PLAYERS_COUNT) && (b < PLAYERS_COUNT) &&
        players_are_mutual_allies(a, b);
}

/** Is `from` at peace with this creature's keeper, where the creature stands? *allied: by the game's own alliance. */
static TbBool truce_covers(const struct Thing *victim, PlayerNumber from, TbBool *allied)
{
    *allied = false;
    if (!world_mode_active() || (victim == NULL) || (victim->class_id != TCls_Creature) || (from < 0) ||
        (from >= PLAYERS_COUNT) || (victim->owner == from))
        return false;
    *allied = world_truce_allied(victim->owner, from);
    return *allied || world_boss_spares(victim, from);
}

/** Test switch KFX_WORLD_TRUCEWATCH=n: every n turns, when something has been held off (or, with friendly fire on,
 *  let through) since the last line, how much damage between keepers at peace that is so far. */
static int watch_every = -1;
static long long watch_held, watch_let;
static long watch_held_hits, watch_let_hits;
static TbBool watch_changed;

static void truce_watch(TbBool spared, HitPoints dmg)
{
    if (watch_every < 0)
    {
        const char *v = getenv("KFX_WORLD_TRUCEWATCH");
        watch_every = (v != NULL) ? max(1, atoi(v)) : 0;
    }
    if (watch_every == 0)
        return;
    if (spared)
    {
        watch_held += dmg;
        watch_held_hits++;
    } else
    {
        watch_let += dmg;
        watch_let_hits++;
    }
    watch_changed = true;
}

static void truce_watch_turn(void)
{
    if ((watch_every <= 0) || !watch_changed || ((game.play_gameturn % (GameTurn)watch_every) != 0))
        return;
    watch_changed = false;
    SYNCLOG("World truce watch: between keepers at peace so far: %lld damage held off (%ld hits), %lld let through "
        "(%ld hits); friendly fire is %s", watch_held, watch_held_hits, watch_let, watch_let_hits,
        world_friendly_fire() ? "on" : "off");
}

TbBool world_truce_no_harm(const struct Thing *victim, HitPoints dmg, PlayerNumber from)
{
    TbBool allied;
    if (!truce_covers(victim, from, &allied))
        return false;
    const TbBool spared = !world_friendly_fire();
    truce_watch(spared, dmg);
    return spared;
}

TbBool world_truce_no_touch(const struct Thing *victim, PlayerNumber from)
{
    TbBool allied;
    return !world_friendly_fire() && truce_covers(victim, from, &allied) && !allied;
}

/** Test switch KFX_WORLD_TRUCETEST=<turn> (single player; with KFX_WORLD_PROFKEEPERS=1 and KFX_WORLD_WAKE=1 for a
 *  computer keeper, and KFX_WORLD_BOSSTEAM for the player's own creatures): at that turn the player's keeper and the
 *  first awake computer keeper become allies, five creatures of that keeper (two Bile Demons, a Warlock, a Hell Hound
 *  and a Dark Mistress, level 8) are put beside the player's creatures at its Dungeon Heart, and a Giant with 200,000
 *  health comes into their midst: the two keepers' creatures fight it side by side, gas and missiles and all. With
 *  KFX_WORLD_TRUCEWATCH the log says how much harm between the two was held off (or, with friendly fire on, let
 *  through). */
void world_truce_turn(void)
{
    truce_watch_turn();
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_TRUCETEST");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    const struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    PlayerNumber k = PLAYER_NONE;
    for (PlayerNumber p = 1; (p < PLAYERS_COUNT) && (k == PLAYER_NONE); p++)
        if (world_player_is_keeper(p) && !world_keeper_is_passive(p) && thing_exists(find_players_dungeon_heart(p)))
            k = p;
    if (!thing_exists(heart) || (k == PLAYER_NONE))
    {
        SYNCLOG("World truce test: no awake computer keeper to ally with");
        return;
    }
    set_ally_with_player(PLAYER0, k, true);
    set_ally_with_player(k, PLAYER0, true);
    static const char *const kinds[5] = {"BILE_DEMON", "BILE_DEMON", "SORCEROR", "HELL_HOUND", "DARK_MISTRESS"};
    int made = 0;
    for (int i = 0; i < 5; i++)
    {
        struct Coord3d pos = heart->mappos;
        pos.x.val += (2 + i) * COORD_PER_STL;
        pos.y.val += 9 * COORD_PER_STL;
        struct Thing *thing = create_creature(&pos, (ThingModel)get_id(creature_desc, kinds[i]), k);
        if (thing_is_invalid(thing))
            continue;
        set_creature_level(thing, 7);
        made++;
    }
    struct Coord3d pos = heart->mappos;
    pos.x.val += 4 * COORD_PER_STL;
    pos.y.val += 7 * COORD_PER_STL;
    struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, "GIANT"), PLAYER_GOOD);
    if (!thing_is_invalid(hero))
    {
        set_creature_level(hero, 9);
        struct CreatureControl *cctrl = creature_control_get_from_thing(hero);
        cctrl->max_health = 200000;
        hero->health = 200000;
    }
    SYNCLOG("World truce test: players 0 and %d are allies now (%s); %d creatures of player %d beside player 0's, and a "
        "Giant with 200,000 health among them (%s)", (int)k, players_are_mutual_allies(PLAYER0, k) ? "allied" : "NOT allied",
        made, (int)k, thing_is_invalid(hero) ? "not made" : "made");
}

void world_truce_reset(void)
{
    truce_friendly_fire = 0;
    truce_test_value(&truce_friendly_fire);
}

size_t world_truce_save_size(void)
{
    return 2;
}

void world_truce_save(uint8_t *buf)
{
    buf[0] = TRUCE_SAVE_VERSION;
    buf[1] = truce_friendly_fire;
}

TbBool world_truce_load(const uint8_t *buf, size_t len)
{
    world_truce_reset();
    if ((len < 2) || (buf[0] != TRUCE_SAVE_VERSION))
        return false;
    truce_friendly_fire = buf[1] ? 1 : 0;
    truce_test_value(&truce_friendly_fire);
    if (truce_friendly_fire)
        SYNCLOG("World truce: friendly fire is on in this world: keepers who are allied or truced can hurt each other's "
            "creatures");
    return true;
}

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_hand.c
 *     Endless world: the Hand of Evil grows with the keeper's player level.
 * @par Purpose:
 *     See world_hand.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_hand.h"

#include <stdlib.h>

#include "world_stream.h"
#include "world_progress.h"
#include "game_legacy.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_lair.h"
#include "config_magic.h"
#include "config_creature.h"
#include "map_data.h"
#include "power_hand.h"
#include "magic_powers.h"
#include "gui_msgs.h"
#include "keeperfx.hpp"
#include "replay_rewind.h"
#include <string.h>
#include "net_state.h"
#include "player_instances.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/** KFX_WORLD_HANDLEVEL=n (test switch): the Hand acts as if every keeper were at player level n. */
static int hand_level_override(void)
{
    static int level = -2;
    if (level == -2)
    {
        const char *v = getenv("KFX_WORLD_HANDLEVEL");
        level = (v != NULL) ? atoi(v) : -1;
        if (level > 0)
            SYNCLOG("World: test switch KFX_WORLD_HANDLEVEL=%d: the Hand acts as at player level %d", level, level);
    }
    return level;
}

static int hand_level(PlayerNumber plyr_idx)
{
    const int over = hand_level_override();
    return (over > 0) ? over : world_player_level(plyr_idx);
}

/** The grip at a player level: the stock eight at level 1, one more for each level after, sixteen at most. */
static int grip_at_level(int level, int stock)
{
    int grip = 7 + level;
    if (grip > WORLD_HAND_GRIP_MAX)
        grip = WORLD_HAND_GRIP_MAX;
    if (grip < stock)
        grip = stock;
    if (grip > MAX_THINGS_IN_HAND)
        grip = MAX_THINGS_IN_HAND;
    return grip;
}

int world_hand_capacity(PlayerNumber plyr_idx, int stock)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX))
        return stock;
    return grip_at_level(hand_level(plyr_idx), stock);
}

int world_hand_level_for_grip(int grip)
{
    for (int level = 1; level <= WORLD_PLAYER_LEVEL_MAX; level++)
        if (grip_at_level(level, 0) >= grip)
            return level;
    return 0;
}

TbBool world_hand_wide_slap(PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX))
        return false;
    return (hand_level(plyr_idx) >= WORLD_HAND_SLAP_LEVEL);
}

/* The hurried: a short burst of walking speed only (audit 2026-11-13: not the stock slap counter, which also
   quickens attacks and work, wakes sleepers and stops meals). Kept per creature, by its control slot (re-check
   2026-11-13: a shared list could fill up and stop every keeper's slaps). Not saved, emptied when a world ends or a
   save is loaded (world_level_end), and rewound with replays. A slot reused by another creature is told apart by
   its creation turn. */
static uint32_t hurry_until[CREATURES_COUNT] kfx_rewindable;
static int32_t hurry_born[CREATURES_COUNT] kfx_rewindable;

static TbBool is_hurried(const struct Thing *thing)
{
    const CctrlIndex i = thing->ccontrol_idx;
    return (i > 0) && (i < CREATURES_COUNT) && (hurry_until[i] > (uint32_t)game.play_gameturn)
        && (hurry_born[i] == thing->creation_turn);
}

long world_hand_speed(const struct Thing *thing, long speed)
{
    if (!world_mode_active() || !is_hurried(thing))
        return speed;
    return speed * WORLD_HAND_HURRY_PERCENT / 100;
}

void world_hand_reset(void)
{
    memset(hurry_until, 0, sizeof(hurry_until));
    memset(hurry_born, 0, sizeof(hurry_born));
}

/** Hurry one creature: walking speed only, for a few seconds; no hurt, anger, cowering, and nothing a fight, work,
 *  sleep or meals feel. A sleeping or fighting creature, or one the stock rules won't let the keeper slap, is left
 *  alone. Returns whether it was hurried. */
static TbBool hurry_creature(struct Thing *creatng, PlayerNumber plyr_idx)
{
    if (!thing_exists(creatng) || !thing_is_creature(creatng) || (creatng->owner != plyr_idx)
        || thing_is_picked_up(creatng) || creature_is_dying(creatng) || creature_is_being_unconscious(creatng)
        || creature_is_sleeping(creatng) || !thing_slappable(creatng, plyr_idx))
        return false;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl) || (cctrl->combat_flags != 0))
        return false;
    const CctrlIndex i = creatng->ccontrol_idx;
    if ((i <= 0) || (i >= CREATURES_COUNT))
        return false;
    hurry_until[i] = (uint32_t)game.play_gameturn + WORLD_HAND_HURRY_TURNS;
    hurry_born[i] = creatng->creation_turn;
    return true;
}

static int slap_neighbours(PlayerNumber plyr_idx, struct Thing *slapped)
{
    const MapSubtlCoord cx = slapped->mappos.x.stl.num, cy = slapped->mappos.y.stl.num;
    int n_hurried = 0;
    for (MapSubtlCoord y = cy - WORLD_HAND_SLAP_REACH; y <= cy + WORLD_HAND_SLAP_REACH; y++)
    {
        for (MapSubtlCoord x = cx - WORLD_HAND_SLAP_REACH; x <= cx + WORLD_HAND_SLAP_REACH; x++)
        {
            if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                continue;
            const struct Map *mapblk = get_map_block_at(x, y);
            long i = get_mapwho_thing_index(mapblk);
            unsigned long k = 0;
            while (i != 0)
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if ((thing != slapped) && thing_is_creature(thing) && hurry_creature(thing, plyr_idx))
                {
                    if (++n_hurried >= WORLD_HAND_SLAP_MAX)
                        return n_hurried;
                }
                if (++k > THINGS_COUNT)
                    break;
            }
        }
    }
    return n_hurried;
}

void world_hand_slapped(struct PlayerInfo *player, struct Thing *slapped)
{
    if ((player == NULL) || !thing_exists(slapped) || !thing_is_creature(slapped))
        return;
    const PlayerNumber plyr_idx = player->id_number;
    /* only the keeper's own creatures: a slap never reaches another keeper's */
    if ((slapped->owner != plyr_idx) || !world_hand_wide_slap(plyr_idx))
        return;
    const int n = slap_neighbours(plyr_idx, slapped);
    if (n > 0)
    {
        SYNCDBG(8, "World: player %d's slap hurried %d creature(s) beside the %s", (int)plyr_idx, n, thing_model_name(slapped));
    }
}

/******************************************************************************/
/** A level gained by this machine's keeper: what the Hand gained, as messages. */
void world_hand_level_news(int before, int after)
{
    if ((after <= before) || (hand_level_override() > 0))
        return;
    const int stock = (my_player_number < PLAYERS_COUNT) ? game.conf.rules[my_player_number].gameplay.max_things_in_hand : 8;
    const int was = grip_at_level(before, stock), now = grip_at_level(after, stock);
    if (now > was)
        message_add_fmt(MsgType_Player, my_player_number, "Your Hand can now hold %d creatures", now);
    if ((before < WORLD_HAND_SLAP_LEVEL) && (after >= WORLD_HAND_SLAP_LEVEL))
        message_add_fmt(MsgType_Player, my_player_number, "Your slap now hurries the creatures beside it");
}

/******************************************************************************/
/** KFX_WORLD_HANDTEST=turn (test switch): at that turn, beside this machine's keeper's heart, make 20 Flies, pick
 *  up as many as the Hand holds (logs it against the capacity), drop them, then slap one and log how many of the
 *  others it hurried. */
/** KFX_WORLD_HANDREAL=turn (test switch): the real Hand, as a player uses it: 20 Flies by this machine's Heart, then
 *  one pick-up through the Hand's own path (use_power_hand) every 4 turns; 90 turns on, what the Hand really holds is
 *  logged against its capacity; then it drops them one by one the stock way and slaps one with the real slap. */
static void hand_real_test(void)
{
    static long at = -2;
    static ThingIndex fl[20];
    static int nfl = 0;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_HANDREAL");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at < 0) || !world_mode_active() || ((long)game.play_gameturn < at) || ((long)game.play_gameturn > at + 200))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (((int)p >= PLAYERS_COUNT) || !thing_exists(heart))
        return;
    struct PlayerInfo *player = get_player(p);
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    const long t = (long)game.play_gameturn - at;
    if (t == 0)
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 6 * COORD_PER_STL;
        nfl = 0;
        for (int i = 0; i < 20; i++)
        {
            struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, "FLY"), p);
            if (!thing_is_invalid(c))
                fl[nfl++] = c->index;
        }
        return;
    }
    if ((t < 80) && ((t % 4) == 0))
    {
        const int k = (int)(t / 4) - 1;
        struct Thing *c = ((k >= 0) && (k < nfl)) ? thing_get(fl[k]) : INVALID_THING;
        if (thing_exists(c) && !thing_is_in_power_hand_list(c, p))
            use_power_hand(p, c->mappos.x.stl.num, c->mappos.y.stl.num, c->index);
        return;
    }
    if (t == 90)
    {
        const int cap = world_hand_capacity(p, game.conf.rules[p].gameplay.max_things_in_hand);
        SYNCLOG("World: hand real test: player %d at level %d, 20 pick-ups tried one by one: the Hand holds %d, capacity %d: %s",
            (int)p, hand_level(p), (int)dungeon->num_things_in_hand, cap, (dungeon->num_things_in_hand == cap) ? "OK" : "WRONG");
        return;
    }
    if ((t > 90) && (t < 160) && ((t % 3) == 0) && (dungeon->num_things_in_hand > 0))
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 6 * COORD_PER_STL;
        dump_first_held_thing_on_map(p, pos.x.stl.num, pos.y.stl.num, true); /* the stock drop */
        return;
    }
    if (t == 170)
    {
        world_hand_reset();
        struct Thing *c = (nfl > 0) ? thing_get(fl[0]) : INVALID_THING;
        if (!thing_exists(c))
            return;
        slap_thing(player, get_player_primary_user(player), c);
        int n = 0;
        for (int i = 1; i < nfl; i++)
        {
            const struct Thing *o = thing_get(fl[i]);
            n += (thing_exists(o) && is_hurried(o)) ? 1 : 0;
        }
        SYNCLOG("World: hand real test: the real slap at level %d hurried %d of the other Flies (wide slap %d)", hand_level(p), n,
            (int)world_hand_wide_slap(p));
    }
}

void world_hand_test_turn(void)
{
    hand_real_test();
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_HANDTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_HANDTEST=%ld", at);
    }
    static ThingIndex later[20];
    static int32_t later_born[20];
    static int nlater;
    if ((at >= 0) && ((long)game.play_gameturn == at + 60) && world_mode_active())
    {
        int still = 0;
        for (int i = 0; i < nlater; i++)
        {
            const struct Thing *t = thing_get(later[i]);
            if (thing_exists(t) && (t->creation_turn == later_born[i]) && is_hurried(t))
                still++;
        }
        SYNCLOG("World: hand test: 60 turns after the slap, %d of the Flies are still hurried", still);
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    /* this machine's keeper, or on a dedicated server the first human keeper with a Heart (a joined player) */
    PlayerNumber p = my_player_number;
    for (PlayerNumber q = 0; netstate_dedicated() && (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
        if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
        {
            p = q;
            break;
        }
    const struct Thing *heart = get_player_soul_container(p);
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !thing_exists(heart))
        return;
    struct PlayerInfo *player = get_player(p);
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    struct Coord3d pos = heart->mappos;
    pos.y.val += 6 * COORD_PER_STL;
    const ThingModel fly = (ThingModel)get_id(creature_desc, "FLY");
    struct Thing *made[20];
    int nmade = 0;
    for (int i = 0; i < 20; i++)
    {
        struct Thing *t = create_creature(&pos, fly, p);
        if (!thing_is_invalid(t))
            made[nmade++] = t;
    }
    const int had = dungeon->num_things_in_hand;
    int picked = 0;
    for (int i = 0; i < nmade; i++)
    {
        if (power_hand_is_full(player))
            break;
        if (place_thing_in_power_hand(made[i], p))
            picked++;
    }
    const int cap = world_hand_capacity(p, game.conf.rules[p].gameplay.max_things_in_hand);
    SYNCLOG("World: hand test: player %d at level %d holds %d of %d Flies (had %d), capacity %d: %s", (int)p,
        hand_level(p), picked, nmade, had, cap, (had + picked == cap) ? "OK" : "WRONG");
    if (getenv("KFX_WORLD_HANDHOLD") != NULL)
        return; /* KFX_WORLD_HANDHOLD=1 as well: they stay in the Hand (for a joined player's screenshot) */
    for (int i = 0; i < nmade; i++)
    {
        if (thing_is_in_power_hand_list(made[i], p))
        {
            remove_thing_from_power_hand_list(made[i], p);
            drop_held_thing_on_ground(dungeon, made[i], &pos);
        }
    }
    if (nmade < 2)
        return;
    world_hand_reset(); /* only this slap's hurry is counted below */
    slap_thing(player, get_player_primary_user(player), made[0]); /* the real slap (it calls world_hand_slapped) */
    nlater = 0;
    for (int i = 1; i < nmade; i++)
    {
        later[nlater] = made[i]->index;
        later_born[nlater++] = made[i]->creation_turn;
    }
    int nhurried = 0, quick_attacks = 0;
    long fast = 0, slow = 0;
    for (int i = 1; i < nmade; i++)
    {
        const struct CreatureControl *cctrl = creature_control_get_from_thing(made[i]);
        if (creature_control_invalid(cctrl))
            continue;
        if (is_hurried(made[i]))
        {
            nhurried++;
            fast = get_creature_speed(made[i]);
        } else
            slow = get_creature_speed(made[i]);
        if (creature_affected_by_slap(made[i]))
            quick_attacks++; /* the stock slap's quicker attacks and work: must stay 0 */
    }
    const int expect = world_hand_wide_slap(p) ? ((nmade - 1 < WORLD_HAND_SLAP_MAX) ? nmade - 1 : WORLD_HAND_SLAP_MAX) : 0;
    SYNCLOG("World: hand test: a slap at level %d hurried %d of the %d Flies beside it (expected %d), speed %ld vs %ld, "
        "slap counter on %d of them (expected 0): %s", hand_level(p), nhurried, nmade - 1, expect, fast, slow,
        quick_attacks, ((nhurried == expect) && (quick_attacks == 0) && ((expect == 0) || (fast > slow))) ? "OK" : "WRONG");
}

#ifdef __cplusplus
}
#endif

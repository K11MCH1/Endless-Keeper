/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_hunt.c
 *     Endless world: The Hunt (2026-11-13; see world_hunt.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - The call: 10 night kills of a kind for the first Alpha, then 20, 30, 40 and 50 more for the next ones.
 *     - The tricks: the Spider's Alpha is fast (Speed), the Skeleton's and the Beetle's armoured (Armour), the Fly's
 *       unseen (Invisibility), the Zombie's turns shots back (Rebound); any other kind's is fast. Kept up while it lives.
 *     - The ranks: 1 its dead drop half as much gold again, 2 twice as much; 3 half its packs within 60 slabs of your
 *       Heart don't come, 4 none of them; 5 a tame one of the kind (at its pack's level) comes to your Lair, once.
 *     - Per keeper: kills and ranks are each keeper's own; an Alpha comes for the keeper whose creatures called it.
 *       Kills made while the caretaker keeps the dungeon don't count.
 *     Saved: HNT1.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_hunt.h"
#include "world_stream.h"
#include "world_wild.h"
#include "world_daynight.h"
#include "world_caretaker.h"
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_magic.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_navigate.h"
#include "thing_objects.h"
#include "post_inc.h"

#define HN_KINDS 8
#define HN_RANKS 5
#define HN_THIN_DIST 60
static const char *const hn_kind_names[HN_KINDS] = {"SPIDER", "SKELETON", "BUG", "FLY", "ZOMBIE", NULL, NULL, NULL};
static const char *const hn_tricks[HN_KINDS] = {"SPELL_SPEED", "SPELL_ARMOUR", "SPELL_ARMOUR", "SPELL_INVISIBILITY",
    "SPELL_REBOUND", "SPELL_SPEED", "SPELL_SPEED", "SPELL_SPEED"};

#pragma pack(1)
struct HnKeeper {
    uint16_t kills[HN_KINDS];        /* night kills since the last Alpha */
    uint8_t rank[HN_KINDS];
    uint8_t called[HN_KINDS];        /* its Alpha comes the next night */
    uint16_t alpha[HN_KINDS];        /* the Alpha out now (thing; 0 none) */
    uint32_t alpha_made[HN_KINDS];   /* ... and its creation turn */
    uint8_t tamed[HN_KINDS];
};
#pragma pack()
static struct HnKeeper hn[PLAYERS_MAX];
static uint8_t hn_was_day = 1;

static int hn_kind_of(ThingModel model)
{
    for (int k = 0; k < HN_KINDS; k++)
        if ((hn_kind_names[k] != NULL) && (get_id(creature_desc, hn_kind_names[k]) == model))
            return k;
    return -1;
}

/** Night kills of a kind that call its next Alpha: 10, 20, 30 ... (test switch KFX_WORLD_HUNTKILLS=n: n, 2n, 3n ...). */
static int hn_needed(int rank)
{
    static int base = -1;
    if (base < 0)
    {
        const char *v = getenv("KFX_WORLD_HUNTKILLS");
        base = ((v != NULL) && (atoi(v) > 0)) ? atoi(v) : 10;
        if (v != NULL)
            SYNCLOG("World hunt test: %d kills call the first Alpha", base);
    }
    return base * (rank + 1);
}

/** An Alpha of kind k is beaten by keeper p (killed, or taken and turned): p's rank with the kind goes up, and at
 *  the last rank a tame one comes to p's Lair. */
static void hn_alpha_beaten(PlayerNumber p, int k, ThingModel model, int level)
{
    struct HnKeeper *h = &hn[p];
    if (h->rank[k] < HN_RANKS)
        h->rank[k]++;
    SYNCLOG("World: player %d kills the %s Alpha: their rank with the kind is %d", (int)p, hn_kind_names[k],
        (int)h->rank[k]);
    if ((h->rank[k] >= HN_RANKS) && !h->tamed[k])
    {
        /* the last rank: a tame one in the Lair */
        struct Room *lair = pick_random_room_of_role(p, RoRoF_LairStorage);
        struct Thing *heart = find_players_dungeon_heart(p);
        struct Coord3d pos;
        if (!room_is_invalid(lair))
        {
            pos.x.val = subtile_coord_center(lair->central_stl_x);
            pos.y.val = subtile_coord_center(lair->central_stl_y);
        } else if (thing_exists(heart))
            pos = heart->mappos;
        else
            return;
        pos.z.val = 0;
        struct Thing *tame = create_creature(&pos, model, p);
        if (thing_exists(tame))
        {
            move_thing_in_map(tame, &pos);
            init_creature_level(tame, level);
            h->tamed[k] = 1;
            SYNCLOG("World: a tame %s comes to player %d's Lair", hn_kind_names[k], (int)p);
        }
    }
}

void world_hunt_kill(struct Thing *victim, struct Thing *killertng, PlayerNumber killer_plyr_idx, unsigned long death_flags)
{
    if (!world_mode_active() || !thing_exists(victim) || !world_wild_creature(victim))
        return;
    /* knocked out (kill_creature makes it unconscious after this): not a kill */
    if (thing_exists(killertng) && (killertng->owner != game.neutral_player_num) && ((death_flags & CrDed_NoEffects) == 0) &&
        creature_can_be_set_unconscious(victim, killertng, death_flags))
        return;
    PlayerNumber p = killer_plyr_idx;
    if (thing_exists(killertng) && (killertng->class_id == TCls_Creature))
        p = killertng->owner;
    if ((p < 0) || (p >= PLAYERS_MAX) || !world_player_is_human(p) || world_caretaker_active(p))
        return;
    const int k = hn_kind_of(victim->model);
    if (k < 0)
        return;
    struct HnKeeper *h = &hn[p];
    /* an Alpha: the rank goes up */
    for (PlayerNumber q = 0; q < PLAYERS_MAX; q++)
        if ((hn[q].alpha[k] == victim->index) && (hn[q].alpha_made[k] == victim->creation_turn))
        {
            hn[q].alpha[k] = 0;
            hn_alpha_beaten(p, k, victim->model, creature_control_get_from_thing(victim)->exp_level);
            return;
        }
    if (world_is_day())
        return;
    /* ranks 1 and 2: more gold from its dead */
    if ((h->rank[k] >= 1) && (victim->creature.gold_carried > 0))
    {
        struct Coord3d pos = victim->mappos;
        pos.z.val = 0;
        drop_gold_pile(victim->creature.gold_carried * min((int)h->rank[k], 2) / 2, &pos);
    }
    if (h->rank[k] >= HN_RANKS)
        return;
    if (h->kills[k] < 65535)
        h->kills[k]++;
    if (!h->called[k] && (h->alpha[k] == 0) && (h->kills[k] >= hn_needed(h->rank[k])))
    {
        h->called[k] = 1;
        h->kills[k] = 0;
        SYNCLOG("World: player %d has hunted enough %s: its Alpha comes the next night", (int)p, hn_kind_names[k]);
    }
}

TbBool world_hunt_pack_allowed(ThingModel model, MapSlabCoord x, MapSlabCoord y)
{
    if (!world_mode_active())
        return true;
    const int k = hn_kind_of(model);
    if (k < 0)
        return true;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (hn[p].rank[k] < 3)
            continue;
        struct Thing *heart = find_players_dungeon_heart(p);
        if (!thing_exists(heart))
            continue;
        const long dx = (long)subtile_slab(heart->mappos.x.stl.num) - x, dy = (long)subtile_slab(heart->mappos.y.stl.num) - y;
        if (dx * dx + dy * dy > (long)HN_THIN_DIST * HN_THIN_DIST)
            continue;
        if (hn[p].rank[k] >= 4)
            return false;
        if (((x * 31 + y * 17 + (long)game.play_gameturn / 20) & 1) == 0)
            return false; /* rank 3: half of them */
    }
    return true;
}

void world_hunt_turn(void)
{
    if (!world_mode_active())
        return;
    const TbBool day = world_is_day();
    const TbBool dusk = !day && hn_was_day;
    hn_was_day = day ? 1 : 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        for (int k = 0; k < HN_KINDS; k++)
        {
            struct HnKeeper *h = &hn[p];
            if (dusk && h->called[k] && world_player_is_human(p))
            {
                const long model = get_id(creature_desc, hn_kind_names[k]);
                struct Thing *alpha = world_wild_alpha(p, (ThingModel)model, 2);
                if (thing_exists(alpha))
                {
                    h->called[k] = 0;
                    h->alpha[k] = alpha->index;
                    h->alpha_made[k] = alpha->creation_turn;
                    SYNCLOG("World: the %s Alpha (level %d) comes out of a burrow for player %d", hn_kind_names[k],
                        (int)creature_control_get_from_thing(alpha)->exp_level + 1, (int)p);
                }
            }
            if ((h->alpha[k] != 0) && ((game.play_gameturn % 64) == 29))
            {
                struct Thing *alpha = thing_get(h->alpha[k]);
                if (!thing_exists(alpha) || (alpha->creation_turn != h->alpha_made[k]) || (alpha->class_id != TCls_Creature))
                {
                    h->alpha[k] = 0; /* faded at dawn, or gone some other way: called again */
                    h->called[k] = 1;
                    continue;
                }
                if (alpha->owner != PLAYER_GOOD)
                {
                    /* taken prisoner and turned: beaten by its new keeper (if a human one who is here), and no longer an
                       Alpha: its trick stops and the ladder goes on */
                    const PlayerNumber q = alpha->owner;
                    h->alpha[k] = 0;
                    if ((q >= 0) && (q < PLAYERS_MAX) && world_player_is_human(q) && !world_caretaker_active(q))
                    {
                        SYNCLOG("World: the %s Alpha is turned by player %d", hn_kind_names[k], (int)q);
                        hn_alpha_beaten(q, k, alpha->model, creature_control_get_from_thing(alpha)->exp_level);
                    } else
                        h->called[k] = 1;
                    continue;
                }
                if (creature_is_being_unconscious(alpha) || creature_is_kept_in_custody(alpha))
                    continue; /* down or held: its trick waits */
                const long spell = get_id(spell_desc, hn_tricks[k]);
                if (spell > 0)
                    apply_spell_effect_to_thing(alpha, (SpellKind)spell, creature_control_get_from_thing(alpha)->exp_level,
                        alpha->owner);
            }
        }
    }
}

void world_hunt_reset(void)
{
    memset(hn, 0, sizeof(hn));
    hn_was_day = 1;
}

size_t world_hunt_save_size(void)
{
    static const struct HnKeeper zero;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (memcmp(&hn[p], &zero, sizeof(zero)) != 0)
            return sizeof(hn);
    return 0;
}

void world_hunt_save(uint8_t *buf)
{
    memcpy(buf, hn, sizeof(hn));
}

TbBool world_hunt_load(const uint8_t *buf, size_t len)
{
    world_hunt_reset();
    if (len < sizeof(hn))
        return false;
    memcpy(hn, buf, sizeof(hn));
    return true;
}

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rivals.c
 *     Endless worlds: rival pairs, two enemy keepers locked in a war that never ends (2026-11-25).
 * @par Purpose:
 *     The user: "2 Enemy Keepers are interlocked in a battle of attrition. Their Dungeon are connected through a
 *     battlefield [...] of constant battles. Imps try to claim each other's land through the battlefield, but to no
 *     avail. These two keepers will not seek to attack Player as they are too busy with each other. They will only
 *     attack when Player attempts to invade their territory. There is a chance, if Player invades their territory,
 *     they will temporarily truce to drive you away. The Two keepers can still expand, but away from each other.
 *     [...] A never-ending battle between 2 enemy keepers."
 *
 *     The generator (world_gen.c, generator version 9 on) places a pair: two mirror-image dungeons facing each other
 *     across a battlefield of nobody's ground (six shapes: lava moats, a lava-ringed island, a ford...). They wake and
 *     sleep together (world_stream.c): their war is on whenever a human comes near either.
 *     - The war (world_keeper_ai.c): each keeper sends waves of its creatures onto the battlefield under Call To Arms,
 *       further forward when it is the stronger, and marks its Imps' way across; Imps claim the battlefield's dirt from
 *       their own war road on, and claim back what the other side took.
 *     - Why it never ends: neither can take the other's home. Inside a keeper's territory (RIV_TERRITORY slabs round
 *       its heart, the battlefield apart) its rival's Imps cannot claim its ground, and its rival's creatures leave its
 *       rooms and its Dungeon Heart alone; neither ever uses KeeperFX's attack plans on the other. The third of the
 *       battlefield at each end is that side's: the other's Imps never claim there, so only the middle third changes
 *       hands (first run: without that the stronger side held 459 of 469 slabs). Each keeper's portal attracts
 *       creatures only up to its war size (second run: the winning side's portal kept filling it, 22 creatures against
 *       5). Reinforcements come through a keeper's portal while its army is clearly the weaker (the threat measure,
 *       world_threat.c), two at a time and twice as often when it is far weaker, at the two armies' average level, of
 *       the generator's keeper pool by distance, at most two Dragons (by its portal too: riv_war_kind; first play
 *       session: copying kinds made both armies all Dragons); a keeper half again as strong as its rival pulls back
 *       to regroup (world_keeper_ai.c). A keeper short of Imps gets one, and one short of gold a trickle. So the
 *       ground in between changes hands, the creatures die and come back, and neither side wins.
 *     - Humans: the pair never goes after a human (no party, no assault, never a target of their war) until a human's
 *       creature (an Imp too) stands in either keeper's territory: that keeper defends. With a chance (RIV_TRUCE_PCT,
 *       rolled once each time a human comes in) the two make a truce (the game's alliance, so friendly fire is off
 *       between them: world_truce.c) and the other keeper sends its creatures to help. The truce ends RIV_TRUCE_LINGER
 *       after the last human creature left both territories, or after RIV_TRUCE_MAX whatever happens; the next can be
 *       rolled only RIV_TRUCE_COOLDOWN after.
 *
 *     Game state: runs on the simulating machine (lockstep machines alike), with no randomness of its own (the truce
 *     roll is a hash of the pair and its count of intrusions); saved as the world extra RIV1.
 *
 * Test hooks: KFX_WORLD_RIVALTEST="T" (with KFX_WORLD_WAKE=1): at T the pair nearest the start is loaded (and wakes);
 *     from then on a summary line every 500 turns. KFX_WORLD_RIVALINVADE="t1,t2[,n]": at t1 n (4) creatures of player
 *     0 stand in the first keeper's territory, at t2 they are taken home. KFX_WORLD_RIVALPASS="t1,t2": at t1 three of
 *     player 0's creatures stand on open ground 28-45 slabs from the pair's first heart, outside both territories (to
 *     see the pair leave them alone), at t2 they are taken home. KFX_WORLD_RIVALTRUCE=pct: the truce chance.
 *     KFX_WORLD_RIVALTRUCEMAX=turns: how long a truce lasts at most. KFX_WORLD_RIVALCAMP=T (a game the user plays,
 *     2026-11-25: the nearest pair is 190+ slabs out): at T a camp of player 0's stands beside the battlefield of the pair
 *     nearest the start (see riv_test_camp). KFX_WORLD_RIVALREVEAL=T: at T the nearest pair's ground and the way to it
 *     revealed to player 0, the camera at its battlefield (riv_test_reveal). KFX_WORLD_RIVALLOG=1: every look logged.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_rivals.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bflib_basics.h"
#include "chat_box.h"
#include "config.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "frontmenu_ingame_map.h"
#include "map_blocks.h"
#include "world_keeper_ai.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "map_data.h"
#include "net_main.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "room_entrance.h"
#include "server_admin.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "world_daynight.h"
#include "world_gen.h"
#include "world_stream.h"
#include "world_threat.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define RIV_LOOK 32                    /* turns between two looks at the pairs */
#define RIV_TERRITORY 24               /* slabs from a heart: its territory (the battlefield apart) */
#define RIV_TRUCE_PCT 40               /* the chance of a truce when a human comes in */
#define RIV_TRUCE_LINGER (20 * 30)     /* a truce outlasts the last intruder by this */
#define RIV_TRUCE_MAX (20 * 240)       /* and lasts this at most */
#define RIV_TRUCE_COOLDOWN (20 * 120)  /* turns from the end of one truce to the next roll */
#define RIV_REINFORCE (20 * 50)        /* turns between reinforcements (the side with fewer: half of it) */
#define RIV_GOLD_GAP (20 * 60)
#define RIV_GOLD 400                   /* gold a keeper short of it gets every RIV_GOLD_GAP */
#define RIV_GOLD_LOW 3000
#define RIV_GOLD_BUILD 1000            /* the trickle while it builds an endgame-size room */
#define RIV_MIN_IMPS 3
#define RIV_PAIRS 32
#define RIV_SAVE_VERSION 2 /* 2: RivPair.rest */

#pragma pack(1)
struct RivPair {
    uint8_t used;
    uint8_t truce;                     /* 1: allied against an intruder now */
    int8_t truce_against;              /* the human they drive away */
    uint8_t intruded;                  /* bit 0: a human was in the first keeper's territory at the last look; bit 1: second */
    int32_t cx, cy;                    /* the pair's name: its first keeper's start chunk */
    uint32_t intrusions;               /* the truce roll's count */
    uint32_t truce_since, truce_seen, truce_next;
    uint32_t last_reinforce[2], last_gold[2];
    uint32_t last_seen;                /* the last look that saw both awake (a full table reuses the oldest) */
    uint16_t truces, reinforcements[2];
    uint8_t rest;                      /* the truce is the safe day's (world_safe_day): it lasts the day and its night */
};
#pragma pack()

static struct RivPair rpairs[RIV_PAIRS];

/** What a keeper's look found (not saved: worked out again every look). */
struct RivSide {
    GameTurn turn;                     /* the look's turn (0: none yet) */
    int pair;                          /* index in rpairs, -1 */
    PlayerNumber rival;
    TbBool truce, rest, invaded, rival_invaded;
    PlayerNumber invader;
    struct Coord3d invader_pos, rival_invader_pos;
};
static struct RivSide rside[PLAYERS_MAX];

static int riv_log = -1;
static TbBool riv_logging(void)
{
    if (riv_log < 0)
        riv_log = (getenv("KFX_WORLD_RIVALLOG") != NULL) ? 1 : 0;
    return riv_log > 0;
}

static TbBool riv_runs(void)
{
    return world_mode_active() && !netstate_is_client();
}

/******************************************************************************/
/* Where things are: a pair's geometry in world slabs */

/** On the battlefield (its rectangle, a little wider)? */
static TbBool riv_on_field(const struct WorldKeeperRival *kr, int64_t wx, int64_t wy)
{
    const TbBool ns = (kr->heart_x == kr->rival_heart_x);
    const int64_t a = ns ? wy - kr->field_y : wx - kr->field_x, p = ns ? wx - kr->field_x : wy - kr->field_y;
    return (a >= -kr->field_half) && (a <= kr->field_half) && (p >= -(kr->field_width + 3)) && (p <= kr->field_width + 3);
}

/** How far along the battlefield a world slab is, from the keeper's own end (0) to its rival's (1000). */
static int riv_field_t(const struct WorldKeeperRival *kr, int64_t wx, int64_t wy)
{
    const TbBool ns = (kr->heart_x == kr->rival_heart_x);
    const int64_t sign = ns ? ((kr->rival_heart_y > kr->heart_y) ? 1 : -1) : ((kr->rival_heart_x > kr->heart_x) ? 1 : -1);
    const int64_t a = sign * (ns ? wy - kr->field_y : wx - kr->field_x);
    return (int)((a + kr->field_half) * 1000 / max(1, 2 * kr->field_half));
}

/** In the territory of the keeper whose heart is at (hx, hy): RIV_TERRITORY slabs round it, the battlefield apart. */
static TbBool riv_in_territory(const struct WorldKeeperRival *kr, int64_t hx, int64_t hy, int64_t wx, int64_t wy)
{
    const int64_t dx = wx - hx, dy = wy - hy;
    return (dx * dx + dy * dy <= (int64_t)RIV_TERRITORY * RIV_TERRITORY) && !riv_on_field(kr, wx, wy);
}

static void riv_thing_world(const struct Thing *t, int64_t *wx, int64_t *wy)
{
    world_slab_to_world(subtile_slab(t->mappos.x.stl.num), subtile_slab(t->mappos.y.stl.num), wx, wy);
}

/** The engine position of a world slab's middle, when it is loaded. */
static TbBool riv_engine_pos(int64_t wx, int64_t wy, struct Coord3d *out)
{
    MapSlabCoord ex, ey;
    if (!world_engine_slab(wx, wy, &ex, &ey))
        return false;
    out->x.val = subtile_coord_center(slab_subtile_center(ex));
    out->y.val = subtile_coord_center(slab_subtile_center(ey));
    out->z.val = 0;
    out->z.val = get_floor_height_at(out);
    return true;
}

/** The world slab on the battlefield's middle line, `t` (0..1000) of the way from the keeper's own end to its rival's. */
static void riv_field_point(const struct WorldKeeperRival *kr, int t, int64_t *wx, int64_t *wy)
{
    const TbBool ns = (kr->heart_x == kr->rival_heart_x);
    const int64_t sign = ns ? ((kr->rival_heart_y > kr->heart_y) ? 1 : -1) : ((kr->rival_heart_x > kr->heart_x) ? 1 : -1);
    const int64_t a = sign * (-(int64_t)kr->field_half + (2 * (int64_t)kr->field_half * t) / 1000);
    *wx = ns ? kr->field_x : kr->field_x + a;
    *wy = ns ? kr->field_y + a : kr->field_y;
}

/******************************************************************************/
/* The look-up for the hooks: once a turn for a player */

struct RivCache {
    GameTurn turn;
    TbBool known, rival_pair;
    struct WorldKeeperRival kr;
};
static struct RivCache rcache[PLAYERS_MAX];

static const struct WorldKeeperRival *riv_of(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_mode_active())
        return NULL;
    struct RivCache *c = &rcache[p];
    if (!c->known || (c->turn != game.play_gameturn))
    {
        c->known = true;
        c->turn = game.play_gameturn;
        c->rival_pair = world_keeper_rival(p, &c->kr);
    }
    return c->rival_pair ? &c->kr : NULL;
}

TbBool world_rivals_is_rival(PlayerNumber plyr_idx)
{
    return riv_of(plyr_idx) != NULL;
}

TbBool world_rivals_may_claim(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const struct WorldKeeperRival *kr = riv_of(plyr_idx);
    if ((kr == NULL) || (kr->rival == PLAYER_NONE))
        return true;
    const struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb))
        return true;
    int64_t wx, wy;
    world_slab_to_world(slb_x, slb_y, &wx, &wy);
    if (riv_on_field(kr, wx, wy))
        return riv_field_t(kr, wx, wy) < 667; /* (its rival's third of the battlefield: never) */
    return (slabmap_owner(slb) != kr->rival) || !riv_in_territory(kr, kr->rival_heart_x, kr->rival_heart_y, wx, wy);
}

TbBool world_rivals_spare_room(PlayerNumber attacker, const struct Room *room)
{
    const struct WorldKeeperRival *kr = riv_of(attacker);
    return (kr != NULL) && (room != NULL) && (kr->rival != PLAYER_NONE) && (room->owner == kr->rival);
}

TbBool world_rivals_spare_heart(PlayerNumber attacker, PlayerNumber heart_owner)
{
    const struct WorldKeeperRival *kr = riv_of(attacker);
    return (kr != NULL) && (kr->rival != PLAYER_NONE) && (heart_owner == kr->rival);
}

TbBool world_rivals_truce_ground(PlayerNumber plyr_idx, PlayerNumber owner)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (owner < 0) || (owner == plyr_idx))
        return false;
    const struct WorldKeeperRival *kr = riv_of(plyr_idx);
    return (kr != NULL) && (kr->rival == owner) && rside[plyr_idx].truce && players_are_mutual_allies(plyr_idx, owner);
}

TbBool world_rivals_site_ok(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const struct WorldKeeperRival *kr = riv_of(plyr_idx);
    if (kr == NULL)
        return true;
    int64_t wx, wy;
    world_slab_to_world(slb_x, slb_y, &wx, &wy);
    /* how far towards its rival's heart, from its own (along the line of the two): at most 4 slabs */
    const double ux = (double)(kr->rival_heart_x - kr->heart_x), uy = (double)(kr->rival_heart_y - kr->heart_y);
    const double len = sqrt(ux * ux + uy * uy);
    if (len < 1.0)
        return true;
    return ((double)(wx - kr->heart_x) * ux + (double)(wy - kr->heart_y) * uy) / len <= 4.0;
}

TbBool world_rivals_view(PlayerNumber plyr_idx, struct WorldRivalView *out)
{
    const struct WorldKeeperRival *kr = riv_of(plyr_idx);
    if (kr == NULL)
        return false;
    if (out == NULL)
        return true;
    memset(out, 0, sizeof(*out));
    const struct RivSide *rs = &rside[plyr_idx];
    const TbBool fresh = (rs->turn != 0) && ((GameTurnDelta)(game.play_gameturn - rs->turn) <= 2 * RIV_LOOK) && (rs->pair >= 0);
    out->rival = (fresh && kr->active && kr->rival_active) ? kr->rival : PLAYER_NONE;
    out->field_kind = kr->field_kind;
    if (fresh)
    {
        out->truce = rs->truce;
        out->rest = rs->rest;
        out->invaded = rs->invaded;
        out->invader = rs->invader;
        out->invader_pos = rs->invader_pos;
        out->rival_invaded = rs->rival_invaded;
        out->rival_invader_pos = rs->rival_invader_pos;
    }
    int64_t wx, wy;
    out->field_known = riv_engine_pos(kr->field_x, kr->field_y, &out->field_mid);
    riv_field_point(kr, 350, &wx, &wy);
    out->field_known = riv_engine_pos(wx, wy, &out->field_own) && out->field_known;
    riv_field_point(kr, 620, &wx, &wy);
    out->field_known = riv_engine_pos(wx, wy, &out->field_far) && out->field_known;
    riv_engine_pos(kr->rival_heart_x, kr->rival_heart_y, &out->rival_heart);
    return true;
}

/******************************************************************************/
/* The pairs' table */

static int riv_pair(int32_t cx, int32_t cy, TbBool make)
{
    int free_i = -1, old_i = -1;
    for (int i = 0; i < RIV_PAIRS; i++)
    {
        if (rpairs[i].used && (rpairs[i].cx == cx) && (rpairs[i].cy == cy))
            return i;
        if (!rpairs[i].used && (free_i < 0))
            free_i = i;
        if (rpairs[i].used && !rpairs[i].truce && ((old_i < 0) || (rpairs[i].last_seen < rpairs[old_i].last_seen)))
            old_i = i;
    }
    if (!make)
        return -1;
    const int i = (free_i >= 0) ? free_i : old_i;
    if (i < 0)
        return -1;
    memset(&rpairs[i], 0, sizeof(rpairs[i]));
    rpairs[i].used = 1;
    rpairs[i].cx = cx;
    rpairs[i].cy = cy;
    rpairs[i].truce_against = -1;
    return i;
}

/** Tells a human player (their own machine, or their user on a server). */
static void riv_tell(PlayerNumber p, const char *text)
{
    SYNCLOG("World rivals: to player %d: %s", (int)p, text);
    if (is_my_player_number(p) && !netstate_dedicated())
    {
        if (chat_box_active())
            chat_box_add(-1, text);
        else
            message_add(MsgType_Blank, 0, text);
        return;
    }
    if (!network_is_active() || !netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static void riv_set_truce(struct RivPair *rp, PlayerNumber a, PlayerNumber b, TbBool on)
{
    if ((a >= 0) && (b >= 0))
    {
        set_ally_with_player(a, b, on);
        set_ally_with_player(b, a, on);
    }
    rp->truce = on ? 1 : 0;
    if (!on)
        rp->rest = 0;
}

static int riv_truce_pct(void)
{
    static int pct = -2;
    if (pct == -2)
    {
        const char *v = getenv("KFX_WORLD_RIVALTRUCE");
        pct = (v != NULL) ? max(0, min(100, atoi(v))) : -1;
    }
    return (pct >= 0) ? pct : RIV_TRUCE_PCT;
}

/** RIV_TRUCE_MAX, or the test switch KFX_WORLD_RIVALTRUCEMAX=turns. */
static GameTurnDelta riv_truce_max(void)
{
    static long t = -2;
    if (t == -2)
    {
        const char *v = getenv("KFX_WORLD_RIVALTRUCEMAX");
        t = (v != NULL) ? max(1L, atol(v)) : -1;
    }
    return (t > 0) ? (GameTurnDelta)t : RIV_TRUCE_MAX;
}

/** The truce roll for the pair's n-th intrusion: the same on every machine and in every load. */
static TbBool riv_truce_roll(const struct RivPair *rp)
{
    uint64_t z = ((uint64_t)(uint32_t)rp->cx << 32) ^ (uint64_t)(uint32_t)rp->cy ^ ((uint64_t)rp->intrusions * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (int)(z % 100) < riv_truce_pct();
}

/******************************************************************************/
/* Keeping the war going */

/** The war size of a keeper: reinforcements stop there. */
static int riv_war_size(const struct WorldKeeperRival *kr)
{
    const double d = hypot((double)kr->heart_x, (double)kr->heart_y);
    return 10 + min(4, (int)(d / 400.0));
}

struct RivArmy {
    double power;                      /* world_threat_army */
    int count, imps;
    long level_sum;
    ThingModel kinds[16];
    int nkinds;
};

static void riv_army(PlayerNumber p, struct RivArmy *a)
{
    memset(a, 0, sizeof(*a));
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    a->imps = dungeon->num_active_diggers;
    struct WorldArmy wa;
    world_threat_army(p, &wa);
    a->power = wa.power;
    long guard = 0;
    for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        const struct Thing *t = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (thing_is_invalid(t) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (creature_is_dying(t))
            continue;
        a->count++;
        a->level_sum += cctrl->exp_level;
        if (a->nkinds < 16)
            a->kinds[a->nkinds++] = t->model;
    }
}

/* The kinds a rival's portal sends to its war: the generator's keeper pool (world_gen.c keeper_creatures, by distance
   from the start), each as likely as there. It was a copy of the kinds the keeper had, or its rival's when it was far
   weaker (the user, 2026-11-25: "The Enemy Keepers only get Dragons": one Dragon from a portal, and both sides copied
   it until all were Dragons). At most RIV_MAX_DRAGONS Dragons, by its portal too (world_rivals_may_attract). */
#define RIV_MAX_DRAGONS 2
static const struct { const char *name; int weight; int min_dist; } riv_pool[] = {
    {"FLY", 4, 0}, {"BUG", 4, 0}, {"SPIDER", 3, 0}, {"DEMONSPAWN", 3, 0}, {"TROLL", 3, 0}, {"ORC", 3, 0},
    {"SORCEROR", 2, 200}, {"HELL_HOUND", 2, 200}, {"DARK_MISTRESS", 2, 260}, {"BILE_DEMON", 2, 260},
    {"TENTACLE", 1, 260}, {"VAMPIRE", 1, 400}, {"DRAGON", 1, 450}};
#define RIV_POOL_N ((int)(sizeof(riv_pool) / sizeof(riv_pool[0])))

static ThingModel riv_dragon_model(void)
{
    static ThingModel m = -1;
    if (m < 0)
        m = (ThingModel)max(0L, (long)get_id(creature_desc, "DRAGON"));
    return m;
}

/** The next kind for the keeper's war (the n-th it is sent): the same on every machine. Far weaker, only kinds whose
 *  creature at that level is at least 70% as strong as its rival's on average (the strongest there is when none is):
 *  the second run of the play-test round, 2026-11-25, had one side's portal draw Bile Demons and Mistresses while
 *  the other's reinforcements, Flies and Beetles at level 1, died as they came (power 157 against 1,186). */
static ThingModel riv_war_kind(PlayerNumber p, const struct WorldKeeperRival *kr, uint32_t n, double strong, int level)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    const double dist = hypot((double)(kr->heart_x - 8), (double)(kr->heart_y - 8));
    const ThingModel dragon = riv_dragon_model();
    const TbBool no_dragon = dungeon_invalid(dungeon) || (dragon <= 0) ||
        (dungeon->owned_creatures_of_model[dragon] >= RIV_MAX_DRAGONS);
    TbBool ok[RIV_POOL_N];
    int total = 0, best = -1;
    double best_power = -1.0;
    for (int i = 0; i < RIV_POOL_N; i++)
    {
        ok[i] = (dist >= riv_pool[i].min_dist) && !(no_dragon && (strcmp(riv_pool[i].name, "DRAGON") == 0));
        if (!ok[i])
            continue;
        if (strong > 0.0)
        {
            const double pw = world_threat_power((ThingModel)max(0L, (long)get_id(creature_desc, riv_pool[i].name)), level);
            if (pw > best_power)
            {
                best_power = pw;
                best = i;
            }
            ok[i] = (pw >= strong * 0.7);
        }
        if (ok[i])
            total += riv_pool[i].weight;
    }
    if ((total <= 0) && (best >= 0))
        return (ThingModel)max(0L, (long)get_id(creature_desc, riv_pool[best].name));
    if (total <= 0)
        return 0;
    uint64_t z = ((uint64_t)(uint32_t)kr->pair_cx << 40) ^ ((uint64_t)(uint32_t)kr->pair_cy << 8) ^ ((uint64_t)kr->which << 60) ^
        ((uint64_t)n * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    int roll = (int)((z ^ (z >> 31)) % (uint64_t)total);
    for (int i = 0; i < RIV_POOL_N; i++)
    {
        if (!ok[i])
            continue;
        if (roll < riv_pool[i].weight)
            return (ThingModel)max(0L, (long)get_id(creature_desc, riv_pool[i].name));
        roll -= riv_pool[i].weight;
    }
    return 0;
}

TbBool world_rivals_may_attract(PlayerNumber plyr_idx, ThingModel crmodel)
{
    const ThingModel dragon = riv_dragon_model();
    if ((crmodel != dragon) || (dragon <= 0) || (riv_of(plyr_idx) == NULL))
        return true;
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    return dungeon_invalid(dungeon) || (dungeon->owned_creatures_of_model[dragon] < RIV_MAX_DRAGONS);
}

static void riv_keep_up(struct RivPair *rp, int side, PlayerNumber p, const struct WorldKeeperRival *kr,
    const struct RivArmy *own, const struct RivArmy *theirs)
{
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    const GameTurn now = game.play_gameturn;
    /* gold: wages and Imps must not run dry in a war that never ends */
    /* (and while it builds an endgame-size room, world_keeper_ai.c, a larger trickle up to that room's cost) */
    const long room_gold = world_keeper_ai_room_gold(p);
    const long gold_low = (room_gold > 0) ? max((long)RIV_GOLD_LOW, room_gold + 1000) : (long)RIV_GOLD_LOW;
    if (((GameTurnDelta)(now - rp->last_gold[side]) >= RIV_GOLD_GAP) && (dungeon->total_money_owned < gold_low))
    {
        const long gift = (room_gold > 0) ? RIV_GOLD_BUILD : RIV_GOLD;
        rp->last_gold[side] = now;
        dungeon->offmap_money_owned += gift;
        dungeon->total_money_owned += gift;
    }
    /* Imps: a keeper with too few gets one at its heart */
    const struct Thing *heartng = find_players_dungeon_heart(p);
    if ((own->imps < RIV_MIN_IMPS) && thing_exists(heartng))
    {
        struct Coord3d pos = heartng->mappos;
        pos.x.val += COORD_PER_STL * 3;
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *imp = create_creature(&pos, get_players_special_digger_model(p), p);
        if (thing_exists(imp))
        {
            move_thing_in_map(imp, &pos);
            if (riv_logging())
                SYNCLOG("World rivals: player %d gets an Imp (%d had)", (int)p, own->imps);
        }
    }
    /* its portal attracts creatures up to its war size only */
    const int war_size = riv_war_size(kr);
    dungeon->max_creatures_attracted = (unsigned char)war_size;
    /* reinforcements through its portal: while its army is clearly the weaker and below its war size; two at a time and
       twice as often when it is far weaker */
    const TbBool far_weaker = (own->power * 10.0 < theirs->power * 7.0);
    const GameTurnDelta gap = far_weaker ? RIV_REINFORCE / 2 : RIV_REINFORCE;
    if ((own->count >= war_size) || (own->power * 10.0 >= theirs->power * 9.0) || ((GameTurnDelta)(now - rp->last_reinforce[side]) < gap) ||
        rp->rest)
        return; /* (resting on a safe day: no war to keep even) */
    const RoomKind entrance = (RoomKind)get_id(room_desc, "ENTRANCE");
    struct Room *room = (entrance > 0) ? room_get(dungeon->room_list_start[entrance]) : INVALID_ROOM;
    if (room_is_invalid(room) || (room->owner != p))
        return;
    const int both = own->count + theirs->count;
    const CrtrExpLevel lvl = (CrtrExpLevel)((both > 0) ? (own->level_sum + theirs->level_sum) / both : 0);
    const int n = (far_weaker && (own->count + 1 < war_size)) ? 2 : 1;
    struct Thing *t = INVALID_THING;
    for (int k = 0; k < n; k++)
    {
        const ThingModel kind = riv_war_kind(p, kr, rp->reinforcements[side],
            far_weaker ? theirs->power / (double)max(1, theirs->count) : 0.0, (int)lvl + 1);
        if (kind <= 0)
            return;
        t = create_creature_at_entrance(room, kind);
        if (!thing_exists(t))
            return;
        set_creature_level(t, lvl);
        rp->reinforcements[side]++;
    }
    rp->last_reinforce[side] = now;
    SYNCLOG("World rivals: player %d's portal sends %d %s (level %d) to its war: %d creatures against %d (power %.0f against %.0f)",
        (int)p, n, thing_model_name(t), (int)lvl + 1, own->count + n, theirs->count, own->power, theirs->power);
}

/******************************************************************************/
/* A look at one pair */

static void riv_look_pair(const struct WorldKeeperRival *ka, PlayerNumber pa)
{
    const int pi = riv_pair(ka->pair_cx, ka->pair_cy, true);
    if (pi < 0)
        return;
    struct RivPair *rp = &rpairs[pi];
    const GameTurn now = game.play_gameturn;
    const PlayerNumber pb = ka->rival;
    struct WorldKeeperRival kb;
    const TbBool both = (pb != PLAYER_NONE) && ka->active && ka->rival_active && world_keeper_rival(pb, &kb);
    rside[pa].turn = now;
    rside[pa].pair = pi;
    rside[pa].rival = both ? pb : PLAYER_NONE;
    if (!both)
    {
        /* no war while one sleeps or is gone: a truce ends (the alliance with it, while both are still this pair) */
        if (rp->truce)
        {
            riv_set_truce(rp, pa, (pb != PLAYER_NONE) ? pb : -1, false);
            SYNCLOG("World rivals: the truce of the pair at %d,%d ends: one of them is asleep or gone", (int)rp->cx, (int)rp->cy);
        }
        rside[pa].truce = false;
        rside[pa].invaded = rside[pa].rival_invaded = false;
        return;
    }
    rp->last_seen = now;
    rside[pb].turn = now;
    rside[pb].pair = pi;
    rside[pb].rival = pa;
    /* humans' creatures in either territory: the nearest to each heart */
    long best_d2[2] = {LONG_MAX, LONG_MAX};
    PlayerNumber who[2] = {PLAYER_NONE, PLAYER_NONE};
    int guests[2] = {0, 0}; /* (for the log) each side's creatures in the other's territory */
    PlayerBitFlags near_humans = pmask_none(); /* humans with a creature within 90 slabs of either heart (the messages) */
    struct Coord3d where[2];
    memset(where, 0, sizeof(where));
    const int64_t hx[2] = {ka->heart_x, kb.heart_x}, hy[2] = {ka->heart_y, kb.heart_y};
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        const struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if (thing_is_picked_up(t) || creature_is_dying(t))
            continue;
        int64_t wx, wy;
        if ((t->owner == pa) || (t->owner == pb))
        {
            if (riv_logging() && !thing_is_creature_digger(t))
            {
                riv_thing_world(t, &wx, &wy);
                const int s = (t->owner == pa) ? 1 : 0; /* (the other side's territory) */
                if (riv_in_territory(ka, hx[s], hy[s], wx, wy))
                    guests[1 - s]++;
            }
            continue;
        }
        if (!world_player_is_human(t->owner))
            continue;
        riv_thing_world(t, &wx, &wy);
        if ((hypot((double)(wx - ka->heart_x), (double)(wy - ka->heart_y)) <= 90.0) ||
            (hypot((double)(wx - kb.heart_x), (double)(wy - kb.heart_y)) <= 90.0))
            pmask_add(&near_humans, t->owner);
        for (int s = 0; s < 2; s++)
        {
            if (!riv_in_territory(ka, hx[s], hy[s], wx, wy))
                continue;
            const long d2 = (long)((wx - hx[s]) * (wx - hx[s]) + (wy - hy[s]) * (wy - hy[s]));
            if (d2 < best_d2[s])
            {
                best_d2[s] = d2;
                who[s] = t->owner;
                where[s] = t->mappos;
            }
        }
    }
    const uint8_t intruded = (uint8_t)(((who[0] != PLAYER_NONE) ? 1 : 0) | ((who[1] != PLAYER_NONE) ? 2 : 0));
    /* the safe day's truce (the user, 2026-11-25: "Allow for a 1 day and night truce for the Enemy Keepers like Player has
       a 1 day and night no raids/wild packs. But the truce is not applicable to Player so they will attack Player on
       sight"): from the safe day's dawn to the next dawn the two are allies; their brains go for a human's creatures
       they see meanwhile (world_keeper_ai.c) */
    if (world_safe_day())
    {
        if (!rp->rest)
        {
            if (!rp->truce)
                riv_set_truce(rp, pa, pb, true);
            rp->rest = 1;
            rp->truce_since = rp->truce_seen = now;
            rp->truce_against = -1;
            SYNCLOG("World rivals: players %d and %d rest from their war for the safe day (%s)", (int)pa, (int)pb,
                wg_field_name(ka->field_kind));
            for (PlayerNumber h = 0; (h < PLAYERS_COUNT) && (h < PLAYERS_MAX); h++)
                if (pmask_has(near_humans, h))
                    riv_tell(h, "The rival keepers rest from their war for the safe day. Not from you: they attack your creatures on sight.");
        }
    } else
    if (rp->rest)
    {
        /* the safe day is over: their war goes on (or, with a human in a territory now, they stay allies against it) */
        rp->rest = 0;
        if (intruded)
        {
            rp->truce_since = rp->truce_seen = now;
            rp->truce_against = (int8_t)((who[0] != PLAYER_NONE) ? who[0] : who[1]);
            SYNCLOG("World rivals: the safe day is over, but players %d and %d stay allies against player %d in their territory",
                (int)pa, (int)pb, (int)rp->truce_against);
        } else
        {
            riv_set_truce(rp, pa, pb, false);
            SYNCLOG("World rivals: the safe day is over: players %d and %d are at war again", (int)pa, (int)pb);
            for (PlayerNumber h = 0; (h < PLAYERS_COUNT) && (h < PLAYERS_MAX); h++)
                if (pmask_has(near_humans, h))
                    riv_tell(h, "The safe day is over: the rival keepers are at war again.");
        }
    } else
    if (rp->truce)
    {
        if (intruded)
            rp->truce_seen = now;
        const TbBool gone = (GameTurnDelta)(now - rp->truce_seen) >= RIV_TRUCE_LINGER;
        const TbBool long_enough = (GameTurnDelta)(now - rp->truce_since) >= riv_truce_max();
        if (gone || long_enough)
        {
            riv_set_truce(rp, pa, pb, false);
            rp->truce_next = now + RIV_TRUCE_COOLDOWN;
            SYNCLOG("World rivals: the truce between players %d and %d ends (%s, after %ld turns); their war goes on", (int)pa,
                (int)pb, gone ? "the intruder left" : "it lasted long enough", (long)(now - rp->truce_since));
            if (rp->truce_against >= 0)
                riv_tell(rp->truce_against, "The rival keepers' truce is over: they are at each other's throats again.");
        }
    } else
    if ((intruded & ~rp->intruded) != 0)
    {
        /* a human has just come into a territory: a truce, or that keeper fights alone */
        const int s = ((intruded & ~rp->intruded) & 1) ? 0 : 1;
        rp->intrusions++;
        const TbBool may = (GameTurnDelta)(now - rp->truce_next) >= 0;
        if (may && riv_truce_roll(rp))
        {
            riv_set_truce(rp, pa, pb, true);
            rp->truce_since = rp->truce_seen = now;
            rp->truce_against = (int8_t)who[s];
            rp->truces++;
            SYNCLOG("World rivals: player %d came into player %d's territory: players %d and %d make a truce to drive it away "
                "(%s)", (int)who[s], (int)(s == 0 ? pa : pb), (int)pa, (int)pb, wg_field_name(ka->field_kind));
            riv_tell(who[s], "The rival keepers lay down their war to drive you out!");
        } else
            SYNCLOG("World rivals: player %d came into player %d's territory: it fights alone, its rival goes on with the war%s",
                (int)who[s], (int)(s == 0 ? pa : pb), may ? "" : " (no truce so soon after the last)");
    }
    rp->intruded = intruded;
    /* what each side's brain sees */
    for (int s = 0; s < 2; s++)
    {
        struct RivSide *rs = &rside[s == 0 ? pa : pb];
        rs->truce = rp->truce;
        rs->rest = rp->rest;
        rs->invaded = (who[s] != PLAYER_NONE);
        rs->invader = who[s];
        rs->invader_pos = where[s];
        rs->rival_invaded = (who[1 - s] != PLAYER_NONE);
        rs->rival_invader_pos = where[1 - s];
    }
    /* keeping the war going */
    struct RivArmy army[2];
    riv_army(pa, &army[0]);
    riv_army(pb, &army[1]);
    riv_keep_up(rp, 0, pa, ka, &army[0], &army[1]);
    riv_keep_up(rp, 1, pb, &kb, &army[1], &army[0]);
    if (riv_logging())
        SYNCLOG("World rivals: pair %d,%d: players %d and %d, %d and %d creatures (%d and %d of them in the other's territory), "
            "%d and %d Imps, truce %d, intruded %d", (int)rp->cx, (int)rp->cy, (int)pa, (int)pb, army[0].count, army[1].count,
            guests[0], guests[1], army[0].imps, army[1].imps, (int)rp->truce, (int)intruded);
}

/******************************************************************************/
/* Test hooks */

static int64_t riv_test_hx[2], riv_test_hy[2];
static TbBool riv_test_have;
static struct WgStructRef riv_test_ref[2];

static void riv_test_parse(const char *name, long *a, long *b, long *c)
{
    *a = *b = *c = -1;
    const char *v = getenv(name);
    if (v == NULL)
        return;
    *a = atol(v);
    const char *p = strchr(v, ',');
    if (p != NULL)
    {
        *b = atol(p + 1);
        p = strchr(p + 1, ',');
        if (p != NULL)
            *c = atol(p + 1);
    }
}

/** Counts the battlefield's slabs by owner: first keeper, second keeper, others claimed, open (path). */
static void riv_test_field_count(int *first, int *second, int *open)
{
    *first = *second = *open = 0;
    const struct WgStructRef *r = &riv_test_ref[0];
    const TbBool ns = (r->heart_x == r->rival_heart_x);
    PlayerNumber pa = PLAYER_NONE, pb = PLAYER_NONE;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        struct WorldKeeperRival kr;
        if (!world_keeper_rival(p, &kr))
            continue;
        if ((kr.heart_x == riv_test_hx[0]) && (kr.heart_y == riv_test_hy[0]))
            pa = p;
        if ((kr.heart_x == riv_test_hx[1]) && (kr.heart_y == riv_test_hy[1]))
            pb = p;
    }
    for (int64_t a = -r->field_half; a <= r->field_half; a++)
        for (int64_t q = -(r->field_width + 2); q <= r->field_width + 2; q++)
        {
            MapSlabCoord ex, ey;
            if (!world_engine_slab(ns ? r->field_x + q : r->field_x + a, ns ? r->field_y + a : r->field_y + q, &ex, &ey))
                continue;
            const struct SlabMap *slb = get_slabmap_block(ex, ey);
            if (slab_kind_has_no_ownership(slb->kind) && (slb->kind != SlbT_PATH))
                continue;
            if (slb->kind == SlbT_PATH)
                (*open)++;
            else if ((pa >= 0) && (slabmap_owner(slb) == pa))
                (*first)++;
            else if ((pb >= 0) && (slabmap_owner(slb) == pb))
                (*second)++;
        }
}

/** Puts n of player 0's creatures on open ground (claimed floor, path or room floor nobody holds or its rival's) round a
 *  world slab, between r1 and r2 slabs out, outside both territories when outside is set. */
static int riv_test_place(int64_t cx, int64_t cy, int r1, int r2, int n, TbBool outside, PlayerNumber on_owner)
{
    static const char *const kinds[4] = {"TROLL", "DEMONSPAWN", "ORC", "DARK_MISTRESS"};
    int made = 0;
    for (int r = r1; (r <= r2) && (made < n); r++)
        for (int k = 0; (k < 8 * r) && (made < n); k += 3)
        {
            const double ang = 6.283185307 * k / (8.0 * r);
            const int64_t wx = cx + (int64_t)lround(r * cos(ang)), wy = cy + (int64_t)lround(r * sin(ang));
            MapSlabCoord ex, ey;
            if (!world_engine_slab(wx, wy, &ex, &ey))
                continue;
            const struct SlabMap *slb = get_slabmap_block(ex, ey);
            const TbBool floor = (slb->kind == SlbT_PATH) || (slb->kind == SlbT_CLAIMED) || (slb->room_index > 0);
            if (!floor || ((on_owner != PLAYER_NONE) && (slabmap_owner(slb) != on_owner)))
                continue;
            if (outside)
            {
                struct WorldKeeperRival kr;
                memset(&kr, 0, sizeof(kr));
                kr.heart_x = riv_test_hx[0];
                kr.heart_y = riv_test_hy[0];
                kr.rival_heart_x = riv_test_hx[1];
                kr.rival_heart_y = riv_test_hy[1];
                kr.field_x = riv_test_ref[0].field_x;
                kr.field_y = riv_test_ref[0].field_y;
                kr.field_half = riv_test_ref[0].field_half;
                kr.field_width = riv_test_ref[0].field_width;
                if (riv_in_territory(&kr, kr.heart_x, kr.heart_y, wx, wy) || riv_in_territory(&kr, kr.rival_heart_x, kr.rival_heart_y, wx, wy) ||
                    riv_on_field(&kr, wx, wy))
                    continue;
            }
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(ex));
            pos.y.val = subtile_coord_center(slab_subtile_center(ey));
            pos.z.val = 0;
            pos.z.val = get_floor_height_at(&pos);
            struct Thing *t = create_creature(&pos, (ThingModel)get_id(creature_desc, kinds[made % 4]), PLAYER0);
            if (!thing_exists(t))
                continue;
            move_thing_in_map(t, &pos);
            set_creature_level(t, 3);
            SYNCLOG("World rivals test: player 0's %s stands at world slab (%ld,%ld), %d slabs from the first heart",
                thing_model_name(t), (long)wx, (long)wy, (int)lround(hypot((double)(wx - riv_test_hx[0]), (double)(wy - riv_test_hy[0]))));
            made++;
        }
    return made;
}

/** Takes player 0's creatures standing within 70 slabs of the pair's middle back to its heart. */
static int riv_test_home(void)
{
    const struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heart))
        return 0;
    int moved = 0;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if ((t->owner != PLAYER0) || thing_is_picked_up(t) || creature_is_dying(t))
            continue;
        int64_t wx, wy;
        riv_thing_world(t, &wx, &wy);
        if (hypot((double)(wx - riv_test_ref[0].field_x), (double)(wy - riv_test_ref[0].field_y)) > 70.0)
            continue;
        struct Coord3d pos = heart->mappos;
        pos.x.val += COORD_PER_STL * 4;
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(t, &pos);
        initialise_thing_state(t, CrSt_CreatureDoingNothing);
        moved++;
    }
    SYNCLOG("World rivals test: %d of player 0's creatures taken home", moved);
    return moved;
}

static void riv_test_turn(void)
{
    static long at = -2, inv1 = -1, inv2 = -1, invn = -1, pass1 = -1, pass2 = -1, unused = -1;
    if (at == -2)
    {
        long b, c;
        riv_test_parse("KFX_WORLD_RIVALTEST", &at, &b, &c);
        riv_test_parse("KFX_WORLD_RIVALINVADE", &inv1, &inv2, &invn);
        riv_test_parse("KFX_WORLD_RIVALPASS", &pass1, &pass2, &unused);
    }
    if ((at < 0) || (world_generator() == NULL))
        return;
    const long now = (long)game.play_gameturn;
    if (now == at)
    {
        static struct WgStructRef refs[256];
        const int n = min(wg_structures_in(world_generator(), -2000, -2000, 2000, 2000, 1u << WgT_Keeper, refs, 256), 256);
        int best = -1;
        for (int i = 0; i < n; i++)
            if ((refs[i].rival == 1) && ((best < 0) || (refs[i].distance < refs[best].distance)))
                best = i;
        if (best < 0)
        {
            SYNCLOG("World rivals test: no rival pair within 2000 slabs of the start");
            return;
        }
        riv_test_ref[0] = refs[best];
        for (int i = 0; i < n; i++)
            if ((refs[i].rival == 2) && (refs[i].id.start_cx == refs[best].rival_id.start_cx) && (refs[i].id.start_cy == refs[best].rival_id.start_cy))
                riv_test_ref[1] = refs[i];
        riv_test_hx[0] = riv_test_ref[0].heart_x;
        riv_test_hy[0] = riv_test_ref[0].heart_y;
        riv_test_hx[1] = riv_test_ref[0].rival_heart_x;
        riv_test_hy[1] = riv_test_ref[0].rival_heart_y;
        riv_test_have = true;
        int loaded = world_load_chunks_round_world(riv_test_hx[0], riv_test_hy[0], 2);
        loaded += world_load_chunks_round_world(riv_test_hx[1], riv_test_hy[1], 2);
        loaded += world_load_chunks_round_world(riv_test_ref[0].field_x, riv_test_ref[0].field_y, 1);
        SYNCLOG("World rivals test: the pair at world slab (%ld,%ld), %s, hearts (%ld,%ld) and (%ld,%ld), %.0f slabs out: %d chunks loaded",
            (long)riv_test_ref[0].field_x, (long)riv_test_ref[0].field_y, wg_field_name(riv_test_ref[0].field_kind),
            (long)riv_test_hx[0], (long)riv_test_hy[0], (long)riv_test_hx[1], (long)riv_test_hy[1], riv_test_ref[0].distance, loaded);
    }
    if (!riv_test_have)
        return;
    if (now == inv1)
    {
        PlayerNumber pa = PLAYER_NONE;
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            struct WorldKeeperRival kr;
            if (world_keeper_rival(p, &kr) && (kr.heart_x == riv_test_hx[0]) && (kr.heart_y == riv_test_hy[0]))
                pa = p;
        }
        const int made = riv_test_place(riv_test_hx[0], riv_test_hy[0], 4, 12, (invn > 0) ? (int)invn : 4, false, pa);
        SYNCLOG("World rivals test: %d of player 0's creatures put in the first keeper's territory (player %d)", made, (int)pa);
    }
    if ((now == inv2) || (now == pass2))
        riv_test_home();
    if (now == pass1)
    {
        const int made = riv_test_place(riv_test_hx[0], riv_test_hy[0], 28, 45, 3, true, PLAYER_NONE);
        SYNCLOG("World rivals test: %d of player 0's creatures put near the pair, outside both territories", made);
    }
    if ((now > at) && (((now - at) % 500) == 0))
    {
        int f1, f2, open;
        riv_test_field_count(&f1, &f2, &open);
        char line[1024] = "";
        for (int s = 0; s < 2; s++)
        {
            PlayerNumber p = PLAYER_NONE;
            for (PlayerNumber q = 0; q < PLAYERS_COUNT; q++)
            {
                struct WorldKeeperRival kr;
                if (world_keeper_rival(q, &kr) && (kr.heart_x == riv_test_hx[s]) && (kr.heart_y == riv_test_hy[s]))
                    p = q;
            }
            if (p == PLAYER_NONE)
            {
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " side %d: not set up;", s + 1);
                continue;
            }
            struct RivArmy a;
            riv_army(p, &a);
            const struct Thing *heart = find_players_dungeon_heart(p);
            const struct Dungeon *d = get_players_num_dungeon(p);
            struct WorldArmy wa;
            world_threat_army(p, &wa);
            const int pi = rside[p].pair;
            /* its room slabs and claimed floor towards its rival (more than 4 slabs from its heart along the hearts' line,
               the battlefield apart) and the rest */
            int rooms_to = 0, rooms_away = 0, floor_to = 0, floor_away = 0;
            {
                const int64_t ux = riv_test_hx[1 - s] - riv_test_hx[s], uy = riv_test_hy[1 - s] - riv_test_hy[s];
                const double len = sqrt((double)(ux * ux + uy * uy));
                struct WorldKeeperRival kr;
                memset(&kr, 0, sizeof(kr));
                kr.heart_x = riv_test_hx[s];
                kr.heart_y = riv_test_hy[s];
                kr.rival_heart_x = riv_test_hx[1 - s];
                kr.rival_heart_y = riv_test_hy[1 - s];
                kr.field_x = riv_test_ref[0].field_x;
                kr.field_y = riv_test_ref[0].field_y;
                kr.field_half = riv_test_ref[0].field_half;
                kr.field_width = riv_test_ref[0].field_width;
                for (int64_t dy = -45; dy <= 45; dy++)
                    for (int64_t dx = -45; dx <= 45; dx++)
                    {
                        MapSlabCoord ex, ey;
                        const int64_t wx = riv_test_hx[s] + dx, wy = riv_test_hy[s] + dy;
                        if (riv_on_field(&kr, wx, wy) || !world_engine_slab(wx, wy, &ex, &ey))
                            continue;
                        const struct SlabMap *slb = get_slabmap_block(ex, ey);
                        if (slabmap_owner(slb) != p)
                            continue;
                        const TbBool to = ((double)(dx * ux + dy * uy) / len > 4.0);
                        if (slb->room_index > 0)
                            *(to ? &rooms_to : &rooms_away) += 1;
                        else if (slb->kind == SlbT_CLAIMED)
                            *(to ? &floor_to : &floor_away) += 1;
                    }
            }
            snprintf(line + strlen(line), sizeof(line) - strlen(line),
                " side %d = player %d: %d creatures (%d Dragons; power %.0f, level %.1f), %d Imps, %ld gold, heart %ld, %d reinforcements, "
                "rooms %d towards its rival / %d away, floor %d / %d;",
                s + 1, (int)p, a.count, (dungeon_invalid(d) || (riv_dragon_model() <= 0)) ? 0 : (int)d->owned_creatures_of_model[riv_dragon_model()], wa.power, (a.count > 0) ? 1.0 + (double)a.level_sum / a.count : 0.0, a.imps,
                dungeon_invalid(d) ? 0L : (long)d->total_money_owned, thing_exists(heart) ? (long)heart->health : -1L,
                (pi >= 0) ? (int)rpairs[pi].reinforcements[s] : 0, rooms_to, rooms_away, floor_to, floor_away);
        }
        int pi = riv_pair(riv_test_ref[0].id.start_cx, riv_test_ref[0].id.start_cy, false);
        SYNCLOG("World rivals test: turn %ld:%s battlefield slabs claimed %d (first) and %d (second), %d open; truce %d, "
            "truces so far %d", now, line, f1, f2, open, (pi >= 0) ? (int)rpairs[pi].truce : 0, (pi >= 0) ? (int)rpairs[pi].truces : 0);
    }
}

/** Test switch KFX_WORLD_RIVALCAMP=T (a played session): at T, beside the middle of the battlefield of the pair nearest
 *  the start (a dirt path away from its side, outside both territories), a 7 x 7 camp of player 0's claimed floor with
 *  four Imps and six creatures of level 4; the pair's ground is revealed to player 0 and this machine's camera goes to
 *  the camp. The pair wakes when the creatures come within a keeper's threat radius, as it would for any player. */
static void riv_test_camp(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_RIVALCAMP");
        at = (v != NULL) ? max(2L, atol(v)) : -1;
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || (world_generator() == NULL))
        return;
    static struct WgStructRef refs[256];
    const int n = min(wg_structures_in(world_generator(), -2000, -2000, 2000, 2000, 1u << WgT_Keeper, refs, 256), 256);
    int best = -1;
    for (int i = 0; i < n; i++)
        if ((refs[i].rival == 1) && ((best < 0) || (refs[i].distance < refs[best].distance)))
            best = i;
    if (best < 0)
    {
        SYNCLOG("World rivals camp: no rival pair within 2000 slabs of the start");
        return;
    }
    const struct WgStructRef *r = &refs[best];
    const TbBool ns = (r->heart_x == r->rival_heart_x);
    const int64_t half = ns ? (r->rival_heart_y - r->heart_y) / 2 : (r->rival_heart_x - r->heart_x) / 2;
    /* the camp's middle: across the line from the battlefield's middle, on the side nearer the start */
    int64_t side = ns ? ((r->field_x > 8) ? -1 : 1) : ((r->field_y > 8) ? -1 : 1);
    const int64_t off = r->field_width + 10;
    const int64_t cx = ns ? r->field_x + side * off : r->field_x, cy = ns ? r->field_y : r->field_y + side * off;
    int loaded = world_load_chunks_round_world(cx, cy, 1);
    loaded += world_load_chunks_round_world(r->heart_x, r->heart_y, 2);
    loaded += world_load_chunks_round_world(r->rival_heart_x, r->rival_heart_y, 2);
    loaded += world_load_chunks_round_world(r->field_x, r->field_y, 1);
    MapSlabCoord ex, ey;
    if (!world_engine_slab(cx, cy, &ex, &ey))
    {
        SYNCLOG("World rivals camp: the camp's ground did not load");
        return;
    }
    /* the camp, and a dirt path 3 wide from it to the battlefield */
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(ex + dx), slab_subtile_center(ey + dy), PLAYER0, 0);
    for (int64_t k = 4; k <= off + 2; k++)
        for (int w = -1; w <= 1; w++)
        {
            const MapSlabCoord px = ns ? ex - (MapSlabCoord)(side * k) : ex + w, py = ns ? ey + w : ey - (MapSlabCoord)(side * k);
            const struct SlabMap *slb = get_slabmap_block(px, py);
            if (slabmap_block_invalid(slb) || (slb->kind == SlbT_PATH) || (slb->kind == SlbT_LAVA) || (slb->kind == SlbT_WATER))
                continue;
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(px), slab_subtile_center(py), game.neutral_player_num, 0);
        }
    /* the pair's ground revealed: both dungeons and the battlefield */
    MapSlabCoord mx, my;
    if (world_engine_slab(r->field_x, r->field_y, &mx, &my))
    {
        const int64_t along = half + 26, across = 28;
        const MapSlabCoord x1 = mx - (MapSlabCoord)(ns ? across : along), x2 = mx + (MapSlabCoord)(ns ? across : along);
        const MapSlabCoord y1 = my - (MapSlabCoord)(ns ? along : across), y2 = my + (MapSlabCoord)(ns ? along : across);
        reveal_map_area(PLAYER0, slab_subtile(x1, 0), slab_subtile(x2, 2), slab_subtile(y1, 0), slab_subtile(y2, 2));
        panel_map_update(slab_subtile(x1, 0), slab_subtile(y1, 0), slab_subtile(x2 - x1 + 1, 0), slab_subtile(y2 - y1 + 1, 0));
    }
    /* four Imps and six creatures on the camp */
    static const char *const kinds[6] = {"TROLL", "DEMONSPAWN", "DARK_MISTRESS", "ORC", "BILE_DEMON", "SORCEROR"};
    int made = 0;
    for (int i = 0; i < 10; i++)
    {
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(ex - 2 + (i % 5)));
        pos.y.val = subtile_coord_center(slab_subtile_center(ey - 1 + 2 * (i / 5)));
        pos.z.val = 0;
        pos.z.val = get_floor_height_at(&pos);
        const ThingModel model = (i < 4) ? get_players_special_digger_model(PLAYER0) : (ThingModel)get_id(creature_desc, kinds[i - 4]);
        struct Thing *t = create_creature(&pos, model, PLAYER0);
        if (!thing_exists(t))
            continue;
        move_thing_in_map(t, &pos);
        if (i >= 4)
            set_creature_level(t, 3);
        made++;
    }
    if (!netstate_dedicated())
        world_local_camera_to(subtile_coord_center(slab_subtile_center(ex)), subtile_coord_center(slab_subtile_center(ey)));
    SYNCLOG("World rivals camp: a camp at world slab (%ld,%ld) beside %s (the pair at (%ld,%ld), hearts %ld slabs apart, %.0f "
        "slabs from the start; the camp %.0f and %.0f slabs from the hearts): %d chunks loaded, %d of player 0's creatures",
        (long)cx, (long)cy, wg_field_name(r->field_kind), (long)r->field_x, (long)r->field_y, (long)(2 * half), r->distance,
        hypot((double)(cx - r->heart_x), (double)(cy - r->heart_y)), hypot((double)(cx - r->rival_heart_x), (double)(cy - r->rival_heart_y)),
        loaded, made);
}

/** Test switch KFX_WORLD_RIVALREVEAL=T (a played session; the user, 2026-11-25: "the map is not revealed"): at T the
 *  ground of the rival pair nearest the start (both dungeons and the battlefield) and the ground between it and the
 *  start are loaded and revealed to player 0, as if seen. Nothing else changes: the pair wakes as it would. */
static void riv_test_reveal(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_RIVALREVEAL");
        at = (v != NULL) ? max(2L, atol(v)) : -1;
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || (world_generator() == NULL))
        return;
    static struct WgStructRef refs[256];
    const int n = min(wg_structures_in(world_generator(), -2000, -2000, 2000, 2000, 1u << WgT_Keeper, refs, 256), 256);
    int best = -1;
    for (int i = 0; i < n; i++)
        if ((refs[i].rival == 1) && ((best < 0) || (refs[i].distance < refs[best].distance)))
            best = i;
    const struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if ((best < 0) || !thing_exists(heart))
    {
        SYNCLOG("World rivals reveal: no rival pair within 2000 slabs of the start, or no heart");
        return;
    }
    const struct WgStructRef *r = &refs[best];
    int64_t sx, sy;
    world_slab_to_world(subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), &sx, &sy);
    /* the box: both hearts 26 slabs round, the battlefield, and the start */
    int64_t x1 = min(r->heart_x, r->rival_heart_x) - 26, x2 = max(r->heart_x, r->rival_heart_x) + 26;
    int64_t y1 = min(r->heart_y, r->rival_heart_y) - 26, y2 = max(r->heart_y, r->rival_heart_y) + 26;
    x1 = min(x1, sx - 10);
    y1 = min(y1, sy - 10);
    x2 = max(x2, sx + 10);
    y2 = max(y2, sy + 10);
    if ((x2 - x1 > 400) || (y2 - y1 > 400))
    {
        SYNCLOG("World rivals reveal: the pair is too far to reveal the way (%ld x %ld slabs)", (long)(x2 - x1), (long)(y2 - y1));
        return;
    }
    int loaded = 0;
    for (int64_t y = y1 + 8; y <= y2 + 8; y += 32)
        for (int64_t x = x1 + 8; x <= x2 + 8; x += 32)
            loaded += world_load_chunks_round_world(x, y, 1);
    MapSlabCoord ex1, ey1, ex2, ey2;
    if (!world_engine_slab(x1, y1, &ex1, &ey1) || !world_engine_slab(x2, y2, &ex2, &ey2))
    {
        SYNCLOG("World rivals reveal: the box did not load (%d chunks loaded)", loaded);
        return;
    }
    reveal_map_area(PLAYER0, slab_subtile(ex1, 0), slab_subtile(ex2, 2), slab_subtile(ey1, 0), slab_subtile(ey2, 2));
    panel_map_update(slab_subtile(ex1, 0), slab_subtile(ey1, 0), slab_subtile(ex2 - ex1 + 1, 0), slab_subtile(ey2 - ey1 + 1, 0));
    MapSlabCoord mx, my;
    if (!netstate_dedicated() && world_engine_slab(r->field_x, r->field_y, &mx, &my))
        world_local_camera_to(subtile_coord_center(slab_subtile_center(mx)), subtile_coord_center(slab_subtile_center(my)));
    SYNCLOG("World rivals reveal: world slabs (%ld,%ld)-(%ld,%ld) revealed to player 0: %s with hearts at (%ld,%ld) and (%ld,%ld), "
        "%d chunks loaded", (long)x1, (long)y1, (long)x2, (long)y2, wg_field_name(r->field_kind), (long)r->heart_x,
        (long)r->heart_y, (long)r->rival_heart_x, (long)r->rival_heart_y, loaded);
}

/******************************************************************************/

void world_rivals_turn(void)
{
    if (!riv_runs())
        return;
    riv_test_reveal();
    riv_test_camp();
    riv_test_turn();
    if ((game.play_gameturn % RIV_LOOK) != 9)
        return;
    /* each pair once, from its first keeper (or from its second when the first is gone) */
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        struct WorldKeeperRival kr;
        if ((p == PLAYER_GOOD) || (p == game.neutral_player_num) || !world_keeper_rival(p, &kr))
            continue;
        if ((kr.which == 2) && (kr.rival != PLAYER_NONE))
            continue;
        riv_look_pair(&kr, p);
    }
}

void world_rivals_reset(void)
{
    memset(rpairs, 0, sizeof(rpairs));
    memset(rside, 0, sizeof(rside));
    memset(rcache, 0, sizeof(rcache));
    /* (the test hooks' pair, riv_test_have, outlives a load: the test goes on after one) */
}

size_t world_rivals_save_size(void)
{
    return 2 + sizeof(rpairs);
}

void world_rivals_save(uint8_t *buf)
{
    buf[0] = RIV_SAVE_VERSION;
    buf[1] = (uint8_t)sizeof(struct RivPair);
    memcpy(buf + 2, rpairs, sizeof(rpairs));
}

TbBool world_rivals_load(const uint8_t *buf, size_t len)
{
    world_rivals_reset();
    if ((len < 2 + sizeof(rpairs)) || (buf[0] != RIV_SAVE_VERSION) || (buf[1] != (uint8_t)sizeof(struct RivPair)))
        return false;
    memcpy(rpairs, buf + 2, sizeof(rpairs));
    return true;
}

#ifdef __cplusplus
}
#endif

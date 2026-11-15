/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pacts.c
 *     Endless world: Hostage Pacts (2026-11-13; see world_pacts.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - Made with in-game commands on a dedicated server: "!pact truce|border|alliance <name>" offers one (for two
 *       minutes), "!pact accept <name>" seals it, "!pact break <name>" breaks it, "!pact" lists yours.
 *     - Within reach: the two Hearts within 240 slabs of each other. Both keepers need a Prison with room, and a
 *       creature of level 10 or higher (not an imp): each side's best goes, chosen by the game.
 *     - Truce: neither side's creatures go for the other's Heart or rooms, nor harm them. Open border: that, and the
 *       two keepers' creatures are allies (they walk past each other). Alliance: that, and each sees what the other
 *       sees (as a co-op party; only for keepers not in a party already).
 *     - Hostages sit in the holder's Prison, fed and healed, and no Hand can take them. Breaking: the breaker's
 *       hostage stays in the other's Prison as an ordinary prisoner (Torture Chamber and all), the other's hostage
 *       walks home to its Heart. A pact also ends (hostages home) when either keeper's Heart is gone.
 *     - A server's own rule on keepers fighting: there is none in the engine; a pact is the players' choice.
 *     Only human keepers make pacts.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pacts.h"
#include "world_stream.h"
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_prisn.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "room_data.h"
#include "server_admin.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "thing_stats.h"
#include "thing_creature.h"
#include "thing_doors.h"
#include "slab_data.h"
#include "power_hand.h"
#include "bflib_planar.h"
#include "config_creature.h"
#include "config_strings.h"
#include "post_inc.h"

/** A creature kind's name as players read it (Troll), else its code name (TROLL). */
static const char *kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *s = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((s != NULL) && (s[0] != '\0')) ? s : creature_code_name(model);
}

#define PACT_MAX 32
#define PACT_REACH 240           /* slabs between the two Hearts */
#define PACT_OFFER_TURNS 2400    /* two minutes */
#define PACT_LEVEL 10            /* a hostage's level at least */

enum PactKind { Pact_None = 0, Pact_Truce, Pact_Border, Pact_Alliance };
static const char *const pact_names[] = {"", "truce", "open border", "alliance"};
static const char *const pact_a[] = {"", "a truce", "an open border", "an alliance"};

struct Pact {
    int16_t a, b;                /* the keepers (a: the one who offered) */
    uint8_t kind;
    uint8_t before;              /* bit 0: the two were allies before the pact (ending it leaves them so) */
    uint8_t hmodel[2];           /* the hostages' kinds (finding one again after a thaw is by kind too) */
    int32_t hostage[2];          /* [0]: a's creature in b's Prison; [1]: b's in a's (thing index, 0: none) */
    int32_t hturn[2];            /* their creation turns (a thing index is reused) */
};
static struct Pact pacts[PACT_MAX];

static struct { int16_t from, to; uint8_t kind; long until; } offers[PACT_MAX];

static int pact_level_needed(void)
{
    static int lv = -1;
    if (lv < 0)
    {
        const char *v = getenv("KFX_WORLD_PACTLEVEL");
        lv = (v != NULL) ? max(1, atoi(v)) : PACT_LEVEL;
    }
    return lv;
}

static struct Pact *pact_between(PlayerNumber a, PlayerNumber b)
{
    for (int i = 0; i < PACT_MAX; i++)
    {
        struct Pact *pc = &pacts[i];
        if ((pc->kind != Pact_None) && (((pc->a == a) && (pc->b == b)) || ((pc->a == b) && (pc->b == a))))
            return pc;
    }
    return NULL;
}

TbBool world_pact_spares(PlayerNumber a, PlayerNumber b)
{
    if (!world_mode_active() || (a == b) || (a < 0) || (b < 0))
        return false;
    return (pact_between(a, b) != NULL);
}

static struct Thing *pact_hostage_thing(const struct Pact *pc, int k)
{
    if (pc->hostage[k] <= 0)
        return INVALID_THING;
    struct Thing *t = thing_get(pc->hostage[k]);
    const PlayerNumber owner = (k == 0) ? pc->a : pc->b;
    if (!thing_exists(t) || !thing_is_creature(t) || (t->owner != owner) || (t->creation_turn != pc->hturn[k]))
        return INVALID_THING;
    return t;
}

TbBool world_pact_hostage(const struct Thing *creatng)
{
    if (!world_mode_active() || !thing_is_creature(creatng))
        return false;
    for (int i = 0; i < PACT_MAX; i++)
    {
        const struct Pact *pc = &pacts[i];
        if (pc->kind == Pact_None)
            continue;
        for (int k = 0; k < 2; k++)
            if ((pc->hostage[k] == creatng->index) && (pc->hturn[k] == creatng->creation_turn))
                return true;
    }
    return false;
}

/******************************************************************************/
/* Talking to the players */

static void pact_tell(PlayerNumber p, const char *text)
{
    if (!netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static PlayerNumber pact_seat_by_name(const char *name)
{
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    for (int i = 0; i < n; i++)
    {
        const char *pn = server_admin_player_name(seats[i]);
        if ((pn != NULL) && (pn[0] != '\0') && (strcasecmp(pn, name) == 0))
            return seats[i];
    }
    return PLAYER_NONE;
}

static const char *pact_name(PlayerNumber p)
{
    const char *n = server_admin_player_name(p);
    return ((n != NULL) && (n[0] != '\0')) ? n : "a keeper";
}

/******************************************************************************/
/* Hostages */

static TbBool pact_hostage_candidate(const struct Thing *t, PlayerNumber owner)
{
    if (!thing_is_creature(t) || (t->owner != owner) || thing_is_picked_up(t) || creature_is_dying(t) ||
        creature_is_being_unconscious(t) || creature_is_kept_in_custody(t) || world_pact_hostage(t) ||
        thing_is_creature_digger(t) || ((t->alloc_flags & TAlF_IsControlled) != 0))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
    return (cctrl->exp_level + 1 >= pact_level_needed());
}

/** The keeper's best creature that may go as a hostage (INVALID_THING: none). */
static struct Thing *pact_pick_hostage(PlayerNumber owner)
{
    struct Thing *best = INVALID_THING;
    int best_lv = -1;
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if (!pact_hostage_candidate(t, owner))
            continue;
        const int lv = creature_control_get_from_thing(t)->exp_level;
        if (lv > best_lv)
        {
            best = t;
            best_lv = lv;
        }
    }
    return best;
}

/** A Prison of the holder's with room for one more (INVALID_ROOM: none). */
static struct Room *pact_prison(PlayerNumber holder)
{
    for (int i = 1; i < ROOMS_COUNT; i++)
    {
        struct Room *room = room_get(i);
        if (room_exists(room) && (room->owner == holder) && room_role_matches(room->kind, RoRoF_Prison) &&
            (room->used_capacity < room->total_capacity))
            return room;
    }
    return INVALID_ROOM;
}

static TbBool pact_in_prison_of(const struct Thing *t, PlayerNumber holder)
{
    const struct Room *room = get_room_thing_is_on(t);
    return room_exists(room) && (room->owner == holder) && room_role_matches(room->kind, RoRoF_Prison) &&
        creature_is_kept_in_prison(t);
}

/** Put a hostage into the holder's Prison. */
static TbBool pact_jail(struct Thing *t, PlayerNumber holder)
{
    struct Room *room = pact_prison(holder);
    struct Coord3d pos;
    if (room_is_invalid(room) || !find_random_valid_position_for_thing_in_room(t, room, &pos))
        return false;
    initialise_thing_state(t, CrSt_CreatureArrivedAtPrison);
    move_thing_in_map(t, &pos);
    return true;
}

/** Send a creature home to its keeper's Heart, free. */
static void pact_send_home(struct Thing *t)
{
    if (!thing_exists(t))
        return;
    struct Thing *heart = get_player_soul_container(t->owner);
    struct CreatureControl *cctrl = creature_control_get_from_thing(t);
    if (thing_is_picked_up(t))
        return;
    if (thing_exists(heart))
    {
        struct Coord3d pos = heart->mappos;
        pos.x.val += 2 * STL_PER_SLB * COORD_PER_STL;
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(t, &pos);
    }
    cctrl->creature_control_flags &= ~CCFlg_NoCompControl;
    set_start_state(t);
}

/******************************************************************************/
/* Making and ending */

static void pact_apply(const struct Pact *pc)
{
    if (pc->kind < Pact_Border)
        return;
    for (int k = 0; k < 2; k++)
    {
        const PlayerNumber p = (k == 0) ? pc->a : pc->b, q = (k == 0) ? pc->b : pc->a;
        if (!player_allied_with(get_player(p), q))
            set_ally_with_player(p, q, true);
        if ((pc->kind == Pact_Alliance) && !is_player_ally_locked(p, q))
            set_player_ally_locked(p, q, true);
    }
}

static void pact_unapply(const struct Pact *pc)
{
    if ((pc->kind < Pact_Border) || (pc->before & 1))
        return; /* (allies before the pact stay allies) */
    for (int k = 0; k < 2; k++)
    {
        const PlayerNumber p = (k == 0) ? pc->a : pc->b, q = (k == 0) ? pc->b : pc->a;
        if (player_invalid(get_player(p)))
            continue;
        set_player_ally_locked(p, q, false);
        set_ally_with_player(p, q, false);
    }
    update_navigation_around_all_doors();
    world_coop_refresh();
}

static const char *pact_check(PlayerNumber a, PlayerNumber b, int kind, TbBool test)
{
    if ((a == b) || !world_player_is_human(a) || !world_player_is_human(b))
        return "a pact is between two keepers who play";
    if (pact_between(a, b) != NULL)
        return "you have a pact with that keeper already";
    struct Thing *ha = get_player_soul_container(a), *hb = get_player_soul_container(b);
    if (!thing_exists(ha) || !thing_exists(hb))
        return "both Dungeon Hearts must stand";
    if (!test)
    {
        const double dx = (double)ha->mappos.x.stl.num - hb->mappos.x.stl.num, dy = (double)ha->mappos.y.stl.num - hb->mappos.y.stl.num;
        if (sqrt(dx * dx + dy * dy) > PACT_REACH * STL_PER_SLB)
            return "your Dungeon Hearts are too far apart for a pact";
    }
    if (world_coop_partner(a) == b)
        return "you are a party already";
    if ((kind == Pact_Alliance) && ((world_coop_ally(a) != PLAYER_NONE) || (world_coop_ally(b) != PLAYER_NONE)))
        return "an alliance is for keepers with no ally yet";
    if (room_is_invalid(pact_prison(a)) || room_is_invalid(pact_prison(b)))
        return "both keepers need a Prison with room for a hostage";
    if (thing_is_invalid(pact_pick_hostage(a)) || thing_is_invalid(pact_pick_hostage(b)))
        return "both keepers need a creature of level 10 or higher to give as a hostage";
    return NULL;
}

static TbBool pact_make(PlayerNumber a, PlayerNumber b, int kind, TbBool test)
{
    const char *why = pact_check(a, b, kind, test);
    if (why != NULL)
    {
        SYNCLOG("World pacts: players %d and %d: no pact (%s)", (int)a, (int)b, why);
        pact_tell(a, why);
        pact_tell(b, why);
        return false;
    }
    struct Pact *pc = NULL;
    for (int i = 0; (i < PACT_MAX) && (pc == NULL); i++)
        if (pacts[i].kind == Pact_None)
            pc = &pacts[i];
    if (pc == NULL)
        return false;
    struct Thing *ta = pact_pick_hostage(a), *tb = pact_pick_hostage(b);
    if (!pact_jail(ta, b) || !pact_jail(tb, a))
        return false;
    memset(pc, 0, sizeof(*pc));
    pc->a = a;
    pc->b = b;
    pc->kind = (uint8_t)kind;
    pc->hostage[0] = ta->index;
    pc->hturn[0] = ta->creation_turn;
    pc->hostage[1] = tb->index;
    pc->hturn[1] = tb->creation_turn;
    pc->hmodel[0] = (uint8_t)ta->model;
    pc->hmodel[1] = (uint8_t)tb->model;
    if (players_are_mutual_allies(a, b))
        pc->before |= 1;
    pact_apply(pc);
    update_navigation_around_all_doors();
    world_coop_refresh();
    SYNCLOG("World pacts: players %d and %d make %s; hostages %s (level %d) and %s (level %d)", (int)a, (int)b,
        pact_a[kind], thing_model_name(ta), creature_control_get_from_thing(ta)->exp_level + 1, thing_model_name(tb),
        creature_control_get_from_thing(tb)->exp_level + 1);
    char msg[200];
    for (int k = 0; k < 2; k++)
    {
        const PlayerNumber p = (k == 0) ? a : b, q = (k == 0) ? b : a;
        const struct Thing *mine = (k == 0) ? ta : tb, *theirs = (k == 0) ? tb : ta;
        snprintf(msg, sizeof(msg), "%s with %s is sealed: your %s sits in their Prison, their %s in yours.",
            pact_a[kind], pact_name(q), kind_name(mine->model), kind_name(theirs->model));
        msg[0] = (char)toupper((unsigned char)msg[0]);
        pact_tell(p, msg);
    }
    return true;
}

/** End a pact. breaker: the keeper who broke it (its hostage stays a prisoner), or PLAYER_NONE (both go home). */
static void pact_end(struct Pact *pc, PlayerNumber breaker, const char *why)
{
    for (int k = 0; k < 2; k++)
    {
        struct Thing *t = pact_hostage_thing(pc, k);
        const PlayerNumber owner = (k == 0) ? pc->a : pc->b;
        if (thing_is_invalid(t) || (owner == breaker))
            continue;
        pact_send_home(t);
    }
    const struct Pact old = *pc;
    memset(pc, 0, sizeof(*pc));
    pact_unapply(&old);
    SYNCLOG("World pacts: the %s of players %d and %d ends (%s)", pact_names[old.kind], (int)old.a, (int)old.b, why);
}

/******************************************************************************/
/* The commands */

TbBool world_pact_command(int user, PlayerNumber me, const char *args)
{
    if (!world_mode_active())
        return false;
    char word[16] = "", name[64] = "";
    while (*args == ' ')
        args++;
    sscanf(args, "%15s %63[^\n]", word, name);
    char msg[220];
    int kind = Pact_None;
    if (strcasecmp(word, "truce") == 0)
        kind = Pact_Truce;
    else if (strcasecmp(word, "border") == 0)
        kind = Pact_Border;
    else if (strcasecmp(word, "alliance") == 0)
        kind = Pact_Alliance;
    if ((me < 0) || (me >= PLAYERS_COUNT))
    {
        server_admin_notice(user, "Only a seated keeper makes pacts.");
        return true;
    }
    if (word[0] == '\0')
    {
        int n = 0;
        for (int i = 0; i < PACT_MAX; i++)
        {
            const struct Pact *pc = &pacts[i];
            if ((pc->kind == Pact_None) || ((pc->a != me) && (pc->b != me)))
                continue;
            snprintf(msg, sizeof(msg), "Your %s with %s.", pact_names[pc->kind], pact_name((pc->a == me) ? pc->b : pc->a));
            server_admin_notice(user, msg);
            n++;
        }
        if (n == 0)
            server_admin_notice(user, "No pacts. !pact truce|border|alliance <name> offers one; hostages of level 10+ seal it.");
        return true;
    }
    const PlayerNumber other = pact_seat_by_name(name);
    if (other == PLAYER_NONE)
    {
        snprintf(msg, sizeof(msg), "No keeper named %.40s here.", name);
        server_admin_notice(user, msg);
        return true;
    }
    if (kind != Pact_None)
    {
        const char *why = pact_check(me, other, kind, false);
        if (why != NULL)
        {
            snprintf(msg, sizeof(msg), "No pact: %s.", why);
            server_admin_notice(user, msg);
            return true;
        }
        int slot = 0;
        for (int i = 0; i < PACT_MAX; i++)
            if ((offers[i].until < (long)game.play_gameturn) || ((offers[i].from == me) && (offers[i].to == other)))
            {
                slot = i;
                break;
            }
        offers[slot].from = (int16_t)me;
        offers[slot].to = (int16_t)other;
        offers[slot].kind = (uint8_t)kind;
        offers[slot].until = (long)game.play_gameturn + PACT_OFFER_TURNS;
        snprintf(msg, sizeof(msg), "You offer %s %s. It stands for two minutes.", pact_name(other), pact_a[kind]);
        server_admin_notice(user, msg);
        snprintf(msg, sizeof(msg), "%s offers you %s, sealed with hostages. Type !pact accept %s to make it.",
            pact_name(me), pact_a[kind], pact_name(me));
        pact_tell(other, msg);
        return true;
    }
    if (strcasecmp(word, "accept") == 0)
    {
        for (int i = 0; i < PACT_MAX; i++)
        {
            if ((offers[i].from != other) || (offers[i].to != me) || (offers[i].until < (long)game.play_gameturn))
                continue;
            offers[i].until = 0;
            if (!pact_make(other, me, offers[i].kind, false))
                server_admin_notice(user, "The pact could not be sealed.");
            return true;
        }
        server_admin_notice(user, "That keeper has offered you no pact (or the offer ran out).");
        return true;
    }
    if (strcasecmp(word, "break") == 0)
    {
        struct Pact *pc = pact_between(me, other);
        if (pc == NULL)
        {
            server_admin_notice(user, "You have no pact with that keeper.");
            return true;
        }
        const int kind_was = pc->kind;
        pact_end(pc, me, "broken");
        snprintf(msg, sizeof(msg), "You broke your %s with %s: your hostage is theirs to keep.", pact_names[kind_was],
            pact_name(other));
        server_admin_notice(user, msg);
        snprintf(msg, sizeof(msg), "%s broke your %s: their hostage is your prisoner now, yours comes home.", pact_name(me),
            pact_names[kind_was]);
        pact_tell(other, msg);
        return true;
    }
    server_admin_notice(user, "!pact truce|border|alliance <name>, !pact accept <name>, !pact break <name>, !pact");
    return true;
}

/******************************************************************************/
/* The turn */

/** Test hook KFX_WORLD_PACTTEST=kind@T (kind 1 truce, 2 border, 3 alliance; on the server): at T the first two human
 *  keepers make that pact (any distance); with ",break@U" the first breaks it at U. */
static void pact_test(void)
{
    static int kind = -1;
    static long at = 0, brk = 0;
    if (kind < 0)
    {
        kind = 0;
        const char *v = getenv("KFX_WORLD_PACTTEST");
        if (v != NULL)
        {
            kind = atoi(v);
            const char *s = strchr(v, '@');
            at = (s != NULL) ? atol(s + 1) : 600;
            s = strstr(v, "break@");
            brk = (s != NULL) ? atol(s + 6) : 0;
        }
    }
    static long cmd = -1;
    if (cmd < 0)
    {
        const char *c = getenv("KFX_WORLD_PACTCMD");
        cmd = (c != NULL) ? atol(c) : 0;
        if ((cmd > 0) && (kind <= 0))
        {
            kind = 3;
            at = cmd;
        }
    }
    if ((kind <= 0) || (((long)game.play_gameturn != at) && ((long)game.play_gameturn != at + 20) &&
        ((long)game.play_gameturn != at + 400) && ((brk <= 0) || (((long)game.play_gameturn != brk) &&
        ((long)game.play_gameturn != brk + 200)))))
        return;
    PlayerNumber two[2] = {PLAYER_NONE, PLAYER_NONE};
    int n = 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (n < 2); p++)
        if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)))
            two[n++] = p;
    if (n < 2)
    {
        SYNCLOG("World pacts test: fewer than two keepers at turn %ld", (long)game.play_gameturn);
        return;
    }
    if ((long)game.play_gameturn == at)
    {
        /* the test gives each a Prison and a creature when it has none */
        for (int k = 0; k < 2; k++)
        {
            const PlayerNumber p = two[k];
            struct Thing *heart = get_player_soul_container(p);
            if (room_is_invalid(pact_prison(p)))
            {
                const RoomKind rk = find_first_roomkind_with_role(RoRoF_Prison);
                const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
                int made = 0;
                for (int dy = -6; (dy <= 6) && (made < 6); dy++)
                    for (int dx = -6; (dx <= 6) && (made < 6); dx++)
                    {
                        const struct SlabMap *slb = get_slabmap_block(hx + dx, hy + dy);
                        if (slabmap_block_invalid(slb) || (slb->kind != SlbT_CLAIMED) || (slabmap_owner(slb) != p))
                            continue;
                        if (!room_is_invalid(place_room(p, rk, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy))))
                            made++;
                    }
                SYNCLOG("World pacts test: player %d gets a Prison of %d slabs", (int)p, made);
            }
            if (thing_is_invalid(pact_pick_hostage(p)))
            {
                struct Coord3d pos = heart->mappos;
                pos.x.val += 3 * STL_PER_SLB * COORD_PER_STL;
                pos.z.val = 0;
                struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, "TROLL"), p);
                if (thing_exists(c))
                    move_thing_in_map(c, &pos);
                SYNCLOG("World pacts test: player %d gets a Troll (%s)", (int)p, thing_exists(c) ? "made" : "failed");
            }
        }
    }
    if (((long)game.play_gameturn == at + 20) && (cmd > 0))
    {
        /* the real path: typed commands through the server's handler (reach and all) */
        char line[96];
        snprintf(line, sizeof(line), "pact alliance %s", pact_name(two[1]));
        SYNCLOG("World pacts test: player %d types \"!%s\"", (int)two[0], line);
        server_admin_player_command(get_player_primary_user(get_player(two[0])), line);
        snprintf(line, sizeof(line), "pact accept %s", pact_name(two[0]));
        SYNCLOG("World pacts test: player %d types \"!%s\"", (int)two[1], line);
        server_admin_player_command(get_player_primary_user(get_player(two[1])), line);
        const struct Thing *ha = get_player_soul_container(two[0]), *hb = get_player_soul_container(two[1]);
        SYNCLOG("World pacts test: the Hearts are %ld slabs apart; a pact now: %d", (long)(get_chessboard_distance(&ha->mappos,
            &hb->mappos) / (STL_PER_SLB * COORD_PER_STL)), (int)(pact_between(two[0], two[1]) != NULL));
    } else if ((long)game.play_gameturn == at + 20)
        pact_make(two[0], two[1], min(kind, (int)Pact_Alliance), true);
    if (((long)game.play_gameturn == at + 400) || ((long)game.play_gameturn == brk))
    {
        const struct Pact *pc = pact_between(two[0], two[1]);
        for (int k = 0; (pc != NULL) && (k < 2); k++)
        {
            const struct Thing *t = pact_hostage_thing(pc, k);
            const PlayerNumber holder = (k == 0) ? pc->b : pc->a;
            SYNCLOG("World pacts test: turn %ld: player %d's hostage %s, in player %d's Prison: %d; pickable by the holder: %d,"
                " slappable: %d", (long)game.play_gameturn, (int)((k == 0) ? pc->a : pc->b), thing_exists(t) ? "found" : "missing",
                (int)holder, thing_exists(t) ? (int)pact_in_prison_of(t, holder) : -1,
                thing_exists(t) ? (int)can_thing_be_picked_up_by_player(t, holder) : -1, thing_exists(t) ? (int)thing_slappable(t, holder) : -1);
        }
    }
    if ((long)game.play_gameturn == brk)
    {
        struct Pact *pc = pact_between(two[0], two[1]);
        if (pc != NULL)
            pact_end(pc, two[0], "broken (test)");
    }
    if ((long)game.play_gameturn == brk + 200)
    {
        /* after the break: player 0's creature stays in player 1's Prison (now pickable); player 1's went home */
        const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
        long guard = 0;
        for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
        {
            struct Thing *t = thing_get(i);
            if (thing_is_invalid(t))
                break;
            i = t->next_of_class;
            if ((t->model != (ThingModel)get_id(creature_desc, "TROLL")) || ((t->owner != two[0]) && (t->owner != two[1])))
                continue;
            const PlayerNumber other = (t->owner == two[0]) ? two[1] : two[0];
            SYNCLOG("World pacts test: after the break: player %d's Troll in player %d's Prison: %d, pickable by them: %d",
                (int)t->owner, (int)other, (int)pact_in_prison_of(t, other), (int)can_thing_be_picked_up_by_player(t, other));
        }
    }
}

/** The hostage of side k: found again when its thing changed (a chunk froze and came back): a creature of its keeper's
 *  kept in the holder's Prison and not a hostage already. */
static struct Thing *pact_find_again(struct Pact *pc, int k)
{
    const PlayerNumber owner = (k == 0) ? pc->a : pc->b, holder = (k == 0) ? pc->b : pc->a;
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if ((t->owner == owner) && ((pc->hmodel[k] == 0) || (t->model == pc->hmodel[k])) && pact_in_prison_of(t, holder) &&
            !world_pact_hostage(t))
        {
            pc->hostage[k] = t->index;
            pc->hturn[k] = t->creation_turn;
            return t;
        }
    }
    return INVALID_THING;
}

void world_pacts_turn(void)
{
    if (!world_mode_active())
        return;
    pact_test();
    if ((game.play_gameturn % 16) != 7)
        return;
    for (int i = 0; i < PACT_MAX; i++)
    {
        struct Pact *pc = &pacts[i];
        if (pc->kind == Pact_None)
            continue;
        if (!thing_exists(get_player_soul_container(pc->a)) || !thing_exists(get_player_soul_container(pc->b)))
        {
            pact_end(pc, PLAYER_NONE, "a Dungeon Heart is gone");
            continue;
        }
        pact_apply(pc);
        for (int k = 0; k < 2; k++)
        {
            const PlayerNumber holder = (k == 0) ? pc->b : pc->a;
            struct Thing *t = pact_hostage_thing(pc, k);
            if (thing_is_invalid(t))
                t = pact_find_again(pc, k);
            if (thing_is_invalid(t))
                continue; /* not on the map now (its chunk is frozen) */
            /* well fed and whole */
            struct CreatureControl *cctrl = creature_control_get_from_thing(t);
            cctrl->hunger_level = 0;
            if (t->health < cctrl->max_health)
                t->health = cctrl->max_health;
            if (!pact_in_prison_of(t, holder) && !thing_is_picked_up(t) && !creature_is_dying(t) &&
                (get_creature_state_besides_interruptions(t) != CrSt_CreatureArrivedAtPrison))
            {
                if (!pact_jail(t, holder))
                {
                    static long told;
                    if ((long)game.play_gameturn - told > 2000)
                    {
                        told = (long)game.play_gameturn;
                        SYNCLOG("World pacts: player %d holds a hostage but has no Prison with room", (int)holder);
                    }
                }
            }
        }
    }
}

/******************************************************************************/
/* The save (the PCT1 extra) */

void world_pacts_player_reset(PlayerNumber p)
{
    for (int i = 0; i < PACT_MAX; i++)
        if ((pacts[i].kind != Pact_None) && ((pacts[i].a == p) || (pacts[i].b == p)))
            pact_end(&pacts[i], PLAYER_NONE, "a keeper has a new dungeon");
    for (int i = 0; i < PACT_MAX; i++)
        if ((offers[i].from == p) || (offers[i].to == p))
            offers[i].until = 0;
}

void world_pacts_reset(void)
{
    memset(pacts, 0, sizeof(pacts));
    memset(offers, 0, sizeof(offers));
}

size_t world_pacts_save_size(void)
{
    return 4 + sizeof(pacts);
}

void world_pacts_save(uint8_t *buf)
{
    const uint32_t n = PACT_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, pacts, sizeof(pacts));
}

TbBool world_pacts_load(const uint8_t *buf, size_t len)
{
    world_pacts_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PACT_MAX) || (len < 4 + sizeof(pacts)))
        return false;
    memcpy(pacts, buf + 4, sizeof(pacts));
    for (int i = 0; i < PACT_MAX; i++)
        if ((pacts[i].kind > Pact_Alliance) || (pacts[i].a < 0) || (pacts[i].b < 0) || (pacts[i].a >= PLAYERS_COUNT) ||
            (pacts[i].b >= PLAYERS_COUNT))
            memset(&pacts[i], 0, sizeof(pacts[i]));
    return true;
}

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_sword.c
 *     Endless world: Take Up the Sword (2026-11-13; see world_sword.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - In-game commands on a dedicated server: the keeper types "!sword <name>" to invite a player, the guest
 *       "!sword accept <name>"; "!sword off" (either) ends it. The invitation stands until then.
 *     - When a raid comes for the keeper, the guest is put into its leader (the Tunneller) in first person, as the
 *       Possess spell does, and stays in it until it dies or the raid ends, then is back at their own dungeon.
 *     - Switching heroes: "!sword next" moves the guest into the next hero of the raid.
 *     - Never while the keeper is away (under the caretaker, or not in the game): no guest is put in then, and one
 *       inside is taken out.
 *     - Cleverer, not stronger: nothing about the hero changes.
 *     - For the defender: the keeper's creatures get twice the experience for hitting the hero a guest drives.
 *     - For the guest: a tally of dungeons breached (a hero they drive comes within 5 slabs of the keeper's Heart,
 *       once per raid), shown by "!sword"; their own dungeon gains nothing.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_sword.h"
#include "world_stream.h"
#include "world_caretaker.h"
#include <string.h>
#include <strings.h>
#include "bflib_basics.h"
#include "bflib_planar.h"
#include "config_creature.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_groups.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "server_admin.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "map_data.h"
#include "thing_factory.h"
#include "creature_states_hero.h"
#include "config_strings.h"
#include "post_inc.h"

/** A creature kind's name as players read it (Troll), else its code name (TROLL). */
static const char *kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *s = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((s != NULL) && (s[0] != '\0')) ? s : creature_code_name(model);
}

struct SwordSeat {
    int16_t guest;               /* the keeper's invited guest (PLAYER_NONE + 1 = 0: none), seat + 1 */
    uint8_t accepted;
    uint8_t breached;            /* this raid counted already */
    int32_t tally;               /* this seat's own tally as a guest: dungeons breached */
    int32_t last_idx, last_turn; /* (the keeper's entry) the hero the guest was last in */
    uint8_t want_next;           /* (the keeper's entry) "!sword next": move into influenced_thing_idx */
    uint8_t pad[3];
};
static struct SwordSeat sw[PLAYERS_MAX];

#define SW_NOTE "SW"
static ThingIndex sw_client_idx;          /* a thin client: the hero the server put this player into */
static GameTurn sw_client_turn;
static TbBool sw_client_pending;          /* not gone in yet: the hero isn't on this machine yet */
static uint8_t sw_unfog[PLAYERS_MAX];     /* the server: guests leading a raid now (their stream is not fogged) */

TbBool world_sword_unfogged(PlayerNumber plyr_idx)
{
    return world_mode_active() && (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && (sw_unfog[plyr_idx] != 0);
}

static PlayerNumber sw_guest(PlayerNumber keeper)
{
    if ((keeper < 0) || (keeper >= PLAYERS_MAX) || (sw[keeper].guest == 0) || !sw[keeper].accepted)
        return PLAYER_NONE;
    return (PlayerNumber)(sw[keeper].guest - 1);
}

static TbBool sw_keeper_here(PlayerNumber keeper)
{
    return world_player_is_human(keeper) && !world_caretaker_active(keeper) &&
        thing_exists(get_player_soul_container(keeper));
}

static TbBool sw_raid_hero_for(const struct Thing *t, PlayerNumber keeper)
{
    if (!thing_is_creature(t) || (t->owner != PLAYER_GOOD) || creature_is_dying(t) || creature_is_being_unconscious(t) ||
        creature_is_kept_in_custody(t))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
    return (cctrl->party.original_objective == CHeroTsk_AttackEnemies) && (cctrl->party.target_plyr_idx == keeper);
}

TbBool world_sword_may_drive(NetUserId user, const struct Thing *thing)
{
    if (!world_mode_active() || !thing_is_creature(thing) || (thing->owner != PLAYER_GOOD))
        return false;
    if (netstate_is_client())
        return (sw_client_idx != 0) && (thing->index == sw_client_idx) && (thing->creation_turn == sw_client_turn);
    const struct PlayerInfo *player = get_user_player(user);
    if (player_invalid(player))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const PlayerNumber keeper = cctrl->party.target_plyr_idx;
    return (sw_guest(keeper) == player->id_number) && (cctrl->party.original_objective == CHeroTsk_AttackEnemies);
}

static NetUserId sw_user(PlayerNumber p)
{
    return get_player_primary_user(get_player(p));
}

static struct Thing *sw_driving(PlayerNumber guest)
{
    const NetUserId u = sw_user(guest);
    if ((u < 0) || (u >= MAX_NET_USERS))
        return INVALID_THING;
    const struct UserState *ustate = get_user_state(u);
    if (ustate->view_type != PVT_CreatureContrl)
        return INVALID_THING;
    struct Thing *t = thing_get(ustate->controlled_thing_idx);
    if (!thing_exists(t) || (t->owner != PLAYER_GOOD))
        return INVALID_THING;
    return t;
}

TbBool world_sword_driven(const struct Thing *thing)
{
    if (!world_mode_active() || !thing_is_creature(thing) || (thing->owner != PLAYER_GOOD))
        return false;
    return ((thing->alloc_flags & TAlF_IsControlled) != 0);
}

static void sw_tell(PlayerNumber p, const char *text)
{
    if (!netstate_is_server())
        return;
    const NetUserId u = sw_user(p);
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static const char *sw_name(PlayerNumber p)
{
    const char *n = server_admin_player_name(p);
    return ((n != NULL) && (n[0] != '\0')) ? n : "a keeper";
}

static PlayerNumber sw_seat_by_name(const char *name)
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

/** Put the guest into a hero (as the Possess spell does). */
static TbBool sw_drive(PlayerNumber guest, struct Thing *hero)
{
    const NetUserId u = sw_user(guest);
    if ((u < 0) || (u >= MAX_NET_USERS) || !thing_exists(hero))
        return false;
    struct UserState *ustate = get_user_state(u);
    if (ustate->view_type != PVT_DungeonTop)
        return false; /* busy: in a creature of their own, or the map */
    ustate->influenced_thing_idx = hero->index;
    ustate->influenced_thing_creation = hero->creation_turn;
    ustate->first_person_dig_claim_mode = false;
    ustate->teleport_destination = 19;
    ustate->battleid = 1;
    set_user_instance(u, PI_DirctCtrl, 0);
    sw_unfog[guest] = 1;
    {
        /* what the hero sees is revealed to the guest's keeper (the stream sends a player only revealed ground and
           the things on it): the hero itself, and its first view, reach the guest's machine */
        const MapSubtlCoord hx = hero->mappos.x.stl.num, hy = hero->mappos.y.stl.num;
        const MapSubtlCoord r = 5 * STL_PER_SLB;
        reveal_map_area(guest, max(0, hx - r), min((int)game.map_subtiles_x, hx + r), max(0, hy - r),
            min((int)game.map_subtiles_y, hy + r));
    }
    ustate->dungeon_camera.x = hero->mappos.x.val;
    ustate->dungeon_camera.y = hero->mappos.y.val;
    if (netstate_is_server() && (u > SERVER_ID))
    {
        char note[48];
        snprintf(note, sizeof(note), SW_NOTE "D %d %ld", (int)hero->index, (long)hero->creation_turn);
        server_admin_notice(u, note); /* its machine goes in too (a thin client runs the view) */
    }
    SYNCLOG("World sword: player %d is put into the raid's %s (level %d)", (int)guest, thing_model_name(hero),
        creature_control_get_from_thing(hero)->exp_level + 1);
    return true;
}

static void sw_leave(PlayerNumber guest)
{
    struct Thing *t = sw_driving(guest);
    if (thing_is_invalid(t))
        return;
    leave_creature_as_controller(sw_user(guest), t);
    if (netstate_is_server() && (sw_user(guest) > SERVER_ID))
        server_admin_notice(sw_user(guest), SW_NOTE "L");
    SYNCLOG("World sword: player %d leaves the raid's %s", (int)guest, thing_model_name(t));
}

/** The raid's hero to drive: its leader first, else the next after `after` (any hero of the raid). */
static struct Thing *sw_pick(PlayerNumber keeper, ThingIndex after)
{
    struct Thing *first = INVALID_THING, *next = INVALID_THING, *leader = INVALID_THING;
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if (!sw_raid_hero_for(t, keeper) || ((t->alloc_flags & TAlF_IsControlled) != 0))
            continue;
        if (thing_is_invalid(first))
            first = t;
        if ((after > 0) && (t->index > after) && thing_is_invalid(next))
            next = t;
        if (thing_is_invalid(leader) && (get_group_leader(t) == t))
            leader = t;
    }
    if (after > 0)
        return thing_is_invalid(next) ? first : next;
    return thing_is_invalid(leader) ? first : leader;
}

TbBool world_sword_net_notice(const char *text)
{
    if ((text == NULL) || (strncmp(text, SW_NOTE, 3) != 0))
        return false;
    const NetUserId me = get_my_user();
    struct UserState *ustate = get_user_state(me);
    if (text[3] == 'D')
    {
        int idx = 0;
        long turn = 0;
        if (sscanf(text + 4, "%d %ld", &idx, &turn) == 2)
        {
            sw_client_idx = (ThingIndex)idx;
            sw_client_turn = (GameTurn)turn;
            {
                const struct Thing *h = thing_get((ThingIndex)idx);
                SYNCLOG("World sword: this machine goes into hero %d (here: %s, creation %ld vs %ld, view %d)", idx,
                    thing_exists(h) ? thing_model_name(h) : "missing", thing_exists(h) ? (long)h->creation_turn : -1L, turn,
                    (int)get_user_view_type(me));
            }
            sw_client_pending = true;
            world_sword_local_turn();
        }
    } else if (text[3] == 'L')
    {
        struct Thing *t = thing_get(ustate->controlled_thing_idx);
        if ((get_user_view_type(me) == PVT_CreatureContrl) && thing_exists(t) && (t->owner == PLAYER_GOOD))
            leave_creature_as_controller(me, t);
        sw_client_idx = 0;
        sw_client_pending = false;
    }
    return true;
}

void world_sword_local_turn(void)
{
    if (!sw_client_pending || !netstate_is_client())
        return;
    const NetUserId me = get_my_user();
    const struct Thing *h = thing_get(sw_client_idx);
    if (!thing_exists(h) || (h->creation_turn != sw_client_turn) || (get_user_view_type(me) != PVT_DungeonTop))
        return;
    struct UserState *ustate = get_user_state(me);
    ustate->influenced_thing_idx = sw_client_idx;
    ustate->influenced_thing_creation = sw_client_turn;
    ustate->first_person_dig_claim_mode = false;
    set_user_instance(me, PI_DirctCtrl, 0);
    sw_client_pending = false;
    SYNCLOG("World sword: this machine goes into the %s", thing_model_name(h));
}

TbBool world_sword_command(int user, PlayerNumber me, const char *args)
{
    if (!world_mode_active())
        return false;
    char word[64] = "", rest[64] = "";
    while (*args == ' ')
        args++;
    sscanf(args, "%63s %63[^\n]", word, rest);
    char msg[220];
    if ((me < 0) || (me >= PLAYERS_COUNT))
    {
        server_admin_notice(user, "Only a seated keeper.");
        return true;
    }
    if (word[0] == '\0')
    {
        const PlayerNumber g = (sw[me].guest > 0) ? (PlayerNumber)(sw[me].guest - 1) : PLAYER_NONE;
        if (g != PLAYER_NONE)
            snprintf(msg, sizeof(msg), "%s leads the raids on your dungeon%s.", sw_name(g), sw[me].accepted ? "" : " (not accepted yet)");
        else
            snprintf(msg, sizeof(msg), "No guest leads your raids. !sword <name> invites one.");
        server_admin_notice(user, msg);
        snprintf(msg, sizeof(msg), "Dungeons you breached as a guest: %ld.", (long)sw[me].tally);
        server_admin_notice(user, msg);
        return true;
    }
    if (strcasecmp(word, "accept") == 0)
    {
        const PlayerNumber k = sw_seat_by_name(rest);
        if ((k == PLAYER_NONE) || (sw[k].guest != me + 1))
        {
            server_admin_notice(user, "That keeper has not invited you.");
            return true;
        }
        sw[k].accepted = 1;
        snprintf(msg, sizeof(msg), "You will lead the raids on %s's dungeon, from the next that comes.", sw_name(k));
        server_admin_notice(user, msg);
        snprintf(msg, sizeof(msg), "%s accepts: they lead the raids on your dungeon now.", sw_name(me));
        sw_tell(k, msg);
        return true;
    }
    if (strcasecmp(word, "off") == 0)
    {
        for (PlayerNumber k = 0; k < PLAYERS_COUNT; k++)
            if ((k == me) || (sw[k].guest == me + 1))
            {
                const PlayerNumber g = sw_guest(k);
                if (g != PLAYER_NONE)
                {
                    sw_leave(g);
                    sw_unfog[g] = 0; /* its stream is fogged again */
                }
                if (sw[k].guest > 0)
                    sw_unfog[sw[k].guest - 1] = 0;
                if (sw[k].guest > 0)
                {
                    snprintf(msg, sizeof(msg), "No more guest raids on %s's dungeon.", sw_name(k));
                    sw_tell(k, msg);
                    sw_tell((PlayerNumber)(sw[k].guest - 1), msg);
                }
                sw[k].guest = 0;
                sw[k].accepted = 0;
            }
        return true;
    }
    if (strcasecmp(word, "next") == 0)
    {
        struct Thing *now = sw_driving(me);
        if (thing_is_invalid(now))
        {
            server_admin_notice(user, "You are not in a raid's hero.");
            return true;
        }
        const PlayerNumber keeper = creature_control_get_from_thing(now)->party.target_plyr_idx;
        struct Thing *to = sw_pick(keeper, now->index);
        if (thing_is_invalid(to) || (to == now))
        {
            server_admin_notice(user, "No other hero of this raid to move into.");
            return true;
        }
        sw_leave(me);
        sw[keeper].want_next = 1;
        /* the move into the next one happens when the view is back (the turn, below) */
        struct UserState *ustate = get_user_state(sw_user(me));
        ustate->influenced_thing_idx = to->index;
        ustate->influenced_thing_creation = to->creation_turn;
        return true;
    }
    const PlayerNumber g = sw_seat_by_name(word);
    if ((g == PLAYER_NONE) || (g == me) || !world_player_is_human(g))
    {
        snprintf(msg, sizeof(msg), "No player named %.40s here. !sword <name> invites one to lead your raids.", word);
        server_admin_notice(user, msg);
        return true;
    }
    sw[me].guest = (int16_t)(g + 1);
    sw[me].accepted = 0;
    snprintf(msg, sizeof(msg), "You invite %s to lead the raids on your dungeon.", sw_name(g));
    server_admin_notice(user, msg);
    snprintf(msg, sizeof(msg), "%s invites you to lead the raids on their dungeon, in the heroes' shoes. !sword accept %s",
        sw_name(me), sw_name(me));
    sw_tell(g, msg);
    return true;
}

/** Test hook KFX_WORLD_SWORDTEST=T (server): at T the first human keeper invites the second, who accepts, and a raid
 *  of three heroes (a Tunneller leading) comes for the first, 12 slabs east of its Heart. */
static void sw_test(void)
{
    static long at = -1;
    static int cmd = 0;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_SWORDTEST");
        at = (v != NULL) ? atol(v) : 0;
        const char *c = getenv("KFX_WORLD_SWORDCMD");
        if ((c != NULL) && (atol(c) > 0))
        {
            at = atol(c);
            cmd = 1;
        }
    }
    if (cmd && ((long)game.play_gameturn == at + 500))
    {
        /* the guest gives it up: back to its own view (where its camera now is) */
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
            if (!thing_is_invalid(sw_driving(p)))
            {
                SYNCLOG("World sword test: player %d types \"!sword off\"", (int)p);
                server_admin_player_command(sw_user(p), "sword off");
            }
        return;
    }
    if (cmd && ((long)game.play_gameturn == at + 300))
    {
        /* the guest moves to the next hero */
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        {
            const struct Thing *now = sw_driving(p);
            if (thing_is_invalid(now))
                continue;
            SYNCLOG("World sword test: player %d (in the %s) types \"!sword next\"", (int)p, thing_model_name(now));
            server_admin_player_command(sw_user(p), "sword next");
        }
        return;
    }
    if ((at <= 0) || ((long)game.play_gameturn != at))
        return;
    PlayerNumber two[2] = {PLAYER_NONE, PLAYER_NONE};
    int n = 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (n < 2); p++)
        if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)))
            two[n++] = p;
    if (n < 2)
    {
        SYNCLOG("World sword test: fewer than two keepers");
        return;
    }
    if (cmd)
    {
        /* the real path: typed commands through the server's handler */
        char line[96];
        snprintf(line, sizeof(line), "sword %s", sw_name(two[1]));
        SYNCLOG("World sword test: player %d types \"!%s\"", (int)two[0], line);
        server_admin_player_command(sw_user(two[0]), line);
        snprintf(line, sizeof(line), "sword accept %s", sw_name(two[0]));
        SYNCLOG("World sword test: player %d types \"!%s\"", (int)two[1], line);
        server_admin_player_command(sw_user(two[1]), line);
        SYNCLOG("World sword test: invited %d, accepted %d", (int)(sw[two[0]].guest == two[1] + 1), (int)sw[two[0]].accepted);
    } else
    {
        sw[two[0]].guest = (int16_t)(two[1] + 1);
        sw[two[0]].accepted = 1;
    }
    struct Thing *heart = get_player_soul_container(two[0]);
    struct Thing *leader = INVALID_THING;
    static const char *const kinds[] = {"TUNNELLER", "KNIGHT", "ARCHER"};
    for (int k = 0; k < 3; k++)
    {
        struct Coord3d pos = heart->mappos;
        pos.x.val += 3 * STL_PER_SLB * COORD_PER_STL;
        pos.y.val += (k - 1) * STL_PER_SLB * COORD_PER_STL; /* inside its dungeon */
        pos.z.val = 0;
        struct Thing *t = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature,
            (ThingModel)get_id(creature_desc, kinds[k]), PLAYER_GOOD);
        if (thing_is_invalid(t))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        cctrl->party.original_objective = CHeroTsk_AttackEnemies;
        cctrl->party.objective = CHeroTsk_AttackEnemies;
        cctrl->party.target_plyr_idx = two[0];
        if (thing_is_invalid(leader))
            leader = t;
        else
            add_creature_to_group(t, leader);
    }
    SYNCLOG("World sword test: player %d invites player %d; a raid of three comes for player %d", (int)two[0], (int)two[1],
        (int)two[0]);
}

void world_sword_turn(void)
{
    if (!world_mode_active())
        return;
    sw_test();
    if ((game.play_gameturn % 8) != 3)
        return;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!sw_unfog[p])
            continue;
        TbBool guest = false;
        for (PlayerNumber k = 0; (k < PLAYERS_COUNT) && !guest; k++)
            guest = (sw_guest(k) == p);
        if (!guest)
            sw_unfog[p] = 0; /* no longer a guest: its stream is fogged again */
    }
    for (PlayerNumber k = 0; k < PLAYERS_COUNT; k++)
    {
        const PlayerNumber g = sw_guest(k);
        if (g == PLAYER_NONE)
            continue;
        struct Thing *now = sw_driving(g);
        if (!sw_keeper_here(k) || !world_player_is_human(g))
        {
            sw_unfog[g] = 0;
            if (!thing_is_invalid(now))
            {
                sw_leave(g);
                sw_tell(g, "The keeper is away: you are taken out of the raid.");
            }
            continue;
        }
        if (!thing_is_invalid(now))
        {
            struct UserState *gst = get_user_state(sw_user(g));
            gst->dungeon_camera.x = now->mappos.x.val; /* what the guest's machine is sent follows the hero */
            gst->dungeon_camera.y = now->mappos.y.val;
            /* the guest sees what the hero sees: the ground round it is revealed to the guest's keeper (its own
               keeper never saw it; a first-person view of unrevealed ground is black) */
            const MapSubtlCoord hx = now->mappos.x.stl.num, hy = now->mappos.y.stl.num;
            const MapSubtlCoord r = 5 * STL_PER_SLB;
            reveal_map_area(g, max(0, hx - r), min((int)game.map_subtiles_x, hx + r), max(0, hy - r),
                min((int)game.map_subtiles_y, hy + r));
            /* breached: within 5 slabs of the Heart, once a raid */
            struct Thing *heart = get_player_soul_container(k);
            if (!sw[k].breached && (get_chessboard_distance(&now->mappos, &heart->mappos) <= 5 * STL_PER_SLB * COORD_PER_STL))
            {
                sw[k].breached = 1;
                sw[g].tally++;
                char msg[160];
                snprintf(msg, sizeof(msg), "You breached %s's dungeon. Dungeons breached: %ld.", sw_name(k), (long)sw[g].tally);
                sw_tell(g, msg);
                SYNCLOG("World sword: player %d breaches player %d's dungeon (tally %ld)", (int)g, (int)k, (long)sw[g].tally);
            }
            continue;
        }
        /* not inside: the next hero (after "!sword next"), else the raid's leader */
        const NetUserId u = sw_user(g);
        if ((u < 0) || (u >= MAX_NET_USERS))
            continue;
        struct UserState *ustate = get_user_state(u);
        if (ustate->view_type != PVT_DungeonTop)
            continue;
        if (!sw[k].want_next && (sw[k].last_idx > 0))
        {
            /* left a hero that still stands, of their own accord: not put back while that hero lives */
            const struct Thing *last = thing_get(sw[k].last_idx);
            if (thing_exists(last) && (last->creation_turn == sw[k].last_turn) && sw_raid_hero_for(last, k))
                continue;
        }
        struct Thing *want = thing_get(ustate->influenced_thing_idx);
        if (!sw[k].want_next || !thing_exists(want) || (want->creation_turn != ustate->influenced_thing_creation) ||
            !sw_raid_hero_for(want, k))
            want = sw_pick(k, 0);
        sw[k].want_next = 0;
        if (thing_is_invalid(want))
        {
            sw[k].breached = 0; /* no raid now: the next one counts again */
            sw[k].last_idx = 0;
            sw_unfog[g] = 0;
            continue;
        }
        if (sw_drive(g, want))
        {
            sw[k].last_idx = want->index;
            sw[k].last_turn = want->creation_turn;
            char msg[160];
            snprintf(msg, sizeof(msg), "You lead the raid on %s's dungeon in the %s's shoes. !sword next moves you to another hero.",
                sw_name(k), kind_name(want->model));
            sw_tell(g, msg);
        }
    }
}

/******************************************************************************/
/* The save (the SWD1 extra) */

void world_sword_player_reset(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    if (!thing_is_invalid(sw_driving(p)))
        sw_leave(p);
    sw_unfog[p] = 0;
    for (PlayerNumber k = 0; k < PLAYERS_MAX; k++)
        if ((k == p) || (sw[k].guest == p + 1))
        {
            sw[k].guest = 0;
            sw[k].accepted = 0;
            sw[k].last_idx = 0;
            sw[k].want_next = 0;
        }
    SYNCLOG("World sword: player %d has a new dungeon: its guest invitations end", (int)p);
}

void world_sword_reset(void)
{
    memset(sw, 0, sizeof(sw));
    memset(sw_unfog, 0, sizeof(sw_unfog));
}

size_t world_sword_save_size(void)
{
    return 4 + sizeof(sw);
}

void world_sword_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, sw, sizeof(sw));
}

TbBool world_sword_load(const uint8_t *buf, size_t len)
{
    world_sword_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (len < 4 + sizeof(sw)))
        return false;
    memcpy(sw, buf + 4, sizeof(sw));
    for (int i = 0; i < PLAYERS_MAX; i++)
        if ((sw[i].guest < 0) || (sw[i].guest > PLAYERS_COUNT))
            sw[i].guest = 0;
    return true;
}

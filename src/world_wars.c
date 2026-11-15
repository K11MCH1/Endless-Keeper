/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wars.c
 *     Endless world: Portal Wars (2026-11-13; see world_wars.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - Made with in-game commands on a dedicated server: "!war <name> <gold>" challenges (the offer stands two
 *       minutes), "!war accept <name>" or "!war refuse <name>" answers, "!war" says how your wars stand.
 *     - The stake: gold, the same sum from both, taken when the war is accepted; the winner gets both. Books and named
 *       prisoners are not built yet.
 *     - The hour: the first dusk after a whole day has passed since the answer (the day to prepare). The defender's
 *       choice of hour is that dusk (there is no other hour to choose yet).
 *     - The attack: up to twelve of the attacker's best creatures (not imps) whose level is at most two above the
 *       defender's best step out of the defender's portal. One that reaches the defender's Heart takes its flame;
 *       if the flame's carrier stands in that portal again, the attacker wins. A carrier who falls drops it: it goes
 *       back to the Heart. All fallen, or dawn: the defender wins.
 *     - Nothing else is lost: a fallen attacker is sent home hurt, never killed or taken; nobody harms the defender's
 *       Heart while it lasts. The defender's creatures fall as in any fight.
 *     - No dungeon can be challenged in its first seven days.
 *     - Absent at the hour (under the caretaker, or not in the game): the war is called off and both stakes go back.
 *       So is it when the defender has no portal then.
 *     Only human keepers.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_wars.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_caretaker.h"
#include "world_wild.h"
#include <string.h>
#include <strings.h>
#include <math.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "room_data.h"
#include "server_admin.h"
#include "slab_data.h"
#include "map_blocks.h"
#include "bflib_planar.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "thing_stats.h"
#include "thing_objects.h"
#include "creature_states_combt.h"
#include "config_strings.h"
#include "post_inc.h"

/** A creature kind's name as players read it (Troll), else its code name (TROLL). */
static const char *kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *s = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((s != NULL) && (s[0] != '\0')) ? s : creature_code_name(model);
}

#define WAR_MAX 8
#define WAR_PARTY 12
#define WAR_OFFER_TURNS 2400
#define WAR_GRACE_DAYS 7

enum WarState { War_None = 0, War_Offered, War_Waiting, War_On };

struct War {
    int16_t att, def;
    uint8_t state;
    uint8_t saw_day;             /* a day began since the answer */
    uint8_t was_day;             /* War_On: it was day at the last look (dawn ends it) */
    uint8_t count;
    int32_t stake;
    int32_t until;               /* War_Offered: the offer's last turn */
    int32_t carrier;             /* thing index of the flame's carrier (0: in the Heart) */
    int32_t carrier_turn;
    int32_t party[WAR_PARTY];    /* thing index and creation turn of each attacker still in it */
    int32_t pturn[WAR_PARTY];
};
static struct War wars[WAR_MAX];

static TbBool war_test_on(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("KFX_WORLD_WARTEST") != NULL) ? 1 : 0;
    return (on == 1);
}

/******************************************************************************/
/* Talking */

static void war_tell(PlayerNumber p, const char *text)
{
    if (!netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static const char *war_name(PlayerNumber p)
{
    const char *n = server_admin_player_name(p);
    return ((n != NULL) && (n[0] != '\0')) ? n : "a keeper";
}

static PlayerNumber war_seat_by_name(const char *name)
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

/******************************************************************************/
/* The party */

static struct Thing *war_member(const struct War *w, int k)
{
    if (w->party[k] <= 0)
        return INVALID_THING;
    struct Thing *t = thing_get(w->party[k]);
    if (!thing_exists(t) || !thing_is_creature(t) || (t->owner != w->att) || (t->creation_turn != w->pturn[k]))
        return INVALID_THING;
    return t;
}

static struct War *war_of_member(const struct Thing *t, int *slot)
{
    for (int i = 0; i < WAR_MAX; i++)
    {
        struct War *w = &wars[i];
        if ((w->state != War_On) || (w->att != t->owner))
            continue;
        for (int k = 0; k < WAR_PARTY; k++)
            if ((w->party[k] == t->index) && (w->pturn[k] == t->creation_turn))
            {
                if (slot != NULL)
                    *slot = k;
                return w;
            }
    }
    return NULL;
}

static void war_home(struct Thing *t, TbBool hurt)
{
    struct Thing *heart = get_player_soul_container(t->owner);
    if (!thing_exists(heart) || thing_is_picked_up(t))
        return;
    struct Coord3d pos = heart->mappos;
    pos.x.val += 2 * STL_PER_SLB * COORD_PER_STL;
    pos.z.val = get_floor_height_at(&pos);
    move_thing_in_map(t, &pos);
    if (hurt)
        t->health = max(1, creature_control_get_from_thing(t)->max_health / 4);
    set_start_state(t);
}

static struct Room *war_portal(PlayerNumber def)
{
    return pick_random_room_of_role(def, RoRoF_CrPoolSpawn);
}

/******************************************************************************/
/* Beginning and ending */

static const char *war_check(PlayerNumber att, PlayerNumber def, long stake)
{
    if ((att == def) || !world_player_is_human(att) || !world_player_is_human(def))
        return "a war is between two keepers who play";
    if (!thing_exists(get_player_soul_container(att)) || !thing_exists(get_player_soul_container(def)))
        return "both Dungeon Hearts must stand";
    if (!war_test_on() && (world_seat_day(def) <= WAR_GRACE_DAYS))
        return "no dungeon can be challenged in its first seven days";
    for (int i = 0; i < WAR_MAX; i++)
        if ((wars[i].state >= War_Waiting) && ((wars[i].att == att) || (wars[i].def == att) || (wars[i].att == def) ||
            (wars[i].def == def)))
            return "one of you has a war arranged already";
    if (stake <= 0)
        return "the stake is a sum of gold";
    if ((get_dungeon(att)->total_money_owned < stake) || (get_dungeon(def)->total_money_owned < stake))
        return "both keepers must have the stake in gold";
    if (room_is_invalid(war_portal(def)))
        return "the defender needs a portal";
    return NULL;
}

static void war_finish(struct War *w, PlayerNumber winner, const char *why)
{
    for (int k = 0; k < WAR_PARTY; k++)
    {
        struct Thing *t = war_member(w, k);
        if (!thing_is_invalid(t))
            war_home(t, false);
    }
    char msg[200];
    if (winner == PLAYER_NONE)
    {
        player_add_offmap_gold(w->att, w->stake);
        player_add_offmap_gold(w->def, w->stake);
        snprintf(msg, sizeof(msg), "The war of %s on %s is called off (%s): both stakes go back.", war_name(w->att),
            war_name(w->def), why);
    } else
    {
        player_add_offmap_gold(winner, 2 * (GoldAmount)w->stake);
        snprintf(msg, sizeof(msg), "%s wins the war (%s) and takes %ld gold.", war_name(winner), why, 2L * w->stake);
    }
    war_tell(w->att, msg);
    war_tell(w->def, msg);
    SYNCLOG("World wars: player %d on player %d ends: %s (%s)", (int)w->att, (int)w->def,
        (winner == PLAYER_NONE) ? "called off" : ((winner == w->att) ? "the attacker wins" : "the defender wins"), why);
    memset(w, 0, sizeof(*w));
    memset(game.world_war_flame, 0, sizeof(game.world_war_flame));
    for (int i = 0; i < WAR_MAX; i++)
        if ((wars[i].state == War_On) && (wars[i].carrier > 0) && (i < (int)(sizeof(game.world_war_flame) / sizeof(game.world_war_flame[0]))))
            game.world_war_flame[i] = (uint16_t)wars[i].carrier;
}

static void war_begin(struct War *w)
{
    if (world_caretaker_active(w->att) || world_caretaker_active(w->def) || !world_player_is_human(w->att) ||
        !world_player_is_human(w->def))
    {
        war_finish(w, PLAYER_NONE, "a keeper was not there at the hour");
        return;
    }
    struct Room *portal = war_portal(w->def);
    if (room_is_invalid(portal))
    {
        war_finish(w, PLAYER_NONE, "the defender has no portal");
        return;
    }
    if (!war_test_on() && (world_seat_day(w->def) <= WAR_GRACE_DAYS))
    {
        war_finish(w, PLAYER_NONE, "the defender's dungeon is in its first seven days");
        return;
    }
    const int cap = max(1, world_keeper_top_level(w->def)) + 2;
    /* the attacker's best, at most two levels above the defender's best */
    int n = 0;
    for (int pass = 0; (pass < WAR_PARTY) && (n < WAR_PARTY); pass++)
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
            if ((t->owner != w->att) || thing_is_creature_digger(t) || thing_is_picked_up(t) || creature_is_dying(t) ||
                creature_is_being_unconscious(t) || creature_is_kept_in_custody(t) || ((t->alloc_flags & TAlF_IsControlled) != 0))
                continue;
            const int lv = creature_control_get_from_thing(t)->exp_level + 1;
            if ((lv > cap) || (lv <= best_lv) || (war_of_member(t, NULL) != NULL))
                continue;
            TbBool taken = false;
            for (int k = 0; k < n; k++)
                if (w->party[k] == t->index)
                    taken = true;
            if (taken)
                continue;
            best = t;
            best_lv = lv;
        }
        if (thing_is_invalid(best))
            break;
        w->party[n] = best->index;
        w->pturn[n] = best->creation_turn;
        n++;
    }
    if (n == 0)
    {
        war_finish(w, w->def, "the attacker had no creature to send");
        return;
    }
    w->count = (uint8_t)n;
    w->state = War_On;
    w->was_day = world_is_day() ? 1 : 0;
    w->carrier = 0;
    for (int k = 0; k < n; k++)
    {
        struct Thing *t = thing_get(w->party[k]);
        struct Coord3d pos;
        if (!find_random_valid_position_for_thing_in_room(t, portal, &pos))
            continue;
        initialise_thing_state(t, CrSt_CreatureDoingNothing);
        move_thing_in_map(t, &pos);
    }
    char msg[200];
    snprintf(msg, sizeof(msg), "War: %d of %s's creatures step out of %s's portal. Take the flame of the Heart back into it!",
        n, war_name(w->att), war_name(w->def));
    war_tell(w->att, msg);
    war_tell(w->def, msg);
    SYNCLOG("World wars: player %d's %d creatures (level cap %d) attack player %d through its portal", (int)w->att, n, cap,
        (int)w->def);
}

/******************************************************************************/
/* Hooks */

TbBool world_war_spare_fallen(struct Thing *creatng)
{
    if (!world_mode_active() || !thing_is_creature(creatng))
        return false;
    int k = 0;
    struct War *w = war_of_member(creatng, &k);
    if (w == NULL)
        return false;
    if (w->carrier == creatng->index)
    {
        w->carrier = 0; /* the flame goes back to the Heart */
        SYNCLOG("World wars: the flame's carrier falls; the flame returns to player %d's Heart", (int)w->def);
    }
    w->party[k] = 0;
    w->pturn[k] = 0;
    war_home(creatng, true);
    return true;
}

TbBool world_war_spares_heart(PlayerNumber dealer, PlayerNumber heart_owner)
{
    if (!world_mode_active())
        return false;
    for (int i = 0; i < WAR_MAX; i++)
        if ((wars[i].state == War_On) && (wars[i].att == dealer) && (wars[i].def == heart_owner))
            return true;
    return false;
}

/******************************************************************************/
/* The commands */

TbBool world_war_command(int user, PlayerNumber me, const char *args)
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
        server_admin_notice(user, "Only a seated keeper makes war.");
        return true;
    }
    if (word[0] == '\0')
    {
        int n = 0;
        for (int i = 0; i < WAR_MAX; i++)
        {
            const struct War *w = &wars[i];
            if ((w->state < War_Waiting) || ((w->att != me) && (w->def != me)))
                continue;
            snprintf(msg, sizeof(msg), "Your war (%s %s, %ld gold each): %s.", (w->att == me) ? "on" : "from",
                war_name((w->att == me) ? w->def : w->att), (long)w->stake,
                (w->state == War_On) ? "on now" : (w->saw_day ? "at this dusk" : "after a day, at dusk"));
            server_admin_notice(user, msg);
            n++;
        }
        if (n == 0)
            server_admin_notice(user, "No war. !war <name> <gold> challenges a keeper; !war accept|refuse <name> answers.");
        return true;
    }
    if ((strcasecmp(word, "accept") == 0) || (strcasecmp(word, "refuse") == 0))
    {
        const PlayerNumber att = war_seat_by_name(rest);
        for (int i = 0; i < WAR_MAX; i++)
        {
            struct War *w = &wars[i];
            if ((w->state != War_Offered) || (w->att != att) || (w->def != me) || (w->until < (long)game.play_gameturn))
                continue;
            if (strcasecmp(word, "refuse") == 0)
            {
                snprintf(msg, sizeof(msg), "%s refuses your war.", war_name(me));
                war_tell(att, msg);
                server_admin_notice(user, "You refuse the war. It costs you nothing.");
                memset(w, 0, sizeof(*w));
                return true;
            }
            const char *why = war_check(att, me, w->stake);
            if (why != NULL)
            {
                snprintf(msg, sizeof(msg), "No war: %s.", why);
                server_admin_notice(user, msg);
                memset(w, 0, sizeof(*w));
                return true;
            }
            take_money_from_dungeon(att, w->stake, 1);
            take_money_from_dungeon(me, w->stake, 1);
            w->state = War_Waiting;
            w->saw_day = 0;
            snprintf(msg, sizeof(msg), "War is agreed: %s attacks %s through %s's own portal at dusk after a day to prepare. "
                "Stake %ld gold each.", war_name(att), war_name(me), war_name(me), (long)w->stake);
            war_tell(att, msg);
            war_tell(me, msg);
            SYNCLOG("World wars: player %d accepts player %d's war (stake %ld)", (int)me, (int)att, (long)w->stake);
            return true;
        }
        server_admin_notice(user, "That keeper has not challenged you (or the challenge ran out).");
        return true;
    }
    const PlayerNumber def = war_seat_by_name(word);
    const long stake = atol(rest);
    if (def == PLAYER_NONE)
    {
        snprintf(msg, sizeof(msg), "No keeper named %.40s here. !war <name> <gold>", word);
        server_admin_notice(user, msg);
        return true;
    }
    const char *why = war_check(me, def, stake);
    if (why != NULL)
    {
        snprintf(msg, sizeof(msg), "No war: %s.", why);
        server_admin_notice(user, msg);
        return true;
    }
    struct War *w = NULL;
    for (int i = 0; (i < WAR_MAX) && (w == NULL); i++)
        if ((wars[i].state == War_None) || ((wars[i].state == War_Offered) && (wars[i].until < (long)game.play_gameturn)))
            w = &wars[i];
    if (w == NULL)
    {
        server_admin_notice(user, "Too many wars in this world just now.");
        return true;
    }
    memset(w, 0, sizeof(*w));
    w->att = (int16_t)me;
    w->def = (int16_t)def;
    w->state = War_Offered;
    w->stake = (int32_t)stake;
    w->until = (int32_t)(game.play_gameturn + WAR_OFFER_TURNS);
    snprintf(msg, sizeof(msg), "You challenge %s to a war for %ld gold. They have two minutes to answer.", war_name(def), stake);
    server_admin_notice(user, msg);
    snprintf(msg, sizeof(msg), "%s challenges you to a war for %ld gold each, through your own portal. !war accept %s or "
        "!war refuse %s (refusing costs nothing).", war_name(me), stake, war_name(me), war_name(me));
    war_tell(def, msg);
    return true;
}

/******************************************************************************/
/* The turn */

/** Test hook KFX_WORLD_WARCMD=T (server, with KFX_WORLD_WARTEST=0 for the seven days): the real path. At T the
 *  first two human keepers get what they lack (as below); at T+20 the first types "!war <second> 500", at T+40 the
 *  second "!war accept <first>"; then the war waits for a day and comes at dusk. */
static void war_cmd_test(void)
{
    static long at = -1;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_WARCMD");
        at = (v != NULL) ? atol(v) : 0;
    }
    if ((at <= 0) || (((long)game.play_gameturn != at + 20) && ((long)game.play_gameturn != at + 40)))
        return;
    PlayerNumber two[2] = {PLAYER_NONE, PLAYER_NONE};
    int n = 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (n < 2); p++)
        if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)))
            two[n++] = p;
    if (n < 2)
        return;
    const PlayerNumber who = ((long)game.play_gameturn == at + 20) ? two[0] : two[1];
    const NetUserId u = get_player_primary_user(get_player(who));
    char line[96];
    if ((long)game.play_gameturn == at + 20)
        snprintf(line, sizeof(line), "war %s 500", war_name(two[1]));
    else
        snprintf(line, sizeof(line), "war accept %s", war_name(two[0]));
    SYNCLOG("World wars test: user %d (player %d) types \"!%s\"", (int)u, (int)who, line);
    server_admin_player_command(u, line);
}

/** Test hook KFX_WORLD_WARTEST=T[,stake] (server): at T the first two human keepers get a portal and four Trolls
 *  (the attacker) when they lack them; at T+20 the war is agreed and begins at once (no day to wait). */
static void war_test(void)
{
    static long at = -1;
    static long stake = 100;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_WARTEST");
        at = (v != NULL) ? atol(v) : 0;
        const char *c = (v != NULL) ? strchr(v, ',') : NULL;
        if (c != NULL)
            stake = atol(c + 1);
    }
    {
        static long cmd_at = -1;
        if (cmd_at < 0)
        {
            const char *c = getenv("KFX_WORLD_WARCMD");
            cmd_at = (c != NULL) ? atol(c) : 0;
        }
        if ((at <= 0) && (cmd_at > 0))
            at = cmd_at; /* the real path: the same setup at T, the commands after */
    }
    if ((at <= 0) || (((long)game.play_gameturn != at) && ((long)game.play_gameturn != at + 20)))
        return;
    PlayerNumber two[2] = {PLAYER_NONE, PLAYER_NONE};
    int n = 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (n < 2); p++)
        if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)))
            two[n++] = p;
    if (n < 2)
    {
        SYNCLOG("World wars test: fewer than two keepers");
        return;
    }
    if ((long)game.play_gameturn == at)
    {
        const RoomKind entk = (RoomKind)get_id(room_desc, "ENTRANCE");
        const PlayerNumber q = two[1];
        if (room_is_invalid(war_portal(q)))
        {
            struct Thing *qh = get_player_soul_container(q);
            const MapSlabCoord hx = subtile_slab(qh->mappos.x.stl.num), hy = subtile_slab(qh->mappos.y.stl.num);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -6; dx <= -4; dx++)
                {
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy), q, 0);
                    place_room(q, entk, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy));
                }
            SYNCLOG("World wars test: player %d gets a portal (%s)", (int)q, room_is_invalid(war_portal(q)) ? "failed" : "made");
        }
        struct Thing *ah = get_player_soul_container(two[0]);
        for (int k = 0; k < 4; k++)
        {
            struct Coord3d pos = ah->mappos;
            pos.x.val += (2 + k) * STL_PER_SLB * COORD_PER_STL;
            pos.z.val = 0;
            struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, "TROLL"), two[0]);
            if (thing_exists(c))
                move_thing_in_map(c, &pos);
        }
        for (int k = 0; k < 2; k++)
            if (get_dungeon(two[k])->total_money_owned < stake)
                player_add_offmap_gold(two[k], stake);
        return;
    }
    if (getenv("KFX_WORLD_WARCMD") != NULL)
        return; /* the real path goes on by commands (war_cmd_test) */
    const char *why = war_check(two[0], two[1], stake);
    if (why != NULL)
    {
        SYNCLOG("World wars test: no war (%s)", why);
        return;
    }
    struct War *w = &wars[0];
    memset(w, 0, sizeof(*w));
    w->att = two[0];
    w->def = two[1];
    w->stake = (int32_t)stake;
    take_money_from_dungeon(two[0], stake, 1);
    take_money_from_dungeon(two[1], stake, 1);
    war_begin(w);
}

static void war_on_turn(struct War *w, int wi)
{
    /* dawn: the defender wins */
    const TbBool day = world_is_day();
    if (day && !w->was_day)
    {
        war_finish(w, w->def, "dawn came first");
        return;
    }
    w->was_day = day ? 1 : 0;
    struct Thing *heart = get_player_soul_container(w->def);
    struct Room *portal = war_portal(w->def);
    if (!thing_exists(heart) || room_is_invalid(portal))
    {
        war_finish(w, PLAYER_NONE, "the defender's Heart or portal is gone");
        return;
    }
    int alive = 0;
    struct Coord3d portal_pos;
    portal_pos.x.val = subtile_coord_center(portal->central_stl_x);
    portal_pos.y.val = subtile_coord_center(portal->central_stl_y);
    portal_pos.z.val = 0;
    for (int k = 0; k < WAR_PARTY; k++)
    {
        struct Thing *t = war_member(w, k);
        if (thing_is_invalid(t))
        {
            if (w->party[k] > 0)
            {
                if (w->carrier == w->party[k])
                    w->carrier = 0;
                w->party[k] = 0; /* gone some other way */
            }
            continue;
        }
        if (creature_is_being_unconscious(t) || creature_is_kept_in_custody(t) || thing_is_picked_up(t))
            continue;
        alive++;
        const TbBool carrier = (w->carrier == t->index);
        if (carrier)
        {
            const struct Room *on = get_room_thing_is_on(t);
            if (room_exists(on) && (on->index == portal->index))
            {
                war_finish(w, w->att, "the flame was carried out through the portal");
                return;
            }
        } else if ((w->carrier == 0) && (get_chessboard_distance(&t->mappos, &heart->mappos) <= 2 * STL_PER_SLB * COORD_PER_STL))
        {
            w->carrier = t->index;
            w->carrier_turn = t->creation_turn;
            char msg[160];
            snprintf(msg, sizeof(msg), "%s's %s has taken the flame of the Heart!", war_name(w->att), kind_name(t->model));
            war_tell(w->att, msg);
            war_tell(w->def, msg);
            SYNCLOG("World wars: player %d's %s takes the flame of player %d's Heart", (int)w->att, thing_model_name(t), (int)w->def);
        }
        /* where it goes: the carrier to the portal, the others to the Heart (between fights) */
        if (get_creature_state_besides_interruptions(t) == CrSt_CreatureObjectCombat)
        {
            const struct CreatureControl *octrl = creature_control_get_from_thing(t);
            if (thing_is_dungeon_heart(thing_get(octrl->combat.battle_enemy_idx)))
                set_start_state(t); /* not the Heart itself: its flame */
        }
        if (((game.play_gameturn + t->index) % 48) == 0)
        {
            const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
            if (cctrl->combat_flags == 0)
            {
                struct Coord3d to = (w->carrier == t->index) ? portal_pos : heart->mappos;
                if (setup_person_move_to_coord(t, &to, NavRtF_Default))
                    t->continue_state = CrSt_CreatureDoingNothing;
            }
        }
    }
    if (alive == 0)
    {
        war_finish(w, w->def, "every attacker fell");
        return;
    }
    if (wi < (int)(sizeof(game.world_war_flame) / sizeof(game.world_war_flame[0])))
        game.world_war_flame[wi] = (uint16_t)w->carrier;
}

void world_wars_turn(void)
{
    if (!world_mode_active())
        return;
    war_test();
    war_cmd_test();
    for (int i = 0; i < WAR_MAX; i++)
    {
        struct War *w = &wars[i];
        switch (w->state)
        {
        case War_Offered:
            if (w->until < (long)game.play_gameturn)
                memset(w, 0, sizeof(*w));
            break;
        case War_Waiting:
            if (world_is_day())
                w->saw_day = 1;
            else if (w->saw_day)
                war_begin(w); /* dusk after a day */
            break;
        case War_On:
            if ((game.play_gameturn % 4) == 1)
                war_on_turn(w, i);
            break;
        default:
            break;
        }
    }
}

void world_wars_local_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % 4) != 0))
        return;
    for (size_t i = 0; i < sizeof(game.world_war_flame) / sizeof(game.world_war_flame[0]); i++)
    {
        if (game.world_war_flame[i] == 0)
            continue;
        struct Thing *t = thing_get(game.world_war_flame[i]);
        if (!thing_exists(t))
            continue;
        struct Coord3d pos = t->mappos;
        pos.z.val += 3 * COORD_PER_STL / 2;
        create_effect_element(&pos, TngEffElm_RedFlame, t->owner);
    }
}

/******************************************************************************/
/* The save (the WAR1 extra) */

void world_wars_player_reset(PlayerNumber p)
{
    for (int i = 0; i < WAR_MAX; i++)
    {
        struct War *w = &wars[i];
        if ((w->state == War_None) || ((w->att != p) && (w->def != p)))
            continue;
        if (w->state == War_Offered)
            memset(w, 0, sizeof(*w));
        else
            war_finish(w, PLAYER_NONE, "a keeper has a new dungeon");
    }
}

void world_wars_reset(void)
{
    memset(wars, 0, sizeof(wars));
    memset(game.world_war_flame, 0, sizeof(game.world_war_flame));
}

size_t world_wars_save_size(void)
{
    return 4 + sizeof(wars);
}

void world_wars_save(uint8_t *buf)
{
    const uint32_t n = WAR_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, wars, sizeof(wars));
}

TbBool world_wars_load(const uint8_t *buf, size_t len)
{
    world_wars_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != WAR_MAX) || (len < 4 + sizeof(wars)))
        return false;
    memcpy(wars, buf + 4, sizeof(wars));
    for (int i = 0; i < WAR_MAX; i++)
        if ((wars[i].state > War_On) || (wars[i].att < 0) || (wars[i].def < 0) || (wars[i].att >= PLAYERS_COUNT) ||
            (wars[i].def >= PLAYERS_COUNT))
            memset(&wars[i], 0, sizeof(wars[i]));
    return true;
}

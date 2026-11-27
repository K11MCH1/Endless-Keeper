/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram.c
 *     Endless worlds: the Pentagram (see world_pentagram.h): the web's rules, its effect totals, its packets, the
 *     player levels' curve and the XP rules, and its save.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pentagram.h"
#include "world_pentagram_data.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <spng.h>

#include "bflib_basics.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_utils.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "packets.h"
#include "gui_msgs.h"
#include "net_state.h"
#include "net_game.h"
#include "world_stream.h"
#include "world_progress.h"
#include "world_circles.h"
#include "world_threat.h"
#include "world_daynight.h"
#include "world_caretaker.h"
#include "server_admin.h"
#include "world_pentagram_screen.h"
#include <stdio.h>
#include "world_expedition.h"
#include "world_reaper.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* The web's state (struct Game: each seat's) */

#define wpg_state game.world_pentagram

struct WorldPentagram *world_pentagram_state(PlayerNumber plyr_idx)
{
    return ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX)) ? &wpg_state[plyr_idx] : NULL;
}

int world_pentagram_points(PlayerNumber plyr_idx, int node)
{
    const struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if ((s == NULL) || (node < 0) || (node >= WPG_NODES))
        return 0;
    return (s->pts[node >> 2] >> ((node & 3) * 2)) & 3;
}

static void wpg_set_points(PlayerNumber plyr_idx, int node, int pts)
{
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if ((s == NULL) || (node < 0) || (node >= WPG_NODES))
        return;
    const int sh = (node & 3) * 2;
    s->pts[node >> 2] = (uint8_t)((s->pts[node >> 2] & ~(3 << sh)) | ((pts & 3) << sh));
    s->version++;
}

TbBool world_pentagram_has(PlayerNumber plyr_idx, int node)
{
    if (node == WPG_HEART_NODE)
        return true; /* the start of every web */
    return world_pentagram_points(plyr_idx, node) > 0;
}

int world_pentagram_spent(PlayerNumber plyr_idx)
{
    int n = 0;
    for (int i = 0; i < WPG_NODES; i++)
        n += world_pentagram_points(plyr_idx, i);
    return n;
}

int world_pentagram_total(PlayerNumber plyr_idx)
{
    return world_mode_active() ? world_player_level(plyr_idx) : 0;
}

int world_pentagram_free(PlayerNumber plyr_idx)
{
    return world_pentagram_total(plyr_idx) - world_pentagram_spent(plyr_idx);
}

int world_pentagram_focus_points(PlayerNumber plyr_idx, int focus)
{
    int n = 0;
    for (int i = 0; i < WPG_NODES; i++)
        if ((wpg_nodes[i].focus == focus) && (wpg_nodes[i].kind != WpgK_Key))
            n += world_pentagram_points(plyr_idx, i);
    return n;
}

int world_pentagram_group_pick(PlayerNumber plyr_idx, int group)
{
    for (int i = 0; i < WPG_NODES; i++)
        if ((wpg_nodes[i].kind == WpgK_Key) && (wpg_nodes[i].group == group) && (world_pentagram_points(plyr_idx, i) > 0))
            return i;
    return -1;
}

static TbBool wpg_joined(PlayerNumber plyr_idx, int node)
{
    for (int e = 0; e < WPG_EDGES; e++)
    {
        const int a = wpg_edges[e][0], b = wpg_edges[e][1];
        if (((a == node) && world_pentagram_has(plyr_idx, b)) || ((b == node) && world_pentagram_has(plyr_idx, a)))
            return true;
    }
    return false;
}

/** Every node but the Heart learnt to the full, every keystone pair picked. */
static TbBool wpg_all_learnt(PlayerNumber plyr_idx)
{
    for (int i = 1; i < WPG_NODES; i++)
    {
        const struct WpgNode *n = &wpg_nodes[i];
        if (n->kind == WpgK_Key)
        {
            if (world_pentagram_group_pick(plyr_idx, n->group) < 0)
                return false;
        }
        else if (world_pentagram_points(plyr_idx, i) < n->max)
            return false;
    }
    return true;
}

static TbBool wpg_gate_open(PlayerNumber plyr_idx, const struct WpgNode *n)
{
    if (n->duo[0] != 255)
        return (world_pentagram_focus_points(plyr_idx, n->duo[0]) >= WPG_DUO_GATE_POINTS) &&
            (world_pentagram_focus_points(plyr_idx, n->duo[1]) >= WPG_DUO_GATE_POINTS);
    return world_pentagram_focus_points(plyr_idx, n->group) >= WPG_GATE_POINTS;
}

TbBool world_pentagram_can_learn(PlayerNumber plyr_idx, int node)
{
    if (!world_mode_active() || (node < 0) || (node >= WPG_NODES) || (world_pentagram_state(plyr_idx) == NULL))
        return false;
    if (world_pentagram_free(plyr_idx) <= 0)
        return false;
    const struct WpgNode *n = &wpg_nodes[node];
    if (node == WPG_HEART_NODE)
        return (world_pentagram_points(plyr_idx, node) == 0) && wpg_all_learnt(plyr_idx);
    if (n->kind == WpgK_Key)
        return (world_pentagram_group_pick(plyr_idx, n->group) < 0) && wpg_gate_open(plyr_idx, n) && wpg_joined(plyr_idx, node);
    const int p = world_pentagram_points(plyr_idx, node);
    return (p < n->max) && ((p > 0) || wpg_joined(plyr_idx, node));
}

const char *world_pentagram_why(PlayerNumber plyr_idx, int node)
{
    static char buf[160];
    if ((node < 0) || (node >= WPG_NODES))
        return "";
    const struct WpgNode *n = &wpg_nodes[node];
    if (node == WPG_HEART_NODE)
        return (world_pentagram_points(plyr_idx, node) > 0) ? "Unlocked: the Reaper's icon is at the top right of the game screen" : "";
    if (world_pentagram_can_learn(plyr_idx, node))
        return "";
    if (((n->kind == WpgK_Key) && (world_pentagram_points(plyr_idx, node) > 0)) ||
        ((n->kind != WpgK_Key) && (world_pentagram_points(plyr_idx, node) >= n->max)))
        return "Learnt";
    if ((n->kind == WpgK_Key) && (world_pentagram_group_pick(plyr_idx, n->group) >= 0))
        return "The other keystone was chosen";
    if (world_pentagram_free(plyr_idx) <= 0)
        return "No Ability Points left";
    if ((n->kind == WpgK_Key) && (n->duo[0] != 255) && !wpg_gate_open(plyr_idx, n))
    {
        snprintf(buf, sizeof(buf), "Needs %d points in each of %s and %s (%d and %d now)", WPG_DUO_GATE_POINTS,
            wpg_foci_names[n->duo[0]], wpg_foci_names[n->duo[1]], world_pentagram_focus_points(plyr_idx, n->duo[0]),
            world_pentagram_focus_points(plyr_idx, n->duo[1]));
        return buf;
    }
    if ((n->kind == WpgK_Key) && !wpg_gate_open(plyr_idx, n))
    {
        snprintf(buf, sizeof(buf), "Needs %d points in %s (%d now)", WPG_GATE_POINTS, wpg_foci_names[n->group],
            world_pentagram_focus_points(plyr_idx, n->group));
        return buf;
    }
    return "Not joined to your web yet";
}

long world_pentagram_reset_cost(PlayerNumber plyr_idx)
{
    return 500L * world_pentagram_spent(plyr_idx);
}

/******************************************************************************/
/* Effect totals: each machine sums them once per change of the web (the state's version) */

static int16_t wpg_fx_sum[PLAYERS_MAX][WFX_COUNT];
static uint8_t wpg_fx_version[PLAYERS_MAX];
static uint8_t wpg_fx_valid[PLAYERS_MAX];

static void wpg_fx_compute(PlayerNumber plyr_idx)
{
    int sum[WFX_COUNT];
    memset(sum, 0, sizeof(sum));
    for (int i = 0; i < WPG_NODES; i++)
    {
        const int p = world_pentagram_points(plyr_idx, i);
        if (p <= 0)
            continue;
        const struct WpgNode *n = &wpg_nodes[i];
        for (int k = 0; k < 2; k++)
            if (n->fx[k] < WFX_COUNT)
                sum[n->fx[k]] += n->v[k][min(p, 3) - 1];
    }
    for (int f = 0; f < WFX_COUNT; f++)
        wpg_fx_sum[plyr_idx][f] = (int16_t)max(-32000, min(32000, sum[f]));
    wpg_fx_version[plyr_idx] = wpg_state[plyr_idx].version;
    wpg_fx_valid[plyr_idx] = 1;
}

int world_pentagram_fx(PlayerNumber plyr_idx, enum WpgEffect fx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || ((int)fx < 0) || (fx >= WFX_COUNT) || !world_mode_active())
        return 0;
    if (!wpg_fx_valid[plyr_idx] || (wpg_fx_version[plyr_idx] != wpg_state[plyr_idx].version))
        wpg_fx_compute(plyr_idx);
    return wpg_fx_sum[plyr_idx][fx];
}

/******************************************************************************/
/* Learning and resetting: game state (the server, or every lockstep machine) */

void world_pentagram_packet_learn(PlayerNumber plyr_idx, const uint8_t *nodes, int count)
{
    if (!world_mode_active() || (world_pentagram_state(plyr_idx) == NULL))
        return;
    for (int i = 0; i < count; i++)
    {
        const int node = nodes[i];
        if (node == 255)
            continue;
        if (!world_pentagram_can_learn(plyr_idx, node))
        {
            SYNCLOG("World: player %d may not learn Pentagram node %d (%s): %s", (int)plyr_idx, node,
                (node < WPG_NODES) ? wpg_node_ids[node] : "?", (node < WPG_NODES) ? world_pentagram_why(plyr_idx, node) : "");
            return;
        }
        wpg_set_points(plyr_idx, node, world_pentagram_points(plyr_idx, node) + 1);
        SYNCLOG("World: player %d learnt %s (%s) %d/%d; %d of %d points spent", (int)plyr_idx, wpg_nodes[node].name,
            wpg_node_ids[node], world_pentagram_points(plyr_idx, node), wpg_nodes[node].max, world_pentagram_spent(plyr_idx),
            world_pentagram_total(plyr_idx));
        if (node == WPG_HEART_NODE)
            server_admin_player_event(plyr_idx, "%s unlocked the Heart of the Pentagram", server_admin_player_name(plyr_idx));
    }
}

void world_pentagram_packet_reset(PlayerNumber plyr_idx)
{
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if (!world_mode_active() || (s == NULL))
        return;
    if (world_raid_on(plyr_idx))
    {
        SYNCLOG("World: player %d may not reset the Pentagram during a raid", (int)plyr_idx);
        return;
    }
    const long cost = world_pentagram_reset_cost(plyr_idx);
    if (cost <= 0)
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon) || (dungeon->total_money_owned < cost))
    {
        SYNCLOG("World: player %d can't pay %ld gold to reset the Pentagram", (int)plyr_idx, cost);
        return;
    }
    take_money_from_dungeon(plyr_idx, cost, 1);
    memset(s->pts, 0, sizeof(s->pts));
    s->version++;
    SYNCLOG("World: player %d reset the Pentagram for %ld gold", (int)plyr_idx, cost);
}

void world_pentagram_seat_reset(PlayerNumber plyr_idx, TbBool keep_record)
{
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if (s == NULL)
        return;
    const uint8_t v = s->version;
    if (keep_record)
    {
        /* the web stays the player's; what kept time for the old dungeon goes */
        s->reaper_thing = 0;
        s->reaper_until = 0;
        s->reaper_angry = 0;
        s->soul_credit = 0;
        return;
    }
    memset(s, 0, sizeof(*s));
    s->version = (uint8_t)(v + 1);
}

/******************************************************************************/
/* This machine: points asked for go a packet a turn (8 to a packet) */

#define WPG_QUEUE 600
static uint8_t wpg_queue[WPG_QUEUE];
static int wpg_queue_n;
static TbBool wpg_reset_wanted;
static TbBool wpg_rest_wanted;
static GameTurn wpg_sent_turn;

void world_pentagram_send_learn(const uint8_t *nodes, int count)
{
    for (int i = 0; (i < count) && (wpg_queue_n < WPG_QUEUE); i++)
        wpg_queue[wpg_queue_n++] = nodes[i];
}

void world_pentagram_send_reset(void)
{
    wpg_reset_wanted = true;
}

void world_pentagram_send_rest(void)
{
    wpg_rest_wanted = true;
}

TbBool world_pentagram_rest_on(PlayerNumber plyr_idx, long day)
{
    const struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    return world_mode_active() && (s != NULL) && (day > 0) && (s->rest_day == (uint32_t)day);
}

int world_pentagram_rest_wait(PlayerNumber plyr_idx)
{
    const struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if ((s == NULL) || (world_pentagram_fx(plyr_idx, WFX_DAY_OF_REST) <= 0))
        return -1;
    const long today = world_day_number();
    if (s->rest_declared == 0)
        return 0;
    const long wait = (long)s->rest_declared + WPG_REST_EVERY - today;
    return (wait > 0) ? (int)wait : 0;
}

TbBool world_pentagram_omen_text(PlayerNumber plyr_idx, char *buf, size_t len)
{
    const struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    const long tomorrow = world_day_number() + 1;
    if (!world_mode_active() || world_is_day() || (s == NULL) || (world_pentagram_fx(plyr_idx, WFX_OMEN) <= 0) ||
        ((s->omen & 0xFF) == 0) || ((s->omen >> 8) != (uint16_t)(tomorrow & 0xFF)))
        return false;
    const int raids = (s->omen & 0xFF) - 1;
    if (raids <= 0)
        snprintf(buf, len, "No raid tomorrow");
    else
        snprintf(buf, len, "%d raid%s tomorrow", raids, (raids == 1) ? "" : "s");
    return true;
}

/** Day of Rest declared (the simulating machine). */
static void wpg_packet_rest(PlayerNumber plyr_idx)
{
    struct WorldPentagram *s = world_pentagram_state(plyr_idx);
    if ((s == NULL) || (world_pentagram_rest_wait(plyr_idx) != 0))
    {
        SYNCLOG("World: player %d may not declare a Day of Rest now", (int)plyr_idx);
        return;
    }
    const long today = world_day_number();
    s->rest_declared = (uint32_t)today;
    s->rest_day = (uint32_t)(today + 1);
    SYNCLOG("World: player %d declares world day %ld a Day of Rest", (int)plyr_idx, today + 1);
    if (is_my_player_number(plyr_idx))
        message_add_fmt(MsgType_Player, plyr_idx, "%s", "Tomorrow is your Day of Rest: no raids, no wild packs near you, dawn to dawn.");
}

TbBool world_pentagram_sending(void)
{
    return (wpg_queue_n > 0) || wpg_reset_wanted || wpg_rest_wanted;
}

/** Test switch KFX_WORLD_PENTAGRAMTEST="turn,xp,mode[,hover]": at that turn this machine's keeper gets that much XP;
 *  mode "open" opens the screen; "some" also learns a few nodes through the packets (Bulwark's path and keystone);
 *  "full" learns the whole web at once (game state, a single-player test only) and opens it; "reaper" the Heart too. */
/** Test hook KFX_WORLD_PENTAGRAMXP=turn,xp (a server, or single player): from that turn on, every 50 turns, each
 *  human seat with less XP gets that much (joiners seated later get it too). */
static void wpg_test_xp(void)
{
    static long turn = -2, xp = 0;
    if (turn == -2)
    {
        const char *v = getenv("KFX_WORLD_PENTAGRAMXP");
        turn = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%ld", &turn, &xp) == 2))
            SYNCLOG("Pentagram test: KFX_WORLD_PENTAGRAMXP=%ld,%ld", turn, xp);
        else
            turn = -1;
    }
    if ((turn < 0) || netstate_is_client() || ((long)game.play_gameturn < turn) || ((game.play_gameturn % 50) != 0))
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
        if (world_player_is_human(p) && player_exists(get_player(p)) && (world_player_xp(p) < (uint32_t)xp))
        {
            world_set_player_xp(p, (uint32_t)xp);
            SYNCLOG("Pentagram test: seat %d given %ld XP: level %d", (int)p, xp, world_player_level(p));
        }
}

static void wpg_test_turn(void)
{
    wpg_test_xp();
    static const char *v = NULL;
    static TbBool read = false;
    if (!read)
    {
        read = true;
        v = getenv("KFX_WORLD_PENTAGRAMTEST");
    }
    if ((v == NULL) || (my_player_number >= PLAYERS_MAX))
        return;
    long turn = 0, xp = 0;
    char mode[16] = "open";
    if (sscanf(v, "%ld,%ld,%15[a-z2]", &turn, &xp, mode) < 2)
        return;
    /* (a mode ending in 2: each keystone pair's second skill is the one learnt) */
    const size_t ml = strlen(mode);
    const TbBool second = (ml > 1) && (mode[ml - 1] == '2');
    if (second)
        mode[ml - 1] = 0;
    /* "rage": as "reaper", and 140 turns on (he can move) he is slapped */
    if ((strcmp(mode, "rage") == 0) && ((GameTurn)(turn + 140) == game.play_gameturn))
    {
        struct WorldPentagram *rs = world_pentagram_state(my_player_number);
        struct Thing *rt = (rs != NULL) ? thing_get(rs->reaper_thing) : INVALID_THING;
        if (thing_exists(rt))
            world_reaper_slapped(rt);
        return;
    }
    if (strcmp(mode, "rage") == 0)
        memcpy(mode, "reaper", 7);
    /* "reaper": 60 turns on, the screen closes and he is summoned two slabs from the Heart */
    if ((strcmp(mode, "reaper") == 0) && ((GameTurn)(turn + 60) == game.play_gameturn))
    {
        world_pentagram_screen_close();
        struct Thing *heart = get_player_soul_container(my_player_number);
        if (thing_exists(heart))
            set_players_packet_action(get_my_player(), PckA_WorldReaperSummon, heart->mappos.x.stl.num + 6,
                heart->mappos.y.stl.num, 0, 0);
        return;
    }
    /* "world": the full web; 30 turns on the screen closes and a Day of Rest is declared (a packet); 40 turns on a free
       expedition sets off 60 slabs east of the Heart (a Grand Expedition: five Imps) */
    if (strcmp(mode, "world") == 0)
    {
        if ((GameTurn)(turn + 30) == game.play_gameturn)
        {
            world_pentagram_screen_close();
            world_pentagram_send_rest();
            SYNCLOG("Pentagram test: Day of Rest asked for (wait %d)", world_pentagram_rest_wait(my_player_number));
            return;
        }
        if ((GameTurn)(turn + 40) == game.play_gameturn)
        {
            struct Thing *heart = get_player_soul_container(my_player_number);
            if (thing_exists(heart))
            {
                const TbResult r = world_expedition_cast_at_subtile(my_player_number, heart->mappos.x.stl.num + 180,
                    heart->mappos.y.stl.num, true);
                SYNCLOG("Pentagram test: expedition cast: %s", (r == Lb_SUCCESS) ? "set off" : "refused");
            }
            return;
        }
    }
    /* "netall" (a joiner, a server's thin client too): 0 XP here (the server gives it: KFX_WORLD_PENTAGRAMXP); at the
       turn everything is learnt through the screen and its packets; 300 turns on a Day of Rest is asked for, 400 on the
       Reaper is summoned beside the Heart */
    if (strcmp(mode, "netall") == 0)
    {
        if ((GameTurn)turn == game.play_gameturn)
        {
            const int n = world_pentagram_screen_test_all(second);
            SYNCLOG("Pentagram test: player %d chose %d points on the screen (level %d, %d points)", (int)my_player_number, n,
                world_player_level(my_player_number), world_pentagram_total(my_player_number));
        } else if ((GameTurn)(turn + 300) == game.play_gameturn)
        {
            world_pentagram_screen_close();
            world_pentagram_send_rest();
            SYNCLOG("Pentagram test: Day of Rest asked for (wait %d)", world_pentagram_rest_wait(my_player_number));
        } else if ((GameTurn)(turn + 400) == game.play_gameturn)
        {
            struct Thing *heart = get_player_soul_container(my_player_number);
            SYNCLOG("Pentagram test: Reaper asked for (ready %d; spent %d of %d)", (int)world_reaper_ready(my_player_number),
                world_pentagram_spent(my_player_number), world_pentagram_total(my_player_number));
            if (thing_exists(heart))
                set_players_packet_action(get_my_player(), PckA_WorldReaperSummon, heart->mappos.x.stl.num + 6,
                    heart->mappos.y.stl.num, 0, 0);
        }
        return;
    }
    if ((strcmp(mode, "motion") == 0) && ((long)game.play_gameturn > turn + 20) && ((long)game.play_gameturn <= turn + 140))
        world_pentagram_screen_test_motion((long)game.play_gameturn - turn - 21);
    if ((GameTurn)turn != game.play_gameturn)
        return;
    const PlayerNumber p = my_player_number;
    if (!netstate_is_client())
        world_set_player_xp(p, (uint32_t)xp);
    SYNCLOG("Pentagram test: player %d at %ld XP: level %d, %d points; mode %s", (int)p, xp, world_player_level(p),
        world_pentagram_total(p), mode);
    if ((strcmp(mode, "full") == 0) || (strcmp(mode, "reaper") == 0) || (strcmp(mode, "world") == 0) || (strcmp(mode, "motion") == 0) ||
        (strcmp(mode, "all") == 0))
    {
        struct WorldPentagram *s = world_pentagram_state(p);
        for (int k = 1; k < WPG_NODES; k++)
        {
            const int i = second ? (WPG_NODES - k) : k;
            const struct WpgNode *n = &wpg_nodes[i];
            if ((n->kind == WpgK_Key) && (world_pentagram_group_pick(p, n->group) >= 0))
                continue;
            wpg_set_points(p, i, (n->kind == WpgK_Key) ? 1 : n->max);
        }
        if ((strcmp(mode, "reaper") == 0) || (strcmp(mode, "all") == 0))
            wpg_set_points(p, WPG_HEART_NODE, 1);
        s->version++;
    }
    if (strcmp(mode, "some") == 0)
    {
        /* the Bulwark's spoke and Focus up to its keystone (through the packets, as a player would) */
        static const char *const ids[] = {"6.s1", "6.s2", "6.s3", "6.entry", "6.core", "6.a", "6.M1", "6.c", "6.M3", "6.M3",
            "6.M3", "6.d", "6.M4", "6.e", "6.KA", "8.s1", "8.s2", "8.s3", "8.entry", "8.core"};
        uint8_t b[32];
        int n = 0;
        for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++)
            for (int i = 0; i < WPG_NODES; i++)
                if (strcmp(wpg_node_ids[i], ids[k]) == 0)
                    b[n++] = (uint8_t)i;
        world_pentagram_send_learn(b, n);
    }
    if (strcmp(mode, "all") != 0) /* ("all": the whole web and the Heart, for a played game: the screen stays shut) */
        world_pentagram_screen_show();
}

/** Each turn on a machine with a keeper: one packet of what is waiting, when the packet is free. */
void world_pentagram_local_turn(void)
{
    wpg_test_turn();
    if (!world_mode_active() || (my_player_number >= PLAYERS_MAX) || !world_pentagram_sending())
        return;
    if (wpg_sent_turn == game.play_gameturn)
        return;
    struct PlayerInfo *player = get_my_player();
    if (player_invalid(player) || (get_players_packet_action(player) != PckA_None))
        return;
    wpg_sent_turn = game.play_gameturn;
    if (wpg_reset_wanted)
    {
        wpg_reset_wanted = false;
        set_players_packet_action(player, PckA_WorldPentagramReset, 0, 0, 0, 0);
        return;
    }
    if (wpg_rest_wanted)
    {
        wpg_rest_wanted = false;
        set_players_packet_action(player, PckA_WorldPentagramReset, 1, 0, 0, 0); /* (par1 1: Day of Rest) */
        return;
    }
    uint8_t b[8];
    memset(b, 255, sizeof(b));
    const int n = min(wpg_queue_n, 8);
    memcpy(b, wpg_queue, (size_t)n);
    memmove(wpg_queue, wpg_queue + n, (size_t)(wpg_queue_n - n));
    wpg_queue_n -= n;
    const uint32_t p1 = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    const uint32_t p2 = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    set_players_packet_action(player, PckA_WorldPentagramLearn, p1, p2, 0, 0);
}

/** packets.c: unpack and learn. */
void world_pentagram_packet(PlayerNumber plyr_idx, unsigned char action, uint32_t p1, uint32_t p2)
{
    if (action == PckA_WorldPentagramReset)
    {
        if (p1 == 1)
            wpg_packet_rest(plyr_idx);
        else
            world_pentagram_packet_reset(plyr_idx);
        return;
    }
    uint8_t b[8];
    for (int i = 0; i < 4; i++)
    {
        b[i] = (uint8_t)(p1 >> (i * 8));
        b[4 + i] = (uint8_t)(p2 >> (i * 8));
    }
    world_pentagram_packet_learn(plyr_idx, b, 8);
}

/******************************************************************************/
/* Player levels and XP (the user, 2026-11-27) */

#define WPG_LEVEL_CAP WPG_FINAL_LEVEL       /* 262: the whole web, and the Heart */
#define WPG_CURVE_KNEE 20                   /* up to here 1,000 + 150 x (L - 1) a level ... */
#define WPG_FLAT_LEVEL_XP 4000              /* ... then this */
#define WPG_DAILY_FULL_XP 1000              /* repeatable XP counted in full a world day; the rest a quarter */

uint32_t world_pentagram_level_xp(int level)
{
    if (level <= 1)
        return 0;
    if (level > WPG_LEVEL_CAP)
        level = WPG_LEVEL_CAP;
    const uint32_t n = (uint32_t)min(level, WPG_CURVE_KNEE);
    uint32_t xp = 1000u * (n - 1) + 150u * (n - 2) * (n - 1) / 2;
    if (level > WPG_CURVE_KNEE)
        xp += (uint32_t)WPG_FLAT_LEVEL_XP * (uint32_t)(level - WPG_CURVE_KNEE);
    return xp;
}

int world_pentagram_level_of(uint32_t xp)
{
    const uint32_t knee = world_pentagram_level_xp(WPG_CURVE_KNEE);
    if (xp >= knee)
        return min(WPG_LEVEL_CAP, WPG_CURVE_KNEE + (int)((xp - knee) / WPG_FLAT_LEVEL_XP));
    int level = 1;
    while ((level < WPG_CURVE_KNEE) && (xp >= world_pentagram_level_xp(level + 1)))
        level++;
    return level;
}

/** x(their strength / ours), kept between a quarter and double (percent). */
static long wpg_strength_pct(double theirs, double ours)
{
    if ((theirs <= 0.0) || (ours <= 0.0))
        return 100;
    const double r = theirs / ours;
    return (long)(100.0 * fmax(0.25, fmin(2.0, r)) + 0.5);
}

void world_pentagram_xp(PlayerNumber plyr_idx, enum WpgXpKind kind, long base, int enemy_level, PlayerNumber victim,
    MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!world_mode_active() || (base <= 0) || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || !world_player_is_human(plyr_idx))
        return;
    if (kind == WpgXp_Admin)
    {
        world_add_player_xp(plyr_idx, (uint32_t)base);
        return;
    }
    if (world_caretaker_active(plyr_idx))
        return; /* nothing earned while the caretaker keeps the dungeon */
    long xp = base;
    /* raids and wild packs: by their strength next to the keeper's army; keepers' Hearts: by the armies */
    if ((kind == WpgXp_Raid) || (kind == WpgXp_Pack))
        xp = xp * wpg_strength_pct((double)max(enemy_level, 1), (double)world_threat_upper_level(plyr_idx)) / 100;
    else if ((kind == WpgXp_KeeperHeart) && (victim >= 0) && (victim < PLAYERS_MAX))
    {
        struct WorldArmy theirs, ours;
        world_threat_army(victim, &theirs);
        world_threat_army(plyr_idx, &ours);
        xp = xp * wpg_strength_pct(theirs.power, ours.power) / 100;
    }
    /* Raze: fortresses and outposts' Hearts; Hunter's Moon: raids at night */
    if ((kind == WpgXp_Fortress) || (kind == WpgXp_OutpostHeart))
        xp = xp * (100 + world_pentagram_fx(plyr_idx, WFX_RAZE_XP_PCT)) / 100;
    if ((kind == WpgXp_Raid) && !world_is_day() && world_pentagram_on(plyr_idx, WFX_HUNTERS_MOON))
        xp *= 2;
    /* the circle where it happened: x1 in the first, a quarter more each circle out, x3 in the ninth */
    if ((slb_x >= 0) && (slb_y >= 0))
    {
        const int c = world_circle_at_engine_slab(slb_x, slb_y);
        xp = xp * (100 + 25 * (max(1, min(9, c)) - 1)) / 100;
    }
    /* the day's cap on what can be farmed */
    if ((kind == WpgXp_Raid) || (kind == WpgXp_Pack))
    {
        struct WorldPentagram *s = world_pentagram_state(plyr_idx);
        const uint32_t day = (uint32_t)world_day_number();
        if (s->xp_day != day)
        {
            s->xp_day = day;
            s->xp_day_done = 0;
        }
        const long full = min(xp, (long)max(0, WPG_DAILY_FULL_XP - (int)s->xp_day_done));
        s->xp_day_done = (uint16_t)min(65535L, (long)s->xp_day_done + full);
        xp = full + (xp - full) / 4;
    }
    if (xp <= 0)
        return;
    SYNCLOG("World: player %d earns %ld XP (kind %d, base %ld)", (int)plyr_idx, xp, (int)kind, base);
    world_add_player_xp(plyr_idx, (uint32_t)xp);
}

/******************************************************************************/
/* Saved with the world: PNT1 (each seat's state with a web or a Reaper day) */

#define WPG_SAVE_VERSION 1

static TbBool wpg_seat_used(PlayerNumber p)
{
    const struct WorldPentagram *s = &wpg_state[p];
    for (size_t i = 0; i < sizeof(s->pts); i++)
        if (s->pts[i] != 0)
            return true;
    return (s->reaper_day != 0) || (s->rest_declared != 0) || (s->xp_day_done != 0) || (s->biome_mask != 0) || (s->siege_n != 0);
}

size_t world_pentagram_save_size(void)
{
    size_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (wpg_seat_used(p))
            n++;
    return (n == 0) ? 0 : (8 + n * (4 + sizeof(struct WorldPentagram)));
}

void world_pentagram_save(uint8_t *buf)
{
    uint32_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (wpg_seat_used(p))
            n++;
    const uint32_t hdr[2] = {WPG_SAVE_VERSION, n};
    memcpy(buf, hdr, 8);
    size_t at = 8;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!wpg_seat_used(p))
            continue;
        const uint32_t plyr = (uint32_t)p;
        memcpy(buf + at, &plyr, 4);
        memcpy(buf + at + 4, &wpg_state[p], sizeof(struct WorldPentagram));
        at += 4 + sizeof(struct WorldPentagram);
    }
}

TbBool world_pentagram_load(const uint8_t *buf, size_t len)
{
    world_pentagram_reset();
    if (len < 8)
        return false;
    uint32_t hdr[2];
    memcpy(hdr, buf, 8);
    if ((hdr[0] != WPG_SAVE_VERSION) || (len < 8 + (size_t)hdr[1] * (4 + sizeof(struct WorldPentagram))))
        return false;
    size_t at = 8;
    for (uint32_t i = 0; i < hdr[1]; i++)
    {
        uint32_t plyr;
        memcpy(&plyr, buf + at, 4);
        if (plyr < PLAYERS_MAX)
        {
            memcpy(&wpg_state[plyr], buf + at + 4, sizeof(struct WorldPentagram));
            wpg_state[plyr].reaper_thing = 0; /* (a Reaper does not outlive the save) */
            wpg_state[plyr].reaper_until = 0;
            wpg_state[plyr].version++;
        }
        at += 4 + sizeof(struct WorldPentagram);
    }
    return true;
}

void world_pentagram_reset(void)
{
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        const uint8_t v = wpg_state[p].version;
        memset(&wpg_state[p], 0, sizeof(struct WorldPentagram));
        wpg_state[p].version = (uint8_t)(v + 1);
    }
    memset(wpg_fx_valid, 0, sizeof(wpg_fx_valid));
    wpg_queue_n = 0;
    wpg_reset_wanted = false;
}

/******************************************************************************/
/* Icons: the skills' PNGs, decoded once */

static unsigned char *wpg_icon_px[WPG_ICONS];
static int wpg_icon_w[WPG_ICONS], wpg_icon_h[WPG_ICONS];
static uint8_t wpg_icon_tried[WPG_ICONS];

int world_pentagram_heart_icon(void)
{
    return wpg_nodes[WPG_HEART_NODE].icon;
}

const unsigned char *world_pentagram_reaper_cursor(unsigned int *len)
{
    *len = wpg_reaper_cursor_len;
    return wpg_reaper_cursor;
}

const unsigned char *world_pentagram_icon(int icon, int *w, int *h)
{
    if ((icon < 0) || (icon >= WPG_ICONS))
        return NULL;
    if (!wpg_icon_tried[icon])
    {
        wpg_icon_tried[icon] = 1;
        spng_ctx *ctx = spng_ctx_new(0);
        struct spng_ihdr ihdr;
        size_t out_size = 0;
        if ((ctx != NULL) && (spng_set_png_buffer(ctx, wpg_icon_png[icon], wpg_icon_len[icon]) == 0) &&
            (spng_get_ihdr(ctx, &ihdr) == 0) && (ihdr.width > 0) && (ihdr.height > 0) && (ihdr.width <= 512) && (ihdr.height <= 512) &&
            (spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &out_size) == 0))
        {
            unsigned char *out = (unsigned char *)malloc(out_size);
            if ((out != NULL) && (spng_decode_image(ctx, out, out_size, SPNG_FMT_RGBA8, 0) == 0))
            {
                wpg_icon_px[icon] = out;
                wpg_icon_w[icon] = (int)ihdr.width;
                wpg_icon_h[icon] = (int)ihdr.height;
            }
            else
                free(out);
        }
        if (ctx != NULL)
            spng_ctx_free(ctx);
    }
    if (wpg_icon_px[icon] == NULL)
        return NULL;
    *w = wpg_icon_w[icon];
    *h = wpg_icon_h[icon];
    return wpg_icon_px[icon];
}

#ifdef __cplusplus
}
#endif

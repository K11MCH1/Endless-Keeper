/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file server_admin.c
 *     The admin connection of a dedicated server (keeperserver.exe).
 * @par Purpose:
 *     keeperserver.exe starts the server with -adminport (and the token in KFX_ADMIN_TOKEN) and connects to
 *     127.0.0.1:<port>. The server sends JSON lines (the console's events, a status every second, answers);
 *     keeperserver sends commands, the ones typed in its console after a "/" (kick, mute, ban, give, stop, restart ...).
 */
/******************************************************************************/
#include "pre_inc.h"
#include <enet6/enet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "server_admin.h"
#include "bflib_basics.h"
#include "bflib_enet.h"
#include "bflib_netsession.h"
#include "globals.h"
#include "game_legacy.h"
#include "keeperfx.hpp"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "thing_list.h"
#include "net_main.h"
#include "net_game.h"
#include "net_state.h"
#include "net_exchange_common.h"
#include "world_stream.h"
#include "world_progress.h"
#include "world_daynight.h"
#include "world_menu.h"
#include "world_net.h"
#include "net_diag.h"
#include "net_portforward.h"
#include "ver_defs.h"
#ifndef _WIN32
#include <sys/socket.h>
#endif
#include "post_inc.h"

#define ADMIN_IN_MAX      8192
#define ADMIN_EVENTS_KEPT 500
#define ADMIN_LINE_MAX    2048
#define BANS_FILE         "save/server_bans.txt"
#define MUTES_FILE        "save/server_mutes.txt"
#define LIST_MAX          1024
#define HEROES_REPORT_S   60

static struct {
    int port;
    char token[80];
    TbBool on;
    ENetSocket listen;
    ENetSocket c;              /* the admin (one at a time) */
    TbBool authed;
    char in[ADMIN_IN_MAX];
    size_t inlen;
    char *out;
    size_t outlen, outcap;
    char *events[ADMIN_EVENTS_KEPT];
    int events_head, events_count;
    time_t started;
    TbBool lists_loaded;
    TbBool net_init, bind_warned;
    time_t next_try;
    NetUserId dropping;        /* the user a kick or ban is dropping (its leave is logged by the kick) */
    uint16_t heroes_seen[PLAYERS_MAX];
    TbBool heroes_known[PLAYERS_MAX];
    unsigned long seq;         /* the number of the last log event (Keeper Server skips the ones it has seen) */
    char drop_why[224];        /* why this side dropped the last admin connection ("" when it didn't) */
    unsigned long drops;       /* admin connections this side dropped */
    char net_sent[512];        /* the network facts last sent (a new "net" line when they change) */
} A;

/* bans and mutes: a token (0 when not known), an address ("" when not known) and a name */
struct AdminEntry {
    uint64_t token;
    char ip[64];
    char name[32];
};
static struct AdminEntry bans[LIST_MAX], mutes[LIST_MAX];
static int bans_count, mutes_count;

static char farewell[160];

/******************************************************************************/
/* JSON */
static size_t json_str(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    if (cap < 3)
        return 0;
    dst[n++] = '"';
    for (; (s != NULL) && (*s != '\0') && (n + 8 < cap); s++)
    {
        const unsigned char ch = (unsigned char)*s;
        if ((ch == '"') || (ch == '\\'))
        {
            dst[n++] = '\\';
            dst[n++] = (char)ch;
        } else if (ch < 0x20)
            n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", ch);
        else if (ch >= 0x80)
            dst[n++] = '?'; /* the game's text is not UTF-8: nothing that would break the admin's JSON */
        else
            dst[n++] = (char)ch;
    }
    dst[n++] = '"';
    dst[n] = '\0';
    return n;
}

/******************************************************************************/
/* the connection */
static void out_append(const char *line)
{
    if ((A.c == ENET_SOCKET_NULL) || !A.authed)
        return;
    const size_t n = strlen(line);
    if (A.outlen + n + 1 > A.outcap)
    {
        size_t cap = (A.outcap > 0) ? A.outcap : 65536;
        while (cap < A.outlen + n + 1)
            cap *= 2;
        if (cap > (16u << 20))
            return; /* the admin isn't reading: lines are dropped, not memory */
        char *nb = (char *)realloc(A.out, cap);
        if (nb == NULL)
            return;
        A.out = nb;
        A.outcap = cap;
    }
    memcpy(A.out + A.outlen, line, n);
    A.outlen += n;
    A.out[A.outlen++] = '\n';
}

/** An event: kept (for an admin that connects later) and sent. */
static void emit(const char *json)
{
    if (A.port <= 0)
        return;
    if (A.events_count == ADMIN_EVENTS_KEPT)
    {
        free(A.events[A.events_head]);
        A.events[A.events_head] = NULL;
        A.events_head = (A.events_head + 1) % ADMIN_EVENTS_KEPT;
        A.events_count--;
    }
    A.events[(A.events_head + A.events_count) % ADMIN_EVENTS_KEPT] = strdup(json);
    A.events_count++;
    out_append(json);
}

/** Closes the admin connection; why (and the socket error, 0 for none) goes to the network log, and to Keeper
    Server when it connects again. */
static void drop_admin(const char *why, int err)
{
    if (A.c != ENET_SOCKET_NULL)
    {
        if (err != 0)
            snprintf(A.drop_why, sizeof(A.drop_why), "%s: %s", why, net_socket_error_text(err));
        else
            snprintf(A.drop_why, sizeof(A.drop_why), "%s", why);
        A.drops++;
        LbNetLog("Server admin: dropped the admin connection (%s); %lu dropped so far\n", A.drop_why, A.drops);
        enet_socket_destroy(A.c);
    }
    A.c = ENET_SOCKET_NULL;
    A.authed = false;
    A.inlen = 0;
    A.outlen = 0;
}

static long uptime_s(void)
{
    return (long)(time(NULL) - A.started);
}

/* the admin socket is read and written with the system's calls, which tell "nothing yet" from a real error
   (ENet's own calls give 0 for both) */
static int admin_send(const char *data, size_t len)
{
#ifdef _WIN32
    return send(A.c, data, (int)min(len, (size_t)65536), 0);
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return (int)send(A.c, data, min(len, (size_t)65536), flags);
#endif
}

static int admin_recv(char *data, size_t len)
{
#ifdef _WIN32
    return recv(A.c, data, (int)len, 0);
#else
    return (int)recv(A.c, data, len, 0);
#endif
}

static void flush_out(int tries)
{
    for (int k = 0; (k < tries) && (A.outlen > 0) && (A.c != ENET_SOCKET_NULL); k++)
    {
        const int n = admin_send(A.out, A.outlen);
        if (n < 0)
        {
            const int err = net_socket_error();
            if (!net_socket_would_block(err))
            {
                drop_admin("sending to Keeper Server failed", err);
                return;
            }
        }
        if (n > 0)
        {
            memmove(A.out, A.out + n, A.outlen - (size_t)n);
            A.outlen -= (size_t)n;
        }
        if ((A.outlen > 0) && (tries > 1))
            SDL_Delay(10);
    }
}

/******************************************************************************/
/* players */
const char *server_admin_player_name(PlayerNumber p)
{
    static char names[4][40];
    static int next;
    char *buf = names[next];
    next = (next + 1) % 4;
    if (p == PLAYER_GOOD)
    {
        snprintf(buf, sizeof(names[0]), "the heroes");
        return buf;
    }
    struct PlayerInfo *player = get_player(p);
    if (!player_invalid(player) && (player->player_name[0] != '\0'))
        snprintf(buf, sizeof(names[0]), "%s", player->player_name);
    else
        snprintf(buf, sizeof(names[0]), "Keeper %d", (int)p + 1);
    return buf;
}

static const char *seat_name(PlayerNumber p)
{
    struct PlayerInfo *player = get_player(p);
    return player_invalid(player) ? "" : player->player_name;
}

static NetUserId seat_user(PlayerNumber p)
{
    struct PlayerInfo *player = get_player(p);
    return player_invalid(player) ? -1 : get_player_primary_user(player);
}

/** The seat whose player has this name (any case), or -1. */
static PlayerNumber seat_by_name(const char *name)
{
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    for (int i = 0; i < n; i++)
    {
        const char *pn = seat_name(seats[i]);
        if ((pn[0] != '\0') && (strcasecmp(pn, name) == 0))
            return seats[i];
    }
    return -1;
}

static void user_ip(NetUserId u, char *buf, size_t len)
{
    buf[0] = '\0';
    if (u > SERVER_ID)
        enet_user_address(u, buf, len);
}

static int seat_rooms(PlayerNumber p)
{
    int n = 0;
    for (RoomIndex i = 1; i < ROOMS_COUNT; i++)
    {
        const struct Room *room = room_get(i);
        if (room_exists(room) && (room->owner == p))
            n++;
    }
    return n;
}

/** Who sits in a seat: the login's token of the user playing it now, else the seat's own. */
static uint64_t seat_token_of(PlayerNumber p)
{
    const NetUserId u = seat_user(p);
    if ((u > SERVER_ID) && (u < MAX_NET_CONNECTIONS) && (netstate.users[u].reconnect_token != 0))
        return netstate.users[u].reconnect_token;
    return world_seat_token(p);
}

static TbBool entry_matches(const struct AdminEntry *e, uint64_t token, const char *name, const char *ip)
{
    if ((e->token != 0) && (token != 0) && (e->token == token))
        return true;
    if ((e->name[0] != '\0') && (name != NULL) && (name[0] != '\0') && (strcasecmp(e->name, name) == 0))
        return true;
    return (e->ip[0] != '\0') && (ip != NULL) && (ip[0] != '\0') && (strcmp(e->ip, ip) == 0);
}

/* mutes go by the seat's own token (two windows on one PC share the login's token but not their seats') */
static TbBool seat_muted(PlayerNumber p)
{
    for (int i = 0; i < mutes_count; i++)
        if (entry_matches(&mutes[i], world_seat_token(p), seat_name(p), NULL))
            return true;
    return false;
}

static TbBool entry_matches(const struct AdminEntry *e, uint64_t token, const char *name, const char *ip);

static TbBool seat_banned(PlayerNumber p)
{
    for (int i = 0; i < bans_count; i++)
        if (entry_matches(&bans[i], seat_token_of(p), seat_name(p), NULL))
            return true;
    return false;
}

static size_t stats_json(char *buf, size_t cap, PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    const TbBool dg = !dungeon_invalid(dungeon);
    const struct WorldPlayerStats *st = world_player_stats(p);
    const NetUserId u = seat_user(p);
    char ip[64], ipj[80], name[80];
    user_ip(u, ip, sizeof(ip));
    json_str(ipj, sizeof(ipj), ip);
    json_str(name, sizeof(name), server_admin_player_name(p));
    const double hours = (st != NULL) ? (double)st->play_turns / ((double)max(turns_per_second, 1) * 3600.0) : 0.0;
    const struct PlayerInfo *player = get_player(p);
    const TbBool defeated = !player_invalid(player) && (player->victory_state == VicS_LostLevel);
    return (size_t)snprintf(buf, cap,
        "\"name\":%s,\"seat\":%d,\"online\":%s,\"level\":%d,\"xp\":%lu,\"creatures\":%d,\"rooms\":%d,\"kills\":%u,"
        "\"heroes_killed\":%u,\"hearts\":%lu,\"outposts\":%lu,\"portals\":%lu,\"hours\":%.3f,\"muted\":%s,\"ip\":%s,"
        "\"gold\":%ld,\"defeated\":%s,\"banned\":%s",
        name, (int)p, (u > SERVER_ID) ? "true" : "false", world_player_level(p), (unsigned long)world_player_xp(p),
        dg ? (int)dungeon->num_active_creatrs : 0, seat_rooms(p),
        dg ? (unsigned)dungeon->battles_won : 0u, dg ? (unsigned)dungeon->battles_won_vs[PLAYER_GOOD] : 0u,
        (st != NULL) ? (unsigned long)st->hearts : 0UL, (st != NULL) ? (unsigned long)st->outposts : 0UL,
        (st != NULL) ? (unsigned long)st->portals : 0UL, hours, seat_muted(p) ? "true" : "false", ipj,
        dg ? (long)dungeon->total_money_owned : 0L, defeated ? "true" : "false", seat_banned(p) ? "true" : "false");
}

/******************************************************************************/
/* bans and mutes on disk: token<TAB>address<TAB>name, "-" for none */
static void list_load(const char *path, struct AdminEntry *list, int *count)
{
    *count = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    char line[256];
    while ((*count < LIST_MAX) && (fgets(line, sizeof(line), f) != NULL))
    {
        char tok[32] = "", ip[64] = "", name[32] = "";
        if ((line[0] == '#') || (sscanf(line, "%31[^\t\r\n]\t%63[^\t\r\n]\t%31[^\t\r\n]", tok, ip, name) < 1))
            continue;
        struct AdminEntry e;
        memset(&e, 0, sizeof(e));
        e.token = (strcmp(tok, "-") == 0) ? 0 : strtoull(tok, NULL, 16);
        snprintf(e.ip, sizeof(e.ip), "%s", (strcmp(ip, "-") == 0) ? "" : ip);
        snprintf(e.name, sizeof(e.name), "%s", (strcmp(name, "-") == 0) ? "" : name);
        list[(*count)++] = e;
    }
    fclose(f);
}

static void list_save(const char *path, const struct AdminEntry *list, int count)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
    {
        WARNLOG("Server: can't write %s", path);
        server_admin_log("error", "Can't write %s", path);
        return;
    }
    for (int i = 0; i < count; i++)
    {
        char tok[32];
        if (list[i].token != 0)
            snprintf(tok, sizeof(tok), "%016llx", (unsigned long long)list[i].token);
        else
            snprintf(tok, sizeof(tok), "-");
        fprintf(f, "%s\t%s\t%s\n", tok, (list[i].ip[0] != '\0') ? list[i].ip : "-", (list[i].name[0] != '\0') ? list[i].name : "-");
    }
    fclose(f);
}

static void lists_load(void)
{
    if (A.lists_loaded)
        return;
    A.lists_loaded = true;
    list_load(BANS_FILE, bans, &bans_count);
    list_load(MUTES_FILE, mutes, &mutes_count);
}

static TbBool list_remove(struct AdminEntry *list, int *count, const char *name, uint64_t token)
{
    TbBool removed = false;
    for (int i = 0; i < *count;)
    {
        if (entry_matches(&list[i], token, name, NULL))
        {
            list[i] = list[--(*count)];
            removed = true;
            continue;
        }
        i++;
    }
    return removed;
}

/******************************************************************************/
/* the public face */
void server_admin_configure(int port, const char *token)
{
    if (!A.on)
    {
        A.listen = ENET_SOCKET_NULL;
        A.c = ENET_SOCKET_NULL;
        A.dropping = -1;
        A.started = time(NULL);
    }
    if (port > 0)
        A.port = port;
    if ((token != NULL) && (token[0] != '\0'))
        snprintf(A.token, sizeof(A.token), "%s", token);
    else if ((A.token[0] == '\0') && (getenv("KFX_ADMIN_TOKEN") != NULL))
        snprintf(A.token, sizeof(A.token), "%s", getenv("KFX_ADMIN_TOKEN"));
}

TbBool server_admin_enabled(void)
{
    return (A.port > 0);
}

void server_admin_log(const char *kind, const char *fmt, ...)
{
    if (A.port <= 0)
        return;
    char text[ADMIN_LINE_MAX / 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (strcmp(kind, "net") == 0)
        LbNetLog("Console: %s\n", text); /* the network log keeps the network's console lines too */
    char tj[ADMIN_LINE_MAX / 2 + 64], kj[48];
    json_str(tj, sizeof(tj), text);
    json_str(kj, sizeof(kj), kind);
    char line[ADMIN_LINE_MAX];
    A.seq++;
    snprintf(line, sizeof(line), "{\"t\":\"log\",\"kind\":%s,\"seq\":%lu,\"turn\":%lu,\"time\":%ld,\"text\":%s}", kj, A.seq,
        (unsigned long)game.play_gameturn, uptime_s(), tj);
    emit(line);
}

void server_admin_chat(int user, int plyr_idx, const char *name, const char *msg)
{
    if (A.port <= 0)
        return;
    char nj[80], mj[600];
    json_str(nj, sizeof(nj), name);
    json_str(mj, sizeof(mj), msg);
    char line[ADMIN_LINE_MAX];
    A.seq++;
    snprintf(line, sizeof(line),
        "{\"t\":\"log\",\"kind\":\"chat\",\"seq\":%lu,\"turn\":%lu,\"time\":%ld,\"name\":%s,\"seat\":%d,\"user\":%d,\"text\":%s}",
        A.seq, (unsigned long)game.play_gameturn, uptime_s(), nj, plyr_idx, user, mj);
    emit(line);
}

TbBool server_admin_user_muted(int user)
{
    if (A.port <= 0)
        return false;
    lists_load();
    if ((user <= SERVER_ID) || (user >= MAX_NET_USERS) || (mutes_count == 0))
        return false;
    const PlayerNumber p = get_user_state(user)->player_id;
    const uint64_t token = ((p >= 0) && (p < PLAYERS_COUNT)) ? world_seat_token(p) : 0;
    for (int i = 0; i < mutes_count; i++)
        if (entry_matches(&mutes[i], token, netstate.users[user].name, NULL))
            return true;
    return false;
}

TbBool server_admin_banned(uint64_t token, const char *name, const char *ip)
{
    lists_load();
    for (int i = 0; i < bans_count; i++)
        if (entry_matches(&bans[i], token, name, ip))
            return true;
    return false;
}

static void send_notice(int user, unsigned char sender, const char *text)
{
    char buf[300];
    size_t n = 0;
    buf[n++] = NETMSG_CHATMESSAGE;
    buf[n++] = (char)sender;
    const size_t tl = strnlen(text, sizeof(buf) - n - 1);
    memcpy(buf + n, text, tl);
    n += tl;
    buf[n++] = '\0';
    if (user >= 0)
    {
        if ((user != SERVER_ID) && IsUserActive(user))
            send_to_active_peers(1, NetSend_Reliable, buf, n, numask_of(user));
    } else
        send_to_active_peers(1, NetSend_Reliable, buf, n, numask_andnot(ALL_NET_CONNECTIONS_MASK, numask_of(SERVER_ID)));
}

void server_admin_notice(int user, const char *text)
{
    send_notice(user, SERVER_NOTICE_SENDER, text);
}

void server_admin_user_left(int user, int how)
{
    if ((A.port <= 0) || (user <= SERVER_ID) || (user >= MAX_NET_USERS) || (user == A.dropping))
        return;
    if (netstate.users[user].progress == USER_UNUSED)
        return;
    const char *detail = enet_last_drop_detail(); /* why ENet dropped it ("" for a drop this side asked for) */
    const PlayerNumber p = get_user_state(user)->player_id;
    if ((p < 0) || (p >= PLAYERS_COUNT))
    {
        /* gone before it had a seat: a join that failed */
        const char *name = (netstate.users[user].name[0] != '\0') ? netstate.users[user].name : "a joiner";
        if (detail[0] != '\0')
            server_admin_log("net", "%s (user %d) dropped before it had a seat: %s", name, user, detail);
        else
            server_admin_log("net", "%s (user %d) was dropped before it had a seat", name, user);
        return;
    }
    if ((how == 1) && (strncmp(detail, "timed out", 9) == 0))
        server_admin_log("leave", "%s %s", server_admin_player_name(p), detail);
    else if ((how == 0) && (strncmp(detail, "disconnected: ", 14) == 0))
        server_admin_log("leave", "%s left the world (%s)", server_admin_player_name(p), detail + 14);
    else
    {
        static const char *hows[] = {"left the world", "timed out", "was dropped (it reconnected)"};
        server_admin_log("leave", "%s %s", server_admin_player_name(p), hows[((how >= 0) && (how < 3)) ? how : 0]);
    }
}

void server_admin_shutdown(TbBool restart)
{
    if (A.port <= 0)
        return;
    char line[128];
    snprintf(line, sizeof(line), "{\"t\":\"bye\",\"turn\":%lu,\"time\":%ld,\"restart\":%s}", (unsigned long)game.play_gameturn,
        uptime_s(), restart ? "true" : "false");
    emit(line);
    flush_out(100); /* the last lines go out before the process ends */
}

const char *server_farewell_text(void)
{
    return (farewell[0] != '\0') ? farewell : NULL;
}

void server_farewell_set(const char *text)
{
    snprintf(farewell, sizeof(farewell), "%s", (text != NULL) ? text : "");
}

/******************************************************************************/
/* commands */
static void cmd_players(void)
{
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    const size_t cap = 4096 + (size_t)n * 640;
    char *buf = (char *)malloc(cap);
    if (buf == NULL)
        return;
    size_t len = (size_t)snprintf(buf, cap, "{\"t\":\"players\",\"turn\":%lu,\"time\":%ld,\"max\":%d,\"list\":[",
        (unsigned long)game.play_gameturn, uptime_s(), n);
    TbBool first = true;
    for (int i = 0; i < n; i++)
    {
        const PlayerNumber p = seats[i];
        /* the seats someone has played (a name), or plays now */
        if ((seat_name(p)[0] == '\0') && (seat_user(p) <= SERVER_ID))
            continue;
        if (len + 700 >= cap)
            break;
        if (!first)
            buf[len++] = ',';
        first = false;
        buf[len++] = '{';
        len += stats_json(buf + len, cap - len, p);
        buf[len++] = '}';
    }
    snprintf(buf + len, cap - len, "]}");
    out_append(buf); /* an answer: not kept */
    free(buf);
}

static void cmd_stats(const char *name)
{
    const PlayerNumber p = seat_by_name(name);
    if (p < 0)
    {
        server_admin_log("warn", "No player named %s", name);
        return;
    }
    char buf[ADMIN_LINE_MAX];
    size_t len = (size_t)snprintf(buf, sizeof(buf), "{\"t\":\"stats\",\"turn\":%lu,\"time\":%ld,", (unsigned long)game.play_gameturn, uptime_s());
    len += stats_json(buf + len, sizeof(buf) - len, p);
    snprintf(buf + len, sizeof(buf) - len, "}");
    out_append(buf);
}

/** Drops a seated player with a reason its game shows (kicked, banned). */
static TbBool drop_with(PlayerNumber p, enum NetJoinRejection reason, const char *text)
{
    const NetUserId u = (p >= 0) ? seat_user(p) : -1;
    if ((u <= SERVER_ID) || !IsUserActive(u))
        return false;
    send_notice(u, SERVER_FAREWELL_SENDER, text); /* the words, before the connection goes (the drop's data too) */
    A.dropping = u;
    netstate.sp->drop_user(u, reason);
    A.dropping = -1;
    return true;
}

static void cmd_kick(const char *name)
{
    const PlayerNumber p = seat_by_name(name);
    if (p < 0)
    {
        server_admin_log("warn", "No player named %s", name);
        return;
    }
    if (!drop_with(p, NetJoin_Kicked, "You were kicked."))
    {
        server_admin_log("warn", "%s isn't online", server_admin_player_name(p));
        return;
    }
    server_admin_log("admin", "%s was kicked", server_admin_player_name(p));
}

static void cmd_ban(const char *name, TbBool ban)
{
    lists_load();
    const PlayerNumber p = seat_by_name(name);
    if (!ban)
    {
        const TbBool removed = list_remove(bans, &bans_count, name, (p >= 0) ? seat_token_of(p) : 0);
        if (removed)
            list_save(BANS_FILE, bans, bans_count);
        server_admin_log(removed ? "admin" : "warn", removed ? "%s is no longer banned" : "%s wasn't banned", name);
        return;
    }
    if (server_admin_banned((p >= 0) ? seat_token_of(p) : 0, name, NULL))
    {
        server_admin_log("warn", "%s is banned already", name);
        return;
    }
    if (bans_count >= LIST_MAX)
    {
        server_admin_log("warn", "The ban list is full");
        return;
    }
    struct AdminEntry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", (p >= 0) ? seat_name(p) : name);
    if (p >= 0)
    {
        e.token = seat_token_of(p);
        user_ip(seat_user(p), e.ip, sizeof(e.ip));
    }
    bans[bans_count++] = e;
    list_save(BANS_FILE, bans, bans_count);
    const TbBool online = (p >= 0) && drop_with(p, NetJoin_Banned, "You were banned from this server.");
    server_admin_log("admin", "%s is banned%s (by name%s%s)", e.name, online ? " and was removed from the world" : "",
        (e.token != 0) ? ", token" : "", (e.ip[0] != '\0') ? ", address" : "");
}

static void cmd_mute(const char *name, TbBool mute)
{
    lists_load();
    const PlayerNumber p = seat_by_name(name);
    if (!mute)
    {
        const TbBool removed = list_remove(mutes, &mutes_count, name, (p >= 0) ? world_seat_token(p) : 0);
        if (removed)
            list_save(MUTES_FILE, mutes, mutes_count);
        server_admin_log(removed ? "admin" : "warn", removed ? "%s can chat again" : "%s wasn't muted", name);
        return;
    }
    if (p < 0)
    {
        server_admin_log("warn", "No player named %s", name);
        return;
    }
    if (seat_muted(p))
    {
        server_admin_log("warn", "%s is muted already", seat_name(p));
        return;
    }
    if (mutes_count >= LIST_MAX)
        return;
    struct AdminEntry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", seat_name(p));
    e.token = world_seat_token(p);
    mutes[mutes_count++] = e;
    list_save(MUTES_FILE, mutes, mutes_count);
    server_admin_log("admin", "%s is muted", e.name);
}

static void cmd_give(const char *name, const char *what, const char *amount_text)
{
    const PlayerNumber p = seat_by_name(name);
    if (p < 0)
    {
        server_admin_log("warn", "No player named %s", name);
        return;
    }
    char *end = NULL;
    const long amount = strtol(amount_text, &end, 10);
    if ((amount <= 0) || (end == amount_text) || (*end != '\0') || (amount > 100000000L))
    {
        server_admin_log("warn", "give: the amount must be a number from 1 to 100000000");
        return;
    }
    char msg[96];
    if (strcasecmp(what, "gold") == 0)
    {
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon))
        {
            server_admin_log("warn", "%s has no dungeon", seat_name(p));
            return;
        }
        player_add_offmap_gold(p, amount);
        server_admin_log("admin", "Gave %ld gold to %s", amount, seat_name(p));
        snprintf(msg, sizeof(msg), "The server gave you %ld gold.", amount);
    } else if (strcasecmp(what, "xp") == 0)
    {
        world_add_player_xp(p, (uint32_t)amount);
        server_admin_log("admin", "Gave %ld XP to %s (level %d)", amount, seat_name(p), world_player_level(p));
        snprintf(msg, sizeof(msg), "The server gave you %ld XP.", amount);
    } else
    {
        server_admin_log("warn", "give <name> gold <amount>, or give <name> XP <amount>");
        return;
    }
    const NetUserId u = seat_user(p);
    if (u > SERVER_ID)
        server_admin_notice(u, msg);
}

static void cmd_reset(const char *name)
{
    const PlayerNumber p = seat_by_name(name);
    if (p < 0)
    {
        server_admin_log("warn", "No player named %s", name);
        return;
    }
    char why[96] = "";
    if (!world_seat_reset(p, why, sizeof(why)))
    {
        server_admin_log("warn", "%s's dungeon was not reset: %s", seat_name(p), why);
        return;
    }
    server_admin_log("admin", "%s's dungeon was reset: a new start dungeon elsewhere (level and upgrades kept)", seat_name(p));
    const NetUserId u = seat_user(p);
    if (u > SERVER_ID)
        server_admin_notice(u, "The server reset your dungeon. You start again elsewhere.");
}

static void say(const char *text)
{
    char msg[300];
    snprintf(msg, sizeof(msg), "Server: %s", text);
    server_admin_notice(-1, msg);
    server_admin_chat(-1, -1, "Server", text);
}

static void run_command(char *line)
{
    while ((*line == ' ') || (*line == '\t'))
        line++;
    if (*line == '/')
        line++; /* the console's command prefix */
    char whole[ADMIN_LINE_MAX];
    snprintf(whole, sizeof(whole), "%s", line);
    char *args[4] = {NULL, NULL, NULL, NULL};
    int nargs = 0;
    const char *rest = NULL; /* everything after the command (say's text) */
    for (char *s = line; (*s != '\0') && (nargs < 4);)
    {
        while (*s == ' ')
            s++;
        if (*s == '\0')
            break;
        if (nargs == 1)
            rest = whole + (s - line);
        args[nargs++] = s;
        while ((*s != '\0') && (*s != ' '))
            s++;
        if (*s == ' ')
            *s++ = '\0';
    }
    if (nargs == 0)
        return;
    const char *cmd = args[0];
    const TbBool quiet = (strcasecmp(cmd, "players") == 0) || (strcasecmp(cmd, "stats") == 0) || (strcasecmp(cmd, "say") == 0);
    if (!quiet)
        server_admin_log("command", "/%s", whole);
    if (strcasecmp(cmd, "players") == 0)
        cmd_players();
    else if ((strcasecmp(cmd, "stats") == 0) && (nargs >= 2))
        cmd_stats(args[1]);
    else if ((strcasecmp(cmd, "kick") == 0) && (nargs >= 2))
        cmd_kick(args[1]);
    else if ((strcasecmp(cmd, "ban") == 0) && (nargs >= 2))
        cmd_ban(args[1], true);
    else if ((strcasecmp(cmd, "unban") == 0) && (nargs >= 2))
        cmd_ban(args[1], false);
    else if ((strcasecmp(cmd, "mute") == 0) && (nargs >= 2))
        cmd_mute(args[1], true);
    else if ((strcasecmp(cmd, "unmute") == 0) && (nargs >= 2))
        cmd_mute(args[1], false);
    else if ((strcasecmp(cmd, "give") == 0) && (nargs >= 4))
        cmd_give(args[1], args[2], args[3]);
    else if ((strcasecmp(cmd, "reset") == 0) && (nargs >= 2))
        cmd_reset(args[1]);
    else if ((strcasecmp(cmd, "say") == 0) && (rest != NULL))
        say(rest);
    else if (strcasecmp(cmd, "save") == 0)
    {
        world_autosave("admin");
        server_admin_log("admin", "The world is saved");
    }
    else if ((strcasecmp(cmd, "stop") == 0) || (strcasecmp(cmd, "restart") == 0))
    {
        const TbBool restart = (strcasecmp(cmd, "restart") == 0);
        server_admin_log("admin", restart ? "Restarting: the world is saved first" : "Stopping: the world is saved first");
        server_admin_notice(-1, restart ? "Server: the server restarts. Come back in a minute." : "Server: the server is closing.");
        world_autosave(restart ? "restart" : "stop");
        server_admin_shutdown(restart);
        exit_keeper = 1;
    }
    else
        server_admin_log("warn", "Unknown command: /%s (try /help)", whole);
}

/******************************************************************************/
static void admin_hello(void)
{
    char sj[80];
    json_str(sj, sizeof(sj), world_seed_text());
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    char line[512];
    /* run: this process (the event numbers start again in a new one); seq: the last event's number */
    snprintf(line, sizeof(line),
        "{\"t\":\"hello\",\"turn\":%lu,\"time\":%ld,\"version\":\"%s\",\"seed\":%s,\"max\":%d,\"run\":%ld,\"seq\":%lu}",
        (unsigned long)game.play_gameturn, uptime_s(), VER_STRING, sj, n, (long)A.started, A.seq);
    out_append(line);
}

/** The network facts Keeper Server shows: the game's UDP port, this PC's address on its network, the address the
    internet sees (STUN), the router's port forwarding, the World List hub's port. */
static void net_facts(char *buf, size_t size, char *lan, size_t lan_size, char *fwd, size_t fwd_size, int *forwarded)
{
    net_local_ipv4(lan, lan_size);
    *forwarded = 0;
    port_forward_status(fwd, fwd_size, forwarded);
    char lj[80], pj[80], fj[400];
    json_str(lj, sizeof(lj), lan);
    json_str(pj, sizeof(pj), external_ipv4_address);
    json_str(fj, sizeof(fj), fwd);
    snprintf(buf, size, "{\"t\":\"net\",\"port\":%d,\"lan\":%s,\"public\":%s,\"public_port\":%d,\"forward\":%s,\"forwarded\":%s,\"hub\":%d}",
        (int)enet_get_bound_port(), lj, pj, (int)external_ipv4_port, fj, (*forwarded != 0) ? "true" : "false", wnet_hub_port());
}

/** A new "net" line when the facts change (checked every second), and a console line when the addresses do. */
static void net_facts_update(void)
{
    char line[512], lan[64], fwd[192];
    int forwarded;
    net_facts(line, sizeof(line), lan, sizeof(lan), fwd, sizeof(fwd), &forwarded);
    if (strcmp(line, A.net_sent) == 0)
        return;
    snprintf(A.net_sent, sizeof(A.net_sent), "%s", line);
    out_append(line); /* not kept: a fresh one goes to each admin that connects */
    static char logged[512];
    char what[512];
    const int port = (int)enet_get_bound_port();
    if (port <= 0)
        return;
    snprintf(what, sizeof(what), "%s|%s|%d|%s", lan, external_ipv4_address, (int)external_ipv4_port, fwd);
    if ((strcmp(what, logged) == 0) || (strncmp(fwd, "asking", 6) == 0))
        return;
    snprintf(logged, sizeof(logged), "%s", what);
    char here[96], there[128];
    if (lan[0] != '\0')
        snprintf(here, sizeof(here), "%s:%d on this network", lan, port);
    else
        snprintf(here, sizeof(here), "UDP port %d (this PC's address is unknown)", port);
    if (external_ipv4_address[0] != '\0')
        snprintf(there, sizeof(there), "%s:%d from the internet", external_ipv4_address, (external_ipv4_port > 0) ? (int)external_ipv4_port : port);
    else
        snprintf(there, sizeof(there), "the internet address is unknown (STUN didn't answer)");
    server_admin_log("net", "Hosting at %s; %s%s%s", here, there, (fwd[0] != '\0') ? "; " : "", fwd);
}

static void admin_lines(void)
{
    for (;;)
    {
        char *nl = (char *)memchr(A.in, '\n', A.inlen);
        if (nl == NULL)
            break;
        *nl = '\0';
        if ((nl > A.in) && (nl[-1] == '\r'))
            nl[-1] = '\0';
        const size_t used = (size_t)(nl - A.in) + 1;
        char cmd[ADMIN_LINE_MAX];
        snprintf(cmd, sizeof(cmd), "%s", A.in);
        memmove(A.in, A.in + used, A.inlen - used);
        A.inlen -= used;
        if (!A.authed)
        {
            if ((strncmp(cmd, "auth ", 5) != 0) || (A.token[0] == '\0') || (strcmp(cmd + 5, A.token) != 0))
            {
                drop_admin("a connection without the right token", 0);
                return;
            }
            A.authed = true;
            LbNetLog("Server admin: Keeper Server connected\n");
            admin_hello();
            for (int i = 0; i < A.events_count; i++)
                out_append(A.events[(A.events_head + i) % ADMIN_EVENTS_KEPT]);
            if (A.drop_why[0] != '\0')
            {
                /* why this side dropped the last connection */
                char wj[300], line[400];
                json_str(wj, sizeof(wj), A.drop_why);
                snprintf(line, sizeof(line), "{\"t\":\"dropped\",\"why\":%s,\"drops\":%lu}", wj, A.drops);
                out_append(line);
                A.drop_why[0] = '\0';
            }
            if (A.net_sent[0] != '\0')
                out_append(A.net_sent);
            continue;
        }
        run_command(cmd);
        if (A.c == ENET_SOCKET_NULL)
            return;
    }
}

void server_admin_poll(void)
{
    if (A.port <= 0)
        return;
    if (!A.on)
    {
        A.on = true;
        lists_load();
    }
    if (A.listen == ENET_SOCKET_NULL)
    {
        /* the game starts the network (and Windows sockets) only later: this machine's own start, and a retry every
           2 s while the port is taken */
        const time_t now = time(NULL);
        if (now < A.next_try)
            return;
        A.next_try = now + 2;
        if (!A.net_init)
            A.net_init = (enet_initialize() == 0);
        A.listen = enet_socket_create(ENET_ADDRESS_TYPE_IPV4, ENET_SOCKET_TYPE_STREAM);
        if (A.listen != ENET_SOCKET_NULL)
        {
            enet_socket_set_option(A.listen, ENET_SOCKOPT_REUSEADDR, 1);
            enet_socket_set_option(A.listen, ENET_SOCKOPT_NONBLOCK, 1);
            ENetAddress addr;
            enet_address_build_loopback(&addr, ENET_ADDRESS_TYPE_IPV4); /* 127.0.0.1 only */
            addr.port = (enet_uint16)A.port;
            if ((enet_socket_bind(A.listen, &addr) < 0) || (enet_socket_listen(A.listen, 2) < 0))
            {
                enet_socket_destroy(A.listen);
                A.listen = ENET_SOCKET_NULL;
            }
        }
        if (A.listen == ENET_SOCKET_NULL)
        {
            if (!A.bind_warned)
            {
                A.bind_warned = true;
                SYNCLOG("Server admin: can't listen on 127.0.0.1:%d yet; trying again", A.port);
                server_admin_log("warn", "The admin connection can't use 127.0.0.1:%d yet (in use?): trying again", A.port);
            }
            return;
        }
        SYNCLOG("Server admin: listening on 127.0.0.1:%d", A.port);
        server_admin_log("boot", "KeeperFX %s: admin connection on 127.0.0.1:%d; %d bans, %d mutes", VER_STRING, A.port, bans_count,
            mutes_count);
    }
    ENetAddress from;
    ENetSocket s = enet_socket_accept(A.listen, &from);
    if (s != ENET_SOCKET_NULL)
    {
        if (A.c != ENET_SOCKET_NULL)
            drop_admin("another Keeper Server connection took over", 0);
        enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
        A.c = s;
        A.authed = false;
        A.inlen = 0;
        A.outlen = 0;
    }
    if (A.c == ENET_SOCKET_NULL)
        return;
    for (;;)
    {
        if (A.inlen >= ADMIN_IN_MAX - 1)
        {
            drop_admin("a command longer than 8 KB", 0);
            return;
        }
        const int n = admin_recv(A.in + A.inlen, ADMIN_IN_MAX - 1 - A.inlen);
        if (n == 0)
        {
            drop_admin("Keeper Server closed the connection", 0);
            return;
        }
        if (n < 0)
        {
            const int err = net_socket_error();
            if (net_socket_would_block(err))
                break; /* nothing more to read now */
            drop_admin("reading from Keeper Server failed", err);
            return;
        }
        A.inlen += (size_t)n;
        admin_lines();
        if (A.c == ENET_SOCKET_NULL)
            return;
    }
    flush_out(1);
}

void server_admin_turn(void)
{
    if ((A.port <= 0) || !network_is_active() || !netstate_is_server())
        return;
    /* play time: the seats a user holds */
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    int online = 0;
    for (int i = 0; i < n; i++)
    {
        if (seat_user(seats[i]) > SERVER_ID)
        {
            world_player_played_turn(seats[i]);
            online++;
        }
    }
    const GameTurn tps = (GameTurn)max(turns_per_second, 1);
    /* the heroes each player's creatures defeated, a line a minute (one a kill would flood the console) */
    if ((game.play_gameturn % (tps * HEROES_REPORT_S)) == 0)
    {
        for (int i = 0; i < n; i++)
        {
            const PlayerNumber p = seats[i];
            const struct Dungeon *dungeon = get_players_num_dungeon(p);
            if (dungeon_invalid(dungeon) || (p >= PLAYERS_MAX))
                continue;
            const uint16_t now = dungeon->battles_won_vs[PLAYER_GOOD];
            if (A.heroes_known[p])
            {
                const uint16_t delta = (uint16_t)(now - A.heroes_seen[p]);
                if ((delta > 0) && (delta < 10000))
                    server_admin_log("heroes", "%s defeated %u hero%s", server_admin_player_name(p), (unsigned)delta, (delta == 1) ? "" : "es");
            }
            A.heroes_seen[p] = now;
            A.heroes_known[p] = true;
        }
    }
    if ((game.play_gameturn % tps) != 0)
        return;
    net_facts_update();
    double sim_ms = 0, stream_ms = 0;
    netstate_server_timing(&sim_ms, &stream_ms);
    char line[512];
    snprintf(line, sizeof(line),
        "{\"t\":\"status\",\"turn\":%lu,\"time\":%ld,\"players\":%d,\"max\":%d,\"day\":%ld,\"sim_ms\":%.2f,\"stream_ms\":%.2f,"
        "\"chunks\":%d,\"explored\":%ld,\"creatures\":%ld,\"tps\":%d}",
        (unsigned long)game.play_gameturn, uptime_s(), online, n, world_day_number(), sim_ms, stream_ms,
        world_loaded_chunk_count(), world_stored_chunk_count(), (long)game.thing_lists[TngList_Creatures].count,
        (int)turns_per_second);
    out_append(line); /* not kept: a fresh one comes every second */
}
/******************************************************************************/

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file server_roles.c
 *     Endless Keeper: roles (2026-11-10, Keeper Server's Permissions tab): what a player may do on a server, and the
 *     colour of their name in chat.
 * @par Purpose:
 *     save/server_roles.txt holds the roles, the top one first (Keeper Server writes it; the server reads it):
 *         @<setting><TAB>0|1                                  (ingame, hierarchy, announce)
 *         <name><TAB><#rrggbb, or - for none><TAB><permission,permission,...>
 *     "Default" is everyone's role. save/server_role_members.txt holds who has which other roles (only the server
 *     writes it): the seat's token, the player's name (for the eye) and the roles. A role is given only by the server's
 *     admin (Keeper Server, on the admin port, 127.0.0.1 only, with its token), and it belongs to the seat's login
 *     token, which a client has to prove at login (net_secure.c): a name, or anything a client says, counts for
 *     nothing. So a role lasts through restarts, dungeon resets and "Start again", and nobody can claim one.
 *     The clients get each seat's name and colour (NETMSG_ROLES, to clients that read it) for their chat lines.
 */
/******************************************************************************/
#include "pre_inc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "server_roles.h"
#include "bflib_basics.h"
#include "globals.h"
#include "game_legacy.h"
#include "keeperfx.hpp"
#include "player_data.h"
#include "net_main.h"
#include "net_game.h"
#include "net_state.h"
#include "net_exchange_common.h"
#include "world_stream.h"
#include "server_admin.h"
#include "post_inc.h"

#define ROLES_FILE        "save/server_roles.txt"
#define ROLE_MEMBERS_FILE "save/server_role_members.txt"
#define ROLES_MAX         32      /* a seat's roles are a bit each */
#define ROLE_NAME_MAX     24
#define MEMBERS_MAX       1024
#define MEMBER_ROLES_MAX  8
#define ROLES_TABLE_EVERY 10      /* turns between looks at the clients' table (half a second) */
#define ROLES_TABLE_NAME  20      /* characters of a name in the table */

static const char *perm_names[RolP_Count] = {"chat", "kick", "ban", "unban", "mute", "unmute", "give", "stats", "players",
    "say", "save", "reset", "restart", "stop", "cheats"};

const char *role_permission_name(int perm)
{
    return ((perm >= 0) && (perm < RolP_Count)) ? perm_names[perm] : NULL;
}

int role_permission_by_name(const char *name)
{
    for (int i = 0; (name != NULL) && (i < RolP_Count); i++)
        if (strcasecmp(perm_names[i], name) == 0)
            return i;
    return -1;
}

struct RoleDef {
    char name[ROLE_NAME_MAX];
    TbBool has_color;
    uint32_t rgb;
    uint32_t perms;
};

struct RoleMember {
    uint64_t token;
    char name[32];
    char roles[MEMBER_ROLES_MAX][ROLE_NAME_MAX];
    int count;
};

static struct {
    TbBool loaded;
    struct RoleDef roles[ROLES_MAX];
    int count;
    int default_index;
    TbBool set_ingame, set_hierarchy, set_announce;
    struct RoleMember members[MEMBERS_MAX];
    int members_count;
    /* the clients' table: built every ROLES_TABLE_EVERY turns, sent to a client when it changed since its last */
    unsigned char table[8 + PLAYERS_MAX * (6 + ROLES_TABLE_NAME)];
    size_t table_len;
    uint32_t serial;
    uint32_t sent_serial[MAX_NET_CONNECTIONS];
} R;

/******************************************************************************/
/* Reading and writing */

/** The next tab-separated field of a line (the line is cut there). */
static char *next_field(char **s)
{
    char *f = *s;
    if (f == NULL)
        return (char *)"";
    char *tab = strchr(f, '\t');
    if (tab != NULL)
    {
        *tab = '\0';
        *s = tab + 1;
    } else
        *s = NULL;
    return f;
}

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while ((n > 0) && ((s[n - 1] == '\r') || (s[n - 1] == '\n') || (s[n - 1] == ' ')))
        s[--n] = '\0';
}

/** A role name: letters, digits, - and _ only (it goes in commands, files and JSON as it is). */
static TbBool role_name_ok(const char *name)
{
    if ((name == NULL) || (name[0] == '\0') || (strlen(name) >= ROLE_NAME_MAX))
        return false;
    for (const char *c = name; *c != '\0'; c++)
        if (!(((*c >= 'a') && (*c <= 'z')) || ((*c >= 'A') && (*c <= 'Z')) || ((*c >= '0') && (*c <= '9')) || (*c == '-') || (*c == '_')))
            return false;
    return true;
}

static int role_index(const char *name)
{
    for (int i = 0; (name != NULL) && (i < R.count); i++)
        if (strcasecmp(R.roles[i].name, name) == 0)
            return i;
    return -1;
}

/** No file yet: what Keeper Server starts with too. */
static void roles_defaults(void)
{
    R.count = 0;
    struct RoleDef *m = &R.roles[R.count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "Moderator");
    m->has_color = true;
    m->rgb = 0x40B410; /* a green the game's palette has (it has no blues) */
    m->perms = (1u << RolP_Kick) | (1u << RolP_Mute) | (1u << RolP_Ban);
    struct RoleDef *d = &R.roles[R.count++];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "Default");
    d->perms = 1u << RolP_Chat;
}

static void roles_load(void)
{
    R.set_ingame = true;
    R.set_hierarchy = true;
    R.set_announce = true;
    FILE *f = fopen(ROLES_FILE, "r");
    if (f == NULL)
        roles_defaults();
    else
    {
        R.count = 0;
        char line[1024];
        while (fgets(line, sizeof(line), f) != NULL)
        {
            trim_end(line);
            if ((line[0] == '#') || (line[0] == '\0'))
                continue;
            char *s = line;
            if (line[0] == '@')
            {
                s = line + 1;
                const char *key = next_field(&s);
                const TbBool on = (s != NULL) && (atoi(s) != 0);
                if (strcasecmp(key, "ingame") == 0)
                    R.set_ingame = on;
                else if (strcasecmp(key, "hierarchy") == 0)
                    R.set_hierarchy = on;
                else if (strcasecmp(key, "announce") == 0)
                    R.set_announce = on;
                continue;
            }
            const char *name = next_field(&s);
            const char *colour = next_field(&s);
            char *perms = next_field(&s);
            if (!role_name_ok(name) || (R.count >= ROLES_MAX) || (role_index(name) >= 0))
                continue;
            struct RoleDef *r = &R.roles[R.count++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", name);
            if (colour[0] == '#')
            {
                r->has_color = true;
                r->rgb = (uint32_t)strtoul(colour + 1, NULL, 16) & 0xFFFFFFu;
            }
            for (char *tok = strtok(perms, ", "); tok != NULL; tok = strtok(NULL, ", "))
            {
                const int pm = role_permission_by_name(tok);
                if (pm >= 0)
                    r->perms |= 1u << pm;
            }
        }
        fclose(f);
    }
    R.default_index = role_index("Default");
    if ((R.default_index < 0) && (R.count < ROLES_MAX))
    {
        struct RoleDef *d = &R.roles[R.count];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "Default");
        d->perms = 1u << RolP_Chat;
        R.default_index = R.count++;
    }
    if (R.default_index < 0)
        R.default_index = R.count - 1; /* 32 roles and none named Default: the last is everyone's */
}

static void members_load(void)
{
    R.members_count = 0;
    FILE *f = fopen(ROLE_MEMBERS_FILE, "r");
    if (f == NULL)
        return;
    char line[1024];
    while ((R.members_count < MEMBERS_MAX) && (fgets(line, sizeof(line), f) != NULL))
    {
        trim_end(line);
        if ((line[0] == '#') || (line[0] == '\0'))
            continue;
        char *s = line;
        const char *tok = next_field(&s);
        const char *name = next_field(&s);
        char *roles = next_field(&s);
        struct RoleMember *m = &R.members[R.members_count];
        memset(m, 0, sizeof(*m));
        m->token = strtoull(tok, NULL, 16);
        if (m->token == 0)
            continue;
        snprintf(m->name, sizeof(m->name), "%s", (strcmp(name, "-") == 0) ? "" : name);
        for (char *r = strtok(roles, ","); (r != NULL) && (m->count < MEMBER_ROLES_MAX); r = strtok(NULL, ","))
            if (r[0] != '\0')
                snprintf(m->roles[m->count++], ROLE_NAME_MAX, "%s", r);
        R.members_count++;
    }
    fclose(f);
}

static void members_save(void)
{
    FILE *f = fopen(ROLE_MEMBERS_FILE, "w");
    if (f == NULL)
    {
        server_admin_log("error", "Can't write %s", ROLE_MEMBERS_FILE);
        return;
    }
    fprintf(f, "# Who has which roles (the server writes this): seat token, name, roles. Roles: server_roles.txt\n");
    for (int i = 0; i < R.members_count; i++)
    {
        const struct RoleMember *m = &R.members[i];
        if (m->count == 0)
            continue;
        fprintf(f, "%016llx\t%s\t", (unsigned long long)m->token, (m->name[0] != '\0') ? m->name : "-");
        for (int k = 0; k < m->count; k++)
            fprintf(f, "%s%s", (k > 0) ? "," : "", m->roles[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void roles_ensure(void)
{
    if (R.loaded)
        return;
    R.loaded = true;
    roles_load();
    members_load();
}

void server_roles_reload(void)
{
    R.loaded = true;
    roles_load();
    members_load();
}

TbBool server_roles_setting(const char *name)
{
    roles_ensure();
    if (strcasecmp(name, "ingame") == 0)
        return R.set_ingame;
    if (strcasecmp(name, "hierarchy") == 0)
        return R.set_hierarchy;
    if (strcasecmp(name, "announce") == 0)
        return R.set_announce;
    return false;
}

/******************************************************************************/
/* A seat's roles */

static struct RoleMember *member_of(uint64_t token)
{
    for (int i = 0; (token != 0) && (i < R.members_count); i++)
        if (R.members[i].token == token)
            return &R.members[i];
    return NULL;
}

/** A bit for each role the seat has (Default always). */
static uint32_t seat_role_bits(PlayerNumber p)
{
    uint32_t bits = (R.default_index >= 0) ? (1u << R.default_index) : 0;
    const struct RoleMember *m = ((p >= 0) && (p < PLAYERS_MAX)) ? member_of(world_seat_token(p)) : NULL;
    for (int k = 0; (m != NULL) && (k < m->count); k++)
    {
        const int idx = role_index(m->roles[k]);
        if (idx >= 0)
            bits |= 1u << idx;
    }
    return bits;
}

static uint32_t bits_perms(uint32_t bits)
{
    uint32_t perms = 0;
    for (int i = 0; i < R.count; i++)
        if (bits & (1u << i))
            perms |= R.roles[i].perms;
    return perms;
}

static PlayerNumber user_seat(int user)
{
    if ((user <= SERVER_ID) || (user >= MAX_NET_USERS))
        return -1;
    const PlayerNumber p = get_user_state(user)->player_id;
    return ((p >= 0) && (p < PLAYERS_COUNT)) ? p : -1;
}

TbBool server_roles_user_can(int user, int perm)
{
    roles_ensure();
    if ((perm < 0) || (perm >= RolP_Count))
        return false;
    const PlayerNumber p = user_seat(user);
    const uint32_t bits = (p >= 0) ? seat_role_bits(p) : ((R.default_index >= 0) ? (1u << R.default_index) : 0);
    return (bits_perms(bits) & (1u << perm)) != 0;
}

int server_roles_seat_rank(PlayerNumber plyr_idx)
{
    roles_ensure();
    const uint32_t bits = seat_role_bits(plyr_idx);
    for (int i = 0; i < R.count; i++)
        if (bits & (1u << i))
            return i;
    return R.count;
}

const char *server_roles_seat_top_name(PlayerNumber plyr_idx)
{
    const int rank = server_roles_seat_rank(plyr_idx);
    return (rank < R.count) ? R.roles[rank].name : "Default";
}

/** The seat's colour: its highest role that has one. */
static TbBool seat_rgb(PlayerNumber p, uint32_t *rgb)
{
    const uint32_t bits = seat_role_bits(p);
    for (int i = 0; i < R.count; i++)
        if ((bits & (1u << i)) && R.roles[i].has_color)
        {
            *rgb = R.roles[i].rgb;
            return true;
        }
    return false;
}

const char *server_roles_seat_color(PlayerNumber plyr_idx)
{
    static char hex[4][8];
    static int next;
    roles_ensure();
    char *buf = hex[next];
    next = (next + 1) % 4;
    uint32_t rgb = 0;
    if (seat_rgb(plyr_idx, &rgb))
        snprintf(buf, sizeof(hex[0]), "#%06lx", (unsigned long)rgb);
    else
        buf[0] = '\0';
    return buf;
}

size_t server_roles_seat_json(char *buf, size_t cap, PlayerNumber plyr_idx)
{
    roles_ensure();
    size_t n = (size_t)snprintf(buf, cap, ",\"roles\":[");
    const uint32_t bits = seat_role_bits(plyr_idx);
    TbBool first = true;
    for (int i = 0; (i < R.count) && (n + 40 < cap); i++)
    {
        if (!(bits & (1u << i)) || (i == R.default_index))
            continue;
        n += (size_t)snprintf(buf + n, cap - n, "%s\"%s\"", first ? "" : ",", R.roles[i].name); /* role names: no quotes */
        first = false;
    }
    if (n + 24 < cap)
        n += (size_t)snprintf(buf + n, cap - n, "],\"color\":\"%s\"", server_roles_seat_color(plyr_idx));
    return n;
}

/******************************************************************************/
/* The admin's commands */

/** The seat whose player has this name (any case), or -1. */
static PlayerNumber seat_named(const char *name)
{
    const PlayerNumber *seats = NULL;
    const int n = world_seat_players(&seats);
    for (int i = 0; i < n; i++)
    {
        const struct PlayerInfo *player = get_player(seats[i]);
        if (!player_invalid(player) && (player->player_name[0] != '\0') && (strcasecmp(player->player_name, name) == 0))
            return seats[i];
    }
    return -1;
}

static void tell_player(PlayerNumber p, const char *text)
{
    const struct PlayerInfo *player = get_player(p);
    const int u = player_invalid(player) ? -1 : get_player_primary_user(player);
    if (u > SERVER_ID)
        server_admin_notice(u, text);
}

void server_roles_admin(const char *args)
{
    roles_ensure();
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", (args != NULL) ? args : "");
    trim_end(buf);
    char *s = buf;
    while (*s == ' ')
        s++;
    char *verb = s;
    while ((*s != '\0') && (*s != ' '))
        s++;
    if (*s == ' ')
        *s++ = '\0';
    while (*s == ' ')
        s++;
    if (strcasecmp(verb, "reload") == 0)
    {
        server_roles_reload();
        return;
    }
    char *role = s;
    while ((*s != '\0') && (*s != ' '))
        s++;
    if (*s == ' ')
        *s++ = '\0';
    while (*s == ' ')
        s++;
    const char *rest = s; /* the player's name (it may have spaces), or rename's new name */
    if (strcasecmp(verb, "rename") == 0)
    {
        if (!role_name_ok(role) || !role_name_ok(rest))
        {
            server_admin_log("warn", "role rename <old> <new> (letters, digits, - and _)");
            return;
        }
        int n = 0;
        for (int i = 0; i < R.members_count; i++)
            for (int k = 0; k < R.members[i].count; k++)
                if (strcasecmp(R.members[i].roles[k], role) == 0)
                {
                    snprintf(R.members[i].roles[k], ROLE_NAME_MAX, "%s", rest);
                    n++;
                }
        if (n > 0)
            members_save();
        return;
    }
    const TbBool add = (strcasecmp(verb, "add") == 0);
    if ((!add && (strcasecmp(verb, "remove") != 0)) || (role[0] == '\0') || (rest[0] == '\0'))
    {
        server_admin_log("warn", "role add <role> <player>, role remove <role> <player>");
        return;
    }
    const int idx = role_index(role);
    if (idx < 0)
    {
        server_admin_log("warn", "No role named %s", role);
        return;
    }
    if (idx == R.default_index)
    {
        server_admin_log("warn", "Everyone has the %s role", R.roles[idx].name);
        return;
    }
    const PlayerNumber p = seat_named(rest);
    if (p < 0)
    {
        server_admin_log("warn", "No player named %s", rest);
        return;
    }
    const uint64_t token = world_seat_token(p);
    const char *pname = get_player(p)->player_name;
    if (token == 0)
    {
        server_admin_log("warn", "%s has no login token (an older game?): a role needs one", pname);
        return;
    }
    struct RoleMember *m = member_of(token);
    if (add)
    {
        if (m == NULL)
        {
            if (R.members_count >= MEMBERS_MAX)
            {
                server_admin_log("warn", "The role list is full");
                return;
            }
            m = &R.members[R.members_count++];
            memset(m, 0, sizeof(*m));
            m->token = token;
        }
        snprintf(m->name, sizeof(m->name), "%s", pname);
        for (int k = 0; k < m->count; k++)
            if (strcasecmp(m->roles[k], R.roles[idx].name) == 0)
            {
                server_admin_log("warn", "%s has the role %s already", pname, R.roles[idx].name);
                return;
            }
        if (m->count >= MEMBER_ROLES_MAX)
        {
            server_admin_log("warn", "%s has %d roles: no more", pname, MEMBER_ROLES_MAX);
            return;
        }
        snprintf(m->roles[m->count++], ROLE_NAME_MAX, "%s", R.roles[idx].name);
        members_save();
        server_admin_log("admin", "%s now has the role %s", pname, R.roles[idx].name);
        char msg[96];
        snprintf(msg, sizeof(msg), "You now have the role %s.", R.roles[idx].name);
        tell_player(p, msg);
    } else
    {
        TbBool removed = false;
        for (int k = 0; (m != NULL) && (k < m->count); k++)
            if (strcasecmp(m->roles[k], R.roles[idx].name) == 0)
            {
                memmove(m->roles[k], m->roles[k + 1], sizeof(m->roles[0]) * (size_t)(m->count - k - 1));
                m->count--;
                removed = true;
                break;
            }
        if (!removed)
        {
            server_admin_log("warn", "%s doesn't have the role %s", pname, R.roles[idx].name);
            return;
        }
        members_save();
        server_admin_log("admin", "%s no longer has the role %s", pname, R.roles[idx].name);
        char msg[96];
        snprintf(msg, sizeof(msg), "You no longer have the role %s.", R.roles[idx].name);
        tell_player(p, msg);
    }
}

/******************************************************************************/
/* The clients' table: each seat's name and colour (version 1: count u16, then per seat: player u8, flags u8 (1: has a
   colour), r, g, b, the name with its NUL) */

void server_roles_turn(void)
{
    if (!network_is_active() || !netstate_is_server() || ((game.play_gameturn % ROLES_TABLE_EVERY) != 0))
        return;
    roles_ensure();
    unsigned char t[sizeof(R.table)];
    size_t n = 0;
    t[n++] = NETMSG_ROLES;
    t[n++] = 1;
    const size_t count_at = n;
    n += 2;
    unsigned count = 0;
    const PlayerNumber *seats = NULL;
    const int ns = world_seat_players(&seats);
    for (int i = 0; i < ns; i++)
    {
        const PlayerNumber p = seats[i];
        const struct PlayerInfo *player = get_player(p);
        if (player_invalid(player) || (player->player_name[0] == '\0') || (p < 0))
            continue;
        if (n + 6 + ROLES_TABLE_NAME > sizeof(t))
            break;
        uint32_t rgb = 0;
        const TbBool coloured = seat_rgb(p, &rgb);
        t[n++] = (unsigned char)p;
        t[n++] = coloured ? 1 : 0;
        t[n++] = (unsigned char)(rgb >> 16);
        t[n++] = (unsigned char)(rgb >> 8);
        t[n++] = (unsigned char)rgb;
        const size_t len = strnlen(player->player_name, ROLES_TABLE_NAME);
        memcpy(t + n, player->player_name, len);
        n += len;
        t[n++] = '\0';
        count++;
    }
    t[count_at] = (unsigned char)(count & 0xFF);
    t[count_at + 1] = (unsigned char)(count >> 8);
    if ((n != R.table_len) || (memcmp(t, R.table, n) != 0))
    {
        memcpy(R.table, t, n);
        R.table_len = n;
        R.serial++;
    }
    for (int c = SERVER_ID + 1; c < MAX_NET_CONNECTIONS; c++)
    {
        if ((netstate.users[c].progress == USER_UNUSED) || ((netstate_client_caps(c) & NETSTATE_CAP_ROLES) == 0))
            continue;
        if (R.sent_serial[c] == R.serial)
            continue;
        send_to_active_peers(1, NetSend_Reliable, (char *)R.table, R.table_len, numask_of(c));
        R.sent_serial[c] = R.serial;
    }
}

void server_roles_caps_changed(int user)
{
    if ((user >= 0) && (user < MAX_NET_CONNECTIONS))
        R.sent_serial[user] = 0;
}

/******************************************************************************/
/* A client */
static struct {
    TbBool active;
    TbBool valid[PLAYERS_MAX];
    TbBool has_color[PLAYERS_MAX];
    uint32_t rgb[PLAYERS_MAX];
    char name[PLAYERS_MAX][ROLES_TABLE_NAME + 1];
} RC;

void roles_client_receive(const char *buf, size_t len)
{
    const unsigned char *b = (const unsigned char *)buf;
    if ((len < 3) || (b[0] != 1))
        return;
    const unsigned count = (unsigned)b[1] | ((unsigned)b[2] << 8);
    size_t pos = 3;
    memset(RC.valid, 0, sizeof(RC.valid));
    for (unsigned i = 0; (i < count) && (pos + 6 <= len); i++)
    {
        const unsigned p = b[pos];
        const TbBool coloured = (b[pos + 1] & 1) != 0;
        const uint32_t rgb = ((uint32_t)b[pos + 2] << 16) | ((uint32_t)b[pos + 3] << 8) | (uint32_t)b[pos + 4];
        pos += 5;
        const unsigned char *nul = (const unsigned char *)memchr(b + pos, '\0', len - pos);
        if (nul == NULL)
            break;
        const size_t nl = (size_t)(nul - (b + pos));
        if (p < PLAYERS_MAX)
        {
            RC.valid[p] = true;
            RC.has_color[p] = coloured;
            RC.rgb[p] = rgb;
            snprintf(RC.name[p], sizeof(RC.name[p]), "%.*s", (int)((nl < ROLES_TABLE_NAME) ? nl : ROLES_TABLE_NAME), (const char *)(b + pos));
        }
        pos += nl + 1;
    }
    RC.active = true;
}

void roles_client_reset(void)
{
    memset(&RC, 0, sizeof(RC));
}

TbBool roles_client_player(PlayerNumber plyr_idx, const char **name, TbBool *has_color, uint32_t *rgb)
{
    if (!RC.active || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || !RC.valid[plyr_idx] || (RC.name[plyr_idx][0] == '\0'))
        return false;
    *name = RC.name[plyr_idx];
    *has_color = RC.has_color[plyr_idx];
    *rgb = RC.rgb[plyr_idx];
    return true;
}

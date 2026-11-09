/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_main.h
 *     Header file for net_main.c.
 * @par Purpose:
 *     Public declarations for shared multiplayer network support routines.
 * @par Comment:
 *     None.
 * @author   KeeperFX Team
 * @date     09 May 2026
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef DK_NET_MAIN_H
#define DK_NET_MAIN_H

#include "bflib_basics.h"
#include "bflib_netsession.h"
#include "user_prefs.h"
#include "ver_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIMEOUT_CONNECT_HOLEPUNCH 5000
#define TIMEOUT_CONNECT_DIRECT_IPV6 5000
#define TIMEOUT_CONNECT_DIRECT_IPV4 5000
#define TIMEOUT_JOIN_LOBBY 5000
#define TIMEOUT_LOBBY_EXCHANGE 5000
#define PEER_TIMEOUT_LIMIT 0
#define PEER_TIMEOUT_MS 15000

#define MIN_NET_USERS 2
/* an endless world: a dedicated host and a user for every human seat (players 0-3, 6-7, 10 up: PLAYERS_MAX - 4) */
#define MAX_NET_USERS 101 /* PLAYERS_MAX - 3 (checked in net_main.c; this header comes before globals.h in places) */
/* users in a stock network game or skirmish: the lobby's seats and colours (an endless world takes all of
   MAX_NET_USERS, its seats are players 0-3 and 6-7) */
#define STOCK_NET_USERS 4
#define MAX_NET_SPECTATORS 50
#define MAX_NET_CONNECTIONS (MAX_NET_USERS + MAX_NET_SPECTATORS)
#define SPECTATOR_CONNECT_DATA 0x53504543
/** 64-bit words of a set of network connections (users and spectators). */
#define NET_MASK_WORDS ((MAX_NET_CONNECTIONS + 63) / 64)
/** A set of network connections, one bit each (connection id c: bit c % 64 of word c / 64). */
typedef struct NetUserMask {
    uint64_t w[NET_MASK_WORDS];
} NetUserMask;

static inline NetUserMask numask_none(void)
{
    NetUserMask m;
    for (int i = 0; i < NET_MASK_WORDS; i++)
        m.w[i] = 0;
    return m;
}
static inline int numask_has(NetUserMask m, int id)
{
    if ((id < 0) || (id >= NET_MASK_WORDS * 64))
        return 0;
    return (int)((m.w[id >> 6] >> (id & 63)) & 1u);
}
static inline void numask_add(NetUserMask *m, int id)
{
    if ((id >= 0) && (id < NET_MASK_WORDS * 64))
        m->w[id >> 6] |= (UINT64_C(1) << (id & 63));
}
static inline void numask_del(NetUserMask *m, int id)
{
    if ((id >= 0) && (id < NET_MASK_WORDS * 64))
        m->w[id >> 6] &= ~(UINT64_C(1) << (id & 63));
}
static inline NetUserMask numask_of(int id)
{
    NetUserMask m = numask_none();
    numask_add(&m, id);
    return m;
}
/** Connections from..to-1. */
static inline NetUserMask numask_range(int from, int to)
{
    NetUserMask m = numask_none();
    for (int id = from; id < to; id++)
        numask_add(&m, id);
    return m;
}
static inline int numask_any(NetUserMask m)
{
    for (int i = 0; i < NET_MASK_WORDS; i++)
        if (m.w[i] != 0)
            return 1;
    return 0;
}
static inline int numask_count(NetUserMask m)
{
    int n = 0;
    for (int i = 0; i < NET_MASK_WORDS; i++)
        n += __builtin_popcountll(m.w[i]);
    return n;
}
static inline NetUserMask numask_or(NetUserMask a, NetUserMask b)
{
    for (int i = 0; i < NET_MASK_WORDS; i++)
        a.w[i] |= b.w[i];
    return a;
}
static inline NetUserMask numask_and(NetUserMask a, NetUserMask b)
{
    for (int i = 0; i < NET_MASK_WORDS; i++)
        a.w[i] &= b.w[i];
    return a;
}
static inline NetUserMask numask_andnot(NetUserMask a, NetUserMask b)
{
    for (int i = 0; i < NET_MASK_WORDS; i++)
        a.w[i] &= ~b.w[i];
    return a;
}
#define ALL_NET_USERS_MASK numask_range(0, MAX_NET_USERS)
#define ALL_NET_CONNECTIONS_MASK numask_range(0, MAX_NET_CONNECTIONS)
#define MAX_NET_PEERS (MAX_NET_USERS - 1)
#define SERVER_ID 0
#define SOLO_HUMAN_ID 0 /* human player's user id in non-multiplayer, when relevant */
#define OBSERVER_USER MAX_NET_CONNECTIONS /* dummy user for observers */
#define NET_MSG_BUFFER_SIZE 65536 /* the joiner bootstrap holds every user's startup packet (each with a slot per user) */
#define INVALID_USER_ID 23456

typedef int NetUserId;

enum NetDropReason {
    NETDROP_MANUAL,
    NETDROP_DISCONNECT,
    NETDROP_TIMEOUT,
};

enum NetMessageType {
    NETMSG_LOGIN,
    NETMSG_USERUPDATE,
    NETMSG_FRONTEND,
    NETMSG_CLIENT_IS_READY,
    NETMSG_HOST_DECLARES_START,
    NETMSG_STARTUP_SYNC,
    NETMSG_GAMEPLAY_UNSEQUENCED,
    NETMSG_RESYNC_DATA,
    NETMSG_UNPAUSE,
    NETMSG_CHATMESSAGE,
    NETMSG_GAMEPLAY_REPAIR,
    NETMSG_GAMEPLAY_TURN_SYNC,
    NETMSG_SPECTATOR_TURN,
    NETMSG_SPECTATOR_BOOTSTRAP,
    NETMSG_SPECTATOR_READY,
    NETMSG_SPECTATOR_CHAT,
    NETMSG_SPECTATOR_NOTICE,
    NETMSG_SPECTATOR_CONVERSION,
    NETMSG_LOBBY_SETTINGS,
    NETMSG_SPECTATOR_SEAT,
    NETMSG_STATE_TURN,       /* thin-client play: the server's state record for a turn (net_state.c) */
    NETMSG_STATE_CAPS,       /* thin-client play: what a client reads (u32 NS_CAP_*), after each keyframe (2026-11-09) */
    NETMSG_KEYREQ,           /* endless worlds: a joining client asks for the world's key (net_secure.c) */
    NETMSG_KEYINFO,          /* the server's answer: its public key and a nonce for this login */
};

enum NetConnectionRole {
    NetRole_Player,
    NetRole_Spectator,
};

enum NetSpectatorState {
    NetSpectator_Loading,
    NetSpectator_SnapshotPending,
    NetSpectator_Streaming,
};

typedef void (*NetDropCallback)(NetUserId id, enum NetDropReason reason);

struct NetSP
{
    TbError (*init)(NetDropCallback drop_callback);
    void (*exit)();
    void (*disconnect)();
    TbError (*host)(const char *session, void *options);
    TbError (*join)(const char *session, void *options);
    void (*update)(enum NetJoinRejection (*new_user)(NetUserId *assigned_id, enum NetConnectionRole role));
    void (*sendmsg_single)(NetUserId destination, const char *buffer, size_t size);
    void (*sendmsg_single_unsequenced)(NetUserId destination, const char *buffer, size_t size);
    void (*sendmsg_all)(const char *buffer, size_t size);
    size_t (*msgready)(NetUserId source, unsigned timeout);
    size_t (*peekmsg)(NetUserId source, const char **buffer);
    size_t (*readmsg)(NetUserId source, char *buffer, size_t max_size);
    void (*drop_user)(NetUserId id, enum NetJoinRejection reason);
    void (*reassign_user)(NetUserId old_id, NetUserId new_id);
};

enum NetUserProgress {
    USER_UNUSED = 0,
    USER_CONNECTED,
    USER_LOGGEDIN,
};

struct GameVersionPacket {
    int32_t major;
    int32_t minor;
    int32_t release;
    int32_t build;
};

#define NET_USER_NAME_SIZE 32

struct NetUser {
    NetUserId id;
    char name[NET_USER_NAME_SIZE];
    enum NetUserProgress progress;
    int ack;
    struct GameVersionPacket version;
    uint32_t connected_at;
    enum NetSpectatorState spectator_state;
    uint8_t spectator_icon;
    TbBool spectator_resync_requested;
    uint32_t spectator_resync_requested_at;
    TbBool spectator_conversion_requested;
    uint64_t reconnect_token;
    TbBool seat_requested;
    NetUserId seat_user;
    UserPreferences seat_prefs;
};

struct NetFrame {
    struct NetFrame *next;
    char *buffer;
    int seq_nbr;
    size_t size;
};

struct NetState {
    const struct NetSP *sp;
    struct NetUser users[MAX_NET_CONNECTIONS];
    TbBool users_timed_out[MAX_NET_CONNECTIONS];
    enum NetJoinRejection users_leave_reason[MAX_NET_CONNECTIONS];
    uint64_t reconnect_tokens[MAX_NET_USERS];
    enum NetJoinRejection leave_reason;
    struct NetFrame *exchg_queue;
    char password[32];
    NetUserId my_id;
    int seq_nbr;
    unsigned max_users;
    uint32_t gameplay_generation;
    char msg_buffer[NET_MSG_BUFFER_SIZE];
    char msg_buffer_null;
    TbBool resync_pending;
    TbBool simulating_disconnect;
    NetUserId pending_spectator_id;
    TbBool locked;
    enum NetSessionPhase phase;
};

struct TbNetworkUserInfo {
    char name[NET_USER_NAME_SIZE];
    int32_t network_user_active;
    uint32_t connection_id;
};

struct ServiceInitData {
    int32_t service_flags;
    int32_t max_connections;
    int32_t buffer_size;
    int32_t timeout_value;
};

enum TbNetworkService {
    NS_ENET_UDP,
};

extern struct NetState netstate;
extern enum NetConnectionRole net_join_role;

static const struct GameVersionPacket net_current_version = { VER_MAJOR, VER_MINOR, VER_RELEASE, VER_BUILD };

static inline TbBool net_versions_match(const struct GameVersionPacket *version_a, const struct GameVersionPacket *version_b)
{
    return (version_a->major == version_b->major) &&
        (version_a->minor == version_b->minor) &&
        (version_a->release == version_b->release) &&
        (version_a->build == version_b->build);
}

TbError LbNetwork_Init(uint32_t srvcindex, uint32_t maxplayrs, struct TbNetworkUserInfo *locplayr, struct ServiceInitData *init_data);
enum NetJoinRejection OnNewUser(NetUserId *assigned_id, enum NetConnectionRole role);
void OnDroppedUser(NetUserId id, enum NetDropReason reason);
TbBool IsUserActive(NetUserId id);
NetUserId find_unused_user_id(NetUserId first_id, NetUserId last_id);
int32_t GetRemoteUserCount(void);
void UpdateLocalPlayerInfo(NetUserId id);
char *begin_net_message(enum NetMessageType msg_type);
void send_message_buffer(NetUserId dest, const char *end_ptr);
void send_remote_buffer(const char *end_ptr);
void SendUserUpdate(NetUserId dest, NetUserId updated_user);
void SendNewUserUpdates(NetUserId new_user);

#ifdef __cplusplus
}
#endif

#endif

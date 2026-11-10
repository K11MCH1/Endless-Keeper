/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_net.h
 *     Endless worlds: the world list (servers) and the menu chat (stage 3.5).
 * @par Purpose:
 *     A hub keeps the list of servers and the chat of the World List menu. A dedicated server runs one; a
 *     Cloudflare Worker can run one too. Game servers register
 *     with the hub; players in the menu get the list, the chat and the names of everyone in the menu, and measure
 *     each server's ping themselves on the server's game port.
 */
/******************************************************************************/
#ifndef DK_WORLD_NET_H
#define DK_WORLD_NET_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define WNET_URL_MAX       256
#define WNET_NAME_MAX      21  /* 20 characters */
#define WNET_TITLE_MAX     41  /* 40 characters */
#define WNET_TEXT_MAX      201 /* 200 characters */
#define WNET_HOST_MAX      64
#define WNET_SERVERS_MAX   64
#define WNET_NAMES_MAX     128
#define WNET_CHAT_MAX      50
#define WNET_HUB_PORT      5560
/* the public World List (2026-11-10): the Cloudflare Worker in tools/keeperfx-world-hub, the default hub when
   WORLD_LIST_SERVER isn't set (a server's own hub: ws://127.0.0.1:5560/hub) */
#define WNET_PUBLIC_HUB    "wss://world.endless-keeper.workers.dev/hub"

struct WnetServer {
    char id[16];
    char title[WNET_TITLE_MAX];
    char host[WNET_HOST_MAX];  /* where to connect (the hub's host when the server is on the hub's machine) */
    int port;
    int players;
    int max;
    int ping_ms;               /* -1: no answer yet */
};

struct WnetChatLine {
    char name[WNET_NAME_MAX];
    char text[WNET_TEXT_MAX];
};

/** The hub address (WORLD_LIST_SERVER in keeperfx.cfg; -worldlist on the command line). */
extern char world_list_server[WNET_URL_MAX];
void world_net_set_list_server(const char *url);
/** -worldlist was given: WORLD_LIST_SERVER in keeperfx.cfg (read after the command line) leaves the address alone. */
extern TbBool world_list_server_from_cmdline;

/* The World List menu: this game as a player in the menu. */
void wnet_menu_open(const char *player_name);
void wnet_menu_close(void);
void wnet_menu_poll(void);
/** A line for the status bar: connecting, connected (with the name in use), or why not. */
const char *wnet_menu_status(void);
/** 0 connecting, 1 connected, 2 can't reach (trying again). */
int wnet_menu_state(void);
/* The World List's own hub (its Server box, 2026-11-09): "" means world_list_server. Kept in settings.toml. */
void wnet_menu_set_hub(const char *url);
const char *wnet_menu_hub_setting(void);
/** The hub the World List connects to. */
const char *wnet_menu_hub(void);
int wnet_menu_server_count(void);
const struct WnetServer *wnet_menu_server(int i);
int wnet_menu_name_count(void);
const char *wnet_menu_name(int i);
int wnet_menu_chat_count(void);
const struct WnetChatLine *wnet_menu_chat_line(int i);
/** Bumped whenever the chat gets a line, so the menu can follow it. */
unsigned long wnet_menu_chat_serial(void);
void wnet_menu_say(const char *text);

/* Parties (2026-11-09): two players in the menu group up, ahead of playing together. The hub relays the invites and
   keeps the parties; a party ends when either player leaves it or the World List. Only parties of two for now. */
/** Connected to the hub, with a name from it. */
TbBool wnet_menu_connected(void);
/** This player's name on the hub ("" until it has one). */
const char *wnet_menu_my_name(void);
/** The other player in this player's party ("" when not in one). */
const char *wnet_menu_party(void);
/** The player whose invite waits for this player's answer ("" when none). */
const char *wnet_menu_invite_from(void);
/** The player this one invited, while the answer is awaited ("" when none). */
const char *wnet_menu_invite_to(void);
void wnet_menu_party_invite(const char *name);
void wnet_menu_party_answer(TbBool accept);
void wnet_menu_party_leave(void);
/* Co-op (2026-11-10): a party enters a world together. The hub gives each party a key; the player who joins a world
   tells the hub ("go"), which tells the partner; both games give the key to the world's server with their login, and
   the server makes them allied keepers. */
/** This player (in a party) enters the world with this id: the partner is told to follow. Returns the party's key
 *  for the login (0: no party, or an older hub). The party is over in the menu. */
uint64_t wnet_menu_party_go(const char *server_id);
/** The partner entered a world: its row in the list (-1: none, or it isn't in this list) and the party's key. Asked
 *  once: the menu joins it. */
int wnet_menu_party_follow(uint64_t *key);

/* This game as a server: it registers with the hub (and with its own hub, if it runs one). */
void wnet_server_begin(const char *title, int port);
/** Each server turn: the players seated and the most that fit. */
void wnet_server_tick(int players, int max);
void wnet_server_end(void);

/** Command line: -title (the world's name in the list), -hubport, -nohub. */
extern char wnet_title[WNET_TITLE_MAX];
extern int wnet_hub_port_wanted;
extern TbBool wnet_hub_off;

/* The hub: a dedicated server runs one (-nohub turns it off; -hubport picks the port). */
TbBool wnet_hub_start(int port);
void wnet_hub_poll(void);
void wnet_hub_stop(void);
TbBool wnet_hub_running(void);
int wnet_hub_port(void);

/** The game port's packet intercept (an ENetHost *): answers a world list ping. 1 when it was one. */
int wnet_game_port_intercept(void *enet_host);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

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

/* The World List menu: this game as a player in the menu. */
void wnet_menu_open(const char *player_name);
void wnet_menu_close(void);
void wnet_menu_poll(void);
/** A line for the status bar: connecting, connected (with the name in use), or why not. */
const char *wnet_menu_status(void);
int wnet_menu_server_count(void);
const struct WnetServer *wnet_menu_server(int i);
int wnet_menu_name_count(void);
const char *wnet_menu_name(int i);
int wnet_menu_chat_count(void);
const struct WnetChatLine *wnet_menu_chat_line(int i);
/** Bumped whenever the chat gets a line, so the menu can follow it. */
unsigned long wnet_menu_chat_serial(void);
void wnet_menu_say(const char *text);

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

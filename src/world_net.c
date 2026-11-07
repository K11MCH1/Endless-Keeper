/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_net.c
 *     Endless worlds: the world list (servers) and the menu chat.
 * @par Purpose:
 *     The protocol: one WebSocket per client, JSON text
 *     frames. This file has the three ends of it:
 *     - the client (libcurl's WebSocket): a player in the World List menu, or a game server registering;
 *     - the hub (a small WebSocket server over ENet's sockets), which a dedicated server runs;
 *     - the ping, measured by each player's game on each server's own game port (UDP).
 */
/******************************************************************************/
#include "pre_inc.h"

#include <SDL3/SDL.h>
#ifndef _WIN32
#include <sys/select.h>
#endif
#include <curl/curl.h>
#include <curl/websockets.h>
#include <json-dom.h>
#include <enet6/enet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "world_net.h"
#include "bflib_basics.h"
#include "bflib_netsession.h"
#include "ver_defs.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
char world_list_server[WNET_URL_MAX] = "ws://127.0.0.1:5560/hub";
char wnet_title[WNET_TITLE_MAX] = "";
int wnet_hub_port_wanted = WNET_HUB_PORT;
TbBool wnet_hub_off = false;

#define WS_MSG_MAX        262144
#define WS_CONNECT_MS     5000
#define WS_RETRY_MS       5000
#define WS_PING_MS        30000
#define SERVER_UPDATE_MS  50000  /* a Worker drops a server it hasn't heard from in 120 s */
#define PING_EVERY_MS     2000
#define PING_MAGIC        "KFXWPING"
#define PONG_MAGIC        "KFXWPONG"

static Uint32 now_ms(void)
{
    return (Uint32)SDL_GetTicks();
}

void world_net_set_list_server(const char *url)
{
    if ((url == NULL) || (url[0] == '\0'))
        return;
    snprintf(world_list_server, sizeof(world_list_server), "%s", url);
}

/** Copies a JSON string field, cut to the size. */
static void json_copy(char *dst, size_t size, const VALUE *obj, const char *key)
{
    const char *s = value_string(value_dict_get(obj, key));
    snprintf(dst, size, "%s", (s != NULL) ? s : "");
}

static int json_int(const VALUE *obj, const char *key, int dflt)
{
    const VALUE *v = value_dict_get(obj, key);
    return (v != NULL) ? (int)value_int32(v) : dflt;
}

/** The host part of a ws:// or wss:// URL. */
static void url_host(const char *url, char *host, size_t size)
{
    const char *p = strstr(url, "://");
    p = (p != NULL) ? p + 3 : url;
    size_t n = 0;
    if (*p == '[')
    {
        p++;
        while ((p[n] != '\0') && (p[n] != ']'))
            n++;
    } else
    {
        while ((p[n] != '\0') && (p[n] != ':') && (p[n] != '/'))
            n++;
    }
    if (n >= size)
        n = size - 1;
    memcpy(host, p, n);
    host[n] = '\0';
}

/******************************************************************************/
/* The client: libcurl's WebSocket. The connection is made on a thread (it may take seconds); after that the main
   thread sends and receives without blocking. */
enum WsState { WsS_Idle = 0, WsS_Connecting, WsS_Open, WsS_Failed };

struct WsClient {
    char url[WNET_URL_MAX];
    CURL *curl;
    SDL_Thread *thread;
    SDL_AtomicInt state;
    char *rbuf;
    size_t rlen;
    Uint32 retry_at;
    Uint32 last_ping;
    TbBool hello_sent;
    char error[96];
    unsigned int fails; /* tries in a row that failed (for the log) */
};

static int ws_connect_thread(void *data)
{
    struct WsClient *c = (struct WsClient *)data;
    CURL *curl = curl_easy_init();
    if (curl == NULL)
    {
        SDL_SetAtomicInt(&c->state, WsS_Failed);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, c->url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)WS_CONNECT_MS);
    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        snprintf(c->error, sizeof(c->error), "%s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        SDL_SetAtomicInt(&c->state, WsS_Failed);
        return -1;
    }
    c->curl = curl;
    SDL_SetAtomicInt(&c->state, WsS_Open);
    return 0;
}

static void ws_close(struct WsClient *c)
{
    if (c->thread != NULL)
    {
        SDL_WaitThread(c->thread, NULL);
        c->thread = NULL;
    }
    if (c->curl != NULL)
    {
        curl_easy_cleanup(c->curl);
        c->curl = NULL;
    }
    free(c->rbuf);
    c->rbuf = NULL;
    c->rlen = 0;
    c->hello_sent = false;
    SDL_SetAtomicInt(&c->state, WsS_Idle);
}

/** Keeps the connection going: starts it, reaps the connecting thread, and tries again after a failure. */
static void ws_maintain(struct WsClient *c, const char *url)
{
    const int state = SDL_GetAtomicInt(&c->state);
    if (state == WsS_Connecting)
        return;
    if ((c->thread != NULL) && (state != WsS_Connecting))
    {
        SDL_WaitThread(c->thread, NULL);
        c->thread = NULL;
        if (state == WsS_Failed)
        {
            if ((c->fails % 60) == 0)
                LbNetLog("World list: can't reach %s (%s)%s\n", c->url, c->error, (c->fails > 0) ? ", still" : "");
            c->fails++;
            c->retry_at = now_ms() + WS_RETRY_MS;
        } else
        {
            LbNetLog("World list: connected to %s\n", c->url);
            c->fails = 0;
        }
    }
    if (((state == WsS_Idle) || (state == WsS_Failed)) && ((int32_t)(now_ms() - c->retry_at) >= 0))
    {
        snprintf(c->url, sizeof(c->url), "%s", url);
        c->error[0] = '\0';
        c->hello_sent = false;
        c->last_ping = now_ms();
        SDL_SetAtomicInt(&c->state, WsS_Connecting);
        c->thread = SDL_CreateThread(ws_connect_thread, "world_list_connect", c);
        if (c->thread == NULL)
        {
            SDL_SetAtomicInt(&c->state, WsS_Failed);
            c->retry_at = now_ms() + WS_RETRY_MS;
        }
    }
}

static TbBool ws_open(struct WsClient *c)
{
    return (SDL_GetAtomicInt(&c->state) == WsS_Open) && (c->thread == NULL) && (c->curl != NULL);
}

/** The connection broke: close it, try again later. */
static void ws_broken(struct WsClient *c, const char *why)
{
    LbNetLog("World list: connection to %s lost (%s)\n", c->url, why);
    ws_close(c);
    SDL_SetAtomicInt(&c->state, WsS_Failed);
    snprintf(c->error, sizeof(c->error), "%s", why);
    c->retry_at = now_ms() + WS_RETRY_MS;
}

static void ws_send(struct WsClient *c, const char *text)
{
    if (!ws_open(c))
        return;
    size_t remaining = strlen(text);
    const Uint32 deadline = now_ms() + 300;
    while (remaining > 0)
    {
        size_t sent = 0;
        CURLcode res = curl_ws_send(c->curl, text, remaining, &sent, 0, CURLWS_TEXT);
        if ((res != CURLE_OK) && (res != CURLE_AGAIN))
        {
            ws_broken(c, curl_easy_strerror(res));
            return;
        }
        text += sent;
        remaining -= sent;
        if (remaining == 0)
            break;
        if ((int32_t)(now_ms() - deadline) >= 0)
        {
            ws_broken(c, "send timed out");
            return;
        }
        SDL_Delay(1);
    }
}

/** One whole text message into out (NUL-terminated); 0 when none waits. */
static size_t ws_recv(struct WsClient *c, char **out)
{
    if (!ws_open(c))
        return 0;
    if (c->rbuf == NULL)
    {
        c->rbuf = (char *)malloc(WS_MSG_MAX);
        if (c->rbuf == NULL)
            return 0;
        c->rlen = 0;
    }
    for (;;)
    {
        size_t got = 0;
        const struct curl_ws_frame *frame = NULL;
        CURLcode res = curl_ws_recv(c->curl, c->rbuf + c->rlen, WS_MSG_MAX - c->rlen - 1, &got, &frame);
        if (res == CURLE_AGAIN)
            return 0;
        if ((res != CURLE_OK) || (frame == NULL) || (frame->flags & CURLWS_CLOSE))
        {
            ws_broken(c, (res != CURLE_OK) ? curl_easy_strerror(res) : "closed by the hub");
            return 0;
        }
        if (frame->flags & (CURLWS_PING | CURLWS_PONG))
            continue;
        c->rlen += got;
        if (c->rlen >= WS_MSG_MAX - 1)
        {
            ws_broken(c, "message too long");
            return 0;
        }
        if (frame->bytesleft || (frame->flags & CURLWS_CONT))
            continue;
        const size_t len = c->rlen;
        c->rbuf[len] = '\0';
        c->rlen = 0;
        *out = c->rbuf;
        return len;
    }
}

static void ws_keepalive(struct WsClient *c)
{
    if (ws_open(c) && ((int32_t)(now_ms() - c->last_ping) >= WS_PING_MS))
    {
        c->last_ping = now_ms();
        ws_send(c, "{\"t\":\"ping\"}");
    }
}

/******************************************************************************/
/* A player in the World List menu */
static struct {
    TbBool on;
    struct WsClient ws;
    char want_name[WNET_NAME_MAX];
    char name[WNET_NAME_MAX];
    char hub_host[WNET_HOST_MAX];
    struct WnetServer servers[WNET_SERVERS_MAX];
    int server_count;
    char names[WNET_NAMES_MAX][WNET_NAME_MAX];
    int name_count;
    struct WnetChatLine chat[WNET_CHAT_MAX];
    int chat_count;
    unsigned long chat_serial;
    char status[160];
    /* pings: one UDP socket, a request to each server every 2 s */
    ENetSocket udp;
    Uint32 last_ping_round;
    uint32_t ping_seq;
    struct { char host[WNET_HOST_MAX]; int port; ENetAddress addr; TbBool resolved; uint32_t seq; Uint32 sent; } pinged[WNET_SERVERS_MAX];
} WM;

static void menu_add_chat(const char *name, const char *text)
{
    if (WM.chat_count == WNET_CHAT_MAX)
    {
        memmove(&WM.chat[0], &WM.chat[1], sizeof(WM.chat[0]) * (WNET_CHAT_MAX - 1));
        WM.chat_count--;
    }
    struct WnetChatLine *line = &WM.chat[WM.chat_count++];
    snprintf(line->name, sizeof(line->name), "%s", name);
    snprintf(line->text, sizeof(line->text), "%s", text);
    WM.chat_serial++;
}

static int menu_ping_slot(const char *host, int port)
{
    for (int i = 0; i < WNET_SERVERS_MAX; i++)
        if (WM.pinged[i].port == port && strcmp(WM.pinged[i].host, host) == 0)
            return i;
    return -1;
}

static void menu_read_servers(const VALUE *msg)
{
    const VALUE *list = value_dict_get(msg, "list");
    if (value_type(list) != VALUE_ARRAY)
        return;
    struct WnetServer old[WNET_SERVERS_MAX];
    const int old_count = WM.server_count;
    memcpy(old, WM.servers, sizeof(old));
    WM.server_count = 0;
    for (int i = 0; (i < (int)value_array_size(list)) && (WM.server_count < WNET_SERVERS_MAX); i++)
    {
        const VALUE *s = value_array_get(list, i);
        struct WnetServer *sv = &WM.servers[WM.server_count];
        memset(sv, 0, sizeof(*sv));
        json_copy(sv->id, sizeof(sv->id), s, "id");
        json_copy(sv->title, sizeof(sv->title), s, "title");
        json_copy(sv->host, sizeof(sv->host), s, "host");
        if (sv->host[0] == '\0')
            snprintf(sv->host, sizeof(sv->host), "%s", WM.hub_host); /* on the hub's own machine */
        sv->port = json_int(s, "port", 5556);
        sv->players = json_int(s, "players", 0);
        sv->max = json_int(s, "max", 0);
        sv->ping_ms = -1;
        for (int k = 0; k < old_count; k++)
            if ((strcmp(old[k].host, sv->host) == 0) && (old[k].port == sv->port))
                sv->ping_ms = old[k].ping_ms;
        WM.server_count++;
    }
}

static void menu_handle(const char *text, size_t len)
{
    VALUE msg = {0};
    if (json_dom_parse(text, len, NULL, 0, &msg, NULL) != 0)
    {
        value_fini(&msg);
        return;
    }
    const char *t = value_string(value_dict_get(&msg, "t"));
    if (t == NULL)
        t = "";
    if (strcmp(t, "welcome") == 0)
    {
        json_copy(WM.name, sizeof(WM.name), &msg, "name");
    } else if (strcmp(t, "servers") == 0)
    {
        menu_read_servers(&msg);
    } else if (strcmp(t, "names") == 0)
    {
        const VALUE *list = value_dict_get(&msg, "list");
        WM.name_count = 0;
        for (int i = 0; (value_type(list) == VALUE_ARRAY) && (i < (int)value_array_size(list)) && (WM.name_count < WNET_NAMES_MAX); i++)
        {
            const char *n = value_string(value_array_get(list, i));
            snprintf(WM.names[WM.name_count++], WNET_NAME_MAX, "%s", (n != NULL) ? n : "");
        }
    } else if (strcmp(t, "history") == 0)
    {
        const VALUE *lines = value_dict_get(&msg, "lines");
        WM.chat_count = 0;
        for (int i = 0; (value_type(lines) == VALUE_ARRAY) && (i < (int)value_array_size(lines)); i++)
        {
            const VALUE *l = value_array_get(lines, i);
            const char *n = value_string(value_dict_get(l, "name"));
            const char *x = value_string(value_dict_get(l, "text"));
            menu_add_chat((n != NULL) ? n : "", (x != NULL) ? x : "");
        }
    } else if (strcmp(t, "chat") == 0)
    {
        const char *n = value_string(value_dict_get(&msg, "name"));
        const char *x = value_string(value_dict_get(&msg, "text"));
        menu_add_chat((n != NULL) ? n : "", (x != NULL) ? x : "");
    } else if (strcmp(t, "error") == 0)
    {
        const char *r = value_string(value_dict_get(&msg, "reason"));
        LbNetLog("World list: the hub says: %s\n", (r != NULL) ? r : "error");
    }
    value_fini(&msg);
}

static void menu_pings(void)
{
    if (WM.udp == ENET_SOCKET_NULL)
    {
        WM.udp = enet_socket_create(ENET_ADDRESS_TYPE_IPV4, ENET_SOCKET_TYPE_DATAGRAM);
        if (WM.udp == ENET_SOCKET_NULL)
            return;
        enet_socket_set_option(WM.udp, ENET_SOCKOPT_NONBLOCK, 1);
        ENetAddress any;
        enet_address_build_any(&any, ENET_ADDRESS_TYPE_IPV4);
        any.port = 0;
        enet_socket_bind(WM.udp, &any);
    }
    /* answers */
    for (;;)
    {
        enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
        if ((enet_socket_wait(WM.udp, &wait, 0) < 0) || !(wait & ENET_SOCKET_WAIT_RECEIVE))
            break;
        unsigned char buf[64];
        ENetBuffer b = {.data = buf, .dataLength = sizeof(buf)};
        ENetAddress from;
        const int n = enet_socket_receive(WM.udp, &from, &b, 1);
        if (n <= 0)
            break;
        if ((n < 16) || (memcmp(buf, PONG_MAGIC, 8) != 0))
            continue;
        uint32_t seq;
        memcpy(&seq, buf + 8, 4);
        for (int i = 0; i < WNET_SERVERS_MAX; i++)
        {
            if (!WM.pinged[i].resolved || (WM.pinged[i].seq != seq) || (WM.pinged[i].sent == 0))
                continue;
            const int ms = (int)(now_ms() - WM.pinged[i].sent);
            WM.pinged[i].sent = 0;
            for (int s = 0; s < WM.server_count; s++)
            {
                if ((WM.servers[s].port == WM.pinged[i].port) && (strcmp(WM.servers[s].host, WM.pinged[i].host) == 0))
                {
                    WM.servers[s].ping_ms = ms;
                    WM.servers[s].players = buf[12]; /* fresher than the hub's */
                    WM.servers[s].max = buf[13];
                }
            }
        }
    }
    /* requests, every 2 s */
    if ((int32_t)(now_ms() - WM.last_ping_round) < PING_EVERY_MS)
        return;
    WM.last_ping_round = now_ms();
    for (int s = 0; s < WM.server_count; s++)
    {
        int i = menu_ping_slot(WM.servers[s].host, WM.servers[s].port);
        if (i < 0)
        {
            for (i = 0; i < WNET_SERVERS_MAX; i++)
                if (WM.pinged[i].port == 0)
                    break;
            if (i == WNET_SERVERS_MAX)
                continue;
            snprintf(WM.pinged[i].host, sizeof(WM.pinged[i].host), "%s", WM.servers[s].host);
            WM.pinged[i].port = WM.servers[s].port;
            WM.pinged[i].resolved = (enet_address_set_host(&WM.pinged[i].addr, ENET_ADDRESS_TYPE_IPV4, WM.servers[s].host) == 0);
            WM.pinged[i].addr.port = (enet_uint16)WM.servers[s].port;
        }
        if (!WM.pinged[i].resolved)
            continue;
        if (WM.pinged[i].sent != 0)
            WM.servers[s].ping_ms = -1; /* the last one went unanswered */
        unsigned char req[12];
        memcpy(req, PING_MAGIC, 8);
        WM.pinged[i].seq = ++WM.ping_seq;
        memcpy(req + 8, &WM.pinged[i].seq, 4);
        ENetBuffer b = {.data = req, .dataLength = sizeof(req)};
        WM.pinged[i].sent = now_ms();
        if (WM.pinged[i].sent == 0)
            WM.pinged[i].sent = 1;
        enet_socket_send(WM.udp, &WM.pinged[i].addr, &b, 1);
    }
}

void wnet_menu_open(const char *player_name)
{
    wnet_menu_close();
    memset(&WM, 0, sizeof(WM));
    WM.udp = ENET_SOCKET_NULL;
    WM.on = true;
    snprintf(WM.want_name, sizeof(WM.want_name), "%s", ((player_name != NULL) && (player_name[0] != '\0')) ? player_name : "Keeper");
    url_host(world_list_server, WM.hub_host, sizeof(WM.hub_host));
    SDL_SetAtomicInt(&WM.ws.state, WsS_Idle);
    LbNetLog("World list: menu open, hub %s, name %s\n", world_list_server, WM.want_name);
}

void wnet_menu_close(void)
{
    if (!WM.on)
        return;
    ws_close(&WM.ws);
    if (WM.udp != ENET_SOCKET_NULL)
        enet_socket_destroy(WM.udp);
    WM.udp = ENET_SOCKET_NULL;
    WM.on = false;
}

void wnet_menu_poll(void)
{
    if (!WM.on)
        return;
    ws_maintain(&WM.ws, world_list_server);
    if (ws_open(&WM.ws))
    {
        if (!WM.ws.hello_sent)
        {
            char esc[WNET_NAME_MAX * 6 + 1];
            char msg[512];
            net_json_escape(esc, sizeof(esc), WM.want_name);
            snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"role\":\"player\",\"name\":\"%s\",\"build\":\"%s\"}", esc, VER_STRING);
            WM.ws.hello_sent = true;
            ws_send(&WM.ws, msg);
        }
        char *text = NULL;
        size_t len;
        while ((len = ws_recv(&WM.ws, &text)) > 0)
            menu_handle(text, len);
        ws_keepalive(&WM.ws);
    }
    const int state = SDL_GetAtomicInt(&WM.ws.state);
    if (ws_open(&WM.ws))
    {
        /* the hub's name for this player, when it isn't the one asked for (the name was taken: " (2)") */
        if ((WM.name[0] != '\0') && (strcmp(WM.name, WM.want_name) != 0))
            snprintf(WM.status, sizeof(WM.status), "World list: %s (you are %s)", world_list_server, WM.name);
        else
            snprintf(WM.status, sizeof(WM.status), "World list: %s", world_list_server);
    }
    else if ((state == WsS_Connecting) && (WM.ws.fails == 0))
        snprintf(WM.status, sizeof(WM.status), "Connecting to %s...", world_list_server);
    else
        snprintf(WM.status, sizeof(WM.status), "Can't reach %s: trying again", world_list_server);
    menu_pings();
}

const char *wnet_menu_status(void) { return WM.status; }
int wnet_menu_server_count(void) { return WM.on ? WM.server_count : 0; }
const struct WnetServer *wnet_menu_server(int i) { return ((i >= 0) && (i < WM.server_count)) ? &WM.servers[i] : NULL; }
int wnet_menu_name_count(void) { return WM.on ? WM.name_count : 0; }
const char *wnet_menu_name(int i) { return ((i >= 0) && (i < WM.name_count)) ? WM.names[i] : ""; }
int wnet_menu_chat_count(void) { return WM.on ? WM.chat_count : 0; }
const struct WnetChatLine *wnet_menu_chat_line(int i) { return ((i >= 0) && (i < WM.chat_count)) ? &WM.chat[i] : NULL; }
unsigned long wnet_menu_chat_serial(void) { return WM.chat_serial; }

void wnet_menu_say(const char *text)
{
    if (!WM.on || (text == NULL) || (text[0] == '\0'))
        return;
    char cut[WNET_TEXT_MAX];
    snprintf(cut, sizeof(cut), "%s", text);
    char esc[WNET_TEXT_MAX * 6 + 1];
    net_json_escape(esc, sizeof(esc), cut);
    char msg[WNET_TEXT_MAX * 6 + 64];
    snprintf(msg, sizeof(msg), "{\"t\":\"say\",\"text\":\"%s\"}", esc);
    ws_send(&WM.ws, msg);
}

/******************************************************************************/
/* A game server, registered with the configured hub and with its own (if it runs one) */
#define REG_MAX 2
static struct {
    TbBool on;
    char title[WNET_TITLE_MAX];
    int port;
    int players, max;
    struct { struct WsClient ws; char url[WNET_URL_MAX]; int sent_players, sent_max; Uint32 last_update; TbBool used; } reg[REG_MAX];
} WS;

/* what a ping on the game port is answered with */
static volatile int status_players = 0, status_max = 0;

static void reg_add(const char *url)
{
    for (int i = 0; i < REG_MAX; i++)
        if (WS.reg[i].used && (strcmp(WS.reg[i].url, url) == 0))
            return;
    for (int i = 0; i < REG_MAX; i++)
    {
        if (WS.reg[i].used)
            continue;
        memset(&WS.reg[i], 0, sizeof(WS.reg[i]));
        WS.reg[i].used = true;
        snprintf(WS.reg[i].url, sizeof(WS.reg[i].url), "%s", url);
        SDL_SetAtomicInt(&WS.reg[i].ws.state, WsS_Idle);
        return;
    }
}

void wnet_server_begin(const char *title, int port)
{
    wnet_server_end();
    memset(&WS, 0, sizeof(WS));
    WS.on = true;
    snprintf(WS.title, sizeof(WS.title), "%s", ((title != NULL) && (title[0] != '\0')) ? title : "Endless world");
    WS.port = port;
    if (wnet_hub_running())
    {
        char own[WNET_URL_MAX];
        snprintf(own, sizeof(own), "ws://127.0.0.1:%d/hub", wnet_hub_port());
        reg_add(own);
    }
    if (world_list_server[0] != '\0')
        reg_add(world_list_server);
    LbNetLog("World list: this server is \"%s\" on port %d\n", WS.title, port);
}

void wnet_server_tick(int players, int max)
{
    status_players = players;
    status_max = max;
    if (!WS.on)
        return;
    WS.players = players;
    WS.max = max;
    for (int i = 0; i < REG_MAX; i++)
    {
        if (!WS.reg[i].used)
            continue;
        struct WsClient *c = &WS.reg[i].ws;
        ws_maintain(c, WS.reg[i].url);
        if (!ws_open(c))
            continue;
        char *text = NULL;
        while (ws_recv(c, &text) > 0)
            ; /* "registered" and "pong": nothing to do */
        if (!ws_open(c))
            continue;
        if (!c->hello_sent)
        {
            char esc[WNET_TITLE_MAX * 6 + 1];
            char msg[512];
            net_json_escape(esc, sizeof(esc), WS.title);
            snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"role\":\"server\",\"title\":\"%s\",\"port\":%d,\"players\":%d,\"max\":%d,\"build\":\"%s\"}",
                esc, WS.port, players, max, VER_STRING);
            c->hello_sent = true;
            WS.reg[i].sent_players = players;
            WS.reg[i].sent_max = max;
            WS.reg[i].last_update = now_ms();
            ws_send(c, msg);
            continue;
        }
        if ((players != WS.reg[i].sent_players) || (max != WS.reg[i].sent_max) ||
            ((int32_t)(now_ms() - WS.reg[i].last_update) >= SERVER_UPDATE_MS))
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "{\"t\":\"update\",\"players\":%d,\"max\":%d}", players, max);
            WS.reg[i].sent_players = players;
            WS.reg[i].sent_max = max;
            WS.reg[i].last_update = now_ms();
            ws_send(c, msg);
        }
        ws_keepalive(c);
    }
}

void wnet_server_end(void)
{
    if (!WS.on)
        return;
    for (int i = 0; i < REG_MAX; i++)
        if (WS.reg[i].used)
            ws_close(&WS.reg[i].ws);
    WS.on = false;
}

/******************************************************************************/
/* The hub: a WebSocket server over ENet's TCP sockets, polled from the game loop */
#define HUB_CLIENTS_MAX 200
#define HUB_IN_MAX      16384
#define HUB_HISTORY     WNET_CHAT_MAX

enum HubRole { HubR_None = 0, HubR_Player, HubR_Server };

struct HubClient {
    ENetSocket s;
    TbBool used;
    TbBool upgraded;
    TbBool closing;
    char ip[WNET_HOST_MAX];
    unsigned char in[HUB_IN_MAX];
    size_t inlen;
    unsigned char *out;
    size_t outlen, outcap;
    int role;
    char name[WNET_NAME_MAX];
    char id[16];
    char title[WNET_TITLE_MAX];
    char host[WNET_HOST_MAX];
    int port, players, max;
    char build[24];
    Uint32 say_times[5];
    int say_pos;
    Uint32 accepted;          /* when it connected: the handshake must come within HUB_HANDSHAKE_MS */
};
#define HUB_HANDSHAKE_MS 10000
#define HUB_OUT_MAX (1u << 20)  /* a client that doesn't read is let go past this */

/** Control characters (and DEL) become spaces: names, chat and titles are shown as text, and each control character
 *  took 6 bytes in JSON. */
static void hub_clean(char *s)
{
    for (; *s != '\0'; s++)
        if (((unsigned char)*s < 0x20) || ((unsigned char)*s == 0x7F))
            *s = ' ';
}

static struct {
    TbBool on;
    int port;
    ENetSocket listen;
    struct HubClient c[HUB_CLIENTS_MAX];
    struct { char name[WNET_NAME_MAX]; char text[WNET_TEXT_MAX]; long long time; } history[HUB_HISTORY];
    int history_count;
    TbBool servers_dirty;
    Uint32 servers_sent;
    int next_id;
} H;

TbBool wnet_hub_running(void) { return H.on; }
int wnet_hub_port(void) { return H.port; }

/* SHA-1 (for the handshake's Sec-WebSocket-Accept) */
struct Sha1 { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t blen; };
static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1_block(struct Sha1 *s, const unsigned char *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1(const unsigned char *data, size_t len, unsigned char out[20])
{
    struct Sha1 s = {{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}, 0, {0}, 0};
    s.len = (uint64_t)len * 8;
    while (len >= 64) { sha1_block(&s, data); data += 64; len -= 64; }
    unsigned char tail[128] = {0};
    memcpy(tail, data, len);
    tail[len] = 0x80;
    const size_t tl = (len < 56) ? 64 : 128;
    for (int i = 0; i < 8; i++)
        tail[tl - 1 - i] = (unsigned char)(s.len >> (8 * i));
    sha1_block(&s, tail);
    if (tl == 128)
        sha1_block(&s, tail + 64);
    for (int i = 0; i < 5; i++)
    {
        out[4 * i] = (unsigned char)(s.h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s.h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s.h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s.h[i];
    }
}
static void base64(const unsigned char *in, size_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3)
    {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((i + 1 < len) ? (uint32_t)in[i + 1] << 8 : 0) | ((i + 2 < len) ? in[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

static void hub_queue(struct HubClient *c, const void *data, size_t len)
{
    if (c->closing && !c->upgraded)
        return;
    if (c->outlen + len > c->outcap)
    {
        size_t cap = (c->outcap == 0) ? 4096 : c->outcap;
        while (cap < c->outlen + len)
            cap *= 2;
        if (cap > HUB_OUT_MAX)
        {
            c->closing = true; /* a client that doesn't read: let it go */
            return;
        }
        unsigned char *nb = (unsigned char *)realloc(c->out, cap);
        if (nb == NULL)
        {
            c->closing = true;
            return;
        }
        c->out = nb;
        c->outcap = cap;
    }
    memcpy(c->out + c->outlen, data, len);
    c->outlen += len;
}

/** A text frame (servers send unmasked). */
static void hub_send_text(struct HubClient *c, const char *text)
{
    if (!c->used || !c->upgraded)
        return;
    const size_t len = strlen(text);
    unsigned char hdr[10];
    size_t hl = 0;
    hdr[hl++] = 0x81;
    if (len < 126)
        hdr[hl++] = (unsigned char)len;
    else if (len < 65536)
    {
        hdr[hl++] = 126;
        hdr[hl++] = (unsigned char)(len >> 8);
        hdr[hl++] = (unsigned char)len;
    } else
    {
        hdr[hl++] = 127;
        for (int i = 7; i >= 0; i--)
            hdr[hl++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    hub_queue(c, hdr, hl);
    hub_queue(c, text, len);
}

static void hub_send_names_all(void);

static void hub_drop(struct HubClient *c)
{
    if (!c->used)
        return;
    const int role = c->role;
    enet_socket_destroy(c->s);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->s = ENET_SOCKET_NULL;
    if (role == HubR_Server)
        H.servers_dirty = true;
    else if (role == HubR_Player)
        hub_send_names_all(); /* everyone else's names box */
}

static size_t json_esc(char *dst, size_t size, const char *src)
{
    net_json_escape(dst, size, src);
    return strlen(dst);
}

/** The servers list as JSON. */
static char *hub_servers_json(void)
{
    const size_t cap = 256 + (size_t)HUB_CLIENTS_MAX * 512;
    char *buf = (char *)malloc(cap);
    if (buf == NULL)
        return NULL;
    size_t o = (size_t)snprintf(buf, cap, "{\"t\":\"servers\",\"list\":[");
    TbBool first = true;
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
    {
        const struct HubClient *c = &H.c[i];
        if (!c->used || (c->role != HubR_Server))
            continue;
        char title[WNET_TITLE_MAX * 6 + 1], host[WNET_HOST_MAX * 6 + 1], build[24 * 6 + 1];
        json_esc(title, sizeof(title), c->title);
        json_esc(host, sizeof(host), c->host);
        json_esc(build, sizeof(build), c->build);
        o += (size_t)snprintf(buf + o, cap - o, "%s{\"id\":\"%s\",\"title\":\"%s\",\"host\":\"%s\",\"port\":%d,\"players\":%d,\"max\":%d,\"build\":\"%s\"}",
            first ? "" : ",", c->id, title, host, c->port, c->players, c->max, build);
        first = false;
        if (o >= cap - 600)
            break;
    }
    snprintf(buf + o, cap - o, "]}");
    return buf;
}

static void hub_send_servers(struct HubClient *to)
{
    char *json = hub_servers_json();
    if (json == NULL)
        return;
    if (to != NULL)
        hub_send_text(to, json);
    else
        for (int i = 0; i < HUB_CLIENTS_MAX; i++)
            if (H.c[i].used && (H.c[i].role == HubR_Player))
                hub_send_text(&H.c[i], json);
    free(json);
}

static void hub_send_names_all(void)
{
    char *buf = (char *)malloc(64 + (size_t)HUB_CLIENTS_MAX * (WNET_NAME_MAX * 6 + 4));
    if (buf == NULL)
        return;
    size_t o = (size_t)sprintf(buf, "{\"t\":\"names\",\"list\":[");
    TbBool first = true;
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
    {
        if (!H.c[i].used || (H.c[i].role != HubR_Player))
            continue;
        char esc[WNET_NAME_MAX * 6 + 1];
        json_esc(esc, sizeof(esc), H.c[i].name);
        o += (size_t)sprintf(buf + o, "%s\"%s\"", first ? "" : ",", esc);
        first = false;
    }
    sprintf(buf + o, "]}");
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
        if (H.c[i].used && (H.c[i].role == HubR_Player))
            hub_send_text(&H.c[i], buf);
    free(buf);
}

static TbBool hub_name_taken(const char *name, const struct HubClient *except)
{
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
        if (H.c[i].used && (&H.c[i] != except) && (H.c[i].role == HubR_Player) && (strcmp(H.c[i].name, name) == 0))
            return true;
    return false;
}

static int hub_count(int role)
{
    int n = 0;
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
        if (H.c[i].used && (H.c[i].role == role))
            n++;
    return n;
}

static void hub_handle(struct HubClient *c, const char *text, size_t len)
{
    VALUE msg = {0};
    if (json_dom_parse(text, len, NULL, 0, &msg, NULL) != 0)
    {
        value_fini(&msg);
        return;
    }
    const char *t = value_string(value_dict_get(&msg, "t"));
    if (t == NULL)
        t = "";
    if (strcmp(t, "ping") == 0)
    {
        hub_send_text(c, "{\"t\":\"pong\"}");
    } else if ((strcmp(t, "hello") == 0) && (c->role == HubR_None))
    {
        const char *role = value_string(value_dict_get(&msg, "role"));
        if ((role != NULL) && (strcmp(role, "player") == 0))
        {
            if (hub_count(HubR_Player) >= WNET_NAMES_MAX)
            {
                hub_send_text(c, "{\"t\":\"error\",\"reason\":\"the menu is full\"}");
                c->closing = true;
            } else
            {
                char want[WNET_NAME_MAX];
                json_copy(want, sizeof(want), &msg, "name");
                hub_clean(want);
                if (want[0] == '\0')
                    snprintf(want, sizeof(want), "Keeper");
                snprintf(c->name, sizeof(c->name), "%s", want);
                for (int k = 2; hub_name_taken(c->name, c) && (k < 1000); k++)
                {
                    char suffix[8];
                    snprintf(suffix, sizeof(suffix), " (%d)", k);
                    snprintf(c->name, sizeof(c->name), "%.*s%s", (int)(WNET_NAME_MAX - 1 - strlen(suffix)), want, suffix);
                }
                json_copy(c->build, sizeof(c->build), &msg, "build");
                c->role = HubR_Player;
                char esc[WNET_NAME_MAX * 6 + 1];
                json_esc(esc, sizeof(esc), c->name);
                char out[256];
                snprintf(out, sizeof(out), "{\"t\":\"welcome\",\"name\":\"%s\"}", esc);
                hub_send_text(c, out);
                hub_send_servers(c);
                hub_send_names_all(); /* the newcomer's names, and everyone else's update */
                /* history */
                char *hist = (char *)malloc(64 + (size_t)HUB_HISTORY * ((WNET_NAME_MAX + WNET_TEXT_MAX) * 6 + 48));
                if (hist != NULL)
                {
                    size_t o = (size_t)sprintf(hist, "{\"t\":\"history\",\"lines\":[");
                    for (int i = 0; i < H.history_count; i++)
                    {
                        char n[WNET_NAME_MAX * 6 + 1], x[WNET_TEXT_MAX * 6 + 1];
                        json_esc(n, sizeof(n), H.history[i].name);
                        json_esc(x, sizeof(x), H.history[i].text);
                        o += (size_t)sprintf(hist + o, "%s{\"name\":\"%s\",\"text\":\"%s\",\"time\":%lld}", (i == 0) ? "" : ",", n, x, H.history[i].time);
                    }
                    sprintf(hist + o, "]}");
                    hub_send_text(c, hist);
                    free(hist);
                }
                LbNetLog("World list hub: %s is in the menu (%s)\n", c->name, c->ip);
            }
        } else if ((role != NULL) && (strcmp(role, "server") == 0))
        {
            if (hub_count(HubR_Server) >= WNET_SERVERS_MAX)
            {
                hub_send_text(c, "{\"t\":\"error\",\"reason\":\"the list is full\"}");
                c->closing = true;
            } else
            {
                c->role = HubR_Server;
                snprintf(c->id, sizeof(c->id), "s%d", ++H.next_id);
                json_copy(c->title, sizeof(c->title), &msg, "title"); hub_clean(c->title);
                json_copy(c->build, sizeof(c->build), &msg, "build");
                c->port = json_int(&msg, "port", 5556);
                c->players = json_int(&msg, "players", 0);
                c->max = json_int(&msg, "max", 0);
                char given[WNET_HOST_MAX];
                json_copy(given, sizeof(given), &msg, "host");
                if (given[0] != '\0')
                    snprintf(c->host, sizeof(c->host), "%s", given);
                else if ((strncmp(c->ip, "127.", 4) == 0) || (strcmp(c->ip, "::1") == 0))
                    c->host[0] = '\0'; /* on the hub's own machine: players use the hub's address */
                else
                    snprintf(c->host, sizeof(c->host), "%s", c->ip);
                char esc[WNET_HOST_MAX * 6 + 1];
                json_esc(esc, sizeof(esc), c->host);
                char out[256];
                snprintf(out, sizeof(out), "{\"t\":\"registered\",\"id\":\"%s\",\"host\":\"%s\"}", c->id, esc);
                hub_send_text(c, out);
                H.servers_dirty = true;
                LbNetLog("World list hub: server \"%s\" registered from %s, port %d\n", c->title, c->ip, c->port);
            }
        }
    } else if ((strcmp(t, "update") == 0) && (c->role == HubR_Server))
    {
        if (value_dict_get(&msg, "players") != NULL)
            c->players = json_int(&msg, "players", c->players);
        if (value_dict_get(&msg, "max") != NULL)
            c->max = json_int(&msg, "max", c->max);
        if (value_dict_get(&msg, "title") != NULL)
        {
            json_copy(c->title, sizeof(c->title), &msg, "title");
            hub_clean(c->title);
        }
        H.servers_dirty = true;
    } else if ((strcmp(t, "say") == 0) && (c->role == HubR_Player))
    {
        /* at most 5 lines in any 5 s */
        const Uint32 now = now_ms();
        if ((c->say_times[c->say_pos] == 0) || ((int32_t)(now - c->say_times[c->say_pos]) >= 5000))
        {
            c->say_times[c->say_pos] = (now == 0) ? 1 : now;
            c->say_pos = (c->say_pos + 1) % 5;
            char text_cut[WNET_TEXT_MAX];
            json_copy(text_cut, sizeof(text_cut), &msg, "text");
            hub_clean(text_cut);
            if (text_cut[0] != '\0')
            {
                if (H.history_count == HUB_HISTORY)
                {
                    memmove(&H.history[0], &H.history[1], sizeof(H.history[0]) * (HUB_HISTORY - 1));
                    H.history_count--;
                }
                const long long tm = (long long)time(NULL);
                snprintf(H.history[H.history_count].name, WNET_NAME_MAX, "%s", c->name);
                snprintf(H.history[H.history_count].text, WNET_TEXT_MAX, "%s", text_cut);
                H.history[H.history_count].time = tm;
                H.history_count++;
                char n[WNET_NAME_MAX * 6 + 1], x[WNET_TEXT_MAX * 6 + 1];
                json_esc(n, sizeof(n), c->name);
                json_esc(x, sizeof(x), text_cut);
                char out[sizeof(n) + sizeof(x) + 64];
                snprintf(out, sizeof(out), "{\"t\":\"chat\",\"name\":\"%s\",\"text\":\"%s\",\"time\":%lld}", n, x, tm);
                for (int i = 0; i < HUB_CLIENTS_MAX; i++)
                    if (H.c[i].used && (H.c[i].role == HubR_Player))
                        hub_send_text(&H.c[i], out);
            }
        }
    }
    value_fini(&msg);
}

/** The HTTP upgrade. */
static void hub_handshake(struct HubClient *c)
{
    c->in[c->inlen] = '\0';
    char *end = strstr((char *)c->in, "\r\n\r\n");
    if (end == NULL)
    {
        if (c->inlen >= HUB_IN_MAX - 1)
            c->closing = true;
        return;
    }
    const size_t used = (size_t)(end + 4 - (char *)c->in);
    char key[128] = {0};
    for (char *line = (char *)c->in; line < end; )
    {
        char *eol = strstr(line, "\r\n");
        if (eol == NULL)
            break;
        if ((eol - line > 18) && (SDL_strncasecmp(line, "Sec-WebSocket-Key:", 18) == 0))
        {
            char *v = line + 18;
            while (*v == ' ')
                v++;
            const size_t n = (size_t)(eol - v);
            if (n < sizeof(key))
            {
                memcpy(key, v, n);
                key[n] = '\0';
            }
        }
        line = eol + 2;
    }
    if (key[0] == '\0')
    {
        const char *page = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 34\r\n\r\nKeeperFX world list hub (/hub)\r\n\r\n";
        hub_queue(c, page, strlen(page));
        c->closing = true;
        c->inlen = 0;
        return;
    }
    char cat[200];
    snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char digest[20];
    sha1((const unsigned char *)cat, strlen(cat), digest);
    char accept[40];
    base64(digest, sizeof(digest), accept);
    char resp[256];
    snprintf(resp, sizeof(resp), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    hub_queue(c, resp, strlen(resp));
    c->upgraded = true;
    memmove(c->in, c->in + used, c->inlen - used);
    c->inlen -= used;
}

/** Whole frames in the input; text ones go to hub_handle. */
static void hub_frames(struct HubClient *c)
{
    static unsigned char msg[HUB_IN_MAX];
    static size_t msglen = 0;
    for (;;)
    {
        if (c->inlen < 2)
            return;
        const unsigned char *p = c->in;
        const int fin = (p[0] & 0x80) != 0, op = p[0] & 0x0F, masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t hl = 2;
        if (len == 126)
        {
            if (c->inlen < 4)
                return;
            len = ((uint64_t)p[2] << 8) | p[3];
            hl = 4;
        } else if (len == 127)
        {
            if (c->inlen < 10)
                return;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | p[2 + i];
            hl = 10;
        }
        if (len > HUB_IN_MAX - 16)
        {
            c->closing = true;
            return;
        }
        const size_t mk = masked ? 4 : 0;
        if (c->inlen < hl + mk + len)
            return;
        unsigned char *payload = c->in + hl + mk;
        if (masked)
            for (size_t i = 0; i < len; i++)
                payload[i] ^= p[hl + (i & 3)];
        if (op == 0x8)
        {
            const unsigned char close_frame[2] = {0x88, 0x00};
            hub_queue(c, close_frame, 2);
            c->closing = true;
        } else if (op == 0x9)
        {
            unsigned char hdr[2] = {0x8A, (unsigned char)((len < 126) ? len : 0)};
            if (len < 126)
            {
                hub_queue(c, hdr, 2);
                hub_queue(c, payload, (size_t)len);
            }
        } else if ((op == 0x1) || (op == 0x0))
        {
            if ((op == 0x1) && (msglen != 0))
                msglen = 0;
            if (msglen + len < sizeof(msg))
            {
                memcpy(msg + msglen, payload, (size_t)len);
                msglen += (size_t)len;
            }
            if (fin)
            {
                msg[msglen] = '\0';
                hub_handle(c, (const char *)msg, msglen);
                msglen = 0;
            }
        }
        const size_t used = hl + mk + (size_t)len;
        if (!c->used)
            return; /* dropped while handling */
        memmove(c->in, c->in + used, c->inlen - used);
        c->inlen -= used;
    }
}

TbBool wnet_hub_start(int port)
{
    wnet_hub_stop();
    memset(&H, 0, sizeof(H));
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
        H.c[i].s = ENET_SOCKET_NULL;
    H.listen = enet_socket_create(ENET_ADDRESS_TYPE_IPV4, ENET_SOCKET_TYPE_STREAM);
    if (H.listen == ENET_SOCKET_NULL)
    {
        LbNetLog("World list hub: no socket\n");
        return false;
    }
    enet_socket_set_option(H.listen, ENET_SOCKOPT_REUSEADDR, 1);
    enet_socket_set_option(H.listen, ENET_SOCKOPT_NONBLOCK, 1);
    ENetAddress addr;
    enet_address_build_any(&addr, ENET_ADDRESS_TYPE_IPV4);
    addr.port = (enet_uint16)port;
    if ((enet_socket_bind(H.listen, &addr) < 0) || (enet_socket_listen(H.listen, 16) < 0))
    {
        LbNetLog("World list hub: can't listen on port %d\n", port);
        enet_socket_destroy(H.listen);
        H.listen = ENET_SOCKET_NULL;
        return false;
    }
    H.on = true;
    H.port = port;
    LbNetLog("World list hub: listening on port %d\n", port);
    return true;
}

void wnet_hub_stop(void)
{
    if (!H.on)
        return;
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
        if (H.c[i].used)
        {
            enet_socket_destroy(H.c[i].s);
            free(H.c[i].out);
        }
    enet_socket_destroy(H.listen);
    memset(&H, 0, sizeof(H));
    H.listen = ENET_SOCKET_NULL;
}

/** The address alone: no port (a.b.c.d:port, [v6]:port) and no IPv4-mapped prefix (::ffff:a.b.c.d). */
static void hub_bare_ip(char *ip)
{
    if (ip[0] == '[')
    {
        char *close = strchr(ip, ']');
        if (close != NULL)
            *close = '\0';
        memmove(ip, ip + 1, strlen(ip + 1) + 1);
    }
    if (strncmp(ip, "::ffff:", 7) == 0)
        memmove(ip, ip + 7, strlen(ip + 7) + 1);
    char *colon = strchr(ip, ':');
    if ((colon != NULL) && (strchr(colon + 1, ':') == NULL))
        *colon = '\0'; /* one colon: IPv4 with a port */
}

void wnet_hub_poll(void)
{
    if (!H.on)
        return;
    /* new connections */
    for (;;)
    {
        ENetAddress from;
        ENetSocket s = enet_socket_accept(H.listen, &from);
        if (s == ENET_SOCKET_NULL)
            break;
        int slot = -1;
        for (int i = 0; (i < HUB_CLIENTS_MAX) && (slot < 0); i++)
            if (!H.c[i].used)
                slot = i;
        if (slot < 0)
        {
            enet_socket_destroy(s);
            continue;
        }
        enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
        struct HubClient *c = &H.c[slot];
        memset(c, 0, sizeof(*c));
        c->s = s;
        c->used = true;
        c->accepted = now_ms();
        enet_address_get_host_ip(&from, c->ip, sizeof(c->ip));
        hub_bare_ip(c->ip);
    }
    for (int i = 0; i < HUB_CLIENTS_MAX; i++)
    {
        struct HubClient *c = &H.c[i];
        if (!c->used)
            continue;
        /* out */
        while (c->outlen > 0)
        {
            ENetBuffer b = {.data = c->out, .dataLength = c->outlen};
            const int n = enet_socket_send(c->s, NULL, &b, 1);
            if (n < 0)
            {
                hub_drop(c);
                break;
            }
            if (n == 0)
                break;
            memmove(c->out, c->out + n, c->outlen - (size_t)n);
            c->outlen -= (size_t)n;
        }
        if (!c->used)
            continue;
        if (!c->upgraded && ((int32_t)(now_ms() - c->accepted) >= HUB_HANDSHAKE_MS))
        {
            hub_drop(c); /* connected and said nothing useful: its slot goes to someone else */
            continue;
        }
        if (c->closing)
        {
            if (c->outlen == 0)
                hub_drop(c);
            continue;
        }
        /* in */
        for (;;)
        {
            enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
            if ((enet_socket_wait(c->s, &wait, 0) < 0) || !(wait & ENET_SOCKET_WAIT_RECEIVE))
                break;
            if (c->inlen >= HUB_IN_MAX - 1)
            {
                c->closing = true;
                break;
            }
            ENetBuffer b = {.data = c->in + c->inlen, .dataLength = HUB_IN_MAX - 1 - c->inlen};
            const int n = enet_socket_receive(c->s, NULL, &b, 1);
            if (n <= 0)
            {
                hub_drop(c); /* readable but nothing: closed (or reset) */
                break;
            }
            c->inlen += (size_t)n;
            if (!c->upgraded)
                hub_handshake(c);
            if (c->upgraded && c->used)
                hub_frames(c);
            if (!c->used || c->closing)
                break;
        }
    }
    if (H.servers_dirty && ((int32_t)(now_ms() - H.servers_sent) >= 1000))
    {
        H.servers_dirty = false;
        H.servers_sent = now_ms();
        hub_send_servers(NULL);
    }
}

/******************************************************************************/
/* The ping, answered on the game port */
int wnet_game_port_intercept(void *enet_host)
{
    ENetHost *host = (ENetHost *)enet_host;
    if ((host == NULL) || (host->receivedDataLength != 12) || (memcmp(host->receivedData, PING_MAGIC, 8) != 0))
        return 0;
    if (!WS.on)
        return 0; /* not an endless world on the World List: as stock, the packet goes to ENet */
    unsigned char reply[16];
    memcpy(reply, PONG_MAGIC, 8);
    memcpy(reply + 8, host->receivedData + 8, 4);
    reply[12] = (unsigned char)((status_players < 0) ? 0 : (status_players > 255) ? 255 : status_players);
    reply[13] = (unsigned char)((status_max < 0) ? 0 : (status_max > 255) ? 255 : status_max);
    reply[14] = 0;
    reply[15] = 0;
    ENetBuffer b = {.data = reply, .dataLength = sizeof(reply)};
    enet_socket_send(host->socket, &host->receivedAddress, &b, 1);
    return 1;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

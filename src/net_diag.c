/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_diag.c
 *     Network diagnostics: socket error texts, this PC's address, and the network log file.
 * @par Purpose:
 *     Why a network connection failed, in words, and a log of the network that survives the next start
 *     (keeperfx.log is written anew each time, and hides addresses).
 */
/******************************************************************************/
#include "pre_inc.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "net_diag.h"
#include "bflib_netsession.h"
#include "ver_defs.h"
#include "post_inc.h"

#define NET_LOG_MAX_BYTES (4L << 20)

int net_socket_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

TbBool net_socket_would_block(int err)
{
#ifdef _WIN32
    return (err == WSAEWOULDBLOCK) || (err == WSAEINPROGRESS);
#else
    return (err == EAGAIN) || (err == EWOULDBLOCK) || (err == EINPROGRESS);
#endif
}

const char *net_socket_error_text(int err)
{
    static char ring[4][160];
    static int next;
    char *buf = ring[next];
    next = (next + 1) % 4;
    const char *what = NULL;
#ifdef _WIN32
    switch (err)
    {
    case WSAEINTR:           what = "interrupted"; break;
    case WSAEACCES:          what = "not allowed (a firewall?)"; break;
    case WSAENOTSOCK:        what = "not a socket any more (it was closed)"; break;
    case WSAEMSGSIZE:        what = "a message too big"; break;
    case WSAEADDRINUSE:      what = "the port is in use by another program"; break;
    case WSAEADDRNOTAVAIL:   what = "that address isn't on this PC"; break;
    case WSAENETDOWN:        what = "the network is down"; break;
    case WSAENETUNREACH:     what = "no route to that network"; break;
    case WSAENETRESET:       what = "the network dropped the connection"; break;
    case WSAECONNABORTED:    what = "the connection was aborted on this PC"; break;
    case WSAECONNRESET:      what = "the other side reset the connection"; break;
    case WSAENOBUFS:         what = "out of network buffers"; break;
    case WSAENOTCONN:        what = "not connected"; break;
    case WSAESHUTDOWN:       what = "the connection was shut down"; break;
    case WSAETIMEDOUT:       what = "timed out"; break;
    case WSAECONNREFUSED:    what = "refused (nothing listens there)"; break;
    case WSAEHOSTDOWN:       what = "the other computer is down"; break;
    case WSAEHOSTUNREACH:    what = "the other computer can't be reached"; break;
    case WSANOTINITIALISED:  what = "Windows sockets aren't started"; break;
    default: break;
    }
    if (what == NULL)
    {
        char sys[120] = "";
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)err, 0, sys, sizeof(sys), NULL);
        for (char *p = sys; *p != '\0'; p++)
            if ((*p == '\r') || (*p == '\n'))
                *p = ' ';
        size_t n = strlen(sys);
        while ((n > 0) && ((sys[n - 1] == ' ') || (sys[n - 1] == '.')))
            sys[--n] = '\0';
        snprintf(buf, sizeof(ring[0]), "%s (error %d)", (sys[0] != '\0') ? sys : "socket error", err);
        return buf;
    }
#else
    what = strerror(err);
#endif
    snprintf(buf, sizeof(ring[0]), "%s (error %d)", what, err);
    return buf;
}

const char *net_join_reason_text(int reason)
{
    switch (reason)
    {
    case NetJoin_Accepted:              return "no reason given";
    case NetJoin_InGame:                return "the game had already started";
    case NetJoin_Locked:                return "joining is locked";
    case NetJoin_Full:                  return "the world is full";
    case NetJoin_Version:               return "a different game version";
    case NetJoin_SpectatorState:        return "the game isn't running yet (spectator)";
    case NetJoin_SpectatorsDisabled:    return "spectators aren't allowed";
    case NetJoin_SpectatorsFull:        return "no room for more spectators";
    case NetJoin_SpectatorsUnsupported: return "spectating isn't supported by that version";
    case NetJoin_SpectatorDesynced:     return "out of sync (spectator)";
    case NetJoin_MapMismatch:           return "a different map version";
    case NetJoin_FxdataMismatch:        return "different fxdata files";
    case NetJoin_Kicked:                return "kicked";
    case NetJoin_Banned:                return "banned";
    case NetJoin_BannedJoin:            return "banned (tried to join again)";
    default:                            return "an unknown reason";
    }
}

TbBool net_local_ipv4(char *buf, size_t size)
{
    buf[0] = '\0';
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return false;
#endif
    /* a UDP "connect" only picks the route: nothing goes out */
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);
    TbBool ok = false;
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0)
    {
        struct sockaddr_in me;
        socklen_t len = sizeof(me);
        if ((getsockname(s, (struct sockaddr *)&me, &len) == 0) && (me.sin_addr.s_addr != 0))
            ok = (inet_ntop(AF_INET, &me.sin_addr, buf, (socklen_t)size) != NULL);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    if (!ok)
        buf[0] = '\0';
    return ok;
}

/******************************************************************************/
static FILE *net_log_fp = NULL;
static SDL_Mutex *net_log_mutex = NULL;

static void net_log_stamp(char *buf, size_t size)
{
    const time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(buf, size, "[%Y-%m-%d %H:%M:%S] ", &tmv);
}

void net_log_file_setup(const char *path, const char *role)
{
    if (net_log_fp != NULL)
        return;
    FILE *f = fopen(path, "rb");
    if (f != NULL)
    {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fclose(f);
        if (size > NET_LOG_MAX_BYTES)
        {
            char old[1024];
            snprintf(old, sizeof(old), "%s.old", path);
            remove(old);
            rename(path, old);
        }
    }
    net_log_fp = fopen(path, "a");
    if (net_log_fp == NULL)
        return;
    net_log_mutex = SDL_CreateMutex();
    char stamp[40];
    net_log_stamp(stamp, sizeof(stamp));
    fprintf(net_log_fp, "\n%s==== KeeperFX %s started (%s) ====\n", stamp, VER_STRING, role);
    fflush(net_log_fp);
}

void net_log_file_write(const char *text)
{
    if ((net_log_fp == NULL) || (text == NULL))
        return;
    char stamp[40];
    net_log_stamp(stamp, sizeof(stamp));
    if (net_log_mutex != NULL)
        SDL_LockMutex(net_log_mutex);
    const size_t n = strlen(text);
    fputs(stamp, net_log_fp);
    fputs(text, net_log_fp);
    if ((n == 0) || (text[n - 1] != '\n'))
        fputc('\n', net_log_fp);
    fflush(net_log_fp);
    if (net_log_mutex != NULL)
        SDL_UnlockMutex(net_log_mutex);
}
/******************************************************************************/

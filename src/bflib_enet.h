/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/**
 * @author   KeeperFX Team
 * @date     18 Oct 2022
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/

#ifndef GIT_BFLIB_ENET_H
#define GIT_BFLIB_ENET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENET_DEFAULT_PORT 5556
extern uint16_t enet_port;

enum {
    ENET_CHANNEL_RELIABLE = 0,
    ENET_CHANNEL_UNSEQUENCED = 1
};

struct NetSP;
struct NetSP* InitEnetSP();
uint32_t GetPing(int id);
/** How long a silent peer is kept before it is dropped (also applied to the peers connected now). */
void LbNetwork_SetPeerTimeout(unsigned long timeout_ms);
uint32_t GetPlayersPing(void);
unsigned int GetPacketLoss(int id);
unsigned int GetClientDataInTransit();
int32_t GetResyncProgress(void);
unsigned int GetClientPacketsLost();
unsigned int GetUploadRateBytesPerSecond();
unsigned int GetDownloadRateBytesPerSecond();
int enet_matchmaking_host_update(void);
extern uint16_t external_ipv4_port;
extern char external_ipv4_address[64];
extern int skip_holepunch;
uint16_t enet_get_bound_port(void);
uint16_t enet_get_bound_ipv6_port(void);
int enet_user_ms_since_receive(int user);
/** A connected user's IP address as text (a dedicated server's console and bans); 0 when there is one. */
int enet_user_address(int user, char *buf, size_t len);
/** A connected user's ping (ms) and packet loss (tenths of a percent); 0 when connected, -1 when not. */
int enet_user_link(int user, int *rtt_ms, int *loss_permille);
/** Why the user being dropped right now dropped, in words (during the drop callback only). */
const char *enet_last_drop_detail(void);
/** A client: why its server dropped, in words ("" when it has not). */
const char *enet_server_drop_detail(void);

#ifdef __cplusplus
}
#endif

#endif //GIT_BFLIB_ENET_H

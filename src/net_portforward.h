/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/**
 * @file net_portforward.h
 *     Port forwarding support using NAT-PMP and UPnP.
 * @par Purpose:
 *     Tries NAT-PMP first (simpler/faster), falls back to UPnP.
 * @author   KeeperFX Team
 * @date     01 Jan 2026
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef NET_PORTFORWARD_H
#define NET_PORTFORWARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void port_forward_set_mapping(uint16_t port);
/** The World List hub's TCP port (0: none), forwarded apart from the game's UDP port (2026-11-09). */
void port_forward_set_tcp_mapping(uint16_t port);
/** A host calls this every turn. About once a minute it looks whether the router still forwards the host's ports
 *  to this computer, and asks again when it does not (a lease that ended, a restarted router, another program on
 *  this computer that took the port away). */
void port_forward_keepalive(void);
/** Waits (5 s at most each) for the router requests under way: at the game's end, so the forwards are removed. */
void port_forward_wait(void);
/** What the last forwarding try gave, in words ("" before any); returns a number that changes with it. */
uint32_t port_forward_status(char *text, size_t size, int *forwarded);

#ifdef __cplusplus
}
#endif

#endif

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
/** What the last forwarding try gave, in words ("" before any); returns a number that changes with it. */
uint32_t port_forward_status(char *text, size_t size, int *forwarded);

#ifdef __cplusplus
}
#endif

#endif

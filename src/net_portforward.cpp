/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/**
 * @file net_portforward.cpp
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
#include "pre_inc.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#include "net_portforward.h"
#include "bflib_basics.h"
#include "bflib_datetm.h"

#define MINIUPNP_STATICLIB
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#define NATPMP_STATICLIB
#ifdef __WIN32__
#include <natpmp/natpmp.h>
#else
#include <natpmp.h>
#endif

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <cstdio>
#include <ctime>
#include <future>
#include <mutex>
#ifdef __WIN32__
#include <winsock2.h>
#include <iphlpapi.h>
#endif

#define NATPMP_TIMEOUT_SECONDS 1.0
#define UPNP_TIMEOUT_MS 3000

#include "post_inc.h"

enum PortForwardMethod {
    PORT_FORWARD_NONE = 0,
    PORT_FORWARD_NATPMP,
    PORT_FORWARD_UPNP
};

static enum PortForwardMethod active_method = PORT_FORWARD_NONE;
static uint16_t mapped_port = 0;
static std::future<void> mapping_task;
static std::atomic<uint32_t> mapping_generation = 0;

/* what the last try gave, for the server's admin (read on the game thread, written on the mapping thread) */
static std::mutex status_mutex;
static char status_text[192] = "";
static int status_forwarded = 0;
static std::atomic<uint32_t> status_generation = 0;

static void set_status(int forwarded, const char *format, ...)
{
    char text[sizeof(status_text)];
    va_list val;
    va_start(val, format);
    vsnprintf(text, sizeof(text), format, val);
    va_end(val);
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        snprintf(status_text, sizeof(status_text), "%s", text);
        status_forwarded = forwarded;
    }
    status_generation++;
}

uint32_t port_forward_status(char *text, size_t size, int *forwarded)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    snprintf(text, size, "%s", status_text);
    if (forwarded != NULL)
        *forwarded = status_forwarded;
    return status_generation;
}

static struct UPNPUrls upnp_urls;
static struct IGDdatas upnp_data;
static char upnp_lanaddr[64];

static natpmp_t natpmp;

/** An address (network byte order) in 100.64.0.0/10, the space internet providers share between homes (CGNAT). */
static int ip_in_shared_space(unsigned long ip)
{
    if ((ip == INADDR_NONE) || (ip == 0)) {
        return 0;
    }
    const unsigned char first_octet = ip & 0xFF;
    const unsigned char second_octet = (ip >> 8) & 0xFF;
    return (first_octet == 100) && (second_octet >= 64) && (second_octet <= 127);
}

#ifdef __WIN32__
/** CGNAT seen from this PC: an adapter that leads to the internet (it has a default gateway) has an address or a
 *  gateway in the shared space. VPN adapters without a gateway don't count: Tailscale gives each PC a 100.64.0.0/10
 *  address of its own (2026-11-08: that made a home router with UPnP look like CGNAT, so nothing was forwarded). */
static int is_cgnat_detected() {
    ULONG buffer_size = 0;
    if (GetAdaptersInfo(NULL, &buffer_size) != ERROR_BUFFER_OVERFLOW) {
        return 0;
    }
    IP_ADAPTER_INFO *adapter_info = (IP_ADAPTER_INFO *)malloc(buffer_size);
    if (!adapter_info) {
        return 0;
    }
    if (GetAdaptersInfo(adapter_info, &buffer_size) != NO_ERROR) {
        free(adapter_info);
        return 0;
    }
    int found = 0;
    for (IP_ADAPTER_INFO *adapter = adapter_info; adapter && !found; adapter = adapter->Next) {
        const unsigned long local_ip = inet_addr(adapter->IpAddressList.IpAddress.String);
        const unsigned long gateway_ip = inet_addr(adapter->GatewayList.IpAddress.String);
        if ((gateway_ip == INADDR_NONE) || (gateway_ip == 0)) {
            if (ip_in_shared_space(local_ip)) {
                LbNetLog("Port forwarding: %s (%s) is in the shared address space but leads nowhere (a VPN such as Tailscale): not CGNAT\n",
                    adapter->Description, adapter->IpAddressList.IpAddress.String);
            }
            continue;
        }
        if (ip_in_shared_space(local_ip) || ip_in_shared_space(gateway_ip)) {
            LbNetLog("Port forwarding: %s has %s, gateway %s: CGNAT\n", adapter->Description,
                adapter->IpAddressList.IpAddress.String, adapter->GatewayList.IpAddress.String);
            found = 1;
        }
    }
    free(adapter_info);
    return found;
}
#else
static int is_cgnat_detected() {
    return 0;
}
#endif

static int natpmp_add_port_mapping(uint16_t port) {
    clock_t start_time = LbTimerClock();
    if (initnatpmp(&natpmp, 0, 0) < 0) {
        LbNetLog("NAT-PMP: Failed to initialize\n");
        return 0;
    }
    if (sendpublicaddressrequest(&natpmp) < 0) {
        LbNetLog("NAT-PMP: Failed to send public address request\n");
        closenatpmp(&natpmp);
        return 0;
    }
    natpmpresp_t response;
    fd_set file_descriptors;
    struct timeval timeout;
    int result;
    do {
        double elapsed = (double)(LbTimerClock() - start_time) / 1000.0;
        if (elapsed > NATPMP_TIMEOUT_SECONDS) {
            LbNetLog("NAT-PMP: Timeout getting public address\n");
            closenatpmp(&natpmp);
            return 0;
        }
        FD_ZERO(&file_descriptors);
        FD_SET(natpmp.s, &file_descriptors);
        getnatpmprequesttimeout(&natpmp, &timeout);
        select(FD_SETSIZE, &file_descriptors, NULL, NULL, &timeout);
        result = readnatpmpresponseorretry(&natpmp, &response);
    } while (result == NATPMP_TRYAGAIN);
    if (result < 0) {
        LbNetLog("NAT-PMP: Failed to get public address\n");
        closenatpmp(&natpmp);
        return 0;
    }
    if (sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, port, port, 0x7FFFFFFF) < 0) {
        LbNetLog("NAT-PMP: Failed to send port mapping request\n");
        closenatpmp(&natpmp);
        return 0;
    }
    do {
        double elapsed = (double)(LbTimerClock() - start_time) / 1000.0;
        if (elapsed > NATPMP_TIMEOUT_SECONDS) {
            LbNetLog("NAT-PMP: Timeout on port mapping\n");
            closenatpmp(&natpmp);
            return 0;
        }
        FD_ZERO(&file_descriptors);
        FD_SET(natpmp.s, &file_descriptors);
        getnatpmprequesttimeout(&natpmp, &timeout);
        select(FD_SETSIZE, &file_descriptors, NULL, NULL, &timeout);
        result = readnatpmpresponseorretry(&natpmp, &response);
    } while (result == NATPMP_TRYAGAIN);
    if (result < 0) {
        LbNetLog("NAT-PMP: Permanent lease rejected, trying timed lease\n");
        if (sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, port, port, 3600) < 0) {
            LbNetLog("NAT-PMP: Failed to send timed port mapping request\n");
            closenatpmp(&natpmp);
            return 0;
        }
        do {
            double elapsed = (double)(LbTimerClock() - start_time) / 1000.0;
            if (elapsed > NATPMP_TIMEOUT_SECONDS) {
                LbNetLog("NAT-PMP: Timeout on timed lease\n");
                closenatpmp(&natpmp);
                return 0;
            }
            FD_ZERO(&file_descriptors);
            FD_SET(natpmp.s, &file_descriptors);
            getnatpmprequesttimeout(&natpmp, &timeout);
            select(FD_SETSIZE, &file_descriptors, NULL, NULL, &timeout);
            result = readnatpmpresponseorretry(&natpmp, &response);
        } while (result == NATPMP_TRYAGAIN);
        if (result < 0) {
            LbNetLog("NAT-PMP: Failed to add port mapping\n");
            closenatpmp(&natpmp);
            return 0;
        }
    }
    LbNetLog("NAT-PMP: port forwarding active on port %u\n", port);
    set_status(1, "UDP port %u forwarded by the router (NAT-PMP)", port);
    mapped_port = port;
    active_method = PORT_FORWARD_NATPMP;
    closenatpmp(&natpmp);
    return 1;
}

static void port_forward_add_mapping_internal(uint16_t port)
{
    if (is_cgnat_detected()) {
        LbNetLog("CGNAT detected, automatic port forwarding unavailable\n");
        set_status(0, "no port forwarding: the internet provider shares one address between many homes (CGNAT)");
        return;
    }
    if (getenv("KFX_PORTFORWARD_DRYRUN") != NULL) {
        /* test hook: the decision only, the router is left alone */
        LbNetLog("Port forwarding: not CGNAT; would ask the router (NAT-PMP, then UPnP) for UDP port %u (dry run)\n", port);
        set_status(0, "dry run: the router would be asked to forward UDP port %u", port);
        return;
    }
    if (natpmp_add_port_mapping(port)) {
        return;
    }
    int error = 0;
    struct UPNPDev *device_list = upnpDiscover(UPNP_TIMEOUT_MS, NULL, NULL, 0, 0, 2, &error);
    if (!device_list) {
        LbNetLog("UPnP: no devices found, port forwarding unavailable, UDP hole punching will be used\n");
        set_status(0, "no port forwarding: no router answered UPnP or NAT-PMP (forward UDP port %u by hand)", port);
        return;
    }
#if (MINIUPNPC_API_VERSION >= 18)
    int internet_gateway_device_result = UPNP_GetValidIGD(device_list, &upnp_urls, &upnp_data, upnp_lanaddr, sizeof(upnp_lanaddr), NULL, 0);
#else
    int internet_gateway_device_result = UPNP_GetValidIGD(device_list, &upnp_urls, &upnp_data, upnp_lanaddr, sizeof(upnp_lanaddr));
#endif
    freeUPNPDevlist(device_list);
    if (internet_gateway_device_result == 0) {
        LbNetLog("UPnP: router found but no valid IGD, port forwarding unavailable, UDP hole punching will be used\n");
        set_status(0, "no port forwarding: the router doesn't offer it (UPnP off?); forward UDP port %u by hand", port);
        FreeUPNPUrls(&upnp_urls);
        return;
    }
    {
        char wan[64] = "";
        if ((UPNP_GetExternalIPAddress(upnp_urls.controlURL, upnp_data.first.servicetype, wan) == UPNPCOMMAND_SUCCESS) &&
            (wan[0] != '\0')) {
            LbNetLog("UPnP: the router's internet address is %s\n", wan);
            if (ip_in_shared_space(inet_addr(wan))) {
                LbNetLog("UPnP: that address is shared between homes (CGNAT): a forward on this router can't reach this PC\n");
                set_status(0, "no port forwarding: the router's internet address %s is shared between many homes (CGNAT)", wan);
                FreeUPNPUrls(&upnp_urls);
                return;
            }
        }
    }
    char port_string[16];
    snprintf(port_string, sizeof(port_string), "%u", port);
    UPNP_DeletePortMapping(upnp_urls.controlURL, upnp_data.first.servicetype, port_string, "UDP", "");
    LbNetLog("UPnP: lanaddr=%s\n", upnp_lanaddr);
    int result = UPNP_AddPortMapping(upnp_urls.controlURL, upnp_data.first.servicetype, port_string, port_string, upnp_lanaddr, "KeeperFX", "UDP", "", "0");
    if (result != UPNPCOMMAND_SUCCESS) {
        LbNetLog("UPnP: permanent lease rejected (error %d), trying timed lease\n", result);
        result = UPNP_AddPortMapping(upnp_urls.controlURL, upnp_data.first.servicetype, port_string, port_string, upnp_lanaddr, "KeeperFX", "UDP", "", "3600");
        if (result != UPNPCOMMAND_SUCCESS) {
            LbNetLog("UPnP: failed to add port mapping (error %d), UDP hole punching will be used\n", result);
            set_status(0, "no port forwarding: the router refused it (UPnP error %d, %s); forward UDP port %u by hand", result,
                strupnperror(result), port);
            FreeUPNPUrls(&upnp_urls);
            return;
        }
    }
    LbNetLog("UPnP: port forwarding active on port %u\n", port);
    set_status(1, "UDP port %u forwarded by the router (UPnP, to %s)", port, upnp_lanaddr);
    mapped_port = port;
    active_method = PORT_FORWARD_UPNP;
}

static void port_forward_remove_mapping(void)
{
    if (active_method == PORT_FORWARD_NONE || mapped_port == 0) {
        return;
    }
    if (active_method == PORT_FORWARD_NATPMP) {
        if (initnatpmp(&natpmp, 0, 0) < 0) {
            LbNetLog("NAT-PMP: Failed to initialize for removal\n");
            mapped_port = 0;
            active_method = PORT_FORWARD_NONE;
            return;
        }
        sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, mapped_port, 0, 0);
        natpmpresp_t response;
        fd_set file_descriptors;
        struct timeval timeout;
        int result;
        clock_t start_time = LbTimerClock();
        do {
            double elapsed = (double)(LbTimerClock() - start_time) / 1000.0;
            if (elapsed > NATPMP_TIMEOUT_SECONDS) {
                break;
            }
            FD_ZERO(&file_descriptors);
            FD_SET(natpmp.s, &file_descriptors);
            getnatpmprequesttimeout(&natpmp, &timeout);
            select(FD_SETSIZE, &file_descriptors, NULL, NULL, &timeout);
            result = readnatpmpresponseorretry(&natpmp, &response);
        } while (result == NATPMP_TRYAGAIN);
        closenatpmp(&natpmp);
    } else if (active_method == PORT_FORWARD_UPNP) {
        char port_string[16];
        snprintf(port_string, sizeof(port_string), "%u", mapped_port);
        UPNP_DeletePortMapping(upnp_urls.controlURL, upnp_data.first.servicetype, port_string, "UDP", NULL);
        FreeUPNPUrls(&upnp_urls);
    }
    LbNetLog("Port forwarding: UDP port %u forward removed\n", mapped_port);
    mapped_port = 0;
    active_method = PORT_FORWARD_NONE;
}

void port_forward_set_mapping(uint16_t port)
{
    uint32_t generation = ++mapping_generation;
    mapping_task = std::async(std::launch::async, [previous = std::move(mapping_task), port, generation]() mutable {
        if (previous.valid()) {
            previous.wait();
            previous = {};
        }
        if (generation != mapping_generation) {
            return;
        }
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return;
        }
#endif
        port_forward_remove_mapping();
        if (port != 0 && generation == mapping_generation) {
            set_status(0, "asking the router to forward UDP port %u", port);
            port_forward_add_mapping_internal(port);
        } else if (port == 0) {
            set_status(0, "%s", "");
        }
#ifdef _WIN32
        WSACleanup();
#endif
    });
}

/******************************************************************************/
/* The World List hub's TCP port (2026-11-09): players elsewhere reach a dedicated server's hub by the server's
   address. Its own task and router state, apart from the game port's (the two may run at once). */
static std::future<void> tcp_task;
static std::atomic<uint32_t> tcp_generation = 0;
static uint16_t tcp_mapped_port = 0;
static enum PortForwardMethod tcp_method = PORT_FORWARD_NONE;
static struct UPNPUrls tcp_urls;
static struct IGDdatas tcp_data;
static char tcp_lanaddr[64];

/** A NAT-PMP TCP mapping for port (lifetime 0 removes it); 1 if the router agreed. */
static int tcp_natpmp(uint16_t port, uint32_t lifetime)
{
    natpmp_t n;
    if (initnatpmp(&n, 0, 0) < 0) {
        return 0;
    }
    int result = -1;
    if (sendnewportmappingrequest(&n, NATPMP_PROTOCOL_TCP, port, (lifetime != 0) ? port : 0, lifetime) >= 0) {
        natpmpresp_t response;
        fd_set file_descriptors;
        struct timeval timeout;
        clock_t start_time = LbTimerClock();
        do {
            if ((double)(LbTimerClock() - start_time) / 1000.0 > NATPMP_TIMEOUT_SECONDS) {
                result = -1;
                break;
            }
            FD_ZERO(&file_descriptors);
            FD_SET(n.s, &file_descriptors);
            getnatpmprequesttimeout(&n, &timeout);
            select(FD_SETSIZE, &file_descriptors, NULL, NULL, &timeout);
            result = readnatpmpresponseorretry(&n, &response);
        } while (result == NATPMP_TRYAGAIN);
    }
    closenatpmp(&n);
    return (result >= 0) ? 1 : 0;
}

static int tcp_upnp_add(uint16_t port)
{
    int error = 0;
    struct UPNPDev *device_list = upnpDiscover(UPNP_TIMEOUT_MS, NULL, NULL, 0, 0, 2, &error);
    if (!device_list) {
        return 0;
    }
    char *lanaddr = tcp_lanaddr; /* kept: the forward is looked up again later */
    lanaddr[0] = '\0';
#if (MINIUPNPC_API_VERSION >= 18)
    int igd = UPNP_GetValidIGD(device_list, &tcp_urls, &tcp_data, lanaddr, sizeof(tcp_lanaddr), NULL, 0);
#else
    int igd = UPNP_GetValidIGD(device_list, &tcp_urls, &tcp_data, lanaddr, sizeof(tcp_lanaddr));
#endif
    freeUPNPDevlist(device_list);
    if (igd == 0) {
        FreeUPNPUrls(&tcp_urls);
        return 0;
    }
    char port_string[16];
    snprintf(port_string, sizeof(port_string), "%u", port);
    UPNP_DeletePortMapping(tcp_urls.controlURL, tcp_data.first.servicetype, port_string, "TCP", "");
    int result = UPNP_AddPortMapping(tcp_urls.controlURL, tcp_data.first.servicetype, port_string, port_string, lanaddr, "KeeperFX World List", "TCP", "", "0");
    if (result != UPNPCOMMAND_SUCCESS) {
        result = UPNP_AddPortMapping(tcp_urls.controlURL, tcp_data.first.servicetype, port_string, port_string, lanaddr, "KeeperFX World List", "TCP", "", "3600");
    }
    if (result != UPNPCOMMAND_SUCCESS) {
        LbNetLog("UPnP: the router refused TCP port %u (error %d, %s)\n", port, result, strupnperror(result));
        FreeUPNPUrls(&tcp_urls);
        return 0;
    }
    LbNetLog("UPnP: TCP port %u (World List hub) forwarded to %s\n", port, lanaddr);
    return 1;
}

void port_forward_set_tcp_mapping(uint16_t port)
{
    uint32_t generation = ++tcp_generation;
    tcp_task = std::async(std::launch::async, [previous = std::move(tcp_task), port, generation]() mutable {
        if (previous.valid()) {
            previous.wait();
            previous = {};
        }
        if (generation != tcp_generation) {
            return;
        }
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return;
        }
#endif
        if (tcp_mapped_port != 0) {
            char port_string[16];
            snprintf(port_string, sizeof(port_string), "%u", tcp_mapped_port);
            if (tcp_method == PORT_FORWARD_UPNP) {
                UPNP_DeletePortMapping(tcp_urls.controlURL, tcp_data.first.servicetype, port_string, "TCP", NULL);
                FreeUPNPUrls(&tcp_urls);
            } else if (tcp_method == PORT_FORWARD_NATPMP) {
                tcp_natpmp(tcp_mapped_port, 0);
            }
            LbNetLog("Port forwarding: TCP port %u forward removed\n", tcp_mapped_port);
            tcp_mapped_port = 0;
            tcp_method = PORT_FORWARD_NONE;
        }
        if ((port != 0) && (generation == tcp_generation)) {
            if (is_cgnat_detected()) {
                LbNetLog("Port forwarding: CGNAT, the World List hub's TCP port %u can't be forwarded\n", port);
            } else if (getenv("KFX_PORTFORWARD_DRYRUN") != NULL) {
                LbNetLog("Port forwarding: would ask the router for TCP port %u (dry run)\n", port);
            } else if (tcp_natpmp(port, 0x7FFFFFFF)) {
                LbNetLog("NAT-PMP: TCP port %u (World List hub) forwarded\n", port);
                tcp_mapped_port = port;
                tcp_method = PORT_FORWARD_NATPMP;
            } else if (tcp_upnp_add(port)) {
                tcp_mapped_port = port;
                tcp_method = PORT_FORWARD_UPNP;
            } else {
                LbNetLog("Port forwarding: the router didn't forward TCP port %u; for players elsewhere, forward it by hand\n", port);
            }
        }
#ifdef _WIN32
        WSACleanup();
#endif
    });
}

/******************************************************************************/
/* Keeping the forwards (2026-11-14). A router forgets a forward when its lease ends, when it restarts, and when
   another program on this computer takes the port away: a game that hosted on a dedicated server's port for a few
   seconds removed the server's forward as it left, and nobody could reach the server by its internet address after
   that. So a host looks about once a minute whether the router still has its forwards, and asks again if not. */
#define PORT_FORWARD_CHECK_MS 45000
#define PORT_FORWARD_NATPMP_RENEW_MS (20 * 60 * 1000)
#define PORT_FORWARD_LEASE_LOW 600 /* seconds left on a timed lease at which it is asked for again */

/** Whether the router still forwards the port to this computer; lease: the seconds its lease has left (0: no end). */
static bool upnp_has_mapping(const struct UPNPUrls *urls, const struct IGDdatas *data, const char *lanaddr, uint16_t port,
    const char *proto, long *lease)
{
    char port_string[16];
    char client[64] = "";
    char internal[16] = "";
    char description[96] = "";
    char enabled[16] = "";
    char duration[32] = "";
    snprintf(port_string, sizeof(port_string), "%u", port);
    const int result = UPNP_GetSpecificPortMappingEntry(urls->controlURL, data->first.servicetype, port_string, proto, NULL,
        client, internal, description, enabled, duration);
    *lease = atol(duration);
    return (result == UPNPCOMMAND_SUCCESS) && (strcmp(client, lanaddr) == 0) && (atoi(internal) == (int)port);
}

/** Asks the router for the forward once more (the same request as the first time); 1 if it agreed. */
static int upnp_ask_again(const struct UPNPUrls *urls, const struct IGDdatas *data, const char *lanaddr, uint16_t port,
    const char *proto, const char *description)
{
    char port_string[16];
    snprintf(port_string, sizeof(port_string), "%u", port);
    int result = UPNP_AddPortMapping(urls->controlURL, data->first.servicetype, port_string, port_string, lanaddr, description, proto, "", "0");
    if (result != UPNPCOMMAND_SUCCESS) {
        result = UPNP_AddPortMapping(urls->controlURL, data->first.servicetype, port_string, port_string, lanaddr, description, proto, "", "3600");
    }
    if (result != UPNPCOMMAND_SUCCESS) {
        LbNetLog("UPnP: the router refused %s port %u this time (error %d, %s)\n", proto, port, result, strupnperror(result));
        return 0;
    }
    return 1;
}

static void keep_udp_forward(void)
{
    static unsigned long natpmp_renewed_ms = 0;
    static unsigned int asked = 0;
    if ((mapped_port == 0) || (active_method == PORT_FORWARD_NONE)) {
        return;
    }
    const uint16_t port = mapped_port;
    if (active_method == PORT_FORWARD_UPNP) {
        long lease = 0;
        if (upnp_has_mapping(&upnp_urls, &upnp_data, upnp_lanaddr, port, "UDP", &lease) && ((lease == 0) || (lease > PORT_FORWARD_LEASE_LOW))) {
            asked = 0;
            return;
        }
        /* (a router that can't be asked about a forward gets the request each time: said once, then now and then) */
        const bool say = ((asked % 20) == 0);
        asked++;
        if (say) {
            LbNetLog("UPnP: the router no longer forwards UDP port %u to this computer: asking for it again\n", port);
        }
        if (upnp_ask_again(&upnp_urls, &upnp_data, upnp_lanaddr, port, "UDP", "KeeperFX")) {
            if (say) {
                LbNetLog("UPnP: port forwarding active on port %u again\n", port);
            }
            set_status(1, "UDP port %u forwarded by the router (UPnP, to %s)", port, upnp_lanaddr);
        } else {
            set_status(0, "the router dropped the forward of UDP port %u and refused it again (forward it by hand)", port);
        }
    } else if (active_method == PORT_FORWARD_NATPMP) {
        const unsigned long now = (unsigned long)LbTimerClock();
        if (natpmp_renewed_ms == 0) {
            natpmp_renewed_ms = now;
        }
        if ((now - natpmp_renewed_ms) >= PORT_FORWARD_NATPMP_RENEW_MS) {
            natpmp_renewed_ms = now;
            natpmp_add_port_mapping(port); /* the same request renews it */
        }
    }
}

static void keep_tcp_forward(void)
{
    static unsigned long natpmp_renewed_ms = 0;
    static unsigned int asked = 0;
    if ((tcp_mapped_port == 0) || (tcp_method == PORT_FORWARD_NONE)) {
        return;
    }
    const uint16_t port = tcp_mapped_port;
    if (tcp_method == PORT_FORWARD_UPNP) {
        long lease = 0;
        if (upnp_has_mapping(&tcp_urls, &tcp_data, tcp_lanaddr, port, "TCP", &lease) && ((lease == 0) || (lease > PORT_FORWARD_LEASE_LOW))) {
            asked = 0;
            return;
        }
        const bool say = ((asked % 20) == 0);
        asked++;
        if (say) {
            LbNetLog("UPnP: the router no longer forwards TCP port %u (World List hub) to this computer: asking for it again\n", port);
        }
        if (upnp_ask_again(&tcp_urls, &tcp_data, tcp_lanaddr, port, "TCP", "KeeperFX World List") && say) {
            LbNetLog("UPnP: TCP port %u (World List hub) forwarded to %s again\n", port, tcp_lanaddr);
        }
    } else if (tcp_method == PORT_FORWARD_NATPMP) {
        const unsigned long now = (unsigned long)LbTimerClock();
        if (natpmp_renewed_ms == 0) {
            natpmp_renewed_ms = now;
        }
        if ((now - natpmp_renewed_ms) >= PORT_FORWARD_NATPMP_RENEW_MS) {
            natpmp_renewed_ms = now;
            tcp_natpmp(port, 0x7FFFFFFF);
        }
    }
}

void port_forward_keepalive(void)
{
    static unsigned long checked_ms = 0;
    const unsigned long now = (unsigned long)LbTimerClock();
    if ((checked_ms != 0) && ((now - checked_ms) < PORT_FORWARD_CHECK_MS)) {
        return;
    }
    checked_ms = now;
    /* each look goes after whatever its port's task is doing (the same chain as setting and removing a forward),
       and is dropped when the port has been set anew meanwhile */
    {
        const uint32_t generation = mapping_generation;
        mapping_task = std::async(std::launch::async, [previous = std::move(mapping_task), generation]() mutable {
            if (previous.valid()) {
                previous.wait();
                previous = {};
            }
            if (generation != mapping_generation) {
                return;
            }
#ifdef _WIN32
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                return;
            }
#endif
            keep_udp_forward();
#ifdef _WIN32
            WSACleanup();
#endif
        });
    }
    {
        const uint32_t generation = tcp_generation;
        tcp_task = std::async(std::launch::async, [previous = std::move(tcp_task), generation]() mutable {
            if (previous.valid()) {
                previous.wait();
                previous = {};
            }
            if (generation != tcp_generation) {
                return;
            }
#ifdef _WIN32
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                return;
            }
#endif
            keep_tcp_forward();
#ifdef _WIN32
            WSACleanup();
#endif
        });
    }
}

void port_forward_wait(void)
{
    if (mapping_task.valid()) {
        mapping_task.wait_for(std::chrono::seconds(5));
    }
    if (tcp_task.valid()) {
        tcp_task.wait_for(std::chrono::seconds(5));
    }
}

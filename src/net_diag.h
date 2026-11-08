/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_diag.h
 *     Network diagnostics: socket error texts, this PC's address, and the network log file.
 */
/******************************************************************************/
#ifndef NET_DIAG_H
#define NET_DIAG_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The last socket error of this thread (WSAGetLastError() on Windows, errno elsewhere). */
int net_socket_error(void);
/** True when the error only means "nothing to read or send yet". */
TbBool net_socket_would_block(int err);
/** What a socket error means, with its number: "the other side reset the connection (error 10054)". */
const char *net_socket_error_text(int err);
/** Why a join was turned away or a peer was dropped (a NetJoinRejection value), in plain words. */
const char *net_join_reason_text(int reason);
/** This PC's address on its network (the one its internet traffic leaves from); no packet is sent. */
TbBool net_local_ipv4(char *buf, size_t size);

/** Starts the network log: every "Net:" line, with the date and time, appended to this file (real addresses,
    unlike keeperfx.log). Kept below 4 MB: a bigger one becomes <file>.old. */
void net_log_file_setup(const char *path, const char *role);
void net_log_file_write(const char *text);

#ifdef __cplusplus
}
#endif
#endif

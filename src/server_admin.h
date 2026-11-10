/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file server_admin.h
 *     Header file for server_admin.c.
 */
/******************************************************************************/
#ifndef DK_SERVER_ADMIN_H
#define DK_SERVER_ADMIN_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The sender byte of a chat line that is a notice from the server (shown with no name). */
#define SERVER_NOTICE_SENDER 0xFE
/** The sender byte of the server's last words to a client it drops (kicked, banned): kept, shown on the menu. */
#define SERVER_FAREWELL_SENDER 0xFD

/** -adminport / -admintoken (keeperserver.exe gives both; the token can also come in KFX_ADMIN_TOKEN). */
void server_admin_configure(int port, const char *token);
TbBool server_admin_enabled(void);
/** Every pass of the game's loops (dedicated server): the admin connection (accepts, commands, sends the events). */
void server_admin_poll(void);
/** Every game turn on a server: play time, the status event. */
void server_admin_turn(void);
/** A line for keeperserver's console (kind: boot, world, connect, join, leave, death, defeat, level, portal, admin,
 *  warn, error). Kept for an admin that connects later too. */
void server_admin_log(const char *kind, const char *fmt, ...);
/** A chat line someone sent (logged with the sender's seat, for its colour). */
void server_admin_chat(int user, int plyr_idx, const char *name, const char *msg);
/** Is this user muted? (its chat goes nowhere and it is told so) */
TbBool server_admin_user_muted(int user);
/** Refuse a login: is this token, name or address banned? */
TbBool server_admin_banned(uint64_t token, const char *name, const char *ip);
/** A notice from the server to one user (user >= 0) or to everyone (-1). */
void server_admin_notice(int user, const char *text);
/** A user left the server (how: 0 left, 1 timed out, 2 dropped). */
void server_admin_user_left(int user, int how);
/** A seated player's in-game "!" command that is one of the admin's, run when a role allows it (server_roles.c): 1 when
 *  it was one (run or refused here), 0 when it is the console's own. */
TbBool server_admin_player_command(int user, const char *text);
/** A seated player's own leave packet (before the drop it causes): "left the world" / "timed out". */
void server_admin_user_leaving(int user, TbBool timed_out);
/** The process is closing: say bye to the admin. */
void server_admin_shutdown(TbBool restart);
/** A player's name as the console shows it ("Keeper 7" for a world keeper with none). */
const char *server_admin_player_name(PlayerNumber plyr_idx);

/** A thin client: the text the server's last words gave (kicked, banned), or NULL. */
const char *server_farewell_text(void);
void server_farewell_set(const char *text);

#ifdef __cplusplus
}
#endif
#endif

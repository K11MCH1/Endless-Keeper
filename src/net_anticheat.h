/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_anticheat.h
 *     Server-side anti-cheat for worlds the server alone simulates (thin clients).
 * @par Purpose:
 *     A player's game only sends input. This checks that input before the simulation takes it, limits how fast a
 *     player may send, and keeps a record of what was refused for the server's admin.
 */
/******************************************************************************/
#ifndef NET_ANTICHEAT_H
#define NET_ANTICHEAT_H

#include "globals.h"
#include "net_main.h"
#include "packets.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a report is about. */
enum AnticheatKind {
    ACk_Impossible = 0, /**< a command an unmodified game cannot send (sizes out of range, cheat states, ...) */
    ACk_Refused,        /**< a command for something that is not the player's (it can also be a click that came late) */
    ACk_Flood,          /**< more messages or chat than a person sends */
    ACk_Login,          /**< a login this server does not take */
    ACk_Count
};

/** True where the checks apply: this machine is the server of a world it alone simulates. */
TbBool anticheat_active(void);
/** A new session starts, or a user comes or goes: its counters start again. */
void anticheat_reset(void);
void anticheat_user_reset(NetUserId user);
/** The server looks at a remote user's input before the simulation takes it. A command the player may not give is
 *  taken out of the packet (the rest of the input, such as the pointer and the camera, stays). */
void anticheat_check_packet(NetUserId user, struct Packet *pckt);
/** A chat line from a remote user: false when the user sends faster than a person types (the line is dropped). */
TbBool anticheat_chat_allowed(NetUserId user);
/** A network message from a remote user is about to be read: false when its share of the moment is used up
 *  (the rest waits in the connection's queue). */
TbBool anticheat_message_allowed(NetUserId user);
/** Counts and logs something a player's game did (the game log, and the admin log as kind "cheat"). */
void anticheat_report(NetUserId user, enum AnticheatKind kind, const char *fmt, ...);
/** How many reports of a kind a user has had since it joined. */
unsigned long anticheat_count(NetUserId user, enum AnticheatKind kind);
/** Test hook (KFX_NET_TESTSAY "#cheat <what>", on a client): this game sends what a changed game would, or logs
 *  what it has been sent ("fog"). See anticheat_test in net_anticheat.c for the words. */
void anticheat_test(const char *what);
/** A login name as it is shown and logged: control characters and bytes that are not text become '?'. */
void anticheat_clean_name(char *name);

#ifdef __cplusplus
}
#endif
#endif

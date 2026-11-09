/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_state.h
 *     Thin-client network play: the server streams the game state, clients only draw it.
 * @par Purpose:
 *     In an endless world played over the network, only the host runs the game. After every turn it sends each
 *     client what changed: the bytes of the game state that changed (as replay rewind diffs them), the world's chunk
 *     pages, and the presentation events (sounds, speech, effects). A client copies the state in and never simulates,
 *     so it cannot drift from the server, and a slow or frozen client holds up nobody. Clients send their inputs; the
 *     host takes them as they come, one a turn, and never waits. Normal levels keep lockstep.
 */
/******************************************************************************/
#ifndef DK_NET_STATE_H
#define DK_NET_STATE_H

#include "bflib_basics.h"
#include "globals.h"
#include "net_main.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Packet;

/** The session plays thin-client: a network game in an endless world (unless KFX_NET_LOCKSTEP is set). */
TbBool netstate_thin_mode(void);
/** This machine is the thin-client server (the host of such a session). */
TbBool netstate_is_server(void);
/** This machine is a thin client (a seated player or a spectator of such a session, not the host). */
TbBool netstate_is_client(void);

/** Dedicated server (-dedicated): the world opens to the network by itself and the host plays no keeper. */
void netstate_set_dedicated(TbBool on);
TbBool netstate_dedicated(void);
/** A client's NETMSG_STATE_CAPS (what it reads), on the server. */
void netstate_server_caps(NetUserId source, const char *buffer, size_t size);
/** -loadslot: the save to load at startup (-1 none; -2 the newest endless save). */
void netstate_set_startup_load_slot(int slot);
int netstate_take_startup_load_slot(void);
int netstate_take_startup_load_slot_peek(void);
/** The thin-client server's ms a turn over the last 200 turns: the simulation, the state stream. */
void netstate_server_timing(double *sim_ms, double *stream_ms);

/** Level start and end: buffers, the shadow copy, the client's turn queue. */
void netstate_level_start(void);
void netstate_level_end(void);

/* --- server --- */
/** After each game turn: diff the state against what the clients have, send the turn's record, and keyframes to the
 *  clients that need one. */
void netstate_server_after_turn(void);
/** An endless world network game takes MAX_NET_USERS users (a stock one keeps its lobby's). */
void netstate_take_all_users(void);
/** The server's simulation time for a turn (game_loop), for the stats line. */
void netstate_server_sim_time(int64_t ns);
/** The input packet a client's user plays this turn: its next input in order, or "keeping still". */
void netstate_server_take_input(NetUserId user, struct Packet *out);
/** A client asked for a keyframe (it joined, fell too far behind, or its check failed). */
void netstate_server_request_keyframe(NetUserId user);
/** A thin client: a camera position it put in a packet (its own moves coming back from the server aren't followed). */
void netstate_client_sent_camera(MapCoord x, MapCoord y);
/** A user slot gets a new occupant: its inputs start afresh. */
void netstate_server_user_reset(NetUserId user);

/* --- client --- */
/** A record or keyframe arrived from the server: queue it (the message is copied). */
TbError netstate_client_receive(const char *buffer, size_t size);
/** How many turns are waiting to be applied. */
int netstate_client_queued(void);
/** Applies the next queued turn; false when there is none yet. */
TbBool netstate_client_apply_next(void);
/** This client was given a seat: no inputs until the keyframe that comes with it. */
void netstate_client_await_keyframe(void);
/** Each turn tick: the local input packet goes to the server with the next input number. */
void netstate_client_send_input(void);
const struct Packet *netstate_client_last_input(void);
void netstate_client_note_stall(void);

/* --- presentation events recorded on the server, replayed on the clients --- */
/** A thing played a sample (thing_play_sample). */
void netstate_event_thing_sample(ThingIndex thing_idx, short smptbl_idx, unsigned short pitch, char fil1D, unsigned char ctype,
    unsigned char flags, long priority, unsigned long loudness);
/** A sample or speech meant for one player only (today gated by is_my_player). */
void netstate_event_player_message(PlayerNumber plyr_idx, long msg_idx, long delay);
void netstate_event_room_message(PlayerNumber plyr_idx, long rkind, long msg_kind);
void netstate_event_player_sample(PlayerNumber plyr_idx, long smptbl_idx);
/** While on, the server records no events (its own camera's footsteps and ambience are local). Nests. */
void netstate_events_suppress(TbBool on);

#ifdef __cplusplus
}
#endif
#endif

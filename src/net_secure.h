/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_secure.h
 *     Endless worlds: logins that never give a server a player's reconnect secret.
 * @par Purpose:
 *     Each machine keeps one secret (settings.toml reconnect_token). A server of an endless world has a key pair
 *     (X25519, kept in the world's save; a new world takes this machine's, save/world_key.bin). A joining client asks
 *     for the public half (NETMSG_KEYREQ / NETMSG_KEYINFO) and sends, sealed to it (XChaCha20-Poly1305 with a key
 *     from X25519 with a one-time key of its own), its token for that server only, BLAKE2b(secret, server key), and
 *     the server password. So a server learns nothing it could use on another server, someone listening on the
 *     network learns nothing, and a captured login can't be sent again (it is bound to the server's one-time nonce).
 *     An older client still sends its secret: the server turns it into the same per-server token. A server that
 *     doesn't answer (older, or not an endless world) gets the login as before.
 */
/******************************************************************************/
#ifndef DK_NET_SECURE_H
#define DK_NET_SECURE_H

#include "bflib_basics.h"
#include "globals.h"
#include "net_main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETSEC_KEY_LEN 32
#define NETSEC_MAGIC 0x31534B45u /* "EKS1" */

/** Random bytes from the operating system (for keys and nonces). */
void netsec_random(void *buf, size_t n);
/** A secret's token for one server: the first 8 bytes of BLAKE2b keyed with the secret over the server's public key
 *  (never 0). An older client's secret becomes the same value on the server. */
uint64_t netsec_derive(uint64_t secret, const uint8_t server_pub[NETSEC_KEY_LEN]);

/* the world's key (the server's side) */
/** The key of a loaded save (its secret half). */
void netsec_world_key_set(const uint8_t secret[NETSEC_KEY_LEN]);
/** This machine's own key (save/world_key.bin, made the first time): for a world that has none yet. */
void netsec_world_key_install(void);
/** Is there a world key (its public half into pub, when not NULL; secret into sec, when not NULL). */
TbBool netsec_world_key(uint8_t pub[NETSEC_KEY_LEN], uint8_t sec[NETSEC_KEY_LEN]);
/** A token as this world knows its owner: a secret (an older client's, a save from before) turned per server. */
uint64_t netsec_world_token(uint64_t secret);
/** This machine's own token in the world (the host's seat). */
uint64_t netsec_local_world_token(uint64_t local_secret);
/** This machine answers key requests now: an endless world it hosts. Also turns the tokens of users logged in
 *  before the world had its key into this world's. */
TbBool netsec_server_ready(void);

/* the login */
/** The server: a client asks for the key (NETMSG_KEYREQ). */
void netsec_server_keyreq(NetUserId source, const char *buf, size_t size);
/** The client: the server's key and nonce (NETMSG_KEYINFO). */
void netsec_client_keyinfo(const char *buf, size_t size);
/** The client, before a login: forget the last server's key; after asking, has it come. */
void netsec_client_reset(void);
TbBool netsec_client_has_key(void);
/** The client: the sealed part of the login (token for this server, password) into dst; its length (0: none). */
size_t netsec_client_login_ext(char *dst, size_t room, uint64_t secret, const char *password);
/** The server: reads the sealed part of a login, if any (1: read, token and password set; 0: none; -1: broken or
 *  not sealed to this server's key and nonce). */
int netsec_server_login_ext(NetUserId source, const char *p, const char *end, uint64_t *token, char *password,
    size_t password_room);
/** The server: a user's token came as an older client's secret (turned once the world has its key). */
void netsec_server_mark_raw(NetUserId user, TbBool raw);

#ifdef __cplusplus
}
#endif
#endif

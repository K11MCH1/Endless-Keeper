/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_secure.c
 *     Endless worlds: logins that never give a server a player's reconnect secret (see net_secure.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "net_secure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "../deps/monocypher/monocypher.h"
#include "bflib_basics.h"
#include "net_main.h"
#include "world_stream.h"
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include <sys/random.h>
#endif
#include "post_inc.h"

#ifdef _WIN32
/* bcrypt.h's, declared here so this file needn't pull in windows.h (the game links bcrypt) */
__declspec(dllimport) long __stdcall BCryptGenRandom(void *algorithm, unsigned char *buffer, unsigned long count,
    unsigned long flags);
#define NETSEC_BCRYPT_SYSTEM_RNG 0x00000002u
#endif

#define NETSEC_KEY_FILE "save/world_key.bin"
#define NETSEC_NONCE_LEN 16
#define NETSEC_MAC_LEN 16
#define NETSEC_PW_MAX 255
#define NETSEC_EXT_HEAD (1 + NETSEC_KEY_LEN + NETSEC_MAC_LEN) /* 'K', the client's one-time public key, the MAC */

/******************************************************************************/
void netsec_random(void *buf, size_t n)
{
#ifdef _WIN32
    if (BCryptGenRandom(NULL, (unsigned char *)buf, (unsigned long)n, NETSEC_BCRYPT_SYSTEM_RNG) >= 0)
        return;
#elif defined(__ANDROID__) || defined(__APPLE__)
    arc4random_buf(buf, n);
    return;
#else
    {
        size_t got = 0;
        while (got < n)
        {
            const ssize_t r = getrandom((uint8_t *)buf + got, n - got, 0);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        if (got == n)
            return;
    }
#endif
    /* never expected: the system had no random bytes. Better than nothing, and said */
    ERRORLOG("No random bytes from the system: using the game's generator");
    for (size_t i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = (uint8_t)SDL_rand_bits();
}

uint64_t netsec_derive(uint64_t secret, const uint8_t server_pub[NETSEC_KEY_LEN])
{
    uint8_t key[8], msg[8 + NETSEC_KEY_LEN], h[8];
    memcpy(key, &secret, 8);
    memcpy(msg, "EKSEAT1", 8); /* with its 0 */
    memcpy(msg + 8, server_pub, NETSEC_KEY_LEN);
    crypto_blake2b_keyed(h, sizeof(h), key, sizeof(key), msg, sizeof(msg));
    crypto_wipe(key, sizeof(key));
    uint64_t t;
    memcpy(&t, h, 8);
    return (t != 0) ? t : 1;
}

/******************************************************************************/
/* the world's key */
static uint8_t wk_sec[NETSEC_KEY_LEN], wk_pub[NETSEC_KEY_LEN];
static TbBool wk_ready;

void netsec_world_key_set(const uint8_t secret[NETSEC_KEY_LEN])
{
    memcpy(wk_sec, secret, NETSEC_KEY_LEN);
    crypto_x25519_public_key(wk_pub, wk_sec);
    wk_ready = true;
}

void netsec_world_key_install(void)
{
    uint8_t sec[NETSEC_KEY_LEN];
    FILE *f = fopen(NETSEC_KEY_FILE, "rb");
    TbBool have = false;
    if (f != NULL)
    {
        have = (fread(sec, 1, sizeof(sec), f) == sizeof(sec));
        fclose(f);
    }
    if (!have)
    {
        netsec_random(sec, sizeof(sec));
        f = fopen(NETSEC_KEY_FILE, "wb");
        if ((f == NULL) || (fwrite(sec, 1, sizeof(sec), f) != sizeof(sec)))
            WARNLOG("World key: can't write %s; this run's worlds get a key of their own", NETSEC_KEY_FILE);
        else
            SYNCLOG("World key: made this machine's (%s)", NETSEC_KEY_FILE);
        if (f != NULL)
            fclose(f);
    }
    netsec_world_key_set(sec);
    crypto_wipe(sec, sizeof(sec));
}

TbBool netsec_world_key(uint8_t pub[NETSEC_KEY_LEN], uint8_t sec[NETSEC_KEY_LEN])
{
    if (!wk_ready)
        return false;
    if (pub != NULL)
        memcpy(pub, wk_pub, NETSEC_KEY_LEN);
    if (sec != NULL)
        memcpy(sec, wk_sec, NETSEC_KEY_LEN);
    return true;
}

uint64_t netsec_world_token(uint64_t secret)
{
    if (secret == 0)
        return 0;
    if (!wk_ready)
        netsec_world_key_install();
    return netsec_derive(secret, wk_pub);
}

uint64_t netsec_local_world_token(uint64_t local_secret)
{
    return world_mode_active() ? netsec_world_token(local_secret) : local_secret;
}

/******************************************************************************/
/* the server */
static uint8_t srv_nonce[MAX_NET_CONNECTIONS][NETSEC_NONCE_LEN];
static TbBool srv_nonce_ok[MAX_NET_CONNECTIONS];
static TbBool user_raw[MAX_NET_CONNECTIONS];
static TbBool any_raw;

void netsec_server_mark_raw(NetUserId user, TbBool raw)
{
    if ((user <= SERVER_ID) || (user >= MAX_NET_CONNECTIONS))
        return;
    user_raw[user] = raw;
    any_raw |= raw;
}

TbBool netsec_server_ready(void)
{
    if (!world_mode_active())
        return false;
    if (!wk_ready)
        netsec_world_key_install();
    if (any_raw)
    {
        /* logged in before the world had its key (a lobby): their secrets become this world's tokens */
        any_raw = false;
        for (NetUserId u = SERVER_ID + 1; u < MAX_NET_CONNECTIONS; u++)
        {
            if (!user_raw[u])
                continue;
            user_raw[u] = false;
            const uint64_t raw = netstate.users[u].reconnect_token;
            netstate.users[u].reconnect_token = netsec_derive(raw, wk_pub);
            if ((u < MAX_NET_USERS) && (raw != 0) && (netstate.reconnect_tokens[u] == raw))
                netstate.reconnect_tokens[u] = netstate.users[u].reconnect_token;
        }
    }
    return true;
}

void netsec_server_keyreq(NetUserId source, const char *buf, size_t size)
{
    uint32_t magic = 0;
    if ((source <= SERVER_ID) || (source >= MAX_NET_CONNECTIONS) || (size < 4))
        return;
    memcpy(&magic, buf, 4);
    if ((magic != NETSEC_MAGIC) || !netsec_server_ready())
        return;
    netsec_random(srv_nonce[source], NETSEC_NONCE_LEN);
    srv_nonce_ok[source] = true;
    char *wp = begin_net_message(NETMSG_KEYINFO);
    memcpy(wp, &magic, 4);
    memcpy(wp + 4, wk_pub, NETSEC_KEY_LEN);
    memcpy(wp + 4 + NETSEC_KEY_LEN, srv_nonce[source], NETSEC_NONCE_LEN);
    send_message_buffer(source, wp + 4 + NETSEC_KEY_LEN + NETSEC_NONCE_LEN);
}

/** The key that seals a login: BLAKE2b over the X25519 secret, keyed by it, of both public keys. */
static void login_key(uint8_t key[32], const uint8_t shared[32], const uint8_t client_pub[32], const uint8_t server_pub[32])
{
    uint8_t msg[2 * NETSEC_KEY_LEN];
    memcpy(msg, client_pub, NETSEC_KEY_LEN);
    memcpy(msg + NETSEC_KEY_LEN, server_pub, NETSEC_KEY_LEN);
    crypto_blake2b_keyed(key, 32, shared, 32, msg, sizeof(msg));
}

/** What a sealed login is bound to: this protocol, the server's key and its nonce for this connection. */
static void login_ad(uint8_t ad[8 + NETSEC_KEY_LEN + NETSEC_NONCE_LEN], const uint8_t server_pub[32], const uint8_t nonce[16])
{
    memcpy(ad, "EKLOGIN1", 8);
    memcpy(ad + 8, server_pub, NETSEC_KEY_LEN);
    memcpy(ad + 8 + NETSEC_KEY_LEN, nonce, NETSEC_NONCE_LEN);
}

int netsec_server_login_ext(NetUserId source, const char *p, const char *end, uint64_t *token, char *password,
    size_t password_room)
{
    if ((p == NULL) || (p >= end) || (*p != 'K'))
        return 0;
    if ((source <= SERVER_ID) || (source >= MAX_NET_CONNECTIONS) || !srv_nonce_ok[source] || !netsec_server_ready())
        return -1;
    srv_nonce_ok[source] = false; /* one login per nonce */
    const size_t len = (size_t)(end - p);
    if ((len < NETSEC_EXT_HEAD + 9) || (len > NETSEC_EXT_HEAD + 9 + NETSEC_PW_MAX))
        return -1;
    const uint8_t *client_pub = (const uint8_t *)p + 1, *mac = client_pub + NETSEC_KEY_LEN, *ct = mac + NETSEC_MAC_LEN;
    const size_t ct_len = len - NETSEC_EXT_HEAD;
    uint8_t shared[32], key[32], ad[8 + NETSEC_KEY_LEN + NETSEC_NONCE_LEN], plain[9 + NETSEC_PW_MAX];
    static const uint8_t nonce[24]; /* each login's key is new (the client's one-time key) */
    crypto_x25519(shared, wk_sec, client_pub);
    login_key(key, shared, client_pub, wk_pub);
    login_ad(ad, wk_pub, srv_nonce[source]);
    const int bad = crypto_aead_unlock(plain, mac, key, nonce, ad, sizeof(ad), ct, ct_len);
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(key, sizeof(key));
    if ((bad != 0) || ((size_t)plain[8] + 9 != ct_len))
    {
        crypto_wipe(plain, sizeof(plain));
        return -1;
    }
    memcpy(token, plain, 8);
    const size_t pw = min((size_t)plain[8], (password_room > 0) ? password_room - 1 : 0);
    if (password_room > 0)
    {
        memcpy(password, plain + 9, pw);
        password[pw] = '\0';
    }
    crypto_wipe(plain, sizeof(plain));
    return 1;
}

/******************************************************************************/
/* the client */
static uint8_t cl_pub[NETSEC_KEY_LEN], cl_nonce[NETSEC_NONCE_LEN];
static TbBool cl_have;

void netsec_client_reset(void)
{
    cl_have = false;
}

TbBool netsec_client_has_key(void)
{
    return cl_have;
}

void netsec_client_keyinfo(const char *buf, size_t size)
{
    uint32_t magic = 0;
    if (size < 4 + NETSEC_KEY_LEN + NETSEC_NONCE_LEN)
        return;
    memcpy(&magic, buf, 4);
    if (magic != NETSEC_MAGIC)
        return;
    memcpy(cl_pub, buf + 4, NETSEC_KEY_LEN);
    memcpy(cl_nonce, buf + 4 + NETSEC_KEY_LEN, NETSEC_NONCE_LEN);
    cl_have = true;
}

size_t netsec_client_login_ext(char *dst, size_t room, uint64_t secret, const char *password)
{
    if (!cl_have)
        return 0;
    const size_t pw = min(strlen(password), (size_t)NETSEC_PW_MAX);
    const size_t need = NETSEC_EXT_HEAD + 9 + pw;
    if (room < need)
        return 0;
    uint8_t e_sec[32], e_pub[32], shared[32], key[32], ad[8 + NETSEC_KEY_LEN + NETSEC_NONCE_LEN], plain[9 + NETSEC_PW_MAX];
    static const uint8_t nonce[24];
    netsec_random(e_sec, sizeof(e_sec));
    crypto_x25519_public_key(e_pub, e_sec);
    crypto_x25519(shared, e_sec, cl_pub);
    login_key(key, shared, e_pub, cl_pub);
    login_ad(ad, cl_pub, cl_nonce);
    const uint64_t token = netsec_derive(secret, cl_pub);
    memcpy(plain, &token, 8);
    plain[8] = (uint8_t)pw;
    memcpy(plain + 9, password, pw);
    uint8_t *out = (uint8_t *)dst;
    out[0] = 'K';
    memcpy(out + 1, e_pub, NETSEC_KEY_LEN);
    crypto_aead_lock(out + NETSEC_EXT_HEAD, out + 1 + NETSEC_KEY_LEN, key, nonce, ad, sizeof(ad), plain, 9 + pw);
    crypto_wipe(e_sec, sizeof(e_sec));
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(key, sizeof(key));
    crypto_wipe(plain, sizeof(plain));
    cl_have = false; /* the nonce was for this login */
    return need;
}

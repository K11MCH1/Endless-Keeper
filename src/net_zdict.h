/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_zdict.h
 *     zstd dictionaries for the thin-client stream, trained on the stream of endless worlds (2026-11-09).
 */
/******************************************************************************/
#ifndef DK_NET_ZDICT_H
#define DK_NET_ZDICT_H

#ifdef __cplusplus
extern "C" {
#endif

#define NS_ZDICT_SHARED_ID 17001u /* the turn's shared part */
#define NS_ZDICT_OWN_ID 17002u    /* a client's own part */
extern const unsigned char ns_zdict_shared[];
extern const unsigned int ns_zdict_shared_len;
extern const unsigned char ns_zdict_own[];
extern const unsigned int ns_zdict_own_len;

#ifdef __cplusplus
}
#endif
#endif

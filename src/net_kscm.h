/******************************************************************************/
/** @file net_kscm.h
 *     KSC-M: a thin-client keyframe coded by the shape of the game's memory (research prototype 2026-11-28).
 */
/******************************************************************************/
#ifndef NET_KSCM_H
#define NET_KSCM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/** What a keyframe record is made of, as net_state.c builds it. */
struct KscmFrame {
    uint32_t header_len;       /* struct NsHeader */
    uint8_t kind_memfull;      /* NsK_MemFull: region u8, length u32, the bytes */
    uint8_t kind_pageadd;      /* NsK_PageAdd: cx i32, cy i32, a page payload */
    uint8_t region_game;       /* NsR_Game: struct Game */
    uint32_t page_map, page_stat, page_light, page_slab, page_ext; /* the parts of a page payload */
};

/** Builds the coding plan from the save tables (main thread, once; later calls do nothing). False: no memory. */
int kscm_init(const struct KscmFrame *frame);
/** Codes a keyframe record (thread-safe after kscm_init). *out is malloc'd. */
int kscm_encode(const uint8_t *rec, size_t len, uint8_t **out, size_t *out_len);
/** Test hook: this machine's layout hash is changed, as a build with another layout would have. */
void kscm_test_spoil_layout(void);
/** Decodes a KSC-M body into rec (exactly len bytes). False: damaged, or made by a build with another layout. */
int kscm_decode(const uint8_t *in, size_t in_len, uint8_t *rec, size_t len);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

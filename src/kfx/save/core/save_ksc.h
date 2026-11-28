/******************************************************************************/
/** @file save_ksc.h
 *     KSC: a record coded by the shape its schema gives it (research prototype, 2026-11-28; see save_ksc.cpp).
 */
/******************************************************************************/
#ifndef KFX_SAVE_KSC_H
#define KFX_SAVE_KSC_H

#include "save_codec.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/** Codes `rec` (one record of struct `root`, encoded as the SCHM chunk `schema` describes) into `out`. */
enum SaveResult save_ksc_encode(const uint8_t *schema, uint32_t schema_len, const char *root, const uint8_t *rec,
    uint32_t rec_len, struct SaveBuffer *out, struct SaveError *err);
/** Decodes a KSC body into `rec`, which has room for exactly `rec_len` bytes (the chunk's raw length). */
enum SaveResult save_ksc_decode(const uint8_t *schema, uint32_t schema_len, const uint8_t *in, uint32_t in_len,
    uint8_t *rec, uint32_t rec_len, struct SaveError *err);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

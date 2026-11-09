/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file kfx_deflate.h
 *     zlib-format compression through libdeflate: the same streams zlib makes and reads, about twice as fast, for
 *     whole buffers (endless worlds' chunk records and saves, the thin-client stream).
 */
/******************************************************************************/
#ifndef DK_KFX_DEFLATE_H
#define DK_KFX_DEFLATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Room needed for kfx_zlib_compress's output of len bytes. */
size_t kfx_zlib_bound(size_t len);
/** Compresses src into dst (zlib format) at a level 1-12; *dst_len is dst's room in, the length out. Thread-safe. */
int kfx_zlib_compress(void *dst, size_t *dst_len, const void *src, size_t len, int level);
/** Decompresses a zlib stream into dst (room dst_room); *out_len is the length made. Thread-safe. */
int kfx_zlib_uncompress(void *dst, size_t dst_room, const void *src, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif

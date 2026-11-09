/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file kfx_deflate.c
 *     zlib-format compression through libdeflate (see kfx_deflate.h).
 */
/******************************************************************************/
#include "kfx_deflate.h"
#include <zlib.h>
#include "../deps/libdeflate/libdeflate.h"

#define KFX_DEFLATE_LEVELS 13

/* each thread its own (a compressor or decompressor isn't thread-safe; the stream's workers compress too) */
static __thread struct libdeflate_compressor *compressors[KFX_DEFLATE_LEVELS];
static __thread struct libdeflate_decompressor *decompressor;

size_t kfx_zlib_bound(size_t len)
{
    const size_t zb = (size_t)compressBound((uLong)len);
    const size_t lb = libdeflate_zlib_compress_bound(NULL, len);
    return (zb > lb) ? zb : lb;
}

int kfx_zlib_compress(void *dst, size_t *dst_len, const void *src, size_t len, int level)
{
    if ((level < 1) || (level >= KFX_DEFLATE_LEVELS))
        level = 1;
    if (compressors[level] == NULL)
        compressors[level] = libdeflate_alloc_compressor(level);
    if (compressors[level] == NULL)
    {
        /* no memory for a compressor: zlib */
        uLongf zl = (uLongf)*dst_len;
        if (compress2((Bytef *)dst, &zl, (const Bytef *)src, (uLong)len, level > 9 ? 9 : level) != Z_OK)
            return 0;
        *dst_len = (size_t)zl;
        return 1;
    }
    const size_t n = libdeflate_zlib_compress(compressors[level], src, len, dst, *dst_len);
    if (n == 0)
        return 0;
    *dst_len = n;
    return 1;
}

int kfx_zlib_uncompress(void *dst, size_t dst_room, const void *src, size_t len, size_t *out_len)
{
    if (decompressor == NULL)
        decompressor = libdeflate_alloc_decompressor();
    if (decompressor != NULL)
    {
        size_t made = 0;
        if (libdeflate_zlib_decompress(decompressor, src, len, dst, dst_room, &made) == LIBDEFLATE_SUCCESS)
        {
            *out_len = made;
            return 1;
        }
    }
    /* no decompressor, or a stream libdeflate turns down (a preset dictionary): zlib */
    uLongf rl = (uLongf)dst_room;
    if (uncompress((Bytef *)dst, &rl, (const Bytef *)src, (uLong)len) != Z_OK)
        return 0;
    *out_len = (size_t)rl;
    return 1;
}

/******************************************************************************/
/** @file sprite_recolour.h
 *     Endless Keeper: true-colour recolouring of sprites (see sprite_recolour.c).
 */
/******************************************************************************/
#ifndef SPRITE_RECOLOUR_H
#define SPRITE_RECOLOUR_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How a recoloured pixel's true colour is made from its own (shaded) colour: the hue is replaced, saturation and
 *  value are scaled and lifted (results kept in 0-1). */
struct SpriteRecolour {
    float hue;               /* degrees */
    float sat_mul, sat_add;
    float val_mul, val_add;
};

/** Registers a 256-byte remap table handed to the renderer (lbSpriteReMapPtr with Lb_SPRITE_REMAP) as a recolour:
 *  `src[i]` is pixel index i's colour before recolouring (identity, or a fade row for a shaded sprite), `mask[i]`
 *  non-zero where pixel index i is recoloured. The software renderer draws `remap` itself (the nearest palette
 *  colours); the OpenGL and Vulkan renderers draw the true colour. The table must not change after registering (a
 *  table with other contents is registered again). Game thread; at most SPRITE_RECOLOURS_MAX tables. */
void sprite_recolour_register(const unsigned char *remap, const unsigned char *src, const unsigned char *mask,
    const struct SpriteRecolour *rc);
#define SPRITE_RECOLOURS_MAX 64

/** For the renderers' colour rows (any thread): when a registered table has these 256 bytes, its recoloured entries
 *  of `out_rgba` (256 x RGBA8) are written in true colour from the palette in use and true is returned. `pal6`: 768
 *  bytes of 0-63 values; `pal_rgba`: 256 x RGBA8. */
TbBool sprite_recolour_row_pal6(const unsigned char *remap, const unsigned char *pal6, unsigned char *out_rgba);
TbBool sprite_recolour_row_rgba(const unsigned char *remap, const unsigned char *pal_rgba, unsigned char *out_rgba);

#ifdef __cplusplus
}
#endif
#endif

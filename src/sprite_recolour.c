/******************************************************************************/
/** @file sprite_recolour.c
 *     Endless Keeper: true-colour recolouring of sprites, with the palette as the fallback.
 * @par Purpose:
 *     The game draws in 256 colours, and its palette has no true blue and no mid neutral greys, so a palette remap can't
 *     give every colour (the user, 2026-11-20: "Cant you code in a flexible palette that we can use anywhere?";
 *     2026-11-21: "blue still looks like lavender"). Code that recolours a sprite hands the renderer a remap table as
 *     always (the software renderer draws that: the nearest palette colours) and registers it here with the colour it
 *     means. The OpenGL and Vulkan renderers turn each remap into a row of true colours on the graphics card anyway
 *     (their colour-row tables): for a registered table they write the true colour there, made from each pixel's own
 *     shaded colour in the palette in use (gamma included), so shading and the sprite's light and dark lines stay.
 *     Any sprite can use it: objects, creatures, effects.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "sprite_recolour.h"

#include <string.h>
#include <math.h>
#include <SDL3/SDL_atomic.h>
#include "kfx/renderer/RendererManager.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RecolourEntry {
    unsigned char remap[256];
    unsigned char src[256];
    unsigned char mask[256];
    struct SpriteRecolour rc;
};
/* append-only: an entry is written in full, then the count is raised (the render thread reads up to the count) */
static struct RecolourEntry entries[SPRITE_RECOLOURS_MAX];
static SDL_AtomicInt entry_count;

void sprite_recolour_register(const unsigned char *remap, const unsigned char *src, const unsigned char *mask,
    const struct SpriteRecolour *rc)
{
    const int n = SDL_GetAtomicInt(&entry_count);
    for (int i = 0; i < n; i++)
        if ((memcmp(entries[i].remap, remap, 256) == 0) && (memcmp(entries[i].src, src, 256) == 0) &&
            (memcmp(entries[i].mask, mask, 256) == 0) && (memcmp(&entries[i].rc, rc, sizeof(*rc)) == 0))
            return; /* (the same table again) */
    if (n >= SPRITE_RECOLOURS_MAX)
    {
        static TbBool told = false;
        if (!told)
            WARNLOG("Sprite recolour: %d tables registered: more draw in palette colours only", SPRITE_RECOLOURS_MAX);
        told = true;
        return;
    }
    memcpy(entries[n].remap, remap, 256);
    memcpy(entries[n].src, src, 256);
    memcpy(entries[n].mask, mask, 256);
    entries[n].rc = *rc;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&entry_count, n + 1);
}

static const struct RecolourEntry *find_entry(const unsigned char *remap)
{
    const int n = SDL_GetAtomicInt(&entry_count);
    SDL_MemoryBarrierAcquire();
    /* the newest first: a table registered again with other colours wins */
    for (int i = n - 1; i >= 0; i--)
        if (memcmp(entries[i].remap, remap, 256) == 0)
            return &entries[i];
    return NULL;
}

static float clamp01(float v)
{
    return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

/** One true colour from a source colour (0-1 RGB). */
static void recolour_rgb(const struct SpriteRecolour *rc, float r, float g, float b, unsigned char *out)
{
    const float cmax = fmaxf(r, fmaxf(g, b)), cmin = fminf(r, fminf(g, b));
    const float sat = (cmax > 0.0f) ? (cmax - cmin) / cmax : 0.0f;
    const float s = clamp01(sat * rc->sat_mul + rc->sat_add), v = clamp01(cmax * rc->val_mul + rc->val_add);
    float hp = fmodf(rc->hue, 360.0f);
    if (hp < 0.0f)
        hp += 360.0f;
    hp /= 60.0f;
    const float c = v * s, x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f)), m = v - c;
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (hp < 1.0f) { r1 = c; g1 = x; }
    else if (hp < 2.0f) { r1 = x; g1 = c; }
    else if (hp < 3.0f) { g1 = c; b1 = x; }
    else if (hp < 4.0f) { g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; b1 = c; }
    else { r1 = c; b1 = x; }
    out[0] = (unsigned char)lrintf(clamp01(r1 + m) * 255.0f);
    out[1] = (unsigned char)lrintf(clamp01(g1 + m) * 255.0f);
    out[2] = (unsigned char)lrintf(clamp01(b1 + m) * 255.0f);
}

TbBool sprite_recolour_row_pal6(const unsigned char *remap, const unsigned char *pal6, unsigned char *out_rgba)
{
    if ((remap == NULL) || (pal6 == NULL))
        return false;
    const struct RecolourEntry *e = find_entry(remap);
    if (e == NULL)
        return false;
    for (int i = 0; i < 256; i++)
    {
        if (!e->mask[i])
            continue;
        const int s = e->src[i];
        recolour_rgb(&e->rc, RendererPaletteChannel8(pal6[3 * s]) / 255.0f, RendererPaletteChannel8(pal6[3 * s + 1]) / 255.0f,
            RendererPaletteChannel8(pal6[3 * s + 2]) / 255.0f, &out_rgba[4 * i]);
    }
    return true;
}

TbBool sprite_recolour_row_rgba(const unsigned char *remap, const unsigned char *pal_rgba, unsigned char *out_rgba)
{
    if ((remap == NULL) || (pal_rgba == NULL))
        return false;
    const struct RecolourEntry *e = find_entry(remap);
    if (e == NULL)
        return false;
    for (int i = 0; i < 256; i++)
    {
        if (!e->mask[i])
            continue;
        const int s = e->src[i];
        recolour_rgb(&e->rc, pal_rgba[4 * s] / 255.0f, pal_rgba[4 * s + 1] / 255.0f, pal_rgba[4 * s + 2] / 255.0f,
            &out_rgba[4 * i]);
    }
    return true;
}

#ifdef __cplusplus
}
#endif

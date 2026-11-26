/******************************************************************************/
// Endless Keeper - true-colour painting for windows
/******************************************************************************/
/** @file ui_paint.c
 *     True-colour painting for windows on the CPU: boxes, shadows, glows, lines, pictures, TrueType text.
 * @par Purpose:
 *     The user, 2026-11-25, of the Keeper panel: "more modern to today's standards", then "Option B and do it in the
 *     style of the new Launcher design" (true colour on OpenGL/Vulkan, a cleaner palette version in software), and
 *     "Please do the same to the Library panel". See ui_paint.h. Shapes are drawn by their distance to each pixel's
 *     middle, so their edges are smooth at any size; text is stb_truetype's, its glyphs kept in a small cache.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "ui_paint.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_sprfnt.h"
#include "bflib_sprite.h"
#include "config.h"
#include "frontend.h"
#include "front_simple.h"
#include "globals.h"
#include "../deps/stb_truetype/stb_truetype.h"
#include "post_inc.h"

/******************************************************************************/
/* The canvas */

TbBool ui_canvas_size(struct UiCanvas *c, int w, int h)
{
    if (!ui_canvas_size_raw(c, w, h))
        return false;
    ui_canvas_clear(c);
    return true;
}

TbBool ui_canvas_size_raw(struct UiCanvas *c, int w, int h)
{
    if ((w <= 0) || (h <= 0))
        return false;
    if ((c->px == NULL) || ((size_t)c->w * (size_t)c->h < (size_t)w * (size_t)h))
    {
        unsigned char *px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
        if (px == NULL)
            return false;
        free(c->px);
        c->px = px;
    }
    c->w = w;
    c->h = h;
    ui_canvas_unclip(c);
    return true;
}

void ui_canvas_free(struct UiCanvas *c)
{
    free(c->px);
    memset(c, 0, sizeof(*c));
}

void ui_canvas_clear(struct UiCanvas *c)
{
    if (c->px != NULL)
        memset(c->px, 0, (size_t)c->w * (size_t)c->h * 4);
}

void ui_canvas_copy(struct UiCanvas *dst, const struct UiCanvas *src)
{
    if ((dst->px == NULL) || (src->px == NULL) || (dst->w != src->w) || (dst->h != src->h))
        return;
    memcpy(dst->px, src->px, (size_t)src->w * (size_t)src->h * 4);
}

void ui_canvas_clip(struct UiCanvas *c, int x0, int y0, int x1, int y1)
{
    c->clip_x0 = max(0, x0);
    c->clip_y0 = max(0, y0);
    c->clip_x1 = min(c->w, x1);
    c->clip_y1 = min(c->h, y1);
}

void ui_canvas_unclip(struct UiCanvas *c)
{
    c->clip_x0 = 0;
    c->clip_y0 = 0;
    c->clip_x1 = c->w;
    c->clip_y1 = c->h;
}

static inline float clamp01(float v)
{
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

/** Lays a straight-alpha colour over a pixel at `cov` coverage. */
static inline void blend(unsigned char *p, UiColour col, float cov)
{
    const float a = (float)(col >> 24) * cov;
    if (a <= 0.0f)
        return;
    const float k = a / 255.0f, ia = 1.0f - k;
    p[0] = (unsigned char)((float)((col >> 16) & 0xFF) * k + (float)p[0] * ia + 0.5f);
    p[1] = (unsigned char)((float)((col >> 8) & 0xFF) * k + (float)p[1] * ia + 0.5f);
    p[2] = (unsigned char)((float)(col & 0xFF) * k + (float)p[2] * ia + 0.5f);
    p[3] = (unsigned char)(a + (float)p[3] * ia + 0.5f);
}

/** A pixel covered by a straight-alpha colour at alpha `a` (0-255), integer blending; `light`: added light. */
static inline void blend_a(unsigned char *p, UiColour col, unsigned a, TbBool light)
{
    if (a == 0)
        return;
    const unsigned r = (col >> 16) & 0xFF, g = (col >> 8) & 0xFF, b = col & 0xFF;
    if (light)
    {
        const unsigned nr = p[0] + (r * a + 127) / 255, ng = p[1] + (g * a + 127) / 255, nb = p[2] + (b * a + 127) / 255;
        p[0] = (unsigned char)min(nr, 255u);
        p[1] = (unsigned char)min(ng, 255u);
        p[2] = (unsigned char)min(nb, 255u);
        return;
    }
    const unsigned ia = 255 - a;
    p[0] = (unsigned char)((r * a + p[0] * ia + 127) / 255);
    p[1] = (unsigned char)((g * a + p[1] * ia + 127) / 255);
    p[2] = (unsigned char)((b * a + p[2] * ia + 127) / 255);
    p[3] = (unsigned char)((a * 255 + p[3] * ia + 127) / 255);
}

/** Adds light (straight-alpha colour, `cov`): the colour channels grow, how see-through it is stays. */
static inline void add_light(unsigned char *p, UiColour col, float cov)
{
    const float k = (float)(col >> 24) * cov / 255.0f;
    if (k <= 0.0f)
        return;
    const int r = p[0] + (int)((float)((col >> 16) & 0xFF) * k + 0.5f);
    const int g = p[1] + (int)((float)((col >> 8) & 0xFF) * k + 0.5f);
    const int b = p[2] + (int)((float)(col & 0xFF) * k + 0.5f);
    p[0] = (unsigned char)min(r, 255);
    p[1] = (unsigned char)min(g, 255);
    p[2] = (unsigned char)min(b, 255);
}

static UiColour mix(UiColour a, UiColour b, float t)
{
    UiColour out = 0;
    for (int sh = 0; sh < 32; sh += 8)
    {
        const float va = (float)((a >> sh) & 0xFF), vb = (float)((b >> sh) & 0xFF);
        out |= ((UiColour)(va + (vb - va) * t + 0.5f) & 0xFF) << sh;
    }
    return out;
}

/** A rounded box's distance from a point (negative inside). */
struct RRect {
    float cx, cy, hw, hh, r;
};

static struct RRect rrect(float x0, float y0, float x1, float y1, float radius)
{
    struct RRect b;
    b.cx = (x0 + x1) * 0.5f;
    b.cy = (y0 + y1) * 0.5f;
    b.hw = fabsf(x1 - x0) * 0.5f;
    b.hh = fabsf(y1 - y0) * 0.5f;
    b.r = max(0.0f, min(radius, min(b.hw, b.hh)));
    return b;
}

static inline float rrect_sd(const struct RRect *b, float px, float py)
{
    const float qx = fabsf(px - b->cx) - (b->hw - b->r), qy = fabsf(py - b->cy) - (b->hh - b->r);
    const float mx = (qx > 0.0f) ? qx : 0.0f, my = (qy > 0.0f) ? qy : 0.0f;
    const float in = (qx > qy) ? qx : qy;
    return sqrtf(mx * mx + my * my) + ((in < 0.0f) ? in : 0.0f) - b->r;
}

/** True when the pixel's middle lies at least `inset` inside the box (its corners counted as round). */
static inline TbBool rrect_deep(const struct RRect *b, float px, float py, float inset)
{
    const float dx = fabsf(px - b->cx), dy = fabsf(py - b->cy);
    if ((dx <= b->hw - inset) && (dy <= b->hh - b->r - inset))
        return true;
    if ((dx <= b->hw - b->r - inset) && (dy <= b->hh - inset))
        return true;
    return false;
}

/** The pixels of row `py` lying at least `inset` inside the box (as rrect_deep): [*xa, *xb). False: none. */
static TbBool rrect_deep_span(const struct RRect *b, float py, float inset, int *xa, int *xb)
{
    const float dy = fabsf(py - b->cy);
    float half;
    if (dy <= b->hh - b->r - inset)
        half = b->hw - inset;
    else if (dy <= b->hh - inset)
        half = b->hw - b->r - inset;
    else
        return false;
    if (half <= 0.0f)
        return false;
    /* pixel middles px + 0.5 within [cx - half, cx + half] */
    *xa = (int)ceilf(b->cx - half - 0.5f);
    *xb = (int)floorf(b->cx + half - 0.5f) + 1;
    return (*xb > *xa);
}

/** A run of pixels covered fully by one straight-alpha colour (integer blending). */
static void blend_run(unsigned char *p, int n, UiColour col)
{
    const unsigned a = col >> 24;
    if (a == 0)
        return;
    const unsigned r = ((col >> 16) & 0xFF) * a, g = ((col >> 8) & 0xFF) * a, bl = (col & 0xFF) * a, ia = 255 - a;
    if (a == 255)
    {
        for (int i = 0; i < n; i++, p += 4)
        {
            p[0] = (unsigned char)((col >> 16) & 0xFF);
            p[1] = (unsigned char)((col >> 8) & 0xFF);
            p[2] = (unsigned char)(col & 0xFF);
            p[3] = 255;
        }
        return;
    }
    for (int i = 0; i < n; i++, p += 4)
    {
        p[0] = (unsigned char)((r + p[0] * ia + 127) / 255);
        p[1] = (unsigned char)((g + p[1] * ia + 127) / 255);
        p[2] = (unsigned char)((bl + p[2] * ia + 127) / 255);
        p[3] = (unsigned char)((a * 255 + p[3] * ia + 127) / 255);
    }
}

static void bounds(const struct UiCanvas *c, float x0, float y0, float x1, float y1, int *ix0, int *iy0, int *ix1, int *iy1)
{
    *ix0 = max(c->clip_x0, (int)floorf(x0));
    *iy0 = max(c->clip_y0, (int)floorf(y0));
    *ix1 = min(c->clip_x1, (int)ceilf(x1));
    *iy1 = min(c->clip_y1, (int)ceilf(y1));
}

void ui_fill_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, UiColour c0, UiColour c1,
    TbBool horizontal)
{
    if ((c->px == NULL) || (x1 <= x0) || (y1 <= y0))
        return;
    const struct RRect b = rrect(x0, y0, x1, y1, radius);
    int ix0, iy0, ix1, iy1;
    bounds(c, x0, y0, x1, y1, &ix0, &iy0, &ix1, &iy1);
    for (int y = iy0; y < iy1; y++)
    {
        const float py = (float)y + 0.5f;
        const UiColour row = horizontal ? c0 : mix(c0, c1, clamp01((py - y0) / (y1 - y0)));
        unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)ix0) * 4;
        int xa = 0, xb = 0;
        const TbBool span = !horizontal && rrect_deep_span(&b, py, 1.0f, &xa, &xb);
        if (span)
        {
            xa = max(xa, ix0);
            xb = min(xb, ix1);
        }
        for (int x = ix0; x < ix1; x++, p += 4)
        {
            if (span && (x == xa) && (xb > xa))
            {
                blend_run(p, xb - xa, row);
                p += (size_t)(xb - xa - 1) * 4;
                x = xb - 1;
                continue;
            }
            const float px = (float)x + 0.5f;
            const UiColour col = horizontal ? mix(c0, c1, clamp01((px - x0) / (x1 - x0))) : row;
            const float cov = rrect_deep(&b, px, py, 1.0f) ? 1.0f : clamp01(0.5f - rrect_sd(&b, px, py));
            if (cov > 0.0f)
                blend(p, col, cov);
        }
    }
}

void ui_stroke_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float width, UiColour col)
{
    if ((c->px == NULL) || (x1 <= x0) || (y1 <= y0) || (width <= 0.0f))
        return;
    const struct RRect b = rrect(x0, y0, x1, y1, radius);
    int ix0, iy0, ix1, iy1;
    bounds(c, x0, y0, x1, y1, &ix0, &iy0, &ix1, &iy1);
    for (int y = iy0; y < iy1; y++)
    {
        const float py = (float)y + 0.5f;
        unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)ix0) * 4;
        int xa = 0, xb = 0;
        const TbBool span = rrect_deep_span(&b, py, width + 1.5f, &xa, &xb);
        for (int x = ix0; x < ix1; x++, p += 4)
        {
            if (span && (x >= xa) && (x < xb))
            {
                p += (size_t)(xb - 1 - x) * 4;
                x = xb - 1;
                continue;
            }
            const float px = (float)x + 0.5f;
            const float d = rrect_sd(&b, px, py);
            const float cov = clamp01(0.5f - d) - clamp01(0.5f - (d + width));
            if (cov > 0.0f)
                blend(p, col, cov);
        }
    }
}

/* the shadow's and the halo's fall from the box's edge: 1 at it, 0 `spread` away */
static inline float falloff(float d, float spread)
{
    if (d <= 0.0f)
        return 1.0f;
    if (d >= spread)
        return 0.0f;
    const float t = 1.0f - d / spread;
    return t * t * t;
}

static void soft_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float spread, float dy,
    UiColour col, TbBool light)
{
    if ((c->px == NULL) || (x1 <= x0) || (y1 <= y0) || (spread <= 0.0f))
        return;
    const struct RRect box = rrect(x0, y0, x1, y1, radius);
    const struct RRect cast = rrect(x0, y0 + dy, x1, y1 + dy, radius);
    int ix0, iy0, ix1, iy1;
    bounds(c, x0 - spread, y0 - spread + min(dy, 0.0f), x1 + spread, y1 + spread + max(dy, 0.0f), &ix0, &iy0, &ix1, &iy1);
    if ((ix1 <= ix0) || (iy1 <= iy0))
        return;
    const float ca = (float)(col >> 24);
    /* the side bands' strength by column: the box and its cast share their x */
    unsigned char *colf = (unsigned char *)malloc((size_t)(ix1 - ix0));
    if (colf == NULL)
        return;
    for (int x = ix0; x < ix1; x++)
    {
        const float d = fabsf((float)x + 0.5f - box.cx) - box.hw;
        colf[x - ix0] = (unsigned char)(ca * falloff(d, spread) * clamp01(d + 0.5f) + 0.5f);
    }
    /* the straight zone across the top and bottom: columns whose middles lie within hw - r of the middle */
    const float sh = box.hw - box.r;
    const int sx0 = max(ix0, (int)ceilf(box.cx - sh - 0.5f)), sx1 = min(ix1, (int)floorf(box.cx + sh - 0.5f) + 1);
    for (int y = iy0; y < iy1; y++)
    {
        const float py = (float)y + 0.5f;
        const float qyc = fabsf(py - cast.cy) - (cast.hh - cast.r), qyb = fabsf(py - box.cy) - (box.hh - box.r);
        unsigned char *row = c->px + (size_t)y * (size_t)c->w * 4;
        int xa = 0, xb = 0;
        const TbBool deep = rrect_deep_span(&box, py, 1.0f, &xa, &xb);
        int done0 = ix0, done1 = ix0; /* columns handled by a fast way in this row */
        if ((qyc <= 0.0f) && (qyb <= 0.0f))
        {
            /* a side-band row of both: strength by column (inside the box nothing) */
            for (int x = ix0; x < ix1; x++)
            {
                if (deep && (x >= xa) && (x < xb))
                {
                    x = xb - 1;
                    continue;
                }
                blend_a(row + (size_t)x * 4, col, colf[x - ix0], light);
            }
            continue;
        }
        if ((qyc > 0.0f) && (qyb > 0.0f) && (sx1 > sx0))
        {
            /* above or below the box (and its cast): one strength for the straight zone's run */
            const float dc = fabsf(py - cast.cy) - cast.hh, db = fabsf(py - box.cy) - box.hh;
            const unsigned a = (unsigned)(ca * falloff(dc, spread) * clamp01(db + 0.5f) + 0.5f);
            if (a > 0)
                for (int x = sx0; x < sx1; x++)
                    blend_a(row + (size_t)x * 4, col, a, light);
            done0 = sx0;
            done1 = sx1;
        }
        /* the rest of the row: the exact way (corners; rows the box and its cast see differently) */
        for (int x = ix0; x < ix1; x++)
        {
            if ((x >= done0) && (x < done1))
            {
                x = done1 - 1;
                continue;
            }
            if (deep && (x >= xa) && (x < xb))
            {
                x = xb - 1;
                continue;
            }
            const float px = (float)x + 0.5f;
            const float d = rrect_sd(&cast, px, py);
            float outside = 1.0f;
            if ((fabsf(px - box.cx) < box.hw + 1.0f) && (fabsf(py - box.cy) < box.hh + 1.0f))
                outside = clamp01(((dy == 0.0f) ? d : rrect_sd(&box, px, py)) + 0.5f);
            blend_a(row + (size_t)x * 4, col, (unsigned)(ca * falloff(d, spread) * outside + 0.5f), light);
        }
    }
    free(colf);
}

void ui_shadow_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float spread, float dy,
    UiColour col)
{
    soft_rrect(c, x0, y0, x1, y1, radius, spread, dy, col, false);
}

void ui_glow_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float spread, UiColour col)
{
    soft_rrect(c, x0, y0, x1, y1, radius, spread, 0.0f, col, true);
}

void ui_line_fade(struct UiCanvas *c, float x0, float y, float x1, float thickness, UiColour col)
{
    if ((c->px == NULL) || (x1 <= x0) || (thickness <= 0.0f))
        return;
    const float fade = (x1 - x0) * 0.12f;
    int ix0, iy0, ix1, iy1;
    bounds(c, x0, y, x1, y + thickness, &ix0, &iy0, &ix1, &iy1);
    for (int py = iy0; py < iy1; py++)
    {
        const float vcov = clamp01(min((float)py + 1.0f, y + thickness) - max((float)py, y));
        unsigned char *p = c->px + ((size_t)py * (size_t)c->w + (size_t)ix0) * 4;
        for (int px = ix0; px < ix1; px++, p += 4)
        {
            const float m = (float)px + 0.5f;
            const float hcov = clamp01(min(m - x0, x1 - m) / max(fade, 1.0f));
            blend(p, col, vcov * hcov);
        }
    }
}

void ui_image(struct UiCanvas *c, int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh, float radius)
{
    if ((c->px == NULL) || (rgba == NULL) || (w <= 0) || (h <= 0) || (sw <= 0) || (sh <= 0))
        return;
    const struct RRect b = rrect((float)x, (float)y, (float)(x + w), (float)(y + h), radius);
    const int ix0 = max(c->clip_x0, x), iy0 = max(c->clip_y0, y);
    const int ix1 = min(c->clip_x1, x + w), iy1 = min(c->clip_y1, y + h);
    for (int py = iy0; py < iy1; py++)
    {
        const int sy = min(sh - 1, (int)(((int64_t)(py - y) * sh) / h));
        unsigned char *p = c->px + ((size_t)py * (size_t)c->w + (size_t)ix0) * 4;
        for (int px = ix0; px < ix1; px++, p += 4)
        {
            const int sx = min(sw - 1, (int)(((int64_t)(px - x) * sw) / w));
            const unsigned char *s = rgba + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            if (s[3] == 0)
                continue;
            const float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            const float cov = ((radius <= 0.0f) || rrect_deep(&b, fx, fy, 1.0f)) ? 1.0f : clamp01(0.5f - rrect_sd(&b, fx, fy));
            if (cov > 0.0f)
                blend(p, UIC(s[0], s[1], s[2], s[3]), cov);
        }
    }
}

void ui_image_gain(struct UiCanvas *c, int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh, float gain)
{
    if ((c->px == NULL) || (rgba == NULL) || (w <= 0) || (h <= 0) || (sw <= 0) || (sh <= 0))
        return;
    const int ix0 = max(c->clip_x0, x), iy0 = max(c->clip_y0, y);
    const int ix1 = min(c->clip_x1, x + w), iy1 = min(c->clip_y1, y + h);
    for (int py = iy0; py < iy1; py++)
    {
        const int sy = min(sh - 1, (int)(((int64_t)(py - y) * sh) / h));
        unsigned char *p = c->px + ((size_t)py * (size_t)c->w + (size_t)ix0) * 4;
        for (int px = ix0; px < ix1; px++, p += 4)
        {
            const int sx = min(sw - 1, (int)(((int64_t)(px - x) * sw) / w));
            const unsigned char *s = rgba + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            if (s[3] == 0)
                continue;
            const int r = min(255, (int)((float)s[0] * gain + 0.5f)), g = min(255, (int)((float)s[1] * gain + 0.5f));
            const int b = min(255, (int)((float)s[2] * gain + 0.5f));
            blend(p, UIC(r, g, b, s[3]), 1.0f);
        }
    }
}

void ui_fill_path(struct UiCanvas *c, const float *xy, const int *counts, int n, UiColour col)
{
    if ((c->px == NULL) || (xy == NULL) || (counts == NULL) || (n <= 0))
        return;
    int total = 0;
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (int i = 0; i < n; i++)
        total += counts[i];
    for (int i = 0; i < total; i++)
    {
        bx0 = min(bx0, xy[i * 2]);
        bx1 = max(bx1, xy[i * 2]);
        by0 = min(by0, xy[i * 2 + 1]);
        by1 = max(by1, xy[i * 2 + 1]);
    }
    const int x0 = max(c->clip_x0, (int)floorf(bx0)), x1 = min(c->clip_x1, (int)ceilf(bx1) + 1);
    const int y0 = max(c->clip_y0, (int)floorf(by0)), y1 = min(c->clip_y1, (int)ceilf(by1) + 1);
    if ((x1 <= x0) || (y1 <= y0) || (total > 512))
        return;
    /* four rows of samples a pixel; on each, the spans between crossings (even-odd) add their exact width */
    float *acc = (float *)calloc((size_t)(x1 - x0 + 2), sizeof(float));
    float xs[512];
    if (acc == NULL)
        return;
    const unsigned ca = col >> 24;
    for (int y = y0; y < y1; y++)
    {
        memset(acc, 0, sizeof(float) * (size_t)(x1 - x0 + 2));
        for (int s = 0; s < 4; s++)
        {
            const float sy = (float)y + ((float)s + 0.5f) * 0.25f;
            int nx = 0, first = 0;
            for (int k = 0; k < n; k++)
            {
                for (int i = 0; i < counts[k]; i++)
                {
                    const float *a = xy + (size_t)(first + i) * 2;
                    const float *b = xy + (size_t)(first + (i + 1) % counts[k]) * 2;
                    if (((a[1] <= sy) && (b[1] > sy)) || ((b[1] <= sy) && (a[1] > sy)))
                        if (nx < 512)
                            xs[nx++] = a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
                }
                first += counts[k];
            }
            for (int i = 1; i < nx; i++) /* (a few crossings: insertion sort) */
            {
                const float v = xs[i];
                int j = i - 1;
                while ((j >= 0) && (xs[j] > v))
                {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = v;
            }
            for (int i = 0; i + 1 < nx; i += 2)
            {
                const float sa = max(xs[i], (float)x0), sb = min(xs[i + 1], (float)x1);
                if (sb <= sa)
                    continue;
                const int ia = (int)floorf(sa), ib = (int)floorf(sb);
                if (ia == ib)
                {
                    acc[ia - x0] += sb - sa;
                    continue;
                }
                acc[ia - x0] += (float)(ia + 1) - sa;
                for (int x = ia + 1; x < ib; x++)
                    acc[x - x0] += 1.0f;
                if (ib < x1)
                    acc[ib - x0] += sb - (float)ib;
            }
        }
        unsigned char *row = c->px + ((size_t)y * (size_t)c->w) * 4;
        for (int x = x0; x < x1; x++)
        {
            const float cov = acc[x - x0] * 0.25f;
            if (cov <= 0.002f)
                continue;
            blend_a(row + (size_t)x * 4, col, (unsigned)(min(cov, 1.0f) * (float)ca + 0.5f), false);
        }
    }
    free(acc);
}

void ui_hollow_circle(struct UiCanvas *c, float cx, float cy, float r, float band)
{
    if ((c->px == NULL) || (r <= 1.0f))
        return;
    band = max(band, 1.0f);
    const int x0 = max(c->clip_x0, (int)floorf(cx - r - 1.0f)), x1 = min(c->clip_x1, (int)ceilf(cx + r + 2.0f));
    const int y0 = max(c->clip_y0, (int)floorf(cy - r - 1.0f)), y1 = min(c->clip_y1, (int)ceilf(cy + r + 2.0f));
    const float lip = max(1.0f, band * 0.18f);
    for (int y = y0; y < y1; y++)
    {
        unsigned char *row = c->px + ((size_t)y * (size_t)c->w) * 4;
        const float dy = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++)
        {
            const float dx = (float)x + 0.5f - cx;
            const float d = sqrtf(dx * dx + dy * dy);
            const float in = r - d; /* how far inside the rim */
            if (in <= -1.0f)
                continue;
            const float edge = (in >= 0.0f) ? 1.0f : 1.0f + in; /* (the rim smooth) */
            /* the ground: darker inside; the shade: deepest at the rim's top, fading in `band` and towards its foot */
            const float up = (d > 0.0f) ? -dy / d : 0.0f; /* 1 at the top, -1 at the foot */
            const float depth = max(0.0f, 1.0f - max(in, 0.0f) / band);
            const float shade = depth * depth * (0.55f + 0.45f * up);
            const float a = (0.30f + 0.62f * shade) * edge;
            unsigned char *p = row + (size_t)x * 4;
            blend_a(p, UIC(0, 0, 0, 255), (unsigned)(min(a, 1.0f) * 255.0f + 0.5f), false);
            /* the light lip: the rim's lower half, a thin line just inside it */
            if ((up < -0.15f) && (in > -1.0f) && (in < lip + 1.0f))
            {
                const float l = (in < 0.0f) ? 1.0f + in : ((in > lip) ? 1.0f - (in - lip) : 1.0f);
                const float k = l * min(1.0f, (-up - 0.15f) * 2.0f);
                if (k > 0.0f)
                    blend_a(p, UIC(255, 214, 170, 255), (unsigned)(k * 46.0f + 0.5f), false);
            }
        }
    }
}

void ui_sprite_rgba(const struct TbSprite *spr, const unsigned char *pal6, unsigned char *out)
{
    if ((spr == NULL) || (out == NULL) || (spr->Data == NULL))
        return;
    const int w = spr->SWidth, h = spr->SHeight;
    unsigned char *idx = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (idx == NULL)
        return;
    LbSpriteDecode(idx, w, spr->Data, w, h); /* (0: see-through; a real index 0 comes as 1) */
    for (int i = 0; i < w * h; i++)
    {
        unsigned char *o = out + (size_t)i * 4;
        const unsigned char k = idx[i];
        if ((k == 0) || (pal6 == NULL))
        {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        o[0] = (unsigned char)((pal6[k * 3 + 0] << 2) | (pal6[k * 3 + 0] >> 4));
        o[1] = (unsigned char)((pal6[k * 3 + 1] << 2) | (pal6[k * 3 + 1] >> 4));
        o[2] = (unsigned char)((pal6[k * 3 + 2] << 2) | (pal6[k * 3 + 2] >> 4));
        o[3] = 255;
    }
    free(idx);
}

/******************************************************************************/
/* Fonts */

struct UiFace {
    stbtt_fontinfo info;
    unsigned char *data;
    int state;          /* 0: not tried, 1: loaded, -1: none */
    float cap_units;    /* the capitals' height in font units */
    float bold;         /* letters made this much of their size wider (a face with no bold of its own) */
};

static struct UiFace ui_faces[UiF_Count];

static unsigned char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    unsigned char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        const long len = ftell(f);
        if ((len > 1024) && (len < 64L * 1024 * 1024) && (fseek(f, 0, SEEK_SET) == 0))
        {
            data = (unsigned char *)malloc((size_t)len);
            if ((data != NULL) && (fread(data, 1, (size_t)len, f) != (size_t)len))
            {
                free(data);
                data = NULL;
            }
        }
    }
    fclose(f);
    return data;
}

static TbBool face_try(struct UiFace *face, const char *path)
{
    unsigned char *data = read_file(path);
    if (data == NULL)
        return false;
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if ((offset < 0) || !stbtt_InitFont(&face->info, data, offset))
    {
        free(data);
        return false;
    }
    face->data = data;
    face->bold = 0.0f;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (stbtt_GetCodepointBox(&face->info, 'H', &x0, &y0, &x1, &y1) && (y1 > 0))
        face->cap_units = (float)y1;
    else
    {
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&face->info, &asc, &desc, &gap);
        face->cap_units = (float)asc * 0.7f;
    }
    SYNCLOG("UI paint: font %s", path);
    return true;
}

static struct UiFace *face_get(int font)
{
    if ((font < 0) || (font >= UiF_Count))
        font = UiF_Body;
    struct UiFace *face = &ui_faces[font];
    if (face->state != 0)
        return (face->state > 0) ? face : NULL;
    face->state = -1;
    static const char *const own[UiF_Count] = {"ui_head.ttf", "ui_body.ttf", "ui_num.ttf"};
    if (face_try(face, prepare_file_path(FGrp_FxData, own[font])))
    {
        face->state = 1;
        return face;
    }
#ifdef _WIN32
    static const char *const head[] = {"palab.ttf", "georgiab.ttf", "segoeuib.ttf", "arialbd.ttf"};
    static const char *const body[] = {"segoeui.ttf", "arial.ttf", "tahoma.ttf"};
    /* (Bahnschrift from Windows 10 on: its regular weight, made bolder here; Windows 7 has Franklin Gothic) */
    static const char *const num[] = {"bahnschrift.ttf", "framd.ttf", "segoeuib.ttf", "arialbd.ttf"};
    const char *windir = getenv("WINDIR");
    if ((windir == NULL) || (windir[0] == '\0'))
        windir = "C:\\Windows";
    const char *const *names = (font == UiF_Head) ? head : ((font == UiF_Num) ? num : body);
    const size_t n = (font == UiF_Head) ? sizeof(head) / sizeof(head[0]) :
        ((font == UiF_Num) ? sizeof(num) / sizeof(num[0]) : sizeof(body) / sizeof(body[0]));
    for (size_t i = 0; i < n; i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, names[i]);
        if (face_try(face, path))
        {
            if ((font == UiF_Num) && (i == 0))
                face->bold = 0.03f;
            face->state = 1;
            return face;
        }
    }
#else
    static const char *const head[] = {"/system/fonts/NotoSerif-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSerif-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSerif-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSerif-Bold.ttf",
        "/usr/share/fonts/noto/NotoSerif-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"};
    static const char *const body[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
    static const char *const num[] = {"/system/fonts/RobotoCondensed-Bold.ttf", "/system/fonts/Roboto-Bold.ttf",
        "/system/fonts/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/system/fonts/Roboto-Regular.ttf"};
    const char *const *names = (font == UiF_Head) ? head : ((font == UiF_Num) ? num : body);
    const size_t n = (font == UiF_Head) ? sizeof(head) / sizeof(head[0]) :
        ((font == UiF_Num) ? sizeof(num) / sizeof(num[0]) : sizeof(body) / sizeof(body[0]));
    for (size_t i = 0; i < n; i++)
    {
        if (face_try(face, names[i]))
        {
            face->state = 1;
            return face;
        }
    }
#endif
    /* one face stands in for the other (numbers: the plain face) */
    const int other = (font == UiF_Head) ? UiF_Body : UiF_Head;
    if (font == UiF_Num)
    {
        struct UiFace *o = face_get(UiF_Body);
        if (o != NULL)
        {
            *face = *o;
            face->state = 1;
            return face;
        }
        return NULL;
    }
    if ((ui_faces[other].state == 0) || (ui_faces[other].state > 0))
    {
        struct UiFace *o = face_get(other);
        if (o != NULL)
        {
            WARNLOG("UI paint: no font for %s: the %s face stands in", (font == UiF_Head) ? "headings" : "text",
                (font == UiF_Head) ? "plain" : "book");
            *face = *o;
            face->state = 1;
            return face;
        }
    }
    WARNLOG("UI paint: no TrueType font found: the game's small font is used");
    return NULL;
}

TbBool ui_font_ready(int font)
{
    return (face_get(font) != NULL);
}

/** The next character of UTF-8 text (bytes that are not UTF-8 are taken as Latin-1). */
static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = p[0];
    int n = 0;
    if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; n = 1; }
    else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; n = 2; }
    else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; n = 3; }
    for (int i = 1; i <= n; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
        {
            *s += 1;
            return p[0]; /* not UTF-8 */
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += 1 + n;
    return cp;
}

/* the glyph cache: a glyph at a size (quarter pixels) and a quarter-pixel shift */
#define UI_GLYPHS 2048
struct UiGlyph {
    uint32_t cp;
    uint16_t px_q;
    uint8_t font, shift, used;
    short w, h, xoff, yoff;
    unsigned char *bmp;
};
static struct UiGlyph ui_glyphs[UI_GLYPHS];
static int ui_glyphs_used;

static void glyphs_clear(void)
{
    for (int i = 0; i < UI_GLYPHS; i++)
        free(ui_glyphs[i].bmp);
    memset(ui_glyphs, 0, sizeof(ui_glyphs));
    ui_glyphs_used = 0;
}

static const struct UiGlyph *glyph_get(struct UiFace *face, int font, uint32_t cp, int px_q, int shift)
{
    uint32_t hsh = cp * 2654435761u ^ (uint32_t)px_q * 40503u ^ (uint32_t)font * 977u ^ (uint32_t)shift * 131u;
    for (int probe = 0; probe < 16; probe++)
    {
        struct UiGlyph *g = &ui_glyphs[(hsh + (uint32_t)probe) & (UI_GLYPHS - 1)];
        if (g->used && (g->cp == cp) && (g->px_q == px_q) && (g->font == font) && (g->shift == shift))
            return g;
        if (!g->used)
        {
            if (ui_glyphs_used > UI_GLYPHS * 3 / 4)
            {
                glyphs_clear();
                return glyph_get(face, font, cp, px_q, shift);
            }
            const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, (float)px_q / 4.0f);
            const float sx = (float)shift / 4.0f;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBoxSubpixel(&face->info, (int)cp, scale, scale, sx, 0.0f, &x0, &y0, &x1, &y1);
            g->used = 1;
            g->cp = cp;
            g->px_q = (uint16_t)px_q;
            g->font = (uint8_t)font;
            g->shift = (uint8_t)shift;
            const float boldpx = face->bold * (float)px_q / 4.0f;
            const int extra = (boldpx > 0.0f) ? (int)ceilf(boldpx) : 0;
            g->w = (short)max(0, x1 - x0 + ((x1 > x0) ? extra : 0));
            g->h = (short)max(0, y1 - y0);
            g->xoff = (short)x0;
            g->yoff = (short)y0;
            g->bmp = NULL;
            ui_glyphs_used++;
            if ((g->w > 0) && (g->h > 0))
            {
                g->bmp = (unsigned char *)calloc((size_t)g->w * (size_t)g->h, 1);
                if (g->bmp != NULL)
                {
                    stbtt_MakeCodepointBitmapSubpixel(&face->info, g->bmp, g->w - extra, g->h, g->w, scale, scale, sx, 0.0f, (int)cp);
                    if (extra > 0)
                    {
                        /* each pixel the most of those up to boldpx to its left (the last one in part) */
                        const float part = boldpx - (float)(extra - 1);
                        for (int gy = 0; gy < g->h; gy++)
                        {
                            unsigned char *row = g->bmp + (size_t)gy * (size_t)g->w;
                            for (int gx = g->w - 1; gx >= 0; gx--)
                            {
                                int v = row[gx];
                                for (int k = 1; k < extra; k++)
                                    if (gx - k >= 0)
                                        v = max(v, (int)row[gx - k]);
                                if (gx - extra >= 0)
                                    v = max(v, (int)((float)row[gx - extra] * part));
                                row[gx] = (unsigned char)v;
                            }
                        }
                    }
                }
            }
            return g;
        }
    }
    glyphs_clear();
    return NULL;
}

/* letters a little fuller than the plain coverage: light text on dark glass looks thin without it */
static unsigned char ui_cov_lut[256];

static void cov_lut_make(void)
{
    if (ui_cov_lut[255] != 0)
        return;
    for (int i = 0; i < 256; i++)
        ui_cov_lut[i] = (unsigned char)(powf((float)i / 255.0f, 0.8f) * 255.0f + 0.5f);
}

/* the game's small font, scaled, when no TrueType face is there */
static int bitmap_scale(float px)
{
    return max(1, (int)(px / 9.0f + 0.5f));
}

static float bitmap_width(float px, float spacing, const char *text)
{
    const int s = bitmap_scale(px);
    float w = 0.0f;
    while (*text)
    {
        const uint32_t cp = next_cp(&text);
        const struct TbSprite *spr = (font_sprites != NULL) ? LbFontCharSprite(font_sprites, cp) : NULL;
        w += (float)(((spr != NULL) ? spr->SWidth : 4) * s) + spacing;
    }
    return w;
}

static void bitmap_text(struct UiCanvas *c, float px, float x, float baseline, UiColour col, float spacing, const char *text)
{
    const int s = bitmap_scale(px);
    unsigned char buf[64 * 64];
    float pen = x;
    while (*text)
    {
        const uint32_t cp = next_cp(&text);
        const struct TbSprite *spr = (font_sprites != NULL) ? LbFontCharSprite(font_sprites, cp) : NULL;
        if ((spr == NULL) || (spr->SWidth > 64) || (spr->SHeight > 64) || (spr->Data == NULL))
        {
            pen += (float)(4 * s) + spacing;
            continue;
        }
        LbSpriteDecode(buf, 64, spr->Data, spr->SWidth, spr->SHeight);
        const int top = (int)(baseline - (float)(spr->SHeight * s * 8 / 11));
        for (int gy = 0; gy < spr->SHeight * s; gy++)
        {
            const int y = top + gy;
            if ((y < c->clip_y0) || (y >= c->clip_y1))
                continue;
            for (int gx = 0; gx < spr->SWidth * s; gx++)
            {
                const int xx = (int)pen + gx;
                if ((xx < c->clip_x0) || (xx >= c->clip_x1) || (buf[(gy / s) * 64 + gx / s] == 0))
                    continue;
                blend(c->px + ((size_t)y * (size_t)c->w + (size_t)xx) * 4, col, 1.0f);
            }
        }
        pen += (float)(spr->SWidth * s) + spacing;
    }
}

float ui_text_width(int font, float px, float spacing, const char *text)
{
    struct UiFace *face = face_get(font);
    if (text == NULL)
        return 0.0f;
    if (face == NULL)
        return bitmap_width(px, spacing, text);
    const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, floorf(px * 4.0f) / 4.0f);
    float w = 0.0f;
    uint32_t prev = 0;
    int n = 0;
    while (*text)
    {
        const uint32_t cp = next_cp(&text);
        if (prev != 0)
            w += (float)stbtt_GetCodepointKernAdvance(&face->info, (int)prev, (int)cp) * scale;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&face->info, (int)cp, &adv, &lsb);
        w += (float)adv * scale + spacing + face->bold * px;
        prev = cp;
        n++;
    }
    return (n > 0) ? w - spacing : 0.0f;
}

float ui_text_cap(int font, float px)
{
    struct UiFace *face = face_get(font);
    if (face == NULL)
        return (float)(bitmap_scale(px) * 8);
    return face->cap_units * stbtt_ScaleForMappingEmToPixels(&face->info, px);
}

float ui_text_fit(int font, float px, float spacing, float max_w, const char *text)
{
    const float w = ui_text_width(font, px, spacing, text);
    if ((w <= max_w) || (w <= 0.0f))
        return px;
    return max(1.0f, px * max_w / w);
}

static void text_pass(struct UiCanvas *c, struct UiFace *face, int font, float px, float x, float baseline, UiColour col,
    float spacing, const char *text)
{
    const int px_q = max(4, (int)floorf(px * 4.0f));
    const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, (float)px_q / 4.0f);
    const unsigned ca8 = col >> 24;
    const int by = (int)floorf(baseline + 0.5f);
    float pen = x;
    uint32_t prev = 0;
    while (*text)
    {
        uint32_t cp = next_cp(&text);
        if (stbtt_FindGlyphIndex(&face->info, (int)cp) == 0)
            cp = '?';
        if (prev != 0)
            pen += (float)stbtt_GetCodepointKernAdvance(&face->info, (int)prev, (int)cp) * scale;
        const int ix = (int)floorf(pen);
        const int shift = (int)((pen - (float)ix) * 4.0f) & 3;
        const struct UiGlyph *g = glyph_get(face, font, cp, px_q, shift);
        if ((g != NULL) && (g->bmp != NULL))
        {
            for (int gy = 0; gy < g->h; gy++)
            {
                const int y = by + g->yoff + gy;
                if ((y < c->clip_y0) || (y >= c->clip_y1))
                    continue;
                const unsigned char *src = g->bmp + (size_t)gy * (size_t)g->w;
                for (int gx = 0; gx < g->w; gx++)
                {
                    const int xx = ix + g->xoff + gx;
                    if ((src[gx] == 0) || (xx < c->clip_x0) || (xx >= c->clip_x1))
                        continue;
                    blend_a(c->px + ((size_t)y * (size_t)c->w + (size_t)xx) * 4, col, (ui_cov_lut[src[gx]] * ca8 + 127) / 255, false);
                }
            }
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&face->info, (int)cp, &adv, &lsb);
        pen += (float)adv * scale + spacing + face->bold * px;
        prev = cp;
    }
}

void ui_text(struct UiCanvas *c, int font, float px, float x, float baseline, UiColour col, float spacing, TbBool shadow,
    const char *text)
{
    if ((c->px == NULL) || (text == NULL) || (text[0] == '\0') || (px < 1.0f))
        return;
    cov_lut_make();
    struct UiFace *face = face_get(font);
    const float dy = max(1.0f, px * 0.07f);
    const UiColour shade = UIC(0, 0, 0, (unsigned)(170.0f * (float)(col >> 24) / 255.0f));
    if (face == NULL)
    {
        if (shadow)
            bitmap_text(c, px, x, baseline + dy, shade, spacing, text);
        bitmap_text(c, px, x, baseline, col, spacing, text);
        return;
    }
    if (shadow)
        text_pass(c, face, font, px, x, baseline + dy, shade, spacing, text);
    text_pass(c, face, font, px, x, baseline, col, spacing, text);
}

void ui_text_box(struct UiCanvas *c, int font, float px, float x, float y, float w, float h, int align, UiColour col,
    float spacing, TbBool shadow, const char *text)
{
    if (text == NULL)
        return;
    const float tw = ui_text_width(font, px, spacing, text);
    const float cap = ui_text_cap(font, px);
    float tx = x;
    if (align == 1)
        tx = x + (w - tw) * 0.5f;
    else if (align == 2)
        tx = x + w - tw;
    ui_text(c, font, px, floorf(tx + 0.5f), floorf(y + (h + cap) * 0.5f + 0.5f), col, spacing, shadow, text);
}

/******************************************************************************/
/* The software renderer's version */

/* the nearest palette colour (never index 0: 0 is the see-through of `idx`), found once per 15-bit colour */
static unsigned char ui_nearest[32768];
static unsigned char ui_nearest_known[32768 / 8];
static const unsigned char *ui_nearest_pal;

static unsigned char nearest_index(const unsigned char *pal6, int r, int g, int b)
{
    if (pal6 != ui_nearest_pal)
    {
        memset(ui_nearest_known, 0, sizeof(ui_nearest_known));
        ui_nearest_pal = pal6;
    }
    const int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    if (ui_nearest_known[key >> 3] & (1 << (key & 7)))
        return ui_nearest[key];
    const int r6 = ((r >> 3) << 3 | 4) >> 2, g6 = ((g >> 3) << 3 | 4) >> 2, b6 = ((b >> 3) << 3 | 4) >> 2;
    int best = 1, best_d = 1 << 30;
    for (int i = 1; i < 256; i++)
    {
        const int dr = r6 - pal6[i * 3], dg = g6 - pal6[i * 3 + 1], db = b6 - pal6[i * 3 + 2];
        const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    ui_nearest[key] = (unsigned char)best;
    ui_nearest_known[key >> 3] |= (unsigned char)(1 << (key & 7));
    return (unsigned char)best;
}

const unsigned char *ui_blit_palette = NULL;

void ui_canvas_blit_8bit(const struct UiCanvas *c, unsigned char *idx, const int *remake, unsigned char *screen, int pitch,
    int scr_w, int scr_h, int x, int y)
{
    if ((c->px == NULL) || (idx == NULL) || (screen == NULL))
        return;
    const int rx0 = (remake != NULL) ? max(0, remake[0]) : 0, ry0 = (remake != NULL) ? max(0, remake[1]) : 0;
    const int rx1 = (remake != NULL) ? min(c->w, remake[2]) : 0, ry1 = (remake != NULL) ? min(c->h, remake[3]) : 0;
    const unsigned char *pal6 = (ui_blit_palette != NULL) ? ui_blit_palette : engine_palette;
    for (int ry = ry0; ry < ry1; ry++)
    {
        for (int rx = rx0; rx < rx1; rx++)
        {
            const int i = ry * c->w + rx;
            const unsigned char *p = c->px + (size_t)i * 4;
            if ((p[3] < 128) || (pal6 == NULL))
            {
                idx[i] = 0;
                continue;
            }
            /* (premultiplied: back to the colour itself) */
            const int r = min(255, p[0] * 255 / p[3]), g = min(255, p[1] * 255 / p[3]), b = min(255, p[2] * 255 / p[3]);
            idx[i] = nearest_index(pal6, r, g, b);
        }
    }
    const int x0 = max(0, x), y0 = max(0, y), x1 = min(scr_w, x + c->w), y1 = min(scr_h, y + c->h);
    for (int py = y0; py < y1; py++)
    {
        const unsigned char *src = idx + (size_t)(py - y) * (size_t)c->w + (size_t)(x0 - x);
        unsigned char *dst = screen + (size_t)py * (size_t)pitch + (size_t)x0;
        for (int px = x0; px < x1; px++, src++, dst++)
            if (*src != 0)
                *dst = *src;
    }
}

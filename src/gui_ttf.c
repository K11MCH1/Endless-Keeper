/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file gui_ttf.c
 *     Lines of text in a TrueType font, made into the game's palette sprites (2026-11-19).
 * @par Purpose:
 *     Big text, such as the Endless circle banner, where the game's bitmap fonts scaled up looked blocky. A line is
 *     rasterised once at the size it is shown (stb_truetype), filled with a top-to-bottom gradient, given a dark
 *     outline and a soft shadow, then cut into tiles at most TTF_TILE pixels wide and high (a 32-bit build's sprite
 *     width and height are a byte each) in two layers: the solid pixels, and the soft edge drawn see-through.
 *     The font: fxdata/banner.ttf when there is one, else one of the system's (Windows: Palatino Linotype Bold,
 *     Georgia Bold, Constantia Bold; Android and Linux: Noto Serif Bold, DejaVu Serif Bold). None: ttf_text_make
 *     returns NULL and the callers draw with the bitmap fonts as before.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "gui_ttf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_sprite.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "config.h"
#include "front_simple.h"
#include "globals.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "kfx/renderer/RendererManager.h"
#include "../deps/stb_truetype/stb_truetype.h"
#include "post_inc.h"

#define TTF_TILE 240
#define TTF_MAX_PX 400
#define TTF_SOLID 0.55f     /* at least this opaque: the solid layer */
#define TTF_SOFT 0.12f      /* at least this: the soft edge; less: not drawn */

static stbtt_fontinfo ttf_font;
static unsigned char *ttf_file = NULL;
static int ttf_state = 0;   /* 0: not tried, 1: loaded, -1: none */

/* top and bottom of each style's gradient, 8-bit RGB */
static const unsigned char ttf_top[TtfS_Count][3] = {{255, 96, 56}, {255, 240, 204}};
static const unsigned char ttf_bottom[TtfS_Count][3] = {{150, 16, 8}, {200, 152, 84}};
static const unsigned char ttf_outline[TtfS_Count][3] = {{26, 4, 2}, {30, 18, 8}};

static unsigned char *ttf_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    unsigned char *data = NULL;
    if ((fseek(f, 0, SEEK_END) == 0))
    {
        const long len = ftell(f);
        if ((len > 1024) && (len < 64L * 1024 * 1024) && (fseek(f, 0, SEEK_SET) == 0))
        {
            data = malloc((size_t)len);
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

static TbBool ttf_try(const char *path)
{
    unsigned char *data = ttf_read(path);
    if (data == NULL)
        return false;
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if ((offset < 0) || !stbtt_InitFont(&ttf_font, data, offset))
    {
        free(data);
        return false;
    }
    ttf_file = data;
    JUSTMSG("TrueType font for big text: %s", path);
    return true;
}

TbBool ttf_available(void)
{
    if (ttf_state != 0)
        return (ttf_state > 0);
    ttf_state = -1;
    if (ttf_try(prepare_file_path(FGrp_FxData, "banner.ttf")))
    {
        ttf_state = 1;
        return true;
    }
#ifdef _WIN32
    static const char *const names[] = {"palab.ttf", "georgiab.ttf", "constanb.ttf"};
    const char *windir = getenv("WINDIR");
    if ((windir == NULL) || (windir[0] == '\0'))
        windir = "C:\\Windows";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, names[i]);
        if (ttf_try(path))
        {
            ttf_state = 1;
            return true;
        }
    }
#else
    static const char *const paths[] = {"/system/fonts/NotoSerif-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSerif-Bold.ttf"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
        if (ttf_try(paths[i]))
        {
            ttf_state = 1;
            return true;
        }
    }
#endif
    WARNMSG("No TrueType font found for big text: the bitmap font is used");
    return false;
}

static inline float ttf_clamp01(float v)
{
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

/** One row of palette pixels (-1: transparent) as the sprite's runs: n > 0 then n pixels, -n a gap, 0 the row's end */
static unsigned char *ttf_rle_row(const short *px, int n, unsigned char *out)
{
    int i = 0;
    while (i < n)
    {
        int k = 0;
        if (px[i] < 0)
        {
            while ((i < n) && (px[i] < 0) && (k < 127))
            {
                i++;
                k++;
            }
            if (i < n) /* a gap at the row's end needs no run */
                *out++ = (unsigned char)(-k);
        }
        else
        {
            unsigned char *count = out++;
            while ((i < n) && (px[i] >= 0) && (k < 127))
            {
                *out++ = (unsigned char)px[i];
                i++;
                k++;
            }
            *count = (unsigned char)k;
        }
    }
    *out++ = 0;
    return out;
}

/** One line of an exact squared-distance transform (Felzenszwalb and Huttenlocher): d[i] = min over j of f[j] + (i-j)^2. */
static void ttf_edt_line(const float *f, int n, float *d, int *v, float *z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -1e30f;
    z[1] = 1e30f;
    for (int q = 1; q < n; q++)
    {
        float s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (float)(q - v[k]));
        while (s <= z[k])
        {
            k--;
            s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (float)(q - v[k]));
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e30f;
    }
    k = 0;
    for (int q = 0; q < n; q++)
    {
        while (z[k + 1] < (float)q)
            k++;
        d[q] = (float)(q - v[k]) * (float)(q - v[k]) + f[v[k]];
    }
}

/** The outline's strength at each pixel: 1 within the radius of the letters (pixels at least half covered), the last
 *  pixel soft, and never less than the pixel's own coverage. False: no memory. */
static TbBool ttf_outline_from_distance(const float *cov, float *edge, int w, int h, int radius)
{
    const int n = max(w, h);
    /* (zeroed: gcc 13 on Linux took f for unset, -Werror=maybe-uninitialized) */
    float *f = calloc((size_t)n, sizeof(float)), *d = calloc((size_t)n, sizeof(float)), *z = calloc((size_t)n + 1, sizeof(float));
    int *v = calloc((size_t)n, sizeof(int));
    if ((f == NULL) || (d == NULL) || (z == NULL) || (v == NULL))
    {
        free(f);
        free(d);
        free(z);
        free(v);
        return false;
    }
    for (int i = 0; i < w * h; i++)
        edge[i] = (cov[i] >= 0.5f) ? 0.0f : 1e20f; /* squared distances, built in place */
    for (int x = 0; x < w; x++)
    {
        for (int y = 0; y < h; y++)
            f[y] = edge[y * w + x];
        ttf_edt_line(f, h, d, v, z);
        for (int y = 0; y < h; y++)
            edge[y * w + x] = d[y];
    }
    for (int y = 0; y < h; y++)
    {
        memcpy(f, &edge[y * w], sizeof(float) * w);
        ttf_edt_line(f, w, d, v, z);
        for (int x = 0; x < w; x++)
        {
            const float o = ttf_clamp01((float)radius + 0.75f - sqrtf(d[x]));
            edge[y * w + x] = (o > cov[y * w + x]) ? o : cov[y * w + x];
        }
    }
    free(f);
    free(d);
    free(z);
    free(v);
    return true;
}

int ttf_text_width(const char *text, int px, int tracking)
{
    if ((text == NULL) || (text[0] == '\0') || (px < 6) || !ttf_available())
        return 0;
    if (px > TTF_MAX_PX)
        px = TTF_MAX_PX;
    const float scale = stbtt_ScaleForPixelHeight(&ttf_font, (float)px);
    const int radius = max(1, (px + 10) / 22);
    const int shadow = max(1, radius * 3 / 4 + 1);
    const float track = (float)px * (float)tracking / 1000.0f;
    const size_t len = strlen(text);
    float pen = 0.0f;
    for (size_t i = 0; i < len; i++)
    {
        const int cp = (unsigned char)text[i];
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&ttf_font, cp, &advance, &bearing);
        pen += (float)advance * scale;
        if (i + 1 < len)
            pen += (float)stbtt_GetCodepointKernAdvance(&ttf_font, cp, (unsigned char)text[i + 1]) * scale + track;
    }
    return (int)ceilf(pen) + 2 * (radius + shadow + 2);
}

/** A line's letters rasterised at px (coverage) and their outline (edge), with the sizes round them. */
struct TtfRaster {
    int w, h, pad, shadow;
    float *cov, *edge;          /* w * h each, the caller frees them */
};

static TbBool ttf_raster(const char *text, int px, int tracking, struct TtfRaster *r)
{
    if (px > TTF_MAX_PX)
        px = TTF_MAX_PX;
    const float scale = stbtt_ScaleForPixelHeight(&ttf_font, (float)px);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&ttf_font, &ascent, &descent, &gap);
    const int radius = max(1, (px + 10) / 22);      /* the outline */
    const int shadow = max(1, radius * 3 / 4 + 1);  /* the shadow's offset, right and down */
    const float track = (float)px * (float)tracking / 1000.0f;
    const size_t len = strlen(text);
    float pen = 0.0f;
    for (size_t i = 0; i < len; i++)
    {
        const int cp = (unsigned char)text[i];
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&ttf_font, cp, &advance, &bearing);
        pen += (float)advance * scale;
        if (i + 1 < len)
            pen += (float)stbtt_GetCodepointKernAdvance(&ttf_font, cp, (unsigned char)text[i + 1]) * scale + track;
    }
    const int pad = radius + shadow + 2;
    const int w = (int)ceilf(pen) + 2 * pad;
    const int h = (int)ceilf((float)(ascent - descent) * scale) + 2 * pad;
    if ((w <= 2 * pad) || (w > 16384))
        return false;
    float *cov = calloc((size_t)w * h, sizeof(float));
    float *edge = calloc((size_t)w * h, sizeof(float));
    if ((cov == NULL) || (edge == NULL))
    {
        free(cov);
        free(edge);
        return false;
    }
    /* the letters' coverage */
    const int baseline = pad + (int)lroundf((float)ascent * scale);
    float x = (float)pad;
    for (size_t i = 0; i < len; i++)
    {
        const int cp = (unsigned char)text[i];
        const float fx = x - floorf(x);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&ttf_font, cp, scale, scale, fx, 0.0f, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if ((gw > 0) && (gh > 0))
        {
            unsigned char *g = malloc((size_t)gw * gh);
            if (g != NULL)
            {
                stbtt_MakeCodepointBitmapSubpixel(&ttf_font, g, gw, gh, gw, scale, scale, fx, 0.0f, cp);
                const int ox = (int)floorf(x) + x0, oy = baseline + y0;
                for (int gy = 0; gy < gh; gy++)
                {
                    const int ty = oy + gy;
                    if ((ty < 0) || (ty >= h))
                        continue;
                    for (int gx = 0; gx < gw; gx++)
                    {
                        const int tx = ox + gx;
                        if ((tx < 0) || (tx >= w))
                            continue;
                        const float c = g[gy * gw + gx] / 255.0f;
                        if (c > cov[ty * w + tx])
                            cov[ty * w + tx] = c;
                    }
                }
                free(g);
            }
        }
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&ttf_font, cp, &advance, &bearing);
        x += (float)advance * scale;
        if (i + 1 < len)
            x += (float)stbtt_GetCodepointKernAdvance(&ttf_font, cp, (unsigned char)text[i + 1]) * scale + track;
    }
    /* the outline: everything within the radius of the letters, with a soft last pixel. From the distance of each
       pixel to the nearest one the letters cover (an exact distance map, two passes): it was the coverage spread
       over a disc, every pixel looking at all of the disc round it - over a second for a banner 2304 x 455 at 387 px,
       and the game stood still meanwhile (the user, 2026-11-28: "pause almost 2 seconds") */
    if (!ttf_outline_from_distance(cov, edge, w, h, radius))
    {
        free(cov);
        free(edge);
        return false;
    }
    r->w = w;
    r->h = h;
    r->pad = pad;
    r->shadow = shadow;
    r->cov = cov;
    r->edge = edge;
    return true;
}

struct TtfText *ttf_text_make(const char *text, int px, enum TtfStyle style, int tracking)
{
    if ((text == NULL) || (text[0] == 0) || (px < 6) || (engine_palette == NULL) || !ttf_available())
        return NULL;
    if ((style < 0) || (style >= TtfS_Count))
        style = TtfS_Red;
    struct TtfRaster ras;
    if (!ttf_raster(text, px, tracking, &ras))
        return NULL;
    const int w = ras.w, h = ras.h, pad = ras.pad, shadow = ras.shadow;
    float *cov = ras.cov, *edge = ras.edge;
    short *row = malloc(sizeof(short) * TTF_TILE);
    if (row == NULL)
    {
        free(cov);
        free(edge);
        return NULL;
    }
    /* colours: the gradient over the outline over the shadow, dithered a little (the palette has few shades of one
       colour: a smooth gradient showed bands); the palette's nearest, remembered per colour (the palette's 6 bits) */
    uint16_t *nearest = malloc(sizeof(uint16_t) * 262144);
    struct TtfText *t = calloc(1, sizeof(struct TtfText));
    const int cols = (w + TTF_TILE - 1) / TTF_TILE, rows = (h + TTF_TILE - 1) / TTF_TILE;
    const int tiles = cols * rows;
    if ((nearest == NULL) || (t == NULL) || ((t->spr = calloc((size_t)tiles * 2, sizeof(struct TbSprite))) == NULL))
    {
        free(nearest);
        free(t);
        free(cov);
        free(edge);
        free(row);
        return NULL;
    }
    memset(nearest, 0xFF, sizeof(uint16_t) * 262144);
    t->w = w;
    t->h = h;
    t->pad = pad;
    t->cols = cols;
    t->rows = rows;
    const float text_top = (float)pad, text_h = (float)(h - 2 * pad);
    TbBool failed = false;
    for (int layer = 0; (layer < 2) && !failed; layer++)
    {
        for (int s = 0; (s < tiles) && !failed; s++)
        {
            const int sx0 = (s % cols) * TTF_TILE, sw = min(TTF_TILE, w - sx0);
            const int sy0 = (s / cols) * TTF_TILE, sh = min(TTF_TILE, h - sy0);
            unsigned char *data = malloc((size_t)sh * (2 * sw + 2));
            if (data == NULL)
            {
                failed = true;
                break;
            }
            unsigned char *out = data;
            for (int py = sy0; py < sy0 + sh; py++)
            {
                const float gy = ttf_clamp01(((float)py - text_top) / max(text_h, 1.0f));
                float fill[3];
                for (int k = 0; k < 3; k++)
                    fill[k] = (float)ttf_top[style][k] + ((float)ttf_bottom[style][k] - (float)ttf_top[style][k]) * gy;
                for (int i = 0; i < sw; i++)
                {
                    const int qx = sx0 + i;
                    const float c = cov[py * w + qx];
                    const float o = edge[py * w + qx];
                    const float a_text = (o > c) ? o : c;
                    float a_shadow = 0.0f;
                    if ((qx - shadow >= 0) && (py - shadow >= 0))
                        a_shadow = edge[(py - shadow) * w + qx - shadow] * 0.6f;
                    const float a = a_text + a_shadow * (1.0f - a_text);
                    const TbBool in_layer = (layer == 0) ? (a >= TTF_SOLID) : ((a >= TTF_SOFT) && (a < TTF_SOLID));
                    if (!in_layer)
                    {
                        row[i] = -1;
                        continue;
                    }
                    const float mix = (a_text > 0.0f) ? (c / a_text) : 0.0f;
                    /* 4x4 ordered dither, about two of the palette's steps from end to end */
                    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
                    const float dither = ((float)bayer[py & 3][qx & 3] - 7.5f) * (8.0f / 16.0f) * mix;
                    int rgb[3];
                    for (int k = 0; k < 3; k++)
                    {
                        const float col = (float)ttf_outline[style][k] + (fill[k] - (float)ttf_outline[style][k]) * mix;
                        /* the shadow is black: the text's colour weighted by its share */
                        rgb[k] = (int)lroundf(col * a_text / a + dither);
                        rgb[k] = max(0, min(255, rgb[k])) >> 2;
                    }
                    const int key = (rgb[0] << 12) | (rgb[1] << 6) | rgb[2];
                    if (nearest[key] == 0xFFFF)
                        nearest[key] = LbPaletteFindColour(engine_palette, (unsigned char)rgb[0], (unsigned char)rgb[1],
                            (unsigned char)rgb[2]);
                    row[i] = (short)nearest[key];
                }
                out = ttf_rle_row(row, sw, out);
            }
            unsigned char *fit = realloc(data, (size_t)(out - data));
            struct TbSprite *spr = &t->spr[layer * tiles + s];
            spr->Data = (fit != NULL) ? fit : data;
            spr->SWidth = sw;
            spr->SHeight = sh;
        }
    }
    free(nearest);
    free(cov);
    free(edge);
    free(row);
    if (failed)
    {
        ttf_text_free(t);
        return NULL;
    }
    return t;
}

void ttf_text_draw(const struct TtfText *t, long x, long y, const unsigned char *remap, TbBool faint)
{
    if (t == NULL)
        return;
    for (int layer = 0; layer < 2; layer++)
    {
        if ((layer == 1) && faint)
            break;
        RendererSetDrawFlags(((layer == 1) || faint) ? Lb_SPRITE_TRANSPAR4 : 0);
        const int tiles = t->cols * t->rows;
        for (int s = 0; s < tiles; s++)
        {
            const struct TbSprite *spr = &t->spr[layer * tiles + s];
            if (spr->Data == NULL)
                continue;
            const long tx = x + (s % t->cols) * TTF_TILE, ty = y + (s / t->cols) * TTF_TILE;
            if (remap != NULL)
                LbSpriteDrawScaledRemap(tx, ty, spr, spr->SWidth, spr->SHeight, remap);
            else
                LbSpriteDrawScaled(tx, ty, spr, spr->SWidth, spr->SHeight);
        }
    }
    RendererSetDrawFlags(0);
}

void ttf_text_free(struct TtfText *t)
{
    if (t == NULL)
        return;
    if (t->spr != NULL)
    {
        RendererForgetSprites(t->spr, 2L * t->cols * t->rows);
        for (int i = 0; i < 2 * t->cols * t->rows; i++)
            free((void *)t->spr[i].Data);
        free(t->spr);
    }
    free(t);
}

struct TtfRgba *ttf_text_make_rgba(const char *text, int px, enum TtfStyle style, int tracking)
{
    if ((text == NULL) || (text[0] == 0) || (px < 6) || !ttf_available())
        return NULL;
    if ((style < 0) || (style >= TtfS_Count))
        style = TtfS_Red;
    struct TtfRaster ras;
    if (!ttf_raster(text, px, tracking, &ras))
        return NULL;
    const int w = ras.w, h = ras.h, pad = ras.pad, shadow = ras.shadow;
    struct TtfRgba *t = calloc(1, sizeof(struct TtfRgba));
    unsigned char *out = (t != NULL) ? malloc((size_t)w * h * 4) : NULL;
    if (out == NULL)
    {
        free(t);
        free(ras.cov);
        free(ras.edge);
        return NULL;
    }
    const float text_top = (float)pad, text_h = (float)(h - 2 * pad);
    for (int py = 0; py < h; py++)
    {
        const float gy = ttf_clamp01(((float)py - text_top) / max(text_h, 1.0f));
        float fill[3];
        for (int k = 0; k < 3; k++)
            fill[k] = (float)ttf_top[style][k] + ((float)ttf_bottom[style][k] - (float)ttf_top[style][k]) * gy;
        unsigned char *o = out + (size_t)py * w * 4;
        for (int qx = 0; qx < w; qx++, o += 4)
        {
            const float c = ras.cov[py * w + qx];
            const float e = ras.edge[py * w + qx];
            const float a_text = (e > c) ? e : c;
            float a_shadow = 0.0f;
            if ((qx - shadow >= 0) && (py - shadow >= 0))
                a_shadow = ras.edge[(py - shadow) * w + qx - shadow] * 0.6f;
            const float a = a_text + a_shadow * (1.0f - a_text);
            if (a < 0.004f)
            {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            /* the gradient over the outline, over a black shadow; premultiplied (the shadow adds only its cover) */
            const float mix = (a_text > 0.0f) ? (c / a_text) : 0.0f;
            for (int k = 0; k < 3; k++)
            {
                const float col = (float)ttf_outline[style][k] + (fill[k] - (float)ttf_outline[style][k]) * mix;
                o[k] = (unsigned char)lroundf(max(0.0f, min(255.0f, col * a_text)));
            }
            o[3] = (unsigned char)lroundf(max(0.0f, min(255.0f, a * 255.0f)));
        }
    }
    free(ras.cov);
    free(ras.edge);
    t->w = w;
    t->h = h;
    t->pad = pad;
    t->px = out;
    return t;
}

void ttf_rgba_free(struct TtfRgba *t)
{
    if (t == NULL)
        return;
    free(t->px);
    free(t);
}

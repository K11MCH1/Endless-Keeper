/******************************************************************************/
// Endless Keeper - the launcher's "Molten" look for in-game windows
/******************************************************************************/
/** @file ui_molten.c
 *     The pieces of a window in the launcher's Molten look, painted in true colour (see ui_molten.h).
 * @par Purpose:
 *     Colours, radii and light are the launcher's (test/launcher-modern e326fb7, launcher/launcher.cpp): the panel's
 *     glass (28,18,14)-(11,8,7) with warm light (255,190,130) along its top and a (255,214,170) edge, the Play button's
 *     molten red (176,34,16)-(84,9,7) with an orange halo, the other buttons' dark glass with gold words, tabs lit
 *     (142,40,18) with a (255,190,104) line. The game shows through the glass (no blur: there is no copy of the frame).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "ui_molten.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_datetm.h"
#include "bflib_video.h"
#include "globals.h"
#include "kfx/renderer/RendererManager.h"
#include "bflib_sprite.h"
#include "custom_sprites.h"
#include "front_simple.h"
#include "world_side_panel.h"
#include "post_inc.h"

#define U(v) ((v) * st->u)

#define UM_ICONS 4096
static unsigned char *um_icon_px[UM_ICONS];
static int um_icon_w[UM_ICONS], um_icon_h[UM_ICONS];
static unsigned char um_icon_tried[UM_ICONS];

const unsigned char *um_icon(long spr_idx, int *w, int *h)
{
    if ((spr_idx <= 0) || (spr_idx >= UM_ICONS))
        return NULL;
    if ((um_icon_px[spr_idx] == NULL) && !um_icon_tried[spr_idx])
    {
        um_icon_tried[spr_idx] = 1;
        int ow = 0, oh = 0;
        unsigned char *ov = world_ui_icon_override(0, spr_idx, &ow, &oh); /* (an edited picture first) */
        if (ov != NULL)
        {
            um_icon_px[spr_idx] = ov;
            um_icon_w[spr_idx] = ow;
            um_icon_h[spr_idx] = oh;
            *w = ow;
            *h = oh;
            return ov;
        }
        const struct TbSprite *spr = get_panel_sprite((short)spr_idx);
        if ((spr == NULL) || (spr->SWidth == 0) || (spr->SHeight == 0) || (engine_palette == NULL))
            return NULL;
        unsigned char *px = (unsigned char *)calloc((size_t)spr->SWidth * spr->SHeight, 4);
        if (px == NULL)
            return NULL;
        ui_sprite_rgba(spr, engine_palette, px);
        /* the drop shadow the panel's icons carry down and to their right (their darkest pixels, (20,16,0) in the game's
           palette) is taken off */
        for (int i = 0; i < spr->SWidth * spr->SHeight; i++)
        {
            unsigned char *q = px + (size_t)i * 4;
            if ((q[3] != 0) && (q[0] * 3 + q[1] * 6 + q[2] < 200))
                q[3] = 0;
        }
        /* (cropped to the picture: the sprite stands where it goes in the panel's frame, with room round it, and a
           near-black drop shadow down and to the right, which is not counted, so the picture itself is centred) */
        const int sw = spr->SWidth, sh = spr->SHeight;
        int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++)
            {
                const unsigned char *q = px + ((size_t)y * sw + x) * 4;
                if ((q[3] != 0) && (q[0] * 3 + q[1] * 6 + q[2] > 400))
                {
                    x0 = min(x0, x);
                    y0 = min(y0, y);
                    x1 = max(x1, x);
                    y1 = max(y1, y);
                }
            }
        if (x1 < x0)
            x0 = y0 = 0, x1 = sw - 1, y1 = sh - 1;
        const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        for (int y = 0; y < ch; y++)
            memmove(px + (size_t)y * cw * 4, px + ((size_t)(y + y0) * sw + x0) * 4, (size_t)cw * 4);
        um_icon_px[spr_idx] = px;
        um_icon_w[spr_idx] = cw;
        um_icon_h[spr_idx] = ch;
    }
    *w = um_icon_w[spr_idx];
    *h = um_icon_h[spr_idx];
    return um_icon_px[spr_idx];
}

void um_icon_draw(struct UiCanvas *c, long spr_idx, float x0, float y0, float x1, float y1, float fill, float gain)
{
    int iw = 0, ih = 0;
    const unsigned char *icon = um_icon(spr_idx, &iw, &ih);
    if ((icon == NULL) || (iw <= 0) || (ih <= 0))
        return;
    const float rw = (x1 - x0) * fill, rh = (y1 - y0) * fill;
    const float fit_f = min(rw / (float)iw, rh / (float)ih);
    const int k = (int)floorf(fit_f);
    const float fit = ((k >= 1) && ((float)k >= fit_f * 0.75f)) ? (float)k : fit_f; /* (a whole number of its pixels when near) */
    const int dw = max(1, (int)((float)iw * fit)), dh = max(1, (int)((float)ih * fit));
    const int dx = (int)floorf((x0 + x1 - (float)dw) * 0.5f + 0.5f), dy = (int)floorf((y0 + y1 - (float)dh) * 0.5f + 0.5f);
    ui_image_gain(c, dx, dy, dw, dh, icon, iw, ih, gain);
}

float um_window_margin(const struct UmStyle *st)
{
    return st->flat ? 0.0f : ceilf(U(44.0f));
}

void um_window(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1)
{
    if (st->flat)
    {
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, UIC(20, 14, 11, 255), UIC(20, 14, 11, 255), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, max(1.0f, floorf(U(1.0f))), UIC(104, 78, 36, 255));
        return;
    }
    const float r = U(16.0f);
    ui_shadow_rrect(c, x0, y0, x1, y1, r, U(40.0f), U(10.0f), UIC(0, 0, 0, 160));
    ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(30, 18, 14, 220), UIC(10, 7, 6, 236), false);
    ui_fill_rrect(c, x0 + 1, y0 + 1, x1 - 1, y0 + U(72.0f), r, UIC(255, 190, 130, 26), UIC(255, 190, 130, 0), false);
    ui_stroke_rrect(c, x0, y0, x1, y1, r, 1.0f, UIC(255, 214, 170, 44));
}

void um_card(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1)
{
    if (st->flat)
    {
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, UIC(30, 22, 17, 255), UIC(30, 22, 17, 255), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, 1.0f, UIC(64, 46, 26, 255));
        return;
    }
    const float r = U(12.0f);
    ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(255, 236, 214, 14), UIC(255, 236, 214, 6), false);
    ui_stroke_rrect(c, x0, y0, x1, y1, r, 1.0f, UIC(255, 214, 170, 28));
}

static void label(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float px,
    UiColour col, float spacing, const char *text)
{
    const float room = (x1 - x0) - U(22.0f);
    const float fpx = ui_text_fit(UiF_Head, px, spacing, room, text);
    ui_text_box(c, UiF_Head, fpx, x0, y0, x1 - x0, y1 - y0, 1, col, spacing, !st->flat, text);
}

void um_button(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, int kind, int state,
    float px, const char *text)
{
    const TbBool hot = (state == UmS_Hot) || (state == UmS_Down) || (state == UmS_Armed);
    const TbBool down = (state == UmS_Down);
    if (st->flat)
    {
        UiColour fill = UIC(30, 22, 17, 255), edge = UIC(104, 78, 36, 255), word = UM_GOLD;
        if (kind == UmB_Primary)
        {
            fill = hot ? UIC(176, 40, 16, 255) : UIC(140, 28, 12, 255);
            edge = UIC(222, 140, 80, 255);
            word = UM_LIGHT;
        } else if (hot)
        {
            fill = UIC(120, 30, 14, 255);
            edge = UM_GOLD_HI;
            word = UIC(255, 244, 214, 255);
        }
        if (state == UmS_Off)
        {
            fill = UIC(26, 20, 16, 255);
            edge = UIC(64, 46, 26, 255);
            word = UM_DIM;
        }
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, fill, fill, false);
        ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, 1.0f, edge);
        label(c, st, x0, y0, x1, y1, px, word, 0.0f, text);
        return;
    }
    const float h = y1 - y0;
    const float dy = down ? U(1.0f) : 0.0f;
    y0 += dy;
    y1 += dy;
    if (kind == UmB_Primary)
    {
        const float r = min(h * 0.5f, U(16.0f));
        ui_shadow_rrect(c, x0, y0, x1, y1, r, U(16.0f), U(4.0f), UIC(0, 0, 0, 130));
        ui_glow_rrect(c, x0, y0, x1, y1, r, U(hot ? 34.0f : 26.0f), UIC(255, 80, 24, hot ? 120 : 80));
        ui_fill_rrect(c, x0, y0, x1, y1, r, hot ? UIC(228, 66, 26, 255) : UIC(190, 40, 18, 255),
            hot ? UIC(126, 18, 10, 255) : UIC(92, 10, 7, 255), false);
        /* the molten glow along its bottom and the sheen on its top */
        ui_fill_rrect(c, x0 + 1, y0 + h * 0.45f, x1 - 1, y1 - 1, r, UIC(255, 150, 44, 0), UIC(255, 150, 44, hot ? 90 : 60), false);
        ui_fill_rrect(c, x0 + 1, y0 + 1, x1 - 1, y0 + h * 0.42f, r, UIC(255, 226, 190, 30), UIC(255, 226, 190, 0), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, U(1.25f)), hot ? UIC(255, 230, 170, 255) : UIC(255, 168, 96, 140));
        label(c, st, x0, y0, x1, y1, px, hot ? UIC(255, 250, 236, 255) : UIC(255, 236, 200, 255), 0.0f, text);
        return;
    }
    const float r = min(h * 0.5f, U(11.0f));
    ui_shadow_rrect(c, x0, y0, x1, y1, r, U(14.0f), U(4.0f), UIC(0, 0, 0, 115));
    if (state == UmS_Off)
    {
        ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(34, 30, 28, 170), UIC(14, 12, 11, 200), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, r, 1.0f, UIC(255, 214, 170, 22));
        label(c, st, x0, y0, x1, y1, px, UIC(160, 146, 122, 158), 0.0f, text);
        return;
    }
    if (hot)
        ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(150, 46, 20, 215), UIC(58, 14, 9, 225), false);
    else
        ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(44, 26, 18, 190), UIC(12, 8, 7, 215), false);
    ui_fill_rrect(c, x0 + 1, y0 + 1, x1 - 1, y0 + h * 0.5f, r, UIC(255, 210, 160, 22), UIC(255, 210, 160, 0), false);
    ui_stroke_rrect(c, x0, y0, x1, y1, r, hot ? max(1.0f, U(1.5f)) : 1.0f, hot ? UM_GOLD_HI : UIC(255, 214, 170, 56));
    label(c, st, x0, y0, x1, y1, px, hot ? UIC(255, 244, 214, 255) : UM_GOLD, 0.0f, text);
}

void um_tab(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, TbBool chosen,
    TbBool hot, float px, const char *text)
{
    const float r = st->flat ? 0.0f : U(8.0f);
    if (chosen)
    {
        if (!st->flat)
            ui_glow_rrect(c, x0, y0, x1, y1, r, U(14.0f), UIC(255, 80, 24, 70));
        ui_fill_rrect(c, x0, y0, x1, y1, r, st->flat ? UIC(126, 30, 14, 255) : UIC(160, 42, 18, 255),
            st->flat ? UIC(126, 30, 14, 255) : UIC(110, 24, 12, 255), false);
        /* the gold line along its top */
        ui_fill_rrect(c, x0 + r * 0.5f, y0, x1 - r * 0.5f, y0 + max(1.0f, U(2.0f)), 0.0f, UIC(255, 190, 104, 255),
            UIC(255, 190, 104, 255), false);
        label(c, st, x0, y0, x1, y1, px, UM_LIGHT, 0.0f, text);
        return;
    }
    if (hot)
        ui_fill_rrect(c, x0, y0, x1, y1, r, st->flat ? UIC(44, 32, 24, 255) : UIC(255, 226, 196, 20),
            st->flat ? UIC(44, 32, 24, 255) : UIC(255, 226, 196, 20), false);
    label(c, st, x0, y0, x1, y1, px, hot ? UM_GOLD_HI : UM_DIM, 0.0f, text);
}

void um_bar(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float frac)
{
    frac = (frac < 0.0f) ? 0.0f : ((frac > 1.0f) ? 1.0f : frac);
    const float h = y1 - y0;
    const float r = st->flat ? 0.0f : h * 0.5f;
    ui_fill_rrect(c, x0, y0, x1, y1, r, st->flat ? UIC(36, 26, 20, 255) : UIC(255, 236, 214, 20),
        st->flat ? UIC(36, 26, 20, 255) : UIC(255, 236, 214, 20), false);
    if (frac <= 0.0f)
        return;
    const float fx = x0 + max(h, (x1 - x0) * frac);
    if (st->flat)
    {
        ui_fill_rrect(c, x0, y0, x0 + (x1 - x0) * frac, y1, 0.0f, UIC(214, 70, 24, 255), UIC(214, 70, 24, 255), false);
        return;
    }
    ui_glow_rrect(c, x0, y0, fx, y1, r, U(12.0f), UIC(255, 100, 30, 70));
    ui_fill_rrect(c, x0, y0, fx, y1, r, UIC(176, 34, 16, 255), UIC(255, 176, 80, 255), true);
    ui_fill_rrect(c, x0 + 1, y0, fx - 1, y0 + h * 0.5f, r, UIC(255, 240, 210, 50), UIC(255, 240, 210, 0), false);
}

void um_well(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float radius)
{
    if (st->flat)
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, UIC(12, 9, 8, 255), UIC(12, 9, 8, 255), false);
    else
        ui_fill_rrect(c, x0, y0, x1, y1, radius, UIC(0, 0, 0, 120), UIC(0, 0, 0, 80), false);
}

void um_frame(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float radius,
    TbBool lit)
{
    if (st->flat)
    {
        ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, lit ? max(1.0f, floorf(U(2.0f))) : 1.0f, lit ? UM_GOLD_HI : UM_GOLD);
        return;
    }
    if (lit)
        ui_glow_rrect(c, x0, y0, x1, y1, radius, U(18.0f), UIC(255, 120, 40, 90));
    ui_stroke_rrect(c, x0, y0, x1, y1, radius, max(1.0f, U(lit ? 2.0f : 1.5f)), lit ? UM_GOLD_HI : UIC(222, 178, 88, 190));
}

void um_rule(struct UiCanvas *c, const struct UmStyle *st, float x0, float y, float x1)
{
    const float t = max(1.0f, floorf(U(0.75f) + 0.5f));
    if (st->flat)
        ui_fill_rrect(c, x0, y, x1, y + t, 0.0f, UIC(64, 46, 26, 255), UIC(64, 46, 26, 255), false);
    else
        ui_line_fade(c, x0, y, x1, t, UIC(255, 214, 170, 80));
}

void um_heading(struct UiCanvas *c, const struct UmStyle *st, float x, float y, float h, float px, const char *text)
{
    ui_text_box(c, UiF_Head, px, x, y, 4096.0f, h, 0, UM_GOLD, U(0.6f), !st->flat, text);
}

/******************************************************************************/
/* The host */

static int um_dump = -1;
static char um_dump_dir[512];

static TbBool um_dumping(void)
{
    if (um_dump < 0)
    {
        const char *v = getenv("KFX_UI_TCDUMP");
        um_dump = ((v != NULL) && (v[0] != '\0')) ? 1 : 0;
        if (um_dump)
        {
            snprintf(um_dump_dir, sizeof(um_dump_dir), "%s", v);
            SYNCLOG("UI paint: test switch KFX_UI_TCDUMP=%s", um_dump_dir);
        }
    }
    return (um_dump > 0);
}

TbBool um_host_flat(void)
{
    if (um_dumping())
        return false;
    return !RendererIsGPUBackend();
}

TbBool um_host_begin(struct UmHost *h, int cw, int ch, const char *key)
{
    char full[sizeof(h->key)];
    snprintf(full, sizeof(full), "%dx%d %d|%s", cw, ch, (int)um_host_flat(), key);
    if ((h->canvas.px != NULL) && (h->canvas.w == cw) && (h->canvas.h == ch) && (strcmp(full, h->key) == 0))
        return false;
    /* (a window with a base has the base copied over all of it: no need to clear it first) */
    if (!((h->base.px != NULL) ? ui_canvas_size_raw(&h->canvas, cw, ch) : ui_canvas_size(&h->canvas, cw, ch)))
        return false;
    memcpy(h->key, full, sizeof(h->key));
    return true;
}

TbBool um_host_base_begin(struct UmHost *h, const char *key)
{
    char full[sizeof(h->base_key)];
    snprintf(full, sizeof(full), "%dx%d %d|%s", h->canvas.w, h->canvas.h, (int)um_host_flat(), key);
    if ((h->base.px != NULL) && (h->base.w == h->canvas.w) && (h->base.h == h->canvas.h) && (strcmp(full, h->base_key) == 0))
        return false;
    if (!ui_canvas_size_raw(&h->base, h->canvas.w, h->canvas.h))
        return false;
    memcpy(h->base_key, full, sizeof(h->base_key));
    return true;
}

void um_host_base_down(struct UmHost *h)
{
    ui_canvas_copy(&h->canvas, &h->base);
}

TbBool um_host_glass_begin(struct UmHost *h, const char *key)
{
    char full[sizeof(h->glass_key)];
    snprintf(full, sizeof(full), "%dx%d %d|%s", h->canvas.w, h->canvas.h, (int)um_host_flat(), key);
    if ((h->glass.px != NULL) && (h->glass.w == h->canvas.w) && (h->glass.h == h->canvas.h) && (strcmp(full, h->glass_key) == 0))
        return false;
    if (!ui_canvas_size(&h->glass, h->canvas.w, h->canvas.h))
        return false;
    memcpy(h->glass_key, full, sizeof(h->glass_key));
    return true;
}

void um_host_glass_down(struct UmHost *h)
{
    ui_canvas_copy(&h->base, &h->glass);
}

uint32_t um_hash(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)buf; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

void um_part(struct UmPart *parts, int *n, float u, float m, float x, float y, float w, float h, float room, uint32_t hash)
{
    if (*n >= UM_PARTS)
        return;
    struct UmPart *p = &parts[(*n)++];
    p->x0 = (int)floorf(m + (x - room) * u) - 2;
    p->y0 = (int)floorf(m + (y - room) * u) - 2;
    p->x1 = (int)ceilf(m + (x + w + room) * u) + 2;
    p->y1 = (int)ceilf(m + (y + h + room) * u) + 2;
    p->hash = hash;
}

static void union_rect(int *d, const struct UmPart *p)
{
    if (d[2] <= d[0])
    {
        d[0] = p->x0;
        d[1] = p->y0;
        d[2] = p->x1;
        d[3] = p->y1;
        return;
    }
    d[0] = min(d[0], p->x0);
    d[1] = min(d[1], p->y0);
    d[2] = max(d[2], p->x1);
    d[3] = max(d[3], p->y1);
}

TbBool um_host_parts(struct UmHost *h, const struct UmPart *parts, int n, TbBool full)
{
    struct UiCanvas *c = &h->canvas;
    if ((c->px == NULL) || (h->base.px == NULL))
        return false;
    n = min(n, UM_PARTS);
    int d[4] = {0, 0, 0, 0};
    if (full || (n != h->nparts))
    {
        d[2] = c->w;
        d[3] = c->h;
    } else
    {
        for (int i = 0; i < n; i++)
        {
            const struct UmPart *o = &h->parts[i], *p = &parts[i];
            if ((o->hash == p->hash) && (o->x0 == p->x0) && (o->y0 == p->y0) && (o->x1 == p->x1) && (o->y1 == p->y1))
                continue;
            union_rect(d, o);
            union_rect(d, p);
        }
    }
    memcpy(h->parts, parts, sizeof(struct UmPart) * (size_t)n);
    h->nparts = n;
    d[0] = max(0, d[0]);
    d[1] = max(0, d[1]);
    d[2] = min(c->w, d[2]);
    d[3] = min(c->h, d[3]);
    if ((d[2] <= d[0]) || (d[3] <= d[1]))
        return false;
    /* the base under it, then the live part painted in it only */
    for (int y = d[1]; y < d[3]; y++)
        memcpy(c->px + ((size_t)y * c->w + d[0]) * 4, h->base.px + ((size_t)y * c->w + d[0]) * 4, (size_t)(d[2] - d[0]) * 4);
    ui_canvas_clip(c, d[0], d[1], d[2], d[3]);
    memcpy(h->dirty, d, sizeof(d));
    return true;
}

void um_host_parts_end(struct UmHost *h)
{
    ui_canvas_unclip(&h->canvas);
}

void um_host_painted(struct UmHost *h, unsigned long started_ms)
{
    h->gen++;
    if (h->gen == 0)
        h->gen = 1;
    h->idx_stale = true;
    if (h->nparts == 0)
    {
        h->dirty[0] = h->dirty[1] = 0;
        h->dirty[2] = h->canvas.w;
        h->dirty[3] = h->canvas.h;
    }
    h->paints++;
    h->paint_ms = (unsigned long)LbTimerClock() - started_ms;
    if (um_dumping() && (h->canvas.px != NULL))
    {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s_%lu.rgba", um_dump_dir, (h->name != NULL) ? h->name : "window", h->paints);
        FILE *f = fopen(path, "wb");
        if (f != NULL)
        {
            const int32_t wh[2] = {h->canvas.w, h->canvas.h};
            fwrite(wh, sizeof(wh), 1, f);
            fwrite(h->canvas.px, 4, (size_t)h->canvas.w * (size_t)h->canvas.h, f);
            fclose(f);
        }
        h->shown_x = h->shown_y = -99999;
    }
}

void um_host_show(struct UmHost *h, int x, int y)
{
    struct UiCanvas *c = &h->canvas;
    if (c->px == NULL)
        return;
    if (um_dumping())
    {
        if ((x != h->shown_x) || (y != h->shown_y))
            SYNCLOG("UI paint: %s_%lu shown at %d,%d (%dx%d) on %dx%d", (h->name != NULL) ? h->name : "window", h->paints, x, y,
                c->w, c->h, (int)MyScreenWidth, (int)MyScreenHeight);
        h->shown_x = x;
        h->shown_y = y;
        return;
    }
    if (!um_host_flat())
    {
        RendererSubmitUIImage(h->slot, x, y, c->w, c->h, c->px, h->gen);
        return;
    }
    const size_t need = (size_t)c->w * (size_t)c->h;
    if ((h->idx == NULL) || (h->idx_cap < need))
    {
        unsigned char *idx = (unsigned char *)malloc(need);
        if (idx == NULL)
            return;
        free(h->idx);
        h->idx = idx;
        h->idx_cap = need;
        h->idx_stale = true;
        h->dirty[0] = h->dirty[1] = 0; /* (a new buffer: all of it) */
        h->dirty[2] = c->w;
        h->dirty[3] = c->h;
    }
    if ((lbDisplay.WScreen == NULL) || (lbDisplay.GraphicsScreenWidth <= 0))
        return;
    ui_canvas_blit_8bit(c, h->idx, h->idx_stale ? h->dirty : NULL, lbDisplay.WScreen, (int)lbDisplay.GraphicsScreenWidth,
        (int)min((long)MyScreenWidth, lbDisplay.GraphicsScreenWidth), (int)min((long)MyScreenHeight, lbDisplay.GraphicsScreenHeight), x, y);
    h->idx_stale = false;
}

void um_host_forget(struct UmHost *h)
{
    ui_canvas_free(&h->canvas);
    ui_canvas_free(&h->base);
    h->base_key[0] = '\0';
    ui_canvas_free(&h->glass);
    h->glass_key[0] = '\0';
    h->nparts = 0;
    free(h->idx);
    h->idx = NULL;
    h->idx_cap = 0;
    h->key[0] = '\0';
}

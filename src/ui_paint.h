/******************************************************************************/
// Endless Keeper - true-colour painting for windows
/******************************************************************************/
/** @file ui_paint.h
 *     Windows painted in true colour on the CPU (2026-11-25): rounded boxes with gradients, soft shadows, glows, lines,
 *     pictures and antialiased TrueType text, into a premultiplied RGBA canvas. OpenGL and Vulkan lay the canvas over
 *     the UI (RendererSubmitUIImage); the software renderer gets it reduced to the game's palette (ui_canvas_blit_8bit).
 *     The look is the launcher's "Molten" one (launcher/fx.cpp on test/launcher-modern): dark glass, warm light.
 * @par Fonts:
 *     The launcher's: a book face for headings and buttons (Windows: Palatino Linotype Bold, Georgia Bold; Android
 *     and Linux: Noto Serif Bold, DejaVu Serif Bold) and a plain face for what is read (Segoe UI, Arial; Roboto, Noto
 *     Sans, DejaVu Sans). fxdata/ui_head.ttf and fxdata/ui_body.ttf come first when there. With neither face the
 *     game's own small font is drawn, scaled.
 */
/******************************************************************************/
#ifndef UI_PAINT_H
#define UI_PAINT_H

#include "bflib_basics.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TbSprite;

/** A colour, straight alpha: 0xAARRGGBB. */
typedef uint32_t UiColour;
#define UIC(r, g, b, a) ((UiColour)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))

struct UiCanvas {
    int w, h;
    unsigned char *px;  /* premultiplied RGBA, top row first */
    /* what is drawn is kept inside this (pixels) */
    int clip_x0, clip_y0, clip_x1, clip_y1;
};

/* the book face (headings), the plain face (text), and a sharp face for numbers (the side panel's gold and levels:
   the user 2026-11-26, "something more sharp than elegant") */
enum UiFont { UiF_Head = 0, UiF_Body, UiF_Num, UiF_Count };

/** Makes the canvas w x h (keeping its memory when it fits) and clears it to transparent. False: out of memory. */
TbBool ui_canvas_size(struct UiCanvas *c, int w, int h);
/** The same without clearing it (what it holds is to be covered anyway). */
TbBool ui_canvas_size_raw(struct UiCanvas *c, int w, int h);
void ui_canvas_free(struct UiCanvas *c);
void ui_canvas_clear(struct UiCanvas *c);
/** dst becomes a copy of src (the same size). */
void ui_canvas_copy(struct UiCanvas *dst, const struct UiCanvas *src);
void ui_canvas_clip(struct UiCanvas *c, int x0, int y0, int x1, int y1);
void ui_canvas_unclip(struct UiCanvas *c);

/** A rounded box, filled top to bottom (or left to right) with a gradient between two colours. */
void ui_fill_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, UiColour c0, UiColour c1,
    TbBool horizontal);
/** Its edge: a line `width` wide just inside it. */
void ui_stroke_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float width, UiColour col);
/** A soft shadow round a rounded box, `spread` wide, moved down by `dy` (nothing under the box itself). */
void ui_shadow_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float spread, float dy,
    UiColour col);
/** The same in light, which adds to what is under it (a halo). */
void ui_glow_rrect(struct UiCanvas *c, float x0, float y0, float x1, float y1, float radius, float spread, UiColour col);
/** A level line that fades out at both ends. */
void ui_line_fade(struct UiCanvas *c, float x0, float y, float x1, float thickness, UiColour col);
/** A picture (straight-alpha RGBA, sw x sh) stretched over the box, its pixels kept sharp, its corners cut round
 *  (`radius` pixels; 0: square). */
void ui_image(struct UiCanvas *c, int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh, float radius);
/** The same, square, its colours brighter by `gain` (1: as they are): a flash that lights only the picture. */
void ui_image_gain(struct UiCanvas *c, int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh, float gain);
/** A shape of straight-edged outlines filled even-odd (an outline inside another is a hole), its edges smooth:
 *  `xy` holds the points (x, y pairs, pixels), `counts` how many points each of the `n` outlines has. */
void ui_fill_path(struct UiCanvas *c, const float *xy, const int *counts, int n, UiColour col);
/** A round hollow pressed into the glass (the side panel's minimap lies in it): darker inside, a shade along its top
 *  edge `band` pixels deep, a faint light lip along its bottom edge. */
void ui_hollow_circle(struct UiCanvas *c, float cx, float cy, float r, float band);
/** A sprite in the palette (6-bit, 768 bytes) as straight-alpha RGBA (its width x height x 4 bytes). */
void ui_sprite_rgba(const struct TbSprite *spr, const unsigned char *pal6, unsigned char *out);

/** True when the font has a TrueType face (else the game's small font stands in). */
TbBool ui_font_ready(int font);
/** A line's width at `px` (the font's em size in pixels), `spacing` pixels between letters. */
float ui_text_width(int font, float px, float spacing, const char *text);
/** The capitals' height at `px`. */
float ui_text_cap(int font, float px);
/** The size at which the line fits `max_w` (`px` or smaller). */
float ui_text_fit(int font, float px, float spacing, float max_w, const char *text);
/** A line of text with its baseline at y. `shadow`: a soft dark shadow under it. */
void ui_text(struct UiCanvas *c, int font, float px, float x, float baseline, UiColour col, float spacing, TbBool shadow,
    const char *text);
/** The same, its capitals centred in the box (x, y, w, h); align 0 left, 1 centre, 2 right. */
void ui_text_box(struct UiCanvas *c, int font, float px, float x, float y, float w, float h, int align, UiColour col,
    float spacing, TbBool shadow, const char *text);

/** The canvas in the game's palette into an 8-bit screen (pitch bytes a row) at (x, y): the software renderer's
 *  version. Its palette indices are made once per change into `idx` (w*h bytes, 0 = see-through; the caller keeps
 *  it): `remake` (x0, y0, x1, y1 in the canvas, or NULL: none) says which part is new. */
/** The palette ui_canvas_blit_8bit reduces to (NULL: the game's, engine_palette); the frontend sets its own while it
 *  draws (its screens use another palette). */
extern const unsigned char *ui_blit_palette;
void ui_canvas_blit_8bit(const struct UiCanvas *c, unsigned char *idx, const int *remake, unsigned char *screen, int pitch,
    int scr_w, int scr_h, int x, int y);

#ifdef __cplusplus
}
#endif
#endif

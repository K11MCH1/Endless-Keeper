/******************************************************************************/
/** @file gui_ttf.h
 *     Header file for gui_ttf.c.
 */
/******************************************************************************/
#ifndef GUI_TTF_H
#define GUI_TTF_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

enum TtfStyle {
    TtfS_Red = 0,       /* bright red to deep red, top to bottom */
    TtfS_Parchment,     /* pale parchment to old gold */
    TtfS_Count
};

/** A line of text made into palette sprites: w x h pixels, with pad pixels of outline and shadow on every side round
 *  the text's own box (its top is the font's ascent). */
struct TtfText {
    int w, h, pad;
    int cols, rows;             /* cut into tiles, each at most TTF_TILE pixels wide and high, row by row */
    struct TbSprite *spr;       /* 2 * cols * rows: the solid layer, then the soft edge */
};

/** The same line in true colour (OpenGL and Vulkan: drawn as a picture, kfx/renderer/UIImage.h): premultiplied RGBA,
 *  w x h, top row first, with the same gradient, outline and shadow and none of the palette's steps. */
struct TtfRgba {
    int w, h, pad;
    unsigned char *px;
};

/** The width ttf_text_make would give the text, in pixels (0: no font): to fit it before it is made. */
int ttf_text_width(const char *text, int px, int tracking);
/** NULL: no TrueType font on this system (callers keep their bitmap font), or nothing to make. px: the font's height
 *  from ascent to descent in pixels; tracking: extra space between letters in thousandths of px. */
struct TtfText *ttf_text_make(const char *text, int px, enum TtfStyle style, int tracking);
/** Draws it at its own size, top-left at x, y. remap: colours through this table (NULL: as made); faint: the solid
 *  layer see-through too and the soft edge left out (the last steps of a fade). */
void ttf_text_draw(const struct TtfText *t, long x, long y, const unsigned char *remap, TbBool faint);
void ttf_text_free(struct TtfText *t);
/** NULL as ttf_text_make; the true-colour version of its text. */
struct TtfRgba *ttf_text_make_rgba(const char *text, int px, enum TtfStyle style, int tracking);
void ttf_rgba_free(struct TtfRgba *t);
/** Whether a TrueType font was found (loads it at the first call). */
TbBool ttf_available(void);

#ifdef __cplusplus
}
#endif
#endif

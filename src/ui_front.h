/******************************************************************************/
/** @file ui_front.h
 *     Endless Keeper: the frontend's screens behind the main menu in the Molten look.
 * @par Purpose:
 *     The game's frontend widgets (buttons, list rows, scroll boxes, sliders, arrows, scroll thumbs, text boxes, icons)
 *     are recorded while a screen draws and painted together, in true colour, into one glass window: the user,
 *     2026-11-26, "modernize in the same Molten design every menu past each of these buttons". WINDOW_STYLE=CLASSIC
 *     keeps the old look.
 */
/******************************************************************************/
#ifndef UI_FRONT_H
#define UI_FRONT_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuiButton;
struct TbSprite;

/** Frontend draw (frontend.cpp): a screen of the set starts drawing / its pieces are painted together. */
void uf_begin(void);
void uf_flush(void);
/** True while a Molten screen draws: the classic drawing of a piece is left out once it is recorded. */
TbBool uf_on(void);
/** Leaves something out of the recording (it draws as it always did, outside the window): returns what uf_resume takes. */
TbBool uf_pause(void);
void uf_resume(TbBool was);

/** The pieces. Each returns true when it was recorded (the caller then draws nothing itself). */
TbBool uf_button(const struct GuiButton *gbtn, const char *text);
/** Words the screen draws (LbTextDrawResized, any of its lists and labels): the text window, the place in it, the line
 *  height; the font (the frontend's four: normal, under the mouse, grey) and the alignment are read here. */
TbBool uf_text_at(int win_x, int win_y, int win_w, int win_h, int posx, int posy, int line_h, const char *text);
TbBool uf_well(int x, int y, int w, int h);
TbBool uf_tab(int x, int y, int w, int h);
TbBool uf_arrow(const struct GuiButton *gbtn, TbBool up);
TbBool uf_slider(const struct GuiButton *gbtn, int value256);
TbBool uf_thumb(int x, int y, int w, int h, int thumb_y, int thumb_h, TbBool hot);
TbBool uf_input(const struct GuiButton *gbtn, const char *text, TbBool active);
TbBool uf_sprite(int x, int y, int w, int h, const struct TbSprite *spr);

/** An in-game window's recorder (the creature window): what its own drawing (the Rap Sheet) draws while it is on is
 *  recorded, then painted by the window into its canvas after its own parts. */
void uf_window_begin(void);
void uf_window_stop(void);
uint32_t uf_window_hash(void);
struct UiCanvas;
struct UmStyle;
void uf_window_paint(struct UiCanvas *c, const struct UmStyle *st, int ox, int oy);
/** the window's pieces: its plain words (cap: their capitals' height; colour: a palette colour, its inks the Molten
 *  ones), its headings, its stone buttons, a vice's frame, a box of the drawing, an arrow */
TbBool uf_plain(int x, int y, int w, int cap, int align, unsigned char colour, const char *text);
TbBool uf_heading(int x, int y, int w, int cap, int align, const char *text);
TbBool uf_card(int x, int y, int w, int h, TbBool hot);
TbBool uf_frame(int x, int y, int w, int h, TbBool marked, TbBool hot);
TbBool uf_box(int x, int y, int w, int h, unsigned char colour);
/** a game sprite in true colour (the game's palette), kept */
const unsigned char *uf_sprite_true(const struct TbSprite *spr);
TbBool uf_arrow_at(int x, int y, int w, int h, TbBool up, TbBool enabled, TbBool hot);

#ifdef __cplusplus
}
#endif
#endif

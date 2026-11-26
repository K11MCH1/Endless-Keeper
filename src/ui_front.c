/******************************************************************************/
/** @file ui_front.c
 *     Endless Keeper: the frontend's screens behind the main menu in the Molten look.
 * @par Purpose:
 *     While one of the frontend's screens draws (uf_begin .. uf_flush, frontend.cpp), each classic widget asks here
 *     first: its place, words and state are recorded instead of its old sprites, and at the end of the frame all of
 *     them are painted into one glass window over the screen's own background (the user, 2026-11-26: "modernize in the
 *     same Molten design every menu past each of these buttons"). The game's buttons stay where they were: only their
 *     looks change, so clicking, scrolling and typing work as before. The window is painted again only when something
 *     in it changes (a key of every piece's state).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "ui_front.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>

#include "ui_paint.h"
#include "ui_molten.h"
#include "bflib_guibtns.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "kfx/renderer/RendererManager.h"
#include "bflib_video.h"
#include "bflib_datetm.h"
#include "config_keeperfx.h"
#include "frontend.h"
#include "gui_frontbtns.h"
#include "vidfade.h"
#include "vidmode.h"
#include "net_state.h"
#include "frontmenu_options.h"
#include "config_strings.h"
#include "front_simple.h"
#include "world_creature_window.h"
#include "post_inc.h"

enum UfKind { UfK_Title = 1, UfK_Button, UfK_Well, UfK_Tab, UfK_Text, UfK_Arrow, UfK_Slider, UfK_Thumb,
    UfK_Input, UfK_Sprite, UfK_Plain, UfK_Heading, UfK_Card, UfK_Frame, UfK_Box };
enum UfFlag { UfF_Enabled = 1, UfF_Hot = 2, UfF_Down = 4, UfF_Up = 8, UfF_Row = 16, UfF_Active = 32, UfF_OnTab = 64 };

#define UF_MAX 200
#define UF_COLS 1
struct UfItem {
    unsigned char kind, flags, font, align;
    int x, y, w, h;
    int a, b;                   /* a slider's value (0..255); a thumb's place and height */
    uint32_t col;               /* the window's pieces: their colour (UiColour) */
    const struct TbSprite *spr;
    const unsigned char *pal;   /* the sprite's palette */
    char text[UF_COLS][96];
};

/* two recorders: the frontend's screen, and an in-game window (the creature window) that paints its own parts and
   then what was recorded */
struct UfRec {
    struct UfItem items[UF_MAX];
    int n;
    TbBool armed;
};
static struct UfRec uf_rec_front, uf_rec_win;
static struct UfRec *uf_cur = &uf_rec_front;
#define uf_items (uf_cur->items)
#define uf_n (uf_cur->n)
#define uf_armed (uf_cur->armed)
static struct UmHost uf_host;

/** The screens of the set: every one behind the main menu's buttons (the main menu itself: world_side_panel.c). */
static TbBool uf_state_modern(int state)
{
    switch (state)
    {
    case FeSt_FELOAD_GAME: case FeSt_NET_SERVICE: case FeSt_NET_SESSION: case FeSt_NET_START: case FeSt_HIGH_SCORES:
    case FeSt_FEOPTIONS: case FeSt_FEDEFINE_KEYS: case FeSt_LEVEL_SELECT: case FeSt_MAPPACK_SELECT: case FeSt_CAMPAIGN_SELECT:
    case FeSt_SKIRMISH_LOBBY: case FeSt_ENDLESS_MENU: case FeSt_ENDLESS_LOAD: case FeSt_WORLD_LIST:
        return true;
    default:
        return false;
    }
}

TbBool uf_on(void)
{
    return uf_armed;
}

TbBool uf_pause(void)
{
    const TbBool was = uf_armed;
    uf_armed = false;
    return was;
}

void uf_resume(TbBool was)
{
    uf_armed = was;
}

void uf_begin(void)
{
    uf_cur = &uf_rec_front;
    uf_n = 0;
    uf_armed = (window_style != WinStyle_Classic) && uf_state_modern(frontend_menu_state) && !netstate_dedicated();
}

static struct UfItem *uf_add(int kind, int x, int y, int w, int h)
{
    if (!uf_armed || (uf_n >= UF_MAX))
        return NULL;
    struct UfItem *it = &uf_items[uf_n++];
    memset(it, 0, sizeof(*it));
    it->kind = (unsigned char)kind;
    it->x = x;
    it->y = y;
    it->w = max(w, 1);
    it->h = max(h, 1);
    return it;
}

static void uf_set_text(struct UfItem *it, int col, const char *text)
{
    snprintf(it->text[col], sizeof(it->text[col]), "%s", (text != NULL) ? text : "");
}

TbBool uf_button(const struct GuiButton *gbtn, const char *text)
{
    if (!uf_armed || (gbtn == NULL))
        return false;
    const TbBool title = (gbtn->id_num == BID_MENU_TITLE);
    struct UfItem *it = uf_add(title ? UfK_Title : UfK_Button, gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height);
    if (it == NULL)
        return true;
    uf_set_text(it, 0, text);
    const TbBool enabled = ((gbtn->flags & LbBtnF_Enabled) != 0);
    /* (under the mouse: the button's own flag; the frontend's number too, which only some of its screens give their
       buttons - the Endless screens' did not light: the user, 2026-11-26) */
    const TbBool hot = enabled && (((gbtn->flags & LbBtnF_MouseOver) != 0) ||
        ((gbtn->content.lval > 0) && (frontend_mouse_over_button == gbtn->content.lval)));
    it->flags = (enabled ? UfF_Enabled : 0) | (hot ? UfF_Hot : 0) |
        ((hot && ((gbtn->button_state_left_pressed != 0) || (gbtn->button_state_right_pressed != 0))) ? UfF_Down : 0);
    return true;
}

TbBool uf_text_at(int win_x, int win_y, int win_w, int win_h, int posx, int posy, int line_h, const char *text)
{
    if (!uf_armed)
        return false;
    if ((text == NULL) || (text[0] == '\0'))
        return true;
    struct UfItem *it = uf_add(UfK_Text, win_x + posx, win_y + posy, win_w - max(posx, 0), max(line_h, 1));
    if (it == NULL)
        return true;
    uf_set_text(it, 0, text);
    int font = 1;
    for (int k = 0; k < 4; k++)
        if ((frontend_font[k] != NULL) && (lbFontPtr == frontend_font[k]))
            font = k;
    it->font = (unsigned char)font;
    const unsigned long fl = RendererGetDrawFlags();
    it->align = (fl & Lb_TEXT_HALIGN_CENTER) ? 1 : ((fl & Lb_TEXT_HALIGN_RIGHT) ? 2 : 0);
    it->b = max(line_h, win_h - max(posy, 0)); /* (room for more lines: long words wrap) */
    /* a left-aligned line under the mouse is a list's row: lit as one */
    it->flags = ((font == 2) ? UfF_Hot : 0) | ((font != 3) ? UfF_Enabled : 0) | (((font == 2) && (it->align == 0)) ? UfF_Row : 0);
    return true;
}

TbBool uf_well(int x, int y, int w, int h)
{
    if (!uf_armed)
        return false;
    uf_add(UfK_Well, x, y, w, h);
    return true;
}

TbBool uf_tab(int x, int y, int w, int h)
{
    if (!uf_armed)
        return false;
    uf_add(UfK_Tab, x, y, w, h);
    return true;
}

TbBool uf_arrow(const struct GuiButton *gbtn, TbBool up)
{
    if (!uf_armed || (gbtn == NULL))
        return false;
    struct UfItem *it = uf_add(UfK_Arrow, gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height);
    if (it == NULL)
        return true;
    const TbBool enabled = ((gbtn->flags & LbBtnF_Enabled) != 0);
    const TbBool hot = enabled && (((gbtn->flags & LbBtnF_MouseOver) != 0) ||
        ((gbtn->content.lval != 0) && (frontend_mouse_over_button == gbtn->content.lval)));
    it->flags = (enabled ? UfF_Enabled : 0) | (hot ? UfF_Hot : 0) | (up ? UfF_Up : 0) |
        ((hot && (gbtn->button_state_left_pressed != 0)) ? UfF_Down : 0);
    return true;
}

TbBool uf_slider(const struct GuiButton *gbtn, int value256)
{
    if (!uf_armed || (gbtn == NULL))
        return false;
    struct UfItem *it = uf_add(UfK_Slider, gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height);
    if (it == NULL)
        return true;
    it->a = max(0, min(value256, 255));
    it->flags = (((gbtn->flags & LbBtnF_Enabled) != 0) ? UfF_Enabled : 0) | ((gbtn->button_state_left_pressed != 0) ? UfF_Down : 0);
    /* a volume's name above it (the icons alone do not say which is which: the user, 2026-11-26) */
    const int str = (gbtn->id_num == BID_SOUND_VOL) ? GUIStr_OptionSoundFx : (gbtn->id_num == BID_MUSIC_VOL) ? GUIStr_OptionMusic :
        (gbtn->id_num == BID_MENTOR_VOL) ? GUIStr_OptionVoice : 0;
    if (str != 0)
        uf_set_text(it, 0, get_string(str));
    return true;
}

TbBool uf_thumb(int x, int y, int w, int h, int thumb_y, int thumb_h, TbBool hot)
{
    if (!uf_armed)
        return false;
    struct UfItem *it = uf_add(UfK_Thumb, x, y, w, h);
    if (it == NULL)
        return true;
    it->a = thumb_y;
    it->b = thumb_h;
    it->flags = UfF_Enabled | (hot ? UfF_Hot : 0);
    return true;
}

TbBool uf_input(const struct GuiButton *gbtn, const char *text, TbBool active)
{
    if (!uf_armed || (gbtn == NULL))
        return false;
    struct UfItem *it = uf_add(UfK_Input, gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height);
    if (it == NULL)
        return true;
    uf_set_text(it, 0, text);
    it->flags = UfF_Enabled | (active ? UfF_Active : 0) | ((active && ((LbTimerClock() / 400) & 1)) ? UfF_Down : 0);
    return true;
}

TbBool uf_sprite(int x, int y, int w, int h, const struct TbSprite *spr)
{
    if (!uf_armed)
        return false;
    if ((spr == NULL) || (spr->SWidth == 0) || (spr->SHeight == 0))
        return true;
    struct UfItem *it = uf_add(UfK_Sprite, x, y, w, h);
    if (it != NULL)
    {
        it->spr = spr;
        it->pal = (uf_cur == &uf_rec_win) ? engine_palette : frontend_palette;
    }
    return true;
}

/******************************************************************************/
/* an in-game window's recorder */

void uf_window_begin(void)
{
    uf_cur = &uf_rec_win;
    uf_rec_win.n = 0;
    uf_rec_win.armed = true;
}

void uf_window_stop(void)
{
    uf_rec_win.armed = false;
    uf_cur = &uf_rec_front;
}

/** a palette colour of the window's drawing in true colour: its inks become the Molten ones */
static uint32_t uf_ink_of(unsigned char colour)
{
    if (colour == world_window_ink(WwI_Gold))
        return UM_GOLD_HI;
    if (colour == world_window_ink(WwI_Dim))
        return UM_DIM;
    if (colour == world_window_ink(WwI_Bright))
        return UM_TEXT;
    if (colour == world_window_ink(WwI_Rule))
        return UIC(110, 84, 52, 255);
    if (engine_palette == NULL)
        return UM_TEXT;
    const unsigned char *p = engine_palette + 3 * colour;
    return UIC(p[0] * 4 + p[0] / 16, p[1] * 4 + p[1] / 16, p[2] * 4 + p[2] / 16, 255);
}

TbBool uf_plain(int x, int y, int w, int cap, int align, unsigned char colour, const char *text)
{
    if (!uf_armed || (uf_cur != &uf_rec_win))
        return false;
    struct UfItem *it = uf_add(UfK_Plain, x, y, w, cap);
    if (it == NULL)
        return true;
    uf_set_text(it, 0, text);
    it->align = (unsigned char)align;
    it->col = uf_ink_of(colour);
    return true;
}

TbBool uf_heading(int x, int y, int w, int cap, int align, const char *text)
{
    if (!uf_armed || (uf_cur != &uf_rec_win))
        return false;
    struct UfItem *it = uf_add(UfK_Heading, x, y, w, cap);
    if (it == NULL)
        return true;
    uf_set_text(it, 0, text);
    it->align = (unsigned char)align;
    return true;
}

TbBool uf_card(int x, int y, int w, int h, TbBool hot)
{
    if (!uf_armed || (uf_cur != &uf_rec_win))
        return false;
    struct UfItem *it = uf_add(UfK_Card, x, y, w, h);
    if (it != NULL)
        it->flags = UfF_Enabled | (hot ? UfF_Hot : 0);
    return true;
}

TbBool uf_frame(int x, int y, int w, int h, TbBool marked, TbBool hot)
{
    if (!uf_armed || (uf_cur != &uf_rec_win))
        return false;
    struct UfItem *it = uf_add(UfK_Frame, x, y, w, h);
    if (it != NULL)
        it->flags = UfF_Enabled | (hot ? UfF_Hot : 0) | (marked ? UfF_Active : 0);
    return true;
}

TbBool uf_box(int x, int y, int w, int h, unsigned char colour)
{
    if (!uf_armed)
        return false;
    struct UfItem *it = uf_add(UfK_Box, x, y, w, h);
    if (it != NULL)
        it->col = uf_ink_of(colour);
    return true;
}

TbBool uf_arrow_at(int x, int y, int w, int h, TbBool up, TbBool enabled, TbBool hot)
{
    if (!uf_armed)
        return false;
    struct UfItem *it = uf_add(UfK_Arrow, x, y, w, h);
    if (it != NULL)
        it->flags = (enabled ? UfF_Enabled : 0) | ((enabled && hot) ? UfF_Hot : 0) | (up ? UfF_Up : 0);
    return true;
}

/******************************************************************************/
/* painting */

/** A frontend sprite in true colour (the frontend's palette), kept for the next paintings. */
#define UF_SPR_CACHE 48
static const struct TbSprite *uf_spr_key[UF_SPR_CACHE];
static const unsigned char *uf_spr_pal[UF_SPR_CACHE];
static unsigned char *uf_spr_px[UF_SPR_CACHE];
static int uf_spr_next;

static const unsigned char *uf_sprite_px(const struct TbSprite *spr, const unsigned char *pal)
{
    if (pal == NULL)
        return NULL;
    for (int i = 0; i < UF_SPR_CACHE; i++)
        if ((uf_spr_key[i] == spr) && (uf_spr_pal[i] == pal))
            return uf_spr_px[i];
    unsigned char *px = (unsigned char *)calloc((size_t)spr->SWidth * spr->SHeight, 4);
    if (px == NULL)
        return NULL;
    ui_sprite_rgba(spr, pal, px);
    const int k = uf_spr_next;
    uf_spr_next = (uf_spr_next + 1) % UF_SPR_CACHE;
    free(uf_spr_px[k]);
    uf_spr_px[k] = px;
    uf_spr_key[k] = spr;
    uf_spr_pal[k] = pal;
    return px;
}

const unsigned char *uf_sprite_true(const struct TbSprite *spr)
{
    return ((spr == NULL) || (spr->SWidth == 0) || (spr->SHeight == 0)) ? NULL : uf_sprite_px(spr, engine_palette);
}

static UiColour uf_ink(const struct UfItem *it)
{
    if ((it->flags & UfF_Enabled) == 0)
        return UM_DIM;
    if (it->flags & UfF_Hot)
        return UM_GOLD_HI;
    return (it->flags & UfF_Row) ? UM_TEXT : UM_SOFT;
}

/** a row lit under the mouse, as a list's chosen line */
static void uf_row_light(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, const struct UfItem *it)
{
    if (!(it->flags & UfF_Row) || !(it->flags & UfF_Hot))
        return;
    const float r = st->flat ? 0.0f : min((y1 - y0) * 0.5f, 8.0f * st->u);
    if (st->flat)
    {
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, UIC(110, 30, 14, 255), UIC(110, 30, 14, 255), false);
        return;
    }
    ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(190, 54, 20, 120), UIC(120, 26, 12, 120), false);
    ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, 0.8f * st->u), UIC(255, 200, 130, 150));
}

static void uf_arrow_paint(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, const struct UfItem *it)
{
    const int state = !(it->flags & UfF_Enabled) ? UmS_Off : ((it->flags & UfF_Down) ? UmS_Down : ((it->flags & UfF_Hot) ? UmS_Hot : UmS_Normal));
    um_button(c, st, x0, y0, x1, y1, UmB_Normal, state, 1.0f, "");
    /* the chevron */
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float hw = min(x1 - x0, (y1 - y0) * 1.6f) * 0.24f, hh = hw * 0.62f;
    const float s = (it->flags & UfF_Up) ? -1.0f : 1.0f;
    const float xy[6] = {cx - hw, cy - s * hh, cx + hw, cy - s * hh, cx, cy + s * hh};
    const int counts[1] = {3};
    ui_fill_path(c, xy, counts, 1, (state == UmS_Off) ? UM_DIM : ((state == UmS_Normal) ? UM_GOLD : UM_GOLD_HI));
}

static void uf_slider_paint(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, const struct UfItem *it)
{
    const float h = y1 - y0;
    const float cy = (y0 + y1) * 0.5f;
    const float kr = h * 0.42f;                         /* the knob's radius */
    const float tx0 = x0 + kr, tx1 = x1 - kr;
    const float th = max(3.0f, h * 0.26f);
    const float frac = (float)it->a / 255.0f;
    const float kx = tx0 + (tx1 - tx0) * frac;
    um_well(c, st, tx0 - th * 0.5f, cy - th * 0.5f, tx1 + th * 0.5f, cy + th * 0.5f, th * 0.5f);
    if (it->text[0][0] != '\0')
        ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, h * 0.62f, 0.0f, tx1 - tx0, it->text[0]), tx0, y0 - h * 0.78f, tx1 - tx0, h * 0.7f, 0,
            UM_SOFT, 0.0f, !st->flat, it->text[0]);
    if (st->flat)
    {
        ui_fill_rrect(c, tx0, cy - th * 0.5f, kx, cy + th * 0.5f, 0.0f, UIC(200, 80, 24, 255), UIC(200, 80, 24, 255), false);
        ui_fill_rrect(c, kx - kr, cy - kr, kx + kr, cy + kr, 0.0f, UM_GOLD, UM_GOLD, false);
        return;
    }
    ui_fill_rrect(c, tx0 - th * 0.5f, cy - th * 0.5f, kx, cy + th * 0.5f, th * 0.5f, UIC(255, 150, 60, 255), UIC(190, 50, 16, 255), false);
    ui_glow_rrect(c, kx - kr, cy - kr, kx + kr, cy + kr, kr, 6.0f * st->u, UIC(255, 120, 40, (it->flags & UfF_Down) ? 120 : 60));
    ui_fill_rrect(c, kx - kr, cy - kr, kx + kr, cy + kr, kr, UIC(255, 226, 160, 255), UIC(196, 140, 60, 255), false);
    ui_stroke_rrect(c, kx - kr, cy - kr, kx + kr, cy + kr, kr, max(1.0f, 0.8f * st->u), UIC(255, 240, 200, 200));
}

static void uf_thumb_paint(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, const struct UfItem *it)
{
    const float w = x1 - x0;
    const float tw = max(4.0f, w * 0.34f);
    const float cx = (x0 + x1) * 0.5f;
    um_well(c, st, cx - tw * 0.5f, y0, cx + tw * 0.5f, y1, tw * 0.5f);
    const float ty0 = max(y0, (float)it->a), ty1 = min(y1, (float)(it->a + it->b));
    if (ty1 <= ty0)
        return;
    const float kw = max(6.0f, w * 0.56f);
    const float r = st->flat ? 0.0f : kw * 0.5f;
    ui_fill_rrect(c, cx - kw * 0.5f, ty0, cx + kw * 0.5f, ty1, r, (it->flags & UfF_Hot) ? UIC(255, 210, 130, 255) : UIC(222, 170, 84, 255),
        UIC(150, 100, 40, 255), false);
    if (!st->flat)
        ui_stroke_rrect(c, cx - kw * 0.5f, ty0, cx + kw * 0.5f, ty1, r, max(1.0f, 0.7f * st->u), UIC(255, 236, 190, 160));
}

static void uf_paint_item(struct UiCanvas *c, const struct UmStyle *st, const struct UfItem *it, const struct UfItem *prev, int ox, int oy)
{
    const float u = st->u;
    const float x0 = (float)(it->x - ox), y0 = (float)(it->y - oy), x1 = x0 + (float)it->w, y1 = y0 + (float)it->h;
    switch (it->kind)
    {
    case UfK_Title:
        ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, (float)it->h * 0.62f, 0.6f * u, x1 - x0, it->text[0]), x0, y0, x1 - x0, y1 - y0, 1,
            UM_BRIGHT, 0.6f * u, !st->flat, it->text[0]);
        um_rule(c, st, x0 + 10.0f * u, y1 + 2.0f * u, x1 - 10.0f * u);
        break;
    case UfK_Button:
    {
        const float inset = (float)it->h * 0.08f;
        const int state = !(it->flags & UfF_Enabled) ? UmS_Off : ((it->flags & UfF_Down) ? UmS_Down : ((it->flags & UfF_Hot) ? UmS_Hot : UmS_Normal));
        um_button(c, st, x0, y0 + inset, x1, y1 - inset, UmB_Normal, state, (float)it->h * 0.42f, it->text[0]);
        break;
    }
    case UfK_Well:
        um_well(c, st, x0, y0, x1, y1, 10.0f * u);
        break;
    case UfK_Tab:
        if (!st->flat)
            ui_fill_rrect(c, x0, y0, x1, y1, 8.0f * u, UIC(255, 214, 150, 18), UIC(255, 214, 150, 4), false);
        um_rule(c, st, x0 + 4.0f * u, y1 - 1.0f, x1 + (x1 - x0) * 0.8f);
        break;
    case UfK_Text:
    {
        if (it->flags & UfF_OnTab)
        {
            /* a section's name on its tab (the game draws it in the font of a line under the mouse): a heading */
            const float px = ui_text_fit(UiF_Head, (float)it->h * 0.70f, 0.3f * u, x1 - x0, it->text[0]);
            ui_text_box(c, UiF_Head, px, x0, y0, x1 - x0, y1 - y0, it->align, UM_GOLD, 0.3f * u, !st->flat, it->text[0]);
            break;
        }
        /* (a row lit once: two words on one row, as a key's name and its key, light it together) */
        if (!((prev != NULL) && (prev->kind == UfK_Text) && (prev->y == it->y) && (prev->h == it->h) && (prev->flags & UfF_Row)))
            uf_row_light(c, st, x0 - 6.0f * u, y0, x1 + 4.0f * u, y1, it);
        const float base = (float)it->h * 0.70f;
        const float w = x1 - x0;
        if ((ui_text_width(UiF_Body, base, 0.0f, it->text[0]) <= w) || (it->b < 2 * it->h))
        {
            const float px = ui_text_fit(UiF_Body, base, 0.0f, w, it->text[0]);
            ui_text_box(c, UiF_Body, px, x0, y0, w, y1 - y0, it->align, uf_ink(it), 0.0f, !st->flat, it->text[0]);
            break;
        }
        /* longer than its line, with room below: wrapped by words */
        const char *s0 = it->text[0];
        float ly = y0;
        while ((*s0 != '\0') && (ly + (float)it->h <= y0 + (float)it->b + 0.5f))
        {
            char line[96];
            int best = 0, k = 0;
            for (;;)
            {
                while ((s0[k] != '\0') && (s0[k] != ' '))
                    k++;
                if (k >= (int)sizeof(line))
                    break;
                memcpy(line, s0, (size_t)k);
                line[k] = '\0';
                if ((best > 0) && (ui_text_width(UiF_Body, base, 0.0f, line) > w))
                    break;
                best = k;
                if (s0[k] == '\0')
                    break;
                k++;
            }
            if (best <= 0)
                best = (int)min(strlen(s0), sizeof(line) - 1);
            memcpy(line, s0, (size_t)best);
            line[best] = '\0';
            ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, base, 0.0f, w, line), x0, ly, w, (float)it->h, it->align, uf_ink(it), 0.0f,
                !st->flat, line);
            s0 += best;
            while (*s0 == ' ')
                s0++;
            ly += (float)it->h;
        }
        break;
    }
    case UfK_Arrow:
        uf_arrow_paint(c, st, x0, y0, x1, y1, it);
        break;
    case UfK_Slider:
        uf_slider_paint(c, st, x0, y0, x1, y1, it);
        break;
    case UfK_Thumb:
    {
        struct UfItem t = *it;
        t.a -= oy;
        uf_thumb_paint(c, st, x0, y0, x1, y1, &t);
        break;
    }
    case UfK_Input:
    {
        um_well(c, st, x0, y0, x1, y1, 6.0f * u);
        if ((it->flags & UfF_Active) && !st->flat)
            ui_stroke_rrect(c, x0, y0, x1, y1, 6.0f * u, max(1.0f, 0.9f * u), UIC(255, 190, 110, 200));
        char line[112];
        snprintf(line, sizeof(line), "%s%s", it->text[0], (it->flags & UfF_Down) ? "_" : "");
        const float pad = 8.0f * u;
        const float px = ui_text_fit(UiF_Body, (float)it->h * 0.62f, 0.0f, x1 - x0 - 2.0f * pad, line);
        ui_text_box(c, UiF_Body, px, x0 + pad, y0, x1 - x0 - 2.0f * pad, y1 - y0, 0, (it->flags & UfF_Active) ? UM_BRIGHT : UM_TEXT, 0.0f,
            !st->flat, line);
        break;
    }
    case UfK_Sprite:
    {
        const unsigned char *px = uf_sprite_px(it->spr, it->pal);
        if (px == NULL)
            break;
        /* the game's own picture at its own shape, fitted in its place */
        const float k = min((x1 - x0) / (float)it->spr->SWidth, (y1 - y0) / (float)it->spr->SHeight);
        const int dw = max(1, (int)((float)it->spr->SWidth * k)), dh = max(1, (int)((float)it->spr->SHeight * k));
        ui_image_gain(c, (int)floorf((x0 + x1 - (float)dw) * 0.5f), (int)floorf((y0 + y1 - (float)dh) * 0.5f), dw, dh, px, it->spr->SWidth,
            it->spr->SHeight, 1.0f);
        break;
    }
    case UfK_Plain:
    {
        const float px = ui_text_fit(UiF_Body, (float)it->h / 0.70f, 0.0f, x1 - x0, it->text[0]);
        ui_text_box(c, UiF_Body, px, x0, y0, x1 - x0, y1 - y0, it->align, (UiColour)it->col, 0.0f, !st->flat, it->text[0]);
        break;
    }
    case UfK_Heading:
    {
        const float px = ui_text_fit(UiF_Head, (float)it->h / 0.68f, 0.5f * u, x1 - x0, it->text[0]);
        ui_text_box(c, UiF_Head, px, x0, y0, x1 - x0, y1 - y0, it->align, UM_GOLD, 0.5f * u, !st->flat, it->text[0]);
        break;
    }
    case UfK_Card:
        um_button(c, st, x0, y0, x1, y1, UmB_Normal, (it->flags & UfF_Hot) ? UmS_Hot : UmS_Normal, 1.0f, "");
        break;
    case UfK_Frame:
    {
        const float r = st->flat ? 0.0f : min((y1 - y0) * 0.3f, 8.0f * u);
        if (it->flags & UfF_Active)
        {
            if (!st->flat)
                ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(200, 40, 20, 70), UIC(120, 20, 10, 70), false);
            ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, 1.2f * u), UIC(230, 70, 40, 230));
        } else
            ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, 1.0f * u), UIC(255, 214, 140, 200));
        break;
    }
    case UfK_Box:
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, (UiColour)it->col, (UiColour)it->col, false);
        break;
    default:
        break;
    }
}

/** What the n pieces recorded show, as a hash (the window is painted again when it changes). */
static uint32_t uf_key_hash(int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
    {
        const struct UfItem *it = &uf_items[i];
        const unsigned char *p = (const unsigned char *)it;
        /* (the struct up to the texts, then each text as far as it goes) */
        const size_t head = offsetof(struct UfItem, text);
        for (size_t k = 0; k < head; k++)
            h = (h ^ p[k]) * 16777619u;
        for (int t = 0; t < UF_COLS; t++)
            for (const char *s = it->text[t]; *s != '\0'; s++)
                h = (h ^ (unsigned char)*s) * 16777619u;
    }
    return h;
}

uint32_t uf_window_hash(void)
{
    struct UfRec *keep = uf_cur;
    uf_cur = &uf_rec_win;
    const uint32_t h = uf_key_hash(uf_n) ^ (uint32_t)uf_n;
    uf_cur = keep;
    return h;
}

void uf_window_paint(struct UiCanvas *c, const struct UmStyle *st, int ox, int oy)
{
    struct UfRec *keep = uf_cur;
    uf_cur = &uf_rec_win;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < uf_n; i++)
        {
            const TbBool under = (uf_items[i].kind == UfK_Well) || (uf_items[i].kind == UfK_Tab) || (uf_items[i].kind == UfK_Card);
            if (under == (pass == 0))
                uf_paint_item(c, st, &uf_items[i], (i > 0) ? &uf_items[i - 1] : NULL, ox, oy);
        }
    uf_cur = keep;
}

void uf_flush(void)
{
    uf_cur = &uf_rec_front;
    const int n = uf_n;
    const TbBool armed = uf_armed;
    uf_n = 0;
    uf_armed = false;
    if (!armed || (n == 0))
        return;
    /* words lying on a section tab are its name */
    for (int i = 0; i < n; i++)
    {
        struct UfItem *t = &uf_items[i];
        if (t->kind != UfK_Text)
            continue;
        const int cy = t->y + t->h / 2;
        for (int k = 0; k < n; k++)
        {
            const struct UfItem *b = &uf_items[k];
            if ((b->kind == UfK_Tab) && (cy >= b->y) && (cy < b->y + b->h) && (t->x >= b->x - 4) && (t->x < b->x + b->w))
            {
                t->flags = (unsigned char)((t->flags | UfF_OnTab) & ~(UfF_Row | UfF_Hot));
                break;
            }
        }
    }
    const TbBool flat = um_host_flat();
    const float u = (float)max(8, (int)units_per_pixel_menu) / 24.0f; /* (as the main menu: a panel unit is 2/3 of a menu unit) */
    const struct UmStyle st = {u, flat};
    int bx0 = uf_items[0].x, by0 = uf_items[0].y, bx1 = bx0 + uf_items[0].w, by1 = by0 + uf_items[0].h;
    for (int i = 1; i < n; i++)
    {
        bx0 = min(bx0, uf_items[i].x);
        by0 = min(by0, uf_items[i].y);
        bx1 = max(bx1, uf_items[i].x + uf_items[i].w);
        by1 = max(by1, uf_items[i].y + uf_items[i].h);
    }
    const int m = flat ? 0 : (int)ceilf(um_window_margin(&st) * 0.6f);
    const int pad = max(2, min((int)ceilf(22.0f * u), min(min(by0, bx0), min((int)MyScreenHeight - by1, (int)MyScreenWidth - bx1)) - 2));
    const int x0 = max(0, bx0 - pad - m), y0 = max(0, by0 - pad - m);
    const int cw = min((int)MyScreenWidth, bx1 + pad + m) - x0, ch = min((int)MyScreenHeight, by1 + pad + m) - y0;
    if ((cw <= 0) || (ch <= 0))
        return;
    char key[96];
    snprintf(key, sizeof(key), "%d %d %d %d %.4f %d %d %08x", x0, y0, cw, ch, (double)u, (int)flat, n, (unsigned)uf_key_hash(n)); /* (n: the count was reset above - hashing none, a hover never repainted it) */
    uf_host.name = "frontscreen";
    uf_host.slot = UmSl_Front; /* (the main menu's slot: never on screen together) */
    if (uf_host.gen < 0x40000000u)
        uf_host.gen = 0x40000000u; /* (its own numbers: a renderer keeps a slot's copy while the number stays) */
    if (um_host_begin(&uf_host, cw, ch, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &uf_host.canvas;
        um_window(c, &st, (float)(bx0 - pad - x0), (float)(by0 - pad - y0), (float)(bx1 + pad - x0), (float)(by1 + pad - y0));
        /* the wells and tabs under the rest, then everything as the screen drew it */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++)
            {
                const TbBool under = (uf_items[i].kind == UfK_Well) || (uf_items[i].kind == UfK_Tab);
                if (under == (pass == 0))
                    uf_paint_item(c, &st, &uf_items[i], (i > 0) ? &uf_items[i - 1] : NULL, x0, y0);
            }
        um_host_painted(&uf_host, t0);
    }
    um_host_show(&uf_host, x0, y0);
}

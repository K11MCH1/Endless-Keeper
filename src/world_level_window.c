/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_level_window.c
 *     Endless worlds: the Library's spellbook and the Rooms & Spells To Level window.
 * @par Purpose:
 *     The user, 2026-11-22: "When hovering over the Library with the hand an animated icon of a spellbook will appear
 *     in the middle of the Library. [...] The spellbook flies up from the ground to above the Library in the middle,
 *     during this upwards move from the ground it does a transition effect from transparent to fully visible. In the
 *     middle it just floats and plays its animation until the hand moves away from the library or clicks on it."
 *     "Clicking on the Spellbook opens up a ui window. On the top middle a title: 'Rooms & Spells To Level'. On the left
 *     side in columns all the available rooms their icons. If rooms are not available, it should show the slot with the
 *     question mark. On the Right side, same layout for the Spells. Clicking on an Icon here distributes all the XP to
 *     the relevant room. The icon will also flash just like it does on the main game panel. The current level should be
 *     stated under the icons. When hovering over an icon, a tooltip window should show up explaining what each level
 *     unlocks."
 *
 *     The book is the game's own spellbook (the keeper sprite of the books in the Library, 16 frames), drawn over the
 *     view on the Hand's layer at the Library's middle, at the view's own size. It shows only with an empty Hand and no
 *     window open. A click on it (its picture, not the room: a click on a researcher still picks it up) opens the
 *     window. What the window shows is game state (world_progress.c: the levels, the bars, the choice): a joined player
 *     sees it as the server has it; the choice goes as a packet (PckA_WorldLevelFocus).
 *
 * Test hook: KFX_WORLD_LEVELTEST=turn: at that turn a 3x3 Library is placed beside the Heart and the book shows over it
 *     (as if hovered) for 60 turns; then the window opens; 40 turns on the Library's tooltip shows, 80 on the Training
 *     Room is chosen, 120 on the window closes: screenshots at those turns.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world_level_window.h"
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_guibtns.h"
#include "bflib_mouse.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "kjm_input.h"
#include "config_magic.h"
#include "config_terrain.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "engine_render.h"
#include "frontend.h"
#include "game_legacy.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "keeperfx.hpp"
#include "local_camera.h"
#include "map_data.h"
#include "net_state.h"
#include "observer.h"
#include "packets.h"
#include "player_data.h"
#include "power_hand.h"
#include "room_data.h"
#include "sprites.h"
#include "vidfade.h"
#include "ui_molten.h"
#include "ui_safe_area.h"
#include "front_simple.h"
#include "world_creature_window.h"
#include "world_keeper_panel.h"
#include "world_progress.h"
#include "world_stream.h"
#include "thing_creature.h"
#include "thing_list.h"
#include "creature_jobs.h"
#include "thing_navigate.h"
#include "config_creature.h"
#include "config_keeperfx.h"
#include "engine_lenses.h"
#include "post_inc.h"

/* The window in design units (2026-11-25, the user: "Please do the same to the Library panel": the Keeper panel's
   Molten look, ui_molten.c). Rooms on the left, spells on the right, four cells a row: an icon in a well, its level
   and its bar under it; the one the Library levels now is lit and flashes. (The foot that said so in words, with its
   bar, was left out: the user, "this part can be left out. Its redundant".) lw_place sets the scale. */
#define LW_E 28.0f
#define LW_W 808.0f
#define LW_H 514.0f
#define LW_TOP 70.0f                 /* the rule under the title */
#define LW_ICON 76.0f                /* a cell's well */
#define LW_GAP_X 16.0f
#define LW_ROW 128.0f                /* a cell's height and the room under it */
#define LW_COLS 4
#define LW_COL_W (LW_COLS * LW_ICON + (LW_COLS - 1) * LW_GAP_X)
#define LW_ROOMS_X LW_E
#define LW_SPELLS_X (LW_W - LW_E - LW_COL_W)
#define LW_GRID_Y 118.0f
#define LW_ROOMS 10
#define LW_SPELLS 7
#define LW_SPELL_BASE 100            /* a cell's lval: rooms 0-9, spells 100-106 */
#define LW_BUTTONS (2 + LW_ROOMS + LW_SPELLS)

/* the book */
#define LB_ANIM 777                  /* the Library's spellbooks' keeper sprite (objects.cfg SPELLBOOK_*) */
#define LB_FRAMES 16
#define LB_RISE_MS 900                /* the rise from the floor (2026-11-26: 450 was over before it was seen) */
#define LB_START_SIZE 30             /* its size on the floor, in hundredths of its size on top: a book as it lies */
#define LB_LIFT 640                  /* map units over the floor it floats at */

static float lw_u = 1.0f;            /* pixels per design unit */
static int lw_px, lw_py, lw_pw, lw_ph, lw_margin;
static struct UmHost lw_host, lw_tip_host;
static struct GuiButton *lw_btn[LW_BUTTONS];
static int lw_hover = -1;            /* the cell the mouse is on (its lval), -1 none */
static int lw_test_hover = -1;       /* the test hook's */
/* icons as true-colour pictures, made once a sprite */
/* the book: the Library it shows over, since when, and where it was drawn last (for the click) */
static int lb_room = 0;
static TbClockMSec lb_since;
static TbBool lb_shown;
static int lb_x1, lb_y1, lb_x2, lb_y2;
static TbBool lb_pressed;
static long lw_test_at = -2;
static int lw_test_room = 0;
static int lw_test_room_kept = 0;

/******************************************************************************/
TbBool world_level_window_open(void)
{
    return world_mode_active() && !netstate_dedicated() && menu_is_active(GMnu_LEVEL_WINDOW);
}

/** A button's lval (the ground and Close: -1). */
static int lw_lval_of(int n)
{
    if ((n >= 2) && (n < 2 + LW_ROOMS))
        return n - 2;
    if ((n >= 2 + LW_ROOMS) && (n < LW_BUTTONS))
        return LW_SPELL_BASE + (n - 2 - LW_ROOMS);
    return -1;
}

/** Where button n is, in design units: x, y, w, h. */
static void lw_rect(int n, float *r)
{
    r[0] = r[1] = r[2] = r[3] = 0.0f;
    if (n == 0)
    {
        r[2] = LW_W;
        r[3] = LW_H;
        return;
    }
    if (n == 1)
    {
        r[0] = LW_W - LW_E - 110.0f;
        r[1] = 18.0f;
        r[2] = 110.0f;
        r[3] = 38.0f;
        return;
    }
    const int lval = lw_lval_of(n);
    if (lval < 0)
        return;
    const int k = (lval >= LW_SPELL_BASE) ? lval - LW_SPELL_BASE : lval;
    const float x0 = (lval >= LW_SPELL_BASE) ? LW_SPELLS_X : LW_ROOMS_X;
    r[0] = x0 + (LW_ICON + LW_GAP_X) * (float)(k % LW_COLS);
    r[1] = LW_GRID_Y + LW_ROW * (float)(k / LW_COLS);
    r[2] = LW_ICON;
    r[3] = LW_ICON + 30.0f; /* (the icon and its level) */
}

/** The window's scale and place: in the middle of the safe area, as large as fits, on a computer no larger than the
 *  screen's height / 720 a unit (as the Keeper panel). */
static void lw_place(struct GuiMenu *gmnu)
{
    const int sx0 = ui_safe_left, sy0 = ui_safe_top;
    const int sw = max(64, ui_safe_right_edge() - ui_safe_left), sh = max(64, ui_safe_bottom_edge() - ui_safe_top);
    float u = min((float)sw * 0.92f / LW_W, (float)sh * 0.92f / LW_H);
#ifndef __ANDROID__
    u = min(u, (float)MyScreenHeight / 720.0f * (float)ui_scale_percent / 100.0f); /* (UI_SCALE) */
#endif
    lw_u = max(0.72f, u);
    lw_pw = (int)(LW_W * lw_u + 0.5f);
    lw_ph = (int)(LW_H * lw_u + 0.5f);
    const struct UmStyle st = {lw_u, um_host_flat()};
    lw_margin = (int)um_window_margin(&st);
    lw_px = max(0, sx0 + (sw - lw_pw) / 2);
    lw_py = max(0, sy0 + (sh - lw_ph) / 2);
    gmnu->pos_x = lw_px;
    gmnu->pos_y = lw_py;
    gmnu->width = lw_pw;
    gmnu->height = lw_ph;
    int n = 0;
    memset(lw_btn, 0, sizeof(lw_btn));
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (gmnu->buttons != NULL) && (n < LW_BUTTONS); i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != gmnu->number))
            continue;
        if (gmnu->buttons[n].gbtype == -1)
            break;
        float r[4];
        lw_rect(n, r);
        lw_btn[n] = gbtn;
        n++;
        gbtn->scr_pos_x = lw_px + (int)(r[0] * lw_u + 0.5f);
        gbtn->scr_pos_y = lw_py + (int)(r[1] * lw_u + 0.5f);
        gbtn->pos_x = gbtn->scr_pos_x;
        gbtn->pos_y = gbtn->scr_pos_y;
        gbtn->width = (int)(r[2] * lw_u + 0.5f);
        gbtn->height = (int)(r[3] * lw_u + 0.5f);
    }
}

static void lwc_place(struct GuiMenu *gmnu);

void world_level_window_show(void)
{
    if (!world_mode_active() || netstate_dedicated())
        return;
    if (menu_is_active(GMnu_CREATURE_WINDOW))
        turn_off_menu(GMnu_CREATURE_WINDOW); /* one window in the middle of the view at a time */
    if (world_keeper_panel_open())
        world_keeper_panel_close();
    lb_shown = false;
    if (!menu_is_active(GMnu_LEVEL_WINDOW))
    {
        turn_on_menu(GMnu_LEVEL_WINDOW);
        const MenuNumber num = menu_id_to_number(GMnu_LEVEL_WINDOW);
        if (num >= 0)
        {
            if (window_style == WinStyle_Classic)
                lwc_place(get_active_menu(num));
            else
                lw_place(get_active_menu(num));
        }
    }
    SYNCLOG("World: the Rooms & Spells To Level window opens");
}

void world_level_window_close(void)
{
    if (menu_is_active(GMnu_LEVEL_WINDOW))
        turn_off_menu(GMnu_LEVEL_WINDOW);
    lw_hover = -1;
}

/******************************************************************************/
/* The window */

/** The cell's entry this frame (false: none there). */
static TbBool lw_entry_of(int lval, struct WorldLevelEntry *out)
{
    static struct WorldLevelEntry rooms[LW_ROOMS], spells[LW_SPELLS];
    /* (no more than the arrays hold: gcc checks a constant lval against them) */
    const int nr = min(world_level_entries(my_player_number, 0, rooms, LW_ROOMS), LW_ROOMS);
    const int ns = min(world_level_entries(my_player_number, 1, spells, LW_SPELLS), LW_SPELLS);
    if ((lval >= 0) && (lval < nr))
    {
        *out = rooms[lval];
        return true;
    }
    if ((lval >= LW_SPELL_BASE) && (lval - LW_SPELL_BASE < ns))
    {
        *out = spells[lval - LW_SPELL_BASE];
        return true;
    }
    return false;
}

/** An entry's icon (the panel's middle-sized one) as a true-colour picture (NULL: none): its shadow off, cropped. */
static const unsigned char *lw_icon(const struct WorldLevelEntry *e, int *w, int *h)
{
    const long spr_idx = (e->kind_class == 0) ? get_room_kind_stats(e->kind)->medsym_sprite_idx :
        get_power_model_stats(e->kind)->medsym_sprite_idx;
    return um_icon(spr_idx, w, h);
}

static TbBool lw_hot(int n)
{
    if ((n < 0) || (n >= LW_BUTTONS) || (lw_btn[n] == NULL) || (lw_btn[n]->width <= 0))
        return false;
    const int lval = lw_lval_of(n);
    if ((lw_test_hover >= 0) && (lval == lw_test_hover))
        return true;
    return ((lw_btn[n]->flags & LbBtnF_MouseOver) != 0);
}

/** The flash of the chosen icon: on and off twice a second (the main panel's flashes every gui_blink_rate turns, by
 *  default each turn; the window is painted anew at each change, so it flashes more calmly). */
static TbBool lw_blink(void)
{
    const unsigned long half = (unsigned long)max(turns_per_second / 4, 1);
    return ((get_gameturn() / half) % 2) != 0;
}

/** What the foot says: where the Library's research goes (false: nowhere). */
static TbBool lw_focus_entry(struct WorldLevelEntry *e)
{
    for (int k = 0; k < LW_ROOMS; k++)
        if (lw_entry_of(k, e) && e->focus)
            return true;
    for (int k = 0; k < LW_SPELLS; k++)
        if (lw_entry_of(LW_SPELL_BASE + k, e) && e->focus)
            return true;
    return false;
}

/** The cell under the mouse (its lval), for the tooltip. */
static void lw_update_hover(void)
{
    lw_hover = -1;
    for (int n = 2; n < LW_BUTTONS; n++)
        if ((lw_btn[n] != NULL) && ((lw_btn[n]->flags & LbBtnF_MouseOver) != 0))
            lw_hover = lw_lval_of(n);
}

/** The window's live parts this frame: Close, each cell (its icon, frame, level and bar; the flash only in the chosen
 *  one), the line for a keeper with nothing to level. Only those that changed are painted again. */
static int lw_parts(struct UmPart *pt)
{
    int n = 0;
    const float u = lw_u, m = (float)lw_margin;
    const TbBool blink = lw_blink();
    float r[4];
    lw_rect(1, r);
    um_part(pt, &n, u, m, r[0], r[1], r[2], r[3], 16.0f,
        um_hash("close %d %d", (int)lw_hot(1), (int)(lw_hot(1) && (lbDisplay.MLeftButton != 0))));
    TbBool any = false;
    for (int b = 2; b < LW_BUTTONS; b++)
    {
        struct WorldLevelEntry e = {0};
        const TbBool has = lw_entry_of(lw_lval_of(b), &e);
        any = any || has;
        lw_rect(b, r);
        const long q = (e.need > 0) ? (long)((double)max(0L, min(e.bar, e.need)) * 400.0 / (double)e.need) : -1L;
        um_part(pt, &n, u, m, r[0], r[1], LW_ICON, LW_ICON + 36.0f, 20.0f, um_hash("cell %d %d %d %d %d %d %ld %d", b, (int)lw_hot(b),
            (int)has, (int)e.owned, (int)e.focus, e.level, q, (int)(e.focus && blink)));
    }
    um_part(pt, &n, u, m, LW_E, LW_GRID_Y + 120.0f, LW_W - 2 * LW_E, 36.0f, 2.0f, um_hash("none %d", (int)any));
    return n;
}

/** One layer: base (the glass, the title, headings, rules, the cells' wells) or the live part over it. */
static void lw_paint(TbBool base)
{
    struct UiCanvas *c = base ? &lw_host.base : &lw_host.canvas;
    const struct UmStyle stv = {lw_u, um_host_flat()};
    const struct UmStyle *st = &stv;
    const float u = lw_u, m = (float)lw_margin;
#define X(v) (m + (v) * u)
#define Y(v) (m + (v) * u)
#define S(v) ((v) * u)
    if (base)
    {
        ui_text_box(c, UiF_Head, S(26.0f), X(LW_E), Y(18.0f), S(LW_W - 2 * LW_E - 130.0f), S(38.0f), 0, UM_BRIGHT, 0.0f, !st->flat,
        "Rooms & Spells To Level");
        um_rule(c, st, X(LW_E), Y(LW_TOP), X(LW_W - LW_E));
        um_heading(c, st, X(LW_ROOMS_X), Y(LW_TOP + 14.0f), S(22.0f), S(16.0f), "Rooms");
        um_heading(c, st, X(LW_SPELLS_X), Y(LW_TOP + 14.0f), S(22.0f), S(16.0f), "Spells");
        /* the line between the two sides */
        const float divx = (LW_ROOMS_X + LW_COL_W + LW_SPELLS_X) * 0.5f;
        const float t = max(1.0f, floorf(S(0.75f) + 0.5f));
        ui_fill_rrect(c, X(divx), Y(LW_TOP + 16.0f), X(divx) + t, Y(LW_H - LW_E), 0.0f,
            st->flat ? UIC(64, 46, 26, 255) : UIC(255, 214, 170, 0), st->flat ? UIC(64, 46, 26, 255) : UIC(255, 214, 170, 50), false);
        for (int n = 2; n < LW_BUTTONS; n++)
        {
            struct WorldLevelEntry e = {0};
            if (!lw_entry_of(lw_lval_of(n), &e))
                continue;
            float r[4];
            lw_rect(n, r);
            um_well(c, st, X(r[0]), Y(r[1]), X(r[0] + LW_ICON), Y(r[1] + LW_ICON), S(12.0f));
        }
        return;
    }
    {
        float r[4];
        lw_rect(1, r);
        um_button(c, st, X(r[0]), Y(r[1]), X(r[0] + r[2]), Y(r[1] + r[3]), UmB_Normal,
            lw_hot(1) ? ((lbDisplay.MLeftButton != 0) ? UmS_Down : UmS_Hot) : UmS_Normal, S(16.0f), "Close");
    }
    const TbBool blink = lw_blink();
    for (int n = 2; n < LW_BUTTONS; n++)
    {
        struct WorldLevelEntry e = {0};
        if (!lw_entry_of(lw_lval_of(n), &e))
            continue;
        float r[4];
        lw_rect(n, r);
        const float rad = S(12.0f);
        const TbBool hot = lw_hot(n);
        const float x0 = X(r[0]), y0 = Y(r[1]), x1 = X(r[0] + LW_ICON), y1 = Y(r[1] + LW_ICON);
        if (e.focus && !st->flat)
            ui_fill_rrect(c, x0, y0, x1, y1, rad, UIC(255, 120, 40, 0), UIC(255, 120, 40, blink ? 70 : 40), false);
        int iw = 0, ih = 0;
        const unsigned char *icon = e.owned ? lw_icon(&e, &iw, &ih) : NULL;
        if (icon != NULL)
        {
            /* the icon as large as fits in the well at a whole number of its pixels, kept sharp */
            const float room = S(LW_ICON * 0.78f);
            const int k = max(1, (int)floorf(room / (float)max(iw, ih)));
            const float fit = (k * max(iw, ih) >= (int)(room * 0.75f)) ? (float)k : room / (float)max(iw, ih);
            const int dw = (int)((float)iw * fit), dh = (int)((float)ih * fit);
            const int dx = (int)((x0 + x1 - (float)dw) * 0.5f), dy = (int)((y0 + y1 - (float)dh) * 0.5f);
            /* (the chosen one flashes as the panel's chosen icon: its own pixels brighter) */
            ui_image_gain(c, dx, dy, dw, dh, icon, iw, ih, (e.focus && blink) ? 1.45f : 1.0f);
        } else
            ui_text_box(c, UiF_Head, S(36.0f), x0, y0, x1 - x0, y1 - y0, 1, UIC(160, 146, 122, 110), 0.0f, false, "?");
        um_frame(c, st, x0, y0, x1, y1, rad, hot || e.focus);
        /* its level and the bar towards the next */
        char line[32];
        if (!e.owned)
            snprintf(line, sizeof(line), "?");
        else
            snprintf(line, sizeof(line), "Lv %d", e.level);
        ui_text_box(c, UiF_Head, S(16.0f), x0 - S(8.0f), Y(r[1] + LW_ICON + 6.0f), x1 - x0 + S(16.0f), S(18.0f), 1,
            e.focus ? UM_GOLD_HI : (e.owned ? UM_TEXT : UM_DIM), 0.0f, !st->flat, line);
        if (e.owned && (e.level < e.top) && (e.need > 0))
            um_bar(c, st, x0 + S(6.0f), Y(r[1] + LW_ICON + 30.0f), x1 - S(6.0f), Y(r[1] + LW_ICON + 36.0f),
                (float)max(0L, min(e.bar, e.need)) / (float)e.need);
    }
    struct WorldLevelEntry e = {0};
    if (!lw_focus_entry(&e) && !lw_entry_of(0, &e) && !lw_entry_of(LW_SPELL_BASE, &e))
        ui_text_box(c, UiF_Body, S(16.0f), X(LW_E), Y(LW_GRID_Y + 120.0f), S(LW_W - 2 * LW_E), S(36.0f), 1, UM_DIM, 0.0f, !st->flat,
            "Nothing to level: build a Library and gain rooms and spells first.");
#undef X
#undef Y
#undef S
}

static void lwc_background(struct GuiMenu *gmnu);

static void lw_background(struct GuiMenu *gmnu)
{
    if (window_style == WinStyle_Classic)
    {
        lwc_background(gmnu);
        return;
    }
    lw_host.name = "library";
    lw_tip_host.name = "library_tip";
    lw_host.slot = UmSl_Library;
    lw_tip_host.slot = UmSl_LibraryTip; /* (over the window) */
    lw_place(gmnu);
    lw_update_hover();
    const int cw = lw_pw + 2 * lw_margin, ch = lw_ph + 2 * lw_margin;
    const unsigned long t0 = (unsigned long)LbTimerClock();
    /* (the base's key: the scale, and which cells there are) */
    char bkey[96];
    int cells = 0;
    for (int n = 2; n < LW_BUTTONS; n++)
    {
        struct WorldLevelEntry e = {0};
        cells = cells * 2 + (int)lw_entry_of(lw_lval_of(n), &e);
    }
    snprintf(bkey, sizeof(bkey), "%.4f %d", (double)lw_u, cells);
    TbBool full = um_host_begin(&lw_host, cw, ch, bkey);
    unsigned long base_ms = 0;
    if (um_host_base_begin(&lw_host, bkey))
    {
        char gkey[64];
        snprintf(gkey, sizeof(gkey), "%.4f %d %d", (double)lw_u, lw_pw, lw_ph);
        if (um_host_glass_begin(&lw_host, gkey))
        {
            const struct UmStyle gst = {lw_u, um_host_flat()};
            const float m = (float)lw_margin;
            um_window(&lw_host.glass, &gst, m, m, m + (float)lw_pw, m + (float)lw_ph);
        }
        um_host_glass_down(&lw_host);
        lw_paint(true);
        full = true;
        base_ms = (unsigned long)LbTimerClock() - t0;
    }
    struct UmPart parts[UM_PARTS];
    const int np = lw_parts(parts);
    if (um_host_parts(&lw_host, parts, np, full))
    {
        lw_paint(false);
        um_host_parts_end(&lw_host);
        um_host_painted(&lw_host, t0);
        if ((lw_host.paints <= 3) || (lw_host.paints % 50 == 0))
            SYNCLOG("World: the Rooms & Spells To Level window painted in %lu ms (base %lu; %dx%d, %s, scale %.2f, %s; %lu paintings)",
                lw_host.paint_ms, base_ms, cw, ch, full ? "all" : "a part", (double)lw_u, um_host_flat() ? "palette" : "true colour", lw_host.paints);
    }
    um_host_show(&lw_host, lw_px - lw_margin, lw_py - lw_margin);
}

static void lw_ground_rclick(struct GuiButton *gbtn)
{
    (void)gbtn;
    world_level_window_close();
}

static void lw_close_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    world_level_window_close();
}

static void lw_cell_click(struct GuiButton *gbtn)
{
    struct WorldLevelEntry e = {0};
    if (!lw_entry_of(gbtn->content.lval, &e) || !e.owned)
        return;
    set_players_packet_action(get_my_player(), PckA_WorldLevelFocus, (long)e.entry, 0, 0, 0);
    SYNCLOG("World: the Library's research goes to the %s (chosen in the window)", e.label);
}

/** The tooltip of the cell under the mouse, over everything in the window: what each level gives. */
static void lw_tooltip(void)
{
    const int lval = (lw_test_hover >= 0) ? lw_test_hover : lw_hover;
    struct WorldLevelEntry e = {0};
    if ((lval < 0) || !lw_entry_of(lval, &e))
        return;
    if (!e.owned)
        return; /* (an item not learnt yet keeps its secret: no tooltip; the user, 2026-11-26) */
    char lines[24][WORLD_LEVEL_LINE];
    const int n = world_level_entry_lines(my_player_number, e.entry, lines, 22);
    char head[96], foot[128];
    snprintf(head, sizeof(head), "%s%s", e.label, (e.kind_class == 1) ? " spell" : "");
    if (!e.owned)
        snprintf(foot, sizeof(foot), "Not yours yet: learn it first (research books).");
    else if (e.level >= e.top)
        snprintf(foot, sizeof(foot), "Level %d, its top level.", e.level);
    else
        snprintf(foot, sizeof(foot), "Level %d. %s", e.level, e.focus ? "The Library levels it now." : "Click to level it.");
    const float u = lw_u;
    const TbBool flat = um_host_flat();
    const struct UmStyle stv = {u, flat};
    const float pad = 16.0f * u, hpx = 19.0f * u, bpx = 14.0f * u, row = 21.0f * u;
    float w = ui_text_width(UiF_Head, hpx, 0.0f, head);
    for (int i = 0; i < n; i++)
        w = max(w, ui_text_width(UiF_Body, bpx, 0.0f, lines[i]));
    w = max(w, ui_text_width(UiF_Body, bpx, 0.0f, foot));
    const int tw = min((int)(w + 2.0f * pad + 0.5f), (int)MyScreenWidth - 8);
    const int th = (int)(pad + 26.0f * u + 8.0f * u + (float)n * row + 8.0f * u + row + pad + 0.5f);
    const int m = (int)(um_window_margin(&stv) * 0.5f);
    char key[sizeof(lw_tip_host.key) - 64];
    snprintf(key, sizeof(key), "%d %d %d %d %d %d %.3f", e.entry, e.level, (int)e.owned, (int)e.focus, n, tw, (double)u);
    if (um_host_begin(&lw_tip_host, tw + 2 * m, th + 2 * m, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &lw_tip_host.canvas;
        const float x0 = (float)m, y0 = (float)m, x1 = (float)(m + tw), y1 = (float)(m + th);
        if (flat)
        {
            ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, UIC(16, 11, 9, 255), UIC(16, 11, 9, 255), false);
            ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, 1.0f, UM_GOLD);
        } else
        {
            const float r = 12.0f * u;
            ui_shadow_rrect(c, x0, y0, x1, y1, r, (float)m, 6.0f * u, UIC(0, 0, 0, 170));
            ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(34, 20, 15, 255), UIC(14, 9, 8, 255), false);
            ui_stroke_rrect(c, x0, y0, x1, y1, r, 1.0f, UIC(222, 178, 88, 110));
        }
        float ty = y0 + pad;
        ui_text_box(c, UiF_Head, hpx, x0 + pad, ty, (float)tw - 2.0f * pad, 26.0f * u, 0, UM_GOLD_HI, 0.0f, !flat, head);
        ty += 26.0f * u + 8.0f * u;
        for (int i = 0; i < n; i++)
        {
            /* reached levels bright, the ones to come dim (the Training Room's two lines are both bright) */
            const TbBool reached = (e.entry == 3) || (e.owned && (i + 2 <= e.level));
            ui_text_box(c, UiF_Body, bpx, x0 + pad, ty, (float)tw - 2.0f * pad, row, 0, reached ? UM_TEXT : UM_DIM, 0.0f, !flat, lines[i]);
            ty += row;
        }
        ty += 8.0f * u;
        ui_text_box(c, UiF_Body, bpx, x0 + pad, ty, (float)tw - 2.0f * pad, row, 0, UM_GOLD, 0.0f, !flat, foot);
        um_host_painted(&lw_tip_host, t0);
    }
    int x = GetMouseX() + (int)(18.0f * u), y = GetMouseY() + (int)(10.0f * u);
    if (x + tw > (int)MyScreenWidth - 4)
        x = max(4, GetMouseX() - tw - (int)(10.0f * u));
    if (y + th > (int)MyScreenHeight - 4)
        y = max(4, (int)MyScreenHeight - 4 - th);
    um_host_show(&lw_tip_host, x - m, y - m);
}

/******************************************************************************/
/* The classic look (WINDOW_STYLE=CLASSIC): the window as it was before 2026-11-25 (the creature window's dress, the
   panel's frames and icons), drawn by its buttons, its foot saying where the research goes. The old code, renamed lwc_. */

/* The window in its own units (as the Keeper panel's: lw_place sets the scale) */
#define LWC_W 520
#define LWC_H 332
#define LWC_EDGE 24
#define LWC_TOP 50                    /* under the title */
#define LWC_CELL_W 58
#define LWC_CELL_H 70
#define LWC_ICON 46
#define LWC_COLS 4
#define LWC_GRID_Y (LWC_TOP + 26)
#define LWC_ROOMS_X LWC_EDGE
#define LWC_SPELLS_X (LWC_W / 2 + 12)
#define LWC_FOOT_Y (LWC_H - 36)
#define LWC_ORN_SIDE 34
#define LWC_ORN_TOP 14
#define LWC_ORN_BOTTOM 30
#define LWC_AIR 2
#define LWC_ROOMS 10
#define LWC_SPELLS 7
#define LWC_SPELL_BASE 100            /* a cell's lval: rooms 0-9, spells 100-106 */

static int lwc_upp = 16;

/** Where button n stood in the classic layout, in its units (x, y, w, h). */
static void lwc_rect(int n, int *r)
{
    r[0] = r[1] = r[2] = r[3] = 0;
    if (n == 0)
    {
        r[2] = LWC_W;
        r[3] = LWC_H;
        return;
    }
    if (n == 1)
    {
        r[0] = LWC_W - LWC_EDGE - 96;
        r[1] = 12;
        r[2] = 96;
        r[3] = 24;
        return;
    }
    const int lval = lw_lval_of(n);
    if (lval < 0)
        return;
    const int k = (lval >= LW_SPELL_BASE) ? lval - LW_SPELL_BASE : lval;
    const int x0 = (lval >= LW_SPELL_BASE) ? LWC_SPELLS_X : LWC_ROOMS_X;
    r[0] = x0 + LWC_CELL_W * (k % LWC_COLS);
    r[1] = LWC_GRID_Y + LWC_CELL_H * (k / LWC_COLS);
    r[2] = LWC_CELL_W - 6;
    r[3] = LWC_CELL_H - 8;
}

/** The classic window on the screen (the old lw_place). */
static void lwc_place(struct GuiMenu *gmnu)
{
    const int sw = MyScreenWidth, sh = MyScreenHeight;
    int upp = sw * 16 * 88 / 100 / (LWC_W + 2 * (LWC_ORN_SIDE + LWC_AIR));
    upp = min(upp, sh * 16 * 86 / 100 / (LWC_H + LWC_ORN_TOP + LWC_ORN_BOTTOM + 2 * LWC_AIR));
    upp = max(upp, 4);
    lwc_upp = upp;
    gmnu->width = LWC_W * upp / 16;
    gmnu->height = LWC_H * upp / 16;
    const int orn_top = LWC_ORN_TOP * upp / 16, orn_bottom = LWC_ORN_BOTTOM * upp / 16;
    gmnu->pos_x = (sw - gmnu->width) / 2;
    gmnu->pos_y = (sh - gmnu->height - orn_top - orn_bottom) / 2 + orn_top;
    int n = 0;
    memset(lw_btn, 0, sizeof(lw_btn));
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (gmnu->buttons != NULL) && (n < LW_BUTTONS); i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != gmnu->number))
            continue;
        if (gmnu->buttons[n].gbtype == -1)
            break;
        int r[4];
        lwc_rect(n, r);
        lw_btn[n] = gbtn;
        n++;
        gbtn->scr_pos_x = gmnu->pos_x + r[0] * upp / 16;
        gbtn->scr_pos_y = gmnu->pos_y + r[1] * upp / 16;
        gbtn->pos_x = gbtn->scr_pos_x;
        gbtn->pos_y = gbtn->scr_pos_y;
        gbtn->width = r[2] * upp / 16;
        gbtn->height = r[3] * upp / 16;
    }
}

static void lwc_box(int x, int y, int w, int h, TbPixel ground, TbPixel rule)
{
    const int lw = max((lwc_upp + 8) / 32, 1);
    LbDrawBox(x, y, w, h, ground);
    LbDrawBox(x, y, w, lw, rule);
    LbDrawBox(x, y + h - lw, w, lw, rule);
    LbDrawBox(x, y, lw, h, rule);
    LbDrawBox(x + w - lw, y, lw, h, rule);
}

/** A progress bar (the user, 2026-11-22: "just place a bar, dont add a timer or any numbers, besides the current
 *  Level"): a sunken well, its part done in gold. */
static void lwc_bar(int x, int y, int w, int h, long have, long need)
{
    lwc_box(x, y, w, h, colours[0][0][0], world_window_ink(WwI_Rule));
    const int lw = max((lwc_upp + 8) / 32, 1);
    if ((need <= 0) || (w <= 2 * lw) || (h <= 2 * lw))
        return;
    const double part = (double)max(0L, min(have, need)) / (double)need;
    const int fill = (int)((w - 2 * lw) * part);
    if (fill > 0)
        LbDrawBox(x + lw, y + lw, fill, h - 2 * lw, world_window_ink(WwI_Gold));
}

static void lwc_background(struct GuiMenu *gmnu)
{
    lwc_place(gmnu);
    const int upp = lwc_upp;
#define U(v) ((v) * upp / 16)
    const int gx = gmnu->pos_x, gy = gmnu->pos_y;
    world_window_frame(gx, gy, gmnu->width, gmnu->height, upp);
    world_window_display(gx + U(130), gy + U(16), U(LWC_W - 260), U(18), 1, "Rooms & Spells To Level");
    const TbPixel rule = world_window_ink(WwI_Rule);
    const int lw = max((upp + 8) / 32, 1);
    LbDrawBox(gx + U(LWC_EDGE), gy + U(LWC_TOP - 6), U(LWC_W - 2 * LWC_EDGE), lw, rule);
    LbDrawBox(gx + U(LWC_W / 2), gy + U(LWC_TOP + 2), lw, U(LWC_FOOT_Y - LWC_TOP - 10), rule);
    world_window_display(gx + U(LWC_ROOMS_X), gy + U(LWC_TOP + 4), U(LWC_COLS * LWC_CELL_W), U(12), 1, "ROOMS");
    world_window_display(gx + U(LWC_SPELLS_X), gy + U(LWC_TOP + 4), U(LWC_COLS * LWC_CELL_W), U(12), 1, "SPELLS");
    LbDrawBox(gx + U(LWC_EDGE), gy + U(LWC_FOOT_Y - 8), U(LWC_W - 2 * LWC_EDGE), lw, rule);
    /* the foot: where the Library's research goes now, and its bar */
    struct WorldLevelEntry e = {0};
    TbBool found = false;
    for (int k = 0; (k < LW_ROOMS) && !found; k++)
        found = lw_entry_of(k, &e) && e.focus;
    for (int k = 0; (k < LW_SPELLS) && !found; k++)
        found = lw_entry_of(LW_SPELL_BASE + k, &e) && e.focus;
    const int cap = max(U(9), world_window_plain_cap());
    const int fy = gy + U(LWC_FOOT_Y);
    if (found)
    {
        char line[96];
        const TbBool working = (world_level_speed(my_player_number, 0, NULL) > 0);
        snprintf(line, sizeof(line), "%s %s", working ? "Levelling:" : "Library idle:", e.label);
        world_window_plain(gx + U(LWC_EDGE), fy + (U(22) - cap) / 2, U(200), cap, 0,
            world_window_ink(working ? WwI_Bright : WwI_Dim), line);
        lwc_bar(gx + U(LWC_EDGE + 210), fy + U(5), U(LWC_W - 2 * LWC_EDGE - 210), U(12), e.bar, e.need);
    } else
        world_window_plain(gx + U(LWC_EDGE), fy + (U(22) - cap) / 2, U(LWC_W - 2 * LWC_EDGE), cap, 1,
            world_window_ink(WwI_Dim), "Nothing to level: build a Library and gain rooms and spells first.");
#undef U
}

static void lwc_close_draw(struct GuiButton *gbtn)
{
    const int bu = simple_button_sprite_height_units_per_px(gbtn, GBS_frontend_button_std_c, 100);
    const int cap = gbtn->height * 15 / 26;
    if (gbtn->width < 72 * bu / 16)
        return;
    draw_bar64k(gbtn->scr_pos_x, gbtn->scr_pos_y, bu, gbtn->width);
    if ((gbtn->flags & LbBtnF_MouseOver) != 0)
    {
        const int rim = 6 * bu / 16;
        draw_lit_bar64k(gbtn->scr_pos_x - rim, gbtn->scr_pos_y - rim, bu, gbtn->width + rim);
    }
    world_window_display(gbtn->scr_pos_x + gbtn->height / 2, gbtn->scr_pos_y + (gbtn->height - cap) / 2,
        gbtn->width - gbtn->height, cap, 1, "Close");
}

static void lwc_cell_draw(struct GuiButton *gbtn)
{
    const int lval = gbtn->content.lval;
    if ((gbtn->flags & LbBtnF_MouseOver) != 0)
        lw_hover = lval;
    else if (lw_hover == lval)
        lw_hover = -1;
    struct WorldLevelEntry e = {0};
    if (!lw_entry_of(lval, &e))
        return;
    const int upp = lwc_upp;
    const int icon = LWC_ICON * upp / 16;
    const int x = gbtn->scr_pos_x + (gbtn->width - icon) / 2, y = gbtn->scr_pos_y;
    const struct TbSprite *frame = get_panel_sprite(GPS_rpanel_frame_portrt_empty);
    if ((frame == NULL) || (frame->SWidth <= 0))
        return;
    const int ps = max(icon * 16 / frame->SWidth, 1);
    draw_gui_panel_sprite_left(x, y, ps, GPS_rpanel_frame_portrt_empty);
    if (!e.owned)
    {
        draw_gui_panel_sprite_left(x, y, ps, GPS_rpanel_frame_portrt_qmark);
    } else
    {
        const long spr = (e.kind_class == 0) ? get_room_kind_stats(e.kind)->medsym_sprite_idx :
            get_power_model_stats(e.kind)->medsym_sprite_idx;
        const TbBool blink = e.focus && ((get_gameturn() % (2 * gui_blink_rate)) >= gui_blink_rate);
        if (e.focus)
            draw_gui_panel_sprite_left(x, y, ps, GPS_rpanel_frame_portrt_light);
        if (blink)
            draw_gui_panel_sprite_rmleft(x, y, ps, spr, 44); /* (flashing as the panel's chosen icon) */
        else
            draw_gui_panel_sprite_left(x, y, ps, spr);
    }
    if ((gbtn->flags & LbBtnF_MouseOver) != 0)
    {
        const TbPixel lit = world_window_ink(WwI_Gold);
        const int lw = max((upp + 8) / 16, 1);
        const int fh = frame->SHeight * ps / 16;
        LbDrawBox(x, y, icon, lw, lit);
        LbDrawBox(x, y + fh - lw, icon, lw, lit);
        LbDrawBox(x, y, lw, fh, lit);
        LbDrawBox(x + icon - lw, y, lw, fh, lit);
    }
    /* the level under it */
    char line[32];
    if (!e.owned)
        snprintf(line, sizeof(line), "?");
    else
        snprintf(line, sizeof(line), "Lv %d", e.level);
    const int fh = frame->SHeight * ps / 16;
    const int cap = max(9 * upp / 16, world_window_plain_cap());
    world_window_plain(gbtn->scr_pos_x - 3 * upp / 16, y + fh + 3 * upp / 16, gbtn->width + 6 * upp / 16, cap, 1,
        world_window_ink(e.focus ? WwI_Gold : (e.owned ? WwI_Bright : WwI_Dim)), line);
    /* its bar towards the next level */
    if (e.owned && (e.level < e.top))
        lwc_bar(x, y + fh + 3 * upp / 16 + cap + 3 * upp / 16, icon, max(4 * upp / 16, 3), e.bar, e.need);
}

/** The tooltip of the cell under the mouse, over everything in the window. */
static void lwc_tooltip(void)
{
    const int lval = (lw_test_hover >= 0) ? lw_test_hover : lw_hover;
    struct WorldLevelEntry e = {0};
    if ((lval < 0) || !lw_entry_of(lval, &e))
        return;
    if (!e.owned)
        return; /* (an item not learnt yet keeps its secret: no tooltip; the user, 2026-11-26) */
    const int upp = lwc_upp;
#define U(v) ((v) * upp / 16)
    char lines[24][WORLD_LEVEL_LINE];
    const int n = world_level_entry_lines(my_player_number, e.entry, lines, 22);
    char head[96], foot[128];
    snprintf(head, sizeof(head), "%s%s", e.label, (e.kind_class == 1) ? " spell" : "");
    if (!e.owned)
        snprintf(foot, sizeof(foot), "Not yours yet: learn it first (research books).");
    else if (e.level >= e.top)
        snprintf(foot, sizeof(foot), "Level %d, its top level.", e.level);
    else
        snprintf(foot, sizeof(foot), "Level %d. %s", e.level, e.focus ? "The Library levels it now." : "Click to level it.");
    const int cap = max(U(9), world_window_plain_cap());
    const int row = cap + U(5);
    int w = world_window_display_width(head, U(13)) + U(24);
    for (int i = 0; i < n; i++)
        w = max(w, world_window_plain_width(lines[i], cap) + U(24));
    w = max(w, world_window_plain_width(foot, cap) + U(24));
    w = min(w, MyScreenWidth - 8);
    const int h = U(12) + U(13) + U(8) + n * row + U(6) + row + U(8);
    int x = GetMouseX() + U(18), y = GetMouseY() + U(10);
    if (x + w > MyScreenWidth - 4)
        x = max(4, GetMouseX() - w - U(10));
    if (y + h > MyScreenHeight - 4)
        y = max(4, MyScreenHeight - 4 - h);
    lwc_box(x, y, w, h, colours[1][1][1], world_window_ink(WwI_Gold));
    int ty = y + U(10);
    world_window_display(x + U(12), ty, w - U(24), U(13), 0, head);
    ty += U(13) + U(8);
    for (int i = 0; i < n; i++)
    {
        /* reached levels bright, the ones to come dim (the Training Room's two lines are both bright) */
        const TbBool reached = (e.entry == 3) || (e.owned && (i + 2 <= e.level));
        world_window_plain(x + U(12), ty, w - U(24), cap, 0, world_window_ink(reached ? WwI_Bright : WwI_Dim), lines[i]);
        ty += row;
    }
    ty += U(6);
    world_window_plain(x + U(12), ty, w - U(24), cap, 0, world_window_ink(WwI_Gold), foot);
#undef U
}

/** A button's drawing: the classic look draws each; the modern one paints them all with the window. */
static void lw_button_draw(struct GuiButton *gbtn)
{
    if (window_style != WinStyle_Classic)
        return;
    if ((lw_btn[1] == gbtn) && (gbtn->content.lval == 0))
        lwc_close_draw(gbtn);
    else if (lw_btn[0] != gbtn)
        lwc_cell_draw(gbtn);
}

#define LW_BTN(click, rclick, v) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, click, rclick, NULL, 0, 0, 0, 0, 0, 1, 1, lw_button_draw, 0, \
    GUIStr_Empty, 0, {.lval = (v)}, 0, NULL }
#define LW_ROOM(k) LW_BTN(lw_cell_click, NULL, (k))
#define LW_SPELL(k) LW_BTN(lw_cell_click, NULL, LW_SPELL_BASE + (k))

/* (places and sizes are set every frame by lw_place; painted with the window, the buttons only take the mouse) */
static struct GuiButtonInit world_level_window_buttons[] = {
  LW_BTN(NULL, lw_ground_rclick, 0),
  LW_BTN(lw_close_click, NULL, 0),
  LW_ROOM(0), LW_ROOM(1), LW_ROOM(2), LW_ROOM(3), LW_ROOM(4), LW_ROOM(5), LW_ROOM(6), LW_ROOM(7), LW_ROOM(8), LW_ROOM(9),
  LW_SPELL(0), LW_SPELL(1), LW_SPELL(2), LW_SPELL(3), LW_SPELL(4), LW_SPELL(5), LW_SPELL(6),
  {              -1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, {.lval = 0}, 0, NULL },
};

struct GuiMenu world_level_window_menu =
 { GMnu_LEVEL_WINDOW, 0, 1, world_level_window_buttons, POS_GAMECTR, POS_GAMECTR, (int)LW_W, (int)LW_H, lw_background, 0, NULL, NULL, 0, 0, 0,};

void world_level_window_draw_last(void)
{
    if (!world_level_window_open())
        return;
    if ((get_local_view_type() != PVT_DungeonTop) || world_keeper_panel_open() || menu_is_active(GMnu_CREATURE_WINDOW))
    {
        world_level_window_close();
        return;
    }
    const MenuNumber num = menu_id_to_number(GMnu_LEVEL_WINDOW);
    if (num < 0)
        return;
    struct GuiMenu *gmnu = get_active_menu(num);
    if ((gmnu == NULL) || (gmnu->visual_state == 0) || !gmnu->is_turned_on)
        return;
    const TbDrawFlagsMask flg_mem = RendererGetDrawFlags();
    const struct TbSpriteSheet *font_mem = lbFontPtr;
    LbTextSetFont(winfont);
    LbTextSetWindow(0, 0, MyScreenWidth / pixel_size, MyScreenHeight / pixel_size);
    if (gmnu->draw_cb != NULL)
        gmnu->draw_cb(gmnu);
    if (gmnu->visual_state == 2)
        draw_menu_buttons(gmnu);
    if (window_style == WinStyle_Classic)
        lwc_tooltip();
    else
        lw_tooltip();
    RendererSetDrawFlags(flg_mem);
    LbTextSetFont(font_mem);
}

/******************************************************************************/
/* The book over the Library */

/** The keeper's own Library under the pointer, or the one the book is over while the pointer is on the book. */
static struct Room *lb_hovered_library(void)
{
    if (lw_test_room > 0)
    {
        struct Room *room = room_get(lw_test_room);
        if (!room_is_invalid(room))
            return room;
    }
    const int pmx = GetMouseX(), pmy = GetMouseY();
    if (lb_shown && (lb_room > 0) && (pmx >= lb_x1) && (pmx < lb_x2) && (pmy >= lb_y1) && (pmy < lb_y2))
    {
        struct Room *room = room_get(lb_room);
        if (!room_is_invalid(room) && (room->owner == my_player_number))
            return room;
    }
    if ((block_pointed_at_x < 0) || (block_pointed_at_y < 0) || (block_pointed_at_x >= game.map_subtiles_x) ||
        (block_pointed_at_y >= game.map_subtiles_y))
        return INVALID_ROOM;
    struct Room *room = subtile_room_get(block_pointed_at_x, block_pointed_at_y);
    if (room_is_invalid(room) || (room->owner != my_player_number) || !room_role_matches(room->kind, RoRoF_Research))
        return INVALID_ROOM;
    return room;
}

static TbBool lb_may_show(void)
{
    if (!world_mode_active() || netstate_dedicated() || (get_local_view_type() != PVT_DungeonTop))
        return false;
    if ((lw_test_room > 0))
        return true;
    const struct UserState *ustate = get_local_user_state();
    if (user_state_invalid(ustate) || (ustate->work_state != PSt_CtrlDungeon) || !power_hand_is_empty(get_my_player()))
        return false;
    if (world_level_window_open() || world_keeper_panel_open() || menu_is_active(GMnu_CREATURE_WINDOW) ||
        gameplay_cursor_is_over_gui())
        return false;
    return true;
}

void world_level_book_draw(void)
{
    struct Room *room = lb_may_show() ? lb_hovered_library() : INVALID_ROOM;
    if (room_is_invalid(room))
    {
        lb_shown = false;
        lb_room = 0;
        return;
    }
    const TbClockMSec now = LbTimerClock();
    if (!lb_shown || (lb_room != room->index))
    {
        lb_since = now;
        lb_room = room->index;
    }
    lb_shown = true;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(room->central_stl_x);
    pos.y.val = subtile_coord_center(room->central_stl_y);
    pos.z.val = 0;
    pos.z.val = get_floor_height_at(&pos);
    /* the view's own size: a slab's width on the screen */
    struct Coord3d a = pos, b = pos;
    a.x.val -= COORD_PER_STL * STL_PER_SLB / 2;
    b.x.val += COORD_PER_STL * STL_PER_SLB / 2;
    if (rotpers == NULL) /* (the view not drawn yet: no projection; the first frame after a load) */
    {
        lb_x1 = lb_y1 = lb_x2 = lb_y2 = 0;
        return;
    }
    int32_t ax, ay, bx, by;
    map_to_screen(&a, &ax, &ay);
    map_to_screen(&b, &bx, &by);
    const double slab_px = sqrt((double)(bx - ax) * (bx - ax) + (double)(by - ay) * (by - ay));
    const long top_scale = max(24L, min((long)(slab_px * 1.6), 800L)); /* (32 = the sprite's own pixels: about a slab tall) */
    /* the rise: from the floor up, slowing as it arrives, growing from a book's size to its size on top, out of nothing to
       fully seen on arrival; then it floats */
    const long age = (long)(now - lb_since);
    double t = (double)age / LB_RISE_MS;
    if (t > 1.0)
        t = 1.0;
    const double ease = 1.0 - (1.0 - t) * (1.0 - t);
    pos.z.val += (MapCoordDelta)(LB_LIFT * ease);
    const long scale = max(8L, (long)((double)top_scale * (LB_START_SIZE + (100 - LB_START_SIZE) * ease) / 100.0));
    if (t >= 1.0)
        pos.z.val += (MapCoordDelta)(28.0 * sin((double)(age - LB_RISE_MS) / 420.0));
    int32_t sx, sy;
    if (!map_to_screen(&pos, &sx, &sy))
    {
        lb_x1 = lb_y1 = lb_x2 = lb_y2 = 0;
        return;
    }
    /* how much of it is seen, in the steps the renderers have (TRANSPAR4 a third, TRANSPAR8 two thirds, ALPHA_75 three
       quarters: kfx/renderer/DrawState.h; they went 2/3, 1/3, 3/4 before 2026-11-26) */
    TbDrawFlagsMask fade = 0;
    if (t < 0.10)
        fade = (TbDrawFlagsMask)-1; /* not yet */
    else if (t < 0.35)
        fade = Lb_SPRITE_TRANSPAR4;
    else if (t < 0.60)
        fade = Lb_SPRITE_TRANSPAR8;
    else if (t < 0.85)
        fade = Lb_SPRITE_ALPHA_75;
    /* where a click finds it (a little more than the book itself) */
    lb_x1 = sx - (int)(30 * scale / 32);
    lb_x2 = sx + (int)(30 * scale / 32);
    lb_y1 = sy - (int)(60 * scale / 32);
    lb_y2 = sy + (int)(4 * scale / 32);
    if (fade == (TbDrawFlagsMask)-1)
        return;
    const int frame = (int)((now / 50) % LB_FRAMES);
    const TbDrawFlagsMask flg_mem = RendererGetDrawFlags();
    RendererSetDrawFlags(fade);
    RendererSubmitKeeperHandSprite((short)sx, (short)sy, LB_ANIM, 0, (unsigned char)frame, scale, fade);
    RendererSetDrawFlags(flg_mem);
}

TbBool world_level_book_hovered(void)
{
    if (!world_mode_active() || !lb_shown || (lb_room <= 0))
        return false;
    const int pmx = GetMouseX(), pmy = GetMouseY();
    return (pmx >= lb_x1) && (pmx < lb_x2) && (pmy >= lb_y1) && (pmy < lb_y2);
}

TbBool world_level_window_inputs(void)
{
    if (!world_mode_active() || !lb_shown || (lb_room <= 0))
    {
        lb_pressed = false;
        return false;
    }
    const int pmx = GetMouseX(), pmy = GetMouseY();
    const TbBool on_book = (pmx >= lb_x1) && (pmx < lb_x2) && (pmy >= lb_y1) && (pmy < lb_y2);
    if (!on_book)
    {
        lb_pressed = false;
        return false;
    }
    if (left_button_clicked)
    {
        /* (the press is the book's: the Hand doesn't pick up a researcher behind it) */
        left_button_clicked = 0;
        lb_pressed = true;
        return true;
    }
    if (left_button_released)
    {
        left_button_released = 0;
        left_button_clicked = 0;
        lb_pressed = false;
        world_level_window_show();
        return true;
    }
    return lb_pressed;
}

/******************************************************************************/
/** KFX_WORLD_LEVELTEST=turn (see the file's head). */
void world_level_window_turn(void)
{
    if (lw_test_at == -2)
    {
        const char *v = getenv("KFX_WORLD_LEVELTEST");
        lw_test_at = (v != NULL) ? atol(v) : -1;
        if (lw_test_at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_LEVELTEST=%ld", lw_test_at);
    }
    if ((lw_test_at < 0) || netstate_dedicated())
        return;
    const long t = (long)game.play_gameturn;
    if (t == lw_test_at)
    {
        const struct Thing *heart = get_player_soul_container(my_player_number);
        if (thing_exists(heart))
        {
            const MapSubtlCoord hx = heart->mappos.x.stl.num, hy = heart->mappos.y.stl.num;
            struct Room *room = INVALID_ROOM;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    struct Room *r = place_room(my_player_number, (RoomKind)get_id(room_desc, "RESEARCH"),
                        hx + (3 + dx) * STL_PER_SLB, hy + dy * STL_PER_SLB);
                    if (!room_is_invalid(r))
                        room = r;
                }
            lw_test_room = room_is_invalid(room) ? 0 : room->index;
            lw_test_room_kept = lw_test_room;
            SYNCLOG("World: level window test: a Library %s (room %d)", room_is_invalid(room) ? "could not be placed" : "placed",
                lw_test_room);
        }
    } else if (t == lw_test_at + 60)
    {
        lw_test_room = 0;
        world_level_window_show();
    } else if (t == lw_test_at + 100)
    {
        lw_test_hover = 2; /* the Library's cell */
        /* the Library and the Training Room the keeper's (as books give them), for the choice and the levels */
        set_room_available(my_player_number, (RoomKind)get_id(room_desc, "RESEARCH"), 1, 1);
        set_room_available(my_player_number, (RoomKind)get_id(room_desc, "TRAINING"), 1, 1);
        /* two Warlocks and a Dragon to research in the test Library (1x + 0.2 + 0.2 + 0.5 = 1.9x) */
        static const char *const kinds[3] = {"SORCEROR", "SORCEROR", "DRAGON"};
        const struct Room *lib = room_get(lw_test_room_kept);
        int sent = 0;
        for (int k = 0; (k < 3) && !room_is_invalid(lib); k++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(lib->central_stl_x);
            pos.y.val = subtile_coord_center(lib->central_stl_y);
            pos.z.val = 0;
            struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, kinds[k]), my_player_number);
            if (!thing_exists(c))
                continue;
            move_thing_in_map(c, &pos);
            if (send_creature_to_job_for_player(c, my_player_number, Job_RESEARCH))
                sent++;
        }
        SYNCLOG("World: level window test: %d of 3 researchers sent to the Library", sent);
    } else if (t == lw_test_at + 130)
    {
        int workers = 0;
        const int sp = world_level_speed(my_player_number, 0, &workers);
        SYNCLOG("World: level window test: the Library works at x%d.%02d with %d researching", sp / 100, sp % 100, workers);
    }
    else if (t == lw_test_at + 140)
    {
        lw_test_hover = -1;
        struct WorldLevelEntry e = {0};
        if (lw_entry_of(3, &e))
        {
            struct GuiButton fake;
            memset(&fake, 0, sizeof(fake));
            fake.content.lval = 3;
            lw_cell_click(&fake);
        }
        SYNCLOG("World: level window test: focus %d, creature level cap %d", world_level_focus(my_player_number),
            world_creature_level_cap(my_player_number));
    } else if (t == lw_test_at + 150)
    {
        /* 203 minutes of the Library at 1x: the Training Room levels 2 (58 minutes) and 3 (87), and half of level 4 (116) */
        world_level_test_time(my_player_number, 203 * 60);
        SYNCLOG("World: level window test: after 203 minutes at 1x: focus %d, creature level cap %d",
            world_level_focus(my_player_number), world_creature_level_cap(my_player_number));
    } else if (t == lw_test_at + 160)
        lw_test_hover = 3;
    else if (t == lw_test_at + 200)
    {
        lw_test_hover = -1;
        world_level_window_close();
    }
}

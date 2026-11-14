/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_creature_window.c
 *     Endless worlds: the creature window.
 * @par Purpose:
 *     The user, 2026-11-14: "let's move the information seen of the 'Information Panel' elsewhere: Build a new UI
 *     window that contains everything the 'Information Panel' shows when clicking on a creature with the
 *     'Information Panel' tool. Same UI design as the World Boss Event ui. Something similar to the screenshot."
 *     (the screenshot: the portrait and the abilities at the left, the statistics in two columns in the middle,
 *     and at the right an "area for new stats, vices, contracts") "Make sure it is all neatly formatted. Text inside
 *     buttons centered, highlights matching the button, text not overflowing, text not cutting out etc.." And, the
 *     same day: "make sure it works for all aspect ratios and resolutions though. Every new ui we have integrated".
 *
 *     The window is a menu in the middle of the view, in the World Boss window's dress (the game's ornate frame, its
 *     lettering for the headings, its button bar). Its left and middle are the Information Panel's own pieces, drawn
 *     by the panel's own code at seven eighths of their size: the portrait, the mood, the level, the name with the
 *     health behind it, the ten abilities with their clocks (the panel showed six a page) and the twenty statistics
 *     (the panel showed ten a page). Its right is the Rap Sheet, drawn by world_misdeeds.c in the game's plain font;
 *     what a keeper can click there are buttons.
 *
 *     Every screen: the game gives a menu one scale (the side panel's), which made this window as high as a 16:9
 *     screen and wider than the view beside the side panel. So the window sets itself out, every frame: it takes the
 *     menus' scale where the whole of it, ornaments and all, fits the view right of the side panel (a 16:9 screen or
 *     wider), and a smaller one where it does not (4:3, 5:4, 16:10); it stands on the view's middle, and its buttons
 *     are put where its own scale has them.
 *
 *     It is open exactly while this player looks at a creature with the Information tool (the player's own state
 *     says so, so nothing has to open or close it by hand): a click on a creature opens it, a click on another
 *     changes it, a right click, the Close button or another tool closes it. The side panel stays on the
 *     Information tab. A creature the player has taken over keeps the panel's pages, as before.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include <stdio.h>
#include <string.h>
#include "world_creature_window.h"
#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "config_creature.h"
#include "config_players.h"
#include "config_strings.h"
#include "creature_control.h"
#include "creature_graphics.h"
#include "custom_sprites.h"
#include "frontend.h"
#include "frontmenu_ingame_tabs.h"
#include "game_legacy.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "net_state.h"
#include "packets.h"
#include "player_data.h"
#include "player_instances.h"
#include "sprites.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "vidfade.h"
#include "world_misdeeds.h"
#include "world_stream.h"
#include "post_inc.h"

/* the Information Panel's own drawing (frontmenu_ingame_tabs.c), used here at other places */
void gui_area_smiley_anger_button(struct GuiButton *gbtn);
void gui_area_experience_button(struct GuiButton *gbtn);
void gui_area_instance_button(struct GuiButton *gbtn);
void maintain_instance(struct GuiButton *gbtn);
void gui_area_stat_button(struct GuiButton *gbtn);

/* The window in its own units. Where there is room a unit is as large as a unit of the side panel (the menus' scale):
   the window is then about as high as the World Boss window, and the panel's pieces stand in it at seven eighths of
   their size (a statistic's box is 60 x 24 on the panel, 52 x 21 here). */
#define CW_W 510
#define CW_H 354
#define CW_TOP 42                               /* where the three parts begin, under the title */
#define CW_FOOT 44                              /* the Close button's strip */
#define CW_PART_H (CW_H - CW_TOP - CW_FOOT)     /* 268 */
#define CW_EDGE 16                              /* from the window's edge to what is in it */
#define CW_PIECES 112                           /* a part of the panel's pieces: two columns of 52, 8 between */
#define CW_LEFT_X CW_EDGE                       /* the portrait and the abilities */
#define CW_MID_X (CW_LEFT_X + CW_PIECES + 16)   /* the statistics */
#define CW_RIGHT_X (CW_MID_X + CW_PIECES + 14)  /* the Rap Sheet */
#define CW_RIGHT_W (CW_W - CW_EDGE - CW_RIGHT_X)
/* The frame's ornaments reach past the window's edge by this much, in its units (the corner pieces: see cw_frame). */
#define CW_ORN_SIDE 34
#define CW_ORN_TOP 14
#define CW_ORN_BOTTOM 30
#define CW_AIR 2                                /* kept free around them */

/******************************************************************************/
/* The lettering.
 *
 * Two fonts. The game's own lettering (data/font2: orange, ornate) is for headings and button words: it reads well
 * large and badly small. Everything else is the game's plain font (data/font1: thin, upright strokes, one colour), in a
 * light colour over a dark shadow - the user, 2026-11-14: "the font for the RAP SHEET is terrible. Should be swapped
 * with something readable".
 *
 * A font's glyphs sit in cells with empty rows above and below the letters, differently in each font, so text is
 * placed by its capitals: the callers give the top of the capitals and their height, and what a font's cell is like
 * is measured from its 'H'. (Placing by the cell made lines touch the buttons under them on a large screen.)
 *
 * The plain font has strokes one pixel wide. Enlarged by a whole number they stay even; at 2.5 times every other
 * stroke is thicker. So a size within reach of a whole multiple (6% up, 20% down) is drawn at that multiple - on a
 * 2560 x 1440 screen the sheet's letters are exactly three times the font, on 1920 x 1080 twice. Smaller than the
 * font itself it loses rows and columns of its letters: who sets text with it asks for its own size at the least
 * (world_window_plain_cap; the Rap Sheet scrolls rather than go below it). */

struct CwFont { const struct TbSpriteSheet *sheet; const unsigned char *key; int cell, top, cap; };
struct CwText { const struct TbSpriteSheet *font; int units, top, cell, cap; TbBool plain; };

/** Where a font has its capitals: its cell's height, the first row of 'H' that is not empty, and how many it has. */
static void cw_font_rows(const struct TbSpriteSheet *sheet, struct CwFont *out)
{
    static struct CwFont known[4];
    static int next = 0;
    const struct TbSprite *spr = (sheet != NULL) ? LbFontCharSprite(sheet, 'H') : NULL;
    const unsigned char *key = (spr != NULL) ? spr->Data : NULL;
    for (int i = 0; i < 4; i++)
        if ((sheet != NULL) && (known[i].sheet == sheet) && (known[i].key == key))
        {
            *out = known[i];
            return;
        }
    struct CwFont f = {sheet, key, 1, 0, 1};
    if ((key != NULL) && (spr->SWidth > 0) && (spr->SHeight > 0) && (spr->SWidth <= 64) && (spr->SHeight <= 64))
    {
        static unsigned char px[64 * 64];
        int first = -1, last = -1;
        LbSpriteDecode(px, 64, key, spr->SWidth, spr->SHeight);
        for (int y = 0; y < spr->SHeight; y++)
            for (int x = 0; x < spr->SWidth; x++)
                if (px[y * 64 + x] != 0)
                {
                    if (first < 0)
                        first = y;
                    last = y;
                    break;
                }
        f.cell = spr->SHeight;
        f.top = max(first, 0);
        f.cap = (first >= 0) ? last - first + 1 : f.cell;
    } else if (sheet != NULL)
    {
        f.cell = max(LbSprFontCharHeight(sheet, ' '), 1);
        f.cap = max(f.cell * 3 / 4, 1);
    }
    if (sheet != NULL)
    {
        known[next] = f;
        next = (next + 1) % 4;
    }
    *out = f;
}

static void cw_text_of(const struct CwFont *f, int units, struct CwText *t)
{
    t->font = f->sheet;
    t->plain = false;
    t->units = units;
    t->top = f->top * units / 16;
    t->cell = (f->cell * units + 15) / 16;
    t->cap = max(f->cap * units / 16, 1);
}

/** The plain font and its scale for capitals `cap` pixels high (see the comment above). */
static void cw_plain_setup(int cap, struct CwText *t)
{
    struct CwFont f;
    cw_font_rows(font_sprites, &f);
    cap = max(cap, 2);
    int units = max(16 * cap / f.cap, 2); /* (16: the font as it is) */
    if (units > 16)
    {
        const int down = units / 16 * 16, up = down + 16;
        if (units != down)
        {
            if ((up - units) * 16 <= units)
                units = up;
            else if ((units - down) * 5 <= units)
                units = down;
        }
    } else if ((units < 16) && ((16 - units) * 16 <= units))
        units = 16;
    cw_text_of(&f, units, t);
    t->plain = true;
}

int world_window_plain_cap(void)
{
    struct CwFont f;
    cw_font_rows(font_sprites, &f);
    return max(f.cap, 1);
}

/** The game's lettering for capitals `cap` pixels high. */
static void cw_display_setup(int cap, struct CwText *t)
{
    struct CwFont f;
    cw_font_rows(winfont, &f);
    cw_text_of(&f, max(16 * max(cap, 2) / f.cap, 3), t);
}

/** The gap between two words. The plain font's own space is as wide as its widest letters, which sets a line's words
 *  too far apart to read as a line: four of its pixels are taken (the small plain font's is three: left as it is). */
static int cw_space(const struct CwText *t)
{
    const int own = LbTextCharWidthM(' ', t->units);
    return t->plain ? max(min(own, 4 * t->units / 16), 1) : own;
}

/** Goes through a line word by word: draws each at (x, y) and on (draw), or only adds up the line's width. */
static int cw_text_words(const struct CwText *t, const char *text, TbBool draw, int x, int y, int win_h)
{
    const int space = cw_space(t);
    int wide = 0;
    for (const char *p = text; *p != 0; )
    {
        if (*p == ' ')
        {
            wide += space;
            p++;
            continue;
        }
        char word[WORLD_WINDOW_LINE];
        size_t n = strcspn(p, " ");
        const size_t keep = min(n, sizeof(word) - 1);
        memcpy(word, p, keep);
        word[keep] = 0;
        if (draw)
        {
            /* (the text's window reaches to the screen's edge: a line is never broken by the drawing itself) */
            LbTextSetWindow(x + wide, y, max(MyScreenWidth - (x + wide), 1), win_h);
            LbTextDrawResized(0, 0, t->units, word);
        }
        wide += LbTextStringWidthM(word, t->units);
        p += n;
    }
    return wide;
}

static int cw_text_width(const struct CwText *t, const char *text)
{
    if ((t->font == NULL) || (text == NULL))
        return 0;
    const struct TbSpriteSheet *font_mem = lbFontPtr;
    LbTextUseByteCoding(false);
    LbTextSetFont(t->font);
    const int wide = cw_text_words(t, text, false, 0, 0, 0);
    LbTextUseByteCoding(true);
    LbTextSetFont(font_mem); /* (the menus' own drawing takes the font as it finds it) */
    return wide;
}

/** Draws a line: (x, y) the top left of its capitals' room, w that room's width; colour -1: the font's own. */
static void cw_text_draw(const struct CwText *t, int x, int y, int w, int align, int colour, const char *text)
{
    if ((t->font == NULL) || (text == NULL) || (text[0] == 0))
        return;
    const unsigned long flg_mem = RendererGetDrawFlags();
    const unsigned char col_mem = RendererGetDrawColour();
    const struct TbSpriteSheet *font_mem = lbFontPtr;
    const int wide = cw_text_width(t, text);
    const int tx = (align == 1) ? x + (w - wide) / 2 : (align == 2) ? x + w - wide : x;
    const int ty = y - t->top;
    const int win_h = t->cell + 4;
    LbTextUseByteCoding(false);
    LbTextSetFont(t->font);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags(Lb_TEXT_HALIGN_LEFT);
    if (colour >= 0)
    {
        const int shadow = max((t->units + 16) / 32, 1);
        RendererAddDrawFlags(Lb_TEXT_ONE_COLOR);
        RendererSetDrawColour(colours[1][1][0]);
        cw_text_words(t, text, true, tx + shadow, ty + shadow, win_h);
        RendererSetDrawColour((unsigned char)colour);
    }
    cw_text_words(t, text, true, tx, ty, win_h);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    LbTextUseByteCoding(true);
    LbTextSetFont(font_mem);
    RendererSetDrawColour(col_mem);
    RendererSetDrawFlags(flg_mem);
}

unsigned char world_window_ink(int ink)
{
    switch (ink)
    {
    case WwI_Dim:
        return colours[12][10][7];
    case WwI_Gold:
        return colours[15][12][3];
    case WwI_Rule:
        return colours[6][5][2];
    default:
        return colours[15][14][11];
    }
}

void world_window_plain(int x, int y, int w, int cap, int align, unsigned char colour, const char *text)
{
    struct CwText t;
    cw_plain_setup(cap, &t);
    int wide = cw_text_width(&t, text);
    for (int tries = 0; (wide > w) && (w > 4) && (t.cap > 2) && (tries < 8); tries++)
    {
        /* too long for its room: smaller letters, on the same middle line */
        cw_plain_setup(max(min(t.cap * w / wide, t.cap - 1), 2), &t);
        wide = cw_text_width(&t, text);
    }
    cw_text_draw(&t, x, y + (cap - t.cap) / 2, w, align, colour, text);
}

int world_window_plain_width(const char *text, int cap)
{
    struct CwText t;
    cw_plain_setup(cap, &t);
    return cw_text_width(&t, text);
}

int world_window_plain_wrap(const char *text, int cap, int w, char lines[][WORLD_WINDOW_LINE], int max_lines)
{
    struct CwText t;
    cw_plain_setup(cap, &t);
    int n = 0;
    const char *p = text;
    while ((*p != 0) && (n < max_lines))
    {
        while (*p == ' ')
            p++;
        if (*p == 0)
            break;
        size_t take = strlen(p);
        if ((n < max_lines - 1) && (cw_text_width(&t, p) > w))
        {
            /* as many whole words as the room holds (one at the least) */
            char buf[WORLD_WINDOW_LINE];
            size_t best = 0;
            for (size_t i = 1; (i <= strlen(p)) && (i < sizeof(buf)); i++)
            {
                if ((p[i] != ' ') && (p[i] != 0))
                    continue;
                memcpy(buf, p, i);
                buf[i] = 0;
                if ((best > 0) && (cw_text_width(&t, buf) > w))
                    break;
                best = i;
            }
            if (best > 0)
                take = best;
        }
        if (take > WORLD_WINDOW_LINE - 1)
            take = WORLD_WINDOW_LINE - 1;
        memcpy(lines[n], p, take);
        lines[n][take] = 0;
        n++;
        p += take;
    }
    return n;
}

void world_window_display(int x, int y, int w, int cap, int align, const char *text)
{
    struct CwText t;
    cw_display_setup(cap, &t);
    int wide = cw_text_width(&t, text);
    for (int tries = 0; (wide > w) && (w > 4) && (t.cap > 2) && (tries < 8); tries++)
    {
        cw_display_setup(max(min(t.cap * w / wide, t.cap - 1), 2), &t);
        wide = cw_text_width(&t, text);
    }
    cw_text_draw(&t, x, y + (cap - t.cap) / 2, w, align, -1, text);
}

int world_window_display_width(const char *text, int cap)
{
    struct CwText t;
    cw_display_setup(cap, &t);
    return cw_text_width(&t, text);
}

/******************************************************************************/
/* The window */

static int cw_upp = 16; /* the window's scale as set out last: pixels a unit, times 16 */

/** The creature the Information tool shows. */
static struct Thing *cw_thing(void)
{
    struct Thing *thing = thing_get(get_user_state(get_displayed_user())->controlled_thing_idx);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
        return INVALID_THING;
    return thing;
}

TbBool world_creature_window_used(void)
{
    return world_mode_active() && !netstate_dedicated();
}

TbBool world_creature_window_open(void)
{
    return world_creature_window_used() && menu_is_active(GMnu_CREATURE_WINDOW);
}

int world_creature_window_panel_menu(int menu)
{
    /* (asked where the Information tool shows a creature; a creature the player has taken over is shown elsewhere and
       keeps the panel's pages) */
    return world_creature_window_used() ? GMnu_QUERY : menu;
}

/** Sets the window out on the screen as it is now: its scale, its place, and its buttons' places. */
static void cw_place(struct GuiMenu *gmnu)
{
    int view_x = local_state.engine_window_x, view_y = local_state.engine_window_y;
    int view_w = local_state.engine_window_width, view_h = local_state.engine_window_height;
    if ((view_w <= 0) || (view_h <= 0) || (view_x + view_w > MyScreenWidth) || (view_y + view_h > MyScreenHeight))
    {
        view_x = 0;
        view_y = 0;
        view_w = MyScreenWidth;
        view_h = MyScreenHeight;
    }
    /* the menus' own scale (create_menu), or as much of it as lets the window and its ornaments into the view */
    int upp = min((int)units_per_pixel, (int)units_per_pixel_min * 16 / 10);
    upp = min(upp, view_w * 16 / (CW_W + 2 * (CW_ORN_SIDE + CW_AIR)));
    upp = min(upp, view_h * 16 / (CW_H + CW_ORN_TOP + CW_ORN_BOTTOM + 2 * CW_AIR));
    upp = max(upp, 4);
    cw_upp = upp;
    gmnu->width = CW_W * upp / 16;
    gmnu->height = CW_H * upp / 16;
    gmnu->pos_x = view_x + (view_w - gmnu->width) / 2;
    gmnu->pos_y = view_y + (view_h - gmnu->height - (CW_ORN_TOP + CW_ORN_BOTTOM) * upp / 16) / 2 + CW_ORN_TOP * upp / 16;
    /* its buttons are in the pool in the table's order (each took the first free place when the menu was made) */
    int n = 0;
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (gmnu->buttons != NULL); i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != gmnu->number))
            continue;
        const struct GuiButtonInit *init = &gmnu->buttons[n];
        if (init->gbtype == -1)
            break;
        n++;
        if (init->width == 0)
            continue; /* (a button of the Rap Sheet: the sheet's drawing says where it is) */
        gbtn->scr_pos_x = gmnu->pos_x + init->scr_pos_x * upp / 16;
        gbtn->scr_pos_y = gmnu->pos_y + init->scr_pos_y * upp / 16;
        gbtn->pos_x = gbtn->scr_pos_x;
        gbtn->pos_y = gbtn->scr_pos_y;
        gbtn->width = init->width * upp / 16;
        gbtn->height = init->height * upp / 16;
    }
}

void world_creature_window_turn(void)
{
    if (!world_creature_window_used())
        return;
    const struct UserState *ustate = get_local_user_state();
    if (user_state_invalid(ustate))
        return;
    /* looking at a creature (PSt_CreatrInfo), or on the way from one creature to the next (the query's own step) */
    const TbBool looking = (ustate->work_state == PSt_CreatrInfo) || (ustate->work_state == PSt_CreatrInfoAll) ||
        (((ustate->work_state == PSt_CreatrQuery) || (ustate->work_state == PSt_QueryAll)) &&
        (ustate->instance_num == PI_QueryCrtr) && menu_is_active(GMnu_CREATURE_WINDOW));
    const TbBool seen = looking && !thing_is_invalid(cw_thing());
    if (seen && (menu_is_active(GMnu_CREATURE_QUERY1) || menu_is_active(GMnu_CREATURE_QUERY2) || menu_is_active(GMnu_CREATURE_QUERY3) ||
        menu_is_active(GMnu_CREATURE_QUERY4) || menu_is_active(GMnu_CREATURE_QUERY5)))
    {
        /* (a page of the Information Panel came up some other way: the window has what it had) */
        turn_off_menu(GMnu_CREATURE_QUERY1);
        turn_off_menu(GMnu_CREATURE_QUERY2);
        turn_off_menu(GMnu_CREATURE_QUERY3);
        turn_off_menu(GMnu_CREATURE_QUERY4);
        turn_off_menu(GMnu_CREATURE_QUERY5);
        initialise_tab_tags_and_menu(GMnu_QUERY);
        turn_on_menu(GMnu_QUERY);
    }
    /* (not while the World Boss window is up: one window in the middle of the view at a time; it is back when that
       one closes, if the creature is still looked at) */
    const TbBool want = seen && !menu_is_active(GMnu_WORLD_BOSS_PICK);
    if (want)
    {
        if (!menu_is_active(GMnu_CREATURE_WINDOW))
        {
            turn_on_menu(GMnu_CREATURE_WINDOW);
            const MenuNumber num = menu_id_to_number(GMnu_CREATURE_WINDOW);
            if (num >= 0)
                cw_place(get_active_menu(num)); /* (at once: the mouse is tested against the buttons before the first frame) */
        }
    } else if (menu_is_active(GMnu_CREATURE_WINDOW))
        turn_off_menu(GMnu_CREATURE_WINDOW);
}

static const char *cw_kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *name = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((name != NULL) && (name[0] != 0)) ? name : creature_code_name(model);
}

/** The game's ornate frame (draw_ornate_slab64k) at the window's own scale: the game's draws its ornaments at the
 *  screen's scale whatever the window's, so on a narrow view they would stand a size too large around a smaller
 *  window, and over the side panel. */
static void cw_frame(int pos_x, int pos_y, int width, int height, int upp)
{
#define F(v) ((v) * upp / 16)
    draw_slab64k_background(pos_x, pos_y, width, height);
    const struct TbSprite *spr = get_button_sprite(GBS_parchment_map_frame_deco_a_tl);
    if ((spr == NULL) || (spr->SWidth == 0))
        return;
    const int su = max((2048 / spr->SWidth) * upp / 16, 1);
    const int step = max(F(32), 1);
    for (int i = F(10); i < width - F(12); i += step)
    {
        LbSpriteDrawResized(pos_x + i, pos_y - F(4), su, get_button_sprite(GBS_borders_frame_thin_tc));
        LbSpriteDrawResized(pos_x + i, pos_y + height, su, get_button_sprite(GBS_borders_frame_thin_bc));
    }
    for (int i = F(10); i < height - F(16); i += step)
    {
        LbSpriteDrawResized(pos_x - F(4), pos_y + i, su, get_button_sprite(GBS_borders_frame_thin_ml));
        LbSpriteDrawResized(pos_x + width, pos_y + i, su, get_button_sprite(GBS_borders_frame_thin_mr));
    }
    LbSpriteDrawResized(pos_x - F(4), pos_y - F(4), su, get_button_sprite(GBS_borders_frame_thin_tl));
    LbSpriteDrawResized(pos_x + width - F(28), pos_y - F(4), su, get_button_sprite(GBS_borders_frame_thin_tr));
    LbSpriteDrawResized(pos_x - F(4), pos_y + height - F(28), su, get_button_sprite(GBS_borders_frame_thin_bl));
    LbSpriteDrawResized(pos_x + width - F(28), pos_y + height - F(28), su, get_button_sprite(GBS_borders_frame_thin_br));
    LbSpriteDrawResized(pos_x - F(32), pos_y - F(14), su, get_button_sprite(GBS_parchment_map_frame_deco_a_tl));
    LbSpriteDrawResized(pos_x - F(34), pos_y + height - F(78), su, get_button_sprite(GBS_parchment_map_frame_deco_a_bl));
    RendererAddDrawFlags(Lb_SPRITE_FLIP_HORIZ);
    LbSpriteDrawResized(pos_x + width - F(96), pos_y - F(14), su, get_button_sprite(GBS_parchment_map_frame_deco_a_tl));
    LbSpriteDrawResized(pos_x + width - F(92), pos_y + height - F(78), su, get_button_sprite(GBS_parchment_map_frame_deco_a_bl));
    RendererClearDrawFlags(Lb_SPRITE_FLIP_HORIZ);
#undef F
}

/** The window's own drawing, under its buttons: the frame, the title, the portrait, the name with the health behind
 *  it, and the Rap Sheet's text. */
static void cw_background(struct GuiMenu *gmnu)
{
    cw_place(gmnu);
    const int upp = cw_upp;
#define U(v) ((v) * upp / 16)
    const int gx = gmnu->pos_x, gy = gmnu->pos_y;
    cw_frame(gx, gy, gmnu->width, gmnu->height, upp);
    struct Thing *thing = cw_thing();
    if (thing_is_invalid(thing))
        return;
    /* the title: the creature's kind, its capitals 14 high on the middle of the strip above the parts */
    world_window_display(gx + U(24), gy + U(15), gmnu->width - U(48), U(14), 1, cw_kind_name(thing->model));
    /* thin lines between the three parts */
    const TbPixel line = world_window_ink(WwI_Rule);
    const int lw = max((upp + 8) / 32, 1);
    LbDrawBox(gx + U(CW_MID_X - 9), gy + U(CW_TOP + 2), lw, U(CW_PART_H - 2), line);
    LbDrawBox(gx + U(CW_RIGHT_X - 7), gy + U(CW_TOP + 2), lw, U(CW_PART_H - 2), line);
    /* the portrait in its frame, as the panel's first page has it (their sizes: shares of a panel's width) */
    {
        const int pp = U(115);
        const int px0 = gx + U(CW_LEFT_X), py0 = gy + U(CW_TOP + 10);
        if (thing->ccontrol_idx > 0)
        {
            const long spr_idx = get_creature_model_graphics(thing->model, CGI_QuerySymbol);
            if (spr_idx > 0)
            {
                const struct TbSprite *spr = get_button_sprite_for_player(spr_idx, thing->owner);
                if ((spr != NULL) && (spr->SWidth > 0))
                    LbSpriteDrawResized(px0 + 12 * pp / 140, py0 + 12 * pp / 140, (pp * 35 / 100) * 16 / spr->SWidth, spr);
            }
        }
        const struct TbSprite *spr = get_panel_sprite(GPS_rpanel_frame_double_hex_med);
        if ((spr != NULL) && (spr->SWidth > 0))
            draw_gui_panel_sprite_left(px0, py0, (pp * 52 / 100) * 16 / spr->SWidth, GPS_rpanel_frame_double_hex_med);
    }
    /* the name bar: the health behind the name (what is lost is black, from the right), the name on its middle */
    {
        const int pn = U(118);
        const int nx = gx + U(CW_LEFT_X), ny = gy + U(CW_TOP + 82);
        const struct TbSprite *spr = get_panel_sprite(GPS_rpanel_bar_long_full);
        if ((spr != NULL) && (spr->SWidth > 0))
            draw_gui_panel_sprite_left(nx, ny, (pn * 95 / 100) * 16 / spr->SWidth, GPS_rpanel_bar_long_full);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl) && (cctrl->max_health > 0))
        {
            const HitPoints now = max((HitPoints)0, min(thing->health, cctrl->max_health));
            long lost = 126 - 2 * (63 * (long)now / cctrl->max_health);
            lost = max(0L, min(lost, 126L));
            LbDrawBox(nx + ((128 - lost) * pn + 70) / 140, ny + (4 * pn + 70) / 140, (lost * pn + 70) / 140, (14 * pn + 70) / 140,
                colours[0][0][0]);
        }
        /* (the bar's inside is 14 of the panel's units high, 4 below its top; the pencil stands at its right end) */
        const int cap = 11 * pn / 140;
        /* (the names rule: the name its keeper gave it, from level 10; else its kind - never a made-up name) */
        const char *given = world_misdeeds_shown_name(thing);
        world_window_display(nx + 6 * pn / 140, ny + 4 * pn / 140 + (14 * pn / 140 - cap) / 2, 104 * pn / 140, cap, 1,
            (given != NULL) ? given : cw_kind_name(thing->model));
    }
    /* the Rap Sheet (its buttons are this menu's, placed by the sheet) */
    world_misdeeds_window_draw(gx + U(CW_RIGHT_X), gy + U(CW_TOP), U(CW_RIGHT_W), U(CW_PART_H), upp);
#undef U
}

/** Close: the Information tool lets go of the creature, as a right click does. */
static void cw_close_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    turn_off_query_menus();
    turn_on_main_panel_menu();
    set_players_packet_action(get_my_player(), PckA_SetPlyrState, PSt_CtrlDungeon, 0, 0, 0);
}

/** The Close button: the game's own button bar, lit while the mouse is on it, its word on its middle. */
static void cw_close_draw(struct GuiButton *gbtn)
{
    const TbBool over = (gbtn->flags & LbBtnF_MouseOver) != 0;
    const int bu = simple_button_sprite_height_units_per_px(gbtn, GBS_frontend_button_std_c, 100);
    const int cap = gbtn->height * 15 / 26;
    if (gbtn->width < 72 * bu / 16)
        return; /* (smaller than the bar's own ends: nothing to draw it with) */
    draw_bar64k(gbtn->scr_pos_x, gbtn->scr_pos_y, bu, gbtn->width);
    if (over)
    {
        /* (the lit bar carries a glowing rim: it goes that rim up and to the left of the bar and is that much wider) */
        const int rim = 6 * bu / 16;
        draw_lit_bar64k(gbtn->scr_pos_x - rim, gbtn->scr_pos_y - rim, bu, gbtn->width + rim);
    }
    world_window_display(gbtn->scr_pos_x + gbtn->height / 2, gbtn->scr_pos_y + (gbtn->height - cap) / 2, gbtn->width - gbtn->height,
        cap, 1, "Close");
}

/* a button's place in the window's units (cw_place puts it there at the window's scale) */
#define CW_AT(x, y, w, h) (x), (y), (x), (y), (w), (h)
/* an ability's slot: a column (0, 1) and a row (0 to 4) of the left part, under the portrait and the name */
#define CW_ABILITY(slot) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, \
    CW_AT(CW_LEFT_X + 60 * ((slot) % 2), CW_TOP + 123 + 30 * ((slot) / 2), 52, 21), \
    gui_area_instance_button, 0, GUIStr_Empty, 0, {.lval = (slot)}, 0, maintain_instance }
/* a statistic: its place (0 to 19: two columns, ten rows), its picture, its tooltip and which one it is */
#define CW_STAT(at, sprite, tip, stat) {LbBtnT_NormalBtn, BID_QUERY_INFO, 0, 0, NULL, NULL, NULL, 0, \
    CW_AT(CW_MID_X + 60 * ((at) % 2), CW_TOP + 9 + 26 * ((at) / 2), 52, 21), \
    gui_area_stat_button, (sprite), (tip), 0, {.lval = (stat)}, 0, NULL }
/* a button of the Rap Sheet: the sheet's drawing puts it where its row is (no size here: cw_place leaves it alone) */
#define CW_SHEET(click, draw, maintain, value) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, (click), NULL, NULL, 0, \
    CW_AT(CW_RIGHT_X, CW_TOP, 0, 0), (draw), 0, GUIStr_Empty, 0, {.lval = (value)}, 0, (maintain) }

static struct GuiButtonInit world_creature_window_buttons[] = {
  /* the window's own ground, under every other button: a right click on it lets go of the creature, as one outside
     the window does (the buttons made after it are looked at first) */
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, cw_close_click, NULL, 0, CW_AT(0, 0, CW_W, CW_H), NULL, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  /* the mood and the level, right of the portrait */
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, CW_AT(CW_LEFT_X + 63, CW_TOP + 25, 49, 21), gui_area_smiley_anger_button, GPS_rpanel_bar_rounded_empty, GUIStr_CreatureAngerDesc, 0, {.lval = 0}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, CW_AT(CW_LEFT_X + 63, CW_TOP + 51, 49, 21), gui_area_experience_button, GPS_rpanel_bar_rounded_full, GUIStr_ExperienceDesc, 0, {.lval = 0}, 0, NULL },
  /* the name: a click goes to the keeper's next creature, a right click to its next of the same kind; the pencil renames */
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, gui_query_next_creature_of_owner, gui_query_next_creature_of_owner_and_model, NULL, 0, CW_AT(CW_LEFT_X + 3, CW_TOP + 85, 94, 12), NULL, 0, GUIStr_NameAndHealthDesc, 0, {.lval = 0}, 0, NULL },
  {LbBtnT_EditBox, BID_DEFAULT, 0, 0, world_misdeeds_rename_done, NULL, NULL, 0, CW_AT(CW_LEFT_X + 98, CW_TOP + 86, 10, 10), world_misdeeds_pencil_draw, 0, GUIStr_Empty, 0, {.lval = 0}, WORLD_MISDEED_NAME_MAX, world_misdeeds_pencil_maintain },
  CW_ABILITY(0), CW_ABILITY(1), CW_ABILITY(2), CW_ABILITY(3), CW_ABILITY(4),
  CW_ABILITY(5), CW_ABILITY(6), CW_ABILITY(7), CW_ABILITY(8), CW_ABILITY(9),
  CW_STAT(0, GPS_crspell_heal_std_s, GUIStr_CreatureHealthDesc, CrLStat_Health),
  CW_STAT(1, GPS_symbols_creatr_stat_strength_std, GUIStr_CreatureStrengthDesc, CrLStat_Strength),
  CW_STAT(2, GPS_symbols_creatr_stat_armor_std, GUIStr_CreatureArmourDesc, CrLStat_Armour),
  CW_STAT(3, GPS_symbols_creatr_stat_defense_std, GUIStr_CreatureDefenceDesc, CrLStat_Defence),
  CW_STAT(4, GPS_symbols_creatr_stat_luck_std, GUIStr_CreatureLuckDesc, CrLStat_Luck),
  CW_STAT(5, GPS_symbols_creatr_stat_dexterity_std, GUIStr_CreatureDexterityDesc, CrLStat_Dexterity),
  CW_STAT(6, GPS_symbols_creatr_stat_wage_std, GUIStr_CreatureWageDesc, CrLStat_GoldWage),
  CW_STAT(7, GPS_symbols_creatr_stat_gold_std, GUIStr_CreatureGoldHeldDesc, CrLStat_GoldHeld),
  CW_STAT(8, GPS_symbols_creatr_stat_age_std, GUIStr_CreatureTimeInDungeonDesc, CrLStat_AgeTime),
  CW_STAT(9, GPS_symbols_creatr_stat_kills_std, GUIStr_CreatureKillsDesc, CrLStat_Kills),
  CW_STAT(10, GPS_crspell_speedup_dis_s, GUIStr_CreatureSpeedDesc, CrLStat_Speed),
  CW_STAT(11, GPS_crspell_whip_std_s, GUIStr_CreatureLoyaltyDesc, CrLStat_Loyalty),
  CW_STAT(12, GPS_room_research_std_s, GUIStr_CreatureResrchSkillDesc, CrLStat_ResearchSkill),
  CW_STAT(13, GPS_room_workshop_std_s, GUIStr_CreatureManfctrSkillDesc, CrLStat_ManufactureSkill),
  CW_STAT(14, GPS_room_training_std_s, GUIStr_CreatureTraingSkillDesc, CrLStat_TrainingSkill),
  CW_STAT(15, GPS_room_scavenge_std_s, GUIStr_CreatureScavngSkillDesc, CrLStat_ScavengeSkill),
  CW_STAT(16, GPS_symbols_creatr_stat_traingcst_std, GUIStr_CreatureTraingCostDesc, CrLStat_TrainingCost),
  CW_STAT(17, GPS_symbols_creatr_stat_scavngcst_std, GUIStr_CreatureScavngCostDesc, CrLStat_ScavengeCost),
  CW_STAT(18, GPS_trapdoor_trap_boulder_std_s, GUIStr_CreatureWeightDesc, CrLStat_Weight),
  CW_STAT(19, GPS_symbols_creatr_stat_blood_std, GUIStr_CreatureBloodTypeDesc, CrLStat_BloodType),
  /* the Rap Sheet's buttons: two picks (a vice or a lesson), five vices to mark, and the rows of training, pay and post */
  CW_SHEET(world_misdeeds_pick_click, world_misdeeds_pick_draw, world_misdeeds_pick_maintain, 0),
  CW_SHEET(world_misdeeds_pick_click, world_misdeeds_pick_draw, world_misdeeds_pick_maintain, 1),
  CW_SHEET(world_misdeeds_mark_click, world_misdeeds_mark_draw, world_misdeeds_mark_maintain, 0),
  CW_SHEET(world_misdeeds_mark_click, world_misdeeds_mark_draw, world_misdeeds_mark_maintain, 1),
  CW_SHEET(world_misdeeds_mark_click, world_misdeeds_mark_draw, world_misdeeds_mark_maintain, 2),
  CW_SHEET(world_misdeeds_mark_click, world_misdeeds_mark_draw, world_misdeeds_mark_maintain, 3),
  CW_SHEET(world_misdeeds_mark_click, world_misdeeds_mark_draw, world_misdeeds_mark_maintain, 4),
  CW_SHEET(world_misdeeds_regimen_click, world_misdeeds_regimen_draw, world_misdeeds_regimen_maintain, 0),
  CW_SHEET(world_misdeeds_regimen_click, world_misdeeds_regimen_draw, world_misdeeds_regimen_maintain, 1),
  CW_SHEET(world_misdeeds_regimen_click, world_misdeeds_regimen_draw, world_misdeeds_regimen_maintain, 2),
  /* ... and the bar the sheet scrolls with where it is higher than its room */
  CW_SHEET(world_misdeeds_scroll_click, world_misdeeds_scroll_draw, world_misdeeds_scroll_maintain, 0),
  /* Close, on the middle of the window's foot */
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, cw_close_click, NULL, NULL, 0, CW_AT((CW_W - 124) / 2, CW_H - 35, 124, 26), cw_close_draw, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  {              -1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, {.lval = 0}, 0, NULL },
};

struct GuiMenu world_creature_window_menu =
 { GMnu_CREATURE_WINDOW, 0, 1, world_creature_window_buttons, POS_GAMECTR, POS_GAMECTR, CW_W, CW_H, cw_background, 0, NULL, NULL, 0, 0, 0,};

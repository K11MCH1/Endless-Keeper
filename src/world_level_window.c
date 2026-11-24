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
#include "world_creature_window.h"
#include "world_keeper_panel.h"
#include "world_progress.h"
#include "world_stream.h"
#include "thing_creature.h"
#include "thing_list.h"
#include "creature_jobs.h"
#include "thing_navigate.h"
#include "config_creature.h"
#include "post_inc.h"

/* The window in its own units (as the Keeper panel's: lw_place sets the scale) */
#define LW_W 520
#define LW_H 332
#define LW_EDGE 24
#define LW_TOP 50                    /* under the title */
#define LW_CELL_W 58
#define LW_CELL_H 70
#define LW_ICON 46
#define LW_COLS 4
#define LW_GRID_Y (LW_TOP + 26)
#define LW_ROOMS_X LW_EDGE
#define LW_SPELLS_X (LW_W / 2 + 12)
#define LW_FOOT_Y (LW_H - 36)
#define LW_ORN_SIDE 34
#define LW_ORN_TOP 14
#define LW_ORN_BOTTOM 30
#define LW_AIR 2
#define LW_ROOMS 10
#define LW_SPELLS 7
#define LW_SPELL_BASE 100            /* a cell's lval: rooms 0-9, spells 100-106 */

/* the book */
#define LB_ANIM 777                  /* the Library's spellbooks' keeper sprite (objects.cfg SPELLBOOK_*) */
#define LB_FRAMES 16
#define LB_RISE_MS 450
#define LB_LIFT 640                  /* map units over the floor it floats at */

static int lw_upp = 16;
static int lw_hover = -1;            /* the cell the mouse is on (its lval), -1 none */
static int lw_test_hover = -1;       /* the test hook's */
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

static void lw_place(struct GuiMenu *gmnu)
{
    const int sw = MyScreenWidth, sh = MyScreenHeight;
    int upp = sw * 16 * 88 / 100 / (LW_W + 2 * (LW_ORN_SIDE + LW_AIR));
    upp = min(upp, sh * 16 * 86 / 100 / (LW_H + LW_ORN_TOP + LW_ORN_BOTTOM + 2 * LW_AIR));
    upp = max(upp, 4);
    lw_upp = upp;
    gmnu->width = LW_W * upp / 16;
    gmnu->height = LW_H * upp / 16;
    const int orn_top = LW_ORN_TOP * upp / 16, orn_bottom = LW_ORN_BOTTOM * upp / 16;
    gmnu->pos_x = (sw - gmnu->width) / 2;
    gmnu->pos_y = (sh - gmnu->height - orn_top - orn_bottom) / 2 + orn_top;
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
        gbtn->scr_pos_x = gmnu->pos_x + init->scr_pos_x * upp / 16;
        gbtn->scr_pos_y = gmnu->pos_y + init->scr_pos_y * upp / 16;
        gbtn->pos_x = gbtn->scr_pos_x;
        gbtn->pos_y = gbtn->scr_pos_y;
        gbtn->width = init->width * upp / 16;
        gbtn->height = init->height * upp / 16;
    }
}

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
            lw_place(get_active_menu(num));
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
    const int nr = world_level_entries(my_player_number, 0, rooms, LW_ROOMS);
    const int ns = world_level_entries(my_player_number, 1, spells, LW_SPELLS);
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

static void lw_box(int x, int y, int w, int h, TbPixel ground, TbPixel rule)
{
    const int lw = max((lw_upp + 8) / 32, 1);
    LbDrawBox(x, y, w, h, ground);
    LbDrawBox(x, y, w, lw, rule);
    LbDrawBox(x, y + h - lw, w, lw, rule);
    LbDrawBox(x, y, lw, h, rule);
    LbDrawBox(x + w - lw, y, lw, h, rule);
}

/** A progress bar (the user, 2026-11-22: "just place a bar, dont add a timer or any numbers, besides the current
 *  Level"): a sunken well, its part done in gold. */
static void lw_bar(int x, int y, int w, int h, long have, long need)
{
    lw_box(x, y, w, h, colours[0][0][0], world_window_ink(WwI_Rule));
    const int lw = max((lw_upp + 8) / 32, 1);
    if ((need <= 0) || (w <= 2 * lw) || (h <= 2 * lw))
        return;
    const double part = (double)max(0L, min(have, need)) / (double)need;
    const int fill = (int)((w - 2 * lw) * part);
    if (fill > 0)
        LbDrawBox(x + lw, y + lw, fill, h - 2 * lw, world_window_ink(WwI_Gold));
}

static void lw_background(struct GuiMenu *gmnu)
{
    lw_place(gmnu);
    const int upp = lw_upp;
#define U(v) ((v) * upp / 16)
    const int gx = gmnu->pos_x, gy = gmnu->pos_y;
    world_window_frame(gx, gy, gmnu->width, gmnu->height, upp);
    world_window_display(gx + U(130), gy + U(16), U(LW_W - 260), U(18), 1, "Rooms & Spells To Level");
    const TbPixel rule = world_window_ink(WwI_Rule);
    const int lw = max((upp + 8) / 32, 1);
    LbDrawBox(gx + U(LW_EDGE), gy + U(LW_TOP - 6), U(LW_W - 2 * LW_EDGE), lw, rule);
    LbDrawBox(gx + U(LW_W / 2), gy + U(LW_TOP + 2), lw, U(LW_FOOT_Y - LW_TOP - 10), rule);
    world_window_display(gx + U(LW_ROOMS_X), gy + U(LW_TOP + 4), U(LW_COLS * LW_CELL_W), U(12), 1, "ROOMS");
    world_window_display(gx + U(LW_SPELLS_X), gy + U(LW_TOP + 4), U(LW_COLS * LW_CELL_W), U(12), 1, "SPELLS");
    LbDrawBox(gx + U(LW_EDGE), gy + U(LW_FOOT_Y - 8), U(LW_W - 2 * LW_EDGE), lw, rule);
    /* the foot: where the Library's research goes now, and its bar */
    struct WorldLevelEntry e = {0};
    TbBool found = false;
    for (int k = 0; (k < LW_ROOMS) && !found; k++)
        found = lw_entry_of(k, &e) && e.focus;
    for (int k = 0; (k < LW_SPELLS) && !found; k++)
        found = lw_entry_of(LW_SPELL_BASE + k, &e) && e.focus;
    const int cap = max(U(9), world_window_plain_cap());
    const int fy = gy + U(LW_FOOT_Y);
    if (found)
    {
        char line[96];
        const TbBool working = (world_level_speed(my_player_number, 0, NULL) > 0);
        snprintf(line, sizeof(line), "%s %s", working ? "Levelling:" : "Library idle:", e.label);
        world_window_plain(gx + U(LW_EDGE), fy + (U(22) - cap) / 2, U(200), cap, 0,
            world_window_ink(working ? WwI_Bright : WwI_Dim), line);
        lw_bar(gx + U(LW_EDGE + 210), fy + U(5), U(LW_W - 2 * LW_EDGE - 210), U(12), e.bar, e.need);
    } else
        world_window_plain(gx + U(LW_EDGE), fy + (U(22) - cap) / 2, U(LW_W - 2 * LW_EDGE), cap, 1,
            world_window_ink(WwI_Dim), "Nothing to level: build a Library and gain rooms and spells first.");
#undef U
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

static void lw_close_draw(struct GuiButton *gbtn)
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

static void lw_cell_click(struct GuiButton *gbtn)
{
    struct WorldLevelEntry e = {0};
    if (!lw_entry_of(gbtn->content.lval, &e) || !e.owned)
        return;
    set_players_packet_action(get_my_player(), PckA_WorldLevelFocus, (long)e.entry, 0, 0, 0);
    SYNCLOG("World: the Library's research goes to the %s (chosen in the window)", e.label);
}

static void lw_cell_draw(struct GuiButton *gbtn)
{
    const int lval = gbtn->content.lval;
    if ((gbtn->flags & LbBtnF_MouseOver) != 0)
        lw_hover = lval;
    else if (lw_hover == lval)
        lw_hover = -1;
    struct WorldLevelEntry e = {0};
    if (!lw_entry_of(lval, &e))
        return;
    const int upp = lw_upp;
    const int icon = LW_ICON * upp / 16;
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
        lw_bar(x, y + fh + 3 * upp / 16 + cap + 3 * upp / 16, icon, max(4 * upp / 16, 3), e.bar, e.need);
}

/** The tooltip of the cell under the mouse, over everything in the window. */
static void lw_tooltip(void)
{
    const int lval = (lw_test_hover >= 0) ? lw_test_hover : lw_hover;
    struct WorldLevelEntry e = {0};
    if ((lval < 0) || !lw_entry_of(lval, &e))
        return;
    const int upp = lw_upp;
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
    lw_box(x, y, w, h, colours[1][1][1], world_window_ink(WwI_Gold));
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

#define LW_AT(x, y, w, h) (x), (y), (x), (y), (w), (h)
#define LW_CELL(v, x0, k) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, lw_cell_click, NULL, NULL, 0, \
    LW_AT((x0) + LW_CELL_W * ((k) % LW_COLS), LW_GRID_Y + LW_CELL_H * ((k) / LW_COLS), LW_CELL_W - 6, LW_CELL_H - 8), \
    lw_cell_draw, 0, GUIStr_Empty, 0, {.lval = (v)}, 0, NULL }
#define LW_ROOM(k) LW_CELL((k), LW_ROOMS_X, (k))
#define LW_SPELL(k) LW_CELL(LW_SPELL_BASE + (k), LW_SPELLS_X, (k))

static struct GuiButtonInit world_level_window_buttons[] = {
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, lw_ground_rclick, NULL, 0, LW_AT(0, 0, LW_W, LW_H), NULL, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, lw_close_click, NULL, NULL, 0, LW_AT(LW_W - LW_EDGE - 96, 12, 96, 24), lw_close_draw, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  LW_ROOM(0), LW_ROOM(1), LW_ROOM(2), LW_ROOM(3), LW_ROOM(4), LW_ROOM(5), LW_ROOM(6), LW_ROOM(7), LW_ROOM(8), LW_ROOM(9),
  LW_SPELL(0), LW_SPELL(1), LW_SPELL(2), LW_SPELL(3), LW_SPELL(4), LW_SPELL(5), LW_SPELL(6),
  {              -1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, {.lval = 0}, 0, NULL },
};

struct GuiMenu world_level_window_menu =
 { GMnu_LEVEL_WINDOW, 0, 1, world_level_window_buttons, POS_GAMECTR, POS_GAMECTR, LW_W, LW_H, lw_background, 0, NULL, NULL, 0, 0, 0,};

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
    int32_t ax, ay, bx, by;
    map_to_screen(&a, &ax, &ay);
    map_to_screen(&b, &bx, &by);
    const double slab_px = sqrt((double)(bx - ax) * (bx - ax) + (double)(by - ay) * (by - ay));
    const long scale = max(24L, min((long)(slab_px * 1.6), 800L)); /* (32 = the sprite's own pixels: about a slab tall) */
    /* the rise: from the floor up, out of nothing, slowing as it arrives; then it floats */
    const long age = (long)(now - lb_since);
    double t = (double)age / LB_RISE_MS;
    if (t > 1.0)
        t = 1.0;
    const double ease = 1.0 - (1.0 - t) * (1.0 - t);
    pos.z.val += (MapCoordDelta)(LB_LIFT * ease);
    if (t >= 1.0)
        pos.z.val += (MapCoordDelta)(28.0 * sin((double)(age - LB_RISE_MS) / 420.0));
    int32_t sx, sy;
    if (!map_to_screen(&pos, &sx, &sy))
    {
        lb_x1 = lb_y1 = lb_x2 = lb_y2 = 0;
        return;
    }
    TbDrawFlagsMask fade = 0;
    if (t < 0.12)
        fade = (TbDrawFlagsMask)-1; /* not yet */
    else if (t < 0.40)
        fade = Lb_SPRITE_TRANSPAR8;
    else if (t < 0.70)
        fade = Lb_SPRITE_TRANSPAR4;
    else if (t < 0.90)
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

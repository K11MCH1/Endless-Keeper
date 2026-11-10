/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file chat_box.c
 *     Endless Keeper: the chat box of a network endless game (2026-11-10).
 * @par Purpose:
 *     Chat and the server's lines ("Server: ...", "You were muted.", the answers to ! commands) go into a box at the
 *     bottom of the screen in the style of the game's message dialogs, instead of the top of the screen, where the
 *     game's own news stays. The chat icon (the bottom of the event column; the events show one slot up) opens and
 *     closes it; its eye leaves only the lines, which stay a while and fade; its tick closes it. Up and down (and the
 *     mouse wheel over it) go through the last lines. The box stays at the bottom: a message dialog opening closes
 *     it, and the box opening closes the dialog (2026-11-10). The choice is kept in settings.toml. Single player and
 *     other games are as before.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "chat_box.h"
#include <stdio.h>
#include <string.h>

#include "globals.h"
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_guibtns.h"
#include "bflib_keybrd.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "config_settings.h"
#include "config_spritecolors.h"
#include "config_strings.h"
#include "custom_sprites.h"
#include "front_input.h"
#include "frontend.h"
#include "frontmenu_ingame_evnt.h"
#include "front_simple.h"
#include "game_legacy.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "gui_msgs.h"
#include "keeperfx.hpp"
#include "kjm_input.h"
#include "local_camera.h"
#include "map_events.h"
#include "net_state.h"
#include "player_data.h"
#include "server_roles.h"
#include "sprites.h"
#include "vidmode.h"
#include "world_stream.h"
#include "post_inc.h"

/******************************************************************************/
#define CB_LINES       60    /* lines kept */
#define CB_TEXT        160
#define CB_W           480   /* the box, in menu units (as the message dialogs) */
#define CB_H           112
#define CB_TEXT_X      40
#define CB_TEXT_W      400
#define CB_SHOW_MS     8000  /* a line alone (the eye) stays this long, */
#define CB_FADE_MS     3000  /* then fades (darker and darker, the last steps see-through) */
#define CB_FADE_STEPS  7
#define CB_FADED_ROWS  6
#define CB_MAX_ROWS    24
#define CB_WRAP_ROWS   8

struct ChatBoxLine {
    PlayerNumber plyr; /* -1: the server's line */
    TbClockMSec at;
    char text[CB_TEXT];
};

static struct ChatBoxLine cb_lines[CB_LINES];
static int cb_first;  /* the oldest line */
static int cb_count;
static int cb_back;   /* lines scrolled back (0: the newest is at the bottom) */
static int cb_mode = CBM_Open;
static TbBool cb_unread; /* new lines while closed: the icon blinks */

static const char *cb_mode_names[CBM_Count] = {"open", "faded (the eye)", "closed"};

static const struct ChatBoxLine *cb_line(int i)
{
    return &cb_lines[(cb_first + i) % CB_LINES];
}

static TbBool cb_typing(void)
{
    const struct UserState *ustate = get_local_user_state();
    return (ustate != NULL) && ((ustate->init_flags & UsrIF_NewMPMessage) != 0);
}

TbBool chat_box_active(void)
{
    return world_mode_active() && network_is_active() && !netstate_dedicated();
}

TbBool chat_box_draws_typing(void)
{
    return chat_box_active();
}

int chat_box_mode_setting(void)
{
    return cb_mode;
}

void chat_box_mode_load(int mode)
{
    if ((mode >= 0) && (mode < CBM_Count))
        cb_mode = mode;
}

static void cb_set_mode(int mode)
{
    if (mode == cb_mode)
        return;
    cb_mode = mode;
    if (mode != CBM_Closed)
        cb_unread = false;
    SYNCLOG("Chat box: %s", cb_mode_names[mode]);
    save_settings();
}

void chat_box_reset(void)
{
    cb_first = 0;
    cb_count = 0;
    cb_back = 0;
    cb_unread = false;
}

void chat_box_add(PlayerNumber plyr_idx, const char *text)
{
    struct ChatBoxLine *ln;
    if (cb_count < CB_LINES) {
        ln = &cb_lines[(cb_first + cb_count) % CB_LINES];
        cb_count++;
    } else {
        ln = &cb_lines[cb_first];
        cb_first = (cb_first + 1) % CB_LINES;
    }
    ln->plyr = plyr_idx;
    ln->at = LbTimerClock();
    snprintf(ln->text, sizeof(ln->text), "%s", text);
    if (cb_back > 0) /* scrolled back: the same lines stay in view */
        cb_back = min(cb_back + 1, cb_count - 1);
    if (cb_mode == CBM_Closed)
        cb_unread = true;
}

/******************************************************************************/
/* Drawing the lines */

/** The text's scale: the message dialogs' own (draw_scrolling_text_at). */
static int cb_text_units(void)
{
    if (dbc_initialized && dbc_enabled)
        return scale_value_by_horizontal_resolution((MyScreenWidth >= 640) ? 16 : 32);
    return scale_ui_value_lofi(16);
}

/** "<name>: " before a line: its role's name (and colour) from the server, else the keeper's name. tint: the role's
 *  colour as a remap of the font (NULL: none). */
static TbBool cb_sender(const struct ChatBoxLine *ln, char *prefix, size_t size, const unsigned char **tint)
{
    const char *name = NULL;
    TbBool coloured = false;
    uint32_t rgb = 0;
    *tint = NULL;
    if (ln->plyr < 0)
        return false;
    if (roles_client_player(ln->plyr, &name, &coloured, &rgb)) {
        if (coloured)
            *tint = message_chat_name_remap(rgb);
    } else {
        const struct PlayerInfo *player = get_player(ln->plyr);
        if (player_invalid(player) || (player->player_name[0] == '\0'))
            return false;
        name = player->player_name;
    }
    snprintf(prefix, size, "%s: ", name);
    return true;
}

/** Splits a line into rows (at spaces; a word wider than a row at a letter): returns the number of rows. */
static int cb_wrap(const char *text, int first_w, int rest_w, int units, short starts[], short lens[], int max_rows)
{
    const int len = (int)strlen(text);
    char buf[CB_TEXT + 1];
    int n = 0;
    int pos = 0;
    while ((pos < len) && (n < max_rows))
    {
        const int avail = (n == 0) ? first_w : rest_w;
        int end = pos;
        while (end < len)
        {
            int next = end;
            while ((next < len) && (text[next] == ' '))
                next++;
            while ((next < len) && (text[next] != ' '))
                next++;
            memcpy(buf, text + pos, next - pos);
            buf[next - pos] = '\0';
            if (LbTextStringWidthM(buf, units) > avail)
                break;
            end = next;
        }
        if (end == pos)
        {
            /* one word wider than the row: the letters that fit (a UTF-8 letter stays whole) */
            end = pos + 1;
            while ((end < len) && (((unsigned char)text[end] & 0xC0) == 0x80))
                end++;
            while (end < len)
            {
                int next = end + 1;
                while ((next < len) && (((unsigned char)text[next] & 0xC0) == 0x80))
                    next++;
                memcpy(buf, text + pos, next - pos);
                buf[next - pos] = '\0';
                if (LbTextStringWidthM(buf, units) > avail)
                    break;
                end = next;
            }
        }
        starts[n] = (short)pos;
        lens[n] = (short)(end - pos);
        n++;
        pos = end;
        while ((pos < len) && (text[pos] == ' '))
            pos++;
    }
    return n;
}

/** The font a fade step darker (step 1..CB_FADE_STEPS-1; the nearest colours the palette has). */
static const unsigned char *cb_dark_remap(int step)
{
    static unsigned char map[CB_FADE_STEPS][256];
    static TbBool made[CB_FADE_STEPS];
    if ((step <= 0) || (step >= CB_FADE_STEPS) || (engine_palette == NULL))
        return NULL;
    if (!made[step])
    {
        const int f = 64 - step * 52 / (CB_FADE_STEPS - 1); /* 64 (as is) down to 12 */
        for (int i = 0; i < 256; i++)
        {
            const unsigned char *c = &engine_palette[3 * i];
            map[step][i] = LbPaletteFindColour(engine_palette, (unsigned char)(c[0] * f / 64), (unsigned char)(c[1] * f / 64),
                (unsigned char)(c[2] * f / 64));
        }
        made[step] = true;
    }
    return map[step];
}

/** Text in the role's colour (tint, or NULL) and the fade's step (0: as is). */
static void cb_draw_text(long x, long y, int units, const char *text, const unsigned char *tint, int step)
{
    const unsigned char *dark = cb_dark_remap(step);
    const unsigned char *map = tint;
    unsigned char both[256];
    if (dark != NULL)
    {
        if (tint != NULL) {
            for (int i = 0; i < 256; i++)
                both[i] = dark[tint[i]];
            map = both;
        } else
            map = dark;
    }
    /* the last steps are see-through too */
    RendererSetDrawFlags((step >= CB_FADE_STEPS - 2) ? Lb_SPRITE_TRANSPAR4 : 0);
    if (map == NULL) {
        LbTextDrawResized(x, y, units, text);
        return;
    }
    const unsigned char *old_remap = LbTextGetRemap();
    LbTextSetRemap(map);
    LbTextDrawResized(x, y, units, text);
    LbTextSetRemap(old_remap);
}

struct CbRow {
    short line;
    short start;
    short len;
    TbBool first;
};

/** The lines in a text area (x, y, w, h on the screen), the newest at the bottom, with the typing line under them.
 *  faded: no box behind them; only lines younger than the fade, which fade; closed: only the typing line. */
static void cb_draw_lines(long x, long y, long w, long h, TbBool faded, TbBool closed)
{
    LbTextSetFont(winfont);
    const int units = cb_text_units();
    const int row_h = max(1, LbTextLineHeight() * units / 16);
    const int rows = clamp((int)(h / row_h), 1, faded ? CB_FADED_ROWS : CB_MAX_ROWS);
    const TbBool typing = cb_typing();
    const TbClockMSec now = LbTimerClock();
    const int indent = LbTextStringWidthM("  ", units);
    struct CbRow row[CB_MAX_ROWS];
    int nrows = 0;
    const int text_rows = rows - (typing ? 1 : 0);
    for (int i = cb_count - 1 - (faded ? 0 : cb_back); (i >= 0) && (nrows < text_rows) && !closed; i--)
    {
        const struct ChatBoxLine *ln = cb_line(i);
        if (faded && (now - ln->at >= CB_SHOW_MS + CB_FADE_MS))
            break;
        char prefix[40];
        const unsigned char *tint;
        const int name_w = cb_sender(ln, prefix, sizeof(prefix), &tint) ? LbTextStringWidthM(prefix, units) : 0;
        short starts[CB_WRAP_ROWS];
        short lens[CB_WRAP_ROWS];
        const int n = cb_wrap(ln->text, (int)w - name_w, (int)w - indent, units, starts, lens, CB_WRAP_ROWS);
        if (n == 0) {
            starts[0] = 0;
            lens[0] = 0;
        }
        for (int k = max(n, 1) - 1; (k >= 0) && (nrows < text_rows); k--)
        {
            row[nrows].line = (short)i;
            row[nrows].start = starts[k];
            row[nrows].len = lens[k];
            row[nrows].first = (k == 0);
            nrows++;
        }
    }
    const unsigned short flags_mem = RendererGetDrawFlags();
    LbTextSetWindow(x, y, w, h);
    /* in the box, the rows are centred as a whole; alone, they sit at the bottom */
    const int top = faded ? (int)h - rows * row_h : ((int)h - rows * row_h) / 2;
    if (typing)
    {
        char text[PLAYER_MP_MESSAGE_LEN + 4];
        snprintf(text, sizeof(text), ">%s_", local_state.mp_message_text);
        const char *shown = text;
        while ((shown[0] != '\0') && (LbTextStringWidthM(shown, units) > w))
            shown++; /* a long line shows its end */
        RendererSetDrawFlags(0);
        LbTextDrawResized(0, top + (rows - 1) * row_h, units, shown);
    }
    for (int r = 0; r < nrows; r++)
    {
        const struct ChatBoxLine *ln = cb_line(row[r].line);
        int step = 0; /* the fade: darker and darker, the last steps see-through */
        if (faded)
        {
            const TbClockMSec age = now - ln->at;
            if (age >= CB_SHOW_MS)
                step = min(CB_FADE_STEPS - 1, (int)((age - CB_SHOW_MS) * CB_FADE_STEPS / CB_FADE_MS) + 1);
        }
        const int ry = top + (rows - 1 - r - (typing ? 1 : 0)) * row_h;
        int rx = 0;
        if (row[r].first)
        {
            char prefix[40];
            const unsigned char *tint;
            if (cb_sender(ln, prefix, sizeof(prefix), &tint))
            {
                cb_draw_text(0, ry, units, prefix, tint, step);
                rx = LbTextStringWidthM(prefix, units);
            }
        } else
            rx = indent;
        char seg[CB_TEXT];
        memcpy(seg, ln->text + row[r].start, row[r].len);
        seg[row[r].len] = '\0';
        cb_draw_text(rx, ry, units, seg, NULL, step);
    }
    RendererSetDrawFlags(flags_mem);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
}

/******************************************************************************/
/* The box */

static void cb_area_draw(struct GuiButton *gbtn);
static void cb_area_maintain(struct GuiButton *gbtn);
static void cb_eye(struct GuiButton *gbtn);
static void cb_up(struct GuiButton *gbtn);
static void cb_down(struct GuiButton *gbtn);
static void cb_up_maintain(struct GuiButton *gbtn);
static void cb_down_maintain(struct GuiButton *gbtn);

static struct GuiButtonInit chat_box_buttons[] = {
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,    NULL, NULL, 0, CB_TEXT_X, 4, CB_TEXT_X, 4, CB_TEXT_W, CB_H - 8, cb_area_draw, 0, GUIStr_Empty, 0, {0}, 0, cb_area_maintain },
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, cb_eye,  NULL, NULL, 0,   4, 4,   4, 4, 30, 24, gui_area_new_normal_button, GPS_message_message_btn_show_act, GUIStr_Empty, 0, {0}, 0, NULL },
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, cb_up,   NULL, NULL, 0, 446, 4, 446, 4, 30, 24, gui_area_new_normal_button, GPS_message_message_btn_up_act, GUIStr_CtrlUp, 0, {0}, 0, cb_up_maintain },
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, cb_down, NULL, NULL, 0, 446, CB_H - 28, 446, CB_H - 28, 30, 24, gui_area_new_normal_button, GPS_message_message_btn_down_act, GUIStr_CtrlDown, 0, {0}, 0, cb_down_maintain },
  {               -1, BID_DEFAULT, 0, 0, NULL,    NULL, NULL, 0,   0, 0,   0, 0,  0,  0, NULL, 0, 0, 0, {0}, 0, NULL },
};

struct GuiMenu chat_box_menu =
 { GMnu_CHAT_BOX, 0, 4, chat_box_buttons, 160, POS_SCRBTM, CB_W, CB_H, gui_round_glass_background, 0, NULL, NULL, 0, 0, 0,};

/** The menus' scale (create_menu's). */
static int cb_menu_units(void)
{
    int units = min((int)units_per_pixel, units_per_pixel_min * 16 / 10);
    if (CB_W * units > RendererPhysicalWidth() * 16)
        units = RendererPhysicalWidth() * 16 / CB_W;
    if (CB_H * units > RendererPhysicalHeight() * 16)
        units = RendererPhysicalHeight() * 16 / CB_H;
    return max(units, 1);
}

/** The box's lowest edge: always the screen's bottom (the user, 2026-11-10: it no longer moves up above a message
 *  dialog; the two never show together, see chat_box_turn). */
static long cb_bottom(void)
{
    return MyScreenHeight;
}

/** A message dialog is up: an event's text, a battle, a dungeon special. */
static TbBool cb_dialog_up(void)
{
    return menu_is_active(GMnu_TEXT_INFO) || menu_is_active(GMnu_BATTLE) || menu_is_active(GMnu_DUNGEON_SPECIAL);
}

/** Where the box is: against the event column, to the screen's right edge, at the bottom. */
struct CbGeom {
    long x;
    long y;
    long w;
    long h;
    int units;
};

static void cb_geometry(struct CbGeom *g)
{
    g->units = cb_menu_units();
    long left = -1;
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (left < 0); i++)
    {
        const struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) != 0) && (gbtn->id_num == BID_MSG_EV01))
            left = gbtn->pos_x + gbtn->width;
    }
    if (left < 0) /* no side panel: where the message dialogs start */
        left = ((160L * (long)units_per_pixel) >> 4) * (long)pixel_size;
    g->x = max(0L, min(left, (long)MyScreenWidth - 64));
    g->w = MyScreenWidth - g->x;
    g->h = (CB_H * g->units + 8) / 16;
    g->y = max(0L, cb_bottom() - g->h);
}

static long cb_scaled(const struct CbGeom *g, int menu_units)
{
    return (menu_units * g->units + 8) / 16;
}

/** The box and its buttons: the eye at the left, up and down at the right, the lines between. */
static void cb_place(void)
{
    const MenuNumber mnu = menu_id_to_number(GMnu_CHAT_BOX);
    if (mnu < 0)
        return;
    struct GuiMenu *amnu = get_active_menu(mnu);
    struct CbGeom g;
    cb_geometry(&g);
    amnu->pos_x = g.x;
    amnu->pos_y = g.y;
    amnu->width = g.w;
    amnu->height = g.h;
    const long edge = cb_scaled(&g, 4);
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != mnu))
            continue;
        long bx = g.x + edge;
        long by = g.y + edge;
        if ((gbtn->click_event == cb_up) || (gbtn->click_event == cb_down)) {
            bx = g.x + g.w - edge - gbtn->width;
            if (gbtn->click_event == cb_down)
                by = g.y + g.h - edge - gbtn->height;
        } else if (gbtn->click_event != cb_eye) { /* the lines */
            bx = g.x + cb_scaled(&g, CB_TEXT_X);
            gbtn->width = max(1L, g.w - 2 * cb_scaled(&g, CB_TEXT_X));
        }
        gbtn->pos_x = bx;
        gbtn->scr_pos_x = bx;
        gbtn->pos_y = by;
        gbtn->scr_pos_y = by;
    }
}

/** The box's text area where the box would be (for the lines alone). */
static void cb_text_rect(long *x, long *y, long *w, long *h)
{
    struct CbGeom g;
    cb_geometry(&g);
    *x = g.x + cb_scaled(&g, CB_TEXT_X);
    *y = g.y + cb_scaled(&g, 4);
    *w = max(1L, g.w - 2 * cb_scaled(&g, CB_TEXT_X));
    *h = cb_scaled(&g, CB_H - 8);
}

static TbBool cb_box_wanted(void)
{
    if (!local_state.show_gui || (get_local_view_type() != PVT_DungeonTop))
        return false;
    return (cb_mode == CBM_Open) || cb_typing(); /* typing opens it for a while */
}

static TbBool cb_box_on_screen(void)
{
    const MenuNumber mnu = menu_id_to_number(GMnu_CHAT_BOX);
    return (mnu >= 0) && get_active_menu(mnu)->is_turned_on;
}

void chat_box_turn(void)
{
    /* the box and a message dialog share the bottom of the screen and never show together (the user, 2026-11-10):
       a dialog opening closes the box (as its icon would), and the box opening (its icon, or typing) closes the
       dialog that is up (as its tick would; its event stays in the column) */
    static TbBool dialog_was_up = false;
    static TbBool box_was_wanted = false;
    const TbBool dialog_up = chat_box_active() && cb_dialog_up();
    if (dialog_up && !dialog_was_up && (cb_mode == CBM_Open))
        cb_set_mode(CBM_Closed);
    const TbBool want = chat_box_active() && cb_box_wanted();
    if (want && !box_was_wanted && dialog_up && dialog_was_up)
    {
        SYNCLOG("Chat box: opened over a message dialog, which closes");
        turn_off_event_box_if_necessary(my_player_number, my_visible_event_idx);
        turn_off_menu(GMnu_TEXT_INFO);
        turn_off_menu(GMnu_BATTLE);
        turn_off_menu(GMnu_DUNGEON_SPECIAL);
    }
    dialog_was_up = chat_box_active() && cb_dialog_up();
    box_was_wanted = want;
    if (want && !menu_is_active(GMnu_CHAT_BOX))
        turn_on_menu(GMnu_CHAT_BOX);
    else if (!want && menu_is_active(GMnu_CHAT_BOX))
        turn_off_menu(GMnu_CHAT_BOX);
    if (want)
        cb_place();
}

static void cb_area_draw(struct GuiButton *gbtn)
{
    cb_draw_lines(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, false, false);
}

static void cb_scroll(int lines)
{
    cb_back = clamp(cb_back + lines, 0, max(0, cb_count - 1));
}

static void cb_area_maintain(struct GuiButton *gbtn)
{
    cb_place();
    if (!wheel_scrolled_up && !wheel_scrolled_down)
        return;
    const MenuNumber mnu = menu_id_to_number(GMnu_CHAT_BOX);
    if ((mnu < 0) || (first_monopoly_menu() >= 0))
        return;
    const struct GuiMenu *amnu = get_active_menu(mnu);
    const long mouse_x = GetMouseX();
    const long mouse_y = GetMouseY();
    if ((mouse_x < amnu->pos_x) || (mouse_x >= amnu->pos_x + amnu->width) || (mouse_y < amnu->pos_y)
     || (mouse_y >= amnu->pos_y + amnu->height))
        return;
    /* the wheel over the box goes through its lines (not the camera's zoom) */
    cb_scroll(wheel_scrolled_up ? 1 : -1);
    wheel_scrolled_up = false;
    wheel_scrolled_down = false;
    clear_key_pressed(KC_MOUSEWHEEL_UP);
    clear_key_pressed(KC_MOUSEWHEEL_DOWN);
}

static void cb_eye(struct GuiButton *gbtn)
{
    cb_set_mode(CBM_Faded);
}

static void cb_up(struct GuiButton *gbtn)
{
    cb_scroll(1);
}

static void cb_down(struct GuiButton *gbtn)
{
    cb_scroll(-1);
}

static void cb_set_enabled(struct GuiButton *gbtn, TbBool enabled)
{
    if (enabled)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void cb_up_maintain(struct GuiButton *gbtn)
{
    cb_set_enabled(gbtn, cb_back < cb_count - 1);
}

static void cb_down_maintain(struct GuiButton *gbtn)
{
    cb_set_enabled(gbtn, cb_back > 0);
}

void chat_box_draw_faded(void)
{
    if (!chat_box_active() || cb_box_on_screen())
        return;
    if (cb_dialog_up() && !cb_typing()) /* the lines alone would lie over the dialog at the bottom */
        return;
    const TbBool closed = (cb_mode == CBM_Closed);
    if (closed && !cb_typing())
        return;
    long x;
    long y;
    long w;
    long h;
    cb_text_rect(&x, &y, &w, &h);
    cb_draw_lines(x, y, w, h, true, closed);
}

/******************************************************************************/
/* The chat icon */

TbBool chat_box_icon_button(const struct GuiButton *gbtn)
{
    return (gbtn->content.lval == 0) && chat_box_active();
}

long event_button_slot(const struct GuiButton *gbtn)
{
    return gbtn->content.lval - (chat_box_active() ? 1 : 0);
}

void chat_box_icon_draw(struct GuiButton *gbtn)
{
    const int units = simple_gui_panel_sprite_height_units_per_px(gbtn, GPS_message_rpanel_msg_questn_act, 100);
    draw_gui_panel_sprite_left(gbtn->scr_pos_x, gbtn->scr_pos_y, units, GPS_message_rpanel_msg_blank_std);
    /* the keeper's face in it (as chat lines had it), which blinks for new lines while the box is closed */
    if (cb_unread && ((LbTimerClock() / 400) % 2 != 0))
        return;
    const struct TbSprite *face = get_panel_sprite(get_player_colored_icon_idx(GPS_plyrsym_symbol_player_red_std_b,
        my_player_number));
    if ((face == NULL) || (face->SHeight < 1) || (face->SWidth < 1))
        return;
    const long face_h = max(1, gbtn->height * 7 / 10);
    const long face_w = max(1L, face->SWidth * face_h / face->SHeight);
    const long fx = gbtn->scr_pos_x + (gbtn->width - face_w) / 2;
    const long fy = gbtn->scr_pos_y + (gbtn->height - face_h) / 2;
    if (!cb_box_on_screen() || (engine_palette == NULL))
    {
        LbSpriteDrawScaled(fx, fy, face, face_w, face_h);
        return;
    }
    /* the box is open: the face lit, as an event's icon is while its message shows (an orange and a yellow glow,
       the face brighter) */
    static unsigned char bright[256];
    static TbBool bright_made = false;
    if (!bright_made)
    {
        for (int i = 0; i < 256; i++)
        {
            const unsigned char *c = &engine_palette[3 * i];
            bright[i] = LbPaletteFindColour(engine_palette, (unsigned char)min(63, c[0] * 4 / 3 + 6),
                (unsigned char)min(63, c[1] * 4 / 3 + 6), (unsigned char)min(63, c[2] * 4 / 3 + 4));
        }
        bright_made = true;
    }
    const TbPixel orange = LbPaletteFindColour(engine_palette, 62, 30, 0);
    const TbPixel yellow = LbPaletteFindColour(engine_palette, 63, 56, 12);
    const long outer = max(2L, face_h / 10);
    const long inner = max(1L, outer / 2);
    static const int dirs[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    for (int d = 0; d < 8; d++)
        LbSpriteDrawScaledOneColour(fx + dirs[d][0] * outer, fy + dirs[d][1] * outer, face, face_w, face_h, orange);
    for (int d = 0; d < 8; d++)
        LbSpriteDrawScaledOneColour(fx + dirs[d][0] * inner, fy + dirs[d][1] * inner, face, face_w, face_h, yellow);
    LbSpriteDrawScaledRemap(fx, fy, face, face_w, face_h, bright);
}

void chat_box_icon_click(struct GuiButton *gbtn)
{
    cb_set_mode((cb_mode == CBM_Open) ? CBM_Closed : CBM_Open);
}

void chat_box_icon_maintain(struct GuiButton *gbtn)
{
    gbtn->btype_value &= ~LbBFeF_NoMouseOver;
    gbtn->flags |= LbBtnF_Enabled;
    gbtn->tooltip_stridx = GUIStr_Empty;
}
/******************************************************************************/
/* Test hook (KFX_NET_TESTSAY "#cb <what>", net_state.c): open, eye, close, icon (the icon's click), add <text> (a line), type <text>
   (the typing line), send (typing ends), up, down, event (the first event's dialog opens). */
void chat_box_test(const char *what)
{
    SYNCLOG("Chat box test: %s", what);
    if (strcmp(what, "open") == 0)
        cb_set_mode(CBM_Open);
    else if (strcmp(what, "eye") == 0)
        cb_set_mode(CBM_Faded);
    else if (strcmp(what, "close") == 0)
        cb_set_mode(CBM_Closed);
    else if (strcmp(what, "icon") == 0)
        chat_box_icon_click(NULL);
    else if (strcmp(what, "up") == 0)
        cb_scroll(1);
    else if (strcmp(what, "down") == 0)
        cb_scroll(-1);
    else if (strncmp(what, "add ", 4) == 0)
        chat_box_add(-1, what + 4);
    else if (strncmp(what, "type ", 5) == 0) {
        get_local_user_state()->init_flags |= UsrIF_NewMPMessage;
        snprintf(local_state.mp_message_text, sizeof(local_state.mp_message_text), "%s", what + 5);
    } else if (strcmp(what, "enter") == 0) {
        lbKeyOn[KC_RETURN] = 1; /* the typed line goes as with the Enter key */
    } else if (strcmp(what, "send") == 0) {
        get_local_user_state()->init_flags &= ~UsrIF_NewMPMessage;
        memset(local_state.mp_message_text, 0, sizeof(local_state.mp_message_text));
    } else if (strcmp(what, "event") == 0) {
        for (int i = 0; i <= EVENT_BUTTONS_COUNT; i++) {
            const EventIndex evidx = get_my_event_button_index(i);
            if (evidx != 0) {
                activate_event_box(evidx);
                break;
            }
        }
    }
}

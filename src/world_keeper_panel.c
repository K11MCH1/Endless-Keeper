/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keeper_panel.c
 *     Endless worlds: the Keeper panel (Profile, Upgrades, Social), opened by clicking the Soul Container.
 * @par Purpose:
 *     The user, 2026-11-19: "Turn the Soul Container into a clickable object [...] When Player hovers over, the hand
 *     should react to it just like it does when it hovers over creatures. When Player clicks on the Soul Container a ui
 *     panel opens up" (their mockup: Profile, Upgrades and Social tabs; the name, a portrait, the level and its XP bar,
 *     the statistics, Migrate Dungeon, Reset Dungeon, the rank).
 *
 *     The Soul Container answers the Hand through its CLICKABLE property (objects.cfg; power_hand.c), so a map maker
 *     can give it to any object, or take it away. A click on this keeper's own one opens the panel here.
 *
 *     Its look (2026-11-25, the user: "more modern to today's standards", then "Option B and do it in the style of the
 *     new Launcher design", the Molten look, from a mockup they approved): the launcher's dark glass with rounded
 *     corners, a soft shadow, molten-red light and gold, read in TrueType faces, painted in true colour on the CPU
 *     (ui_paint.c, ui_molten.c) and laid over the UI by OpenGL and Vulkan; the software renderer shows the same,
 *     flat and square, in the game's palette. Two layouts (the profile card beside the record, or above it), at a scale
 *     made every frame from the screen's safe area, so that all of it fits any screen; the canvas is painted again only
 *     when something it shows changes.
 *
 *     Its numbers are the keeper's lifetime record (struct WorldPlayerStats, world_progress.c) and its player level, in
 *     struct Game: a joined player sees them too. The portrait is one of the creature kinds the keeper has (a click on
 *     it shows them). The rank is its place among the keepers of this world, by level and then XP. Migrate Dungeon arms
 *     the placing of world_migrate.c (and is Cancel Migration while one goes on); Reset Dungeon (asked twice) is the
 *     Keeper Server's /reset: the dungeon starts again elsewhere, keeping level, XP and the record (the user,
 *     2026-11-19). Upgrades and Social are empty for now (the user: "Empty tabs for now").
 *
 * Test hook: KFX_WORLD_PANELTEST=turn: at that turn the panel opens on Profile; 60 turns on the portrait choice shows,
 *     100 on the first kind is picked; 160 on Upgrades shows, 200 on Profile again; 240 on Migrate Dungeon is pressed
 *     (the placing stays armed), 320 on a trap is chosen to place (its cursor box): screenshots at those turns.
 *     KFX_WORLD_PANELHOVER=n (with it): the mouse is held over button n (KpButton) while the panel is open.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world_keeper_panel.h"
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_guibtns.h"
#include "bflib_mouse.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "kjm_input.h"
#include "config_creature.h"
#include "creature_graphics.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "frontend.h"
#include "front_simple.h"
#include "game_legacy.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "gui_msgs.h"
#include "net_state.h"
#include "packets.h"
#include "player_data.h"
#include "power_hand.h"
#include "sprites.h"
#include "thing_data.h"
#include "ui_molten.h"
#include "ui_safe_area.h"
#include "world_creature_window.h"
#include "world_migrate.h"
#include "world_progress.h"
#include "world_stream.h"
#include "vidfade.h"
#include "observer.h"
#include "local_camera.h"
#include "thing_list.h"
#include "config_strings.h"
#include "config_keeperfx.h"
#include "config_powerhands.h"
/* (engine_render.h names a global my; only this from it) */
#include "world_level_window.h"
#include "engine_lenses.h"
#include "world_pentagram_screen.h"
#include "post_inc.h"

/* The window in design units. Two layouts: wide (the profile card beside the record) and stacked (the record under
   the profile), whichever comes out larger on the screen. */
#define KP_E 28.0f                      /* the window's inner margin */
#define KP_WIDE_W 880.0f
#define KP_WIDE_H 500.0f
#define KP_STACK_W 620.0f
#define KP_STACK_H 640.0f
#define KP_TOP 70.0f                    /* the rule under the tabs: the strip above it drags the window */
#define KP_PICK_COLS 6
#define KP_PICK_ROWS 4
#define KP_PICKS (KP_PICK_COLS * KP_PICK_ROWS)

enum KpTab { KpT_Profile = 0, KpT_Upgrades, KpT_Social };
enum KpWhat { KpW_Migrate = 1, KpW_Cancel = 2, KpW_Reset = 3, KpW_Portrait = 4 };
/* the buttons, in the order of world_keeper_panel_buttons[] */
enum KpButton { KpB_Ground = 0, KpB_Tab0, KpB_Tab1, KpB_Tab2, KpB_Close, KpB_Portrait, KpB_Migrate, KpB_Reset, KpB_Pick0,
    KpB_Count = KpB_Pick0 + KP_PICKS };

struct KpRect {
    float x, y, w, h;
};

static struct {
    TbBool wide;
    float u;                /* pixels per design unit */
    float w, h;             /* the window in design units */
    int px, py, pw, ph;     /* the window's body on the screen (pixels) */
    int margin;             /* the canvas's room round it for the shadow (pixels) */
} kp_lay;

static struct UmHost kp_host;
static struct GuiButton *kp_btn[KpB_Count]; /* this frame's buttons, by KpButton */
static int kp_tab;
static TbBool kp_picking;
static GameTurn kp_reset_asked;        /* Reset Dungeon was pressed once at this turn (0: not) */
static ThingModel kp_kinds[KP_PICKS];  /* the kinds the portrait choice shows, this frame */
static int kp_kinds_n;
/* dragging by the top strip (the user, 2026-11-19: "movable by click holding on the top part of the panel") */
static int kp_off_x, kp_off_y;         /* where the player put it, from the safe area's middle (pixels) */
static TbBool kp_dragging, kp_dragged, kp_was_down;
static int kp_grab_mx, kp_grab_my, kp_grab_x, kp_grab_y;
static long kp_hover_test = -2;        /* KFX_WORLD_PANELHOVER */
/* portraits as true-colour pictures, made once a kind */
static unsigned char *kp_face_px[CREATURE_TYPES_MAX];
static int kp_face_w[CREATURE_TYPES_MAX], kp_face_h[CREATURE_TYPES_MAX];

/******************************************************************************/
TbBool world_keeper_panel_open(void)
{
    return world_mode_active() && !netstate_dedicated() && menu_is_active(GMnu_KEEPER_PANEL);
}

/** Where each button is, in design units (none: zero size, so it cannot be hovered or pressed). */
static struct KpRect kp_rect(int b)
{
    struct KpRect r = {0, 0, 0, 0};
    const float W = kp_lay.w, H = kp_lay.h, E = KP_E;
    const TbBool profile = (kp_tab == KpT_Profile);
    if (b == KpB_Ground)
    {
        r.w = W;
        r.h = H;
    } else if ((b >= KpB_Tab0) && (b <= KpB_Tab2))
    {
        r.x = E + 118.0f * (float)(b - KpB_Tab0);
        r.y = 18.0f;
        r.w = 112.0f;
        r.h = 38.0f;
    } else if (b == KpB_Close)
    {
        r.x = W - E - 110.0f;
        r.y = 18.0f;
        r.w = 110.0f;
        r.h = 38.0f;
    } else if (!profile)
        return r;
    else if (b == KpB_Portrait)
    {
        const float s = kp_lay.wide ? 124.0f : 112.0f;
        r.x = kp_lay.wide ? E + (292.0f - s) * 0.5f : E;
        r.y = kp_lay.wide ? 114.0f : 92.0f;
        r.w = r.h = s;
    } else if (b == KpB_Migrate)
    {
        r.x = E;
        r.y = H - E - 52.0f;
        r.w = kp_lay.wide ? 236.0f : 210.0f;
        r.h = 52.0f;
    } else if (b == KpB_Reset)
    {
        r.x = E + (kp_lay.wide ? 252.0f : 222.0f);
        r.y = H - E - 52.0f;
        r.w = kp_lay.wide ? 200.0f : 180.0f;
        r.h = 52.0f;
    } else if ((b >= KpB_Pick0) && (b < KpB_Pick0 + KP_PICKS) && kp_picking)
    {
        /* the portrait choice: a grid in the record's place */
        const int k = b - KpB_Pick0;
        const float gx = kp_lay.wide ? E + 292.0f + 28.0f : E, gy = kp_lay.wide ? 120.0f : 260.0f;
        const float gw = kp_lay.wide ? W - E - gx : W - 2.0f * E, gap = 8.0f;
        const float cell = min((gw - gap * (KP_PICK_COLS - 1)) / KP_PICK_COLS, (258.0f - gap * (KP_PICK_ROWS - 1)) / KP_PICK_ROWS);
        r.x = gx + (cell + gap) * (float)(k % KP_PICK_COLS);
        r.y = gy + (cell + gap) * (float)(k / KP_PICK_COLS);
        r.w = r.h = cell;
    }
    return r;
}

/** Sets the window out on the screen as it is now (its layout, scale, place and its buttons' places). The user,
 *  2026-11-19: "Make the panel bigger so it also draws over the panel on the left": it stands on the middle of the
 *  whole screen (side panel and all) inside the safe area (phones' rounded corners and camera holes), where the
 *  player dragged it (kp_drag), its body kept on the screen. On a computer it grows no larger than the screen's
 *  height / 720 a unit (it need not fill a big screen); on a phone it takes all the room there is (touch). */
static void kp_place(struct GuiMenu *gmnu)
{
    const int sx0 = ui_safe_left, sy0 = ui_safe_top;
    const int sw = max(64, ui_safe_right_edge() - ui_safe_left), sh = max(64, ui_safe_bottom_edge() - ui_safe_top);
    float u_wide = min((float)sw * 0.92f / KP_WIDE_W, (float)sh * 0.92f / KP_WIDE_H);
    float u_stack = min((float)sw * 0.92f / KP_STACK_W, (float)sh * 0.92f / KP_STACK_H);
#ifndef __ANDROID__
    const float cap = (float)MyScreenHeight / 720.0f * (float)ui_scale_percent / 100.0f; /* (UI_SCALE) */
    u_wide = min(u_wide, cap);
    u_stack = min(u_stack, cap);
#endif
    kp_lay.wide = (u_wide >= u_stack * 0.98f);
    kp_lay.u = max(0.72f, kp_lay.wide ? u_wide : u_stack); /* (smaller, text is no longer read: it may stand off the screen, and drag) */
    kp_lay.w = kp_lay.wide ? KP_WIDE_W : KP_STACK_W;
    kp_lay.h = kp_lay.wide ? KP_WIDE_H : KP_STACK_H;
    kp_lay.pw = (int)(kp_lay.w * kp_lay.u + 0.5f);
    kp_lay.ph = (int)(kp_lay.h * kp_lay.u + 0.5f);
    const struct UmStyle st = {kp_lay.u, um_host_flat()};
    kp_lay.margin = (int)um_window_margin(&st);
    int x = sx0 + (sw - kp_lay.pw) / 2 + kp_off_x;
    int y = sy0 + (sh - kp_lay.ph) / 2 + kp_off_y;
    /* (kept on the screen: its body, or as much as fits, top left first) */
    x = max(0, min(x, (int)MyScreenWidth - kp_lay.pw));
    y = max(0, min(y, (int)MyScreenHeight - kp_lay.ph));
    kp_off_x = x - (sx0 + (sw - kp_lay.pw) / 2);
    kp_off_y = y - (sy0 + (sh - kp_lay.ph) / 2);
    kp_lay.px = x;
    kp_lay.py = y;
    gmnu->pos_x = x;
    gmnu->pos_y = y;
    gmnu->width = kp_lay.pw;
    gmnu->height = kp_lay.ph;
    int n = 0;
    memset(kp_btn, 0, sizeof(kp_btn));
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (gmnu->buttons != NULL) && (n < KpB_Count); i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != gmnu->number))
            continue;
        if (gmnu->buttons[n].gbtype == -1)
            break;
        const struct KpRect r = kp_rect(n);
        kp_btn[n] = gbtn;
        n++;
        gbtn->scr_pos_x = x + (int)(r.x * kp_lay.u + 0.5f);
        gbtn->scr_pos_y = y + (int)(r.y * kp_lay.u + 0.5f);
        gbtn->pos_x = gbtn->scr_pos_x;
        gbtn->pos_y = gbtn->scr_pos_y;
        gbtn->width = (int)(r.w * kp_lay.u + 0.5f);
        gbtn->height = (int)(r.h * kp_lay.u + 0.5f);
    }
}

static void kpc_place(struct GuiMenu *gmnu);

void world_keeper_panel_show(void)
{
    if (!world_mode_active() || netstate_dedicated())
        return;
    if (menu_is_active(GMnu_CREATURE_WINDOW))
        turn_off_menu(GMnu_CREATURE_WINDOW); /* one window in the middle of the view at a time */
    kp_tab = KpT_Profile;
    kp_picking = false;
    kp_reset_asked = 0;
    if (!menu_is_active(GMnu_KEEPER_PANEL))
    {
        turn_on_menu(GMnu_KEEPER_PANEL);
        const MenuNumber num = menu_id_to_number(GMnu_KEEPER_PANEL);
        if (num >= 0)
        {
            /* (at once: the mouse is tested against the buttons before the first frame) */
            if (window_style == WinStyle_Classic)
                kpc_place(get_active_menu(num));
            else
                kp_place(get_active_menu(num));
        }
    }
    SYNCLOG("World: the Keeper panel opens");
}

void world_keeper_panel_close(void)
{
    if (menu_is_active(GMnu_KEEPER_PANEL))
        turn_off_menu(GMnu_KEEPER_PANEL);
    kp_picking = false;
    kp_reset_asked = 0;
}

/******************************************************************************/
/* What the Profile shows */

static int kp_rank(PlayerNumber me)
{
    const int lv = world_player_level(me);
    const uint32_t xp = world_player_xp(me);
    int rank = 1;
    for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
    {
        if ((q == me) || !world_player_is_human(q) || !player_exists(get_player(q)) || world_seat_deleted(q))
            continue;
        const int ql = world_player_level(q);
        if ((ql > lv) || ((ql == lv) && (world_player_xp(q) > xp)))
            rank++;
    }
    return rank;
}

static void kp_time(uint32_t turns, char *out, size_t len)
{
    const unsigned long tps = (unsigned long)max(turns_per_second, 1);
    const unsigned long secs = turns / tps;
    if (secs >= 3600)
        snprintf(out, len, "%luh %02lum", secs / 3600, (secs / 60) % 60);
    else
        snprintf(out, len, "%lum %02lus", secs / 60, secs % 60);
}

/** A number with thousands apart: 1,204. */
static void kp_count(unsigned long v, char *out, size_t len)
{
    char digits[32];
    snprintf(digits, sizeof(digits), "%lu", v);
    const size_t n = strlen(digits);
    size_t o = 0;
    for (size_t i = 0; (i < n) && (o + 2 < len); i++)
    {
        if ((i > 0) && ((n - i) % 3 == 0))
            out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

/** The kinds of creature the keeper has now (the portrait's choice). */
static void kp_find_kinds(void)
{
    kp_kinds_n = 0;
    const struct Dungeon *dungeon = get_players_num_dungeon(my_player_number);
    if (dungeon_invalid(dungeon))
        return;
    for (ThingModel m = 1; (m < game.conf.crtr_conf.model_count) && (m < CREATURE_TYPES_MAX) && (kp_kinds_n < KP_PICKS); m++)
        if (dungeon->owned_creatures_of_model[m] > 0)
            kp_kinds[kp_kinds_n++] = m;
}

static const char *kp_kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *name = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((name != NULL) && (name[0] != 0)) ? name : creature_code_name(model);
}

/** A creature kind's portrait as a true-colour picture (made once; NULL: none). */
static const unsigned char *kp_face(ThingModel model, int *w, int *h)
{
    if ((model <= 0) || (model >= CREATURE_TYPES_MAX))
        return NULL;
    if (kp_face_px[model] == NULL)
    {
        const long spr_idx = get_creature_model_graphics(model, CGI_QuerySymbol);
        const struct TbSprite *spr = (spr_idx > 0) ? get_button_sprite_for_player(spr_idx, my_player_number) : NULL;
        if ((spr == NULL) || (spr->SWidth == 0) || (spr->SHeight == 0) || (engine_palette == NULL))
            return NULL;
        kp_face_px[model] = (unsigned char *)calloc((size_t)spr->SWidth * spr->SHeight, 4);
        if (kp_face_px[model] == NULL)
            return NULL;
        ui_sprite_rgba(spr, engine_palette, kp_face_px[model]);
        kp_face_w[model] = spr->SWidth;
        kp_face_h[model] = spr->SHeight;
    }
    *w = kp_face_w[model];
    *h = kp_face_h[model];
    return kp_face_px[model];
}

/******************************************************************************/
/* The drag */

/** A press on the top strip (over the rule under the tabs) takes hold; once the mouse has moved a few pixels the
 *  window follows it until the button is let go, and the click that ends a drag does nothing else. */
static void kp_drag(const struct GuiMenu *gmnu)
{
    const TbBool down = (lbDisplay.MLeftButton != 0);
    const int mx = GetMouseX(), my = GetMouseY();
    if (down && !kp_was_down)
    {
        kp_dragged = false;
        const int bottom = gmnu->pos_y + (int)(KP_TOP * kp_lay.u);
        kp_dragging = (mx >= gmnu->pos_x) && (mx < gmnu->pos_x + gmnu->width) && (my >= gmnu->pos_y) && (my < bottom);
        kp_grab_mx = mx;
        kp_grab_my = my;
        kp_grab_x = kp_off_x;
        kp_grab_y = kp_off_y;
    }
    if (down && kp_dragging)
    {
        const int dx = mx - kp_grab_mx, dy = my - kp_grab_my;
        if (kp_dragged || (abs(dx) + abs(dy) > max(4, (int)(kp_lay.u * 6.0f))))
        {
            kp_dragged = true;
            kp_off_x = kp_grab_x + dx;
            kp_off_y = kp_grab_y + dy;
        }
    }
    if (!down)
        kp_dragging = false;
    kp_was_down = down;
}

/** A button's click at the end of a drag is the drag's, not the button's. */
static TbBool kp_click_was_drag(void)
{
    if (!kp_dragged)
        return false;
    kp_dragged = false;
    return true;
}

/******************************************************************************/
/* The painting */

static TbBool kp_hot(int b)
{
    if ((b < 0) || (b >= KpB_Count) || (kp_btn[b] == NULL) || (kp_btn[b]->width <= 0))
        return false;
    if (kp_hover_test == b)
        return true;
    return ((kp_btn[b]->flags & LbBtnF_MouseOver) != 0) && !kp_dragging;
}

static int kp_state(int b)
{
    if (!kp_hot(b))
        return UmS_Normal;
    return (lbDisplay.MLeftButton != 0) ? UmS_Down : UmS_Hot;
}

static TbBool kp_reset_is_asked(void)
{
    return (kp_reset_asked != 0) && (game.play_gameturn <= kp_reset_asked + 5 * (GameTurn)max(turns_per_second, 1));
}

/** The record's six numbers as shown. */
static void kp_stat_values(char vals[6][32])
{
    const struct WorldPlayerStats *ws = world_player_stats(my_player_number);
    kp_time(ws->play_turns, vals[0], 32);
    kp_count(ws->hearts, vals[1], 32);
    kp_count(ws->hearts_lost, vals[2], 32);
    kp_count(ws->heroes_killed, vals[3], 32);
    kp_count(ws->creatures_killed, vals[4], 32);
    char n[24];
    kp_count(ws->dug_farthest, n, sizeof(n));
    snprintf(vals[5], 32, "%s slabs", n);
}

/** The panel's live parts this frame (where each is and what it shows): only those that changed are painted again
 *  (2026-11-25, speed at 4K: the time played changes every second, a hover changes one button). */
static int kp_parts(struct UmPart *pt)
{
    int n = 0;
    const float u = kp_lay.u, m = (float)kp_lay.margin, W = kp_lay.w, H = kp_lay.h, E = KP_E;
    const PlayerNumber me = my_player_number;
    for (int t = 0; t < 3; t++)
    {
        const struct KpRect r = kp_rect(KpB_Tab0 + t);
        um_part(pt, &n, u, m, r.x, r.y, r.w, r.h, 16.0f, um_hash("tab %d %d %d", t, (int)(t == kp_tab), (int)kp_hot(KpB_Tab0 + t)));
    }
    {
        const struct KpRect r = kp_rect(KpB_Close);
        um_part(pt, &n, u, m, r.x, r.y, r.w, r.h, 16.0f, um_hash("close %d", kp_state(KpB_Close)));
    }
    if (kp_tab != KpT_Profile)
        return n;
    const struct PlayerInfo *player = get_player(me);
    const struct KpRect pr = kp_rect(KpB_Portrait);
    const uint32_t who = um_hash("who %s %d %lu", (player != NULL) ? player->player_name : "", world_player_level(me),
        (unsigned long)world_player_xp(me));
    if (kp_lay.wide)
        um_part(pt, &n, u, m, E, pr.y + pr.h, 292.0f, 378.0f - (pr.y + pr.h), 14.0f, who);
    else
        um_part(pt, &n, u, m, pr.x + pr.w, pr.y, W - E - (pr.x + pr.w), 130.0f, 14.0f, who);
    um_part(pt, &n, u, m, pr.x, pr.y, pr.w, pr.h, 20.0f,
        um_hash("face %d %d", (int)world_player_stats(me)->portrait, (int)(kp_hot(KpB_Portrait) || kp_picking)));
    const float rx = kp_lay.wide ? E + 292.0f + 28.0f : E, ry = kp_lay.wide ? 92.0f : 232.0f;
    const float rw = kp_lay.wide ? W - E - rx : W - 2 * E;
    um_part(pt, &n, u, m, rx, ry, rw, 20.0f, 2.0f, um_hash("mig %d", world_migrate_state(me)));
    if (kp_picking)
    {
        kp_find_kinds();
        int hot = -1, kinds = 0;
        for (int k = 0; k < kp_kinds_n; k++)
        {
            kinds = kinds * 31 + kp_kinds[k];
            if (kp_hot(KpB_Pick0 + k))
                hot = k;
        }
        um_part(pt, &n, u, m, rx, ry, rw, 286.0f, 20.0f, um_hash("pick %d %d %d", kp_kinds_n, kinds, hot));
    } else
    {
        char vals[6][32];
        kp_stat_values(vals);
        const float gap = 12.0f, cw = (rw - gap) / 2.0f, ch = 78.0f, top = ry + 28.0f;
        for (int i = 0; i < 6; i++)
            um_part(pt, &n, u, m, rx + (cw + gap) * (float)(i % 2), top + (ch + gap) * (float)(i / 2), cw, ch, 2.0f,
                um_hash("stat %d %s", i, vals[i]));
    }
    {
        const struct KpRect r = kp_rect(KpB_Migrate);
        um_part(pt, &n, u, m, r.x, r.y, r.w, r.h, 36.0f, um_hash("migrate %d %d", world_migrate_state(me), kp_state(KpB_Migrate)));
    }
    {
        const struct KpRect r = kp_rect(KpB_Reset);
        um_part(pt, &n, u, m, r.x, r.y, r.w, r.h, 18.0f, um_hash("reset %d %d", (int)kp_reset_is_asked(), kp_state(KpB_Reset)));
    }
    {
        const float bw = kp_lay.wide ? 168.0f : 140.0f;
        um_part(pt, &n, u, m, W - E - bw, H - E - 52.0f, bw, 52.0f, 2.0f, um_hash("rank %d", kp_rank(me)));
    }
    return n;
}

/** One layer: base (the glass, the cards and their labels: what changes with the layout, the tab and the choice
 *  only), or the live part over it. */
static void kp_paint(TbBool base)
{
    struct UiCanvas *c = base ? &kp_host.base : &kp_host.canvas;
    const TbBool live = !base;
    const struct UmStyle stv = {kp_lay.u, um_host_flat()};
    const struct UmStyle *st = &stv;
    const float u = kp_lay.u, m = (float)kp_lay.margin, W = kp_lay.w, H = kp_lay.h, E = KP_E;
#define X(v) (m + (v) * u)
#define Y(v) (m + (v) * u)
#define S(v) ((v) * u)
    const PlayerNumber me = my_player_number;
    /* the tabs and Close */
    static const char *tabs[3] = {"Profile", "Upgrades", "Social"};
    for (int t = 0; (t < 3) && live; t++)
    {
        const struct KpRect r = kp_rect(KpB_Tab0 + t);
        um_tab(c, st, X(r.x), Y(r.y), X(r.x + r.w), Y(r.y + r.h), (t == kp_tab), kp_hot(KpB_Tab0 + t), S(17.0f), tabs[t]);
    }
    if (live)
    {
        const struct KpRect r = kp_rect(KpB_Close);
        um_button(c, st, X(r.x), Y(r.y), X(r.x + r.w), Y(r.y + r.h), UmB_Normal, kp_state(KpB_Close), S(16.0f), "Close");
    }
    if (base)
        um_rule(c, st, X(E), Y(KP_TOP), X(W - E));
    if (kp_tab != KpT_Profile)
    {
        if (live)
            return;
        const float mid = (KP_TOP + H) * 0.5f;
        ui_text_box(c, UiF_Head, S(30.0f), X(E), Y(mid - 44.0f), S(W - 2 * E), S(40.0f), 1, UM_BRIGHT, 0.0f, !st->flat,
            tabs[(kp_tab >= 0) && (kp_tab < 3) ? kp_tab : 0]);
        ui_text_box(c, UiF_Body, S(17.0f), X(E), Y(mid + 4.0f), S(W - 2 * E), S(24.0f), 1, UM_DIM, 0.0f, !st->flat, "Coming soon.");
        return;
    }
    /* the profile: portrait, name, level and its bar */
    const struct PlayerInfo *player = get_player(me);
    const char *name = ((player != NULL) && (player->player_name[0] != 0)) ? player->player_name : "Keeper";
    const int lv = world_player_level(me);
    const uint32_t xp = world_player_xp(me);
    char level[32], xpline[64], toline[64];
    snprintf(level, sizeof(level), "Level %d", lv);
    float frac = 1.0f;
    {
        char a[24], b[24];
        kp_count(xp, a, sizeof(a));
        if (lv >= WORLD_PLAYER_LEVEL_MAX)
        {
            snprintf(xpline, sizeof(xpline), "%s XP", a);
            snprintf(toline, sizeof(toline), "Highest level");
        } else
        {
            const uint32_t from = world_player_level_xp(lv), to = world_player_level_xp(lv + 1);
            const uint32_t have = (xp > from) ? (xp - from) : 0, need = (to > from) ? (to - from) : 1;
            frac = (float)min(have, need) / (float)need;
            kp_count(to, b, sizeof(b));
            snprintf(xpline, sizeof(xpline), "%s / %s XP", a, b);
            kp_count(need - min(have, need), a, sizeof(a));
            snprintf(toline, sizeof(toline), "%s to Level %d", a, lv + 1);
        }
    }
    const struct KpRect pr = kp_rect(KpB_Portrait);
    float tx, tw, ty;   /* the name's column */
    if (kp_lay.wide)
    {
        if (base)
            um_card(c, st, X(E), Y(92.0f), X(E + 292.0f), Y(92.0f + 286.0f));
        tx = E + 24.0f;
        tw = 292.0f - 48.0f;
        ty = pr.y + pr.h + 12.0f;
        if (live)
            ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, S(30.0f), 0.0f, S(tw), name), X(tx), Y(ty), S(tw), S(36.0f), 1,
            UM_BRIGHT, 0.0f, !st->flat, name);
        if (live)
            ui_text_box(c, UiF_Head, S(18.0f), X(tx), Y(ty + 40.0f), S(tw), S(22.0f), 1, UM_GOLD_HI, 0.0f, !st->flat, level);
        ty += 74.0f;
    } else
    {
        tx = pr.x + pr.w + 24.0f;
        tw = W - E - tx;
        if (live)
            ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, S(30.0f), 0.0f, S(tw), name), X(tx), Y(pr.y + 2.0f), S(tw), S(36.0f), 0,
            UM_BRIGHT, 0.0f, !st->flat, name);
        if (live)
            ui_text_box(c, UiF_Head, S(18.0f), X(tx), Y(pr.y + 42.0f), S(tw), S(22.0f), 0, UM_GOLD_HI, 0.0f, !st->flat, level);
        ty = pr.y + 76.0f;
    }
    if (live)
    {
        um_bar(c, st, X(tx), Y(ty), X(tx + tw), Y(ty + 14.0f), frac);
        ui_text_box(c, UiF_Body, S(14.0f), X(tx), Y(ty + 22.0f), S(tw), S(18.0f), 0, UM_SOFT, 0.0f, !st->flat, xpline);
        ui_text_box(c, UiF_Body, S(14.0f), X(tx), Y(ty + 22.0f), S(tw), S(18.0f), 2, UM_DIM, 0.0f, !st->flat, toline);
    }
    /* the portrait (a click on it shows the choice) */
    if (live)
    {
        const float r = S(14.0f);
        const TbBool lit = kp_hot(KpB_Portrait) || kp_picking;
        um_well(c, st, X(pr.x), Y(pr.y), X(pr.x + pr.w), Y(pr.y + pr.h), r);
        int fw = 0, fh = 0;
        const unsigned char *face = kp_face(world_player_stats(me)->portrait, &fw, &fh);
        const int in = (int)max(1.0f, S(2.0f));
        if (face != NULL)
            ui_image(c, (int)X(pr.x) + in, (int)Y(pr.y) + in, (int)S(pr.w) - 2 * in, (int)S(pr.h) - 2 * in, face, fw, fh,
                st->flat ? 0.0f : r - (float)in);
        else
        {
            ui_text_box(c, UiF_Head, S(48.0f), X(pr.x), Y(pr.y + 12.0f), S(pr.w), S(pr.h * 0.55f), 1, UM_GOLD, 0.0f, !st->flat, "?");
            ui_text_box(c, UiF_Body, S(14.0f), X(pr.x), Y(pr.y + pr.h * 0.7f), S(pr.w), S(18.0f), 1, UM_DIM, 0.0f, !st->flat, "Choose");
        }
        um_frame(c, st, X(pr.x), Y(pr.y), X(pr.x + pr.w), Y(pr.y + pr.h), r, lit);
    }
    /* the record, or the portrait choice in its place */
    const float rx = kp_lay.wide ? E + 292.0f + 28.0f : E, ry = kp_lay.wide ? 92.0f : 232.0f;
    const float rw = kp_lay.wide ? W - E - rx : W - 2 * E;
    if (live && (world_migrate_state(me) != WMig_None))
        ui_text_box(c, UiF_Body, S(14.0f), X(rx), Y(ry), S(rw), S(20.0f), 2, UM_EMBER, 0.0f, !st->flat,
            (world_migrate_state(me) == WMig_Going) ? "Migration in progress" : "Returning home");
    if (kp_picking)
    {
        if (base)
            goto foot;
        int hot = -1;
        for (int k = 0; k < kp_kinds_n; k++)
            if (kp_hot(KpB_Pick0 + k))
                hot = k;
        char head[96];
        if (hot >= 0)
            snprintf(head, sizeof(head), "Choose a Portrait: %s", kp_kind_name(kp_kinds[hot]));
        else
            snprintf(head, sizeof(head), "Choose a Portrait");
        um_heading(c, st, X(rx), Y(ry), S(20.0f), S(15.0f), head);
        if (kp_kinds_n == 0)
            ui_text_box(c, UiF_Body, S(15.0f), X(rx), Y(ry + 80.0f), S(rw), S(20.0f), 1, UM_DIM, 0.0f, !st->flat,
                "You have no creatures to choose from.");
        for (int k = 0; k < kp_kinds_n; k++)
        {
            const struct KpRect r = kp_rect(KpB_Pick0 + k);
            const float rad = S(10.0f);
            um_well(c, st, X(r.x), Y(r.y), X(r.x + r.w), Y(r.y + r.h), rad);
            int fw = 0, fh = 0;
            const unsigned char *face = kp_face(kp_kinds[k], &fw, &fh);
            if (face != NULL)
                ui_image(c, (int)X(r.x + r.w * 0.1f), (int)Y(r.y + r.h * 0.1f), (int)S(r.w * 0.8f), (int)S(r.h * 0.8f), face,
                    fw, fh, 0.0f);
            um_frame(c, st, X(r.x), Y(r.y), X(r.x + r.w), Y(r.y + r.h), rad, (k == hot));
        }
    } else
    {
        if (base)
            um_heading(c, st, X(rx), Y(ry), S(20.0f), S(15.0f), "Lifetime Record");
        char vals[6][32];
        kp_stat_values(vals);
        static const char *labels[6] = {"TIME PLAYED", "KEEPERS DEFEATED", "HEARTS LOST", "HEROES KILLED", "CREATURES KILLED",
            "LONGEST DISTANCE DUG"};
        const float gap = 12.0f, cw = (rw - gap) / 2.0f, ch = 78.0f, top = ry + 28.0f;
        for (int i = 0; i < 6; i++)
        {
            const float cx = rx + (cw + gap) * (float)(i % 2), cy = top + (ch + gap) * (float)(i / 2);
            if (base)
            {
                um_card(c, st, X(cx), Y(cy), X(cx + cw), Y(cy + ch));
                const float lpx = ui_text_fit(UiF_Body, S(13.0f), S(1.0f), S(cw - 36.0f), labels[i]);
                ui_text_box(c, UiF_Body, lpx, X(cx + 18.0f), Y(cy + 12.0f), S(cw - 36.0f), S(18.0f), 0, UM_DIM, S(1.0f), false, labels[i]);
                continue;
            }
            ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, S(25.0f), 0.0f, S(cw - 36.0f), vals[i]), X(cx + 18.0f), Y(cy + 36.0f),
                S(cw - 36.0f), S(30.0f), 0, UM_BRIGHT, 0.0f, !st->flat, vals[i]);
        }
    }
    /* the foot: Migrate Dungeon, Reset Dungeon, the rank */
foot:
    if (base)
    {
        um_rule(c, st, X(E), Y(H - E - 52.0f - 22.0f), X(W - E));
        const float bw = kp_lay.wide ? 168.0f : 140.0f, bx = W - E - bw, by = H - E - 52.0f;
        if (st->flat)
            ui_stroke_rrect(c, X(bx), Y(by), X(bx + bw), Y(by + 52.0f), 0.0f, 1.0f, UM_GOLD);
        else
        {
            ui_fill_rrect(c, X(bx), Y(by), X(bx + bw), Y(by + 52.0f), S(12.0f), UIC(255, 236, 214, 10), UIC(255, 236, 214, 10), false);
            ui_stroke_rrect(c, X(bx), Y(by), X(bx + bw), Y(by + 52.0f), S(12.0f), 1.0f, UIC(222, 178, 88, 115));
        }
        return;
    }
    {
        const int mig = world_migrate_state(me);
        const struct KpRect r = kp_rect(KpB_Migrate);
        um_button(c, st, X(r.x), Y(r.y), X(r.x + r.w), Y(r.y + r.h), UmB_Primary, (mig == WMig_Returning) ? UmS_Off : kp_state(KpB_Migrate),
            S(19.0f), (mig == WMig_None) ? "Migrate Dungeon" : ((mig == WMig_Going) ? "Cancel Migration" : "Returning Home"));
    }
    {
        const struct KpRect r = kp_rect(KpB_Reset);
        const TbBool asked = kp_reset_is_asked();
        um_button(c, st, X(r.x), Y(r.y), X(r.x + r.w), Y(r.y + r.h), UmB_Danger, asked ? UmS_Armed : kp_state(KpB_Reset), S(18.0f),
            asked ? "Are you sure?" : "Reset Dungeon");
    }
    {
        const float bw = kp_lay.wide ? 168.0f : 140.0f, bx = W - E - bw, by = H - E - 52.0f;
        char rank[16];
        snprintf(rank, sizeof(rank), "#%d", kp_rank(me));
        const float lw = ui_text_width(UiF_Head, S(13.0f), S(2.6f), "RANK"), nw = ui_text_width(UiF_Head, S(32.0f), 0.0f, rank);
        const float gapw = S(12.0f), x0 = X(bx) + (S(bw) - (lw + gapw + nw)) * 0.5f;
        ui_text_box(c, UiF_Head, S(13.0f), x0, Y(by), lw + 2.0f, S(52.0f), 0, UM_GOLD, S(2.6f), !st->flat, "RANK");
        ui_text_box(c, UiF_Head, S(32.0f), x0 + lw + gapw, Y(by), nw + 2.0f, S(52.0f), 0, UM_GOLD_HI, 0.0f, !st->flat, rank);
    }
#undef X
#undef Y
#undef S
}

static void kpc_background(struct GuiMenu *gmnu);

static void kp_background(struct GuiMenu *gmnu)
{
    if (window_style == WinStyle_Classic)
    {
        kpc_background(gmnu);
        return;
    }
    if (kp_hover_test == -2)
    {
        const char *v = getenv("KFX_WORLD_PANELHOVER");
        kp_hover_test = (v != NULL) ? atol(v) : -1;
    }
    kp_host.name = "panel";
    kp_drag(gmnu);
    kp_place(gmnu);
    const int cw = kp_lay.pw + 2 * kp_lay.margin, ch = kp_lay.ph + 2 * kp_lay.margin;
    const unsigned long t0 = (unsigned long)LbTimerClock();
    char bkey[96];
    snprintf(bkey, sizeof(bkey), "%d %.4f %d %d", (int)kp_lay.wide, (double)kp_lay.u, kp_tab, (int)kp_picking);
    TbBool full = um_host_begin(&kp_host, cw, ch, bkey);
    unsigned long base_ms = 0;
    if (um_host_base_begin(&kp_host, bkey))
    {
        char gkey[64];
        snprintf(gkey, sizeof(gkey), "%.4f %d %d", (double)kp_lay.u, kp_lay.pw, kp_lay.ph);
        if (um_host_glass_begin(&kp_host, gkey))
        {
            const struct UmStyle gst = {kp_lay.u, um_host_flat()};
            const float m = (float)kp_lay.margin;
            um_window(&kp_host.glass, &gst, m, m, m + (float)kp_lay.pw, m + (float)kp_lay.ph);
        }
        um_host_glass_down(&kp_host);
        kp_paint(true);
        full = true;
        base_ms = (unsigned long)LbTimerClock() - t0;
    }
    struct UmPart parts[UM_PARTS];
    const int np = kp_parts(parts);
    if (um_host_parts(&kp_host, parts, np, full))
    {
        kp_paint(false);
        um_host_parts_end(&kp_host);
        um_host_painted(&kp_host, t0);
        if (full || (kp_host.paints <= 3) || (kp_host.paints % 50 == 0))
            SYNCLOG("World: the Keeper panel painted in %lu ms (base %lu; %dx%d, %s, %s layout, scale %.2f, %s; %lu paintings)",
                kp_host.paint_ms, base_ms, cw, ch, full ? "all" : "a part", kp_lay.wide ? "wide" : "stacked", (double)kp_lay.u,
                um_host_flat() ? "palette" : "true colour", kp_host.paints);
    }
    um_host_show(&kp_host, kp_lay.px - kp_lay.margin, kp_lay.py - kp_lay.margin);
}

/******************************************************************************/
/* The classic look (WINDOW_STYLE=CLASSIC): the panel as it was before 2026-11-25, in the creature window's dress (the
   game's ornate frame, its lettering for headings and buttons, the plain font for what is read, the side panel's bars
   and frames), drawn by its buttons. Its code is the old one, renamed kpc_. */

/* The window in its own units (a unit is a side panel's unit where there is room: kp_place). */
#define KPC_W 480
#define KPC_H 280
#define KPC_EDGE 24
#define KPC_TAB_Y 12
#define KPC_TAB_H 24
#define KPC_TOP 48                       /* under the tabs */
#define KPC_FOOT_Y (KPC_H - 40)           /* the buttons at the foot */
#define KPC_FOOT_H 26
#define KPC_RIGHT_X 250                  /* the statistics */
#define KPC_VALUE_X 370
#define KPC_VALUE_W (KPC_W - KPC_EDGE - KPC_VALUE_X)
#define KPC_PORTRAIT 84
/* the portrait choice: a grid in the statistics' place (right of the level's bar) */
#define KPC_PICK_X (KPC_W - KPC_EDGE - KPC_PICK_COLS * KPC_PICK_CELL + 4)
#define KPC_PICK_Y 70
#define KPC_PICK_CELL 36
#define KPC_PICK_COLS 6
#define KPC_PICK_ROWS 4
#define KPC_PICKS (KPC_PICK_COLS * KPC_PICK_ROWS)
/* the frame's ornaments past the window's edge (as the creature window's) */
#define KPC_ORN_SIDE 34
#define KPC_ORN_TOP 14
#define KPC_ORN_BOTTOM 30
#define KPC_AIR 2

static int kpc_upp = 16;

/** Where button n stood in the classic layout, in its units (x, y, w, h). */
static void kpc_rect(int n, int *r)
{
    r[0] = r[1] = r[2] = r[3] = 0;
    if (n == KpB_Ground)
    {
        r[2] = KPC_W;
        r[3] = KPC_H;
    } else if ((n >= KpB_Tab0) && (n <= KpB_Tab2))
    {
        r[0] = KPC_EDGE + 100 * (n - KpB_Tab0);
        r[1] = KPC_TAB_Y;
        r[2] = 96;
        r[3] = KPC_TAB_H;
    } else if (n == KpB_Close)
    {
        r[0] = KPC_W - KPC_EDGE - 96;
        r[1] = KPC_TAB_Y;
        r[2] = 96;
        r[3] = KPC_TAB_H;
    } else if (n == KpB_Portrait)
    {
        r[0] = KPC_EDGE;
        r[1] = KPC_TOP + 26;
        r[2] = r[3] = KPC_PORTRAIT;
    } else if (n == KpB_Migrate)
    {
        r[0] = KPC_EDGE;
        r[1] = KPC_FOOT_Y;
        r[2] = 150;
        r[3] = KPC_FOOT_H;
    } else if (n == KpB_Reset)
    {
        r[0] = KPC_EDGE + 162;
        r[1] = KPC_FOOT_Y;
        r[2] = 140;
        r[3] = KPC_FOOT_H;
    } else if ((n >= KpB_Pick0) && (n < KpB_Pick0 + KPC_PICKS))
    {
        const int k = n - KpB_Pick0;
        r[0] = KPC_PICK_X + KPC_PICK_CELL * (k % KPC_PICK_COLS);
        r[1] = KPC_PICK_Y + KPC_PICK_CELL * (k / KPC_PICK_COLS);
        r[2] = r[3] = KPC_PICK_CELL - 4;
    }
}

/** The classic window on the screen as it is now (the old kp_place). */
static void kpc_place(struct GuiMenu *gmnu)
{
    const int sw = MyScreenWidth, sh = MyScreenHeight;
    int upp = sw * 16 * 90 / 100 / (KPC_W + 2 * (KPC_ORN_SIDE + KPC_AIR));
    upp = min(upp, sh * 16 * 86 / 100 / (KPC_H + KPC_ORN_TOP + KPC_ORN_BOTTOM + 2 * KPC_AIR));
    upp = max(upp, 4);
    kpc_upp = upp;
    gmnu->width = KPC_W * upp / 16;
    gmnu->height = KPC_H * upp / 16;
    const int orn_top = KPC_ORN_TOP * upp / 16, orn_bottom = KPC_ORN_BOTTOM * upp / 16;
    int x = (sw - gmnu->width) / 2 + kp_off_x;
    int y = (sh - gmnu->height - orn_top - orn_bottom) / 2 + orn_top + kp_off_y;
    /* (kept on the screen: its body, if not all its ornaments) */
    x = max(0, min(x, sw - gmnu->width));
    y = max(orn_top / 2, min(y, sh - gmnu->height));
    kp_off_x = x - (sw - gmnu->width) / 2;
    kp_off_y = y - ((sh - gmnu->height - orn_top - orn_bottom) / 2 + orn_top);
    gmnu->pos_x = x;
    gmnu->pos_y = y;
    kp_lay.u = (float)upp / 16.0f; /* (the drag's threshold) */
    int n = 0;
    memset(kp_btn, 0, sizeof(kp_btn));
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (gmnu->buttons != NULL) && (n < KpB_Count); i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != gmnu->number))
            continue;
        if (gmnu->buttons[n].gbtype == -1)
            break;
        int r[4];
        kpc_rect(n, r);
        kp_btn[n] = gbtn;
        n++;
        gbtn->scr_pos_x = gmnu->pos_x + r[0] * upp / 16;
        gbtn->scr_pos_y = gmnu->pos_y + r[1] * upp / 16;
        gbtn->pos_x = gbtn->scr_pos_x;
        gbtn->pos_y = gbtn->scr_pos_y;
        gbtn->width = r[2] * upp / 16;
        gbtn->height = r[3] * upp / 16;
    }
}

/** A sunken box for a number: black with a thin rule round it. */
static void kpc_well(int x, int y, int w, int h)
{
    const int lw = max((kpc_upp + 8) / 32, 1);
    LbDrawBox(x, y, w, h, colours[0][0][0]);
    const TbPixel rule = world_window_ink(WwI_Rule);
    LbDrawBox(x, y, w, lw, rule);
    LbDrawBox(x, y + h - lw, w, lw, rule);
    LbDrawBox(x, y, lw, h, rule);
    LbDrawBox(x + w - lw, y, lw, h, rule);
}

/** A creature kind's portrait, sized w x w pixels at (x, y). */
static void kpc_portrait(int x, int y, int w, ThingModel model)
{
    const long spr_idx = get_creature_model_graphics(model, CGI_QuerySymbol);
    if (spr_idx <= 0)
        return;
    const struct TbSprite *spr = get_button_sprite_for_player(spr_idx, my_player_number);
    if ((spr == NULL) || (spr->SWidth == 0))
        return;
    LbSpriteDrawResized(x, y, w * 16 / spr->SWidth, spr);
}

/** The game's bar (draw_bar64k), lit while the mouse is on it or it is chosen, its word on its middle. */
static void kpc_bar(struct GuiButton *gbtn, TbBool lit, const char *word)
{
    const int bu = simple_button_sprite_height_units_per_px(gbtn, GBS_frontend_button_std_c, 100);
    const int cap = gbtn->height * 15 / 26;
    if (gbtn->width < 72 * bu / 16)
        return;
    draw_bar64k(gbtn->scr_pos_x, gbtn->scr_pos_y, bu, gbtn->width);
    if (lit)
    {
        const int rim = 6 * bu / 16;
        draw_lit_bar64k(gbtn->scr_pos_x - rim, gbtn->scr_pos_y - rim, bu, gbtn->width + rim);
    }
    world_window_display(gbtn->scr_pos_x + gbtn->height / 2, gbtn->scr_pos_y + (gbtn->height - cap) / 2,
        gbtn->width - gbtn->height, cap, 1, word);
}

/** The drag: a press on the top strip (the tabs' row and the frame over it) takes hold; once the mouse has moved a few
 *  pixels the window follows it until the button is let go, and the click that ends a drag does nothing else. */
static void kpc_drag(const struct GuiMenu *gmnu)
{
    const TbBool down = (lbDisplay.MLeftButton != 0);
    const int mx = GetMouseX(), my = GetMouseY();
    if (down && !kp_was_down)
    {
        kp_dragged = false;
        const int top = gmnu->pos_y - KPC_ORN_TOP * kpc_upp / 16, bottom = gmnu->pos_y + (KPC_TOP - 6) * kpc_upp / 16;
        kp_dragging = (mx >= gmnu->pos_x) && (mx < gmnu->pos_x + gmnu->width) && (my >= top) && (my < bottom);
        kp_grab_mx = mx;
        kp_grab_my = my;
        kp_grab_x = kp_off_x;
        kp_grab_y = kp_off_y;
    }
    if (down && kp_dragging)
    {
        const int dx = mx - kp_grab_mx, dy = my - kp_grab_my;
        if (kp_dragged || (abs(dx) + abs(dy) > max(4, kpc_upp / 8)))
        {
            kp_dragged = true;
            kp_off_x = kp_grab_x + dx;
            kp_off_y = kp_grab_y + dy;
        }
    }
    if (!down)
        kp_dragging = false;
    kp_was_down = down;
}

static void kpc_background(struct GuiMenu *gmnu)
{
    kpc_drag(gmnu);
    kpc_place(gmnu);
    const int upp = kpc_upp;
#define U(v) ((v) * upp / 16)
    const int gx = gmnu->pos_x, gy = gmnu->pos_y;
    world_window_frame(gx, gy, gmnu->width, gmnu->height, upp);
    const TbPixel rule = world_window_ink(WwI_Rule);
    const int lw = max((upp + 8) / 32, 1);
    LbDrawBox(gx + U(KPC_EDGE), gy + U(KPC_TOP - 6), U(KPC_W - 2 * KPC_EDGE), lw, rule);
    const PlayerNumber me = my_player_number;
    if (kp_tab != KpT_Profile)
    {
        const char *what = (kp_tab == KpT_Upgrades) ? "UPGRADES" : "SOCIAL";
        world_window_display(gx + U(KPC_EDGE), gy + U(110), U(KPC_W - 2 * KPC_EDGE), U(18), 1, what);
        world_window_plain(gx + U(KPC_EDGE), gy + U(142), U(KPC_W - 2 * KPC_EDGE), max(U(9), world_window_plain_cap()), 1,
            world_window_ink(WwI_Dim), "Coming soon.");
        return;
    }
    /* the name */
    const struct PlayerInfo *player = get_player(me);
    const char *name = ((player != NULL) && (player->player_name[0] != 0)) ? player->player_name : "Keeper";
    world_window_display(gx + U(KPC_EDGE), gy + U(KPC_TOP + 4), U(KPC_RIGHT_X - KPC_EDGE - 16), U(15), 0, name);
    /* the level and its bar: the XP since this level began, of what the next one needs */
    {
        const int lv = world_player_level(me);
        const uint32_t xp = world_player_xp(me);
        char line[64];
        snprintf(line, sizeof(line), "LEVEL %d", lv);
        const int ly = gy + U(KPC_TOP + 28 + KPC_PORTRAIT + 10);
        world_window_display(gx + U(KPC_EDGE), ly, U(160), U(11), 0, line);
        const int bw = U(200);
        const int pn = bw * 100 / 95; /* (the bar's own measures are 140ths of this: see the creature window's name bar) */
        const int bx = gx + U(KPC_EDGE), by = ly + U(17);
        const struct TbSprite *spr = get_panel_sprite(GPS_rpanel_bar_long_full);
        if ((spr != NULL) && (spr->SWidth > 0))
            draw_gui_panel_sprite_left(bx, by, bw * 16 / spr->SWidth, GPS_rpanel_bar_long_full);
        long lost = 0;
        if (lv >= WORLD_PLAYER_LEVEL_MAX)
            snprintf(line, sizeof(line), "%lu XP (highest level)", (unsigned long)xp);
        else
        {
            const uint32_t from = world_player_level_xp(lv), to = world_player_level_xp(lv + 1);
            const uint32_t have = (xp > from) ? (xp - from) : 0, need = (to > from) ? (to - from) : 1;
            lost = 126 - (long)(126.0 * (double)min(have, need) / (double)need);
            snprintf(line, sizeof(line), "%lu / %lu", (unsigned long)xp, (unsigned long)to);
        }
        lost = max(0L, min(lost, 126L));
        if (lost > 0)
            LbDrawBox(bx + ((128 - lost) * pn + 70) / 140, by + (4 * pn + 70) / 140, (lost * pn + 70) / 140, (14 * pn + 70) / 140,
                colours[0][0][0]);
        const int cap = max(U(9), world_window_plain_cap()); /* (as the statistics' numbers) */
        world_window_plain(bx, by + (4 * pn + 70) / 140 + ((14 * pn / 140) - cap) / 2, bw * 128 / 133, cap, 1,
            world_window_ink(WwI_Bright), line);
    }
    /* "Migration in progress" over its button */
    if (world_migrate_state(me) != WMig_None)
    {
        const int cap = max(U(8), world_window_plain_cap());
        world_window_plain(gx + U(KPC_EDGE), gy + U(KPC_FOOT_Y) - cap - U(5), U(150), cap, 1, world_window_ink(WwI_Gold),
            (world_migrate_state(me) == WMig_Going) ? "Migration in progress" : "Returning home");
    }
    /* the rank */
    {
        char line[32];
        snprintf(line, sizeof(line), "RANK #%d", kp_rank(me));
        const int rx = gx + U(KPC_W - KPC_EDGE - 128), ry = gy + U(KPC_FOOT_Y - 6);
        kpc_well(rx, ry, U(128), U(KPC_FOOT_H + 10));
        world_window_display(rx, ry + U(9), U(128), U(18), 1, line);
    }
    if (kp_picking)
    {
        /* the choice of portrait over the statistics */
        kp_find_kinds();
        world_window_display(gx + U(KPC_PICK_X), gy + U(KPC_TOP + 4), U(KPC_PICK_COLS * KPC_PICK_CELL), U(13), 1, "CHOOSE A PORTRAIT");
        if (kp_kinds_n == 0)
            world_window_plain(gx + U(KPC_PICK_X), gy + U(KPC_PICK_Y + 40), U(KPC_PICK_COLS * KPC_PICK_CELL),
                max(U(9), world_window_plain_cap()), 1, world_window_ink(WwI_Dim), "You have no creatures to choose from.");
        return;
    }
    /* the statistics */
    world_window_display(gx + U(KPC_RIGHT_X), gy + U(KPC_TOP + 4), U(KPC_W - KPC_EDGE - KPC_RIGHT_X), U(15), 2, "STATS");
    const struct WorldPlayerStats *st = world_player_stats(me);
    char vals[6][32];
    kp_time(st->play_turns, vals[0], sizeof(vals[0]));
    snprintf(vals[1], sizeof(vals[1]), "%lu", (unsigned long)st->hearts);
    snprintf(vals[2], sizeof(vals[2]), "%lu", (unsigned long)st->hearts_lost);
    snprintf(vals[3], sizeof(vals[3]), "%lu", (unsigned long)st->heroes_killed);
    snprintf(vals[4], sizeof(vals[4]), "%lu", (unsigned long)st->creatures_killed);
    snprintf(vals[5], sizeof(vals[5]), "%lu slabs", (unsigned long)st->dug_farthest);
    static const char *labels[6] = {"TIME PLAYED", "KEEPERS DEFEATED", "HEARTS LOST", "HEROES KILLED", "CREATURES KILLED",
        "LONGEST DISTANCE DUG"};
    for (int r = 0; r < 6; r++)
    {
        const int ry = gy + U(KPC_TOP + 30 + 26 * r);
        world_window_display(gx + U(KPC_RIGHT_X - 110), ry + U(4), U(KPC_VALUE_X - 8 - (KPC_RIGHT_X - 110)), U(9), 2, labels[r]);
        kpc_well(gx + U(KPC_VALUE_X), ry, U(KPC_VALUE_W), U(17));
        const int cap = max(U(9), world_window_plain_cap());
        world_window_plain(gx + U(KPC_VALUE_X + 4), ry + (U(17) - cap) / 2, U(KPC_VALUE_W - 8), cap, 2,
            world_window_ink(WwI_Bright), vals[r]);
    }
#undef U
}

static void kpc_close_draw(struct GuiButton *gbtn)
{
    kpc_bar(gbtn, (gbtn->flags & LbBtnF_MouseOver) != 0, "Close");
}

static void kpc_tab_draw(struct GuiButton *gbtn)
{
    static const char *names[3] = {"Profile", "Upgrades", "Social"};
    const int t = gbtn->content.lval;
    kpc_bar(gbtn, (t == kp_tab) || ((gbtn->flags & LbBtnF_MouseOver) != 0), names[(t >= 0) && (t < 3) ? t : 0]);
}

static void kpc_portrait_draw(struct GuiButton *gbtn)
{
    if (kp_tab != KpT_Profile)
        return;
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width;
    kpc_well(x, y, w, gbtn->height);
    const ThingModel model = world_player_stats(my_player_number)->portrait;
    if (model > 0)
        kpc_portrait(x, y, w, model); /* (to the frame's edges: the user, 2026-11-19) */
    else
    {
        world_window_display(x, y + w / 4, w, w / 3, 1, "?");
        world_window_plain(x, y + w * 3 / 4, w, max(w / 9, world_window_plain_cap()), 1, world_window_ink(WwI_Dim), "Choose");
    }
    if (((gbtn->flags & LbBtnF_MouseOver) != 0) || kp_picking)
    {
        const TbPixel lit = world_window_ink(WwI_Gold);
        const int lw = max((kpc_upp + 8) / 16, 1);
        LbDrawBox(x, y, w, lw, lit);
        LbDrawBox(x, y + gbtn->height - lw, w, lw, lit);
        LbDrawBox(x, y, lw, gbtn->height, lit);
        LbDrawBox(x + w - lw, y, lw, gbtn->height, lit);
    }
}

static void kpc_pick_draw(struct GuiButton *gbtn)
{
    const int k = gbtn->content.lval;
    if ((kp_tab != KpT_Profile) || !kp_picking || (k >= kp_kinds_n))
        return;
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width;
    kpc_well(x, y, w, gbtn->height);
    kpc_portrait(x + w / 10, y + w / 10, w * 8 / 10, kp_kinds[k]);
    if ((gbtn->flags & LbBtnF_MouseOver) != 0)
    {
        const TbPixel lit = world_window_ink(WwI_Gold);
        const int lw = max((kpc_upp + 8) / 16, 1);
        LbDrawBox(x, y, w, lw, lit);
        LbDrawBox(x, y + gbtn->height - lw, w, lw, lit);
        LbDrawBox(x, y, lw, gbtn->height, lit);
        LbDrawBox(x + w - lw, y, lw, gbtn->height, lit);
        /* its name under the grid */
        const struct GuiMenu *gmnu = get_active_menu(gbtn->gmenu_idx);
        if (gmnu != NULL)
            world_window_plain(gmnu->pos_x + KPC_PICK_X * kpc_upp / 16, gmnu->pos_y + (KPC_PICK_Y + KPC_PICK_ROWS * KPC_PICK_CELL + 4) * kpc_upp / 16,
                KPC_PICK_COLS * KPC_PICK_CELL * kpc_upp / 16, max(9 * kpc_upp / 16, world_window_plain_cap()), 1, world_window_ink(WwI_Bright),
                kp_kind_name(kp_kinds[k]));
    }
}

static void kpc_migrate_draw(struct GuiButton *gbtn)
{
    if (kp_tab != KpT_Profile)
        return;
    const int st = world_migrate_state(my_player_number);
    kpc_bar(gbtn, ((gbtn->flags & LbBtnF_MouseOver) != 0) && (st != WMig_Returning),
        (st == WMig_None) ? "Migrate Dungeon" : "Cancel Migration");
}

static void kpc_reset_draw(struct GuiButton *gbtn)
{
    if (kp_tab != KpT_Profile)
        return;
    const TbBool asked = (kp_reset_asked != 0) && (game.play_gameturn <= kp_reset_asked + 5 * (GameTurn)max(turns_per_second, 1));
    kpc_bar(gbtn, ((gbtn->flags & LbBtnF_MouseOver) != 0) || asked, asked ? "Are you sure?" : "Reset Dungeon");
}

/** A button's drawing: the classic look draws each; the modern one paints them all with the window. */
static void kp_button_draw(struct GuiButton *gbtn)
{
    if (window_style != WinStyle_Classic)
        return;
    int n = -1;
    for (int b = 0; (b < KpB_Count) && (n < 0); b++)
        if (kp_btn[b] == gbtn)
            n = b;
    if ((n >= KpB_Tab0) && (n <= KpB_Tab2))
        kpc_tab_draw(gbtn);
    else if (n == KpB_Close)
        kpc_close_draw(gbtn);
    else if (n == KpB_Portrait)
        kpc_portrait_draw(gbtn);
    else if (n == KpB_Migrate)
        kpc_migrate_draw(gbtn);
    else if (n == KpB_Reset)
        kpc_reset_draw(gbtn);
    else if (n >= KpB_Pick0)
        kpc_pick_draw(gbtn);
}

/******************************************************************************/
/* The buttons (painted with the window: they only take the mouse) */

static void kp_ground_rclick(struct GuiButton *gbtn)
{
    (void)gbtn;
    world_keeper_panel_close();
}

static void kp_close_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (kp_click_was_drag())
        return;
    world_keeper_panel_close();
}

static void kp_tab_click(struct GuiButton *gbtn)
{
    if (kp_click_was_drag())
        return;
    /* the Pentagram (2026-11-27): Upgrades opens its own screen, away from the game */
    if (gbtn->content.lval == KpT_Upgrades)
    {
        world_keeper_panel_close();
        world_pentagram_screen_show();
        return;
    }
    kp_tab = gbtn->content.lval;
    kp_picking = false;
    kp_reset_asked = 0;
}

static void kp_portrait_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (kp_click_was_drag())
        return;
    if (kp_tab != KpT_Profile)
        return;
    kp_picking = !kp_picking;
}

static void kp_pick_click(struct GuiButton *gbtn)
{
    if (kp_click_was_drag())
        return;
    const int k = gbtn->content.lval;
    if ((kp_tab != KpT_Profile) || !kp_picking || (k >= kp_kinds_n))
        return;
    set_players_packet_action(get_my_player(), PckA_WorldKeeperPanel, ((unsigned long)KpW_Portrait << 28), (unsigned long)kp_kinds[k], 0, 0);
    kp_picking = false;
}

static void kp_migrate_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (kp_click_was_drag())
        return;
    if (kp_tab != KpT_Profile)
        return;
    if (world_migrate_state(my_player_number) == WMig_Going)
    {
        world_migrate_request_cancel();
        return;
    }
    if (world_migrate_state(my_player_number) != WMig_None)
        return; /* (on its way back: nothing to do but wait) */
    if (!thing_exists(get_player_soul_container(my_player_number)))
        return;
    /* the panel closes and the Hand becomes the Soul Container (world_migrate.c) */
    world_keeper_panel_close();
    world_migrate_arm();
}

static void kp_reset_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (kp_click_was_drag())
        return;
    if (kp_tab != KpT_Profile)
        return;
    if (!kp_reset_is_asked())
    {
        kp_reset_asked = max(game.play_gameturn, (GameTurn)1);
        return;
    }
    kp_reset_asked = 0;
    set_players_packet_action(get_my_player(), PckA_WorldKeeperPanel, ((unsigned long)KpW_Reset << 28), 0, 0, 0);
    world_keeper_panel_close();
}

/* (places and sizes are set every frame by kp_place) */
#define KP_AT 0, 0, 0, 0, 1, 1
#define KP_BTN(click, rclick, n) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, click, rclick, NULL, 0, KP_AT, kp_button_draw, 0, GUIStr_Empty, 0, {.lval = (n)}, 0, NULL }
#define KP_PICK(k) KP_BTN(kp_pick_click, NULL, k)

static struct GuiButtonInit world_keeper_panel_buttons[] = {
  /* the window's own ground, under every other button: a right click on it closes the panel */
  KP_BTN(NULL, kp_ground_rclick, 0),
  KP_BTN(kp_tab_click, NULL, KpT_Profile), KP_BTN(kp_tab_click, NULL, KpT_Upgrades), KP_BTN(kp_tab_click, NULL, KpT_Social),
  KP_BTN(kp_close_click, NULL, 0),
  KP_BTN(kp_portrait_click, NULL, 0),
  KP_BTN(kp_migrate_click, NULL, 0),
  KP_BTN(kp_reset_click, NULL, 0),
  KP_PICK(0), KP_PICK(1), KP_PICK(2), KP_PICK(3), KP_PICK(4), KP_PICK(5), KP_PICK(6), KP_PICK(7),
  KP_PICK(8), KP_PICK(9), KP_PICK(10), KP_PICK(11), KP_PICK(12), KP_PICK(13), KP_PICK(14), KP_PICK(15),
  KP_PICK(16), KP_PICK(17), KP_PICK(18), KP_PICK(19), KP_PICK(20), KP_PICK(21), KP_PICK(22), KP_PICK(23),
  {              -1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, {.lval = 0}, 0, NULL },
};

struct GuiMenu world_keeper_panel_menu =
 { GMnu_KEEPER_PANEL, 0, 1, world_keeper_panel_buttons, POS_GAMECTR, POS_GAMECTR, (int)KP_WIDE_W, (int)KP_WIDE_H, kp_background, 0, NULL, NULL, 0, 0, 0,};

void world_keeper_panel_draw_last(void)
{
    if (!world_keeper_panel_open())
        return;
    const MenuNumber num = menu_id_to_number(GMnu_KEEPER_PANEL);
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
    RendererSetDrawFlags(flg_mem);
    LbTextSetFont(font_mem); /* (audit 2026-11-21: what is drawn after it took this font) */
}

/******************************************************************************/
/** Where the Soul Container's picture stands on the screen (its box: a slab wide and high, objects.cfg Size_XY/Size_Z),
 *  in pixels. False: not on the screen. */
TbBool map_to_screen(struct Coord3d *pos, int32_t *screen_x, int32_t *screen_y); /* engine_render.c */
static TbBool kp_container_on_screen(int *x0, int *y0, int *x1, int *y1)
{
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart) || (get_local_view_type() != PVT_DungeonTop))
        return false;
    /* (not before the view has been drawn once: its projection is set then - the first frame after a load came here
       first and called an empty one, 2026-11-26) */
    if (rotpers == NULL)
        return false;
    struct Coord3d base = heart->mappos, top = heart->mappos, a = heart->mappos, b = heart->mappos;
    const MapCoordDelta half = COORD_PER_STL * STL_PER_SLB / 2;
    top.z.val += (MapCoordDelta)max((int)heart->clipbox_size_z, (int)(COORD_PER_STL * STL_PER_SLB)) * 11 / 10;
    a.x.val -= half;
    b.x.val += half;
    int32_t bx, by, tx, ty, ax, ay, cx, cy;
    if (!map_to_screen(&base, &bx, &by) || !map_to_screen(&top, &tx, &ty) || !map_to_screen(&a, &ax, &ay) || !map_to_screen(&b, &cx, &cy))
        return false;
    const double slab_px = sqrt((double)(cx - ax) * (cx - ax) + (double)(cy - ay) * (cy - ay));
    const int w = (int)(slab_px * 0.55);
    *x0 = min(bx, tx) - w;
    *x1 = max(bx, tx) + w;
    *y0 = min(by, ty) - (int)(slab_px * 0.35); /* (its glow and crown stand above its box) */
    *y1 = max(by, ty) + (int)(slab_px * 0.3);
    return (*x1 > *x0) && (*y1 > *y0);
}

/** The pointer on the Soul Container's picture, with nothing else under the Hand first (a creature in front keeps it). */
static TbBool kp_container_hovered(void)
{
    if (local_state.local_thing_under_hand != 0)
    {
        const struct Thing *under = thing_get(local_state.local_thing_under_hand);
        if (thing_exists(under) && !object_is_clickable_by_player(under, my_player_number))
            return false;
    }
    int x0, y0, x1, y1;
    if (!kp_container_on_screen(&x0, &y0, &x1, &y1))
        return false;
    const int px = GetMouseX(), py = GetMouseY();
    return (px >= x0) && (px < x1) && (py >= y0) && (py < y1);
}

TbBool world_windows_escape(void)
{
    if (world_keeper_panel_open())
    {
        if (kp_picking)
            kp_picking = false; /* (the portrait choice closes first) */
        else
            world_keeper_panel_close();
        return true;
    }
    if (world_level_window_open())
    {
        world_level_window_close();
        return true;
    }
    return false;
}

TbBool world_hand_hover_local(void)
{
    if (!world_mode_active() || netstate_dedicated() || (get_local_view_type() != PVT_DungeonTop))
        return false;
    const struct UserState *ustate = get_local_user_state();
    if (user_state_invalid(ustate) || (ustate->work_state != PSt_CtrlDungeon) || !power_hand_is_empty(get_my_player()))
        return false;
    if (gameplay_cursor_is_over_gui() || world_keeper_panel_open() || world_level_window_open() || menu_is_active(GMnu_CREATURE_WINDOW))
        return false;
    return world_level_book_hovered() || kp_container_hovered();
}

void world_hand_draw_hover(void)
{
    if (!world_hand_hover_local() || power_hand_is_hovering(get_displayed_user()))
        return; /* (the game's own open Hand is drawn then) */
    const struct PlayerInfo *player = get_my_player();
    int32_t cx, cy;
    if ((player == NULL) || !get_gameplay_cursor_position(&cx, &cy))
        return;
    /* its frame as the game's own Hand steps it (anim_speed 256ths of a frame a turn); the game's Hand object's own frame
       when there is one, so that it goes on smoothly from it */
    const struct PowerHandConfigStats *hcs = &game.conf.power_hand_conf.pwrhnd_cfg_stats[player->hand_idx];
    const long anim = hcs->anim_idx[HndA_Hover];
    const long frames = max((long)keepersprite_frames((unsigned short)anim), 1L);
    long frame = (long)((game.play_gameturn * (unsigned long)max((int)hcs->anim_speed[HndA_Hover], 1)) / 256UL) % frames;
    const struct UserState *ustate = get_local_user_state();
    const struct Thing *handtng = user_state_invalid(ustate) ? INVALID_THING : thing_get(ustate->hand_thing_idx);
    if (thing_exists(handtng) && (handtng->anim_sprite == anim))
        frame = handtng->current_frame;
    RendererSubmitKeeperHandSprite(cx + scale_ui_value(60 * global_hand_scale), cy + scale_ui_value(40 * global_hand_scale),
        anim, 0, (unsigned char)frame, scale_ui_value(64 * global_hand_scale), RendererGetDrawFlags());
}

TbBool world_keeper_panel_inputs(void)
{
    if (world_migrate_inputs())
        return true;
    if (!world_mode_active() || !left_button_released)
        return false;
    const struct UserState *ustate = get_local_user_state();
    if ((get_local_view_type() != PVT_DungeonTop) || user_state_invalid(ustate) || (ustate->work_state != PSt_CtrlDungeon) ||
        gameplay_cursor_is_over_gui())
        return false;
    const struct Thing *thing = thing_get(local_state.local_thing_under_hand);
    if ((!thing_exists(thing) || !object_is_clickable_by_player(thing, my_player_number)) &&
        !(power_hand_is_empty(get_my_player()) && kp_container_hovered()))
        return false;
    left_button_released = 0;
    left_button_clicked = 0;
    if (world_keeper_panel_open())
        world_keeper_panel_close();
    else
        world_keeper_panel_show();
    return true;
}

void world_keeper_panel_packet(PlayerNumber p, unsigned long par1, unsigned long par2)
{
    if (!world_mode_active() || netstate_is_client() || (p < 0) || (p >= PLAYERS_COUNT) || !world_player_is_human(p))
        return;
    const int what = (int)(par1 >> 28);
    const MapSubtlCoord stl_x = (MapSubtlCoord)(par1 & 0x0FFFFFFFUL);
    switch (what)
    {
    case KpW_Migrate:
        world_migrate_packet(p, 1, stl_x, (MapSubtlCoord)par2);
        break;
    case KpW_Cancel:
        world_migrate_packet(p, 2, 0, 0);
        break;
    case KpW_Portrait:
        world_record_portrait(p, (ThingModel)par2);
        break;
    case KpW_Reset:
    {
        char why[128] = "";
        world_migrate_seat_reset(p);
        if (world_seat_reset(p, why, sizeof(why)))
            SYNCLOG("World: player %d reset its dungeon from the Keeper panel", (int)p);
        else
            SYNCLOG("World: player %d's dungeon reset from the Keeper panel failed: %s", (int)p, why);
        break;
    }
    default:
        break;
    }
}

/** KFX_WORLD_PANELTEST=turn (see the file's head); KFX_WORLD_CONTAINERHOVER=turn: the pointer on the Soul Container's
 *  picture (its upper part, where the ground under the pointer is not the Container's) from that turn on. */
void world_keeper_panel_turn(void)
{
    static long esc_at = -2;
    if (esc_at == -2)
    {
        const char *v = getenv("KFX_WORLD_PANELESC");
        esc_at = (v != NULL) ? atol(v) : -1;
    }
    if ((esc_at >= 0) && ((long)game.play_gameturn == esc_at))
    {
        lbKeyOn[KC_ESCAPE] = 1;
        SYNCLOG("World: escape test: ESC pressed (Keeper panel open %d, Library window open %d)", (int)world_keeper_panel_open(),
            (int)world_level_window_open());
    }
    if ((esc_at >= 0) && ((long)game.play_gameturn == esc_at + 2))
        SYNCLOG("World: escape test: after ESC: Keeper panel open %d, Library window open %d, pause menu open %d",
            (int)world_keeper_panel_open(), (int)world_level_window_open(), (int)menu_is_active(GMnu_OPTIONS));
    static long hand_at = -2;
    if (hand_at == -2)
    {
        const char *v = getenv("KFX_WORLD_CONTAINERHOVER");
        hand_at = (v != NULL) ? atol(v) : -1;
    }
    if ((hand_at >= 0) && ((long)game.play_gameturn >= hand_at) && !netstate_dedicated())
    {
        int x0, y0, x1, y1;
        if (kp_container_on_screen(&x0, &y0, &x1, &y1))
        {
            LbMouseSetPosition((x0 + x1) / 2, y0 + (y1 - y0) / 4);
            if ((long)game.play_gameturn == hand_at)
                SYNCLOG("World: container hover test: the pointer on the Soul Container's picture (%d..%d, %d..%d), open Hand %d", x0, x1, y0, y1,
                    (int)world_hand_hover_local());
        }
    }
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_PANELTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_PANELTEST=%ld", at);
    }
    if (world_keeper_panel_open() && (get_local_view_type() != PVT_DungeonTop))
        world_keeper_panel_close();
    world_migrate_local_turn();
    if ((at < 0) || netstate_dedicated())
        return;
    const long t = (long)game.play_gameturn;
    if (t == at)
        world_keeper_panel_show();
    else if (t == at + 60)
        kp_picking = true;
    else if (t == at + 100)
    {
        kp_find_kinds();
        SYNCLOG("World: panel test: %d kinds to choose from", kp_kinds_n);
        if (kp_kinds_n > 0)
        {
            struct GuiButton fake;
            memset(&fake, 0, sizeof(fake));
            fake.content.lval = 0;
            kp_pick_click(&fake);
        }
    } else if (t == at + 160)
        kp_tab = KpT_Upgrades;
    else if (t == at + 200)
    {
        kp_tab = KpT_Profile;
        const struct WorldPlayerStats *st = world_player_stats(my_player_number);
        SYNCLOG("World: panel test: level %d, %lu XP, rank %d, played %lu turns, %lu keepers, %lu lost, %lu heroes, "
            "%lu creatures, %lu slabs, portrait %d", world_player_level(my_player_number), (unsigned long)world_player_xp(my_player_number),
            kp_rank(my_player_number), (unsigned long)st->play_turns, (unsigned long)st->hearts, (unsigned long)st->hearts_lost,
            (unsigned long)st->heroes_killed, (unsigned long)st->creatures_killed, (unsigned long)st->dug_farthest, (int)st->portrait);
    } else if (t == at + 240)
    {
        /* Migrate Dungeon pressed: the panel closes and the Hand is the Soul Container with its 5x5 highlight */
        struct GuiButton fake;
        memset(&fake, 0, sizeof(fake));
        kp_migrate_click(&fake);
        /* (the pointer over the view, for the screenshot; with KFX_WORLD_MIGRATEPREP the camera is on its 5x5, 14 slabs
           east of the Heart, so that the press at +280 sets the Soul Container down there) */
        const struct Thing *heart = get_player_soul_container(my_player_number);
        if ((getenv("KFX_WORLD_MIGRATEPREP") != NULL) && thing_exists(heart))
            world_local_camera_to(heart->mappos.x.val + 14 * COORD_PER_SLB, heart->mappos.y.val);
        LbMouseSetPosition(local_state.engine_window_x + local_state.engine_window_width / 2,
            local_state.engine_window_y + local_state.engine_window_height / 2);
        SYNCLOG("World: panel test: Migrate Dungeon pressed, placing %d, panel open %d", (int)world_migrate_armed(),
            (int)world_keeper_panel_open());
    } else if (t == at + 320)
    {
        /* a trap chosen to place, the pointer over the view: its cursor box is a slab's (it once reached up to the
           map's edge on an endless map) */
        world_migrate_disarm();
        set_players_packet_action(get_my_player(), PckA_SetPlyrState, PSt_PlaceTrap, 1, 0, 0);
        LbMouseSetPosition(local_state.engine_window_x + local_state.engine_window_width / 2,
            local_state.engine_window_y + local_state.engine_window_height / 2);
        SYNCLOG("World: panel test: a trap chosen to place");
    } else if ((t == at + 280) || (t == at + 281))
    {
        /* a real mouse's press and release while placing (lbDisplay is what the game reads the mouse from) */
        lbDisplay.MLeftButton = (t == at + 280);
        if (t == at + 280)
            lbDisplay.LeftButton = 1;
        SYNCLOG("World: panel test: left button %s while placing", (t == at + 280) ? "down" : "up");
    }
}

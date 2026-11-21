/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keeper_panel.c
 *     Endless worlds: the Keeper panel (Profile, Upgrades, Social), opened by clicking the Soul Container.
 * @par Purpose:
 *     The user, 2026-11-19: "Turn the Soul Container into a clickable object [...] When Player hovers over, the hand
 *     should react to it just like it does when it hovers over creatures. When Player clicks on the Soul Container a ui
 *     panel opens up" (their mockup: Profile, Upgrades and Social tabs; the name, a portrait, the level and its XP bar,
 *     the statistics, Migrate Dungeon, Reset Dungeon, the rank). "Design it in a fitting art language for the game."
 *
 *     The Soul Container answers the Hand through its CLICKABLE property (objects.cfg; power_hand.c), so a map maker
 *     can give it to any object, or take it away. A click on this keeper's own one opens the panel here.
 *
 *     The panel is a window in the middle of the view in the creature window's dress (the game's ornate frame, its
 *     lettering for headings and buttons, the plain font for what is read, the side panel's bars and frames), set out
 *     at its own scale every frame so that all of it fits any screen. Its numbers are the keeper's lifetime record
 *     (struct WorldPlayerStats, world_progress.c) and its player level, in struct Game: a joined player sees them too.
 *     The portrait is one of the creature kinds the keeper has (a click on it shows them). The rank is its place
 *     among the keepers of this world, by level and then XP. Migrate Dungeon arms the placing of world_migrate.c (and
 *     is Cancel Migration while one goes on); Reset Dungeon (asked twice) is the Keeper Server's /reset: the dungeon
 *     starts again elsewhere, keeping level, XP and the record (the user, 2026-11-19). Upgrades and Social are empty
 *     for now (the user: "Empty tabs for now").
 *
 * Test hook: KFX_WORLD_PANELTEST=turn: at that turn the panel opens on Profile; 60 turns on the portrait choice shows,
 *     100 on the first kind is picked; 160 on Upgrades shows, 200 on Profile again; 240 on Migrate Dungeon is pressed
 *     (the placing stays armed), 320 on a trap is chosen to place (its cursor box): screenshots at those turns.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world_keeper_panel.h"
#include "bflib_basics.h"
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
#include "world_creature_window.h"
#include "world_migrate.h"
#include "world_progress.h"
#include "world_stream.h"
#include "vidfade.h"
#include "observer.h"
#include "local_camera.h"
#include "thing_list.h"
#include "config_strings.h"
#include "post_inc.h"

/* The window in its own units (a unit is a side panel's unit where there is room: kp_place). */
#define KP_W 480
#define KP_H 280
#define KP_EDGE 24
#define KP_TAB_Y 12
#define KP_TAB_H 24
#define KP_TOP 48                       /* under the tabs */
#define KP_FOOT_Y (KP_H - 40)           /* the buttons at the foot */
#define KP_FOOT_H 26
#define KP_RIGHT_X 250                  /* the statistics */
#define KP_VALUE_X 370
#define KP_VALUE_W (KP_W - KP_EDGE - KP_VALUE_X)
#define KP_PORTRAIT 84
/* the portrait choice: a grid in the statistics' place (right of the level's bar) */
#define KP_PICK_X (KP_W - KP_EDGE - KP_PICK_COLS * KP_PICK_CELL + 4)
#define KP_PICK_Y 70
#define KP_PICK_CELL 36
#define KP_PICK_COLS 6
#define KP_PICK_ROWS 4
#define KP_PICKS (KP_PICK_COLS * KP_PICK_ROWS)
/* the frame's ornaments past the window's edge (as the creature window's) */
#define KP_ORN_SIDE 34
#define KP_ORN_TOP 14
#define KP_ORN_BOTTOM 30
#define KP_AIR 2

enum KpTab { KpT_Profile = 0, KpT_Upgrades, KpT_Social };
enum KpWhat { KpW_Migrate = 1, KpW_Cancel = 2, KpW_Reset = 3, KpW_Portrait = 4 };

static int kp_upp = 16;
static int kp_tab;
static TbBool kp_picking;
static GameTurn kp_reset_asked;        /* Reset Dungeon was pressed once at this turn (0: not) */
static ThingModel kp_kinds[KP_PICKS];  /* the kinds the portrait choice shows, this frame */
/* dragging by the top strip (the user, 2026-11-19: "movable by click holding on the top part of the panel") */
static int kp_off_x, kp_off_y;         /* where the player put it, from the screen's middle (pixels) */
static TbBool kp_dragging, kp_dragged, kp_was_down;
static int kp_grab_mx, kp_grab_my, kp_grab_x, kp_grab_y;
static int kp_kinds_n;

/******************************************************************************/
TbBool world_keeper_panel_open(void)
{
    return world_mode_active() && !netstate_dedicated() && menu_is_active(GMnu_KEEPER_PANEL);
}

/** Sets the window out on the screen as it is now (its scale, its place, its buttons' places). The user, 2026-11-19:
 *  "Make the panel bigger so it also draws over the panel on the left": it stands on the middle of the whole screen
 *  (side panel and all), as large as fits most of it, and where the player dragged it (kp_drag), kept on the screen. */
static void kp_place(struct GuiMenu *gmnu)
{
    const int sw = MyScreenWidth, sh = MyScreenHeight;
    int upp = sw * 16 * 90 / 100 / (KP_W + 2 * (KP_ORN_SIDE + KP_AIR));
    upp = min(upp, sh * 16 * 86 / 100 / (KP_H + KP_ORN_TOP + KP_ORN_BOTTOM + 2 * KP_AIR));
    upp = max(upp, 4);
    kp_upp = upp;
    gmnu->width = KP_W * upp / 16;
    gmnu->height = KP_H * upp / 16;
    const int orn_top = KP_ORN_TOP * upp / 16, orn_bottom = KP_ORN_BOTTOM * upp / 16;
    int x = (sw - gmnu->width) / 2 + kp_off_x;
    int y = (sh - gmnu->height - orn_top - orn_bottom) / 2 + orn_top + kp_off_y;
    /* (kept on the screen: its body, if not all its ornaments) */
    x = max(0, min(x, sw - gmnu->width));
    y = max(orn_top / 2, min(y, sh - gmnu->height));
    kp_off_x = x - (sw - gmnu->width) / 2;
    kp_off_y = y - ((sh - gmnu->height - orn_top - orn_bottom) / 2 + orn_top);
    gmnu->pos_x = x;
    gmnu->pos_y = y;
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
            kp_place(get_active_menu(num)); /* (at once: the mouse is tested against the buttons before the first frame) */
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

/** A creature kind's portrait, sized w x w pixels at (x, y). */
static void kp_portrait(int x, int y, int w, ThingModel model)
{
    const long spr_idx = get_creature_model_graphics(model, CGI_QuerySymbol);
    if (spr_idx <= 0)
        return;
    const struct TbSprite *spr = get_button_sprite_for_player(spr_idx, my_player_number);
    if ((spr == NULL) || (spr->SWidth == 0))
        return;
    LbSpriteDrawResized(x, y, w * 16 / spr->SWidth, spr);
}

/** A sunken box for a number: black with a thin rule round it. */
static void kp_well(int x, int y, int w, int h)
{
    const int lw = max((kp_upp + 8) / 32, 1);
    LbDrawBox(x, y, w, h, colours[0][0][0]);
    const TbPixel rule = world_window_ink(WwI_Rule);
    LbDrawBox(x, y, w, lw, rule);
    LbDrawBox(x, y + h - lw, w, lw, rule);
    LbDrawBox(x, y, lw, h, rule);
    LbDrawBox(x + w - lw, y, lw, h, rule);
}

/** The game's bar (draw_bar64k), lit while the mouse is on it or it is chosen, its word on its middle. */
static void kp_bar(struct GuiButton *gbtn, TbBool lit, const char *word)
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
static void kp_drag(const struct GuiMenu *gmnu)
{
    const TbBool down = (lbDisplay.MLeftButton != 0);
    const int mx = GetMouseX(), my = GetMouseY();
    if (down && !kp_was_down)
    {
        kp_dragged = false;
        const int top = gmnu->pos_y - KP_ORN_TOP * kp_upp / 16, bottom = gmnu->pos_y + (KP_TOP - 6) * kp_upp / 16;
        kp_dragging = (mx >= gmnu->pos_x) && (mx < gmnu->pos_x + gmnu->width) && (my >= top) && (my < bottom);
        kp_grab_mx = mx;
        kp_grab_my = my;
        kp_grab_x = kp_off_x;
        kp_grab_y = kp_off_y;
    }
    if (down && kp_dragging)
    {
        const int dx = mx - kp_grab_mx, dy = my - kp_grab_my;
        if (kp_dragged || (abs(dx) + abs(dy) > max(4, kp_upp / 8)))
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

static void kp_background(struct GuiMenu *gmnu)
{
    kp_drag(gmnu);
    kp_place(gmnu);
    const int upp = kp_upp;
#define U(v) ((v) * upp / 16)
    const int gx = gmnu->pos_x, gy = gmnu->pos_y;
    world_window_frame(gx, gy, gmnu->width, gmnu->height, upp);
    const TbPixel rule = world_window_ink(WwI_Rule);
    const int lw = max((upp + 8) / 32, 1);
    LbDrawBox(gx + U(KP_EDGE), gy + U(KP_TOP - 6), U(KP_W - 2 * KP_EDGE), lw, rule);
    const PlayerNumber me = my_player_number;
    if (kp_tab != KpT_Profile)
    {
        const char *what = (kp_tab == KpT_Upgrades) ? "UPGRADES" : "SOCIAL";
        world_window_display(gx + U(KP_EDGE), gy + U(110), U(KP_W - 2 * KP_EDGE), U(18), 1, what);
        world_window_plain(gx + U(KP_EDGE), gy + U(142), U(KP_W - 2 * KP_EDGE), max(U(9), world_window_plain_cap()), 1,
            world_window_ink(WwI_Dim), "Coming soon.");
        return;
    }
    /* the name */
    const struct PlayerInfo *player = get_player(me);
    const char *name = ((player != NULL) && (player->player_name[0] != 0)) ? player->player_name : "Keeper";
    world_window_display(gx + U(KP_EDGE), gy + U(KP_TOP + 4), U(KP_RIGHT_X - KP_EDGE - 16), U(15), 0, name);
    /* the level and its bar: the XP since this level began, of what the next one needs */
    {
        const int lv = world_player_level(me);
        const uint32_t xp = world_player_xp(me);
        char line[64];
        snprintf(line, sizeof(line), "LEVEL %d", lv);
        const int ly = gy + U(KP_TOP + 28 + KP_PORTRAIT + 10);
        world_window_display(gx + U(KP_EDGE), ly, U(160), U(11), 0, line);
        const int bw = U(200);
        const int pn = bw * 100 / 95; /* (the bar's own measures are 140ths of this: see the creature window's name bar) */
        const int bx = gx + U(KP_EDGE), by = ly + U(17);
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
        world_window_plain(gx + U(KP_EDGE), gy + U(KP_FOOT_Y) - cap - U(5), U(150), cap, 1, world_window_ink(WwI_Gold),
            (world_migrate_state(me) == WMig_Going) ? "Migration in progress" : "Returning home");
    }
    /* the rank */
    {
        char line[32];
        snprintf(line, sizeof(line), "RANK #%d", kp_rank(me));
        const int rx = gx + U(KP_W - KP_EDGE - 128), ry = gy + U(KP_FOOT_Y - 6);
        kp_well(rx, ry, U(128), U(KP_FOOT_H + 10));
        world_window_display(rx, ry + U(9), U(128), U(18), 1, line);
    }
    if (kp_picking)
    {
        /* the choice of portrait over the statistics */
        kp_find_kinds();
        world_window_display(gx + U(KP_PICK_X), gy + U(KP_TOP + 4), U(KP_PICK_COLS * KP_PICK_CELL), U(13), 1, "CHOOSE A PORTRAIT");
        if (kp_kinds_n == 0)
            world_window_plain(gx + U(KP_PICK_X), gy + U(KP_PICK_Y + 40), U(KP_PICK_COLS * KP_PICK_CELL),
                max(U(9), world_window_plain_cap()), 1, world_window_ink(WwI_Dim), "You have no creatures to choose from.");
        return;
    }
    /* the statistics */
    world_window_display(gx + U(KP_RIGHT_X), gy + U(KP_TOP + 4), U(KP_W - KP_EDGE - KP_RIGHT_X), U(15), 2, "STATS");
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
        const int ry = gy + U(KP_TOP + 30 + 26 * r);
        world_window_display(gx + U(KP_RIGHT_X - 110), ry + U(4), U(KP_VALUE_X - 8 - (KP_RIGHT_X - 110)), U(9), 2, labels[r]);
        kp_well(gx + U(KP_VALUE_X), ry, U(KP_VALUE_W), U(17));
        const int cap = max(U(9), world_window_plain_cap());
        world_window_plain(gx + U(KP_VALUE_X + 4), ry + (U(17) - cap) / 2, U(KP_VALUE_W - 8), cap, 2,
            world_window_ink(WwI_Bright), vals[r]);
    }
#undef U
}

/******************************************************************************/
/* The buttons */

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

static void kp_close_draw(struct GuiButton *gbtn)
{
    kp_bar(gbtn, (gbtn->flags & LbBtnF_MouseOver) != 0, "Close");
}

static void kp_tab_click(struct GuiButton *gbtn)
{
    if (kp_click_was_drag())
        return;
    kp_tab = gbtn->content.lval;
    kp_picking = false;
    kp_reset_asked = 0;
}

static void kp_tab_draw(struct GuiButton *gbtn)
{
    static const char *names[3] = {"Profile", "Upgrades", "Social"};
    const int t = gbtn->content.lval;
    kp_bar(gbtn, (t == kp_tab) || ((gbtn->flags & LbBtnF_MouseOver) != 0), names[(t >= 0) && (t < 3) ? t : 0]);
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

static void kp_portrait_draw(struct GuiButton *gbtn)
{
    if (kp_tab != KpT_Profile)
        return;
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width;
    kp_well(x, y, w, gbtn->height);
    const ThingModel model = world_player_stats(my_player_number)->portrait;
    if (model > 0)
        kp_portrait(x, y, w, model); /* (to the frame's edges: the user, 2026-11-19) */
    else
    {
        world_window_display(x, y + w / 4, w, w / 3, 1, "?");
        world_window_plain(x, y + w * 3 / 4, w, max(w / 9, world_window_plain_cap()), 1, world_window_ink(WwI_Dim), "Choose");
    }
    if (((gbtn->flags & LbBtnF_MouseOver) != 0) || kp_picking)
    {
        const TbPixel lit = world_window_ink(WwI_Gold);
        const int lw = max((kp_upp + 8) / 16, 1);
        LbDrawBox(x, y, w, lw, lit);
        LbDrawBox(x, y + gbtn->height - lw, w, lw, lit);
        LbDrawBox(x, y, lw, gbtn->height, lit);
        LbDrawBox(x + w - lw, y, lw, gbtn->height, lit);
    }
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

static void kp_pick_draw(struct GuiButton *gbtn)
{
    const int k = gbtn->content.lval;
    if ((kp_tab != KpT_Profile) || !kp_picking || (k >= kp_kinds_n))
        return;
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width;
    kp_well(x, y, w, gbtn->height);
    kp_portrait(x + w / 10, y + w / 10, w * 8 / 10, kp_kinds[k]);
    if ((gbtn->flags & LbBtnF_MouseOver) != 0)
    {
        const TbPixel lit = world_window_ink(WwI_Gold);
        const int lw = max((kp_upp + 8) / 16, 1);
        LbDrawBox(x, y, w, lw, lit);
        LbDrawBox(x, y + gbtn->height - lw, w, lw, lit);
        LbDrawBox(x, y, lw, gbtn->height, lit);
        LbDrawBox(x + w - lw, y, lw, gbtn->height, lit);
        /* its name under the grid */
        const struct GuiMenu *gmnu = get_active_menu(gbtn->gmenu_idx);
        if (gmnu != NULL)
            world_window_plain(gmnu->pos_x + KP_PICK_X * kp_upp / 16, gmnu->pos_y + (KP_PICK_Y + KP_PICK_ROWS * KP_PICK_CELL + 4) * kp_upp / 16,
                KP_PICK_COLS * KP_PICK_CELL * kp_upp / 16, max(9 * kp_upp / 16, world_window_plain_cap()), 1, world_window_ink(WwI_Bright),
                kp_kind_name(kp_kinds[k]));
    }
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

static void kp_migrate_draw(struct GuiButton *gbtn)
{
    if (kp_tab != KpT_Profile)
        return;
    const int st = world_migrate_state(my_player_number);
    kp_bar(gbtn, ((gbtn->flags & LbBtnF_MouseOver) != 0) && (st != WMig_Returning),
        (st == WMig_None) ? "Migrate Dungeon" : "Cancel Migration");
}

static void kp_reset_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (kp_click_was_drag())
        return;
    if (kp_tab != KpT_Profile)
        return;
    if ((kp_reset_asked == 0) || (game.play_gameturn > kp_reset_asked + 5 * (GameTurn)max(turns_per_second, 1)))
    {
        kp_reset_asked = max(game.play_gameturn, (GameTurn)1);
        return;
    }
    kp_reset_asked = 0;
    set_players_packet_action(get_my_player(), PckA_WorldKeeperPanel, ((unsigned long)KpW_Reset << 28), 0, 0, 0);
    world_keeper_panel_close();
}

static void kp_reset_draw(struct GuiButton *gbtn)
{
    if (kp_tab != KpT_Profile)
        return;
    const TbBool asked = (kp_reset_asked != 0) && (game.play_gameturn <= kp_reset_asked + 5 * (GameTurn)max(turns_per_second, 1));
    kp_bar(gbtn, ((gbtn->flags & LbBtnF_MouseOver) != 0) || asked, asked ? "Are you sure?" : "Reset Dungeon");
}

#define KP_AT(x, y, w, h) (x), (y), (x), (y), (w), (h)
#define KP_TAB(t) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, kp_tab_click, NULL, NULL, 0, \
    KP_AT(KP_EDGE + 100 * (t), KP_TAB_Y, 96, KP_TAB_H), kp_tab_draw, 0, GUIStr_Empty, 0, {.lval = (t)}, 0, NULL }
#define KP_PICK(k) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, kp_pick_click, NULL, NULL, 0, \
    KP_AT(KP_PICK_X + KP_PICK_CELL * ((k) % KP_PICK_COLS), KP_PICK_Y + KP_PICK_CELL * ((k) / KP_PICK_COLS), KP_PICK_CELL - 4, KP_PICK_CELL - 4), \
    kp_pick_draw, 0, GUIStr_Empty, 0, {.lval = (k)}, 0, NULL }

static struct GuiButtonInit world_keeper_panel_buttons[] = {
  /* the window's own ground, under every other button: a right click on it closes the panel */
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, kp_ground_rclick, NULL, 0, KP_AT(0, 0, KP_W, KP_H), NULL, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  KP_TAB(KpT_Profile), KP_TAB(KpT_Upgrades), KP_TAB(KpT_Social),
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, kp_close_click, NULL, NULL, 0, KP_AT(KP_W - KP_EDGE - 96, KP_TAB_Y, 96, KP_TAB_H), kp_close_draw, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, kp_portrait_click, NULL, NULL, 0, KP_AT(KP_EDGE, KP_TOP + 26, KP_PORTRAIT, KP_PORTRAIT), kp_portrait_draw, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, kp_migrate_click, NULL, NULL, 0, KP_AT(KP_EDGE, KP_FOOT_Y, 150, KP_FOOT_H), kp_migrate_draw, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, kp_reset_click, NULL, NULL, 0, KP_AT(KP_EDGE + 162, KP_FOOT_Y, 140, KP_FOOT_H), kp_reset_draw, 0, GUIStr_Empty, 0, {.lval = 0}, 0, NULL },
  KP_PICK(0), KP_PICK(1), KP_PICK(2), KP_PICK(3), KP_PICK(4), KP_PICK(5), KP_PICK(6), KP_PICK(7),
  KP_PICK(8), KP_PICK(9), KP_PICK(10), KP_PICK(11), KP_PICK(12), KP_PICK(13), KP_PICK(14), KP_PICK(15),
  KP_PICK(16), KP_PICK(17), KP_PICK(18), KP_PICK(19), KP_PICK(20), KP_PICK(21), KP_PICK(22), KP_PICK(23),
  {              -1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, {.lval = 0}, 0, NULL },
};

struct GuiMenu world_keeper_panel_menu =
 { GMnu_KEEPER_PANEL, 0, 1, world_keeper_panel_buttons, POS_GAMECTR, POS_GAMECTR, KP_W, KP_H, kp_background, 0, NULL, NULL, 0, 0, 0,};

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
    if (!thing_exists(thing) || !object_is_clickable_by_player(thing, my_player_number))
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

/** KFX_WORLD_PANELTEST=turn (see the file's head). */
void world_keeper_panel_turn(void)
{
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

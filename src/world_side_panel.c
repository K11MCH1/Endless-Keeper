/******************************************************************************/
// Endless Keeper - the side panel in the Molten look
/******************************************************************************/
/** @file world_side_panel.c
 *     The side panel in the launcher's Molten look (see world_side_panel.h).
 * @par Design:
 *     The game's buttons of the main menu and of the Rooms, Research and Workshop tabs are moved each frame to the
 *     layout of the mockup the user approved (2026-11-26, with their changes: no gold ring round the minimap but a
 *     hollow pressed into the glass, zoom out and in at the minimap's lower corners, a solid map icon beside the
 *     cogwheel, sharp digits, icons in the middle of their wells). Their clicks, tooltips and upkeep are the game's;
 *     the drawing of those this file paints is left out (draw_menu_buttons asks world_side_panel_paints).
 *     The minimap keeps the game's place and size (its clicks and the Hand's drops on it are the game's): the layout
 *     is laid round it, in panel units of 2/3 of the game's menu unit (the mockup's: 2 pixels at 1440 high).
 *     Pictures: the panel (slot 4) and the gold (slot 5) under the game's UI, so the minimap, the event buttons, the
 *     tooltips and the Hand are over them; the compass letters (slot 3) over the minimap.
 *     The Information and Creatures tabs keep the game's drawing for now (stage two).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_side_panel.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_guibtns.h"
#include "bflib_math.h"
#include "bflib_sprite.h"
#include "bflib_video.h"
#include "config_keeperfx.h"
#include "config_magic.h"
#include "config_strings.h"
#include "frontmenu_options.h"
#include "kjm_input.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "frontend.h"
#include "frontmenu_ingame_map.h"
#include "front_simple.h"
#include "game_legacy.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "local_camera.h"
#include "magic_powers.h"
#include "net_state.h"
#include "player_data.h"
#include "power_process.h"
#include "room_workshop.h"
#include "sprites.h"
#include "thing_doors.h"
#include "ui_molten.h"
#include "world_fill.h"
#include "world_progress.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "chat_box.h"
#include "gui_draw.h"
#include "creature_states_rsrch.h"
#include "world_colours.h"
#include "config_spritecolors.h"
#include "frontmenu_ingame_tabs.h"
#include "keeperfx.hpp"
#include "creature_graphics.h"
#include "config_creature.h"
#include "config.h"
#include <spng.h>
#include "gui_vscroll.h"
#include "packets.h"
#include "frontmenu_ingame_evnt.h"
#include "map_events.h"
#include "world_seasons.h"
#include "ui_safe_area.h"
#include "ui_front.h"
#include "thing_creature.h"
#include "thing_stats.h"
#include "creature_instances.h"
#include "local_possession.h"
#include "world_misdeeds.h"
#include "world_creature_window.h"
#include "world_pentagram_hooks.h"
#include "world_pentagram.h"
#include "post_inc.h"

/* the game's draw functions of the buttons painted here (frontmenu_ingame_tabs.c, frontmenu_ingame_evnt.c) */
void gui_area_room_button(struct GuiButton *gbtn);
void gui_area_spell_button(struct GuiButton *gbtn);
void gui_area_trap_button(struct GuiButton *gbtn);
void gui_area_event_button(struct GuiButton *gbtn);
void gui_area_trap_build_info_button(struct GuiButton *gbtn);
long find_room_type_capacity_total_percentage(PlayerNumber plyr_idx, RoomKind rkind);
long anger_get_creature_highest_anger_type_and_byte_percentage(struct Thing *creatng, int32_t *out_angr_typ, int32_t *out_angr_prct);

/******************************************************************************/
/* The layout, in panel units from the menu's origin (the minimap's middle is at 103.5, 103.5) */

#define SP_GLASS_X0 6.0f
#define SP_GLASS_X1 201.0f
#define SP_GLASS_Y0 6.0f
#define SP_MAP_C 103.5f
#define SP_CORNER 28.0f       /* the round buttons at the minimap's corners */
#define SP_GOLD_Y 200.0f
#define SP_GOLD_H 38.0f
#define SP_TABS_Y 244.0f
#define SP_TAB_W 32.0f
#define SP_TAB_H 36.0f
#define SP_TAB_STEP 35.0f
#define SP_COL_X 17.5f        /* the tabs', strip's and grid's left edge */
#define SP_COL_W 172.0f
#define SP_STRIP_Y 288.0f
#define SP_STRIP_H 42.0f
#define SP_GRID_Y 338.0f
#define SP_CELL 40.0f
#define SP_CELL_STEP 44.0f
#define SP_NEXT_Y 524.0f
#define SP_NEXT_H 20.0f
#define SP_NEXT_W 90.0f
#define SP_EV 30.0f           /* the event buttons */
#define SP_EV_STEP 36.0f
#define SP_EV_Y 534.0f

enum SpRole {
    SpR_None = 0, SpR_Options, SpR_ZoomIn, SpR_ZoomOut, SpR_Map, SpR_Assist, SpR_Gold, SpR_Strip, SpR_Next, SpR_TrapInfo,
    SpR_Tab0, /* .. SpR_Tab0 + 4 */
    SpR_Cell0 = SpR_Tab0 + 5, /* .. SpR_Cell0 + 15 */
    SpR_Ev0 = SpR_Cell0 + 16, /* the event buttons, .. SpR_Ev0 + 12 (EV01 the lowest) */
    SpR_Q0 = SpR_Ev0 + 13,    /* the Information tab's buttons in their list's order, .. SpR_Q0 + 18 */
    SpR_C0 = SpR_Q0 + 19,     /* the Creatures tab's, .. SpR_C0 + 28 */
    SpR_P0 = SpR_C0 + 29,     /* a creature's pages (taken over; queried outside endless worlds), .. SpR_P0 + 12 */
    SpR_Count = SpR_P0 + 13
};

static float sp_u = 2.0f;                   /* pixels a panel unit */
static float sp_ox, sp_oy;                  /* the menu's origin on the screen */
static float sp_map_cx, sp_map_cy, sp_map_r; /* the minimap on the screen (the game's) */
static struct GuiButton *sp_btn[SpR_Count]; /* this frame's buttons by role */
static int sp_tab_menu = -1;                /* the tab whose grid is up (GMnu_ROOM...), -1: none of those */
static int sp_info_menu = -1;               /* GMnu_QUERY, GMnu_CREATURE or a creature's page (sp_cq_ident) when up */
static TbBool sp_placed;
struct SpQuery;
struct SpCrRow;
static const struct SpQuery *sp_q_now;   /* the Information or Creatures tab's state as painted this frame */
static const struct SpCrRow *sp_rows_now;

/** The modern side panel: in every level (endless worlds, campaigns, map packs, multiplayer) unless the window style
 *  is Classic; the day's dial and the World Boss strip are endless pieces of their own. */
TbBool world_side_panel_on(void)
{
    return (window_style != WinStyle_Classic) && !netstate_dedicated() && (menu_id_to_number(GMnu_MAIN) >= 0);
}

static int sp_px(float v)
{
    return (int)floorf(v + 0.5f);
}

static void sp_set(struct GuiButton *gbtn, float x, float y, float w, float h)
{
    gbtn->scr_pos_x = (short)sp_px(sp_ox + x * sp_u);
    gbtn->scr_pos_y = (short)sp_px(sp_oy + y * sp_u);
    gbtn->pos_x = gbtn->scr_pos_x;
    gbtn->pos_y = gbtn->scr_pos_y;
    gbtn->width = (short)max(1, sp_px(w * sp_u));
    gbtn->height = (short)max(1, sp_px(h * sp_u));
}

static TbBool sp_tab_menu_ident(int ident)
{
    return (ident == GMnu_ROOM) || (ident == GMnu_ROOM2) || (ident == GMnu_SPELL) || (ident == GMnu_SPELL2) ||
        (ident == GMnu_SPELL_LOST) || (ident == GMnu_TRAP) || (ident == GMnu_TRAP2);
}

/** A creature's pages in the side panel: its abilities (1, 2) and its statistics (3, 4); the Rap Sheet (5) keeps the
 *  game's drawing. */
static TbBool sp_cq_ident(int ident)
{
    return (ident == GMnu_CREATURE_QUERY1) || (ident == GMnu_CREATURE_QUERY2) || (ident == GMnu_CREATURE_QUERY3) ||
        (ident == GMnu_CREATURE_QUERY4);
}

static TbBool sp_cq_stats_page(void)
{
    return (sp_info_menu == GMnu_CREATURE_QUERY3) || (sp_info_menu == GMnu_CREATURE_QUERY4);
}

/** A tab menu's button by the place the game gives it (its init's units): its role. */
static int sp_tab_role(const struct GuiButtonInit *init)
{
    if ((init->width == 32) && (init->height == 36) && (init->scr_pos_y >= 238))
    {
        const int col = max(0, min(3, (init->scr_pos_x - 2 + 16) / 32));
        const int row = max(0, min(3, (init->scr_pos_y - 238 + 19) / 38));
        return SpR_Cell0 + row * 4 + col;
    }
    if (init->width == 126)
        return SpR_Strip;
    if ((init->scr_pos_x == 78) && (init->scr_pos_y == 188))
        return SpR_Next;
    if ((init->scr_pos_x == 110) && (init->scr_pos_y == 215))
        return SpR_TrapInfo;
    return SpR_None;
}

static int sp_main_role(const struct GuiButtonInit *init)
{
    switch (init->id_num)
    {
    case BID_OPTIONS: return SpR_Options;
    case BID_MAP_ZOOM_IN: return SpR_ZoomIn;
    case BID_MAP_ZOOM_OU: return SpR_ZoomOut;
    case BID_MAP_ZOOM_FS: return SpR_Map;
    case BID_ASSIST: return SpR_Assist;
    case BID_INFO_TAB: return SpR_Tab0 + 0;
    case BID_ROOM_TAB: return SpR_Tab0 + 1;
    case BID_SPELL_TAB: return SpR_Tab0 + 2;
    case BID_MNFCT_TAB: return SpR_Tab0 + 3;
    case BID_CREATR_TAB: return SpR_Tab0 + 4;
    case BID_MSG_EV01: case BID_MSG_EV02: case BID_MSG_EV03: case BID_MSG_EV04: case BID_MSG_EV05: case BID_MSG_EV06:
    case BID_MSG_EV07: case BID_MSG_EV08: case BID_MSG_EV09: case BID_MSG_EV10: case BID_MSG_EV11: case BID_MSG_EV12:
    case BID_MSG_EV13:
        return SpR_Ev0 + (init->id_num - BID_MSG_EV01);
    default:
        break;
    }
    if ((init->id_num == BID_DEFAULT) && (init->tooltip_stridx == GUIStr_TeamMoneyAvailable))
        return SpR_Gold;
    return SpR_None;
}

static float sp_glass_bottom(void)
{
    if ((sp_info_menu == GMnu_QUERY) || (sp_info_menu == GMnu_CREATURE) || sp_cq_ident(sp_info_menu))
        return 576.0f;
    return ((sp_btn[SpR_Next] != NULL) && (sp_btn[SpR_Next]->flags & LbBtnF_Visible)) ? SP_NEXT_Y + SP_NEXT_H + 8.0f :
        SP_GRID_Y + 3.0f * SP_CELL_STEP + SP_CELL + 8.0f;
}

/** The place of a role's button (panel units). */
static float sp_ev_col(void); /* (the event buttons' column: where the panel has slid) */
static void sp_role_rect(int role, float *r)
{
    const float lo = SP_MAP_C - 79.5f - SP_CORNER * 0.5f, hi = SP_MAP_C + 79.5f - SP_CORNER * 0.5f;
    r[2] = r[3] = SP_CORNER;
    switch (role)
    {
    case SpR_Map: r[0] = lo; r[1] = lo; return;
    case SpR_Options: r[0] = hi; r[1] = lo; return;
    case SpR_ZoomOut: r[0] = lo; r[1] = hi; return;
    case SpR_ZoomIn: r[0] = hi; r[1] = hi; return;
    case SpR_Assist: r[0] = 12.0f; r[1] = SP_GOLD_Y + 7.0f; r[2] = r[3] = 24.0f; return;
    case SpR_Gold: r[0] = 40.0f; r[1] = SP_GOLD_Y; r[2] = 127.0f; r[3] = SP_GOLD_H; return;
    case SpR_Strip: r[0] = SP_COL_X; r[1] = SP_STRIP_Y; r[2] = SP_COL_W; r[3] = SP_STRIP_H; return;
    case SpR_Next: r[0] = SP_MAP_C - SP_NEXT_W * 0.5f; r[1] = SP_NEXT_Y; r[2] = SP_NEXT_W; r[3] = SP_NEXT_H; return;
    case SpR_TrapInfo: r[0] = SP_COL_X + SP_COL_W - 22.0f; r[1] = SP_STRIP_Y + 4.0f; r[2] = 16.0f; r[3] = 20.0f; return;
    default:
        break;
    }
    if ((role >= SpR_Tab0) && (role < SpR_Tab0 + 5))
    {
        r[0] = SP_COL_X + (float)(role - SpR_Tab0) * SP_TAB_STEP;
        r[1] = SP_TABS_Y;
        r[2] = SP_TAB_W;
        r[3] = SP_TAB_H;
        return;
    }
    if ((role >= SpR_Cell0) && (role < SpR_Cell0 + 16))
    {
        const int k = role - SpR_Cell0;
        r[0] = SP_COL_X + (float)(k % 4) * SP_CELL_STEP;
        r[1] = SP_GRID_Y + (float)(k / 4) * SP_CELL_STEP;
        r[2] = r[3] = SP_CELL;
        return;
    }
    if ((role >= SpR_Q0) && (role < SpR_Q0 + 19))
    {
        /* the Information tab: tendencies, payday, research and workshop, the keepers' rows, the two page buttons */
        static const float q[19][4] = {
            {105.5f, 548.0f, 84.0f, 24.0f}, {17.5f, 548.0f, 84.0f, 24.0f},     /* query a creature, more keepers */
            {17.5f, 288.0f, 84.0f, 28.0f}, {105.5f, 288.0f, 84.0f, 28.0f},     /* imprison, flee */
            {17.5f, 324.0f, 172.0f, 36.0f},                                    /* payday */
            {17.5f, 368.0f, 84.0f, 36.0f}, {105.5f, 368.0f, 84.0f, 36.0f},     /* research, workshop */
            {117.5f, 414.0f, 72.0f, 28.0f}, {117.5f, 446.0f, 72.0f, 28.0f}, {117.5f, 478.0f, 72.0f, 28.0f}, {117.5f, 510.0f, 72.0f, 28.0f},
            {17.5f, 414.0f, 70.0f, 28.0f}, {17.5f, 446.0f, 70.0f, 28.0f}, {17.5f, 478.0f, 70.0f, 28.0f}, {17.5f, 510.0f, 70.0f, 28.0f},
            {91.5f, 417.0f, 22.0f, 22.0f}, {91.5f, 449.0f, 22.0f, 22.0f}, {91.5f, 481.0f, 22.0f, 22.0f}, {91.5f, 513.0f, 22.0f, 22.0f}};
        memcpy(r, q[role - SpR_Q0], sizeof(q[0]));
        return;
    }
    if ((role >= SpR_C0) && (role < SpR_C0 + 29))
    {
        /* the Creatures tab: the three activities over their columns, up and down, six rows of a kind and its counts */
        const int n = role - SpR_C0;
        if (n < 3)
        {
            r[0] = 54.0f + (float)n * 46.0f; r[1] = 288.0f; r[2] = 43.5f; r[3] = 28.0f;
        } else if (n == 3)
        {
            r[0] = 19.5f; r[1] = 288.0f; r[2] = r[3] = 28.0f;
        } else if (n == 4)
        {
            r[0] = 19.5f; r[1] = 538.0f; r[2] = r[3] = 28.0f;
        } else
        {
            const int row = (n - 5) / 4, col = (n - 5) % 4;
            r[1] = 324.0f + (float)row * 35.0f;
            r[3] = 32.0f;
            if (col == 0)
            {
                r[0] = 17.5f; r[2] = 32.0f;
            } else
            {
                r[0] = 54.0f + (float)(col - 1) * 46.0f; r[2] = 43.5f;
            }
        }
        return;
    }
    if ((role >= SpR_P0) && (role < SpR_P0 + 13))
    {
        /* a creature's pages: the page button at the foot, as the tabs' "next" */
        const int n = role - SpR_P0;
        r[0] = r[1] = r[2] = r[3] = 0.0f;
        if (n == 0)
        {
            r[0] = SP_MAP_C - SP_NEXT_W * 0.5f; r[1] = 536.0f; r[2] = SP_NEXT_W; r[3] = 22.0f;
        } else if (sp_cq_stats_page())
        {
            /* the statistics: the name (and the pencil while it can rename), ten statistics in two columns */
            const TbBool pencil = (sp_btn[SpR_P0 + 2] != NULL) && ((sp_btn[SpR_P0 + 2]->flags & LbBtnF_Visible) != 0);
            if (n == 1)
            {
                r[0] = 17.5f; r[1] = 288.0f; r[2] = pencil ? 140.0f : 172.0f; r[3] = 28.0f;
            }
            else if (n == 2)
            {
                r[0] = 161.5f; r[1] = 288.0f; r[2] = r[3] = 28.0f;
            }
            else
            {
                const int k = n - 3;
                r[0] = 17.5f + (float)(k % 2) * 88.0f; r[1] = 326.0f + (float)(k / 2) * 40.0f; r[2] = 84.0f; r[3] = 34.0f;
            }
        } else if (n <= 9)
        {
            /* the abilities: the mood and the level beside the portrait (sp_cq_portrait), the name, six abilities */
            if (n == 1)
            {
                r[0] = 101.5f; r[1] = 288.0f; r[2] = 88.0f; r[3] = 36.0f;
            }
            else if (n == 2)
            {
                r[0] = 101.5f; r[1] = 330.0f; r[2] = 88.0f; r[3] = 36.0f;
            }
            else if (n == 3)
            {
                r[0] = 17.5f; r[1] = 374.0f; r[2] = 172.0f; r[3] = 28.0f;
            }
            else
            {
                const int k = n - 4;
                r[0] = 17.5f + (float)(k % 2) * 88.0f; r[1] = 410.0f + (float)(k / 2) * 40.0f; r[2] = 84.0f; r[3] = 34.0f;
            }
        }
        return;
    }
    if ((role >= SpR_Ev0) && (role < SpR_Ev0 + 13))
    {
        /* the event buttons: a column of round buttons beside the glass, the first at the foot */
        r[0] = sp_ev_col();
        r[1] = SP_EV_Y - (float)(role - SpR_Ev0) * SP_EV_STEP;
        r[2] = r[3] = SP_EV;
        return;
    }
    r[0] = r[1] = r[2] = r[3] = 0.0f;
}

/* The panel slides away (the user, 2026-11-26): mode 1, by the tab under the event buttons, tucks the panel off the
   screen's left with its event buttons against the edge; mode 2, by TAB, sends the panel and the event buttons off to
   the left and the day's dial and the World Boss strip off to the right. Each part slides on its own (0 home, 1 away). */
static int sp_hide_mode;
static float sp_a_panel, sp_a_events, sp_a_right;
static unsigned long sp_anim_ms;
#define SP_SLIDE_MS 360.0f

static float sp_ease(float a)
{
    a = (a < 0.0f) ? 0.0f : ((a > 1.0f) ? 1.0f : a);
    return a * a * (3.0f - 2.0f * a);
}

static void sp_animate(void)
{
    /* test switch KFX_WORLD_SLIDETEST=mode[,turn]: at that turn (200) the panel slides as its tab (1) or TAB (2) would */
    static int test_mode = -1;
    static long test_turn = 200;
    if (test_mode < 0)
    {
        const char *v = getenv("KFX_WORLD_SLIDETEST");
        test_mode = (v != NULL) ? atoi(v) : 0;
        if ((v != NULL) && (strchr(v, ',') != NULL))
            test_turn = atol(strchr(v, ',') + 1);
    }
    static long test_back = -1;
    if ((test_back < 0) && (getenv("KFX_WORLD_SLIDETEST") != NULL))
    {
        const char *c2 = strchr(getenv("KFX_WORLD_SLIDETEST"), ',');
        c2 = (c2 != NULL) ? strchr(c2 + 1, ',') : NULL;
        test_back = (c2 != NULL) ? atol(c2 + 1) : 0;
    }
    if ((test_mode > 0) && ((long)game.play_gameturn == test_turn))
        sp_hide_mode = test_mode;
    if ((test_back > 0) && ((long)game.play_gameturn == test_back))
        sp_hide_mode = 0; /* (...and at the third number, back) */
    const unsigned long now = (unsigned long)LbTimerClock();
    const float step = (sp_anim_ms == 0) ? 1.0f : min(1.0f, (float)(now - sp_anim_ms) / SP_SLIDE_MS);
    sp_anim_ms = now;
    const float tp = (sp_hide_mode >= 1) ? 1.0f : 0.0f, te = (sp_hide_mode == 2) ? 1.0f : 0.0f;
    sp_a_panel += max(-step, min(step, tp - sp_a_panel));
    sp_a_events += max(-step, min(step, te - sp_a_events));
    sp_a_right += max(-step, min(step, te - sp_a_right));
}

/** The game's minimap is left out while the panel is away or on its way (it cannot slide: its place is kept on the
 *  screen), and so are its clicks. */
TbBool world_side_minimap_away(void)
{
    return world_side_hud_modern() && (sp_a_panel > 0.001f);
}

/** How far the dial and the World Boss strip have slid right (TAB): both alike, as far as the wider needs to leave. */
static int sp_right_slide(float u)
{
    return (int)(sp_ease(sp_a_right) * (250.0f * u + (float)((int)MyScreenWidth - ui_safe_right_edge())));
}

/** The dial and the strip slid right and still (TAB). */
static TbBool sp_right_away(void)
{
    return (sp_hide_mode == 2) && (sp_a_right >= 1.0f);
}

TbBool world_side_hud_toggle(void)
{
    if (!world_side_hud_modern())
        return false;
    sp_hide_mode = (sp_hide_mode == 2) ? 0 : 2;
    SYNCLOG("World: the side panel %s (TAB)", (sp_hide_mode == 2) ? "and the dial slide away" : "comes back");
    return true;
}

static float sp_shift; /* the panel's slide, in pixels (0 or less) */

/** Where the minimap is (as the game computes it each frame: draw_whole_status_panel, get_small_map_inputs). */
static void sp_geometry(void)
{
    const struct GuiMenu *gmnu = get_active_menu(menu_id_to_number(GMnu_MAIN));
    const int mm_upp = max(1, (gmnu->width * 16 + 140 / 2) / 140);
    const long mx = scale_value_for_resolution_with_upp(local_state.minimap_pos_x, mm_upp);
    const long my = scale_value_for_resolution_with_upp(local_state.minimap_pos_y, mm_upp);
    const int diag = 2 * (PANEL_MAP_RADIUS * mm_upp / 16);
    sp_map_r = (float)diag * 0.5f;
    sp_map_cx = (float)mx + sp_map_r;
    sp_map_cy = (float)my + sp_map_r;
    sp_u = (float)mm_upp / 24.0f; /* (the game's menu unit is mm_upp/16 pixels; a panel unit is 2/3 of it) */
    sp_ox = sp_map_cx - SP_MAP_C * sp_u;
    sp_oy = sp_map_cy - SP_MAP_C * sp_u;
    /* (slid away: the whole panel, its glass's shadow too, off the screen's left) */
    sp_shift = -sp_ease(sp_a_panel) * (sp_ox + (SP_GLASS_X1 + 30.0f) * sp_u);
    sp_ox += sp_shift;
    sp_map_cx += sp_shift;
}

/** The event buttons' column (panel units from sp_ox): beside the glass; with the panel tucked against the screen's left
 *  edge; with everything away, off the screen. */
static float sp_ev_col(void)
{
    const float home = SP_GLASS_X1 + 6.0f;
    const float edge = ((float)ui_safe_left + 8.0f * sp_u - sp_ox) / sp_u;
    const float tucked = max(home, edge);
    const float gone = (-(SP_EV + 30.0f) * sp_u - sp_ox) / sp_u;
    return tucked + (gone - tucked) * sp_ease(sp_a_events);
}

/** The tab under the event buttons (panel units from sp_ox): x, y, w, h. */
static void sp_tab_rect(float *r)
{
    /* (as large as an event button, one of their steps below the lowest: the user, 2026-11-26) */
    r[0] = sp_ev_col();
    r[2] = SP_EV;
    r[3] = SP_EV;
    r[1] = min(SP_EV_Y + SP_EV_STEP, ((float)MyScreenHeight - sp_oy) / sp_u - r[3] - 6.0f);
}

void world_side_panel_place(void)
{
    if (world_side_hud_modern())
        sp_animate();
    else
        sp_hide_mode = 0, sp_a_panel = sp_a_events = sp_a_right = 0.0f;
    memset(sp_btn, 0, sizeof(sp_btn));
    sp_tab_menu = -1;
    sp_info_menu = -1;
    sp_placed = false;
    if (!world_side_panel_on())
        return;
    sp_geometry();
    for (int k = 0; k < ACTIVE_MENUS_COUNT; k++)
    {
        struct GuiMenu *gmnu = &active_menus[k];
        if ((gmnu->visual_state == 0) || (gmnu->buttons == NULL))
            continue;
        const TbBool main = (gmnu->ident == GMnu_MAIN);
        const TbBool info = ((gmnu->ident == GMnu_QUERY) || (gmnu->ident == GMnu_CREATURE) || sp_cq_ident(gmnu->ident)) &&
            gmnu->is_turned_on;
        if (!main && !info && (!sp_tab_menu_ident(gmnu->ident) || !gmnu->is_turned_on))
            continue;
        if (info)
            sp_info_menu = gmnu->ident;
        else if (!main)
            sp_tab_menu = gmnu->ident;
        /* (a menu's buttons are made in the order of its list, each in the first free place: in order) */
        int n = 0;
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            struct GuiButton *gbtn = &active_buttons[i];
            if (((gbtn->flags & LbBtnF_Active) == 0) || (gbtn->gmenu_idx != gmnu->number))
                continue;
            if (gmnu->buttons[n].gbtype == -1)
                break;
            const int role = main ? sp_main_role(&gmnu->buttons[n]) : (gmnu->ident == GMnu_QUERY) ? ((n < 19) ? SpR_Q0 + n : SpR_None) :
                (gmnu->ident == GMnu_CREATURE) ? ((n < 29) ? SpR_C0 + n : SpR_None) :
                sp_cq_ident(gmnu->ident) ? ((n < 13) ? SpR_P0 + n : SpR_None) : sp_tab_role(&gmnu->buttons[n]);
            n++;
            if (role == SpR_None)
                continue;
            sp_btn[role] = gbtn;
        }
    }
    for (int role = 1; role < SpR_Count; role++)
    {
        if (sp_btn[role] == NULL)
            continue;
        float r[4];
        sp_role_rect(role, r);
        sp_set(sp_btn[role], r[0], r[1], r[2], r[3]);
    }
    sp_placed = true;
}

TbBool world_side_panel_paints(const struct GuiButton *gbtn)
{
    if (!sp_placed || (gbtn == NULL))
        return false;
    for (int role = 1; role < SpR_Count; role++)
        if (sp_btn[role] == gbtn)
            return (role != SpR_TrapInfo) && !((role >= SpR_Ev0) && chat_box_icon_button(gbtn));
    return false;
}

/******************************************************************************/
/* What a cell shows */

struct SpCell {
    int show;           /* 0: an empty well, 1: an icon, 2: a question mark (to research), 3: the sell sign */
    long spr;
    TbBool down, hot, chosen, active, dark;
    int level;          /* 0: none */
    TbBool reached;
    float bar;          /* <0: none */
};

/** The game's blink: true in the half of its beat where blinking things show their other picture. */
static TbBool sp_beat(void)
{
    return (get_gameturn() % (2 * max(gui_blink_rate, 1))) >= gui_blink_rate;
}

static TbBool sp_blink(void)
{
    const unsigned long half = (unsigned long)max(turns_per_second / 4, 1);
    return ((get_gameturn() / half) % 2) != 0;
}

static void sp_bar_of(int kind_class, int kind, struct SpCell *cell)
{
    int xp = 0, need = 0;
    if (world_amenity_bar(my_player_number, kind_class, kind, &xp, &need) && (xp > 0) && (need > 0))
        cell->bar = (float)min(xp, need) / (float)need;
}

static void sp_cell_of(const struct GuiButton *gbtn, struct SpCell *cell)
{
    memset(cell, 0, sizeof(*cell));
    cell->bar = -1.0f;
    if ((gbtn == NULL) || ((gbtn->flags & LbBtnF_Visible) == 0))
        return;
    cell->hot = ((gbtn->flags & LbBtnF_MouseOver) != 0);
    cell->down = (gbtn->button_state_left_pressed != 0) || (gbtn->button_state_right_pressed != 0);
    const struct UserState *ustate = get_user_state_prediction(get_displayed_user());
    struct Dungeon *dungeon = get_my_dungeon();
    if (gbtn->draw_call == gui_area_new_no_anim_button)
    {
        if (gbtn->sprite_idx == GPS_rpanel_frame_portrt_sell)
        {
            cell->show = 3;
            cell->chosen = (ustate->work_state == PSt_Sell);
        }
        return;
    }
    if (gbtn->draw_call == gui_area_room_button)
    {
        const RoomKind rkind = gbtn->content.lval;
        if (!((dungeon->room_buildable[rkind] & 1) || (dungeon->room_resrchable[rkind] == 1) ||
            (dungeon->room_resrchable[rkind] == 2) || ((dungeon->room_resrchable[rkind] == 4) && (dungeon->room_buildable[rkind] & 2))))
            return;
        if ((gbtn->flags & LbBtnF_Enabled) == 0)
        {
            cell->show = 2;
            return;
        }
        cell->show = 1;
        cell->spr = gbtn->sprite_idx + ((dungeon->total_money_owned < get_room_kind_stats(rkind)->cost) ? 1 : 0);
        cell->chosen = (ustate->work_state == PSt_BuildRoom) && (ustate->chosen_room_kind == rkind);
        world_room_level_badge(my_player_number, rkind, &cell->level, &cell->reached);
        sp_bar_of(0, rkind, cell);
        return;
    }
    if (gbtn->draw_call == gui_area_spell_button)
    {
        const PowerKind pwkind = gbtn->content.lval;
        if (!dungeon->magic_resrchable[pwkind] && (dungeon->magic_level[pwkind] <= 0))
            return;
        if ((gbtn->flags & LbBtnF_Enabled) == 0)
        {
            if ((pwkind == PwrK_HOLDAUDNC) && (dungeon->magic_level[pwkind] > 0))
            {
                cell->show = 1;
                cell->spr = gbtn->sprite_idx + 1;
            } else
                cell->show = 2;
            return;
        }
        const struct PowerConfigStats *powerst = get_power_model_stats(pwkind);
        const int ws = powerst->work_state;
        cell->show = 1;
        cell->spr = gbtn->sprite_idx + ((dungeon->total_money_owned < compute_power_price(dungeon->owner, pwkind, 0)) ? 1 : 0);
        cell->active = ((ws == PSt_CallToArms) && player_uses_power_call_to_arms(my_player_number)) ||
            ((ws == PSt_SightOfEvil) && player_uses_power_sight(my_player_number)) ||
            ((pwkind == PwrK_OBEY) && player_uses_power_obey(my_player_number));
        cell->chosen = (ws != PSt_None) && (ws != PSt_CtrlDungeon) && (ustate->work_state == ws) &&
            (game.chosen_spell_type == pwkind);
        cell->dark = (((ws == PSt_CallToArms) && player_uses_power_call_to_arms(my_player_number)) ||
            ((ws == PSt_SightOfEvil) && player_uses_power_sight(my_player_number))) && !sp_beat();
        world_power_level_badge(my_player_number, pwkind, &cell->level, &cell->reached);
        sp_bar_of(1, pwkind, cell);
        return;
    }
    if (gbtn->draw_call == gui_area_trap_button)
    {
        const int manufctr_idx = gbtn->content.lval;
        const struct ManufactureData *manufctr = get_manufacture_data(manufctr_idx);
        const ThingModel model = manufctr->tngmodel;
        TbBool buildable, placeable, built;
        if (manufctr->tngclass == TCls_Trap)
        {
            buildable = is_trap_buildable(my_player_number, model);
            placeable = is_trap_placeable(my_player_number, model);
            built = is_trap_built(my_player_number, model);
        } else if (manufctr->tngclass == TCls_Door)
        {
            buildable = is_door_buildable(my_player_number, model);
            placeable = is_door_placeable(my_player_number, model);
            built = is_door_built(my_player_number, model);
        } else
            return;
        if (!buildable && !placeable && !built)
            return;
        cell->chosen = (game.manufactr_element == manufctr_idx) &&
            (((manufctr->tngclass == TCls_Trap) && (ustate->work_state == PSt_PlaceTrap) && (ustate->chosen_trap_kind == model)) ||
             ((manufctr->tngclass == TCls_Door) && (ustate->work_state == PSt_PlaceDoor) && (ustate->chosen_door_kind == model)));
        if (buildable && !placeable && !built)
        {
            cell->show = 2; /* (the Workshop makes it once researched: a question mark, as the game's) */
            return;
        }
        unsigned int amount;
        if (manufctr->tngclass == TCls_Trap)
        {
            cell->active = player_has_deployed_trap_of_model(my_player_number, model);
            amount = dungeon->mnfct_info.trap_amount_placeable[model];
        } else
        {
            cell->active = player_has_deployed_door_of_model(my_player_number, model, -1);
            amount = dungeon->mnfct_info.door_amount_placeable[model];
        }
        cell->active = false; /* (placed ones are many: the game's light frame for them is left out, it lit nearly all) */
        cell->show = 1;
        cell->spr = gbtn->sprite_idx + ((amount < 1) ? 1 : 0);
        if (manufctr->tngclass == TCls_Trap)
        {
            world_trap_level_badge(my_player_number, model, &cell->level, &cell->reached);
            sp_bar_of(2, model, cell);
        }
        return;
    }
}

/** The chosen icon's strip: the big picture, the name, the price or the number to place, the bar. False: nothing. */
struct SpStrip {
    long spr;
    char name[64];
    char value[24];
    TbBool dim, dark;
    float bar;
};

static TbBool sp_strip_of(const struct GuiButton *gbtn, struct SpStrip *s)
{
    memset(s, 0, sizeof(*s));
    s->bar = -1.0f;
    if ((gbtn == NULL) || (sp_tab_menu < 0))
        return false;
    struct Dungeon *dungeon = get_my_dungeon();
    const struct UserState *ustate = get_user_state_prediction(get_displayed_user());
    if ((sp_tab_menu == GMnu_ROOM) || (sp_tab_menu == GMnu_ROOM2))
    {
        const RoomKind rkind = gbtn->content.lval;
        if ((rkind == RoK_NONE) || (gbtn->sprite_idx <= 0))
            return false;
        const struct RoomConfigStats *roomst = get_room_kind_stats(rkind);
        const int boxsize = max(1, (int)ustate->boxsize);
        long cost = (long)world_pentagram_room_cost(my_player_number, rkind, roomst->cost); /* (Gilded Halls) */
        if ((ustate->work_state == PSt_BuildRoom) && (boxsize > 1))
            cost *= boxsize;
        s->dim = (ustate->render_roomspace.total_roomspace_cost > dungeon->total_money_owned);
        s->dark = !s->dim && (ustate->work_state == PSt_BuildRoom) && (ustate->chosen_room_kind == game.chosen_room_kind) && !sp_beat();
        s->spr = gbtn->sprite_idx + (s->dim ? 1 : 0);
        snprintf(s->name, sizeof(s->name), "%s", get_string(roomst->name_stridx));
        snprintf(s->value, sizeof(s->value), "%ld", cost);
        const int i = world_fill_is_kind(rkind) ? world_fill_meter(my_player_number) :
            find_room_type_capacity_total_percentage(my_player_number, rkind);
        if ((rkind != RoK_ENTRANCE) && (rkind != RoK_DUNGHEART) && (i >= 0))
            s->bar = (float)min(i, 256) / 256.0f;
        return true;
    }
    if ((sp_tab_menu == GMnu_SPELL) || (sp_tab_menu == GMnu_SPELL2) || (sp_tab_menu == GMnu_SPELL_LOST))
    {
        const PowerKind pwkind = gbtn->content.lval;
        const struct PowerConfigStats *powerst = get_power_model_stats(pwkind);
        if (power_model_stats_invalid(powerst) || (gbtn->sprite_idx <= 0))
            return false;
        const GoldAmount price = compute_power_price(dungeon->owner, pwkind, 0);
        s->dim = (dungeon->total_money_owned < price);
        s->dark = !s->dim && (ustate->work_state == powerst->work_state) && sp_beat();
        s->spr = gbtn->sprite_idx + (s->dim ? 1 : 0);
        snprintf(s->name, sizeof(s->name), "%s", get_string(powerst->name_stridx));
        snprintf(s->value, sizeof(s->value), "%ld", (long)price);
        const int age = find_spell_age_percentage(my_player_number, pwkind);
        if (((powerst->config_flags & PwCF_HasProgress) != 0) && (age >= 0))
            s->bar = (float)min(age, 256) / 256.0f;
        return true;
    }
    if ((sp_tab_menu == GMnu_TRAP) || (sp_tab_menu == GMnu_TRAP2))
    {
        const int manufctr_idx = gbtn->content.lval;
        if ((manufctr_idx == 0) || (gbtn->sprite_idx <= 0))
            return false;
        const struct ManufactureData *manufctr = get_manufacture_data(manufctr_idx);
        const ThingModel model = manufctr->tngmodel;
        unsigned int amount = 0;
        TextStringId name = 0;
        if (manufctr->tngclass == TCls_Trap)
        {
            if (is_trap_buildable(my_player_number, model) && !is_trap_placeable(my_player_number, model) && !is_trap_built(my_player_number, model))
                return false;
            amount = dungeon->mnfct_info.trap_amount_placeable[model];
            name = game.conf.trapdoor_conf.trap_cfgstats[model].name_stridx;
        } else if (manufctr->tngclass == TCls_Door)
        {
            if (is_door_buildable(my_player_number, model) && !is_door_placeable(my_player_number, model) && !is_door_built(my_player_number, model))
                return false;
            amount = dungeon->mnfct_info.door_amount_placeable[model];
            name = game.conf.trapdoor_conf.door_cfgstats[model].name_stridx;
        } else
            return false;
        s->dim = (amount < 1);
        s->dark = !s->dim && (((manufctr->tngclass == TCls_Trap) && (ustate->chosen_trap_kind == model) && (ustate->work_state == PSt_PlaceTrap)) ||
            ((manufctr->tngclass == TCls_Door) && (ustate->chosen_door_kind == model) && (ustate->work_state == PSt_PlaceDoor))) && !sp_beat();
        s->spr = gbtn->sprite_idx + (s->dim ? 1 : 0);
        snprintf(s->name, sizeof(s->name), "%s", get_string(name));
        snprintf(s->value, sizeof(s->value), "\xC3\x97%u", amount); /* (x, the times sign) */
        return true;
    }
    return false;
}

/******************************************************************************/
/* The tab icons and the sell sign: the game's own, cut off their stone and frames (the mockup's cut, 2026-11-26) */

static unsigned char *sp_cut_px[11];
static int sp_cut_w[11], sp_cut_h[11];
static TbBool sp_cut_tried[11];

/** The game's drop shadow colour under its icons, (20,16,0): taken off so an icon stands in the middle of its place. */
static TbBool sp_shadow(const unsigned char *p)
{
    return (p[0] == 20) && (p[1] == 16) && (p[2] == 0);
}

static int sp_lum(const unsigned char *p)
{
    return (p[0] * 3 + p[1] * 6 + p[2]) / 10;
}

static unsigned char *sp_sprite(long idx, int *w, int *h)
{
    const struct TbSprite *spr = get_panel_sprite((short)idx);
    if ((spr == NULL) || (spr->SWidth == 0) || (spr->SHeight == 0) || (engine_palette == NULL))
        return NULL;
    unsigned char *px = (unsigned char *)calloc((size_t)spr->SWidth * spr->SHeight, 4);
    if (px == NULL)
        return NULL;
    ui_sprite_rgba(spr, engine_palette, px);
    *w = spr->SWidth;
    *h = spr->SHeight;
    return px;
}

/** Keeps of `mask` its largest group of touching pixels and the groups of at least `least` pixels. */
static void sp_keep_groups(unsigned char *mask, int w, int h, int least)
{
    int *lab = (int *)calloc((size_t)w * h, sizeof(int));
    int *stack = (int *)malloc(sizeof(int) * (size_t)w * h);
    int sizes[1024];
    int n = 0;
    if ((lab == NULL) || (stack == NULL))
    {
        free(lab);
        free(stack);
        return;
    }
    for (int i = 0; i < w * h; i++)
    {
        if (!mask[i] || lab[i] || (n >= 1023))
            continue;
        n++;
        int sp = 0, count = 0;
        stack[sp++] = i;
        lab[i] = n;
        while (sp > 0)
        {
            const int q = stack[--sp];
            count++;
            const int qx = q % w, qy = q / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    const int nx = qx + dx, ny = qy + dy;
                    if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h))
                        continue;
                    const int nq = ny * w + nx;
                    if (mask[nq] && !lab[nq])
                    {
                        lab[nq] = n;
                        stack[sp++] = nq;
                    }
                }
        }
        sizes[n] = count;
    }
    int big = 0;
    for (int k = 1; k <= n; k++)
        if ((big == 0) || (sizes[k] > sizes[big]))
            big = k;
    for (int i = 0; i < w * h; i++)
        mask[i] = (lab[i] != 0) && ((lab[i] == big) || (sizes[lab[i]] >= least));
    free(lab);
    free(stack);
}

/** The picture of `px` where mask is set, cropped to it (NULL: nothing). */
static void sp_cut_store(int k, unsigned char *px, const unsigned char *mask, int w, int h)
{
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            if (!mask[y * w + x])
            {
                px[(y * w + x) * 4 + 3] = 0;
                continue;
            }
            x0 = min(x0, x);
            y0 = min(y0, y);
            x1 = max(x1, x);
            y1 = max(y1, y);
        }
    if (x1 < x0)
        return;
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    unsigned char *o = (unsigned char *)malloc((size_t)cw * ch * 4);
    if (o == NULL)
        return;
    for (int y = 0; y < ch; y++)
        memcpy(o + (size_t)y * cw * 4, px + ((size_t)(y + y0) * w + x0) * 4, (size_t)cw * 4);
    sp_cut_px[k] = o;
    sp_cut_w[k] = cw;
    sp_cut_h[k] = ch;
}

static void sp_cut_tab(int k, int v)
{
    /* the stone's colours: the tabs' bottom rows and their left edge */
    static uint32_t stone[512];
    static int nstone = -1;
    if (nstone < 0)
    {
        nstone = 0;
        for (long s = GPS_rpanel_rpanel_tab_infoa; s <= GPS_rpanel_rpanel_tab_crtrd; s++)
        {
            int w = 0, h = 0;
            unsigned char *px = sp_sprite(s, &w, &h);
            if (px == NULL)
                continue;
            for (int y = 14; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    if (!((y >= 34) || (x < 4)))
                        continue;
                    const unsigned char *p = px + ((size_t)y * w + x) * 4;
                    if (p[3] == 0)
                        continue;
                    const uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                    int j = 0;
                    while ((j < nstone) && (stone[j] != c))
                        j++;
                    if ((j == nstone) && (nstone < 512))
                        stone[nstone++] = c;
                }
            free(px);
        }
    }
    static const short boxes[5][4] = {{3, 2, 22, 33}, {3, 1, 25, 33}, {4, 3, 24, 33}, {4, 2, 22, 33}, {4, 2, 24, 33}};
    int w = 0, h = 0;
    unsigned char *px = sp_sprite(GPS_rpanel_rpanel_tab_infoa + 2 * k + v, &w, &h);
    if (px == NULL)
        return;
    unsigned char *m = (unsigned char *)calloc((size_t)w * h, 1);
    unsigned char *rowf = (unsigned char *)calloc((size_t)w * h, 1);
    unsigned char *colf = (unsigned char *)calloc((size_t)w * h, 1);
    if ((m == NULL) || (rowf == NULL) || (colf == NULL))
    {
        free(px); free(m); free(rowf); free(colf);
        return;
    }
    for (int i = 0; i < w * h; i++)
    {
        const unsigned char *p = px + (size_t)i * 4;
        if (p[3] == 0)
            continue;
        const uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        int j = 0;
        while ((j < nstone) && (stone[j] != c))
            j++;
        m[i] = (j == nstone);
        /* the query sign is green (some of its greens are the stone's too) */
        if ((k == 0) && (p[1] >= p[0]) && (p[1] > p[2] + 20) && (p[1] > 60))
            m[i] = 1;
        /* the hammer's head and handle (its greys are the stone's) */
        if (k == 3)
        {
            const int x = i % w, y = i / w;
            if ((((y >= 3) && (y < 14) && (x >= 5) && (x < 22)) || ((y >= 12) && (y < 33) && (x >= 11) && (x < 17))) && (sp_lum(p) > 40))
                m[i] = 1;
        }
    }
    sp_keep_groups(m, w, h, 3);
    /* within its box; the holes inside it on both its row and its column filled; one pixel of its dark outline */
    const short *b = boxes[k];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((x < b[0]) || (y < b[1]) || (x > b[2]) || (y > b[3]))
                m[y * w + x] = 0;
    for (int y = 0; y < h; y++)
    {
        int a = -1, z = -1;
        for (int x = 0; x < w; x++)
            if (m[y * w + x]) { if (a < 0) a = x; z = x; }
        for (int x = a; (a >= 0) && (x <= z); x++)
            rowf[y * w + x] = 1;
    }
    for (int x = 0; x < w; x++)
    {
        int a = -1, z = -1;
        for (int y = 0; y < h; y++)
            if (m[y * w + x]) { if (a < 0) a = y; z = y; }
        for (int y = a; (a >= 0) && (y <= z); y++)
            colf[y * w + x] = 1;
    }
    for (int i = 0; i < w * h; i++)
        if (rowf[i] && colf[i] && (px[(size_t)i * 4 + 3] != 0))
            m[i] = 1;
    memset(rowf, 0, (size_t)w * h); /* (now the outline) */
    for (int y = b[1]; y <= b[3] && y < h; y++)
        for (int x = b[0]; x <= b[2] && x < w; x++)
        {
            const unsigned char *p = px + ((size_t)y * w + x) * 4;
            if (m[y * w + x] || (p[3] == 0) || (sp_lum(p) >= 45))
                continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    const int nx = x + dx, ny = y + dy;
                    if ((nx >= 0) && (ny >= 0) && (nx < w) && (ny < h) && m[ny * w + nx])
                        rowf[y * w + x] = 1;
                }
        }
    for (int i = 0; i < w * h; i++)
        m[i] |= rowf[i];
    sp_cut_store((v == 0) ? k : 6 + k, px, m, w, h);
    free(px); free(m); free(rowf); free(colf);
}

static void sp_cut_sell(void)
{
    int fw = 0, fh = 0, w = 0, h = 0;
    unsigned char *f = sp_sprite(GPS_rpanel_frame_portrt_empty, &fw, &fh);
    unsigned char *px = sp_sprite(GPS_rpanel_frame_portrt_sell, &w, &h);
    unsigned char *m = ((px != NULL) && (f != NULL) && (fw == w) && (fh == h)) ? (unsigned char *)calloc((size_t)w * h, 1) : NULL;
    if (m != NULL)
    {
        /* what differs from the empty frame, inside its border, red (the frame's edges are olive) */
        for (int y = 1; y < h - 1; y++) /* (the whole sign: the frame is left out by comparing with the empty one) */
            for (int x = 1; x < w - 1; x++)
            {
                const unsigned char *p = px + ((size_t)y * w + x) * 4, *q = f + ((size_t)y * w + x) * 4;
                const int d = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
                m[y * w + x] = (p[3] != 0) && (d > 20) && (p[0] > p[1] + 25);
            }
        sp_keep_groups(m, w, h, 4);
        sp_cut_store(5, px, m, w, h);
    }
    free(f);
    free(px);
    free(m);
}

/** Tab icon k (0-4: bright; 6-10: dark) or the sell sign (5). */
static const unsigned char *sp_cut(int k, int *w, int *h)
{
    if (!sp_cut_tried[k])
    {
        sp_cut_tried[k] = true;
        int ow = 0, oh = 0;
        unsigned char *ov = world_ui_icon_override(10, k, &ow, &oh);
        if (ov != NULL)
        {
            sp_cut_px[k] = ov;
            sp_cut_w[k] = ow;
            sp_cut_h[k] = oh;
        }
        else if (k < 5)
            sp_cut_tab(k, 0);
        else if (k >= 6)
            sp_cut_tab(k - 6, 1);
        else
            sp_cut_sell();
    }
    *w = sp_cut_w[k];
    *h = sp_cut_h[k];
    return sp_cut_px[k];
}

static void sp_picture(struct UiCanvas *c, const unsigned char *px, int iw, int ih, float x0, float y0, float x1, float y1,
    float fill, float gain);

/** The colours of the stone frames round the panel's small buttons (their outer two pixels), gathered once. */
static TbBool sp_frame_colour(const unsigned char *p)
{
    static uint32_t cols[768];
    static int n = -1;
    if (n < 0)
    {
        n = 0;
        static const short from[][2] = {{278, 281}, {282, 287}, {342, 354}, {473, 476}, {492, 495}};
        for (int g = 0; g < 5; g++)
            for (long sidx = from[g][0]; sidx <= from[g][1]; sidx++)
            {
                int w = 0, h = 0;
                unsigned char *px = sp_sprite(sidx, &w, &h);
                if (px == NULL)
                    continue;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        if ((x >= 2) && (y >= 2) && (x < w - 2) && (y < h - 2))
                            continue;
                        const unsigned char *q = px + ((size_t)y * w + x) * 4;
                        if (q[3] == 0)
                            continue;
                        const uint32_t c = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
                        int j = 0;
                        while ((j < n) && (cols[j] != c))
                            j++;
                        if ((j == n) && (n < 768))
                            cols[n++] = c;
                    }
                free(px);
            }
    }
    const uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    for (int j = 0; j < n; j++)
        if (cols[j] == c)
            return true;
    return false;
}

/** A small button's own icon (a panel sprite): its stone frame - frame colours reached from the sprite's edge - and its
 *  near-black shadow taken off, cropped. Kept for the game's run. */
static const unsigned char *sp_framed_pic(long spr, int *w, int *h)
{
    #define SP_FP 2048
    static unsigned char *px_of[SP_FP];
    static short w_of[SP_FP], h_of[SP_FP];
    static TbBool tried[SP_FP];
    if ((spr <= 0) || (spr >= SP_FP))
        return um_icon(spr, w, h);
    if (!tried[spr])
    {
        tried[spr] = true;
        int ow = 0, oh = 0;
        unsigned char *ov = world_ui_icon_override(1, spr, &ow, &oh);
        if (ov != NULL)
        {
            px_of[spr] = ov; w_of[spr] = (short)ow; h_of[spr] = (short)oh;
            *w = ow; *h = oh;
            return ov;
        }
        int pw = 0, ph = 0;
        unsigned char *px = sp_sprite(spr, &pw, &ph);
        unsigned char *gone = (px != NULL) ? (unsigned char *)calloc((size_t)pw * ph, 1) : NULL;
        int *stack = (gone != NULL) ? (int *)malloc(sizeof(int) * (size_t)pw * ph) : NULL;
        if (stack != NULL)
        {
            int sp = 0;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    if ((x > 0) && (y > 0) && (x < pw - 1) && (y < ph - 1))
                        continue;
                    const int i = y * pw + x;
                    const unsigned char *q = px + (size_t)i * 4;
                    if ((q[3] != 0) && sp_frame_colour(q) && !gone[i])
                    {
                        gone[i] = 1;
                        stack[sp++] = i;
                    }
                }
            while (sp > 0)
            {
                const int i = stack[--sp];
                const int x = i % pw, y = i / pw;
                static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; k++)
                {
                    const int nx = x + d[k][0], ny = y + d[k][1];
                    if ((nx < 0) || (ny < 0) || (nx >= pw) || (ny >= ph))
                        continue;
                    const int ni = ny * pw + nx;
                    const unsigned char *q = px + (size_t)ni * 4;
                    /* (a frame is a few pixels deep: the flood stops there, so an icon of the frame's colours stays; the
                       assistant's wedge is all frame) */
                    const int depth = min(min(nx, ny), min(pw - 1 - nx, ph - 1 - ny));
                    if ((depth > 4) && !((spr >= GPS_rpanel_rpanel_btn_cassisti_act) && (spr <= GPS_rpanel_rpanel_btn_cassistw_std)))
                        continue;
                    if (!gone[ni] && (q[3] != 0) && sp_frame_colour(q))
                    {
                        gone[ni] = 1;
                        stack[sp++] = ni;
                    }
                }
            }
            unsigned char *m = gone; /* (now what stays) */
            for (int i = 0; i < pw * ph; i++)
            {
                const unsigned char *q = px + (size_t)i * 4;
                m[i] = !m[i] && (q[3] != 0) && (q[0] * 3 + q[1] * 6 + q[2] >= 120) && !sp_shadow(q);
            }
            sp_keep_groups(m, pw, ph, 3);
            int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    if (!m[y * pw + x])
                    {
                        px[(y * pw + x) * 4 + 3] = 0;
                        continue;
                    }
                    x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y);
                }
            if (x1 >= x0)
            {
                const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
                unsigned char *o = (unsigned char *)malloc((size_t)cw * ch * 4);
                if (o != NULL)
                {
                    for (int y = 0; y < ch; y++)
                        memcpy(o + (size_t)y * cw * 4, px + ((size_t)(y + y0) * pw + x0) * 4, (size_t)cw * 4);
                    px_of[spr] = o;
                    w_of[spr] = (short)cw;
                    h_of[spr] = (short)ch;
                }
            }
        }
        free(stack);
        free(gone);
        free(px);
    }
    if (px_of[spr] == NULL)
        return um_icon(spr, w, h);
    *w = w_of[spr];
    *h = h_of[spr];
    return px_of[spr];
    #undef SP_FP
}

/** The sprite a normal button shows (gui_area_new_normal_button): its lit picture while pressed, its plain one else. */
static long sp_normal_spr(const struct GuiButton *b)
{
    if (b == NULL)
        return 0;
    const TbBool down = (b->button_state_left_pressed != 0) || (b->button_state_right_pressed != 0);
    return b->sprite_idx + (((b->flags & LbBtnF_Enabled) != 0) && down ? 0 : 1);
}

/** A small button as a tile with the game's icon on it (dimmed when it cannot be used). */
static void sp_icon_tile(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, long spr, TbBool hot,
    TbBool down, TbBool enabled, TbBool lit, float fill, float gain)
{
    const float u = st->u, r = min(10.0f * u, (y1 - y0) * 0.45f);
    if (st->flat)
    {
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, lit ? UIC(150, 36, 16, 255) : UIC(30, 21, 17, 255), lit ? UIC(150, 36, 16, 255) : UIC(30, 21, 17, 255), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, 1.0f, (hot || lit) ? UM_GOLD_HI : UIC(104, 78, 36, 255));
    } else
    {
        if (lit)
        {
            ui_glow_rrect(c, x0, y0, x1, y1, r, 10.0f * u, UIC(255, 80, 24, 70));
            ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(160, 42, 18, 255), UIC(110, 24, 12, 255), false);
        } else
            ui_fill_rrect(c, x0, y0, x1, y1, r, down ? UIC(0, 0, 0, 110) : UIC(255, 226, 196, hot ? 26 : 14),
                down ? UIC(0, 0, 0, 80) : UIC(255, 226, 196, hot ? 10 : 4), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, r, 1.0f, (hot || lit) ? UIC(255, 214, 150, 150) : UIC(255, 214, 170, 40));
    }
    int iw = 0, ih = 0;
    const unsigned char *pic = sp_framed_pic(spr, &iw, &ih);
    sp_picture(c, pic, iw, ih, x0, y0, x1, y1, fill, !enabled ? 0.45f : gain);
}

static void sp_picture(struct UiCanvas *c, const unsigned char *px, int iw, int ih, float x0, float y0, float x1, float y1,
    float fill, float gain)
{
    if ((px == NULL) || (iw <= 0) || (ih <= 0))
        return;
    const float rw = (x1 - x0) * fill, rh = (y1 - y0) * fill;
    const float fit_f = min(rw / (float)iw, rh / (float)ih);
    const int k = (int)floorf(fit_f);
    const float fit = ((k >= 1) && ((float)k >= fit_f * 0.75f)) ? (float)k : fit_f; /* (a whole number of its pixels when near) */
    const int dw = max(1, (int)((float)iw * fit)), dh = max(1, (int)((float)ih * fit));
    ui_image_gain(c, (int)floorf((x0 + x1 - (float)dw) * 0.5f + 0.5f), (int)floorf((y0 + y1 - (float)dh) * 0.5f + 0.5f), dw, dh,
        px, iw, ih, gain);
}

/******************************************************************************/
/* The painting */

static struct UmHost sp_glass_host; /* the glass: under everything */
static struct UmHost sp_host;      /* the panel: under the game's UI */
static struct UmHost sp_gold_host; /* the gold: slot 5, under it */
static struct UmHost sp_over_host; /* the compass letters: slot 3, over it */
static int sp_cx0, sp_cy0;         /* the panel canvas's top left on the screen */

#define X(v) ((float)(v) * u + (sp_ox - (float)sp_cx0))
#define Y(v) ((float)(v) * u + (sp_oy - (float)sp_cy0))
#define S(v) ((float)(v) * u)

/** The vector icons, in a 24 x 24 box: points and outlines */
static const float sp_icon_map[] = {2.0f, 5.2f, 8.2f, 3.0f, 15.8f, 5.6f, 22.0f, 3.4f, 22.0f, 18.8f, 15.8f, 21.0f, 8.2f, 18.4f, 2.0f, 20.6f,
    9.6f, 5.6f, 14.4f, 7.2f, 14.4f, 18.4f, 9.6f, 16.8f};
static const int sp_icon_map_n[] = {8, 4};
static const float sp_icon_minus[] = {3.0f, 9.6f, 21.0f, 9.6f, 21.0f, 14.4f, 3.0f, 14.4f};
static const int sp_icon_minus_n[] = {4};
static const float sp_icon_plus[] = {9.6f, 3.0f, 14.4f, 3.0f, 14.4f, 9.6f, 21.0f, 9.6f, 21.0f, 14.4f, 14.4f, 14.4f, 14.4f, 21.0f,
    9.6f, 21.0f, 9.6f, 14.4f, 3.0f, 14.4f, 3.0f, 9.6f, 9.6f, 9.6f};
static const int sp_icon_plus_n[] = {12};

/** A shape of the 24-unit box drawn `size` pixels wide with its middle at (cx, cy). */
static void sp_shape(struct UiCanvas *c, const float *pts, const int *counts, int n, float cx, float cy, float size, UiColour col)
{
    float xy[256];
    int total = 0;
    for (int i = 0; i < n; i++)
        total += counts[i];
    if (total > 128)
        return;
    const float k = size / 24.0f;
    for (int i = 0; i < total; i++)
    {
        xy[i * 2] = cx + (pts[i * 2] - 12.0f) * k;
        xy[i * 2 + 1] = cy + (pts[i * 2 + 1] - 12.0f) * k;
    }
    ui_fill_path(c, xy, counts, n, col);
}

/** The cogwheel: eight teeth round a hole. */
static void sp_cog(struct UiCanvas *c, float cx, float cy, float size, UiColour col)
{
    float xy[2 * (8 * 6 + 24)];
    int counts[2];
    const float k = size / 24.0f, ro = 10.6f * k, rr = 7.9f * k, rh = 3.7f * k;
    int n = 0;
    for (int t = 0; t < 8; t++)
    {
        const float a = (float)t * (float)M_PI / 4.0f;
        const float pa[4] = {a - 0.36f, a - 0.21f, a + 0.21f, a + 0.36f};
        const float pr[4] = {rr, ro, ro, rr};
        for (int j = 0; j < 4; j++)
        {
            xy[n * 2] = cx + cosf(pa[j]) * pr[j];
            xy[n * 2 + 1] = cy + sinf(pa[j]) * pr[j];
            n++;
        }
        /* the root between this tooth and the next */
        const float am = a + (float)M_PI / 8.0f;
        xy[n * 2] = cx + cosf(am) * rr;
        xy[n * 2 + 1] = cy + sinf(am) * rr;
        n++;
    }
    counts[0] = n;
    for (int j = 0; j < 24; j++)
    {
        const float a = (float)j * 2.0f * (float)M_PI / 24.0f;
        xy[n * 2] = cx + cosf(a) * rh;
        xy[n * 2 + 1] = cy + sinf(a) * rh;
        n++;
    }
    counts[1] = 24;
    ui_fill_path(c, xy, counts, 2, col);
}

/** A round button: dark glass, a faint edge, a soft shadow; lit when the pointer is on it. */
static void sp_round(struct UiCanvas *c, const struct UmStyle *st, float cx, float cy, float r, TbBool hot, TbBool down)
{
    const float u = st->u;
    if (st->flat)
    {
        ui_fill_rrect(c, cx - r, cy - r, cx + r, cy + r, r, UIC(28, 18, 14, 255), UIC(28, 18, 14, 255), false);
        ui_stroke_rrect(c, cx - r, cy - r, cx + r, cy + r, r, 1.0f, hot ? UM_GOLD_HI : UIC(104, 78, 36, 255));
        return;
    }
    ui_shadow_rrect(c, cx - r, cy - r, cx + r, cy + r, r, S(9.0f), S(2.5f), UIC(0, 0, 0, 150));
    if (hot && !down)
        ui_glow_rrect(c, cx - r, cy - r, cx + r, cy + r, r, S(8.0f), UIC(255, 120, 40, 60));
    ui_fill_rrect(c, cx - r, cy - r, cx + r, cy + r, r, down ? UIC(24, 15, 11, 235) : UIC(46, 28, 19, 225),
        down ? UIC(8, 6, 5, 240) : UIC(13, 9, 7, 238), false);
    ui_stroke_rrect(c, cx - r, cy - r, cx + r, cy + r, r, max(1.0f, S(0.6f)), hot ? UIC(255, 214, 150, 150) : UIC(255, 214, 170, 72));
}

static void sp_tab(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, TbBool on, TbBool hot,
    int icon, TbBool hot_down)
{
    const float u = st->u, r = S(10.0f);
    if (st->flat)
    {
        const UiColour f = on ? UIC(150, 36, 16, 255) : UIC(30, 21, 17, 255);
        ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, f, f, false);
        ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, 1.0f, (on || hot) ? UM_GOLD_HI : UIC(80, 60, 40, 255));
    } else if (on)
    {
        ui_glow_rrect(c, x0, y0, x1, y1, r, S(14.0f), UIC(255, 80, 24, 80));
        ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(160, 42, 18, 255), UIC(110, 24, 12, 255), false);
        ui_fill_rrect(c, x0 + S(1.0f), y0, x1 - S(1.0f), y0 + S(10.0f), r, UIC(255, 190, 104, 90), UIC(255, 190, 104, 0), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, S(0.6f)), UIC(255, 190, 104, hot ? 230 : 170));
    } else
    {
        ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(255, 226, 196, hot ? 28 : 16), UIC(255, 226, 196, hot ? 12 : 5), false);
        ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, S(0.6f)), hot ? UIC(255, 214, 150, 120) : UIC(255, 214, 170, 36));
    }
    int iw = 0, ih = 0;
    const unsigned char *px = sp_cut((on || hot_down) ? icon : 6 + icon, &iw, &ih);
    if (px != NULL)
    {
        /* (the icons at the same scale, each in the middle of its tile: a whole number of their pixels) */
        const float fit = S(26.0f) / 33.0f;
        const float whole = floorf(fit + 0.25f);
        float k = ((whole >= 1.0f) && (whole >= fit * 0.75f)) ? whole : fit; /* (else as it fits: kept the tile's size) */
        /* never past the tile: an edited icon of any size, at any screen size and UI_SCALE, keeps a margin inside it
           (the user, 2026-11-26: "Make sure they fit in their slot") */
        const float room_w = (x1 - x0) * 0.84f, room_h = (y1 - y0) * 0.84f;
        if ((float)iw * k > room_w)
            k = room_w / (float)iw;
        if ((float)ih * k > room_h)
            k = room_h / (float)ih;
        const int dw = (int)((float)iw * k), dh = (int)((float)ih * k);
        ui_image_gain(c, (int)floorf((x0 + x1 - (float)dw) * 0.5f + 0.5f), (int)floorf((y0 + y1 - (float)dh) * 0.5f + 0.5f), dw, dh,
            px, iw, ih, hot ? 1.15f : 1.0f);
    }
}

/** A cell's contents over its well (painted in the base). */
static void sp_paint_cell(struct UiCanvas *c, const struct UmStyle *st, int k, const struct SpCell *cell, TbBool blink)
{
    const float u = st->u;
    float r[4];
    sp_role_rect(SpR_Cell0 + k, r);
    const float x0 = X(r[0]), y0 = Y(r[1]), x1 = X(r[0] + r[2]), y1 = Y(r[1] + r[3]);
    const float rad = S(10.0f);
    if (cell->show == 0)
        return;
    if (cell->active && !st->flat)
        ui_fill_rrect(c, x0, y0, x1, y1, rad, UIC(255, 120, 40, 0), UIC(255, 120, 40, 60), false);
    if (cell->chosen && !st->flat)
        ui_fill_rrect(c, x0, y0, x1, y1, rad, UIC(255, 120, 40, 0), UIC(255, 120, 40, blink ? 70 : 40), false);
    const float gain = (cell->down || cell->dark) ? 0.6f : ((cell->chosen && blink) ? 1.45f : (cell->hot ? 1.12f : 1.0f));
    if (cell->show == 1)
        um_icon_draw(c, cell->spr, x0, y0, x1, y1, 0.74f, gain);
    else if (cell->show == 2)
        ui_text_box(c, UiF_Head, S(26.0f), x0, y0, x1 - x0, y1 - y0, 1, UIC(200, 170, 110, 150), 0.0f, false, "?");
    else if (cell->show == 3)
    {
        int iw = 0, ih = 0;
        const unsigned char *px = sp_cut(5, &iw, &ih);
        sp_picture(c, px, iw, ih, x0, y0, x1, y1, 0.74f, gain);
    }
    um_frame(c, st, x0, y0, x1, y1, rad, cell->chosen || cell->hot);
    if (cell->level > 0)
    {
        char t[8];
        snprintf(t, sizeof(t), "%d", cell->level);
        const float px = S(10.5f);
        const float tw = ui_text_width(UiF_Num, px, 0.0f, t);
        /* (its middle on the well's top left corner: the user, 2026-11-26) */
        const float bw = tw + S(6.0f), bh = S(13.0f);
        const float bx0 = floorf(x0 - bw * 0.5f + 0.5f), by0 = floorf(y0 - bh * 0.5f + 0.5f), bx1 = bx0 + bw, by1 = by0 + bh;
        if (st->flat)
            ui_fill_rrect(c, bx0, by0, bx1, by1, 0.0f, UIC(12, 8, 6, 255), UIC(12, 8, 6, 255), false);
        else
        {
            ui_fill_rrect(c, bx0, by0, bx1, by1, S(4.0f), UIC(12, 8, 6, 214), UIC(12, 8, 6, 214), false);
            ui_stroke_rrect(c, bx0, by0, bx1, by1, S(4.0f), 1.0f, UIC(255, 214, 170, 46));
        }
        ui_text_box(c, UiF_Num, px, bx0, by0, bx1 - bx0, by1 - by0, 1, cell->reached ? UM_BRIGHT : UIC(150, 140, 128, 255), 0.0f,
            false, t);
    }
    if (cell->bar >= 0.0f)
        um_bar(c, st, x0 + S(5.0f), y1 - S(6.0f), x1 - S(5.0f), y1 - S(3.0f), cell->bar);
}

static void sp_paint_strip(struct UiCanvas *c, const struct UmStyle *st, const struct SpStrip *s)
{
    const float u = st->u;
    float r[4];
    sp_role_rect(SpR_Strip, r);
    const float x0 = X(r[0]), y0 = Y(r[1]), y1 = Y(r[1] + r[3]), x1 = X(r[0] + r[2]);
    um_icon_draw(c, s->spr, x0 + S(3.0f), y0 + S(3.0f), x0 + S(39.0f), y1 - S(3.0f), 0.94f, s->dark ? 0.6f : 1.0f);
    const float tx = x0 + S(44.0f), tr = x1 - S(8.0f);
    const float vw = ui_text_width(UiF_Num, S(14.0f), 0.0f, s->value);
    const float npx = ui_text_fit(UiF_Body, S(12.5f), 0.0f, tr - tx - vw - S(8.0f), s->name);
    ui_text_box(c, UiF_Body, npx, tx, y0 + S(5.0f), tr - tx, S(16.0f), 0, UM_BRIGHT, 0.0f, !st->flat, s->name);
    ui_text_box(c, UiF_Num, S(14.0f), tx, y0 + S(5.0f), tr - tx, S(16.0f), 2, s->dim ? UIC(200, 90, 60, 255) : UM_GOLD, 0.0f,
        !st->flat, s->value);
    if (s->bar >= 0.0f)
        um_bar(c, st, tx, y0 + S(27.0f), tr, y0 + S(31.0f), s->bar);
}

static TbBool sp_hot(int role)
{
    return (sp_btn[role] != NULL) && ((sp_btn[role]->flags & LbBtnF_MouseOver) != 0);
}

static TbBool sp_down(int role)
{
    return (sp_btn[role] != NULL) && ((sp_btn[role]->button_state_left_pressed != 0) || (sp_btn[role]->button_state_right_pressed != 0));
}

static TbBool sp_shown(int role)
{
    return (sp_btn[role] != NULL) && ((sp_btn[role]->flags & LbBtnF_Visible) != 0) &&
        ((role != SpR_Assist) || ((sp_btn[role]->flags & LbBtnF_Enabled) != 0));
}

static int sp_active_tab(void)
{
    if ((sp_tab_menu == GMnu_ROOM) || (sp_tab_menu == GMnu_ROOM2))
        return 1;
    if ((sp_tab_menu == GMnu_SPELL) || (sp_tab_menu == GMnu_SPELL2) || (sp_tab_menu == GMnu_SPELL_LOST))
        return 2;
    if ((sp_tab_menu == GMnu_TRAP) || (sp_tab_menu == GMnu_TRAP2))
        return 3;
    if (menu_is_active(GMnu_QUERY))
        return 0;
    if (menu_is_active(GMnu_CREATURE))
        return 4;
    return -1;
}

static void sp_paint_round(struct UiCanvas *c, const struct UmStyle *st, int role)
{
    const float u = st->u;
    float r[4];
    sp_role_rect(role, r);
    const float cx = X(r[0] + r[2] * 0.5f), cy = Y(r[1] + r[3] * 0.5f), rad = S(r[2] * 0.5f);
    const TbBool hot = sp_hot(role), down = sp_down(role);
    sp_round(c, st, cx, cy, rad, hot, down);
    const UiColour gold = hot ? UM_GOLD_HI : UM_GOLD;
    const float sz = rad * 2.0f * 0.62f;
    switch (role)
    {
    case SpR_Map: sp_shape(c, sp_icon_map, sp_icon_map_n, 2, cx, cy, sz, gold); break;
    case SpR_Options: sp_cog(c, cx, cy, sz, gold); break;
    case SpR_ZoomOut: sp_shape(c, sp_icon_minus, sp_icon_minus_n, 1, cx, cy, sz * 0.92f, gold); break;
    case SpR_ZoomIn: sp_shape(c, sp_icon_plus, sp_icon_plus_n, 1, cx, cy, sz * 0.92f, gold); break;
    case SpR_Assist:
    {
        const struct Dungeon *dungeon = get_my_dungeon();
        long spr = sp_btn[role]->sprite_idx;
        if (((dungeon->computer_enabled & 0x01) != 0) && sp_beat())
            spr += 2;
        if (!down)
            spr += 1;
        int iw = 0, ih = 0;
        const unsigned char *pic = sp_framed_pic(spr, &iw, &ih);
        sp_picture(c, pic, iw, ih, cx - rad, cy - rad, cx + rad, cy + rad, 0.7f, hot ? 1.12f : 1.0f);
        break;
    }
    default:
        break;
    }
}

static void sp_paint_query(struct UiCanvas *c, const struct UmStyle *st, const struct SpQuery *q);
static void sp_paint_creatures(struct UiCanvas *c, const struct UmStyle *st, const struct SpCrRow *rows);
static void sp_paint_cq(struct UiCanvas *c, const struct UmStyle *st);

static void sp_paint(TbBool base, const struct SpCell *cells, const struct SpStrip *strip, TbBool has_strip, int tab)
{
    struct UiCanvas *c = base ? &sp_host.base : &sp_host.canvas;
    const struct UmStyle stv = {sp_u, um_host_flat()};
    const struct UmStyle *st = &stv;
    const float u = sp_u;
    if (base)
    {
        /* the hollow the minimap lies in (no ring: the user, "make the UI panel emboss to the inside") */
        if (st->flat)
            ui_fill_rrect(c, sp_map_cx - (float)sp_cx0 - sp_map_r - S(4.0f), sp_map_cy - (float)sp_cy0 - sp_map_r - S(4.0f),
                sp_map_cx - (float)sp_cx0 + sp_map_r + S(4.0f), sp_map_cy - (float)sp_cy0 + sp_map_r + S(4.0f), sp_map_r + S(4.0f),
                UIC(10, 7, 6, 255), UIC(10, 7, 6, 255), false);
        else
            ui_hollow_circle(c, sp_map_cx - (float)sp_cx0, sp_map_cy - (float)sp_cy0, sp_map_r + S(5.0f), S(16.0f));
        if (tab >= 1 && tab <= 3)
        {
            for (int k = 0; k < 16; k++)
            {
                float r[4];
                sp_role_rect(SpR_Cell0 + k, r);
                const float x0 = X(r[0]), y0 = Y(r[1]), x1 = X(r[0] + r[2]), y1 = Y(r[1] + r[3]);
                um_well(c, st, x0, y0, x1, y1, S(10.0f));
                if (!st->flat)
                    ui_stroke_rrect(c, x0, y0, x1, y1, S(10.0f), 1.0f, UIC(255, 214, 170, 26));
            }
            float r[4];
            sp_role_rect(SpR_Strip, r);
            um_card(c, st, X(r[0]), Y(r[1]), X(r[0] + r[2]), Y(r[1] + r[3]));
        }
        return;
    }
    const int rounds[] = {SpR_Map, SpR_Options, SpR_ZoomOut, SpR_ZoomIn, SpR_Assist};
    for (size_t i = 0; i < sizeof(rounds) / sizeof(rounds[0]); i++)
        if (sp_shown(rounds[i]))
            sp_paint_round(c, st, rounds[i]);
    for (int t = 0; t < 5; t++)
    {
        if (!sp_shown(SpR_Tab0 + t))
            continue;
        float r[4];
        sp_role_rect(SpR_Tab0 + t, r);
        sp_tab(c, st, X(r[0]), Y(r[1]), X(r[0] + r[2]), Y(r[1] + r[3]), (t == tab), sp_hot(SpR_Tab0 + t), t, sp_down(SpR_Tab0 + t));
    }
    if ((sp_info_menu == GMnu_QUERY) && (sp_q_now != NULL))
        sp_paint_query(c, st, sp_q_now);
    if ((sp_info_menu == GMnu_CREATURE) && (sp_rows_now != NULL))
        sp_paint_creatures(c, st, sp_rows_now);
    if (sp_cq_ident(sp_info_menu))
        sp_paint_cq(c, st);
    if (tab >= 1 && tab <= 3)
    {
        const TbBool blink = sp_blink();
        for (int k = 0; k < 16; k++)
            sp_paint_cell(c, st, k, &cells[k], blink);
        if (has_strip)
            sp_paint_strip(c, st, strip);
        if (sp_shown(SpR_Next))
        {
            float r[4];
            sp_role_rect(SpR_Next, r);
            const TbBool second = (sp_tab_menu == GMnu_ROOM2) || (sp_tab_menu == GMnu_SPELL2) || (sp_tab_menu == GMnu_TRAP2);
            (void)second;
            sp_icon_tile(c, st, X(r[0]), Y(r[1]), X(r[0] + r[2]), Y(r[1] + r[3]), sp_normal_spr(sp_btn[SpR_Next]), sp_hot(SpR_Next),
                sp_down(SpR_Next), (sp_btn[SpR_Next]->flags & LbBtnF_Enabled) != 0, false, 0.8f, 1.0f);
        }
    }
}

static void sp_draw_gold(void)
{
    const struct Dungeon *dungeon = get_my_dungeon();
    long long v = dungeon->total_money_owned;
    char digits[32], t[48];
    snprintf(digits, sizeof(digits), "%lld", (v < 0) ? 0LL : v);
    /* with thousands marks: 29,793 */
    const int nd = (int)strlen(digits);
    int o = 0;
    for (int i = 0; i < nd; i++)
    {
        t[o++] = digits[i];
        if ((nd - 1 - i > 0) && ((nd - 1 - i) % 3 == 0))
            t[o++] = ',';
    }
    t[o] = '\0';
    const float u = sp_u;
    float r[4];
    sp_role_rect(SpR_Gold, r);
    const float extra = 24.0f; /* (the number may be wider than the button: room to its sides) */
    const int w = sp_px(S(r[2] + 2.0f * extra)), h = sp_px(S(r[3]));
    const int x = sp_px(sp_ox + S(r[0] - extra)), y = sp_px(sp_oy + S(r[1]));
    char key[96];
    snprintf(key, sizeof(key), "%s %.4f %d", t, (double)u, (int)um_host_flat());
    sp_gold_host.name = "side_gold";
    sp_gold_host.slot = UmSl_Gold;
    if (um_host_begin(&sp_gold_host, w, h, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        const float px = ui_text_fit(UiF_Num, S(33.0f), S(0.4f), (float)w - S(8.0f), t);
        ui_text_box(&sp_gold_host.canvas, UiF_Num, px, 0.0f, 0.0f, (float)w, (float)h, 1, UIC(255, 224, 140, 255), S(0.4f),
            !um_host_flat(), t);
        um_host_painted(&sp_gold_host, t0);
    }
    um_host_show(&sp_gold_host, x, y);
}

/******************************************************************************/
/* The Information and Creatures tabs */

PlayerNumber world_info_panel_player(int idx); /* (frontmenu_ingame_tabs.c) */
void gui_set_query(struct GuiButton *gbtn);

static const float sp_icon_up[] = {12.0f, 6.0f, 21.0f, 15.0f, 17.6f, 18.4f, 12.0f, 12.8f, 6.4f, 18.4f, 3.0f, 15.0f};
static const float sp_icon_down[] = {12.0f, 18.0f, 21.0f, 9.0f, 17.6f, 5.6f, 12.0f, 11.2f, 6.4f, 5.6f, 3.0f, 9.0f};
static const int sp_icon_chev_n[] = {6};

/** A picture inside a frame `inset` pixels deep (the activity signs): its shadow off, cropped. Cached by sprite. */
static const unsigned char *sp_inner_pic(long spr, int inset, int *w, int *h)
{
    static unsigned char *px_of[1024];
    static short w_of[1024], h_of[1024];
    static TbBool tried[1024];
    if ((spr <= 0) || (spr >= 1024))
        return NULL;
    if (!tried[spr])
    {
        tried[spr] = true;
        int ow = 0, oh = 0;
        unsigned char *ov = world_ui_icon_override(2, spr, &ow, &oh);
        if (ov != NULL)
        {
            px_of[spr] = ov; w_of[spr] = (short)ow; h_of[spr] = (short)oh;
            *w = ow; *h = oh;
            return ov;
        }
        /* (cut as the small buttons are: the frame flooded off from the edge; a fixed inset cut the pictures' tops and
           feet off, the user 2026-11-26) */
        (void)inset;
        return sp_framed_pic(spr, w, h);
        int pw = 0, ph = 0;
        unsigned char *px = sp_sprite(spr, &pw, &ph);
        unsigned char *m = (px != NULL) ? (unsigned char *)calloc((size_t)pw * ph, 1) : NULL;
        if (m != NULL)
        {
            for (int y = inset; y < ph - inset; y++)
                for (int x = inset; x < pw - inset; x++)
                {
                    const unsigned char *q = px + ((size_t)y * pw + x) * 4;
                    m[y * pw + x] = (q[3] != 0) && (q[0] * 3 + q[1] * 6 + q[2] >= 200);
                }
            sp_keep_groups(m, pw, ph, 3);
            int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    if (!m[y * pw + x])
                    {
                        px[(y * pw + x) * 4 + 3] = 0;
                        continue;
                    }
                    x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y);
                }
            if (x1 >= x0)
            {
                const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
                unsigned char *o = (unsigned char *)malloc((size_t)cw * ch * 4);
                if (o != NULL)
                {
                    for (int y = 0; y < ch; y++)
                        memcpy(o + (size_t)y * cw * 4, px + ((size_t)(y + y0) * pw + x0) * 4, (size_t)cw * 4);
                    px_of[spr] = o;
                    w_of[spr] = (short)cw;
                    h_of[spr] = (short)ch;
                }
            }
        }
        free(m);
        free(px);
    }
    *w = w_of[spr];
    *h = h_of[spr];
    return px_of[spr];
}

static void sp_rect_px(int role, float *x0, float *y0, float *x1, float *y1)
{
    const float u = sp_u;
    float r[4];
    sp_role_rect(role, r);
    *x0 = X(r[0]); *y0 = Y(r[1]); *x1 = X(r[0] + r[2]); *y1 = Y(r[1] + r[3]);
}

/** A card with a label and a bar (payday, research, workshop): `spr` an icon at its left (0: none, the label instead). */
static void sp_bar_card(struct UiCanvas *c, const struct UmStyle *st, int role, long spr, const char *label, const char *value,
    float frac, TbBool hot)
{
    const float u = st->u;
    float x0, y0, x1, y1;
    sp_rect_px(role, &x0, &y0, &x1, &y1);
    um_card(c, st, x0, y0, x1, y1);
    if (hot)
        ui_stroke_rrect(c, x0, y0, x1, y1, S(12.0f), 1.0f, UIC(255, 214, 150, 110));
    float tx = x0 + S(8.0f);
    if (spr > 0)
    {
        um_icon_draw(c, spr, x0 + S(3.0f), y0 + S(3.0f), x0 + S(31.0f), y1 - S(3.0f), 0.9f, 1.0f);
        tx = x0 + S(34.0f);
    }
    const float tr = x1 - S(8.0f);
    if (label != NULL)
        ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, S(11.5f), 0.0f, tr - tx - S(40.0f), label), tx, y0 + S(3.0f), tr - tx, S(16.0f), 0,
            UM_TEXT, 0.0f, !st->flat, label);
    if (value != NULL)
        ui_text_box(c, UiF_Num, S(13.0f), tx, y0 + S(3.0f), tr - tx, S(16.0f), 2, UM_GOLD, 0.0f, !st->flat, value);
    um_bar(c, st, tx, y1 - S(11.0f), tr, y1 - S(7.0f), frac);
}

struct SpQuery {
    PlayerNumber plyr[4];
    int rooms[4], crtrs[4], crtrs_max[4];
    TbBool alive[4], ally_on[4], ally_ok[4], show_max, face_dark[4];
    long ally_spr[4], tend_spr[2];
    long pay;
    float pay_frac, rsch_frac, mnfct_frac;
    TbBool imprison, flee, prison_ok;
};

static void sp_query_of(struct SpQuery *q)
{
    memset(q, 0, sizeof(*q));
    struct Dungeon *dungeon = get_my_dungeon();
    q->pay = (long)dungeon->creatures_total_pay;
    const long gap = game.conf.rules[my_player_number].gameplay.pay_day_gap;
    q->pay_frac = (gap > 0) ? (float)min((long)game.pay_day_progress[my_player_number], gap) / (float)gap : 0.0f;
    int req = 0, prog = 0;
    const struct ResearchVal *rv = get_players_current_research_val(my_player_number);
    if (!world_room_level_research_bar(my_player_number, &req, &prog) && (rv != NULL))
    {
        req = rv->req_amount;
        prog = (int)(dungeon->research_progress >> 8);
    }
    q->rsch_frac = (req > 0) ? (float)min(prog, req) / (float)req : 0.0f;
    if (dungeon->manufacture_class != TCls_Empty)
    {
        const long mreq = manufacture_points_required(dungeon->manufacture_class, dungeon->manufacture_kind);
        q->mnfct_frac = (mreq > 0) ? (float)min((long)(dungeon->manufacture_progress >> 8), mreq) / (float)mreq : 0.0f;
    }
    q->imprison = (game.creatures_tend_imprison != 0);
    q->flee = (game.creatures_tend_flee != 0);
    q->prison_ok = (sp_btn[SpR_Q0 + 2] != NULL) && ((sp_btn[SpR_Q0 + 2]->flags & LbBtnF_Enabled) != 0);
    for (int k = 0; k < 4; k++)
    {
        const PlayerNumber p = world_info_panel_player(k);
        q->plyr[k] = p;
        if (p < 0)
            continue;
        struct PlayerInfo *player = get_player(p);
        struct Dungeon *d = get_players_dungeon(player);
        if (dungeon_invalid(d))
        {
            q->plyr[k] = -1;
            continue;
        }
        q->rooms[k] = (int)d->total_rooms;
        q->crtrs[k] = (int)d->num_active_creatrs;
        q->crtrs_max[k] = (int)d->max_creatures_attracted;
        q->show_max = (game.conf.rules[p].gameplay.display_portal_limit == true);
        q->alive[k] = world_mode_active() ? ((player->victory_state != VicS_LostLevel) && world_player_met(p)) :
            (player->victory_state != VicS_LostLevel);
        q->ally_ok[k] = (sp_btn[SpR_Q0 + 15 + k] != NULL) && ((sp_btn[SpR_Q0 + 15 + k]->flags & LbBtnF_Enabled) != 0);
        q->ally_on[k] = player_allied_with(get_my_player(), p);
        /* the ally sign as gui_area_ally shows it: the keeper's colour in one half of the blink if allied that way */
        q->ally_spr[k] = GPS_plyrsym_symbol_player_any_dis;
        if (sp_beat() ? player_allied_with(get_my_player(), p) : player_allied_with(player, my_player_number))
            q->ally_spr[k] = get_player_colored_icon_idx(GPS_plyrsym_symbol_player_red_std_b, p);
        q->face_dark[k] = !(((d->num_active_creatrs < d->max_creatures_attracted) && (!game.pool.is_empty)) || sp_beat());
    }
    for (int k = 0; k < 2; k++)
    {
        const struct GuiButton *b = sp_btn[SpR_Q0 + 2 + k];
        if (b == NULL)
            continue;
        long spr = b->sprite_idx;
        const TbBool down = (b->button_state_left_pressed != 0) || (b->button_state_right_pressed != 0);
        if ((b->flags & LbBtnF_Enabled) != 0)
        {
            const unsigned char *on = (const unsigned char *)b->content.ptr;
            if (!down && (on != NULL) && (*on > 0) && sp_beat())
                spr += 2;
            if (!down)
                spr += 1;
        }
        q->tend_spr[k] = spr;
    }
}

static uint32_t sp_query_hash(int n, const struct SpQuery *q)
{
    const TbBool hot = sp_hot(SpR_Q0 + n), down = sp_down(SpR_Q0 + n);
    if ((n >= 7) && (n < 19))
    {
        const int k = (n - 7) % 4;
        return um_hash("q%d %d %d %d %d %d %d %d %d %d %ld %d", n, q->plyr[k], q->rooms[k], q->crtrs[k], q->crtrs_max[k], (int)q->alive[k],
            (int)q->ally_on[k], (int)q->ally_ok[k], (int)hot, (int)q->show_max, q->ally_spr[k], (int)q->face_dark[k]);
    }
    return um_hash("q%d %d %d %d %d %d %ld %d %d %d %ld %ld %d", n, (int)hot, (int)down, (int)q->imprison, (int)q->flee, (int)q->prison_ok, q->pay,
        (int)(q->pay_frac * 400.0f), (int)(q->rsch_frac * 400.0f), (int)(q->mnfct_frac * 400.0f), q->tend_spr[0], q->tend_spr[1],
        (sp_btn[SpR_Q0 + n] != NULL) ? (int)sp_btn[SpR_Q0 + n]->flags : 0);
}

static void sp_paint_query(struct UiCanvas *c, const struct UmStyle *st, const struct SpQuery *q)
{
    const float u = st->u;
    float x0, y0, x1, y1;
    /* the tendencies: lit while on */
    /* the tendencies: the game's pictures, blinking while on (gui_area_flash_cycle_button) */
    for (int k = 2; k <= 3; k++)
    {
        const struct GuiButton *b = sp_btn[SpR_Q0 + k];
        if (b == NULL)
            continue;
        sp_rect_px(SpR_Q0 + k, &x0, &y0, &x1, &y1);
        sp_icon_tile(c, st, x0, y0, x1, y1, q->tend_spr[k - 2], sp_hot(SpR_Q0 + k), sp_down(SpR_Q0 + k), (b->flags & LbBtnF_Enabled) != 0,
            false, 0.86f, 1.0f);
    }
    char t[32];
    snprintf(t, sizeof(t), "%ld", q->pay);
    sp_bar_card(c, st, SpR_Q0 + 4, 0, "Payday", t, q->pay_frac, sp_hot(SpR_Q0 + 4));
    sp_bar_card(c, st, SpR_Q0 + 5, (sp_btn[SpR_Q0 + 5] != NULL) ? sp_btn[SpR_Q0 + 5]->sprite_idx : 0, NULL, NULL, q->rsch_frac,
        sp_hot(SpR_Q0 + 5));
    sp_bar_card(c, st, SpR_Q0 + 6, (sp_btn[SpR_Q0 + 6] != NULL) ? sp_btn[SpR_Q0 + 6]->sprite_idx : 0, NULL, NULL, q->mnfct_frac,
        sp_hot(SpR_Q0 + 6));
    /* the keepers: rooms, allied, creatures */
    for (int k = 0; k < 4; k++)
    {
        const PlayerNumber p = q->plyr[k];
        if (p < 0)
            continue;
        sp_rect_px(SpR_Q0 + 11 + k, &x0, &y0, &x1, &y1);
        um_card(c, st, x0, y0, x1, y1);
        um_icon_draw(c, get_player_colored_icon_idx(GPS_plyrsym_symbol_room_red_std_a, p), x0 + S(2.0f), y0 + S(2.0f), x0 + S(26.0f),
            y1 - S(2.0f), 0.9f, 1.0f);
        snprintf(t, sizeof(t), "%d", q->rooms[k]);
        ui_text_box(c, UiF_Num, S(13.0f), x0 + S(28.0f), y0, x1 - x0 - S(34.0f), y1 - y0, 2, UM_BRIGHT, 0.0f, !st->flat, t);
        sp_rect_px(SpR_Q0 + 7 + k, &x0, &y0, &x1, &y1);
        um_card(c, st, x0, y0, x1, y1);
        const int face = q->alive[k] ? GPS_plyrsym_symbol_player_red_std_a : GPS_plyrsym_symbol_player_red_dead;
        um_icon_draw(c, get_player_colored_icon_idx(face, p), x0 + S(2.0f), y0 + S(2.0f), x0 + S(26.0f), y1 - S(2.0f), 0.9f,
            q->face_dark[k] ? 0.55f : 1.0f); /* (darkened in the blink's other half while its portal is full, as the game) */
        if (q->show_max)
            snprintf(t, sizeof(t), "%d/%d", q->crtrs[k], q->crtrs_max[k]);
        else
            snprintf(t, sizeof(t), "%d", q->crtrs[k]);
        ui_text_box(c, UiF_Num, S(13.0f), x0 + S(28.0f), y0, x1 - x0 - S(34.0f), y1 - y0, 2, UM_BRIGHT, 0.0f, !st->flat, t);
        if (q->ally_ok[k])
        {
            sp_rect_px(SpR_Q0 + 15 + k, &x0, &y0, &x1, &y1);
            um_icon_draw(c, q->ally_spr[k], x0, y0, x1, y1, 1.0f, sp_hot(SpR_Q0 + 15 + k) ? 1.15f : 1.0f);
        }
    }
    for (int k = 0; k <= 1; k++)
    {
        if (!sp_shown(SpR_Q0 + k))
            continue;
        sp_rect_px(SpR_Q0 + k, &x0, &y0, &x1, &y1);
        sp_icon_tile(c, st, x0, y0, x1, y1, sp_normal_spr(sp_btn[SpR_Q0 + k]), sp_hot(SpR_Q0 + k), sp_down(SpR_Q0 + k),
            (sp_btn[SpR_Q0 + k]->flags & LbBtnF_Enabled) != 0, false, 0.8f, 1.0f);
    }
}

struct SpCrRow {
    long spr;           /* the kind's picture (0: no row) */
    TbBool has;         /* any of the kind */
    long count[3];
    int angry[3];       /* of them, angry */
    TbBool bright[3];
};

static void sp_creatures_of(struct SpCrRow *rows)
{
    memset(rows, 0, sizeof(struct SpCrRow) * 6);
    struct Dungeon *dungeon = get_my_dungeon();
    for (int r = 0; r < 6; r++)
    {
        const struct GuiButton *pic = sp_btn[SpR_C0 + 5 + 4 * r];
        if ((pic == NULL) || ((pic->flags & LbBtnF_Visible) == 0))
            continue;
        rows[r].spr = pic->sprite_idx;
        rows[r].has = ((pic->flags & LbBtnF_Enabled) != 0);
        ThingModel crmodel;
        if ((r > 0) && (top_of_breed_list + r < game.conf.crtr_conf.model_count))
            crmodel = breed_activities[top_of_breed_list + r];
        else
            crmodel = get_players_special_digger_model(my_player_number);
        for (int j = 0; j < 3; j++)
        {
            const struct GuiButton *cell = sp_btn[SpR_C0 + 6 + 4 * r + j];
            if ((cell == NULL) || (cell->content.lptr == NULL) || (crmodel <= 0) || (crmodel >= game.conf.crtr_conf.model_count))
                continue;
            const long total = *cell->content.lptr;
            const long job = (cell->content.lptr - &activity_list[0]) & 0x03;
            rows[r].count[j] = total;
            if (total > 0)
            {
                const long a = max(0L, min((long)dungeon->guijob_angry_creatrs_count[crmodel][job], total));
                rows[r].angry[j] = (int)(a * 100 / total);
                rows[r].bright[j] = (dungeon->guijob_all_creatrs_count[crmodel][job] != 0);
            }
        }
    }
}

static void sp_paint_creatures(struct UiCanvas *c, const struct UmStyle *st, const struct SpCrRow *rows)
{
    const float u = st->u;
    float x0, y0, x1, y1;
    static const long heads[3] = {GPS_rpanel_tab_crtr_wandr_std, GPS_rpanel_tab_crtr_work_std, GPS_rpanel_tab_crtr_fight_std};
    for (int j = 0; j < 3; j++)
    {
        sp_rect_px(SpR_C0 + j, &x0, &y0, &x1, &y1);
        const TbBool hot = sp_hot(SpR_C0 + j);
        um_card(c, st, x0, y0, x1, y1);
        if (hot)
            ui_stroke_rrect(c, x0, y0, x1, y1, S(12.0f), 1.0f, UIC(255, 214, 150, 130));
        int iw = 0, ih = 0;
        const unsigned char *pic = sp_inner_pic(heads[j] - (sp_down(SpR_C0 + j) ? 1 : 0), 5, &iw, &ih);
        sp_picture(c, pic, iw, ih, x0, y0, x1, y1, 0.78f, hot ? 1.15f : 1.0f);
    }
    for (int k = 3; k <= 4; k++)
    {
        if (!sp_shown(SpR_C0 + k))
            continue;
        sp_rect_px(SpR_C0 + k, &x0, &y0, &x1, &y1);
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, rad = (x1 - x0) * 0.5f;
        const TbBool on = (sp_btn[SpR_C0 + k]->flags & LbBtnF_Enabled) != 0;
        sp_round(c, st, cx, cy, rad, sp_hot(SpR_C0 + k), sp_down(SpR_C0 + k));
        int iw = 0, ih = 0;
        const unsigned char *pic = sp_framed_pic(sp_normal_spr(sp_btn[SpR_C0 + k]), &iw, &ih);
        sp_picture(c, pic, iw, ih, cx - rad, cy - rad, cx + rad, cy + rad, 0.62f, on ? 1.0f : 0.45f);
    }
    for (int r = 0; r < 6; r++)
    {
        if (rows[r].spr == 0)
            continue;
        sp_rect_px(SpR_C0 + 5 + 4 * r, &x0, &y0, &x1, &y1);
        um_well(c, st, x0, y0, x1, y1, S(8.0f));
        um_frame(c, st, x0, y0, x1, y1, S(8.0f), sp_hot(SpR_C0 + 5 + 4 * r));
        {
            /* the creatures' pictures at one scale, each on its tile's middle: fitted one by one, the Imp's (11 x 12, the
               others about 15 x 20) came out larger than the rest (the user, 2026-11-26); a few pixels smaller: the user */
            int iw = 0, ih = 0;
            const unsigned char *px = um_icon(rows[r].spr, &iw, &ih);
            if ((px != NULL) && (iw > 0) && (ih > 0))
            {
                const float bw = (x1 - x0) * 0.66f, bh = (y1 - y0) * 0.66f;
                const float k = min(min(bw / (float)iw, bh / (float)ih), bh / 20.0f);
                const int dw = max(1, (int)((float)iw * k + 0.5f)), dh = max(1, (int)((float)ih * k + 0.5f));
                ui_image_gain(c, (int)floorf((x0 + x1 - (float)dw) * 0.5f + 0.5f), (int)floorf((y0 + y1 - (float)dh) * 0.5f + 0.5f), dw, dh, px,
                    iw, ih, rows[r].has ? 1.0f : 0.55f);
            }
        }
        for (int j = 0; j < 3; j++)
        {
            sp_rect_px(SpR_C0 + 6 + 4 * r + j, &x0, &y0, &x1, &y1);
            um_card(c, st, x0, y0, x1, y1);
            if (sp_hot(SpR_C0 + 6 + 4 * r + j))
                ui_stroke_rrect(c, x0, y0, x1, y1, S(12.0f), 1.0f, UIC(255, 214, 150, 130));
            char t[16];
            snprintf(t, sizeof(t), "%ld", rows[r].count[j]);
            ui_text_box(c, UiF_Num, S(14.0f), x0, y0, x1 - x0, y1 - y0, 1, /* (on the middle: the user, 2026-11-26) */
                (rows[r].count[j] > 0) ? (rows[r].bright[j] ? UM_BRIGHT : UM_SOFT) : UIC(110, 100, 88, 255), 0.0f, !st->flat, t);
            /* how many of them are angry: a red bar along the foot */
            if (rows[r].angry[j] > 0)
            {
                const float bx0 = x0 + S(6.0f), bx1 = x1 - S(6.0f), by0 = y1 - S(6.0f), by1 = y1 - S(3.5f);
                ui_fill_rrect(c, bx0, by0, bx1, by1, S(1.2f), UIC(255, 236, 214, 20), UIC(255, 236, 214, 20), false);
                ui_fill_rrect(c, bx0, by0, bx0 + (bx1 - bx0) * (float)rows[r].angry[j] / 100.0f, by1, S(1.2f), UIC(220, 40, 24, 255),
                    UIC(255, 90, 50, 255), true);
            }
        }
    }
}

static uint32_t sp_creature_hash(int n, const struct SpCrRow *rows)
{
    if (n < 5)
        return um_hash("c%d %d %d %d", n, (int)sp_shown(SpR_C0 + n), (int)sp_hot(SpR_C0 + n),
            (int)(sp_down(SpR_C0 + n) || ((sp_btn[SpR_C0 + n] != NULL) && (sp_btn[SpR_C0 + n]->flags & LbBtnF_Enabled))));
    const int r = (n - 5) / 4;
    return um_hash("c%d %ld %d %ld %ld %ld %d %d %d %d%d%d %d", n, rows[r].spr, (int)rows[r].has, rows[r].count[0], rows[r].count[1],
        rows[r].count[2], rows[r].angry[0], rows[r].angry[1], rows[r].angry[2], (int)rows[r].bright[0], (int)rows[r].bright[1],
        (int)rows[r].bright[2], (int)sp_hot(SpR_C0 + n));
}

/******************************************************************************/
/* The event buttons (the notifications): round buttons with the event's sign cut off its stone frame */

float interpolate_synced(float previous, float current); /* (engine_render.h) */

/******************************************************************************/
/* A creature's pages (the one taken over; the one queried outside endless worlds): the creature window's look
   (world_creature_window.c) in the panel's column (the user, 2026-11-27: "the panel during creature possession needs to
   be modernized into the Molten design") */

static void sp_cq_portrait(float *r)
{
    r[0] = 17.5f; r[1] = 288.0f; r[2] = 78.0f; r[3] = 78.0f;
}

static struct Thing *sp_cq_thing(void)
{
    struct Thing *thing = thing_get(get_user_state(get_displayed_user())->controlled_thing_idx);
    return (thing_is_creature(thing) && (thing->ccontrol_idx > 0)) ? thing : NULL;
}

static const char *sp_cq_kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *name = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((name != NULL) && (name[0] != 0)) ? name : creature_code_name(model);
}

/** An ability's slot (0-9) on the page: its kind, whether the creature has it, its clock (1: ready) and whether it is the
 *  one chosen (possession: the one the mouse uses). */
static CrInstance sp_cq_ability(struct Thing *thing, int slot, TbBool *avail, float *clock, TbBool *chosen)
{
    const CrInstance inst = creature_instance_get_available_id_for_pos(thing, slot);
    *avail = creature_instance_is_available(thing, inst);
    *clock = 1.0f;
    *chosen = false;
    if (!*avail)
        return inst;
    const struct CreatureControl *lcc = get_local_creature_control(thing);
    if (creature_control_invalid(lcc))
        return inst;
    const GameTurn turn = get_local_creature_turn(thing);
    const struct InstanceInfo *inf = creature_instance_info_get(inst);
    if (lcc->instance_id == inst)
        *clock = 0.0f;
    else if (!creature_instance_has_reset_at(thing, lcc, inst, turn))
    {
        const int32_t need = ((thing->alloc_flags & TAlF_IsControlled) != 0) ? inf->fp_reset_time : inf->reset_time;
        const int32_t done = (GameTurnDelta)(turn - lcc->instance_use_turn[inst]) + lcc->inst_action_turns - lcc->inst_total_turns;
        *clock = (need > 0) ? (float)max(0, min(need, done)) / (float)need : 1.0f;
    }
    *chosen = (lcc->active_instance_id == inst);
    return inst;
}

static struct GuiButton *sp_cq_pencil_btn(void)
{
    return sp_cq_stats_page() ? sp_btn[SpR_P0 + 2] : NULL;
}

/** What the page shows now, as a key. */
static uint32_t sp_cq_hash(void)
{
    struct Thing *thing = sp_cq_thing();
    if (thing == NULL)
        return um_hash("none %d", sp_info_menu);
    char key[768];
    int kn = 0;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    int32_t angr_typ = 0, angr_prct = 0;
    anger_get_creature_highest_anger_type_and_byte_percentage(thing, &angr_typ, &angr_prct);
    const char *given = world_misdeeds_shown_name(thing);
    const struct GuiButton *pencil = sp_cq_pencil_btn();
    const TbBool typing = (pencil != NULL) && (pencil == input_button);
    kn += snprintf(key + kn, sizeof(key) - kn, "%d %d %d %d %ld %ld %d %ld %ld %d|%s|%s|%d|", sp_info_menu, (int)thing->index, (int)thing->model,
        (int)thing->owner, (long)thing->health, creature_control_invalid(cctrl) ? 0L : (long)cctrl->max_health,
        creature_control_invalid(cctrl) ? 0 : (int)cctrl->exp_level, creature_control_invalid(cctrl) ? 0L : (long)cctrl->exp_points,
        (long)creature_thing_exp_to_next_level(thing), (int)(5 * angr_prct / 256), (given != NULL) ? given : "",
        typing ? world_misdeeds_name_typed() : "", typing && ((LbTimerClock() / 250) & 1));
    for (int n = 3; (n < 13) && (kn < (int)sizeof(key) - 48); n++)
    {
        const struct GuiButton *b = sp_btn[SpR_P0 + n];
        if (b == NULL)
            continue;
        if (sp_cq_stats_page())
            kn += snprintf(key + kn, sizeof(key) - kn, "|%s", creature_statistic_text(thing, (CreatureLiveStatId)b->content.lval));
        else if (n >= 4)
        {
            TbBool avail, chosen;
            float clock;
            const CrInstance inst = sp_cq_ability(thing, (int)b->content.lval, &avail, &clock, &chosen);
            kn += snprintf(key + kn, sizeof(key) - kn, ";%d,%d,%d,%d", (int)inst, (int)avail, (int)(clock * 64.0f), (int)chosen);
        }
    }
    return um_hash("%s", key);
}

static void sp_paint_cq(struct UiCanvas *c, const struct UmStyle *st)
{
    const float u = st->u;
    struct Thing *thing = sp_cq_thing();
    float x0, y0, x1, y1;
    /* the page button */
    if (sp_shown(SpR_P0))
    {
        sp_rect_px(SpR_P0, &x0, &y0, &x1, &y1);
        sp_icon_tile(c, st, x0, y0, x1, y1, sp_normal_spr(sp_btn[SpR_P0]), sp_hot(SpR_P0), sp_down(SpR_P0),
            (sp_btn[SpR_P0]->flags & LbBtnF_Enabled) != 0, false, 0.8f, 1.0f);
    }
    if (thing == NULL)
        return;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const TbBool stats = sp_cq_stats_page();
    if (!stats)
    {
        /* the portrait */
        float r[4];
        sp_cq_portrait(r);
        x0 = X(r[0]); y0 = Y(r[1]); x1 = X(r[0] + r[2]); y1 = Y(r[1] + r[3]);
        const float rad = S(14.0f);
        um_well(c, st, x0, y0, x1, y1, rad);
        const long spr_idx = get_creature_model_graphics(thing->model, CGI_QuerySymbol);
        const struct TbSprite *spr = (spr_idx > 0) ? get_button_sprite_for_player(spr_idx, thing->owner) : NULL;
        const unsigned char *px = (spr != NULL) ? uf_sprite_true(spr) : NULL;
        if ((px != NULL) && (spr->SWidth > 0) && (spr->SHeight > 0))
        {
            const float k = min((x1 - x0 - S(14.0f)) / (float)spr->SWidth, (y1 - y0 - S(14.0f)) / (float)spr->SHeight);
            const int dw = max(1, (int)((float)spr->SWidth * k)), dh = max(1, (int)((float)spr->SHeight * k));
            ui_image_gain(c, (int)((x0 + x1) * 0.5f - (float)dw * 0.5f), (int)((y0 + y1) * 0.5f - (float)dh * 0.5f), dw, dh, px, spr->SWidth,
                spr->SHeight, 1.0f);
        }
        um_frame(c, st, x0, y0, x1, y1, rad, false);
        /* the mood: its face and its word */
        if (sp_btn[SpR_P0 + 1] != NULL)
        {
            sp_rect_px(SpR_P0 + 1, &x0, &y0, &x1, &y1);
            const TbBool hot = sp_hot(SpR_P0 + 1);
            um_card(c, st, x0, y0, x1, y1);
            int32_t angr_typ = 0, angr_prct = 0;
            anger_get_creature_highest_anger_type_and_byte_percentage(thing, &angr_typ, &angr_prct);
            const int pos = max(0, min(4, 5 * (int)angr_prct / 256));
            static const char *const moods[5] = {"Very happy", "Happy", "Content", "Unhappy", "Angry"};
            um_icon_draw(c, GPS_symbols_creatr_mood_vhappy_std + pos, x0 + S(3.0f), y0 + S(3.0f), x0 + S(31.0f), y1 - S(3.0f), 0.9f,
                hot ? 1.15f : 1.0f);
            ui_text_box(c, UiF_Body, S(10.0f), x0 + S(33.0f), y0 + S(3.0f), x1 - x0 - S(37.0f), S(13.0f), 0, UM_DIM, 0.0f, !st->flat, "Mood");
            ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, S(12.0f), 0.0f, x1 - x0 - S(37.0f), moods[pos]), x0 + S(33.0f), y0 + S(17.0f),
                x1 - x0 - S(37.0f), S(15.0f), 0, (pos >= 3) ? UM_EMBER : UM_TEXT, 0.0f, !st->flat, moods[pos]);
        }
        /* the level and the way to the next */
        if ((sp_btn[SpR_P0 + 2] != NULL) && !creature_control_invalid(cctrl))
        {
            sp_rect_px(SpR_P0 + 2, &x0, &y0, &x1, &y1);
            const TbBool hot = sp_hot(SpR_P0 + 2);
            um_card(c, st, x0, y0, x1, y1);
            char lv[24];
            snprintf(lv, sizeof(lv), "Level %d", (int)cctrl->exp_level + 1);
            ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, S(14.0f), 0.0f, x1 - x0 - S(16.0f), lv), x0 + S(8.0f), y0 + S(3.0f), x1 - x0 - S(16.0f),
                S(18.0f), 0, hot ? UM_GOLD_HI : UM_GOLD, 0.0f, !st->flat, lv);
            const long need = creature_thing_exp_to_next_level(thing) << 8;
            const float frac = (need > 0) ? (float)min((long)cctrl->exp_points, need) / (float)need : 1.0f;
            um_bar(c, st, x0 + S(8.0f), y1 - S(11.0f), x1 - S(8.0f), y1 - S(6.0f), frac);
        }
    }
    /* the name, over its health (a click: the keeper's next creature) */
    const int name_role = stats ? SpR_P0 + 1 : SpR_P0 + 3;
    if (sp_btn[name_role] != NULL)
    {
        sp_rect_px(name_role, &x0, &y0, &x1, &y1);
        const float rad = S(8.0f);
        um_well(c, st, x0, y0, x1, y1, rad);
        float frac = 0.0f;
        if (!creature_control_invalid(cctrl) && (cctrl->max_health > 0))
            frac = (float)max((HitPoints)0, min(thing->health, cctrl->max_health)) / (float)cctrl->max_health;
        if (frac > 0.0f)
        {
            const float hx = x0 + S(2.5f) + (x1 - x0 - S(5.0f)) * frac;
            const UiColour top = (frac > 0.5f) ? UIC(150, 30, 20, 255) : UIC(190, 40, 20, 255);
            ui_fill_rrect(c, x0 + S(2.5f), y0 + S(2.5f), hx, y1 - S(2.5f), rad - S(2.5f), top, UIC(90, 14, 10, 255), false);
        }
        const char *given = world_misdeeds_shown_name(thing);
        const char *name = (given != NULL) ? given : sp_cq_kind_name(thing->model);
        const struct GuiButton *pencil = sp_cq_pencil_btn();
        if ((pencil != NULL) && (pencil == input_button))
        {
            char line[WORLD_MISDEED_NAME_MAX + 8];
            snprintf(line, sizeof(line), "%s%s", world_misdeeds_name_typed(), ((LbTimerClock() / 250) & 1) ? "_" : "");
            ui_stroke_rrect(c, x0, y0, x1, y1, rad, max(1.0f, S(1.0f)), UIC(255, 200, 120, 220));
            ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, S(13.0f), 0.0f, x1 - x0 - S(14.0f), line), x0 + S(7.0f), y0, x1 - x0 - S(14.0f),
                y1 - y0, 1, UM_BRIGHT, 0.0f, !st->flat, line);
        } else
            ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, S(14.0f), 0.0f, x1 - x0 - S(14.0f), name), x0 + S(7.0f), y0, x1 - x0 - S(14.0f),
                y1 - y0, 1, sp_hot(name_role) ? UM_GOLD_HI : UM_BRIGHT, 0.0f, !st->flat, name);
        if (stats && sp_shown(SpR_P0 + 2))
        {
            sp_rect_px(SpR_P0 + 2, &x0, &y0, &x1, &y1);
            um_button(c, st, x0, y0, x1, y1, UmB_Normal, sp_hot(SpR_P0 + 2) ? UmS_Hot : UmS_Normal, 1.0f, "");
            world_window_pencil(c, x0, y0, x1, y1, sp_hot(SpR_P0 + 2));
        }
    }
    for (int n = stats ? 3 : 4; n < 13; n++)
    {
        const struct GuiButton *b = sp_btn[SpR_P0 + n];
        if (b == NULL)
            continue;
        sp_rect_px(SpR_P0 + n, &x0, &y0, &x1, &y1);
        const TbBool hot = sp_hot(SpR_P0 + n);
        if (stats)
        {
            /* a statistic: its picture and its value */
            um_card(c, st, x0, y0, x1, y1);
            if (hot && !st->flat)
                ui_stroke_rrect(c, x0, y0, x1, y1, S(10.0f), max(1.0f, S(1.0f)), UIC(255, 214, 150, 170));
            um_icon_draw(c, b->sprite_idx, x0 + S(3.0f), y0 + S(3.0f), x0 + S(31.0f), y1 - S(3.0f), 0.86f, hot ? 1.15f : 1.0f);
            const char *v = creature_statistic_text(thing, (CreatureLiveStatId)b->content.lval);
            ui_text_box(c, UiF_Num, ui_text_fit(UiF_Num, S(14.0f), 0.0f, x1 - x0 - S(40.0f), v), x0 + S(32.0f), y0, x1 - x0 - S(39.0f), y1 - y0, 2,
                hot ? UM_GOLD_HI : UM_TEXT, 0.0f, !st->flat, v);
            continue;
        }
        /* an ability: its picture, its key, its clock; lit while chosen */
        TbBool avail, chosen;
        float clock;
        const int slot = (int)b->content.lval;
        const CrInstance inst = sp_cq_ability(thing, slot, &avail, &clock, &chosen);
        if (!avail)
        {
            um_well(c, st, x0, y0, x1, y1, S(9.0f));
            continue;
        }
        const struct InstanceInfo *inf = creature_instance_info_get(inst);
        um_button(c, st, x0, y0, x1, y1, UmB_Normal, hot ? UmS_Hot : (chosen ? UmS_Armed : UmS_Normal), 1.0f, "");
        um_icon_draw(c, inf->symbol_spridx, x0 + S(3.0f), y0 + S(3.0f), x0 + S(31.0f), y1 - S(3.0f), 0.86f, (clock >= 1.0f) ? 1.0f : 0.5f);
        char key[4];
        snprintf(key, sizeof(key), "%d", (slot + 1) % 10);
        ui_text_box(c, UiF_Num, S(12.0f), x0 + S(33.0f), y0 + S(3.0f), x1 - x0 - S(40.0f), S(14.0f), 2, UM_DIM, 0.0f, !st->flat, key);
        um_bar(c, st, x0 + S(34.0f), y1 - S(11.0f), x1 - S(7.0f), y1 - S(6.0f), clock);
    }
}

/** The page's own drawing (the menu's background: portrait, frame, name box) is left out where the panel paints it. */
TbBool world_side_panel_paints_page(const struct GuiMenu *gmnu)
{
    return sp_placed && (gmnu != NULL) && sp_cq_ident(gmnu->ident) && (gmnu->ident == sp_info_menu);
}

static struct UmHost sp_ev_host;
#define SP_EV_PICS 4096
static unsigned char *sp_ev_px[SP_EV_PICS];
static short sp_ev_w[SP_EV_PICS], sp_ev_h[SP_EV_PICS];
static TbBool sp_ev_tried[SP_EV_PICS];

/** An event's sign: what differs from the blank frame (GPS_message_rpanel_msg_blank_std), cropped; a picture of
 *  another size (a custom icon) as the panel's icons are. */
static const unsigned char *sp_ev_pic(long spr, int *w, int *h)
{
    if ((spr <= 0) || (spr >= SP_EV_PICS))
        return NULL;
    if (!sp_ev_tried[spr])
    {
        sp_ev_tried[spr] = true;
        int ow = 0, oh = 0;
        unsigned char *ov = world_ui_icon_override(3, spr, &ow, &oh);
        if (ov != NULL)
        {
            sp_ev_px[spr] = ov; sp_ev_w[spr] = (short)ow; sp_ev_h[spr] = (short)oh;
        }
        int fw = 0, fh = 0, pw = 0, ph = 0;
        unsigned char *f = (ov == NULL) ? sp_sprite(GPS_message_rpanel_msg_blank_std, &fw, &fh) : NULL;
        unsigned char *px = (ov == NULL) ? sp_sprite(spr, &pw, &ph) : NULL;
        if ((f != NULL) && (px != NULL) && (fw == pw) && (fh == ph))
        {
            unsigned char *m = (unsigned char *)calloc((size_t)pw * ph, 1);
            if (m != NULL)
            {
                for (int i = 0; i < pw * ph; i++)
                {
                    const unsigned char *a = px + (size_t)i * 4, *b = f + (size_t)i * 4;
                    const int d = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) + ((a[3] != b[3]) ? 255 : 0);
                    m[i] = (a[3] != 0) && (d > 24) && !sp_shadow(a); /* (its drop shadow off: it stands in the middle) */
                }
                sp_keep_groups(m, pw, ph, 3);
                /* (sp_cut_store keeps its result in the tab slots: here the event's own) */
                int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
                for (int y = 0; y < ph; y++)
                    for (int x = 0; x < pw; x++)
                    {
                        if (!m[y * pw + x])
                        {
                            px[(y * pw + x) * 4 + 3] = 0;
                            continue;
                        }
                        x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y);
                    }
                if (x1 >= x0)
                {
                    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
                    unsigned char *o = (unsigned char *)malloc((size_t)cw * ch * 4);
                    if (o != NULL)
                    {
                        for (int y = 0; y < ch; y++)
                            memcpy(o + (size_t)y * cw * 4, px + ((size_t)(y + y0) * pw + x0) * 4, (size_t)cw * 4);
                        sp_ev_px[spr] = o;
                        sp_ev_w[spr] = (short)cw;
                        sp_ev_h[spr] = (short)ch;
                    }
                }
                free(m);
            }
        }
        free(f);
        free(px);
    }
    if (sp_ev_px[spr] == NULL)
        return um_icon(spr, w, h);
    *w = sp_ev_w[spr];
    *h = sp_ev_h[spr];
    return sp_ev_px[spr];
}

struct SpEv {
    long spr;       /* 0: none */
    TbBool hot, lit, blink;
    float y;        /* panel units (falling: on its way down) */
};

static void sp_ev_of(int k, struct SpEv *ev)
{
    memset(ev, 0, sizeof(*ev));
    const struct GuiButton *gbtn = sp_btn[SpR_Ev0 + k];
    if ((gbtn == NULL) || ((gbtn->flags & LbBtnF_Visible) == 0) || chat_box_icon_button(gbtn))
        return;
    const EventIndex evidx = get_my_event_button_index(event_button_slot(gbtn));
    if (evidx == 0)
        return;
    const struct Event *event = &game.event[evidx];
    /* the sprite gui_area_event_button shows: the coloured sign, its lit picture while open or pressed, its blink */
    const TbBool custom = is_custom_icon(event->icon_idx);
    long spr = (event->icon_idx >= 0) ? event->icon_idx : event_button_info[event->kind].bttn_sprite;
    if (((event->kind == EvKind_Objective) || (event->kind == EvKind_Information) || (event->kind == EvKind_QuickInformation)) && custom &&
        (evidx == my_visible_event_idx))
        spr++;
    if (sp_beat())
    {
        switch (event->kind)
        {
        case EvKind_Information:
        case EvKind_QuickInformation:
            if (!(my_event_button_state[evidx] & EvBtnS_Read) && (!custom || (get_custom_icon_frame_count(event->icon_idx) >= 3)))
            {
                spr += 2;
                ev->blink = true;
            }
            break;
        case EvKind_FriendlyFight:
        case EvKind_EnemyFight:
        case EvKind_HeartAttacked:
            if ((event->mappos_x != 0) || (event->mappos_y != 0))
            {
                spr += 2;
                ev->blink = true;
            }
            break;
        default:
            break;
        }
    }
    const TbBool down = (gbtn->button_state_left_pressed != 0) || (gbtn->button_state_right_pressed != 0);
    if (!down && (evidx != my_visible_event_idx) && !custom)
        spr++;
    ev->spr = spr;
    ev->hot = ((gbtn->flags & LbBtnF_MouseOver) != 0);
    ev->lit = (evidx == my_visible_event_idx) || down;
    float r[4];
    sp_role_rect(SpR_Ev0 + k, r);
    ev->y = r[1];
    if (flag_is_set(event->flags, EvF_BtnFalling))
        ev->y = interpolate_synced(r[1] - SP_EV, r[1]);
}

static void sp_draw_events(void)
{
    const float u = sp_u;
    const TbBool flat = um_host_flat();
    const struct UmStyle stv = {u, flat};
    struct SpEv evs[13];
    for (int k = 0; k < 13; k++)
        sp_ev_of(k, &evs[k]);
    const int m = (int)ceilf(S(12.0f));
    const int x0 = sp_px(sp_ox + S(sp_ev_col())) - m, y0 = 0;
    const int w = sp_px(S(SP_EV)) + 2 * m, h = (int)MyScreenHeight;
    /* the tab under them: points the way the panel will go */
    float tr[4];
    sp_tab_rect(tr);
    const TbBool tab_on = (sp_hide_mode != 2) && (sp_a_events < 0.999f);
    const TbBool tab_hot = tab_on && world_side_tab_over();
    sp_ev_host.name = "side_events";
    sp_ev_host.slot = UmSl_Events;
    char key[64];
    snprintf(key, sizeof(key), "%.4f %d %d %d", (double)u, w, h, (int)flat);
    const unsigned long t0 = (unsigned long)LbTimerClock();
    TbBool full = um_host_begin(&sp_ev_host, w, h, key);
    if (um_host_base_begin(&sp_ev_host, key))
    {
        ui_canvas_clear(&sp_ev_host.base);
        full = true;
    }
    struct UmPart parts[UM_PARTS];
    int np = 0;
    const float offx = ((float)sp_ox - (float)x0) / u, offy = ((float)sp_oy - (float)y0) / u;
    for (int k = 0; k < 13; k++)
    {
        float r[4];
        sp_role_rect(SpR_Ev0 + k, r);
        /* (a part from where it falls from to where it lands) */
        um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] - SP_EV + offy, r[2], r[3] + SP_EV, 12.0f,
            um_hash("e%d %ld %d %d %d %d", k, evs[k].spr, (int)evs[k].hot, (int)evs[k].lit, (int)evs[k].blink, (int)(evs[k].y * 4.0f)));
    }
    um_part(parts, &np, u, 0.0f, tr[0] + offx, tr[1] + offy, tr[2], tr[3], 12.0f,
        um_hash("tab %d %d %d", (int)tab_on, (int)tab_hot, (int)(sp_hide_mode >= 1)));
    if (um_host_parts(&sp_ev_host, parts, np, full))
    {
        struct UiCanvas *c = &sp_ev_host.canvas;
        for (int k = 0; k < 13; k++)
        {
            const struct SpEv *ev = &evs[k];
            if (ev->spr == 0)
                continue;
            float r[4];
            sp_role_rect(SpR_Ev0 + k, r);
            const float rad = S(SP_EV * 0.5f);
            const float cx = sp_ox - (float)x0 + S(r[0]) + rad, cy = sp_oy - (float)y0 + S(ev->y) + rad;
            sp_round(c, &stv, cx, cy, rad, ev->hot || ev->lit, false);
            if (ev->lit)
                ui_stroke_rrect(c, cx - rad, cy - rad, cx + rad, cy + rad, rad, max(1.0f, S(1.5f)), UM_GOLD_HI);
            int iw = 0, ih = 0;
            const unsigned char *pic = sp_ev_pic(ev->spr, &iw, &ih);
            sp_picture(c, pic, iw, ih, cx - rad, cy - rad, cx + rad, cy + rad, 0.66f, ev->hot ? 1.15f : 1.0f);
        }
        if (tab_on)
        {
            /* the tab: a small Molten button, an ember chevron the way the panel goes (back: right; away: left) */
            const float tx0 = sp_ox - (float)x0 + S(tr[0]), ty0 = sp_oy - (float)y0 + S(tr[1]);
            const float tx1 = tx0 + S(tr[2]), ty1 = ty0 + S(tr[3]);
            const float cx = (tx0 + tx1) * 0.5f, cy = (ty0 + ty1) * 0.5f, hw = S(4.5f), hh = S(7.0f), th = S(2.8f);
            sp_round(c, &stv, cx, cy, (tx1 - tx0) * 0.5f, tab_hot, false); /* (round, as the event buttons) */
            const float d = (sp_hide_mode >= 1) ? 1.0f : -1.0f; /* (the chevron's point: right to bring it back) */
            const float xy[12] = {cx - d * hw, cy - hh, cx - d * hw + d * th, cy - hh, cx + d * hw + d * th * 0.5f, cy,
                cx - d * hw + d * th, cy + hh, cx - d * hw, cy + hh, cx + d * hw - d * th * 0.5f, cy};
            const int counts[1] = {6};
            ui_fill_path(c, xy, counts, 1, tab_hot ? UIC(255, 170, 90, 255) : UIC(232, 96, 36, 255));
        }
        um_host_parts_end(&sp_ev_host);
        um_host_painted(&sp_ev_host, t0);
    }
    um_host_show(&sp_ev_host, x0, y0);
}

static void sp_icon_dump(void);

void world_side_panel_draw(void)
{
    if (!world_side_panel_on())
        return;
    world_side_panel_place();
    /* slid away and still: the panel is neither painted nor shown until it moves again; the event buttons stay where
       the tab left them (the user, 2026-11-27: "cant you just hide after the move transitions?") */
    if ((sp_hide_mode >= 1) && (sp_a_panel >= 1.0f))
    {
        if ((sp_hide_mode == 1) || (sp_a_events < 1.0f))
            sp_draw_events();
        return;
    }
    sp_icon_dump(); /* (test switch KFX_UI_ICONDUMP) */
    const float u = sp_u;
    const TbBool flat = um_host_flat();
    const struct UmStyle stv = {u, flat};
    sp_host.name = "side_panel";
    sp_host.slot = UmSl_Side;
    const int tab = sp_active_tab();
    const float bottom = sp_glass_bottom();
    /* the glass: against the screen's left, top and bottom edges (the user, 2026-11-26: "0 margin top, left and
       bottom"), only its right edge seen; its own picture, painted again only when its size changes */
    {
        const int gm = flat ? 0 : (int)ceilf(um_window_margin(&stv) * 0.6f);
        const int gx1 = sp_px(sp_ox - sp_shift + S(SP_GLASS_X1)); /* (its home width: shown slid, not painted again) */
        const int gw = gx1 + gm, gh = (int)MyScreenHeight;
        char gk[96];
        snprintf(gk, sizeof(gk), "%.4f %d %d %d", (double)u, gw, gh, (int)flat);
        sp_glass_host.name = "side_glass";
        sp_glass_host.slot = UmSl_SideGlass;
        if (um_host_begin(&sp_glass_host, gw, gh, gk))
        {
            const unsigned long tg = (unsigned long)LbTimerClock();
            const float over = S(80.0f); /* (its rounded corners and top light off the screen) */
            um_window(&sp_glass_host.canvas, &stv, -over, -over, (float)gx1, (float)gh + over);
            um_host_painted(&sp_glass_host, tg);
        }
        um_host_show(&sp_glass_host, sp_px(sp_shift), 0);
    }
    const int m = flat ? 0 : (int)ceilf(S(16.0f));
    sp_cx0 = sp_px(sp_ox + S(SP_GLASS_X0)) - m; /* (may stand off the screen's left while the panel slides: not painted again) */
    sp_cy0 = max(0, sp_px(sp_oy + S(SP_GLASS_Y0)) - m);
    const int cw = sp_px(sp_ox + S(SP_GLASS_X1)) + m - sp_cx0, ch = sp_px(sp_oy + S(bottom)) + m - sp_cy0;
    struct SpCell cells[16];
    for (int k = 0; k < 16; k++)
        sp_cell_of(((tab >= 1) && (tab <= 3)) ? sp_btn[SpR_Cell0 + k] : NULL, &cells[k]);
    struct SpStrip strip;
    TbBool has_strip = ((tab >= 1) && (tab <= 3)) ? sp_strip_of(sp_btn[SpR_Strip], &strip) : false;
    if (!has_strip)
        memset(&strip, 0, sizeof(strip));
    /* test switch KFX_WORLD_SIDETEST=1: the first room chosen (its strip, flashing), the second under the pointer, levels
       on both and a bar on the second (a windowless run has no mouse and no build) */
    static int sp_test = -1;
    static GameTurn sp_test_turn = 0;
    if (sp_test < 0)
    {
        /* (=q, =s, =w, =c: that tab is opened instead: Information, Research, Workshop, Creatures) */
        const char *v = getenv("KFX_WORLD_SIDETEST");
        sp_test = ((v != NULL) && (v[0] == '1')) ? 1 : 0;
        if ((v != NULL) && (v[0] != '\0') && (v[0] != '1'))
        {
            /* (=o, =x, =v, =n, =a: the Options menu, Quit, Graphics, Sound, the assistant; =t: a tooltip) */
            const char k = v[0];
            if ((k == 'o') || (k == 'x') || (k == 'v') || (k == 'n') || (k == 'a') || (k == 'l') || (k == 'e'))
                turn_on_menu((k == 'o') ? GMnu_OPTIONS : (k == 'x') ? GMnu_QUIT : (k == 'v') ? GMnu_VIDEO : (k == 'n') ? GMnu_SOUND :
                    (k == 'l') ? GMnu_LOAD : (k == 'e') ? GMnu_SAVE : GMnu_AUTOPILOT);
            else if (k == 't')
                sp_test = 2;
            else if ((k == 'g') || (k == 'h') || (k == 'i')) /* (a creature taken over: its first page, its third, or */
                sp_test = (k == 'g') ? 3 : (k == 'h') ? 4 : 5;  /*  its fourth and the wheel on: no Rap Sheet then) */
            else if (k == 'u') /* (paused: the Paused button) */
                set_packet_pause_toggle();
            else
                set_menu_mode((k == 'q') ? GMnu_QUERY : (k == 's') ? GMnu_SPELL : (k == 'w') ? GMnu_TRAP : GMnu_CREATURE);
        }
    }
    if ((sp_test >= 3) && (sp_test <= 5) && (game.play_gameturn > 30) && (get_user_view_type(get_my_user()) == PVT_CreatureContrl) && (sp_cq_thing() != NULL))
    {
        /* (the test hook takes the creature over without the instance that opens its page) */
        const MenuID page = (sp_test == 3) ? GMnu_CREATURE_QUERY1 : (sp_test == 4) ? GMnu_CREATURE_QUERY3 : GMnu_CREATURE_QUERY4;
        turn_on_menu(page);
        set_menu_visible_on(page); /* (as toggle_first_person_menu: the test's takeover leaves it hidden) */
        sp_test_turn = game.play_gameturn;
        sp_test = (sp_test == 5) ? 6 : 0;
    }
    if ((sp_test == 6) && (game.play_gameturn > sp_test_turn + 20))
    {
        wheel_scrolled_down = true; /* (the page cycle as the input turn runs it) */
        update_query_menu();
        sp_test_turn = game.play_gameturn;
        sp_test = 7;
    }
    if ((sp_test == 7) && (game.play_gameturn > sp_test_turn + 20))
    {
        SYNCLOG("World: side panel test - after the fourth page the wheel opens: page 1 %d, Rap Sheet %d",
            (int)menu_is_active(GMnu_CREATURE_QUERY1), (int)menu_is_active(GMnu_CREATURE_QUERY5));
        sp_test = 0;
    }
    if (sp_test == 2)
        world_ui_tooltip("Treasure Room: stores your gold. Imps take gold here; a fuller room holds more of it.", (long)(MyScreenWidth / 2),
            (long)(MyScreenHeight / 2));
    if ((sp_test == 1) && (tab == 1) && (cells[0].show == 1) && (sp_btn[SpR_Cell0] != NULL))
    {
        cells[0].chosen = true;
        cells[0].level = 13;
        cells[0].reached = true;
        cells[1].hot = true;
        cells[1].level = 4;
        cells[1].bar = 0.35f;
        const RoomKind rkind = sp_btn[SpR_Cell0]->content.lval;
        const struct RoomConfigStats *roomst = get_room_kind_stats(rkind);
        strip.spr = roomst->bigsym_sprite_idx;
        snprintf(strip.name, sizeof(strip.name), "%s", get_string(roomst->name_stridx));
        snprintf(strip.value, sizeof(strip.value), "%ld", (long)world_pentagram_room_cost(my_player_number, rkind, roomst->cost));
        strip.bar = 0.62f;
        has_strip = true;
    }
    /* the base: the glass, the hollow, the wells; by the scale, the size and the tab */
    char bkey[128];
    snprintf(bkey, sizeof(bkey), "%.4f %d %d %d %d %d %d %.1f %.1f", (double)u, cw, ch, sp_cx0, sp_cy0, tab, sp_info_menu, (double)sp_map_cx,
        (double)sp_map_cy);
    const unsigned long t0 = (unsigned long)LbTimerClock();
    TbBool full = um_host_begin(&sp_host, cw, ch, bkey);
    if (um_host_base_begin(&sp_host, bkey))
    {
        ui_canvas_clear(&sp_host.base); /* (over the glass's own picture) */
        sp_paint(true, cells, &strip, has_strip, tab);
        full = true;
    }
    /* the live parts: each button, tab, cell and the strip by what it shows */
    struct UmPart parts[UM_PARTS];
    int np = 0;
    const int rounds[] = {SpR_Map, SpR_Options, SpR_ZoomOut, SpR_ZoomIn, SpR_Assist};
    const float offx = (sp_ox - (float)sp_cx0) / u, offy = (sp_oy - (float)sp_cy0) / u; /* (um_part's units: from the canvas) */
    for (size_t i = 0; i < sizeof(rounds) / sizeof(rounds[0]); i++)
    {
        float r[4];
        sp_role_rect(rounds[i], r);
        int on = 0;
        if (rounds[i] == SpR_Assist && sp_shown(SpR_Assist))
            on = 1 + (int)((get_my_dungeon()->computer_enabled & 0x01) != 0) * (1 + (int)sp_beat());
        um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 12.0f,
            um_hash("r%d %d %d %d %d", rounds[i], (int)sp_shown(rounds[i]), (int)sp_hot(rounds[i]), (int)sp_down(rounds[i]), on));
    }
    for (int t = 0; t < 5; t++)
    {
        float r[4];
        sp_role_rect(SpR_Tab0 + t, r);
        um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 16.0f,
            um_hash("t%d %d %d %d %d", t, (int)sp_shown(SpR_Tab0 + t), (int)sp_hot(SpR_Tab0 + t), (int)(t == tab), (int)sp_down(SpR_Tab0 + t)));
    }
    if ((tab >= 1) && (tab <= 3))
    {
        const TbBool blink = sp_blink();
        for (int k = 0; k < 16; k++)
        {
            const struct SpCell *cl = &cells[k];
            float r[4];
            sp_role_rect(SpR_Cell0 + k, r);
            um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 18.0f,
                um_hash("c%d %d %ld %d%d%d%d%d %d %d %d %d", k, cl->show, cl->spr, (int)cl->down, (int)cl->hot, (int)cl->chosen,
                    (int)cl->active, (int)cl->dark, cl->level, (int)cl->reached, (int)(cl->bar * 400.0f), (int)(cl->chosen && blink)));
        }
        float r[4];
        sp_role_rect(SpR_Strip, r);
        um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 2.0f,
            has_strip ? um_hash("s%ld %s %s %d %d %d", strip.spr, strip.name, strip.value, (int)strip.dim, (int)strip.dark, (int)(strip.bar * 400.0f)) : 0u);
        sp_role_rect(SpR_Next, r);
        um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 6.0f,
            um_hash("n%d %d %d %d %d", (int)sp_shown(SpR_Next), (int)sp_hot(SpR_Next), (int)sp_down(SpR_Next), sp_tab_menu,
                (sp_btn[SpR_Next] != NULL) ? (int)sp_btn[SpR_Next]->flags : 0));
    }
    static struct SpQuery q;
    static struct SpCrRow rows[6];
    sp_q_now = NULL;
    sp_rows_now = NULL;
    if (sp_info_menu == GMnu_QUERY)
    {
        sp_query_of(&q);
        sp_q_now = &q;
        for (int n = 0; n < 19; n++)
        {
            float r[4];
            sp_role_rect(SpR_Q0 + n, r);
            um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 10.0f, sp_query_hash(n, &q));
        }
    } else if (sp_info_menu == GMnu_CREATURE)
    {
        sp_creatures_of(rows);
        sp_rows_now = rows;
        for (int n = 0; n < 29; n++)
        {
            float r[4];
            sp_role_rect(SpR_C0 + n, r);
            um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 12.0f, sp_creature_hash(n, rows));
        }
    } else if (sp_cq_ident(sp_info_menu))
    {
        /* (what the page shows as one key: a part is painted again when it or its own button's state changes) */
        const uint32_t all = sp_cq_hash();
        for (int n = 0; n < 13; n++)
        {
            float r[4];
            sp_role_rect(SpR_P0 + n, r);
            if (r[2] <= 0.0f)
                continue;
            um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 10.0f,
                um_hash("p%d %08x %d %d %d", n, (unsigned)all, (int)sp_shown(SpR_P0 + n), (int)sp_hot(SpR_P0 + n), (int)sp_down(SpR_P0 + n)));
        }
        if (!sp_cq_stats_page())
        {
            float r[4];
            sp_cq_portrait(r);
            um_part(parts, &np, u, 0.0f, r[0] + offx, r[1] + offy, r[2], r[3], 10.0f, um_hash("pp %08x", (unsigned)all));
        }
    }
    if (um_host_parts(&sp_host, parts, np, full))
    {
        sp_paint(false, cells, &strip, has_strip, tab);
        um_host_parts_end(&sp_host);
        um_host_painted(&sp_host, t0);
        if ((sp_host.paints <= 3) || (sp_host.paints % 200 == 0))
            SYNCLOG("World: the side panel painted in %lu ms (%dx%d, %s, scale %.2f, %s; %lu paintings)", sp_host.paint_ms, cw, ch,
                full ? "all" : "a part", (double)u, flat ? "palette" : "true colour", sp_host.paints);
    }
    um_host_show(&sp_host, sp_cx0, sp_cy0);
    sp_draw_gold();
    sp_draw_events();
}

void world_side_panel_draw_over(void)
{
    if (!world_side_panel_on() || world_side_minimap_away())
        return;
    const struct Camera *cam = get_local_active_camera();
    if (cam == NULL)
        return;
    const float u = sp_u;
    const float rr = sp_map_r - S(12.0f);
    const float px = S(15.0f);
    const int pad = (int)ceilf(S(10.0f));
    const int w = (int)ceilf(2.0f * sp_map_r) + 2 * pad, h = w;
    const int x = sp_px(sp_map_cx - sp_map_r) - pad, y = sp_px(sp_map_cy - sp_map_r) - pad;
    const int ang = cam->rotation_angle_x & 2047;
    char key[96];
    snprintf(key, sizeof(key), "%d %.4f %d %d", ang, (double)u, w, (int)um_host_flat());
    sp_over_host.name = "side_compass";
    sp_over_host.slot = UmSl_Compass;
    if (um_host_begin(&sp_over_host, w, h, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        const float a = (float)ang * 2.0f * (float)M_PI / 2048.0f;
        const float sn = sinf(a), cs = cosf(a);
        const float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
        const struct { TextStringId id; float dx, dy; } lab[4] = {
            {GUIStr_MapN, -rr * sn, -rr * cs}, {GUIStr_MapS, rr * sn, rr * cs},
            {GUIStr_MapE, rr * cs, -rr * sn}, {GUIStr_MapW, -rr * cs, rr * sn}};
        for (int i = 0; i < 4; i++)
            ui_text_box(&sp_over_host.canvas, UiF_Head, px, cx + lab[i].dx - S(14.0f), cy + lab[i].dy - S(10.0f), S(28.0f), S(20.0f), 1,
                um_host_flat() ? UIC(112, 90, 48, 255) : UIC(222, 178, 88, 102), 0.0f, !um_host_flat(), get_string(lab[i].id));
            /* (60% see-through: the user; the palette version has no see-through letters: a dim gold) */
        um_host_painted(&sp_over_host, t0);
    }
    um_host_show(&sp_over_host, x, y);
}
#undef X
#undef Y
#undef S

/******************************************************************************/
/* The view's own pieces: the day's dial and the World Boss strip (the mockup's top right) */

static struct UmHost sp_dial_host;  /* slot 6, under the game's UI (as the dial was: under the windows) */
static struct UmHost sp_boss_host;  /* slot 7 */

TbBool world_side_hud_modern(void)
{
    return (window_style != WinStyle_Classic) && !netstate_dedicated(); /* (every level, as the side panel) */
}

/** The scale of the view's pieces: the side panel's (its menu's width), else the game's UI unit. */
static float sp_hud_u(void)
{
    const int num = menu_id_to_number(GMnu_MAIN);
    if (num >= 0)
    {
        const struct GuiMenu *gmnu = get_active_menu(num);
        return (float)max(1, (gmnu->width * 16 + 140 / 2) / 140) / 24.0f;
    }
    return (float)max(16, (int)units_per_pixel) / 24.0f;
}

int world_side_dial_draw(const struct TbSprite *disc, int frame)
{
    const float u = sp_hud_u();
    const TbBool flat = um_host_flat();
    const int top = ui_safe_top;
    const float R = (float)ui_safe_right_edge() - 8.0f * u;
    const float r = 34.0f * u;
    const float cx_s = R - 30.0f * u - r, cy_s = (float)top + 10.0f * u + r;
    if (sp_right_away())
        return top + (int)floorf(116.0f * u + 0.5f); /* (slid away and still: not painted; the strip's place as ever) */
    static long shown = -2;
    if (shown == -2)
    {
        const char *e = getenv("KFX_WORLD_DAYLABEL"); /* (the game's test hook: the day shown) */
        shown = (e != NULL) ? atol(e) : -1;
    }
    char text[32];
    snprintf(text, sizeof(text), "%s %ld", world_is_day() ? "Day" : "Night", (shown > 0) ? shown : world_day_number());
    char omen[32];
    const TbBool has_omen = world_pentagram_omen_text(my_player_number, omen, sizeof(omen)); /* (the Pentagram's Omen) */
    const TbBool safe = world_safe_day();
    const long today = world_day_number();
    const int today_i = (int)((max(today, 1L) - 1) % SEASON_YEAR_DAYS);
    const int m = (int)ceilf(20.0f * u);
    const int x0 = (int)floorf(cx_s - r) - m, y0 = max(0, (int)floorf(cy_s - r) - m);
    const int w = (int)ceilf(2.0f * r) + 2 * m, h = (int)ceilf(cy_s + r + (has_omen ? 48.0f : 30.0f) * u) + m - y0;
    char key[160];
    snprintf(key, sizeof(key), "%d %s %d %d %.4f %d %s", frame, text, (int)safe, today_i, (double)u, (int)flat, has_omen ? omen : "");
    sp_dial_host.name = "side_dial";
    sp_dial_host.slot = UmSl_Dial;
    if (um_host_begin(&sp_dial_host, w, h, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &sp_dial_host.canvas;
        const float cx = cx_s - (float)x0, cy = cy_s - (float)y0;
        if (!flat)
        {
            ui_shadow_rrect(c, cx - r, cy - r, cx + r, cy + r, r, 12.0f * u, 4.0f * u, UIC(0, 0, 0, 150));
            ui_glow_rrect(c, cx - r, cy - r, cx + r, cy + r, r, 16.0f * u, UIC(255, 170, 90, 60));
        }
        /* the dial's picture, round */
        if ((disc != NULL) && (disc->SWidth > 0) && (disc->SHeight > 0) && (engine_palette != NULL))
        {
            unsigned char *px = (unsigned char *)calloc((size_t)disc->SWidth * disc->SHeight, 4);
            if (px != NULL)
            {
                ui_sprite_rgba(disc, engine_palette, px);
                const int d = (int)floorf(2.0f * r + 0.5f);
                ui_image(c, (int)floorf(cx - r + 0.5f), (int)floorf(cy - r + 0.5f), d, d, px, disc->SWidth, disc->SHeight, (float)d * 0.5f);
                free(px);
            }
        }
        /* its gold ring, and the year's days on it (the seasons' colours, today larger) */
        const float t = max(1.0f, 2.0f * u);
        ui_stroke_rrect(c, cx - r, cy - r, cx + r, cy + r, r, t, flat ? UM_GOLD : UIC(222, 178, 88, 230));
        static const UiColour season[WSe_COUNT] = {UIC(120, 204, 120, 255), UIC(240, 168, 60, 255), UIC(176, 150, 92, 255),
            UIC(120, 140, 236, 255)};
        for (int i = 0; i < SEASON_YEAR_DAYS; i++)
        {
            const float a = -(float)M_PI / 2.0f + 2.0f * (float)M_PI * (float)i / (float)SEASON_YEAR_DAYS;
            const float rr = r - t * 0.5f;
            const float dx = cx + cosf(a) * rr, dy = cy + sinf(a) * rr;
            const float dr = (i == today_i) ? 2.6f * u : 1.3f * u;
            const UiColour col = season[(i / SEASON_DAYS) % WSe_COUNT];
            if (i == today_i)
                ui_fill_rrect(c, dx - dr - 1.0f, dy - dr - 1.0f, dx + dr + 1.0f, dy + dr + 1.0f, dr + 1.0f, UIC(20, 12, 8, 255),
                    UIC(20, 12, 8, 255), false);
            ui_fill_rrect(c, dx - dr, dy - dr, dx + dr, dy + dr, dr, col, col, false);
        }
        /* "Day N": green on a safe day and its night (no raids, no wild packs: the user, 2026-11-19) */
        ui_text_box(c, UiF_Head, 15.0f * u, 0.0f, cy + r + 6.0f * u, (float)w, 20.0f * u, 1,
            safe ? UIC(130, 230, 110, 255) : UM_BRIGHT, 0.0f, !flat, text);
        /* the Pentagram's Omen: by night, tomorrow's raids, a smaller line under it */
        if (has_omen)
            ui_text_box(c, UiF_Body, max(11.0f, 12.0f * u), 0.0f, cy + r + 26.0f * u, (float)w, 16.0f * u, 1, UM_GOLD_HI, 0.0f,
                !flat, omen);
        um_host_painted(&sp_dial_host, t0);
    }
    um_host_show(&sp_dial_host, x0 + sp_right_slide(u), y0);
    return top + (int)floorf((has_omen ? 134.0f : 116.0f) * u + 0.5f); /* (air between the day and the strip: the user) */
}

void world_side_boss_strip(int y, const char *left, const char *right, TbBool clickable, TbBool hover, int *box)
{
    const float u = sp_hud_u();
    const TbBool flat = um_host_flat();
    const int edge = (int)MyScreenWidth; /* (against the screen's very edge: the user; its words inside the safe area) */
    const float inset = (float)max(0, edge - ui_safe_right_edge());
    const int sw = (int)floorf(206.0f * u + inset + 0.5f), sh = (int)floorf(30.0f * u + 0.5f);
    const int m = flat ? 0 : (int)ceilf(14.0f * u);
    const int x0 = edge - sw - m, y0 = max(0, y - m);
    const int w = edge - x0, h = y + sh + m - y0;
    const int slid = sp_right_slide(u); /* (TAB: off to the right, with the dial) */
    if (sp_right_away())
    {
        box[2] = 0; /* (slid away and still: not painted, and its button goes) */
        return;
    }
    box[0] = edge - sw + slid;
    box[1] = y;
    box[2] = sw;
    box[3] = sh;
    const TbBool lit = clickable && hover;
    char key[192];
    snprintf(key, sizeof(key), "%s|%s|%d %.4f %d %d", left, right, (int)lit, (double)u, w, (int)flat);
    sp_boss_host.name = "side_boss";
    sp_boss_host.slot = UmSl_Boss;
    if (um_host_begin(&sp_boss_host, w, h, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &sp_boss_host.canvas;
        const float bx0 = (float)(edge - sw - x0), by0 = (float)(y - y0), by1 = by0 + (float)sh; /* (its home place: the picture is what slides, not the box in it) */
        const float r = (float)sh * 0.5f;
        /* rounded on its left; on its right square and against the screen's edge (the canvas ends there: the box goes
           on past it, so its right corners are cut off) */
        const float bx1 = (float)w + r + 2.0f;
        if (flat)
        {
            ui_fill_rrect(c, bx0, by0, bx1, by1, 0.0f, UIC(22, 15, 12, 255), UIC(22, 15, 12, 255), false);
            ui_stroke_rrect(c, bx0, by0, bx1, by1, 0.0f, 1.0f, lit ? UM_GOLD_HI : UIC(104, 78, 36, 255));
        } else
        {
            ui_shadow_rrect(c, bx0, by0, bx1, by1, r, (float)m, 4.0f * u, UIC(0, 0, 0, 150));
            if (lit)
                ui_glow_rrect(c, bx0, by0, bx1, by1, r, 10.0f * u, UIC(255, 120, 40, 70));
            ui_fill_rrect(c, bx0, by0, bx1, by1, r, UIC(30, 18, 14, 220), UIC(10, 7, 6, 235), false);
            ui_fill_rrect(c, bx0 + 1.0f, by0 + 1.0f, bx1, by0 + (float)sh * 0.5f, r, UIC(255, 190, 130, 22), UIC(255, 190, 130, 0), false);
            ui_stroke_rrect(c, bx0, by0, bx1, by1, r, 1.0f, lit ? UIC(255, 214, 150, 150) : UIC(255, 214, 170, 44));
        }
        const float pad = 14.0f * u;
        const float tw = (float)w - bx0 - 2.0f * pad - inset;
        const float rw = ui_text_width(UiF_Num, 16.0f * u, 0.0f, right);
        const float lpx = ui_text_fit(UiF_Body, 12.0f * u, 0.0f, tw - rw - 8.0f * u, left);
        ui_text_box(c, UiF_Body, lpx, bx0 + pad, by0, tw, (float)sh, 0, lit ? UM_SOFT : UM_DIM, 0.0f, !flat, left);
        ui_text_box(c, UiF_Num, 16.0f * u, bx0 + pad, by0, tw, (float)sh, 2, UM_EMBER, 0.0f, !flat, right);
        um_host_painted(&sp_boss_host, t0);
    }
    um_host_show(&sp_boss_host, x0 + slid, y0);
}

/******************************************************************************/
/* The pointer's tooltip in the modern look: dark glass, the whole text (wrapped, not scrolled) */

static struct UmHost sp_tip_host;

TbBool world_ui_tooltip(const char *text, long pos_x, long pos_y)
{
    if (!world_side_hud_modern() || (text == NULL))
        return false;
    if (text[0] == '\0')
        return true;
    const float u = sp_hud_u();
    const TbBool flat = um_host_flat();
    /* the text as players read it: the game's control bytes left out */
    char clean[512];
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)text; (*s != '\0') && (n + 1 < sizeof(clean)); s++)
        if (*s >= 32)
            clean[n++] = (char)*s;
    clean[n] = '\0';
    /* lines of at most `wrap` wide, broken at spaces */
    const float px = 13.0f * u, pad = 10.0f * u, line_h = 18.0f * u;
    const float wrap = min(300.0f * u, (float)MyScreenWidth * 0.6f);
    char lines[6][256];
    int nl = 0;
    float widest = 0.0f;
    const char *p = clean;
    while ((*p != '\0') && (nl < 6))
    {
        while (*p == ' ')
            p++;
        size_t best = 0, len = strlen(p);
        for (size_t k = 1; k <= len; k++)
        {
            if ((p[k] != ' ') && (p[k] != '\0'))
                continue;
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%.*s", (int)min(k, sizeof(tmp) - 1), p);
            if ((ui_text_width(UiF_Body, px, 0.0f, tmp) > wrap) && (best > 0))
                break;
            best = k;
        }
        if (best == 0)
            best = min(len, (size_t)255);
        snprintf(lines[nl], sizeof(lines[nl]), "%.*s", (int)min(best, (size_t)255), p);
        widest = max(widest, ui_text_width(UiF_Body, px, 0.0f, lines[nl]));
        nl++;
        p += best;
    }
    if (nl == 0)
        return true;
    const int tw = (int)ceilf(widest + 2.0f * pad), th = (int)ceilf((float)nl * line_h + 2.0f * pad - 4.0f * u);
    const int m = flat ? 0 : (int)ceilf(10.0f * u);
    /* where the game puts its box: up and to the right of the pointer, inside the safe area */
    int x = (int)pos_x + (int)(18.0f * u), y = (int)pos_y - th - (int)(18.0f * u);
    x = max(ui_safe_left + 4, min(x, ui_safe_right_edge() - tw - 4));
    y = max(ui_safe_top + 4, min(y, ui_safe_bottom_edge() - th - 4));
    char key[600];
    snprintf(key, sizeof(key), "%s|%.4f %d", clean, (double)u, (int)flat);
    sp_tip_host.name = "tooltip";
    sp_tip_host.slot = UmSl_Tooltip;
    if (um_host_begin(&sp_tip_host, tw + 2 * m, th + 2 * m, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &sp_tip_host.canvas;
        const float x0 = (float)m, y0 = (float)m, x1 = (float)(m + tw), y1 = (float)(m + th);
        if (flat)
        {
            ui_fill_rrect(c, x0, y0, x1, y1, 0.0f, UIC(16, 11, 9, 255), UIC(16, 11, 9, 255), false);
            ui_stroke_rrect(c, x0, y0, x1, y1, 0.0f, 1.0f, UM_GOLD);
        } else
        {
            const float r = 9.0f * u;
            ui_shadow_rrect(c, x0, y0, x1, y1, r, (float)m, 3.0f * u, UIC(0, 0, 0, 160));
            ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(36, 22, 16, 246), UIC(16, 10, 8, 250), false);
            ui_stroke_rrect(c, x0, y0, x1, y1, r, 1.0f, UIC(222, 178, 88, 120));
        }
        for (int i = 0; i < nl; i++)
            ui_text_box(c, UiF_Body, px, x0 + pad, y0 + pad - 2.0f * u + (float)i * line_h, (float)tw - 2.0f * pad, line_h, 0, UM_BRIGHT,
                0.0f, !flat, lines[i]);
        um_host_painted(&sp_tip_host, t0);
    }
    um_host_show(&sp_tip_host, x - m, y - m);
    return true;
}

/******************************************************************************/
/* The game's own menus over the view (Options and what it opens: Quit, Graphics, Sound, the assistant, Load, Save) in
   the modern look: dark glass for their ornate frame, their buttons painted where the game put them (titles and text
   bars as headings and buttons, icon buttons as tiles with their icon cut off its pedestal, sliders as bars). The
   buttons' clicks, tooltips and upkeep are the game's. */

void gui_area_text(struct GuiButton *gbtn);
void gui_area_slider(struct GuiButton *gbtn);
void gui_area_normal_button(struct GuiButton *gbtn);
void draw_load_button(struct GuiButton *gbtn);

/** The middle of an icon button's tile (screen y): the row of the slider beside it, when one lies across its area (the
 *  nearest), else its area's middle. */
static float sp_icon_row(const struct GuiButton *b, const float *slider_cy, int n_sliders)
{
    float best = (float)b->scr_pos_y + (float)b->height * 0.5f, dist = 1e9f;
    for (int k = 0; k < n_sliders; k++)
    {
        if ((slider_cy[k] < (float)b->scr_pos_y) || (slider_cy[k] > (float)(b->scr_pos_y + b->height)))
            continue;
        const float d = fabsf(slider_cy[k] - ((float)b->scr_pos_y + (float)b->height * 0.5f));
        if (d < dist)
        {
            dist = d;
            best = slider_cy[k];
        }
    }
    return best;
}

/** A volume slider's name (the game's own words for it), or NULL. */
static const char *sp_slider_name(const struct GuiButton *b)
{
    const int str = (b->id_num == BID_SOUND_VOL) ? GUIStr_OptionSoundFx : (b->id_num == BID_MUSIC_VOL) ? GUIStr_OptionMusic :
        (b->id_num == BID_MENTOR_VOL) ? GUIStr_OptionVoice : 0;
    const char *t = (str != 0) ? get_string(str) : NULL;
    return ((t != NULL) && (t[0] != '\0')) ? t : NULL;
}

enum SpMenuKind { SpM_None = 0, SpM_Title, SpM_Label, SpM_Bar, SpM_Slider, SpM_IconBtn, SpM_IconPanel, SpM_LoadRow, SpM_Scroll };

static int sp_menu_kind(const struct GuiButton *gbtn, TbBool first);

/** Every button of the menu is of a kind painted here (else the menu keeps the game's look: its picture would hide the
 *  game's drawing of the others). */
static TbBool sp_menu_known(const struct GuiMenu *gmnu)
{
    TbBool first = true;
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
    {
        const struct GuiButton *b = &active_buttons[i];
        if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
            continue;
        const int kind = sp_menu_kind(b, first);
        first = false;
        if ((kind == 0) && (b->draw_call != NULL) && (b->draw_call != gui_area_new_null_button))
            return false;
    }
    return true;
}

static TbBool sp_menu_modern(const struct GuiMenu *gmnu)
{
    return world_side_hud_modern() && (gmnu != NULL) && (gmnu->draw_cb == gui_pretty_background) && sp_menu_known(gmnu);
}

/** How a menu's button is painted (SpM_None: the game draws it). `first`: the menu's first button. */
static int sp_menu_kind(const struct GuiButton *gbtn, TbBool first)
{
    if (gbtn->draw_call == gui_area_text)
    {
        if (first && (gbtn->click_event == NULL))
            return SpM_Title;
        return (gbtn->sprite_idx == 0) ? SpM_Label : SpM_Bar;
    }
    if (gbtn->draw_call == gui_area_slider)
        return SpM_Slider;
    if (gbtn->draw_call == draw_load_button)
        return SpM_LoadRow;
    if (gbtn->draw_call == gui_vscroll_draw)
        return SpM_Scroll;
    if ((gbtn->draw_call == gui_area_no_anim_button) || (gbtn->draw_call == gui_area_normal_button))
        return SpM_IconBtn;
    if ((gbtn->draw_call == gui_area_new_vertical_button) || (gbtn->draw_call == gui_area_compsetting_button) ||
        (gbtn->draw_call == gui_area_new_normal_button))
        return SpM_IconPanel;
    return SpM_None;
}

static TbBool sp_menu_first(const struct GuiButton *gbtn)
{
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
    {
        const struct GuiButton *b = &active_buttons[i];
        if (((b->flags & LbBtnF_Active) != 0) && (b->gmenu_idx == gbtn->gmenu_idx))
            return (b == gbtn);
    }
    return false;
}

TbBool world_ui_menu_paints(const struct GuiButton *gbtn)
{
    if ((gbtn == NULL) || (gbtn->gmenu_idx < 0))
        return false;
    const struct GuiMenu *gmnu = get_active_menu(gbtn->gmenu_idx);
    if (world_side_hud_modern() && (gmnu != NULL) && (gmnu->ident == GMnu_TEXT_INFO))
        return true; /* (the message box paints all its buttons: world_ui_textbox_draw) */
    if (!sp_menu_modern(gmnu))
        return false;
    return sp_menu_kind(gbtn, sp_menu_first(gbtn)) != SpM_None;
}

/* the options' icons stand on one pedestal (the same pixels in all of them): found once by comparing them */
static unsigned char *sp_pedestal;      /* where it is: 1 */
static unsigned char *sp_pedestal_rgb;  /* its colour there */
static int sp_ped_w, sp_ped_h;

static unsigned char *sp_bsprite(long idx, int *w, int *h)
{
    const struct TbSprite *spr = get_button_sprite((short)idx);
    if ((spr == NULL) || (spr->SWidth == 0) || (spr->SHeight == 0) || (engine_palette == NULL))
        return NULL;
    unsigned char *px = (unsigned char *)calloc((size_t)spr->SWidth * spr->SHeight, 4);
    if (px == NULL)
        return NULL;
    ui_sprite_rgba(spr, engine_palette, px);
    *w = spr->SWidth;
    *h = spr->SHeight;
    return px;
}

static void sp_pedestal_find(void)
{
    static TbBool done;
    if (done)
        return;
    done = true;
    static const long set[5] = {GBS_options_button_save, GBS_options_button_load, GBS_options_button_sound,
        GBS_options_button_graphc, GBS_options_button_exit};
    unsigned char *px[5];
    int w = 0, h = 0, ok = 1;
    for (int k = 0; k < 5; k++)
    {
        int pw = 0, ph = 0;
        px[k] = sp_bsprite(set[k], &pw, &ph);
        if ((px[k] == NULL) || ((k > 0) && ((pw != w) || (ph != h))))
            ok = 0;
        w = pw;
        h = ph;
    }
    if (ok && (w > 0) && (h > 0))
    {
        sp_pedestal = (unsigned char *)calloc((size_t)w * h, 1);
        sp_pedestal_rgb = (unsigned char *)calloc((size_t)w * h, 3);
        if ((sp_pedestal != NULL) && (sp_pedestal_rgb != NULL))
        {
            sp_ped_w = w;
            sp_ped_h = h;
            for (int i = 0; i < w * h; i++)
            {
                /* the colour most of the five have here; the pedestal where four or five agree */
                for (int a = 0; a < 5; a++)
                {
                    const unsigned char *p = px[a] + (size_t)i * 4;
                    if (p[3] == 0)
                        continue;
                    int same = 0;
                    for (int b = 0; b < 5; b++)
                    {
                        const unsigned char *q = px[b] + (size_t)i * 4;
                        same += (q[3] != 0) && (q[0] == p[0]) && (q[1] == p[1]) && (q[2] == p[2]);
                    }
                    if (same >= 4)
                    {
                        sp_pedestal[i] = 1;
                        memcpy(sp_pedestal_rgb + (size_t)i * 3, p, 3);
                    }
                    break;
                }
            }
        }
    }
    for (int k = 0; k < 5; k++)
        free(px[k]);
}

/** A menu button's icon without its pedestal (a button sprite: `button`, else a panel sprite), cropped. */
static const unsigned char *sp_menu_icon(long spr, TbBool button, int *w, int *h)
{
    #define SP_MI 1024
    static unsigned char *px_of[2][SP_MI];
    static short w_of[2][SP_MI], h_of[2][SP_MI];
    static TbBool tried[2][SP_MI];
    const int b = button ? 1 : 0;
    if ((spr <= 0) || (spr >= SP_MI))
        return NULL;
    if (!tried[b][spr])
    {
        tried[b][spr] = true;
        int ow = 0, oh = 0;
        unsigned char *ov = world_ui_icon_override(button ? 5 : 4, spr, &ow, &oh);
        if (ov != NULL)
        {
            px_of[b][spr] = ov; w_of[b][spr] = (short)ow; h_of[b][spr] = (short)oh;
            *w = ow; *h = oh;
            return ov;
        }
        sp_pedestal_find();
        int pw = 0, ph = 0;
        unsigned char *px = button ? sp_bsprite(spr, &pw, &ph) : sp_sprite(spr, &pw, &ph);
        unsigned char *m = (px != NULL) ? (unsigned char *)calloc((size_t)pw * ph, 1) : NULL;
        if (m != NULL)
        {
            const TbBool ped = (sp_pedestal != NULL) && (pw == sp_ped_w) && (ph == sp_ped_h);
            for (int i = 0; i < pw * ph; i++)
            {
                const unsigned char *p = px + (size_t)i * 4;
                m[i] = (p[3] != 0);
                if (m[i] && ped && sp_pedestal[i] && (memcmp(p, sp_pedestal_rgb + (size_t)i * 3, 3) == 0))
                    m[i] = 0;
                if (m[i] && sp_shadow(p)) /* (the drop shadow: the icon stands in the middle) */
                    m[i] = 0;
            }
            sp_keep_groups(m, pw, ph, 4);
            int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    if (!m[y * pw + x])
                    {
                        px[(y * pw + x) * 4 + 3] = 0;
                        continue;
                    }
                    x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y);
                }
            if (x1 >= x0)
            {
                const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
                unsigned char *o = (unsigned char *)malloc((size_t)cw * ch * 4);
                if (o != NULL)
                {
                    for (int y = 0; y < ch; y++)
                        memcpy(o + (size_t)y * cw * 4, px + ((size_t)(y + y0) * pw + x0) * 4, (size_t)cw * 4);
                    px_of[b][spr] = o;
                    w_of[b][spr] = (short)cw;
                    h_of[b][spr] = (short)ch;
                }
            }
        }
        free(m);
        free(px);
    }
    *w = w_of[b][spr];
    *h = h_of[b][spr];
    return px_of[b][spr];
    #undef SP_MI
}

static void sp_menu_text(const struct GuiButton *gbtn, char *out, size_t n)
{
    out[0] = '\0';
    if ((gbtn->tooltip_stridx != GUIStr_Empty) && (gbtn->tooltip_stridx != -GUIStr_Empty))
        snprintf(out, n, "%s", get_string((gbtn->tooltip_stridx > 0) ? gbtn->tooltip_stridx : -gbtn->tooltip_stridx));
    else if (gbtn->content.str != NULL)
        snprintf(out, n, "%s", gbtn->content.str);
    /* (the game's control bytes left out) */
    size_t o = 0;
    for (size_t i = 0; out[i] != '\0'; i++)
        if ((unsigned char)out[i] >= 32)
            out[o++] = out[i];
    out[o] = '\0';
}

static struct UmHost sp_menu_host[2];
static int sp_menu_ident[2] = {-1, -1};

/* a menu dragged by its title strip, as the Keeper panel and the Library's window (the user, 2026-11-26) */
static int sp_drag_ident = -1;
static int sp_drag_mx, sp_drag_my;
static TbBool sp_drag_was_down;

/** Moves a menu and every button of it by (dx, dy), its body kept on the screen. */
static void sp_menu_move(struct GuiMenu *gmnu, int dx, int dy)
{
    dx = max(dx, -gmnu->pos_x);
    dx = min(dx, (int)MyScreenWidth - (gmnu->pos_x + gmnu->width));
    dy = max(dy, -gmnu->pos_y);
    dy = min(dy, (int)MyScreenHeight - (gmnu->pos_y + gmnu->height));
    if ((dx == 0) && (dy == 0))
        return;
    gmnu->pos_x += dx;
    gmnu->pos_y += dy;
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
    {
        struct GuiButton *b = &active_buttons[i];
        if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
            continue;
        b->scr_pos_x += dx;
        b->scr_pos_y += dy;
        b->pos_x += dx;
        b->pos_y += dy;
    }
}

static void sp_menu_drag(struct GuiMenu *gmnu)
{
    const TbBool down = (lbDisplay.MLeftButton != 0);
    const int mx = GetMouseX(), my = GetMouseY();
    if (down && !sp_drag_was_down)
    {
        /* the strip above the first button: the title (a button there, as Quit's, keeps its click) */
        int strip = gmnu->pos_y + (int)(56.0f * sp_hud_u());
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            const struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number) || (b->click_event == NULL))
                continue;
            strip = min(strip, (int)b->scr_pos_y);
        }
        if ((mx >= gmnu->pos_x) && (mx < gmnu->pos_x + gmnu->width) && (my >= gmnu->pos_y) && (my < strip))
        {
            sp_drag_ident = gmnu->ident;
            sp_drag_mx = mx;
            sp_drag_my = my;
        }
    }
    if (down && (sp_drag_ident == gmnu->ident))
    {
        sp_menu_move(gmnu, mx - sp_drag_mx, my - sp_drag_my);
        sp_drag_mx = mx;
        sp_drag_my = my;
    }
    if (!down)
        sp_drag_ident = -1;
    sp_drag_was_down = down;
}

TbBool world_ui_menu_draw(struct GuiMenu *gmnu)
{
    if (!sp_menu_modern(gmnu))
        return false;
    sp_menu_drag(gmnu);
    /* no growing frame: the menu is there at once; while it closes nothing is drawn */
    if (gmnu->visual_state == 1)
    {
        gmnu->fade_time = 1;
        gmnu->visual_state = 2;
    }
    if (gmnu->visual_state != 2)
        return true;
    /* the buttons the game lays across the title (Quit's No and Yes) go under it */
    {
        int title_bottom = -1;
        TbBool first_b = true;
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
                continue;
            const int kind = sp_menu_kind(b, first_b);
            first_b = false;
            if (kind == SpM_Title)
            {
                title_bottom = b->scr_pos_y + b->height + (int)(10.0f * sp_hud_u());
                continue;
            }
            if ((title_bottom >= 0) && (b->scr_pos_y < title_bottom) && (title_bottom + b->height <= gmnu->pos_y + gmnu->height))
            {
                b->scr_pos_y = (short)title_bottom;
                b->pos_y = b->scr_pos_y;
            }
        }
    }
    int hi = (sp_menu_ident[0] == gmnu->ident) ? 0 : (sp_menu_ident[1] == gmnu->ident) ? 1 : -1;
    if (hi < 0)
    {
        hi = (sp_menu_ident[0] < 0) ? 0 : (sp_menu_ident[1] < 0) ? 1 : ((gmnu->ident & 1) ? 1 : 0);
        sp_menu_ident[hi] = gmnu->ident;
    }
    struct UmHost *h = &sp_menu_host[hi];
    h->name = (hi == 0) ? "menu" : "menu2";
    h->slot = (hi == 0) ? UmSl_Menu : UmSl_Menu2;
    const float u = sp_hud_u();
    const TbBool flat = um_host_flat();
    const struct UmStyle stv = {u, flat};
    const int m = flat ? 0 : (int)ceilf(um_window_margin(&stv) * 0.7f);
    const int cw = gmnu->width + 2 * m, ch = gmnu->height + 2 * m;
    /* the state: each button's kind, place, hover, press, value */
    char key[1024];
    int kn = snprintf(key, sizeof(key), "%d %d %d %.4f %d|", gmnu->ident, gmnu->width, gmnu->height, (double)u, (int)flat);
    TbBool first = true;
    for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (kn < (int)sizeof(key) - 48); i++)
    {
        const struct GuiButton *b = &active_buttons[i];
        if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
            continue;
        const int kind = sp_menu_kind(b, first);
        first = false;
        if (kind == SpM_None)
            continue;
        int spr = b->sprite_idx;
        if ((b->gbtype == LbBtnT_ToggleBtn) && (b->content.ptr != NULL))
            spr += *(const unsigned char *)b->content.ptr;
        char text[160];
        sp_menu_text(b, text, sizeof(text));
        kn += snprintf(key + kn, sizeof(key) - (size_t)kn, "%d,%d,%d,%d,%d,%d,%d,%x,%d;", kind, b->scr_pos_x - gmnu->pos_x, b->scr_pos_y - gmnu->pos_y,
            spr, (int)b->flags, (int)b->button_state_left_pressed, (int)b->slide_val, um_hash("%s", text), (int)(b == input_button));
        if (kind == SpM_Scroll)
            kn += snprintf(key + kn, sizeof(key) - (size_t)kn, "s%ld,%ld;", (long)gui_vscroll_offset, (long)gui_vscroll_max_offset());
    }
    if (um_host_begin(h, cw, ch, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &h->canvas;
        um_window(c, &stv, (float)m, (float)m, (float)(m + gmnu->width), (float)(m + gmnu->height));
        /* one tile size for every icon of the menu, the smallest button's: the game's button areas differ a little
           (Load Game's is narrower than Save Game's), which made one icon smaller than its neighbour (the user, 2026-11-26) */
        float icon_side = 0.0f;
        int icon_big = 0; /* the largest icon's side: every icon of the menu at the scale that fits it (the Quit icon, a
                             few pixels shorter, was enlarged above the rest: the user, 2026-11-26) */
        float icon_cx[24], icon_cy[24];
        int n_icons = 0;
        /* the sliders' rows: an icon beside one stands on its row (the Sound menu's areas are taller than their rows, so
           their middles missed the sliders: the user, 2026-11-26) */
        float slider_cy[16];
        int n_sliders = 0;
        first = true;
        for (int i = 0; (i < ACTIVE_BUTTONS_COUNT) && (n_sliders < 16); i++)
        {
            const struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
                continue;
            const int kind = sp_menu_kind(b, first);
            first = false;
            if ((kind == SpM_Slider) && ((b->flags & LbBtnF_Visible) != 0))
                slider_cy[n_sliders++] = (float)b->scr_pos_y + (float)b->height * 0.5f;
        }
        first = true;
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            const struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
                continue;
            const int kind = sp_menu_kind(b, first);
            first = false;
            if (((kind == SpM_IconBtn) || (kind == SpM_IconPanel)) && ((b->flags & LbBtnF_Visible) != 0))
            {
                int spr = b->sprite_idx;
                if ((b->gbtype == LbBtnT_ToggleBtn) && (b->content.ptr != NULL))
                    spr += *(const unsigned char *)b->content.ptr;
                int iw = 0, ih = 0;
                if (sp_menu_icon(spr, kind == SpM_IconBtn, &iw, &ih) != NULL)
                    icon_big = max(icon_big, max(iw, ih));
                const float sd = (float)min(b->width, b->height) * 0.96f;
                icon_side = (icon_side <= 0.0f) ? sd : min(icon_side, sd);
                if (n_icons < (int)(sizeof(icon_cx) / sizeof(icon_cx[0])))
                {
                    icon_cx[n_icons] = (float)b->scr_pos_x + (float)b->width * 0.5f;
                    icon_cy[n_icons] = sp_icon_row(b, slider_cy, n_sliders);
                    n_icons++;
                }
            }
        }
        /* ... and no larger than the room between two icons: the game's areas overlap in some menus (the Sound menu's
           volume icons stand one over the other), and square tiles round them did too (the user, 2026-11-26) */
        for (int a = 0; a < n_icons; a++)
            for (int k = a + 1; k < n_icons; k++)
            {
                const float gap = max(fabsf(icon_cx[a] - icon_cx[k]), fabsf(icon_cy[a] - icon_cy[k])) - 6.0f * u;
                if (gap > 8.0f)
                    icon_side = min(icon_side, gap);
            }
        first = true;
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) == 0) || (b->gmenu_idx != gmnu->number))
                continue;
            const int kind = sp_menu_kind(b, first);
            first = false;
            if ((kind == SpM_None) || ((b->flags & LbBtnF_Visible) == 0))
                continue;
            const float x0 = (float)(b->scr_pos_x - gmnu->pos_x + m), y0 = (float)(b->scr_pos_y - gmnu->pos_y + m);
            const float x1 = x0 + (float)b->width, y1 = y0 + (float)b->height;
            const TbBool enabled = ((b->flags & LbBtnF_Enabled) != 0);
            const TbBool hot = ((b->flags & LbBtnF_MouseOver) != 0) && enabled;
            const TbBool down = (b->button_state_left_pressed != 0) || (b->button_state_right_pressed != 0);
            char text[160];
            switch (kind)
            {
            case SpM_Title:
                sp_menu_text(b, text, sizeof(text));
                ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, 22.0f * u, 0.6f * u, (float)gmnu->width - 40.0f * u, text), (float)m,
                    y0, (float)gmnu->width, y1 - y0, 1, UM_BRIGHT, 0.6f * u, !flat, text);
                um_rule(c, &stv, (float)m + 24.0f * u, y1 + 2.0f * u, (float)(m + gmnu->width) - 24.0f * u);
                break;
            case SpM_Label:
                if (enabled)
                {
                    sp_menu_text(b, text, sizeof(text));
                    ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, 13.0f * u, 0.0f, x1 - x0, text), x0, y0, x1 - x0, y1 - y0, 1, UM_TEXT,
                        0.0f, !flat, text);
                }
                break;
            case SpM_LoadRow:
            case SpM_Bar:
                if (enabled || (b->sprite_idx == 2) || (kind == SpM_LoadRow))
                {
                    sp_menu_text(b, text, sizeof(text));
                    if (b == input_button) /* (a name being typed: its caret) */
                        snprintf(text + strlen(text), sizeof(text) - strlen(text), "_");
                    if (b->click_event != NULL)
                        um_button(c, &stv, x0, y0, x1, y1, UmB_Normal, down ? UmS_Down : (hot ? UmS_Hot : UmS_Normal), 14.0f * u, text);
                    else
                    {
                        um_card(c, &stv, x0, y0, x1, y1);
                        ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, 13.0f * u, 0.0f, x1 - x0 - 12.0f * u, text), x0, y0, x1 - x0,
                            y1 - y0, 1, UM_TEXT, 0.0f, !flat, text);
                    }
                }
                break;
            case SpM_Scroll:
            {
                /* a thin track with its thumb where the list is; the arrows' places at its ends */
                const long max_off = gui_vscroll_max_offset();
                const float cx = (x0 + x1) * 0.5f, tw = max(3.0f, 6.0f * u), ar = min((x1 - x0) * 0.5f, 12.0f * u);
                const float ty0 = y0 + 2.0f * ar + 4.0f * u, ty1 = y1 - 2.0f * ar - 4.0f * u;
                sp_round(c, &stv, cx, y0 + ar, ar, false, false);
                sp_shape(c, sp_icon_up, sp_icon_chev_n, 1, cx, y0 + ar, ar * 1.1f, (gui_vscroll_offset > 0) ? UM_GOLD : UIC(110, 92, 64, 255));
                sp_round(c, &stv, cx, y1 - ar, ar, false, false);
                sp_shape(c, sp_icon_down, sp_icon_chev_n, 1, cx, y1 - ar, ar * 1.1f,
                    (gui_vscroll_offset < max_off) ? UM_GOLD : UIC(110, 92, 64, 255));
                if (ty1 > ty0)
                {
                    ui_fill_rrect(c, cx - tw * 0.5f, ty0, cx + tw * 0.5f, ty1, tw * 0.5f, UIC(255, 236, 214, 24), UIC(255, 236, 214, 24), false);
                    const float th = max(tw * 3.0f, (ty1 - ty0) * 0.25f);
                    const float f = (max_off > 0) ? (float)gui_vscroll_offset / (float)max_off : 0.0f;
                    const float tt = ty0 + (ty1 - ty0 - th) * f;
                    ui_fill_rrect(c, cx - tw * 0.5f, tt, cx + tw * 0.5f, tt + th, tw * 0.5f, UM_GOLD_HI, UM_GOLD, false);
                }
                break;
            }
            case SpM_Slider:
                if (enabled)
                {
                    const float cy = (y0 + y1) * 0.5f, th = max(2.0f, 5.0f * u), kr = 8.0f * u;
                    const float bx0 = x0 + kr, bx1 = x1 - kr;
                    /* its name above it (the icons alone do not say which is which: the user, 2026-11-26) */
                    const char *name = sp_slider_name(b);
                    if (name != NULL)
                        ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, 14.0f * u, 0.0f, bx1 - bx0, name), bx0, cy - kr - 22.0f * u, bx1 - bx0,
                            18.0f * u, 0, hot ? UM_GOLD_HI : UM_SOFT, 0.0f, !flat, name);
                    const float kx = bx0 + (bx1 - bx0) * (float)b->slide_val / 255.0f;
                    ui_fill_rrect(c, bx0, cy - th * 0.5f, bx1, cy + th * 0.5f, th * 0.5f, UIC(255, 236, 214, 30), UIC(255, 236, 214, 30), false);
                    ui_fill_rrect(c, bx0, cy - th * 0.5f, kx, cy + th * 0.5f, th * 0.5f, UIC(176, 34, 16, 255), UIC(255, 176, 80, 255), true);
                    if (!flat)
                        ui_glow_rrect(c, kx - kr, cy - kr, kx + kr, cy + kr, kr, 8.0f * u, UIC(255, 120, 40, hot ? 110 : 60));
                    ui_fill_rrect(c, kx - kr, cy - kr, kx + kr, cy + kr, kr, UM_GOLD_HI, UM_GOLD, false);
                }
                break;
            case SpM_IconBtn:
            case SpM_IconPanel:
            {
                int spr = b->sprite_idx;
                if ((b->gbtype == LbBtnT_ToggleBtn) && (b->content.ptr != NULL))
                    spr += *(const unsigned char *)b->content.ptr;
                /* a square tile round the icon (the game's button areas are taller, and overlap in some menus) */
                const float side = (icon_side > 0.0f) ? icon_side : min(x1 - x0, y1 - y0) * 0.96f, mx = (x0 + x1) * 0.5f;
                const float my = sp_icon_row(b, slider_cy, n_sliders) - (float)gmnu->pos_y + (float)m;
                const float tx0 = mx - side * 0.5f, ty0 = my - side * 0.5f, tx1 = mx + side * 0.5f, ty1 = my + side * 0.5f;
                const float r = 12.0f * u;
                if (flat)
                {
                    ui_fill_rrect(c, tx0, ty0, tx1, ty1, 0.0f, UIC(30, 21, 17, 255), UIC(30, 21, 17, 255), false);
                    ui_stroke_rrect(c, tx0, ty0, tx1, ty1, 0.0f, 1.0f, hot ? UM_GOLD_HI : UIC(104, 78, 36, 255));
                } else
                {
                    if (hot)
                        ui_glow_rrect(c, tx0, ty0, tx1, ty1, r, 12.0f * u, UIC(255, 120, 40, 80));
                    ui_fill_rrect(c, tx0, ty0, tx1, ty1, r, down ? UIC(0, 0, 0, 120) : UIC(255, 226, 196, hot ? 26 : 14),
                        down ? UIC(0, 0, 0, 90) : UIC(255, 226, 196, hot ? 10 : 4), false);
                    ui_stroke_rrect(c, tx0, ty0, tx1, ty1, r, 1.0f, hot ? UIC(255, 214, 150, 150) : UIC(255, 214, 170, 50));
                }
                int iw = 0, ih = 0;
                const unsigned char *pic = sp_menu_icon(spr, kind == SpM_IconBtn, &iw, &ih);
                if ((pic != NULL) && (icon_big > 0))
                {
                    const float fit_f = (tx1 - tx0) * 0.78f / (float)icon_big;
                    const int kk = (int)floorf(fit_f);
                    const float fit = ((kk >= 1) && ((float)kk >= fit_f * 0.75f)) ? (float)kk : fit_f;
                    const int dw = max(1, (int)((float)iw * fit)), dh = max(1, (int)((float)ih * fit));
                    ui_image_gain(c, (int)floorf((tx0 + tx1 - (float)dw) * 0.5f + 0.5f), (int)floorf((ty0 + ty1 - (float)dh) * 0.5f + 0.5f), dw, dh,
                        pic, iw, ih, !enabled ? 0.45f : (down ? 0.75f : (hot ? 1.12f : 1.0f)));
                } else
                    sp_picture(c, pic, iw, ih, tx0, ty0, tx1, ty1, 0.78f, !enabled ? 0.45f : (down ? 0.75f : (hot ? 1.12f : 1.0f)));
                break;
            }
            default:
                break;
            }
        }
        um_host_painted(h, t0);
    }
    um_host_show(h, gmnu->pos_x - m, gmnu->pos_y - m);
    return true;
}

/******************************************************************************/
/* The main menu in the modern look: its buttons (frontend_draw_button) gathered each frame and painted together on a
   glass panel, the title as a heading, the version in the plain face. Only the Main Menu screen for now. */

#define SP_FRONT_MAX 16
struct SpFrontBtn {
    int x, y, w, h;
    char text[96];
    TbBool title, enabled, hot, down;
};
static struct SpFrontBtn sp_front[SP_FRONT_MAX];
static int sp_front_n;
static char sp_front_ver[2][128];
static int sp_front_ver_y, sp_front_ver_h;
static TbBool sp_front_has_ver;
static struct UmHost sp_front_host, sp_front_ver_host;

static TbBool sp_front_modern(void)
{
    return (window_style != WinStyle_Classic) && (frontend_menu_state == FeSt_MAIN_MENU);
}

TbBool world_ui_front_button(const struct GuiButton *gbtn, const char *text)
{
    if (!sp_front_modern() || (gbtn == NULL))
        return false;
    if (sp_front_n >= SP_FRONT_MAX)
        return true;
    struct SpFrontBtn *b = &sp_front[sp_front_n++];
    b->x = gbtn->scr_pos_x;
    b->y = gbtn->scr_pos_y;
    b->w = gbtn->width;
    b->h = gbtn->height;
    snprintf(b->text, sizeof(b->text), "%s", (text != NULL) ? text : "");
    b->title = (gbtn->id_num == BID_MENU_TITLE);
    b->enabled = ((gbtn->flags & LbBtnF_Enabled) != 0);
    b->hot = b->enabled && (gbtn->content.lval > 0) && (frontend_mouse_over_button == gbtn->content.lval);
    b->down = b->hot && ((gbtn->button_state_left_pressed != 0) || (gbtn->button_state_right_pressed != 0));
    return true;
}

TbBool world_ui_front_version(int y, int h, const char *line1, const char *line2)
{
    if (!sp_front_modern())
        return false;
    snprintf(sp_front_ver[0], sizeof(sp_front_ver[0]), "%s", line1);
    snprintf(sp_front_ver[1], sizeof(sp_front_ver[1]), "%s", line2);
    sp_front_ver_y = y;
    sp_front_ver_h = h;
    sp_front_has_ver = true;
    return true;
}

void world_ui_front_flush(void)
{
    const int n = sp_front_n;
    sp_front_n = 0;
    const TbBool ver = sp_front_has_ver;
    sp_front_has_ver = false;
    if (!sp_front_modern() || ((n == 0) && !ver))
        return;
    const TbBool flat = um_host_flat();
    /* the scale: a button's height is 46 of the game's menu units; a panel unit is 2/3 of one (as the side panel) */
    const float u = (n > 0) ? (float)sp_front[0].h / 46.0f * (2.0f / 3.0f) : 2.0f;
    const struct UmStyle stv = {u, flat};
    if (n > 0)
    {
        int bx0 = sp_front[0].x, by0 = sp_front[0].y, bx1 = bx0 + sp_front[0].w, by1 = by0 + sp_front[0].h;
        for (int i = 1; i < n; i++)
        {
            bx0 = min(bx0, sp_front[i].x); by0 = min(by0, sp_front[i].y);
            bx1 = max(bx1, sp_front[i].x + sp_front[i].w); by1 = max(by1, sp_front[i].y + sp_front[i].h);
        }
        /* (the padding round the buttons, less where the screen's edge is nearer) */
        const int m = flat ? 0 : (int)ceilf(um_window_margin(&stv) * 0.6f);
        const int pad = max(2, min((int)ceilf(22.0f * u), min(by0, (int)MyScreenHeight - by1) - 2));
        const int x0 = bx0 - pad - m, y0 = by0 - pad - m;
        const int cw = (bx1 - bx0) + 2 * (pad + m), ch = (by1 - by0) + 2 * (pad + m);
        char key[1600];
        int kn = snprintf(key, sizeof(key), "%d %d %d %d %.4f %d|", x0, y0, cw, ch, (double)u, (int)flat);
        for (int i = 0; (i < n) && (kn < (int)sizeof(key) - 140); i++)
            kn += snprintf(key + kn, sizeof(key) - (size_t)kn, "%d,%d,%d,%d,%s,%d%d%d%d;", sp_front[i].x, sp_front[i].y, sp_front[i].w,
                sp_front[i].h, sp_front[i].text, (int)sp_front[i].title, (int)sp_front[i].enabled, (int)sp_front[i].hot, (int)sp_front[i].down);
        sp_front_host.name = "front";
        sp_front_host.slot = UmSl_Front;
        if (um_host_begin(&sp_front_host, cw, ch, key))
        {
            const unsigned long t0 = (unsigned long)LbTimerClock();
            struct UiCanvas *c = &sp_front_host.canvas;
            um_window(c, &stv, (float)m, (float)m, (float)(cw - m), (float)(ch - m));
            for (int i = 0; i < n; i++)
            {
                const struct SpFrontBtn *b = &sp_front[i];
                const float fx0 = (float)(b->x - x0), fy0 = (float)(b->y - y0), fx1 = fx0 + (float)b->w, fy1 = fy0 + (float)b->h;
                if (b->title)
                {
                    ui_text_box(c, UiF_Head, ui_text_fit(UiF_Head, (float)b->h * 0.62f, 0.6f * u, fx1 - fx0, b->text), fx0, fy0, fx1 - fx0,
                        fy1 - fy0, 1, UM_BRIGHT, 0.6f * u, !flat, b->text);
                    um_rule(c, &stv, fx0 + 10.0f * u, fy1 + 2.0f * u, fx1 - 10.0f * u);
                    continue;
                }
                const float inset = (float)b->h * 0.08f;
                const int state = !b->enabled ? UmS_Off : (b->down ? UmS_Down : (b->hot ? UmS_Hot : UmS_Normal));
                /* (every button alike: the user, 2026-11-26, "dont make the top button this color. Make it the same as the rest") */
                um_button(c, &stv, fx0, fy0 + inset, fx1, fy1 - inset, UmB_Normal, state, (float)b->h * 0.42f, b->text);
            }
            um_host_painted(&sp_front_host, t0);
        }
        um_host_show(&sp_front_host, x0, y0);
    }
    if (ver)
    {
        const float px = max(10.0f, (float)sp_front_ver_h * 0.5f);
        const float w = max(ui_text_width(UiF_Body, px, 0.0f, sp_front_ver[0]), ui_text_width(UiF_Body, px, 0.0f, sp_front_ver[1]));
        const int cw = (int)ceilf(w) + 8, ch = (int)ceilf((float)sp_front_ver_h * 2.2f) + 4;
        char key[300];
        snprintf(key, sizeof(key), "%s|%s|%d %d", sp_front_ver[0], sp_front_ver[1], sp_front_ver_h, (int)flat);
        sp_front_ver_host.name = "front_version";
        sp_front_ver_host.slot = UmSl_Front2;
        if (um_host_begin(&sp_front_ver_host, cw, ch, key))
        {
            const unsigned long t0 = (unsigned long)LbTimerClock();
            ui_text_box(&sp_front_ver_host.canvas, UiF_Body, px, 4.0f, 0.0f, (float)cw, (float)sp_front_ver_h, 0, UM_SOFT, 0.0f, !flat,
                sp_front_ver[0]);
            ui_text_box(&sp_front_ver_host.canvas, UiF_Body, px * 0.9f, 4.0f, (float)sp_front_ver_h, (float)cw, (float)sp_front_ver_h, 0, UM_DIM,
                0.0f, !flat, sp_front_ver[1]);
            um_host_painted(&sp_front_ver_host, t0);
        }
        um_host_show(&sp_front_ver_host, ui_safe_left + (int)(6.0f * u), sp_front_ver_y - sp_front_ver_h);
    }
}

/******************************************************************************/
/* Test switch KFX_UI_ICONDUMP=<folder>: every icon the modern look uses, as it uses it (cut off its stone, shadow off,
   cropped) and the game's sprite it comes from, written once as <folder>/<category>__<name>.rgba (int32 width, int32
   height, int32 premultiplied 0/1, RGBA rows) for a script to turn into PNG folders (2026-11-26, the user wants to edit
   them). */

extern struct GuiButtonInit options_menu_buttons[], quit_menu_buttons[], autopilot_menu_buttons[], video_menu_buttons[],
    sound_menu_buttons[];

static int sp_dump_n;

static void sp_dump_write(const char *folder, const char *name, const unsigned char *rgba, int w, int h, int premul)
{
    if ((folder == NULL) || (rgba == NULL) || (w <= 0) || (h <= 0))
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.rgba", folder, name);
    for (char *q = path + strlen(folder) + 1; *q != '\0'; q++)
        if ((*q == '/') || (*q == '\\') || (*q == ':') || (*q == '*') || (*q == '?') || (*q == '"') || (*q == '<') || (*q == '>') || (*q == '|'))
            *q = '_';
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    const int32_t hdr[3] = {w, h, premul};
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(rgba, (size_t)w * (size_t)h * 4, 1, f);
    fclose(f);
    sp_dump_n++;
}

/** The game's own sprite (panel or button set) as it is. */
static void sp_dump_original(const char *folder, const char *name, long spr, TbBool button)
{
    if (folder == NULL)
        return;
    int w = 0, h = 0;
    unsigned char *px = button ? sp_bsprite(spr, &w, &h) : sp_sprite(spr, &w, &h);
    if (px == NULL)
        return;
    char n2[512];
    snprintf(n2, sizeof(n2), "Original game sprites__%s", name);
    sp_dump_write(folder, n2, px, w, h, 0);
    free(px);
}

/** An icon as used (`kind`: 0 um_icon, 1 framed button, 2 activity header, 3 event sign, 4 menu icon (panel), 5 menu
 *  icon (button set)) and its original. `name` is "<category>__<file name>". */
static void sp_ov_register(const char *name, long spr, int kind);

static void sp_dump_icon(const char *folder, const char *name, long spr, int kind)
{
    if (spr <= 0)
        return;
    sp_ov_register(name, spr, kind);
    if (folder == NULL)
        return; /* (only the names: the edited pictures' places) */
    int w = 0, h = 0;
    const unsigned char *px = NULL;
    switch (kind)
    {
    case 1: px = sp_framed_pic(spr, &w, &h); break;
    case 2: px = sp_inner_pic(spr, 5, &w, &h); break;
    case 3: px = sp_ev_pic(spr, &w, &h); break;
    case 4: px = sp_menu_icon(spr, false, &w, &h); break;
    case 5: px = sp_menu_icon(spr, true, &w, &h); break;
    default: px = um_icon(spr, &w, &h); break;
    }
    sp_dump_write(folder, name, px, w, h, 0);
    sp_dump_original(folder, name, spr, kind == 5);
}

static void sp_dump_vector(const char *folder, const char *name, int which)
{
    if (folder == NULL)
        return;
    struct UiCanvas c = {0};
    if (!ui_canvas_size(&c, 96, 96))
        return;
    switch (which)
    {
    case 0: sp_shape(&c, sp_icon_map, sp_icon_map_n, 2, 48.0f, 48.0f, 80.0f, UM_GOLD); break;
    case 1: sp_cog(&c, 48.0f, 48.0f, 80.0f, UM_GOLD); break;
    case 2: sp_shape(&c, sp_icon_minus, sp_icon_minus_n, 1, 48.0f, 48.0f, 80.0f, UM_GOLD); break;
    case 3: sp_shape(&c, sp_icon_plus, sp_icon_plus_n, 1, 48.0f, 48.0f, 80.0f, UM_GOLD); break;
    default: break;
    }
    sp_dump_write(folder, name, c.px, c.w, c.h, 1);
    ui_canvas_free(&c);
}

static void sp_dump_menu(const char *folder, const char *menu, const struct GuiButtonInit *btns)
{
    for (int i = 0; btns[i].gbtype != -1; i++)
    {
        const struct GuiButtonInit *b = &btns[i];
        int kind = -1;
        if ((b->draw_call == gui_area_no_anim_button) || (b->draw_call == gui_area_normal_button))
            kind = 5;
        else if ((b->draw_call == gui_area_new_vertical_button) || (b->draw_call == gui_area_compsetting_button) ||
            (b->draw_call == gui_area_new_normal_button))
            kind = 4;
        if (kind < 0)
            continue;
        const char *tip = (b->tooltip_stridx > 0) ? get_string(b->tooltip_stridx) : "";
        /* the tooltip's title (before its colon), else the icon's own name */
        char label[48];
        snprintf(label, sizeof(label), "%.40s", tip);
        for (char *q = label; *q != '\0'; q++)
        {
            if ((*q == ':') || (*q == '[') || (*q == '.'))
            {
                *q = '\0';
                break;
            }
            if ((unsigned char)*q < 32)
                *q = ' ';
        }
        if (label[0] == '\0')
            snprintf(label, sizeof(label), "%s", (b->sprite_idx == GBS_options_button_snd_music) ? "Music volume" :
                (b->sprite_idx == GBS_options_button_snd_sounds) ? "Sound volume" :
                (b->sprite_idx == GBS_optionsbutton_snd_voice) ? "Voice volume" : "Icon");
        const int states = (b->gbtype == LbBtnT_ToggleBtn) ? max(1, (int)b->maxval) : 1;
        for (int s = 0; s < states; s++)
        {
            char name[256];
            if (states > 1)
                snprintf(name, sizeof(name), "Menus %s__%02d %s (state %d)", menu, i, label, s + 1);
            else
                snprintf(name, sizeof(name), "Menus %s__%02d %s", menu, i, label);
            sp_dump_icon(folder, name, b->sprite_idx + s, kind);
        }
    }
}

static void sp_icon_dump(void)
{
    /* once: every icon's name registered (where its edited picture is looked for); with the test switch also written */
    static int done;
    if (done)
        return;
    done = 1;
    const char *folder = getenv("KFX_UI_ICONDUMP");
    if ((folder != NULL) && (folder[0] == '\0'))
        folder = NULL;
    char name[256];
    /* the side panel's tabs (chosen: bright; else dark) and the sell sign */
    static const char *const tabs[5] = {"Information", "Rooms", "Research", "Workshop", "Creatures"};
    for (int k = 0; k < 11; k++)
    {
        int w = 0, h = 0;
        if (k == 5)
            snprintf(name, sizeof(name), "Side panel tabs__Sell (rooms and workshop)");
        else
            snprintf(name, sizeof(name), "Side panel tabs__%s tab (%s)", tabs[(k < 5) ? k : k - 6], (k < 5) ? "chosen" : "not chosen");
        sp_ov_register(name, k, 10);
        if (folder == NULL)
            continue;
        const unsigned char *px = sp_cut(k, &w, &h);
        sp_dump_write(folder, name, px, w, h, 0);
        sp_dump_original(folder, name, (k == 5) ? GPS_rpanel_frame_portrt_sell : GPS_rpanel_rpanel_tab_infoa + 2 * ((k < 5) ? k : k - 6) + ((k < 5) ? 0 : 1),
            false);
    }
    /* rooms, spells, traps, doors: the grid's (small) and the strip's (big) pictures, and their "cannot afford" ones */
    for (int r = 1; r < game.conf.slab_conf.room_types_count; r++)
    {
        const struct RoomConfigStats *rs = get_room_kind_stats(r);
        const char *code = room_code_name(r);
        snprintf(name, sizeof(name), "Rooms__%s small", code); sp_dump_icon(folder, name, rs->medsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Rooms__%s small (cannot afford)", code); sp_dump_icon(folder, name, rs->medsym_sprite_idx + 1, 0);
        snprintf(name, sizeof(name), "Rooms__%s big", code); sp_dump_icon(folder, name, rs->bigsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Rooms__%s big (cannot afford)", code); sp_dump_icon(folder, name, rs->bigsym_sprite_idx + 1, 0);
    }
    for (int p = 1; p < game.conf.magic_conf.power_types_count; p++)
    {
        const struct PowerConfigStats *ps = get_power_model_stats(p);
        if (ps->medsym_sprite_idx <= 0)
            continue;
        snprintf(name, sizeof(name), "Spells__%s small", ps->code_name); sp_dump_icon(folder, name, ps->medsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Spells__%s small (cannot afford)", ps->code_name); sp_dump_icon(folder, name, ps->medsym_sprite_idx + 1, 0);
        snprintf(name, sizeof(name), "Spells__%s big", ps->code_name); sp_dump_icon(folder, name, ps->bigsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Spells__%s big (cannot afford)", ps->code_name); sp_dump_icon(folder, name, ps->bigsym_sprite_idx + 1, 0);
    }
    for (int t = 1; t < game.conf.trapdoor_conf.trap_types_count; t++)
    {
        const struct TrapConfigStats *ts = &game.conf.trapdoor_conf.trap_cfgstats[t];
        if (ts->medsym_sprite_idx <= 0)
            continue;
        snprintf(name, sizeof(name), "Traps__%s small", ts->code_name); sp_dump_icon(folder, name, ts->medsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Traps__%s small (none to place)", ts->code_name); sp_dump_icon(folder, name, ts->medsym_sprite_idx + 1, 0);
        snprintf(name, sizeof(name), "Traps__%s big", ts->code_name); sp_dump_icon(folder, name, ts->bigsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Traps__%s big (none to place)", ts->code_name); sp_dump_icon(folder, name, ts->bigsym_sprite_idx + 1, 0);
    }
    for (int d = 1; d < game.conf.trapdoor_conf.door_types_count; d++)
    {
        const struct DoorConfigStats *ds = &game.conf.trapdoor_conf.door_cfgstats[d];
        if (ds->medsym_sprite_idx <= 0)
            continue;
        snprintf(name, sizeof(name), "Doors__%s small", ds->code_name); sp_dump_icon(folder, name, ds->medsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Doors__%s small (none to place)", ds->code_name); sp_dump_icon(folder, name, ds->medsym_sprite_idx + 1, 0);
        snprintf(name, sizeof(name), "Doors__%s big", ds->code_name); sp_dump_icon(folder, name, ds->bigsym_sprite_idx, 0);
        snprintf(name, sizeof(name), "Doors__%s big (none to place)", ds->code_name); sp_dump_icon(folder, name, ds->bigsym_sprite_idx + 1, 0);
    }
    /* the event buttons' signs: normal and lit (open or pressed); their blink pictures are other signs of this list */
    static const struct { long spr; const char *name; } evs[] = {
        {GPS_message_rpanel_msg_room_act, "New room"}, {GPS_message_rpanel_msg_spell_act, "New spell"},
        {GPS_message_rpanel_msg_manufct_act, "New trap or door"}, {GPS_message_rpanel_msg_creatr_act, "New creature"},
        {GPS_message_rpanel_msg_questn_act, "Objective (question mark)"}, {GPS_message_rpanel_msg_exclam_act, "Warning (exclamation)"},
        {GPS_message_rpanel_msg_battle_act, "Fight"}, {GPS_message_rpanel_msg_battlr_act, "Fight (blink)"},
        {GPS_message_rpanel_msg_inforb_act, "Information (blue)"}, {GPS_message_rpanel_msg_inforg_act, "Information (green)"},
        {GPS_message_rpanel_msg_infolg_act, "Information (blink)"}, {GPS_message_rpanel_msg_bonusbox_act, "Special box"},
        {GPS_message_rpanel_msg_payday_act, "Payday"}, {GPS_message_rpanel_msg_alarm_act, "Alarm"},
        {GPS_message_rpanel_msg_exclam2_act, "Heart attacked"}, {GPS_message_rpanel_msg_exclam2r_act, "Heart attacked (blink)"}};
    for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); i++)
    {
        snprintf(name, sizeof(name), "Event buttons__%s", evs[i].name); sp_dump_icon(folder, name, evs[i].spr + 1, 3);
        snprintf(name, sizeof(name), "Event buttons__%s (lit)", evs[i].name); sp_dump_icon(folder, name, evs[i].spr, 3);
    }
    /* the panel's small buttons */
    static const struct { long spr; const char *name; } btns[] = {
        {GPS_rpanel_rpanel_btn_up_act, "Scroll up (pressed)"}, {GPS_rpanel_rpanel_btn_up_std, "Scroll up"},
        {GPS_rpanel_rpanel_btn_down_act, "Scroll down (pressed)"}, {GPS_rpanel_rpanel_btn_down_std, "Scroll down"},
        {GPS_rpanel_tendency_attacke_act, "Attack tendency on (pressed)"}, {GPS_rpanel_tendency_attacke_std, "Attack tendency on"},
        {GPS_rpanel_tendency_attackd_act, "Attack tendency off (pressed)"}, {GPS_rpanel_tendency_attackd_std, "Attack tendency off"},
        {GPS_rpanel_tendency_fleee_act, "Flee on (pressed)"}, {GPS_rpanel_tendency_fleee_std, "Flee on"},
        {GPS_rpanel_tendency_fleed_act, "Flee off (pressed)"}, {GPS_rpanel_tendency_fleed_std, "Flee off (blink)"},
        {GPS_rpanel_tendency_prisne_act, "Imprison on (pressed)"}, {GPS_rpanel_tendency_prisne_std, "Imprison on"},
        {GPS_rpanel_tendency_prisnd_act, "Imprison off (pressed)"}, {GPS_rpanel_tendency_prisnd_std, "Imprison off (blink)"},
        {GPS_rpanel_tendency_prisnu_dis, "Imprison (no prison)"},
        {GPS_rpanel_rpanel_btn_nxpage_act, "Next page (pressed)"}, {GPS_rpanel_rpanel_btn_nxpage_std, "Next page"},
        {GPS_rpanel_rpanel_btn_crinfo_act, "Query a creature (pressed)"}, {GPS_rpanel_rpanel_btn_crinfo_std, "Query a creature"},
        {GPS_rpanel_rpanel_btn_cassisti_act, "Assistant (pressed)"}, {GPS_rpanel_rpanel_btn_cassisti_std, "Assistant"},
        {GPS_rpanel_rpanel_btn_cassistw_act, "Assistant on (blink, pressed)"}, {GPS_rpanel_rpanel_btn_cassistw_std, "Assistant on (blink)"}};
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++)
    {
        snprintf(name, sizeof(name), "Side panel buttons__%s", btns[i].name);
        sp_dump_icon(folder, name, btns[i].spr, 1);
    }
    /* the Creatures tab's activities, and each creature kind's picture */
    static const struct { long spr; const char *name; } acts[] = {
        {GPS_rpanel_tab_crtr_wandr_std, "Wandering"}, {GPS_rpanel_tab_crtr_wandr_act, "Wandering (pressed)"},
        {GPS_rpanel_tab_crtr_work_std, "Working"}, {GPS_rpanel_tab_crtr_work_act, "Working (pressed)"},
        {GPS_rpanel_tab_crtr_fight_std, "Fighting"}, {GPS_rpanel_tab_crtr_fight_act, "Fighting (pressed)"}};
    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++)
    {
        snprintf(name, sizeof(name), "Creatures tab activities__%s", acts[i].name);
        sp_dump_icon(folder, name, acts[i].spr, 2);
    }
    for (int m = 1; m < game.conf.crtr_conf.model_count; m++)
    {
        snprintf(name, sizeof(name), "Creatures__%s", creature_code_name(m));
        sp_dump_icon(folder, name, get_creature_model_graphics(m, CGI_HandSymbol), 0);
    }
    /* the Information tab's keeper signs, in each keeper colour */
    for (int p = 0; p < 9; p++)
    {
        snprintf(name, sizeof(name), "Keeper signs__player %d rooms", p);
        sp_dump_icon(folder, name, get_player_colored_icon_idx(GPS_plyrsym_symbol_room_red_std_a, p), 0);
        snprintf(name, sizeof(name), "Keeper signs__player %d face", p);
        sp_dump_icon(folder, name, get_player_colored_icon_idx(GPS_plyrsym_symbol_player_red_std_a, p), 0);
        snprintf(name, sizeof(name), "Keeper signs__player %d defeated", p);
        sp_dump_icon(folder, name, get_player_colored_icon_idx(GPS_plyrsym_symbol_player_red_dead, p), 0);
        snprintf(name, sizeof(name), "Keeper signs__player %d allied", p);
        sp_dump_icon(folder, name, get_player_colored_icon_idx(GPS_plyrsym_symbol_player_red_std_b, p), 0);
    }
    sp_dump_icon(folder, "Keeper signs__not allied", GPS_plyrsym_symbol_player_any_dis, 0);
    /* the menus' icons */
    sp_dump_menu(folder, "Options", options_menu_buttons);
    sp_dump_menu(folder, "Quit", quit_menu_buttons);
    sp_dump_menu(folder, "Assistant", autopilot_menu_buttons);
    sp_dump_menu(folder, "Graphics", video_menu_buttons);
    sp_dump_menu(folder, "Sound", sound_menu_buttons);
    /* drawn by the code, not sprites (for reference) */
    sp_dump_vector(folder, "Drawn by code__Full map", 0);
    sp_dump_vector(folder, "Drawn by code__Options cogwheel", 1);
    sp_dump_vector(folder, "Drawn by code__Zoom out", 2);
    sp_dump_vector(folder, "Drawn by code__Zoom in", 3);
    if (folder != NULL)
        SYNCLOG("UI icons: %d pictures written to %s", sp_dump_n, folder);
}

/******************************************************************************/
/* Edited icons: a PNG at the icon's place (the names KFX_UI_ICONDUMP writes) is drawn instead of the game's sprite as cut
   here - from new ui/Modern UI icons/<folder>/<name>.png in the game's folder (on the PC, where the user edits them),
   else from fxdata/ui_icons/<folder>/<name>.png (the copies the release ships, config/fxdata/ui_icons). Looked for once
   per icon. */

#define SP_OV_MAX 1024
struct SpOverride {
    short kind;
    long spr;
    char rel[200];
};
static struct SpOverride sp_ov[SP_OV_MAX];
static int sp_ov_n;

static void sp_ov_register(const char *name, long spr, int kind)
{
    for (int i = 0; i < sp_ov_n; i++)
        if ((sp_ov[i].kind == kind) && (sp_ov[i].spr == spr))
            return;
    if (sp_ov_n >= SP_OV_MAX)
        return;
    struct SpOverride *o = &sp_ov[sp_ov_n++];
    o->kind = (short)kind;
    o->spr = spr;
    /* the export's file name: the same characters left out, "__" a folder, the name's end trimmed */
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s", name);
    for (char *q = tmp; *q != '\0'; q++)
        if ((*q == '/') || (*q == '\\') || (*q == ':') || (*q == '*') || (*q == '?') || (*q == '"') || (*q == '<') || (*q == '>') || (*q == '|'))
            *q = '_';
    size_t n = 0;
    for (const char *q = tmp; (*q != '\0') && (n + 1 < sizeof(o->rel)); q++)
    {
        if ((q[0] == '_') && (q[1] == '_'))
        {
            while ((n > 0) && (o->rel[n - 1] == ' '))
                n--;
            o->rel[n++] = '/';
            q++;
            continue;
        }
        o->rel[n++] = *q;
    }
    while ((n > 0) && (o->rel[n - 1] == ' '))
        n--;
    o->rel[n] = '\0';
}

static unsigned char *sp_png_read(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    unsigned char *data = NULL;
    long size = 0;
    if ((fseek(f, 0, SEEK_END) == 0) && ((size = ftell(f)) > 0) && (fseek(f, 0, SEEK_SET) == 0))
    {
        data = (unsigned char *)malloc((size_t)size);
        if ((data != NULL) && (fread(data, 1, (size_t)size, f) != (size_t)size))
        {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    if (data == NULL)
        return NULL;
    unsigned char *out = NULL;
    spng_ctx *ctx = spng_ctx_new(0);
    struct spng_ihdr ihdr;
    size_t out_size = 0;
    if ((ctx != NULL) && (spng_set_png_buffer(ctx, data, (size_t)size) == 0) && (spng_get_ihdr(ctx, &ihdr) == 0) &&
        (ihdr.width > 0) && (ihdr.height > 0) && (ihdr.width <= 1024) && (ihdr.height <= 1024) &&
        (spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &out_size) == 0))
    {
        out = (unsigned char *)malloc(out_size);
        if ((out != NULL) && (spng_decode_image(ctx, out, out_size, SPNG_FMT_RGBA8, 0) != 0))
        {
            free(out);
            out = NULL;
        }
        *w = (int)ihdr.width;
        *h = (int)ihdr.height;
    }
    if (ctx != NULL)
        spng_ctx_free(ctx);
    free(data);
    if (out == NULL)
        return NULL;
    /* cropped to what is drawn (so it stands in the middle as the cut ones do) */
    int x0 = *w, y0 = *h, x1 = -1, y1 = -1;
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++)
            if (out[((size_t)y * *w + x) * 4 + 3] != 0)
            {
                x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y);
            }
    if (x1 < x0)
    {
        free(out);
        return NULL;
    }
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    for (int y = 0; y < ch; y++)
        memmove(out + (size_t)y * cw * 4, out + ((size_t)(y + y0) * *w + x0) * 4, (size_t)cw * 4);
    *w = cw;
    *h = ch;
    return out;
}

unsigned char *world_ui_icon_override(int kind, long spr, int *w, int *h)
{
    sp_icon_dump();
    for (int i = 0; i < sp_ov_n; i++)
    {
        if ((sp_ov[i].kind != kind) || (sp_ov[i].spr != spr))
            continue;
        char rel[260];
        snprintf(rel, sizeof(rel), "new ui/Modern UI icons/%s.png", sp_ov[i].rel);
        unsigned char *px = sp_png_read(rel, w, h); /* (the user's edits first: the package's copies may be older) */
        if (px == NULL)
        {
            snprintf(rel, sizeof(rel), "ui_icons/%s.png", sp_ov[i].rel);
            px = sp_png_read(prepare_file_path(FGrp_FxData, rel), w, h);
        }
        if (px != NULL)
            SYNCLOG("UI icons: %s.png used (%dx%d)", sp_ov[i].rel, *w, *h);
        return px;
    }
    return NULL;
}

/******************************************************************************/
/* The "Paused" box: a Molten button (clicking it goes on, as the game's box) */

static struct UmHost sp_paused_host;

TbBool world_ui_paused(const char *text, int x, int y, int w, int h, TbBool hot)
{
    if (!world_side_hud_modern() || (text == NULL))
        return false;
    const float u = sp_hud_u();
    const TbBool flat = um_host_flat();
    const struct UmStyle stv = {u, flat};
    const TbBool down = hot && (lbDisplay.MLeftButton != 0);
    /* the game's box is its words' size: the button a little roomier, with the same middle */
    const float px = 18.0f * u;
    const int bw = max(w, (int)ceilf(ui_text_width(UiF_Head, px, 0.8f * u, text) + 48.0f * u));
    const int bh = max(h, (int)ceilf(34.0f * u));
    const int bx = x + w / 2 - bw / 2, by = y + h / 2 - bh / 2;
    const int m = flat ? 0 : (int)ceilf(16.0f * u);
    char key[160];
    snprintf(key, sizeof(key), "%s|%d %d %d %d %.4f %d", text, bw, bh, (int)hot, (int)down, (double)u, (int)flat);
    sp_paused_host.name = "paused";
    sp_paused_host.slot = UmSl_Spare;
    if (um_host_begin(&sp_paused_host, bw + 2 * m, bh + 2 * m, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &sp_paused_host.canvas;
        if (!flat)
            ui_shadow_rrect(c, (float)m, (float)m, (float)(m + bw), (float)(m + bh), 10.0f * u, (float)m, 4.0f * u, UIC(0, 0, 0, 150));
        um_button(c, &stv, (float)m, (float)m, (float)(m + bw), (float)(m + bh), UmB_Primary, down ? UmS_Down : (hot ? UmS_Hot : UmS_Normal),
            px, text);
        um_host_painted(&sp_paused_host, t0);
    }
    um_host_show(&sp_paused_host, bx - m, by - m);
    return true;
}

/******************************************************************************/
/* the side panel's tab: tucks the panel away and brings it back (the user, 2026-11-26) */

TbBool world_side_tab_over(void)
{
    if (!world_side_panel_on() || (sp_hide_mode == 2) || (sp_a_events >= 0.999f) || (sp_u <= 0.0f))
        return false;
    float r[4];
    sp_tab_rect(r);
    const int mx = GetMouseX(), my = GetMouseY();
    const float x0 = sp_ox + r[0] * sp_u, y0 = sp_oy + r[1] * sp_u;
    return ((float)mx >= x0) && ((float)mx < x0 + r[2] * sp_u) && ((float)my >= y0) && ((float)my < y0 + r[3] * sp_u);
}

void world_side_tab_click(void)
{
    sp_hide_mode = (sp_hide_mode == 1) ? 0 : 1;
    SYNCLOG("World: the side panel %s (its tab)", (sp_hide_mode == 1) ? "slides away" : "comes back");
}

/******************************************************************************/
/* The message box at the foot of the view (the user, 2026-11-27: "modernize the dialogue box. Instead of the arrows,
   give it a scroll bar that also works by mousescrolling while the mouse hovers over the dialogue box") */

static struct UmHost sp_tb_host;
static float sp_tb_scroll;          /* how far the words are scrolled (pixels) */
static float sp_tb_max;             /* ... at the most */
static float sp_tb_line;            /* a line's height (pixels): the wheel's step */
static uint32_t sp_tb_text_hash;
static int sp_tb_bar[4];            /* the scroll bar on the screen (x, y, w, h); w 0: none */
static float sp_tb_thumb[2];        /* its thumb (top, height), from the bar's top */
static TbBool sp_tb_dragging, sp_tb_was_down;
static int sp_tb_grab_y;
static float sp_tb_grab_scroll;
static int sp_tb_menu = -1;         /* the box's menu number while it is painted */

#define SP_TB_LINES 24

/** The words broken into lines that fit w pixels (at most max); returns how many. */
static int sp_tb_wrap(const char *text, float px, float w, char lines[][160], int max)
{
    int n = 0;
    const char *p = text;
    while ((*p != '\0') && (n < max))
    {
        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;
        char line[160];
        int best = 0, k = 0;
        for (;;)
        {
            while ((p[k] != '\0') && (p[k] != ' ') && (p[k] != '\n'))
                k++;
            if (k >= (int)sizeof(line))
                break;
            memcpy(line, p, (size_t)k);
            line[k] = '\0';
            if ((best > 0) && (ui_text_width(UiF_Body, px, 0.0f, line) > w))
                break;
            best = k;
            if ((p[k] == '\0') || (p[k] == '\n'))
                break;
            k++;
        }
        if (best <= 0)
            best = (int)min(strlen(p), sizeof(line) - 1);
        memcpy(lines[n], p, (size_t)best);
        lines[n][best] = '\0';
        n++;
        p += best;
        if (*p == '\n')
            p++;
    }
    return n;
}

static struct GuiButton *sp_tb_button(const struct GuiMenu *gmnu, int id)
{
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
    {
        struct GuiButton *b = &active_buttons[i];
        if (((b->flags & LbBtnF_Active) != 0) && (b->gmenu_idx == gmnu->number) && (b->id_num == id))
            return b;
    }
    return NULL;
}

TbBool world_ui_textbox_draw(struct GuiMenu *gmnu)
{
    if (!world_side_hud_modern() || (gmnu == NULL) || (gmnu->ident != GMnu_TEXT_INFO))
        return false;
    if (gmnu->visual_state == 1)
    {
        gmnu->fade_time = 1;
        gmnu->visual_state = 2; /* (there at once, as the modern menus) */
    }
    if (gmnu->visual_state != 2)
        return true;
    sp_tb_menu = gmnu->number;
    const float u = sp_hud_u();
    const TbBool flat = um_host_flat();
    const struct UmStyle stv = {u, flat};
    /* the arrows go: the scroll bar takes their place */
    for (int id = BID_OBJ_SCRL_UP; id <= BID_OBJ_SCRL_DWN; id++)
    {
        struct GuiButton *b = sp_tb_button(gmnu, id);
        if (b != NULL)
        {
            b->scr_pos_x = -4000;
            b->pos_x = -4000;
        }
    }
    const struct GuiButton *eye = sp_tb_button(gmnu, BID_EVENT_ZOOM), *tick = sp_tb_button(gmnu, BID_OBJ_CLOSE);
    const int m = flat ? 0 : (int)ceilf(um_window_margin(&stv) * 0.7f);
    const int cw = gmnu->width + 2 * m, ch = gmnu->height + 2 * m;
    /* the words: right of the buttons, left of the bar */
    const float pad = 12.0f * u;
    const float left = (float)((eye != NULL) ? (eye->scr_pos_x + eye->width) : gmnu->pos_x) + pad;
    const float bar_w = max(4.0f, 7.0f * u);
    const float right = (float)(gmnu->pos_x + gmnu->width) - pad - bar_w - 8.0f * u;
    const float top = (float)gmnu->pos_y + 10.0f * u, bottom = (float)(gmnu->pos_y + gmnu->height) - 10.0f * u;
    const float px = 15.0f * u;
    sp_tb_line = px * 1.32f;
    /* test switch KFX_WORLD_LONGTEXT=1: the message three times over, to see the scroll bar */
    static int long_test = -1;
    if (long_test < 0)
        long_test = (getenv("KFX_WORLD_LONGTEXT") != NULL) ? 1 : 0;
    if ((long_test == 1) && (game.evntbox_scroll_window.text[0] != 0))
    {
        char t3[sizeof(game.evntbox_scroll_window.text)];
        snprintf(t3, sizeof(t3), "%s %s %s", game.evntbox_scroll_window.text, game.evntbox_scroll_window.text, game.evntbox_scroll_window.text);
        snprintf(game.evntbox_scroll_window.text, sizeof(game.evntbox_scroll_window.text), "%s", t3);
        long_test = 2;
    }
    const char *text = game.evntbox_scroll_window.text;
    const uint32_t th = um_hash("%s|%.3f", text, (double)(right - left));
    if (th != sp_tb_text_hash)
    {
        sp_tb_text_hash = th;
        sp_tb_scroll = 0.0f; /* (a new message starts at its top) */
    }
    static char lines[SP_TB_LINES][160];
    const int n = sp_tb_wrap(text, px, right - left, lines, SP_TB_LINES);
    const float total = (float)n * sp_tb_line, room = bottom - top;
    sp_tb_max = max(0.0f, total - room);
    sp_tb_scroll = max(0.0f, min(sp_tb_scroll, sp_tb_max));
    if (sp_tb_max > 0.0f)
    {
        sp_tb_bar[0] = (int)(right + 8.0f * u);
        sp_tb_bar[1] = (int)top;
        sp_tb_bar[2] = (int)bar_w;
        sp_tb_bar[3] = (int)room;
        const float track = room;
        sp_tb_thumb[1] = max(track * room / total, 14.0f * u);
        sp_tb_thumb[0] = (track - sp_tb_thumb[1]) * (sp_tb_scroll / sp_tb_max);
    } else
        sp_tb_bar[2] = 0;
    const TbBool eye_on = (eye != NULL) && ((eye->flags & LbBtnF_Enabled) != 0) && ((eye->flags & LbBtnF_Visible) != 0);
    const TbBool tick_on = (tick != NULL) && ((tick->flags & LbBtnF_Visible) != 0);
    const int mx = GetMouseX(), my = GetMouseY();
    const TbBool bar_hot = (sp_tb_bar[2] > 0) && (mx >= sp_tb_bar[0] - 4) && (mx < sp_tb_bar[0] + sp_tb_bar[2] + 4) &&
        (my >= sp_tb_bar[1]) && (my < sp_tb_bar[1] + sp_tb_bar[3]);
    char key[160];
    snprintf(key, sizeof(key), "%d %d %d %d %.4f %d %08x %.1f %d%d%d%d%d %d", gmnu->pos_x, gmnu->pos_y, cw, ch, (double)u, (int)flat,
        (unsigned)th, (double)sp_tb_scroll, (int)eye_on, (int)(eye_on && (eye->flags & LbBtnF_MouseOver)), (int)tick_on,
        (int)(tick_on && (tick->flags & LbBtnF_MouseOver)), (int)(bar_hot || sp_tb_dragging), n * 4 +
        (int)(eye_on && eye->button_state_left_pressed) * 2 + (int)(tick_on && (tick->button_state_left_pressed || tick->button_state_right_pressed)));
    sp_tb_host.name = "message";
    sp_tb_host.slot = UmSl_Front; /* (the frontend's slot: free in a game) */
    if (sp_tb_host.gen < 0x20000000u)
        sp_tb_host.gen = 0x20000000u; /* (its own numbers: the frontend's windows use the slot there) */
    if (um_host_begin(&sp_tb_host, cw, ch, key))
    {
        const unsigned long t0 = (unsigned long)LbTimerClock();
        struct UiCanvas *c = &sp_tb_host.canvas;
        const float ox = (float)(gmnu->pos_x - m), oy = (float)(gmnu->pos_y - m);
        if (flat)
            um_window(c, &stv, (float)m, (float)m, (float)(m + gmnu->width), (float)(m + gmnu->height));
        else
        {
            /* see-through glass, as the original box: the world shows through it (the user, 2026-11-27) */
            const float x0 = (float)m, y0 = (float)m, x1 = (float)(m + gmnu->width), y1 = (float)(m + gmnu->height);
            const float r = 14.0f * u;
            ui_fill_rrect(c, x0, y0, x1, y1, r, UIC(24, 16, 12, 120), UIC(10, 7, 5, 150), false);
            ui_fill_rrect(c, x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + (y1 - y0) * 0.4f, r, UIC(255, 226, 190, 16), UIC(255, 226, 190, 0), false);
            ui_stroke_rrect(c, x0, y0, x1, y1, r, max(1.0f, u), UIC(255, 214, 170, 70));
        }
        /* the eye (to the event's place) and the tick (close): the game's own pictures */
        const struct GuiButton *rb[2] = {eye_on ? eye : NULL, tick_on ? tick : NULL};
        for (int k = 0; k < 2; k++)
        {
            const struct GuiButton *b = rb[k];
            if (b == NULL)
                continue;
            const float r = (float)min(b->width, b->height) * 0.5f;
            const float cx = (float)b->scr_pos_x - ox + (float)b->width * 0.5f, cy = (float)b->scr_pos_y - oy + (float)b->height * 0.5f;
            const TbBool hot = ((b->flags & LbBtnF_MouseOver) != 0);
            /* (the game's picture alone, as the original box: no button behind it; the user, 2026-11-27) */
            /* (as the game draws them: the plain picture, the lit one, sprite_idx, only while pressed; the user, 2026-11-27) */
            const TbBool pressed = (b->button_state_left_pressed != 0) || (b->button_state_right_pressed != 0);
            int iw = 0, ih = 0;
            const unsigned char *pic = um_icon(b->sprite_idx + (pressed ? 0 : 1), &iw, &ih);
            sp_picture(c, pic, iw, ih, cx - r * 1.4f, cy - r * 1.4f, cx + r * 1.4f, cy + r * 1.4f, 0.9f, hot ? 1.15f : 1.0f);
        }
        /* the words, clipped to their room */
        ui_canvas_clip(c, (int)(left - ox), (int)(top - oy), (int)(right - ox) + 1, (int)(bottom - oy) + 1);
        const float y0 = (total <= room) ? top + (room - total) * 0.5f : top - sp_tb_scroll;
        for (int i = 0; i < n; i++)
        {
            const float ly = y0 + (float)i * sp_tb_line;
            if ((ly + sp_tb_line < top) || (ly > bottom))
                continue;
            ui_text_box(c, UiF_Body, ui_text_fit(UiF_Body, px, 0.0f, right - left, lines[i]), left - ox, ly - oy, right - left, sp_tb_line, 1,
                UM_TEXT, 0.0f, !flat, lines[i]);
        }
        ui_canvas_unclip(c);
        /* the bar: a thin track and its thumb */
        if (sp_tb_bar[2] > 0)
        {
            const float bx0 = (float)sp_tb_bar[0] - ox, by0 = (float)sp_tb_bar[1] - oy, bw = (float)sp_tb_bar[2], bh = (float)sp_tb_bar[3];
            const float r = flat ? 0.0f : bw * 0.5f;
            ui_fill_rrect(c, bx0, by0, bx0 + bw, by0 + bh, r, UIC(255, 236, 214, 22), UIC(255, 236, 214, 22), false);
            const float t0y = by0 + sp_tb_thumb[0], t1y = t0y + sp_tb_thumb[1];
            ui_fill_rrect(c, bx0, t0y, bx0 + bw, t1y, r, (bar_hot || sp_tb_dragging) ? UM_GOLD_HI : UM_GOLD, UIC(150, 104, 44, 255), false);
        }
        um_host_painted(&sp_tb_host, t0);
    }
    um_host_show(&sp_tb_host, gmnu->pos_x - m, gmnu->pos_y - m);
    return true;
}

/** The box's words' button (its maintain, each turn the box is up): the mouse wheel over the box scrolls it, the bar's
 *  thumb drags, a click on the bar above or below the thumb pages. */
void world_ui_textbox_maintain(struct GuiButton *gbtn)
{
    if (!world_side_hud_modern() || (gbtn == NULL))
        return;
    const struct GuiMenu *gmnu = get_active_menu(gbtn->gmenu_idx);
    if ((gmnu == NULL) || (gmnu->ident != GMnu_TEXT_INFO) || (sp_tb_max <= 0.0f))
    {
        sp_tb_dragging = false;
        return;
    }
    const int mx = GetMouseX(), my = GetMouseY();
    const TbBool over = (mx >= gmnu->pos_x) && (mx < gmnu->pos_x + gmnu->width) && (my >= gmnu->pos_y) && (my < gmnu->pos_y + gmnu->height);
    if (over && (wheel_scrolled_up || wheel_scrolled_down))
    {
        /* (the box's, not the camera's zoom) */
        sp_tb_scroll = max(0.0f, min(sp_tb_scroll + (wheel_scrolled_down ? 1.0f : -1.0f) * 2.0f * sp_tb_line, sp_tb_max));
        wheel_scrolled_up = false;
        wheel_scrolled_down = false;
        clear_key_pressed(KC_MOUSEWHEEL_UP);
        clear_key_pressed(KC_MOUSEWHEEL_DOWN);
    }
    const TbBool down = (lbDisplay.MLeftButton != 0);
    const TbBool on_bar = (sp_tb_bar[2] > 0) && (mx >= sp_tb_bar[0] - 4) && (mx < sp_tb_bar[0] + sp_tb_bar[2] + 4) &&
        (my >= sp_tb_bar[1]) && (my < sp_tb_bar[1] + sp_tb_bar[3]);
    if (down && !sp_tb_was_down && on_bar)
    {
        const float at = (float)(my - sp_tb_bar[1]);
        if ((at >= sp_tb_thumb[0]) && (at < sp_tb_thumb[0] + sp_tb_thumb[1]))
        {
            sp_tb_dragging = true;
            sp_tb_grab_y = my;
            sp_tb_grab_scroll = sp_tb_scroll;
        } else
        {
            const float page = (float)sp_tb_bar[3] - sp_tb_line;
            sp_tb_scroll = max(0.0f, min(sp_tb_scroll + ((at < sp_tb_thumb[0]) ? -page : page), sp_tb_max));
        }
    }
    if (down && sp_tb_dragging)
    {
        const float track = (float)sp_tb_bar[3] - sp_tb_thumb[1];
        if (track > 0.0f)
            sp_tb_scroll = max(0.0f, min(sp_tb_grab_scroll + (float)(my - sp_tb_grab_y) * sp_tb_max / track, sp_tb_max));
    }
    if (!down)
        sp_tb_dragging = false;
    sp_tb_was_down = down;
}

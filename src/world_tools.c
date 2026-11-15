/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_tools.c
 *     Endless world: the Keeper's Tools panel.
 * @par Purpose:
 *     See world_tools.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "world_tools.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "bflib_keybrd.h"
#include "bflib_mouse.h"
#include "kjm_input.h"
#include "world_stream.h"
#include "game_legacy.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "console_cmd.h"
#include "local_camera.h"
#include "gui_parchment.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "frontend.h"
#include "chat_box.h"
#include "net_state.h"
#include "world_feats.h"
#include "world_watchfire.h"
#include "world_masonry.h"
#include "world_falseheart.h"
#include "world_champion.h"
#include "world_attune.h"
#include "world_rigging.h"
#include "world_cookbook.h"
#include "world_heresy.h"
#include "world_unions.h"
#include "world_exodus.h"
#include "world_dressing.h"
#include "world_gen.h"
#include "keeperfx.hpp"
#include "packets.h"
#include "map_data.h"
#include "map_blocks.h"
#include "config_settings.h"
#include "front_input.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* how a tool is used: at once, or with the cursor (once, or until a right click) */
enum { TlNow = 0, TlPlaceOnce, TlPlaceMany };

struct KeeperTool {
    const char *name;
    const char *cmd;  /* the chat command it gives */
    uint8_t use;
};

#define TOOLS_COUNT 16
static const struct KeeperTool tools[TOOLS_COUNT] = {
    {"Rap Sheet",        "!feats",      TlNow},
    {"Attunement",       "!attune",     TlNow},
    {"Cookbook",         "!cookbook",   TlNow},
    {"Heresies",         "!heresy",     TlNow},
    {"Unholy Unions",    "!union",      TlNow},
    {"Dungeon looks",    "!look",       TlNow},
    {"Possess Champion", "!possess",    TlNow},
    {"Exodus",           "!exodus",     TlNow},
    {"Watchfire",        "!watchfire",  TlPlaceOnce},
    {"Backfill",         "!backfill",   TlPlaceMany},
    {"Masonry",          "!masonry",    TlPlaceMany},
    {"False Heart",      "!falseheart", TlPlaceOnce},
    {"Make Champion",    "!champion",   TlPlaceOnce},
    {"Rig an Alarm",     "!rig",        TlPlaceMany},
    {"Settle the Heart", "!settle",     TlPlaceOnce},
    {"Counting door",    "!count 3",    TlPlaceOnce},
};
static char tool_cost[TOOLS_COUNT][48];

static int armed = -1; /* the tool waiting for a map click */
/* the choice pages: a list tool's button opens a page of its choices (and shows the list); the last button goes back */
enum { TpTools = 0, TpHeresies, TpUnions, TpLooks };
static int page = TpTools;

static void make_costs(void)
{
    if (tool_cost[0][0] != 0)
        return;
    snprintf(tool_cost[0], sizeof(tool_cost[0]), "free: your feats");
    snprintf(tool_cost[1], sizeof(tool_cost[1]), "free: points from levels");
    snprintf(tool_cost[2], sizeof(tool_cost[2]), "free: recipes you made");
    snprintf(tool_cost[3], sizeof(tool_cost[3]), "free: your tomes");
    snprintf(tool_cost[4], sizeof(tool_cost[4]), "free: needs a Temple");
    snprintf(tool_cost[5], sizeof(tool_cost[5]), "free: the looks you have");
    snprintf(tool_cost[6], sizeof(tool_cost[6]), "free");
    snprintf(tool_cost[7], sizeof(tool_cost[7]), "Heart above 3/4 health");
    snprintf(tool_cost[8], sizeof(tool_cost[8]), "%d gold, %d a night", WATCHFIRE_PRICE, WATCHFIRE_NIGHT_PRICE);
    snprintf(tool_cost[9], sizeof(tool_cost[9]), "free: imps fill it");
    snprintf(tool_cost[10], sizeof(tool_cost[10]), "%d gold a slab", MASONRY_PRICE);
    snprintf(tool_cost[11], sizeof(tool_cost[11]), "%d gold, one at a time", FALSEHEART_PRICE);
    snprintf(tool_cost[12], sizeof(tool_cost[12]), "free, once in %d days", CHAMPION_CHANGE_DAYS);
    snprintf(tool_cost[13], sizeof(tool_cost[13]), "free: the Alarm, then devices");
    snprintf(tool_cost[14], sizeof(tool_cost[14]), "Heart arrives at half health");
    snprintf(tool_cost[15], sizeof(tool_cost[15]), "free: locks after 3 heroes");
}

/** The chat commands, as typed (the same chain as the chat line's). */
static TbBool run_command(const char *text, MapCoord x, MapCoord y)
{
    return world_feats_command(text) || world_watchfire_command(text, x, y) || world_masonry_command(text, x, y)
        || world_falseheart_command(text, x, y) || world_champion_command(text, x, y) || world_attune_command(text)
        || world_rigging_command(text, x, y) || world_cookbook_command(text) || world_heresy_command(text)
        || world_union_command(text) || world_exodus_command(text, x, y) || world_dressing_command(text);
}

static void cursor_or_heart(MapCoord *x, MapCoord *y)
{
    struct Coord3d pos;
    if ((get_local_view_type() == PVT_DungeonTop) && screen_to_map(get_local_active_camera(), GetMouseX(), GetMouseY(), &pos))
    {
        *x = pos.x.val;
        *y = pos.y.val;
        return;
    }
    console_cmd_default_cursor(get_my_player(), x, y);
}

/******************************************************************************/
/* the panel */
#define TL_W 480
#define TL_H 360
#define TL_BTN_W 222
#define TL_BTN_H 30
#define TL_ROW 34
#define TL_TOP 50

static char tl_close[] = "Close";

static void toggle_panel(void)
{
    if (menu_is_active(GMnu_KEEPER_TOOLS))
    {
        turn_off_menu(GMnu_KEEPER_TOOLS);
        return;
    }
    if (get_local_view_type() == PVT_MapScreen)
        zoom_from_parchment_map();
    page = TpTools;
    turn_on_menu(GMnu_KEEPER_TOOLS);
    SYNCLOG("World: the Keeper's Tools open");
}

static void tl_close_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    turn_off_menu(GMnu_KEEPER_TOOLS);
}

#define TL_BACK (TOOLS_COUNT - 1)

/** A choice of the page shown: its label, its line and its command; false when the slot is empty. */
static TbBool page_choice(int slot, char *label, size_t llen, char *line, size_t nlen, char *cmd, size_t clen)
{
    if (slot == TL_BACK)
    {
        snprintf(label, llen, "Back");
        snprintf(line, nlen, "to the tools");
        cmd[0] = '\0';
        return true;
    }
    if (page == TpHeresies)
    {
        if (slot == 0)
        {
            snprintf(label, llen, "List your tomes");
            snprintf(line, nlen, "what each one does");
            snprintf(cmd, clen, "!heresy");
            return true;
        }
        if ((slot >= 1) && (slot <= 2 * HERESY_TOMES_PER_KEEPER))
        {
            const int n = (slot - 1) % HERESY_TOMES_PER_KEEPER + 1;
            const TbBool take = (slot <= HERESY_TOMES_PER_KEEPER);
            snprintf(label, llen, "%s tome %d", take ? "Keep" : "Burn", n);
            snprintf(line, nlen, take ? "two heresies at most" : "at your Temple: undone");
            snprintf(cmd, clen, "!heresy %s %d", take ? "take" : "burn", n);
            return true;
        }
        return false;
    }
    if (page == TpUnions)
    {
        static const char *const names[5] = {"List the unions", "Scrying Room", "The Chair", "Poisoned Hatchery", "Infirmary"};
        static const char *const words[5] = {"", " library", " prison", " hatchery", " lair"};
        static const char *const lines[5] = {"and which you have", "Library + Sight of Evil", "Prison + Lightning Trap",
            "Hatchery + Poison Gas Trap", "Lair + Heal"};
        if ((slot < 0) || (slot >= 5))
            return false;
        snprintf(label, llen, "%s", names[slot]);
        snprintf(line, nlen, "%s", lines[slot]);
        snprintf(cmd, clen, "!union%s", words[slot]);
        return true;
    }
    if (page == TpLooks)
    {
        if (slot == 0)
        {
            snprintf(label, llen, "List the looks");
            snprintf(line, nlen, "the ones you have unlocked");
            snprintf(cmd, clen, "!look");
            return true;
        }
        if (slot == 1)
        {
            snprintf(label, llen, "Plain look");
            snprintf(line, nlen, "take the look off");
            snprintf(cmd, clen, "!look off");
            return true;
        }
        const int b = slot - 2;
        if ((b < 0) || (b >= 8))
            return false;
        snprintf(label, llen, "%s", wg_biome_name(b));
        snprintf(line, nlen, "once unlocked");
        snprintf(cmd, clen, "!look %s", wg_biome_name(b));
        return true;
    }
    return false;
}

static void tl_tool_click(struct GuiButton *gbtn)
{
    const int t = (int)gbtn->btype_value & 0xFF;
    if ((t < 0) || (t >= TOOLS_COUNT))
        return;
    MapCoord x, y;
    if (page != TpTools)
    {
        char label[48], line[48], cmd[64];
        if (!page_choice(t, label, sizeof(label), line, sizeof(line), cmd, sizeof(cmd)))
            return;
        if (t == TL_BACK)
        {
            page = TpTools;
            return;
        }
        cursor_or_heart(&x, &y);
        run_command(cmd, x, y); /* the panel stays: the answer shows beside it */
        SYNCLOG("World: tools panel: %s", cmd);
        return;
    }
    if ((t == 3) || (t == 4) || (t == 5))
    {
        page = (t == 3) ? TpHeresies : (t == 4) ? TpUnions : TpLooks;
        cursor_or_heart(&x, &y);
        run_command(tools[t].cmd, x, y); /* the list, and the page of choices */
        SYNCLOG("World: tools panel: %s (its choices)", tools[t].cmd);
        return;
    }
    if (tools[t].use == TlNow)
    {
        cursor_or_heart(&x, &y);
        /* a window tool opens its own window; the panel steps aside */
        turn_off_menu(GMnu_KEEPER_TOOLS);
        run_command(tools[t].cmd, x, y);
        SYNCLOG("World: tools panel: %s", tools[t].cmd);
        return;
    }
    armed = (armed == t) ? -1 : t;
    turn_off_menu(GMnu_KEEPER_TOOLS); /* the map, to click on */
    SYNCLOG("World: tools panel: %s armed", tools[t].cmd);
}

static void tl_background(struct GuiMenu *gmnu);
static void tl_draw_tool(struct GuiButton *gbtn);

#define TL_BTN(i, col, row) \
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, tl_tool_click, NULL, NULL, i, 12 + (col) * (TL_BTN_W + 12), TL_TOP + (row) * TL_ROW, \
   12 + (col) * (TL_BTN_W + 12), TL_TOP + (row) * TL_ROW, TL_BTN_W, TL_BTN_H, tl_draw_tool, 0, GUIStr_Empty, 0, {0}, 0, NULL }

static struct GuiButtonInit world_tools_buttons[] = {
  TL_BTN(0, 0, 0), TL_BTN(1, 0, 1), TL_BTN(2, 0, 2), TL_BTN(3, 0, 3), TL_BTN(4, 0, 4), TL_BTN(5, 0, 5), TL_BTN(6, 0, 6),
  TL_BTN(7, 0, 7), TL_BTN(8, 1, 0), TL_BTN(9, 1, 1), TL_BTN(10, 1, 2), TL_BTN(11, 1, 3), TL_BTN(12, 1, 4),
  TL_BTN(13, 1, 5), TL_BTN(14, 1, 6), TL_BTN(15, 1, 7),
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, tl_close_click, NULL, NULL, 0, 999, TL_H - 40, 999, TL_H - 40, 110, 28, gui_area_text, 1, GUIStr_Empty, 0, {.str = tl_close}, 0, NULL },
  {              -1, BID_DEFAULT, 0, 0, NULL,           NULL, NULL, 0,   0,        0,   0,        0,   0,  0, NULL,          0, 0,            0, {0},              0, NULL },
};

struct GuiMenu world_tools_menu =
 { GMnu_KEEPER_TOOLS, 0, 1, world_tools_buttons, POS_GAMECTR, POS_GAMECTR, TL_W, TL_H, tl_background, 0, NULL, NULL, 0, 1, 0,};

/** A text remap that tints the font: 1 gold, 2 grey; built once from the palette. */
static const unsigned char *tl_tint(int which)
{
    static unsigned char maps[2][256];
    static TbBool made = false;
    if ((which < 1) || (which > 2))
        return NULL;
    if (!made)
    {
        for (int i = 0; i < 256; i++)
        {
            const unsigned char *c = &engine_palette[3 * i];
            const int lum = (c[0] + c[1] + c[2]) / 3;
            maps[0][i] = LbPaletteFindColour(engine_palette, (unsigned char)min(63, lum + 8), (unsigned char)min(63, lum * 4 / 5 + 4),
                (unsigned char)(lum * 2 / 7));
            maps[1][i] = LbPaletteFindColour(engine_palette, (unsigned char)(lum * 2 / 3), (unsigned char)(lum * 2 / 3),
                (unsigned char)(lum * 2 / 3));
        }
        made = true;
    }
    return maps[which - 1];
}

static void tl_text(int x, int y, int w, int px, int align, const char *text, int tint)
{
    const unsigned long flg_mem = RendererGetDrawFlags();
    LbTextSetFont(winfont);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags((align == 1) ? Lb_TEXT_HALIGN_CENTER : (align == 2) ? Lb_TEXT_HALIGN_RIGHT : Lb_TEXT_HALIGN_LEFT);
    LbTextSetWindow(x, y, w, 2 * px + 4);
    const int line = max(LbTextLineHeight(), 1);
    const unsigned char *old_remap = LbTextGetRemap();
    const unsigned char *map = tl_tint(tint);
    if (map != NULL)
        LbTextSetRemap(map);
    LbTextDrawResized(0, 0, max(36 * px / line, 4), text);
    if (map != NULL)
        LbTextSetRemap(old_remap);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

static TbPixel tl_colour(int r, int g, int b)
{
    return LbPaletteFindColour(engine_palette, (unsigned char)r, (unsigned char)g, (unsigned char)b);
}

static void tl_frame(int x, int y, int w, int h, int e, TbPixel fill, TbPixel edge)
{
    LbDrawBox(x, y, w, h, fill);
    LbDrawBox(x, y, w, e, edge);
    LbDrawBox(x, y + h - e, w, e, edge);
    LbDrawBox(x, y, e, h, edge);
    LbDrawBox(x + w - e, y, e, h, edge);
}

static TbBool mouse_in(int x, int y, int w, int h)
{
    const int mx = GetMouseX(), my = GetMouseY();
    return (mx >= x) && (mx < x + w) && (my >= y) && (my < y + h);
}

static void tl_draw_tool(struct GuiButton *gbtn)
{
    const int t = (int)gbtn->btype_value & 0xFF;
    if ((t < 0) || (t >= TOOLS_COUNT))
        return;
    make_costs();
    static TbPixel fill, hover, edge, edge_on;
    if (fill == 0)
    {
        fill = tl_colour(10, 9, 9);
        hover = tl_colour(17, 15, 12);
        edge = tl_colour(18, 16, 13);
        edge_on = tl_colour(52, 40, 12);
    }
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width, h = gbtn->height;
    const int e = max(h / 30, 1);
    const TbBool over = mouse_in(x, y, w, h);
    const int px = max(h * 6 / TL_BTN_H, 6), px2 = max(h * 5 / TL_BTN_H, 5);
    if (page != TpTools)
    {
        char label[48], line[48], cmd[64];
        if (!page_choice(t, label, sizeof(label), line, sizeof(line), cmd, sizeof(cmd)))
            return; /* an empty slot on this page */
        tl_frame(x, y, w, h, e, (over || gbtn->button_state_left_pressed) ? hover : fill, (t == TL_BACK) ? edge_on : edge);
        tl_text(x + h / 4, y + h / 10, w - h / 2, px, 0, label, 1);
        tl_text(x + h / 4, y + h / 2, w - h / 2, px2, 0, line, 2);
        return;
    }
    tl_frame(x, y, w, h, e, (over || gbtn->button_state_left_pressed) ? hover : fill, (armed == t) ? edge_on : edge);
    tl_text(x + h / 4, y + h / 10, w - h / 2, px, 0, tools[t].name, 1);
    tl_text(x + h / 4, y + h / 10, w - h / 2, px2, 2, (tools[t].use == TlNow) ? (((t >= 3) && (t <= 5)) ? "choices" : "") : "map", 2);
    tl_text(x + h / 4, y + h / 2, w - h / 2, px2, 0, tool_cost[t], 2);
}

static void tl_background(struct GuiMenu *gmnu)
{
    const int upp = max((gmnu->width * 16 + TL_W / 2) / TL_W, 1);
#define S(v) ((v) * upp / 16)
    static TbPixel panel, edge;
    if (panel == 0)
    {
        panel = tl_colour(5, 5, 7);
        edge = tl_colour(22, 20, 16);
    }
    const int x = gmnu->pos_x, y = gmnu->pos_y, w = gmnu->width, h = gmnu->height;
    tl_frame(x, y, w, h, max(S(1), 1), panel, edge);
    static const char *const titles[4] = {"The Keeper's Tools", "The Keeper's Tools: Heresies",
        "The Keeper's Tools: Unholy Unions", "The Keeper's Tools: Dungeon looks"};
    tl_text(x + S(12), y + S(8), w - S(24), max(S(8), 7), 0, titles[(page >= 0) && (page < 4) ? page : 0], 1);
    tl_text(x + S(12), y + S(10), w - S(24), max(S(6), 6), 2, "K opens and shuts this", 2);
    tl_text(x + S(12), y + S(28), w - S(24), max(S(5), 5), 0, (page == TpTools) ?
        "A map tool: click the map where it goes; a right click cancels. The chat commands work too." :
        "Pick one; the answer shows below. Back returns to the tools.", 2);
#undef S
}

TbBool world_tools_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strcasecmp(text + 1, "tools") != 0)
        return false;
    toggle_panel();
    return true;
}

/******************************************************************************/
/* the "Tools" button on screen and the armed tool's hint */
static void button_rect(int *x, int *y, int *w, int *h)
{
    *w = 64 * units_per_pixel / 16;
    *h = 14 * units_per_pixel / 16;
    *x = MyScreenWidth - *w - 6 * units_per_pixel / 16;
    *y = 48 * units_per_pixel / 16;
}

static TbBool overlay_shown(void)
{
    return world_mode_active() && !netstate_dedicated() && (get_local_view_type() == PVT_DungeonTop) && !replay.load_enable;
}

void world_tools_draw_overlay(void)
{
    if (!overlay_shown())
    {
        armed = -1;
        return;
    }
    static TbPixel fill, hover, edge, edge_on;
    if (fill == 0)
    {
        fill = tl_colour(5, 5, 7);
        hover = tl_colour(14, 12, 10);
        edge = tl_colour(22, 20, 16);
        edge_on = tl_colour(52, 40, 12);
    }
    int x, y, w, h;
    button_rect(&x, &y, &w, &h);
    const int e = max(units_per_pixel / 16, 1);
    tl_frame(x, y, w, h, e, mouse_in(x, y, w, h) ? hover : fill, menu_is_active(GMnu_KEEPER_TOOLS) ? edge_on : edge);
    tl_text(x, y + h / 6, w, max(h * 5 / 14, 5), 1, "Tools (K)", 1);
    if (armed >= 0)
    {
        make_costs();
        char text[160];
        snprintf(text, sizeof(text), "%s (%s): click the map. Right click cancels.", tools[armed].name, tool_cost[armed]);
        const int hw = 360 * units_per_pixel / 16, hh = 16 * units_per_pixel / 16;
        const int hx = (MyScreenWidth - hw) / 2, hy = 6 * units_per_pixel / 16;
        tl_frame(hx, hy, hw, hh, e, fill, edge_on);
        tl_text(hx, hy + hh / 6, hw, max(hh * 5 / 16, 5), 1, text, 1);
    }
}

/** K opens the panel unless the player's own key settings already give K to a game key (then the button and
 *  "!tools" still do). */
static TbBool k_is_free(void)
{
    for (int i = 0; i < GAME_KEYS_COUNT; i++)
        if (settings.kbkeys[i].code == KC_K)
            return false;
    return true;
}

TbBool world_tools_inputs(void)
{
    if (!overlay_shown())
        return false;
    const struct UserState *ustate = get_local_user_state();
    const TbBool typing = ((ustate->init_flags & UsrIF_NewMPMessage) != 0) || chat_box_draws_typing();
    if (!typing && k_is_free() && is_key_pressed(KC_K, KMod_NONE))
    {
        clear_key_pressed(KC_K);
        armed = -1;
        toggle_panel();
        return true;
    }
    int x, y, w, h;
    button_rect(&x, &y, &w, &h);
    if (mouse_in(x, y, w, h) && (left_button_clicked || left_button_released || left_button_held))
    {
        if (left_button_clicked)
        {
            armed = -1;
            toggle_panel();
        }
        left_button_clicked = 0;
        left_button_released = 0;
        left_button_held = 0;
        return true;
    }
    if (armed < 0)
        return false;
    if (right_button_clicked || right_button_released || right_button_held)
    {
        if (right_button_released)
        {
            SYNCLOG("World: tools panel: %s called off", tools[armed].cmd);
            armed = -1;
        }
        right_button_clicked = 0;
        right_button_released = 0;
        right_button_held = 0;
        return true;
    }
    if (left_button_clicked || left_button_held || left_button_released)
    {
        if (left_button_released)
        {
            struct Coord3d pos;
            if (screen_to_map(get_local_active_camera(), GetMouseX(), GetMouseY(), &pos))
            {
                const int t = armed;
                if (tools[t].use == TlPlaceOnce)
                    armed = -1;
                run_command(tools[t].cmd, pos.x.val, pos.y.val);
                SYNCLOG("World: tools panel: %s at subtile %d,%d", tools[t].cmd, (int)coord_subtile(pos.x.val),
                    (int)coord_subtile(pos.y.val));
            }
        }
        left_button_clicked = 0;
        left_button_held = 0;
        left_button_released = 0;
        return true;
    }
    return false;
}

/** KFX_WORLD_TOOLSTEST=turn (test switch): the panel opens at that turn; 100 turns on, the Watchfire is armed from its
 *  button and "clicked" 6 slabs east of the Heart; 100 turns on, Backfill is armed and right-clicked off; 100 turns on
 *  the Rap Sheet button is pressed. */
/** KFX_WORLD_REVEALCHECK=turn (test switch, any machine, a joined client too): at that turn, which slabs this machine's
 *  keeper sees as revealed in a 13x13 square centred 10 slabs east of its Heart (a watchfire's light there) is logged
 *  as a count, from this machine's own copy of the map. */
static void reveal_check(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_REVEALCHECK");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at < 0) || ((long)game.play_gameturn < at) || ((int)my_player_number >= PLAYERS_COUNT))
        return;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    at = -1;
    const MapSlabCoord cx = subtile_slab(heart->mappos.x.stl.num) + 10, cy = subtile_slab(heart->mappos.y.stl.num);
    int seen = 0;
    for (MapSlabCoord y = cy - 6; y <= cy + 6; y++)
        for (MapSlabCoord x = cx - 6; x <= cx + 6; x++)
        {
            const struct Map *mapblk = get_map_block_at(slab_subtile_center(x), slab_subtile_center(y));
            seen += (!map_block_invalid(mapblk) && map_block_revealed(mapblk, my_player_number)) ? 1 : 0;
        }
    SYNCLOG("World: reveal check (player %d): %d of 169 slabs round 10 east of the Heart are revealed", (int)my_player_number, seen);
}

void world_tools_frame(void)
{
    reveal_check();
    {
        /* KFX_WORLD_COOKDIGEST=turn on a joined client too: this machine's digest of the recipes (its cookbook page
           names them from its own copy of the world's seed) */
        static long dg = -2;
        if (dg == -2)
        {
            const char *v = getenv("KFX_WORLD_COOKDIGEST");
            dg = (v != NULL) ? atol(v) : -1;
        }
        if ((dg >= 0) && ((long)game.play_gameturn >= dg) && netstate_thin_mode() && !network_is_host())
        {
            dg = -1;
            SYNCLOG("World: cookbook digest on this joined client %08x", (unsigned)world_cookbook_digest());
        }
    }
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_TOOLSTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_TOOLSTEST=%ld", at);
    }
    if ((at < 0) || !world_mode_active())
        return;
    static int step = 0;
    static long start = -1;
    if (start < 0)
    {
        /* from the turn this machine's keeper has its Heart (a joined player gets its seat a while after it joins) */
        if (((int)my_player_number >= PLAYERS_COUNT) || !thing_exists(get_player_soul_container(my_player_number))
            || ((long)game.play_gameturn < at))
            return;
        start = (long)game.play_gameturn;
    }
    const long turn = (long)game.play_gameturn - start + at;
    struct GuiButton fake;
    memset(&fake, 0, sizeof(fake));
    if ((step == 0) && (turn >= at))
    {
        step = 1;
        toggle_panel();
    } else
    if ((step == 1) && (turn >= at + 100))
    {
        step = 2;
        fake.btype_value = 8; /* Watchfire */
        tl_tool_click(&fake);
        const struct Thing *heart = get_player_soul_container(my_player_number);
        if (thing_exists(heart) && (armed == 8))
        {
            const int t = armed;
            armed = -1;
            run_command(tools[t].cmd, heart->mappos.x.val + 6 * COORD_PER_STL * STL_PER_SLB, heart->mappos.y.val);
            SYNCLOG("World: tools test: the Watchfire was placed from the panel");
        }
    } else
    if ((step == 2) && (turn >= at + 200))
    {
        step = 3;
        fake.btype_value = 9; /* Backfill */
        tl_tool_click(&fake);
        SYNCLOG("World: tools test: Backfill armed: %d", (armed == 9));
        armed = -1;
        SYNCLOG("World: tools test: Backfill called off");
        toggle_panel();
    } else
    if ((step == 3) && (turn >= at + 300))
    {
        step = 4;
        fake.btype_value = 0; /* Rap Sheet */
        tl_tool_click(&fake);
    } else
    if ((step >= 4) && (step < 4 + 12) && (turn >= at + 300 + 40 * (step - 3)) && (getenv("KFX_WORLD_TOOLSALL") != NULL))
    {
        /* KFX_WORLD_TOOLSALL=1 as well: then every other button, one each 40 turns; a map tool is used 6 slabs east of
           the Heart (Settle: 2 slabs, which is refused) */
        static const int order[12] = {3, 4, 5, 2, 1, 7, 14, 11, 10, 12, 6, 13};
        const int t = order[step - 4];
        step++;
        turn_off_menu(GMnu_KEEPER_FEATS);
        turn_off_menu(GMnu_KEEPER_ATTUNE);
        page = TpTools; /* (Back, after a list tool's page) */
        fake.btype_value = (unsigned short)t;
        tl_tool_click(&fake);
        const struct Thing *heart = get_player_soul_container(my_player_number);
        if (thing_exists(heart) && (armed == t))
        {
            armed = -1;
            const int slabs = (t == 14) ? 2 : 6;
            run_command(tools[t].cmd, heart->mappos.x.val + slabs * COORD_PER_STL * STL_PER_SLB, heart->mappos.y.val);
        }
        SYNCLOG("World: tools test: pressed %s", tools[t].name);
        if (t == 1)
        {
            /* in the Attunement window: its first "+" (the packet its button sends) */
            set_players_packet_action(get_my_player(), PckA_WorldAttune, 0, 0, 0, 0);
            SYNCLOG("World: tools test: Attunement's first + pressed");
        }
    } else
    if ((step == 4 + 12) && (turn >= at + 300 + 40 * 13))
    {
        /* the input path itself: Backfill armed, a left release on the map (wherever the mouse is), then a right
           release; what the dungeon view's inputs would get from a real mouse */
        step++;
        turn_off_menu(GMnu_KEEPER_FEATS);
        turn_off_menu(GMnu_KEEPER_ATTUNE);
        fake.btype_value = 9;
        tl_tool_click(&fake);
        left_button_released = 1;
        const TbBool took_left = world_tools_inputs();
        const int still = armed;
        right_button_released = 1;
        const TbBool took_right = world_tools_inputs();
        SYNCLOG("World: tools test: input path: left taken %d (still armed %d), right taken %d (armed now %d)",
            (int)took_left, (int)(still == 9), (int)took_right, armed);
    } else
    if ((step == 4 + 13) && (turn >= at + 300 + 40 * 14))
    {
        /* a choice page: the panel opens, Dungeon looks shows its page, the Earthen look is picked; the page stays up
           (for a screenshot 20 turns on) and Back is pressed 400 turns on */
        step++;
        page = TpTools;
        if (!menu_is_active(GMnu_KEEPER_TOOLS))
            toggle_panel();
        fake.btype_value = 5;
        tl_tool_click(&fake);
        fake.btype_value = (unsigned short)((getenv("KFX_WORLD_TOOLSLOOK") != NULL) ? atoi(getenv("KFX_WORLD_TOOLSLOOK")) : 2); /* (KFX_WORLD_TOOLSLOOK: another slot) */
        tl_tool_click(&fake);
        SYNCLOG("World: tools test: the looks page is up (page %d)", page);
    } else
    if ((step == 4 + 14) && (turn >= at + 300 + 40 * 14 + 400))
    {
        step++;
        fake.btype_value = TL_BACK;
        tl_tool_click(&fake);
        SYNCLOG("World: tools test: Back (page %d)", page);
        turn_off_menu(GMnu_KEEPER_TOOLS); /* and the panel shut, to see the dungeon */
    }
}

#ifdef __cplusplus
}
#endif

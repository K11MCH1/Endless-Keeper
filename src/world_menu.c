/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_menu.c
 *     Endless Mode: the main-menu button, its menu (Continue, New Run, Load Game, Return to Main Menu), the
 *     list of endless saves, and autosaving endless runs.
 * @par Purpose:
 *     Endless Mode plays the hidden map pack levels/endless.cfg, whose one level is a stub that the engine
 *     replaces with a generated world (ENDLESS_WORLD script command, world_stream.c). New Run gives that level
 *     a fresh random seed. A save made while that map pack is loaded is an endless save: those are the ones
 *     Continue (the newest) and Load Game (all of them) offer. Each run has one autosave slot, named
 *     "Endless <seed> d<day> (auto)", written at every dawn and when the player quits to the menu.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "world_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "globals.h"
#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_guibtns.h"
#include "bflib_keybrd.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "config.h"
#include "config_campaigns.h"
#include "config_strings.h"
#include "frontend.h"
#include "front_input.h"
#include "game_legacy.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "kjm_input.h"
#include "player_data.h"
#include "replay.h"
#include "world_daynight.h"
#include "world_stream.h"
#include "world_pages.h"
#include "kfx/save/SaveManager.h"
#include "net_game.h"
#include "keeperfx.hpp"
#include "front_network.h"
#include "frontmenu_net.h"
#include "world_net.h"
#include "net_lobby.h"
#include "scrcapt.h"
#include "bflib_inputctrl.h"
#include "config_settings.h"
#include "server_admin.h"
#include "net_state.h"
#include "packets.h"
#include "observer.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define WM_SAVE_FNAME "fx1g%04d.sav"
#define WM_ROWS 6
#define WM_ROW_CONTENT 45
#define WM_MAX_SAVES 512

/** The endless saves, newest first (slot numbers), as found when the menu was set up. */
static int wm_saves[WM_MAX_SAVES];
static int wm_saves_count;
static int wm_scroll;

static TbBool wm_is_endless_fname(const char *fname)
{
    if (fname == NULL)
        return false;
    const char *base = fname;
    for (const char *p = fname; *p != '\0'; p++)
    {
        if ((*p == '/') || (*p == '\\'))
            base = p + 1;
    }
    return (strcasecmp(base, ENDLESS_MAPPACK_FNAME) == 0);
}

TbBool world_menu_endless_campaign_loaded(void)
{
    return wm_is_endless_fname(campaign.fname);
}

static time_t wm_slot_mtime(int slot)
{
    struct stat st;
    const char *fname = prepare_file_fmtpath(FGrp_Save, WM_SAVE_FNAME, slot);
    if ((fname == NULL) || (stat(fname, &st) != 0))
        return 0;
    return st.st_mtime;
}

/** Re-reads the save catalogue and lists the endless saves, newest file first. */
static void wm_list_saves(void)
{
    static time_t mtimes[WM_MAX_SAVES];
    wm_saves_count = 0;
    SaveManager_LoadCatalogue();
    int slots = SaveManager_SlotCount();
    for (int slot = 0; (slot < slots) && (wm_saves_count < WM_MAX_SAVES); slot++)
    {
        const struct CatalogueEntry *centry = SaveManager_Entry(slot);
        if ((centry == NULL) || ((centry->flags & CEF_InUse) == 0) || !wm_is_endless_fname(centry->campaign_fname))
            continue;
        time_t mt = wm_slot_mtime(slot);
        int i = wm_saves_count++;
        /* insertion by modification time, newest first */
        while ((i > 0) && (mtimes[i - 1] < mt))
        {
            wm_saves[i] = wm_saves[i - 1];
            mtimes[i] = mtimes[i - 1];
            i--;
        }
        wm_saves[i] = slot;
        mtimes[i] = mt;
    }
    if (wm_scroll > wm_saves_count - WM_ROWS)
        wm_scroll = wm_saves_count - WM_ROWS;
    if (wm_scroll < 0)
        wm_scroll = 0;
}

/** The newest endless save that isn't known to be unloadable, or -1. */
static int wm_continue_slot(void)
{
    for (int i = 0; i < wm_saves_count; i++)
    {
        const struct CatalogueEntry *centry = SaveManager_Entry(wm_saves[i]);
        if ((centry != NULL) && ((centry->flags & CEF_Unloadable) == 0))
            return wm_saves[i];
    }
    return -1;
}

/** Starts loading a save from the frontend, the way the stock Load Game menu does. */
static void wm_load_slot(int slot)
{
    enum SaveCheckResult chk = SaveManager_Check(slot, NULL, 0);
    if (chk == SvChk_Loadable)
    {
        world_set_next_seed(NULL);
        SYNCLOG("Endless Mode: loading save slot %d", slot);
        frontend_start_load_game(slot);
    } else
    if (chk == SvChk_Missing)
    {
        SaveManager_DisableSlot(slot);
        wm_list_saves();
    } else
    {
        set_flag(SaveManager_Entry(slot)->flags, CEF_Unloadable);
        create_frontend_error_box(get_string(SaveManager_CheckMessage(chk)));
    }
}

static void wm_continue(struct GuiButton *gbtn);
static void wm_new_run(struct GuiButton *gbtn);
static void wl_join_server(int i);
static void wl_join(const char *address);
static const char *wl_player_name(void);
static void wl_test_scroll(int delta);
static void wl_set_name(const char *name);
static void wm_host(struct GuiButton *gbtn);
static void wm_join(struct GuiButton *gbtn);

/** The player chose Host Network Game: the lobby's Start begins the endless world. */
TbBool world_menu_mp_host = false;

/** Test hook: KFX_WORLD_MENU="E,N" presses the Endless Mode buttons by themselves, one step every 2.5 s
 *  while one of the menus (main, Endless Mode, endless saves, World List) is up: E Endless Mode, M main menu,
 *  L Load Game, N New Run, C Continue, R<n> the n-th row of the save list (0 = newest), H Host (a new run
 *  open to the network), J Join (the World List), W the World List, P<n> a screenshot of the frame, T<n> a chat
 *  line, G<n> join the n-th world of the list, A join 127.0.0.1:5556, X<port> join 127.0.0.1:<port> (a failed join), D<n>/U<n> scroll the list, K<n> the name becomes Tester<n>, anything else
 *  waits. The steps carry on after a game, so one string can drive a whole session. */
void world_menu_test_tick(void)
{
    static const char *steps = NULL;
    static TbBool read = false;
    static TbClockMSec next = 0;
    if (!read)
    {
        steps = getenv("KFX_WORLD_MENU");
        read = true;
    }
    if ((steps == NULL) || (*steps == '\0'))
        return;
    TbClockMSec now = LbTimerClock();
    if (next == 0)
        next = now + 2500;
    if (now < next)
        return;
    next = now + 2500;
    char cmd = *steps;
    int arg = atoi(steps + 1);
    const char *comma = strchr(steps, ',');
    steps = (comma != NULL) ? comma + 1 : "";
    SYNCLOG("Endless Mode test: step %c%d", cmd, arg);
    switch (cmd)
    {
    case 'E': frontend_set_state(FeSt_ENDLESS_MENU); break;
    case 'M': frontend_set_state(FeSt_MAIN_MENU); break;
    case 'L': frontend_set_state(FeSt_ENDLESS_LOAD); break;
    case 'N': wm_new_run(NULL); break;
    case 'H': wm_host(NULL); break;
    case 'J': wm_join(NULL); break;
    case 'W': frontend_set_state(FeSt_WORLD_LIST); break;
    case 'P':
    {
        /* the frame as drawn (no desktop capture) */
        char fname[64];
        snprintf(fname, sizeof(fname), "scrshots/menu_%d_%d.png", arg, (int)LbTimerClock() % 100000);
        take_screenshot(fname);
        break;
    }
    case 'T':
    {
        char line[64];
        snprintf(line, sizeof(line), "hello from %s (%d)", wl_player_name(), arg);
        wnet_menu_say(line);
        break;
    }
    case 'G': wl_join_server(arg); break;
    case 'A': wl_join("127.0.0.1:5556"); break;
    case 'X':
    {
        /* a join that fails (nothing listens on that port): its box must come up over the list */
        char addr[32];
        snprintf(addr, sizeof(addr), "127.0.0.1:%d", (arg > 0) ? arg : 5599);
        wl_join(addr);
        break;
    }
    case 'D': wl_test_scroll(max(arg, 1)); break;

    case 'U': wl_test_scroll(-max(arg, 1)); break;
    case 'K':
    {
        char name[24];
        snprintf(name, sizeof(name), "Tester%d", arg);
        wl_set_name(name);
        break;
    }
    case 'O':
        /* the network service screen: online service and a new session, as -server does */
        if (setup_network_service(FrontendNetSvc_Online))
        {
            frontnet_service_setup();
            frontnet_session_setup();
            frontnet_session_create(NULL);
            frontend_set_state(FeSt_NET_START);
        }
        break;
    case 'S': set_packet_start(NULL); break;
    case 'C': wm_continue(NULL); break;
    case 'R':
        if ((arg >= 0) && (arg < wm_saves_count))
            wm_load_slot(wm_saves[arg]);
        break;
    default: break;
    }
}

/******************************************************************************/
/* Drawing: the stock large button, with a literal caption */

static void wm_draw_caption(struct GuiButton *gbtn, const char *text)
{
    frontend_draw_button(gbtn, 1, text, Lb_TEXT_HALIGN_CENTER);
}

void world_menu_draw_endless_button(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Endless Mode");
}

void world_menu_endless_maintain(struct GuiButton *gbtn)
{
    world_menu_test_tick();
}

static void wm_title_maintain(struct GuiButton *gbtn)
{
    world_menu_test_tick();
}

static void wm_draw_title(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Endless Mode");
}

static void wm_draw_load_title(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Endless Saves");
}

static void wm_draw_continue(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Continue");
}

static void wm_draw_new_run(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "New Run");
}

static void wm_draw_load_game(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Load Game");
}

static void wm_draw_host(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Host Network Game");
}

static void wm_draw_join(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Join Network Game");
}

static void wm_draw_return(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Return to Main Menu");
}

static void wm_draw_back(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "Back");
}

/******************************************************************************/
/* The Endless Mode menu */

static void wm_continue_maintain(struct GuiButton *gbtn)
{
    if (wm_continue_slot() >= 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wm_continue(struct GuiButton *gbtn)
{
    int slot = wm_continue_slot();
    if (slot >= 0)
        wm_load_slot(slot);
}

static void wm_load_game_maintain(struct GuiButton *gbtn)
{
    if (wm_saves_count > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

/** A seed nobody has played: wall clock and timer mixed, 1..99999999 (so it fits the autosave name). */
static void wm_fresh_seed(char *seed, size_t len)
{
    uint64_t x = ((uint64_t)time(NULL) << 20) ^ (uint64_t)LbTimerClock() ^ ((uint64_t)rand() << 40);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    snprintf(seed, len, "%lu", (unsigned long)(x % 99999999ULL) + 1);
}

void world_menu_fresh_seed(char *seed, size_t len)
{
    wm_fresh_seed(seed, len);
}

int world_menu_newest_endless_slot(void)
{
    wm_list_saves();
    return wm_continue_slot();
}

static void wm_host(struct GuiButton *gbtn)
{
    /* a new run that opens to the network at once: others join it from the world list (no lobby) */
    SYNCLOG("Endless Mode: hosting a network run");
    wm_new_run(NULL);
    world_menu_open_on_start = true;
}

static void wm_join(struct GuiButton *gbtn)
{
    world_menu_mp_host = false;
    SYNCLOG("Endless Mode: joining a network run");
    frontend_set_state(FeSt_WORLD_LIST);
}

TbBool world_menu_lobby_start(void)
{
    if (!world_menu_mp_host)
        return false;
    world_menu_mp_host = false;
    /* as the command-line autostart: the start chat, which the host sends and every client follows */
    snprintf(local_state.mp_message_text, PLAYER_MP_MESSAGE_LEN, "levels/endless:1");
    lbInkey = KC_RETURN;
    SYNCLOG("Endless Mode: the lobby starts the endless world");
    return true;
}

static void wm_new_run(struct GuiButton *gbtn)
{
    if (!change_campaign(CampgnT_Mappack, ENDLESS_MAPPACK_FNAME))
    {
        ERRORLOG("Endless Mode: can't load the map pack levels/%s", ENDLESS_MAPPACK_FNAME);
        create_frontend_error_box("Endless Mode needs levels/" ENDLESS_MAPPACK_FNAME);
        return;
    }
    if ((campaign.freeplay_levels_count < 1) || (campaign.freeplay_levels[0] <= 0))
    {
        ERRORLOG("Endless Mode: the map pack has no level");
        create_frontend_error_box("Endless Mode: the map pack has no level");
        return;
    }
    char seed[16];
    wm_fresh_seed(seed, sizeof(seed));
    world_set_next_seed(seed);
    world_menu_mp_host = false;
    SYNCLOG("Endless Mode: new run, seed %s", seed);
    game.selected_level_number = campaign.freeplay_levels[0];
    frontend_set_state(FeSt_START_KPRLEVEL);
}

static struct GuiButtonInit wm_menu_buttons[] = {
  { LbBtnT_NormalBtn, BID_MENU_TITLE, 0, 0, NULL,                  NULL, NULL,                 0, 999,  26, 999,  26, 371, 46, wm_draw_title,     0, GUIStr_Empty, 0, {1}, 0, wm_title_maintain },
  { LbBtnT_NormalBtn, BID_DEFAULT,    0, 0, wm_continue,           NULL, frontend_over_button, 0, 999,  92, 999,  92, 371, 46, wm_draw_continue,  0, GUIStr_Empty, 0, {8}, 0, wm_continue_maintain },
  { LbBtnT_NormalBtn, BID_DEFAULT,    0, 0, wm_new_run,            NULL, frontend_over_button, 0, 999, 138, 999, 138, 371, 46, wm_draw_new_run,   0, GUIStr_Empty, 0, {2}, 0, NULL },
  { LbBtnT_NormalBtn, BID_DEFAULT,    0, 0, frontend_change_state, NULL, frontend_over_button, FeSt_ENDLESS_LOAD, 999, 184, 999, 184, 371, 46, wm_draw_load_game, 0, GUIStr_Empty, 0, {3}, 0, wm_load_game_maintain },
  { LbBtnT_NormalBtn, BID_DEFAULT,    0, 0, wm_host,               NULL, frontend_over_button, 0, 999, 250, 999, 250, 371, 46, wm_draw_host,      0, GUIStr_Empty, 0, {4}, 0, NULL },
  { LbBtnT_NormalBtn, BID_DEFAULT,    0, 0, wm_join,               NULL, frontend_over_button, 0, 999, 296, 999, 296, 371, 46, wm_draw_join,      0, GUIStr_Empty, 0, {5}, 0, NULL },
  { LbBtnT_NormalBtn, BID_DEFAULT,    0, 0, frontend_change_state, NULL, frontend_over_button, FeSt_MAIN_MENU, 999, 404, 999, 404, 371, 46, wm_draw_return, 0, GUIStr_Empty, 0, {6}, 0, NULL },
  {-1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, GUIStr_Empty, 0, {0}, 0, NULL },
};

struct GuiMenu world_endless_menu =
 { GMnu_FEENDLESS, 0, 1, wm_menu_buttons, POS_SCRCTR, POS_SCRCTR, 640, 480, NULL, 0, NULL, NULL, 0, 0, 0,};

void world_menu_setup(void)
{
    wm_list_saves();
    SYNCLOG("Endless Mode: %d endless saves, continue slot %d", wm_saves_count, wm_continue_slot());
}

/******************************************************************************/
/* The list of endless saves: the stock frontend Load Game list, over the endless saves only */

static int wm_row_index(const struct GuiButton *gbtn)
{
    return wm_scroll + (int)gbtn->content.lval - WM_ROW_CONTENT;
}

static void wm_row_maintain(struct GuiButton *gbtn)
{
    int i = wm_row_index(gbtn);
    if ((i >= 0) && (i < wm_saves_count))
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wm_row_click(struct GuiButton *gbtn)
{
    int i = wm_row_index(gbtn);
    if ((i >= 0) && (i < wm_saves_count))
        wm_load_slot(wm_saves[i]);
}

static void wm_row_draw(struct GuiButton *gbtn)
{
    int i = wm_row_index(gbtn);
    if ((i < 0) || (i >= wm_saves_count))
        return;
    const struct CatalogueEntry *centry = SaveManager_Entry(wm_saves[i]);
    if (centry == NULL)
        return;
    int font_idx = frontend_button_caption_font(gbtn, frontend_mouse_over_button);
    if ((centry->flags & CEF_Unloadable) != 0)
        font_idx = 3; /* the grey font */
    LbTextSetFont(frontend_font[font_idx]);
    RendererSetDrawFlags(Lb_TEXT_HALIGN_LEFT);
    int tx_units_per_px = (gbtn->height * 13 / 11) * 16 / LbTextLineHeight();
    int height = LbTextLineHeight() * tx_units_per_px / 16;
    LbTextSetWindow(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, height);
    LbTextDrawResized(0, 0, tx_units_per_px, centry->textname);
}

static void wm_up_maintain(struct GuiButton *gbtn)
{
    if (wm_scroll > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    if ((wheel_scrolled_up || is_key_pressed(KC_UP, KMod_NONE)) && (wm_scroll > 0))
        wm_scroll--;
}

static void wm_down_maintain(struct GuiButton *gbtn)
{
    if (wm_scroll < wm_saves_count - WM_ROWS)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    if ((wheel_scrolled_down || is_key_pressed(KC_DOWN, KMod_NONE)) && (wm_scroll < wm_saves_count - WM_ROWS))
        wm_scroll++;
}

static void wm_up(struct GuiButton *gbtn)
{
    if (wm_scroll > 0)
        wm_scroll--;
}

static void wm_down(struct GuiButton *gbtn)
{
    if (wm_scroll < wm_saves_count - WM_ROWS)
        wm_scroll++;
}

static void wm_scroll_drag(struct GuiButton *gbtn)
{
    wm_scroll = frontend_scroll_tab_to_offset(gbtn, GetMouseY(), WM_ROWS - 2, wm_saves_count);
    if (wm_scroll > wm_saves_count - WM_ROWS)
        wm_scroll = wm_saves_count - WM_ROWS;
    if (wm_scroll < 0)
        wm_scroll = 0;
}

static void wm_scroll_draw(struct GuiButton *gbtn)
{
    frontend_draw_scroll_tab(gbtn, wm_scroll, WM_ROWS - 2, wm_saves_count);
}

static struct GuiButtonInit wm_load_buttons[] = {
  { LbBtnT_NormalBtn,   BID_MENU_TITLE, 0, 0, NULL,           NULL, NULL,                 0, 999,  30, 999,  30, 371,  46, wm_draw_load_title,           0, GUIStr_Empty, 0, {7},  0, wm_title_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  82, 124,  82, 124, 220,  26, frontend_draw_scroll_box_tab, 0, GUIStr_Empty, 0, {28}, 0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  82, 150,  82, 150, 450, 182, frontend_draw_scroll_box,     0, GUIStr_Empty, 0, {26}, 0, NULL },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wm_up,          NULL, frontend_over_button, 0, 532, 149, 532, 149,  26,  14, frontend_draw_slider_button,  0, GUIStr_Empty, 0, {17}, 0, wm_up_maintain },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wm_down,        NULL, frontend_over_button, 0, 532, 317, 532, 317,  26,  14, frontend_draw_slider_button,  0, GUIStr_Empty, 0, {18}, 0, wm_down_maintain },
  { LbBtnT_DraggableBtn,BID_DEFAULT,    0, 0, wm_scroll_drag, NULL, NULL,                 0, 536, 163, 534, 163,  20, 154, wm_scroll_draw,               0, GUIStr_Empty, 0, {40}, 0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 102, 125, 102, 125, 220,  26, frontend_draw_text,           0, GUIStr_Empty, 0, {30}, 0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wm_row_click,   NULL, frontend_over_button, 0,  95, 157,  95, 157, 424,  22, wm_row_draw,                  0, GUIStr_Empty, 0, {45}, 0, wm_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wm_row_click,   NULL, frontend_over_button, 0,  95, 185,  95, 185, 424,  22, wm_row_draw,                  0, GUIStr_Empty, 0, {46}, 0, wm_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wm_row_click,   NULL, frontend_over_button, 0,  95, 213,  95, 213, 424,  22, wm_row_draw,                  0, GUIStr_Empty, 0, {47}, 0, wm_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wm_row_click,   NULL, frontend_over_button, 0,  95, 241,  95, 241, 424,  22, wm_row_draw,                  0, GUIStr_Empty, 0, {48}, 0, wm_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wm_row_click,   NULL, frontend_over_button, 0,  95, 269,  95, 269, 424,  22, wm_row_draw,                  0, GUIStr_Empty, 0, {49}, 0, wm_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wm_row_click,   NULL, frontend_over_button, 0,  95, 297,  95, 297, 424,  22, wm_row_draw,                  0, GUIStr_Empty, 0, {50}, 0, wm_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, frontend_change_state, NULL, frontend_over_button, FeSt_ENDLESS_MENU, 999, 404, 999, 404, 371, 46, wm_draw_back, 0, GUIStr_Empty, 0, {6}, 0, NULL },
  {-1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, GUIStr_Empty, 0, {0}, 0, NULL },
};

struct GuiMenu world_endless_load_menu =
 { GMnu_FEENDLESS_LOAD, 0, 1, wm_load_buttons, POS_SCRCTR, POS_SCRCTR, 640, 480, NULL, 0, NULL, NULL, 0, 0, 0,};

void world_menu_load_setup(void)
{
    wm_scroll = 0;
    wm_list_saves();
}

/******************************************************************************/
/* The World List (stage 3.5): the worlds on the hub (players, ping, title), an address box to join any world
   directly, and the menu chat with everyone's names. Joining is the -connect way: a running world takes the joiner
   as a spectator and then gives it a seat. */
#define WL_ROWS        5
#define WL_ROW_CONTENT 45  /* the rows' content values 45-49 (their hover ids) */
#define WL_NAME_LINES  5
#define WL_CHAT_LINES  5
/* a thin client left its world: the menu after the game is the World List, with why (empty: it chose to leave) */
static TbBool wl_return_pending = false;
static char wl_return_message[320] = "";
static int wl_scroll = 0;
static int wl_selected = -1;
static char wl_selected_key[WNET_HOST_MAX + 16];
static char wl_address[64] = "";
static char wl_say[WNET_TEXT_MAX] = "";
static char wl_name[sizeof(net_player_name)] = "";
static TbBool wl_say_reopen = false;
TbBool world_menu_open_on_start = false;

void world_menu_return_to_list(const char *message)
{
    wl_return_pending = true;
    snprintf(wl_return_message, sizeof(wl_return_message), "%s", (message != NULL) ? message : "");
    if (wl_return_message[0] != 0)
        SYNCLOG("World list: back with \"%s\"", wl_return_message);
}

TbBool world_menu_take_return_to_list(void)
{
    const TbBool pending = wl_return_pending;
    wl_return_pending = false;
    return pending;
}

/** Text in a part of a button: x and w in thousandths of its width, one line of its height. */
static void wl_text_in(const struct GuiButton *gbtn, int x1000, int w1000, int y, int h, int font_idx, unsigned int align, const char *text)
{
    LbTextSetFont(frontend_font[font_idx]);
    RendererSetDrawFlags(align);
    const int units = (h * 13 / 11) * 16 / LbTextLineHeight();
    LbTextSetWindow(gbtn->scr_pos_x + gbtn->width * x1000 / 1000, y, gbtn->width * w1000 / 1000, LbTextLineHeight() * units / 16);
    LbTextDrawResized(0, 0, units, text);
}

/** The name in the menu: -nick's (or the last network game's), else the one in the settings. */
static const char *wl_player_name(void)
{
    if (net_player_name[0] != '\0')
        return net_player_name;
    return net_config_info.net_player_name;
}

static void wl_key_of(const struct WnetServer *sv, char *key, size_t size)
{
    snprintf(key, size, "%s:%d", sv->host, sv->port);
}

static void wl_draw_title(struct GuiButton *gbtn)
{
    wm_draw_caption(gbtn, "World List");
}

static void wl_title_maintain(struct GuiButton *gbtn)
{
    if (wl_return_message[0] != '\0')
    {
        /* why the last world ended; opened once the list is up, so it is drawn on top of it */
        create_frontend_error_box(wl_return_message);
        wl_return_message[0] = '\0';
    }
    wnet_menu_poll();
    /* the selection follows its world when the list changes */
    wl_selected = -1;
    for (int i = 0; (i < wnet_menu_server_count()) && (wl_selected_key[0] != '\0'); i++)
    {
        char key[WNET_HOST_MAX + 16];
        wl_key_of(wnet_menu_server(i), key, sizeof(key));
        if (strcmp(key, wl_selected_key) == 0)
            wl_selected = i;
    }
    const int max_scroll = max(0, wnet_menu_server_count() - WL_ROWS);
    if (wl_scroll > max_scroll)
        wl_scroll = max_scroll;
    world_menu_test_tick();
}

static void wl_draw_status(struct GuiButton *gbtn)
{
    wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_LEFT, wnet_menu_status());
}


/* the columns, in thousandths of a row: players, ping, the world's name, and its address on the right */
#define WL_COL_PING   120
#define WL_COL_WORLD  225
#define WL_COL_ADDR   630

static void wl_draw_header(struct GuiButton *gbtn)
{
    wl_text_in(gbtn, 0, WL_COL_PING, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_LEFT, "Players");
    wl_text_in(gbtn, WL_COL_PING, WL_COL_WORLD - WL_COL_PING, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_LEFT, "Ping");
    wl_text_in(gbtn, WL_COL_WORLD, WL_COL_ADDR - WL_COL_WORLD, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_LEFT, "World");
    wl_text_in(gbtn, WL_COL_ADDR, 1000 - WL_COL_ADDR, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_RIGHT, "Address");
}

static int wl_row_index(const struct GuiButton *gbtn)
{
    return wl_scroll + (int)gbtn->content.lval - WL_ROW_CONTENT;
}

static void wl_row_draw(struct GuiButton *gbtn)
{
    const int i = wl_row_index(gbtn);
    const struct WnetServer *sv = wnet_menu_server(i);
    if (sv == NULL)
        return;
    int font_idx = frontend_button_caption_font(gbtn, frontend_mouse_over_button);
    if (i == wl_selected)
        font_idx = 2;
    char players[16], ping[16];
    snprintf(players, sizeof(players), "%d/%d", sv->players, sv->max);
    if (sv->ping_ms >= 0)
        snprintf(ping, sizeof(ping), "%d ms", sv->ping_ms);
    else
        snprintf(ping, sizeof(ping), "-");
    char addr[WNET_HOST_MAX + 16];
    if (strchr(sv->host, ':') != NULL)
        snprintf(addr, sizeof(addr), "[%s]:%d", sv->host, sv->port); /* IPv6 */
    else
        snprintf(addr, sizeof(addr), "%s:%d", sv->host, sv->port);
    wl_text_in(gbtn, 0, WL_COL_PING, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_LEFT, players);
    wl_text_in(gbtn, WL_COL_PING, WL_COL_WORLD - WL_COL_PING, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_LEFT, ping);
    wl_text_in(gbtn, WL_COL_WORLD, WL_COL_ADDR - WL_COL_WORLD, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_LEFT, sv->title);
    wl_text_in(gbtn, WL_COL_ADDR, 1000 - WL_COL_ADDR, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_RIGHT, addr);
}

static void wl_row_maintain(struct GuiButton *gbtn)
{
    if (wnet_menu_server(wl_row_index(gbtn)) != NULL)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

/** Back to the World List after a join that didn't work. The join's own error box ("Unable to join game") was
 *  opened before the list's menu, which would then be drawn over it: it is closed, and opened again on top once the
 *  list is up (wl_title_maintain, as for a world that ended). */
static void wl_back_to_list(void)
{
    if (menu_id_to_number(GMnu_FEERROR_BOX) >= 0)
    {
        snprintf(wl_return_message, sizeof(wl_return_message), "%s", gui_message_text);
        frontend_close_error_box(NULL);
    }
    frontend_set_state(FeSt_WORLD_LIST);
}

/** Joins the world at host[:port], the -connect way. */
static void wl_join(const char *address)
{
    if ((address == NULL) || (address[0] == '\0'))
        return;
    char addr[80];
    if ((strchr(address, ':') == NULL) || ((address[0] == '[') && (strstr(address, "]:") == NULL)))
        snprintf(addr, sizeof(addr), "%s:5556", address);
    else
        snprintf(addr, sizeof(addr), "%s", address);
    SYNCLOG("World list: joining %s", addr);
    wnet_menu_close();
    LbNetwork_ClearCmdLineSessions();
    LbNetwork_InitSessionsFromCmdLine(addr);
    if (!setup_network_service(FrontendNetSvc_Online))
    {
        wl_back_to_list();
        return;
    }
    frontnet_service_setup();
    frontnet_session_setup();
    net_number_of_sessions = 0;
    memset(net_session, 0, sizeof(net_session));
    if (LbNetwork_EnumerateSessions(enum_sessions_callback, 0) == 0)
    {
        net_session_index_active = 0;
        frontnet_session_join(NULL);
    }
    /* still on the session screen: the join didn't work (its message box says why); back to the list */
    if (frontend_menu_state == FeSt_NET_SESSION)
    {
        LbNetwork_Stop();
        wl_back_to_list();
    }
}

static void wl_join_server(int i)
{
    const struct WnetServer *sv = wnet_menu_server(i);
    if (sv == NULL)
        return;
    char addr[WNET_HOST_MAX + 16];
    if (strchr(sv->host, ':') != NULL)
        snprintf(addr, sizeof(addr), "[%s]:%d", sv->host, sv->port);
    else
        snprintf(addr, sizeof(addr), "%s:%d", sv->host, sv->port);
    wl_join(addr);
}

static void wl_row_click(struct GuiButton *gbtn)
{
    const int i = wl_row_index(gbtn);
    const struct WnetServer *sv = wnet_menu_server(i);
    if (sv == NULL)
        return;
    if (i == wl_selected)
    {
        wl_join_server(i); /* a second click on the chosen world joins it */
        return;
    }
    wl_selected = i;
    wl_key_of(sv, wl_selected_key, sizeof(wl_selected_key));
}

static void wl_up_maintain(struct GuiButton *gbtn)
{
    if (wl_scroll > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    if ((input_button == NULL) && (wheel_scrolled_up || is_key_pressed(KC_UP, KMod_NONE)) && (wl_scroll > 0))
        wl_scroll--;
}

static void wl_down_maintain(struct GuiButton *gbtn)
{
    const int max_scroll = wnet_menu_server_count() - WL_ROWS;
    if (wl_scroll < max_scroll)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    if ((input_button == NULL) && (wheel_scrolled_down || is_key_pressed(KC_DOWN, KMod_NONE)) && (wl_scroll < max_scroll))
        wl_scroll++;
}

static void wl_up(struct GuiButton *gbtn)
{
    if (wl_scroll > 0)
        wl_scroll--;
}

static void wl_down(struct GuiButton *gbtn)
{
    if (wl_scroll < wnet_menu_server_count() - WL_ROWS)
        wl_scroll++;
}

static void wl_test_scroll(int delta)
{
    wl_scroll = max(0, min(wl_scroll + delta, wnet_menu_server_count() - WL_ROWS));
}

static void wl_scroll_drag(struct GuiButton *gbtn)
{
    const int n = wnet_menu_server_count();
    wl_scroll = frontend_scroll_tab_to_offset(gbtn, GetMouseY(), WL_ROWS - 2, n);
    if (wl_scroll > n - WL_ROWS)
        wl_scroll = n - WL_ROWS;
    if (wl_scroll < 0)
        wl_scroll = 0;
}

static void wl_scroll_draw(struct GuiButton *gbtn)
{
    frontend_draw_scroll_tab(gbtn, wl_scroll, WL_ROWS - 2, wnet_menu_server_count());
}

/** A new name from the Name box (or a test): kept in the settings, and the hub hears it at once. */
static void wl_set_name(const char *name)
{
    if ((name == NULL) || (name[0] == '\0') || (strcmp(name, wl_player_name()) == 0))
    {
        snprintf(wl_name, sizeof(wl_name), "%s", wl_player_name());
        return;
    }
    snprintf(net_player_name, sizeof(net_player_name), "%s", name);
    snprintf(tmp_net_player_name, sizeof(tmp_net_player_name), "%s", name);
    snprintf(net_config_info.net_player_name, sizeof(net_config_info.net_player_name), "%s", name);
    save_settings();
    snprintf(wl_name, sizeof(wl_name), "%s", net_player_name);
    SYNCLOG("World list: the name is now %s", wl_name);
    wnet_menu_close();
    wnet_menu_open(wl_name);
}

static void wl_name_done(struct GuiButton *gbtn)
{
    char name[sizeof(wl_name)];
    snprintf(name, sizeof(name), "%s", wl_name);
    wl_set_name(name);
}

static void wl_draw_name_label(struct GuiButton *gbtn)
{
    wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y, gbtn->height, 1, Lb_TEXT_HALIGN_LEFT, "Name:");
}

static void wl_draw_address_label(struct GuiButton *gbtn)
{
    wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y, gbtn->height, 1, Lb_TEXT_HALIGN_LEFT, "Address:");
}

static void wl_address_done(struct GuiButton *gbtn)
{
    wl_join(wl_address);
}

static void wl_draw_connect(struct GuiButton *gbtn)
{
    frontend_draw_button(gbtn, 0, "Connect", Lb_TEXT_HALIGN_CENTER);
}

static void wl_connect(struct GuiButton *gbtn)
{
    wl_join(wl_address);
}

static void wl_connect_maintain(struct GuiButton *gbtn)
{
    if (wl_address[0] != '\0')
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

/** An entry box on the stock text bar: its text with a blinking cursor while typing, else a hint when empty. */
static void wl_draw_entry(struct GuiButton *gbtn)
{
    char text[WNET_TEXT_MAX + 8];
    int font_idx = 1;
    if (gbtn == input_button)
    {
        font_idx = 2;
        snprintf(text, sizeof(text), "%s%s", gbtn->content.str, ((LbTimerClock() / 200) & 1) ? "_" : "");
    } else if (gbtn->content.str[0] == '\0')
    {
        font_idx = 3;
        snprintf(text, sizeof(text), "%s", (gbtn->content.str == wl_say) ? "Click here to chat" :
            (gbtn->content.str == wl_name) ? "your name" : "host or host:port");
    } else
    {
        snprintf(text, sizeof(text), "%s", gbtn->content.str);
    }
    /* a long line shows its end, where the typing is */
    const size_t keep = (gbtn->content.str == wl_say) ? 60 : (gbtn->content.str == wl_name) ? 22 : 40;
    const size_t len = strlen(text);
    frontnet_draw_scroll_selection_box(gbtn, font_idx, (len > keep) ? text + (len - keep) : text);
}

static void wl_draw_names(struct GuiButton *gbtn)
{
    const int n = wnet_menu_name_count();
    const int line_h = gbtn->height / WL_NAME_LINES;
    for (int k = 0; (k < n) && (k < WL_NAME_LINES); k++)
    {
        char more[24];
        const char *text = wnet_menu_name(k);
        if ((k == WL_NAME_LINES - 1) && (n > WL_NAME_LINES))
        {
            snprintf(more, sizeof(more), "+%d more", n - k);
            text = more;
        }
        wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y + k * line_h, line_h, 1, Lb_TEXT_HALIGN_LEFT, text);
    }
}

static void wl_draw_chat(struct GuiButton *gbtn)
{
    /* the newest lines, at the bottom */
    const int n = wnet_menu_chat_count();
    const int line_h = gbtn->height / WL_CHAT_LINES;
    const int first = max(0, n - WL_CHAT_LINES);
    for (int k = first; k < n; k++)
    {
        const struct WnetChatLine *line = wnet_menu_chat_line(k);
        char text[WNET_NAME_MAX + WNET_TEXT_MAX + 4];
        snprintf(text, sizeof(text), "%s: %s", line->name, line->text);
        wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y + (k - first) * line_h, line_h, 1, Lb_TEXT_HALIGN_LEFT, text);
    }
}

static void wl_say_done(struct GuiButton *gbtn)
{
    wnet_menu_say(wl_say);
    wl_say[0] = '\0';
    wl_say_reopen = true; /* keep typing: the box opens again */
}

static void wl_say_maintain(struct GuiButton *gbtn)
{
    if (wl_say_reopen && (input_button == NULL))
    {
        wl_say_reopen = false;
        input_button = gbtn;
        LbStartTextInput();
        setup_input_field(gbtn, "");
    }
}

static void wl_draw_join(struct GuiButton *gbtn)
{
    frontend_draw_button(gbtn, 0, "Join", Lb_TEXT_HALIGN_CENTER);
}

static void wl_join_click(struct GuiButton *gbtn)
{
    if (wl_selected >= 0)
        wl_join_server(wl_selected);
    else
        wl_join(wl_address);
}

static void wl_join_maintain(struct GuiButton *gbtn)
{
    if ((wl_selected >= 0) || (wl_address[0] != '\0'))
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wl_draw_back(struct GuiButton *gbtn)
{
    frontend_draw_button(gbtn, 0, "Back", Lb_TEXT_HALIGN_CENTER);
}

static struct GuiButtonInit wl_buttons[] = {
  { LbBtnT_NormalBtn,   BID_MENU_TITLE, 0, 0, NULL,           NULL, NULL,                 0, 999,   4, 999,   4, 371,  46, wl_draw_title,          0, GUIStr_Empty, 0, {1},  0, wl_title_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  18,  57,  18,  57, 376,  16, wl_draw_status,         0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 400,  55, 400,  55,  56,  20, wl_draw_name_label,     0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_EditBox,     BID_DEFAULT,    0, 0, wl_name_done,   NULL, frontend_over_button, 0, 456,  52, 456,  52, 174,  24, wl_draw_entry,          0, GUIStr_Empty, 0, {.str = wl_name}, sizeof(wl_name) - 1, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  10,  78,  10,  78, 594, 142, frontnet_draw_lobby_panel, 0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wl_up,          NULL, frontend_over_button, 0, 604,  77, 604,  77,  26,  14, frontend_draw_slider_button, 0, GUIStr_Empty, 0, {17}, 0, wl_up_maintain },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wl_down,        NULL, frontend_over_button, 0, 604, 207, 604, 207,  26,  14, frontend_draw_slider_button, 0, GUIStr_Empty, 0, {18}, 0, wl_down_maintain },
  { LbBtnT_DraggableBtn,BID_DEFAULT,    0, 0, wl_scroll_drag, NULL, NULL,                 0, 608,  91, 608,  91,  20, 116, wl_scroll_draw,         0, GUIStr_Empty, 0, {40}, 0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  22,  81,  22,  81, 572,  21, wl_draw_header,         0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_row_click,   NULL, frontend_over_button, 0,  22, 103,  22, 103, 572,  22, wl_row_draw,            0, GUIStr_Empty, 0, {45}, 0, wl_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_row_click,   NULL, frontend_over_button, 0,  22, 126,  22, 126, 572,  22, wl_row_draw,            0, GUIStr_Empty, 0, {46}, 0, wl_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_row_click,   NULL, frontend_over_button, 0,  22, 149,  22, 149, 572,  22, wl_row_draw,            0, GUIStr_Empty, 0, {47}, 0, wl_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_row_click,   NULL, frontend_over_button, 0,  22, 172,  22, 172, 572,  22, wl_row_draw,            0, GUIStr_Empty, 0, {48}, 0, wl_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_row_click,   NULL, frontend_over_button, 0,  22, 195,  22, 195, 572,  22, wl_row_draw,            0, GUIStr_Empty, 0, {49}, 0, wl_row_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  14, 234,  14, 234,  80,  22, wl_draw_address_label,  0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_EditBox,     BID_DEFAULT,    0, 0, wl_address_done,NULL, frontend_over_button, 0,  96, 230,  96, 230, 282,  30, wl_draw_entry,          0, GUIStr_Empty, 0, {.str = wl_address}, sizeof(wl_address) - 1, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_connect,     NULL, frontend_over_button, 0, 384, 222, 384, 222, 247,  46, wl_draw_connect,        0, GUIStr_Empty, 0, {3},  0, wl_connect_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  10, 274,  10, 274, 140, 116, frontnet_draw_lobby_panel_without_scrollbar, 0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  20, 282,  20, 282, 122, 100, wl_draw_names,          0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 156, 274, 156, 274, 474, 116, frontnet_draw_lobby_panel_without_scrollbar, 0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 168, 282, 168, 282, 452, 100, wl_draw_chat,           0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_EditBox,     BID_DEFAULT,    0, 0, wl_say_done,    NULL, frontend_over_button, 0, 156, 394, 156, 394, 474,  28, wl_draw_entry,          0, GUIStr_Empty, 0, {.str = wl_say}, sizeof(wl_say) - 1, wl_say_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_join_click,  NULL, frontend_over_button, 0,  60, 428,  60, 428, 247,  46, wl_draw_join,           0, GUIStr_Empty, 0, {5},  0, wl_join_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, frontend_change_state, NULL, frontend_over_button, FeSt_ENDLESS_MENU, 333, 428, 333, 428, 247, 46, wl_draw_back, 0, GUIStr_Empty, 0, {6}, 0, NULL },
  {-1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, GUIStr_Empty, 0, {0}, 0, NULL },
};

struct GuiMenu world_list_menu =
 { GMnu_FEWORLD_LIST, 0, 1, wl_buttons, POS_SCRCTR, POS_SCRCTR, 640, 480, NULL, 0, NULL, NULL, 0, 0, 0,};

void world_list_menu_setup(void)
{
    wl_scroll = 0;
    wl_selected = -1;
    wl_selected_key[0] = '\0';
    wl_say[0] = '\0';
    wl_say_reopen = false;
    /* as the stock network screens do: a name of its own for a player who never set one, kept in the settings */
    if ((net_config_info.net_player_name[0] == '\0') || (strcmp(net_config_info.net_player_name, get_string(GUIStr_MnuNoName)) == 0))
    {
        snprintf(net_config_info.net_player_name, sizeof(net_config_info.net_player_name), "Keeper%03d", rand() % NET_DEFAULT_NAME_NUMBER_RANGE);
        save_settings();
    }
    if (net_player_name[0] == '\0')
    {
        snprintf(net_player_name, sizeof(net_player_name), "%s", net_config_info.net_player_name);
        snprintf(tmp_net_player_name, sizeof(tmp_net_player_name), "%s", net_config_info.net_player_name);
    }
    snprintf(wl_name, sizeof(wl_name), "%s", wl_player_name());
    wnet_menu_open(wl_name);
}

void world_list_menu_close(void)
{
    wnet_menu_close();
}

/******************************************************************************/
/* Autosave */

static TbBool wm_autosave_name_matches(const char *textname, const char *prefix)
{
    static const char suffix[] = " (auto)";
    size_t n = strlen(textname), np = strlen(prefix), ns = sizeof(suffix) - 1;
    return (n >= np + ns) && (strncmp(textname, prefix, np) == 0) && (strcmp(textname + n - ns, suffix) == 0);
}

/** The newest loadable autosave of a seed ("Endless <seed> d<day> (auto)"), or -1. */
int world_menu_slot_for_seed(const char *seed)
{
    if ((seed == NULL) || (seed[0] == '\0'))
        return -1;
    char prefix[SAVE_TEXTNAME_LEN];
    snprintf(prefix, sizeof(prefix), "Endless %.8s d", seed);
    wm_list_saves();
    for (int i = 0; i < wm_saves_count; i++)
    {
        const struct CatalogueEntry *centry = SaveManager_Entry(wm_saves[i]);
        if ((centry != NULL) && ((centry->flags & CEF_Unloadable) == 0) && wm_autosave_name_matches(centry->textname, prefix))
            return wm_saves[i];
    }
    return -1;
}

/** -loadseed with no save of the seed: a new world with it (the seed is set). Its frontend state. */
int world_menu_new_world_state(void)
{
    if (!change_campaign(CampgnT_Mappack, ENDLESS_MAPPACK_FNAME) || (campaign.freeplay_levels_count < 1)
        || (campaign.freeplay_levels[0] <= 0))
    {
        ERRORLOG("Endless Mode: can't load the map pack levels/%s", ENDLESS_MAPPACK_FNAME);
        server_admin_log("error", "The endless map pack levels/%s is missing", ENDLESS_MAPPACK_FNAME);
        return FeSt_MAIN_MENU;
    }
    world_menu_mp_host = false;
    game.selected_level_number = campaign.freeplay_levels[0];
    return FeSt_START_KPRLEVEL;
}

/** save/endless_worlds.txt: the endless saves (slot, seed, day, saved at, name), for keeperserver.exe's world list. */
void world_menu_write_index(void)
{
    wm_list_saves();
    FILE *f = fopen("save/endless_worlds.txt", "w");
    if (f == NULL)
        return;
    fprintf(f, "# slot\tseed\tday\tsaved (unix time)\tname: written by KeeperFX, read by keeperserver.exe\n");
    for (int i = 0; i < wm_saves_count; i++)
    {
        const struct CatalogueEntry *centry = SaveManager_Entry(wm_saves[i]);
        if (centry == NULL)
            continue;
        char seed[16] = "-";
        long day = 0;
        if (sscanf(centry->textname, "Endless %15s d%ld", seed, &day) < 1)
            snprintf(seed, sizeof(seed), "-");
        fprintf(f, "%d\t%s\t%ld\t%lld\t%s\n", wm_saves[i], seed, day, (long long)wm_slot_mtime(wm_saves[i]), centry->textname);
    }
    fclose(f);
}

void world_autosave(const char *why)
{
    if (!world_mode_active() || !world_menu_endless_campaign_loaded() || replay.load_enable)
        return;
    /* a network game: only the host keeps the run (every machine shares the save folder when tested on one PC) */
    if (network_is_active() && !network_is_host())
        return;
    struct PlayerInfo *player = get_my_player();
    if (player->victory_state == VicS_LostLevel)
        return;
    const char *seed = world_seed_text();
    long day = world_day_number();
    if (day > 9999)
        day = 9999;
    char prefix[SAVE_TEXTNAME_LEN];
    char name[SAVE_TEXTNAME_LEN];
    snprintf(prefix, sizeof(prefix), "Endless %.8s d", seed);
    snprintf(name, sizeof(name), "Endless %.8s d%ld (auto)", seed, day);
    SaveManager_LoadCatalogue();
    int slots = SaveManager_SlotCount();
    int slot = -1, free_slot = -1;
    for (int s = 0; s < slots; s++)
    {
        const struct CatalogueEntry *centry = SaveManager_Entry(s);
        if (centry == NULL)
            continue;
        if ((centry->flags & CEF_InUse) == 0)
        {
            if (free_slot < 0)
                free_slot = s;
            continue;
        }
        if (wm_is_endless_fname(centry->campaign_fname) && wm_autosave_name_matches(centry->textname, prefix))
        {
            slot = s;
            break;
        }
    }
    if (slot < 0)
        slot = (free_slot >= 0) ? free_slot : slots;
    if (!SaveManager_FillSlot(slot, name) || !SaveManager_Save(slot))
    {
        WARNLOG("Endless Mode: autosave (%s) to slot %d failed", why, slot);
        server_admin_log("error", "Saving the world (%s) to slot %d failed", why, slot);
        return;
    }
    SYNCLOG("Endless Mode: autosaved (%s) as \"%s\" in slot %d", why, name, slot);
    server_admin_log("save", "The world is saved (%s): \"%s\" in slot %d", why, name, slot);
    if (netstate_dedicated())
        world_menu_write_index();
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

/******************************************************************************/
/* Start again (2026-11-08): 5 seconds after this machine's keeper is defeated in an endless world, a box asks
   "Start again" (the seat is freed and the player gets a new start dungeon at the next spawn point: the start's gold
   and kit, level 1) or "Quit". The request goes to the server as a packet; the box comes back if nothing happened
   after a while (no place for a new start, a lost packet). */
#define WR_PROMPT_MS 5000   /* after the defeat */
#define WR_RETRY_MS 15000   /* asked again when the keeper hasn't started again by then */
static Uint64 wr_defeat_seen = 0;   /* when this machine saw its keeper defeated (0: it isn't) */
static Uint64 wr_asked_at = 0;      /* when "Start again" was clicked (0: not yet) */
static TbBool wr_shown = false;     /* the box was shown for this defeat */
static Uint64 wr_camera_until = 0;  /* started again: the camera goes to the new heart once it is here (until then) */
static char wr_title[] = "You have been defeated";
static char wr_again[] = "Start again";
static char wr_quit_text[] = "Quit";

static void wr_start_again(struct GuiButton *gbtn)
{
    (void)gbtn;
    struct PlayerInfo *player = get_my_player();
    SYNCLOG("World: this keeper asks to start again");
    wr_asked_at = SDL_GetTicks();
    set_players_packet_action(player, PckA_WorldRespawn, 0, 0, 0, 0);
}

static void wr_quit(struct GuiButton *gbtn)
{
    SYNCLOG("World: this defeated keeper quits");
    gui_quit_game(gbtn);
}

static struct GuiButtonInit world_respawn_buttons[] = {
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,           NULL, NULL, 0, 999, 12, 999, 12, 260, 32, gui_area_text, 2, GUIStr_Empty, 0, {.str = wr_title},     0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, wr_start_again, NULL, NULL, 0,  18, 58,  18, 58, 140, 32, gui_area_text, 1, GUIStr_Empty, 0, {.str = wr_again},     0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, wr_quit,        NULL, NULL, 0, 172, 58, 172, 58, 110, 32, gui_area_text, 1, GUIStr_Empty, 0, {.str = wr_quit_text}, 0, NULL },
  {              -1, BID_DEFAULT, 0, 0, NULL,           NULL, NULL, 0,   0,  0,   0,  0,   0,  0, NULL,          0, 0,            0, {0},                 0, NULL },
};

struct GuiMenu world_respawn_menu =
 { GMnu_WORLD_RESPAWN, 0, 1, world_respawn_buttons, POS_GAMECTR, POS_GAMECTR, 300, 104, gui_pretty_background, 0, NULL, NULL, 0, 1, 0,};

void world_respawn_prompt_turn(void)
{
    if (!world_mode_active() || netstate_dedicated() || replay.load_enable || observer_is_active())
        return;
    struct PlayerInfo *player = get_my_player();
    const TbBool lost = player_exists(player) && (player->victory_state == VicS_LostLevel);
    const Uint64 now = SDL_GetTicks();
    if (!lost)
    {
        if (wr_defeat_seen != 0)
        {
            /* started again (or a load): the box goes, the panel comes back */
            if (menu_is_active(GMnu_WORLD_RESPAWN))
                turn_off_menu(GMnu_WORLD_RESPAWN);
            if (wr_shown)
            {
                SYNCLOG("World: this keeper has started again");
                set_gui_visible(true);
                turn_on_menu(GMnu_MAIN);
                turn_on_menu(GMnu_ROOM);
                wr_camera_until = now + 10000;
            }
        }
        wr_defeat_seen = 0;
        wr_asked_at = 0;
        wr_shown = false;
        /* the new heart may come a turn or two after the news (a thin client): the camera goes when it is here */
        if ((wr_camera_until != 0) && ((now > wr_camera_until) || world_local_camera_to_heart()))
            wr_camera_until = 0;
        return;
    }
    if (wr_defeat_seen == 0)
        wr_defeat_seen = now;
    if (menu_is_active(GMnu_WORLD_RESPAWN))
    {
        /* test hook KFX_WORLD_RESPAWN=<ms>: "Start again" clicks itself that long after the box came up */
        static Uint64 shown_at = 0;
        const char *auto_ms = getenv("KFX_WORLD_RESPAWN");
        if (shown_at == 0)
            shown_at = now;
        if ((auto_ms != NULL) && (now - shown_at >= (Uint64)atoi(auto_ms)) && (wr_asked_at == 0))
        {
            SYNCLOG("World test: \"Start again\" clicks itself");
            shown_at = 0;
            turn_off_menu(GMnu_WORLD_RESPAWN);
            wr_start_again(NULL);
        }
        return;
    }
    if (now - wr_defeat_seen < WR_PROMPT_MS)
        return;
    if ((wr_asked_at != 0) && (now - wr_asked_at < WR_RETRY_MS))
        return; /* waiting for the server */
    if (wr_asked_at != 0)
        SYNCLOG("World: still defeated %d s after asking to start again: asking again", (int)((now - wr_asked_at) / 1000));
    else
        SYNCLOG("World: this keeper was defeated: \"Start again\" or \"Quit\"");
    wr_asked_at = 0;
    wr_shown = true;
    turn_on_menu(GMnu_WORLD_RESPAWN);
}
/******************************************************************************/

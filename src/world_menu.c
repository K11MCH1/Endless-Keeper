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
#include "vidfade.h"
#include "bflib_video.h"
#include "custom_sprites.h"
#include "bflib_vidraw.h"
#include "sprites.h"
#include "bflib_inputctrl.h"
#include "config_settings.h"
#include "server_admin.h"
#include "net_state.h"
#include "packets.h"
#include "observer.h"
#include "gui_parchment.h"
#include "frontmenu_ingame_tabs.h"
#include "local_camera.h"
#include "world_attract.h"
#include "chat_box.h"
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

/** A dedicated server's autosave ("Server <seed> dN (auto)"): the player's Continue leaves it alone. */
static TbBool wm_is_server_save(const struct CatalogueEntry *centry)
{
    return (centry != NULL) && (strncmp(centry->textname, "Server ", 7) == 0);
}

/** The newest endless save that isn't known to be unloadable, or -1. The player's Continue skips a dedicated
 *  server's autosaves (they share the save folder: Continue opened the server's world as its first joiner, and the
 *  single-player copy overwrote the server's slot; audit 2026-11-09); a server's own takes its newest one first. */
static int wm_continue_slot_for(TbBool server)
{
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < wm_saves_count; i++)
        {
            const struct CatalogueEntry *centry = SaveManager_Entry(wm_saves[i]);
            if ((centry == NULL) || ((centry->flags & CEF_Unloadable) != 0))
                continue;
            if (!server && wm_is_server_save(centry))
                continue;
            if (server && (pass == 0) && !wm_is_server_save(centry))
                continue;
            return wm_saves[i];
        }
        if (!server)
            break;
    }
    return -1;
}

static int wm_continue_slot(void)
{
    return wm_continue_slot_for(false);
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
static TbBool wm_run_started = false; /* the last New Run got as far as starting the level */
static void wl_join_server(int i);
static void wl_join(const char *address);
static TbBool wl_join_plain;
static uint64_t wl_follow_key;
static const char *wl_player_name(void);
static void wl_test_scroll(int delta);
static void wl_set_name(const char *name);
static void wl_server_apply(void);
static void wm_host(struct GuiButton *gbtn);
static void wm_join(struct GuiButton *gbtn);
static void wl_invite_click(struct GuiButton *gbtn);
static void wl_leave_click(struct GuiButton *gbtn);
static void wp_answer(TbBool accept);
/* the player chosen in the names box (by name: the list changes as people come and go), and its scroll */
static char wl_party_sel[WNET_NAME_MAX] = "";
static char wl_server[WNET_URL_MAX] = "";  /* the Server box: the hub's address as shown (host, host:port or a URL) */
static int wl_names_scroll = 0;
static int wl_chat_back = 0;              /* chat lines scrolled back from the newest (0: following the newest) */
static unsigned long wl_chat_seen = 0;    /* the chat serial last seen: new lines keep a scrolled-back view still */
static int wl_names_max_scroll(void);
static int wl_chat_max_back(void);
static char wp_text[WNET_NAME_MAX + 32] = ""; /* the invite box's question */

/** The player chose Host Network Game: the lobby's Start begins the endless world. */
TbBool world_menu_mp_host = false;

/** Test hook: KFX_WORLD_MENU="E,N" presses the Endless Mode buttons by themselves, one step every 2.5 s
 *  while one of the menus (main, Endless Mode, endless saves, World List) is up: E Endless Mode, M main menu,
 *  L Load Game, N New Run, C Continue, R<n> the n-th row of the save list (0 = newest), H Host (a new run
 *  open to the network), J Join (the World List), W the World List, P<n> a screenshot of the frame, T<n> a chat
 *  line, G<n> join the n-th world of the list, A join 127.0.0.1:5556, X<port> join 127.0.0.1:<port> (a failed join), D<n>/U<n> scroll the list, K<n> the name becomes Tester<n>,
 *  Q<n> choose the n-th name in the names box other than this player's own (0: the first), I click "Invite to party",
 *  Y accept the invite, Z refuse it, V leave the party, F<n> scroll the names box by n, B<n> scroll the chat back by n
 *  (negative: forward), h<port> the Server box becomes 127.0.0.1:<port> (h0: empty, the default hub), anything else
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
    case 'Q':
        for (int k = 0, other = 0; k < wnet_menu_name_count(); k++)
        {
            if (strcmp(wnet_menu_name(k), wnet_menu_my_name()) == 0)
                continue;
            if (other++ == arg)
            {
                snprintf(wl_party_sel, sizeof(wl_party_sel), "%s", wnet_menu_name(k));
                break;
            }
        }
        break;
    case 'I': wl_invite_click(NULL); break;
    case 'Y': wp_answer(true); break;
    case 'Z': wp_answer(false); break;
    case 'V': wl_leave_click(NULL); break;
    case 'F': wl_names_scroll = max(0, min(wl_names_scroll + arg, wl_names_max_scroll())); break;
    case 'B': wl_chat_back = max(0, min(wl_chat_back + arg, wl_chat_max_back())); break;
    case 'h':
        if (arg > 0)
            snprintf(wl_server, sizeof(wl_server), "127.0.0.1:%d", arg);
        else
            wl_server[0] = '\0';
        wl_server_apply();
        break;
    case 'w':
    {
        /* the Server box typed with KFX_WORLD_BOXTEXT (any text: a host, a URL) */
        const char *t = getenv("KFX_WORLD_BOXTEXT");
        snprintf(wl_server, sizeof(wl_server), "%s", (t != NULL) ? t : "");
        wl_server_apply();
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

/* The main menu's Continue (Endless Keeper, 2026-11-10): the newest Endless Mode save. */
void world_menu_main_continue_maintain(struct GuiButton *gbtn)
{
    static Uint64 listed_at = 0;
    if ((listed_at == 0) || (SDL_GetTicks() - listed_at >= 2000))
    {
        listed_at = SDL_GetTicks() | 1;
        wm_list_saves();
    }
    wm_continue_maintain(gbtn);
}

void world_menu_main_continue(struct GuiButton *gbtn)
{
    wm_list_saves();
    wm_continue(gbtn);
}

int world_menu_newest_endless_slot(void)
{
    wm_list_saves();
    return wm_continue_slot_for(true); /* -loadslot continue: a dedicated server's own world */
}

static void wm_host(struct GuiButton *gbtn)
{
    /* a new run that opens to the network at once: others join it from the world list (no lobby) */
    SYNCLOG("Endless Mode: hosting a network run");
    wm_new_run(NULL);
    world_menu_open_on_start = wm_run_started; /* a run that never started doesn't open the next one (audit) */
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
    wm_run_started = false;
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
    wm_run_started = true;
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
/* The World List (stage 3.5): the worlds on the hub (title, players, ping, address) and the menu chat with everyone's
   names, where players group up into parties. Joining is the -connect way: a running world takes the joiner as a
   spectator and then gives it a seat. (A world that isn't listed is joined by address from Multiplayer > Online/LAN:
   the World List's own address box went, 2026-11-09.) */
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
    /* an invite: "Join <name> to party?" over the list until it is answered (or ends) */
    const char *from = wnet_menu_invite_from();
    if ((from[0] != '\0') && (menu_id_to_number(GMnu_FEPARTY_INVITE) < 0))
    {
        snprintf(wp_text, sizeof(wp_text), "Join %s to party?", from);
        if (game_is_busy_doing_gui_string_input())
            kill_button_area_input();
        turn_on_menu(GMnu_FEPARTY_INVITE);
    } else if ((from[0] == '\0') && (menu_id_to_number(GMnu_FEPARTY_INVITE) >= 0))
        turn_off_menu(GMnu_FEPARTY_INVITE);
    /* the chosen player stays chosen while they are in the list (and not in this player's party) */
    TbBool listed = false;
    for (int k = 0; (k < wnet_menu_name_count()) && (wl_party_sel[0] != '\0') && !listed; k++)
        listed = (strcmp(wnet_menu_name(k), wl_party_sel) == 0);
    if (!listed || (strcmp(wl_party_sel, wnet_menu_my_name()) == 0) || (wnet_menu_party()[0] != '\0'))
        wl_party_sel[0] = '\0';
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
    {
        /* co-op: the partner entered a world: this game follows it there */
        uint64_t key = 0;
        const int row = wnet_menu_party_follow(&key);
        if (row >= 0)
        {
            if (game_is_busy_doing_gui_string_input())
                kill_button_area_input();
            if (menu_id_to_number(GMnu_FEPARTY_INVITE) >= 0)
                turn_off_menu(GMnu_FEPARTY_INVITE);
            wl_follow_key = key;
            wl_join_server(row);
            return;
        }
    }
    world_menu_test_tick();
}

/** A Cloudflare Worker's host (like the public hub's): it is reached only over wss:// on the usual port, so the
 *  Server box shows it without "wss://" and "/hub", and takes it back that way. */
static TbBool wl_is_workers_host(const char *host, size_t len)
{
    static const char suffix[] = ".workers.dev";
    const size_t sn = sizeof(suffix) - 1;
    if ((len <= sn) || (memchr(host, ':', len) != NULL) || (memchr(host, '/', len) != NULL))
        return false;
    return (SDL_strncasecmp(host + len - sn, suffix, sn) == 0);
}

/** The hub's URL for what was typed in the Server box: "host" or "host:port" (port 5560 when none; IPv6 in [ ]),
 *  a *.workers.dev host (wss://host/hub), or a ws:// / wss:// address as it is. "" for an empty box. */
static void wl_hub_url_of(const char *typed, char *url, size_t size)
{
    char t[WNET_URL_MAX];
    while (*typed == ' ')
        typed++;
    snprintf(t, sizeof(t), "%s", typed);
    for (size_t n = strlen(t); (n > 0) && (t[n - 1] == ' '); n--)
        t[n - 1] = '\0';
    const char *first_colon = strchr(t, ':');
    if (t[0] == '\0')
        url[0] = '\0';
    else if ((strncmp(t, "ws://", 5) == 0) || (strncmp(t, "wss://", 6) == 0))
        snprintf(url, size, "%s", t);
    else if (t[0] == '[')
        snprintf(url, size, (strstr(t, "]:") != NULL) ? "ws://%s/hub" : "ws://%s:5560/hub", t);
    else if (wl_is_workers_host(t, strlen(t)))
        snprintf(url, size, "wss://%s/hub", t);
    else if (first_colon == NULL)
        snprintf(url, size, "ws://%s:%d/hub", t, WNET_HUB_PORT);
    else if (strchr(first_colon + 1, ':') == NULL)
        snprintf(url, size, "ws://%s/hub", t); /* host:port */
    else
        snprintf(url, size, "ws://[%s]:%d/hub", t, WNET_HUB_PORT); /* a bare IPv6 address */
}

/** What the Server box shows for a hub's URL: the host (with :port when it isn't 5560), a *.workers.dev host alone,
 *  else the whole URL. */
static void wl_hub_short(const char *url, char *out, size_t size)
{
    const size_t n = strlen(url);
    if ((strncmp(url, "wss://", 6) == 0) && (n > 10) && (strcmp(url + n - 4, "/hub") == 0) &&
        wl_is_workers_host(url + 6, n - 10))
    {
        snprintf(out, size, "%.*s", (int)(n - 10), url + 6);
        return;
    }
    if ((strncmp(url, "ws://", 5) == 0) && (n > 9) && (strcmp(url + n - 4, "/hub") == 0))
    {
        char core[WNET_URL_MAX];
        snprintf(core, sizeof(core), "%.*s", (int)(n - 9), url + 5);
        const size_t c = strlen(core);
        if ((c > 5) && (strcmp(core + c - 5, ":5560") == 0))
            core[c - 5] = '\0';
        snprintf(out, size, "%s", core);
    } else
        snprintf(out, size, "%s", url);
}

/** The Server box's address becomes the World List's hub (kept in the settings), and the menu connects to it. */
static void wl_server_apply(void)
{
    char url[WNET_URL_MAX];
    wl_hub_url_of(wl_server, url, sizeof(url));
    const char *now = wnet_menu_hub();
    const char *next = (url[0] != '\0') ? url : world_list_server;
    const TbBool moved = (strcmp(now, next) != 0);
    /* an emptied box forgets the address even when it is the default's: a later default then applies */
    if (moved || (strcmp(wnet_menu_hub_setting(), url) != 0))
    {
        wnet_menu_set_hub(url);
        save_settings();
        if (moved)
        {
            SYNCLOG("World list: the hub is now %s", next);
            wnet_menu_close();
            wnet_menu_open(wl_name);
        }
    }
    wl_hub_short(wnet_menu_hub(), wl_server, sizeof(wl_server));
}

static void wl_server_done(struct GuiButton *gbtn)
{
    wl_server_apply();
}

/** A label beside an entry box, on the box's row and as tall as it: its text the size and height of the box's
 *  (frontnet_draw_scroll_selection_box), so the two line up. */
static void wl_draw_box_label(struct GuiButton *gbtn, const char *text)
{
    LbTextSetFont(frontend_font[1]);
    RendererSetDrawFlags(Lb_TEXT_HALIGN_LEFT);
    const int units = (gbtn->height * 13 / 14) * 16 / LbTextLineHeight();
    LbTextSetWindow(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, LbTextLineHeight() * units / 16);
    LbTextDrawResized(0, 0, units, text);
}

static void wl_draw_server_label(struct GuiButton *gbtn)
{
    wl_draw_box_label(gbtn, "Server:");
}


/* the columns, in thousandths of a row: the world's name, players in the middle, and the ping on the right (the
   user, 2026-11-10: the address column went, the ping took its place) */
#define WL_COL_PLAYERS 400
#define WL_COL_PING    600

static void wl_draw_header(struct GuiButton *gbtn)
{
    wl_text_in(gbtn, 0, WL_COL_PLAYERS, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_LEFT, "World");
    wl_text_in(gbtn, WL_COL_PLAYERS, WL_COL_PING - WL_COL_PLAYERS, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_CENTER, "Players");
    wl_text_in(gbtn, WL_COL_PING, 1000 - WL_COL_PING, gbtn->scr_pos_y, gbtn->height, 3, Lb_TEXT_HALIGN_RIGHT, "Ping");
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
    wl_text_in(gbtn, 0, WL_COL_PLAYERS, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_LEFT, sv->title);
    wl_text_in(gbtn, WL_COL_PLAYERS, WL_COL_PING - WL_COL_PLAYERS, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_CENTER, players);
    wl_text_in(gbtn, WL_COL_PING, 1000 - WL_COL_PING, gbtn->scr_pos_y, gbtn->height, font_idx, Lb_TEXT_HALIGN_RIGHT, ping);
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
    if (wl_join_plain)
        net_party_key = 0; /* a join by address: no party goes along */
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

/* the party's key for the join about to be made (the partner's "go" gave it; 0: this player's own click) */
static uint64_t wl_follow_key = 0;
static TbBool wl_join_plain = true; /* wl_join called on its own (not for a row of the list) */

static void wl_join_server(int i)
{
    const struct WnetServer *sv = wnet_menu_server(i);
    if (sv == NULL)
        return;
    /* co-op: in a party, the partner follows into the same world, and both logins carry the party's key */
    net_party_key = (wl_follow_key != 0) ? wl_follow_key : wnet_menu_party_go(sv->id);
    wl_follow_key = 0;
    if (net_party_key != 0)
        SYNCLOG("World list: joining \"%s\" with the party", sv->title);
    char addr[WNET_HOST_MAX + 16];
    if (strchr(sv->host, ':') != NULL)
        snprintf(addr, sizeof(addr), "[%s]:%d", sv->host, sv->port);
    else
        snprintf(addr, sizeof(addr), "%s:%d", sv->host, sv->port);
    wl_join_plain = false;
    wl_join(addr);
    wl_join_plain = true;
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

/* where the names box and the chat are on the screen, each with its scrollbar (left, top, right, bottom; from their
   last maintain): the wheel over one scrolls it, anywhere else the world list */
static int wl_names_box[4];
static int wl_chat_box[4];
static TbBool wl_mouse_in(const int box[4])
{
    const int x = GetMouseX(), y = GetMouseY();
    return (x >= box[0]) && (x < box[2]) && (y >= box[1]) && (y < box[3]);
}
static TbBool wl_mouse_over_names(void) { return wl_mouse_in(wl_names_box); }
static TbBool wl_mouse_over_chat(void) { return wl_mouse_in(wl_chat_box); }
static void wl_box_from_panel(int box[4], const struct GuiButton *gbtn)
{
    box[0] = gbtn->scr_pos_x;
    box[1] = gbtn->scr_pos_y;
    box[3] = gbtn->scr_pos_y + gbtn->height;
    if (box[2] < box[0] + gbtn->width)
        box[2] = box[0] + gbtn->width;
}
static void wl_names_panel_maintain(struct GuiButton *gbtn) { wl_box_from_panel(wl_names_box, gbtn); }
static void wl_chat_panel_maintain(struct GuiButton *gbtn) { wl_box_from_panel(wl_chat_box, gbtn); }

static void wl_up_maintain(struct GuiButton *gbtn)
{
    if (wl_scroll > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    if ((input_button == NULL) && ((wheel_scrolled_up && !wl_mouse_over_names() && !wl_mouse_over_chat()) || is_key_pressed(KC_UP, KMod_NONE)) && (wl_scroll > 0))
        wl_scroll--;
}

static void wl_down_maintain(struct GuiButton *gbtn)
{
    const int max_scroll = wnet_menu_server_count() - WL_ROWS;
    if (wl_scroll < max_scroll)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    if ((input_button == NULL) && ((wheel_scrolled_down && !wl_mouse_over_names() && !wl_mouse_over_chat()) || is_key_pressed(KC_DOWN, KMod_NONE)) && (wl_scroll < max_scroll))
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

/** A list's scroll tab in its track: at the top for offset 0, at the bottom for max_offset. */
static void wl_draw_tab(struct GuiButton *gbtn, int offset, int max_offset)
{
    const int units_per_px = simple_frontend_sprite_width_units_per_px(gbtn, GFS_slider_indicator_std, 100);
    const struct TbSprite *spr = get_frontend_sprite(GFS_slider_indicator_std);
    const int room = gbtn->height - spr->SHeight * units_per_px / 16;
    const int y = (max_offset > 0) ? (int)((long)max(0, min(offset, max_offset)) * room / max_offset) : 0;
    LbSpriteDrawResized(gbtn->scr_pos_x, gbtn->scr_pos_y + y, units_per_px, spr);
}

/** The offset (0..max_offset) for the tab dragged to the mouse. */
static int wl_tab_offset(struct GuiButton *gbtn, int max_offset)
{
    const int units_per_px = simple_frontend_sprite_width_units_per_px(gbtn, GFS_slider_indicator_std, 100);
    const struct TbSprite *spr = get_frontend_sprite(GFS_slider_indicator_std);
    const int tab_h = spr->SHeight * units_per_px / 16;
    const int room = gbtn->height - tab_h;
    if ((max_offset <= 0) || (room <= 0))
        return 0;
    const int rel = max(0, min(GetMouseY() - gbtn->scr_pos_y - tab_h / 2, room));
    return (rel * max_offset + room / 2) / room;
}

static void wl_scroll_drag(struct GuiButton *gbtn)
{
    wl_scroll = wl_tab_offset(gbtn, max(0, wnet_menu_server_count() - WL_ROWS));
}

static void wl_scroll_draw(struct GuiButton *gbtn)
{
    wl_draw_tab(gbtn, wl_scroll, max(0, wnet_menu_server_count() - WL_ROWS));
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
    wl_draw_box_label(gbtn, "Name:");
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
    } else if (gbtn->content.str == wl_server)
    {
        /* the hub's address, and whether the World List reached it */
        const int state = wnet_menu_state();
        font_idx = (state == 1) ? 1 : 3;
        snprintf(text, sizeof(text), "%s%s", gbtn->content.str, (state == 1) ? "" : (state == 0) ? "  (connecting...)" : "  (can't reach)");
    } else if (gbtn->content.str[0] == '\0')
    {
        font_idx = 3;
        snprintf(text, sizeof(text), "%s", (gbtn->content.str == wl_say) ? "Click here to chat" : "your name");
    } else
    {
        snprintf(text, sizeof(text), "%s", gbtn->content.str);
    }
    /* a long line shows its end, where the typing is */
    const size_t keep = (gbtn->content.str == wl_say) ? 60 : (gbtn->content.str == wl_server) ? 44 : 22;
    const size_t len = strlen(text);
    frontnet_draw_scroll_selection_box(gbtn, font_idx, (len > keep) ? text + (len - keep) : text);
}

/** The font recoloured green, for the other player in this player's party. The front end's palette has one green
 *  (the rest are its own reds, browns and greys): each colour goes to the palette colour nearest a green as bright,
 *  so the letters' bright middle turns green and their dark edges stay dark. */
static const unsigned char *wl_green_remap(void)
{
    static unsigned char remap[256];
    static TbBool made = false;
    if (!made)
    {
        for (int i = 0; i < 256; i++)
        {
            const unsigned char *c = &frontend_palette[3 * i];
            const int v = max(c[0], max(c[1], c[2]));
            remap[i] = LbPaletteFindColour(frontend_palette, v / 4, v, v / 8);
        }
        made = true;
    }
    return remap;
}

/** The names box line under the mouse (-1: none). */
static int wl_names_line_at_mouse(const struct GuiButton *gbtn)
{
    const int line_h = max(1, gbtn->height / WL_NAME_LINES);
    const int x = GetMouseX(), y = GetMouseY();
    if ((x < gbtn->scr_pos_x) || (x >= gbtn->scr_pos_x + gbtn->width) || (y < gbtn->scr_pos_y))
        return -1;
    const int k = (y - gbtn->scr_pos_y) / line_h;
    return (k < WL_NAME_LINES) ? k : -1;
}

static int wl_names_max_scroll(void)
{
    return max(0, wnet_menu_name_count() - WL_NAME_LINES);
}

static void wl_draw_names(struct GuiButton *gbtn)
{
    const int n = wnet_menu_name_count();
    const int line_h = gbtn->height / WL_NAME_LINES;
    const int hover = (first_monopoly_menu() < 0) ? wl_names_line_at_mouse(gbtn) : -1;
    for (int k = 0; (k < WL_NAME_LINES) && (wl_names_scroll + k < n); k++)
    {
        const int i = wl_names_scroll + k;
        const char *name = wnet_menu_name(i);
        int font_idx = 1;
        const unsigned char *remap = NULL;
        const char *text = name;
        if ((wnet_menu_party()[0] != '\0') && (strcmp(name, wnet_menu_party()) == 0))
        {
            remap = wl_green_remap(); /* the other player in this one's party */
        } else if ((strcmp(name, wl_party_sel) == 0) || ((k == hover) && (strcmp(name, wnet_menu_my_name()) != 0)))
        {
            font_idx = 2; /* chosen, or under the mouse (this player's own name can't be chosen) */
        }
        const unsigned char *old_remap = LbTextGetRemap();
        if (remap != NULL)
            LbTextSetRemap(remap);
        wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y + k * line_h, line_h, font_idx, Lb_TEXT_HALIGN_LEFT, text);
        if (remap != NULL)
            LbTextSetRemap(old_remap);
    }
}

/** A click on a name chooses that player (a second click lets go). */
static void wl_names_click(struct GuiButton *gbtn)
{
    const int k = wl_names_line_at_mouse(gbtn);
    const int i = wl_names_scroll + k;
    if ((k < 0) || (i >= wnet_menu_name_count()))
        return;
    const char *name = wnet_menu_name(i);
    if ((strcmp(name, wnet_menu_my_name()) == 0) || (strcmp(name, wnet_menu_party()) == 0))
        return;
    if (strcmp(name, wl_party_sel) == 0)
        wl_party_sel[0] = '\0';
    else
        snprintf(wl_party_sel, sizeof(wl_party_sel), "%s", name);
}

static void wl_names_maintain(struct GuiButton *gbtn)
{
    const int n = wnet_menu_name_count();
    if (n > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
    const int max_scroll = wl_names_max_scroll();
    if ((input_button == NULL) && (first_monopoly_menu() < 0) && wl_mouse_over_names())
    {
        if (wheel_scrolled_up && (wl_names_scroll > 0))
            wl_names_scroll--;
        if (wheel_scrolled_down && (wl_names_scroll < max_scroll))
            wl_names_scroll++;
    }
    if (wl_names_scroll > max_scroll)
        wl_names_scroll = max_scroll;
}

static void wl_names_up(struct GuiButton *gbtn)
{
    if (wl_names_scroll > 0)
        wl_names_scroll--;
}

static void wl_names_down(struct GuiButton *gbtn)
{
    if (wl_names_scroll < wl_names_max_scroll())
        wl_names_scroll++;
}

static void wl_names_up_maintain(struct GuiButton *gbtn)
{
    wl_names_box[2] = gbtn->scr_pos_x + gbtn->width; /* the box's right edge: its scrollbar's */
    if (wl_names_scroll > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wl_names_down_maintain(struct GuiButton *gbtn)
{
    if (wl_names_scroll < wl_names_max_scroll())
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wl_names_drag(struct GuiButton *gbtn)
{
    wl_names_scroll = wl_tab_offset(gbtn, wl_names_max_scroll());
}

static void wl_names_tab_draw(struct GuiButton *gbtn)
{
    wl_draw_tab(gbtn, wl_names_scroll, wl_names_max_scroll());
}

/** A full-size button (as Join and Back) with its caption at 70%, as frontend_draw_button places one. */
static void wl_draw_small_caption_button(struct GuiButton *gbtn, const char *text)
{
    frontend_draw_button(gbtn, 0, NULL, Lb_TEXT_HALIGN_CENTER);
    const int font_idx = ((gbtn->flags & LbBtnF_Enabled) == 0) ? 3 : frontend_button_caption_font(gbtn, frontend_mouse_over_button);
    const int units_per_px = simple_frontend_sprite_height_units_per_px(gbtn, GFS_hugebutton_a05l, 100);
    const int text_units = units_per_px * 7 / 10;
    const struct TbSprite *spr = get_frontend_sprite(GFS_hugebutton_a05l);
    RendererSetDrawFlags(Lb_TEXT_HALIGN_CENTER);
    LbTextSetFont(frontend_font[font_idx]);
    const int h = LbTextHeight(text) * text_units / 16;
    const int x = gbtn->scr_pos_x + ((40 * units_per_px / 16) >> 1);
    const int y = gbtn->scr_pos_y + ((spr->SHeight * units_per_px / 16 - h) >> 1);
    LbTextSetWindow(x, y, gbtn->width - 40 * units_per_px / 16, h);
    LbTextDrawResized(0, 0, text_units, text);
}

static void wl_draw_invite(struct GuiButton *gbtn)
{
    wl_draw_small_caption_button(gbtn, "Invite to party");
}

static void wl_draw_leave(struct GuiButton *gbtn)
{
    wl_draw_small_caption_button(gbtn, "Leave party");
}

static void wl_invite_click(struct GuiButton *gbtn)
{
    wnet_menu_party_invite(wl_party_sel);
}

static void wl_leave_click(struct GuiButton *gbtn)
{
    wnet_menu_party_leave();
}

/** "Invite to party": a player is chosen, this one is in no party (only parties of two), and no invite is out. */
static void wl_invite_maintain(struct GuiButton *gbtn)
{
    if (wnet_menu_connected() && (wl_party_sel[0] != '\0') && (wnet_menu_party()[0] == '\0') && (wnet_menu_invite_to()[0] == '\0'))
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wl_leave_maintain(struct GuiButton *gbtn)
{
    if (wnet_menu_party()[0] != '\0')
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static int wl_chat_max_back(void)
{
    return max(0, wnet_menu_chat_count() - WL_CHAT_LINES);
}

static void wl_draw_chat(struct GuiButton *gbtn)
{
    /* the newest lines at the bottom, or older ones when scrolled back */
    const int n = wnet_menu_chat_count();
    const int line_h = gbtn->height / WL_CHAT_LINES;
    const int first = max(0, n - WL_CHAT_LINES - wl_chat_back);
    for (int k = first; (k < n) && (k < first + WL_CHAT_LINES); k++)
    {
        const struct WnetChatLine *line = wnet_menu_chat_line(k);
        char text[WNET_NAME_MAX + WNET_TEXT_MAX + 4];
        if (line->name[0] != '\0')
            snprintf(text, sizeof(text), "%s: %s", line->name, line->text);
        else
            snprintf(text, sizeof(text), "%s", line->text); /* a note of this game's own (the party) */
        wl_text_in(gbtn, 0, 1000, gbtn->scr_pos_y + (k - first) * line_h, line_h, 1, Lb_TEXT_HALIGN_LEFT, text);
    }
}

static void wl_chat_maintain(struct GuiButton *gbtn)
{
    /* new lines: a view scrolled back stays on the same lines (it moves back by as many) */
    const unsigned long serial = wnet_menu_chat_serial();
    if (serial != wl_chat_seen)
    {
        if (wl_chat_back > 0)
            wl_chat_back += (int)(serial - wl_chat_seen);
        wl_chat_seen = serial;
    }
    if ((input_button == NULL) && (first_monopoly_menu() < 0) && wl_mouse_over_chat())
    {
        if (wheel_scrolled_up)
            wl_chat_back++;
        if (wheel_scrolled_down && (wl_chat_back > 0))
            wl_chat_back--;
    }
    wl_chat_back = max(0, min(wl_chat_back, wl_chat_max_back()));
}

static void wl_chat_up(struct GuiButton *gbtn)
{
    if (wl_chat_back < wl_chat_max_back())
        wl_chat_back++;
}

static void wl_chat_down(struct GuiButton *gbtn)
{
    if (wl_chat_back > 0)
        wl_chat_back--;
}

static void wl_chat_up_maintain(struct GuiButton *gbtn)
{
    wl_chat_box[2] = gbtn->scr_pos_x + gbtn->width; /* the box's right edge: its scrollbar's */
    if (wl_chat_back < wl_chat_max_back())
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wl_chat_down_maintain(struct GuiButton *gbtn)
{
    if (wl_chat_back > 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

static void wl_chat_drag(struct GuiButton *gbtn)
{
    const int max_back = wl_chat_max_back();
    wl_chat_back = max_back - wl_tab_offset(gbtn, max_back);
}

static void wl_chat_tab_draw(struct GuiButton *gbtn)
{
    const int max_back = wl_chat_max_back();
    wl_draw_tab(gbtn, max_back - wl_chat_back, max_back);
}

static void wl_say_done(struct GuiButton *gbtn)
{
    if (left_button_clicked || right_button_clicked)
        return; /* a click elsewhere ended the typing: the line isn't sent, the box doesn't take the keys back */
    wnet_menu_say(wl_say);
    wl_say[0] = '\0';
    wl_chat_back = 0; /* back to the newest lines, where this one comes */
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
}

static void wl_join_maintain(struct GuiButton *gbtn)
{
    if (wl_selected >= 0)
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
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  14,  52,  14,  52,  66,  24, wl_draw_server_label,   0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_EditBox,     BID_DEFAULT,    0, 0, wl_server_done, NULL, frontend_over_button, 0,  72,  52,  72,  52, 318,  24, wl_draw_entry,          0, GUIStr_Empty, 0, {.str = wl_server}, sizeof(wl_server) - 1, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 400,  52, 400,  52,  56,  24, wl_draw_name_label,     0, GUIStr_Empty, 0, {0},  0, NULL },
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
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0,  10, 226,  10, 226, 114, 112, frontnet_draw_lobby_panel, 0, GUIStr_Empty, 0, {0},  0, wl_names_panel_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_names_click, NULL, frontend_over_button, 0,  20, 233,  20, 233, 100,  98, wl_draw_names,          0, GUIStr_Empty, 0, {0},  0, wl_names_maintain },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wl_names_up,    NULL, frontend_over_button, 0, 124, 225, 124, 225,  26,  14, frontend_draw_slider_button, 0, GUIStr_Empty, 0, {36}, 0, wl_names_up_maintain },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wl_names_down,  NULL, frontend_over_button, 0, 124, 324, 124, 324,  26,  14, frontend_draw_slider_button, 0, GUIStr_Empty, 0, {37}, 0, wl_names_down_maintain },
  { LbBtnT_DraggableBtn,BID_DEFAULT,    0, 0, wl_names_drag,  NULL, NULL,                 0, 128, 239, 128, 239,  20,  85, wl_names_tab_draw,      0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 156, 226, 156, 226, 448, 112, frontnet_draw_lobby_panel, 0, GUIStr_Empty, 0, {0},  0, wl_chat_panel_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, NULL,           NULL, NULL,                 0, 168, 233, 168, 233, 430,  98, wl_draw_chat,           0, GUIStr_Empty, 0, {0},  0, wl_chat_maintain },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wl_chat_up,     NULL, frontend_over_button, 0, 604, 225, 604, 225,  26,  14, frontend_draw_slider_button, 0, GUIStr_Empty, 0, {38}, 0, wl_chat_up_maintain },
  { LbBtnT_HoldableBtn, BID_DEFAULT,    0, 0, wl_chat_down,   NULL, frontend_over_button, 0, 604, 324, 604, 324,  26,  14, frontend_draw_slider_button, 0, GUIStr_Empty, 0, {39}, 0, wl_chat_down_maintain },
  { LbBtnT_DraggableBtn,BID_DEFAULT,    0, 0, wl_chat_drag,   NULL, NULL,                 0, 608, 239, 608, 239,  20,  85, wl_chat_tab_draw,       0, GUIStr_Empty, 0, {0},  0, NULL },
  { LbBtnT_EditBox,     BID_DEFAULT,    0, 0, wl_say_done,    NULL, frontend_over_button, 0, 156, 342, 156, 342, 474,  28, wl_draw_entry,          0, GUIStr_Empty, 0, {.str = wl_say}, sizeof(wl_say) - 1, wl_say_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_invite_click,NULL, frontend_over_button, 0,  60, 374,  60, 374, 247,  46, wl_draw_invite,         0, GUIStr_Empty, 0, {2},  0, wl_invite_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_leave_click, NULL, frontend_over_button, 0, 333, 374, 333, 374, 247,  46, wl_draw_leave,          0, GUIStr_Empty, 0, {4},  0, wl_leave_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, wl_join_click,  NULL, frontend_over_button, 0,  60, 428,  60, 428, 247,  46, wl_draw_join,           0, GUIStr_Empty, 0, {5},  0, wl_join_maintain },
  { LbBtnT_NormalBtn,   BID_DEFAULT,    0, 0, frontend_change_state, NULL, frontend_over_button, FeSt_ENDLESS_MENU, 333, 428, 333, 428, 247, 46, wl_draw_back, 0, GUIStr_Empty, 0, {6}, 0, NULL },
  {-1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, GUIStr_Empty, 0, {0}, 0, NULL },
};

struct GuiMenu world_list_menu =
 { GMnu_FEWORLD_LIST, 0, 1, wl_buttons, POS_SCRCTR, POS_SCRCTR, 640, 480, NULL, 0, NULL, NULL, 0, 0, 0,};

/* "Join <name> to party?": opened over the World List while an invite waits for this player's answer, on the error
   box's scroll (it takes all the clicks until answered). Escape refuses. */
static void wp_answer(TbBool accept)
{
    wnet_menu_party_answer(accept);
    if (menu_id_to_number(GMnu_FEPARTY_INVITE) >= 0)
        turn_off_menu(GMnu_FEPARTY_INVITE);
}

static void wp_accept(struct GuiButton *gbtn)
{
    wp_answer(true);
}

static void wp_refuse(struct GuiButton *gbtn)
{
    wp_answer(false);
}

static void wp_draw_accept(struct GuiButton *gbtn)
{
    frontend_draw_button(gbtn, 0, "Accept", Lb_TEXT_HALIGN_CENTER);
}

static void wp_draw_refuse(struct GuiButton *gbtn)
{
    frontend_draw_button(gbtn, 0, "Refuse", Lb_TEXT_HALIGN_CENTER);
}

static void wp_maintain(struct GuiButton *gbtn)
{
    if (is_key_pressed(KC_ESCAPE, KMod_DONTCARE))
    {
        clear_key_pressed(KC_ESCAPE);
        wp_answer(false);
    }
}

static struct GuiButtonInit world_party_buttons[] = {
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,      NULL, NULL,                 0, 999, 172, 999, 172, 450, 136, frontend_draw_error_text_box, 0, GUIStr_Empty, 0, {.str = wp_text}, 0, wp_maintain },
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wp_accept, NULL, frontend_over_button, 0, 110, 258, 110, 258, 204,  38, wp_draw_accept, 0, GUIStr_Empty, 0, {8}, 0, NULL },
  { LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wp_refuse, NULL, frontend_over_button, 0, 326, 258, 326, 258, 204,  38, wp_draw_refuse, 0, GUIStr_Empty, 0, {9}, 0, NULL },
  {-1, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0, GUIStr_Empty, 0, {0}, 0, NULL },
};

struct GuiMenu world_party_menu =
 { GMnu_FEPARTY_INVITE, 0, 1, world_party_buttons, POS_SCRCTR, POS_SCRCTR, 640, 480, NULL, 0, NULL, NULL, 0, 1, 0,};

void world_list_menu_setup(void)
{
    wl_scroll = 0;
    wl_selected = -1;
    wl_selected_key[0] = '\0';
    wl_say[0] = '\0';
    wl_say_reopen = false;
    wl_party_sel[0] = '\0';
    wl_names_scroll = 0;
    wl_chat_back = 0;
    wl_chat_seen = 0;
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
    wl_hub_short(wnet_menu_hub(), wl_server, sizeof(wl_server));
    wnet_menu_open(wl_name);
}

void world_list_menu_close(void)
{
    if (menu_id_to_number(GMnu_FEPARTY_INVITE) >= 0)
        turn_off_menu(GMnu_FEPARTY_INVITE);
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

/** The newest loadable autosave of a seed, or -1. A dedicated server's own ("Server <seed> d<day> (auto)") first, else
 *  the player's ("Endless <seed> d<day> (auto)"): a server that looked for the player's only lost its own play at every
 *  start (2026-11-09 to 2026-11-10). */
int world_menu_slot_for_seed(const char *seed)
{
    if ((seed == NULL) || (seed[0] == '\0'))
        return -1;
    wm_list_saves();
    for (int kind = netstate_dedicated() ? 0 : 1; kind < 2; kind++)
    {
        char prefix[SAVE_TEXTNAME_LEN];
        snprintf(prefix, sizeof(prefix), "%s %.8s d", (kind == 0) ? "Server" : "Endless", seed);
        for (int i = 0; i < wm_saves_count; i++)
        {
            const struct CatalogueEntry *centry = SaveManager_Entry(wm_saves[i]);
            if ((centry != NULL) && ((centry->flags & CEF_Unloadable) == 0) && wm_autosave_name_matches(centry->textname, prefix))
                return wm_saves[i];
        }
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
        /* the player's autosaves and a dedicated server's own (their seeds and days too: Keeper Server goes by them) */
        if ((sscanf(centry->textname, "Endless %15s d%ld", seed, &day) < 1) && (sscanf(centry->textname, "Server %15s d%ld", seed, &day) < 1))
            snprintf(seed, sizeof(seed), "-");
        fprintf(f, "%d\t%s\t%ld\t%lld\t%s\n", wm_saves[i], seed, day, (long long)wm_slot_mtime(wm_saves[i]), centry->textname);
    }
    fclose(f);
}

void world_autosave(const char *why)
{
    if (!world_mode_active() || !world_menu_endless_campaign_loaded() || replay.load_enable)
        return;
    if (world_attract_active())
        return; /* the launcher's background keeps nothing */
    /* a network game: only the host keeps the run (every machine shares the save folder when tested on one PC) */
    if (network_is_active() && !network_is_host())
        return;
    struct PlayerInfo *player = get_my_player();
    if ((player->victory_state == VicS_LostLevel) && !network_is_active())
        return; /* a lost single-player run; a defeated host still keeps the others' world (audit 2026-11-09) */
    const char *seed = world_seed_text();
    long day = world_day_number();
    if (day > 9999)
        day = 9999;
    char prefix[SAVE_TEXTNAME_LEN];
    char name[SAVE_TEXTNAME_LEN];
    /* a dedicated server's saves have their own name: the player's Continue leaves them alone */
    const char *kind = netstate_dedicated() ? "Server" : "Endless";
    snprintf(prefix, sizeof(prefix), "%s %.8s d", kind, seed);
    snprintf(name, sizeof(name), "%s %.8s d%ld (auto)", kind, seed, day);
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

/* !delete dungeon (party dungeons, the user, 2026-11-10): typed in the chat of an online endless world, it opens this
   box here (the line isn't sent); Yes sends the request to the server as a packet, which deletes the dungeon this
   player plays (solo or co-op: its ground is made again) and sends the player back to the World List. */
static char wd_title[] = "Are you sure?";
static char wd_yes[] = "Yes";
static char wd_no[] = "No";

static Uint64 wd_asked_at = 0; /* when this player said Yes (0: not) */

TbBool world_delete_just_asked(void)
{
    const TbBool asked = (wd_asked_at != 0) && (SDL_GetTicks() - wd_asked_at < 30000);
    wd_asked_at = 0;
    return asked;
}

static void wd_yes_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    SYNCLOG("World: this player deletes its dungeon");
    wd_asked_at = SDL_GetTicks();
    turn_off_menu(GMnu_WORLD_DELETE);
    set_players_packet_action(get_my_player(), PckA_WorldDelete, 0, 0, 0, 0);
    if (chat_box_active())
        chat_box_add(-1, "Deleting your dungeon...");
}

static void wd_no_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    turn_off_menu(GMnu_WORLD_DELETE);
}

static struct GuiButtonInit world_delete_buttons[] = {
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,         NULL, NULL, 0, 999, 12, 999, 12, 260, 32, gui_area_text, 2, GUIStr_Empty, 0, {.str = wd_title}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, wd_yes_click, NULL, NULL, 0,  30, 58,  30, 58, 110, 32, gui_area_text, 1, GUIStr_Empty, 0, {.str = wd_yes},   0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, wd_no_click,  NULL, NULL, 0, 160, 58, 160, 58, 110, 32, gui_area_text, 1, GUIStr_Empty, 0, {.str = wd_no},    0, NULL },
  {              -1, BID_DEFAULT, 0, 0, NULL,         NULL, NULL, 0,   0,  0,   0,  0,   0,  0, NULL,          0, 0,            0, {0},              0, NULL },
};

struct GuiMenu world_delete_menu =
 { GMnu_WORLD_DELETE, 0, 1, world_delete_buttons, POS_GAMECTR, POS_GAMECTR, 300, 104, gui_pretty_background, 0, NULL, NULL, 0, 1, 0,};

TbBool world_delete_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    const char *s = text + 1;
    while (*s == ' ')
        s++;
    if (strncasecmp(s, "delete", 6) != 0)
        return false;
    s += 6;
    while (*s == ' ')
        s++;
    if (strncasecmp(s, "dungeon", 7) != 0)
        return false;
    s += 7;
    while (*s == ' ')
        s++;
    if (*s != '\0')
        return false;
    if (!network_is_active() || !netstate_is_client())
    {
        const char *say = "!delete dungeon works when you play in someone's world (a server), not in your own.";
        if (chat_box_active())
            chat_box_add(-1, say);
        else
            message_add(MsgType_Blank, 0, say);
        return true;
    }
    SYNCLOG("World: !delete dungeon: asking first");
    if (chat_box_active())
        chat_box_add(-1, "!delete dungeon: the dungeon you play goes for good, and its ground is made again.");
    if (get_local_view_type() == PVT_MapScreen)
        zoom_from_parchment_map(); /* the box's buttons get no clicks on the full-screen map */
    turn_on_menu(GMnu_WORLD_DELETE);
    return true;
}

static void wd_yes_click(struct GuiButton *gbtn);

void world_respawn_prompt_turn(void)
{
    if (!world_mode_active() || netstate_dedicated() || replay.load_enable || observer_is_active())
        return;
    {
        /* test hook KFX_WORLD_DELETEYES=<ms>: !delete dungeon's box answers Yes itself that long after it came up */
        static Uint64 delete_box_at = 0;
        const char *yes_ms = getenv("KFX_WORLD_DELETEYES");
        if ((yes_ms != NULL) && menu_is_active(GMnu_WORLD_DELETE))
        {
            if (delete_box_at == 0)
                delete_box_at = SDL_GetTicks();
            else if (SDL_GetTicks() - delete_box_at >= (Uint64)atoi(yes_ms))
            {
                SYNCLOG("World test: !delete dungeon's Yes clicks itself");
                delete_box_at = 0;
                wd_yes_click(NULL);
            }
        }
    }
    struct PlayerInfo *player = get_my_player();
    {
        /* test hook KFX_WORLD_QUITAFTER=<s>: this client leaves the world (as the Quit button) that long after its keeper
           is in it */
        static Uint64 quit_from = 0;
        static TbBool quit_done = false;
        const char *quit_after = getenv("KFX_WORLD_QUITAFTER");
        if ((quit_after != NULL) && !quit_done && player_exists(player) && (player->victory_state != VicS_LostLevel))
        {
            if (quit_from == 0)
                quit_from = SDL_GetTicks();
            else if (SDL_GetTicks() - quit_from >= (Uint64)atoi(quit_after) * 1000)
            {
                quit_done = true;
                SYNCLOG("World test: this client leaves the world");
                gui_quit_game(NULL);
                return;
            }
        }
    }
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
                turn_off_all_panel_menus(); /* the lost player's spell panel goes */
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
    /* on the full-screen map the box's buttons get no clicks (a click there zooms): back to the dungeon first */
    if (get_local_view_type() == PVT_MapScreen)
        zoom_from_parchment_map();
    turn_on_menu(GMnu_WORLD_RESPAWN);
}
/******************************************************************************/

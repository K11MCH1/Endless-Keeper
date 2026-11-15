/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_attune.c
 *     Endless world: Attunement, the keeper's skill tree of powers.
 * @par Purpose:
 *     See world_attune.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "world_attune.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "bflib_guibtns.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "world_stream.h"
#include "world_progress.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "config_magic.h"
#include "magic_powers.h"
#include "packets.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "gui_parchment.h"
#include "local_camera.h"
#include "frontend.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "dungeon_data.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define rank game.world_attune

static const char *const branch_names[ATTUNE_BRANCHES] = {"Keeping an army alive", "Fighting at home", "Leading"};
static const char *const branch_powers[ATTUNE_BRANCHES] = {"Heal, Protect Monster, Speed Monster",
    "Freeze, Slow, Lightning Strike", "Possession, Must Obey, Call to Arms"};

static int branch_of(PowerKind pw)
{
    switch (pw)
    {
    case PwrK_HEALCRTR: case PwrK_PROTECT: case PwrK_SPEEDCRTR:
        return 0;
    case PwrK_FREEZE: case PwrK_SLOW: case PwrK_LIGHTNING:
        return 1;
    case PwrK_POSSESS: case PwrK_OBEY: case PwrK_CALL2ARMS:
        return 2;
    default:
        return -1;
    }
}

GoldAmount world_attune_price(PlayerNumber p, PowerKind pwkind, GoldAmount price)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_MAX) || (price <= 0))
        return price;
    const int b = branch_of(pwkind);
    if (b < 0)
        return price;
    const int r = rank[p][b];
    return (r <= 0) ? price : max((GoldAmount)1, price * (100 - ATTUNE_PCT_PER_RANK * min(r, ATTUNE_RANKS)) / 100);
}

int world_attune_points_left(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return 0;
    int spent = 0;
    for (int b = 0; b < ATTUNE_BRANCHES; b++)
        spent += rank[p][b];
    return max(0, world_player_level(p) - 1 - spent);
}

void world_attune_spend(PlayerNumber p, int branch)
{
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()) || (p < 0) || (p >= PLAYERS_COUNT)
        || (p >= PLAYERS_MAX) || (branch < 0) || (branch >= ATTUNE_BRANCHES))
        return;
    if ((world_attune_points_left(p) <= 0) || (rank[p][branch] >= ATTUNE_RANKS))
        return;
    rank[p][branch]++;
    SYNCLOG("World: player %d attunes to %s, rank %d (%d point%s left)", (int)p, branch_names[branch],
        (int)rank[p][branch], world_attune_points_left(p), (world_attune_points_left(p) == 1) ? "" : "s");
}

void world_attune_seat_reset(PlayerNumber p)
{
    if ((p >= 0) && (p < PLAYERS_MAX))
        memset(rank[p], 0, sizeof(rank[p]));
}

/******************************************************************************/
/* the window */
#define AT_W 420
#define AT_H 230
static char at_close[] = "Close";
static char at_plus[ATTUNE_BRANCHES][4] = {"+", "+", "+"};

static void at_close_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    turn_off_menu(GMnu_KEEPER_ATTUNE);
}

static void at_plus_click(struct GuiButton *gbtn)
{
    const int b = (int)gbtn->btype_value & 0xFF;
    set_players_packet_action(get_my_player(), PckA_WorldAttune, (unsigned long)b, 0, 0, 0);
}

static void at_background(struct GuiMenu *gmnu);

static struct GuiButtonInit world_attune_buttons[] = {
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, at_plus_click,  NULL, NULL, 0, AT_W - 86,  50, AT_W - 86,  50, 74, 28, gui_area_text, 1, GUIStr_Empty, 0, {.str = at_plus[0]}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, at_plus_click,  NULL, NULL, 1, AT_W - 86, 100, AT_W - 86, 100, 74, 28, gui_area_text, 1, GUIStr_Empty, 0, {.str = at_plus[1]}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, at_plus_click,  NULL, NULL, 2, AT_W - 86, 150, AT_W - 86, 150, 74, 28, gui_area_text, 1, GUIStr_Empty, 0, {.str = at_plus[2]}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, at_close_click, NULL, NULL, 0, 999, AT_H - 38, 999, AT_H - 38, 110, 28, gui_area_text, 1, GUIStr_Empty, 0, {.str = at_close}, 0, NULL },
  {              -1, BID_DEFAULT, 0, 0, NULL,           NULL, NULL, 0,   0,         0,   0,         0,   0,  0, NULL,          0, 0,            0, {0},              0, NULL },
};

struct GuiMenu world_attune_menu =
 { GMnu_KEEPER_ATTUNE, 0, 1, world_attune_buttons, POS_GAMECTR, POS_GAMECTR, AT_W, AT_H, at_background, 0, NULL, NULL, 0, 1, 0,};

static const unsigned char *at_tint(int which)
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

static void at_text(int x, int y, int w, int px, int align, const char *text, int tint)
{
    const unsigned long flg_mem = RendererGetDrawFlags();
    LbTextSetFont(winfont);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags((align == 1) ? Lb_TEXT_HALIGN_CENTER : (align == 2) ? Lb_TEXT_HALIGN_RIGHT : Lb_TEXT_HALIGN_LEFT);
    LbTextSetWindow(x, y, w, 2 * px + 4);
    const int line = max(LbTextLineHeight(), 1);
    const unsigned char *old_remap = LbTextGetRemap();
    const unsigned char *map = at_tint(tint);
    if (map != NULL)
        LbTextSetRemap(map);
    LbTextDrawResized(0, 0, max(36 * px / line, 4), text);
    if (map != NULL)
        LbTextSetRemap(old_remap);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

static void at_background(struct GuiMenu *gmnu)
{
    const int upp = max((gmnu->width * 16 + AT_W / 2) / AT_W, 1);
#define S(v) ((v) * upp / 16)
    static TbPixel panel, edge, pip_off, pip_on;
    if (panel == 0)
    {
        panel = LbPaletteFindColour(engine_palette, 5, 5, 7);
        edge = LbPaletteFindColour(engine_palette, 22, 20, 16);
        pip_off = LbPaletteFindColour(engine_palette, 14, 13, 12);
        pip_on = LbPaletteFindColour(engine_palette, 58, 46, 16);
    }
    const int x = gmnu->pos_x, y = gmnu->pos_y, w = gmnu->width, h = gmnu->height;
    const int e = max(S(1), 1);
    LbDrawBox(x, y, w, h, panel);
    LbDrawBox(x, y, w, e, edge);
    LbDrawBox(x, y + h - e, w, e, edge);
    LbDrawBox(x, y, e, h, edge);
    LbDrawBox(x + w - e, y, e, h, edge);
    const PlayerNumber p = my_player_number;
    if (!world_mode_active() || (p >= PLAYERS_MAX))
        return;
    char text[96];
    at_text(x + S(12), y + S(8), w - S(24), max(S(8), 7), 0, "Attunement", 1);
    snprintf(text, sizeof(text), "%d point%s to spend", world_attune_points_left(p), (world_attune_points_left(p) == 1) ? "" : "s");
    at_text(x + S(12), y + S(10), w - S(24), max(S(6), 6), 2, text, 2);
    for (int b = 0; b < ATTUNE_BRANCHES; b++)
    {
        const int by = y + S(44 + 50 * b);
        const int r = rank[p][b];
        at_text(x + S(12), by, S(250), max(S(6), 6), 0, branch_names[b], (r > 0) ? 1 : 2);
        snprintf(text, sizeof(text), "%s: %d%% cheaper", branch_powers[b], ATTUNE_PCT_PER_RANK * r);
        at_text(x + S(12), by + S(12), S(300), max(S(5), 6), 0, text, 2);
        for (int k = 0; k < ATTUNE_RANKS; k++)
            LbDrawBox(x + S(12 + 14 * k), by + S(26), S(10), S(6), (k < r) ? pip_on : pip_off);
    }
#undef S
}

TbBool world_attune_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strcasecmp(text + 1, "attune") != 0)
        return false;
    if (menu_is_active(GMnu_KEEPER_ATTUNE))
        turn_off_menu(GMnu_KEEPER_ATTUNE);
    else
    {
        if (get_local_view_type() == PVT_MapScreen)
            zoom_from_parchment_map();
        turn_on_menu(GMnu_KEEPER_ATTUNE);
    }
    return true;
}

/** KFX_WORLD_ATTUNETEST=turn (test switch): this machine's keeper gets 15000 XP, spends points 2, 2, 1 in the three
 *  branches (as the "+" buttons do) and opens the window; prices before and after are logged. */
void world_attune_turn(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_ATTUNETEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_ATTUNETEST=%ld", at);
    }
    {
        /* KFX_WORLD_ATTUNEXP=turn (test switch, a dedicated server): the first human keeper with a Heart (a joined
           player) gets 15000 XP at that turn, to spend from its own window */
        static long xp_at = -2;
        if (xp_at == -2)
        {
            const char *v = getenv("KFX_WORLD_ATTUNEXP");
            xp_at = (v != NULL) ? atol(v) : -1;
        }
        if ((xp_at >= 0) && ((long)game.play_gameturn == xp_at) && world_mode_active())
            for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
                if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
                {
                    world_add_player_xp(q, 15000);
                    SYNCLOG("World: attune test: player %d gets 15000 XP: level %d, %d points", (int)q, world_player_level(q),
                        world_attune_points_left(q));
                    break;
                }
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    const PlayerNumber p = my_player_number;
    world_add_player_xp(p, 15000);
    const GoldAmount heal0 = compute_power_price(p, PwrK_HEALCRTR, 0), light0 = compute_power_price(p, PwrK_LIGHTNING, 0),
        cta0 = compute_power_price(p, PwrK_CALL2ARMS, 0);
    static const int spend[] = {0, 0, 1, 1, 2};
    for (int i = 0; i < (int)(sizeof(spend) / sizeof(spend[0])); i++)
        world_attune_spend(p, spend[i]);
    SYNCLOG("World: attune test: level %d, %d points left; Heal %ld -> %ld, Lightning %ld -> %ld, Call to Arms %ld -> %ld, "
        "Sight of Evil unchanged %d", world_player_level(p), world_attune_points_left(p), (long)heal0,
        (long)compute_power_price(p, PwrK_HEALCRTR, 0), (long)light0, (long)compute_power_price(p, PwrK_LIGHTNING, 0),
        (long)cta0, (long)compute_power_price(p, PwrK_CALL2ARMS, 0), (int)(world_attune_price(p, PwrK_SIGHT, 100) == 100));
    world_attune_command("!attune");
}

/******************************************************************************/
void world_attune_reset(void)
{
    memset(rank, 0, sizeof(rank));
}

size_t world_attune_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int b = 0; b < ATTUNE_BRANCHES; b++)
            if (rank[p][b] != 0)
                return 4 + sizeof(rank);
    return 0;
}

void world_attune_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, rank, sizeof(rank));
}

TbBool world_attune_load(const uint8_t *buf, size_t len)
{
    world_attune_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (4 + sizeof(rank) > len))
        return false;
    memcpy(rank, buf + 4, sizeof(rank));
    return true;
}

#ifdef __cplusplus
}
#endif

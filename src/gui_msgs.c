/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file gui_msgs.c
 *     Game GUI Messages functions.
 * @par Purpose:
 *     Functions to display and maintain GUI Messages.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     14 May 2010 - 21 Nov 2012
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "gui_msgs.h"
#include <stdarg.h>

#include "globals.h"
#include "bflib_basics.h"
#include "bflib_sprfnt.h"
#include "bflib_vidraw.h"
#include "config_spritecolors.h"
#include "creature_graphics.h"
#include "creature_instances.h"
#include "gui_draw.h"
#include "frontend.h"
#include "game_legacy.h"
#include "frontmenu_ingame_evnt.h"
#include "sprites.h"
#include "custom_sprites.h"
#include "keeperfx.hpp"
#include "timer.h"
#include "observer.h"
#include "player_instances.h"
#include "world_stream.h"
#include "server_roles.h"
#include "front_simple.h"
#include "bflib_video.h"
#include "kfx/renderer/RendererManager.h"
#include "world_attract.h"
#include "chat_box.h"
#include "player_data.h"
#include "post_inc.h"

/******************************************************************************/

int chat_font_size = 20;

/* each message's chat sender (-1: not a chat line), beside game.messages and moved with them: a chat line from an
   endless server shows the sender's name first, in its role's colour (server_roles.c) */
static short msg_sender[GUI_MESSAGES_COUNT];

static void msg_sender_insert(void)
{
    memmove(&msg_sender[1], &msg_sender[0], sizeof(msg_sender[0]) * (GUI_MESSAGES_COUNT - 1));
    msg_sender[0] = -1;
}

void message_set_chat_sender(PlayerNumber plyr_idx)
{
    msg_sender[0] = plyr_idx;
}

/** The chat font tinted with a role's colour: each palette colour becomes the role's colour at that colour's brightness
 *  (the nearest the game's palette has), so letters keep their shading and dark edge. A few colours are kept. */
const unsigned char *message_chat_name_remap(uint32_t rgb)
{
    /* one per role colour (a server has at most 32 roles): the renderer reads a remap when it draws the frame, so
       one must not be remade for another colour while a frame still shows it (4 entries did, with 5+ colours) */
    static struct { uint32_t rgb; TbBool made; unsigned char map[256]; } cache[32];
    static int next;
    if (engine_palette == NULL)
        return NULL;
    for (int k = 0; k < (int)(sizeof(cache) / sizeof(cache[0])); k++)
        if (cache[k].made && (cache[k].rgb == rgb))
            return cache[k].map;
    const int k = next;
    next = (next + 1) % (int)(sizeof(cache) / sizeof(cache[0]));
    const int tr = (int)((rgb >> 16) & 0xFF) >> 2, tg = (int)((rgb >> 8) & 0xFF) >> 2, tb = (int)(rgb & 0xFF) >> 2;
    for (int i = 0; i < 256; i++)
    {
        const unsigned char *c = &engine_palette[3 * i];
        /* the font's main colour (about 34 of 63 bright) takes the role's colour itself; darker ones a darker shade */
        const int lum = (c[0] * 30 + c[1] * 59 + c[2] * 11) / 100;
        const int f = min(64, lum * 64 / 34);
        cache[k].map[i] = LbPaletteFindColour(engine_palette, (unsigned char)(tr * f / 64), (unsigned char)(tg * f / 64),
            (unsigned char)(tb * f / 64));
    }
    cache[k].rgb = rgb;
    cache[k].made = true;
    return cache[k].map;
}

/** "<name>: " before a chat line (in its role's colour, with a dark edge); returns its width. */
static int draw_chat_sender(PlayerNumber plyr_idx, long x, long y, int units_per_px)
{
    const char *name = NULL;
    TbBool coloured = false;
    uint32_t rgb = 0;
    if ((plyr_idx < 0) || !roles_client_player(plyr_idx, &name, &coloured, &rgb))
        return 0;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s: ", name);
    const unsigned char *remap = coloured ? message_chat_name_remap(rgb) : NULL;
    if (remap != NULL)
    {
        const unsigned char *old_remap = LbTextGetRemap();
        const unsigned short flags = RendererGetDrawFlags();
        RendererSetDrawFlags(flags & ~(Lb_TEXT_ONE_COLOR | Lb_TEXT_REMAP));
        LbTextSetRemap(remap);
        LbTextDrawResized(x, y, units_per_px, prefix);
        LbTextSetRemap(old_remap);
        RendererSetDrawFlags(flags);
    } else
        LbTextDrawResized(x, y, units_per_px, prefix);
    return LbTextStringWidthM(prefix, units_per_px);
}

/** Endless worlds: where the message lines start and how large they are drawn (the user, 2026-11-15: in the view's
 *  top left corner, and smaller). x and y: the first line's text, as message_draw places it; *tx: the text's units
 *  per pixel; *upp: the units per pixel the icons and spacing take, scaled as the text is. */
void message_world_place(long *x, long *y, int *tx, int *upp)
{
    const int full = font_size_units_per_px(chat_font_size);
    const int small = max(16, full * WORLD_MSG_SIZE / 16); /* never smaller than the font itself */
    const int u = (int)((long)units_per_pixel * small / max(full, 1));
    const int margin = 6 * units_per_pixel / 16; /* the day's dial stands as far from the top */
    const long left = local_state.show_gui ? status_panel_width : 0;
    /* the icons stand left of and above their line's text: up to 10 units left, 20 up (a creature's hand symbol) */
    *x = left + margin + 10 * u / 16;
    /* 10 pixels higher on a 2560 x 1440 screen (the user, 2026-11-15), the same share of the height on any other */
    *y = max(0, margin + 20 * u / 16 - 28 * units_per_pixel / 160);
    *tx = small;
    *upp = u;
}

/** Endless worlds: a notification drawn in lines no wider than max_w, broken between words (the user, 2026-11-19: a
 *  long one ran off the screen). Returns how many lines it took past the first. */
static int message_draw_wrapped(long x, long y, int units_per_px, const char *text, long max_w, int line_h)
{
    if (max_w < 40 * units_per_px / 16)
    {
        LbTextDrawResized(x, y, units_per_px, text);
        return 0;
    }
    char line[sizeof(game.messages[0].text)];
    int extra = 0;
    const char *p = text;
    while (*p != '\0')
    {
        /* as many words as fit (at least one) */
        int len = 0, fit = 0;
        while (p[len] != '\0')
        {
            int end = len;
            while ((p[end] != '\0') && (p[end] != ' '))
                end++;
            if (end >= (int)sizeof(line))
                break;
            memcpy(line, p, end);
            line[end] = '\0';
            if ((fit > 0) && (LbTextStringWidthM(line, units_per_px) > max_w))
                break;
            fit = end;
            len = (p[end] == ' ') ? end + 1 : end;
            if (p[end] == '\0')
                break;
        }
        if (fit == 0)
            fit = (int)strnlen(p, sizeof(line) - 1);
        memcpy(line, p, fit);
        line[fit] = '\0';
        LbTextDrawResized(x, y + extra * line_h, units_per_px, line);
        p += fit;
        while (*p == ' ')
            p++;
        if (*p != '\0')
            extra++;
    }
    return extra;
}

void message_draw(void)
{
    if (world_attract_active())
        return; /* the launcher's background: the picture alone */
    SYNCDBG(7,"Starting");
    LbTextSetFont(winfont);
    int ps_units_per_px;
    int icon_units_per_px;
    int tx_units_per_px = font_size_units_per_px(chat_font_size);
    int upp = units_per_pixel;
    long x0 = 0, y0 = 0;
    if (world_mode_active())
    {
        message_world_place(&x0, &y0, &tx_units_per_px, &upp);
        if (((get_local_user_state()->init_flags & UsrIF_NewMPMessage) != 0) && !chat_box_draws_typing())
            y0 += tx_units_per_px * CHAT_LINE_SPACING_PERCENT / 100; /* the typing line stands first */
    }
    const struct TbSprite* spr;
    {
        //just used for height, color irrelevant here
        spr = get_panel_sprite(GPS_plyrsym_symbol_player_red_std_b);
        ps_units_per_px = (22 * upp) / spr->SHeight;
        icon_units_per_px = (LbTextLineHeight() * tx_units_per_px) / spr->SHeight;
    }
    TbBool low_res = (MyScreenHeight < 400);
    int h = LbTextLineHeight();
    long y = world_mode_active() ? y0 : 28 * upp / 16;
    if (game.armageddon_cast_turn != 0)
    {
        if ( (bonus_timer_enabled()) || (script_timer_enabled()) || display_variable_enabled() )
        {
            y += (h*upp/16) << (unsigned char)low_res;
        }
    }
    /* endless worlds (the user, 2026-11-19: "the notification messages need to go from Bottom to Top"): the newest at the
       foot of the list, the older ones above it (the list keeps the newest first: drawn from its end) */
    const TbBool newest_last = world_mode_active();
    for (int n = 0; n < game.active_messages_count; n++)
    {
        const int i = newest_last ? (game.active_messages_count - 1 - n) : n;
        if (game.messages[i].target_idx == my_player_number || game.messages[i].target_idx == -1
         || (observer_is_active() && game.messages[i].target_idx == PLAYER_NEUTRAL)) {
            long x = world_mode_active() ? x0 : 148 * upp / 16;
            LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
            RendererClearDrawFlags(Lb_TEXT_ONE_COLOR);
            int text_offset = 32 * upp / 16;
            if ((game.messages[i].type == MsgType_Player) || (game.messages[i].type == MsgType_Spectator) || (game.messages[i].type == MsgType_Local)) {
                text_offset = text_offset * icon_units_per_px / ps_units_per_px;
            }
            const int sender_w = (game.messages[i].type == MsgType_Player) ? draw_chat_sender(msg_sender[i], x + text_offset, y, tx_units_per_px) : 0;
            int extra_lines = 0;
            if (world_mode_active())
            {
                /* up to the last eighth of the screen (the day/night dial's corner) */
                const long tx = x + text_offset + sender_w;
                extra_lines = message_draw_wrapped(tx, y, tx_units_per_px, game.messages[i].text,
                    MyScreenWidth - MyScreenWidth / 8 - tx, tx_units_per_px * CHAT_LINE_SPACING_PERCENT / 100);
            } else
                LbTextDrawResized(x + text_offset + sender_w, y, tx_units_per_px, game.messages[i].text);
            unsigned long spr_idx = 0;
            PlayerNumber plyr_idx = game.messages[i].plyr_idx;
            switch (game.messages[i].type)
            {
                case MsgType_Player:
                {
                    if (player_is_roaming(plyr_idx))
                    {
                        spr_idx = GPS_plyrsym_symbol_player_red_std_b;
                    }
                    else if (game.messages[i].plyr_idx == game.neutral_player_num)
                    {
                        spr_idx = ((get_gameturn() >> 1) & 3) + GPS_plyrsym_symbol_player_red_std_b;
                        plyr_idx = 0;
                    }
                    else
                    {
                        TbBool alive = player_has_heart_or_needs_none(game.messages[i].plyr_idx);
                        if (world_mode_active()) /* endless: another keeper's heart is often not on this machine */
                            alive = (get_player(game.messages[i].plyr_idx)->victory_state != VicS_LostLevel);
                        spr_idx = alive ? GPS_plyrsym_symbol_player_red_std_b : GPS_plyrsym_symbol_player_red_dead;
                    }
                    break;
                }
                case MsgType_Creature:
                {
                    spr_idx = get_creature_model_graphics(game.messages[i].plyr_idx, CGI_HandSymbol);
                    x -= (7 * upp / 16);
                    y -= (20 * upp / 16);
                    break;
                }
                case MsgType_CreatureSpell:
                {
                    struct SpellConfig* spconf = get_spell_config(game.messages[i].plyr_idx);
                    spr_idx = spconf->medsym_sprite_idx;
                    x -= (10 * upp / 16);
                    y -= (10 * upp / 16);
                    break;
                }
                case MsgType_Room:
                {
                    const struct RoomConfigStats* roomst = get_room_kind_stats(game.messages[i].plyr_idx);
                    spr_idx = roomst->medsym_sprite_idx;
                    x -= (10 * upp / 16);
                    y -= (10 * upp / 16);
                    break;
                }
                case MsgType_KeeperSpell:
                {
                    struct PowerConfigStats* powerst = get_power_model_stats(game.messages[i].plyr_idx);
                    spr_idx = powerst->medsym_sprite_idx;
                    x -= (10 * upp / 16);
                    y -= (10 * upp / 16);
                    break;
                }
                case MsgType_Query:
                {
                    spr_idx = (game.messages[i].plyr_idx + GPS_plyrsym_symbol_room_yellow_std_a);
                    x -= (10 * upp / 16);
                    y -= (10 * upp / 16);
                    break;
                }
                case MsgType_Custom:
                case MsgType_Spectator:
                {
                    spr_idx = game.messages[i].plyr_idx;
                    break;
                }
                case MsgType_Blank:
                case MsgType_Local:
                {
                    break;
                }
                case MsgType_CreatureInstance:
                {
                    struct InstanceInfo* inst_inf = creature_instance_info_get(game.messages[i].plyr_idx);
                    spr_idx = inst_inf->symbol_spridx;
                    x -= (10 * upp / 16);
                    y -= (10 * upp / 16);
                    break;
                }
                default:
                {
                    ERRORLOG("Unrecognised message type: %u", game.messages[i].type);
                    break;
                }
            }
            switch (game.messages[i].type)
            {
                case MsgType_Spectator: {
                    spr = get_panel_sprite(spr_idx);
                    int32_t creature_units_per_px = spr->SHeight * icon_units_per_px / 50;
                    LbSpriteDrawResized(x - 4 * creature_units_per_px / 16, y - creature_units_per_px, icon_units_per_px * 18 / 21, spr);
                    break;
                }
                case MsgType_Player:
                {
                    draw_gui_panel_sprite_left_player(x, y, icon_units_per_px, spr_idx, plyr_idx);
                    break;
                }
                case MsgType_Creature:
                {
                    spr = get_panel_sprite(spr_idx);
                    LbSpriteDrawResized(x, y, ps_units_per_px, spr);
                    break;
                }
                case MsgType_CreatureSpell:
                case MsgType_Room:
                case MsgType_KeeperSpell:
                case MsgType_Query:
                case MsgType_CreatureInstance:
                case MsgType_Custom:
                {
                    spr = get_panel_sprite(spr_idx);
                    LbSpriteDrawResized(x, y, ps_units_per_px, spr);
                    break;
                }
            }
            y += tx_units_per_px * CHAT_LINE_SPACING_PERCENT / 100 * (1 + extra_lines);
            switch (game.messages[i].type)
            {
                case MsgType_Player:
                {
                    break;
                }
                case MsgType_Creature:
                {
                    y += (20 * upp / 16) << (unsigned char)low_res;
                    break;
                }
                case MsgType_CreatureSpell:
                case MsgType_Room:
                case MsgType_KeeperSpell:
                case MsgType_Query:
                case MsgType_CreatureInstance:
                {
                    y += (10 * upp / 16) << (unsigned char)low_res;
                    break;
                }
            }
        }
    }
}

void message_update(void)
{
    SYNCDBG(6,"Starting");
    int i = game.active_messages_count - 1;
    // Set end turn for all messages
    while (i >= 0)
    {
        struct GuiMessage* gmsg = &game.messages[i];
        if (get_gameturn() > gmsg->expiration_turn)
        {
            game.active_messages_count--;
            game.messages[game.active_messages_count].text[0] = 0;
        }
        i--;
    }
}

void zero_messages(void)
{
    game.active_messages_count = 0;
    for (int i = 0; i < GUI_MESSAGES_COUNT; i++)
        msg_sender[i] = -1;
    for (int i = 0; i < 3; i++)
    {
      memset(&game.messages[i], 0, sizeof(struct GuiMessage));
    }
}

void clear_messages_from_player(char type, PlayerNumber plyr_idx)
{
    for (int i = 0; i < game.active_messages_count; i++)
    {
        if (game.messages[i].type == type)
        {
            if ( (game.messages[i].plyr_idx == plyr_idx) || (plyr_idx == -1) )
            {
                delete_message(i);
            }
        }
    }
}

void delete_message(unsigned char msg_idx)
{
    if (msg_idx < GUI_MESSAGES_COUNT - 1)
        memmove(&msg_sender[msg_idx], &msg_sender[msg_idx + 1], sizeof(msg_sender[0]) * (GUI_MESSAGES_COUNT - 1 - msg_idx));
    msg_sender[GUI_MESSAGES_COUNT - 1] = -1;
    memset(&game.messages[msg_idx], 0, sizeof(struct GuiMessage));
    if (msg_idx < game.active_messages_count - 1)
    {
        for (int i = msg_idx; i < game.active_messages_count; i++)
        {
            game.messages[i] = game.messages[i+1];
        }
        memset(&game.messages[game.active_messages_count - 1], 0, sizeof(struct GuiMessage));
    }
    game.active_messages_count--;
}

static void message_add_one(char type, short idx, const char *text)
{
    SYNCDBG(2,"Player %d: %s",idx,text);
    msg_sender_insert();
    for (int i = GUI_MESSAGES_COUNT - 1; i > 0; i--)
    {
        memcpy(&game.messages[i], &game.messages[i-1], sizeof(struct GuiMessage));
    }
    snprintf(game.messages[0].text, sizeof(game.messages[0].text), "%s", text);
    game.messages[0].plyr_idx = idx;
    game.messages[0].expiration_turn = get_gameturn() + GUI_MESSAGES_DELAY;
    game.messages[0].target_idx = -1;
    game.messages[0].type = type;
    game.messages[0].icon_idx = 0;
    if (game.active_messages_count < GUI_MESSAGES_COUNT) {
        game.active_messages_count++;
    }
}

/* Endless worlds (the user, 2026-11-19: a season's message was cut off): a message longer than one message holds (63
   letters; the size is part of the save) goes in as several, cut between words, in reading order; the pieces after
   the first are blank ones (no icon again). */
void message_add(char type, short idx, const char *text)
{
    const size_t room = sizeof(game.messages[0].text) - 1;
    if (!world_mode_active() || (text == NULL) || (strlen(text) <= room))
    {
        message_add_one(type, idx, (text != NULL) ? text : "");
        return;
    }
    const char *p = text;
    TbBool first = true;
    for (int pieces = 0; (*p != '\0') && (pieces < GUI_MESSAGES_COUNT); pieces++)
    {
        size_t len = strlen(p);
        if (len > room)
        {
            /* best after a sentence's end, else between words in the second half, else in the word */
            len = room;
            while ((len > room / 3) && !((p[len] == ' ') && (p[len - 1] == '.')))
                len--;
            if (p[len] != ' ')
            {
                len = room;
                while ((len > room / 2) && (p[len] != ' '))
                    len--;
                if (p[len] != ' ')
                    len = room;
            }
        }
        char piece[sizeof(game.messages[0].text)];
        memcpy(piece, p, len);
        piece[len] = '\0';
        message_add_one(first ? type : MsgType_Blank, first ? idx : 0, piece);
        first = false;
        p += len;
        while (*p == ' ')
            p++;
    }
}

void message_add_fmt(char type, short idx, const char *fmt_str, ...)
{
    static char full_msg_text[2048];
    va_list val;
    va_start(val, fmt_str);
    vsnprintf(full_msg_text, sizeof(full_msg_text), fmt_str, val);
    message_add(type, idx, full_msg_text);
    va_end(val);
}

void targeted_message_add(char type, PlayerNumber plyr_idx, PlayerNumber target_idx, unsigned long timeout, const char *fmt_str, ...)
{
    va_list val;
    va_start(val, fmt_str);
    static char full_msg_text[2048];
    vsnprintf(full_msg_text, sizeof(full_msg_text), fmt_str, val);
    SYNCDBG(2,"Player %d: %s",(int)plyr_idx,full_msg_text);
    msg_sender_insert();
    for (int i = GUI_MESSAGES_COUNT - 1; i > 0; i--)
    {
        memcpy(&game.messages[i], &game.messages[i-1], sizeof(struct GuiMessage));
    }
    snprintf(game.messages[0].text, sizeof(game.messages[0].text), "%s", full_msg_text);
    game.messages[0].plyr_idx = plyr_idx;
    game.messages[0].expiration_turn = get_gameturn() + timeout;
    game.messages[0].target_idx = target_idx;
    game.messages[0].type = type;
    game.messages[0].icon_idx = 0;
    if (game.active_messages_count < GUI_MESSAGES_COUNT) {
        game.active_messages_count++;
    }
    va_end(val);
}

void show_game_time_taken(unsigned long fps, unsigned long turns)
{
    struct GameTime gt;
    get_game_time(&gt, turns, fps);
    struct PlayerInfo* player = get_my_player();
    targeted_message_add(MsgType_Local, player->id_number, player->id_number, GUI_MESSAGES_DELAY, "%s: %02ld:%02ld:%02ld", get_string(GUIStr_Time), gt.Hours, gt.Minutes, gt.Seconds);
}

void show_real_time_taken(void)
{
    update_time();
    struct PlayerInfo* player = get_my_player();
    targeted_message_add(MsgType_Local, player->id_number, player->id_number, GUI_MESSAGES_DELAY, "%s: %02ld:%02ld:%02ld:%03ld", get_string(GUIStr_Time), Timer.Hours, Timer.Minutes, Timer.Seconds, Timer.MSeconds);
}
/******************************************************************************/

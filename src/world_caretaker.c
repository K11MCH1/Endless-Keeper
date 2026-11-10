/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_caretaker.c
 *     Endless worlds: the caretaker that keeps an offline player's dungeon.
 * @par Purpose:
 *     A player who leaves an endless world (2026-11-08) leaves a caretaker in charge, not a full computer keeper: it
 *     puts creatures to train and to work in their best rooms (the library, the workshop), places the doors and traps
 *     the workshop has made, and defends - the heart or rooms attacked, a breach, fights, imps in danger. It builds no
 *     room, digs nothing (the player's own dig marks wait for them), attacks nobody and casts nothing, and the
 *     dungeon's gold doesn't go below what it was when the player left, so wages are paid and nobody gets angry.
 *     When the player comes back, the caretaker goes.
 */
/******************************************************************************/
#include "pre_inc.h"
#include <string.h>
#include "world_caretaker.h"
#include "bflib_basics.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_computer.h"
#include "config_compp.h"
#include "dungeon_data.h"
#include "keeperfx.hpp"
#include "world_stream.h"
#include "server_admin.h"
#include "net_game.h"
#include "post_inc.h"

/* what it keeps of a computer keeper, by name (the rest - building, digging, attacking, spells, money - is left out) */
static const char *const kept_checks[] = {
    "MOVE CREATURE TO ROOM",      /* to the training room */
    "MOVE CREATURE TO BEST ROOM", /* the library, the workshop ... */
    "COMPUTER CHECK HATES",
    "CHECK AVAILIABLE TRAP",      /* the workshop's traps and doors, where they guard */
    "CHECK AVAILIABLE DOOR",
};
static const char *const kept_events[] = {
    "EVENT HEART UNDER ATTACK",
    "EVENT DUNGEON BREACH",
    "EVENT ROOM ATTACK",
    "EVENT FIGHT",
    "EVENT FIGHT TEST",
    "EVENT CHECK FIGHTERS",
    "EVENT SAVE IMPS",
    "EVENT ROOM LOST",
    "EVENT ENEMY DOOR",
};

static PlayerBitFlags caretaker_on;
static GoldAmount caretaker_gold[PLAYERS_MAX];

static TbBool name_in(const char *name, const char *const *list, int count)
{
    for (int i = 0; i < count; i++)
        if (strcasecmp(name, list[i]) == 0)
            return true;
    return false;
}

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

/** The computer model with the most of the kept checks and events (a skirmish one by preference). */
static long caretaker_model(void)
{
    long best = -1;
    int best_score = -1;
    for (long m = 0; m < COMPUTER_MODELS_COUNT; m++)
    {
        const struct ComputerType *cpt = get_computer_type_template(m);
        if ((cpt == NULL) || (cpt->name[0] == '\0'))
            continue;
        int score = 0;
        for (int i = 0; i < COMPUTER_CHECKS_COUNT; i++)
        {
            const struct ComputerCheck *c = &comp_player_conf.check_types[cpt->checks[i]];
            if (c->name[0] == '\0')
                break;
            score += name_in(c->name, kept_checks, COUNT_OF(kept_checks)) ? 1 : 0;
        }
        for (int i = 0; i < COMPUTER_EVENTS_COUNT; i++)
        {
            const struct ComputerEvent *e = &comp_player_conf.event_types[cpt->events[i]];
            if (e->name[0] == '\0')
                break;
            score += name_in(e->name, kept_events, COUNT_OF(kept_events)) ? 1 : 0;
        }
        if ((m >= comp_player_conf.skirmish_first) && (m <= comp_player_conf.skirmish_last))
            score = score * 2 + 1;
        if (score > best_score)
        {
            best_score = score;
            best = m;
        }
    }
    return (best >= 0) ? best : get_random_skirmish_computer_model();
}

TbBool world_caretaker_active(PlayerNumber plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && pmask_has(caretaker_on, plyr_idx);
}

TbBool world_caretaker_start(PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx >= PLAYERS_MAX))
        return false;
    struct PlayerInfo *player = get_player(plyr_idx);
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (!player_exists(player) || dungeon_invalid(dungeon))
        return false;
    if (world_seat_is_coop(plyr_idx))
        return false; /* party dungeons (2026-11-10): a co-op dungeon has no caretaker; it freezes while its player is away */
    const long model = caretaker_model();
    if (!setup_a_computer_player(plyr_idx, model))
        return false;
    struct Computer2 *comp = get_computer_player(plyr_idx);
    /* no process at all: no room is built, nothing dug, nobody attacked, no spell cast */
    memset(&comp->processes[0], 0, sizeof(comp->processes[0]));
    comp->processes[0].flags = ComProc_ListEnd;
    int checks = 0;
    for (int i = 0; i < COMPUTER_CHECKS_COUNT; i++)
    {
        const struct ComputerCheck c = comp->checks[i];
        if (((c.flags & ComChk_Unkn0002) != 0) || (c.name[0] == '\0'))
            break;
        if (name_in(c.name, kept_checks, COUNT_OF(kept_checks)))
            comp->checks[checks++] = c;
    }
    if (checks < COMPUTER_CHECKS_COUNT)
    {
        memset(&comp->checks[checks], 0, sizeof(comp->checks[checks]));
        comp->checks[checks].flags = ComChk_Unkn0002; /* the end of the checks */
    }
    int events = 0;
    for (int i = 0; i < COMPUTER_EVENTS_COUNT; i++)
    {
        const struct ComputerEvent e = comp->events[i];
        if (e.name[0] == '\0')
            break;
        if (name_in(e.name, kept_events, COUNT_OF(kept_events)))
            comp->events[events++] = e;
    }
    if (events < COMPUTER_EVENTS_COUNT)
        memset(&comp->events[events], 0, sizeof(comp->events[events]));
    player->allocflags |= PlaF_CompCtrl | PlaF_Placeholder;
    if ((dungeon->computer_enabled & 0x01) == 0)
        toggle_computer_player(plyr_idx);
    pmask_add(&caretaker_on, plyr_idx);
    caretaker_gold[plyr_idx] = dungeon->total_money_owned;
    SYNCLOG("World: player %d (%s) is away: a caretaker keeps the dungeon (model %ld: %d checks, %d events; gold held at %ld)",
        (int)plyr_idx, player->player_name, model, checks, events, (long)dungeon->total_money_owned);
    return true;
}

void world_caretaker_end(PlayerNumber plyr_idx)
{
    if (!world_caretaker_active(plyr_idx))
        return;
    pmask_del(&caretaker_on, plyr_idx);
    SYNCLOG("World: player %d is back: the caretaker goes", (int)plyr_idx);
}

/** Is someone playing this seat on this machine or through it (the local player alone, unless a network game)? */
static TbBool seat_played(PlayerNumber p)
{
    if (!network_is_active())
        return (p == my_player_number);
    for (NetUserId user = 0; user < MAX_NET_USERS; user++)
        if (user_present(user) && (get_net_user_player_number(user) == p))
            return true;
    return false;
}

/** The caretaker's computer off and its seat a player's again. */
static void caretaker_release(PlayerNumber p, struct PlayerInfo *player, struct Dungeon *dungeon)
{
    player->allocflags &= ~(PlaF_CompCtrl | PlaF_Placeholder);
    if (!dungeon_invalid(dungeon) && ((dungeon->computer_enabled & 0x01) != 0))
        toggle_computer_player(p);
    pmask_del(&caretaker_on, p);
}

void world_caretaker_turn(void)
{
    if (!world_mode_active())
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!world_player_is_human(p))
            continue;
        struct PlayerInfo *player = get_player(p);
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (!player_exists(player) || dungeon_invalid(dungeon) || !flag_is_set(player->allocflags, PlaF_CompCtrl))
            continue;
        if (seat_played(p))
        {
            /* a world saved while a caretaker kept this seat (a server stopped with its player away) comes back with
               the caretaker's computer on; the game ignores the input of a computer-controlled player - Quit, the
               assist button, Alt+F4 - so a seat someone plays is never left like that (2026-11-09) */
            caretaker_release(p, player, dungeon);
            SYNCLOG("World: player %d is played here: the caretaker's computer control left from the save goes", (int)p);
        } else
        if (!pmask_has(caretaker_on, p) && flag_is_set(player->allocflags, PlaF_Placeholder) &&
            ((dungeon->computer_enabled & 0x01) != 0) && (player->victory_state == VicS_Undecided))
        {
            /* the caretaker of a seat still away when the world was saved: it goes on (no digging, gold held) */
            pmask_add(&caretaker_on, p);
            caretaker_gold[p] = dungeon->total_money_owned;
            SYNCLOG("World: player %d is still away: its caretaker goes on (gold held at %ld)", (int)p,
                (long)dungeon->total_money_owned);
        }
    }
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!pmask_has(caretaker_on, p))
            continue;
        const struct PlayerInfo *player = get_player(p);
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (!player_exists(player) || dungeon_invalid(dungeon) || ((dungeon->computer_enabled & 0x01) == 0) ||
            (player->victory_state != VicS_Undecided))
        {
            pmask_del(&caretaker_on, p); /* someone took the seat, or it was lost */
            continue;
        }
        /* wages and training are paid as usual, and the gold they took comes back: it never goes below the mark */
        if (dungeon->total_money_owned < caretaker_gold[p])
            player_add_offmap_gold(p, caretaker_gold[p] - dungeon->total_money_owned);
    }
}

void world_caretaker_reset(void)
{
    caretaker_on = pmask_none();
    memset(caretaker_gold, 0, sizeof(caretaker_gold));
}
/******************************************************************************/

/** In single player, can this machine's packets be processed? process_packets drops them when the local user maps
 *  to no player, the player doesn't exist, or it is computer-controlled; Quit and Alt+F4 then quit directly. */
TbBool world_local_input_lost(void)
{
    if (!world_mode_active() || network_is_active())
        return false;
    const PlayerNumber plyr = get_net_user_player_number(get_local_user());
    if (plyr < 0)
        return true;
    const struct PlayerInfo *player = get_player(plyr);
    return !player_exists(player) || flag_is_set(player->allocflags, PlaF_CompCtrl);
}

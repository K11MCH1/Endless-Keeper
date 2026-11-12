/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_anticheat.c
 *     Server-side anti-cheat for worlds the server alone simulates (thin clients).
 * @par Purpose:
 *     The classic game ran the same simulation on every machine, so its packet handlers trust what a packet says.
 *     In a world only the server simulates, a player's game is just a source of input, and a changed game can send
 *     any input. Here the server checks each command against what that player may do, before the simulation sees
 *     it; limits how many messages and chat lines a player may send; and writes down what it refused.
 * @par Comment:
 *     Nothing here runs on a player's machine, and nothing here changes a game that every machine simulates
 *     (the campaign, skirmish and classic multiplayer): anticheat_active() is false there.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "net_anticheat.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "bflib_basics.h"
#include "game_legacy.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "config_players.h"
#include "dungeon_data.h"
#include "map_events.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_creature.h"
#include "creature_control.h"
#include "power_hand.h"
#include "power_specials.h"
#include "roomspace.h"
#include "slab_data.h"
#include "server_admin.h"
#include "world_stream.h"
#include "map_data.h"
#include "net_exchange_common.h"
#include "post_inc.h"

/* A report is written to the logs at most this often for one user and kind; the rest are counted and summed up. */
#define AC_LOG_BURST        8      /* lines in a row */
#define AC_LOG_WINDOW_MS    60000  /* then nothing more of that kind for the rest of this time */
/* Chat: a player may send a short burst, then about one line every two seconds. */
#define AC_CHAT_BURST       6
#define AC_CHAT_REFILL_MS   2000
/* Network messages from one user in a window of time (an unmodified game sends a handful a turn: its input,
   sometimes chat). What is over waits in the connection's queue; the window is wall-clock time, so a paused
   world does not block anyone. */
#define AC_MSG_WINDOW_MS    100
#define AC_MSG_IN_WINDOW    96

struct AcUser {
    unsigned long count[ACk_Count];
    unsigned long hidden[ACk_Count];     /* reports not written since the last line of that kind */
    uint64_t log_window_start[ACk_Count];
    int log_in_window[ACk_Count];
    int chat_spent;                      /* lines of the burst used up */
    uint64_t chat_refill_at;
    uint64_t msg_window_start;
    int msg_in_window;
    TbBool msg_flood_reported;
};
static struct AcUser ac_users[MAX_NET_USERS];

TbBool anticheat_active(void)
{
    return netstate_thin_mode() && (netstate.my_id == SERVER_ID);
}

void anticheat_reset(void)
{
    memset(ac_users, 0, sizeof(ac_users));
}

void anticheat_user_reset(NetUserId user)
{
    if ((user < 0) || (user >= MAX_NET_USERS))
        return;
    memset(&ac_users[user], 0, sizeof(ac_users[user]));
}

unsigned long anticheat_count(NetUserId user, enum AnticheatKind kind)
{
    if ((user < 0) || (user >= MAX_NET_USERS) || ((int)kind < 0) || (kind >= ACk_Count))
        return 0;
    return ac_users[user].count[kind];
}

static const char *kind_name(enum AnticheatKind kind)
{
    switch (kind)
    {
    case ACk_Impossible: return "impossible command";
    case ACk_Refused:    return "refused command";
    case ACk_Flood:      return "flood";
    case ACk_Login:      return "login";
    default:             return "report";
    }
}

static const char *user_name(NetUserId user)
{
    if ((user >= 0) && (user < MAX_NET_USERS) && (netstate.users[user].name[0] != '\0'))
        return netstate.users[user].name;
    return "?";
}

void anticheat_report(NetUserId user, enum AnticheatKind kind, const char *fmt, ...)
{
    if ((user < 0) || (user >= MAX_NET_USERS) || ((int)kind < 0) || (kind >= ACk_Count))
        return;
    struct AcUser *au = &ac_users[user];
    au->count[kind]++;
    const uint64_t now = SDL_GetTicks();
    if ((au->log_window_start[kind] == 0) || (now - au->log_window_start[kind] >= AC_LOG_WINDOW_MS))
    {
        au->log_window_start[kind] = now;
        au->log_in_window[kind] = 0;
    }
    if (au->log_in_window[kind] >= AC_LOG_BURST)
    {
        au->hidden[kind]++;
        return;
    }
    au->log_in_window[kind]++;
    char text[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    char more[48] = "";
    if (au->hidden[kind] > 0)
    {
        snprintf(more, sizeof(more), " (and %lu more not shown)", au->hidden[kind]);
        au->hidden[kind] = 0;
    }
    const int plyr = (int)get_net_user_player_number(user);
    SYNCLOG("Anti-cheat: %s (user %d, player %d), %s: %s; %lu of this kind so far%s", user_name(user), (int)user, plyr,
        kind_name(kind), text, au->count[kind], more);
    server_admin_log("cheat", "%s: %s: %s (%lu so far)%s", user_name(user), kind_name(kind), text, au->count[kind], more);
}

void anticheat_clean_name(char *name)
{
    if (name == NULL)
        return;
    for (unsigned char *p = (unsigned char *)name; *p != '\0'; p++)
    {
        if ((*p < 0x20) || (*p == 0x7F))
            *p = '?';
    }
}

TbBool anticheat_chat_allowed(NetUserId user)
{
    if (!anticheat_active() || (user <= SERVER_ID) || (user >= MAX_NET_USERS))
        return true;
    struct AcUser *au = &ac_users[user];
    const uint64_t now = SDL_GetTicks();
    if (au->chat_spent <= 0)
        au->chat_refill_at = now;
    while ((au->chat_spent > 0) && (now - au->chat_refill_at >= AC_CHAT_REFILL_MS))
    {
        au->chat_spent--;
        au->chat_refill_at += AC_CHAT_REFILL_MS;
    }
    if (au->chat_spent >= AC_CHAT_BURST)
    {
        anticheat_report(user, ACk_Flood, "chat lines faster than one every %d seconds", AC_CHAT_REFILL_MS / 1000);
        return false;
    }
    au->chat_spent++;
    return true;
}

TbBool anticheat_message_allowed(NetUserId user)
{
    if (!anticheat_active() || (user <= SERVER_ID) || (user >= MAX_NET_USERS))
        return true;
    struct AcUser *au = &ac_users[user];
    const uint64_t now = SDL_GetTicks();
    if ((au->msg_window_start == 0) || (now - au->msg_window_start >= AC_MSG_WINDOW_MS))
    {
        au->msg_window_start = now;
        au->msg_in_window = 0;
        au->msg_flood_reported = false;
    }
    if (au->msg_in_window >= AC_MSG_IN_WINDOW)
    {
        if (!au->msg_flood_reported)
        {
            au->msg_flood_reported = true;
            anticheat_report(user, ACk_Flood, "more than %d network messages in a tenth of a second", AC_MSG_IN_WINDOW);
        }
        return false;
    }
    au->msg_in_window++;
    return true;
}


/******************************************************************************/
/* the checks on a packet */

/** Takes the command out of a packet; the pointer, the buttons and (where the packet has one) the camera stay. */
static void drop_action(struct Packet *pckt)
{
    const TbBool had_camera = packet_action_has_camera_position((enum TbPacketAction)pckt->action);
    pckt->action = PckA_None;
    pckt->actn_par1 = 0;
    pckt->actn_par2 = 0;
    if (!had_camera)
    {
        /* its par3/par4 sit where the camera position goes: with no action they would read as one */
        pckt->actn_par3 = 0;
        pckt->actn_par4 = 0;
    }
}

/** The work states a player's game may ask for. The others are the cheat menu's (make a creature, kill, steal a
 *  room, ...) and the "free" ones, which skip the checks of a power: only where cheats are allowed. */
static TbBool work_state_is_ordinary(long state)
{
    switch (state)
    {
    case PSt_None:
    case PSt_CtrlDungeon:
    case PSt_BuildRoom:
    case PSt_HoldInHand:
    case PSt_CallToArms:
    case PSt_CastPowerOnSubtile:
    case PSt_SightOfEvil:
    case PSt_Slap:
    case PSt_CtrlPassngr:
    case PSt_CtrlDirect:
    case PSt_CreatrQuery:
    case PSt_OrderCreatr:
    case PSt_CreatrInfo:
    case PSt_PlaceTrap:
    case PSt_PlaceDoor:
    case PST_CastPowerOnTarget:
    case PSt_Sell:
    case PST_CastGenericLevelPower:
        return true;
    default:
        return false;
    }
}

static TbBool thing_on_players_ground(const struct Thing *thing, PlayerNumber plyr_idx)
{
    return slabmap_owner(get_slabmap_thing_is_on(thing)) == plyr_idx;
}

void anticheat_check_packet(NetUserId user, struct Packet *pckt)
{
    if (!anticheat_active() || (pckt == NULL) || (user <= SERVER_ID) || (user >= MAX_NET_USERS) || is_local_user(user))
        return;
    const PlayerNumber plyr_idx = get_net_user_player_number(user);
    const struct UserState *ustate = get_user_state(user);
    if ((plyr_idx < 0) || user_state_invalid(ustate))
        return; /* a watcher: its packets do nothing in the world */
    const struct PlayerInfo *player = get_player(plyr_idx);
    const long par1 = pckt->actn_par1, par2 = pckt->actn_par2;
    switch (pckt->action)
    {
    case PckA_SetPlyrState:
        if ((par1 < 0) || (par1 >= PSt_ListEnd))
        {
            anticheat_report(user, ACk_Impossible, "work state %ld does not exist", par1);
            drop_action(pckt);
        } else if (!work_state_is_ordinary(par1) && !user_cheats_allowed(user))
        {
            anticheat_report(user, ACk_Impossible, "cheat work state %s", player_state_code_name(par1));
            drop_action(pckt);
        }
        break;
    case PckA_EventBoxTurnOff:
    case PckA_ZoomToEvent:
        if ((par1 < 1) || (par1 >= EVENTS_COUNT))
        {
            anticheat_report(user, ACk_Impossible, "event %ld does not exist", par1);
            drop_action(pckt);
        } else if (game.event[par1].owner != plyr_idx)
        {
            anticheat_report(user, ACk_Refused, "event %ld is not this player's", par1);
            drop_action(pckt);
        }
        break;
    case PckA_UseSpecialBox:
    {
        const struct Thing *thing = thing_get(par1);
        if (!thing_exists(thing) || !can_thing_be_picked_up_by_player(thing, plyr_idx))
        {
            anticheat_report(user, ACk_Refused, "special box or book %ld is not within this player's reach", par1);
            drop_action(pckt);
        }
        break;
    }
    case PckA_ResurrectCrtr:
    {
        const struct Thing *boxtng = thing_get(par1);
        const PlayerNumber owner = (PlayerNumber)(par2 & 0x0F);
        const ThingModel crmodel = (ThingModel)((par2 >> 4) & 0xFF);
        const CrtrExpLevel exp_level = (CrtrExpLevel)((par2 >> 12) & 0x0F);
        const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
        TbBool dead_one = false;
        if (!dungeon_invalid(dungeon))
        {
            for (int i = 0; (i < dungeon->dead_creatures_count) && (i < DEAD_CREATURES_MAX_COUNT); i++)
            {
                if ((dungeon->dead_creatures[i].model == crmodel) && (dungeon->dead_creatures[i].exp_level == exp_level))
                    dead_one = true;
            }
        }
        if (!thing_exists(boxtng) || (box_thing_to_special(boxtng) != SpcKind_Resurrect) || !thing_on_players_ground(boxtng, plyr_idx))
        {
            anticheat_report(user, ACk_Refused, "resurrect: thing %ld is not a Resurrect box on this player's ground", par1);
            drop_action(pckt);
        } else if ((owner != plyr_idx) || !dead_one)
        {
            anticheat_report(user, ACk_Impossible, "resurrect: %s level %d for player %d is not one of this player's dead",
                creature_code_name(crmodel), (int)exp_level + 1, (int)owner);
            drop_action(pckt);
        }
        break;
    }
    case PckA_TransferCreatr:
    {
        const struct Thing *boxtng = thing_get(par1);
        const struct Thing *creatng = thing_get(par2);
        if (!thing_exists(boxtng) || (box_thing_to_special(boxtng) != SpcKind_TrnsfrCrtr) || !thing_on_players_ground(boxtng, plyr_idx)
         || !thing_exists(creatng) || !thing_is_creature(creatng) || (creatng->owner != plyr_idx))
        {
            anticheat_report(user, ACk_Refused, "transfer: box %ld or creature %ld is not this player's", par1, par2);
            drop_action(pckt);
        }
        break;
    }
    case PckA_CtrlCrtrSetInstnc:
    {
        const struct Thing *creatng = thing_get(ustate->controlled_thing_idx);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
        if ((par1 < 0) || (par1 >= INSTANCE_TYPES_MAX))
        {
            anticheat_report(user, ACk_Impossible, "creature ability %ld does not exist", par1);
            drop_action(pckt);
        } else if ((par1 != 0) && (!thing_exists(creatng) || creature_control_invalid(cctrl) || !cctrl->instance_available[par1]))
        {
            anticheat_report(user, ACk_Refused, "creature ability %ld is not one the possessed creature has", par1);
            drop_action(pckt);
        }
        break;
    }
    case PckA_ToggleComputerProcessing:
        /* a switch for the whole world (every computer keeper), not a player's */
        anticheat_report(user, ACk_Impossible, "switching the computer keepers off or on");
        drop_action(pckt);
        break;
    case PckA_GoSpectator:
        if (player->victory_state != VicS_LostLevel)
        {
            anticheat_report(user, ACk_Refused, "going spectator without having lost");
            drop_action(pckt);
        }
        break;
    case PckA_SetViewType:
    case PckA_SaveViewType:
    case PckA_LoadViewType:
        if ((par1 < PVT_None) || (par1 > PVT_MapFadeOut))
        {
            anticheat_report(user, ACk_Impossible, "view type %ld does not exist", par1);
            drop_action(pckt);
        } else if (((par1 == PVT_CreatureContrl) || (par1 == PVT_CreaturePasngr)) && (ustate->controlled_thing_idx <= 0))
        {
            anticheat_report(user, ACk_Refused, "a creature's view with no creature possessed");
            drop_action(pckt);
        }
        break;
    case PckA_SetRoomspaceMan:
    case PckA_SetRoomspaceDefault:
        if ((par1 < 1) || (par1 > MAX_USER_ROOMSPACE_WIDTH))
        {
            anticheat_report(user, ACk_Impossible, "a room or dig box %ld slabs wide", par1);
            pckt->actn_par1 = max(1L, min(par1, (long)MAX_USER_ROOMSPACE_WIDTH));
        }
        break;
    case PckA_SetRoomspaceHighlight:
    case PckA_ApplyRoomspaceDigTag:
        if ((par2 < 0) || (par2 > MAX_USER_ROOMSPACE_WIDTH))
        {
            anticheat_report(user, ACk_Impossible, "a dig box %ld slabs wide", par2);
            pckt->actn_par2 = max(0L, min(par2, (long)MAX_USER_ROOMSPACE_WIDTH));
        }
        /* fall through */
    case PckA_RoomspaceHighlightToggle:
        if ((par1 < box_placement_mode) || (par1 > single_subtile_mode))
        {
            anticheat_report(user, ACk_Impossible, "placement mode %ld does not exist", par1);
            pckt->actn_par1 = box_placement_mode;
        }
        break;
    default:
        break;
    }
}

/******************************************************************************/
/* Test hook: a client that behaves like a changed game (KFX_NET_TESTSAY "#cheat <what>").
 *   freectrl   the work state that possesses any creature        bigbox    a dig box 5000 slabs wide
 *   event      closing event 250 (does not exist)                box       using special box / book thing 3
 *   resurrect  a level 10 Horned Reaper from nowhere             ability   creature ability 1999, then 5000
 *   comp       switching the computer keepers off                spectate  going spectator while alive
 *   view       view type 99, then a creature's view              flood     30 chat lines at once
 *   zoom X Y   the camera X, Y slabs from this keeper's heart    fog       logs what this game has been sent */
void anticheat_test(const char *what)
{
    struct PlayerInfo *player = get_my_player();
    long a = 0, b = 0;
    if (strcmp(what, "freectrl") == 0)
        set_players_packet_action(player, PckA_SetPlyrState, PSt_FreeCtrlDirect, 0, 0, 0);
    else if (strcmp(what, "bigbox") == 0)
        set_players_packet_action(player, PckA_SetRoomspaceMan, 5000, 0, 0, 0);
    else if (strcmp(what, "event") == 0)
        set_players_packet_action(player, PckA_EventBoxTurnOff, 250, 0, 0, 0);
    else if (strcmp(what, "box") == 0)
        set_players_packet_action(player, PckA_UseSpecialBox, 3, 0, 0, 0);
    else if (strcmp(what, "resurrect") == 0)
        set_players_packet_action(player, PckA_ResurrectCrtr, 3, (my_player_number & 0x0F) | (14 << 4) | (9 << 12), 0, 0);
    else if (strcmp(what, "ability") == 0)
        set_players_packet_action(player, PckA_CtrlCrtrSetInstnc, 1999, 0, 0, 0);
    else if (strcmp(what, "ability2") == 0)
        set_players_packet_action(player, PckA_CtrlCrtrSetInstnc, 5000, 0, 0, 0);
    else if (strcmp(what, "comp") == 0)
        set_players_packet_action(player, PckA_ToggleComputerProcessing, 0, 0, 0, 0);
    else if (strcmp(what, "spectate") == 0)
        set_players_packet_action(player, PckA_GoSpectator, 0, 0, 0, 0);
    else if (strcmp(what, "view") == 0)
        set_players_packet_action(player, PckA_SetViewType, 99, 0, 0, 0);
    else if (strcmp(what, "view2") == 0)
        set_players_packet_action(player, PckA_SetViewType, PVT_CreatureContrl, 0, 0, 0);
    else if (strcmp(what, "flood") == 0)
    {
        for (int i = 0; i < 30; i++)
        {
            char line[32];
            snprintf(line, sizeof(line), "flood line %d", i + 1);
            send_network_chat_message(netstate.my_id, line);
        }
    } else if (sscanf(what, "zoom %ld %ld", &a, &b) == 2)
    {
        const struct Thing *heart = find_players_dungeon_heart(my_player_number);
        if (thing_exists(heart))
            set_players_packet_action(player, PckA_ZoomToPosition,
                (heart->mappos.x.stl.num + a * STL_PER_SLB) * COORD_PER_STL, (heart->mappos.y.stl.num + b * STL_PER_SLB) * COORD_PER_STL, 0, 0);
    } else if (strcmp(what, "fog") == 0)
    {
        /* what this game holds: things that are not its keeper's, and how many of them stand on ground the keeper
           has not revealed (a fair server sends none of those) */
        long foreign = 0, hidden = 0, hidden_traps = 0, revealed_slabs = 0, known_slabs = 0;
        for (long i = 1; i < THINGS_COUNT; i++)
        {
            const struct Thing *thing = thing_get(i);
            if (!thing_exists(thing) || (thing->owner == my_player_number) || ((thing->alloc_flags & TAlF_IsInLimbo) != 0))
                continue;
            if ((thing->class_id != TCls_Creature) && (thing->class_id != TCls_Trap) && (thing->class_id != TCls_Door) && (thing->class_id != TCls_Object))
                continue;
            foreign++;
            if (!thing_revealed(thing, my_player_number))
            {
                hidden++;
                if (hidden <= 6)
                {
                    const struct Thing *heart0 = find_players_dungeon_heart(my_player_number);
                    const struct Map *blk = get_map_block_at(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
                    SYNCLOG("Anti-cheat test (this machine): on unrevealed ground: %s index %d owner %d at subtile (%d,%d), %d,%d slabs from the heart, map data here: %s",
                        thing_model_name(thing), (int)thing->index, (int)thing->owner, (int)thing->mappos.x.stl.num, (int)thing->mappos.y.stl.num,
                        thing_exists(heart0) ? (int)(thing->mappos.x.stl.num - heart0->mappos.x.stl.num) / 3 : 0,
                        thing_exists(heart0) ? (int)(thing->mappos.y.stl.num - heart0->mappos.y.stl.num) / 3 : 0,
                        map_block_invalid(blk) ? "none" : "yes");
                }
            }
            if ((thing->class_id == TCls_Trap) && !thing->trap.revealed)
                hidden_traps++;
        }
        const struct Thing *heart = find_players_dungeon_heart(my_player_number);
        if (thing_exists(heart))
        {
            /* the ground this game has map data for, in a square of 161 slabs round the heart */
            for (int dy = -80; dy <= 80; dy++)
            {
                for (int dx = -80; dx <= 80; dx++)
                {
                    const struct Map *mapblk = get_map_block_at(heart->mappos.x.stl.num + dx * STL_PER_SLB, heart->mappos.y.stl.num + dy * STL_PER_SLB);
                    if (map_block_invalid(mapblk))
                        continue;
                    known_slabs++;
                    if (map_block_revealed(mapblk, my_player_number))
                        revealed_slabs++;
                }
            }
        }
        SYNCLOG("Anti-cheat test (this machine): %ld things of others, %ld of them on ground this keeper has not revealed, "
            "%ld unrevealed traps of others; map data for %ld slabs round the heart, %ld of them revealed",
            foreign, hidden, hidden_traps, known_slabs, revealed_slabs);
        return;
    } else
    {
        SYNCLOG("Anti-cheat test (this machine): unknown word \"%s\"", what);
        return;
    }
    SYNCLOG("Anti-cheat test (this machine): sent \"%s\"", what);
}

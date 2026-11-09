/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_game.c
 *     Network game support for Dungeon Keeper.
 * @par Purpose:
 *     Functions to exchange packets through network.
 * @par Comment:
 *     None.
 * @author   KeeperFX Team
 * @date     11 Mar 2010 - 09 Oct 2010
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "net_matchmaking.h"
#include "net_game.h"

#include "globals.h"
#include "bflib_basics.h"
#include "bflib_coroutine.h"
#include "bflib_datetm.h"
#include "bflib_enet.h"
#include "net_exchange_common.h"
#include "net_lobby.h"
#include "net_main.h"
#include "net_resync.h"

#include "player_data.h"
#include "player_instances.h"
#include "front_landview.h"
#include "player_utils.h"
#include "light_data.h"
#include "packets.h"
#include "frontend.h"
#include "front_network.h"
#include "config_settings.h"
#include "config_keeperfx.h"
#include "config_strings.h"
#include "config_campaigns.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "engine_camera.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "map_events.h"
#include "net_exchange_gameplay.h"
#include "net_input_lag.h"
#include "net_checksums.h"
#include "net_spectator.h"
#include "keeperfx.hpp"
#include "world_stream.h"
#include "net_state.h"
#include "world_pages.h"
#include "world_net.h"
#include "world_menu.h"
#include "thing_creature.h"
#include "gui_frontmenu.h"
#include "server_admin.h"
#include "net_diag.h"
#include "world_caretaker.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct TbNetworkUserInfo net_user_info[MAX_NET_USERS];
extern int32_t multiplayer_speed_adjustment_ns;
/******************************************************************************/

#pragma pack(1)
struct StartupSyncPacket {
    uint8_t startup_sync_packet_valid;
    TbBigChecksum map_checksums[NETWORK_STARTUP_MAP_FILE_COUNT];
    TbBigChecksum required_sprite_zip_checksums[REQUIRED_SPRITE_ZIP_COUNT];
    UserPreferences user_prefs;
    uint8_t initial_input_lag_turns;
    uint32_t initial_action_seed;
    char frontend_alliances;
    int8_t player_slots[MAX_NET_USERS];
};
#pragma pack()

short setup_network_service(enum FrontendNetService service)
{
  struct ServiceInitData *init_data = NULL;
  SYNCMSG("Initializing 4-players type %d network", service);
  memset(net_user_info, 0, sizeof(net_user_info));
  network_lobby_ping = 0;
  if (service != FrontendNetSvc_Online && service != FrontendNetSvc_LAN) {
    process_network_error(-800);
    return 0;
  }
  if ( LbNetwork_Init(NS_ENET_UDP, STOCK_NET_USERS, &net_user_info[0], init_data) )
  {
    process_network_error(-800);
    return 0;
  }
  net_service_index_selected = service;
  if (service == FrontendNetSvc_LAN) {
    frontend_button_info[11].capstr_idx = GUIStr_MnuLanLobby;
    frontend_button_info[12].capstr_idx = GUIStr_MnuLanLobbies;
  } else {
    frontend_button_info[11].capstr_idx = GUIStr_MnuOnlineLobby;
    frontend_button_info[12].capstr_idx = GUIStr_MnuOnlineLobbies;
  }
  frontend_set_state(FeSt_NET_SESSION);
  return 1;
}

int setup_old_network_service(void)
{
    return setup_network_service(net_service_index_selected);
}

TbBool network_is_host(void)
{
    return netstate.my_id == SERVER_ID;
}

// user->player map received with the spectator bootstrap, applied once the level is set up
static PlayerNumber spectator_user_player_number[MAX_NET_USERS];

PlayerNumber get_net_user_player_number(NetUserId user)
{
    if ((user < 0) || (user >= MAX_NET_USERS)) {
        return PLAYER_NONE;
    }
    return game.user_states[user].player_id;
}

// user exists to the game layer (even if their connection has dropped,
// the game needs to hear about it through the proper channel first to avoid
// nondeterminism.)
TbBool user_present(NetUserId user)
{
    return get_net_user_player_number(user) >= 0;
}

void set_net_user_player_number(NetUserId user, PlayerNumber plyr_idx)
{
    if ((user < 0) || (user >= MAX_NET_USERS)) {
        return;
    }
    game.user_states[user].player_id = plyr_idx;
}

static const unsigned char user_preference_flags[UPref_Count] = {
    [UPref_FrontView]             = 0,
    [UPref_Wibble]                = UPF_ApplyOnLoad,
    [UPref_Cheats]                = UPF_ApplyOnLoad | UPF_ApplyOnTakeover,
    [UPref_SkipHeartZoom]         = UPF_ApplyOnLoad,
    [UPref_StartingHighlightMode] = 0,
    [UPref_StartingIsometricTilt] = UPF_ApplyOnLoad,
    [UPref_MaxZoomIso]            = UPF_ApplyOnLoad,
    [UPref_MaxZoomFrontview]      = UPF_ApplyOnLoad,
    [UPref_Censorship]            = UPF_ApplyOnLoad | UPF_ApplyOnTakeover,
    [UPref_WallHeight]            = UPF_ApplyOnLoad,
    [UPref_MapFade]               = UPF_ApplyOnLoad | UPF_ApplyOnTakeover,
    [UPref_StartingIsometricZoom] = 0,
    [UPref_StartingFrontviewZoom] = 0,
    [UPref_StartingTendencies]    = 0,
    [UPref_MinimapZoom]           = UPF_ApplyOnLoad,
    [UPref_StartingComputerAssistantKind] = UPF_ApplyOnLoad,
    [UPref_ComputerAssistantDisabled] = UPF_ApplyOnLoad | UPF_ApplyOnTakeover,
};

void build_local_user_preferences(UserPreferences prefs)
{
    memset(prefs, 0, sizeof(UserPreferences));
    TbBool front_view = false;
    TbBool wibble = true;
    rotate_mode_to_dungeon_view(settings.video_rotate_mode, &front_view, &wibble);
    prefs[UPref_FrontView] = front_view;
    prefs[UPref_Wibble] = wibble;
    prefs[UPref_Cheats] = start_params.easter_egg || frontnet_service_selected(FrontendNetSvc_Skirmish);
    if (network_is_active() && network_is_host()) {
        prefs[UPref_Cheats] = net_config_info.cheats_enabled;
        prefs[UPref_ComputerAssistantDisabled] = !net_config_info.computer_assistant;
    }
    prefs[UPref_SkipHeartZoom] = get_skip_heart_zoom_feature();
    prefs[UPref_StartingHighlightMode] = get_starting_highlight_mode();
    prefs[UPref_StartingIsometricTilt] = settings.isometric_tilt;
    prefs[UPref_MaxZoomIso] = zoom_distance_setting;
    prefs[UPref_MaxZoomFrontview] = frontview_zoom_distance_setting;
    prefs[UPref_Censorship] = local_censorship_enabled();
    prefs[UPref_WallHeight] = settings.video_cluedo_mode;
    prefs[UPref_MapFade] = min(get_parchment_map_fade_turns(), PARCHMENT_MAP_FADE_MAX_TURNS);
    prefs[UPref_StartingIsometricZoom] = settings.isometric_view_zoom_level;
    prefs[UPref_StartingFrontviewZoom] = settings.frontview_zoom_level;
    if (IMPRISON_BUTTON_DEFAULT)
        prefs[UPref_StartingTendencies] |= CrTend_Imprison;
    if (FLEE_BUTTON_DEFAULT)
        prefs[UPref_StartingTendencies] |= CrTend_Flee;
    prefs[UPref_MinimapZoom] = settings.minimap_zoom;
    prefs[UPref_StartingComputerAssistantKind] = settings.computer_assistant_kind;
}

static void apply_user_starting_preferences(struct UserState *ustate, unsigned char flags)
{
    const uint32_t *prefs = ustate->prefs;
    if (flags != UPF_ApplyOnTakeover)
        ustate->dungeon_camera.pitch = clamp((int32_t)prefs[UPref_StartingIsometricTilt], CAMERA_TILT_MIN, CAMERA_TILT_MAX);
    if (flags != UPF_NewGame)
        return;
    ustate->dungeon_camera.zoom[false] = (prefs[UPref_StartingIsometricZoom] != 0) ? (int32_t)prefs[UPref_StartingIsometricZoom] : CAMERA_ZOOM_MAX;
    ustate->dungeon_camera.zoom[true] = (prefs[UPref_StartingFrontviewZoom] != 0) ? (int32_t)prefs[UPref_StartingFrontviewZoom] : FRONTVIEW_CAMERA_ZOOM_MAX;
}

void apply_user_preferences(NetUserId user, const UserPreferences prefs, unsigned char flags)
{
    struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate))
        return;
    for (int pref = 0; pref < UPref_Count; pref++)
    {
        if ((flags == UPF_NewGame) || ((user_preference_flags[pref] & flags) != 0))
            ustate->prefs[pref] = prefs[pref];
    }
    apply_user_starting_preferences(ustate, flags);
}

void apply_local_user_preferences(NetUserId user, unsigned char flags)
{
    UserPreferences prefs;
    build_local_user_preferences(prefs);
    apply_user_preferences(user, prefs, flags);
}

void apply_user_start_tendencies(NetUserId user)
{
    const struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate))
        return;
    struct PlayerInfo *player = get_user_player(user);
    TbBool imprison = (ustate->prefs[UPref_StartingTendencies] & CrTend_Imprison) != 0;
    TbBool flee = (ustate->prefs[UPref_StartingTendencies] & CrTend_Flee) != 0;
    set_creature_tendencies(player, CrTend_Imprison, imprison);
    set_creature_tendencies(player, CrTend_Flee, flee);
    if (player->id_number == my_player_number) {
        game.creatures_tend_imprison = imprison;
        game.creatures_tend_flee = flee;
    }
}

TbBool user_cheats_allowed(NetUserId user)
{
    if (get_user_state(SERVER_ID)->prefs[UPref_Cheats] == 0)
        return false;
    if ((game.game_kind == GKind_MultiGame) || !user_present(user))
        return true;
    return get_user_state(user)->prefs[UPref_Cheats] != 0;
}

TbBool computer_assistant_allowed(void)
{
    return get_user_state(SERVER_ID)->prefs[UPref_ComputerAssistantDisabled] == 0;
}

TbBool game_censorship_enabled(void)
{
    for (NetUserId user = 0; user < MAX_NET_USERS; user++)
    {
        if (user_present(user) && (get_user_state(user)->prefs[UPref_Censorship] != 0))
            return true;
    }
    return false;
}

static void setup_players_from_startup_packets(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    for (NetUserId i = 0; i < MAX_NET_USERS; i++) {
        const struct StartupSyncPacket *sync = &startup_sync_packets[i];
        if (!net_user_info[i].network_user_active) {
            continue;
        }
        int k = get_net_user_player_number(i);
        if (k < 0) {
            continue;
        }
        struct PlayerInfo *player = get_player(k);
        player->id_number = k;
        player->allocflags |= PlaF_Allocated | PlaF_Seat;
        init_user_state(i, k);
        apply_user_preferences(i, sync->user_prefs, UPF_NewGame);
        init_player(player, 0);
        apply_user_start_tendencies(i);
        snprintf(player->player_name, sizeof(struct TbNetworkPlayerName), "%s", network_user_name(i));
    }
}

static TbBool verify_map_checksums(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    const TbBigChecksum *host = startup_sync_packets[SERVER_ID].map_checksums;
    for (int i = 0; i < MAX_NET_USERS; i++) {
        const TbBigChecksum *client = startup_sync_packets[i].map_checksums;
        if (!net_user_info[i].network_user_active) {
            continue;
        }
        int diff_count = 0;
        for (int j = 0; j < NETWORK_STARTUP_MAP_FILE_COUNT; j++) {
            if (client[j] == host[j]) {
                continue;
            }
            if (diff_count == 0) {
                ERRORLOG("Level checksums differ for player %d", i);
            }
            ERRORLOG("Level file map%05u.%s differs for player %d", get_loaded_level_number(), network_startup_compare_files[j], i);
            diff_count++;
        }
        if (diff_count != 0) {
            return false;
        }
    }
    NETLOG("Map checksums are verified");
    return true;
}

static TbBool verify_startup_sprite_zip_checksums(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    NetUserId host_user_id = SERVER_ID;
    const struct StartupSyncPacket *host_sync = &startup_sync_packets[host_user_id];
    TbBool verified = true;
    for (NetUserId i = 0; i < MAX_NET_USERS; i++) {
        if (!net_user_info[i].network_user_active || i == host_user_id) {
            continue;
        }
        const struct StartupSyncPacket *client_sync = &startup_sync_packets[i];
        for (int zip_idx = 0; zip_idx < REQUIRED_SPRITE_ZIP_COUNT; zip_idx++) {
            if (client_sync->required_sprite_zip_checksums[zip_idx] == host_sync->required_sprite_zip_checksums[zip_idx]) {
                continue;
            }
            WARNLOG("Custom sprite zip differs between %s and %s: %s", network_user_name(host_user_id), network_user_name(i), required_sprite_zips[zip_idx]);
            message_add_fmt(MsgType_Blank, 0, "/fxdata/%.30s differs for %.12s", required_sprite_zips[zip_idx], network_user_name(i));
            verified = false;
        }
    }
    return verified;
}

static struct StartupSyncPacket s_local_startup_sync;
static struct StartupSyncPacket s_startup_sync_packets[MAX_NET_USERS];
/* a joiner's bootstrap carries every user's startup packet in one message */
_Static_assert(sizeof(int32_t) + DISKPATH_SIZE + sizeof(PlayerNumber) * MAX_NET_USERS + sizeof(s_startup_sync_packets) + 16
    <= NET_MSG_BUFFER_SIZE, "the spectator bootstrap doesn't fit a network message");

static TbBool reject_network_startup(enum NetJoinRejection reason)
{
    netstate.leave_reason = reason;
    create_frontend_error_box(net_join_error_text(reason));
    return false;
}

static TbBool startup_peers_accepted(void)
{
    for (NetUserId i = 0; i < MAX_NET_USERS; i++) {
        if (netstate.users_leave_reason[i] != NetJoin_Accepted) {
            return reject_network_startup(netstate.users_leave_reason[i]);
        }
    }
    return true;
}

static enum NetJoinRejection spectator_startup_rejection(void)
{
    TbBigChecksum checksums[NETWORK_STARTUP_MAP_FILE_COUNT];
    calculate_network_startup_map_checksums(checksums);
    if (memcmp(checksums, s_startup_sync_packets[SERVER_ID].map_checksums, sizeof(checksums)) != 0) {
        return NetJoin_MapMismatch;
    }
    if (memcmp(required_sprite_zip_checksums, s_startup_sync_packets[SERVER_ID].required_sprite_zip_checksums, sizeof(required_sprite_zip_checksums)) != 0) {
        return NetJoin_FxdataMismatch;
    }
    return NetJoin_Accepted;
}

// the preferences a user sent in the startup sync, for network games
TbBool get_startup_user_preferences(NetUserId user, UserPreferences prefs)
{
    if (!network_is_active() || (user < 0) || (user >= MAX_NET_USERS) || !s_startup_sync_packets[user].startup_sync_packet_valid)
        return false;
    memcpy(prefs, s_startup_sync_packets[user].user_prefs, sizeof(UserPreferences));
    return true;
}



static uint8_t calculate_initial_input_lag(void)
{
    int32_t player_count = 0;
    for (int32_t i = 0; i < MAX_NET_USERS; i++) {
        if (net_user_info[i].network_user_active) {
            player_count++;
        }
    }
    uint64_t ping = network_lobby_ping;
    if (player_count == 2) {
        ping /= 2;
    }
    uint64_t input_lag_turns = 0;
    if (turns_per_second > 0) {
        input_lag_turns = (ping * turns_per_second + 999) / 1000;
    }
    uint64_t uncapped_input_lag_turns = input_lag_turns;
    if (input_lag_turns > 0) {
        input_lag_turns -= 1;
    }
    if (input_lag_turns > MAXIMUM_INPUT_LAG_TURNS) {
        input_lag_turns = MAXIMUM_INPUT_LAG_TURNS;
    }
    JUSTLOG("Initial input lag: (%llu ms * %d turns/s + 999) / 1000 = %llu turns, adjusted to %llu", (unsigned long long)ping, turns_per_second, (unsigned long long)uncapped_input_lag_turns, (unsigned long long)input_lag_turns);
    return input_lag_turns;
}

static void build_local_startup_sync(void)
{
    memset(&s_local_startup_sync, 0, sizeof(s_local_startup_sync));
    s_local_startup_sync.startup_sync_packet_valid = 1;
    calculate_network_startup_map_checksums(s_local_startup_sync.map_checksums);
    memcpy(s_local_startup_sync.required_sprite_zip_checksums, required_sprite_zip_checksums, sizeof(s_local_startup_sync.required_sprite_zip_checksums));
    build_local_user_preferences(s_local_startup_sync.user_prefs);
    s_local_startup_sync.initial_input_lag_turns = calculate_initial_input_lag();
    s_local_startup_sync.initial_action_seed = (uint32_t)initial_replay_seed;
    net_resolve_player_colours(s_local_startup_sync.player_slots, &s_local_startup_sync.frontend_alliances);
}

static TbBool setup_network_player_numbers(const int8_t player_slots[MAX_NET_USERS])
{
    SYNCDBG(6, "Starting");
    for (NetUserId i = 0; i < MAX_NET_USERS; i++) {
        set_net_user_player_number(i, PLAYER_NONE);
        if (!net_user_info[i].network_user_active) {
            continue;
        }
        if (player_slots[i] < PLAYER_NONE || player_slots[i] >= MAX_NET_USERS) {
            ERRORLOG("Network user %d has invalid player slot %d", (int)i, (int)player_slots[i]);
            return false;
        }
        set_net_user_player_number(i, player_slots[i]);
        netstate.users[i].spectator_conversion_requested = (player_slots[i] == PLAYER_NONE);
    }
    my_player_number = PLAYER_NEUTRAL;
    if (user_present(netstate.my_id)) {
        my_player_number = get_net_user_player_number(netstate.my_id);
    }
    return true;
}

static TbBool net_startup_sync_exchange_and_apply(void)
{
    memset(s_startup_sync_packets, 0, sizeof(s_startup_sync_packets));
    TbError exchange_result = exchange_frame_block(NETMSG_STARTUP_SYNC, &s_local_startup_sync, s_startup_sync_packets, sizeof(struct StartupSyncPacket));
    if (!startup_peers_accepted()) {
        return false;
    }
    if (exchange_result != Lb_OK) {
        ERRORLOG("Startup sync exchange failed");
        return false;
    }

    for (int i = 0; i < MAX_NET_USERS; i++) {
        if (net_user_info[i].network_user_active && !s_startup_sync_packets[i].startup_sync_packet_valid) {
            ERRORLOG("Startup sync exchange missed one or more peers");
            return false;
        }
    }
    if (!verify_map_checksums(s_startup_sync_packets)) {
        return reject_network_startup(NetJoin_MapMismatch);
    }

    if (!verify_startup_sprite_zip_checksums(s_startup_sync_packets)) {
        return reject_network_startup(NetJoin_FxdataMismatch);
    }
    const struct StartupSyncPacket *host_sync = &s_startup_sync_packets[SERVER_ID];
    game.input_lag_turns = host_sync->initial_input_lag_turns;
    input_lag_reset();
    game.skip_initial_input_turns = calculate_skip_input();
    NETLOG("Startup input lag: %d", game.input_lag_turns);
    if (host_sync->initial_action_seed != (uint32_t)initial_replay_seed)
    {
        ERRORLOG("Initial action seed %u differs from host's %u", (unsigned)initial_replay_seed, (unsigned)host_sync->initial_action_seed);
        initial_replay_seed = host_sync->initial_action_seed;
    }
    frontend_alliances = host_sync->frontend_alliances;
    net_store_lobby_empty_seats(host_sync->player_slots);
    if (!setup_network_player_numbers(host_sync->player_slots)) {
        return false;
    }
    setup_players_from_startup_packets(s_startup_sync_packets);
    return true;
}

void setup_count_players(void)
{
  if (game.game_kind == GKind_LocalGame)
  {
    game.human_players_count = 1;
  } else
  {
    game.human_players_count = 0;
    for (int i = 0; i < MAX_NET_USERS; i++)
    {
      if (user_present(i))
        game.human_players_count++;
    }
  }
}

TbBool init_players_network_game(void)
{
    if (net_join_role == NetRole_Spectator) {
        enum NetJoinRejection rejection = spectator_startup_rejection();
        if (rejection != NetJoin_Accepted) {
            reject_network_startup(rejection);
            LbNetwork_Stop();
            return false;
        }
        for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
            set_net_user_player_number(user, spectator_user_player_number[user]);
        }
        setup_players_from_startup_packets(s_startup_sync_packets);
        return true;
    }
    SYNCDBG(4,"Starting");
    TbBool initialized = true;
    for (int zip_idx = 0; zip_idx < REQUIRED_SPRITE_ZIP_COUNT; zip_idx++) {
        if (required_sprite_zip_checksums[zip_idx] != 0) {
            continue;
        }
        WARNLOG("Required custom sprite zip missing: %s", required_sprite_zips[zip_idx]);
        message_add_fmt(MsgType_Blank, 0, "/fxdata/%.30s missing", required_sprite_zips[zip_idx]);
        initialized = reject_network_startup(NetJoin_FxdataMismatch);
        break;
    }
    if (initialized) {
        build_local_startup_sync();
        initialized = net_startup_sync_exchange_and_apply() && network_spectator_convert_seatless_users();
    }
    if (!initialized) {
        LbNetwork_Stop();
    }
    return initialized;
}

void network_game_started(void)
{
    net_lobby_set_phase(NetPhase_InGame);
    if (netstate.my_id == SERVER_ID && frontnet_service_selected(FrontendNetSvc_Online)) {
        LevelNumber map_number = get_level_number();
        struct LevelInformation *level_info = get_level_info(map_number);
        const char *map_name = "";
        if (level_info) {
            map_name = level_info->name;
            if (level_info->name_stridx > 0) {
                map_name = get_string(level_info->name_stridx);
            }
        }
        matchmaking_start_game((int)map_number, map_name);
    }
}

/** Check whether a network user is active.
 *
 * @param user
 * @return
 */
TbBool network_user_active(NetUserId user)
{
    if ((user < 0) || (user >= MAX_NET_USERS))
        return false;
    return (net_user_info[user].network_user_active != 0);
}

const char *network_user_name(NetUserId user)
{
    if ((user < 0) || (user >= MAX_NET_USERS))
        return NULL;
    return net_user_info[user].name;
}

TbBool network_human_contenders_remain(void)
{
    for (PlayerNumber player_idx = 0; player_idx < PLAYERS_COUNT; player_idx++) {
        struct PlayerInfo *player = get_player(player_idx);
        if (is_active_keeper(player) && ((player->allocflags & PlaF_CompCtrl) == 0) && !player_cannot_win(player_idx)) {
            return true;
        }
    }
    return false;
}

static TbBool network_has_remote_users_remaining(void)
{
    for (NetUserId user_id = 0; user_id < MAX_NET_USERS; user_id += 1) {
        if (user_id == netstate.my_id) {
            continue;
        }
        if ((user_id < (NetUserId)netstate.max_users) && (netstate.users[user_id].progress != USER_UNUSED)) {
            return true;
        }
        if (user_present(user_id)) {
            const struct PlayerInfo *player = get_player(get_net_user_player_number(user_id));
            if (player_exists(player) && ((player->allocflags & PlaF_CompCtrl) == 0)) {
                return true;
            }
        }
    }
    return false;
}

static TbBool replay_has_remote_humans(void)
{
    const NetUserId local_user = replay.head.recording_user;
    for (NetUserId user_id = 0; user_id < MAX_NET_USERS; user_id++) {
        const PlayerNumber plyr_idx = get_net_user_player_number(user_id);
        if ((user_id == local_user) || (plyr_idx < 0)) {
            continue;
        }
        const struct PlayerInfo *player = get_player(plyr_idx);
        if (player_exists(player) && ((player->allocflags & PlaF_CompCtrl) == 0)) {
            return true;
        }
    }
    return false;
}

static void replace_network_player_with_ai(struct PlayerInfo *player)
{
    player->allocflags |= PlaF_CompCtrl | PlaF_Placeholder;
    // seat_network_user undoes this when a spectator takes the seat
    setup_a_computer_player(player->id_number, get_random_skirmish_computer_model());
    toggle_computer_player(player->id_number);
    message_add(MsgType_Player, player->id_number, get_string(GUIStr_NetAiTookOver));
    JUSTLOG("p:%d computer took over", player->id_number);
}

// used when ending a netplay game or recording.
// local single-player must have the local user in slot 0.
static void forget_user(NetUserId user)
{
    struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate)) {
        return;
    }
    delete_power_hand(user);
    if (ustate->cursor_light_idx != 0) {
        light_delete_light(ustate->cursor_light_idx);
    }
    for (PlayerNumber plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++) {
        struct PlayerInfo *player = get_player(plyr_idx);
        if (player->roomspace.is_active && player->roomspace.user == user) {
            player->roomspace.is_active = false;
        }
    }
    PlayerNumber reconnect_player_id = ustate->player_id;
    memset(ustate, 0, sizeof(*ustate));
    ustate->player_id = PLAYER_NONE;
    ustate->reconnect_player_id = reconnect_player_id;
}

void remap_user_to_solo(struct PlayerInfo *myplyr, NetUserId old_user)
{
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        if (user != old_user) {
            forget_user(user);
        }
    }
    struct UserState *old_state = get_user_state(old_user);
    if ((old_user != SOLO_HUMAN_ID) && !user_state_invalid(old_state)) {
        *get_user_state(SOLO_HUMAN_ID) = *old_state;
        memset(old_state, 0, sizeof(*old_state));
        old_state->player_id = PLAYER_NONE;
    }
    set_net_user_player_number(SOLO_HUMAN_ID, myplyr->id_number);
    if (myplyr->roomspace.is_active && (myplyr->roomspace.user == old_user)) {
        myplyr->roomspace.user = SOLO_HUMAN_ID;
    }
}

static void stop_network_game_state(void)
{
    const NetUserId local_user = get_local_user();
    LbNetwork_Stop();
    network_spectator_clear_roles();
    memset(net_user_info, 0, sizeof(net_user_info));
    remap_user_to_solo(get_my_player(), local_user);
    clear_flag(local_system_flags, GSF_NetGameNoSync);
    clear_flag(local_system_flags, GSF_NetSeedNoSync);
    fe_network_active = 0;
    game.game_kind = GKind_LocalGame;
    game.input_lag_turns = 0;
    game.skip_initial_input_turns = 0;
    input_lag_reset();
    multiplayer_speed_adjustment_ns = 0;
    setup_count_players();
}

static void stop_network_game_and_quit_to_main_menu(void)
{
    stop_network_game_state();
    quit_game = 1;
}

static void stop_network_game_and_continue_locally(void)
{
    struct PlayerInfo *survivor;
    if (network_is_active()) {
        stop_network_game_state();
        survivor = get_my_player();
    } else {
        NetUserId survivor_user = replay.head.recording_user;
        PlayerNumber plyr_idx = get_net_user_player_number(survivor_user);
        if (plyr_idx < 0) {
            survivor_user = get_local_user();
            plyr_idx = my_player_number;
        }
        survivor = get_player(plyr_idx);
        remap_user_to_solo(survivor, survivor_user);
        game.game_kind = GKind_LocalGame;
        setup_count_players();
    }
    survivor->display_objective_turn = get_gameturn() + 1;
}

static TbBool host_already_won_level(void)
{
    GameTurn newest_turn = get_gameturn();
    for (GameTurnDelta offset = 0; offset <= game.input_lag_turns; offset += 1) {
        if ((GameTurn)offset > newest_turn) {
            break;
        }
        const struct Packet *host_packet = get_history_packet(SERVER_ID, newest_turn - offset);
        if (host_packet != NULL && host_packet->action == PckA_FinishGame && host_packet->actn_par1 == VicS_WonLevel) {
            return true;
        }
    }
    return false;
}

// placeholders stay open for their human to return until their outcome
// is settled, then they are deallocated
void resolve_placeholders(void)
{
    if (world_mode_active()) {
        return; /* an endless world keeps every seat for its player, a defeated one too (audit 2026-11-09) */
    }
    for (PlayerNumber plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++) {
        struct PlayerInfo *player = get_player(plyr_idx);
        if (!player_exists(player) || !player_is_placeholder(player)) {
            continue;
        }
        if ((player->victory_state == VicS_WonLevel) || player_defeat_settled(plyr_idx)) {
            player->allocflags &= ~PlaF_Allocated;
        }
    }
}

void hand_waiting_placeholders_to_ai(void)
{
    if (world_mode_active()) {
        /* an endless world: a caretaker keeps an away player's dungeon (defends, trains, researches; no digging or
           building), on a server too. This used to run only while a server's network was still starting, and gave
           every away player a full computer keeper that dug its dungeon out for the whole session */
        for (PlayerNumber plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++) {
            struct PlayerInfo *player = get_player(plyr_idx);
            if (!player_exists(player) || !player_is_placeholder(player)
                || (player->victory_state != VicS_Undecided) || flag_is_set(player->allocflags, PlaF_CompCtrl)) {
                continue;
            }
            world_caretaker_start(plyr_idx);
        }
        return;
    }
    if (netstate_thin_mode()) {
        return; /* a server: a departed player's dungeon stays as it is until they come back */
    }
    for (PlayerNumber plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++) {
        struct PlayerInfo *player = get_player(plyr_idx);
        if (!player_exists(player) || !player_is_placeholder(player)
            || (player->victory_state != VicS_Undecided) || flag_is_set(player->allocflags, PlaF_CompCtrl)) {
            continue;
        }
        replace_network_player_with_ai(player);
    }
}

void seat_network_user(NetUserId user, PlayerNumber plyr_idx, const UserPreferences seat_prefs)
{
    struct PlayerInfo *player = get_player(plyr_idx);
    JUSTLOG("p:%d user %d took over from the computer", (int)plyr_idx, (int)user);
    player->allocflags &= ~(PlaF_CompCtrl | PlaF_Placeholder);
    if ((get_players_dungeon(player)->computer_enabled & 0x01) != 0) {
        toggle_computer_player(plyr_idx);
    }
    world_caretaker_end(plyr_idx); /* an endless world: the player is back */
    init_user_state(user, plyr_idx);
    UserPreferences prefs;
    if (!get_startup_user_preferences(user, prefs)) {
        memcpy(prefs, seat_prefs, sizeof(UserPreferences));
    }
    apply_user_preferences(user, prefs, UPF_NewGame);
    apply_user_preferences(user, seat_prefs, UPF_ApplyOnLoad);
    init_user_controls(get_user_state(user));
    reset_user_view_type(user, PVT_DungeonTop);
    init_player_cameras(player);
    snprintf(player->player_name, sizeof(player->player_name), "%s", network_user_name(user));
    setup_count_players();
}

static TbBool abandon_network_player(struct PlayerInfo *player, const char *announce_fmt)
{
    if ((player->allocflags & PlaF_CompCtrl) == 0) {
        if (network_is_active()) {
            // re-negotiate input latency
            network_lobby_ping = GetPlayersPing();
            input_lag_reset_request(calculate_initial_input_lag());
        }
        if (announce_fmt != NULL && player->player_name[0] != '\0' && player->victory_state != VicS_LostLevel) {
            message_add_fmt(MsgType_Blank, 0, announce_fmt, player->player_name);
        }
        JUSTLOG("p:%d player %s departed", player->id_number, player->player_name);
        if ((player->victory_state == VicS_Undecided) || (world_mode_active() && (player->victory_state == VicS_LostLevel))) {
            /* an endless world keeps a defeated seat too: its player comes back to it and can start again there */
            player->allocflags |= PlaF_Placeholder;
        }
    }
    resolve_placeholders();
    if ((player->victory_state != VicS_Undecided) && !(world_mode_active() && player_is_placeholder(player))) {
        player->allocflags &= ~PlaF_Allocated;
        return false;
    }
    return player_is_placeholder(player);
}

// Holds a departed player's seat for reconnect, or writes off a decided one.
static TbBool remove_user_from_game(NetUserId user, const char *announce_fmt)
{
    struct UserState *ustate = get_user_state(user);
    if (user_state_invalid(ustate) || (ustate->player_id == PLAYER_NONE)) {
        return false;
    }
    struct PlayerInfo *player = get_player(ustate->player_id);
    JUSTLOG("u:%d user left the game (player %d)", (int)user, (int)ustate->player_id);
    forget_user(user);
    // another user may still be driving this keeper
    if (!player_exists(player) || get_player_primary_user(player) >= 0) {
        return false;
    }
    return abandon_network_player(player, announce_fmt);
}

static void leave_network_if_alone(void)
{
    if (netstate_thin_mode()) {
        return; /* a server keeps its session open for people to join */
    }
    if (network_is_host() && (net_config_info.spectators_enabled || net_lobby_spectator_count() > 0 || network_spectator_seat_open(NULL))) {
        return;
    }
    if (!network_has_remote_users_remaining()) {
        stop_network_game_and_continue_locally();
    }
}

void process_user_leave_game_packet(NetUserId user)
{
    if (network_is_active() && network_user_is_spectator(netstate.my_id) && user == SERVER_ID) {
        stop_network_game_and_quit_to_main_menu();
        return;
    }
    if (user == get_local_user()) {
        if (network_is_active()) {
            stop_network_game_and_quit_to_main_menu();
        } else {
            quit_game = 1;
        }
        get_my_player()->allocflags &= ~PlaF_Allocated;
        return;
    }
    if (game.game_kind != GKind_MultiGame) {
        // the user's keeper simply vanishes
        struct PlayerInfo *player = get_player(get_net_user_player_number(user));
        forget_user(user);
        if (player_exists(player) && get_player_primary_user(player) < 0) {
            player->allocflags &= ~(PlaF_Allocated | PlaF_Seat);
        }
        return;
    }

    // note: packet processed in sync with simulation, it's okay to do this.
    // replays take this path too, so the departure is visible there.
    if (network_is_active()) {
        OnDroppedUser(user, NETDROP_MANUAL);
    }
    const TbBool timed_out = (get_packet(user)->actn_par1 == NET_LEAVE_TIMED_OUT);
    const char *announce_fmt = get_string(GUIStr_NetPlayerLeftGame);
    if (timed_out) {
        announce_fmt = get_string(GUIStr_NetPlayerTimedOut);
    }
    if (remove_user_from_game(user, announce_fmt) && network_is_active() && (user != SERVER_ID)) {
        if (timed_out && !netstate_thin_mode()) {
            process_pause_packet(1, 0);
        }
        net_lobby_refresh_metadata();
    }
    if (network_is_active()) {
        leave_network_if_alone();
    } else if (replay.load_enable && !replay_has_remote_humans()) {
        stop_network_game_and_continue_locally();
    }
}

// (host-only) host sends packets for dropped users, indicating
// the user has dropped.
void host_spoof_dropped_user_packets(void)
{
    if (!network_is_active() || !network_is_host()) {
        return;
    }
    GameTurn first_turn = get_gameturn();
    if (first_turn > (GameTurn)game.input_lag_turns) {
        first_turn -= game.input_lag_turns;
    }
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        if ((user == netstate.my_id) || network_user_active(user) || !user_present(user)) {
            continue;
        }
        struct Packet spoofed;
        memset(&spoofed, 0, sizeof(spoofed));
        int32_t leave_reason = 0;
        if (netstate.users_timed_out[user]) {
            leave_reason = NET_LEAVE_TIMED_OUT;
        }
        set_packet_action(&spoofed, PckA_ForceApplicationClose, leave_reason, 0, 0, 0);
        // only the turns user_has_required_turn_packets expects
        for (GameTurn turn = first_turn; turn < first_turn + 2; turn++) {
            if (get_history_packet(user, turn) != NULL) {
                continue;
            }
            spoofed.turn = turn;
            store_packet_history(user, &spoofed);
        }
    }
}

static void remove_remote_users_after_host_lost(NetUserId local_user)
{
    remove_user_from_game(SERVER_ID, get_string(GUIStr_NetPlayerTimedOut));
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        if (user != local_user) {
            remove_user_from_game(user, NULL);
        }
    }
}

void process_disconnected_network_players(void)
{
    if (!network_is_active() || network_is_host() || (netstate.users[SERVER_ID].progress != USER_UNUSED)) {
        return;
    }
    struct UserState *ustate = get_local_user_state();
    if (host_already_won_level()) {
        ustate->additional_flags &= ~UsrAF_UnlockedLordTorture;
        quit_game = 1;
        return;
    }
    if (netstate.my_id >= MAX_NET_USERS) {
        const char *error = net_join_error_text(net_join_rejection);
        if (error != NULL) {
            queue_frontend_error_box(error);
        }
        stop_network_game_and_quit_to_main_menu();
        return;
    }
    remove_remote_users_after_host_lost(netstate.my_id);
    replay_record_network_stopped();
    stop_network_game_and_continue_locally();
}

/** A thin client whose server is gone (the host closed the world, or it stopped answering): back to the menu with a
 *  message. It can't play on alone as a lockstep client does: it never simulated the world. */
TbBool network_thin_client_server_gone(void)
{
    if (!network_is_active() || network_is_host() || (netstate.users[SERVER_ID].progress != USER_UNUSED))
        return false;
    const TbBool timed_out = netstate.users_timed_out[SERVER_ID];
    SYNCLOG("Thin client: the server is gone (%s)", timed_out ? "it stopped answering" : "it closed the connection");
    const char *why = server_farewell_text();
    if ((why == NULL) && (net_join_rejection >= NetJoin_Kicked) && (net_join_rejection <= NetJoin_BannedJoin))
        why = net_join_error_text(net_join_rejection);
    char lost[320];
    if ((why == NULL) && timed_out)
    {
        /* why, as ENet saw it: how long nothing came, the ping and the packet loss before */
        const char *detail = enet_server_drop_detail();
        if (detail[0] != '\0')
            snprintf(lost, sizeof(lost), "The connection to the world's server was lost: %s.", detail);
        else
            snprintf(lost, sizeof(lost), "The connection to the world's server was lost.");
        why = lost;
    }
    LbNetLog("Thin client: back to the World List: %s\n", (why != NULL) ? why : "the server closed the world");
    world_menu_return_to_list((why != NULL) ? why : "The world's server has closed.");
    server_farewell_set(NULL);
    turn_off_all_menus();
    free_swipe_graphic();
    stop_network_game_and_quit_to_main_menu();
    return true;
}

/** A thin client leaving (Quit, or closing the game): the quit has gone to the server, which keeps the dungeon for
 *  when the player comes back; this machine leaves at once. Nothing is saved here: the world is the server's. */
void network_thin_client_leave(TbBool close_app)
{
    SYNCLOG("Thin client: leaving the world%s", close_app ? " and closing the game" : "");
    world_menu_return_to_list(NULL);
    turn_off_all_menus();
    free_swipe_graphic();
    stop_network_game_and_quit_to_main_menu();
    if (close_app)
        exit_keeper = 1;
}

void apply_recorded_network_stop(void)
{
    remove_remote_users_after_host_lost(replay.head.recording_user);
    stop_network_game_and_continue_locally();
}

static char spectator_join_error[TEXT_BUFFER_LENGTH];

int32_t network_session_join(void)
{
    int32_t plyr_num;
    net_join_rejection = NetJoin_Accepted;
    spectator_join_error[0] = '\0';
    reset_attempting_to_join_cancel();
    display_attempting_to_join_message(-1);
    if (attempting_to_join_cancel_requested())
        return -1;
    snprintf(join_lobby_id, sizeof(join_lobby_id), "%s", net_session[net_session_index_active]->join_address);
    if (LbNetwork_Join(net_session[net_session_index_active], net_player_name, &plyr_num, NULL) == 0)
        return plyr_num;
    if ((net_join_rejection == NetJoin_InGame) && (net_join_role == NetRole_Player)
     && (net_session[net_session_index_active]->phase == NetPhase_Unknown) && !attempting_to_join_cancel_requested()) {
        /* a direct connect to a game in progress: join it the way the session list does */
        net_join_role = NetRole_Spectator;
        net_join_rejection = NetJoin_Accepted;
        spectator_join_error[0] = '\0';
        if (LbNetwork_Join(net_session[net_session_index_active], net_player_name, &plyr_num, NULL) == 0)
            return plyr_num;
    }
    join_lobby_id[0] = '\0';
    if (!attempting_to_join_cancel_requested()) {
        if (frontnet_service_selected(FrontendNetSvc_Online)) {
            net_session_index_active = -1;
            net_session_index_active_id = -1;
            matchmaking_request_list();
        }
        const char *error = net_join_error_text(net_join_rejection);
        if (spectator_join_error[0] != '\0') {
            error = spectator_join_error;
        }
        if (error) {
            create_frontend_error_box(error);
        } else {
            process_network_error(-802);
        }
    }
    return -1;
}

void sync_initial_network_seed(void)
{
   if (net_join_role == NetRole_Spectator) {
       game.action_random_seed = s_startup_sync_packets[SERVER_ID].initial_action_seed;
       game.ai_random_seed = game.action_random_seed * 9377 + 9391;
       game.player_random_seed = game.action_random_seed * 9473 + 9479;
       initial_replay_seed = game.action_random_seed;
       return;
   }
   if (!network_is_active()) {
      return;
   }
   /* the endless world's seed travels with it: the host's pending seed (a random one for a new run), so every
      machine generates the host's world (sent before the level and its world are loaded) */
   struct { uint32_t action_seed; int32_t timestamp_tz; int64_t timestamp; char world_seed[64]; } initial;
   memset(&initial, 0, sizeof(initial));
   initial.action_seed = game.action_random_seed;
   initial.timestamp_tz = game.timestamp_tz;
   initial.timestamp = game.timestamp;
   snprintf(initial.world_seed, sizeof(initial.world_seed), "%s", world_next_seed_text());
   if (!LbNetwork_Resync(&initial, sizeof(initial))) {
      ERRORLOG("Initial sync failed");
      return;
   }
   initial.world_seed[sizeof(initial.world_seed) - 1] = '\0';
   world_set_next_seed(initial.world_seed);
   game.action_random_seed = initial.action_seed;
   game.timestamp = initial.timestamp;
   game.timestamp_tz = initial.timestamp_tz;
   game.ai_random_seed = game.action_random_seed * 9377 + 9391;
   game.player_random_seed = game.action_random_seed * 9473 + 9479;
   initial_replay_seed = game.action_random_seed;
   NETLOG("Initial network seed synced: action_seed=%u", game.action_random_seed);
}
/** Thin-client play: this running single-player endless world becomes a server that others join, as Minecraft's
 *  Open to LAN. The session is created at once (online service: -connect and the session list find it), the game is
 *  a network game from now on, and joiners get what they check against: this game's map and sprite checksums. */
TbBool network_open_running_game(void)
{
    if (network_is_active() || !world_mode_active() || !world_paged) {
        WARNLOG("Open to network: needs a chunked endless world in single player");
        return false;
    }
    /* the network service, without the frontend's session screen (setting it up would change the campaign) */
    memset(net_user_info, 0, sizeof(net_user_info));
    network_lobby_ping = 0;
    if (LbNetwork_Init(NS_ENET_UDP, MAX_NET_USERS, &net_user_info[0], NULL)) {
        WARNLOG("Open to network: the network service didn't start");
        return false;
    }
    /* no stock service: an endless world isn't put on KeeperFX's public list or the LAN; the world list has it */
    net_service_index_selected = FrontendNetSvc_None;
    if (net_player_name[0] == '\0') {
        snprintf(net_player_name, sizeof(net_player_name), "%s", net_config_info.net_player_name);
    }
    uint32_t plyr_num = 0;
    if (LbNetwork_Create(net_player_name, &plyr_num, NULL) != Lb_OK) {
        WARNLOG("Open to network: the session couldn't be created");
        LbNetwork_Stop();
        return false;
    }
    netstate.phase = NetPhase_InGame;
    set_flag(local_system_flags, GSF_NetworkActive);
    fe_network_active = 1;
    game.game_kind = GKind_MultiGame;
    memset(net_user_info, 0, sizeof(net_user_info));
    net_user_info[SERVER_ID].network_user_active = 1;
    snprintf(net_user_info[SERVER_ID].name, sizeof(net_user_info[SERVER_ID].name), "%s", net_player_name);
    set_net_user_player_number(SERVER_ID, my_player_number);
    /* what a joiner checks before it loads the level */
    memset(s_startup_sync_packets, 0, sizeof(s_startup_sync_packets));
    struct StartupSyncPacket *sync = &s_startup_sync_packets[SERVER_ID];
    sync->startup_sync_packet_valid = 1;
    calculate_network_startup_map_checksums(sync->map_checksums);
    memcpy(sync->required_sprite_zip_checksums, required_sprite_zip_checksums, sizeof(sync->required_sprite_zip_checksums));
    build_local_user_preferences(sync->user_prefs);
    sync->initial_action_seed = game.action_random_seed;
    for (NetUserId u = 0; u < MAX_NET_USERS; u++) {
        sync->player_slots[u] = (u == SERVER_ID) ? my_player_number : PLAYER_NONE;
    }
    initialize_packet_history();
    game.input_lag_turns = 0;
    game.skip_initial_input_turns = 0;
    netstate_level_start(); /* the state stream */
    if (!netstate_dedicated())
    {
        /* the host's keeper is the host's, unless the seat is someone else's (a server's save played here) */
        if ((world_seat_token(my_player_number) == 0) || (world_seat_token(my_player_number) == net_local_reconnect_token())) {
            world_set_seat_token(my_player_number, net_local_reconnect_token());
        }
        /* a single-player world had no name on the host's keeper: the others see it by the host's nickname */
        struct PlayerInfo *host = get_my_player();
        snprintf(host->player_name, sizeof(host->player_name), "%s", net_player_name);
    }
    /* the world list: a dedicated server keeps one (the hub) unless -nohub; every open world registers */
    if (netstate_dedicated() && !wnet_hub_off)
    {
        if (wnet_hub_start(wnet_hub_port_wanted))
        {
            char lan[64];
            if (!net_local_ipv4(lan, sizeof(lan)))
                snprintf(lan, sizeof(lan), "<this PC>");
            server_admin_log("world", "World List hub on TCP port %d (ws://%s:%d/hub)", wnet_hub_port_wanted, lan, wnet_hub_port_wanted);
        }
        else
            server_admin_log("warn", "The World List hub could not start on TCP port %d (in use?)", wnet_hub_port_wanted);
    }
    {
        char title[WNET_TITLE_MAX];
        if (wnet_title[0] != '\0')
            snprintf(title, sizeof(title), "%s", wnet_title);
        else if (netstate_dedicated())
            snprintf(title, sizeof(title), "World %s", world_seed_text());
        else
            snprintf(title, sizeof(title), "%s's world", net_player_name);
        wnet_server_begin(title, (int)enet_get_bound_port());
        server_admin_log("world", "\"%s\" is open to players on UDP port %u", title, (unsigned)enet_get_bound_port());
    }
    message_add(MsgType_Blank, 0, "The world is open: others can join");
    SYNCLOG("Server: the world is open to the network (port %u, campaign %s, level %d)", (unsigned)enet_get_bound_port(),
        campaign.fname, (int)get_loaded_level_number());
    return true;
}

/** After a save is loaded in an endless world, outside a network game: a world saved while it was a server plays on
 *  in single player. The other players' keepers stay in the world as departed players (their names keep their seats
 *  for when the world is opened again). */
void network_after_save_loaded(void)
{
    if (network_is_active() || !world_mode_active()) {
        return;
    }
    game.game_kind = GKind_LocalGame;
    /* this machine's own seat, by its reconnect token (a server's save: player 0 is the server's first joiner,
       whoever that was); else the save's own player */
    const uint64_t token = net_local_reconnect_token();
    PlayerNumber mine = PLAYER_NONE;
    const PlayerNumber *seats = NULL;
    const int seat_count = world_seat_players(&seats);
    for (int s = 0; (s < seat_count) && (mine == PLAYER_NONE) && (token != 0); s++) {
        if (world_seat_token(seats[s]) == token) {
            mine = seats[s];
        }
    }
    if ((mine != PLAYER_NONE) && (mine != my_player_number)) {
        SYNCLOG("World: loaded in single player as this machine's own seat, player %d (the save's was %d)", (int)mine,
            (int)my_player_number);
        my_player_number = mine;
        game.local_plyr_idx = mine;
    }
    /* the seat takes input again: one a server left defeated (de-allocated) or as a placeholder would have every
       packet dropped (Quit, Alt+F4, the assistant). A caretaker's computer control is released by the caretaker's
       next turn, which also turns its computer off (audit 2026-11-09) */
    struct PlayerInfo *myplyr = get_my_player();
    myplyr->id_number = my_player_number;
    myplyr->allocflags |= PlaF_Allocated | PlaF_Seat;
    if (!flag_is_set(myplyr->allocflags, PlaF_CompCtrl)) {
        myplyr->allocflags &= ~PlaF_Placeholder;
    }
    remap_user_to_solo(myplyr, get_local_user());
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++) {
        struct PlayerInfo *player = get_player(p);
        if ((p != my_player_number) && player_exists(player) && flag_is_set(player->allocflags, PlaF_Seat)
         && (get_player_primary_user(player) < 0)) {
            player->allocflags |= PlaF_Placeholder;
        }
    }
    setup_count_players();
}

void network_spectator_send_bootstrap(NetUserId user_id)
{
    char *write_pos = begin_net_message(NETMSG_SPECTATOR_BOOTSTRAP);
    int32_t level = get_loaded_level_number();
    memcpy(write_pos, &level, sizeof(level));
    write_pos += sizeof(level);
    /* the campaign file's name without folders (a level started from the command line may carry them; the joiner
       accepts a bare name only) */
    const char *fname = campaign.fname;
    for (const char *c = campaign.fname; *c != '\0'; c++) {
        if ((*c == '/') || (*c == '\\')) {
            fname = c + 1;
        }
    }
    snprintf(write_pos, DISKPATH_SIZE, "%s", fname);
    write_pos += strlen(write_pos) + 1;
    PlayerNumber user_player_number[MAX_NET_USERS];
    for (NetUserId user = 0; user < MAX_NET_USERS; user++) {
        user_player_number[user] = get_net_user_player_number(user);
    }
    memcpy(write_pos, user_player_number, sizeof(user_player_number));
    write_pos += sizeof(user_player_number);
    memcpy(write_pos, s_startup_sync_packets, sizeof(s_startup_sync_packets));
    write_pos += sizeof(s_startup_sync_packets);
    send_message_buffer(user_id, write_pos);
}

TbError process_network_spectator_bootstrap(NetUserId source, const char *buffer, size_t size)
{
    if (source != SERVER_ID || net_join_role != NetRole_Spectator || size < sizeof(int32_t) + 1 + sizeof(spectator_user_player_number) + sizeof(s_startup_sync_packets)) {
        return Lb_FAIL;
    }
    int32_t level;
    memcpy(&level, buffer, sizeof(level));
    buffer += sizeof(level);
    size -= sizeof(level);
    size_t name_length = strnlen(buffer, min(size, (size_t)DISKPATH_SIZE));
    if (name_length == 0 || name_length >= DISKPATH_SIZE || name_length + 1 + sizeof(spectator_user_player_number) + sizeof(s_startup_sync_packets) != size || strchr(buffer, '/') || strchr(buffer, '\\') || strstr(buffer, "..")) {
        return Lb_FAIL;
    }
    char campaign_file[DISKPATH_SIZE];
    uint8_t pack = prepare_campaign_file_name(buffer, campaign_file, sizeof(campaign_file));
    if (pack == CampgnT_Default && is_campaign_in_list(campaign_file, &mp_mappacks_list)) {
        pack = CampgnT_MultiplayerMappack;
    }
    if (!change_campaign(pack, campaign_file) || strcasecmp(campaign.fname, campaign_file) != 0) {
        snprintf(spectator_join_error, sizeof(spectator_join_error), get_string(GUIStr_NetMissingMapPack), campaign_file);
        return Lb_FAIL;
    }
    if (level <= 0 || get_level_info(level) == NULL) {
        snprintf(spectator_join_error, sizeof(spectator_join_error), get_string(GUIStr_NetMapPackMissingLevel), campaign_file, level);
        return Lb_FAIL;
    }
    buffer += name_length + 1;
    memcpy(spectator_user_player_number, buffer, sizeof(spectator_user_player_number));
    buffer += sizeof(spectator_user_player_number);
    memcpy(s_startup_sync_packets, buffer, sizeof(s_startup_sync_packets));
    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        /* a joiner hasn't loaded the level yet: an endless world's players 10-15 aren't in use here before it */
        if (spectator_user_player_number[id] < PLAYER_NONE || spectator_user_player_number[id] >= PLAYERS_MAX) {
            return Lb_FAIL;
        }
    }
    set_selected_level_number(level);
    netstate.phase = NetPhase_InGame;
    return Lb_OK;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

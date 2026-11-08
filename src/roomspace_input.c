/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file roomspace_input.c
 *     Turns local mouse input into dig tag packet actions, and shows those tags and the cursor box before the game state catches up.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "observer.h"
#include "game_legacy.h"
#include "config_sounds.h"
#include "map_data.h"
#include "cursor_tag.h"
#include "engine_render.h"
#include "frontmenu_ingame_evnt.h"
#include "player_data.h"
#include "power_hand.h"
#include "slab_data.h"
#include "tasks_list.h"
#include "packets.h"
#include "net_exchange_gameplay.h"
#include "roomspace.h"
#include "roomspace_input.h"

#include "keeperfx.hpp"
#include "world_pages.h"
#include "net_state.h"
#include "spdigger_stack.h"
#include "frontmenu_ingame_map.h"
#include "dungeon_data.h"
#include "local_camera.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static struct DigTagPrediction dig_tags;

static struct {
    MapSlabCoord drag_start_slb_x;
    MapSlabCoord drag_start_slb_y;
    MapSlabCoord previous_slb_x;
    MapSlabCoord previous_slb_y;
    TbBool cursor_button_down;
    TbBool cursor_locked;
    unsigned char cursor_state;
    TbBool untag_mode;
} dig_cursor;

static GameTurn cursor_release_turn;
static MapSlabCoord room_drag_start_slb_x;
static MapSlabCoord room_drag_start_slb_y;

/* a thin client's own dig marks: its dungeon's dig tasks on the server, by slab (the map's mark bits are the
   server's own player's) */
static unsigned char thin_marks[MAX_TILES_X * MAX_TILES_Y];
/* the slab each entry is for: the index repeats every 170 slabs in an endless world, so a dig task far away (an
   offline keeper's) showed as a mark nearby that no untag could take away (2026-11-08) */
static SlabCodedCoords thin_mark_owner[MAX_TILES_X * MAX_TILES_Y];
static SlabCodedCoords thin_mark_list[MAPTASKS_COUNT];
static SlabCodedCoords thin_mark_slabs[MAPTASKS_COUNT];
static int thin_mark_count = 0;

static void thin_marks_changed(SlabCodedCoords slb_num)
{
    panel_map_update(slab_subtile(slb_num_decode_x(slb_num), 0), slab_subtile(slb_num_decode_y(slb_num), 0), STL_PER_SLB, STL_PER_SLB);
}

/** Each thin client turn: the marks from the task list as the server has it now (the minimap redraws what changed). */
static void rebuild_thin_marks(void)
{
    static SlabCodedCoords old_slabs[MAPTASKS_COUNT];
    const int old_count = thin_mark_count;
    memcpy(old_slabs, thin_mark_slabs, sizeof(SlabCodedCoords) * old_count);
    for (int i = 0; i < thin_mark_count; i++)
        thin_marks[thin_mark_list[i]] = 0;
    thin_mark_count = 0;
    struct Dungeon *dungeon = get_players_num_dungeon(my_player_number);
    if (!dungeon_invalid(dungeon))
    {
        const long imax = min((long)dungeon->highest_task_number, (long)MAPTASKS_COUNT);
        for (long i = 0; i < imax; i++)
        {
            const struct MapTask *mtask = &dungeon->task_list[i];
            if ((mtask->kind == SDDigTask_None) || (mtask->coords == 0))
                continue;
            const MapSlabCoord slb_x = subtile_slab(stl_num_decode_x(mtask->coords));
            const MapSlabCoord slb_y = subtile_slab(stl_num_decode_y(mtask->coords));
            const SlabCodedCoords slb_num = get_slab_number(slb_x, slb_y);
            const SlabCodedCoords idx = dig_pred_index(slb_num);
            if (thin_marks[idx])
            {
                /* two tasks on one entry: the one nearer the view is shown */
                const struct Camera *cam = get_local_active_camera();
                if (cam != NULL)
                {
                    const MapSlabCoord vx = coord_slab(cam->mappos.x.val), vy = coord_slab(cam->mappos.y.val);
                    const SlabCodedCoords was = thin_mark_owner[idx];
                    const long d_new = labs((long)slb_x - vx) + labs((long)slb_y - vy);
                    const long d_was = labs((long)slb_num_decode_x(was) - vx) + labs((long)slb_num_decode_y(was) - vy);
                    if (d_new < d_was)
                    {
                        thin_mark_owner[idx] = slb_num;
                        for (int k = 0; k < thin_mark_count; k++)
                            if (thin_mark_list[k] == idx)
                                thin_mark_slabs[k] = slb_num;
                    }
                }
                continue;
            }
            if (thin_mark_count >= MAPTASKS_COUNT)
                continue;
            thin_marks[idx] = 1;
            thin_mark_owner[idx] = slb_num;
            thin_mark_list[thin_mark_count] = idx;
            thin_mark_slabs[thin_mark_count] = slb_num;
            thin_mark_count++;
        }
    }
    /* the minimap: the slabs that gained or lost a mark */
    for (int i = 0; i < old_count; i++)
    {
        const SlabCodedCoords idx = dig_pred_index(old_slabs[i]);
        if (!thin_marks[idx] || (thin_mark_owner[idx] != old_slabs[i]))
            thin_marks_changed(old_slabs[i]);
    }
    for (int i = 0; i < thin_mark_count; i++)
    {
        TbBool had = false;
        for (int k = 0; (k < old_count) && !had; k++)
            had = (old_slabs[k] == thin_mark_slabs[i]);
        if (!had)
            thin_marks_changed(thin_mark_slabs[i]);
    }
}

/** Tests: the dig marks predicted here and not yet settled. */
int dig_prediction_count(void)
{
    return dig_tags.slab_count;
}

void reset_dig_prediction(void)
{
    memset(&dig_tags, 0, sizeof(dig_tags));
    memset(&dig_cursor, 0, sizeof(dig_cursor));
    cursor_release_turn = 0;
    set_local_user_state_prediction(NULL);
}

static void reconcile_dig_predictions(struct DigTagPrediction *tags, GameTurn processed_turn)
{
    tags->task_count = get_players_dungeon(get_my_player())->task_count;
    for (int i = 0; i < tags->slab_count;) {
        SlabCodedCoords slb_num = tags->slabs[i];
        if ((GameTurnDelta)(tags->slab_turns[dig_pred_index(slb_num)] - processed_turn) <= 0) {
            tags->slab_tag_modes[dig_pred_index(slb_num)] = 0;
            tags->slabs[i] = tags->slabs[--tags->slab_count];
            continue;
        }
        TbBool tagged = find_from_task_list_by_slab(my_player_number, slb_num_decode_x(slb_num), slb_num_decode_y(slb_num)) != -1;
        tags->task_count += (tags->slab_tag_modes[dig_pred_index(slb_num)] == DigTagMode_Tag) - tagged;
        i++;
    }
}

/** A thin client: a predicted mark lasts until the server's state has it (or 3 s, if the server turned it down). */
#define THIN_DIG_PREDICTION_TURNS 60
static void reconcile_thin_dig_predictions(struct DigTagPrediction *tags)
{
    tags->task_count = get_players_dungeon(get_my_player())->task_count;
    for (int i = 0; i < tags->slab_count;) {
        SlabCodedCoords slb_num = tags->slabs[i];
        const unsigned char mode = tags->slab_tag_modes[dig_pred_index(slb_num)];
        TbBool tagged = find_from_task_list_by_slab(my_player_number, slb_num_decode_x(slb_num), slb_num_decode_y(slb_num)) != -1;
        TbBool settled = (mode == DigTagMode_Tag) ? tagged : !tagged;
        if (settled || ((GameTurnDelta)(get_gameturn() - tags->slab_turns[dig_pred_index(slb_num)]) > THIN_DIG_PREDICTION_TURNS)) {
            tags->slab_tag_modes[dig_pred_index(slb_num)] = 0;
            tags->slabs[i] = tags->slabs[--tags->slab_count];
            continue;
        }
        tags->task_count += (mode == DigTagMode_Tag) - tagged;
        i++;
    }
}

static TbBool dig_input_is_blocked(const struct Packet *pckt)
{
    if (pckt == NULL) {
        return true;
    }
    if ((pckt->control_flags & (PCtr_Gui | PCtr_MapCoordsValid)) != PCtr_MapCoordsValid) {
        return true;
    }
    switch (pckt->action)
    {
    case PckA_UsePwrHandPick:
    case PckA_UsePwrOnThing:
    case PckA_BookmarkLoad: // Dragging minimap
        return true;
    }
    if (battle_creature_over > 0) {
        return true;
    }
    return false;
}

static TbBool dig_input_is_enabled(void)
{
    return !replay.load_enable && (get_my_player() != &local_observer_player);
}

TbBool local_cursor_is_locked(void)
{
    if (dig_cursor.cursor_button_down && dig_cursor.cursor_locked) {
        return true;
    }
    if (get_gameturn() - cursor_release_turn <= game.input_lag_turns) {
        return false;
    }
    return get_local_user_state()->one_click_lock_cursor;
}

static TbBool update_dig_highlight(const struct Packet *pckt, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    struct UserState *ustate = get_local_user_state();
    struct PlayerInfo *player = get_my_player();
    unsigned char cursor_context = (unsigned char)((pckt->additional_packet_values & PCAdV_ContextMask) >> 1);
    TbBool cursor_is_locked = dig_cursor.cursor_button_down && dig_cursor.cursor_locked && ((pckt->control_flags & PCtr_LBtnClick) == 0) && ((pckt->control_flags & (PCtr_LBtnHeld | PCtr_LBtnRelease)) != 0);
    if ((cursor_context != CSt_PickAxe) && !cursor_is_locked) {
        if ((cursor_context != CSt_PowerHand) || power_hand_is_empty(player) || !can_dig_here(stl_x, stl_y, my_player_number, true)) {
            return false;
        }
    }
    if (!cursor_is_locked && (((pckt->control_flags & PCtr_LBtnAnyAction) == 0) || ((pckt->control_flags & PCtr_LBtnClick) != 0))) {
        ustate->swap_to_untag_mode = 0;
    }
    ustate->one_click_lock_cursor = cursor_is_locked;
    ustate->render_roomspace.drag_mode = cursor_is_locked;
    if (cursor_is_locked) {
        ustate->render_roomspace.drag_start_x = dig_cursor.drag_start_slb_x;
        ustate->render_roomspace.drag_start_y = dig_cursor.drag_start_slb_y;
        ustate->render_roomspace.untag_mode = dig_cursor.untag_mode;
    }
    get_dungeon_highlight_user_roomspace(&ustate->render_roomspace, player, get_local_user(), pckt, stl_x, stl_y, &dig_tags);
    tag_cursor_blocks_dig(player, get_local_user(), pckt, &ustate->render_roomspace, stl_x, stl_y, true);
    return true;
}

static void apply_packet_prediction(NetUserId user, const struct Packet *pckt)
{
    struct UserState *ustate = get_user_state(user);
    apply_roomspace_packet_action(user, pckt);
    apply_user_pref_packet_action(ustate, pckt);
    apply_first_person_packet_action(ustate, pckt);
    apply_creature_input_packet_values(ustate, pckt);
    ustate->primary_cursor_state = (pckt->additional_packet_values & PCAdV_ContextMask) >> 1;
    if (pckt->action == PckA_SetPlyrState) {
        ustate->work_state = pckt->actn_par1;
        set_user_work_state_kind(ustate, pckt->actn_par2);
    }
    if (pckt->action == PckA_PlyrQueryCreature) {
        ustate->influenced_thing_idx = pckt->actn_par1;
    }
}

/**
 * Runs the singleplayer roomspace cursor code for a local packet, on the user state as it will be once the pending local packets are processed.
 * pending_turns counts the pending packets before this one; it is negative when this packet has been processed already.
 */
static TbBool get_predicted_roomspace(const struct Packet *pckt, GameTurnDelta pending_turns, TbBool include_drop_preview, struct UserState *predicted_ustate)
{
    NetUserId user = get_local_user();
    struct UserState *ustate = get_local_user_state();
    struct Packet *direct_packet = get_local_packet();
    struct UserState saved_ustate = *ustate;
    struct Packet saved_packet = *direct_packet;
    for (GameTurnDelta i = pending_turns; i > 0; i--) {
        const struct Packet *pending_pckt = get_history_packet(user, get_gameturn() - i);
        if (pending_pckt != NULL) {
            apply_packet_prediction(user, pending_pckt);
        }
    }
    TbBool has_roomspace = !dig_input_is_blocked(pckt);
    if ((pckt != NULL) && (pending_turns >= 0)) {
        apply_packet_prediction(user, pckt);
    }
    if (has_roomspace) {
        *direct_packet = *pckt;
        MapSubtlCoord stl_x = coord_subtile(pckt->pos_x);
        MapSubtlCoord stl_y = coord_subtile(pckt->pos_y);
        ustate->render_roomspace.drag_start_x = room_drag_start_slb_x;
        ustate->render_roomspace.drag_start_y = room_drag_start_slb_y;
        if (ustate->work_state == PSt_BuildRoom) {
            update_dungeon_build_roomspace_preview(user, stl_x, stl_y);
        } else if (ustate->work_state == PSt_Sell) {
            update_dungeon_sell_roomspace_preview(user, stl_x, stl_y);
        } else if (ustate->work_state != PSt_CtrlDungeon) {
            tag_cursor_blocks_for_work_state(user, stl_x, stl_y);
        } else {
            has_roomspace = update_dig_highlight(pckt, stl_x, stl_y);
            TbBool holding_thing = !power_hand_is_empty(get_my_player());
            TbBool chosen_subtile_is_high = has_roomspace && holding_thing;
            set_flag_value(ustate->additional_flags, UsrAF_ChosenSubTileIsHigh, chosen_subtile_is_high);
            if (chosen_subtile_is_high) {
                ustate->thing_under_hand = 0;
            }
            if (!has_roomspace && include_drop_preview && holding_thing) {
                tag_cursor_blocks_thing_in_hand(user, stl_x, stl_y);
                has_roomspace = true;
            }
        }
    }
    *predicted_ustate = *ustate;
    *ustate = saved_ustate;
    *direct_packet = saved_packet;
    return has_roomspace;
}

static void apply_dig_tag_selection(struct Packet *pckt, struct UserState *predicted_ustate)
{
    struct RoomSpace *roomspace = &predicted_ustate->render_roomspace;
    dig_cursor.untag_mode = roomspace->untag_mode;
    TbBool apply_selection = (pckt->control_flags & PCtr_LBtnHeld) != 0;
    if (predicted_ustate->roomspace_highlight_mode == drag_placement_mode) {
        apply_selection = (pckt->control_flags & PCtr_LBtnRelease) != 0;
    }
    if (!apply_selection) {
        return;
    }
    dig_tags.turn = netstate_is_client() ? get_gameturn() : pckt->turn; /* a thin client's packet turn is an input number */
    int changed_slab_count = apply_roomspace_dig_tag_selection(my_player_number, roomspace, dig_cursor.previous_slb_x, dig_cursor.previous_slb_y, predicted_ustate->roomspace_highlight_mode, &dig_tags);
    if (changed_slab_count > 0) {
        /* 8 bits each; normal maps are narrower than 256 slabs, the chunked world sends the low bits (and
           packets_input.c takes them back to the slab nearest the cursor) */
        MapSlabCoord prev_x = dig_cursor.previous_slb_x, prev_y = dig_cursor.previous_slb_y;
        if (world_paged)
        {
            prev_x &= 0xFF;
            prev_y &= 0xFF;
        }
        uint16_t previous_slb = (uint16_t)prev_x | ((uint16_t)prev_y << 8);
        set_packet_action(pckt, PckA_ApplyRoomspaceDigTag, predicted_ustate->roomspace_highlight_mode, predicted_ustate->roomspace_width, previous_slb, roomspace->untag_mode);
        play_non_3d_sample(snd_tile_dig);
    }
}

void update_dig_tag_input(void)
{
    struct Packet *pckt = get_local_packet();
    if (!dig_input_is_enabled()) {
        reset_dig_prediction();
        return;
    }
    MapSlabCoord slb_x = coord_slab(pckt->pos_x);
    MapSlabCoord slb_y = coord_slab(pckt->pos_y);
    if (((pckt->control_flags & (PCtr_LBtnRelease | PCtr_RBtnRelease)) != 0) && ((pckt->control_flags & PCtr_HeldAnyButton) == 0)) {
        cursor_release_turn = get_gameturn();
    }
    if ((pckt->control_flags & (PCtr_Gui | PCtr_MapCoordsValid | PCtr_HeldAnyButton | PCtr_LBtnRelease)) == PCtr_MapCoordsValid) {
        room_drag_start_slb_x = slb_x;
        room_drag_start_slb_y = slb_y;
    }
    if (((pckt->control_flags & PCtr_RBtnClick) != 0) && ((pckt->control_flags & PCtr_LBtnHeld) != 0)) {
        memset(&dig_cursor, 0, sizeof(dig_cursor));
        return;
    }
    if ((pckt->control_flags & PCtr_LBtnAnyAction) == 0) {
        dig_cursor.cursor_button_down = false;
        return;
    }
    TbBool ignore_packet = ((pckt->action != PckA_None) && (pckt->action != PckA_SetRoomspaceHighlight)) || (get_local_user_state_prediction()->work_state != PSt_CtrlDungeon) || dig_input_is_blocked(pckt);
    unsigned char cursor_context = (unsigned char)((pckt->additional_packet_values & PCAdV_ContextMask) >> 1);
    TbBool click = (pckt->control_flags & PCtr_LBtnClick) != 0;
    if (click) {
        dig_cursor.cursor_button_down = !ignore_packet;
        dig_cursor.cursor_locked = false;
        dig_cursor.cursor_state = cursor_context;
    }
    const struct Packet *previous_pckt = netstate_is_client() ? netstate_client_last_input() : get_history_packet(get_local_user(), pckt->turn - 1);
    if (click || dig_input_is_blocked(previous_pckt)) {
        dig_cursor.previous_slb_x = slb_x;
        dig_cursor.previous_slb_y = slb_y;
    }
    if (ignore_packet || !dig_cursor.cursor_button_down) {
        return;
    }
    struct Packet selection_pckt = *pckt;
    if (!dig_cursor.cursor_locked) {
        selection_pckt.control_flags |= PCtr_LBtnClick;
    }
    struct UserState predicted_ustate;
    TbBool has_roomspace = get_predicted_roomspace(&selection_pckt, game.input_lag_turns, false, &predicted_ustate);
    if (!dig_cursor.cursor_locked) {
        if (click && !has_roomspace && (cursor_context == CSt_PowerHand) && (local_state.local_thing_under_hand != 0)) {
            dig_cursor.cursor_button_down = false;
            return;
        }
        dig_cursor.drag_start_slb_x = slb_x;
        dig_cursor.drag_start_slb_y = slb_y;
        dig_cursor.cursor_locked = has_roomspace && (cursor_context == dig_cursor.cursor_state);
    }
    if (dig_cursor.cursor_locked) {
        apply_dig_tag_selection(pckt, &predicted_ustate);
    }
    dig_cursor.previous_slb_x = slb_x;
    dig_cursor.previous_slb_y = slb_y;
}

/** The map flags to show a block with: on a thin client its marks are this player's own (see above); elsewhere the
 *  map's flags as they are. For the views that read the map's flags directly (front view, minimap, parchment). */
unsigned char dig_marks_display_flags(MapSubtlCoord stl_x, MapSubtlCoord stl_y, unsigned char map_flags)
{
    if (!netstate_is_client())
        return map_flags;
    return get_dig_prediction_render_flags(stl_x, stl_y, map_flags);
}

unsigned char get_dig_prediction_render_flags(MapSubtlCoord stl_x, MapSubtlCoord stl_y, unsigned char base_map_flags)
{
    MapSlabCoord slb_x = subtile_slab(stl_x);
    MapSlabCoord slb_y = subtile_slab(stl_y);
    if (slab_coords_invalid(slb_x, slb_y)) {
        return base_map_flags;
    }
    unsigned char predicted_dig_tag_mode = 0;
    if (netstate_is_client()) {
        /* the map's mark bits are the server's own player's: this player's marks are its dig tasks */
        base_map_flags &= ~(SlbAtFlg_TaggedValuable | SlbAtFlg_Unexplored);
        const SlabCodedCoords slb_num = get_slab_number(slb_x, slb_y);
        const SlabCodedCoords idx = dig_pred_index(slb_num);
        if (thin_marks[idx] && (thin_mark_owner[idx] == slb_num))
            predicted_dig_tag_mode = DigTagMode_Tag;
    }
    if (dig_input_is_enabled() && (dig_tags.slab_tag_modes[dig_pred_index(get_slab_number(slb_x, slb_y))] != 0)) {
        predicted_dig_tag_mode = dig_tags.slab_tag_modes[dig_pred_index(get_slab_number(slb_x, slb_y))];
    }
    if (predicted_dig_tag_mode == DigTagMode_Untag) {
        return base_map_flags & ~(SlbAtFlg_TaggedValuable | SlbAtFlg_Unexplored);
    }
    if (predicted_dig_tag_mode != DigTagMode_Tag) {
        return base_map_flags;
    }
    if ((base_map_flags & SlbAtFlg_Valuable) == 0) {
        return base_map_flags | SlbAtFlg_Unexplored;
    }
    if (!subtile_revealed(stl_x, stl_y, my_player_number)) {
        base_map_flags |= SlbAtFlg_Unexplored;
    }
    return base_map_flags | SlbAtFlg_TaggedValuable;
}

void update_dig_prediction_cursor_preview(void)
{
    set_local_user_state_prediction(NULL);
    if (netstate_is_client()) {
        rebuild_thin_marks();
    }
    if (!dig_input_is_enabled()) {
        return;
    }
    const struct Packet *pckt;
    if (netstate_is_client()) {
        pckt = netstate_client_last_input();
        reconcile_thin_dig_predictions(&dig_tags);
    } else {
        pckt = get_history_packet(get_local_user(), get_gameturn());
        reconcile_dig_predictions(&dig_tags, get_local_packet()->turn);
    }
    if (get_local_user_state()->work_state != PSt_OrderCreatr) {
        map_volume_box.visible = 0;
    }
    struct UserState predicted_ustate;
    get_predicted_roomspace(pckt, game.input_lag_turns - 1, true, &predicted_ustate);
    set_local_user_state_prediction(&predicted_ustate);
    if ((pckt != NULL) && ((pckt->control_flags & PCtr_LBtnRelease) != 0)) {
        dig_cursor.cursor_button_down = false;
    }
}
/******************************************************************************/
#ifdef __cplusplus
}
#endif

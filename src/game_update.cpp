/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file game_update.cpp
 *     Module which contains functions for updating the game state.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "platform.h"
#include "keeperfx.hpp"

#include "bflib_video.h"
#include "bflib_sound.h"
#include "bflib_planar.h"

#include "api.h"
#include "version.h"
#include "gui_msgs.h"
#include "packets.h"
#include "config_terrain.h"
#include "config_creature.h"
#include "lua_triggers.h"
#include "lvl_script.h"
#include "thing_list.h"
#include "thing_physics.h"
#include "player_utils.h"
#include "player_computer.h"
#include "engine_camera.h"
#include "local_camera.h"
#include "local_possession.h"
#include "engine_textures.h"
#include "thing_stats.h"
#include "thing_creature.h"
#include "thing_objects.h"
#include "thing_effects.h"
#include "thing_doors.h"
#include "slab_data.h"
#include "room_entrance.h"
#include "room_util.h"
#include "map_columns.h"
#include "map_events.h"
#include "map_blocks.h"
#include "creature_control.h"
#include "creature_states.h"
#include "light_data.h"
#include "magic_powers.h"
#include "power_process.h"
#include "power_hand.h"
#include "game_merge.h"
#include "net_game.h"
#include "gui_soundmsgs.h"
#include "sounds.h"
#include "vidfade.h"
#include "game_legacy.h"
#include "game_loop.h"
#include "room_library.h"
#include "room_workshop.h"
#include <cstdint>

#include "kfx/profiling/KfxProfiling.h"
#include "world_stream.h"
#include "world_pages.h"
#include "net_state.h"
#include "roomspace_input.h"
#include "net_spectator.h"
#include "creature_graphics.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

static void check_players_lost(void)
{
  long i;
  SYNCDBG(8,"Starting");
  struct PlayerInfo* player;
  struct Dungeon* dungeon;
  for (i=0; i < PLAYERS_COUNT; i++)
  {
      player = get_player(i);
      dungeon = get_players_dungeon(player);
      if (is_active_keeper(player))
      {
          // A player who plays without a heart is only defeated by the level script
          if (!player_needs_heart(i))
              continue;
          struct Thing *heartng;
          heartng = get_player_soul_container(i);
          if (heartng->owner != i)
          {
              init_player_start(player, true);
              if (dungeon->dnheart_idx == 0)
              {
                  initialise_devastate_dungeon_from_heart(player->id_number);
              }
          }
          if ((!thing_exists(heartng) || ((heartng->active_state == ObSt_BeingDestroyed) && !(dungeon->backup_heart_idx > 0))) && (player->victory_state == VicS_Undecided))
          {
            event_kill_all_players_events(i);
            set_player_as_lost_level(player);
            //this would easily prevent computer player activities on dead player, but it also makes dead player unable to use
            //floating spirit, so defeated keepers must stay active keepers
            if (is_my_player_number(i)) {
                RendererPaletteSet(engine_palette);
            }
          }
      }
  }
}

static void blast_slab(MapSlabCoord slb_x, MapSlabCoord slb_y, PlayerNumber plyr_idx)
{
    struct SlabMap *slb;
    slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb)) {
        return;
    }
    if (slabmap_owner(slb) != plyr_idx) {
        return;
    }
    struct Thing *doortng;
    doortng = get_door_for_position(slab_subtile_center(slb_x), slab_subtile_center(slb_y));
    if (!thing_is_invalid(doortng)) {
        destroy_door(doortng);
    }
    struct SlabConfigStats *slabst;
    slabst = get_slab_stats(slb);
    if (slabst->category == SlbAtCtg_FortifiedGround)
    {
      place_slab_type_on_map(SlbT_PATH, slab_subtile_center(slb_x), slab_subtile_center(slb_y), game.neutral_player_num, 1);
      decrease_dungeon_area(plyr_idx, 1);
      do_unprettying(game.neutral_player_num, slb_x, slb_y);
      do_slab_efficiency_alteration(slb_x, slb_y);
      struct Coord3d pos;
      pos.x.val = subtile_coord_center(slab_subtile_center(slb_x));
      pos.y.val = subtile_coord_center(slab_subtile_center(slb_y));
      pos.z.val = get_floor_height_at(&pos);
      create_effect_element(&pos, TngEffElm_RedFlameBig, plyr_idx);
    }
}

static void process_dungeon_devastation_effects(void)
{
    SYNCDBG(8,"Starting");
    int plyr_idx;
    for (plyr_idx=0; plyr_idx < PLAYERS_COUNT; plyr_idx++)
    {
        struct Dungeon *dungeon;
        dungeon = get_players_num_dungeon(plyr_idx);
        if (dungeon->devastation_turn == 0)
            continue;
        if ((get_gameturn() & 1) != 0)
            continue;
        dungeon->devastation_turn++;
        if (dungeon->devastation_turn >= max(game.map_tiles_x,game.map_tiles_y))
            continue;
        if (world_paged)
        {
            // Paged mode: the ring stops once it's past every side of the loaded chunks' bounding box
            MapSlabCoord bx1, by1, bx2, by2;
            map_loop_slab_bounds(&bx1, &by1, &bx2, &by2);
            const MapSlabCoord cslb_x = subtile_slab(dungeon->devastation_centr_x);
            const MapSlabCoord cslb_y = subtile_slab(dungeon->devastation_centr_y);
            const MapSlabCoord reach = max(max(cslb_x - bx1, bx2 - cslb_x), max(cslb_y - by1, by2 - cslb_y));
            if ((bx1 > bx2) || (by1 > by2) || (reach < 0) || (dungeon->devastation_turn > (GameTurn)reach))
                continue;
        }
        MapSlabCoord slb_x;
        MapSlabCoord slb_y;
        int i;
        int range;
        slb_x = subtile_slab(dungeon->devastation_centr_x) - dungeon->devastation_turn;
        slb_y = subtile_slab(dungeon->devastation_centr_y) - dungeon->devastation_turn;
        range = 2*dungeon->devastation_turn;
        for (i = 0; i <= range; i++)
        {
            blast_slab(slb_x + i, slb_y,         dungeon->owner);
            blast_slab(slb_x + i, slb_y + range, dungeon->owner);
        }
        for (i = 0; i <= range; i++)
        {
            blast_slab(slb_x,         slb_y + i, dungeon->owner);
            blast_slab(slb_x + range, slb_y + i, dungeon->owner);
        }
    }
}

/**
 * Increments paydays_owed for all players creatures
 * returns amount of creatures needing payday for player
 */
static int set_players_creatures_to_get_paid(PlayerNumber plyr_idx)
{
    unsigned long k;
    long i;
    int count = 0;
    const struct StructureList *slist;
    slist = get_list_for_thing_class(TCls_Creature);
    i = slist->index;
    k = 0;
    while (i != 0)
    {
        struct Thing *thing;
        thing = thing_get(i);
        if (thing_is_invalid(thing))
        {
            ERRORLOG("Jump to invalid thing detected");
            break;
        }
        i = thing->next_of_class;
        // Per-thing code
        if (thing->owner == plyr_idx)
        {
            struct CreatureModelConfig *crconf;
            crconf = creature_stats_get_from_thing(thing);
            if (crconf->pay != 0)
            {
                struct CreatureControl *cctrl;
                cctrl = creature_control_get_from_thing(thing);
                if (cctrl->paydays_advanced > 0)
                {
                    cctrl->paydays_advanced--;
                } else
                {
                    if (!creature_is_kept_in_custody_by_enemy(thing))
                    {
                        cctrl->paydays_owed++;
                        count++;
                    }
                    else
                    {
                        cctrl->paydays_advanced--;
                    }
                }
            }
        }
        // Per-thing code ends
        k++;
        if (k > THINGS_COUNT)
        {
            ERRORLOG("Infinite loop detected when sweeping things list");
            break;
        }
    }
    return count;
}

static void process_payday(void)
{
    PlayerNumber plyr_idx;
    for (plyr_idx=0; plyr_idx < PLAYERS_COUNT; plyr_idx++)
    {
        game.pay_day_progress[plyr_idx] = game.pay_day_progress[plyr_idx] + (game.conf.rules[plyr_idx].gameplay.pay_day_speed / 100);
        if (player_is_roaming(plyr_idx) || (plyr_idx == game.neutral_player_num)) {
            continue;
        }
        struct PlayerInfo *player;
        player = get_player(plyr_idx);
        if (is_active_keeper(player))
        {
            compute_and_update_player_payday_total(plyr_idx);
            compute_and_update_player_backpay_total(plyr_idx);
        }
    }
    int player_paid_creatures_count;
    for (plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++)
    {
        if (game.conf.rules[plyr_idx].gameplay.pay_day_gap <= game.pay_day_progress[plyr_idx])
        {
            output_player_message(plyr_idx, SMsg_Payday, 0);
            game.pay_day_progress[plyr_idx] = 0;
            player_paid_creatures_count = set_players_creatures_to_get_paid(plyr_idx);
            if (player_paid_creatures_count > 0)
            {
                struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
                event_create_event_or_update_nearby_existing_event(0, 0, EvKind_CreaturePayday, plyr_idx, dungeon->creatures_total_pay);
            }
        }
    }
}

static void process_dungeons(void)
{
  SYNCDBG(7,"Starting");
  check_players_lost();
  resolve_placeholders();
  hand_waiting_placeholders_to_ai();
  process_dungeon_power_magic();
  process_dungeon_devastation_effects();
  process_entrance_generation();
  process_payday();
  process_things_in_dungeon_hand();
  SYNCDBG(9,"Finished");
}

static void update_near_creatures_for_footsteps(int32_t *near_creatures, const struct Coord3d *srcpos)
{
    long near_distance[3];
    // Don't allow creatures which are far by over 20 subtiles
    near_distance[0] = subtile_coord(20,0);
    near_distance[1] = subtile_coord(20,0);
    near_distance[2] = subtile_coord(20,0);
    near_creatures[0] = 0;
    near_creatures[1] = 0;
    near_creatures[2] = 0;
    // Find the closest thing for footsteps
    struct Thing *thing;
    unsigned long k;
    long i;
    const struct StructureList *slist;
    slist = get_list_for_thing_class(TCls_Creature);
    i = slist->index;
    k = 0;
    while (i != 0)
    {
        thing = thing_get(i);
        if (thing_is_invalid(thing))
        {
            ERRORLOG("Jump to invalid thing detected");
            break;
        }
        i = thing->next_of_class;
        // Per-thing code
        thing->state_flags &= ~TF1_DoFootsteps;
        if ( (!thing_is_picked_up(thing)) && (!thing_is_dragged_or_pulled(thing)) )
        {
            struct CreatureSound *crsound;
            crsound = get_creature_sound(thing, CrSnd_Foot);
            if (crsound->index != 0)
            {
                struct CreatureControl *cctrl;
                cctrl = creature_control_get_from_thing(thing);
                long ndist;
                ndist = get_chessboard_distance(srcpos, &thing->mappos);
                if (ndist < near_distance[0])
                {
                    if (((cctrl->distance_to_destination != 0) && thing_touching_floor(thing)) || ((thing->movement_flags & TMvF_Flying) != 0))
                    {
                        // Insert the new item to our list
                        int n;
                        for (n = 2; n>0; n--)
                        {
                            near_creatures[n] = near_creatures[n-1];
                            near_distance[n] = near_distance[n-1];
                        }
                        near_distance[0] = ndist;
                        near_creatures[0] = thing->index;
                    }
                }
            }
        }
        // Per-thing code ends
        k++;
        if (k > THINGS_COUNT)
        {
            ERRORLOG("Infinite loop detected when sweeping things list");
            break;
        }
    }
}

static long stop_playing_flight_sample_in_all_flying_creatures(void)
{
    struct Thing *thing;
    unsigned long k;
    long i;
    long naffected;
    naffected = 0;
    const struct StructureList *slist;
    slist = get_list_for_thing_class(TCls_Creature);
    i = slist->index;
    k = 0;
    while (i != 0)
    {
        thing = thing_get(i);
        if (thing_is_invalid(thing))
        {
          ERRORLOG("Jump to invalid thing detected");
          break;
        }
        i = thing->next_of_class;
        // Per-thing code
        if ((get_creature_model_flags(thing) & CMF_IsDiptera) && ((thing->state_flags & TF1_DoFootsteps) == 0))
        {
            if ( S3DEmitterIsPlayingSample(thing->snd_emitter_id, 25) ) {
                S3DDeleteSampleFromEmitter(thing->snd_emitter_id, 25);
            }
        }
        // Per-thing code ends
        k++;
        if (k > THINGS_COUNT)
        {
          ERRORLOG("Infinite loop detected when sweeping things list");
          break;
        }
    }
    return naffected;
}

static void update_footsteps_nearest_camera(struct Camera *cam)
{
    static long timeslice = 0;
    static int32_t near_creatures[3];
    struct Coord3d srcpos;
    SYNCDBG(6,"Starting");
    if (cam == NULL)
        return;
    srcpos.x.val = cam->mappos.x.val;
    srcpos.y.val = cam->mappos.y.val;
    srcpos.z.val = cam->mappos.z.val;
    if (timeslice == 0) {
        update_near_creatures_for_footsteps(near_creatures, &srcpos);
    }
    long i;
    for (i=0; i < 3; i++)
    {
        struct Thing *thing;
        if (near_creatures[i] == 0)
            break;
        thing = thing_get(near_creatures[i]);
        if (thing_is_creature(thing)) {
            thing->state_flags |= TF1_DoFootsteps;
            play_thing_walking(thing);
        }
    }
    if (timeslice == 0)
    {
        stop_playing_flight_sample_in_all_flying_creatures();
    }
    timeslice = (timeslice + 1) % 4;
}

static int clear_active_dungeons_stats(void)
{
  struct Dungeon *dungeon;
  int i;
  for (i=0; i < PLAYERS_COUNT; i++)
  {
      dungeon = get_dungeon(i);
      if (dungeon_invalid(dungeon))
          break;
      memset((char *)dungeon->crmodel_state_type_count, 0, game.conf.crtr_conf.model_count * STATE_TYPES_COUNT * sizeof(uint16_t));
      memset((char *)dungeon->guijob_all_creatrs_count, 0, game.conf.crtr_conf.model_count *3*sizeof(uint16_t));
      memset((char *)dungeon->guijob_angry_creatrs_count, 0, game.conf.crtr_conf.model_count *3*sizeof(uint16_t));
  }
  return i;
}





/**
 * rules can change by dkscript/lua.
 * Checks if a gamerule for lighting has changed and updates the lights if they are.
 * This function also refreshes the light status of the map.
*/
static void update_global_lighting()
{
    if (!game.lish.light_auto_sync)
        return;

    // Check if any values have changed
    if (
        game.conf.rules[0].gameplay.global_ambient_light != game.lish.global_ambient_light ||
        game.conf.rules[0].gameplay.light_enabled != game.lish.light_enabled
    ){

        // GlobalAmbientLight
        if (game.conf.rules[0].gameplay.global_ambient_light != game.lish.global_ambient_light)
        {
            game.lish.global_ambient_light = game.conf.rules[0].gameplay.global_ambient_light;
        }

        // LightEnabled
        if (game.conf.rules[0].gameplay.light_enabled != game.lish.light_enabled)
        {
            game.lish.light_enabled = game.conf.rules[0].gameplay.light_enabled;
        }

        // Refresh the lights
        light_stat_refresh();
    }
}

void update(void)
{
    KFX_ZONE_COLOR("update", KFX_COLOR_SIMULATION);
    SYNCDBG(4,"Starting for turn %ld",(long)get_gameturn());

    const int64_t world_prof_turn = world_prof_now();
    WORLD_PROF(WPS_Packets, process_packets());
    update_local_cameras();
    api_update_server();

    if (quit_game || exit_keeper) {
        return;
    }
    if (game.game_kind == GKind_NonInteractiveState)
    {
        game.map_changed_for_navigation = 0;
        return;
    }

    if (!flag_is_set(game.operation_flags,GOF_Paused))
    {
        for (int i = 1; i < EVENTS_COUNT; i++) {
            game.event[i].flags &= ~EvF_BtnFalling;
        }
        if (local_state.lightning_palette_active)
        {
            PaletteSetUserPalette(get_local_user(), engine_palette);
            local_state.lightning_palette_active = false;
        }
        clear_active_dungeons_stats();
        update_creature_pool_state();
        WORLD_PROF(WPS_Things, update_things());
        WORLD_PROF(WPS_Rooms, process_rooms());
        WORLD_PROF(WPS_Dungeons, process_dungeons());
        WORLD_PROF(WPS_Research, update_research(); update_manufacturing());
        WORLD_PROF(WPS_Events, event_process_events(); update_all_events());
        WORLD_PROF(WPS_Script, process_level_script(); process_fx_lines(); lua_on_game_tick());
        if ((game.view_mode_flags & GNFldD_ComputerPlayerProcessing) != 0)
            WORLD_PROF(WPS_Computers, process_computer_players2());
        WORLD_PROF(WPS_Players, process_players());
        process_action_points();
        netstate_events_suppress(true);
        if ((get_user_view_type(get_my_user()) == PVT_CreatureContrl) || (get_user_view_type(get_my_user()) == PVT_CreaturePasngr))
        {
            struct Thing *thing = thing_get(get_user_state(get_displayed_user())->controlled_thing_idx);
            update_first_person_object_ambience(thing);
        }
        update_footsteps_nearest_camera(get_local_active_camera());
        netstate_events_suppress(false);
        PaletteFadePlayer(get_displayed_player());
        process_armageddon();
        WORLD_PROF(WPS_Lighting, update_global_lighting());
        WORLD_PROF(WPS_World, world_process_turn());
#if (BFDEBUG_LEVEL > 9)
        lights_stats_debug_dump();
        things_stats_debug_dump();
        creature_stats_debug_dump();
#endif
        world_prof_add(WPS_Turn, world_prof_turn);
#ifdef TRACY_ENABLE
        FrameMarkNamed("game turn");
        TracyPlot("Things: creatures", (int64_t)game.thing_lists[TngList_Creatures].count);
        TracyPlot("World: chunks loaded", (int64_t)world_loaded_chunk_count());
#endif
        game.play_gameturn++;
        world_after_turn();
        if (replay.turns_packetoff == game.play_gameturn)
            exit_keeper = 1;
    }
    netstate_events_suppress(true); /* this machine's own predicted sounds: not sent to thin clients */
    update_local_possession();
    update_local_first_person_view();
    netstate_events_suppress(false);

    message_update();
    update_camera_shake();
    netstate_events_suppress(true);
    update_player_sounds();
    netstate_events_suppress(false);
    SYNCDBG(6,"Finished");
}



/** A thin client: creatures as this machine's player sees them. The server's rendering flags are its own player's
 *  view (update_creature_rendering_flags is per viewer): here the creature this player possesses is hidden from it,
 *  the host's isn't, and invisible creatures show faint to their owner only. */
static void thin_client_creature_rendering(void)
{
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    for (ThingIndex i = slist->index; i > 0;)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        const unsigned char keep = thing->rendering_flags & TRF_AnimateOnce;
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        const TbBool entering = !creature_control_invalid(cctrl) && (thing->active_state == CrSt_CreatureHeroEntering) && (cctrl->countdown >= 0);
        update_creature_rendering_flags(thing);
        thing->rendering_flags |= keep;
        if (entering)
            thing->rendering_flags |= TRF_Invisible; /* still coming down from the ceiling */
    }
}

/** A thin client's turn (network play in an endless world): no simulation. The server's state for the turn comes
 *  in (several turns while behind), then only this machine's presentation runs: effects, sounds, the camera,
 *  messages. */
void update_thin_client(void)
{
    KFX_ZONE_COLOR("update_thin_client", KFX_COLOR_SIMULATION);
    const int queued = netstate_client_queued();
    int turns = (queued > 3) ? min(queued - 2, 50) : 1;
    while ((turns-- > 0) && netstate_client_apply_next()) {
    }
    thin_client_creature_rendering();
    update_local_cameras();
    update_dig_prediction_cursor_preview();
    network_spectator_thin_notice();
    if (quit_game || exit_keeper) {
        return;
    }
    if (!flag_is_set(game.operation_flags, GOF_Paused))
    {
        if (local_state.lightning_palette_active)
        {
            PaletteSetUserPalette(get_local_user(), engine_palette);
            local_state.lightning_palette_active = false;
        }
        /* effects come with the state (the server makes them round every player's view); the ambient sounds near
           this camera play here */
        update_things_sounds_in_list(&game.thing_lists[TngList_AmbientSnds]);
        if ((get_user_view_type(get_my_user()) == PVT_CreatureContrl) || (get_user_view_type(get_my_user()) == PVT_CreaturePasngr))
        {
            struct Thing *thing = thing_get(get_user_state(get_displayed_user())->controlled_thing_idx);
            update_first_person_object_ambience(thing);
        }
        update_footsteps_nearest_camera(get_local_active_camera());
        PaletteFadePlayer(get_displayed_player());
        world_client_turn();
    }
    netstate_events_suppress(true); /* this machine's own predicted sounds: not sent to thin clients */
    update_local_possession();
    update_local_first_person_view();
    netstate_events_suppress(false);
    message_update();
    update_camera_shake();
    update_player_sounds();
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif
/******************************************************************************/
/******************************************************************************/

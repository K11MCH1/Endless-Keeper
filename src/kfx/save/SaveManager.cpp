/******************************************************************************/
/** @file SaveManager.cpp
 *     The save manager: savegame slots (save, load, check) and the C entry points of everything it does.
 */
/******************************************************************************/
#include "pre_inc.h"
#include <chrono>
#include <string>
#include "game_saves.h"

#include "globals.h"
#include "bflib_basics.h"
#include "bflib_dernc.h"

#include "config.h"
#include "config_campaigns.h"
#include "config_settings.h"
#include "dungeon_stats.h"
#include "config_creature.h"
#include "config_crtrmodel.h"
#include "config_compp.h"
#include "sound_manager.h"
#include "custom_sprites.h"
#include "front_simple.h"
#include "frontend.h"
#include "frontmenu_ingame_tabs.h"
#include "front_landview.h"
#include "front_highscore.h"
#include "front_lvlstats.h"
#include "front_network.h"
#include "lens_api.h"
#include "local_camera.h"
#include "gui_soundmsgs.h"
#include "game_legacy.h"
#include "game_merge.h"
#include "frontmenu_ingame_map.h"
#include "gui_boxmenu.h"
#include "net_exchange_gameplay.h"
#include "packets.h"
#include "player_data.h"
#include "roomspace.h"
#include "config_keeperfx.h"
#include "keeperfx.hpp"
#include "api.h"
#include "lvl_filesdk1.h"
#include "lua_base.h"
#include "lua_triggers.h"
#include "moonphase.h"
#include "ariadne_update.h"
#include "replay.h"
#include "world_pages.h"
#include "user_prefs.h"
#include "light_data.h"
#include "map_ceiling.h"
#include "map_columns.h"
#include "map_data.h"
#include "thing_creature.h"
#include "kfx/platform/IPlatform.h"
#include "world_stream.h"
#include "bflib_datetm.h"
#include "kfx_jobs.h"
#include "post_inc.h"

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "kfx/save/SaveManager_Internal.h"
#include "kfx/save/core/save_bytes.h"
#include "kfx/save/core/save_config_overlay.h"
#include "kfx/save/core/save_names.h"
#include "kfx/save/core/schema/save_tables_decl.h"

namespace {

IFileSystem &Fs() { return *GetPlatform()->GetFileSystem(); }

void set_detail(char *detail, size_t detail_len, const char *text)
{
    if ((detail != nullptr) && (detail_len > 0))
        snprintf(detail, detail_len, "%s", text);
}

} // namespace

/******************************************************************************/

/******************************************************************************/
/* A save written on its own thread. Building a savegame of an endless world takes most of a second (2026-11-13: the
 * 124 MB game record 0.5 s to encode and 0.27 s to pack), and the dawn autosave held every player's game up for it.
 * The main thread now only makes what the save is made of - a copy of the game and the level data, the Lua state, the
 * endless world's blob, the slot's entry - and a thread builds the file from those and writes it.
 * The thread reads nothing of the running game: only what is in this struct, the save tables (constants), the
 * config baseline (the main thread leaves it alone while a save is being written: take_conf_baseline waits) and,
 * for a thing's union, the object config (read only). It writes no log line; what it found is logged when the main
 * thread takes the result. */
namespace {

struct BackgroundSave {
    SDL_Thread *thread = nullptr;
    SDL_AtomicInt finished = {0};
    /* what the save is made of (the thread only reads these) */
    Game *game_copy = nullptr;
    IntralevelData *ilvl_copy = nullptr;
    const Configs *baseline = nullptr;
    char *lua_data = nullptr;
    uint32_t lua_len = 0;
    unsigned char *world_data = nullptr;
    uint32_t world_len = 0;
    bool has_world = false;
    CatalogueEntry centry = {};
    IFileSystem *filesystem = nullptr;
    std::string fname;
    int slot = -1;
    /* what came of it (written by the thread before it sets finished) */
    bool ok = false;
    char error[400] = "";
    uint32_t file_len = 0;
    int64_t held_ns = 0, copy_ns = 0, world_ns = 0, build_ns = 0, write_ns = 0;
    /* a finished save's result, until it is taken */
    enum SaveBackgroundState result = SBg_None;
} background_save;

/* The copy of the game, a block a job. */
const size_t COPY_BLOCK = 2u << 20;
struct CopyBlocks {
    uint8_t *dst;
    const uint8_t *src;
    size_t len;
};

void copy_blocks_job(uint32_t start, uint32_t end, uint32_t, void *args)
{
    const CopyBlocks *blocks = static_cast<const CopyBlocks *>(args);
    const size_t from = static_cast<size_t>(start) * COPY_BLOCK;
    const size_t to = std::min(static_cast<size_t>(end) * COPY_BLOCK, blocks->len);
    if (from < to)
        memcpy(blocks->dst + from, blocks->src + from, to - from);
}

void background_save_free(BackgroundSave &save)
{
    free(save.game_copy);
    free(save.ilvl_copy);
    free(save.lua_data);
    free(save.world_data);
    save.game_copy = nullptr;
    save.ilvl_copy = nullptr;
    save.lua_data = nullptr;
    save.world_data = nullptr;
}

int background_save_thread(void *)
{
    BackgroundSave &save = background_save;
    /* (below the game's own thread: on a machine with one core free it works while the game waits for its next turn) */
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    const int64_t t0 = get_time_tick_ns();
    SaveBufferOwner out;
    SaveError err;
    bool ok = true;
    /* (the copy is not the running game, so Build leaves the endless world out: its blob, made with the copy, follows) */
    if (savegame::Build(save.game_copy, save.ilvl_copy, save.baseline, save.lua_data, save.lua_len, &save.centry, &out,
        &err) != SVR_Ok)
    {
        snprintf(save.error, sizeof(save.error), "can't build the save: %s", err.message);
        ok = false;
    }
    if (ok && save.has_world && (save_chunk_add(&out, SCID_World, 0, save.world_data, save.world_len, false) != 0))
    {
        snprintf(save.error, sizeof(save.error), "can't build the save: out of memory");
        ok = false;
    }
    const int64_t t1 = get_time_tick_ns();
    if (ok && (save.filesystem->WriteAll(save.fname.c_str(), out.data, out.len) != FsStatus::Ok))
    {
        snprintf(save.error, sizeof(save.error), "the file could not be written");
        ok = false;
    }
    save.build_ns = t1 - t0;
    save.write_ns = get_time_tick_ns() - t1;
    save.file_len = out.len;
    save.ok = ok;
    SDL_SetAtomicInt(&save.finished, 1);
    return 0;
}

} // namespace

bool SaveManager::SaveInBackground(int slot)
{
    BackgroundSave &save = background_save;
    if (save.thread != nullptr)
    {
        if (!SDL_GetAtomicInt(&save.finished))
            return false; /* the last one is still being written */
        BackgroundWait();
    }
    if (!EnsureSlot(slot))
        return false;
    const int64_t t0 = get_time_tick_ns();
    /* In the order a save made here takes them: the lights, the Lua state, the game, the endless world. */
    light_export_system_state(&game.lightst);
    size_t lua_len = 0;
    save.lua_data = lua_get_serialised_data(&lua_len);
    save.game_copy = static_cast<Game *>(malloc(sizeof(Game)));
    save.ilvl_copy = static_cast<IntralevelData *>(malloc(sizeof(IntralevelData)));
    if ((save.lua_data == nullptr) || (lua_len > INT32_MAX) || (save.game_copy == nullptr) || (save.ilvl_copy == nullptr))
    {
        background_save_free(save); /* (no memory for the copy, or no Lua state: a save made here says why) */
        return false;
    }
    const int64_t t1 = get_time_tick_ns();
    /* (the game is 138 MB: copied in blocks on the job threads, a third of the time one thread takes) */
    CopyBlocks blocks = { reinterpret_cast<uint8_t *>(save.game_copy), reinterpret_cast<const uint8_t *>(&game), sizeof(Game) };
    kfx_parallel_for(static_cast<uint32_t>((sizeof(Game) + COPY_BLOCK - 1) / COPY_BLOCK), 1, copy_blocks_job, &blocks);
    memcpy(save.ilvl_copy, &intralvl, sizeof(IntralevelData));
    const int64_t t2 = get_time_tick_ns();
    save.has_world = world_mode_active();
    save.world_len = 0;
    if (save.has_world && !world_save_blob(&save.world_data, &save.world_len))
    {
        background_save_free(save);
        return false;
    }
    save.copy_ns = t2 - t1;
    save.world_ns = get_time_tick_ns() - t2;
    save.lua_len = static_cast<uint32_t>(lua_len);
    save.baseline = conf_baseline;
    save.centry = catalogue_[slot];
    save.filesystem = &Fs();
    save.fname = prepare_file_fmtpath(FGrp_Save, SAVED_GAME_FILENAME, slot);
    save.slot = slot;
    save.ok = false;
    save.error[0] = '\0';
    save.result = SBg_None;
    save.held_ns = get_time_tick_ns() - t0;
    SDL_SetAtomicInt(&save.finished, 0);
    save.thread = SDL_CreateThread(background_save_thread, "save", nullptr);
    if (save.thread == nullptr)
    {
        background_save_free(save);
        return false;
    }
    return true;
}

void SaveManager::BackgroundWait()
{
    BackgroundSave &save = background_save;
    if (save.thread == nullptr)
        return;
    SDL_WaitThread(save.thread, nullptr);
    save.thread = nullptr;
    background_save_free(save);
    if (save.ok)
    {
        UpdateLastFileAfterSave(save.slot);
        api_event("GAME_SAVED");
        SAVELOG("slot %d written on its own thread: the game was held %.1f ms for it (its copy %.1f, the endless world "
            "%.1f); building %.1f ms, the file %.1f ms (%u bytes)", save.slot, (double)save.held_ns / 1e6,
            (double)save.copy_ns / 1e6, (double)save.world_ns / 1e6, (double)save.build_ns / 1e6,
            (double)save.write_ns / 1e6, (unsigned)save.file_len);
    } else
    {
        SAVEWARN("can't write save file \"%s\": %s", save.fname.c_str(), save.error);
    }
    save.result = save.ok ? SBg_Done : SBg_Failed;
}

enum SaveBackgroundState SaveManager::BackgroundPoll()
{
    BackgroundSave &save = background_save;
    if (save.thread != nullptr)
    {
        if (!SDL_GetAtomicInt(&save.finished))
            return SBg_Busy;
        BackgroundWait();
    }
    const enum SaveBackgroundState result = save.result;
    save.result = SBg_None;
    return result;
}

/**
 * Saves the game state file (savegame).
 * @note FillSlot() should be called before to fill level information.
 */
bool SaveManager::Save(int slot)
{
    BackgroundWait(); /* (a save still being written on its own thread: this one comes after it) */
    if (!EnsureSlot(slot))
    {
        SAVEERR("can't save: slot index %d is out of range", slot);
        return false;
    }
    const char *fname = prepare_file_fmtpath(FGrp_Save, SAVED_GAME_FILENAME, slot);
    // Some game data lives outside the saved structs; make sure it's current first.
    light_export_system_state(&game.lightst);
    if (!savegame::Write(Fs(), fname, &catalogue_[slot]))
    {
        SAVEWARN("can't write save file \"%s\"", fname);
        return false;
    }
    UpdateLastFileAfterSave(slot);
    api_event("GAME_SAVED");
    return true;
}

enum SaveCheckResult SaveManager::Check(int slot, char *detail, size_t detail_len)
{
    if ((slot < 0) || (slot >= SAVE_SLOTS_LIMIT))
        return SvChk_Missing;
    char reason[256];
    int title_ok;
    char fname[DISKPATH_SIZE];
    snprintf(fname, sizeof(fname), "%s", prepare_file_fmtpath(FGrp_Save, SAVED_GAME_FILENAME, slot));
    enum SaveCheckResult result = savegame::CheckFile(Fs(), fname, nullptr, &title_ok, reason, sizeof(reason));
    // An endless world this build can't restore is refused here, before a load changes anything.
    if (result == SvChk_Loadable)
        result = savegame::CheckWorld(Fs(), fname, reason, sizeof(reason));
    if (result != SvChk_Loadable)
        SAVEWARN("slot %d can't be loaded: %s", slot, reason);
    set_detail(detail, detail_len, reason);
    return result;
}

enum SaveCheckResult SaveManager::Verify(int slot, char *detail, size_t detail_len)
{
    enum SaveCheckResult result = Check(slot, detail, detail_len);
    if (result != SvChk_Loadable)
        return result;
    SaveError err;
    enum SaveResult file_result = savegame::Verify(Fs(), prepare_file_fmtpath(FGrp_Save, SAVED_GAME_FILENAME, slot), &err);
    if (file_result != SVR_Ok)
        set_detail(detail, detail_len, err.message);
    return savegame::Classify(file_result);
}

TextStringId SaveManager::CheckMessage(SaveCheckResult result)
{
    switch (result)
    {
    case SvChk_Incompatible:
        return GUIStr_SaveIncompatible;
    case SvChk_Damaged:
        return GUIStr_SaveDamaged;
    default:
        return GUIStr_SaveLoadFailed;
    }
}

bool SaveManager::KeepUnreadableFile(const char *fname)
{
    return savefile::KeepUnreadable(Fs(), fname);
}

/******************************************************************************/

/** A save read into scratch memory. Nothing in the running game changes until all of it has been read. */
struct LoadedSave {
    std::unique_ptr<Game, savebytes::FreeDeleter> game;
    std::unique_ptr<IntralevelData, savebytes::FreeDeleter> ilvl;
    CatalogueEntry centry = {};
    savegame::ReadResult read_result = {};

    ~LoadedSave() { Release(); }
    void Release()
    {
        savegame::FreeReadResult(&read_result);
        game.reset();
        ilvl.reset();
    }
};

/** Reads and decodes a KFXS save (already known to be loadable by Check) into scratch memory. */
/** Where a load's time goes (2026-11-24: Continue Game took 2.6 s with OpenGL ES and 4-6 s with Vulkan on a phone
 *  after a fresh start): one log line at the end, the steps over 20 ms. */
namespace {
struct LoadSteps {
    std::chrono::steady_clock::time_point start, last;
    std::string text;
    void Begin() { start = last = std::chrono::steady_clock::now(); text.clear(); }
    void Mark(const char* step)
    {
        const auto now = std::chrono::steady_clock::now();
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        last = now;
        if (ms >= 20)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s%s %ld ms", text.empty() ? "" : ", ", step, ms);
            text += buf;
        }
    }
    long Total() const
    {
        return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }
};
LoadSteps load_steps;
}

static enum SaveCheckResult read_game_kfxs(const char *fname, struct LoadedSave *loaded_save)
{
    struct SaveError err;
    loaded_save->game.reset(static_cast<Game *>(malloc(sizeof(Game))));
    loaded_save->ilvl.reset(static_cast<IntralevelData *>(malloc(sizeof(IntralevelData))));
    if (!loaded_save->game || !loaded_save->ilvl)
    {
        SAVEWARN("out of memory reading \"%s\"", fname);
        loaded_save->Release();
        return SvChk_Missing;
    }
    enum SaveResult result = savegame::Read(Fs(), fname, &loaded_save->centry, loaded_save->game.get(), loaded_save->ilvl.get(), &loaded_save->read_result, &err);
    if (result != SVR_Ok)
    {
        SAVEWARN("can't read \"%s\": %s", fname, err.message);
        loaded_save->Release();
        return savegame::Classify(result);
    }
    return SvChk_Loadable;
}

/** Rebuilds the parts of struct Game that the save doesn't carry; they depend on the loaded map and config. */
static void rebuild_derived_game_state(void)
{
    set_map_size(game.map_tiles_x, game.map_tiles_y);
    ceiling_set_info(12, 4, 1);
    init_top_texture_to_cube_table();
    init_creature_scores();
}

/**
 * Puts a save that has been read in full into the running game. NAMS/CONF only mean anything once config
 * files are loaded for the save's own campaign and level, so that comes first. From here on the running
 * game is changed.
 */
static TbBool apply_game_kfxs(const char *fname, struct LoadedSave *loaded_save, struct CatalogueEntry *centry)
{
    const savegame::ReadResult *read_result = &loaded_save->read_result;
    struct SaveError err;
    *centry = loaded_save->centry;
    // Config loading reads the level numbers, and writes into parts of game (slab sets, research, ...)
    // that the save replaces afterwards, so the save goes in only after it.
    game.loaded_level_number = centry->level_num;
    game.selected_level_number = centry->level_num;
    if (!change_campaign(CampgnT_Default, centry->campaign_fname))
    {
        SAVEERR("can't load campaign \"%s\" for \"%s\"", centry->campaign_fname, fname);
        return false;
    }
    free_level_strings_data();
    load_map_string_data(&campaign, centry->level_num, get_level_fgroup(centry->level_num));
    load_steps.Mark("campaign + strings");
    // Load configs which may have per-campaign part, and can even be modified within a level
    recheck_all_mod_exist();
    load_steps.Mark("mods");
    init_custom_sprites(centry->level_num);
    load_steps.Mark("custom sprites");
    load_stats_files();
    load_steps.Mark("config files");
    snprintf(high_score_entry, PLAYER_NAME_LENGTH, "%s", centry->player_name);
    check_and_auto_fix_stats();
    // Same point as at a level start: the baseline the next save's config overlay is diffed against is the
    // config as loaded from files, without this save's own changes.
    take_conf_baseline();
    load_steps.Mark("config baseline");
    // The save replaces the game; the config just loaded and the process's own fields (timing, seeds, ...) stay.
    save_copy_runtime(&save_desc_Game, loaded_save->game.get(), &game);
    memcpy(&game, loaded_save->game.get(), sizeof(game));
    memcpy(&intralvl, loaded_save->ilvl.get(), sizeof(intralvl));
    // Apply the save's config changes on top of the fresh config.
    if (save_config_overlay_apply(read_result->conf_data, read_result->conf_len, &game.conf, &err) != SVR_Ok)
    {
        SAVEWARN("can't apply the config overlay of \"%s\": %s", fname, err.message);
        return false;
    }
    // Refuses if a used index no longer names the same config entry in this build.
    if (save_names_check(read_result->nams_data, read_result->nams_len, &game, &err) != SVR_Ok)
    {
        SAVEWARN("\"%s\" refused: %s (level %d of campaign \"%s\")", fname, err.message,
            (int)centry->level_num, centry->campaign_fname);
        return false;
    }
    if (read_result->lua_len > 0)
    {
        // has to be loaded here as level num only filled while gamestruct loaded, and need it for setting serialised_data
        if (!open_lua_script(get_loaded_level_number()) || !lua_set_serialised_data(read_result->lua_data, read_result->lua_len, lua_bytecode_in_saves))
        {
            SAVEWARN("can't restore the Lua state of \"%s\"", fname);
            return false;
        }
    }
    load_steps.Mark("game state + Lua");
    if (read_result->world_len > 0)
    {
        if (!world_load_blob(read_result->world_data, read_result->world_len))
        {
            /* without its world the level can't run (a chunked map would run on the flat arrays) */
            SAVEWARN("the endless world of \"%s\" could not be restored", fname);
            return false;
        }
    } else
    {
        if (game.map_tiles_x == WP_PAGED_TILES)
        {
            SAVEWARN("\"%s\" is a chunked endless world without its world data", fname);
            return false;
        }
        world_load_none();
    }
    load_steps.Mark("world");
    rebuild_derived_game_state();
    update_trap_tab_to_config();
    update_room_tab_to_config();
    load_steps.Mark("derived state");
    return true;
}

enum SaveLoadOutcome SaveManager::Load(int slot_num, enum SaveCheckResult *why)
{
    load_steps.Begin();
    BackgroundWait(); /* (a save still being written on its own thread may be this very slot's) */
    load_steps.Mark("waiting for a save");
    *why = SvChk_Loadable;
    if (!EnsureSlot(slot_num))
    {
        SAVEERR("can't load: slot index %d is out of range", (int)slot_num);
        *why = SvChk_Missing;
        return SvLoad_Refused;
    }
    SYNCDBG(6,"Starting");
    // Nothing may change before the save has been read in full: a refused save leaves the
    // frontend or the running game as it was.
    *why = Check(slot_num, nullptr, 0);
    if (*why != SvChk_Loadable)
        return SvLoad_Refused;
    // prepare_file_fmtpath()'s buffer is overwritten by its next call, and
    // apply_game_kfxs() below triggers several (change_campaign, load_stats_files...),
    // so take a stable copy now.
    char fname[DISKPATH_SIZE];
    snprintf(fname, sizeof(fname), "%s", prepare_file_fmtpath(FGrp_Save, SAVED_GAME_FILENAME, slot_num));
    if (!Fs().Exists(fname))
    {
        SAVEWARN("can't open saved game file \"%s\"", fname);
        DisableSlot(slot_num);
        *why = SvChk_Missing;
        return SvLoad_Refused;
    }
    struct LoadedSave loaded_save;
    load_steps.Mark("checking the slot");
    *why = read_game_kfxs(fname, &loaded_save);
    if (*why != SvChk_Loadable)
        return SvLoad_Refused;
    load_steps.Mark("reading the file");
    reset_eye_lenses();
    load_steps.Mark("eye lenses");
    struct CatalogueEntry* centry = &catalogue_[slot_num];

        // Check if the game version is compatible
    if ((centry->game_ver_major != VER_MAJOR) || (centry->game_ver_minor != VER_MINOR) ||
        (centry->game_ver_release != VER_RELEASE) || (centry->game_ver_build != VER_BUILD))
    {
        SAVEWARN("\"%s\" was made by version %d.%d.%d.%d, this is %d.%d.%d.%d",
            fname, (int)centry->game_ver_major, (int)centry->game_ver_minor,
            (int)centry->game_ver_release, (int)centry->game_ver_build,
            VER_MAJOR, VER_MINOR, VER_RELEASE, VER_BUILD);
    }

    // Here is the actual loading
    TbBool applied = apply_game_kfxs(fname, &loaded_save, centry);
    loaded_save.Release();
    if (!applied)
    {
        if (game.loaded_level_number == 0)
        {
            game.loaded_level_number = centry->level_num;
        }
        SAVEWARN("slot %d didn't load correctly", (int)slot_num);
        return SvLoad_Broken;
    }
    my_player_number = game.local_plyr_idx;
    // Re-apply creature sound overrides: the config overlay (or a fresh config load)
    // may have restored session-specific negative bank indices; fix them to match
    // the current session's custom bank layout.
    sound_manager_reapply_creature_sounds();
    snprintf(game.campaign_fname, sizeof(game.campaign_fname), "%s", campaign.fname);
    local_state.show_gui = true;
    load_steps.Mark("sounds");
    init_navigation();
    load_steps.Mark("navigation");
    reinit_level_after_load();
    load_steps.Mark("level after load");
    take_game_timestamp();
    reinit_packets_after_load();
    initialize_packet_history();
    clear_packets();
    process_pause_packet(0, 0);
    clear_flag(game.operation_flags, GOF_Paused);
    clear_flag(game.operation_flags, GOF_WorldInfluence);
    close_main_cheat_menu();
    close_creature_cheat_menu();
    close_instance_cheat_menu();
    close_secondary_cheat_menu();
    output_message(SMsg_GameLoaded, 0);
    calculate_moon_phase(false,false);
    update_extra_levels_visibility();
    struct PlayerInfo* player = get_my_player();
    local_state.lightning_palette_active = false;
    local_state.freeze_palette_active = false;
    local_state.view_type = PVT_None;
    local_state.show_panel = true;
    local_state.palette_fade_step_pain = 0;
    local_state.palette_fade_step_possession = 0;
    local_state.lens_palette = 0;
    local_state.minimap_pos_x = 11;
    local_state.minimap_pos_y = 11;
    local_state.roomspace_size = DEFAULT_USER_ROOMSPACE_WIDTH;
    PaletteSetUserPalette(get_local_user(), engine_palette);
    apply_local_user_preferences(get_local_user(), UPF_ApplyOnLoad);
    init_local_cameras(player);
    // Update the lights system state
    load_steps.Mark("packets + state");
    light_import_system_state(&game.lightst);
    light_rebuild_after_load();
    load_steps.Mark("lights");
    // Victory state
    if (player->victory_state != VicS_Undecided)
    {
      frontstats_initialise();
      struct Dungeon* dungeon = get_players_dungeon(player);
      dungeon->lvstats.player_score = 0;
      dungeon->lvstats.allow_save_score = 1;
    }
    game.loaded_swipe_idx = -1;
    SAVELOG("loaded level %d of %s", game.continue_level_number, campaign.name);
    SYNCLOG("Save load timing: %ld ms; %s", load_steps.Total(), load_steps.text.empty() ? "no step over 20 ms" : load_steps.text.c_str());

    api_event("GAME_LOADED");

    return SvLoad_Done;
}

/******************************************************************************/

enum SaveResult SaveManager::ReadHighScores(const char *fname, HighScore **table, uint32_t *count, SaveError *err)
{
    std::vector<HighScore> scores;
    enum SaveResult result = savefile::ReadRecords(Fs(), fname, scores, err);
    *count = (uint32_t)scores.size();
    *table = nullptr;
    if (!scores.empty())
    {
        *table = static_cast<HighScore *>(malloc(scores.size() * sizeof(HighScore)));
        if (*table == nullptr)
        {
            *count = 0;
            return save_fail(err, SVR_NoMemory, "out of memory");
        }
        memcpy(*table, scores.data(), scores.size() * sizeof(HighScore));
    }
    return result;
}

bool SaveManager::WriteHighScores(const char *fname, const HighScore *table, uint32_t count)
{
    return savefile::WriteRecords(Fs(), fname, table, count);
}

/******************************************************************************/

SaveManager &GetSaveManager()
{
    static SaveManager manager;
    return manager;
}

extern "C" {

int SaveManager_Save(int slot) { return GetSaveManager().Save(slot); }
int SaveManager_SaveInBackground(int slot) { return GetSaveManager().SaveInBackground(slot); }
enum SaveBackgroundState SaveManager_BackgroundPoll(void) { return GetSaveManager().BackgroundPoll(); }
void SaveManager_BackgroundWait(void) { GetSaveManager().BackgroundWait(); }
enum SaveLoadOutcome SaveManager_Load(int slot, enum SaveCheckResult *why) { return GetSaveManager().Load(slot, why); }
enum SaveCheckResult SaveManager_Check(int slot, char *detail, size_t detail_len) { return GetSaveManager().Check(slot, detail, detail_len); }
enum SaveCheckResult SaveManager_Verify(int slot, char *detail, size_t detail_len) { return GetSaveManager().Verify(slot, detail, detail_len); }
TextStringId SaveManager_CheckMessage(enum SaveCheckResult result) { return SaveManager::CheckMessage(result); }
int SaveManager_KeepUnreadableFile(const char *fname) { return SaveManager::KeepUnreadableFile(fname); }

int SaveManager_LoadCatalogue(void) { return GetSaveManager().LoadCatalogue(); }
int SaveManager_InitialiseSlots(void) { return GetSaveManager().InitialiseSlots(); }
int SaveManager_SavedGamesCount(void) { return GetSaveManager().SavedGamesCount(); }
int SaveManager_SlotCount(void) { return GetSaveManager().SlotCount(); }
struct CatalogueEntry *SaveManager_Entry(int slot) { return GetSaveManager().Entry(slot); }
int SaveManager_FillSlot(int slot, const char *textname) { return GetSaveManager().FillSlot(slot, textname); }
int SaveManager_FillEntry(struct CatalogueEntry *centry, const char *textname) { return SaveManager::FillEntry(centry, textname); }
int SaveManager_DisableSlot(int slot) { return GetSaveManager().DisableSlot(slot); }

int SaveManager_ContinueAvailable(void) { return GetSaveManager().ContinueAvailable(); }
enum ContinueTargets SaveManager_LoadContinue(void) { return GetSaveManager().LoadContinue(); }
int SaveManager_SaveLevelProgress(LevelNumber lvnum, unsigned char victory_state) { return GetSaveManager().SaveLevelProgress(lvnum, victory_state); }
int SaveManager_ResumeCampaign(const char *campaign_fname) { return GetSaveManager().ResumeCampaign(campaign_fname); }
int SaveManager_CampaignHasProgress(const struct GameCampaign *campgn) { return GetSaveManager().CampaignHasProgress(campgn); }
void SaveManager_UpdateCampaignPercents(struct CampaignsList *clist) { GetSaveManager().UpdateCampaignPercents(clist); }
void SaveManager_DeleteContinueLink(void) { GetSaveManager().DeleteContinueLink(); }

enum SaveResult SaveManager_ReadHighScores(const char *fname, struct HighScore **table, uint32_t *count,
    struct SaveError *err)
{
    return GetSaveManager().ReadHighScores(fname, table, count, err);
}

int SaveManager_WriteHighScores(const char *fname, const struct HighScore *table, uint32_t count)
{
    return GetSaveManager().WriteHighScores(fname, table, count);
}

} // extern "C"

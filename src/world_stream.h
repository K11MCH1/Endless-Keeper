/******************************************************************************/
/** @file world_stream.h
 *     Endless world: a fixed-size map window that slides over a generated world.
 * @par Purpose:
 *     A level with ENDLESS_WORLD(<seed>) in its script is played as a window of
 *     WORLD_WINDOW_CHUNKS x WORLD_WINDOW_CHUNKS chunks (16x16 slabs each) plus a
 *     one-slab rock frame. When the view nears an edge, the whole game state is
 *     shifted by whole chunks (world_shift.c), the chunks left behind are frozen
 *     into a chunk store and the uncovered band is filled from the store or from
 *     the generator (world_gen.c). Nothing here runs on other levels.
 */
/******************************************************************************/
#ifndef DK_WORLD_STREAM_H
#define DK_WORLD_STREAM_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define WORLD_CHUNK_SLABS 16
#define WORLD_CHUNK_SUBTILES (WORLD_CHUNK_SLABS * 3)
#define WORLD_WINDOW_CHUNKS 10
#define WORLD_WINDOW_SLABS (WORLD_WINDOW_CHUNKS * WORLD_CHUNK_SLABS + 2)
/** The window shifts when the view comes within this many chunks of an edge... */
#define WORLD_SHIFT_MARGIN_CHUNKS 3
/** ...by this many chunks. */
#define WORLD_SHIFT_CHUNKS 3

/** True while an endless-world level is being played (after ENDLESS_WORLD was seen). */
TbBool world_mode_active(void);
/** The next world_save_blob is a resync for other machines: no seats' owners, upgrades or the world's key in it. */
void world_blob_for_network(TbBool on);
/** ENDLESS_WORLD script command, at script load: remembers the seed for world_level_start. */
TbBool world_request(const char *seed_text);
/** Endless Mode's New Run: the next ENDLESS_WORLD uses this seed instead of the script's (once). NULL clears it. */
void world_set_next_seed(const char *seed_text);
/** The seed the next world level will use instead of its script's ("" when none). */
const char *world_next_seed_text(void);
/** The seed as written (script or New Run), for save names. */
const char *world_seed_text(void);
/** After the level's map files are loaded: generates the first window around the start. */
void world_level_start(void);
/** Every game turn (from update()): shifts the window when needed, wakes keepers, checks hearts. */
void world_process_turn(void);
/** A thin client's world turn (network play): only this machine's presentation (colours, test shots). */
void world_client_turn(void);
void world_client_test_input(void);
/** A seat's owner: its player's reconnect token (the server's; 0 for none). */
uint64_t world_seat_token(PlayerNumber plyr_idx);
/** An imp's swing at gold (endless worlds): experience from mining. */
void world_imp_mined_gold(struct Thing *creatng);
void world_set_seat_token(PlayerNumber plyr_idx, uint64_t token);
/** A player joined the running world (server): a new start dungeon, gold, the kit, and the view on the heart. */
void world_seat_joined(PlayerNumber plyr_idx);
/** Start again (2026-11-08): may this defeated keeper's seat start again (an endless world's human seat)? */
TbBool world_seat_can_respawn(PlayerNumber plyr_idx);
/** Frees the defeated keeper's seat (its leftovers nobody's, its records from zero) and gives it to the same player
 *  again with a new start dungeon at the next spawn point. Game state: the server, or the one machine, does it. */
TbBool world_seat_respawn(PlayerNumber plyr_idx);
/** Dungeon reset (2026-11-09, Keeper Server's /reset): the keeper's dungeon is wiped (its ground made again from the
 *  generator) and the same seat starts elsewhere with a new start dungeon, keeping level, XP, research and upgrades.
 *  False (and why) when it can't. Game state: the server, or the one machine, does it. */
TbBool world_seat_reset(PlayerNumber plyr_idx, char *why, size_t why_len);
/** This machine's camera to its keeper's heart (false while the heart isn't known here yet). */
TbBool world_local_camera_to_heart(void);
/** After each game turn, once game.play_gameturn has moved on (between turns, like a save from the menu). */
void world_after_turn(void);
/** When the level ends or another level is loaded: frees the world. */
void world_level_end(void);

/** Computer keepers that are generated but haven't seen the player yet don't run their AI. */
TbBool world_keeper_is_passive(PlayerNumber plyr_idx);
TbBool world_pays_at_nearest_treasury(PlayerNumber plyr_idx);
uint64_t world_seed(void);
/** The biome at an engine subtile (world levels). */
int world_biome_at_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
int world_biome_at_engine_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);
TbBool world_player_heart_loaded(void);
/** Network resync: the host's world over this machine's (no exploration or heart binding after it), and then,
 *  after the host's navigation mesh is in, what derives from the map. */
TbBool world_load_blob_for_resync(const unsigned char *data, uint32_t len);
void world_after_resync(void);
/** A human player's heart isn't in a frozen chunk. */
TbBool world_human_heart_loaded(PlayerNumber plyr_idx);
/** A human player of the endless world: PLAYER0, or (network games) another user's player; not a world keeper,
 *  heroes or neutral. */
TbBool world_player_is_human(PlayerNumber plyr_idx);
/** Who holds a player slot: a world keeper (1000 + its index; slots are reused) or the player number itself. */
int32_t world_player_identity(PlayerNumber plyr_idx);
/** Is this player number held by a world keeper (not a seat)? */
TbBool world_player_is_keeper(PlayerNumber plyr_idx);
/** The players a human seat can take in a network world, in the order they are given out. */
int world_seat_players(const PlayerNumber **seats);
TbBool world_view_slab(MapSlabCoord *slb_x, MapSlabCoord *slb_y);
/** Where a user is looking (possessed creature or dungeon camera): game state, the same on every machine. */
TbBool world_user_view_slab(int user, MapSlabCoord *slb_x, MapSlabCoord *slb_y);
void world_slab_to_world(MapSlabCoord slb_x, MapSlabCoord slb_y, int64_t *wx, int64_t *wy);
void world_remove_thing_quietly(struct Thing *thing);
/** Is a world chunk made already (stored, or on the map)? */
TbBool world_chunk_made(int32_t cx, int32_t cy);
/** Slabs from a world slab to the nearest player start. */
double world_dist_to_nearest_start(int64_t x, int64_t y);
/** The world's generator (NULL outside a world). */
struct WgWorld *world_generator(void);
/** The other human seats' copies of the start: how many, and does one lie in this box of world slabs? */
int world_stamp_count(void);
TbBool world_stamps_overlap(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** The engine slab of a world slab, when it is on the map now (a loaded page, or inside the window). */
TbBool world_engine_slab(int64_t wx, int64_t wy, MapSlabCoord *ex, MapSlabCoord *ey);
/** The world chunks on the map now; returns how many (at most max). */
int world_loaded_chunks(int32_t *wcx, int32_t *wcy, int max);
/** How many chunks are loaded now; how many the world keeps (explored). */
int world_loaded_chunk_count(void);
long world_stored_chunk_count(void);

/** Development timer (KFX_WORLD_PROFILE=1, any level): world_prof_now() is 0 when it is off; world_prof_add adds
 *  the time since t0 to a section; every 200 game turns the averages and maxima are logged. */
enum WorldProfSection {
    WPS_Turn = 0, WPS_Packets, WPS_Things, WPS_Rooms, WPS_Dungeons, WPS_Research, WPS_Events, WPS_Script,
    WPS_Computers, WPS_Players, WPS_Lighting, WPS_World, WPS_Chunks, WPS_Frame,
    WPS_CmCollect, WPS_CmLoad, WPS_CmFreeze, WPS_CmAfter, WPS_CmScores, WPS_CmGold,
    WPS_FzRecord, WPS_FzClear, WPS_LdData, WPS_LdFill, WPS_RfNav, WPS_RfCeil, WPS_RfLight, WPS_RfPanel,
    WPS_PkChecksum, WPS_PkWrite, WPS_PkUsers,
    /* the things of a turn by kind, and route finding: summed over a turn (world_prof_sum) */
    WPS_TCreatures, WPS_TTraps, WPS_TShots, WPS_TObjects, WPS_TEffects, WPS_TOther, WPS_Path, WPS_COUNT
};
int64_t world_prof_now(void);
void world_prof_add(int section, int64_t t0);
/** Adds the time since t0 to a section shown as a total a turn (many calls a turn). */
void world_prof_sum(int section, int64_t t0);
#include "kfx/profiling/KfxProfilingC.h"
/* each section is a Tracy zone too (KEEPERFX_TRACY builds), named by its statement */
#define WORLD_PROF(section, stmt) do { KFX_C_ZONE_BEGIN(wp_zone_, #stmt); const int64_t wp_t0_ = world_prof_now(); stmt; \
    world_prof_add((section), wp_t0_); KFX_C_ZONE_END(wp_zone_); } while (0)
/** True when the endless level runs on the chunked (paged) world rather than the sliding window. */
TbBool world_is_chunked(void);
/** Slab kinds in a box of engine slabs, whether loaded or not: loaded chunks are read from the map, frozen ones
 *  from the chunk store, the rest from the generator. Nothing is loaded or registered. kinds and player_owned
 *  hold (x2-x1+1)*(y2-y1+1) cells, row by row; player_owned is 1 where plyr_idx owns the slab. */
TbBool world_read_slab_box(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2, uint8_t *kinds,
    uint8_t *player_owned, PlayerNumber plyr_idx);
/** Chunked world: loads the chunks within r of the chunk holding an engine slab now (a raid's landing spot).
 *  Returns how many were loaded. */
int world_load_chunks_round(MapSlabCoord slb_x, MapSlabCoord slb_y, int r);
/** The same round a world slab (loaded or not). */
int world_load_chunks_round_world(int64_t wx, int64_t wy, int r);
/** Chunked world: reads up to `budget` chunks of a box of engine slabs that aren't loaded into world_read_slab_box's
 *  cache ahead of time (no effect on the game; it only spreads the work). Returns how many it read. */
int world_peek_warm(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2, int budget);

/** Minimap: the panel colour code of an engine subtile outside the loaded window (or in the frame), from
 *  the map memory. 0 (unexplored) when nothing is remembered. Never generates or loads anything. */
unsigned short world_minimap_code(MapSubtlCoord stl_x, MapSubtlCoord stl_y);

/** Full-screen (parchment) map: it shows a game-map-sized view of the world that can be scrolled beyond the
 *  loaded window; outside the window it draws the map memory. These are local view settings, not game
 *  state. The view's top-left engine slab (outside 0..map_tiles when scrolled away); the view is centred
 *  on the camera again whenever the map is opened. */
void world_parchment_view_origin(MapSlabCoord *slb_x, MapSlabCoord *slb_y);
/** The same for a view of width x height pixels at block pixels per slab, and how many pixels of the top left
 *  slab are scrolled out of sight (0..block-1): the view scrolls smoothly, a pixel at a time. */
void world_parchment_view(long width, long height, long block, MapSlabCoord *slb_x, MapSlabCoord *slb_y,
    long *off_x, long *off_y);
void world_parchment_scroll(int dx_slabs, int dy_slabs);
/** Scrolls by 1/256 slabs (and stops a glide). */
void world_parchment_scroll_fine(int64_t dx, int64_t dy);
/** Glides the view to the player's Dungeon Heart. */
void world_parchment_home(void);
/** Glides the view to an engine subtile (a battle). */
void world_parchment_glide_to_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Moves a glide on by this much real time (each frame). */
void world_parchment_glide(float seconds);
/** Test hook KFX_WORLD_MAPEDGE: true while it puts the game's mouse at the window's edges (the system cursor is
 *  elsewhere, so the window check would say the mouse is outside). */
TbBool world_test_mouse_in_window(void);
/** Engine slab of the player's Dungeon Heart, also while its chunk is dropped (then outside the window). */
TbBool world_parchment_heart(MapSlabCoord *slb_x, MapSlabCoord *slb_y);
/** PckA_ZoomFromMap for a subtile outside the loaded window: moves the window there, in the game turn, and
 *  rewrites the subtile into the new window. Returns true if it moved the window. */
TbBool world_zoom_from_map(int user, int32_t *stl_x, int32_t *stl_y);
/** After the zoom packet: puts the local cameras on the new position if the window moved. */
void world_after_zoom_from_map(int user);

/** Saves: the world state as one blob (malloc'd, caller frees), and loading it back. */
TbBool world_save_blob(unsigned char **data, uint32_t *len);
TbBool world_load_blob(const unsigned char *data, uint32_t len);
/** What world_blob_check says of a saved world. */
enum WorldBlobCheck {
    WBC_Ok = 0,
    /** Made by a build this one can't read (e.g. chunk pages kept as raw memory of other map cells). */
    WBC_Incompatible,
    /** Cut short or not a world. */
    WBC_Damaged,
};
/** Reads a saved world without loading it (nothing changes), as far as the chunk pages, so a load can refuse a
 *  world it couldn't restore before it changes anything. why (may be NULL) gets the reason in plain text. */
enum WorldBlobCheck world_blob_check(const unsigned char *data, uint32_t len, char *why, size_t why_len);
/** Called after a save without a world blob was loaded: no world. */
void world_load_none(void);

#ifdef __cplusplus
}
#endif
#endif

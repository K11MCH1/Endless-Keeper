/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pages.h
 *     Chunked endless world: the engine map held as pages, one per loaded 16x16-slab chunk.
 * @par Purpose:
 *     In paged mode (`world_paged`) the map has no fixed size: the slab, subtile, navigation, light and minimap
 *     data that normal levels keep in flat arrays (game.map, game.slabmap, ...) live in pages that exist only for
 *     loaded chunks. The usual accessors (get_map_block_at, get_slabmap_block, ...) look here when paged mode is
 *     on; a coordinate in a chunk that isn't loaded reads as an invalid block, like a place outside the map.
 *     Engine coordinates are world coordinates plus WP_ORIGIN_SLB slabs, and the virtual map is WP_PAGED_TILES
 *     slabs across, so the row lengths of packed numbers are powers of two (2^20 slabs, 2^22 subtiles). The size
 *     is bounded by the pathfinding, which keeps map units (subtile * 256) in 32 bits: its whole mesh, with a
 *     margin round the map, must stay below 2^23 subtiles across.
 *     Normal levels never turn paged mode on and keep their flat arrays.
 */
/******************************************************************************/
#ifndef DK_WORLD_PAGES_H
#define DK_WORLD_PAGES_H

#include "bflib_basics.h"
#include "globals.h"
#include "map_data.h"
#include "slab_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WP_CHUNK_SLABS 16
#define WP_CHUNK_STL (WP_CHUNK_SLABS * STL_PER_SLB)
#define WP_CHUNK_STL_CELLS (WP_CHUNK_STL * WP_CHUNK_STL)
#define WP_CHUNK_SLAB_CELLS (WP_CHUNK_SLABS * WP_CHUNK_SLABS)
/** Virtual map size in paged mode: map_tiles_x/y. */
#define WP_PAGED_TILES (1L << 20)
/** map_subtiles_x/y in paged mode: one less than the subtile row length 2^22 (more than 3 * WP_PAGED_TILES; the
 *  extra subtiles belong to slabs past the map and are never loaded). */
#define WP_STL_ROW_BITS 22
#define WP_PAGED_SUBTILES ((1L << WP_STL_ROW_BITS) - 1)
#define WP_SLB_ROW_BITS 20
/** Engine slab of world slab 0 (the middle of the virtual map). */
#define WP_ORIGIN_SLB (1L << 19)
/** Engine chunks per axis. */
#define WP_CHUNKS_PER_AXIS (WP_PAGED_TILES / WP_CHUNK_SLABS)

struct WorldPage {
    int32_t cx, cy;                                 /**< engine chunk */
    struct WorldPage *next;                         /**< the list of loaded pages */
    uint32_t wanted_turn;                           /**< the chunk manager's last pass that wanted it (0: not yet) */
    struct Map map[WP_CHUNK_STL_CELLS];             /**< as game.map */
    NavColour nav[WP_CHUNK_STL_CELLS];              /**< as game.navigation_map */
    unsigned short stat_light[WP_CHUNK_STL_CELLS];  /**< as game.lish.stat_light_map */
    unsigned short lightness[WP_CHUNK_STL_CELLS];   /**< as game.lish.subtile_lightness */
    unsigned short panel[WP_CHUNK_STL_CELLS];       /**< the minimap's colour codes (PanelMap) */
    unsigned char ceiling[WP_CHUNK_STL_CELLS];      /**< map_ceiling.c's scratch cache */
    struct SlabMap slab[WP_CHUNK_SLAB_CELLS];       /**< as game.slabmap */
    unsigned char ext[WP_CHUNK_SLAB_CELLS];         /**< as game.slab_ext_data */
    unsigned char ext_initial[WP_CHUNK_SLAB_CELLS]; /**< as game.slab_ext_data_initial */
    int32_t cell_page;                              /**< its number in the last wp_cells_begin (scratch maps) */
    uint32_t island_mark;                           /**< wp_island_bounds' visited mark */
};

/** True while the engine map lives in pages (a chunked endless world is being played). */
extern TbBool world_paged;

/** Turns paged mode on with an empty map of WP_PAGED_TILES slabs (sets the map size), or off (frees every page). */
void wp_set_paged(TbBool paged);

/** The page of an engine chunk, or NULL when it isn't loaded. */
struct WorldPage *wp_page(int32_t cx, int32_t cy);
/** The page of an engine chunk, created (zeroed) when it isn't loaded yet. NULL if out of range or out of memory. */
struct WorldPage *wp_create(int32_t cx, int32_t cy);
/** Frees a chunk's page. */
void wp_free(int32_t cx, int32_t cy);
/** The loaded pages, as a list (wp_first, then ->next). */
struct WorldPage *wp_first(void);
int wp_count(void);
/** Bounding box of the loaded chunks in engine slabs (inclusive). False when nothing is loaded. */
TbBool wp_bounds(MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2, MapSlabCoord *y2);

/** The page holding an engine subtile / slab, or NULL. */
struct WorldPage *wp_page_of_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
struct WorldPage *wp_page_of_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** Index of a subtile / slab inside its page. */
static inline int wp_stl_index(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    return (int)((stl_y % WP_CHUNK_STL) * WP_CHUNK_STL + (stl_x % WP_CHUNK_STL));
}
static inline int wp_slb_index(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    return (int)((slb_y % WP_CHUNK_SLABS) * WP_CHUNK_SLABS + (slb_x % WP_CHUNK_SLABS));
}

/** Map-wide loops: the slab range to walk ([0, map_tiles) normally, the loaded chunks' bounding box in paged
 *  mode; empty when nothing is loaded). Inclusive bounds; check x1 <= x2. */
void map_loop_slab_bounds(MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2, MapSlabCoord *y2);
/** The same in subtiles ([0, map_subtiles] normally). */
void map_loop_subtile_bounds(MapSubtlCoord *x1, MapSubtlCoord *y1, MapSubtlCoord *x2, MapSubtlCoord *y2);

/** Row-major loops that should only visit loaded ground: the loaded spans of row y within [x1, x2], in increasing x
 *  (pairs first, last in spans; returns how many). unit is WP_CHUNK_SLABS for slab rows, WP_CHUNK_STL for subtile
 *  rows. Normal maps: the whole [x1, x2] as one span, so a loop over the spans is the stock loop. Past max_spans
 *  spans, the rest of the row is one span (the loop body must still skip unloaded cells). */
#define MAP_ROW_SPANS_MAX 128
int map_row_spans(int32_t y, int32_t x1, int32_t x2, int32_t unit, int32_t *spans, int max_spans);

/** Scratch maps over the loaded slabs (paged mode), for searches that need a cell per slab. wp_cells_begin()
 *  numbers the loaded pages in row-major chunk order (by cy, then cx) and returns how many there are; a slab's cell
 *  is then its page's number * WP_CHUNK_SLAB_CELLS + its index in the page, or -1 when its chunk isn't loaded.
 *  The numbering stays the same until a page is created or freed, so nested searches can share it. */
int wp_cells_begin(void);
int32_t wp_slab_cell(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** The numbered pages; wp_cells_page(cell / WP_CHUNK_SLAB_CELLS) is a cell's page. */
struct WorldPage *wp_cells_page(int n);
MapSlabCoord wp_cell_slab_x(int32_t cell);
MapSlabCoord wp_cell_slab_y(int32_t cell);

/** Bounding box (engine slabs, inclusive) of the island of loaded chunks holding a slab: the chunks reachable from
 *  its chunk through loaded chunks, diagonals included. False when that chunk isn't loaded. */
TbBool wp_island_bounds(MapSlabCoord slb_x, MapSlabCoord slb_y, MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2,
    MapSlabCoord *y2);

/** Position of a loaded slab among all loaded slabs in row-major order (0 .. wp_cells_begin() * 256 - 1), or -1
 *  when its chunk isn't loaded; and back. */
int64_t wp_rowmajor_ordinal(MapSlabCoord slb_x, MapSlabCoord slb_y);
TbBool wp_rowmajor_slab(int64_t ord, MapSlabCoord *slb_x, MapSlabCoord *slb_y);

/** The loaded slabs in row-major order (as a loop over the bounding box that skips unloaded slabs):
 *  struct WpSlabIter it; wp_slab_iter_init(&it); while (wp_slab_iter_next(&it, &x, &y, &slb)) ... */
struct WpSlabIter { int g, e, k, y, x; };
void wp_slab_iter_init(struct WpSlabIter *it);
TbBool wp_slab_iter_next(struct WpSlabIter *it, MapSlabCoord *slb_x, MapSlabCoord *slb_y, struct SlabMap **slb);

#ifdef __cplusplus
}
#endif
#endif

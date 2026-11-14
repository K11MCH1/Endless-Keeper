/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pages.c
 *     Chunked endless world: the engine map held as pages, one per loaded chunk (see world_pages.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pages.h"

#include <stdlib.h>
#include <string.h>

#include "game_legacy.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
TbBool world_paged = false;

/* Two-level directory over the engine chunks: WP_DIR_TOP x WP_DIR_TOP blocks of WP_DIR_SUB x WP_DIR_SUB pages. */
#define WP_DIR_SUB_BITS 8
#define WP_DIR_SUB (1 << WP_DIR_SUB_BITS)
#define WP_DIR_TOP (WP_CHUNKS_PER_AXIS / WP_DIR_SUB)

static struct WorldPage **wp_dir[WP_DIR_TOP * WP_DIR_TOP];
static struct WorldPage *wp_list = NULL;
static int wp_loaded = 0;
/* wp_bounds of the loaded pages, kept until a page is created or freed (map-wide loops ask for it many times a turn) */
static TbBool wp_bounds_valid = false;
static TbBool wp_cells_valid = false; /* wp_cells (below) matches the loaded pages */
static int32_t wp_bounds_cx1, wp_bounds_cy1, wp_bounds_cx2, wp_bounds_cy2;
/* the last page found, for the next lookup */
static int32_t wp_last_cx = -1, wp_last_cy = -1;
static struct WorldPage *wp_last = NULL;

static struct WorldPage **wp_slot(int32_t cx, int32_t cy, TbBool create)
{
    if ((cx < 0) || (cy < 0) || (cx >= WP_CHUNKS_PER_AXIS) || (cy >= WP_CHUNKS_PER_AXIS))
        return NULL;
    struct WorldPage ***top = &wp_dir[(cy >> WP_DIR_SUB_BITS) * WP_DIR_TOP + (cx >> WP_DIR_SUB_BITS)];
    if (*top == NULL)
    {
        if (!create)
            return NULL;
        *top = (struct WorldPage **)calloc(WP_DIR_SUB * WP_DIR_SUB, sizeof(struct WorldPage *));
        if (*top == NULL)
            return NULL;
    }
    return &(*top)[(cy & (WP_DIR_SUB - 1)) * WP_DIR_SUB + (cx & (WP_DIR_SUB - 1))];
}

struct WorldPage *wp_page(int32_t cx, int32_t cy)
{
    if ((cx == wp_last_cx) && (cy == wp_last_cy))
        return wp_last;
    struct WorldPage **slot = wp_slot(cx, cy, false);
    struct WorldPage *pg = (slot != NULL) ? *slot : NULL;
    wp_last_cx = cx;
    wp_last_cy = cy;
    wp_last = pg;
    return pg;
}

struct WorldPage *wp_create(int32_t cx, int32_t cy)
{
    struct WorldPage **slot = wp_slot(cx, cy, true);
    if (slot == NULL)
        return NULL;
    if (*slot == NULL)
    {
        struct WorldPage *pg = (struct WorldPage *)calloc(1, sizeof(struct WorldPage));
        if (pg == NULL)
        {
            ERRORLOG("World pages: out of memory for chunk (%d,%d)", (int)cx, (int)cy);
            return NULL;
        }
        pg->cx = cx;
        pg->cy = cy;
        pg->next = wp_list;
        wp_list = pg;
        wp_loaded++;
        wp_bounds_valid = false;
        wp_cells_valid = false;
        *slot = pg;
        if ((cx == wp_last_cx) && (cy == wp_last_cy))
            wp_last = pg;
    }
    return *slot;
}

void wp_free(int32_t cx, int32_t cy)
{
    struct WorldPage **slot = wp_slot(cx, cy, false);
    if ((slot == NULL) || (*slot == NULL))
        return;
    struct WorldPage *pg = *slot;
    for (struct WorldPage **pp = &wp_list; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == pg)
        {
            *pp = pg->next;
            break;
        }
    }
    *slot = NULL;
    wp_loaded--;
    wp_bounds_valid = false;
    wp_cells_valid = false;
    if (wp_last == pg)
        wp_last = NULL;
    free(pg);
}

static void wp_free_all(void)
{
    while (wp_list != NULL)
        wp_free(wp_list->cx, wp_list->cy);
    for (int i = 0; i < WP_DIR_TOP * WP_DIR_TOP; i++)
    {
        free(wp_dir[i]);
        wp_dir[i] = NULL;
    }
    wp_last_cx = wp_last_cy = -1;
    wp_last = NULL;
}

struct WorldPage *wp_first(void)
{
    return wp_list;
}

int wp_count(void)
{
    return wp_loaded;
}

TbBool wp_bounds(MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2, MapSlabCoord *y2)
{
    if (wp_list == NULL)
        return false;
    if (!wp_bounds_valid)
    {
        int32_t bx1 = wp_list->cx, by1 = wp_list->cy, bx2 = bx1, by2 = by1;
        for (struct WorldPage *pg = wp_list->next; pg != NULL; pg = pg->next)
        {
            bx1 = min(bx1, pg->cx); by1 = min(by1, pg->cy);
            bx2 = max(bx2, pg->cx); by2 = max(by2, pg->cy);
        }
        wp_bounds_cx1 = bx1; wp_bounds_cy1 = by1; wp_bounds_cx2 = bx2; wp_bounds_cy2 = by2;
        wp_bounds_valid = true;
    }
    const int32_t cx1 = wp_bounds_cx1, cy1 = wp_bounds_cy1, cx2 = wp_bounds_cx2, cy2 = wp_bounds_cy2;
    *x1 = (MapSlabCoord)(cx1 * WP_CHUNK_SLABS);
    *y1 = (MapSlabCoord)(cy1 * WP_CHUNK_SLABS);
    *x2 = (MapSlabCoord)(cx2 * WP_CHUNK_SLABS + WP_CHUNK_SLABS - 1);
    *y2 = (MapSlabCoord)(cy2 * WP_CHUNK_SLABS + WP_CHUNK_SLABS - 1);
    return true;
}

struct WorldPage *wp_page_of_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if ((stl_x < 0) || (stl_y < 0))
        return NULL;
    return wp_page(stl_x / WP_CHUNK_STL, stl_y / WP_CHUNK_STL);
}

struct WorldPage *wp_page_of_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if ((slb_x < 0) || (slb_y < 0))
        return NULL;
    return wp_page(slb_x / WP_CHUNK_SLABS, slb_y / WP_CHUNK_SLABS);
}

void wp_set_paged(TbBool paged)
{
    wp_free_all();
    world_paged = paged;
    if (paged)
        set_map_size((MapSlabCoord)WP_PAGED_TILES, (MapSlabCoord)WP_PAGED_TILES);
}

void map_loop_slab_bounds(MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2, MapSlabCoord *y2)
{
    if (!world_paged)
    {
        *x1 = 0;
        *y1 = 0;
        *x2 = game.map_tiles_x - 1;
        *y2 = game.map_tiles_y - 1;
        return;
    }
    if (!wp_bounds(x1, y1, x2, y2))
    {
        *x1 = *y1 = 0;
        *x2 = *y2 = -1;
    }
}

/* wp_cells_begin: the loaded pages sorted by (cy, cx) */
static struct WorldPage **wp_cells = NULL;
static int32_t *wp_cells_row = NULL; /* per numbered page: the first page of its chunk row, and one past the last */
static int32_t *wp_cells_row_end = NULL;
static int wp_cells_count = 0, wp_cells_cap = 0;

static int wp_cells_cmp(const void *a, const void *b)
{
    const struct WorldPage *pa = *(const struct WorldPage *const *)a, *pb = *(const struct WorldPage *const *)b;
    if (pa->cy != pb->cy)
        return (pa->cy < pb->cy) ? -1 : 1;
    if (pa->cx != pb->cx)
        return (pa->cx < pb->cx) ? -1 : 1;
    return 0;
}

int wp_cells_begin(void)
{
    if (wp_cells_valid)
        return wp_cells_count;
    if (wp_loaded > wp_cells_cap)
    {
        int cap = wp_loaded + 64;
        struct WorldPage **arr = (struct WorldPage **)realloc(wp_cells, sizeof(struct WorldPage *) * cap);
        if (arr == NULL)
            return 0;
        wp_cells = arr;
        int32_t *rows = (int32_t *)realloc(wp_cells_row, sizeof(int32_t) * cap);
        if (rows == NULL)
            return 0;
        wp_cells_row = rows;
        int32_t *row_ends = (int32_t *)realloc(wp_cells_row_end, sizeof(int32_t) * cap);
        if (row_ends == NULL)
            return 0;
        wp_cells_row_end = row_ends;
        wp_cells_cap = cap;
    }
    int n = 0;
    for (struct WorldPage *pg = wp_list; (pg != NULL) && (n < wp_cells_cap); pg = pg->next)
        wp_cells[n++] = pg;
    qsort(wp_cells, (size_t)n, sizeof(struct WorldPage *), wp_cells_cmp);
    for (int i = 0; i < n; i++)
        wp_cells[i]->cell_page = i;
    for (int g = 0; g < n; )
    {
        int e = g + 1;
        while ((e < n) && (wp_cells[e]->cy == wp_cells[g]->cy))
            e++;
        for (int k = g; k < e; k++)
        {
            wp_cells_row[k] = g;
            wp_cells_row_end[k] = e;
        }
        g = e;
    }
    wp_cells_count = n;
    wp_cells_valid = true;
    return n;
}

/* Island floods: a mark for each flood, and the queue of pages it walks */
static uint32_t wp_mark = 0;
static struct WorldPage **wp_queue = NULL;
static int wp_queue_cap = 0;

static TbBool wp_queue_room(void)
{
    if (wp_loaded <= wp_queue_cap)
        return true;
    struct WorldPage **q = (struct WorldPage **)realloc(wp_queue, sizeof(struct WorldPage *) * (wp_loaded + 64));
    if (q == NULL)
        return false;
    wp_queue = q;
    wp_queue_cap = wp_loaded + 64;
    return true;
}

/** A mark no page carries yet. */
static uint32_t wp_new_mark(void)
{
    if (++wp_mark == 0)
    {
        for (struct WorldPage *pg = wp_list; pg != NULL; pg = pg->next)
            pg->island_mark = 0;
        wp_mark = 1;
    }
    return wp_mark;
}

/** Marks the island of loaded chunks that holds a page (the chunks reachable from it through loaded chunks, diagonals
 *  included) and gives its box in engine chunks. wp_queue_room() must have said yes. */
static void wp_island_flood(struct WorldPage *start, uint32_t mark, int32_t *bx1, int32_t *by1, int32_t *bx2, int32_t *by2)
{
    int head = 0, tail = 0;
    start->island_mark = mark;
    wp_queue[tail++] = start;
    int32_t cx1 = start->cx, cy1 = start->cy, cx2 = start->cx, cy2 = start->cy;
    while (head < tail)
    {
        const struct WorldPage *pg = wp_queue[head++];
        cx1 = min(cx1, pg->cx); cy1 = min(cy1, pg->cy);
        cx2 = max(cx2, pg->cx); cy2 = max(cy2, pg->cy);
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                struct WorldPage **slot = wp_slot(pg->cx + dx, pg->cy + dy, false);
                struct WorldPage *nb = (slot != NULL) ? *slot : NULL;
                if ((nb == NULL) || (nb->island_mark == mark) || (tail >= wp_queue_cap))
                    continue;
                nb->island_mark = mark;
                wp_queue[tail++] = nb;
            }
        }
    }
    *bx1 = cx1; *by1 = cy1; *bx2 = cx2; *by2 = cy2;
}

TbBool wp_island_bounds(MapSlabCoord slb_x, MapSlabCoord slb_y, MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2,
    MapSlabCoord *y2)
{
    if ((slb_x < 0) || (slb_y < 0))
        return false;
    struct WorldPage *start = wp_page(slb_x / WP_CHUNK_SLABS, slb_y / WP_CHUNK_SLABS);
    if (start == NULL)
        return false;
    if (!wp_queue_room())
        return wp_bounds(x1, y1, x2, y2); /* the whole box: bigger, still right */
    int32_t cx1, cy1, cx2, cy2;
    wp_island_flood(start, wp_new_mark(), &cx1, &cy1, &cx2, &cy2);
    *x1 = (MapSlabCoord)(cx1 * WP_CHUNK_SLABS);
    *y1 = (MapSlabCoord)(cy1 * WP_CHUNK_SLABS);
    *x2 = (MapSlabCoord)(cx2 * WP_CHUNK_SLABS + WP_CHUNK_SLABS - 1);
    *y2 = (MapSlabCoord)(cy2 * WP_CHUNK_SLABS + WP_CHUNK_SLABS - 1);
    return true;
}

static int wp_box_cmp(const void *a, const void *b)
{
    const struct WpBox *ba = (const struct WpBox *)a, *bb = (const struct WpBox *)b;
    if (ba->y1 != bb->y1)
        return (ba->y1 < bb->y1) ? -1 : 1;
    if (ba->x1 != bb->x1)
        return (ba->x1 < bb->x1) ? -1 : 1;
    return 0;
}

int wp_island_boxes(struct WpBox *boxes, int max)
{
    if ((wp_list == NULL) || (max <= 0))
        return 0;
    if (!wp_queue_room())
    {
        /* no memory for the flood: the one box round everything (bigger, still right) */
        wp_bounds(&boxes[0].x1, &boxes[0].y1, &boxes[0].x2, &boxes[0].y2);
        return 1;
    }
    const uint32_t mark = wp_new_mark();
    int n = 0;
    for (struct WorldPage *pg = wp_list; pg != NULL; pg = pg->next)
    {
        if (pg->island_mark == mark)
            continue;
        int32_t cx1, cy1, cx2, cy2;
        wp_island_flood(pg, mark, &cx1, &cy1, &cx2, &cy2);
        struct WpBox box;
        box.x1 = (MapSlabCoord)(cx1 * WP_CHUNK_SLABS);
        box.y1 = (MapSlabCoord)(cy1 * WP_CHUNK_SLABS);
        box.x2 = (MapSlabCoord)(cx2 * WP_CHUNK_SLABS + WP_CHUNK_SLABS - 1);
        box.y2 = (MapSlabCoord)(cy2 * WP_CHUNK_SLABS + WP_CHUNK_SLABS - 1);
        if (n < max)
        {
            boxes[n++] = box;
            continue;
        }
        /* more islands than boxes: the last box takes this one in */
        struct WpBox *last = &boxes[max - 1];
        last->x1 = min(last->x1, box.x1); last->y1 = min(last->y1, box.y1);
        last->x2 = max(last->x2, box.x2); last->y2 = max(last->y2, box.y2);
    }
    /* boxes that overlap become one, until none do: no slab is walked twice */
    for (TbBool joined = true; joined;)
    {
        joined = false;
        for (int a = 0; a < n; a++)
        {
            for (int b = a + 1; b < n; b++)
            {
                if ((boxes[a].x1 > boxes[b].x2) || (boxes[b].x1 > boxes[a].x2) || (boxes[a].y1 > boxes[b].y2) ||
                    (boxes[b].y1 > boxes[a].y2))
                    continue;
                boxes[a].x1 = min(boxes[a].x1, boxes[b].x1); boxes[a].y1 = min(boxes[a].y1, boxes[b].y1);
                boxes[a].x2 = max(boxes[a].x2, boxes[b].x2); boxes[a].y2 = max(boxes[a].y2, boxes[b].y2);
                boxes[b] = boxes[n - 1];
                n--;
                b--;
                joined = true;
            }
        }
    }
    /* (the pages' list is in the order they were made: the boxes come out top to bottom, whatever that was) */
    qsort(boxes, (size_t)n, sizeof(struct WpBox), wp_box_cmp);
    return n;
}

int64_t wp_rowmajor_ordinal(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const int32_t cell = wp_slab_cell(slb_x, slb_y);
    if (cell < 0)
        return -1;
    const int k = cell / WP_CHUNK_SLAB_CELLS, g = wp_cells_row[k], n = wp_cells_row_end[k] - g;
    return (int64_t)g * WP_CHUNK_SLAB_CELLS + (int64_t)(slb_y % WP_CHUNK_SLABS) * n * WP_CHUNK_SLABS
        + (int64_t)(k - g) * WP_CHUNK_SLABS + (slb_x % WP_CHUNK_SLABS);
}

TbBool wp_rowmajor_slab(int64_t ord, MapSlabCoord *slb_x, MapSlabCoord *slb_y)
{
    if ((ord < 0) || (ord >= (int64_t)wp_cells_count * WP_CHUNK_SLAB_CELLS))
        return false;
    const int g = wp_cells_row[ord / WP_CHUNK_SLAB_CELLS], n = wp_cells_row_end[g] - g;
    const int64_t local = ord - (int64_t)g * WP_CHUNK_SLAB_CELLS;
    const int y = (int)(local / (n * WP_CHUNK_SLABS)), rem = (int)(local % (n * WP_CHUNK_SLABS));
    const struct WorldPage *pg = wp_cells[g + rem / WP_CHUNK_SLABS];
    *slb_x = pg->cx * WP_CHUNK_SLABS + rem % WP_CHUNK_SLABS;
    *slb_y = pg->cy * WP_CHUNK_SLABS + y;
    return true;
}

/* the iterator's chunk row: pages g..e-1 share a cy */
static void wp_slab_iter_row(struct WpSlabIter *it)
{
    it->e = it->g;
    if (it->g >= wp_cells_count)
        return;
    it->e = it->g + 1;
    while ((it->e < wp_cells_count) && (wp_cells[it->e]->cy == wp_cells[it->g]->cy))
        it->e++;
    it->k = it->g;
    it->y = 0;
    it->x = 0;
}

void wp_slab_iter_init(struct WpSlabIter *it)
{
    if (!world_paged)
        wp_cells_count = 0;
    else
        wp_cells_begin();
    it->g = 0;
    wp_slab_iter_row(it);
}

TbBool wp_slab_iter_next(struct WpSlabIter *it, MapSlabCoord *slb_x, MapSlabCoord *slb_y, struct SlabMap **slb)
{
    if (it->g >= wp_cells_count)
        return false;
    struct WorldPage *pg = wp_cells[it->k];
    *slb_x = pg->cx * WP_CHUNK_SLABS + it->x;
    *slb_y = pg->cy * WP_CHUNK_SLABS + it->y;
    *slb = &pg->slab[it->y * WP_CHUNK_SLABS + it->x];
    if (++it->x == WP_CHUNK_SLABS)
    {
        it->x = 0;
        if (++it->k == it->e)
        {
            it->k = it->g;
            if (++it->y == WP_CHUNK_SLABS)
            {
                it->g = it->e;
                wp_slab_iter_row(it);
            }
        }
    }
    return true;
}

void wp_stl_iter_init(struct WpStlIter *it)
{
    struct WpSlabIter rows;
    wp_slab_iter_init(&rows);
    it->g = rows.g;
    it->e = rows.e;
    it->k = rows.k;
    it->y = 0;
    it->x = 0;
}

TbBool wp_stl_iter_next(struct WpStlIter *it, MapSubtlCoord *stl_x, MapSubtlCoord *stl_y, struct Map **mapblk)
{
    if (it->g >= wp_cells_count)
        return false;
    struct WorldPage *pg = wp_cells[it->k];
    *stl_x = pg->cx * WP_CHUNK_STL + it->x;
    *stl_y = pg->cy * WP_CHUNK_STL + it->y;
    *mapblk = &pg->map[it->y * WP_CHUNK_STL + it->x];
    if (++it->x == WP_CHUNK_STL)
    {
        it->x = 0;
        if (++it->k == it->e)
        {
            it->k = it->g;
            if (++it->y == WP_CHUNK_STL)
            {
                /* the next row of chunks: the pages that share the next cy */
                struct WpSlabIter rows;
                rows.g = it->e;
                wp_slab_iter_row(&rows);
                it->g = rows.g;
                it->e = rows.e;
                it->k = rows.k;
                it->y = 0;
            }
        }
    }
    return true;
}

int32_t wp_slab_cell(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if ((slb_x < 0) || (slb_y < 0))
        return -1;
    struct WorldPage *pg = wp_page(slb_x / WP_CHUNK_SLABS, slb_y / WP_CHUNK_SLABS);
    if ((pg == NULL) || (pg->cell_page < 0) || (pg->cell_page >= wp_cells_count) || (wp_cells[pg->cell_page] != pg))
        return -1;
    return pg->cell_page * WP_CHUNK_SLAB_CELLS + wp_slb_index(slb_x, slb_y);
}

struct WorldPage *wp_cells_page(int n)
{
    return ((n >= 0) && (n < wp_cells_count)) ? wp_cells[n] : NULL;
}

MapSlabCoord wp_cell_slab_x(int32_t cell)
{
    const struct WorldPage *pg = wp_cells_page(cell / WP_CHUNK_SLAB_CELLS);
    return (pg == NULL) ? -1 : pg->cx * WP_CHUNK_SLABS + (cell % WP_CHUNK_SLABS);
}

MapSlabCoord wp_cell_slab_y(int32_t cell)
{
    const struct WorldPage *pg = wp_cells_page(cell / WP_CHUNK_SLAB_CELLS);
    return (pg == NULL) ? -1 : pg->cy * WP_CHUNK_SLABS + ((cell % WP_CHUNK_SLAB_CELLS) / WP_CHUNK_SLABS);
}

int map_row_spans(int32_t y, int32_t x1, int32_t x2, int32_t unit, int32_t *spans, int max_spans)
{
    if (x2 < x1)
        return 0;
    if (!world_paged)
    {
        spans[0] = x1;
        spans[1] = x2;
        return 1;
    }
    if ((y < 0) || (x2 < 0) || (max_spans <= 0))
        return 0;
    if (x1 < 0)
        x1 = 0;
    const int32_t cy = y / unit;
    int n = 0;
    for (int32_t cx = x1 / unit; cx <= x2 / unit; cx++)
    {
        struct WorldPage **slot = wp_slot(cx, cy, false);
        if ((slot == NULL) || (*slot == NULL))
            continue;
        const int32_t a = max(cx * unit, x1), b = min(cx * unit + unit - 1, x2);
        if ((n > 0) && (spans[2 * n - 1] + 1 == a))
        {
            spans[2 * n - 1] = b;
            continue;
        }
        if (n >= max_spans)
        {
            spans[2 * n - 1] = x2;
            return n;
        }
        spans[2 * n] = a;
        spans[2 * n + 1] = b;
        n++;
    }
    return n;
}

void map_loop_subtile_bounds(MapSubtlCoord *x1, MapSubtlCoord *y1, MapSubtlCoord *x2, MapSubtlCoord *y2)
{
    if (!world_paged)
    {
        *x1 = 0;
        *y1 = 0;
        *x2 = game.map_subtiles_x;
        *y2 = game.map_subtiles_y;
        return;
    }
    MapSlabCoord sx1, sy1, sx2, sy2;
    if (!wp_bounds(&sx1, &sy1, &sx2, &sy2))
    {
        *x1 = *y1 = 0;
        *x2 = *y2 = -1;
        return;
    }
    *x1 = sx1 * STL_PER_SLB;
    *y1 = sy1 * STL_PER_SLB;
    *x2 = sx2 * STL_PER_SLB + STL_PER_SLB - 1;
    *y2 = sy2 * STL_PER_SLB + STL_PER_SLB - 1;
}
/******************************************************************************/
#ifdef __cplusplus
}
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file player_complookup.c
 *     Computer player lookups definitions.
 * @par Purpose:
 *     Implements lookups used for computer player tasks.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     10 Mar 2009 - 20 Nov 2012
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "player_complookup.h"

#include <limits.h>
#include <string.h>

#include "globals.h"
#include "bflib_basics.h"

#include "map_data.h"
#include "slab_data.h"
#include "game_legacy.h"
#include "front_simple.h"
#include "config_terrain.h"
#include "world_pages.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/******************************************************************************/
#ifdef __cplusplus
}
#endif
/******************************************************************************/
struct GoldLookup *get_gold_lookup(long idx)
{
    return &game.gold_lookup[idx];
}

long gold_lookup_index(const struct GoldLookup *gldlook)
{
    long i = ((char*)gldlook - (char*)&game.gold_lookup[0]);
    if ( (i < 0) || (i >= GOLD_LOOKUP_COUNT*sizeof(struct GoldLookup)) )
        return 0;
    return i / sizeof(struct GoldLookup);
}

/** Finds a gold vein with smaller amount of gold and gem slabs than given values.
 *  Gems slabs count has higher priority than gold slabs count.
 *
 * @param higher_gold_slabs
 * @param higher_gem_slabs
 * @return
 */
long smaller_gold_vein_lookup_idx(long higher_gold_slabs, long higher_gem_slabs)
{
    long gold_slabs = higher_gold_slabs;
    long gem_slabs = higher_gem_slabs;
    long gold_idx = -1;
    for (long i = 0; i < GOLD_LOOKUP_COUNT; i++)
    {
        struct GoldLookup* gldlook = get_gold_lookup(i);
        if (gldlook->num_gem_slabs == gem_slabs)
        {
            if (gldlook->num_gold_slabs < gold_slabs)
            {
              gold_slabs = gldlook->num_gold_slabs;
              gold_idx = i;
            }
        } else
        if (gldlook->num_gem_slabs < gem_slabs)
        {
            gem_slabs = gldlook->num_gem_slabs;
            gold_slabs = gldlook->num_gold_slabs;
            gold_idx = i;
        }
    }
    return gold_idx;
}

/* Paged mode (world_pages.h): the virtual map is far too big for a treasure map indexed by slab number, so the
 * treasure map has a cell per loaded slab (wp_cells_begin, wp_slab_cell) and the vein list holds cells; every slab of
 * a chunk that isn't loaded shares one extra cell at index treasure_cells, marked as already done. */
static SlabCodedCoords treasure_cells = 0;

/** Index of a slab in the treasure map: the slab number normally, its cell in paged mode. */
static SlabCodedCoords treasure_map_index(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!world_paged)
        return get_slab_number(slb_x, slb_y);
    const int32_t cell = wp_slab_cell(slb_x, slb_y);
    return (cell < 0) ? treasure_cells : (SlabCodedCoords)cell;
}

/** Slab coordinates of a treasure map index (as slb_num_decode_x/y normally). */
static MapSlabCoord treasure_map_decode_x(SlabCodedCoords tmap_idx)
{
    if (!world_paged)
        return slb_num_decode_x(tmap_idx);
    return wp_cell_slab_x((int32_t)tmap_idx);
}

static MapSlabCoord treasure_map_decode_y(SlabCodedCoords tmap_idx)
{
    if (!world_paged)
        return slb_num_decode_y(tmap_idx);
    return wp_cell_slab_y((int32_t)tmap_idx);
}

void check_treasure_map(unsigned char *treasure_map, SlabCodedCoords *vein_list, int32_t *gold_next_idx, MapSlabCoord veinslb_x, MapSlabCoord veinslb_y)
{
    long gold_idx;
    // First, find a vein
    long vein_total = 0;
    MapSlabCoord slb_x = veinslb_x;
    MapSlabCoord slb_y = veinslb_y;
    int64_t accumulated_x_coordinate = 0; // 64 bits: chunked-world slab coordinates are about 2^20
    int64_t accumulated_y_coordinate = 0;
    long coordinate_sample_count = 0;
    long gem_slabs = 0;
    long gold_slabs = 0;
    SlabCodedCoords slb_num = treasure_map_index(slb_x, slb_y);
    treasure_map[slb_num] |= 0x02;
    for (long vein_idx = 0; vein_idx <= vein_total; vein_idx++)
    {
        accumulated_x_coordinate += slb_x;
        accumulated_y_coordinate += slb_y;
        coordinate_sample_count++;
        SlabCodedCoords slb_around = treasure_map_index(slb_x, slb_y);
        struct SlabMap* slb = world_paged ? get_slabmap_block(slb_x, slb_y) : get_slabmap_direct(slb_around);
        if (slb->kind == SlbT_GEMS)
        {
            gem_slabs++;
        } else
        {
            if (slb->kind != SlbT_GOLD)
            {
                struct SlabConfigStats* slabst = get_slab_kind_stats(slb->kind);
                GoldAmount gold_per_dense_block = slabst->gold_held;
                slabst = get_slab_kind_stats(SlbT_GOLD);
                GoldAmount gold_per_block = slabst->gold_held;
                if (gold_per_block != 0)
                {
                    gold_slabs = gold_slabs + (gold_per_dense_block / gold_per_block);
                }
                else
                {
                    ERRORLOG("Gold slabs hold no gold");
                    gold_slabs++;
                }
            }
            else
            {
                gold_slabs++;
            }
            slb_around = treasure_map_index(slb_x-1, slb_y);
            if ((treasure_map[slb_around] & 0x03) == 0)
            {
                treasure_map[slb_around] |= 0x02;
                vein_list[vein_total] = slb_around;
                vein_total++;
            }
            slb_around = treasure_map_index(slb_x+1, slb_y);
            if ((treasure_map[slb_around] & 0x03) == 0)
            {
                treasure_map[slb_around] |= 0x02;
                vein_list[vein_total] = slb_around;
                vein_total++;
            }
            slb_around = treasure_map_index(slb_x, slb_y-1);
            if ((treasure_map[slb_around] & 0x03) == 0)
            {
                treasure_map[slb_around] |= 0x02;
                vein_list[vein_total] = slb_around;
                vein_total++;
            }
            slb_around = treasure_map_index(slb_x, slb_y+1);
            if ((treasure_map[slb_around] & 0x03) == 0)
            {
                treasure_map[slb_around] |= 0x02;
                vein_list[vein_total] = slb_around;
                vein_total++;
            }
        }
        // Move to next slab in list
        slb_x = treasure_map_decode_x(vein_list[vein_idx]);
        slb_y = treasure_map_decode_y(vein_list[vein_idx]);
    }
    // Now get a GoldLookup struct to put the vein into
    if (*gold_next_idx < GOLD_LOOKUP_COUNT)
    {
        gold_idx = *gold_next_idx;
        (*gold_next_idx)++;
    } else
    {
        gold_idx = smaller_gold_vein_lookup_idx(gold_slabs, gem_slabs);
    }
    // Write the vein to GoldLookup item
    if (gold_idx != -1)
    {
        struct GoldLookup* gldlook = get_gold_lookup(gold_idx);
        memset(gldlook, 0, sizeof(struct GoldLookup));
        gldlook->flags |= 0x01;

        MapSlabCoord centerslb_x = accumulated_x_coordinate / coordinate_sample_count;
        MapSlabCoord centerslb_y = accumulated_y_coordinate / coordinate_sample_count;
        SlabCodedCoords center_slb = treasure_map_index(centerslb_x, centerslb_y);

        MapSubtlCoord stl_x = slab_subtile_center(centerslb_x);
        MapSubtlCoord stl_y = slab_subtile_center(centerslb_y);
        if (!(treasure_map[center_slb] & 0x02)) //Center slab is not in treasure map. Can happen on donut shaped gold veins.
        {
            // Find nearest vein slab
            long best_dist = LONG_MAX;

            // vein_list holds vein_total entries (the first slab isn't in it);
            // entries past those are left over from an earlier vein.
            for (long i = vein_total - 1; i >= 0; i--)
            {
                MapSlabCoord vein_x = treasure_map_decode_x(vein_list[i]);
                MapSlabCoord vein_y = treasure_map_decode_y(vein_list[i]);

                long dx = vein_x - centerslb_x;
                long dy = vein_y - centerslb_y;

                long dist = dx * dx + dy * dy;

                if (dist < best_dist)
                {
                    best_dist = dist;
                    stl_x = slab_subtile_center(vein_x);
                    stl_y = slab_subtile_center(vein_y);
                }
            }
        }

        gldlook->stl_x = stl_x;
        gldlook->stl_y = stl_y;
        gldlook->num_gold_slabs = gold_slabs;
        gldlook->num_gem_slabs = gem_slabs;
        SYNCDBG(8,"Added vein %d at (%d,%d)",(int)gold_idx,(int)gldlook->stl_x,(int)gldlook->stl_y);
    }
}

/**
 * Scans map for gold veins, and fills up gold_lookup array with veins found.
 */
void check_map_for_gold(void)
{
    MapSlabCoord slb_x;
    MapSlabCoord slb_y;
    SlabCodedCoords slb_num;
    SYNCDBG(8,"Starting");
    for (long i = 0; i < GOLD_LOOKUP_COUNT; i++)
    {
        memset(&game.gold_lookup[i], 0, sizeof(struct GoldLookup));
    }
    // Make a map with treasure areas marked
    unsigned char* treasure_map = (unsigned char*)big_scratch;
    if (world_paged)
    {
        // Paged mode: the treasure map has a cell per loaded slab, plus the outside cell
        const int npages = wp_cells_begin();
        if (npages > 0)
        {
            size_t cells = (size_t)npages * WP_CHUNK_SLAB_CELLS;
            size_t list_offs = (cells + 1 + 7) & ~(size_t)7;
            if (list_offs + cells * sizeof(SlabCodedCoords) <= 16u * 1024 * 1024)
            {
                treasure_cells = (SlabCodedCoords)cells;
                SlabCodedCoords* paged_vein_list = (SlabCodedCoords*)&big_scratch[list_offs];
                for (int n = 0; n < npages; n++)
                {
                    struct WorldPage *pg = wp_cells_page(n);
                    for (int i = 0; i < WP_CHUNK_SLAB_CELLS; i++)
                    {
                        slb_num = (SlabCodedCoords)n * WP_CHUNK_SLAB_CELLS + i;
                        treasure_map[slb_num] = 0;
                        const struct SlabConfigStats* slabst = get_slab_stats(&pg->slab[i]);
                        // Mark areas which are not valuable
                        if ((slabst->block_flags & (SlbAtFlg_Valuable)) == 0) {
                            treasure_map[slb_num] |= 0x01;
                        }
                    }
                }
                treasure_map[cells] = 0x03;
                /* the loaded slabs in row-major order (pages sorted by chunk row, then column), as the stock loop */
                int32_t paged_gold_next_idx = 0;
                for (int g = 0; g < npages; )
                {
                    int e = g + 1;
                    while ((e < npages) && (wp_cells_page(e)->cy == wp_cells_page(g)->cy))
                        e++;
                    for (int y = 0; y < WP_CHUNK_SLABS; y++)
                    {
                        for (int k = g; k < e; k++)
                        {
                            struct WorldPage *pg = wp_cells_page(k);
                            for (int x = 0; x < WP_CHUNK_SLABS; x++)
                            {
                                slb_num = (SlabCodedCoords)k * WP_CHUNK_SLAB_CELLS + y * WP_CHUNK_SLABS + x;
                                if ( ((treasure_map[slb_num] & 0x01) == 0) && ((treasure_map[slb_num] & 0x02) == 0) )
                                {
                                    check_treasure_map(treasure_map, paged_vein_list, &paged_gold_next_idx,
                                        pg->cx * WP_CHUNK_SLABS + x, pg->cy * WP_CHUNK_SLABS + y);
                                }
                            }
                        }
                    }
                    g = e;
                }
                SYNCDBG(8,"Found %d possible digging locations",paged_gold_next_idx);
            } else
            {
                WARNLOG("%d loaded chunks are too many for the gold search", npages);
            }
        }
        game.turn_last_checked_for_gold = get_gameturn();
        return;
    }
    SlabCodedCoords* vein_list = (SlabCodedCoords*)&big_scratch[game.map_tiles_x * game.map_tiles_y];
    for (slb_y = 0; slb_y < game.map_tiles_y; slb_y++)
    {
        for (slb_x = 0; slb_x < game.map_tiles_x; slb_x++)
        {
            slb_num = get_slab_number(slb_x, slb_y);
            struct SlabMap* slb = get_slabmap_direct(slb_num);
            treasure_map[slb_num] = 0;
            const struct SlabConfigStats* slabst = get_slab_stats(slb);
            // Mark areas which are not valuable
            if ((slabst->block_flags & (SlbAtFlg_Valuable)) == 0) {
                treasure_map[slb_num] |= 0x01;
            }
        }
    }
    // Add treasures to lookup as gold veins
    int32_t gold_next_idx = 0;
    for (slb_y = 0; slb_y < game.map_tiles_y; slb_y++)
    {
        for (slb_x = 0; slb_x < game.map_tiles_x; slb_x++)
        {
            slb_num = get_slab_number(slb_x, slb_y);
            if ( ((treasure_map[slb_num] & 0x01) == 0) && ((treasure_map[slb_num] & 0x02) == 0) )
            {
                check_treasure_map(treasure_map, vein_list, &gold_next_idx, slb_x, slb_y);
            }
        }
    }
    SYNCDBG(8,"Found %d possible digging locations",gold_next_idx);
    game.turn_last_checked_for_gold = get_gameturn();
}
/******************************************************************************/

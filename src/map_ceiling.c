/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_ceiling.c
 *     Map ceiling support functions.
 * @par Purpose:
 *     Functions to calculate the ceiling.
 * @par Comment:
 *     None.
 * @author   keeperFx Team
 * @date     12 Nov 2022
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/

#include "pre_inc.h"
#include "globals.h"
#include "bflib_planar.h"
#include "map_columns.h"
#include "front_simple.h"
#include "globals.h"
#include "game_legacy.h"
#include "world_pages.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C"
{
#endif

static char ceiling_cache[MAX_SUBTILES_X*MAX_SUBTILES_Y];

/** A subtile's cache cell: ceiling_cache, or its chunk page in paged mode (a chunk that isn't loaded gives a
 *  scratch cell holding -1, no ceiling). */
static char *ceiling_cache_cell(SubtlCodedCoords stl_num)
{
    if (world_paged)
    {
        static char unloaded_cell;
        MapSubtlCoord x = stl_num_decode_x(stl_num), y = stl_num_decode_y(stl_num);
        struct WorldPage *pg = wp_page_of_subtile(x, y);
        if (pg != NULL)
            return (char *)&pg->ceiling[wp_stl_index(x, y)];
        unloaded_cell = -1;
        return &unloaded_cell;
    }
    return &ceiling_cache[stl_num];
}

static int find_column_height_including_lintels(struct Column *col)
{
    unsigned char i;
    if (!col->solidmask)
        return 0;
    for (i = COLUMN_STACK_HEIGHT - 1; !col->cubes[i]; i--)
        ;
    return i + 1;
}

static int ceiling_block_is_solid_including_corners_return_height(SubtlCodedCoords stl_num, MapSubtlCoord cstl_x, MapSubtlCoord cstl_y)
{
    MapSubtlCoord stl_x = stl_num_decode_x(stl_num);
    MapSubtlCoord stl_y = stl_num_decode_y(stl_num);
    struct Map *mapblk = get_map_block_at(stl_x,stl_y);
    struct Column *col = get_map_column(mapblk);

    if ((mapblk->flags & SlbAtFlg_Blocking) == 0 && (col->bitfields & CLF_CEILING_MASK) == 0)
    {
        if (cstl_x <= 0)
        {
            if (cstl_y > 0)
            {
                mapblk = get_map_block_at(stl_x,stl_y - 1);
                col = get_map_column(mapblk);
                if ((mapblk->flags & SlbAtFlg_Blocking) != 0 || (col->bitfields & CLF_CEILING_MASK) != 0)
                {
                    if ((col->bitfields & CLF_CEILING_MASK) != 0)
                        return find_column_height_including_lintels(col);
                    return col->bitfields >> 4;
                }
            }
        }
        else
        {
            mapblk = get_map_block_at(stl_x - 1,stl_y);
            col = get_map_column(mapblk);
            if ((mapblk->flags & SlbAtFlg_Blocking) != 0 || (col->bitfields & CLF_CEILING_MASK) != 0)
            {
                if ((col->bitfields & CLF_CEILING_MASK) != 0)
                    return find_column_height_including_lintels(col);
                return col->bitfields >> 4;
            }
            if (cstl_y > 0)
            {
                mapblk = get_map_block_at(stl_x,stl_y - 1);
                col = get_map_column(mapblk);
                if ((mapblk->flags & SlbAtFlg_Blocking) != 0 || (col->bitfields & CLF_CEILING_MASK) != 0)
                {
                    if ((col->bitfields & CLF_CEILING_MASK) != 0)
                        return find_column_height_including_lintels(col);
                    return col->bitfields >> 4;
                }
                mapblk = get_map_block_at(stl_x - 1,stl_y - 1);
                col = get_map_column(mapblk);
                if ((mapblk->flags & SlbAtFlg_Blocking) != 0 || (col->bitfields & CLF_CEILING_MASK) != 0)
                {
                    if ((col->bitfields & CLF_CEILING_MASK) != 0)
                        return find_column_height_including_lintels(col);
                    return col->bitfields >> 4;
                }
            }
        }
        return -1;
    }

    mapblk = get_map_block_at(stl_x,stl_y);
    col = get_map_column(mapblk);
    if ((col->bitfields & CLF_CEILING_MASK) != 0)
        return find_column_height_including_lintels(col);
    return col->bitfields >> 4;
}

static int ceiling_calculate_height_from_nearest_walls(int result, int number_of_steps)
{
    int step_adjustment;
    step_adjustment = game.ceiling_step * number_of_steps;
    if (result >= game.ceiling_height_max)
    {
        if (result > game.ceiling_height_max)
        {
            result -= step_adjustment;
            if (result <= game.ceiling_height_min)
                return game.ceiling_height_min;
        }
    }
    else
    {
        result += step_adjustment;
        if (result >= game.ceiling_height_max)
            return game.ceiling_height_max;
    }
    return result;
}

void ceiling_partially_recompute_heights(MapSubtlCoord sx, MapSubtlCoord sy, MapSubtlCoord ex, MapSubtlCoord ey)
{
    int ceiling_height;
    int spiral_step_count;
    struct MapOffset *spir;
    MapSubtlCoord search_stl_x;
    MapSubtlCoord search_stl_y;
    int found_ceiling_height;
    TbBool near_wall;
    unsigned int number_of_steps;
    int current_ceiling_height;
    int *ceiling_height_result_ptr;
    int ceil_dist = game.ceiling_dist;
    if (game.ceiling_dist > 4)
        ceil_dist = 4;
    MapSubtlCoord computation_start_stl_x = sx - ceil_dist;
    if (computation_start_stl_x <= 0)
        computation_start_stl_x = 0;
    MapSubtlCoord computation_start_stl_y = sy - ceil_dist;
    if (sy - ceil_dist <= 0)
        computation_start_stl_y = 0;

    MapSubtlCoord computation_end_stl_x = ex + ceil_dist;
    if (ex + ceil_dist >= (game.map_subtiles_x + 1))
        computation_end_stl_x = (game.map_subtiles_x + 1);
    MapSubtlCoord computation_end_stl_y = ey + ceil_dist;
    if (computation_end_stl_y >= (game.map_subtiles_y + 1))
        computation_end_stl_y = (game.map_subtiles_y + 1);

    //ceiling_cache = (signed char*)scratch;

    MapSubtlCoord solid_check_start_stl_x = computation_start_stl_x - game.ceiling_dist;
    if (solid_check_start_stl_x <= 0)
        solid_check_start_stl_x = 0;

    MapSubtlCoord solid_check_start_stl_y = computation_start_stl_y - game.ceiling_dist;
    if (solid_check_start_stl_y <= 0)
        solid_check_start_stl_y = 0;

    MapSubtlCoord solid_check_end_stl_x = computation_end_stl_x + game.ceiling_dist;
    if (solid_check_end_stl_x >= (game.map_subtiles_x + 1))
        solid_check_end_stl_x = (game.map_subtiles_x + 1);
    MapSubtlCoord solid_check_end_stl_y = computation_end_stl_y + game.ceiling_dist;
    if (solid_check_end_stl_y >= (game.map_subtiles_y + 1))
        solid_check_end_stl_y = (game.map_subtiles_y + 1);

    if (world_paged && (solid_check_end_stl_x > solid_check_start_stl_x) && (solid_check_end_stl_y > solid_check_start_stl_y))
    {
        /* the paged world: the same computation over a local copy of the solid heights. The spiral reaches at most
           ceiling_dist subtiles, so it never leaves the solid-check box; looking each step up in the chunk pages
           cost most of a chunk load. */
        const MapSubtlCoord bw = solid_check_end_stl_x - solid_check_start_stl_x;
        const MapSubtlCoord bh = solid_check_end_stl_y - solid_check_start_stl_y;
        signed char *box = (signed char *)malloc((size_t)bw * (size_t)bh);
        if (box != NULL)
        {
            for (MapSubtlCoord y = 0; y < bh; y++)
                for (MapSubtlCoord x = 0; x < bw; x++)
                {
                    MapSubtlCoord bx = solid_check_start_stl_x + x, by = solid_check_start_stl_y + y;
                    /* a chunk that isn't loaded reads -1 (no ceiling), as its cache cell does */
                    box[y * bw + x] = (wp_page_of_subtile(bx, by) == NULL) ? (signed char)-1 :
                        (signed char)ceiling_block_is_solid_including_corners_return_height(get_subtile_number(bx, by), bx, by);
                }
            for (MapSubtlCoord cy = computation_start_stl_y; cy < computation_end_stl_y; cy++)
            {
                for (MapSubtlCoord cx = computation_start_stl_x; cx < computation_end_stl_x; cx++)
                {
                    int height = box[(cy - solid_check_start_stl_y) * bw + (cx - solid_check_start_stl_x)];
                    if (height <= -1)
                    {
                        int found = -1;
                        unsigned int steps = 0;
                        TbBool is_near = false;
                        for (int k = 0; k < game.ceiling_search_dist; k++)
                        {
                            MapSubtlCoord sx2 = cx + spiral_step[k].h, sy2 = cy + spiral_step[k].v;
                            if ((sx2 < 0) || (sx2 >= game.map_subtiles_x) || (sy2 < 0) || (sy2 >= game.map_subtiles_y))
                                continue;
                            if ((sx2 >= solid_check_start_stl_x) && (sx2 < solid_check_end_stl_x) &&
                                (sy2 >= solid_check_start_stl_y) && (sy2 < solid_check_end_stl_y))
                                found = box[(sy2 - solid_check_start_stl_y) * bw + (sx2 - solid_check_start_stl_x)];
                            else
                                found = *ceiling_cache_cell(get_subtile_number(sx2, sy2));
                            if (found > -1)
                            {
                                steps = chessboard_distance(cx, cy, sx2, sy2);
                                is_near = true;
                                break;
                            }
                        }
                        height = is_near ? ceiling_calculate_height_from_nearest_walls(found, steps) : game.ceiling_height_max;
                    }
                    get_map_block_at_pos(get_subtile_number(cx, cy))->filled_subtiles = height;
                }
            }
            free(box);
            return;
        }
    }

    MapSubtlCoord cstl_y = solid_check_start_stl_y;
    while (cstl_y < solid_check_end_stl_y)
    {
        MapSubtlCoord cstl_x = solid_check_start_stl_x;
        while (cstl_x < solid_check_end_stl_x)
        {
            SubtlCodedCoords stl_num = get_subtile_number(cstl_x,cstl_y);
            *ceiling_cache_cell(stl_num) = ceiling_block_is_solid_including_corners_return_height(stl_num,cstl_x,cstl_y);
            cstl_x++;
        }
        cstl_y++;
    }

    MapSubtlCoord current_stl_y = computation_start_stl_y;
    while (current_stl_y < computation_end_stl_y)
    {
        MapSubtlCoord current_stl_x = computation_start_stl_x;
        while (computation_end_stl_x > current_stl_x)
        {
            SubtlCodedCoords stl_num2 = get_subtile_number(current_stl_x,current_stl_y);
            ceiling_height = *ceiling_cache_cell(stl_num2);
            current_ceiling_height = ceiling_height;
            if (ceiling_height <= -1)
            {
                ceiling_height_result_ptr = &current_ceiling_height;
                spiral_step_count = 0;
                spir = spiral_step;
                if (game.ceiling_search_dist > 0)
                {
                    while (1)
                    {
                        search_stl_x = current_stl_x + spir->h;
                        search_stl_y = current_stl_y + spir->v;
                        if (search_stl_x >= 0 && search_stl_x < game.map_subtiles_x && search_stl_y >= 0 && search_stl_y < game.map_subtiles_y)
                        {
                            found_ceiling_height = *ceiling_cache_cell(get_subtile_number(search_stl_x ,search_stl_y));
                            if (found_ceiling_height > -1)
                                break;
                        }
                        ++spiral_step_count;
                        ++spir;
                        if (spiral_step_count >= game.ceiling_search_dist)
                            goto search_distance_exceeded;
                    }
                    *ceiling_height_result_ptr = found_ceiling_height;
                    number_of_steps = chessboard_distance(current_stl_x, current_stl_y, search_stl_x, search_stl_y);
                    near_wall = true;
                }
                else
                {
                search_distance_exceeded:
                    near_wall = false;
                }
                if (near_wall)
                    ceiling_height = ceiling_calculate_height_from_nearest_walls(current_ceiling_height, number_of_steps);
                else
                    ceiling_height = game.ceiling_height_max;
            }

            struct Map* mapblk = get_map_block_at_pos(stl_num2);
            mapblk->filled_subtiles = ceiling_height;
            current_stl_x++;
        }
        current_stl_y ++;
    }
}

static long get_ceiling_filled_subtiles_from_cubes(const struct Column *col)
{
    if (col->solidmask == 0) {
        return 0;
    }
    int i;
    for (i = COLUMN_STACK_HEIGHT-1; i >= 0; i--)
    {
        if (col->cubes[i] != 0)
            break;
    }
    return i + 1;
}

static int get_ceiling_or_floor_filled_subtiles(SubtlCodedCoords stl_num)
{
    const struct Map *mapblk;
    mapblk = get_map_block_at_pos(stl_num);
    const struct Column *col;
    col = get_map_column(mapblk);
    if (get_map_ceiling_filled_subtiles(mapblk) > 0) {
        return get_ceiling_filled_subtiles_from_cubes(col);
    } else {
        return get_map_floor_filled_subtiles(mapblk);
    }
}

/** One subtile of ceiling_init. */
static void ceiling_init_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
        {
            int filled_h;
            if (map_pos_solid_at_ceiling(stl_x, stl_y))
            {
                filled_h = get_ceiling_or_floor_filled_subtiles(get_subtile_number(stl_x,stl_y));
            } else
            if (stl_x > 0 && map_pos_solid_at_ceiling(stl_x-1, stl_y))
            {
                filled_h = get_ceiling_or_floor_filled_subtiles(get_subtile_number(stl_x-1,stl_y));
            } else
            if (stl_y > 0 && map_pos_solid_at_ceiling(stl_x, stl_y-1))
            {
                filled_h = get_ceiling_or_floor_filled_subtiles(get_subtile_number(stl_x,stl_y-1));
            } else
            if (stl_x > 0 && stl_y > 0 && map_pos_solid_at_ceiling(stl_x-1, stl_y-1)) {
                filled_h = get_ceiling_or_floor_filled_subtiles(get_subtile_number(stl_x-1,stl_y-1));
            } else {
                filled_h = -1;
            }

            if (filled_h <= -1)
            {
              if (game.ceiling_search_dist <= 0)
              {
                  filled_h = game.ceiling_height_max;
              }
              else
              {
                int i = 0;
                while ( 1 )
                {
                    struct MapOffset *sstep = &spiral_step[i];
                    MapSubtlCoord cstl_x = stl_x + sstep->h;
                    MapSubtlCoord cstl_y = stl_y + sstep->v;
                    if ((cstl_x >= 0) && (cstl_x <= game.map_subtiles_x))
                    {
                        if ((cstl_y >= 0) && (cstl_y <= game.map_subtiles_y))
                        {
                            filled_h = ceiling_block_is_solid_including_corners_return_height(get_subtile_number(stl_x + sstep->v ,stl_y + sstep->h), cstl_x, cstl_y);
                            if (filled_h > -1)
                            {
                                int delta_tmp = abs(stl_x - cstl_x);
                                int delta_max = abs(stl_y - cstl_y);
                                if (delta_max < delta_tmp)
                                    delta_max = delta_tmp;
                                if (filled_h < game.ceiling_height_max)
                                {
                                    filled_h += game.ceiling_step * delta_max;
                                    if (filled_h > game.ceiling_height_max)
                                        filled_h = game.ceiling_height_max;
                                } else
                                if ( filled_h > game.ceiling_height_max )
                                {
                                    filled_h -= game.ceiling_step * delta_max;
                                    if (filled_h < game.ceiling_height_min)
                                        filled_h = game.ceiling_height_min;
                                }
                                break;
                            }
                        }
                    }
                    i++;
                    if (i >= game.ceiling_search_dist) {
                        filled_h = game.ceiling_height_max;
                        break;
                    }
                }
              }
            }
            struct Map *mapblk = get_map_block_at(stl_x,stl_y);
            set_mapblk_filled_subtiles(mapblk, filled_h);
        }
}

void ceiling_init()
{
    if (world_paged)
    {
        /* paged mode: the loaded subtiles, in the order a walk of their bounding box had them (a subtile reads what
           the ones left of it and above it were given). The box itself is not walked: with two dungeons far apart,
           or a World Boss arena in its own corner of the world, nearly all of it is ground that isn't loaded, and
           each of those subtiles cost a search of the whole ceiling distance. */
        struct WpStlIter it;
        MapSubtlCoord stl_x, stl_y;
        struct Map *mapblk;
        wp_stl_iter_init(&it);
        while (wp_stl_iter_next(&it, &stl_x, &stl_y, &mapblk))
            ceiling_init_subtile(stl_x, stl_y);
        return;
    }
    for (MapSubtlCoord stl_y=0; stl_y < game.map_subtiles_y; stl_y++)
    {
        for (MapSubtlCoord stl_x=0; stl_x < game.map_subtiles_x; stl_x++)
        {
            ceiling_init_subtile(stl_x, stl_y);
        }
    }
}

short ceiling_set_info(long height_max, long height_min, long step)
{
    SYNCDBG(6,"Starting");
    long dist;
    if (step <= 0)
    {
      ERRORLOG("Illegal ceiling step value");
      return 0;
    }
    if (height_max > 15)
    {
      ERRORLOG("Max height is too high");
      return 0;
    }
    if (height_min > height_max)
    {
      ERRORLOG("Ceiling max height is smaller than min height");
      return 0;
    }
    dist = (height_max - height_min) / step;
    if ( dist >= 2500 )
      dist = 2500;
    game.ceiling_dist = dist;
    if (dist > 20)
    {
      ERRORLOG("Ceiling search distance too big");
      return 0;
    }
    game.ceiling_height_max = height_max;
    game.ceiling_height_min = height_min;
    game.ceiling_step = step;
    game.ceiling_search_dist = (2*game.ceiling_dist+1) * (2*game.ceiling_dist+1);
    return 1;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

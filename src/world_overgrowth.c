/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_overgrowth.c
 *     Endless world: the Overgrowth (generator version 8's biome).
 * @par Purpose:
 *     Roadmap "The Overgrowth": "A new biome in which dug earth does not stay dug. Each dawn, the earth creeps back over
 *     any tunnel or cave floor that has no owner, slab by slab, starting at the edges." Claimed ground stays open for
 *     good; the earth never closes on a slab that something stands on.
 *
 *     Decided (2026-11-13, the page's open point "how fast"): one layer a dawn. At dawn every dirt path slab nobody
 *     owns, in the Overgrowth, with earth beside it (an edge), is listed; through the morning they close, a few a turn
 *     (OG_A_TURN), each one only if it is still nobody's dirt path and nothing (creature, object, body) is on it. A
 *     one-slab tunnel closes in a dawn; a cave narrows by a slab all round each dawn. Only loaded ground: a chunk away
 *     keeps its tunnels until it is back (they close at its next dawn on the map). Nothing is saved: the list is made
 *     again at the next dawn.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_overgrowth.h"

#include <string.h>
#include <stdlib.h>

#include "world_stream.h"
#include "world_gen.h"
#include "world_daynight.h"
#include "bflib_basics.h"
#include "game_legacy.h"
#include "map_blocks.h"
#include "map_data.h"
#include "slab_data.h"
#include "thing_data.h"
#include "player_data.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define OG_MAX 16384     /* slabs listed at a dawn at most */
#define OG_A_TURN 8      /* closed a turn at most */
#define OG_CHUNKS 8192

static int64_t og_x[OG_MAX], og_y[OG_MAX];
static int og_count, og_next;
static long og_day = -1;
static int og_closed;
static int og_was_day = -1;           /* -1: not known yet (a load): the first dawn seen starts it */

static TbBool og_nobodys_path(MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && (slb->kind == SlbT_PATH) && (slabmap_owner(slb) == game.neutral_player_num);
}

static TbBool og_earth(MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && ((slb->kind == SlbT_EARTH) || (slb->kind == SlbT_TORCHDIRT));
}

/** Anything on a slab (a creature, an object, a body ...)? */
static TbBool og_something_on(MapSlabCoord x, MapSlabCoord y)
{
    for (int sy = 0; sy < STL_PER_SLB; sy++)
        for (int sx = 0; sx < STL_PER_SLB; sx++)
        {
            const struct Map *mapblk = get_map_block_at(slab_subtile(x, sx), slab_subtile(y, sy));
            if (!map_block_invalid(mapblk) && (get_mapwho_thing_index(mapblk) != 0))
                return true;
        }
    return false;
}

static void og_list(void)
{
    static int32_t wcx[OG_CHUNKS], wcy[OG_CHUNKS];
    struct WgWorld *gen = world_generator();
    og_count = og_next = 0;
    og_closed = 0;
    if (gen == NULL)
        return;
    const int n = world_loaded_chunks(wcx, wcy, OG_CHUNKS);
    for (int c = 0; c < n; c++)
    {
        const int64_t x0 = (int64_t)wcx[c] * WG_CHUNK, y0 = (int64_t)wcy[c] * WG_CHUNK;
        /* a chunk with no Overgrowth at its corners or middle is passed over (a border's few slabs wait a dawn) */
        if ((wg_biome_at(gen, x0 + 8, y0 + 8) != WgB_Overgrowth) && (wg_biome_at(gen, x0, y0) != WgB_Overgrowth) &&
            (wg_biome_at(gen, x0 + 15, y0) != WgB_Overgrowth) && (wg_biome_at(gen, x0, y0 + 15) != WgB_Overgrowth) &&
            (wg_biome_at(gen, x0 + 15, y0 + 15) != WgB_Overgrowth))
            continue;
        for (int ly = 0; ly < WG_CHUNK; ly++)
            for (int lx = 0; lx < WG_CHUNK; lx++)
            {
                MapSlabCoord x, y;
                if (!world_engine_slab(x0 + lx, y0 + ly, &x, &y) || !og_nobodys_path(x, y))
                    continue;
                if (!og_earth(x + 1, y) && !og_earth(x - 1, y) && !og_earth(x, y + 1) && !og_earth(x, y - 1))
                    continue;
                if (wg_biome_at(gen, x0 + lx, y0 + ly) != WgB_Overgrowth)
                    continue;
                if (og_count < OG_MAX)
                {
                    og_x[og_count] = x0 + lx;
                    og_y[og_count] = y0 + ly;
                    og_count++;
                }
            }
    }
    if (og_count > 0)
        SYNCLOG("World: the Overgrowth: dawn, %d slabs of nobody's dug ground at its edges begin to close", og_count);
}

void world_overgrowth_turn(void)
{
    if (!world_mode_active() || (world_gen_version() < 8))
        return;
    const int is_day = world_is_day() ? 1 : 0;
    const TbBool dawn = (og_was_day == 0) && is_day;
    og_was_day = is_day;
    if (!is_day)
        return;
    const long day = world_day_number();
    if (dawn && (day != og_day))
    {
        og_day = day;
        og_list();
    }
    for (int k = 0; (k < OG_A_TURN) && (og_next < og_count); k++, og_next++)
    {
        MapSlabCoord x, y;
        if (!world_engine_slab(og_x[og_next], og_y[og_next], &x, &y) || !og_nobodys_path(x, y) || og_something_on(x, y))
            continue; /* claimed meanwhile, or something stands there: it stays open */
        place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
        og_closed++;
    }
    if ((og_next == og_count) && (og_count > 0))
    {
        SYNCLOG("World: the Overgrowth: %d of the %d slabs closed this morning", og_closed, og_count);
        og_count = og_next = 0;
    }
}

void world_overgrowth_reset(void)
{
    og_count = og_next = og_closed = 0;
    og_day = -1;
    og_was_day = -1;
}

#ifdef __cplusplus
}
#endif

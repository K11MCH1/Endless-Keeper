/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pools.c
 *     Endless world: a creature pool for each keeper (see world_pools.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pools.h"
#include <string.h>
#include "bflib_basics.h"
#include "creature_control.h"
#include "game_legacy.h"
#include "player_data.h"
#include "world_stream.h"
#include "room_entrance.h"
#include "keeperfx.hpp"
#include "config_creature.h"
#include "post_inc.h"

#define WPL_SAVE_VERSION 1

static int32_t wpl_base[CREATURE_TYPES_MAX];  /* the script's pool, as the world began */
static TbBool wpl_base_set;
static int32_t wpl[PLAYERS_MAX][CREATURE_TYPES_MAX];
static PlayerBitFlags wpl_on;                  /* the keepers that have their own pool */

static void wpl_take_base(void)
{
    if (wpl_base_set)
        return;
    memcpy(wpl_base, game.pool.crtr_kind, sizeof(wpl_base));
    wpl_base_set = true;
    long total = 0;
    for (int m = 0; m < CREATURE_TYPES_MAX; m++)
        total += (wpl_base[m] > 0) ? wpl_base[m] : 0;
    SYNCLOG("World pools: each keeper's pool is a copy of the script's (%ld creatures)", total);
}

void world_pools_first_turn(void)
{
    wpl_take_base();
}

void world_pools_seat_started(PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    wpl_take_base();
    memcpy(wpl[plyr_idx], wpl_base, sizeof(wpl_base));
    pmask_add(&wpl_on, plyr_idx);
    SYNCLOG("World pools: player %d has a fresh pool", (int)plyr_idx);
}

int32_t *world_pool_for(PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (plyr_idx >= PLAYERS_COUNT))
        return NULL;
    if (pmask_has(wpl_on, plyr_idx))
        return wpl[plyr_idx];
    if (!world_player_is_human(plyr_idx))
        return NULL;
    /* a world saved before the pools: its keepers get theirs now (the world's pool as it stands, if the script's
       wasn't taken: a load) */
    world_pools_seat_started(plyr_idx);
    return wpl[plyr_idx];
}

void world_pools_reset(void)
{
    memset(wpl_base, 0, sizeof(wpl_base));
    wpl_base_set = false;
    memset(wpl, 0, sizeof(wpl));
    wpl_on = pmask_none();
}

/* the record: version, the base (set or not, then the counts), the keepers' pools (count, then player and counts) */
size_t world_pools_save_size(void)
{
    uint32_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (pmask_has(wpl_on, p))
            n++;
    return 4 + 4 + sizeof(wpl_base) + 4 + n * (4 + sizeof(wpl[0]));
}

void world_pools_save(uint8_t *buf)
{
    uint32_t v = WPL_SAVE_VERSION;
    memcpy(buf, &v, 4);
    buf += 4;
    v = wpl_base_set ? 1 : 0;
    memcpy(buf, &v, 4);
    buf += 4;
    memcpy(buf, wpl_base, sizeof(wpl_base));
    buf += sizeof(wpl_base);
    uint32_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (pmask_has(wpl_on, p))
            n++;
    memcpy(buf, &n, 4);
    buf += 4;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!pmask_has(wpl_on, p))
            continue;
        const uint32_t plyr = (uint32_t)p;
        memcpy(buf, &plyr, 4);
        buf += 4;
        memcpy(buf, wpl[p], sizeof(wpl[p]));
        buf += sizeof(wpl[p]);
    }
}

TbBool world_pools_load(const uint8_t *buf, size_t len)
{
    world_pools_reset();
    uint32_t v = 0, set = 0, n = 0;
    if (len < 4 + 4 + sizeof(wpl_base) + 4)
        return false;
    memcpy(&v, buf, 4);
    memcpy(&set, buf + 4, 4);
    if (v != WPL_SAVE_VERSION)
        return true; /* a newer record: the keepers get theirs again when asked */
    memcpy(wpl_base, buf + 8, sizeof(wpl_base));
    wpl_base_set = (set != 0);
    size_t pos = 8 + sizeof(wpl_base);
    memcpy(&n, buf + pos, 4);
    pos += 4;
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t plyr = 0;
        if (pos + 4 + sizeof(wpl[0]) > len)
            return false;
        memcpy(&plyr, buf + pos, 4);
        pos += 4;
        if (plyr < PLAYERS_MAX)
        {
            memcpy(wpl[plyr], buf + pos, sizeof(wpl[0]));
            pmask_add(&wpl_on, (PlayerNumber)plyr);
        }
        pos += sizeof(wpl[0]);
    }
    return true;
}

/** Test hook KFX_POOL_TEST=1 (a server): once two keepers have their own pool, the first gives one Dragon to its
 *  portal and gets it back by a death; the log shows each pool and the world's at each step. */
void world_pools_test(void)
{
    static int on = -1;
    static TbBool done = false;
    if (on < 0)
        on = (getenv("KFX_POOL_TEST") != NULL) ? 1 : 0;
    if (!on || done)
        return;
    PlayerNumber first = PLAYER_NONE, second = PLAYER_NONE;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if (!pmask_has(wpl_on, p))
            continue;
        if (first == PLAYER_NONE)
            first = p;
        else if (second == PLAYER_NONE)
            second = p;
    }
    if (second == PLAYER_NONE)
        return;
    done = true;
    const long dragon = get_id(creature_desc, "DRAGON");
    if (dragon <= 0)
        return;
    SYNCLOG("Pool test: Dragons before: player %d %d, player %d %d, world %d", (int)first, (int)creature_pool_count(first, dragon),
        (int)second, (int)creature_pool_count(second, dragon), (int)game.pool.crtr_kind[dragon]);
    remove_creature_from_generate_pool((ThingModel)dragon, first);
    SYNCLOG("Pool test: player %d's portal takes one: player %d %d, player %d %d, world %d", (int)first, (int)first,
        (int)creature_pool_count(first, dragon), (int)second, (int)creature_pool_count(second, dragon), (int)game.pool.crtr_kind[dragon]);
    add_creature_to_pool_of(first, (ThingModel)dragon, 1);
    SYNCLOG("Pool test: it dies and goes back: player %d %d, player %d %d, world %d", (int)first,
        (int)creature_pool_count(first, dragon), (int)second, (int)creature_pool_count(second, dragon), (int)game.pool.crtr_kind[dragon]);
}

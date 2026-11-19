/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_threat.c
 *     Endless world: how strong a keeper's army is, for sizing raids and wild packs (see world_threat.h).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_threat.h"

#include <stdlib.h>
#include <string.h>

#include "game_legacy.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "creature_control.h"
#include "config_creature.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define THREAT_FULL 10          /* the strongest creatures counted in full; the rest at a quarter */
#define THREAT_REST_SHARE 0.25
#define THREAT_MAX_COUNTED 1024

double world_threat_power(ThingModel model, int level)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    if (crconf == NULL)
        return 0.0;
    if (level < 1)
        level = 1;
    /* the game's own growth per level (35% health and 35% strength in the stock config): fighting power grows with
       both, so about with the square of the level */
    const double h = 1.0 + (double)game.conf.crtr_conf.exp.health_increase_on_exp * (level - 1) / 100.0;
    const double s = 1.0 + (double)game.conf.crtr_conf.exp.strength_increase_on_exp * (level - 1) / 100.0;
    const double health = (crconf->health > 0) ? (double)crconf->health : 1.0;
    const double strength = (crconf->strength > 0) ? (double)crconf->strength : 1.0;
    return health * h * strength * s / 1000.0;
}

static int cmp_desc(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? 1 : (x > y) ? -1 : 0;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

void world_threat_army(PlayerNumber plyr_idx, struct WorldArmy *army)
{
    memset(army, 0, sizeof(*army));
    army->median = army->upper = army->best = 1;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    static double power[THREAT_MAX_COUNTED];
    static int level[THREAT_MAX_COUNTED];
    int n = 0;
    long guard = 0;
    /* the creature list (the imps are on the digger list, not counted: mining imps and Create Imp's level 3 imps outgrew
       the army and brought raids far above it; audit 2026-11-09) */
    for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT) && (n < THREAT_MAX_COUNTED);)
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        const int lv = (int)cctrl->exp_level + 1;
        power[n] = world_threat_power(thing->model, lv);
        level[n] = lv;
        n++;
    }
    army->count = n;
    if (n == 0)
        return;
    qsort(power, (size_t)n, sizeof(power[0]), cmp_desc);
    qsort(level, (size_t)n, sizeof(level[0]), cmp_int);
    for (int k = 0; k < n; k++)
        army->power += (k < THREAT_FULL) ? power[k] : power[k] * THREAT_REST_SHARE;
    army->median = level[n / 2];
    army->upper = level[(3 * n) / 4];
    army->best = level[n - 1];
}

int world_threat_upper_level(PlayerNumber plyr_idx)
{
    struct WorldArmy army;
    world_threat_army(plyr_idx, &army);
    return army.upper;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

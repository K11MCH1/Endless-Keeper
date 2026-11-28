/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_risen.c
 *     Endless world: the creatures that rise from the dead - Vampires, Skeletons and Ghosts - and their limits.
 * @par Purpose:
 *     The user, 2026-11-28 (their keeper kept getting Vampires from the Graveyard): "Cap 4 but also slower (e.g. the 2nd
 *     needs twice as many, the 3rd three times...), so they get rarer", the corpses counted "only 1 day and 1 night,
 *     FIFO", and "Do the same for Skeletons and Ghosts".
 *     - A keeper has at most WRIS_CAP of each kind (all of that kind it owns, however they came).
 *     - Each event is kept for one world day and its night (world_day_turns), the oldest first: a corpse rotted in the
 *       Graveyard (Vampires), a prisoner starved to death in the Prison (Skeletons), a creature dead under torture
 *       (Ghosts). Nothing builds up while a keeper is at the cap: the old ones run out.
 *     - The next one rises when the events kept reach (the ones it has + 1) times the kind's own number: for Vampires
 *       the corpses the Graveyard asks (its level asks fewer), for Skeletons and Ghosts one death. Those events, the
 *       oldest, are used up. Otherwise the prisoner or the tortured creature just dies (a Graveyard corpse just rots).
 *     Saved: RIS1.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_risen.h"

#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "player_utils.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "player_data.h"
#include "config_creature.h"
#include "world_daynight.h"
#include "world_stream.h"
#include "post_inc.h"

#define WRIS_CAP 4      /* of each kind a keeper may have */
#define WRIS_KEEP 128   /* events kept a kind (more in a day are the oldest's loss) */
#define WRIS_SAVE_VERSION 1
#define WRIS_TEST_PLAYER ((PlayerNumber)0)

static const char *const wris_names[WRis_Count] = {"Vampire", "Skeleton", "Ghost"};
static GameTurn wris_at[PLAYERS_MAX][WRis_Count][WRIS_KEEP]; /* when each was, the oldest first */
static int wris_n[PLAYERS_MAX][WRis_Count];

static void wris_drop_oldest(PlayerNumber p, int kind, int count)
{
    const int n = wris_n[p][kind];
    count = (count > n) ? n : count;
    if (count <= 0)
        return;
    memmove(&wris_at[p][kind][0], &wris_at[p][kind][count], sizeof(GameTurn) * (size_t)(n - count));
    wris_n[p][kind] = n - count;
}

/** Events older than one world day and its night run out. */
static void wris_expire(PlayerNumber p, int kind)
{
    const GameTurnDelta keep = (GameTurnDelta)world_day_turns();
    int old = 0;
    while ((old < wris_n[p][kind]) && ((GameTurnDelta)(game.play_gameturn - wris_at[p][kind][old]) > keep))
        old++;
    wris_drop_oldest(p, kind, old);
}

TbBool world_risen_counts(PlayerNumber p, int kind, int base_needed, ThingModel model)
{
    if ((p < 0) || (p >= PLAYERS_MAX) || (kind < 0) || (kind >= WRis_Count))
        return false;
    wris_expire(p, kind);
    if (wris_n[p][kind] >= WRIS_KEEP)
        wris_drop_oldest(p, kind, 1);
    wris_at[p][kind][wris_n[p][kind]++] = game.play_gameturn;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    const int owned = (dungeon_invalid(dungeon) || (model <= 0) || (model >= CREATURE_TYPES_MAX)) ? 0 :
        (int)dungeon->owned_creatures_of_model[model];
    if (owned >= WRIS_CAP)
    {
        SYNCLOG("World risen: player %d has %d %ss (the most it may have): %d in the last day kept, none rises",
            (int)p, owned, wris_names[kind], wris_n[p][kind]);
        return false;
    }
    const int needed = ((base_needed > 0) ? base_needed : 1) * (owned + 1);
    if (wris_n[p][kind] < needed)
    {
        SYNCLOG("World risen: player %d: %d of the %d the next %s needs (it has %d)", (int)p, wris_n[p][kind], needed,
            wris_names[kind], owned);
        return false;
    }
    wris_drop_oldest(p, kind, needed);
    SYNCLOG("World risen: player %d: a %s rises (its %d%s; %d used up)", (int)p, wris_names[kind], owned + 1,
        (owned == 0) ? "st" : (owned == 1) ? "nd" : (owned == 2) ? "rd" : "th", needed);
    return true;
}

/******************************************************************************/
/* Test switch KFX_WORLD_RISENTEST=T (with short days: KFX_WORLD_DAYNIGHT): at T player 0 gets five Vampire events (base 2:
   the 2nd and the 4th raise one, no creature is made); a day and its night later one more (the fifth ran out: none);
   then it is given four Vampires and one more event (at the cap: none). */

void world_risen_test_turn(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_RISENTEST");
        at = (v != NULL) ? atol(v) : -1;
    }
    if (at < 0)
        return;
    const long t = (long)game.play_gameturn;
    const ThingModel vampire = (ThingModel)get_id(creature_desc, "VAMPIRE");
    const long later = at + world_day_turns() + 10;
    if (t == at)
        for (int i = 1; i <= 5; i++)
            SYNCLOG("World risen test: event %d: %s", i, world_risen_counts(WRIS_TEST_PLAYER, WRis_Vampire, 2, vampire) ? "rises" : "no");
    else if (t == later)
        SYNCLOG("World risen test: a day and night later: %s (kept %d)", world_risen_counts(WRIS_TEST_PLAYER, WRis_Vampire, 2, vampire) ?
            "rises" : "no", wris_n[WRIS_TEST_PLAYER][WRis_Vampire]);
    else if (t == later + 20)
    {
        const struct Thing *heart = get_player_soul_container(WRIS_TEST_PLAYER);
        for (int i = 0; (i < 4) && thing_exists(heart); i++)
            create_creature((struct Coord3d *)&heart->mappos, vampire, WRIS_TEST_PLAYER);
    } else if (t == later + 40)
        for (int i = 1; i <= 3; i++)
            SYNCLOG("World risen test: with four Vampires, event %d: %s", i,
                world_risen_counts(WRIS_TEST_PLAYER, WRis_Vampire, 2, vampire) ? "rises" : "no");
}

/******************************************************************************/
/* The save (RIS1): a version, then for each player and kind a count and the events' ages in turns */

void world_risen_reset(void)
{
    memset(wris_n, 0, sizeof(wris_n));
}

size_t world_risen_save_size(void)
{
    size_t n = 1;
    TbBool any = false;
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; k < WRis_Count; k++)
        {
            n += 1 + sizeof(uint32_t) * (size_t)wris_n[p][k];
            any = any || (wris_n[p][k] > 0);
        }
    return any ? n : 0;
}

void world_risen_save(uint8_t *buf)
{
    uint8_t *at = buf;
    *at++ = WRIS_SAVE_VERSION;
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; k < WRis_Count; k++)
        {
            *at++ = (uint8_t)wris_n[p][k];
            for (int i = 0; i < wris_n[p][k]; i++)
            {
                const uint32_t age = (uint32_t)(GameTurnDelta)(game.play_gameturn - wris_at[p][k][i]);
                memcpy(at, &age, sizeof(age));
                at += sizeof(age);
            }
        }
}

TbBool world_risen_load(const uint8_t *buf, size_t len)
{
    world_risen_reset();
    if ((len < 1) || (buf[0] != WRIS_SAVE_VERSION))
        return false;
    size_t at = 1;
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; k < WRis_Count; k++)
        {
            if (at >= len)
                return false;
            const int n = buf[at++];
            if ((n > WRIS_KEEP) || (at + sizeof(uint32_t) * (size_t)n > len))
            {
                world_risen_reset();
                return false;
            }
            for (int i = 0; i < n; i++)
            {
                uint32_t age;
                memcpy(&age, buf + at, sizeof(age));
                at += sizeof(age);
                wris_at[p][k][i] = game.play_gameturn - (GameTurn)age;
            }
            wris_n[p][k] = n;
        }
    return true;
}

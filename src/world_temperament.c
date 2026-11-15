/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_temperament.c
 *     Endless world: world temperaments (see world_temperament.h).
 * @par Purpose:
 *     Roadmap "World Temperaments": "A world can be dealt a temperament: three traits and one wonder." Optional, chosen
 *     once when a world is first created, never given to a world that exists, never changed.
 *
 *     Decided (2026-11-13, the page's open point "the list of traits and of wonders"): four axes, each with a harder
 *     and a kinder pole; a temperament is three of the four axes, and when two of its traits are harder the third is
 *     kinder (no world gets only hardship):
 *       nights     Long-nighted (nights half as long again: the night's packs and raids) | Short-nighted (nights 60%)
 *       gold       Poor in gold (a third of the gold is earth) | Rich in gold (a quarter of the gold is dense)
 *       strongholds  Bristling with strongholds (outposts in 75% of their regions) | Few strongholds (35%; 55% is usual)
 *       the wild   Wild (half as many wild creatures again) | Tame (half as many)
 *     One wonder, to begin with the Well of Gold: a shrine (a 3x3 Temple nobody owns, in a 5x5 hall) 350 to 500 slabs
 *     from the start; the keeper who holds it (owns its Temple) gets 250 gold every 30 seconds for as long as it holds it.
 *     The traits act where the world is made and run (new ground's gold, the outposts' regions, the night, the wild
 *     packs' caps), so they only touch what the world makes from then on; the ground's generator is not changed.
 *     Dealt by the seed (the same seed gets the same temperament). Saved: XTR2 record "TMP1".
 *     The switch at a world's creation is the test switch KFX_WORLD_TEMPERAMENT=1 for now (the New Run menu's switch is
 *     not built), and the World List does not show it yet: it is told to the keepers when the world begins.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_temperament.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "world_stream.h"
#include "world_gen.h"
#include "world_daynight.h"
#include "bflib_basics.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "slab_data.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
enum { TA_Nights = 0, TA_Gold, TA_Strongholds, TA_Wild, TA_COUNT };
static const char *const ta_names[TA_COUNT][3] = {
    /* -1 harder, +1 kinder */
    {"Long-nighted", "", "Short-nighted"},
    {"Poor in gold", "", "Rich in gold"},
    {"Bristling with strongholds", "", "Few strongholds"},
    {"Wild", "", "Tame"},
};

static struct {
    uint8_t dealt;
    int8_t trait[TA_COUNT];       /* -1 harder, +1 kinder, 0 not in it */
    uint8_t wonder;               /* 1: the Well of Gold */
    int64_t wx, wy;               /* the wonder's middle (world slab) */
} tmp;
#define WONDER_GIFT 250
#define WONDER_EVERY 600

static uint64_t tmp_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void tmp_apply(void)
{
    world_daynight_night_percent(!tmp.dealt ? 100 : (tmp.trait[TA_Nights] < 0) ? 150 : ((tmp.trait[TA_Nights] > 0) ? 60 : 100));
}

void world_temper_reset(void)
{
    memset(&tmp, 0, sizeof(tmp));
    tmp_apply();
}

TbBool world_temper_text(char *buf, size_t len)
{
    if (!tmp.dealt)
    {
        if (len > 0)
            buf[0] = 0;
        return false;
    }
    size_t n = 0;
    buf[0] = 0;
    for (int a = 0; a < TA_COUNT; a++)
        if (tmp.trait[a] != 0)
            n += (size_t)snprintf(buf + n, (n < len) ? len - n : 0, "%s%s", (n > 0) ? ", " : "", ta_names[a][tmp.trait[a] + 1]);
    if (tmp.wonder == 1)
        snprintf(buf + n, (n < len) ? len - n : 0, "; holds the Well of Gold");
    return true;
}

/** A new world (on the simulating machine, its first turn): dealt a temperament if the switch is on. */
void world_temper_new_world(int64_t home_x, int64_t home_y)
{
    const char *v = getenv("KFX_WORLD_TEMPERAMENT");
    world_temper_reset();
    if ((v == NULL) || (atoi(v) == 0))
        return;
    uint64_t h = tmp_mix(world_seed() ^ 0x54454D50ULL);
    /* three of the four axes */
    const int left_out = (int)(h % TA_COUNT);
    int harder = 0;
    for (int a = 0; a < TA_COUNT; a++)
    {
        if (a == left_out)
            continue;
        h = tmp_mix(h);
        tmp.trait[a] = (h & 1) ? -1 : 1;
        harder += (tmp.trait[a] < 0);
    }
    if (harder == 3)
    {
        /* never only hardship: the last of them turns kind */
        for (int a = TA_COUNT - 1; a >= 0; a--)
            if (tmp.trait[a] < 0)
            {
                tmp.trait[a] = 1;
                break;
            }
    }
    tmp.wonder = 1;
    h = tmp_mix(h);
    const double ang = (double)(h % 3600) * (3.14159265358979 / 1800.0);
    const double dist = 350.0 + (double)((h >> 16) % 151);
    tmp.wx = home_x + (int64_t)llround(cos(ang) * dist);
    tmp.wy = home_y + (int64_t)llround(sin(ang) * dist);
    /* its hall (5x5) whole inside one chunk */
    for (int k = 0; k < 2; k++)
    {
        int64_t *pv = (k == 0) ? &tmp.wx : &tmp.wy;
        const int64_t c = (*pv >= 0) ? (*pv / WG_CHUNK) : -((-*pv + WG_CHUNK - 1) / WG_CHUNK);
        int64_t o = *pv - c * WG_CHUNK;
        if (o < 3) o = 3;
        if (o > WG_CHUNK - 4) o = WG_CHUNK - 4;
        *pv = c * WG_CHUNK + o;
    }
    tmp.dealt = 1;
    tmp_apply();
    char text[200];
    world_temper_text(text, sizeof(text));
    SYNCLOG("World: this world's temperament: %s (the Well of Gold at world slab (%lld,%lld))", text, (long long)tmp.wx,
        (long long)tmp.wy);
}

/** The first turn (every machine that has it): the keepers are told. */
void world_temper_first_turn(void)
{
    char text[200];
    if (world_temper_text(text, sizeof(text)))
        message_add_fmt(MsgType_Blank, 0, "This world is %s", text);
}

int world_temper_outpost_chance(int base)
{
    if (!tmp.dealt || (tmp.trait[TA_Strongholds] == 0))
        return base;
    return (tmp.trait[TA_Strongholds] < 0) ? 75 : 35;
}

int world_temper_wild_cap(int cap)
{
    if (!tmp.dealt || (tmp.trait[TA_Wild] == 0))
        return cap;
    return (tmp.trait[TA_Wild] < 0) ? cap * 3 / 2 : cap / 2;
}

int world_temper_gold(void)
{
    return tmp.dealt ? tmp.trait[TA_Gold] : 0;
}

TbBool world_temper_wonder(int64_t *wx, int64_t *wy)
{
    if (!tmp.dealt || (tmp.wonder == 0))
        return false;
    *wx = tmp.wx;
    *wy = tmp.wy;
    return true;
}

/** Once a turn on the simulating machine: the Well of Gold's gift to the keeper who holds it. */
void world_temper_turn(void)
{
    if (!tmp.dealt || (tmp.wonder != 1) || ((game.play_gameturn % WONDER_EVERY) != 0))
        return;
    MapSlabCoord sx, sy;
    if (!world_engine_slab(tmp.wx, tmp.wy, &sx, &sy))
        return;
    const struct SlabMap *slb = get_slabmap_block(sx, sy);
    if (slabmap_block_invalid(slb) || (slb->room_index <= 0))
        return;
    const struct Room *room = room_get(slb->room_index);
    if (room_is_invalid(room) || (room->owner == game.neutral_player_num) || (room->owner == PLAYER_GOOD) ||
        !player_is_keeper(room->owner))
        return;
    player_add_offmap_gold(room->owner, WONDER_GIFT);
    SYNCLOG("World: the Well of Gold gives player %d %d gold", (int)room->owner, WONDER_GIFT);
}

/** The thin stream: the nights' percent the server's world has (0: as usual), in each record's header. */
int world_temper_nights_wire(void)
{
    if (!tmp.dealt || (tmp.trait[TA_Nights] == 0))
        return 0;
    return (tmp.trait[TA_Nights] < 0) ? 150 : 60;
}

/** A thin client: the server's world has nights of pct percent (0: as usual); the day and night follow it. */
void world_temper_client_nights(int pct)
{
    static int had = 0;
    if (pct == had)
        return;
    had = pct;
    world_daynight_night_percent((pct > 0) ? pct : 100);
}

/* saves: "TMP1" in XTR2: dealt, the four traits, the wonder, its place */
#define TMP_SAVED 22
size_t world_temper_save_size(void)
{
    return tmp.dealt ? TMP_SAVED : 0;
}

void world_temper_save(uint8_t *buf)
{
    buf[0] = tmp.dealt;
    for (int a = 0; a < TA_COUNT; a++)
        buf[1 + a] = (uint8_t)tmp.trait[a];
    buf[5] = tmp.wonder;
    memcpy(buf + 6, &tmp.wx, 8);
    memcpy(buf + 14, &tmp.wy, 8);
}

TbBool world_temper_load(const uint8_t *buf, size_t len)
{
    world_temper_reset();
    if (len < TMP_SAVED)
        return false;
    tmp.dealt = buf[0];
    for (int a = 0; a < TA_COUNT; a++)
        tmp.trait[a] = (int8_t)buf[1 + a];
    tmp.wonder = buf[5];
    memcpy(&tmp.wx, buf + 6, 8);
    memcpy(&tmp.wy, buf + 14, 8);
    tmp_apply();
    return true;
}

#ifdef __cplusplus
}
#endif

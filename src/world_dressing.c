/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dressing.c
 *     Endless world: dressing the dungeon.
 * @par Purpose:
 *     See world_dressing.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_dressing.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>

#include "world_stream.h"
#include "world_pages.h"
#include "world_gen.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "slab_data.h"
#include "map_data.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define DRESS_BIOMES 8   /* the land biomes: Earthen ... Ruins */

struct Dressing {
    uint8_t strongholds[DRESS_BIOMES];  /* destroyed in each biome */
    int8_t wearing;                     /* the biome worn (-1: none) */
    int8_t was_wearing;                 /* the one to take off (a change in progress) */
    uint8_t pad[2];
};
static struct Dressing dr[PLAYERS_MAX];
static uint32_t dr_next;

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: dressing, to player %d: %s", (int)p, text);
    if (is_my_player_number(p) && !netstate_dedicated())
    {
        if (chat_box_active())
            chat_box_add(-1, text);
        else
            message_add(MsgType_Blank, 0, text);
        return;
    }
    if (!network_is_active() || !netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static TbBool unlocked(PlayerNumber p, int biome)
{
    return (biome == WgB_Earthen) || ((biome >= 0) && (biome < DRESS_BIOMES) && (dr[p].strongholds[biome] >= DRESSING_STRONGHOLDS));
}

void world_dressing_stronghold_destroyed(PlayerNumber by, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!simulating() || (by < 0) || (by >= PLAYERS_MAX) || !world_player_is_human(by))
        return;
    int biome = world_biome_parent(world_biome_at_subtile(stl_x, stl_y));
    if ((biome < 0) || (biome >= DRESS_BIOMES))
        return;
    struct Dressing *d = &dr[by];
    if (d->strongholds[biome] < 255)
        d->strongholds[biome]++;
    if (d->strongholds[biome] == DRESSING_STRONGHOLDS)
    {
        char text[128];
        snprintf(text, sizeof(text), "Your dungeon may wear the look of %s now (\"!look %s\").", wg_biome_name(biome), wg_biome_name(biome));
        tell_keeper(by, text);
    }
}

void world_dressing_packet(PlayerNumber p, int what)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    if ((what != 0) && (what != 255) && ((what < 1) || (what > DRESS_BIOMES)))
        return; /* (a value no button sends: a damaged or forged packet) */
    struct Dressing *d = &dr[p];
    char text[200];
    if (what == 255)
    {
        int n = snprintf(text, sizeof(text), "Looks unlocked:");
        for (int b = 0; b < DRESS_BIOMES; b++)
            if (unlocked(p, b) && (n < (int)sizeof(text) - 24))
                n += snprintf(text + n, sizeof(text) - n, " %s", wg_biome_name(b));
            else if (n < (int)sizeof(text) - 24)
                n += snprintf(text + n, sizeof(text) - n, " (%s %d/%d)", wg_biome_name(b), (int)d->strongholds[b], DRESSING_STRONGHOLDS);
        tell_keeper(p, text);
        return;
    }
    if (what == 0)
    {
        if (d->wearing >= 0)
            d->was_wearing = d->wearing;
        d->wearing = -1;
        tell_keeper(p, "Your dungeon takes off its look.");
        return;
    }
    const int biome = what - 1;
    if (!unlocked(p, biome))
    {
        snprintf(text, sizeof(text), "The look of %s is earned by destroying %d strongholds there (%d so far).",
            wg_biome_name(biome), DRESSING_STRONGHOLDS, (int)d->strongholds[biome]);
        tell_keeper(p, text);
        return;
    }
    d->was_wearing = -1;
    d->wearing = (int8_t)biome;
    snprintf(text, sizeof(text), "Your dungeon puts on the look of %s.", wg_biome_name(biome));
    tell_keeper(p, text);
}

TbBool world_dressing_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strncasecmp(text + 1, "look", 4) != 0)
        return false;
    const char *arg = text + 5;
    if ((*arg != '\0') && (*arg != ' '))
        return false;
    while (*arg == ' ')
        arg++;
    int what = 255;
    if (strcasecmp(arg, "off") == 0)
        what = 0;
    else if (*arg != '\0')
    {
        for (int b = 0; b < DRESS_BIOMES; b++)
        {
            const char *name = wg_biome_name(b);
            /* the biome's name, or its first word ("frozen" for Frozen Hollows) */
            const size_t len = strlen(arg);
            if ((strncasecmp(name, arg, len) == 0) && ((name[len] == '\0') || (name[len] == ' ') || (name[len] == '_')))
            {
                what = b + 1;
                break;
            }
        }
    }
    set_players_packet_action(get_my_player(), PckA_WorldLook, (unsigned long)what, 0, 0, 0);
    return true;
}

static void dress_chunk(MapSlabCoord x0, MapSlabCoord y0)
{
    for (MapSlabCoord y = y0; y < y0 + WORLD_CHUNK_SLABS; y++)
    {
        for (MapSlabCoord x = x0; x < x0 + WORLD_CHUNK_SLABS; x++)
        {
            const struct SlabMap *slb = get_slabmap_block(x, y);
            if (slabmap_block_invalid(slb))
                continue;
            const PlayerNumber p = slabmap_owner(slb);
            if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX))
                continue;
            const struct Dressing *d = &dr[p];
            if ((d->wearing < 0) && (d->was_wearing < 0))
                continue;
            const SlabCodedCoords n = get_slab_number(x, y);
            unsigned char *ext = slab_ext_ptr(n);
            const unsigned char *initial = slab_ext_initial_ptr(n);
            unsigned char want;
            if (d->wearing >= 0)
                want = (unsigned char)((*ext & ~0x1F) | (wg_biome_texture(d->wearing) & 0x1F));
            else
                want = (unsigned char)((*ext & ~0x1F) | (*initial & 0x1F)); /* back to the ground's own */
            if (*ext != want)
                *ext = want;
        }
    }
}

/** KFX_WORLD_DRESSTEST=turn (test switch): this machine's keeper is credited 5 strongholds in the Frozen Hollows,
 *  asks for the list, puts the Frozen look on, and 600 turns later the texture of its Heart's slab is logged, then it
 *  takes the look off; 600 turns later again the texture is logged. */
static void dress_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_DRESSTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_DRESSTEST=%ld", at);
    }
    {
        /* KFX_WORLD_DRESSCREDIT=turn (test switch, a dedicated server): the first human keeper with a Heart (a joined
           player) is credited 5 strongholds in the Frozen Hollows at that turn, to pick the look itself */
        static long credit_at = -2;
        if (credit_at == -2)
        {
            const char *v = getenv("KFX_WORLD_DRESSCREDIT");
            credit_at = (v != NULL) ? atol(v) : -1;
        }
        if ((credit_at >= 0) && ((long)game.play_gameturn == credit_at))
            for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
                if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
                {
                    dr[q].strongholds[WgB_Frozen] = DRESSING_STRONGHOLDS;
                    SYNCLOG("World: dress test: player %d is credited %d Frozen strongholds", (int)q, DRESSING_STRONGHOLDS);
                    break;
                }
    }
    if (at < 0)
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const SlabCodedCoords hn = get_slab_number(subtile_slab(heart->mappos.x.stl.num) + 2, subtile_slab(heart->mappos.y.stl.num));
    if ((long)game.play_gameturn == at)
    {
        world_dressing_packet(p, WgB_Frozen + 1); /* refused: not earned yet */
        for (int i = 0; i < DRESSING_STRONGHOLDS; i++)
            dr[p].strongholds[WgB_Frozen]++;
        world_dressing_packet(p, 255);
        world_dressing_packet(p, WgB_Frozen + 1);
        SYNCLOG("World: dress test: slab by the Heart texture %d (initial %d), Frozen's texture %d", (int)(*slab_ext_ptr(hn) & 0x1F),
            (int)(*slab_ext_initial_ptr(hn) & 0x1F), wg_biome_texture(WgB_Frozen));
    } else if ((long)game.play_gameturn == at + 600)
    {
        SYNCLOG("World: dress test: dressed, texture %d", (int)(*slab_ext_ptr(hn) & 0x1F));
        world_dressing_packet(p, 0);
    } else if ((long)game.play_gameturn == at + 1200)
        SYNCLOG("World: dress test: undressed, texture %d", (int)(*slab_ext_ptr(hn) & 0x1F));
}

void world_dressing_turn(void)
{
    if (!simulating())
        return;
    dress_test();
    if (world_is_chunked())
    {
        const int n = wp_count();
        if (n <= 0)
            return;
        int k = (int)(dr_next++ % (uint32_t)n);
        struct WorldPage *pg = wp_first();
        while ((k-- > 0) && (pg != NULL))
            pg = pg->next;
        if (pg != NULL)
            dress_chunk((MapSlabCoord)pg->cx * WORLD_CHUNK_SLABS, (MapSlabCoord)pg->cy * WORLD_CHUNK_SLABS);
    } else
    {
        const uint32_t n = WORLD_WINDOW_CHUNKS * WORLD_WINDOW_CHUNKS;
        const uint32_t k = dr_next++ % n;
        dress_chunk(1 + (MapSlabCoord)(k % WORLD_WINDOW_CHUNKS) * WORLD_CHUNK_SLABS, 1 + (MapSlabCoord)(k / WORLD_WINDOW_CHUNKS) * WORLD_CHUNK_SLABS);
    }
}

/******************************************************************************/
void world_dressing_reset(void)
{
    memset(dr, 0, sizeof(dr));
    for (int p = 0; p < PLAYERS_MAX; p++)
    {
        dr[p].wearing = -1;
        dr[p].was_wearing = -1;
    }
    dr_next = 0;
}

size_t world_dressing_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
    {
        if ((dr[p].wearing >= 0) || (dr[p].was_wearing >= 0))
            return 4 + sizeof(dr);
        for (int b = 0; b < DRESS_BIOMES; b++)
            if (dr[p].strongholds[b] != 0)
                return 4 + sizeof(dr);
    }
    return 0;
}

void world_dressing_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, dr, sizeof(dr));
}

TbBool world_dressing_load(const uint8_t *buf, size_t len)
{
    world_dressing_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (4 + sizeof(dr) > len))
        return false;
    memcpy(dr, buf + 4, sizeof(dr));
    return true;
}

void world_dressing_seat_reset(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    memset(&dr[p], 0, sizeof(dr[p]));
    dr[p].wearing = -1;
    dr[p].was_wearing = -1;
}

#ifdef __cplusplus
}
#endif

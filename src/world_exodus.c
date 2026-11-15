/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_exodus.c
 *     Endless world: moving the Heart.
 * @par Purpose:
 *     See world_exodus.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_exodus.h"
#include "creature_states.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <math.h>

#include "world_stream.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "room_util.h"
#include "slab_data.h"
#include "map_data.h"
#include "map_blocks.h"
#include "thing_data.h"
#include "thing_navigate.h"
#include "thing_stats.h"
#include "config_terrain.h"
#include "config_objects.h"
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
static uint8_t ex_on[PLAYERS_MAX];   /* the Exodus is ordered */

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: exodus, to player %d: %s", (int)p, text);
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

static TbBool open_for_heart(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    const struct Map *mapblk = get_map_block_at(slab_subtile_center(x), slab_subtile_center(y));
    if (slabmap_block_invalid(slb) || map_block_invalid(mapblk) || !map_block_revealed(mapblk, p))
        return false;
    if ((slb->kind < SlbT_PATH) || slab_kind_is_liquid(slb->kind) || slab_is_wall(x, y) || !room_is_invalid(slab_room_get(x, y)))
        return false;
    const PlayerNumber owner = slabmap_owner(slb);
    return (owner == p) || (owner == game.neutral_player_num) || (owner == PLAYER_NEUTRAL);
}

/** Another keeper's land (not an ally's) within EXODUS_NEIGHBOUR_SLABS of a slab? Its owner, or -1. */
static PlayerNumber other_keeper_near(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    for (MapSlabCoord j = y - EXODUS_NEIGHBOUR_SLABS; j <= y + EXODUS_NEIGHBOUR_SLABS; j++)
        for (MapSlabCoord i = x - EXODUS_NEIGHBOUR_SLABS; i <= x + EXODUS_NEIGHBOUR_SLABS; i++)
        {
            const struct SlabMap *slb = get_slabmap_block(i, j);
            if (slabmap_block_invalid(slb))
                continue;
            const PlayerNumber o = slabmap_owner(slb);
            if ((o == p) || (o < 0) || (o >= PLAYERS_COUNT) || (o == PLAYER_GOOD) || (o == PLAYER_NEUTRAL)
                || (o == game.neutral_player_num) || !player_is_keeper(o) || players_are_mutual_allies(p, o))
                continue;
            return o;
        }
    return -1;
}

/** An enemy creature (heroes, or a keeper not allied) within EXODUS_SIEGE_SLABS of the Heart: a siege, no setting off. */
#define EXODUS_SIEGE_SLABS 10
static TbBool enemy_near_heart(PlayerNumber p, const struct Thing *heart)
{
    const MapSubtlCoord r = EXODUS_SIEGE_SLABS * STL_PER_SLB;
    const MapSubtlCoord cx = heart->mappos.x.stl.num, cy = heart->mappos.y.stl.num;
    for (MapSubtlCoord sy = max(0, cy - r); (sy <= cy + r) && (sy < game.map_subtiles_y); sy++)
        for (MapSubtlCoord sx = max(0, cx - r); (sx <= cx + r) && (sx < game.map_subtiles_x); sx++)
        {
            long i = get_mapwho_thing_index(get_map_block_at(sx, sy));
            unsigned long guard = 0;
            while ((i != 0) && (guard++ < THINGS_COUNT))
            {
                const struct Thing *t = thing_get(i);
                if (thing_is_invalid(t))
                    break;
                i = t->next_on_mapblk;
                if (thing_is_creature(t) && (t->owner != p) && players_are_enemies(p, t->owner) &&
                    !creature_is_kept_in_custody(t) && !creature_is_being_unconscious(t) && !creature_is_dying(t))
                    return true;
            }
        }
    return false;
}

static void settle(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    /* not an escape: the Heart must still be above three quarters of its health, and not under siege */
    const HitPoints max_health = get_object_model_stats(heart->model)->health;
    if ((max_health > 0) && (heart->health * 4 < max_health * 3))
    {
        tell_keeper(p, "The Heart must be above three quarters of its health to set off.");
        return;
    }
    if (enemy_near_heart(p, heart))
    {
        tell_keeper(p, "Not with enemies at the Heart: drive them off first.");
        return;
    }
    const MapSlabCoord ox = subtile_slab(heart->mappos.x.stl.num), oy = subtile_slab(heart->mappos.y.stl.num);
    const MapSlabCoord nx = subtile_slab(stl_x), ny = subtile_slab(stl_y);
    const long dx = nx - ox, dy = ny - oy;
    if (dx * dx + dy * dy < (long)EXODUS_MIN_SLABS * EXODUS_MIN_SLABS)
    {
        tell_keeper(p, "The new home must be at least 40 slabs from the old one.");
        return;
    }
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            if (!open_for_heart(p, nx + i, ny + j))
            {
                tell_keeper(p, "The Heart needs open ground you have seen, three slabs by three, and no room there.");
                return;
            }
    /* shared worlds: no Heart set down on another keeper's doorstep (an ally's is fine) */
    if (other_keeper_near(p, nx, ny) >= 0)
    {
        tell_keeper(p, "Not so near another keeper's land: 20 slabs at least (an ally's is fine).");
        return;
    }
    /* the new pedestal, the Heart onto it */
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(nx + i), slab_subtile_center(ny + j), p, 0);
            replace_slab_from_script(nx + i, ny + j, SlbT_DUNGHEART);
        }
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(nx));
    pos.y.val = subtile_coord_center(slab_subtile_center(ny));
    pos.z.val = heart->mappos.z.val;
    move_thing_in_map(heart, &pos);
    reset_interpolation_of_thing(heart); /* a move, not a walk: no in-between frames */
    heart->health = max(1, heart->health / 2); /* the road wore it down */
    /* the old pedestal: ruins nobody owns */
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
        {
            replace_slab_from_script(ox + i, oy + j, SlbT_PATH);
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(ox + i), slab_subtile_center(oy + j), game.neutral_player_num, 0);
        }
    ex_on[p] = 0;
    SYNCLOG("World: player %d's Heart is set down %ld slabs from where it stood", (int)p, (long)sqrt((double)(dx * dx + dy * dy)));
    tell_keeper(p, "Your Heart is set down in its new home; the old one stands as ruins.");
}

void world_exodus_packet(PlayerNumber p, int what, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    if (what == 1)
    {
        if (ex_on[p])
        {
            ex_on[p] = 0;
            tell_keeper(p, "The Exodus is called off.");
            return;
        }
        const HitPoints max_health = get_object_model_stats(heart->model)->health;
        if ((max_health > 0) && (heart->health * 4 < max_health * 3))
        {
            tell_keeper(p, "The Heart must be above three quarters of its health to set off.");
            return;
        }
        ex_on[p] = 1;
        tell_keeper(p, "The Exodus is ordered: point at the new home and \"!settle\".");
        return;
    }
    if (what == 2)
    {
        if (!ex_on[p])
        {
            tell_keeper(p, "Order the Exodus first (\"!exodus\").");
            return;
        }
        settle(p, stl_x, stl_y);
    }
}

TbBool world_exodus_command(const char *text, MapCoord cursor_x, MapCoord cursor_y)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    int what;
    if (strcasecmp(text + 1, "exodus") == 0)
        what = 1;
    else if (strcasecmp(text + 1, "settle") == 0)
        what = 2;
    else
        return false;
    set_players_packet_action(get_my_player(), PckA_WorldExodus, (unsigned long)coord_subtile(cursor_x) | ((unsigned long)what << 28),
        (unsigned long)coord_subtile(cursor_y), 0, 0);
    return true;
}

/** KFX_WORLD_EXODUSTEST=turn (test switch): this machine's keeper tries to settle without the Exodus (refused), orders
 *  it, tries 10 slabs away (refused), then settles 42 slabs off (30 east, 30 south) on ground cleared and revealed for the test. */
void world_exodus_turn(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_EXODUSTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_EXODUSTEST=%ld", at);
    }
    {
        /* KFX_WORLD_EXODUSSPLIT=turn (test switch): this machine's keeper orders the Exodus at that turn and settles 400
           turns later (30,30 slabs off, ground cleared then), so a save and load can come between */
        static long split = -2;
        if (split == -2)
        {
            const char *v = getenv("KFX_WORLD_EXODUSSPLIT");
            split = (v != NULL) ? atol(v) : -1;
        }
        if ((split >= 0) && simulating() && (((long)game.play_gameturn == split) || ((long)game.play_gameturn == split + 400)))
        {
            const PlayerNumber q = my_player_number;
            const struct Thing *h = get_player_soul_container(q);
            if (thing_exists(h) && ((long)game.play_gameturn == split))
                world_exodus_packet(q, 1, 0, 0);
            else if (thing_exists(h))
            {
                const MapSlabCoord sx = subtile_slab(h->mappos.x.stl.num) + 30, sy = subtile_slab(h->mappos.y.stl.num) + 30;
                for (int j = -2; j <= 2; j++)
                    for (int i = -2; i <= 2; i++)
                        place_slab_type_on_map(SlbT_PATH, slab_subtile_center(sx + i), slab_subtile_center(sy + j), game.neutral_player_num, 0);
                reveal_map_area(q, slab_subtile(sx - 3, 0), slab_subtile(sx + 3, 2), slab_subtile(sy - 3, 0), slab_subtile(sy + 3, 2));
                SYNCLOG("World: exodus split test: settling, the Exodus ordered %d", (int)ex_on[q]);
                world_exodus_packet(q, 2, slab_subtile_center(sx), slab_subtile_center(sy));
            }
        }
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !simulating())
        return;
    /* this machine's keeper, or on a dedicated server the first human keeper with a Heart (a joined player) */
    PlayerNumber p = my_player_number;
    for (PlayerNumber q = 0; netstate_dedicated() && (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
        if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
        {
            p = q;
            break;
        }
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const MapSlabCoord tx = hx + 30, ty = hy + 30; /* 42 slabs off, well inside the loaded window */
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++)
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(tx + i), slab_subtile_center(ty + j), game.neutral_player_num, 0);
    reveal_map_area(p, slab_subtile(tx - 3, 0), slab_subtile(tx + 3, 2), slab_subtile(ty - 3, 0), slab_subtile(ty + 3, 2));
    world_exodus_packet(p, 2, slab_subtile_center(tx), slab_subtile_center(ty));
    world_exodus_packet(p, 1, 0, 0);
    world_exodus_packet(p, 2, slab_subtile_center(hx + 10), slab_subtile_center(hy));
    if (getenv("KFX_WORLD_EXODUSNEAR") != NULL)
    {
        /* KFX_WORLD_EXODUSNEAR=1 as well: another keeper's claimed slab 10 slabs from the target: refused; taken away
           again before the real settle */
        PlayerNumber o = -1;
        for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (o < 0); q++)
            if ((q != p) && (q != PLAYER_GOOD) && (q != PLAYER_NEUTRAL) && (q != game.neutral_player_num) && player_is_keeper(q)
                && !players_are_mutual_allies(p, q))
                o = q;
        if (o >= 0)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(tx - 10), slab_subtile_center(ty), o, 0);
            SYNCLOG("World: exodus test: player %d's land 10 slabs from the target; settling there:", (int)o);
            world_exodus_packet(p, 2, slab_subtile_center(tx), slab_subtile_center(ty));
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(tx - 10), slab_subtile_center(ty), game.neutral_player_num, 0);
        }
    }
    world_exodus_packet(p, 2, slab_subtile_center(tx), slab_subtile_center(ty));
    const struct Thing *h2 = get_player_soul_container(p);
    SYNCLOG("World: exodus test: the Heart is %ld,%ld slabs from where it stood; old pedestal slab kind %d owner %d",
        (long)(subtile_slab(h2->mappos.x.stl.num) - hx), (long)(subtile_slab(h2->mappos.y.stl.num) - hy),
        (int)get_slabmap_block(hx, hy)->kind, (int)slabmap_owner(get_slabmap_block(hx, hy)));
}

/******************************************************************************/
void world_exodus_reset(void)
{
    memset(ex_on, 0, sizeof(ex_on));
}

size_t world_exodus_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (ex_on[p])
            return 4 + sizeof(ex_on);
    return 0;
}

void world_exodus_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, ex_on, sizeof(ex_on));
}

TbBool world_exodus_load(const uint8_t *buf, size_t len)
{
    world_exodus_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (4 + sizeof(ex_on) > len))
        return false;
    memcpy(ex_on, buf + 4, sizeof(ex_on));
    return true;
}

void world_exodus_seat_reset(PlayerNumber p)
{
    if ((p >= 0) && (p < PLAYERS_MAX))
        ex_on[p] = 0;
}

#ifdef __cplusplus
}
#endif

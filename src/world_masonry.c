/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_masonry.c
 *     Endless world: backfill and masonry, imps putting ground back.
 * @par Purpose:
 *     See world_masonry.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_masonry.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <limits.h>

#include "world_stream.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "map_blocks.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "thing_navigate.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_terrain.h"
#include "config_creature.h"
#include "thing_physics.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "world_caretaker.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct MasonryOrder {
    uint8_t kind;           /* 0: free; WMo_Backfill, WMo_Masonry */
    uint8_t owner;
    uint16_t work;          /* imp-turns done */
    uint8_t told_wait;      /* the wait for a creature on the slab was logged (in what was padding) */
    uint8_t pad[3];
    int64_t wx, wy;         /* world slab */
    uint32_t sent_turn;     /* when an imp was last sent */
};
static struct MasonryOrder mo[MASONRY_ORDERS_MAX];

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

/** A word to the keeper wherever it plays (this machine, or the server's notice to its joined client). */
static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: masonry, to player %d: %s", (int)p, text);
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

static TbBool is_other_keeper(PlayerNumber owner, PlayerNumber p)
{
    return (owner != p) && (owner >= 0) && (owner < PLAYERS_COUNT) && (owner != PLAYER_GOOD) && (owner != PLAYER_NEUTRAL)
        && (owner != game.neutral_player_num) && player_is_keeper(owner);
}

/** Another keeper's land on the slab or among the eight round it? */
static TbBool near_other_keeper(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
            const struct SlabMap *slb = get_slabmap_block(x + dx, y + dy);
            if (!slabmap_block_invalid(slb) && is_other_keeper(slabmap_owner(slb), p))
                return true;
        }
    return false;
}

/** What may be ordered on this slab now; NULL when it may, else why not. */
static const char *refusal(PlayerNumber p, MapSlabCoord x, MapSlabCoord y, int kind)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return "There is nothing there.";
    if (near_other_keeper(p, x, y))
        return "Not on another keeper's land, nor right beside it.";
    if (!room_is_invalid(slab_room_get(x, y)))
        return "Not on a room.";
    const PlayerNumber owner = slabmap_owner(slb);
    if (kind == WMo_Backfill)
    {
        if ((slb->kind == SlbT_PATH) || ((slb->kind == SlbT_CLAIMED) && (owner == p)))
            return NULL;
        return "Backfill fills open floor: bare path, or your own claimed floor.";
    }
    if ((slb->kind == SlbT_CLAIMED) && (owner == p))
        return NULL;
    return "Masonry builds on your own claimed floor.";
}

/** Creatures standing on the slab: any (none may be walled in), and whether one is another keeper's. */
static int creatures_on(MapSlabCoord x, MapSlabCoord y, PlayerNumber p, TbBool *others)
{
    int n = 0;
    *others = false;
    for (MapSubtlCoord sy = slab_subtile(y, 0); sy <= slab_subtile(y, 2); sy++)
        for (MapSubtlCoord sx = slab_subtile(x, 0); sx <= slab_subtile(x, 2); sx++)
        {
            long i = get_mapwho_thing_index(get_map_block_at(sx, sy));
            unsigned long guard = 0;
            while ((i != 0) && (guard++ < THINGS_COUNT))
            {
                const struct Thing *t = thing_get(i);
                if (thing_is_invalid(t))
                    break;
                i = t->next_on_mapblk;
                if (!thing_is_creature(t))
                    continue;
                n++;
                if (t->owner != p)
                    *others = true;
            }
        }
    return n;
}

/** Another keeper's creature (not an ally's) within BACKFILL_KEEP_OFF slabs: filling bare path then might wall it in
 *  (a cave's way out), so a backfill there waits. */
#define BACKFILL_KEEP_OFF 8
static TbBool other_creature_near(MapSlabCoord x, MapSlabCoord y, PlayerNumber p)
{
    const MapSubtlCoord r = BACKFILL_KEEP_OFF * STL_PER_SLB;
    const MapSubtlCoord cx = slab_subtile_center(x), cy = slab_subtile_center(y);
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
                if (thing_is_creature(t) && is_other_keeper(t->owner, p) && !players_are_mutual_allies(p, t->owner))
                    return true;
            }
        }
    return false;
}

void world_masonry_order(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y, int kind)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p)
        || ((kind != WMo_Backfill) && (kind != WMo_Masonry)))
        return;
    const MapSlabCoord x = subtile_slab(stl_x), y = subtile_slab(stl_y);
    const char *why = refusal(p, x, y, kind);
    if (why != NULL)
    {
        tell_keeper(p, why);
        return;
    }
    int64_t wx, wy;
    world_slab_to_world(x, y, &wx, &wy);
    int mine = 0;
    struct MasonryOrder *free_o = NULL;
    for (int i = 0; i < MASONRY_ORDERS_MAX; i++)
    {
        if (mo[i].kind == 0)
        {
            if (free_o == NULL)
                free_o = &mo[i];
            continue;
        }
        if ((mo[i].wx == wx) && (mo[i].wy == wy) && (mo[i].owner != p))
        {
            tell_keeper(p, "Another keeper's imps have that slab in hand.");
            return;
        }
        if ((mo[i].wx == wx) && (mo[i].wy == wy))
        {
            mo[i].kind = 0; /* the same slab again: the order is taken back */
            tell_keeper(p, "Order taken back.");
            return;
        }
        mine += (mo[i].owner == p) ? 1 : 0;
    }
    if ((free_o == NULL) || (mine >= MASONRY_ORDERS_PER_KEEPER))
    {
        tell_keeper(p, "Your imps have enough to do: 32 orders at most.");
        return;
    }
    memset(free_o, 0, sizeof(*free_o));
    free_o->kind = (uint8_t)kind;
    free_o->owner = (uint8_t)p;
    free_o->wx = wx;
    free_o->wy = wy;
    tell_keeper(p, (kind == WMo_Backfill) ? "Backfill ordered." : "Masonry ordered (150 gold when the wall is built).");
}

TbBool world_masonry_command(const char *text, MapCoord cursor_x, MapCoord cursor_y)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    int kind;
    if (strcasecmp(text + 1, "backfill") == 0)
        kind = WMo_Backfill;
    else if (strcasecmp(text + 1, "masonry") == 0)
        kind = WMo_Masonry;
    else
        return false;
    /* the order's kind rides in the top bits of the first parameter (subtiles stay below 2^28) */
    set_players_packet_action(get_my_player(), PckA_WorldMasonry, (unsigned long)coord_subtile(cursor_x) | ((unsigned long)kind << 28),
        (unsigned long)coord_subtile(cursor_y), 0, 0);
    return true;
}

/** The imps of the keeper beside the slab (the eight round it, not on it). */
static int imps_beside(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    int n = 0;
    for (MapSubtlCoord sy = slab_subtile(y - 1, 0); sy <= slab_subtile(y + 1, 2); sy++)
        for (MapSubtlCoord sx = slab_subtile(x - 1, 0); sx <= slab_subtile(x + 1, 2); sx++)
        {
            if ((subtile_slab(sx) == x) && (subtile_slab(sy) == y))
                continue;
            long i = get_mapwho_thing_index(get_map_block_at(sx, sy));
            unsigned long guard = 0;
            while ((i != 0) && (guard++ < THINGS_COUNT))
            {
                const struct Thing *t = thing_get(i);
                if (thing_is_invalid(t))
                    break;
                i = t->next_on_mapblk;
                if (thing_is_creature(t) && (t->owner == p) && thing_is_creature_digger(t))
                    n++;
            }
        }
    return n;
}

/** Sends the keeper's nearest free imp to stand on open floor beside the slab. */
static void send_imp(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    static const signed char d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    MapSlabCoord tx = x, ty = y;
    TbBool found = false;
    for (int d = 0; (d < 4) && !found; d++)
    {
        const struct SlabMap *slb = get_slabmap_block(x + d4[d][0], y + d4[d][1]);
        if (!slabmap_block_invalid(slb) && (slb->kind >= SlbT_PATH) && !slab_kind_is_liquid(slb->kind) && !slab_is_wall(x + d4[d][0], y + d4[d][1]))
        {
            tx = x + d4[d][0];
            ty = y + d4[d][1];
            found = true;
        }
    }
    if (!found)
        return;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    struct Thing *best = INVALID_THING;
    long best_d = LONG_MAX;
    long guard = 0;
    for (long i = dungeon->digger_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *t = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (!thing_exists(t) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (thing_is_picked_up(t) || creature_is_dying(t) || (t->creature.gold_carried > 0))
            continue;
        const long dx = subtile_slab(t->mappos.x.stl.num) - tx, dy = subtile_slab(t->mappos.y.stl.num) - ty;
        const long d = dx * dx + dy * dy;
        if (d < best_d)
        {
            best_d = d;
            best = t;
        }
    }
    if (thing_is_invalid(best))
        return;
    if (setup_person_move_to_position(best, slab_subtile_center(tx), slab_subtile_center(ty), 0))
        best->continue_state = CrSt_ImpDoingNothing;
}

static void finish(struct MasonryOrder *o, MapSlabCoord x, MapSlabCoord y)
{
    const PlayerNumber p = o->owner;
    if (o->kind == WMo_Backfill)
    {
        place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
        SYNCLOG("World: player %d's imps backfill engine slab (%d,%d)", (int)p, (int)x, (int)y);
    } else
    {
        place_slab_type_on_map(SlbT_WALLDRAPE, slab_subtile_center(x), slab_subtile_center(y), p, 0);
        SYNCLOG("World: player %d's imps build a wall at engine slab (%d,%d) for %d gold", (int)p, (int)x, (int)y, MASONRY_PRICE);
    }
    o->kind = 0;
}

/** KFX_WORLD_MASONRYTEST=turn (test switch): this machine's keeper orders backfill on a path slab and masonry on a
 *  claimed slab dug next to its Heart's room (1000 gold given), and one order on another keeper's land is refused;
 *  the results are logged 600 turns later. */
static void masonry_test(void)
{
    static long at = -2;
    static MapSlabCoord bx, by, mx, my;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_MASONRYTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_MASONRYTEST=%ld", at);
    }
    if (at < 0)
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    if ((long)game.play_gameturn == at)
    {
        const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
        bx = hx + 3; by = hy + 4;
        mx = hx - 3; my = hy + 4;
        place_slab_type_on_map(SlbT_PATH, slab_subtile_center(bx), slab_subtile_center(by), game.neutral_player_num, 0);
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(mx), slab_subtile_center(my), p, 0);
        get_players_num_dungeon(p)->total_money_owned += 1000;
        world_masonry_order(p, slab_subtile_center(bx), slab_subtile_center(by), WMo_Backfill);
        world_masonry_order(p, slab_subtile_center(mx), slab_subtile_center(my), WMo_Masonry);
        world_masonry_order(p, slab_subtile_center(hx), slab_subtile_center(hy), WMo_Masonry); /* the Heart: a room */
        SYNCLOG("World: masonry test: orders given at (+3,+4) backfill and (-3,+4) masonry");
        if (getenv("KFX_WORLD_MASONRYREFUSE") != NULL)
        {
            /* KFX_WORLD_MASONRYREFUSE=1 as well: a slab of another keeper's land at (+8,+4) with path beside it at (+9,+4)
               and on it (+8,+4): both refused; then a creature of that keeper stands on the backfill slab at (+3,+4):
               that order must wait while it stands there */
            PlayerNumber other = -1;
            for (PlayerNumber o = 0; (o < PLAYERS_COUNT) && (other < 0); o++)
                if ((o != p) && is_other_keeper(o, p))
                    other = o;
            if (other < 0)
                SYNCLOG("World: masonry test: no other keeper in this world");
            else
            {
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + 8), slab_subtile_center(hy + 4), other, 0);
                place_slab_type_on_map(SlbT_PATH, slab_subtile_center(hx + 9), slab_subtile_center(hy + 4), game.neutral_player_num, 0);
                world_masonry_order(p, slab_subtile_center(hx + 9), slab_subtile_center(hy + 4), WMo_Backfill);
                world_masonry_order(p, slab_subtile_center(hx + 8), slab_subtile_center(hy + 4), WMo_Backfill);
                struct Coord3d pos;
                memset(&pos, 0, sizeof(pos));
                pos.x.val = subtile_coord_center(slab_subtile_center(hx + 3));
                pos.y.val = subtile_coord_center(slab_subtile_center(hy + 4));
                struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, "FLY"), other);
                if (!thing_is_invalid(c))
                    c->mappos.z.val = get_thing_height_at(c, &c->mappos);
                TbBool others = false;
                const int on = creatures_on(hx + 3, hy + 4, p, &others);
                SYNCLOG("World: masonry test: other keeper %d; its creature %s on the backfill slab at (+3,+4): creatures on it %d, another keeper's %d", (int)other,
                    thing_is_invalid(c) ? "could not be made" : "stands", on, (int)others);
            }
        }
    } else if ((long)game.play_gameturn == at + 600)
    {
        const struct SlabMap *b = get_slabmap_block(bx, by), *m = get_slabmap_block(mx, my);
        SYNCLOG("World: masonry test: backfill slab is now kind %d (earth %d), masonry slab kind %d owner %d (wall %d)",
            (int)b->kind, SlbT_EARTH, (int)m->kind, (int)slabmap_owner(m), (int)SlbT_WALLDRAPE);
    }
}

void world_masonry_turn(void)
{
    if (!simulating())
        return;
    masonry_test();
    if ((game.play_gameturn % 16) != 9)
        return;
    for (int i = 0; i < MASONRY_ORDERS_MAX; i++)
    {
        struct MasonryOrder *o = &mo[i];
        if (o->kind == 0)
            continue;
        MapSlabCoord x, y;
        if (!world_engine_slab(o->wx, o->wy, &x, &y))
            continue; /* not loaded: it waits */
        const PlayerNumber p = o->owner;
        if ((p < PLAYERS_COUNT) && world_caretaker_active(p))
            continue; /* away: the orders wait for the keeper (the caretaker's dungeon does not change) */
        if ((p >= PLAYERS_COUNT) || !world_player_is_human(p) || (refusal(p, x, y, o->kind) != NULL))
        {
            o->kind = 0; /* the ground changed, or its keeper is gone */
            continue;
        }
        const int imps = imps_beside(p, x, y);
        if (imps == 0)
        {
            if ((uint32_t)game.play_gameturn - o->sent_turn >= 64)
            {
                o->sent_turn = (uint32_t)game.play_gameturn;
                send_imp(p, x, y);
            }
            continue;
        }
        o->work = (uint16_t)min(65535, (int)o->work + 16 * imps);
        if (o->work < MASONRY_WORK_TURNS)
            continue;
        TbBool others;
        if (creatures_on(x, y, p, &others) > 0)
        {
            /* none may be walled in or buried: it waits */
            if (!o->told_wait)
                SYNCLOG("World: player %d's %s order waits: a creature stands on the slab (another keeper's: %d)", (int)p,
                    (o->kind == WMo_Backfill) ? "backfill" : "masonry", (int)others);
            o->told_wait = 1;
            continue;
        }
        if ((o->kind == WMo_Backfill) && (slabmap_owner(get_slabmap_block(x, y)) != p) && other_creature_near(x, y, p))
        {
            /* bare path: another keeper's creature nearby might be walled in by it; it waits until they are gone */
            if (o->told_wait != 2)
                SYNCLOG("World: player %d's backfill order waits: another keeper's creature is within %d slabs", (int)p,
                    BACKFILL_KEEP_OFF);
            o->told_wait = 2;
            continue;
        }
        if (o->kind == WMo_Masonry)
        {
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            if (dungeon_invalid(dungeon) || (dungeon->total_money_owned < MASONRY_PRICE))
                continue; /* it waits for the gold */
            take_money_from_dungeon(p, MASONRY_PRICE, 1);
        }
        finish(o, x, y);
    }
}

/******************************************************************************/
void world_masonry_reset(void)
{
    memset(mo, 0, sizeof(mo));
}

size_t world_masonry_save_size(void)
{
    int n = 0;
    for (int i = 0; i < MASONRY_ORDERS_MAX; i++)
        n += (mo[i].kind != 0) ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct MasonryOrder) : 0;
}

void world_masonry_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < MASONRY_ORDERS_MAX; i++)
    {
        if (mo[i].kind == 0)
            continue;
        memcpy(buf + at, &mo[i], sizeof(struct MasonryOrder));
        at += sizeof(struct MasonryOrder);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_masonry_load(const uint8_t *buf, size_t len)
{
    world_masonry_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct MasonryOrder) + 4 > len)
        return false;
    for (uint32_t i = 0; (i < n) && (i < MASONRY_ORDERS_MAX); i++)
        memcpy(&mo[i], buf + 4 + (size_t)i * sizeof(struct MasonryOrder), sizeof(struct MasonryOrder));
    return true;
}

#ifdef __cplusplus
}
#endif

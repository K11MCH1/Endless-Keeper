/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_watchfire.c
 *     Endless world: watchfires.
 * @par Purpose:
 *     See world_watchfire.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_watchfire.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <math.h>

#include "world_stream.h"
#include "world_daynight.h"
#include "world_misdeeds.h"
#include "world_caretaker.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_stats.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "net_main.h"
#include "net_state.h"
#include "server_admin.h"
#include "keeperfx.hpp"
#include "room_util.h"
#include "map_blocks.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct Watchfire {
    uint8_t used;
    uint8_t owner;
    uint8_t pad[2];
    int64_t wx, wy;          /* its slab, in world slabs (the engine window moves) */
    uint32_t until;          /* the turn it goes out */
    int32_t paid_night;      /* the last night it was paid for (a world day number) */
    ThingIndex obj;          /* its brazier (a stand-in object) */
    int32_t obj_born;
};
static struct Watchfire wf[WATCHFIRES_MAX];

static ThingModel fire_model(void)
{
    static ThingModel model = -1;
    if (model < 0)
        model = (ThingModel)get_id(object_desc, "CANDLESTCK"); /* stands in for a brazier until there is art */
    return model;
}

static TbBool lit(const struct Watchfire *f)
{
    return f->used && ((uint32_t)game.play_gameturn < f->until);
}

/** The lit fire whose light holds this engine slab (of this keeper, or any: owner < 0), NULL when none. */
static const struct Watchfire *fire_at(MapSlabCoord x, MapSlabCoord y, PlayerNumber owner)
{
    int64_t wx, wy;
    world_slab_to_world(x, y, &wx, &wy);
    for (int i = 0; i < WATCHFIRES_MAX; i++)
    {
        const struct Watchfire *f = &wf[i];
        if (!lit(f) || ((owner >= 0) && (f->owner != owner)))
            continue;
        const int64_t dx = wx - f->wx, dy = wy - f->wy;
        if (dx * dx + dy * dy <= (int64_t)WATCHFIRE_REACH * WATCHFIRE_REACH)
            return f;
    }
    return NULL;
}

TbBool world_watchfire_covers(MapSlabCoord x, MapSlabCoord y)
{
    return world_mode_active() && (fire_at(x, y, -1) != NULL);
}

TbBool world_watchfire_covers_for(PlayerNumber owner, MapSlabCoord x, MapSlabCoord y)
{
    return world_mode_active() && (owner >= 0) && (fire_at(x, y, owner) != NULL);
}

TbBool world_watchfire_push(MapSlabCoord *x, MapSlabCoord *y)
{
    if (!world_mode_active())
        return false;
    const struct Watchfire *f = fire_at(*x, *y, -1);
    if (f == NULL)
        return false;
    MapSlabCoord fx, fy;
    if (!world_engine_slab(f->wx, f->wy, &fx, &fy))
        return false;
    int dx = *x - fx, dy = *y - fy;
    if ((dx == 0) && (dy == 0))
        dx = 1;
    /* out along the same line, to one slab past the edge of the light */
    const double len = sqrt((double)dx * dx + (double)dy * dy);
    *x = fx + (MapSlabCoord)floor(dx * (WATCHFIRE_REACH + 1) / len + 0.5);
    *y = fy + (MapSlabCoord)floor(dy * (WATCHFIRE_REACH + 1) / len + 0.5);
    return true;
}

TbBool world_watchfire_unnerved(const struct Thing *creatng)
{
    if (!world_mode_active() || world_is_day() || !thing_exists(creatng) || (creatng->owner >= PLAYERS_COUNT)
        || !world_player_is_human(creatng->owner))
        return false;
    const struct Thing *heart = get_player_soul_container(creatng->owner);
    if (!thing_exists(heart))
        return false;
    const long dx = subtile_slab(creatng->mappos.x.stl.num) - subtile_slab(heart->mappos.x.stl.num);
    const long dy = subtile_slab(creatng->mappos.y.stl.num) - subtile_slab(heart->mappos.y.stl.num);
    if (dx * dx + dy * dy <= (long)WATCHFIRE_NERVE_SLABS * WATCHFIRE_NERVE_SLABS)
        return false;
    if (world_misdeeds_fearless_in_dark(creatng))
        return false;
    if (world_caretaker_active(creatng->owner))
        return false; /* an away keeper's dungeon under the caretaker gets no harder */
    return (fire_at(subtile_slab(creatng->mappos.x.stl.num), subtile_slab(creatng->mappos.y.stl.num), creatng->owner) == NULL);
}

static int workshops_of(PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return 0;
    int n = 0;
    long guard = 0;
    for (RoomIndex i = dungeon->room_list_start[RoK_WORKSHOP]; (i != 0) && (guard++ < ROOMS_COUNT);)
    {
        const struct Room *room = room_get(i);
        if (room_is_invalid(room))
            break;
        i = room->next_of_owner;
        n++;
    }
    return n;
}

static int fires_of(PlayerNumber p)
{
    int n = 0;
    for (int i = 0; i < WATCHFIRES_MAX; i++)
        n += (lit(&wf[i]) && (wf[i].owner == p)) ? 1 : 0;
    return n;
}

/** Tell the keeper who asked: on this machine when it is theirs, else (the server) a notice to the joined player. */
static void tell(PlayerNumber p, const char *text)
{
    SYNCLOG("World: watchfire, player %d: %s", (int)p, text);
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

/** Shared worlds: another keeper's land (not an ally's) within 20 slabs of a slab? */
static TbBool other_keeper_near(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    for (MapSlabCoord j = y - 20; j <= y + 20; j++)
        for (MapSlabCoord i = x - 20; i <= x + 20; i++)
        {
            const struct SlabMap *slb = get_slabmap_block(i, j);
            if (slabmap_block_invalid(slb))
                continue;
            const PlayerNumber o = slabmap_owner(slb);
            if ((o == p) || (o < 0) || (o >= PLAYERS_COUNT) || (o == PLAYER_GOOD) || (o == PLAYER_NEUTRAL)
                || (o == game.neutral_player_num) || !player_is_keeper(o) || players_are_mutual_allies(p, o))
                continue;
            return true;
        }
    return false;
}

void world_watchfire_place(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    const int shops = workshops_of(p);
    if (shops == 0)
    {
        tell(p, "A watchfire is built in a Workshop: you have none.");
        return;
    }
    if (fires_of(p) >= shops * WATCHFIRE_PER_WORKSHOP)
    {
        tell(p, "Your Workshops keep no more watchfires burning.");
        return;
    }
    const struct Map *mapblk = get_map_block_at(stl_x, stl_y);
    const struct SlabMap *slb = get_slabmap_for_subtile(stl_x, stl_y);
    if (map_block_invalid(mapblk) || slabmap_block_invalid(slb) || !map_block_revealed(mapblk, p))
    {
        tell(p, "A watchfire can only be set down where you have already seen.");
        return;
    }
    if ((slb->kind < SlbT_PATH) || slab_kind_is_liquid(slb->kind) || slab_is_wall(subtile_slab(stl_x), subtile_slab(stl_y)))
    {
        tell(p, "A watchfire needs open floor.");
        return;
    }
    /* shared worlds: not on another keeper's doorstep (an ally's is fine) */
    if (other_keeper_near(p, subtile_slab(stl_x), subtile_slab(stl_y)))
    {
        tell(p, "Not so near another keeper's land: 20 slabs at least (an ally's is fine).");
        return;
    }
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon) || (dungeon->total_money_owned < WATCHFIRE_PRICE))
    {
        tell(p, "A watchfire costs 300 gold.");
        return;
    }
    struct Watchfire *f = NULL;
    for (int i = 0; (f == NULL) && (i < WATCHFIRES_MAX); i++)
        if (!lit(&wf[i]))
            f = &wf[i];
    if (f == NULL)
        return;
    take_money_from_dungeon(p, WATCHFIRE_PRICE, 1);
    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->owner = (uint8_t)p;
    world_slab_to_world(subtile_slab(stl_x), subtile_slab(stl_y), &f->wx, &f->wy);
    f->until = (uint32_t)game.play_gameturn + (uint32_t)world_day_turns();
    f->paid_night = world_is_day() ? 0 : (int32_t)world_day_number(); /* set down at night: tonight is paid */
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(subtile_slab(stl_x)));
    pos.y.val = subtile_coord_center(slab_subtile_center(subtile_slab(stl_y)));
    pos.z.val = 0;
    pos.z.val = get_floor_height_at(&pos);
    struct Thing *obj = create_object(&pos, fire_model(), p, -1);
    if (!thing_is_invalid(obj))
    {
        f->obj = obj->index;
        f->obj_born = obj->creation_turn;
    }
    char text[96];
    snprintf(text, sizeof(text), "A watchfire burns for a day (%d of %d burning).", fires_of(p), shops * WATCHFIRE_PER_WORKSHOP);
    tell(p, text);
}

TbBool world_watchfire_command(const char *text, MapCoord cursor_x, MapCoord cursor_y)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strcasecmp(text + 1, "watchfire") != 0)
        return false;
    set_players_packet_action(get_my_player(), PckA_WorldWatchfire, (unsigned long)coord_subtile(cursor_x),
        (unsigned long)coord_subtile(cursor_y), 0, 0);
    return true;
}

/** The fire's brazier, found again by its place when its thing changed (a chunk that froze and came back makes a new
 *  thing); NULL when its ground isn't loaded or it is gone. */
static struct Thing *brazier_of(struct Watchfire *f, TbBool *loaded)
{
    MapSlabCoord ex, ey;
    *loaded = world_engine_slab(f->wx, f->wy, &ex, &ey);
    struct Thing *obj = thing_get(f->obj);
    if (thing_exists(obj) && (obj->creation_turn == f->obj_born) && (obj->class_id == TCls_Object) && (obj->model == fire_model()))
        return obj;
    if (!*loaded)
        return NULL;
    obj = find_base_thing_on_mapwho(TCls_Object, fire_model(), slab_subtile_center(ex), slab_subtile_center(ey));
    if (thing_is_invalid(obj) || (obj->owner != f->owner))
        return NULL;
    f->obj = obj->index;
    f->obj_born = obj->creation_turn;
    SYNCLOG("World: player %d's watchfire brazier found again by its place (thing %d)", (int)f->owner, (int)obj->index);
    return obj;
}

/* pad[0]: the fire is out and its brazier waits for its ground to be loaded, to be taken away */
#define WF_OUT_WAITING 1

static void put_out(struct Watchfire *f, const char *why)
{
    TbBool loaded = false;
    struct Thing *obj = brazier_of(f, &loaded);
    if (f->pad[0] != WF_OUT_WAITING)
        SYNCLOG("World: player %d's watchfire at world slab (%lld,%lld) goes out: %s", (int)f->owner, (long long)f->wx,
            (long long)f->wy, why);
    if (!thing_is_invalid(obj))
        destroy_object(obj);
    else if (!loaded)
    {
        /* its ground is frozen: the brazier is taken away when it comes back */
        if (f->pad[0] != WF_OUT_WAITING)
            SYNCLOG("World: the brazier waits for its ground to be loaded");
        f->pad[0] = WF_OUT_WAITING;
        f->until = 0;
        return;
    }
    if (f->pad[0] == WF_OUT_WAITING)
        SYNCLOG("World: player %d's out watchfire at world slab (%lld,%lld): its brazier is taken away", (int)f->owner,
            (long long)f->wx, (long long)f->wy);
    memset(f, 0, sizeof(*f));
}

/** KFX_WORLD_FIRETEST=turn (test switch): this machine's keeper gets 1000 gold and a fire 10 slabs east of its Heart
 *  (the ground revealed first); its light, a pack pushed out of it, and a creature's nerve are logged. */
static void fire_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_FIRETEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_FIRETEST=%ld", at);
    }
    if ((at < 0) || ((long)game.play_gameturn != at))
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
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    dungeon->total_money_owned += 1000;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const MapSlabCoord fx = hx + 10, fy = hy;
    if (workshops_of(p) == 0)
    {
        /* a 3x3 Workshop below the Heart, for the test */
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
            {
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1 + dx), slab_subtile_center(hy + 6 + dy), p, 0);
                replace_slab_from_script(hx - 1 + dx, hy + 6 + dy, SlbT_WORKSHOP);
            }
    }
    place_slab_type_on_map(SlbT_PATH, slab_subtile_center(fx), slab_subtile_center(fy), PLAYER_NEUTRAL, 0); /* open floor */
    reveal_map_area(p, slab_subtile(fx, 0), slab_subtile(fx, 2), slab_subtile(fy, 0), slab_subtile(fy, 2));
    const int before = fires_of(p);
    if (getenv("KFX_WORLD_FIRENEAR") != NULL)
    {
        /* KFX_WORLD_FIRENEAR=1 as well: another keeper's claimed slab 8 slabs from the spot first: refused; taken away
           again before the real placing */
        PlayerNumber o = -1;
        for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (o < 0); q++)
            if ((q != p) && (q != PLAYER_GOOD) && (q != PLAYER_NEUTRAL) && (q != game.neutral_player_num) && player_is_keeper(q)
                && !players_are_mutual_allies(p, q))
                o = q;
        if (o >= 0)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(fx + 8), slab_subtile_center(fy), o, 0);
            world_watchfire_place(p, slab_subtile_center(fx), slab_subtile_center(fy));
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(fx + 8), slab_subtile_center(fy), game.neutral_player_num, 0);
        }
    }
    world_watchfire_place(p, slab_subtile_center(fx), slab_subtile_center(fy));
    MapSlabCoord px = fx + 2, py = fy + 1;
    const TbBool pushed = world_watchfire_push(&px, &py);
    SYNCLOG("World: fire test: fires %d -> %d (Workshops %d); covers its slab %d, 6 east %d, 7 east %d; a pack at +2,+1 "
        "%s to %+d,%+d", before, fires_of(p), workshops_of(p), (int)world_watchfire_covers(fx, fy),
        (int)world_watchfire_covers(fx + 6, fy), (int)world_watchfire_covers(fx + 7, fy), pushed ? "pushed" : "not pushed",
        (int)(px - fx), (int)(py - fy));
}

/** KFX_WORLD_FIRERELINK=1 with KFX_WORLD_FIRETEST (test switch): 100 turns after the fire, its brazier is made again
 *  as a new thing; every 200 turns whether a brazier stands at the fire's slab is logged. */
static void fire_test_after(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_FIRERELINK");
        const char *t = getenv("KFX_WORLD_FIRETEST");
        at = ((v != NULL) && (t != NULL)) ? atol(t) : -1;
    }
    if ((at < 0) || (((long)game.play_gameturn - at) % 200 != 100) || ((long)game.play_gameturn < at))
        return;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord fx = subtile_slab(heart->mappos.x.stl.num) + 10, fy = subtile_slab(heart->mappos.y.stl.num);
    if ((long)game.play_gameturn == at + 100)
    {
        /* the brazier is made again as a new thing, as a chunk that froze and came back does; when the fire goes out,
           the new one must go */
        struct Thing *old_obj = find_base_thing_on_mapwho(TCls_Object, fire_model(), slab_subtile_center(fx), slab_subtile_center(fy));
        if (!thing_is_invalid(old_obj))
        {
            struct Coord3d pos = old_obj->mappos;
            destroy_object(old_obj);
            struct Thing *n = create_object(&pos, fire_model(), my_player_number, -1);
            SYNCLOG("World: fire test: the brazier is made again as thing %d", thing_is_invalid(n) ? -1 : (int)n->index);
        }
    }
    const struct Thing *obj = find_base_thing_on_mapwho(TCls_Object, fire_model(), slab_subtile_center(fx), slab_subtile_center(fy));
    SYNCLOG("World: fire test: fires burning %d, a brazier at the fire's slab %d", fires_of(my_player_number),
        (int)!thing_is_invalid(obj));
}

void world_watchfire_turn(void)
{
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()))
        return;
    fire_test();
    fire_test_after();
    if ((game.play_gameturn % 32) != 23)
        return;
    const int32_t night = world_is_day() ? 0 : (int32_t)world_day_number();
    for (int i = 0; i < WATCHFIRES_MAX; i++)
    {
        struct Watchfire *f = &wf[i];
        if (!f->used)
            continue;
        if (!lit(f))
        {
            put_out(f, "it has burnt for a day");
            continue;
        }
        if ((night != 0) && (f->paid_night != night) && world_caretaker_active(f->owner))
            f->paid_night = night; /* away: the caretaker's nights are free */
        if ((night != 0) && (f->paid_night != night))
        {
            /* a night of burning costs gold; unpaid, it goes out */
            struct Dungeon *dungeon = get_players_num_dungeon(f->owner);
            if (dungeon_invalid(dungeon) || (dungeon->total_money_owned < WATCHFIRE_NIGHT_PRICE))
            {
                put_out(f, "no gold for the night");
                continue;
            }
            take_money_from_dungeon(f->owner, WATCHFIRE_NIGHT_PRICE, 1);
            f->paid_night = night;
            SYNCLOG("World: player %d pays %d gold for a watchfire's night", (int)f->owner, WATCHFIRE_NIGHT_PRICE);
        }
        /* the ground in its light stays revealed (where it is loaded) */
        MapSlabCoord ex, ey;
        if (world_engine_slab(f->wx, f->wy, &ex, &ey))
            reveal_map_area(f->owner, slab_subtile(ex - WATCHFIRE_REACH, 0), slab_subtile(ex + WATCHFIRE_REACH, 2),
                slab_subtile(ey - WATCHFIRE_REACH, 0), slab_subtile(ey + WATCHFIRE_REACH, 2));
    }
}

void world_watchfire_seat_reset(PlayerNumber p)
{
    for (int i = 0; i < WATCHFIRES_MAX; i++)
        if (wf[i].used && (wf[i].owner == p) && (wf[i].pad[0] != WF_OUT_WAITING))
            put_out(&wf[i], "its keeper started again");
}

/******************************************************************************/
void world_watchfire_reset(void)
{
    memset(wf, 0, sizeof(wf));
}

size_t world_watchfire_save_size(void)
{
    int n = 0;
    for (int i = 0; i < WATCHFIRES_MAX; i++)
        n += wf[i].used ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct Watchfire) : 0;
}

void world_watchfire_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < WATCHFIRES_MAX; i++)
    {
        if (!wf[i].used)
            continue;
        memcpy(buf + at, &wf[i], sizeof(struct Watchfire));
        at += sizeof(struct Watchfire);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_watchfire_load(const uint8_t *buf, size_t len)
{
    world_watchfire_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct Watchfire) + 4 > len)
        return false;
    for (uint32_t i = 0; (i < n) && (i < WATCHFIRES_MAX); i++)
        memcpy(&wf[i], buf + 4 + (size_t)i * sizeof(struct Watchfire), sizeof(struct Watchfire));
    return true;
}

#ifdef __cplusplus
}
#endif

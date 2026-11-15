/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_falseheart.c
 *     Endless world: the false Heart, a decoy for raids.
 * @par Purpose:
 *     See world_falseheart.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_falseheart.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <math.h>

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
#include "room_util.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_creature.h"
#include "creature_control.h"
#include "creature_states_hero.h"
#include "creature_states.h"
#include "config_objects.h"
#include "config_terrain.h"
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
struct FalseHeart {
    uint8_t used;
    uint8_t lures;      /* raids it has drawn (in what was padding; older saves: 0) */
    uint8_t pad[2];
    int64_t wx, wy;
    ThingIndex obj;
    int32_t obj_born;
};
static struct FalseHeart fh[PLAYERS_MAX];

#define FH_LEADERS 64
struct FhLeader {
    ThingIndex idx;
    int32_t born;
    uint8_t keeper;     /* the keeper whose decoy it digs to */
    uint8_t pad[3];
};
static struct FhLeader fl[FH_LEADERS];

TbBool script_support_send_tunneller_to_dungeon_heart(struct Thing *creatng, PlayerNumber plyr_idx);

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static ThingModel decoy_model(void)
{
    static ThingModel model = -1;
    if (model < 0)
        model = (ThingModel)get_id(object_desc, "STATUE_HORNY"); /* stands in for a decoy Heart until there is art */
    return model;
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: false heart, to player %d: %s", (int)p, text);
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

static TbBool has_workshop(PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    return !dungeon_invalid(dungeon) && (dungeon->room_list_start[RoK_WORKSHOP] != 0);
}

/* decoys taken away while their ground was frozen: their statues go once it is loaded again (not saved) */
#define FH_GONE_MAX 64
struct FhGone {
    uint8_t used;
    uint8_t owner;
    uint8_t pad[6];
    int64_t wx, wy;
};
static struct FhGone fh_gone[FH_GONE_MAX];

/** The decoy's statue, found again by its place when its thing changed (a chunk that froze and came back makes a new
 *  thing); NULL when its ground isn't loaded (*loaded false) or it is gone. */
static struct Thing *statue_at(int64_t wx, int64_t wy, PlayerNumber owner, ThingIndex idx, int32_t born, TbBool *loaded)
{
    MapSlabCoord x, y;
    *loaded = world_engine_slab(wx, wy, &x, &y);
    struct Thing *obj = thing_get(idx);
    if ((idx != 0) && thing_exists(obj) && (obj->creation_turn == born) && (obj->class_id == TCls_Object) && (obj->model == decoy_model()))
        return obj;
    if (!*loaded)
        return NULL;
    obj = find_base_thing_on_mapwho(TCls_Object, decoy_model(), slab_subtile_center(x), slab_subtile_center(y));
    return (!thing_is_invalid(obj) && (obj->owner == owner)) ? obj : NULL;
}

static void remove_decoy(PlayerNumber p)
{
    struct FalseHeart *f = &fh[p];
    TbBool loaded = false;
    struct Thing *obj = f->used ? statue_at(f->wx, f->wy, p, f->obj, f->obj_born, &loaded) : NULL;
    if (obj != NULL)
        destroy_object(obj);
    else if (f->used && !loaded)
    {
        for (int i = 0; i < FH_GONE_MAX; i++)
            if (!fh_gone[i].used)
            {
                fh_gone[i].used = 1;
                fh_gone[i].owner = (uint8_t)p;
                fh_gone[i].wx = f->wx;
                fh_gone[i].wy = f->wy;
                SYNCLOG("World: player %d's false Heart is taken away; its statue goes when its ground is loaded", (int)p);
                break;
            }
    }
    memset(f, 0, sizeof(*f));
}

/** Statues of decoys taken away while their ground was frozen. */
static void clear_gone(void)
{
    for (int i = 0; i < FH_GONE_MAX; i++)
    {
        if (!fh_gone[i].used)
            continue;
        TbBool loaded = false;
        struct Thing *obj = statue_at(fh_gone[i].wx, fh_gone[i].wy, fh_gone[i].owner, 0, 0, &loaded);
        if (!loaded)
            continue;
        if (obj != NULL)
        {
            destroy_object(obj);
            SYNCLOG("World: player %d's old false Heart statue is taken away", (int)fh_gone[i].owner);
        }
        memset(&fh_gone[i], 0, sizeof(fh_gone[i]));
    }
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

void world_falseheart_place(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    if (!has_workshop(p))
    {
        tell_keeper(p, "A false Heart is built in a Workshop: you have none.");
        return;
    }
    const struct Map *mapblk = get_map_block_at(stl_x, stl_y);
    const struct SlabMap *slb = get_slabmap_for_subtile(stl_x, stl_y);
    if (map_block_invalid(mapblk) || slabmap_block_invalid(slb) || !map_block_revealed(mapblk, p))
    {
        tell_keeper(p, "A false Heart can only be set where you have already seen.");
        return;
    }
    if ((slb->kind < SlbT_PATH) || slab_kind_is_liquid(slb->kind) || slab_is_wall(subtile_slab(stl_x), subtile_slab(stl_y)))
    {
        tell_keeper(p, "A false Heart needs open floor.");
        return;
    }
    /* shared worlds: not on another keeper's doorstep (an ally's is fine) */
    if (other_keeper_near(p, subtile_slab(stl_x), subtile_slab(stl_y)))
    {
        tell_keeper(p, "Not so near another keeper's land: 20 slabs at least (an ally's is fine).");
        return;
    }
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon->total_money_owned < FALSEHEART_PRICE)
    {
        tell_keeper(p, "A false Heart costs 500 gold.");
        return;
    }
    take_money_from_dungeon(p, FALSEHEART_PRICE, 1);
    remove_decoy(p); /* one at a time: the old one goes */
    struct FalseHeart *f = &fh[p];
    f->used = 1;
    f->lures = 0; /* a new decoy: it has drawn none yet */
    world_slab_to_world(subtile_slab(stl_x), subtile_slab(stl_y), &f->wx, &f->wy);
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(subtile_slab(stl_x)));
    pos.y.val = subtile_coord_center(slab_subtile_center(subtile_slab(stl_y)));
    pos.z.val = 0;
    pos.z.val = get_floor_height_at(&pos);
    struct Thing *obj = create_object(&pos, decoy_model(), p, -1);
    if (!thing_is_invalid(obj))
    {
        f->obj = obj->index;
        f->obj_born = obj->creation_turn;
    }
    tell_keeper(p, "A false Heart stands: a raid that lands nearer to it than to your Heart may go for it first.");
}

TbBool world_falseheart_command(const char *text, MapCoord cursor_x, MapCoord cursor_y)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strcasecmp(text + 1, "falseheart") != 0)
        return false;
    set_players_packet_action(get_my_player(), PckA_WorldFalseHeart, (unsigned long)coord_subtile(cursor_x),
        (unsigned long)coord_subtile(cursor_y), 0, 0);
    return true;
}

static uint32_t fh_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

TbBool world_falseheart_lure(struct Thing *leader, PlayerNumber p, MapSlabCoord land_x, MapSlabCoord land_y, uint32_t salt)
{
    if (!simulating() || !thing_exists(leader) || (p < 0) || (p >= PLAYERS_MAX) || !fh[p].used)
        return false;
    MapSlabCoord dx_, dy_;
    if (!world_engine_slab(fh[p].wx, fh[p].wy, &dx_, &dy_))
        return false;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return false;
    const long hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const long dh = (land_x - hx) * (land_x - hx) + (land_y - hy) * (land_y - hy);
    const long dd = (land_x - dx_) * (land_x - dx_) + (land_y - dy_) * (land_y - dy_);
    if (dd >= dh)
        return false; /* the real Heart is nearer: no lure */
    if (fh[p].lures >= FALSEHEART_LURES)
        return false; /* spent: the raids have learned it (a raid that never reaches it does not smash it) */
    if ((int)(fh_hash(salt ^ (uint32_t)game.play_gameturn) % 100) < FALSEHEART_SEE_THROUGH)
    {
        SYNCLOG("World: a raid on player %d sees through its false Heart", (int)p);
        return false;
    }
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(dx_));
    pos.y.val = subtile_coord_center(slab_subtile_center(dy_));
    pos.z.val = 0;
    if (!send_tunneller_to_point_in_dungeon(leader, p, &pos))
        return false;
    for (int i = 0; i < FH_LEADERS; i++)
    {
        if (fl[i].idx != 0)
            continue;
        fl[i].idx = leader->index;
        fl[i].born = leader->creation_turn;
        fl[i].keeper = (uint8_t)p;
        break;
    }
    SYNCLOG("World: a raid on player %d is lured to its false Heart (%ld slabs from the landing, the Heart %ld)", (int)p,
        (long)sqrt((double)dd), (long)sqrt((double)dh));
    fh[p].lures++;
    if (fh[p].lures >= FALSEHEART_LURES)
        tell_keeper(p, "Your false Heart has drawn its last raid: word of it has spread, and it will fool no more.");
    return true;
}

/** KFX_WORLD_FALSEHEARTTEST=turn (test switch): this machine's keeper gets 1000 gold, a Workshop below its Heart if it
 *  has none, and a false Heart on an open slab 15 east of it; a raid comes 50 turns later (with KFX_WORLD_RAIDCHANCE). */
static void fh_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_FALSEHEARTTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_FALSEHEARTTEST=%ld", at);
    }
    if ((at < 0) || ((long)game.play_gameturn != at))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    get_players_num_dungeon(p)->total_money_owned += 1000;
    if (!has_workshop(p))
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
            {
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1 + dx), slab_subtile_center(hy + 6 + dy), p, 0);
                replace_slab_from_script(hx - 1 + dx, hy + 6 + dy, SlbT_WORKSHOP);
            }
    const char *dir = getenv("KFX_WORLD_FALSEHEARTDIR"); /* test switch: "x,y" slabs from the Heart (default 15,0) */
    int ox = 15, oy = 0;
    if (dir != NULL)
        sscanf(dir, "%d,%d", &ox, &oy);
    place_slab_type_on_map(SlbT_PATH, slab_subtile_center(hx + ox), slab_subtile_center(hy + oy), game.neutral_player_num, 0);
    reveal_map_area(p, slab_subtile(hx + ox, 0), slab_subtile(hx + ox, 2), slab_subtile(hy + oy, 0), slab_subtile(hy + oy, 2));
    if (getenv("KFX_WORLD_FHNEAR") != NULL)
    {
        /* KFX_WORLD_FHNEAR=1 as well: another keeper's claimed slab 8 slabs from the spot first: refused; taken away
           again before the real placing */
        PlayerNumber o = -1;
        for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (o < 0); q++)
            if ((q != p) && (q != PLAYER_GOOD) && (q != PLAYER_NEUTRAL) && (q != game.neutral_player_num) && player_is_keeper(q)
                && !players_are_mutual_allies(p, q))
                o = q;
        if (o >= 0)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + ox - 8), slab_subtile_center(hy + oy), o, 0);
            world_falseheart_place(p, slab_subtile_center(hx + ox), slab_subtile_center(hy + oy));
            place_slab_type_on_map(SlbT_PATH, slab_subtile_center(hx + ox - 8), slab_subtile_center(hy + oy), game.neutral_player_num, 0);
        }
    }
    world_falseheart_place(p, slab_subtile_center(hx + ox), slab_subtile_center(hy + oy));
    if (getenv("KFX_WORLD_FHRELINK") != NULL)
    {
        /* KFX_WORLD_FHRELINK=1 as well: the statue is made again as a new thing (as a chunk that froze and came back
           does), then the decoy is taken away: the new statue must go with it */
        struct Thing *old_obj = find_base_thing_on_mapwho(TCls_Object, decoy_model(), slab_subtile_center(hx + ox),
            slab_subtile_center(hy + oy));
        if (!thing_is_invalid(old_obj))
        {
            struct Coord3d pos = old_obj->mappos;
            destroy_object(old_obj);
            struct Thing *n = create_object(&pos, decoy_model(), p, -1);
            if (!thing_is_invalid(n))
                n->creation_turn += 1; /* as if made on another turn */
        }
        remove_decoy(p);
        const struct Thing *left = find_base_thing_on_mapwho(TCls_Object, decoy_model(), slab_subtile_center(hx + ox),
            slab_subtile_center(hy + oy));
        SYNCLOG("World: false heart test: statue made again and the decoy taken away: a statue left there %d",
            (int)!thing_is_invalid(left));
    }
}

void world_falseheart_turn(void)
{
    if (!simulating())
        return;
    fh_test();
    if ((game.play_gameturn % 64) == 35)
        clear_gone();
    if ((game.play_gameturn % 8) != 3)
        return;
    for (int i = 0; i < FH_LEADERS; i++)
    {
        struct FhLeader *l = &fl[i];
        if (l->idx == 0)
            continue;
        struct Thing *t = thing_get(l->idx);
        const PlayerNumber p = l->keeper;
        if (!thing_exists(t) || (t->creation_turn != l->born) || creature_is_dying(t) || !fh[p].used)
        {
            memset(l, 0, sizeof(*l));
            continue;
        }
        MapSlabCoord dx_, dy_;
        if (!world_engine_slab(fh[p].wx, fh[p].wy, &dx_, &dy_))
            continue;
        if ((abs(subtile_slab(t->mappos.x.stl.num) - dx_) > FALSEHEART_REACH) || (abs(subtile_slab(t->mappos.y.stl.num) - dy_) > FALSEHEART_REACH))
        {
            if ((getenv("KFX_WORLD_FALSEHEARTTEST") != NULL) && ((game.play_gameturn % 200) == 3))
                SYNCLOG("World: false heart test: a lured %s is %d,%d slabs from the decoy, state %d", thing_model_name(t),
                    (int)(subtile_slab(t->mappos.x.stl.num) - dx_), (int)(subtile_slab(t->mappos.y.stl.num) - dy_),
                    (int)t->active_state);
            continue;
        }
        /* there: the decoy is smashed, and the raid knows */
        remove_decoy(p);
        memset(l, 0, sizeof(*l));
        script_support_send_tunneller_to_dungeon_heart(t, p);
        SYNCLOG("World: a raid leader smashes player %d's false Heart and sets off for the real one", (int)p);
        tell_keeper(p, "Raiders smashed your false Heart: they know now, and come for the real one.");
    }
}

/******************************************************************************/
void world_falseheart_reset(void)
{
    memset(fh, 0, sizeof(fh));
    memset(fl, 0, sizeof(fl));
    memset(fh_gone, 0, sizeof(fh_gone));
}

size_t world_falseheart_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (fh[p].used)
            return 8 + sizeof(fh) + sizeof(fl);
    return 0;
}

void world_falseheart_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX, m = FH_LEADERS;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, &m, 4);
    memcpy(buf + 8, fh, sizeof(fh));
    memcpy(buf + 8 + sizeof(fh), fl, sizeof(fl));
}

TbBool world_falseheart_load(const uint8_t *buf, size_t len)
{
    world_falseheart_reset();
    uint32_t n = 0, m = 0;
    if (len < 8)
        return false;
    memcpy(&n, buf, 4);
    memcpy(&m, buf + 4, 4);
    if (8 + (size_t)n * sizeof(struct FalseHeart) + (size_t)m * sizeof(struct FhLeader) > len)
        return false;
    memcpy(fh, buf + 8, sizeof(struct FalseHeart) * (size_t)min((uint32_t)PLAYERS_MAX, n));
    memcpy(fl, buf + 8 + (size_t)n * sizeof(struct FalseHeart), sizeof(struct FhLeader) * (size_t)min((uint32_t)FH_LEADERS, m));
    return true;
}

void world_falseheart_seat_reset(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    if (fh[p].used)
        remove_decoy(p);
    for (int i = 0; i < FH_LEADERS; i++)
        if ((fl[i].idx != 0) && (fl[i].keeper == p))
            memset(&fl[i], 0, sizeof(fl[i]));
}

#ifdef __cplusplus
}
#endif

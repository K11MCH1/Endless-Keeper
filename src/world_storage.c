/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_storage.c
 *     Endless world: the Storage Room, where a keeper's loot is kept.
 * @par Purpose:
 *     See world_storage.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_storage.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "world_stream.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_navigate.h"
#include "thing_stats.h"
#include "power_hand.h"
#include "config_terrain.h"
#include "config_objects.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct LootEntry {
    ThingIndex idx;        /* 0: free */
    int32_t born;          /* the object's creation turn: an index reused by another thing is not loot */
    uint8_t owner;         /* the keeper it belongs to (PLAYER_NEUTRAL: nobody's yet) */
    uint8_t sub_x, sub_y;  /* where it lay last seen (its subtile within its slab, and below its world slab): in a */
    uint8_t pad;           /* chunk that freezes and comes back it is a new thing, found again there (2026-11-13) */
    ThingModel model;
    int64_t wx, wy;
};

static void note_place(struct LootEntry *e, const struct Thing *t)
{
    e->model = t->model;
    world_slab_to_world(subtile_slab(t->mappos.x.stl.num), subtile_slab(t->mappos.y.stl.num), &e->wx, &e->wy);
    e->sub_x = (uint8_t)(t->mappos.x.stl.num % STL_PER_SLB);
    e->sub_y = (uint8_t)(t->mappos.y.stl.num % STL_PER_SLB);
}
static struct LootEntry loot[WORLD_LOOT_MAX];

/** The STORAGE room kind when the configuration has one (0: none). */
static RoomKind storage_kind(void)
{
    static int kind = -2;
    if (kind == -2)
    {
        const long k = get_id(room_desc, "STORAGE");
        kind = (k > 0) ? (int)k : 0;
    }
    return (RoomKind)kind;
}

static struct LootEntry *entry_of(const struct Thing *thing)
{
    if (!thing_exists(thing))
        return NULL;
    for (int i = 0; i < WORLD_LOOT_MAX; i++)
        if ((loot[i].idx == thing->index) && (loot[i].born == thing->creation_turn))
            return &loot[i];
    return NULL;
}

TbBool world_storage_is_loot(const struct Thing *thing, PlayerNumber *owner)
{
    const struct LootEntry *e = entry_of(thing);
    if ((e != NULL) && (owner != NULL))
        *owner = e->owner;
    return (e != NULL);
}

void world_storage_forget(const struct Thing *thing)
{
    struct LootEntry *e = entry_of(thing);
    if (e != NULL)
        memset(e, 0, sizeof(*e));
}

static struct Room *heart_room(PlayerNumber p)
{
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return NULL;
    struct Room *room = get_room_thing_is_on(heart);
    return (room_is_invalid(room) || (room->owner != p)) ? NULL : room;
}

/** The slabs loot is kept on: a Storage Room's own slabs; for the Heart's room (in a world only the Heart's pedestal,
 *  where objects cannot lie), the keeper's own claimed floor in the ring of 16 slabs around it. */
#define STORAGE_SLOTS_MAX 1024
static int storage_slots(const struct Room *room, MapSlabCoord *xs, MapSlabCoord *ys)
{
    int n = 0;
    if (room->kind != RoK_DUNGHEART)
    {
        long guard = 0;
        for (SlabCodedCoords i = room->slabs_list; (i != 0) && (guard++ < 4096) && (n < STORAGE_SLOTS_MAX); i = get_next_slab_number_in_room(i))
        {
            xs[n] = slb_num_decode_x(i);
            ys[n] = slb_num_decode_y(i);
            n++;
        }
        return n;
    }
    const struct Thing *heart = get_player_soul_container(room->owner);
    if (!thing_exists(heart))
        return 0;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    for (int dy = -2; dy <= 2; dy++)
    {
        for (int dx = -2; dx <= 2; dx++)
        {
            if ((abs(dx) <= 1) && (abs(dy) <= 1))
                continue;
            const struct SlabMap *slb = get_slabmap_block(hx + dx, hy + dy);
            if (slabmap_block_invalid(slb) || (slb->kind != SlbT_CLAIMED) || (slabmap_owner(slb) != room->owner))
                continue;
            xs[n] = hx + dx;
            ys[n] = hy + dy;
            n++;
        }
    }
    return n;
}

/** Is there a loot item on this slab (other than `except`)? */
static TbBool slab_has_loot(MapSlabCoord x, MapSlabCoord y, const struct Thing *except)
{
    for (int i = 0; i < WORLD_LOOT_MAX; i++)
    {
        if (loot[i].idx == 0)
            continue;
        const struct Thing *t = thing_get(loot[i].idx);
        if (!thing_exists(t) || (t == except) || (t->creation_turn != loot[i].born) || thing_is_picked_up(t))
            continue;
        if ((subtile_slab(t->mappos.x.stl.num) == x) && (subtile_slab(t->mappos.y.stl.num) == y))
            return true;
    }
    return false;
}

static MapSlabCoord slot_x[STORAGE_SLOTS_MAX], slot_y[STORAGE_SLOTS_MAX];

static int room_loot(const struct Room *room, int *capacity)
{
    const int n = storage_slots(room, slot_x, slot_y);
    int used = 0;
    for (int k = 0; k < n; k++)
        used += slab_has_loot(slot_x[k], slot_y[k], NULL) ? 1 : 0;
    *capacity = n;
    return used;
}

struct Room *world_storage_room(PlayerNumber p)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_COUNT))
        return NULL;
    const RoomKind kind = storage_kind();
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    struct Room *best = NULL;
    int best_spare = -1;
    if ((kind > 0) && !dungeon_invalid(dungeon))
    {
        long guard = 0;
        for (RoomIndex i = dungeon->room_list_start[kind]; (i != 0) && (guard++ < ROOMS_COUNT);)
        {
            struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            int cap = 0;
            const int used = room_loot(room, &cap);
            if (cap - used > best_spare)
            {
                best = room;
                best_spare = cap - used;
            }
        }
    }
    return (best != NULL) ? best : heart_room(p);
}

int world_storage_count(PlayerNumber p, int *capacity)
{
    *capacity = 0;
    const struct Room *room = world_storage_room(p);
    if (room == NULL)
        return 0;
    return room_loot(room, capacity);
}

static TbBool in_storage(const struct Thing *thing, const struct Room *room)
{
    if ((room == NULL) || !thing_exists(thing))
        return false;
    const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num), y = subtile_slab(thing->mappos.y.stl.num);
    const int n = storage_slots(room, slot_x, slot_y);
    for (int k = 0; k < n; k++)
        if ((slot_x[k] == x) && (slot_y[k] == y))
            return true;
    return false;
}

TbBool world_storage_put(struct Thing *thing, PlayerNumber p)
{
    struct Room *room = world_storage_room(p);
    if ((room == NULL) || !thing_exists(thing) || thing_is_picked_up(thing))
        return false;
    if (in_storage(thing, room) && !slab_has_loot(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num), thing))
        return true;
    const int n = storage_slots(room, slot_x, slot_y);
    for (int k = 0; k < n; k++)
    {
        if (slab_has_loot(slot_x[k], slot_y[k], thing))
            continue;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(slot_x[k]));
        pos.y.val = subtile_coord_center(slab_subtile_center(slot_y[k]));
        pos.z.val = 0;
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(thing, &pos);
        return true;
    }
    return false;
}

TbBool world_storage_add_loot(struct Thing *thing, PlayerNumber p)
{
    if (!thing_exists(thing))
        return false;
    struct LootEntry *e = entry_of(thing);
    for (int i = 0; (e == NULL) && (i < WORLD_LOOT_MAX); i++)
        if (loot[i].idx == 0)
            e = &loot[i];
    if (e == NULL)
    {
        SYNCLOG("World: storage: the loot registry is full; the %s is not loot", thing_model_name(thing));
        return false;
    }
    e->idx = thing->index;
    e->born = thing->creation_turn;
    e->owner = (uint8_t)((p >= 0) && (p < PLAYERS_MAX) ? p : PLAYER_NEUTRAL);
    if ((p >= 0) && (p < PLAYERS_COUNT) && (p != PLAYER_NEUTRAL))
        world_storage_put(thing, p);
    note_place(e, thing);
    return true;
}

/** KFX_WORLD_STORAGETEST=turn (test switch): at that turn 20 loot items (a statue object stands in for loot that
 *  has no art yet) appear beside this machine's keeper's Heart; their keeper's storage takes what it can; the counts
 *  are logged then, and again 300 turns later. */
static void storage_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_STORAGETEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_STORAGETEST=%ld", at);
    }
    if ((at >= 0) && ((long)game.play_gameturn == at + 50) && (getenv("KFX_WORLD_STORERELINK") != NULL))
    {
        /* KFX_WORLD_STORERELINK=1 as well: the first loot item is made again as a new thing where it lies, as a chunk
           that froze and came back does; after the next upkeep it must still be on the list */
        for (int i = 0; i < WORLD_LOOT_MAX; i++)
        {
            struct Thing *t = thing_get(loot[i].idx);
            if ((loot[i].idx == 0) || !thing_exists(t))
                continue;
            struct Coord3d pos = t->mappos;
            const ThingModel m = t->model;
            const PlayerNumber own = t->owner;
            destroy_object(t);
            struct Thing *n = create_object(&pos, m, own, -1);
            if (!thing_is_invalid(n))
                n->creation_turn += 1;
            SYNCLOG("World: storage test: loot item %d made again as thing %d", i, thing_is_invalid(n) ? -1 : (int)n->index);
            break;
        }
    }
    if ((at < 0) || (((long)game.play_gameturn != at) && ((long)game.play_gameturn != at + 300)))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    if ((long)game.play_gameturn == at)
    {
        const ThingModel model = (ThingModel)get_id(object_desc, "STATUEWO");
        int made = 0, stored = 0;
        for (int i = 0; i < 20; i++)
        {
            struct Coord3d pos = heart->mappos;
            pos.y.val += (6 + i % 3) * COORD_PER_STL;
            pos.x.val += (i % 5 - 2) * COORD_PER_STL;
            struct Thing *t = create_object(&pos, model, p, -1);
            if (thing_is_invalid(t))
                continue;
            made++;
            world_storage_add_loot(t, p);
            stored += in_storage(t, world_storage_room(p)) ? 1 : 0;
        }
        int cap = 0;
        const int used = world_storage_count(p, &cap);
        const struct Room *room = world_storage_room(p);
        SYNCLOG("World: storage test: %d loot items made, %d went into %s (%d of %d slabs hold loot)", made, stored,
            (room != NULL) ? room_code_name(room->kind) : "nowhere", used, cap);
    } else
    {
        int cap = 0, n = 0;
        const int used = world_storage_count(p, &cap);
        for (int i = 0; i < WORLD_LOOT_MAX; i++)
            n += (loot[i].idx != 0) ? 1 : 0;
        SYNCLOG("World: storage test: later, %d loot items registered, %d of %d storage slabs hold loot", n, used, cap);
        char where[512];
        int w = 0;
        where[0] = '\0';
        for (int i = 0; (i < WORLD_LOOT_MAX) && (w < (int)sizeof(where) - 40); i++)
        {
            if (loot[i].idx == 0)
                continue;
            const struct Thing *t = thing_get(loot[i].idx);
            if (!thing_exists(t))
                continue;
            w += snprintf(where + w, sizeof(where) - w, " (%d,%d%s%s)",
                (int)(subtile_slab(t->mappos.x.stl.num) - subtile_slab(heart->mappos.x.stl.num)),
                (int)(subtile_slab(t->mappos.y.stl.num) - subtile_slab(heart->mappos.y.stl.num)),
                thing_is_picked_up(t) ? " held" : "", (t->creation_turn != loot[i].born) ? " other" : "");
        }
        SYNCLOG("World: storage test: loot by slab from the Heart:%s", where);
    }
}

void world_storage_turn(void)
{
    if (!world_mode_active())
        return;
    storage_test();
    if ((game.play_gameturn % 256) != 71)
        return;
    /* the registry's upkeep: gone things leave it; a keeper's loot on its own ground is brought into storage */
    for (int i = 0; i < WORLD_LOOT_MAX; i++)
    {
        if (loot[i].idx == 0)
            continue;
        struct Thing *t = thing_get(loot[i].idx);
        if (!thing_exists(t) || (t->creation_turn != loot[i].born) || (t->class_id != TCls_Object))
        {
            /* frozen with its chunk (it waits), back as a new thing (found where it lay), or gone */
            MapSlabCoord ex, ey;
            if ((loot[i].model > 0) && !world_engine_slab(loot[i].wx, loot[i].wy, &ex, &ey))
                continue;
            t = (loot[i].model > 0) ? find_base_thing_on_mapwho(TCls_Object, loot[i].model, slab_subtile(ex, loot[i].sub_x),
                slab_subtile(ey, loot[i].sub_y)) : INVALID_THING;
            if (thing_is_invalid(t) || (entry_of(t) != NULL))
            {
                memset(&loot[i], 0, sizeof(loot[i]));
                continue;
            }
            loot[i].idx = t->index;
            loot[i].born = t->creation_turn;
            SYNCLOG("World: storage: a loot item is found again where it lay");
        }
        if (!thing_is_picked_up(t))
            note_place(&loot[i], t);
        const PlayerNumber p = loot[i].owner;
        if ((p >= PLAYERS_COUNT) || (p == PLAYER_NEUTRAL) || thing_is_picked_up(t))
            continue;
        const struct SlabMap *slb = get_slabmap_for_subtile(t->mappos.x.stl.num, t->mappos.y.stl.num);
        if (slabmap_block_invalid(slb) || (slabmap_owner(slb) != p))
            continue; /* out in the world: it is brought home by hand */
        struct Room *room = world_storage_room(p);
        if ((room != NULL) && !in_storage(t, room))
        {
            world_storage_put(t, p);
            note_place(&loot[i], t);
        }
    }
}

/******************************************************************************/
void world_storage_reset(void)
{
    memset(loot, 0, sizeof(loot));
}

size_t world_storage_save_size(void)
{
    int n = 0;
    for (int i = 0; i < WORLD_LOOT_MAX; i++)
        n += (loot[i].idx != 0) ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct LootEntry) : 0;
}

void world_storage_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < WORLD_LOOT_MAX; i++)
    {
        if (loot[i].idx == 0)
            continue;
        memcpy(buf + at, &loot[i], sizeof(struct LootEntry));
        at += sizeof(struct LootEntry);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_storage_load(const uint8_t *buf, size_t len)
{
    world_storage_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct LootEntry) + 4 > len)
        return false;
    for (uint32_t i = 0; (i < n) && (i < WORLD_LOOT_MAX); i++)
        memcpy(&loot[i], buf + 4 + (size_t)i * sizeof(struct LootEntry), sizeof(struct LootEntry));
    return true;
}

#ifdef __cplusplus
}
#endif

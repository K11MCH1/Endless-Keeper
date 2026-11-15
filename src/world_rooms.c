/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rooms.c
 *     Endless world: what a room gains from where and how it is built.
 * @par Purpose:
 *     See world_rooms.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_rooms.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "world_stream.h"
#include "world_gen.h"
#include "game_legacy.h"
#include "player_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_stats.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "keeperfx.hpp"
#include "player_instances.h"
#include "gui_msgs.h"
#include "dungeon_data.h"
#include "room_util.h"
#include "map_blocks.h"
#include "thing_list.h"
#include "creature_states_lair.h"
#include "thing_creature.h"
#include "creature_control.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* Neighbours (the night of 2026-11-13): six pairs. Each pair gives one room an effect when it shares a wall with the
   other: two pull against the rest (a Graveyard beside a Lair, a Training Room beside a Library). */
struct NeighbourPair {
    RoomKind room;          /* the room that gains */
    RoomKind beside;        /* ... when it touches this one */
    enum WorldRoomMod what;
    int pct;
    TbBool not_vampires;    /* a Vampire doesn't mind it */
    const char *line;
};
static const struct NeighbourPair pairs[] = {
    {RoK_TORTURE,  RoK_PRISON,    WRM_Torture,  25, false, "a Torture Chamber beside the Prison breaks captives sooner"},
    {RoK_LIBRARY,  RoK_TEMPLE,    WRM_Work,     20, false, "a Library beside the Temple reads forbidden things sooner"},
    {RoK_LAIR,     RoK_GARDEN,    WRM_LairHeal, 25, false, "a Lair beside a Hatchery: sleepers wake fed and mend faster"},
    {RoK_LAIR,     RoK_GRAVEYARD, WRM_LairHeal,-25, true,  "a Lair beside a Graveyard: everyone but Vampires sleeps badly"},
    {RoK_WORKSHOP, RoK_TREASURE,  WRM_Work,     15, false, "a Workshop beside the Treasury: gold at hand, work goes faster"},
    {RoK_LIBRARY,  RoK_TRAINING,  WRM_Work,    -15, false, "a Library beside a Training Room: the din slows the reading"},
};
#define NEIGHBOUR_PAIRS ((int)(sizeof(pairs) / sizeof(pairs[0])))
#define NEIGHBOUR_TURNS 128

static uint32_t nb_mask[ROOMS_COUNT];
static uint32_t nb_turn[ROOMS_COUNT]; /* 1 + the turn the mask was worked out (0: never) */
static uint32_t nb_key[ROOMS_COUNT];
static int8_t nb_biome[ROOMS_COUNT];  /* Lie of the Land: the biome under the room's first slab (-1: none) */
#define NB_WATER_BIT 31                /* in nb_mask: the room touches water */  /* the room it was worked out for (kind, slabs): an index reused by another room is not */

static uint32_t room_key(const struct Room *room)
{
    return ((uint32_t)room->kind << 16) | (uint32_t)room->slabs_count;
}

static uint8_t sh_shape[ROOMS_COUNT];
static uint32_t sh_turn[ROOMS_COUNT];
static uint32_t sh_key[ROOMS_COUNT];

void world_rooms_reset(void)
{
    memset(nb_mask, 0, sizeof(nb_mask));
    memset(nb_turn, 0, sizeof(nb_turn));
    memset(sh_shape, 0, sizeof(sh_shape));
    memset(sh_turn, 0, sizeof(sh_turn));
    memset(sh_key, 0, sizeof(sh_key));
    memset(nb_key, 0, sizeof(nb_key));
    memset(nb_biome, -1, sizeof(nb_biome));
}

static uint32_t work_out_neighbours(const struct Room *room)
{
    uint32_t mask = 0;
    static const signed char dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    long guard = 0;
    for (SlabCodedCoords i = room->slabs_list; (i != 0) && (guard++ < 4096); i = get_next_slab_number_in_room(i))
    {
        const MapSlabCoord x = slb_num_decode_x(i), y = slb_num_decode_y(i);
        for (int d = 0; d < 4; d++)
        {
            const struct SlabMap *nslb = get_slabmap_block(x + dirs[d][0], y + dirs[d][1]);
            if (!slabmap_block_invalid(nslb) && (nslb->kind == SlbT_WATER))
                mask |= 1u << NB_WATER_BIT;
            const struct Room *other = slab_room_get(x + dirs[d][0], y + dirs[d][1]);
            if (room_is_invalid(other) || (other == room) || (other->owner != room->owner) || (other->kind >= 32))
                continue;
            mask |= 1u << other->kind;
        }
    }
    return mask;
}

int world_room_biome(const struct Room *room)
{
    if (room_is_invalid(room) || (room->index <= 0) || (room->index >= ROOMS_COUNT))
        return -1;
    static int forced = -2; /* KFX_WORLD_ROOMBIOME=n (test switch): every room stands in biome n */
    if (forced == -2)
    {
        const char *v = getenv("KFX_WORLD_ROOMBIOME");
        forced = (v != NULL) ? atoi(v) : -1;
        if (forced >= 0)
            SYNCLOG("World: test switch KFX_WORLD_ROOMBIOME=%d (%s)", forced, wg_biome_name(forced));
    }
    if (forced >= 0)
        return forced;
    world_room_neighbour_kinds(room); /* worked out with the neighbours */
    return nb_biome[room->index];
}

uint32_t world_room_neighbour_kinds(const struct Room *room)
{
    if (room_is_invalid(room) || (room->index <= 0) || (room->index >= ROOMS_COUNT))
        return 0;
    const RoomIndex r = room->index;
    const uint32_t now = (uint32_t)game.play_gameturn;
    if ((nb_turn[r] == 0) || (nb_turn[r] - 1 > now) || (now - (nb_turn[r] - 1) >= NEIGHBOUR_TURNS) || (nb_key[r] != room_key(room)))
    {
        nb_mask[r] = work_out_neighbours(room);
        nb_turn[r] = now + 1;
        nb_biome[r] = -1;
        if (room->slabs_list != 0)
        {
            const SlabCodedCoords first = room->slabs_list;
            const int b = world_biome_at_subtile(slab_subtile_center(slb_num_decode_x(first)), slab_subtile_center(slb_num_decode_y(first)));
            nb_biome[r] = (int8_t)(((b >= 0) && (b < WgB_COUNT)) ? b : -1);
        }
        nb_key[r] = room_key(room);
    }
    return nb_mask[r];
}

static int neighbour_pct(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng, int *malus)
{
    int bonus = 0;
    uint32_t mask = 0;
    TbBool mask_known = false;
    for (int k = 0; k < NEIGHBOUR_PAIRS; k++)
    {
        const struct NeighbourPair *pr = &pairs[k];
        if ((pr->room != room->kind) || (pr->what != what))
            continue;
        if (!mask_known)
        {
            mask = world_room_neighbour_kinds(room);
            mask_known = true;
        }
        if (!(mask & (1u << pr->beside)))
            continue;
        static ThingModel vampire = -1;
        if (vampire < 0)
            vampire = (ThingModel)get_id(creature_desc, "VAMPIRE");
        if (pr->not_vampires && (creatng != NULL) && thing_exists(creatng) && (creatng->model == vampire))
            continue;
        if (pr->pct >= 0)
            bonus += pr->pct;
        else
            *malus += pr->pct;
    }
    return bonus;
}

/******************************************************************************/
/* The Shape of a Room (the night of 2026-11-13): how a room is cut gives it a character. Worked out from the map
   every 128 turns, like the neighbours. */
static const char *const shape_names[WRS_COUNT] = {"", "a great hall", "a set of cells", "a gallery", "a ring",
    "a library of alcoves", "an open reading floor", "a vault"};
static const char *const shape_lines[WRS_COUNT] = {"",
    "full, its sleepers heal faster",
    "",
    "creatures that shoot train faster",
    "creatures that fight hand to hand train faster",
    "mastery research goes faster",
    "new research goes faster",
    ""};
static enum WorldRoomShape work_out_shape(const struct Room *room)
{
    MapSlabCoord x1 = INT32_MAX, y1 = INT32_MAX, x2 = INT32_MIN, y2 = INT32_MIN; /* engine slabs run far past 32767 in a world */
    int count = 0, dead_ends = 0, doors = 0, open = 0;
    static const signed char dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    long guard = 0;
    for (SlabCodedCoords i = room->slabs_list; (i != 0) && (guard++ < 4096); i = get_next_slab_number_in_room(i))
    {
        const MapSlabCoord x = slb_num_decode_x(i), y = slb_num_decode_y(i);
        x1 = min(x1, x); y1 = min(y1, y); x2 = max(x2, x); y2 = max(y2, y);
        count++;
        int inside = 0;
        for (int d = 0; d < 4; d++)
        {
            const MapSlabCoord nx = x + dirs[d][0], ny = y + dirs[d][1];
            if (slab_room_get(nx, ny) == room)
            {
                inside++;
                continue;
            }
            if (room->kind != RoK_TREASURE)
                continue;
            const struct SlabMap *slb = get_slabmap_block(nx, ny);
            if (slabmap_block_invalid(slb))
                continue;
            if (slab_is_door(nx, ny))
                doors++;
            else if (!slab_is_wall(nx, ny) && (slb->kind >= SlbT_PATH))
                open++;
        }
        if (inside == 1)
            dead_ends++;
    }
    if (count == 0)
        return WRS_None;
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    const TbBool filled = (count * 10 >= w * h * 9);
    switch (room->kind)
    {
    case RoK_LAIR:
        if (count >= 25)
            return WRS_GreatHall;
        if (count <= 6)
            return WRS_Cells;
        break;
    case RoK_TRAINING:
        if (filled && (min(w, h) <= 2) && (max(w, h) >= 5))
            return WRS_Gallery;
        if (filled && (w == h) && (w >= 4))
            return WRS_Ring;
        break;
    case RoK_LIBRARY:
        if (dead_ends >= 2)
            return WRS_Alcoves;
        if (filled && (count >= 9) && (dead_ends == 0))
            return WRS_OpenFloor;
        break;
    case RoK_TREASURE:
        if ((doors == 1) && (open == 0))
            return WRS_Vault;
        break;
    default:
        break;
    }
    return WRS_None;
}

enum WorldRoomShape world_room_shape(const struct Room *room)
{
    if (!world_mode_active() || room_is_invalid(room) || (room->index <= 0) || (room->index >= ROOMS_COUNT))
        return WRS_None;
    const RoomIndex r = room->index;
    const uint32_t now = (uint32_t)game.play_gameturn;
    if ((sh_turn[r] == 0) || (sh_turn[r] - 1 > now) || (now - (sh_turn[r] - 1) >= NEIGHBOUR_TURNS) || (sh_key[r] != room_key(room)))
    {
        sh_shape[r] = (uint8_t)work_out_shape(room);
        sh_turn[r] = now + 1;
        sh_key[r] = room_key(room);
    }
    return (enum WorldRoomShape)sh_shape[r];
}

const char *world_room_shape_name(enum WorldRoomShape shape)
{
    return ((shape > WRS_None) && (shape < WRS_COUNT)) ? shape_names[shape] : "";
}

static int shape_pct(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng)
{
    const enum WorldRoomShape shape = world_room_shape(room);
    switch (shape)
    {
    case WRS_GreatHall:
        return ((what == WRM_LairHeal) && (room->total_capacity > 0) && (room->used_capacity * 4 >= room->total_capacity * 3)) ? 15 : 0;
    case WRS_Gallery:
    case WRS_Ring:
    {
        if ((what != WRM_Work) || (creatng == NULL) || !thing_exists(creatng))
            return 0;
        const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(creatng);
        const int want = (shape == WRS_Gallery) ? AttckT_Ranged : AttckT_Melee;
        return (crconf->attack_preference == want) ? 20 : 0;
    }
    case WRS_Alcoves:
    case WRS_OpenFloor:
    {
        if ((what != WRM_Work) || (room->owner >= PLAYERS_MAX))
            return 0;
        const TbBool mastery = (game.world_room_researching[room->owner] != 0);
        return (mastery == (shape == WRS_Alcoves)) ? 20 : 0;
    }
    default:
        return 0;
    }
}

/** Each machine, every 64 turns: this machine's keeper hears when a room of theirs takes a shape. */
void world_rooms_present(void)
{
    static uint8_t told[ROOMS_COUNT];
    static PlayerNumber watched = -2;
    if (!world_mode_active() || ((game.play_gameturn % 64) != 37) || (my_player_number >= PLAYERS_MAX))
        return;
    const PlayerNumber p = my_player_number;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    const TbBool news = (watched == p);
    for (RoomKind kind = 1; kind < RoK_TYPES_COUNT; kind++)
    {
        if ((kind != RoK_LAIR) && (kind != RoK_TRAINING) && (kind != RoK_LIBRARY) && (kind != RoK_TREASURE))
            continue;
        long guard = 0;
        for (RoomIndex i = dungeon->room_list_start[kind]; (i != 0) && (guard++ < ROOMS_COUNT);)
        {
            const struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            const enum WorldRoomShape shape = world_room_shape(room);
            if (news && (shape != WRS_None) && (told[room->index] != shape))
            {
                if (shape_lines[shape][0] != '\0')
                    message_add_fmt(MsgType_Room, room->kind, "Your %s is %s: %s", get_string(get_room_kind_stats(room->kind)->name_stridx), shape_names[shape], shape_lines[shape]);
                else
                    message_add_fmt(MsgType_Room, room->kind, "Your %s is %s", get_string(get_room_kind_stats(room->kind)->name_stridx), shape_names[shape]);
            }
            told[room->index] = (uint8_t)shape;
        }
    }
    watched = p;
}

/******************************************************************************/
/* Lie of the Land (the night of 2026-11-13): the biome under a room changes what it can do. */
struct LandRule {
    int biome;
    RoomKind room;
    enum WorldRoomMod what;
    int pct;
    const char *line;
};
static const struct LandRule land_rules[] = {
    {WgB_Volcanic,  RoK_WORKSHOP,  WRM_Work,      20, "a forge: work goes 20% faster"},
    {WgB_Ruins,     RoK_LIBRARY,   WRM_Work,      20, "mastery research 20% faster"},
    {WgB_Sandstone, RoK_TRAINING,  WRM_Work,      15, "an arena: training 15% faster"},
    {WgB_Flooded,   RoK_GARDEN,    WRM_Hatch,     25, "beside the water, chickens hatch 25% sooner"},
    {WgB_Frozen,    RoK_GRAVEYARD, WRM_GraveKeep,100, "bodies keep twice as long"},
};
#define LAND_RULES ((int)(sizeof(land_rules) / sizeof(land_rules[0])))

static int land_pct(const struct Room *room, enum WorldRoomMod what)
{
    const int biome = world_room_biome(room);
    if (biome < 0)
        return 0;
    for (int k = 0; k < LAND_RULES; k++)
    {
        const struct LandRule *lr = &land_rules[k];
        if ((lr->biome != biome) || (lr->room != room->kind) || (lr->what != what))
            continue;
        if ((room->kind == RoK_LIBRARY) && ((room->owner >= PLAYERS_MAX) || (game.world_room_researching[room->owner] == 0)))
            return 0; /* Ruins: mastery research only */
        if ((room->kind == RoK_GARDEN) && !(world_room_neighbour_kinds(room) & (1u << NB_WATER_BIT)))
            return 0; /* Flooded: beside the water */
        return lr->pct;
    }
    return 0;
}

const char *world_room_land_line(const struct Room *room)
{
    const int biome = world_room_biome(room);
    for (int k = 0; (k < LAND_RULES) && (biome >= 0); k++)
        if ((land_rules[k].biome == biome) && (land_rules[k].room == room->kind))
            return land_rules[k].line;
    return NULL;
}

int world_room_mod(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng)
{
    if (!world_mode_active() || room_is_invalid(room) || (room->owner >= PLAYERS_MAX) || !world_player_is_human(room->owner))
        return 0;
    int malus = 0;
    int bonus = neighbour_pct(room, what, creatng, &malus);
    bonus += shape_pct(room, what, creatng); /* The Shape of a Room */
    if (what == WRM_GraveKeep)
        return land_pct(room, what); /* not a speed: kept apart from the cap */
    bonus += land_pct(room, what);    /* Lie of the Land */
    if (bonus > WORLD_ROOM_BONUS_CAP)
        bonus = WORLD_ROOM_BONUS_CAP;
    return bonus + malus;
}

long world_room_apply(const struct Room *room, enum WorldRoomMod what, const struct Thing *creatng, long value)
{
    const int pct = world_room_mod(room, what, creatng);
    return (pct == 0) ? value : value * (100 + pct) / 100;
}

long world_room_apply_here(const struct Thing *creatng, enum WorldRoomMod what, long value)
{
    if (!world_mode_active())
        return value;
    const struct Room *room = get_room_thing_is_on(creatng);
    if (room_is_invalid(room) || (room->owner != creatng->owner))
        return value;
    return world_room_apply(room, what, creatng, value);
}

/** KFX_WORLD_ROOMBRAWL=turn (test switch): in each Lair of this machine's keeper a Vampire and a Warlock (lair enemies)
 *  are made with that Lair as theirs; on the Vampire's next lair-enemy look (the stock one, every 64 turns) whether it
 *  starts a fight is logged with the Lair's shape. */
static void room_brawl_test(void)
{
    static long at = -2;
    static ThingIndex vamp[4], rooms[4];
    static int n = 0;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_ROOMBRAWL");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at < 0) || ((long)game.play_gameturn < at) || ((long)game.play_gameturn > at + 64) || !world_mode_active())
        return;
    const PlayerNumber p = my_player_number;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    if ((long)game.play_gameturn == at)
    {
        n = 0;
        for (RoomIndex i = dungeon->room_list_start[RoK_LAIR]; (i != 0) && (n < 4);)
        {
            struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            struct Coord3d pos;
            memset(&pos, 0, sizeof(pos));
            pos.x.val = subtile_coord_center(room->central_stl_x);
            pos.y.val = subtile_coord_center(room->central_stl_y);
            struct Thing *v = create_creature(&pos, (ThingModel)get_id(creature_desc, "VAMPIRE"), p);
            struct Thing *w = create_creature(&pos, (ThingModel)get_id(creature_desc, "SORCEROR"), p);
            if (thing_is_invalid(v) || thing_is_invalid(w))
                continue;
            creature_control_get_from_thing(v)->lair_room_id = room->index;
            creature_control_get_from_thing(w)->lair_room_id = room->index;
            vamp[n] = v->index;
            rooms[n++] = room->index;
        }
        return;
    }
    for (int k = 0; k < n; k++)
    {
        struct Thing *v = thing_get(vamp[k]);
        struct Room *room = room_get(rooms[k]);
        if (!thing_exists(v) || room_is_invalid(room) || (((get_gameturn() + v->index) & 63) != 0))
            continue;
        const long fight = process_lair_enemy(v, room);
        SYNCLOG("World: room brawl test: Lair #%d (%s, %d slabs): the Vampire starts a fight with the Warlock %ld",
            (int)room->index, world_room_shape_name(world_room_shape(room)), (int)room->slabs_count, fight);
    }
}

/** KFX_WORLD_ROOMTEST=turn (test switch): at that turn, every room of this machine's keeper is logged with the rooms
 *  it touches and its bonuses. */
void world_rooms_turn(void)
{
    room_brawl_test();
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_ROOMTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_ROOMTEST=%ld", at);
    }
    if ((at > 100) && ((long)game.play_gameturn == at - 100) && world_mode_active() && (getenv("KFX_WORLD_ROOMLAYOUT") != NULL))
    {
        /* KFX_WORLD_ROOMLAYOUT=1 (test switch): 100 turns before, two strips of six 3x3 rooms below this machine's Heart,
           laid so that every pair touches: Prison|Torture|Lair|Graveyard|Library|Temple and
           Hatchery|Lair|Workshop|Treasury|Library|Training */
        const PlayerNumber me = my_player_number;
        const struct Thing *heart = get_player_soul_container(me);
        if (thing_exists(heart))
        {
            const char *layout = getenv("KFX_WORLD_ROOMLAYOUT");
            if ((layout != NULL) && (atoi(layout) == 2))
            {
                /* the shapes: a gallery and a ring (Training), a Library with two alcoves, a great hall and cells (Lair) */
                static const struct { signed char x, y, w, h; unsigned char kind; } parts[] = {
                    {-9, 6, 6, 2, SlbT_TRAINING}, {-2, 6, 4, 4, SlbT_TRAINING},
                    {3, 6, 3, 3, SlbT_LIBRARY}, {6, 6, 1, 1, SlbT_LIBRARY}, {6, 8, 1, 1, SlbT_LIBRARY},
                    {-9, 11, 5, 5, SlbT_LAIR}, {-2, 11, 2, 2, SlbT_LAIR}};
                const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
                for (int k = 0; k < (int)(sizeof(parts) / sizeof(parts[0])); k++)
                    for (int dy = 0; dy < parts[k].h; dy++)
                        for (int dx = 0; dx < parts[k].w; dx++)
                        {
                            const MapSlabCoord x = hx + parts[k].x + dx, y = hy + parts[k].y + dy;
                            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(y), me, 0);
                            replace_slab_from_script(x, y, parts[k].kind);
                        }
                SYNCLOG("World: room test: the shapes laid below player %d's Heart", (int)me);
                return;
            }
            static const unsigned char strips[2][6] = {
                {SlbT_PRISON, SlbT_TORTURE, SlbT_LAIR, SlbT_GRAVEYARD, SlbT_LIBRARY, SlbT_TEMPLE},
                {SlbT_GARDEN, SlbT_LAIR, SlbT_WORKSHOP, SlbT_TREASURE, SlbT_LIBRARY, SlbT_TRAINING}};
            const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
            for (int s = 0; s < 2; s++)
                for (int r = 0; r < 6; r++)
                    for (int dy = 0; dy < 3; dy++)
                        for (int dx = 0; dx < 3; dx++)
                        {
                            const MapSlabCoord x = hx - 9 + r * 3 + dx, y = hy + 6 + s * 4 + dy;
                            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(y), me, 0);
                            replace_slab_from_script(x, y, strips[s][r]);
                        }
            /* water beside the Hatchery (Lie of the Land: Flooded Grottos) */
            place_slab_type_on_map(SlbT_WATER, slab_subtile_center(hx - 10), slab_subtile_center(hy + 11), PLAYER_NEUTRAL, 0);
            SYNCLOG("World: room test: two strips of rooms laid below player %d's Heart", (int)me);
        }
        return;
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    const PlayerNumber p = my_player_number;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    int n = 0;
    for (RoomKind kind = 1; kind < RoK_TYPES_COUNT; kind++)
    {
        long guard = 0;
        for (RoomIndex i = dungeon->room_list_start[kind]; (i != 0) && (guard++ < ROOMS_COUNT);)
        {
            const struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            const uint32_t mask = world_room_neighbour_kinds(room);
            char touches[160];
            int t = 0;
            touches[0] = '\0';
            for (RoomKind k = 1; (k < RoK_TYPES_COUNT) && (t < (int)sizeof(touches) - 24); k++)
                if (mask & (1u << k))
                    t += snprintf(touches + t, sizeof(touches) - t, "%s%s", (t > 0) ? "," : "", room_code_name(k));
            const int biome = world_room_biome(room);
            SYNCLOG("World: room test: %s #%d (%d slabs, %s, %s%s) touches [%s]: work %+d%%, torture %+d%%, lair heal %+d%%, hatch %+d%%, bodies kept %+d%%",
                room_code_name(room->kind), (int)room->index, (int)room->slabs_count,
                (world_room_shape(room) != WRS_None) ? world_room_shape_name(world_room_shape(room)) : "no shape",
                (biome >= 0) ? wg_biome_name(biome) : "?", (world_room_neighbour_kinds(room) & (1u << NB_WATER_BIT)) ? ", by water" : "", touches,
                world_room_mod(room, WRM_Work, NULL), world_room_mod(room, WRM_Torture, NULL), world_room_mod(room, WRM_LairHeal, NULL),
                world_room_mod(room, WRM_Hatch, NULL), world_room_mod(room, WRM_GraveKeep, NULL));
            if (room->kind == RoK_LAIR)
            {
                /* the Vampire exception: a Vampire and an Orc, made for the moment at the Heart, asked the same */
                const struct Thing *heart = get_player_soul_container(p);
                struct Thing *vamp = thing_exists(heart) ? create_creature((struct Coord3d *)&heart->mappos,
                    (ThingModel)get_id(creature_desc, "VAMPIRE"), p) : NULL;
                struct Thing *orc = thing_exists(heart) ? create_creature((struct Coord3d *)&heart->mappos,
                    (ThingModel)get_id(creature_desc, "ORC"), p) : NULL;
                SYNCLOG("World: room test: LAIR #%d lair heal for a Vampire %+d%%, for an Orc %+d%%", (int)room->index,
                    thing_is_invalid(vamp) ? -999 : world_room_mod(room, WRM_LairHeal, vamp),
                    thing_is_invalid(orc) ? -999 : world_room_mod(room, WRM_LairHeal, orc));
                if (!thing_is_invalid(vamp))
                    delete_thing_structure(vamp, 0);
                if (!thing_is_invalid(orc))
                    delete_thing_structure(orc, 0);
            }
            n++;
        }
    }
    SYNCLOG("World: room test: %d rooms of player %d", n, (int)p);
}

#ifdef __cplusplus
}
#endif

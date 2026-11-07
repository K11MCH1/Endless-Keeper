/******************************************************************************/
/** @file world_shift.c
 *     Endless world: moving the whole game state by whole chunks.
 * @par Purpose:
 *     The save system's field tables (kfx/save/core/schema) describe every saved struct and field: offset,
 *     stored type, array dimensions, unions. world_shift_state walks the live game state with them and, for
 *     every field that the table below marks as a position, moves it. Arrays indexed by position are moved
 *     as a whole. world_shift_audit checks that every position-like field of the save tables is marked
 *     (moved or explicitly left alone), so a field added later can't be missed silently.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_shift.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "game_legacy.h"
#include "map_data.h"
#include "map_blocks.h"
#include "map_columns.h"
#include "slab_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "creature_control.h"
#include "creature_states.h"
#include "player_data.h"
#include "light_data.h"
#include "ariadne.h"
#include "ariadne_update.h"
#include "map_ceiling.h"
#include "frontmenu_ingame_map.h"
#include "kfx/save/core/save_schema.h"
#include "kfx/save/core/schema/save_tables_decl.h"
#include "post_inc.h"

/******************************************************************************/
enum WorldShiftKind {
    WSK_None = 0,
    /** A Coord3d / Coord2d member: x.val and y.val (map coordinates, 256 per subtile). */
    WSK_Pos,
    /** Scalars: map coordinates, subtiles, slabs, packed subtile and slab numbers. */
    WSK_CoordX, WSK_CoordY,
    WSK_StlX, WSK_StlY,
    WSK_SlbX, WSK_SlbY,
    WSK_StlNum, WSK_SlbNum,
    /** Not a position, or handled outside the table (moved arrays, rebuilt data, logs). */
    WSK_Keep,
};

struct WorldShiftTag {
    const char *strct;
    const char *field;
    unsigned char kind;
};

static const struct WorldShiftTag shift_tags[] = {
    /* things */
    {"Thing", "mappos", WSK_Pos},
    {"Thing", "previous_mappos", WSK_Pos},
    {"Thing_shot", "shot.originpos", WSK_Pos},
    {"Thing_shot_lizard", "shot_lizard.x", WSK_CoordX},
    {"Thing_cave_in", "cave_in.x", WSK_StlX},
    {"Thing_cave_in", "cave_in.y", WSK_StlY},
    {"Thing", "veloc_push_once", WSK_Keep}, {"Thing", "veloc_base", WSK_Keep},
    {"Thing", "veloc_push_add", WSK_Keep}, {"Thing", "velocity", WSK_Keep},
    {"Thing_shot_lizard", "shot_lizard.posint", WSK_Keep},
    {"Thing_food", "food.possession_startup_timer", WSK_Keep},
    /* creatures */
    {"CreatureControl", "moveto_pos", WSK_Pos},
    {"CreatureControl", "digger_working_stl", WSK_Keep}, /* by hand, in shift_creature_raw_unions */
    {"CreatureControl", "digger_task_stl", WSK_Keep},
    {"CreatureControl", "party.member_pos_stl", WSK_StlNum},
    {"CreatureControl", "patrol.pos", WSK_Pos},
    {"CreatureControl", "teleport_x", WSK_StlX},
    {"CreatureControl", "teleport_y", WSK_StlY},
    {"CreatureControl", "targtstl_x", WSK_StlX},
    {"CreatureControl", "targtstl_y", WSK_StlY},
    {"CreatureControl", "collided_door_subtile", WSK_StlNum},
    {"CreatureControl", "damage_wall_coords", WSK_StlNum},
    {"CreatureControl", "flee_pos", WSK_Pos},
    {"CreatureControl", "alarm_stl_x", WSK_StlX},
    {"CreatureControl", "alarm_stl_y", WSK_StlY},
    {"CreatureControl", "moveaccel", WSK_Keep},
    {"CreatureControl", "shot_shift_x", WSK_Keep}, {"CreatureControl", "shot_shift_y", WSK_Keep},
    {"CreatureControl", "shot_shift_z", WSK_Keep},
    {"CreatureControl", "fighting_at_same_position", WSK_Keep},
    {"CreatureControl", "distance_to_destination", WSK_Keep},
    {"MemberPos", "stl_num", WSK_StlNum},
    {"Navigation", "pos_next", WSK_Pos},
    {"Navigation", "pos_final", WSK_Pos},
    {"Navigation", "first_colliding_block", WSK_StlNum},
    {"Navigation", "second_colliding_block", WSK_StlNum},
    {"Navigation", "dist_to_final_pos", WSK_Keep}, {"Navigation", "distance_to_next_pos", WSK_Keep},
    {"Ariadne", "startpos", WSK_Pos},
    {"Ariadne", "endpos", WSK_Pos},
    {"Ariadne", "current_waypoint_pos", WSK_Pos},
    {"Ariadne", "next_position", WSK_Pos},
    {"Ariadne", "previous_position", WSK_Pos},
    {"Ariadne", "waypoints", WSK_Pos},
    {"Ariadne", "manoeuvre_fixed_position", WSK_Pos},
    {"Ariadne", "manoeuvre_requested_position", WSK_Pos},
    /* rooms */
    {"Room", "central_stl_x", WSK_StlX},
    {"Room", "central_stl_y", WSK_StlY},
    {"Room", "slabs_list", WSK_SlbNum},
    {"Room", "slabs_list_tail", WSK_SlbNum},
    {"Room", "flame_slb", WSK_SlbNum},
    {"Room", "flame_stl", WSK_Keep},
    {"Room", "slabs_count", WSK_Keep},
    /* dungeons and players */
    {"Dungeon", "mappos", WSK_Pos},
    {"Dungeon", "sight_casted_stl_x", WSK_StlX},
    {"Dungeon", "sight_casted_stl_y", WSK_StlY},
    {"Dungeon", "cta_stl_x", WSK_StlX},
    {"Dungeon", "cta_stl_y", WSK_StlY},
    {"Dungeon", "essential_pos", WSK_Pos},
    {"Dungeon", "devastation_centr_x", WSK_StlX},
    {"Dungeon", "devastation_centr_y", WSK_StlY},
    {"Dungeon", "last_combat_location", WSK_Pos},
    {"Dungeon", "last_eventful_death_location", WSK_Pos},
    {"Dungeon", "last_trap_event_location", WSK_Pos},
    {"Dungeon", "camera_deviate_quake", WSK_Keep}, {"Dungeon", "camera_deviate_jump", WSK_Keep},
    {"Dungeon", "total_area", WSK_Keep}, {"Dungeon", "room_manage_area", WSK_Keep},
    {"MapTask", "coords", WSK_StlNum},
    {"DiggerStack", "stl_num", WSK_StlNum},
    {"Wander", "points", WSK_None}, /* SubtileXY, below */
    {"Wander", "last_checked_slb_num", WSK_SlbNum},
    {"SubtileXY", "stl_x", WSK_StlX},
    {"SubtileXY", "stl_y", WSK_StlY},
    {"RoomSpace", "left", WSK_SlbX}, {"RoomSpace", "right", WSK_SlbX},
    {"RoomSpace", "top", WSK_SlbY}, {"RoomSpace", "bottom", WSK_SlbY},
    {"RoomSpace", "centreX", WSK_SlbX}, {"RoomSpace", "centreY", WSK_SlbY},
    {"RoomSpace", "buildx", WSK_SlbX}, {"RoomSpace", "buildy", WSK_SlbY},
    {"RoomSpace", "drag_start_x", WSK_SlbX}, {"RoomSpace", "drag_start_y", WSK_SlbY},
    {"RoomSpace", "drag_end_x", WSK_SlbX}, {"RoomSpace", "drag_end_y", WSK_SlbY},
    {"RoomSpace", "slab_grid", WSK_Keep}, {"RoomSpace", "slab_count", WSK_Keep},
    {"RoomSpace", "invalid_slabs_count", WSK_Keep}, {"RoomSpace", "is_roomspace_a_single_subtile", WSK_Keep},
    {"UserState", "cursor_subtile_x", WSK_StlX}, {"UserState", "cursor_subtile_y", WSK_StlY},
    {"UserState", "previous_cursor_subtile_x", WSK_StlX}, {"UserState", "previous_cursor_subtile_y", WSK_StlY},
    {"UserState", "cursor_clicked_subtile_x", WSK_StlX}, {"UserState", "cursor_clicked_subtile_y", WSK_StlY},
    {"UserState", "zoom_to_pos_x", WSK_CoordX}, {"UserState", "zoom_to_pos_y", WSK_CoordY},
    {"UserState", "cursor_light_idx", WSK_Keep}, {"UserState", "teleport_destination", WSK_Keep},
    {"UserState", "instance_num", WSK_Keep},
    {"DungeonCamera", "x", WSK_CoordX}, {"DungeonCamera", "y", WSK_CoordY},
    {"DungeonCamera", "zoom", WSK_Keep}, {"DungeonCamera", "yaw", WSK_Keep}, {"DungeonCamera", "pitch", WSK_Keep},
    /* computer players */
    {"Computer2", "trap_locations", WSK_Pos},
    {"Computer2", "soe_targets", WSK_Keep}, /* coarse grid, cleared by hand */
    {"OpponentRelation", "pos_A", WSK_Pos},
    {"ComputerDig", "pos_E", WSK_Pos}, {"ComputerDig", "pos_dest", WSK_Pos},
    {"ComputerDig", "pos_begin", WSK_Pos}, {"ComputerDig", "pos_next", WSK_Pos},
    {"ComputerDig", "last_backwards_step_stl_x", WSK_StlX},
    {"ComputerDig", "last_backwards_step_stl_y", WSK_StlY},
    {"ComputerDig", "valuable_slabs_tagged", WSK_Keep}, {"ComputerDig", "room.slabs_processed", WSK_Keep},
    {"ComputerTask", "new_room_pos", WSK_Pos},
    {"ComputerTask", "starting_position", WSK_Pos},
    {"ComputerTask_create_room", "create_room.startpos", WSK_Pos},
    {"ComputerTask_create_room", "create_room.endpos", WSK_Pos},
    {"ComputerTask_dig_somewhere", "dig_somewhere.startpos", WSK_Pos},
    {"ComputerTask_dig_somewhere", "dig_somewhere.endpos", WSK_Pos},
    {"ComputerTask_dig_to_gold", "dig_to_gold.startpos", WSK_Pos},
    {"ComputerTask_dig_to_gold", "dig_to_gold.endpos", WSK_Pos},
    {"ComputerTask_dig_to_room", "dig_to_room.startpos", WSK_Pos},
    {"ComputerTask_dig_to_room", "dig_to_room.endpos", WSK_Pos},
    {"ComputerTask_magic_cta", "magic_cta.target_pos", WSK_Pos},
    {"ComputerTask_move_to_defend", "move_to_defend.target_pos", WSK_Pos},
    {"ComputerTask_move_to_pos", "move_to_pos.target_pos", WSK_Pos},
    {"ComputerTask_pickup_for_attack", "pickup_for_attack.target_pos", WSK_Pos},
    /* the rest of the game */
    {"Game", "mouse_light_pos", WSK_Pos},
    {"Game", "armageddon_mappos", WSK_Pos},
    {"Game", "triggered_object_location", WSK_Pos},
    {"Game", "hand_over_subtile_x", WSK_StlX},
    {"Game", "hand_over_subtile_y", WSK_StlY},
    {"Game", "something_light_x", WSK_Keep}, {"Game", "something_light_y", WSK_Keep},
    {"Bookmark", "x", WSK_StlX},
    {"Bookmark", "y", WSK_StlY},
    {"ActionPoint", "mappos", WSK_Pos},
    {"Event", "mappos_x", WSK_CoordX},
    {"Event", "mappos_y", WSK_CoordY},
    {"GoldLookup", "stl_x", WSK_StlX},
    {"GoldLookup", "stl_y", WSK_StlY},
    {"ScriptFxLine", "from", WSK_Pos},
    {"ScriptFxLine", "here", WSK_Pos},
    {"ScriptFxLine", "to", WSK_Pos},
    {"ScriptFxLine", "cx", WSK_CoordX},
    {"ScriptFxLine", "cy", WSK_CoordY},
    {"Light", "mappos", WSK_Pos},
    {"Light", "previous_mappos", WSK_Pos},
    {"Light", "attached_slb", WSK_SlbNum},
    /* moved as whole arrays, rebuilt after the shift, or logs */
    {"Game", "map", WSK_Keep},
    {"Game", "slabmap", WSK_Keep},
    {"Game", "navigation_map", WSK_Keep},
    {"Game", "slab_ext_data", WSK_Keep},
    {"Game", "slab_ext_data_initial", WSK_Keep},
    {"Game", "columns_data", WSK_Keep},
    {"Game", "slabset", WSK_Keep},
    {"Game", "slabobjs", WSK_Keep},
    {"Game", "conf", WSK_Keep},
    {"Game", "packets", WSK_Keep},
    {"Game", "log_snapshot", WSK_Keep},
    {"Game", "host_checksums", WSK_Keep},
    {"Game", "map_subtiles_x", WSK_Keep}, {"Game", "map_subtiles_y", WSK_Keep},
    {"Game", "map_tiles_x", WSK_Keep}, {"Game", "map_tiles_y", WSK_Keep},
    {"Game", "slabset_num", WSK_Keep}, {"Game", "slabobjs_num", WSK_Keep}, {"Game", "slabobjs_idx", WSK_Keep},
    {"LightsShadows", "stat_light_map", WSK_Keep},
    {"LightsShadows", "subtile_lightness", WSK_Keep},
    {"LightsShadows", "shadow_cache", WSK_Keep},
    {"IntralevelData", "transferred_creatures", WSK_Keep},
    /* not positions (indices, counters, sizes, angles, GUI states) */
    {"PlayerInfo", "possession_lock", WSK_Keep},
    {"UserState", "cursor_button_down", WSK_Keep}, {"UserState", "primary_cursor_state", WSK_Keep},
    {"UserState", "secondary_cursor_state", WSK_Keep}, {"UserState", "one_click_lock_cursor", WSK_Keep},
    {"UserState", "full_slab_cursor", WSK_Keep},
    {"CreatureControl", "target_room_id", WSK_Keep}, {"CreatureControl", "party.target_plyr_idx", WSK_Keep},
    {"CreatureControl", "timebomb_target_id", WSK_Keep},
    {"Ariadne", "current_waypoint", WSK_Keep}, {"Ariadne", "stored_waypoints", WSK_Keep},
    {"Ariadne", "total_waypoints", WSK_Keep}, {"Ariadne", "straight_dist_to_next_waypoint", WSK_Keep},
    {"Thing_shot", "shot.target_idx", WSK_Keep}, {"Thing_shot_lizard", "shot_lizard.target_idx", WSK_Keep},
    {"Thing", "move_angle_xy", WSK_Keep}, {"Thing", "clipbox_size_xy", WSK_Keep}, {"Thing", "solid_size_xy", WSK_Keep},
    {"ComputerTask_attack_magic", "attack_magic.target_thing_idx", WSK_Keep},
    {"ComputerTask_sacrifice_imp", "sacrifice_imp.target_state", WSK_Keep},
    {"ComputerTask_move_to_defend", "move_to_defend.target_state", WSK_Keep},
    {"ComputerTask_move_to_pos", "move_to_pos.target_thing_idx", WSK_Keep},
    {"ComputerTask_move_to_pos", "move_to_pos.repeat_num", WSK_Keep},
    {"ComputerTask_move_to_pos", "move_to_pos.target_state", WSK_Keep},
    {"ComputerTask_pickup_for_attack", "pickup_for_attack.target_state", WSK_Keep},
    {"ComputerTask_dig_to_room", "dig_to_room.target_room_idx", WSK_Keep},
    {"ComputerTask_dig_to_room", "dig_to_room.target_plyr_idx", WSK_Keep},
    {"ComputerTask_dig_to_gold", "dig_to_gold.target_lookup_idx", WSK_Keep},
    {"ComputerTask_dig_to_gold", "dig_to_gold.slabs_dig_count", WSK_Keep},
    {"ComputerTask_dig_somewhere", "dig_somewhere.target_plyr_idx", WSK_Keep},
    {"Dungeon", "scavenge_targets", WSK_Keep},
    {"Event", "target", WSK_Keep},
    {"GoldLookup", "num_gold_slabs", WSK_Keep}, {"GoldLookup", "num_gem_slabs", WSK_Keep},
    {"TextScrollWindow", "start_y", WSK_Keep},
    {"GuiMessage", "target_idx", WSK_Keep},
    {"PartyMember", "target", WSK_Keep},
    {"ScriptVariable", "variable_target", WSK_Keep}, {"ScriptVariable", "variable_target_type", WSK_Keep},
    {"Game", "heart_lost_message_target", WSK_Keep},
    /* script locations can hold subtile coordinates (MLoc_COORDS); world levels don't use them */
    {"TunnellerTrigger", "location", WSK_Keep}, {"PartyTrigger", "location", WSK_Keep},
    {"ScriptValue", "union:sac", WSK_Keep},
};

/******************************************************************************/
struct WorldShiftCtx {
    MapSlabCoord dslb_x, dslb_y;
    long fields_moved;
};

static const struct WorldShiftTag *find_tag(const char *strct, const char *field)
{
    for (size_t i = 0; i < sizeof(shift_tags) / sizeof(shift_tags[0]); i++)
    {
        if ((strcmp(shift_tags[i].strct, strct) == 0) && (strcmp(shift_tags[i].field, field) == 0))
            return &shift_tags[i];
    }
    return NULL;
}

static int32_t clamp_i32(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) return (int32_t)lo;
    if (v > hi) return (int32_t)hi;
    return (int32_t)v;
}

static int64_t shift_coord(int64_t v, MapSlabCoord dslb, MapSubtlCoord stl_max)
{
    return clamp_i32(v - (int64_t)dslb * STL_PER_SLB * COORD_PER_STL, 0, (int64_t)stl_max * COORD_PER_STL - 1);
}

static int64_t shift_stl(int64_t v, MapSlabCoord dslb, MapSubtlCoord stl_max)
{
    return clamp_i32(v - (int64_t)dslb * STL_PER_SLB, 0, stl_max - 1);
}

static int64_t shift_slb(int64_t v, MapSlabCoord dslb, MapSlabCoord slb_max)
{
    return clamp_i32(v - dslb, 0, slb_max - 1);
}

/** A packed subtile number (stride map_subtiles_x+1). 0 and values outside the map mean "none". */
static int64_t shift_stl_num(int64_t v, const struct WorldShiftCtx *ctx)
{
    const int64_t stride = game.map_subtiles_x + 1;
    if ((v <= 0) || (v >= stride * (game.map_subtiles_y + 1)))
        return v;
    int64_t x = v % stride, y = v / stride;
    x = shift_stl(x, ctx->dslb_x, game.map_subtiles_x);
    y = shift_stl(y, ctx->dslb_y, game.map_subtiles_y);
    return y * stride + x;
}

/** A packed subtile number that becomes 0 ("none") when it leaves the window, instead of being clamped onto
 *  the edge: diggers' targets, so an imp never digs a slab nobody marked. */
static int64_t shift_stl_num_or_none(int64_t v, const struct WorldShiftCtx *ctx)
{
    const int64_t stride = game.map_subtiles_x + 1;
    if ((v <= 0) || (v >= stride * (game.map_subtiles_y + 1)))
        return v;
    int64_t x = v % stride - (int64_t)ctx->dslb_x * STL_PER_SLB, y = v / stride - (int64_t)ctx->dslb_y * STL_PER_SLB;
    if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
        return 0;
    return y * stride + x;
}

/** A packed slab number (stride map_tiles_x). 0 and values outside the map mean "none". */
static int64_t shift_slb_num(int64_t v, const struct WorldShiftCtx *ctx)
{
    const int64_t stride = game.map_tiles_x;
    if ((v <= 0) || (v >= stride * game.map_tiles_y))
        return v;
    int64_t x = v % stride, y = v / stride;
    x = shift_slb(x, ctx->dslb_x, game.map_tiles_x);
    y = shift_slb(y, ctx->dslb_y, game.map_tiles_y);
    return y * stride + x;
}

static void shift_pos_raw(int32_t *xval, int32_t *yval, const struct WorldShiftCtx *ctx)
{
    if ((*xval == 0) && (*yval == 0))
        return;
    *xval = (int32_t)shift_coord(*xval, ctx->dslb_x, game.map_subtiles_x);
    *yval = (int32_t)shift_coord(*yval, ctx->dslb_y, game.map_subtiles_y);
}

static void shift_scalar(void *mem, uint32_t elem, uint8_t stored_type, unsigned char kind, struct WorldShiftCtx *ctx)
{
    struct SaveValue val;
    if (save_value_load_mem(mem, elem, stored_type, &val) != 0)
        return;
    if (val.is_float)
        return;
    int64_t v = val.neg ? -(int64_t)val.mag : (int64_t)val.mag;
    if (v == 0)
        return; /* unset */
    int64_t nv = v;
    switch (kind)
    {
    case WSK_CoordX: nv = shift_coord(v, ctx->dslb_x, game.map_subtiles_x); break;
    case WSK_CoordY: nv = shift_coord(v, ctx->dslb_y, game.map_subtiles_y); break;
    case WSK_StlX: nv = shift_stl(v, ctx->dslb_x, game.map_subtiles_x); break;
    case WSK_StlY: nv = shift_stl(v, ctx->dslb_y, game.map_subtiles_y); break;
    case WSK_SlbX: nv = shift_slb(v, ctx->dslb_x, game.map_tiles_x); break;
    case WSK_SlbY: nv = shift_slb(v, ctx->dslb_y, game.map_tiles_y); break;
    case WSK_StlNum: nv = shift_stl_num(v, ctx); break;
    case WSK_SlbNum: nv = shift_slb_num(v, ctx); break;
    default: return;
    }
    if (nv == v)
        return;
    val.neg = (nv < 0);
    val.mag = (uint64_t)(nv < 0 ? -nv : nv);
    save_value_store_mem(mem, elem, stored_type, &val, 1);
    ctx->fields_moved++;
}

/******************************************************************************/
/* Per table: the shift kind of each field (looked up once), and whether a position is in it somewhere, so
 * the walk can skip big arrays of position-free structs. */
#define SHIFT_MEMO_MAX 512
struct DescInfo {
    const struct SaveStructDesc *desc;
    signed char has_pos;      /* -1 while being worked out */
    unsigned char *kinds;     /* per field: WorldShiftKind */
};
static struct DescInfo desc_info[SHIFT_MEMO_MAX];
static int desc_info_count;

static struct DescInfo *get_desc_info(const struct SaveStructDesc *desc, int depth)
{
    for (int i = 0; i < desc_info_count; i++)
        if (desc_info[i].desc == desc)
            return &desc_info[i];
    if ((desc_info_count >= SHIFT_MEMO_MAX) || (depth > 16))
        return NULL;
    struct DescInfo *di = &desc_info[desc_info_count++];
    di->desc = desc;
    di->has_pos = -1;
    di->kinds = (unsigned char *)calloc(desc->field_count ? desc->field_count : 1, 1);
    TbBool found = (strcmp(desc->name, "Coord3d") == 0) || (strcmp(desc->name, "Coord2d") == 0);
    for (uint32_t f = 0; f < desc->field_count; f++)
    {
        const struct SaveFieldDesc *field = &desc->fields[f];
        const struct WorldShiftTag *tag = find_tag(desc->name, field->name);
        unsigned char kind = (tag != NULL) ? tag->kind : WSK_None;
        if (field->flags & (SVF_DERIVED | SVF_RUNTIME))
            kind = WSK_Keep;
        if (di->kinds != NULL)
            di->kinds[f] = kind;
        if (kind == WSK_Keep)
            continue;
        if ((kind != WSK_None) && (field->stored_type != SV_STRUCT))
            found = true;
        if (field->stored_type == SV_STRUCT && field->sub != NULL)
        {
            if (kind == WSK_Pos)
                found = true;
            else
            {
                struct DescInfo *sub = get_desc_info(field->sub, depth + 1);
                if ((sub == NULL) || (sub->has_pos != 0))
                    found = true;
            }
        }
        if ((field->stored_type == SV_UNION) && (field->uni != NULL))
        {
            for (uint32_t m = 0; m < field->uni->member_count; m++)
            {
                if (field->uni->members[m].sub == NULL)
                    continue;
                struct DescInfo *sub = get_desc_info(field->uni->members[m].sub, depth + 1);
                if ((sub == NULL) || (sub->has_pos != 0))
                    found = true;
            }
        }
    }
    di->has_pos = found ? 1 : 0;
    return di;
}

/** Array elements that are not in use (free thing slots and the like) are skipped. */
static TbBool element_in_use(const struct SaveStructDesc *desc, const uint8_t *el)
{
    if (desc == &save_desc_Thing)
        return (((const struct Thing *)el)->alloc_flags & TAlF_Exists) != 0;
    if (desc == &save_desc_CreatureControl)
        return (((const struct CreatureControl *)el)->creature_control_flags & CCFlg_Exists) != 0;
    if (desc == &save_desc_Room)
        return (((const struct Room *)el)->alloc_flags & 0x01) != 0;
    if (desc == &save_desc_Light)
        return (((const struct Light *)el)->flags & LgtF_Allocated) != 0;
    return true;
}

static void walk_struct(struct DescInfo *di, uint8_t *base, struct WorldShiftCtx *ctx, int depth);

static void walk_field_struct_array(const struct SaveFieldDesc *field, uint8_t *bytes, unsigned char kind,
    struct WorldShiftCtx *ctx, int depth)
{
    const uint32_t count = save_field_count(field);
    const uint32_t elem = field->mem_size / count;
    struct DescInfo *sub = (kind == WSK_Pos) ? NULL : get_desc_info(field->sub, 0);
    if ((kind != WSK_Pos) && ((sub == NULL) || (sub->has_pos == 0)))
        return;
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t *el = bytes + (size_t)i * elem;
        if (kind == WSK_Pos)
        {
            /* Coord3d and Coord2d both start with x.val, y.val (int32) */
            shift_pos_raw((int32_t *)el, (int32_t *)(el + 4), ctx);
            ctx->fields_moved++;
        } else
        if (element_in_use(field->sub, el))
        {
            walk_struct(sub, el, ctx, depth + 1);
        }
    }
}

static void walk_struct(struct DescInfo *di, uint8_t *base, struct WorldShiftCtx *ctx, int depth)
{
    if ((di == NULL) || (depth > 16) || (di->has_pos == 0) || (di->kinds == NULL))
        return;
    const struct SaveStructDesc *desc = di->desc;
    for (uint32_t f = 0; f < desc->field_count; f++)
    {
        const struct SaveFieldDesc *field = &desc->fields[f];
        unsigned char kind = di->kinds[f];
        if (kind == WSK_Keep)
            continue;
        uint8_t *bytes = base + field->offset;
        if (field->stored_type == SV_UNION)
        {
            int pick = -1;
            struct SaveError err;
            memset(&err, 0, sizeof(err));
            if (save_union_pick(desc, field, base, &pick, &err) != SVR_Ok)
                continue;
            if ((pick >= 0) && (field->uni->members[pick].sub != NULL))
                walk_struct(get_desc_info(field->uni->members[pick].sub, 0), base, ctx, depth + 1);
            continue;
        }
        if (field->stored_type == SV_STRUCT)
        {
            if (field->sub != NULL)
                walk_field_struct_array(field, bytes, kind, ctx, depth);
            continue;
        }
        if ((kind == WSK_None) || (field->flags & SVF_RAW))
            continue;
        const uint32_t count = save_field_count(field);
        const uint32_t elem = field->mem_size / count;
        for (uint32_t i = 0; i < count; i++)
            shift_scalar(bytes + (size_t)i * elem, elem, field->stored_type, kind, ctx);
    }
}

/******************************************************************************/
/* Arrays indexed by position */

static void move_grid(void *data, size_t elem, long stride, long rows, long dx, long dy)
{
    unsigned char *src = (unsigned char *)malloc(elem * stride * rows);
    if (src == NULL)
    {
        ERRORLOG("Out of memory");
        return;
    }
    memcpy(src, data, elem * stride * rows);
    unsigned char *dst = (unsigned char *)data;
    memset(dst, 0, elem * stride * rows);
    for (long y = 0; y < rows; y++)
    {
        long sy = y + dy;
        if ((sy < 0) || (sy >= rows))
            continue;
        long x1 = 0, x2 = stride;
        if (dx > 0) x2 = stride - dx; else x1 = -dx;
        if (x2 <= x1)
            continue;
        memcpy(dst + elem * (y * stride + x1), src + elem * (sy * stride + x1 + dx), elem * (x2 - x1));
    }
    free(src);
}

static void move_map_arrays(const struct WorldShiftCtx *ctx)
{
    const long sdx = ctx->dslb_x * STL_PER_SLB, sdy = ctx->dslb_y * STL_PER_SLB;
    move_grid(game.map, sizeof(struct Map), game.map_subtiles_x + 1, game.map_subtiles_y + 1, sdx, sdy);
    move_grid(game.navigation_map, sizeof(game.navigation_map[0]), game.map_subtiles_x + 1, game.map_subtiles_y + 1, sdx, sdy);
    move_grid(game.slabmap, sizeof(struct SlabMap), game.map_tiles_x, game.map_tiles_y, ctx->dslb_x, ctx->dslb_y);
    move_grid(game.slab_ext_data, sizeof(game.slab_ext_data[0]), game.map_tiles_x, game.map_tiles_y, ctx->dslb_x, ctx->dslb_y);
    move_grid(game.slab_ext_data_initial, sizeof(game.slab_ext_data_initial[0]), game.map_tiles_x, game.map_tiles_y, ctx->dslb_x, ctx->dslb_y);
    /* room slab lists are packed slab numbers */
    for (long i = 0; i < game.map_tiles_x * game.map_tiles_y; i++)
    {
        struct SlabMap *slb = &game.slabmap[i];
        if (slb->next_in_room != 0)
            slb->next_in_room = (SlabCodedCoords)shift_slb_num(slb->next_in_room, ctx);
    }
}

/******************************************************************************/
/* What the save tables don't describe */

static void shift_creature_raw_unions(struct WorldShiftCtx *ctx)
{
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
            continue;
        if (thing_is_creature_digger(thing))
        {
            cctrl->digger_working_stl = (SubtlCodedCoords)shift_stl_num_or_none(cctrl->digger_working_stl, ctx);
            cctrl->digger_task_stl = (SubtlCodedCoords)shift_stl_num_or_none(cctrl->digger_task_stl, ctx);
        }
        CrtrStateId states[2] = {thing->active_state, thing->continue_state};
        for (int k = 0; k < 2; k++)
        {
            switch (states[k])
            {
            case CrSt_AtTrainingRoom: case CrSt_Training:
                if (cctrl->training.pole_stl_x != 0)
                {
                    cctrl->training.pole_stl_x = (MapSubtlCoord)shift_stl(cctrl->training.pole_stl_x, ctx->dslb_x, game.map_subtiles_x);
                    cctrl->training.pole_stl_y = (MapSubtlCoord)shift_stl(cctrl->training.pole_stl_y, ctx->dslb_y, game.map_subtiles_y);
                }
                k = 2;
                break;
            case CrSt_AtWorkshopRoom: case CrSt_Manufacturing:
                if (cctrl->workshop.stl_x != 0)
                {
                    cctrl->workshop.stl_x = (MapSubtlCoord)shift_stl(cctrl->workshop.stl_x, ctx->dslb_x, game.map_subtiles_x);
                    cctrl->workshop.stl_y = (MapSubtlCoord)shift_stl(cctrl->workshop.stl_y, ctx->dslb_y, game.map_subtiles_y);
                }
                k = 2;
                break;
            case CrSt_AtScavengerRoom: case CrSt_Scavengering:
                if (cctrl->scavenge.stl_9D_x != 0)
                {
                    cctrl->scavenge.stl_9D_x = (MapSubtlCoord)shift_stl(cctrl->scavenge.stl_9D_x, ctx->dslb_x, game.map_subtiles_x);
                    cctrl->scavenge.stl_9D_y = (MapSubtlCoord)shift_stl(cctrl->scavenge.stl_9D_y, ctx->dslb_y, game.map_subtiles_y);
                }
                k = 2;
                break;
            default:
                break;
            }
        }
    }
}

/** Slab objects (torches, room furniture, heart flames) keep the slab they belong to in parent_idx. */
static void shift_slab_object_parents(struct WorldShiftCtx *ctx)
{
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing))
            continue;
        if ((thing->class_id != TCls_Object) && (thing->class_id != TCls_EffectGen))
            continue;
        /* thing->mappos is already moved: an attached object's old parent slab lies dslb further on */
        long slb_x = subtile_slab(thing->mappos.x.stl.num) + ctx->dslb_x;
        long slb_y = subtile_slab(thing->mappos.y.stl.num) + ctx->dslb_y;
        long p = thing->parent_idx;
        if ((p <= 0) || (p >= (long)game.map_tiles_x * game.map_tiles_y))
            continue;
        long px = p % game.map_tiles_x, py = p / game.map_tiles_x;
        if ((labs(px - slb_x) <= 1) && (labs(py - slb_y) <= 1))
            thing->parent_idx = (ThingIndex)shift_slb_num(p, ctx);
    }
}

static void shift_camera(struct Camera *cam, struct WorldShiftCtx *ctx)
{
    shift_pos_raw(&cam->mappos.x.val, &cam->mappos.y.val, ctx);
    shift_pos_raw(&cam->rotation_pivot.x.val, &cam->rotation_pivot.y.val, ctx);
}

static void shift_local_cameras(struct WorldShiftCtx *ctx)
{
    struct LocalCameraState *lcs = &local_state.camera;
    for (int i = 0; i < CamIV_EndList; i++)
    {
        shift_camera(&lcs->current[i], ctx);
        shift_camera(&lcs->previous[i], ctx);
        shift_camera(&lcs->destination[i], ctx);
    }
    shift_pos_raw(&lcs->move_target[0], &lcs->move_target[1], ctx);
    if (lcs->has_rotation_pivot)
        shift_pos_raw(&lcs->rotation_pivot_x, &lcs->rotation_pivot_y, ctx);
}

/******************************************************************************/
void world_shift_state(MapSlabCoord dslb_x, MapSlabCoord dslb_y)
{
    struct WorldShiftCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dslb_x = dslb_x;
    ctx.dslb_y = dslb_y;
    move_map_arrays(&ctx);
    walk_struct(get_desc_info(&save_desc_Game, 0), (uint8_t *)&game, &ctx, 0);
    shift_creature_raw_unions(&ctx);
    shift_slab_object_parents(&ctx);
    shift_local_cameras(&ctx);
    for (int i = 0; i < PLAYERS_COUNT; i++)
        memset(game.computer[i].soe_targets, 0, sizeof(game.computer[i].soe_targets));
    SYNCLOG("Shifted the world by (%d,%d) slabs, %ld fields moved", (int)dslb_x, (int)dslb_y, ctx.fields_moved);
}

void world_shift_rebuild(MapSubtlCoord band_sx, MapSubtlCoord band_sy, MapSubtlCoord band_ex, MapSubtlCoord band_ey)
{
    const TbClockMSec t0 = LbTimerClock();
    init_navigation();
    const TbClockMSec t1 = LbTimerClock();
    /* ceiling heights live in the map blocks and moved with them; only the new band and the frame changed */
    const MapSubtlCoord mx = game.map_subtiles_x - 1, my = game.map_subtiles_y - 1;
    ceiling_partially_recompute_heights(band_sx, band_sy, band_ex, band_ey);
    ceiling_partially_recompute_heights(0, 0, mx, STL_PER_SLB);
    ceiling_partially_recompute_heights(0, my - STL_PER_SLB, mx, my);
    ceiling_partially_recompute_heights(0, 0, STL_PER_SLB, my);
    ceiling_partially_recompute_heights(mx - STL_PER_SLB, 0, mx, my);
    const TbClockMSec t2 = LbTimerClock();
    light_rebuild_after_load();
    const TbClockMSec t3 = LbTimerClock();
    panel_map_update(0, 0, game.map_subtiles_x + 1, game.map_subtiles_y + 1);
    const TbClockMSec t4 = LbTimerClock();
    SYNCLOG("World: rebuild timing: navigation %ld, ceiling %ld, lights %ld, minimap %ld ms",
        (long)(t1 - t0), (long)(t2 - t1), (long)(t3 - t2), (long)(t4 - t3));
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl))
            cctrl->arid.may_need_reroute = 1;
    }
    game.map_changed_for_navigation = 1;
}

/******************************************************************************/
static TbBool name_looks_positional(const char *name)
{
    static const char *const words[] = {"stl", "slb", "pos", "coord", "subtile", "slab", "_x", "_y", "location",
        "camera", "cursor", "waypoint", "target"};
    char low[96];
    size_t n = strlen(name);
    if (n >= sizeof(low)) n = sizeof(low) - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[n] = 0;
    if ((strcmp(low, "x") == 0) || (strcmp(low, "y") == 0) || (strcmp(low, "cx") == 0) || (strcmp(low, "cy") == 0))
        return true;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strstr(low, words[i]) != NULL)
            return true;
    return false;
}

static int audit_struct(const struct SaveStructDesc *desc, const struct SaveStructDesc **seen, int *nseen, int depth)
{
    if (depth > 16)
        return 0;
    for (int i = 0; i < *nseen; i++)
        if (seen[i] == desc)
            return 0;
    if (*nseen < SHIFT_MEMO_MAX)
        seen[(*nseen)++] = desc;
    if ((strcmp(desc->name, "Coord3d") == 0) || (strcmp(desc->name, "Coord2d") == 0) || (strcmp(desc->name, "CoordDelta3d") == 0))
        return 0;
    int missing = 0;
    for (uint32_t f = 0; f < desc->field_count; f++)
    {
        const struct SaveFieldDesc *field = &desc->fields[f];
        const struct WorldShiftTag *tag = find_tag(desc->name, field->name);
        if ((tag != NULL) && (tag->kind == WSK_Keep))
            continue;
        if (field->flags & (SVF_DERIVED | SVF_RUNTIME))
            continue;
        if (field->stored_type == SV_STRUCT && field->sub != NULL)
        {
            const char *sn = field->sub->name;
            if ((tag == NULL) && ((strcmp(sn, "Coord3d") == 0) || (strcmp(sn, "Coord2d") == 0)))
            {
                WARNLOG("World shift: %s.%s (%s) is not in the shift table", desc->name, field->name, sn);
                missing++;
            }
            missing += audit_struct(field->sub, seen, nseen, depth + 1);
            continue;
        }
        if (field->stored_type == SV_UNION && field->uni != NULL)
        {
            for (uint32_t m = 0; m < field->uni->member_count; m++)
                if (field->uni->members[m].sub != NULL)
                    missing += audit_struct(field->uni->members[m].sub, seen, nseen, depth + 1);
            continue;
        }
        if ((field->flags & SVF_RAW) && (tag == NULL))
        {
            /* raw memory: CreatureControl unions are moved by hand (shift_creature_raw_unions) */
            if (strcmp(desc->name, "CreatureControl") != 0)
            {
                WARNLOG("World shift: raw field %s.%s is not covered", desc->name, field->name);
                missing++;
            }
            continue;
        }
        if ((tag == NULL) && (field->stored_type != SV_STR) && name_looks_positional(field->name))
        {
            WARNLOG("World shift: %s.%s looks like a position and is not in the shift table", desc->name, field->name);
            missing++;
        }
    }
    return missing;
}

int world_shift_audit(void)
{
    const struct SaveStructDesc *seen[SHIFT_MEMO_MAX];
    int nseen = 0;
    int missing = audit_struct(&save_desc_Game, seen, &nseen, 0);
    SYNCLOG("World shift audit: %d struct tables checked, %d position-like fields not in the table", nseen, missing);
    return missing;
}

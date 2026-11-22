/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_expedition.c
 *     Endless worlds: the Expedition spell (2026-11-21; see world_expedition.h).
 * @par Purpose:
 *     The user's rules are in the header. Decisions on what they left open (the recommended choice each time):
 *     - Available from a dungeon's start (no research), in every endless world (old worlds get it too).
 *     - Speed: the keeper's Imps' walking speed (base speed; about 16 turns a slab).
 *     - The Imps: the first three free Imps (idle ones first): not in a hand, possessed, fighting, unconscious, in
 *       custody, dragging something or carrying the Soul Container. They wait at the Heart, out of the world (the
 *       hold heroes waiting at an exit door use), and come back there.
 *     - Where the dots set off: walking from the Heart towards the place, the last slab the keeper owns before a gap
 *       of more than 12 slabs that are not theirs (the dungeon's outermost wall that way).
 *     - The finds, within 25 slabs of the leading dot (15 until 2026-11-22): rooms of the heroes (outposts, fortresses) and hero gates,
 *       portals not the keeper's, other keepers' Hearts (not allies'), spellbooks and specials not on the keeper's
 *       ground, neutral creatures; only on ground the keeper hasn't seen (seen ground needs no mark). One mark within
 *       6 slabs. A mark goes when the keeper's view reaches its slab; at most 64 a keeper (the oldest goes).
 *     - The chunks round the leading dot are loaded while it walks (as raiders load theirs), so what lies there can
 *       be found; nothing is shown to the keeper by it.
 *     - At most 8 expeditions at once a keeper. Start again / Reset Dungeon brings its Imps back and clears its marks.
 *     Saved: EXP1 (the expeditions and the marks).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_expedition.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bflib_basics.h"
#include "bflib_video.h"
#include "chat_box.h"
#include "config.h"
#include "config_creature.h"
#include "config_crtrstates.h"
#include "config_effects.h"
#include "config_magic.h"
#include "config_spritecolors.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_spdig.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "gui_frontmenu.h"
#include "gui_parchment.h"
#include "magic_powers.h"
#include "map_data.h"
#include "net_main.h"
#include "net_state.h"
#include "packets.h"
#include "player_data.h"
#include "power_hand.h"
#include "room_data.h"
#include "room_list.h"
#include "server_admin.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_stats.h"
#include "vidmode.h"
#include "world_colours.h"
#include "world_stream.h"
#include "world_progress.h"
#include "world_expedition_icons.h"
#include "vidfade.h"
#include "player_instances.h"
#include "kfx/renderer/RendererManager.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEX_COST_NAME "POWER_EXPEDITION"
#define WEX_IMPS 3
#define WEX_RADIUS 25        /* slabs round the leading dot where things are found */
#define WEX_MARK_APART 6     /* slabs: one mark here */
#define WEX_GAP 12           /* slabs not the keeper's that end the dungeon on the line out */
#define WEX_SCAN_TURNS 8
#define WEX_SPACING 1.4      /* slabs between the dots */

enum WexFind { WExF_Structure = 1, WExF_Portal, WExF_Keeper, WExF_Book, WExF_Special, WExF_Neutral };
static const char *const wex_find_names[] = {"", "structure", "portal", "keeper", "spellbook", "special", "neutral creatures"};

#define WEX_TAB(p) (game.world_expedition[p])
#define WEX_MARKS(p) (game.world_expedition_mark[p])

static TbBool wex_player_ok(PlayerNumber p)
{
    return (p >= 0) && (p < PLAYERS_COUNT) && (p < PLAYERS_MAX);
}

/** Game state changes here (not on a joined machine, which gets the state from the server). */
static TbBool wex_runs(void)
{
    return world_mode_active() && !netstate_is_client();
}

PowerKind world_expedition_power_kind(void)
{
    const long k = get_id(power_desc, WEX_COST_NAME);
    /* (0 when the config has none: PowerKind is unsigned, so -1 would be a power) */
    return ((k > 0) && (k < game.conf.magic_conf.power_types_count)) ? (PowerKind)k : 0;
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: expedition, to player %d: %s", (int)p, text);
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

/******************************************************************************/
/* places */

static void wex_world_of_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y, int64_t *wx, int64_t *wy)
{
    world_slab_to_world(subtile_slab(stl_x), subtile_slab(stl_y), wx, wy);
}

/** Map coordinates of a world place in slabs (fractions too), loaded or not; false when the engine can't say. */
static TbBool wex_map_of_world(double wx, double wy, int32_t *mx, int32_t *my)
{
    const double fx = floor(wx), fy = floor(wy);
    MapSlabCoord ex, ey;
    if (!world_engine_slab_unloaded((int64_t)fx, (int64_t)fy, &ex, &ey) &&
        !world_engine_slab((int64_t)fx, (int64_t)fy, &ex, &ey))
        return false;
    const double x = ((double)ex + (wx - fx)) * COORD_PER_STL * STL_PER_SLB;
    const double y = ((double)ey + (wy - fy)) * COORD_PER_STL * STL_PER_SLB;
    if ((fabs(x) > 2.0e9) || (fabs(y) > 2.0e9))
        return false;
    *mx = (int32_t)x;
    *my = (int32_t)y;
    return true;
}

/** Where an expedition's dot k is now (world slabs, fractions too); k 0 leads. */
static void wex_dot_at(const struct WorldExpedition *e, int k, double *wx, double *wy)
{
    const double dx = (double)e->to_x - e->from_x, dy = (double)e->to_y - e->from_y;
    const double len = sqrt(dx * dx + dy * dy);
    double done = (e->walk_turns > 0) ? (double)(game.play_gameturn - (GameTurn)e->start_turn) / e->walk_turns : 1.0;
    if (done < 0.0)
        done = 0.0;
    if (done > 1.0)
        done = 1.0;
    double along = done * len - k * WEX_SPACING;
    if (along < 0.0)
        along = 0.0;
    const double ux = (len > 0.0) ? dx / len : 0.0, uy = (len > 0.0) ? dy / len : 0.0;
    /* the two behind walk a little to either side of the line */
    const double side = (k == 1) ? 0.5 : (k == 2) ? -0.5 : 0.0;
    *wx = e->from_x + 0.5 + ux * along - uy * side * ((along > 0.0) ? 1.0 : 0.0);
    *wy = e->from_y + 0.5 + uy * along + ux * side * ((along > 0.0) ? 1.0 : 0.0);
}

/******************************************************************************/
/* the Imps */

TbBool world_expedition_imp(const struct Thing *thing)
{
    return thing_exists(thing) && (thing->class_id == TCls_Creature) && (thing->active_state == CrSt_ImpOnExpedition);
}

short imp_on_expedition(struct Thing *thing)
{
    /* away: nothing to do until world_expedition_turn brings it back */
    return CrStRet_Unchanged;
}

static TbBool wex_carries_heart(const struct Thing *thing, PlayerNumber p)
{
    for (int k = 0; k < 4; k++)
        if (game.world_migration[p].imp[k] == thing->index)
            return true;
    return false;
}

static TbBool wex_imp_free(const struct Thing *thing, PlayerNumber p)
{
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->owner != p) ||
        !thing_is_creature_special_digger(thing))
        return false;
    if (thing_is_picked_up(thing) || ((thing->alloc_flags & TAlF_IsControlled) != 0) || (thing->health <= 0))
        return false;
    if (creature_is_being_unconscious(thing) || creature_is_kept_in_custody(thing) ||
        creature_is_dragging_or_being_dragged(thing) || creature_is_dying(thing))
        return false;
    if (world_expedition_imp(thing) || wex_carries_heart(thing, p))
        return false;
    return get_creature_state_type(thing) != CrStTyp_FightCrtr;
}

/** Up to WEX_IMPS free Imps of the keeper's, idle ones first. */
static int wex_choose_imps(PlayerNumber p, struct Thing **out)
{
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return 0;
    int n = 0;
    for (int pass = 0; (pass < 2) && (n < WEX_IMPS); pass++)
    {
        long guard = 0;
        for (ThingIndex i = dungeon->digger_list_start; (i > 0) && (n < WEX_IMPS) && (guard++ <= (long)THINGS_COUNT);)
        {
            struct Thing *thing = thing_get(i);
            if (thing_is_invalid(thing))
                break;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            i = cctrl->players_next_creature_idx;
            if (!wex_imp_free(thing, p))
                continue;
            const TbBool idle = (get_creature_state_type(thing) == CrStTyp_Idle);
            if ((pass == 0) != idle)
                continue;
            TbBool had = false;
            for (int k = 0; k < n; k++)
                had |= (out[k] == thing);
            if (!had)
                out[n++] = thing;
        }
    }
    return n;
}

static void wex_heart_pos(PlayerNumber p, struct Coord3d *pos, TbBool *found)
{
    struct Thing *heartng = get_player_soul_container(p);
    *found = thing_exists(heartng);
    if (*found)
        *pos = heartng->mappos;
}

static void wex_send_imp(struct Thing *thing, PlayerNumber p)
{
    initialise_thing_state(thing, CrSt_ImpOnExpedition);
    create_effect(&thing->mappos, ball_puff_effects[world_display_colour(p)], p);
    place_thing_in_creature_controlled_limbo(thing);
    /* it waits at the Heart (out of the world): its chunk stays loaded, and it comes back there */
    struct Coord3d pos;
    TbBool found;
    wex_heart_pos(p, &pos, &found);
    if (found)
        thing->mappos = pos;
}

static void wex_return_imp(PlayerNumber p, ThingIndex idx, int32_t made, int k)
{
    struct Thing *thing = thing_get(idx);
    if ((idx <= 0) || !thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->owner != p) ||
        ((int32_t)thing->creation_turn != made) || ((thing->state_flags & TF1_InCtrldLimbo) == 0))
        return;
    struct Coord3d pos = thing->mappos;
    TbBool found;
    wex_heart_pos(p, &pos, &found);
    /* round the Heart, as new Imps come */
    static const int ox[WEX_IMPS] = {-256, 256, 0}, oy[WEX_IMPS] = {192, 192, -256};
    pos.x.val += ox[k % WEX_IMPS];
    pos.y.val += oy[k % WEX_IMPS];
    pos.z.val = get_floor_height_at(&pos);
    thing->mappos = pos;
    remove_thing_from_creature_controlled_limbo(thing);
    set_start_state(thing);
    create_effect(&thing->mappos, ball_puff_effects[world_display_colour(p)], p);
}

static void wex_end(PlayerNumber p, int i, const char *why)
{
    struct WorldExpedition *e = &WEX_TAB(p)[i];
    if (!e->used)
        return;
    for (int k = 0; k < WEX_IMPS; k++)
        wex_return_imp(p, e->imp[k], e->imp_made[k], k);
    SYNCLOG("World: expedition %d of player %d ends (%s) at world slab (%ld,%ld)", i, (int)p, why, (long)e->to_x,
        (long)e->to_y);
    memset(e, 0, sizeof(*e));
}

/******************************************************************************/
/* setting off */

/** The dungeon's outermost slab on the line from the Heart towards a world slab (the Heart's slab when none). */
static void wex_set_off_place(PlayerNumber p, int64_t hx, int64_t hy, int64_t tx, int64_t ty, int64_t *ox, int64_t *oy)
{
    *ox = hx;
    *oy = hy;
    const double dx = (double)(tx - hx), dy = (double)(ty - hy);
    const double len = sqrt(dx * dx + dy * dy);
    int gap = 0;
    for (int s = 1; s <= (int)len; s++)
    {
        const int64_t x = hx + (int64_t)llround(dx * s / len), y = hy + (int64_t)llround(dy * s / len);
        MapSlabCoord ex, ey;
        TbBool owned = false;
        if (world_engine_slab(x, y, &ex, &ey))
            owned = (slabmap_owner(get_slabmap_block(ex, ey)) == p);
        if (owned)
        {
            *ox = x;
            *oy = y;
            gap = 0;
        } else if (++gap > WEX_GAP)
            break;
    }
}

TbResult world_expedition_cast(PlayerNumber p, int64_t wx, int64_t wy, TbBool free)
{
    if (!wex_runs() || !wex_player_ok(p))
        return Lb_FAIL;
    const PowerKind kind = world_expedition_power_kind();
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if ((kind <= 0) || dungeon_invalid(dungeon) || !is_power_available(p, kind))
        return Lb_FAIL;
    if (world_apart_slab(wx, wy))
    {
        tell_keeper(p, "No expedition can reach that place.");
        return Lb_FAIL;
    }
    struct Thing *heartng = get_player_soul_container(p);
    if (!thing_exists(heartng))
        return Lb_FAIL;
    int slot = -1;
    for (int i = 0; (i < WORLD_EXPEDITIONS_MAX) && (slot < 0); i++)
        if (!WEX_TAB(p)[i].used)
            slot = i;
    if (slot < 0)
    {
        tell_keeper(p, "Eight expeditions are out already.");
        return Lb_FAIL;
    }
    struct Thing *imps[WEX_IMPS] = {NULL};
    if (wex_choose_imps(p, imps) < WEX_IMPS)
    {
        tell_keeper(p, "An expedition needs 3 free Imps.");
        return Lb_FAIL;
    }
    if (!free && !pay_for_spell(p, kind, 0))
        return Lb_FAIL;
    int64_t hx, hy, ox, oy;
    wex_world_of_subtile(heartng->mappos.x.stl.num, heartng->mappos.y.stl.num, &hx, &hy);
    wex_set_off_place(p, hx, hy, wx, wy, &ox, &oy);
    struct WorldExpedition *e = &WEX_TAB(p)[slot];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->from_x = (int32_t)ox;
    e->from_y = (int32_t)oy;
    e->to_x = (int32_t)wx;
    e->to_y = (int32_t)wy;
    e->start_turn = (int32_t)game.play_gameturn;
    /* the Imps' walking speed (map units a turn) */
    const struct CreatureModelConfig *crconf = creature_stats_get(get_players_special_digger_model(p));
    const int speed = max(16, (int)crconf->base_speed);
    const double dist = sqrt((double)(wx - ox) * (wx - ox) + (double)(wy - oy) * (wy - oy));
    e->walk_turns = max(20, (int32_t)(dist * COORD_PER_STL * STL_PER_SLB / speed));
    for (int k = 0; k < WEX_IMPS; k++)
    {
        e->imp[k] = imps[k]->index;
        e->imp_made[k] = (int32_t)imps[k]->creation_turn;
        wex_send_imp(imps[k], p);
    }
    SYNCLOG("World: expedition %d of player %d sets off from world slab (%ld,%ld) to (%ld,%ld): %.0f slabs, %d turns, "
        "Imps %d %d %d", slot, (int)p, (long)ox, (long)oy, (long)wx, (long)wy, dist, (int)e->walk_turns,
        (int)e->imp[0], (int)e->imp[1], (int)e->imp[2]);
    return Lb_SUCCESS;
}

TbResult world_expedition_cast_at_subtile(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y, TbBool free)
{
    if (!world_mode_active())
        return Lb_FAIL;
    int64_t wx, wy;
    wex_world_of_subtile(stl_x, stl_y, &wx, &wy);
    return world_expedition_cast(p, wx, wy, free);
}

TbBool world_expedition_map_click(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_mode_active())
        return false;
    const PowerKind kind = world_expedition_power_kind();
    const struct UserState *ustate = get_local_user_state_prediction();
    if ((kind <= 0) || user_state_invalid(ustate) || (ustate->chosen_power_kind != kind) ||
        (ustate->work_state != PSt_CastPowerOnSubtile))
        return false;
    int64_t wx, wy;
    wex_world_of_subtile(stl_x, stl_y, &wx, &wy);
    if ((wx < INT32_MIN) || (wx > INT32_MAX) || (wy < INT32_MIN) || (wy > INT32_MAX))
        return false;
    set_players_packet_action(get_my_player(), PckA_WorldExpedition, (long)wx, (long)wy, 0, 0);
    SYNCLOG("World: expedition asked for at world slab (%ld,%ld) from the full map", (long)wx, (long)wy);
    return true;
}

TbBool world_expedition_map_pointer(void)
{
    if (!world_mode_active())
        return false;
    const PowerKind kind = world_expedition_power_kind();
    const struct UserState *ustate = get_local_user_state_prediction();
    if ((kind <= 0) || user_state_invalid(ustate) || (ustate->chosen_power_kind != kind) ||
        (ustate->work_state != PSt_CastPowerOnSubtile))
        return false;
    const struct PowerConfigStats *powerst = get_power_model_stats(kind);
    set_pointer_graphic_spell(get_player_colored_pointer_icon_idx(powerst->pointer_sprite_idx, my_player_number),
        get_gameturn());
    return true;
}

void world_expedition_packet(PlayerNumber p, int32_t wx, int32_t wy)
{
    world_expedition_cast(p, wx, wy, false);
}

/******************************************************************************/
/* finding */

/** A spellbook, or one of the research books of endless worlds (their own object: world_progress.c). */
static TbBool wex_is_book(const struct Thing *thing)
{
    return thing_is_spellbook(thing) || (thing_is_object(thing) && (world_progress_book_model() > 0) &&
        (thing->model == world_progress_book_model()));
}

struct WexScan {
    PlayerNumber p;
    double x, y; /* the leading dot (world slabs) */
};

static void wex_add_mark(PlayerNumber p, int64_t wx, int64_t wy, int kind)
{
    struct WorldExpeditionMark *m = WEX_MARKS(p);
    int free_at = -1;
    for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
    {
        if (!m[i].used)
        {
            if (free_at < 0)
                free_at = i;
            continue;
        }
        if ((llabs(m[i].x - wx) <= WEX_MARK_APART) && (llabs(m[i].y - wy) <= WEX_MARK_APART))
            return;
    }
    if (free_at < 0)
    {
        /* full: the oldest (the first) goes, the others move up */
        memmove(&m[0], &m[1], sizeof(m[0]) * (WORLD_EXPEDITION_MARKS - 1));
        free_at = WORLD_EXPEDITION_MARKS - 1;
    }
    m[free_at].x = (int32_t)wx;
    m[free_at].y = (int32_t)wy;
    m[free_at].kind = (uint8_t)kind;
    m[free_at].used = 1;
    SYNCLOG("World: expedition of player %d finds a %s at world slab (%ld,%ld)", (int)p, wex_find_names[kind],
        (long)wx, (long)wy);
}

/** A find at an engine subtile: marked for each expedition within reach whose keeper hasn't seen it. */
static void wex_found(const struct WexScan *scans, int nscans, MapSubtlCoord stl_x, MapSubtlCoord stl_y, int kind,
    PlayerNumber owner)
{
    int64_t wx, wy;
    wex_world_of_subtile(stl_x, stl_y, &wx, &wy);
    for (int s = 0; s < nscans; s++)
    {
        const PlayerNumber p = scans[s].p;
        const double dx = (wx + 0.5) - scans[s].x, dy = (wy + 0.5) - scans[s].y;
        if (dx * dx + dy * dy > (double)WEX_RADIUS * WEX_RADIUS)
            continue;
        if (owner == p)
            continue;
        if ((kind == WExF_Keeper) && players_are_mutual_allies(p, owner))
            continue;
        if (subtile_revealed(stl_x, stl_y, p))
            continue;
        wex_add_mark(p, wx, wy, kind);
    }
}

static void wex_scan(const struct WexScan *scans, int nscans)
{
    /* rooms: the heroes', portals, Hearts */
    for (struct Room *room = start_rooms; room < end_rooms; room++)
    {
        if (!room_exists(room))
            continue;
        int kind = 0;
        if (room_role_matches(room->kind, RoRoF_KeeperStorage))
            kind = ((room->owner != PLAYER_GOOD) && (room->owner != game.neutral_player_num)) ? WExF_Keeper : 0;
        else if (room_role_matches(room->kind, RoRoF_CrPoolSpawn))
            kind = WExF_Portal;
        else if (room->owner == PLAYER_GOOD)
            kind = WExF_Structure;
        if (kind != 0)
            wex_found(scans, nscans, room->central_stl_x, room->central_stl_y, kind, room->owner);
    }
    /* objects: spellbooks and specials not on a keeper's own ground, hero gates */
    const struct StructureList *objects = get_list_for_thing_class(TCls_Object);
    long guard = 0;
    for (ThingIndex i = objects->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!thing_exists(thing) || thing_is_picked_up(thing))
            continue;
        int kind = 0;
        if (wex_is_book(thing))
            kind = WExF_Book;
        else if (thing_is_special_box(thing))
            kind = WExF_Special;
        else if (object_is_hero_gate(thing))
            kind = WExF_Structure;
        if (kind == 0)
            continue;
        const PlayerNumber ground = (PlayerNumber)slabmap_owner(get_slabmap_thing_is_on(thing));
        wex_found(scans, nscans, thing->mappos.x.stl.num, thing->mappos.y.stl.num, kind,
            (kind == WExF_Structure) ? PLAYER_GOOD : ground);
    }
    /* neutral creatures */
    const struct StructureList *creatures = get_list_for_thing_class(TCls_Creature);
    guard = 0;
    for (ThingIndex i = creatures->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!thing_exists(thing) || thing_is_picked_up(thing) || !is_neutral_thing(thing))
            continue;
        wex_found(scans, nscans, thing->mappos.x.stl.num, thing->mappos.y.stl.num, WExF_Neutral, thing->owner);
    }
}

/** Marks whose slab the keeper has seen now go. */
static void wex_marks_seen(PlayerNumber p)
{
    struct WorldExpeditionMark *m = WEX_MARKS(p);
    for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
    {
        MapSlabCoord ex, ey;
        if (!m[i].used || !world_engine_slab(m[i].x, m[i].y, &ex, &ey))
            continue;
        if (subtile_revealed(slab_subtile_center(ex), slab_subtile_center(ey), p))
        {
            SYNCLOG("World: expedition mark of player %d at world slab (%ld,%ld) seen", (int)p, (long)m[i].x,
                (long)m[i].y);
            memset(&m[i], 0, sizeof(m[i]));
        }
    }
}

/******************************************************************************/
/* upkeep */

/** Imps away that no expedition holds (a save without EXP1, an expedition cleared) come back. */
static void wex_strays(void)
{
    const struct StructureList *creatures = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = creatures->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!world_expedition_imp(thing) || !wex_player_ok(thing->owner))
            continue;
        TbBool held = false;
        for (int e = 0; (e < WORLD_EXPEDITIONS_MAX) && !held; e++)
        {
            const struct WorldExpedition *x = &WEX_TAB(thing->owner)[e];
            for (int k = 0; (k < WEX_IMPS) && x->used; k++)
                held |= (x->imp[k] == thing->index) && (x->imp_made[k] == (int32_t)thing->creation_turn);
        }
        if (!held)
        {
            SYNCLOG("World: expedition: Imp %d of player %d away with no expedition, back", (int)thing->index,
                (int)thing->owner);
            wex_return_imp(thing->owner, thing->index, (int32_t)thing->creation_turn, 0);
        }
    }
}

/******************************************************************************/
/* Test hook KFX_WORLD_EXPTEST="T,dx,dy[,mode]" (local, single player): at T this machine's keeper gets 3 Imps if it
   has fewer and the gold, and casts an Expedition towards its Heart + (dx, dy) world slabs, then (T+10) a second one
   towards (+dy, -dx). "EXPTEST" lines every 100 turns: expeditions out, marks, Imps away, the creature list's Imp
   count and how many of them can be picked up; when both are back, each Imp is checked ("EXPTEST ERROR" when one is
   still held). Mode 1: the spell is chosen (the pointer, for screenshots); mode 2: chosen too, and the full map opens at T+200 on the
   first expedition's leading dot; mode 3: a mark 9 slabs north-east of the Heart at T+1 and the full map at T+30; mode 4: chosen with
   the spell page open (its panel button). */

static long wext_at = -1;
static int wext_dx = 0, wext_dy = 0, wext_mode = 0;
static ThingIndex wext_imps[2 * WEX_IMPS];
static int wext_imps_n = 0;
static TbBool wext_done = false;

static void wext_counts(PlayerNumber p, int *listed, int *pickable)
{
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    *listed = *pickable = 0;
    if (dungeon_invalid(dungeon))
        return;
    const ThingModel m = get_players_special_digger_model(p);
    for (int s = 0; s < STATE_TYPES_COUNT; s++)
        *listed += dungeon->crmodel_state_type_count[m][s];
    for (int j = 0; j < 3; j++)
        *pickable += dungeon->guijob_all_creatrs_count[m][j];
}

static void wext_turn(void)
{
    static TbBool read = false;
    if (!read)
    {
        read = true;
        const char *v = getenv("KFX_WORLD_EXPTEST");
        if ((v != NULL) && (sscanf(v, "%ld,%d,%d,%d", &wext_at, &wext_dx, &wext_dy, &wext_mode) >= 3))
            SYNCLOG("World: test switch KFX_WORLD_EXPTEST=%s", v);
        else
            wext_at = -1;
    }
    if ((wext_at < 0) || (game.game_kind == GKind_MultiGame))
        return;
    const long t = (long)game.play_gameturn;
    const PlayerNumber p = my_player_number;
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    int64_t hx, hy;
    wex_world_of_subtile(heart->mappos.x.stl.num, heart->mappos.y.stl.num, &hx, &hy);
    if (t == wext_at - 40)
    {
        int have = 0;
        struct Thing *imps[WEX_IMPS];
        have = wex_choose_imps(p, imps);
        for (int k = have; k < WEX_IMPS + 1; k++)
            magic_use_available_power_on_subtile(p, PwrK_MKDIGGER, 0, heart->mappos.x.stl.num + 2 * (k - 1),
                heart->mappos.y.stl.num + 4, PwCast_None, PwMod_CastForFree);
        player_add_offmap_gold(p, 110000);
        SYNCLOG("World: EXPTEST: turn %ld, %d free Imps before, gold given", t, have);
    }
    if ((t == wext_at) || (t == wext_at + 10))
    {
        const TbBool first = (t == wext_at);
        const int64_t tx = hx + (first ? wext_dx : wext_dy), ty = hy + (first ? wext_dy : -wext_dx);
        int listed0, pick0;
        wext_counts(p, &listed0, &pick0);
        const struct Dungeon *dungeon = get_players_num_dungeon(p);
        const GoldAmount gold0 = dungeon->total_money_owned;
        const TbResult r = world_expedition_cast(p, tx, ty, false);
        int listed1, pick1;
        wext_counts(p, &listed1, &pick1);
        SYNCLOG("World: EXPTEST: turn %ld, cast to world slab (%ld,%ld): %s, gold %ld -> %ld, list (before the count "
            "is redone) %d/%d", t, (long)tx, (long)ty, (r == Lb_SUCCESS) ? "set off" : "REFUSED", (long)gold0,
            (long)dungeon->total_money_owned, listed1, pick1);
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
        {
            const struct WorldExpedition *e = &WEX_TAB(p)[i];
            for (int k = 0; (k < WEX_IMPS) && e->used && (e->start_turn == (int32_t)game.play_gameturn); k++)
                if (wext_imps_n < 2 * WEX_IMPS)
                    wext_imps[wext_imps_n++] = e->imp[k];
        }
        if (first && ((wext_mode == 1) || (wext_mode == 2) || (wext_mode == 4)))
            set_user_work_state(get_my_user(), PSt_CastPowerOnSubtile, world_expedition_power_kind());
        if (first && (wext_mode == 4))
            set_menu_mode(GMnu_SPELL); /* (mode 4: the spell page with its button, for the panel's shots) */
    }
    if ((wext_mode == 3) && (t == wext_at + 1))
        wex_add_mark(p, hx + 9, hy - 9, WExF_Portal); /* (mode 3: a mark near the Heart, for the minimap's shots) */
    if ((wext_mode == 3) && (t == wext_at + 30))
        zoom_to_parchment_map();
    if ((wext_mode == 1) && (t >= wext_at))
    {
        /* the pointer over the 3D view, right of the middle (the shots show the spell's pointer playing) */
        lbDisplay.MMouseX = lbDisplay.MouseX = (long)lbDisplay.PhysicalScreenWidth * 2 / 3;
        lbDisplay.MMouseY = lbDisplay.MouseY = (long)lbDisplay.PhysicalScreenHeight / 3;
    }
    if ((wext_mode == 2) && (t == wext_at + 200))
    {
        zoom_to_parchment_map();
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
            if (WEX_TAB(p)[i].used)
            {
                double x, y;
                wex_dot_at(&WEX_TAB(p)[i], 0, &x, &y);
                world_parchment_centre_on((int64_t)x, (int64_t)y);
                break;
            }
    }
    if ((t > wext_at) && ((t - wext_at) % 100 == 0) && !wext_done)
    {
        int out = 0, marks = 0, away = 0, listed, pick;
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
            out += WEX_TAB(p)[i].used ? 1 : 0;
        for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
            marks += WEX_MARKS(p)[i].used ? 1 : 0;
        for (int k = 0; k < wext_imps_n; k++)
            away += world_expedition_imp(thing_get(wext_imps[k])) ? 1 : 0;
        wext_counts(p, &listed, &pick);
        SYNCLOG("World: EXPTEST: turn %ld, %d out, %d marks, %d of %d Imps away, the list: %d Imps, %d can be picked "
            "up", t, out, marks, away, wext_imps_n, listed, pick);
        /* what lies round each leading dot, found or not (is its ground loaded?) */
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
        {
            const struct WorldExpedition *e = &WEX_TAB(p)[i];
            if (!e->used)
                continue;
            double x, y;
            wex_dot_at(e, 0, &x, &y);
            MapSlabCoord ex, ey;
            const TbBool loaded = world_engine_slab((int64_t)x, (int64_t)y, &ex, &ey);
            int crtrs = 0, neutral = 0, books = 0, boxes = 0, rooms = 0, seen = 0;
            const struct StructureList *cl = get_list_for_thing_class(TCls_Creature);
            for (ThingIndex ci = cl->index; ci > 0;)
            {
                struct Thing *th = thing_get(ci);
                if (thing_is_invalid(th))
                    break;
                ci = th->next_of_class;
                int64_t wx, wy;
                wex_world_of_subtile(th->mappos.x.stl.num, th->mappos.y.stl.num, &wx, &wy);
                if ((wx - x) * (wx - x) + (wy - y) * (wy - y) > WEX_RADIUS * WEX_RADIUS)
                    continue;
                crtrs++;
                neutral += is_neutral_thing(th) ? 1 : 0;
                seen += subtile_revealed(th->mappos.x.stl.num, th->mappos.y.stl.num, p) ? 1 : 0;
            }
            const struct StructureList *ol = get_list_for_thing_class(TCls_Object);
            for (ThingIndex oi = ol->index; oi > 0;)
            {
                struct Thing *th = thing_get(oi);
                if (thing_is_invalid(th))
                    break;
                oi = th->next_of_class;
                int64_t wx, wy;
                wex_world_of_subtile(th->mappos.x.stl.num, th->mappos.y.stl.num, &wx, &wy);
                if ((wx - x) * (wx - x) + (wy - y) * (wy - y) > WEX_RADIUS * WEX_RADIUS)
                    continue;
                books += wex_is_book(th) ? 1 : 0;
                boxes += thing_is_special_box(th) ? 1 : 0;
            }
            for (struct Room *room = start_rooms; room < end_rooms; room++)
            {
                if (!room_exists(room))
                    continue;
                int64_t wx, wy;
                wex_world_of_subtile(room->central_stl_x, room->central_stl_y, &wx, &wy);
                rooms += ((wx - x) * (wx - x) + (wy - y) * (wy - y) <= WEX_RADIUS * WEX_RADIUS) ? 1 : 0;
            }
            if (i == 0)
            {
                int all_neutral = 0, all_books = 0, all_boxes = 0, all_hero_rooms = 0, all_hearts = 0;
                double near_n = 1e9, near_b = 1e9;
                for (ThingIndex ci = cl->index; ci > 0;)
                {
                    struct Thing *th = thing_get(ci);
                    if (thing_is_invalid(th))
                        break;
                    ci = th->next_of_class;
                    if (!is_neutral_thing(th))
                        continue;
                    all_neutral++;
                    int64_t wx, wy;
                    wex_world_of_subtile(th->mappos.x.stl.num, th->mappos.y.stl.num, &wx, &wy);
                    near_n = fmin(near_n, sqrt((wx - x) * (wx - x) + (wy - y) * (wy - y)));
                }
                for (ThingIndex oi = ol->index; oi > 0;)
                {
                    struct Thing *th = thing_get(oi);
                    if (thing_is_invalid(th))
                        break;
                    oi = th->next_of_class;
                    if (thing_is_special_box(th))
                        all_boxes++;
                    if (!wex_is_book(th))
                        continue;
                    all_books++;
                    int64_t wx, wy;
                    wex_world_of_subtile(th->mappos.x.stl.num, th->mappos.y.stl.num, &wx, &wy);
                    near_b = fmin(near_b, sqrt((wx - x) * (wx - x) + (wy - y) * (wy - y)));
                }
                for (struct Room *room = start_rooms; room < end_rooms; room++)
                    if (room_exists(room))
                    {
                        all_hero_rooms += (room->owner == PLAYER_GOOD) ? 1 : 0;
                        all_hearts += room_role_matches(room->kind, RoRoF_KeeperStorage) ? 1 : 0;
                    }
                SYNCLOG("World: EXPTEST:   loaded anywhere: %d neutral creatures (nearest %.0f slabs), %d spellbooks "
                    "(nearest %.0f), %d specials, %d hero rooms, %d Hearts", all_neutral, near_n, all_books, near_b,
                    all_boxes, all_hero_rooms, all_hearts);
            }
            SYNCLOG("World: EXPTEST:   expedition %d's dot at world slab (%.0f,%.0f), its ground %s: %d creatures "
                "(%d neutral, %d seen), %d spellbooks, %d specials, %d rooms within %d slabs", i, x, y,
                loaded ? "loaded" : "NOT loaded", crtrs, neutral, seen, books, boxes, rooms, WEX_RADIUS);
        }
        if (out == 0)
        {
            wext_done = true;
            int errors = 0;
            for (int k = 0; k < wext_imps_n; k++)
            {
                const struct Thing *thing = thing_get(wext_imps[k]);
                const TbBool back = thing_exists(thing) && ((thing->state_flags & TF1_InCtrldLimbo) == 0) &&
                    !world_expedition_imp(thing) && ((thing->rendering_flags & TRF_Invisible) == 0);
                errors += back ? 0 : 1;
                SYNCLOG("World: EXPTEST%s: Imp %d %s at subtile (%d,%d), state %d", back ? "" : " ERROR",
                    (int)wext_imps[k], back ? "back" : "NOT back", thing_exists(thing) ? (int)thing->mappos.x.stl.num : -1,
                    thing_exists(thing) ? (int)thing->mappos.y.stl.num : -1,
                    thing_exists(thing) ? (int)thing->active_state : -1);
            }
            for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
                if (WEX_MARKS(p)[i].used)
                    SYNCLOG("World: EXPTEST: mark %d: %s at world slab (%ld,%ld)", i,
                        wex_find_names[WEX_MARKS(p)[i].kind], (long)WEX_MARKS(p)[i].x, (long)WEX_MARKS(p)[i].y);
            SYNCLOG("World: EXPTEST: done, %d errors", errors);
        }
    }
}

void world_expedition_turn(void)
{
    if (!wex_runs())
        return;
    wext_turn();
    struct WexScan scans[PLAYERS_MAX * WORLD_EXPEDITIONS_MAX];
    int nscans = 0;
    const TbBool scan_now = ((game.play_gameturn % WEX_SCAN_TURNS) == 0);
    for (PlayerNumber p = 0; wex_player_ok(p); p++)
    {
        struct PlayerInfo *player = get_player(p);
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
        {
            struct WorldExpedition *e = &WEX_TAB(p)[i];
            if (!e->used)
                continue;
            if (!player_exists(player) || (player->victory_state == VicS_LostLevel) ||
                !thing_exists(get_player_soul_container(p)))
            {
                wex_end(p, i, "the keeper is gone");
                continue;
            }
            /* its Imps stay away: a state another system set is put back (its Imps hold still out of the world) */
            for (int k = 0; k < WEX_IMPS; k++)
            {
                struct Thing *thing = thing_get(e->imp[k]);
                if ((e->imp[k] > 0) && thing_exists(thing) && ((int32_t)thing->creation_turn == e->imp_made[k]) &&
                    (thing->active_state != CrSt_ImpOnExpedition))
                {
                    SYNCLOG("World: expedition: Imp %d was set to state %d while away, held again", (int)thing->index,
                        (int)thing->active_state);
                    internal_set_thing_state(thing, CrSt_ImpOnExpedition);
                    if ((thing->state_flags & TF1_InCtrldLimbo) == 0)
                        place_thing_in_creature_controlled_limbo(thing);
                }
            }
            if ((GameTurnDelta)(game.play_gameturn - (GameTurn)e->start_turn) >= e->walk_turns)
            {
                wex_end(p, i, "arrived");
                continue;
            }
            if (scan_now && (nscans < (int)(sizeof(scans) / sizeof(scans[0]))))
            {
                scans[nscans].p = p;
                wex_dot_at(e, 0, &scans[nscans].x, &scans[nscans].y);
                nscans++;
            }
        }
        if ((game.play_gameturn % 32) == (GameTurn)(p % 32))
            wex_marks_seen(p);
        /* the spell from a dungeon's start (old worlds too) */
        if (((game.play_gameturn % 64) == 3) && player_exists(player) && !player_is_roaming(p))
        {
            const PowerKind kind = world_expedition_power_kind();
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            if ((kind > 0) && !dungeon_invalid(dungeon) && (dungeon->magic_level[kind] == 0) &&
                thing_exists(get_player_soul_container(p)))
                set_power_available(p, kind, 1, 1);
        }
    }
    if (nscans > 0)
        wex_scan(scans, nscans);
    if ((game.play_gameturn % 64) == 17)
        wex_strays();
}

void world_expedition_want_chunks(void (*want)(int64_t wx, int64_t wy, int r))
{
    if (!world_mode_active())
        return;
    for (PlayerNumber p = 0; wex_player_ok(p); p++)
    {
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
        {
            const struct WorldExpedition *e = &WEX_TAB(p)[i];
            if (!e->used)
                continue;
            double x, y;
            wex_dot_at(e, 0, &x, &y);
            want((int64_t)floor(x), (int64_t)floor(y), 2); /* (two chunks: the finds reach 25 slabs) */
            /* and ahead, so that its ground is there before it */
            const double dx = (double)e->to_x - e->from_x, dy = (double)e->to_y - e->from_y;
            const double len = sqrt(dx * dx + dy * dy);
            if (len > 1.0)
                want((int64_t)floor(x + dx / len * 12.0), (int64_t)floor(y + dy / len * 12.0), 1);
        }
    }
}

void world_expedition_seat_reset(PlayerNumber p)
{
    if (!wex_player_ok(p))
        return;
    for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
        wex_end(p, i, "the dungeon starts again");
    memset(WEX_MARKS(p), 0, sizeof(game.world_expedition_mark[0]));
}

/******************************************************************************/
/* drawing (every machine: from struct Game) */

void world_expedition_view(PlayerNumber p, struct WorldExpeditionView *v)
{
    v->dots = v->pings = v->marks = 0;
    if (!world_mode_active() || !wex_player_ok(p))
        return;
    for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
    {
        const struct WorldExpedition *e = &WEX_TAB(p)[i];
        if (!e->used)
            continue;
        if ((v->pings < WEX_VIEW_MAX) &&
            wex_map_of_world(e->to_x + 0.5, e->to_y + 0.5, &v->ping_x[v->pings], &v->ping_y[v->pings]))
            v->pings++;
        for (int k = 0; k < WEX_IMPS; k++)
        {
            double x, y;
            wex_dot_at(e, k, &x, &y);
            if ((v->dots < WEX_VIEW_MAX) && wex_map_of_world(x, y, &v->dot_x[v->dots], &v->dot_y[v->dots]))
                v->dots++;
        }
    }
    const struct WorldExpeditionMark *m = WEX_MARKS(p);
    for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
        if (m[i].used && (v->marks < WEX_VIEW_MAX) &&
            wex_map_of_world(m[i].x + 0.5, m[i].y + 0.5, &v->mark_x[v->marks], &v->mark_y[v->marks]))
            v->marks++;
}

unsigned char world_expedition_colour(void)
{
    return colours[15][0][0];
}

/******************************************************************************/
/* the icons */

static long wex_cursor_icon = 0;
static long wex_qmark_icon = 0;

void world_expedition_register_icons(void)
{
    const TbBool lowres = (RendererPhysicalWidth() <= LOWRES_SCREEN_SIZE);
    const unsigned char *big[2] = {lowres ? wei_big_lo : wei_big, lowres ? wei_big_lo_dis : wei_big_dis};
    const unsigned int big_len[2] = {lowres ? wei_big_lo_len : wei_big_len, lowres ? wei_big_lo_dis_len : wei_big_dis_len};
    const unsigned char *btn[2] = {lowres ? wei_button_lo : wei_button, lowres ? wei_button_lo_dis : wei_button_dis};
    const unsigned int btn_len[2] = {lowres ? wei_button_lo_len : wei_button_len,
        lowres ? wei_button_lo_dis_len : wei_button_dis_len};
    const unsigned char *cur[8] = {wei_cursor0, wei_cursor1, wei_cursor2, wei_cursor3, wei_cursor4, wei_cursor5,
        wei_cursor6, wei_cursor7};
    const unsigned int cur_len[8] = {wei_cursor0_len, wei_cursor1_len, wei_cursor2_len, wei_cursor3_len,
        wei_cursor4_len, wei_cursor5_len, wei_cursor6_len, wei_cursor7_len};
    const unsigned char *qm[1] = {wei_qmark};
    const unsigned int qm_len[1] = {wei_qmark_len};
    custom_sprites_add_builtin_icon("ENDLESS_EXPEDITION_BIG", big, big_len, 2);
    custom_sprites_add_builtin_icon("ENDLESS_EXPEDITION_BUTTON", btn, btn_len, 2);
    /* the pointer's eight frames one after another: the spell pointer plays them (vidmode.c, frame = turn % 8) */
    wex_cursor_icon = custom_sprites_add_builtin_icon("ENDLESS_EXPEDITION_CURSOR", cur, cur_len, 8);
    wex_qmark_icon = custom_sprites_add_builtin_icon("ENDLESS_EXPEDITION_QMARK", qm, qm_len, 1);
}

TbBool world_expedition_pointer_hotspot(long spridx, long *hot_x, long *hot_y)
{
    if ((wex_cursor_icon <= 0) || (spridx < wex_cursor_icon) || (spridx >= wex_cursor_icon + 8))
        return false;
    const struct TbSprite *spr = get_new_icon_sprite((short)spridx);
    if (spr == NULL)
        return false;
    *hot_x = spr->SWidth / 2;
    *hot_y = spr->SHeight / 2;
    return true;
}

long world_expedition_qmark_icon(void)
{
    return wex_qmark_icon;
}

void world_expedition_qmark_bitmap(const unsigned char **index, const unsigned char **mask, int *w, int *h)
{
    *index = wei_qmark_index;
    *mask = wei_qmark_mask;
    *w = WEI_QMARK_W;
    *h = WEI_QMARK_H;
}

/******************************************************************************/
/* the state */

void world_expedition_states_fallback(void)
{
    struct CreatureStateConfig *st = game.conf.crtr_conf.states;
    const int idx = CrSt_ImpOnExpedition;
    if ((idx < game.conf.crtr_conf.states_count) && (st[idx].process_state != 0))
        return;
    memset(&st[idx], 0, sizeof(st[0]));
    snprintf(st[idx].name, sizeof(st[0].name), "%s", "ImpOnExpedition");
    st[idx].process_state = 139;
    st[idx].state_type = CrStTyp_OwnNeeds;
    if (game.conf.crtr_conf.states_count <= idx)
        game.conf.crtr_conf.states_count = idx + 1;
}

/******************************************************************************/
/* EXP1 (an extra of the world save): per expedition 16 int32 (player, slot, from x y, to x y, start, walk, the three
   Imps and their creation turns, 0), then per mark 4 int32 (player, x, y, kind) */

static void wex_counts(uint32_t *ne, uint32_t *nm)
{
    *ne = *nm = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
            *ne += game.world_expedition[p][i].used ? 1 : 0;
        for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
            *nm += game.world_expedition_mark[p][i].used ? 1 : 0;
    }
}

size_t world_expedition_save_size(void)
{
    uint32_t ne, nm;
    wex_counts(&ne, &nm);
    return ((ne == 0) && (nm == 0)) ? 0 : (8 + (size_t)ne * 64 + (size_t)nm * 16);
}

void world_expedition_save(uint8_t *buf)
{
    uint32_t ne, nm;
    wex_counts(&ne, &nm);
    memcpy(buf, &ne, 4);
    memcpy(buf + 4, &nm, 4);
    uint8_t *at = buf + 8;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        for (int i = 0; i < WORLD_EXPEDITIONS_MAX; i++)
        {
            const struct WorldExpedition *e = &game.world_expedition[p][i];
            if (!e->used)
                continue;
            const int32_t v[16] = {(int32_t)p, i, e->from_x, e->from_y, e->to_x, e->to_y, e->start_turn, e->walk_turns,
                e->imp[0], e->imp[1], e->imp[2], e->imp_made[0], e->imp_made[1], e->imp_made[2], 0, 0};
            memcpy(at, v, sizeof(v));
            at += sizeof(v);
        }
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        for (int i = 0; i < WORLD_EXPEDITION_MARKS; i++)
        {
            const struct WorldExpeditionMark *m = &game.world_expedition_mark[p][i];
            if (!m->used)
                continue;
            const int32_t v[4] = {(int32_t)p, m->x, m->y, m->kind};
            memcpy(at, v, sizeof(v));
            at += sizeof(v);
        }
}

TbBool world_expedition_load(const uint8_t *buf, size_t len)
{
    uint32_t ne = 0, nm = 0;
    if (len < 8)
        return false;
    memcpy(&ne, buf, 4);
    memcpy(&nm, buf + 4, 4);
    if ((ne > (len - 8) / 64) || (nm > (len - 8 - (size_t)ne * 64) / 16))
        return false;
    world_expedition_reset();
    const uint8_t *at = buf + 8;
    for (uint32_t k = 0; k < ne; k++, at += 64)
    {
        int32_t v[16];
        memcpy(v, at, sizeof(v));
        if ((v[0] < 0) || (v[0] >= PLAYERS_MAX) || (v[1] < 0) || (v[1] >= WORLD_EXPEDITIONS_MAX))
            continue;
        struct WorldExpedition *e = &game.world_expedition[v[0]][v[1]];
        e->used = 1;
        e->from_x = v[2];
        e->from_y = v[3];
        e->to_x = v[4];
        e->to_y = v[5];
        e->start_turn = v[6];
        e->walk_turns = v[7];
        for (int i = 0; i < WEX_IMPS; i++)
        {
            e->imp[i] = (uint16_t)v[8 + i];
            e->imp_made[i] = v[11 + i];
        }
    }
    int fill[PLAYERS_MAX] = {0};
    for (uint32_t k = 0; k < nm; k++, at += 16)
    {
        int32_t v[4];
        memcpy(v, at, sizeof(v));
        if ((v[0] < 0) || (v[0] >= PLAYERS_MAX) || (fill[v[0]] >= WORLD_EXPEDITION_MARKS))
            continue;
        struct WorldExpeditionMark *m = &game.world_expedition_mark[v[0]][fill[v[0]]++];
        m->x = v[1];
        m->y = v[2];
        m->kind = (uint8_t)v[3];
        m->used = 1;
    }
    return true;
}

void world_expedition_reset(void)
{
    memset(game.world_expedition, 0, sizeof(game.world_expedition));
    memset(game.world_expedition_mark, 0, sizeof(game.world_expedition_mark));
}

#ifdef __cplusplus
}
#endif

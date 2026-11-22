/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_fill.c
 *     Endless worlds: the Fill room (Imps fill marked floor with earth) - since 2026-11-22 a cave-in fill.
 * @par Purpose:
 *     The user, 2026-11-22: the Fill room becomes a spell-like ability with the Cave-In icon. Its button and highlight are
 *     the Fill room's, but only the keeper's own claimed floor with nothing on it (no creature, enemy, body, object, trap,
 *     room) can be chosen: anything else is red. Each slab placed costs 50 gold (terrain.cfg) and fills at once: the
 *     Cave-In spell's falling rocks, dust, quake and sound on that slab, and dirt rising from the floor (SLAB50's four
 *     heights, one cube more each step), then earth. Such a fill is a mark with "cave" set (struct Game world_fill[]: a
 *     thin client sees it; saved in FIL1, owner + 0x100); the Imps leave it alone. Imp marks of older saves are still
 *     filled by Imps as described below.
 *
 *     See world_fill.h. FILL is a room kind of terrain.cfg (the panel button, the highlight, the cursor, the big icon
 *     with its price all come with that), but no room is ever built: keeper_build_room hands its slabs here. A placed
 *     slab is a mark (struct Game world_fill[], so a thin client draws it; the world save's FIL1). Marks are world
 *     slabs (the engine window moves).
 *
 *     The Imps. The digger stack gets a DigTsk_FillSlab task per mark (spdigger_stack.c); an Imp that takes one walks to
 *     the side of the slab (the subtile next to it on a floor slab beside it, as for reinforcing a wall), digs at it
 *     (the dig animation, CrInst_DIG: instf_dig hands each stroke here), and after six strokes the slab becomes earth.
 *     While a creature, a body or an object is on the slab, the Imp waits (it gives up after 400 turns).
 *
 *     No creature shut in. Filling a slab can cut the floor round it in two. Before an Imp takes the task and again at
 *     its last stroke, the floor round the slab is searched (orthogonal steps, 24 slabs about it, the slab itself
 *     counted as filled): each part of it that does not reach out of that box (nor holds the keeper's Heart) is a
 *     pocket. The fill waits while a pocket has a creature in it, or a slab still marked, or the Imp's own slab is in
 *     one, or every part is a pocket. An empty pocket may be closed (an area can be sealed); marked areas are filled
 *     from the inside out. A mark may be placed under creatures; the keeper's own creatures on it (not walking) are told
 *     to go at least 4 slabs off when it is placed and every 16 turns after (the user, 2026-11-19). Those in a pocket
 *     the fill would close are sent out the same way, to the filling Imp's side, the moment it is there (the user, 2026-11-19: an Imp reinforcing the walls
 *     there moves away); an enemy, or a creature that cannot be moved (held, fighting, carried), keeps it waiting. Several Imps filling one area therefore never
 *     wall one of them in: the last slab that would do it waits until the Imp stands outside.
 *
 *     Sell Room takes marks away (no gold back: the user, 2026-11-19).
 *
 * Test hook: KFX_WORLD_FILLTEST=turn: at that turn this machine's keeper gets a claimed floor 7x7 east of its Heart
 *     (loaded ground only), 3,000 gold, and marks: a 3x3 block (filled from outside in, the middle last), and the opening
 *     of an earth pocket a creature is kept in until 1,200 turns later (the opening must wait, then fill once the
 *     creature is taken out). Every fill is checked afterwards: "FILLTEST ERROR" if anybody was shut in. With
 *     KFX_WORLD_FILLTEST=turn,1 the FILL room is also chosen (build mode) for screenshots of the panel.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_fill.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "bflib_sound.h"
#include "config_magic.h"
#include "config_sounds.h"
#include "config_terrain.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_crtrstates.h"
#include "creature_control.h"
#include "creature_instances.h"
#include "creature_states.h"
#include "creature_states_spdig.h"
#include "dungeon_data.h"
#include "engine_render.h"
#include "engine_camera.h"
#include "frontend.h"
#include "frontmenu_ingame_tabs.h"
#include "game_legacy.h"
#include "gui_soundmsgs.h"
#include "map_blocks.h"
#include "map_data.h"
#include "map_utils.h"
#include "net_state.h"
#include "player_data.h"
#include "player_utils.h"
#include "room_data.h"
#include "slab_data.h"
#include "sounds.h"
#include "spdigger_stack.h"
#include "thing_data.h"
#include "thing_creature.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "thing_shots.h"
#include "thing_stats.h"
#include "thing_traps.h"
#include "creature_states_mood.h"
#include "config_magic.h"
#include "thing_objects.h"
#include "magic_powers.h"
#include "world_stream.h"
#include "world_fill_icons.h"
#include "custom_sprites.h"
#include "bflib_video.h"
#include "kfx/renderer/RendererManager.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define WF_PLAYER_MARKS 512   /* marks one keeper may have at a time */
#define WF_STROKES 6          /* dig strokes to fill a slab */
#define WF_WAIT_MAX 400       /* turns an Imp waits at a slab with something on it */
#define WF_RECHECK 64         /* turns before a waiting ("blocked") mark is looked at again */
#define WF_WALK_MAX 300       /* turns an Imp may take to walk to the mark it took */
#define WF_BOX 24             /* the pocket search: slabs about the mark */
#define WF_BOX_SIDE (2 * WF_BOX + 1)
#define WF_DRAW_MAX 160       /* wireframes drawn in one frame */

#define wfm game.world_fill

/******************************************************************************/
/* places */

/** The mark's slab in the engine window; false when that ground is not loaded. */
static TbBool wf_engine(const struct WorldFillMark *m, MapSlabCoord *ex, MapSlabCoord *ey)
{
    if (!world_mode_active())
    {
        *ex = m->x;
        *ey = m->y;
        return true;
    }
    return world_engine_slab(m->x, m->y, ex, ey);
}

static void wf_world(MapSlabCoord x, MapSlabCoord y, int32_t *wx, int32_t *wy)
{
    if (!world_mode_active())
    {
        *wx = x;
        *wy = y;
        return;
    }
    int64_t ox, oy;
    world_slab_to_world(x, y, &ox, &oy);
    *wx = (int32_t)ox;
    *wy = (int32_t)oy;
}

static SubtlCodedCoords wf_center_stl(MapSlabCoord x, MapSlabCoord y)
{
    return get_subtile_number(slab_subtile_center(x), slab_subtile_center(y));
}

/** An Imp's mark (not a cave-in fill). */
#define WF_IMP_MARK(m) ((m)->used && ((m)->cave == 0))
#define WF_CAVE_STEP 8   /* turns between the dirt's heights (SLAB50 frames 0-3: 1 to 4 cubes) */
#define WF_CAVE_FRAMES 4
#define WF_CAVE_WAIT 400 /* turns a finished fill waits for its slab to be clear before it gives up (floor again) */

/** The mark on that engine slab (any keeper's), or -1. */
static int wf_find(MapSlabCoord x, MapSlabCoord y)
{
    int32_t wx, wy;
    wf_world(x, y, &wx, &wy);
    for (int i = 0; i < WORLD_FILL_MAX; i++)
        if (wfm[i].used && (wfm[i].x == wx) && (wfm[i].y == wy))
            return i;
    return -1;
}

static int wf_count(PlayerNumber plyr_idx)
{
    int n = 0;
    for (int i = 0; i < WORLD_FILL_MAX; i++)
        if (WF_IMP_MARK(&wfm[i]) && (wfm[i].owner == plyr_idx))
            n++;
    return n;
}

int world_fill_room_kind(void)
{
    static int cached = -2;
    static int cached_count = -1;
    if ((cached == -2) || (cached_count != game.conf.slab_conf.room_types_count))
    {
        cached_count = game.conf.slab_conf.room_types_count;
        const long k = get_id(room_desc, "FILL");
        cached = ((k > 0) && (k < game.conf.slab_conf.room_types_count)) ? (int)k : -1;
    }
    return cached;
}

TbBool world_fill_is_kind(RoomKind rkind)
{
    const int k = world_fill_room_kind();
    return (k > 0) && (rkind == k);
}

enum WfThings {
    WfT_All = 0,       /* a creature, a body, an object, a trap or a door */
    WfT_Creatures,     /* a creature */
    WfT_NotCreatures,  /* the rest (a mark may be placed under creatures: the user, 2026-11-19) */
};

static TbBool wf_slab_has_things(MapSlabCoord x, MapSlabCoord y, int what)
{
    const TbBool creatures_only = (what == WfT_Creatures);
    for (int dy = 0; dy < STL_PER_SLB; dy++)
    {
        for (int dx = 0; dx < STL_PER_SLB; dx++)
        {
            const struct Map *mapblk = get_map_block_at(slab_subtile(x, dx), slab_subtile(y, dy));
            if (map_block_invalid(mapblk))
                continue;
            ThingIndex i = get_mapwho_thing_index(mapblk);
            int k = 0;
            while ((i != 0) && (k++ < 256))
            {
                const struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if (thing_is_picked_up(thing))
                    continue;
                switch (thing->class_id)
                {
                case TCls_Creature:
                    if (what != WfT_NotCreatures)
                        return true;
                    break;
                case TCls_Object:
                    /* a wall's torch hangs over the floor beside it (and barrels, statues, anvils): what a room placed
                       there would clear does not keep a mark or a fill back (2026-11-19 test: Imps waited at torches) */
                    if (get_object_model_stats(thing->model)->model_flags & OMF_DestroyedOnRoomPlace)
                        break;
                    if (!creatures_only)
                        return true;
                    break;
                case TCls_DeadCreature:
                case TCls_Trap:
                case TCls_Door:
                    if (!creatures_only)
                        return true;
                    break;
                default:
                    break;
                }
            }
        }
    }
    return false;
}

/** Floor a mark may lie on: own claimed floor or unclaimed path, no room. */
static TbBool wf_floor_for(PlayerNumber plyr_idx, MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || (slb->room_index > 0))
        return false;
    if (slb->kind == SlbT_CLAIMED)
        return (slabmap_owner(slb) == plyr_idx);
    return (slb->kind == SlbT_PATH);
}

TbBool world_fill_can_mark(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    /* the cave-in fill (2026-11-22): the keeper's own claimed floor only, with nothing on it (red otherwise) */
    const struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb) || (slb->room_index > 0) || (slb->kind != SlbT_CLAIMED) || (slabmap_owner(slb) != plyr_idx))
        return false;
    if (wf_find(slb_x, slb_y) >= 0)
        return false;
    return !wf_slab_has_things(slb_x, slb_y, WfT_All);
}

TbBool world_fill_marked_by(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const int i = wf_find(slb_x, slb_y);
    return (i >= 0) && (wfm[i].owner == plyr_idx);
}

static void wf_send_off_mark(MapSlabCoord x, MapSlabCoord y, PlayerNumber owner);

/******************************************************************************/
/* placing and selling */

TbBool world_fill_place(int user, PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const int rk = world_fill_room_kind();
    struct PlayerInfo *player = get_player(plyr_idx);
    struct Dungeon *dungeon = get_players_dungeon(player);
    if ((rk <= 0) || dungeon_invalid(dungeon) || !is_room_available(plyr_idx, rk))
        return false;
    if (!can_build_room_at_slab(plyr_idx, rk, slb_x, slb_y))
        return false;
    int free_at = -1;
    for (int i = 0; (i < WORLD_FILL_MAX) && (free_at < 0); i++)
        if (!wfm[i].used)
            free_at = i;
    if (free_at < 0)
    {
        if (is_my_player_number(plyr_idx))
            output_message(SMsg_WorkerJobsLimit, 500);
        return false;
    }
    const struct RoomConfigStats *roomst = get_room_kind_stats(rk);
    if ((dungeon->total_money_owned < roomst->cost) || (take_money_from_dungeon(plyr_idx, roomst->cost, 1) < 0))
    {
        output_player_message(plyr_idx, SMsg_GoldNotEnough, 0);
        return false;
    }
    /* a cave-in fill: it begins now (wf_cave_turn) */
    struct WorldFillMark *m = &wfm[free_at];
    memset(m, 0, sizeof(*m));
    wf_world(slb_x, slb_y, &m->x, &m->y);
    m->owner = (uint8_t)plyr_idx;
    m->cave = 1;
    m->check_turn = (int32_t)game.play_gameturn;
    m->used = 1;
    if (is_my_player_number(plyr_idx))
    {
        play_non_3d_sample(snd_tile_place);
        const struct UserState *ustate = (user >= 0) ? get_user_state(user) : NULL;
        if ((ustate != NULL) && !user_state_invalid(ustate) && (ustate->boxsize > 2)) /* (more slabs follow this one, as a room's) */
        {
            play_non_3d_sample(snd_larg_tile_down);
            play_non_3d_sample(snd_larg_tile_up);
        }
    }
    SYNCDBG(8, "Player %d brings the ceiling down on engine slab (%d,%d)", (int)plyr_idx, (int)slb_x, (int)slb_y);
    return true;
}

TbBool world_fill_unmark(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const int i = wf_find(slb_x, slb_y);
    if ((i < 0) || (wfm[i].owner != plyr_idx) || wfm[i].cave)
        return false;
    memset(&wfm[i], 0, sizeof(wfm[i])); /* (no gold back; an Imp at it sees it gone and leaves) */
    play_player_sample(plyr_idx, snd_tile_sell); /* (the Sell Room sound, as for a room's slab: the user, 2026-11-20) */
    return true;
}

int world_fill_meter(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return -1;
    const long pending = wf_count(plyr_idx);
    const long done = game.world_fill_done[plyr_idx];
    if (pending + done == 0)
        return -1;
    return (int)(done * 256 / (pending + done));
}

/******************************************************************************/
/* the wireframes */

void world_fill_draw_marks(long cam_x, long cam_y, long cam_z, int frontview, void *cam, unsigned char stl_width)
{
    const PlayerNumber me = my_player_number;
    const struct MapVolumeBox saved = map_volume_box;
    int drawn = 0;
    const long reach = 40 * COORD_PER_STL; /* (marks farther from the camera are off the screen) */
    for (int i = 0; (i < WORLD_FILL_MAX) && (drawn < WF_DRAW_MAX); i++)
    {
        const struct WorldFillMark *m = &wfm[i];
        MapSlabCoord x, y;
        if (!WF_IMP_MARK(m) || (m->owner != me) || !wf_engine(m, &x, &y))
            continue;
        const long bx = subtile_coord(slab_subtile(x, 0), 0), by = subtile_coord(slab_subtile(y, 0), 0);
        if (saved.visible && (bx >= min(saved.beg_x, saved.end_x)) && (bx < max(saved.beg_x, saved.end_x)) &&
            (by >= min(saved.beg_y, saved.end_y)) && (by < max(saved.beg_y, saved.end_y)))
            continue; /* (the cursor's box - Sell Room's, a room's - is drawn there, not the mark: the user, 2026-11-20) */
        if (!frontview && ((labs(bx - cam_x) > reach) || (labs(by - cam_y) > reach)))
            continue;
        map_volume_box.beg_x = bx;
        map_volume_box.beg_y = by;
        map_volume_box.end_x = subtile_coord(slab_subtile(x + 1, 0), 0);
        map_volume_box.end_y = subtile_coord(slab_subtile(y + 1, 0), 0);
        map_volume_box.floor_height_z = 1;
        if (frontview)
            create_frontview_map_volume_box((struct Camera *)cam, stl_width, SLC_DARKGREEN);
        else
            create_map_volume_box(cam_x, cam_y, cam_z, SLC_DARKGREEN);
        drawn++;
    }
    map_volume_box = saved;
}

/******************************************************************************/
/* nobody shut in */

static uint16_t wf_label[WF_BOX_SIDE * WF_BOX_SIDE];
static int32_t wf_queue[WF_BOX_SIDE * WF_BOX_SIDE];
static uint8_t wf_marked[WF_BOX_SIDE * WF_BOX_SIDE];

/** 1: floor a creature walks over; 0: not (walls, earth, rock, lava, the slab being filled); 2: ground not loaded. */
static int wf_open(MapSlabCoord x, MapSlabCoord y, MapSlabCoord fx, MapSlabCoord fy)
{
    if ((x == fx) && (y == fy))
        return 0;
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return 2;
    if (slb->kind == SlbT_LAVA)
        return 0;
    const struct Map *mapblk = get_map_block_at(slab_subtile_center(x), slab_subtile_center(y));
    if (map_block_invalid(mapblk))
        return 2;
    if ((mapblk->flags & SlbAtFlg_Blocking) && !(mapblk->flags & SlbAtFlg_IsDoor))
        return 0;
    return 1;
}

enum WfCheck {
    WfC_Strict = 0, /* any creature in the way counts */
    WfC_Plan,       /* the keeper's own creatures that can be sent away do not count (an Imp may take the task) */
    WfC_Shoo,       /* they are sent away now (4 slabs off, on the filling Imp's side), and count until they are out */
    WfC_Away,       /* the mark's own call: the keeper's creatures on it are sent 4 slabs off */
};

/** The keeper's creature can be told to walk away (not held, fighting, carried, knocked out or dragging). */
static TbBool wf_movable(const struct Thing *c)
{
    if (thing_is_picked_up(c) || creature_is_dying(c) || creature_is_being_unconscious(c) || creature_is_kept_in_custody(c) ||
        creature_is_dragging_or_being_dragged(c))
        return false;
    const CrtrStateId st = get_creature_state_besides_interruptions(c);
    if ((st == CrSt_ImpFills) || (st == CrSt_ImpArrivesAtFill))
        return false; /* (an Imp filling the next mark from here: it is waited for, not sent off - the test, 2026-11-19) */
    /* work, even deep work (an Imp reinforcing), is broken off: walking does not override it by the game's own rules, so
       an Imp reinforcing on a mark stayed (the user, 2026-11-20: "Imps need to stop and move away") */
    const long stype = get_creature_state_type(c);
    if ((stype == CrStTyp_Work) || (stype == CrStTyp_DeepWork) || (stype == CrStTyp_Idle))
        return true;
    return can_change_from_state_to(c, c->active_state, CrSt_MoveToPosition) && (stype != CrStTyp_FightCrtr);
}

static TbBool wf_standable(MapSlabCoord x, MapSlabCoord y, int pass);
static TbBool wf_log_moves = false; /* (KFX_WORLD_FILLTEST: every creature a mark sends off is logged) */
/* A waiting mark's own call (no Imp at it): creatures in a pocket go out to this open side's slab */
static TbBool wf_shoo_side = false;
static MapSlabCoord wf_shoo_sx, wf_shoo_sy;

/* Away: the user, 2026-11-19: "The Wireframe itself should tell creatures to stop what they are doing and move away at
   least 4 slabs away". The place they go: open floor at least 4 slabs from the mark (not marked, not kept out), the
   nearest over open ground from where the search starts (the mark, or the filling Imp's slab: its side). */
#define WF_AWAY 4
#define WF_AWAY_R 12
#define WF_AWAY_SIDE (2 * WF_AWAY_R + 1)
static uint8_t wf_away_seen[WF_AWAY_SIDE * WF_AWAY_SIDE];
static int16_t wf_away_q[WF_AWAY_SIDE * WF_AWAY_SIDE];

static TbBool wf_away_spot(PlayerNumber owner, MapSlabCoord kmx, MapSlabCoord kmy, MapSlabCoord sx, MapSlabCoord sy,
    MapSlabCoord *ox, MapSlabCoord *oy)
{
    static const signed char d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    memset(wf_away_seen, 0, sizeof(wf_away_seen));
    int qn = 0;
    const int start = WF_AWAY_R * WF_AWAY_SIDE + WF_AWAY_R;
    wf_away_seen[start] = 1;
    wf_away_q[qn++] = (int16_t)start;
    for (int qi = 0; qi < qn; qi++)
    {
        const int c = wf_away_q[qi];
        const MapSlabCoord cx = sx - WF_AWAY_R + c % WF_AWAY_SIDE, cy = sy - WF_AWAY_R + c / WF_AWAY_SIDE;
        if ((max(abs(cx - kmx), abs(cy - kmy)) >= WF_AWAY) && wf_standable(cx, cy, 0) && !world_fill_keep_out(owner, cx, cy))
        {
            *ox = cx;
            *oy = cy;
            return true;
        }
        for (int e = 0; e < 4; e++)
        {
            const MapSlabCoord ax = cx + d4[e][0], ay = cy + d4[e][1];
            if ((abs(ax - sx) > WF_AWAY_R) || (abs(ay - sy) > WF_AWAY_R) || (wf_open(ax, ay, kmx, kmy) != 1))
                continue;
            const int ac = (ay - sy + WF_AWAY_R) * WF_AWAY_SIDE + (ax - sx + WF_AWAY_R);
            if (wf_away_seen[ac])
                continue;
            wf_away_seen[ac] = 1;
            wf_away_q[qn++] = (int16_t)ac;
        }
    }
    return false;
}

/** The creatures on a slab in the fill's way. The user, 2026-11-19: they are prompted to move away the moment an Imp
 *  is there to finish it ("if an imp was reinforcing a wall adjacent to the pocket ... that imp should move away").
 *  True when one is in the way (WfC_Plan: one that cannot be sent away). */
static TbBool wf_creatures_in_way(MapSlabCoord x, MapSlabCoord y, PlayerNumber owner, int mode, const struct Thing *worker,
    MapSlabCoord kmx, MapSlabCoord kmy)
{
    TbBool in_way = false;
    int away = 0; /* 0: not looked for yet, 1: found, -1: none */
    MapSlabCoord ax = 0, ay = 0;
    for (int k = 0; k < STL_PER_SLB * STL_PER_SLB; k++)
    {
        const struct Map *mapblk = get_map_block_at(slab_subtile(x, k % 3), slab_subtile(y, k / 3));
        ThingIndex i = map_block_invalid(mapblk) ? 0 : get_mapwho_thing_index(mapblk);
        for (int n = 0; (i != 0) && (n < 64); n++)
        {
            struct Thing *c = thing_get(i);
            if (thing_is_invalid(c))
                break;
            i = c->next_on_mapblk;
            if (!thing_is_creature(c) || thing_is_picked_up(c) || (c == worker))
                continue;
            const TbBool own = (c->owner == owner) && wf_movable(c);
            if (mode == WfC_Plan)
            {
                /* (an Imp on its way to fill, or filling, stands outside by the time it counts: the last stroke looks) */
                const CrtrStateId cst = get_creature_state_besides_interruptions(c);
                const TbBool filler = (c->owner == owner) && ((cst == CrSt_ImpFills) || (cst == CrSt_ImpArrivesAtFill));
                if (!own && !filler)
                    in_way = true;
                continue;
            }
            in_way = true;
            if (((mode == WfC_Shoo) || (mode == WfC_Away)) && own && (c->active_state != CrSt_MoveToPosition))
            {
                /* (one walking - passing by, or sent off already - is let be) */
                if (away == 0)
                {
                    MapSlabCoord sx = (worker != NULL) ? subtile_slab(worker->mappos.x.stl.num) : kmx;
                    MapSlabCoord sy = (worker != NULL) ? subtile_slab(worker->mappos.y.stl.num) : kmy;
                    if ((worker == NULL) && (mode == WfC_Shoo) && wf_shoo_side)
                    {
                        sx = wf_shoo_sx;
                        sy = wf_shoo_sy;
                    }
                    away = wf_away_spot(owner, kmx, kmy, sx, sy, &ax, &ay) ? 1 : -1;
                }
                if (wf_log_moves)
                    SYNCLOG("World: Fill: %s %d (%s) %s from engine slab (%d,%d)", thing_model_name(c), (int)c->index,
                        creature_state_code_name(get_creature_state_besides_interruptions(c)),
                        (away > 0) ? "sent 4+ slabs off" : "has nowhere to go", (int)kmx, (int)kmy);
                if (away > 0)
                {
                    cleanup_current_thing_state(c); /* (its job is ended as the game ends one: a room's place, a held thing) */
                    clear_creature_instance(c);
                    if (setup_person_move_to_position(c, slab_subtile_center(ax), slab_subtile_center(ay), NavRtF_Default))
                        c->continue_state = creature_is_for_dungeon_diggers_list(c) ? CrSt_ImpLastDidJob : CrSt_CreatureDoingNothing;
                    else
                        set_start_state(c);
                }
            }
        }
    }
    return in_way;
}

/* Keep out: the pockets a waiting fill would close (found by its WfC_Shoo checks, kept 32 turns). Imps do not choose
   to reinforce from there, nor from a marked slab (world_fill_keep_out): the 2026-11-19 test's Imp, sent out of a
   pocket, went straight back in to reinforce its walls. Engine slabs (they last only moments). */
#define WF_KEEP_SETS 8
#define WF_KEEP_SLABS 512
static struct { PlayerNumber owner; GameTurnDelta until; int n; MapSlabCoord x[WF_KEEP_SLABS], y[WF_KEEP_SLABS]; } wf_keep[WF_KEEP_SETS];

TbBool world_fill_keep_out(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (wf_find(slb_x, slb_y) >= 0)
        return true;
    for (int s = 0; s < WF_KEEP_SETS; s++)
    {
        if ((wf_keep[s].n == 0) || (wf_keep[s].owner != plyr_idx) || ((GameTurnDelta)game.play_gameturn > wf_keep[s].until))
            continue;
        for (int k = 0; k < wf_keep[s].n; k++)
            if ((wf_keep[s].x[k] == slb_x) && (wf_keep[s].y[k] == slb_y))
                return true;
    }
    return false;
}

/** The mark's own call: the keeper's creatures on it (not walking) go 4 slabs off. */
static void wf_send_off_mark(MapSlabCoord x, MapSlabCoord y, PlayerNumber owner)
{
    wf_creatures_in_way(x, y, owner, WfC_Away, NULL, x, y);
}

/** Would filling engine slab (fx,fy) shut a creature in? stand: the Imp's slab (have_stand false: not known yet). */
static TbBool wf_fill_safe(PlayerNumber owner, MapSlabCoord fx, MapSlabCoord fy, TbBool have_stand,
    MapSlabCoord stand_x, MapSlabCoord stand_y, const char **why, int mode, const struct Thing *worker)
{
    static const signed char d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    MapSlabCoord hx = -100000, hy = -100000;
    const struct Thing *heart = get_player_soul_container(owner);
    if (thing_exists(heart))
    {
        hx = subtile_slab(heart->mappos.x.stl.num);
        hy = subtile_slab(heart->mappos.y.stl.num);
    }
    memset(wf_label, 0, sizeof(wf_label));
    memset(wf_marked, 0, sizeof(wf_marked));
    for (int i = 0; i < WORLD_FILL_MAX; i++)
    {
        MapSlabCoord kx, ky;
        if (WF_IMP_MARK(&wfm[i]) && wf_engine(&wfm[i], &kx, &ky) && (abs(kx - fx) <= WF_BOX) && (abs(ky - fy) <= WF_BOX))
            wf_marked[(ky - fy + WF_BOX) * WF_BOX_SIDE + (kx - fx + WF_BOX)] = 1;
    }
    int nlabel = 0;
    TbBool escapes[5] = {false, false, false, false, false};
    int qbeg[5] = {0}, qend[5] = {0};
    int qn = 0;
    for (int d = 0; d < 4; d++)
    {
        const MapSlabCoord nx = fx + d4[d][0], ny = fy + d4[d][1];
        const int o = wf_open(nx, ny, fx, fy);
        if (o == 0)
            continue;
        const int cell = (ny - fy + WF_BOX) * WF_BOX_SIDE + (nx - fx + WF_BOX);
        if (wf_label[cell] != 0)
            continue; /* (the same part as an earlier side) */
        const int r = ++nlabel;
        escapes[r] = (o == 2);
        qbeg[r] = qn;
        wf_label[cell] = (uint16_t)r;
        wf_queue[qn++] = cell;
        for (int qi = qbeg[r]; qi < qn; qi++)
        {
            const int c = wf_queue[qi];
            const MapSlabCoord cx = fx - WF_BOX + c % WF_BOX_SIDE, cy = fy - WF_BOX + c / WF_BOX_SIDE;
            if ((cx == hx) && (cy == hy))
                escapes[r] = true; /* (the keeper's Heart is on this side) */
            for (int e = 0; e < 4; e++)
            {
                const MapSlabCoord ax = cx + d4[e][0], ay = cy + d4[e][1];
                const int oo = wf_open(ax, ay, fx, fy);
                if (oo == 0)
                    continue;
                if ((abs(ax - fx) > WF_BOX) || (abs(ay - fy) > WF_BOX) || (oo == 2))
                {
                    escapes[r] = true; /* (it goes on out of the box: not a pocket) */
                    continue;
                }
                const int ac = (ay - fy + WF_BOX) * WF_BOX_SIDE + (ax - fx + WF_BOX);
                if (wf_label[ac] != 0)
                    continue;
                wf_label[ac] = (uint16_t)r;
                wf_queue[qn++] = ac;
            }
        }
        qend[r] = qn;
    }
    if (nlabel <= 1)
        return true; /* the slab cuts nothing in two */
    TbBool any_out = false;
    for (int r = 1; r <= nlabel; r++)
        any_out = any_out || escapes[r];
    if (!any_out)
    {
        *why = "every side would be a pocket";
        return false;
    }
    wf_shoo_side = false;
    if ((mode == WfC_Shoo) && (worker == NULL))
    {
        /* (a waiting mark's own call: the open side is where the creatures of a pocket are sent) */
        for (int d = 0; (d < 4) && !wf_shoo_side; d++)
        {
            const MapSlabCoord nx = fx + d4[d][0], ny = fy + d4[d][1];
            const int lab = wf_label[(ny - fy + WF_BOX) * WF_BOX_SIDE + (nx - fx + WF_BOX)];
            if ((lab > 0) && escapes[lab])
            {
                wf_shoo_side = true;
                wf_shoo_sx = nx;
                wf_shoo_sy = ny;
            }
        }
        if (!wf_shoo_side)
            mode = WfC_Strict;
    }
    for (int r = 1; r <= nlabel; r++)
    {
        if (escapes[r])
            continue;
        if (have_stand && (abs(stand_x - fx) <= WF_BOX) && (abs(stand_y - fy) <= WF_BOX) &&
            (wf_label[(stand_y - fy + WF_BOX) * WF_BOX_SIDE + (stand_x - fx + WF_BOX)] == r))
        {
            *why = "the Imp would be shut in";
            return false;
        }
        for (int qi = qbeg[r]; qi < qend[r]; qi++)
        {
            const int c = wf_queue[qi];
            if (wf_marked[c])
            {
                /* a slab still to be filled would be out of the Imps' reach: the inner ones go first */
                *why = "a marked slab would be shut in";
                return false;
            }
        }
        TbBool in_way = false;
        if (mode == WfC_Shoo)
        {
            /* remembered as kept out for a while (the set of this owner's that is oldest is used again) */
            int s = 0;
            for (int t = 1; t < WF_KEEP_SETS; t++)
                if (wf_keep[t].until < wf_keep[s].until)
                    s = t;
            wf_keep[s].owner = owner;
            wf_keep[s].until = (GameTurnDelta)game.play_gameturn + 32;
            wf_keep[s].n = 0;
            for (int qi = qbeg[r]; (qi < qend[r]) && (wf_keep[s].n < WF_KEEP_SLABS); qi++)
            {
                wf_keep[s].x[wf_keep[s].n] = fx - WF_BOX + wf_queue[qi] % WF_BOX_SIDE;
                wf_keep[s].y[wf_keep[s].n] = fy - WF_BOX + wf_queue[qi] / WF_BOX_SIDE;
                wf_keep[s].n++;
            }
        }
        for (int qi = qbeg[r]; qi < qend[r]; qi++)
        {
            const int c = wf_queue[qi];
            if (wf_creatures_in_way(fx - WF_BOX + c % WF_BOX_SIDE, fy - WF_BOX + c / WF_BOX_SIDE, owner, mode, worker, fx, fy))
                in_way = true; /* (on: every creature of the pocket is told to come out) */
        }
        if (in_way)
        {
            *why = "a creature would be shut in";
            return false;
        }
    }
    return true;
}

/******************************************************************************/
/* the Imps */

/* Up to three Imps fill one mark, as up to three dig at one slab (the user, 2026-11-20), each from its own side;
   their strokes add up. */
#define WF_WORKERS 3

/** Slot k's Imp is still on its way to this mark, or filling it; else the slot is free again. */
static TbBool wf_slot_busy(struct WorldFillMark *m, int k, MapSlabCoord x, MapSlabCoord y)
{
    if (m->worker[k] == 0)
        return false;
    struct Thing *thing = thing_get(m->worker[k]);
    if (thing_exists(thing) && thing_is_creature(thing) && (thing->owner == m->owner) && !creature_is_dying(thing) &&
        !thing_is_picked_up(thing))
    {
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl) && (cctrl->digger_working_stl == wf_center_stl(x, y)))
        {
            if ((thing->active_state == CrSt_ImpFills) || (thing->active_state == CrSt_ImpArrivesAtFill))
                return true;
            if (thing->continue_state == CrSt_ImpArrivesAtFill)
            {
                if ((uint16_t)((uint16_t)game.play_gameturn - m->walk_from[k]) <= WF_WALK_MAX)
                    return true;
                /* still not there: it gives the mark up and looks for other work (the 2026-11-20 test: an Imp sent on to
                   the next mark stood "walking" on another mark for a thousand turns, and that one's Imp waited for it) */
                SYNCLOG("World: Fill: Imp %d did not reach the mark at engine slab (%d,%d) in %d turns (at subtile %d,%d, "
                    "going to %d,%d): it gives it up", (int)thing->index, (int)x, (int)y, WF_WALK_MAX, (int)thing->mappos.x.stl.num,
                    (int)thing->mappos.y.stl.num, (int)cctrl->moveto_pos.x.stl.num, (int)cctrl->moveto_pos.y.stl.num);
                set_start_state(thing);
            }
        }
    }
    m->worker[k] = 0;
    return false;
}

static int wf_busy_count(struct WorldFillMark *m, MapSlabCoord x, MapSlabCoord y)
{
    int n = 0;
    for (int k = 0; k < WF_WORKERS; k++)
        if (wf_slot_busy(m, k, x, y))
            n++;
    return n;
}

static int wf_slot_of(const struct WorldFillMark *m, ThingIndex idx)
{
    for (int k = 0; k < WF_WORKERS; k++)
        if (m->worker[k] == idx)
            return k;
    return -1;
}

/** The Imp is one of the mark's, or there is room for it (fewer than three). */
static TbBool wf_room_for(struct WorldFillMark *m, const struct Thing *thing, MapSlabCoord x, MapSlabCoord y)
{
    const int n = wf_busy_count(m, x, y);
    return (wf_slot_of(m, thing->index) >= 0) || (n < WF_WORKERS);
}

static TbBool wf_add_worker(struct WorldFillMark *m, const struct Thing *thing)
{
    if (wf_slot_of(m, thing->index) >= 0)
        return true;
    for (int k = 0; k < WF_WORKERS; k++)
        if (m->worker[k] == 0)
        {
            m->worker[k] = thing->index;
            m->walk_from[k] = (uint16_t)game.play_gameturn;
            return true;
        }
    return false;
}

static void wf_drop_worker(struct WorldFillMark *m, const struct Thing *thing)
{
    const int k = wf_slot_of(m, thing->index);
    if (k >= 0)
        m->worker[k] = 0;
}

static int wf_mark_of_imp(const struct Thing *thing, MapSlabCoord *x, MapSlabCoord *y)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl))
        return -1;
    *x = subtile_slab(stl_num_decode_x(cctrl->digger_working_stl));
    *y = subtile_slab(stl_num_decode_y(cctrl->digger_working_stl));
    const int i = wf_find(*x, *y);
    if ((i < 0) || (wfm[i].owner != thing->owner))
        return -1;
    return i;
}

int world_fill_add_to_imp_stack(struct Dungeon *dungeon, int max_tasks)
{
    int n = 0;
    const PlayerNumber p = dungeon->owner;
    for (int i = 0; (i < WORLD_FILL_MAX) && (n < max_tasks); i++)
    {
        struct WorldFillMark *m = &wfm[i];
        MapSlabCoord x, y;
        if (!WF_IMP_MARK(m) || (m->owner != p) || !wf_engine(m, &x, &y))
            continue;
        if (wf_busy_count(m, x, y) >= WF_WORKERS)
            continue;
        if (m->blocked && ((long)(game.play_gameturn - m->check_turn) < WF_RECHECK))
            continue;
        if (!add_to_dungeon_imp_stack_using_pos(wf_center_stl(x, y), DigTsk_FillSlab, dungeon))
            break;
        n++;
    }
    return n;
}

/** A floor slab beside the mark an Imp can stand on while it fills (pass 0: not marked itself; pass 1: any). */
static TbBool wf_standable(MapSlabCoord x, MapSlabCoord y, int pass)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || (slb->kind == SlbT_LAVA) || (slb->kind == SlbT_WATER))
        return false;
    const struct Map *mapblk = get_map_block_at(slab_subtile_center(x), slab_subtile_center(y));
    if (map_block_invalid(mapblk) || (mapblk->flags & SlbAtFlg_Blocking))
        return false;
    return (pass > 0) || (wf_find(x, y) < 0);
}

static long wf_try_assign(struct Thing *thing, int i, MapSlabCoord x, MapSlabCoord y);

long world_fill_check_out_worker(struct Thing *thing, struct DiggerStack *dstack)
{
    const MapSlabCoord x = subtile_slab(stl_num_decode_x(dstack->stl_num));
    const MapSlabCoord y = subtile_slab(stl_num_decode_y(dstack->stl_num));
    const int i = wf_find(x, y);
    if ((i < 0) || (wfm[i].owner != thing->owner))
    {
        dstack->task_type = DigTsk_None;
        return 0;
    }
    return wf_try_assign(thing, i, x, y);
}

static long wf_try_assign(struct Thing *thing, int i, MapSlabCoord x, MapSlabCoord y)
{
    struct WorldFillMark *m = &wfm[i];
    if (!wf_room_for(m, thing, x, y))
        return 0;
    if (m->blocked && ((long)(game.play_gameturn - m->check_turn) < WF_RECHECK))
        return 0;
    /* the four sides, nearest to the Imp first */
    int order[4] = {0, 1, 2, 3};
    long dist[4];
    const MapSubtlCoord bx = slab_subtile_center(x), by = slab_subtile_center(y);
    for (int k = 0; k < 4; k++)
    {
        const MapSubtlCoord sx = bx + 2 * small_around[k].delta_x, sy = by + 2 * small_around[k].delta_y;
        dist[k] = labs((long)thing->mappos.x.stl.num - sx) + labs((long)thing->mappos.y.stl.num - sy);
    }
    for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++)
            if (dist[order[b]] < dist[order[a]])
            {
                const int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
    const char *why = NULL;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int k = 0; k < 4; k++)
        {
            const int s = order[k];
            const MapSlabCoord nx = x + small_around[s].delta_x, ny = y + small_around[s].delta_y;
            if (!wf_standable(nx, ny, pass))
                continue;
            const MapSubtlCoord sx = bx + 2 * small_around[s].delta_x, sy = by + 2 * small_around[s].delta_y;
            if (imp_will_soon_be_working_at_excluding(thing, sx, sy))
                continue;
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(sx);
            pos.y.val = subtile_coord_center(sy);
            pos.z.val = 0;
            pos.z.val = get_thing_height_at(thing, &pos);
            if (!creature_can_navigate_to_with_storage(thing, &pos, NavRtF_Default))
                continue;
            if (!wf_fill_safe(m->owner, x, y, true, nx, ny, &why, WfC_Plan, NULL))
                continue;
            if (!setup_person_move_to_position(thing, sx, sy, NavRtF_Default))
                continue;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            thing->continue_state = CrSt_ImpArrivesAtFill;
            cctrl->digger_working_stl = wf_center_stl(x, y);
            cctrl->digger.last_did_job = SDLstJob_NonDiggerTask;
            if (wf_busy_count(m, x, y) == 0)
            {
                m->wait = 0;
                m->hits = 0;
            }
            wf_add_worker(m, thing);
            m->blocked = 0;
            return 1;
        }
    }
    if ((why != NULL) && (wf_busy_count(m, x, y) == 0))
    {
        /* every side it could be filled from would shut somebody in: it waits (not while Imps are at it: one Imp's look
           set it waiting under the Imps on their way, and they turned back - the 2026-11-20 test) */
        m->blocked = 1;
        m->check_turn = (int32_t)game.play_gameturn;
        SYNCDBG(7, "Fill at engine slab (%d,%d) waits: %s", (int)x, (int)y, why);
    }
    return 0;
}

/** An Imp done with a fill takes the nearest free mark within 12 slabs at once, as digging goes on to the next slab
 *  (the user, 2026-11-20: they filled one, wandered, filled the next). */
#define WF_NEXT_REACH 12
static TbBool wf_take_next(struct Thing *thing)
{
    int cand[8];
    long cdist[8];
    int n = 0;
    const MapSlabCoord ix = subtile_slab(thing->mappos.x.stl.num), iy = subtile_slab(thing->mappos.y.stl.num);
    for (int i = 0; i < WORLD_FILL_MAX; i++)
    {
        struct WorldFillMark *m = &wfm[i];
        MapSlabCoord x, y;
        if (!WF_IMP_MARK(m) || (m->owner != thing->owner) || !wf_engine(m, &x, &y))
            continue;
        const long d = max(abs(x - ix), abs(y - iy));
        if ((d > WF_NEXT_REACH) || (wf_busy_count(m, x, y) >= WF_WORKERS) || (m->blocked && ((long)(game.play_gameturn - m->check_turn) < WF_RECHECK)))
            continue;
        /* keep the eight nearest */
        int at = n;
        if (n < 8)
            n++;
        else if (d >= cdist[7])
            continue;
        else
            at = 7;
        while ((at > 0) && (cdist[at - 1] > d))
        {
            cand[at] = cand[at - 1];
            cdist[at] = cdist[at - 1];
            at--;
        }
        cand[at] = i;
        cdist[at] = d;
    }
    for (int k = 0; k < n; k++)
    {
        MapSlabCoord x, y;
        if (wf_engine(&wfm[cand[k]], &x, &y) && (wf_try_assign(thing, cand[k], x, y) > 0))
            return true;
    }
    return false;
}

short imp_arrives_at_fill(struct Thing *thing)
{
    MapSlabCoord x, y;
    const int i = wf_mark_of_imp(thing, &x, &y);
    if ((i < 0) || (wfm[i].blocked && (wf_slot_of(&wfm[i], thing->index) < 0)))
    {
        internal_set_thing_state(thing, CrSt_ImpLastDidJob);
        return 1;
    }
    if (!wf_add_worker(&wfm[i], thing))
    {
        internal_set_thing_state(thing, CrSt_ImpLastDidJob);
        return 1;
    }
    internal_set_thing_state(thing, CrSt_ImpFills);
    return 1;
}

short imp_fills(struct Thing *thing)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    MapSlabCoord x, y;
    const int i = wf_mark_of_imp(thing, &x, &y);
    const MapSubtlDelta dist_x = abs(thing->mappos.x.stl.num - (MapSubtlDelta)cctrl->moveto_pos.x.stl.num);
    const MapSubtlDelta dist_y = abs(thing->mappos.y.stl.num - (MapSubtlDelta)cctrl->moveto_pos.y.stl.num);
    if ((i < 0) || (wfm[i].blocked && (wf_slot_of(&wfm[i], thing->index) < 0)) || (dist_x + dist_y >= 1))
    {
        if (i >= 0)
            wf_drop_worker(&wfm[i], thing);
        clear_creature_instance(thing);
        if (wf_take_next(thing)) /* (the next mark near, as digging goes on) */
            return 1;
        internal_set_thing_state(thing, CrSt_ImpLastDidJob);
        return 0;
    }
    struct WorldFillMark *m = &wfm[i];
    if (!wf_add_worker(m, thing))
    {
        /* (three Imps are at it already) */
        clear_creature_instance(thing);
        if (wf_take_next(thing))
            return 1;
        internal_set_thing_state(thing, CrSt_ImpLastDidJob);
        return 0;
    }
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x));
    pos.y.val = subtile_coord_center(slab_subtile_center(y));
    pos.z.val = subtile_coord(1, 0);
    if (creature_turn_to_face(thing, &pos) > 0)
        return 1;
    if ((m->wait > 0) || wf_slab_has_things(x, y, WfT_All))
    {
        /* something is in the way: the keeper's own creatures on the slab, or in a pocket it would close, are sent off
           (to this Imp's slab) at once; it waits for them, and for the rest (and gives it up after a while) */
        if ((m->wait % 8) == 0)
        {
            const char *why = "";
            const TbBool on_it = wf_creatures_in_way(x, y, m->owner, WfC_Shoo, thing, x, y);
            const TbBool safe = wf_fill_safe(m->owner, x, y, true, subtile_slab(thing->mappos.x.stl.num),
                subtile_slab(thing->mappos.y.stl.num), &why, WfC_Shoo, thing);
            if (!on_it && safe && !wf_slab_has_things(x, y, WfT_All))
                m->wait = 0;
            else if (m->wait == 0)
                m->wait = 1;
        }
        if ((m->wait > 0) && (++m->wait > WF_WAIT_MAX))
        {
            m->wait = 0;
            memset(m->worker, 0, sizeof(m->worker));
            m->blocked = 1;
            m->check_turn = (int32_t)game.play_gameturn;
            clear_creature_instance(thing);
            internal_set_thing_state(thing, CrSt_ImpLastDidJob);
            return 0;
        }
        if (m->wait > 0)
            return 1;
    }
    if (game.conf.rules[thing->owner].workers.digger_work_experience != 0)
    {
        cctrl->exp_points += digger_work_experience(thing);
        check_experience_upgrade(thing);
    }
    if (cctrl->instance_id == CrInst_NULL)
        set_creature_instance(thing, CrInst_DIG, 0, 0);
    return 1;
}

static void wf_test_after_fill(PlayerNumber p, MapSlabCoord x, MapSlabCoord y);

/** The dig's falling rubble in reverse (the user, 2026-11-19: "Digging dirt creates a falling rubble effect. Try
 *  adding that but in reverse when the dirt is built"): the dig rubble's own pieces (EFFECT_DIRT_RUBBLE's elements,
 *  27-28) thrown up from the floor all over the slab, drifting in, as the wall rises. A look only (unsynced numbers,
 *  as the effects' own). */
static void wf_rising_rubble(MapSlabCoord x, MapSlabCoord y, PlayerNumber owner, int per_subtile)
{
    const MapCoord cx = subtile_coord_center(slab_subtile_center(x)), cy = subtile_coord_center(slab_subtile_center(y));
    for (int k = 0; k < STL_PER_SLB * STL_PER_SLB; k++)
    {
        for (int n = 0; n < per_subtile; n++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord(slab_subtile(x, k % 3), 0) + 32 + UNSYNC_RANDOM(COORD_PER_STL - 64);
            pos.y.val = subtile_coord(slab_subtile(y, k / 3), 0) + 32 + UNSYNC_RANDOM(COORD_PER_STL - 64);
            pos.z.val = subtile_coord(1, 0) + 8;
            struct Thing *elem = create_effect_element(&pos, 27 + UNSYNC_RANDOM(2), owner);
            if (thing_is_invalid(elem))
                return;
            elem->veloc_push_add.x.val += (cx - pos.x.val) / 12;
            elem->veloc_push_add.y.val += (cy - pos.y.val) / 12;
            elem->veloc_push_add.z.val += 90 + UNSYNC_RANDOM(70);
            elem->state_flags |= TF1_PushAdd;
            elem->move_angle_xy = LbArcTanAngle(elem->veloc_push_add.x.val, elem->veloc_push_add.y.val) & ANGLE_MASK;
        }
    }
}

TbBool world_fill_dig_hit(struct Thing *creatng)
{
    if (creatng->active_state != CrSt_ImpFills)
        return false;
    MapSlabCoord x, y;
    const int i = wf_mark_of_imp(creatng, &x, &y);
    if (i < 0)
        return true;
    struct WorldFillMark *m = &wfm[i];
    struct ShotConfigStats *shotst = get_shot_model_stats(ShM_Dig);
    thing_play_sample(creatng, shotst->dig.sndsample_idx + SOUND_RANDOM(shotst->dig.sndsample_range), NORMAL_PITCH, 0, 3, 0, 2,
        FULL_LOUDNESS);
    create_effect(&creatng->mappos, shotst->dig.effect_model, creatng->owner);
    if (++m->hits < WF_STROKES)
    {
        if (m->hits == WF_STROKES - 1)
            wf_rising_rubble(x, y, creatng->owner, 1); /* (the dirt starts to rise a stroke before the wall) */
        return true;
    }
    m->hits = 0;
    const char *why = "";
    if (wf_slab_has_things(x, y, WfT_All) ||
        !wf_fill_safe(m->owner, x, y, true, subtile_slab(creatng->mappos.x.stl.num), subtile_slab(creatng->mappos.y.stl.num), &why,
            WfC_Strict, NULL))
    {
        m->wait = 1; /* (imp_fills sends the keeper's creatures away and waits) */
        SYNCDBG(7, "Fill at engine slab (%d,%d) waits at its last stroke: %s", (int)x, (int)y, why);
        return true;
    }
    const PlayerNumber p = m->owner;
    memset(m, 0, sizeof(*m));
    wf_rising_rubble(x, y, creatng->owner, 2);
    place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
    do_slab_efficiency_alteration(x, y);
    /* (no dug slab's rubble: it was left floating by the new wall - the user, 2026-11-19; it rises instead, above) */
    thing_play_sample(creatng, snd_dig_dirt, NORMAL_PITCH, 0, 3, 0, 2, FULL_LOUDNESS);
    if (game.world_fill_done[p] < UINT16_MAX)
        game.world_fill_done[p]++;
    /* the marks about it that waited are looked at again now: a fill may have been all they waited for (the user,
       2026-11-20: in a dead-end tunnel marked from its end, the Imp filled two and walked off - the next slab had been
       set aside as "would shut a marked slab in" before the end was filled, for 64 turns) */
    for (int k = 0; k < WORLD_FILL_MAX; k++)
    {
        MapSlabCoord kx, ky;
        if (wfm[k].used && wfm[k].blocked && wf_engine(&wfm[k], &kx, &ky) && (abs(kx - x) <= WF_BOX) && (abs(ky - y) <= WF_BOX))
            wfm[k].blocked = 0;
    }
    wf_test_after_fill(p, x, y);
    return true;
}

void world_fill_states_fallback(void)
{
    struct CreatureStateConfig *st = game.conf.crtr_conf.states;
    static const struct { int idx; const char *name; FuncIdx func; unsigned char type; } add[2] = {
        {CrSt_ImpArrivesAtFill, "ImpArrivesAtFill", 137, CrStTyp_Work},
        {CrSt_ImpFills, "ImpFills", 138, CrStTyp_DeepWork},
    };
    for (int k = 0; k < 2; k++)
    {
        if ((add[k].idx < game.conf.crtr_conf.states_count) && (st[add[k].idx].process_state != 0))
            continue;
        memset(&st[add[k].idx], 0, sizeof(st[0]));
        snprintf(st[add[k].idx].name, sizeof(st[0].name), "%s", add[k].name);
        st[add[k].idx].process_state = add[k].func;
        st[add[k].idx].state_type = add[k].type;
        st[add[k].idx].react_to_cta = 1;
        if (game.conf.crtr_conf.states_count <= add[k].idx)
            game.conf.crtr_conf.states_count = add[k].idx + 1;
    }
}

/******************************************************************************/
/* the icons */

void world_fill_register_icons(void)
{
    const TbBool lowres = (RendererPhysicalWidth() <= LOWRES_SCREEN_SIZE);
    const unsigned char *big[2] = {lowres ? wfi_big_lo : wfi_big, lowres ? wfi_big_lo_dis : wfi_big_dis};
    const unsigned int big_len[2] = {lowres ? wfi_big_lo_len : wfi_big_len, lowres ? wfi_big_lo_dis_len : wfi_big_dis_len};
    const unsigned char *btn[2] = {lowres ? wfi_button_lo : wfi_button, lowres ? wfi_button_lo_dis : wfi_button_dis};
    const unsigned int btn_len[2] = {lowres ? wfi_button_lo_len : wfi_button_len, lowres ? wfi_button_lo_dis_len : wfi_button_dis_len};
    const unsigned char *cur[1] = {wfi_cursor};
    const unsigned int cur_len[1] = {wfi_cursor_len};
    custom_sprites_add_builtin_icon("ENDLESS_FILL_BIG", big, big_len, 2);
    custom_sprites_add_builtin_icon("ENDLESS_FILL_BUTTON", btn, btn_len, 2);
    custom_sprites_add_builtin_icon("ENDLESS_FILL_CURSOR", cur, cur_len, 1);
}

/******************************************************************************/
/* upkeep */

static void wf_test_turn(void);

/** Sends the creatures off a slab that is filling (the Cave-In spell's way out); true when none is left on it that
 *  cannot go (held, possessed, in custody). Bodies, objects and traps on it are counted in *others. */
static TbBool wf_cave_clear(MapSlabCoord x, MapSlabCoord y, int *others)
{
    TbBool clear = true;
    *others = 0;
    for (int dy = 0; dy < STL_PER_SLB; dy++)
        for (int dx = 0; dx < STL_PER_SLB; dx++)
        {
            const struct Map *mapblk = get_map_block_at(slab_subtile(x, dx), slab_subtile(y, dy));
            if (map_block_invalid(mapblk))
                continue;
            ThingIndex i = get_mapwho_thing_index(mapblk);
            int k = 0;
            while ((i != 0) && (k++ < 256))
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if (thing_is_picked_up(thing))
                    continue;
                if (thing->class_id == TCls_Creature)
                {
                    clear = false;
                    if (((thing->alloc_flags & TAlF_IsControlled) == 0) && !creature_is_kept_in_custody(thing) &&
                        !creature_is_being_unconscious(thing) && (get_creature_state_besides_interruptions(thing) != CrSt_CreatureEscapingDeath))
                    {
                        if (cleanup_current_thing_state(thing) && setup_move_out_of_cave_in(thing))
                            thing->continue_state = CrSt_CreatureEscapingDeath;
                    }
                } else if ((thing->class_id == TCls_Object) && !(get_object_model_stats(thing->model)->model_flags & OMF_DestroyedOnRoomPlace))
                    (*others)++;
                else if ((thing->class_id == TCls_DeadCreature) || (thing->class_id == TCls_Trap) || (thing->class_id == TCls_Door))
                    (*others)++;
            }
        }
    return clear && (*others == 0);
}

/** The cave-in fills (every turn): the Cave-In spell's rocks and dust over the slab, its quake and sound at the start,
 *  the dirt rising a cube every WF_CAVE_STEP turns (SLAB50), then earth. */
static void wf_cave_turn(void)
{
    const struct PowerConfigStats *powerst = get_power_model_stats(PwrK_CAVEIN);
    for (int i = 0; i < WORLD_FILL_MAX; i++)
    {
        struct WorldFillMark *m = &wfm[i];
        if (!m->used || !m->cave)
            continue;
        if ((m->owner >= PLAYERS_MAX) || !player_exists(get_player(m->owner)))
        {
            memset(m, 0, sizeof(*m));
            continue;
        }
        MapSlabCoord x, y;
        if (!wf_engine(m, &x, &y))
            continue; /* (ground not loaded: it waits) */
        const long t = (long)(game.play_gameturn - (GameTurn)m->check_turn);
        struct Coord3d mid;
        mid.x.val = subtile_coord_center(slab_subtile_center(x));
        mid.y.val = subtile_coord_center(slab_subtile_center(y));
        mid.z.val = 0;
        if (t == 0)
        {
            /* the start: the quake for its keeper, the dust and the spell's sound */
            struct Dungeon *dungeon = get_players_num_dungeon(m->owner);
            if (!dungeon_invalid(dungeon) && (dungeon->camera_deviate_quake < 24))
                dungeon->camera_deviate_quake = 24;
            mid.z.val = get_floor_height_at(&mid) + 384;
            struct Thing *dust = create_effect(&mid, TngEff_HarmlessGas4, m->owner);
            if (!thing_is_invalid(dust) && (powerst != NULL))
                thing_play_sample(dust, powerst->select_sound_idx, 25, 0, 3, 0, 2, FULL_LOUDNESS);
        }
        if ((t < WF_CAVE_STEP * (WF_CAVE_FRAMES + 1)) && ((t % 6) == 0))
        {
            /* rocks falling from the ceiling over the slab */
            struct Coord3d pos = mid;
            pos.x.val += GAME_RANDOM(640) - 320;
            pos.y.val += GAME_RANDOM(640) - 320;
            pos.z.val = get_ceiling_height(&pos) - 128;
            struct Thing *rock = create_effect_element(&pos, TngEff_Flash, m->owner);
            if (!thing_is_invalid(rock) && (powerst != NULL))
                rock->health = powerst->duration;
        }
        if ((t > 0) && ((t % 12) == 0) && (t < WF_CAVE_STEP * (WF_CAVE_FRAMES + 1)))
        {
            struct Coord3d pos = mid;
            pos.z.val = get_floor_height_at(&pos) + 256;
            create_effect(&pos, TngEff_HarmlessGas4, m->owner);
        }
        int others = 0;
        if ((m->hits < WF_CAVE_FRAMES) && (t >= (long)WF_CAVE_STEP * (m->hits + 1)))
        {
            /* the dirt a cube higher; whoever came onto the slab is sent off it */
            wf_cave_clear(x, y, &others);
            place_animating_slab_type_on_map(SlbT_SLAB50, (char)m->hits, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num);
            m->hits++;
            continue;
        }
        if ((m->hits >= WF_CAVE_FRAMES) && (t >= (long)WF_CAVE_STEP * (WF_CAVE_FRAMES + 1)))
        {
            if (!wf_cave_clear(x, y, &others))
            {
                if (++m->wait < WF_CAVE_WAIT)
                    continue;
                /* something never left: the slab is floor again (no gold back) */
                SYNCLOG("World: player %d's cave-in fill at engine slab (%d,%d) gave up: something stayed on it", (int)m->owner, (int)x, (int)y);
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(y), m->owner, 0);
                memset(m, 0, sizeof(*m));
                continue;
            }
            place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
            if (m->owner < PLAYERS_MAX)
                game.world_fill_done[m->owner]++;
            SYNCDBG(8, "Player %d's cave-in fill at engine slab (%d,%d) is earth", (int)m->owner, (int)x, (int)y);
            memset(m, 0, sizeof(*m));
        }
    }
}

/* Walking Imps that do not move (the 2026-11-20 test: an Imp that had just filled the slab beside it and was sent on
   stood still for 300 turns; its way was most likely worked out through the slab it had filled): every 16 turns, one
   that is where it was is given its way again after 48 turns, and gives the mark up after 96. */
static int32_t wf_walk_x[WORLD_FILL_MAX * WF_WORKERS], wf_walk_y[WORLD_FILL_MAX * WF_WORKERS];
static uint8_t wf_walk_still[WORLD_FILL_MAX * WF_WORKERS];

static void wf_walkers_turn(void)
{
    for (int j = 0; j < WORLD_FILL_MAX * WF_WORKERS; j++)
    {
        const int i = j / WF_WORKERS, k = j % WF_WORKERS;
        struct WorldFillMark *m = &wfm[i];
        struct Thing *imp = (m->used && (m->worker[k] != 0)) ? thing_get(m->worker[k]) : NULL;
        if (!thing_exists(imp) || !thing_is_creature(imp) || (imp->continue_state != CrSt_ImpArrivesAtFill) ||
            (imp->active_state != CrSt_MoveToPosition))
        {
            wf_walk_still[j] = 0;
            continue;
        }
        if ((wf_walk_x[j] != imp->mappos.x.stl.num) || (wf_walk_y[j] != imp->mappos.y.stl.num))
        {
            wf_walk_x[j] = imp->mappos.x.stl.num;
            wf_walk_y[j] = imp->mappos.y.stl.num;
            wf_walk_still[j] = 0;
            continue;
        }
        wf_walk_still[j]++;
        struct CreatureControl *cctrl = creature_control_get_from_thing(imp);
        if (wf_walk_still[j] == 3)
        {
            const MapSubtlCoord tx = cctrl->moveto_pos.x.stl.num, ty = cctrl->moveto_pos.y.stl.num;
            if (setup_person_move_to_position(imp, tx, ty, NavRtF_Default))
                imp->continue_state = CrSt_ImpArrivesAtFill;
        }
        else if (wf_walk_still[j] >= 6)
        {
            wf_walk_still[j] = 0;
            m->worker[k] = 0;
            set_start_state(imp);
        }
    }
}

void world_fill_turn(void)
{
    if (netstate_is_client() || !world_mode_active())
        return;
    wf_test_turn();
    const int rk = world_fill_room_kind();
    if ((rk > 0) && ((game.play_gameturn % 64) == 21))
    {
        /* every keeper has it from the start (worlds saved before it came get it too) */
        for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
        {
            struct PlayerInfo *player = get_player(p);
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            if (player_exists(player) && !dungeon_invalid(dungeon) && ((dungeon->room_buildable[rk] & 1) == 0))
                set_room_available(p, rk, 1, 1);
        }
    }
    wf_cave_turn();
    if ((game.play_gameturn % 16) != 7)
        return;
    wf_walkers_turn();
    for (int i = 0; i < WORLD_FILL_MAX; i++)
    {
        struct WorldFillMark *m = &wfm[i];
        if (!WF_IMP_MARK(m))
            continue;
        if ((m->owner >= PLAYERS_MAX) || !player_exists(get_player(m->owner)))
        {
            memset(m, 0, sizeof(*m));
            continue;
        }
        MapSlabCoord x, y;
        if (!wf_engine(m, &x, &y))
            continue; /* (ground not loaded: kept as it is) */
        const struct SlabMap *slb = get_slabmap_block(x, y);
        if (slabmap_block_invalid(slb))
            continue;
        /* it is no floor for a mark now (dug, built on, flooded, taken by another keeper, a trap or door put on it) */
        if (!wf_floor_for(m->owner, x, y) || slab_has_trap_on(x, y) || slab_has_door_thing_on(x, y))
        {
            memset(m, 0, sizeof(*m));
            continue;
        }
        wf_send_off_mark(x, y, m->owner); /* the keeper's creatures on it go 4 slabs off */
        if (m->blocked)
        {
            /* a waiting mark keeps calling the keeper's creatures out of the pocket it would close (and keeps the Imps
               from going back in to reinforce), Imp at it or not (the 2026-11-20 test: once the filling Imp gave up,
               they went back in, and the mark waited for good) */
            const char *why = "";
            wf_fill_safe(m->owner, x, y, false, 0, 0, &why, WfC_Shoo, NULL);
        }
    }
}

void world_fill_seat_reset(PlayerNumber plyr_idx)
{
    for (int i = 0; i < WORLD_FILL_MAX; i++)
        if (wfm[i].used && (wfm[i].owner == plyr_idx))
            memset(&wfm[i], 0, sizeof(wfm[i]));
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        game.world_fill_done[plyr_idx] = 0;
}

/* FIL1 (an extra of the world save): the marks, 12 bytes each (world slab x, y, owner) */
size_t world_fill_save_size(void)
{
    uint32_t n = 0;
    for (int i = 0; i < WORLD_FILL_MAX; i++)
        if (wfm[i].used)
            n++;
    return (n == 0) ? 0 : (4 + n * 12);
}

void world_fill_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *at = buf + 4;
    for (int i = 0; i < WORLD_FILL_MAX; i++)
    {
        if (!wfm[i].used)
            continue;
        const int32_t v[3] = {wfm[i].x, wfm[i].y, (int32_t)wfm[i].owner | (wfm[i].cave ? 0x100 : 0)}; /* (a cave-in fill: + 0x100) */
        memcpy(at, v, sizeof(v));
        at += sizeof(v);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_fill_load(const uint8_t *buf, size_t len)
{
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > WORLD_FILL_MAX) || ((size_t)n * 12 + 4 > len))
        return false;
    world_fill_reset();
    for (uint32_t k = 0; k < n; k++)
    {
        int32_t v[3];
        memcpy(v, buf + 4 + k * 12, sizeof(v));
        const int32_t owner = v[2] & 0xFF;
        if ((v[2] < 0) || (owner >= PLAYERS_MAX))
            continue;
        wfm[k].x = v[0];
        wfm[k].y = v[1];
        wfm[k].owner = (uint8_t)owner;
        if (v[2] & 0x100)
        {
            wfm[k].cave = 1; /* a cave-in fill under way: it begins again (its dirt rises from the frame it had) */
            wfm[k].check_turn = (int32_t)game.play_gameturn;
        }
        wfm[k].used = 1;
    }
    return true;
}

void world_fill_reset(void)
{
    memset(wf_keep, 0, sizeof(wf_keep));
    memset(game.world_fill, 0, sizeof(game.world_fill));
    memset(game.world_fill_done, 0, sizeof(game.world_fill_done));
}

/******************************************************************************/
/* KFX_WORLD_FILLTEST=turn[,1] (see the file's head) */

static long wft_at = -2;
static int wft_choose = 0;
static PlayerNumber wft_p = -1;
static ThingIndex wft_crtr = 0;
static MapSlabCoord wft_px, wft_py; /* the pocket's middle */
static MapSlabCoord wft_ox, wft_oy; /* its opening (marked) */
static int wft_fills = 0, wft_errors = 0;
static ThingIndex wft_sitter = 0;   /* a creature standing on the block's middle when it is marked */
static ThingIndex wft_reinf = 0;    /* an Imp reinforcing on a slab that is then marked */
static MapSlabCoord wft_rx, wft_ry;
static MapSlabCoord wft_bx, wft_by; /* the block's middle */

static void wf_test_after_fill(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    if (wft_at < 0)
        return;
    wft_fills++;
    /* nobody may be in a pocket the fill made: seen as if it were still to be filled, it must be "safe" */
    const char *why = "";
    const TbBool ok = wf_fill_safe(p, x, y, false, 0, 0, &why, WfC_Strict, NULL) || (strcmp(why, "every side would be a pocket") == 0) ||
        (strcmp(why, "a marked slab would be shut in") == 0);
    if (!ok)
        wft_errors++;
    SYNCLOG("World: FILLTEST%s: turn %ld, fill %d at engine slab (%d,%d)%s%s", ok ? "" : " ERROR", (long)game.play_gameturn,
        wft_fills, (int)x, (int)y, ok ? "" : ": ", ok ? "" : why);
    if ((x == wft_ox) && (y == wft_oy))
    {
        const struct Thing *crtr = (wft_crtr != 0) ? thing_get(wft_crtr) : NULL;
        const TbBool in = thing_exists(crtr) && (subtile_slab(crtr->mappos.x.stl.num) == wft_px) && (subtile_slab(crtr->mappos.y.stl.num) == wft_py);
        SYNCLOG("World: FILLTEST: the pocket's opening was filled at turn %ld (the creature was %s)", (long)game.play_gameturn,
            in ? "IN THE POCKET: ERROR" : "out");
    }
}

/** (the test's log) the first thing on the slab that keeps a fill waiting */
static void wft_thing_on(MapSlabCoord x, MapSlabCoord y, char *out, size_t len)
{
    snprintf(out, len, "-");
    for (int k = 0; k < STL_PER_SLB * STL_PER_SLB; k++)
    {
        const struct Map *mapblk = get_map_block_at(slab_subtile(x, k % 3), slab_subtile(y, k / 3));
        ThingIndex i = map_block_invalid(mapblk) ? 0 : get_mapwho_thing_index(mapblk);
        for (int n = 0; (i != 0) && (n < 64); n++)
        {
            const struct Thing *thing = thing_get(i);
            if (thing_is_invalid(thing))
                break;
            i = thing->next_on_mapblk;
            if (((thing->class_id == TCls_Object) && !(get_object_model_stats(thing->model)->model_flags & OMF_DestroyedOnRoomPlace)) ||
                (thing->class_id == TCls_Creature) || (thing->class_id == TCls_DeadCreature) ||
                (thing->class_id == TCls_Trap) || (thing->class_id == TCls_Door))
            {
                snprintf(out, len, "%s %s %d %s", thing_class_code_name(thing->class_id), thing_model_name(thing), (int)thing->index,
                    (thing->class_id == TCls_Creature) ? creature_state_code_name(get_creature_state_besides_interruptions(thing)) : "");
                return;
            }
        }
    }
}

static void wft_slab(PlayerNumber p, MapSlabCoord x, MapSlabCoord y, SlabKind kind)
{
    int64_t wx, wy;
    MapSlabCoord ex, ey;
    world_slab_to_world(x, y, &wx, &wy);
    if (!world_engine_slab(wx, wy, &ex, &ey) || (ex != x) || (ey != y))
        return;
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || (slb->room_index > 0))
        return;
    place_slab_type_on_map(kind, slab_subtile_center(x), slab_subtile_center(y), (kind == SlbT_EARTH) ? game.neutral_player_num : p, 0);
    set_slab_explored(p, x, y); /* (marks go only where the keeper sees) */
}

static void wft_mark(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    const TbBool ok = world_fill_place(-1, p, x, y);
    if (!ok)
    {
        const struct SlabMap *slb = get_slabmap_block(x, y);
        SYNCLOG("World: FILLTEST: engine slab (%d,%d) could not be marked (kind %d owner %d room %d revealed %d things %d)",
            (int)x, (int)y, (int)slb->kind, (int)slabmap_owner(slb), (int)slb->room_index,
            (int)subtile_revealed(slab_subtile_center(x), slab_subtile_center(y), p), (int)wf_slab_has_things(x, y, WfT_All));
    }
}

static void wf_test_turn(void)
{
    if (wft_at == -2)
    {
        const char *v = getenv("KFX_WORLD_FILLTEST");
        wft_at = -1;
        if (v != NULL)
        {
            sscanf(v, "%ld,%d", &wft_at, &wft_choose);
            wf_log_moves = true;
            SYNCLOG("World: test switch KFX_WORLD_FILLTEST=%s", v);
        }
    }
    if (wft_at < 0)
        return;
    const long t = (long)game.play_gameturn;
    if ((wft_p >= 0) && (wft_crtr != 0))
    {
        struct Thing *crtr = thing_get(wft_crtr);
        if (!thing_exists(crtr))
            wft_crtr = 0;
        else if (t >= wft_at + 1200)
        {
            /* taken out of the pocket now: the opening may be filled */
            struct Coord3d pos;
            set_coords_to_slab_center(&pos, wft_ox + 3, wft_oy);
            move_thing_in_map(crtr, &pos);
            wft_crtr = 0;
            SYNCLOG("World: FILLTEST: turn %ld, the creature is taken out of the pocket", t);
        }
        else if ((subtile_slab(crtr->mappos.x.stl.num) != wft_px) || (subtile_slab(crtr->mappos.y.stl.num) != wft_py))
        {
            struct Coord3d pos;
            set_coords_to_slab_center(&pos, wft_px, wft_py);
            move_thing_in_map(crtr, &pos); /* (kept in its middle slab: it would walk out through the opening) */
        }
    }
    if ((wft_p >= 0) && ((t - wft_at) % 200 == 0) && (t > wft_at))
    {
        const int left = wf_count(wft_p);
        int blocked = 0;
        for (int i = 0; i < WORLD_FILL_MAX; i++)
            if (wfm[i].used && (wfm[i].owner == wft_p) && wfm[i].blocked)
                blocked++;
        SYNCLOG("World: FILLTEST: turn %ld, %d fills done, %d marks left (%d waiting), %d errors, meter %d/256", t, wft_fills,
            left, blocked, wft_errors, world_fill_meter(wft_p));
        for (int i = 0; i < WORLD_FILL_MAX; i++)
        {
            struct WorldFillMark *m = &wfm[i];
            MapSlabCoord x, y;
            if (!m->used || (m->owner != wft_p) || !wf_engine(m, &x, &y))
                continue;
            char on[96];
            wft_thing_on(x, y, on, sizeof(on));
            const struct Thing *w = (m->worker[0] != 0) ? thing_get(m->worker[0]) : NULL;
            const char *why = "";
            if (!wf_fill_safe(m->owner, x, y, false, 0, 0, &why, WfC_Strict, NULL))
                (void)0;
            SYNCLOG("World: FILLTEST:   mark (%d,%d): blocked %d (now: %s), hits %d, wait %d, on it: %s; Imp %d %s", (int)x, (int)y,
                (int)m->blocked, why[0] ? why : "safe", (int)m->hits, (int)m->wait, on, (int)m->worker[0],
                thing_exists(w) ? creature_state_code_name(w->active_state) : "-");
        }
    }
    if ((wft_sitter != 0) && ((t == wft_at + 60) || (t == wft_at + 150)))
    {
        const struct Thing *s = thing_get(wft_sitter);
        if (thing_exists(s))
        {
            const int d = max(abs(subtile_slab(s->mappos.x.stl.num) - wft_bx), abs(subtile_slab(s->mappos.y.stl.num) - wft_by));
            SYNCLOG("World: FILLTEST: turn %ld, the creature that stood on the block's middle is %d slabs from it%s (state %s)", t, d,
                (t == wft_at + 150) ? ((d >= 4) ? "" : ": NOT 4 SLABS OFF, ERROR") : "", creature_state_code_name(s->active_state));
        }
    }
    if ((wft_p >= 0) && (wft_reinf == 0) && (t >= wft_at + 450) && (t < wft_at + 1200) && ((t % 10) == 0))
    {
        /* an Imp reinforcing: its slab is marked (it must stop and go at least 4 slabs off) */
        const struct Dungeon *dungeon = get_dungeon(wft_p);
        for (ThingIndex i = dungeon->digger_list_start, n = 0; (i != 0) && (n < 200); n++)
        {
            struct Thing *imp = thing_get(i);
            const struct CreatureControl *icc = creature_control_get_from_thing(imp);
            if (creature_control_invalid(icc))
                break;
            i = icc->players_next_creature_idx;
            if (imp->active_state != CrSt_ImpReinforces)
                continue;
            const MapSlabCoord rx = subtile_slab(imp->mappos.x.stl.num), ry = subtile_slab(imp->mappos.y.stl.num);
            if (world_fill_can_mark(wft_p, rx, ry) && world_fill_place(-1, wft_p, rx, ry))
            {
                wft_reinf = imp->index;
                wft_rx = rx;
                wft_ry = ry;
                SYNCLOG("World: FILLTEST: turn %ld, Imp %d reinforcing at engine slab (%d,%d): that slab is marked", t, (int)imp->index,
                    (int)rx, (int)ry);
                break;
            }
        }
    }
    if (wft_reinf != 0)
    {
        static long logged_at = -1;
        const struct Thing *imp = thing_get(wft_reinf);
        if (thing_exists(imp) && (logged_at < 0) && (imp->active_state != CrSt_ImpReinforces) &&
            (max(abs(subtile_slab(imp->mappos.x.stl.num) - wft_rx), abs(subtile_slab(imp->mappos.y.stl.num) - wft_ry)) >= 4))
        {
            logged_at = t;
            SYNCLOG("World: FILLTEST: turn %ld, the reinforcing Imp stopped and is 4+ slabs off (state %s)", t,
                creature_state_code_name(imp->active_state));
        }
    }
    if (t != wft_at)
        return;
    const PlayerNumber p = my_player_number;
    struct Thing *heart = get_player_soul_container(p);
    const int rk = world_fill_room_kind();
    if (!thing_exists(heart) || (rk <= 0))
    {
        SYNCLOG("World: FILLTEST: no Heart or no FILL room (kind %d)", rk);
        return;
    }
    wft_p = p;
    set_room_available(p, rk, 1, 1);
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const MapSlabCoord ox = hx + 5, oy = hy - 3; /* the 7x7's top left */
    for (MapSlabCoord x = hx + 2; x < ox; x++)
        wft_slab(p, x, hy, SlbT_CLAIMED); /* (a corridor from the Heart room) */
    for (MapSlabCoord y = oy; y < oy + 7; y++)
        for (MapSlabCoord x = ox; x < ox + 7; x++)
            wft_slab(p, x, y, SlbT_CLAIMED);
    /* the pocket: earth round (ox+4..ox+6, oy+4..oy+6)'s middle, open to the west */
    wft_px = ox + 5;
    wft_py = oy + 5;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (((dx != 0) || (dy != 0)) && !((dx == -1) && (dy == 0)))
                wft_slab(p, wft_px + dx, wft_py + dy, SlbT_EARTH);
    wft_ox = wft_px - 1;
    wft_oy = wft_py;
    player_add_offmap_gold(p, 3000);
    /* the user's case (2026-11-20): a 1-wide dead-end tunnel of 8 slabs south of the 7x7, marked from its end back to
       its start all at once; it must be filled one slab after another */
    {
        const MapSlabCoord tx = ox + 1;
        for (MapSlabCoord ty = oy + 7; ty < oy + 7 + 8; ty++)
        {
            wft_slab(p, tx - 1, ty, SlbT_EARTH);
            wft_slab(p, tx + 1, ty, SlbT_EARTH);
            wft_slab(p, tx, ty, SlbT_CLAIMED);
        }
        wft_slab(p, tx, oy + 7 + 8, SlbT_EARTH);
        for (MapSlabCoord ty = oy + 7 + 7; ty >= oy + 7; ty--)
            wft_mark(p, tx, ty);
        SYNCLOG("World: FILLTEST: a dead-end tunnel of 8 marks at engine slab (%d,%d)-(%d,%d)", (int)tx, (int)(oy + 7), (int)tx,
            (int)(oy + 14));
    }
    /* a creature on the block's middle: the mark goes down all the same, and sends it at least 4 slabs off */
    wft_bx = ox + 2;
    wft_by = oy + 2;
    {
        long smodel = get_id(creature_desc, "FLY"); /* (not an Imp: an Imp would come back to fill) */
        if (smodel <= 0)
            smodel = get_players_special_digger_model(p);
        struct Coord3d spos;
        set_coords_to_slab_center(&spos, wft_bx, wft_by);
        struct Thing *sitter = create_creature(&spos, (ThingModel)smodel, p);
        wft_sitter = thing_exists(sitter) ? sitter->index : 0;
    }
    /* the block (the middle is filled last) */
    for (MapSlabCoord y = oy + 1; y < oy + 4; y++)
        for (MapSlabCoord x = ox + 1; x < ox + 4; x++)
            wft_mark(p, x, y);
    /* a creature in the pocket, and its opening marked */
    long model = get_id(creature_desc, "FLY");
    if (model <= 0)
        model = get_players_special_digger_model(p);
    struct Coord3d pos;
    set_coords_to_slab_center(&pos, wft_px, wft_py);
    wft_mark(p, wft_ox, wft_oy);
    struct Thing *crtr = create_creature(&pos, (ThingModel)model, p);
    wft_crtr = thing_exists(crtr) ? crtr->index : 0;
    SYNCLOG("World: FILLTEST: turn %ld, player %d: 9 marks east of the Heart (engine slab %d,%d) and a pocket's opening at "
        "(%d,%d) with a %s kept in until turn %ld; %d marks", t, (int)p, (int)(ox + 1), (int)(oy + 1), (int)wft_ox, (int)wft_oy,
        thing_exists(crtr) ? thing_model_name(crtr) : "(none)", wft_at + 1200, wf_count(p));
    if (wft_choose && is_my_player_number(p))
        activate_room_build_mode(rk, get_room_kind_stats(rk)->tooltip_stridx);
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_migrate.c
 *     Endless worlds: Move The Heart (the dungeon's migration).
 * @par Purpose:
 *     See world_migrate.h. The state is in struct Game (world_keeper_record.h), so a thin client's panel shows it;
 *     only the simulating machine changes it.
 *
 *     The carry. The Heart's way is found over slabs (floor it can be carried over: no rock, earth, gold or wall, no
 *     lava, no door but the keeper's own), from where it is to the new home's middle, and kept in world slabs (the
 *     engine window moves). Four Imps are given the four corners round the Heart, right at it; each is walked to its
 *     corner and held there. The Heart goes up once all four are at their corners, and moves along its way only on a turn
 *     when all four are at them still (it waits in the air for one that is behind), at a walk the slowest keeps up with.
 *     An Imp that is fighting, fleeing, afraid, picked up, knocked out or dead lets go: the Heart drops to the floor
 *     where it is, and the task stays - a free Imp takes the empty corner as soon as there is one. Over its pedestal
 *     (raised: no floor to walk on at the corners) an Imp walks to the pedestal's corner and steps up onto its corner of
 *     the Heart, and is taken along with it until there is floor again.
 *
 *     Arriving. The new home's middle 3x3 is made a Dungeon Heart pedestal, as a start dungeon's: pedestal and flames,
 *     the room's own (the slabs' own objects), claimed floor round it. The Heart is set on it at the height it had over the old one. The old room stays as it is, its
 *     flames put out. Called off, the Heart is carried back and set on its old pedestal again.
 *
 *     The creatures. While the Heart is on the move, every second each of the keeper's creatures (not fighting, held or
 *     carried) is kept at its own place of the escort: a slab of the Heart's way from three behind it to four ahead,
 *     a subtile across. After the move, until a Lair is built near it, the ones with nothing to do are called back. Once a Heart has moved, rooms more than 40 slabs from it are not taken by its creatures
 *     (find_first_valid_position_for_thing_anywhere_in_room asks world_migrate_room_too_far), so they stay by it and
 *     use the old rooms only where they are near (the user, 2026-11-19: "Kept, used only if near").
 *
 * Test hook: KFX_WORLD_MIGRATETEST=turn: at that turn this machine's keeper (on a server: the first joined keeper)
 *     gets a 5x5 of its claimed floor 14 slabs east of its Heart, joined to the dungeon by a claimed corridor, and
 *     four Imps if it has fewer; then migrates there. KFX_WORLD_MIGRATECANCEL=n as well: called off n turns later.
 *     KFX_WORLD_MIGRATEPREP="turn[,distance[,creatures]]" instead: the same ground (the 5x5 that many slabs east, 14 when
 *     not given) and Imps, and nothing ordered (for a played game); with creatures, a bigger dungeon too: five rooms
 *     south-west of the Heart and that many creatures (levels 1-5) in them, and 20,000 gold for their wages (34 slabs east
 *     puts every one of those rooms past the 40-slab reach). Only loaded ground is written (about 39 slabs round the Heart).
 *     A fourth number, raiders: that many heroes (levels 3-6) come out of a pocket 8 slabs north of the tunnel's middle once
 *     the carried Soul Container is within 8 slabs of it, and make for the tunnel (an ambush). A fifth, south: the new home
 *     that many slabs south as well, with open claimed ground between (a diagonal way).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_migrate.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "bflib_math.h"
#include "bflib_sound.h"
#include "config_sounds.h"
#include "bflib_mouse.h"
#include "kjm_input.h"
#include "chat_box.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_graphics.h"
#include "dungeon_data.h"
#include "engine_render.h"
#include "engine_camera.h"
#include "frontend.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "map_blocks.h"
#include "map_columns.h"
#include "map_data.h"
#include "net_main.h"
#include "net_state.h"
#include "packets.h"
#include "player_data.h"
#include "player_utils.h"
#include "power_hand.h"
#include "room_data.h"
#include "room_util.h"
#include "server_admin.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_objects.h"
#include "thing_stats.h"
#include "cursor_tag.h"
#include "kfx/renderer/RendererManager.h"
#include "world_stream.h"
#include "observer.h"
#include "local_camera.h"
#include "bflib_planar.h"
#include "keeperfx.hpp"
#include "config.h"
#include "config_crtrstates.h"
#include "player_instances.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define WM_CORNER 224        /* from the Heart's middle to an Imp's corner, in map units, where they can stand */
#define WM_AT 192            /* an Imp this near its corner is at it (creatures walk to subtiles' middles); the Soul
                                Container moves only on a turn when all four are (the user, 2026-11-19: "should not move
                                until then") */
#define WM_LIFT 112          /* how high the carried Heart goes over the floor */
#define WM_FOLLOW_SLABS 6    /* creatures farther than this from the moving Heart are sent to it */
#define WM_WAIT_SLABS 8      /* after the move, idle creatures farther than this are called back (till a Lair is near) */
#define WM_PATH_MAX 8192

#define wm game.world_migration

static TbBool simulating(void)
{
    return world_mode_active() && !netstate_is_client();
}

static TbBool wm_player_ok(PlayerNumber p)
{
    return (p >= 0) && (p < PLAYERS_COUNT) && (p < PLAYERS_MAX);
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: migration, to player %d: %s", (int)p, text);
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

int world_migrate_state(PlayerNumber p)
{
    return wm_player_ok(p) ? wm[p].state : WMig_None;
}

int world_migrate_carriers(PlayerNumber p)
{
    return wm_player_ok(p) ? wm[p].carriers : 0;
}

/******************************************************************************/
/* Where the Heart can go */

/** Is the slab the keeper's own claimed floor, with nothing built on it? (The new home's 25.) */
static TbBool wm_home_slab(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || (slb->kind != SlbT_CLAIMED) || (slabmap_owner(slb) != p))
        return false;
    if (!room_is_invalid(slab_room_get(x, y)))
        return false;
    const struct Map *mapblk = get_map_block_at(slab_subtile_center(x), slab_subtile_center(y));
    return !map_block_invalid(mapblk) && map_block_revealed(mapblk, p);
}

static TbBool wm_home_fits(PlayerNumber p, MapSlabCoord cx, MapSlabCoord cy)
{
    const int r = WORLD_MIGRATE_ROOM_SIZE / 2;
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (!wm_home_slab(p, cx + i, cy + j))
                return false;
    return true;
}

/** Can the Heart be carried over this slab? */
static TbBool wm_passable(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    if ((x < 0) || (y < 0) || (x >= game.map_tiles_x) || (y >= game.map_tiles_y))
        return false;
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb))
        return false;
    if (slb->kind == SlbT_DUNGHEART)
        return (slabmap_owner(slb) == p); /* (its own pedestal: it floats over it) */
    if (slab_kind_is_door(slb->kind))
        return (slabmap_owner(slb) == p);
    if (slb->kind == SlbT_LAVA)
        return false;
    const struct SlabConfigStats *slabst = get_slab_kind_stats(slb->kind);
    return (slabst->block_flags & (SlbAtFlg_Filled | SlbAtFlg_Digable | SlbAtFlg_Valuable)) == 0;
}

static TbBool wm_standable(MapCoord x, MapCoord y);

/* each keeper's way, in world slabs: [0] is the Heart's next slab */
static int64_t *wm_path[PLAYERS_MAX];
static int wm_path_len[PLAYERS_MAX], wm_path_at[PLAYERS_MAX];
static GameTurn wm_path_made[PLAYERS_MAX];

static void wm_path_clear(PlayerNumber p)
{
    free(wm_path[p]);
    wm_path[p] = NULL;
    wm_path_len[p] = 0;
    wm_path_at[p] = 0;
}

/** Finds the Heart's way from (sx,sy) to (tx,ty), engine slabs, over slabs it can be carried over (breadth first, the
 *  four sides), inside a box round the two with WM_PATH_MARGIN slabs to spare (an endless map's engine coordinates
 *  are far larger than any search could cover). False when there is none. */
#define WM_PATH_MARGIN 48
#define WM_PATH_BOX 1024
static TbBool wm_find_path(PlayerNumber p, MapSlabCoord sx, MapSlabCoord sy, MapSlabCoord tx, MapSlabCoord ty)
{
    wm_path_clear(p);
    const long x0 = max(0L, (long)min(sx, tx) - WM_PATH_MARGIN), y0 = max(0L, (long)min(sy, ty) - WM_PATH_MARGIN);
    const long x1 = min((long)game.map_tiles_x - 1, (long)max(sx, tx) + WM_PATH_MARGIN);
    const long y1 = min((long)game.map_tiles_y - 1, (long)max(sy, ty) + WM_PATH_MARGIN);
    const long w = x1 - x0 + 1, h = y1 - y0 + 1;
    if ((sx < x0) || (sy < y0) || (tx < x0) || (ty < y0) || (sx > x1) || (sy > y1) || (tx > x1) || (ty > y1) ||
        (w > WM_PATH_BOX) || (h > WM_PATH_BOX))
        return false;
    int32_t *from = (int32_t *)malloc((size_t)w * h * sizeof(int32_t));
    int32_t *queue = (int32_t *)malloc((size_t)w * h * sizeof(int32_t));
    if ((from == NULL) || (queue == NULL))
    {
        free(from);
        free(queue);
        return false;
    }
    for (long i = 0; i < w * h; i++)
        from[i] = -1;
    long head = 0, tail = 0;
    const int32_t start = (int32_t)((sy - y0) * w + (sx - x0)), goal = (int32_t)((ty - y0) * w + (tx - x0));
    from[start] = start;
    queue[tail++] = start;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while ((head < tail) && (from[goal] < 0))
    {
        const int32_t c = queue[head++];
        const long cx = c % w, cy = c / w;
        for (int d = 0; d < 4; d++)
        {
            const long nx = cx + dx[d], ny = cy + dy[d];
            if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h))
                continue;
            const int32_t n = (int32_t)(ny * w + nx);
            if ((from[n] >= 0) || !wm_passable(p, x0 + nx, y0 + ny))
                continue;
            from[n] = c;
            queue[tail++] = n;
        }
    }
    TbBool found = (from[goal] >= 0);
    if (found)
    {
        int len = 0;
        for (int32_t c = goal; c != start; c = from[c])
            len++;
        if (len > WM_PATH_MAX)
            found = false;
        else
        {
            wm_path[p] = (int64_t *)malloc(sizeof(int64_t) * 2 * (len > 0 ? len : 1));
            if (wm_path[p] == NULL)
                found = false;
            else
            {
                int k = len;
                for (int32_t c = goal; c != start; c = from[c])
                {
                    k--;
                    world_slab_to_world(x0 + c % w, y0 + c / w, &wm_path[p][2 * k], &wm_path[p][2 * k + 1]);
                }
                wm_path_len[p] = len;
                wm_path_at[p] = 0;
                wm_path_made[p] = game.play_gameturn;
            }
        }
    }
    free(from);
    free(queue);
    return found;
}

/** Can the Soul Container be carried in a straight line from a to b: at every point of the line, all four of its Imps'
 *  corners (WM_CORNER from it, and a little more) on floor an Imp can walk on? (A shortcut that only kept the line
 *  itself clear cut round the corners of walls with the Imps behind them: the user's picture, 2026-11-19.) */
#define WM_LINE_ROOM (WM_CORNER + 48)
static TbBool wm_line_clear(PlayerNumber p, const struct Coord3d *a, const struct Coord3d *b)
{
    const long dx = (long)b->x.val - (long)a->x.val, dy = (long)b->y.val - (long)a->y.val;
    const long len = LbSqrL(dx * dx + dy * dy);
    const long steps = max(1L, len / 64);
    for (long i = 0; i <= steps; i++)
    {
        const long x = a->x.val + dx * i / steps, y = a->y.val + dy * i / steps;
        for (int c = 0; c < 4; c++)
        {
            const long cx = x + ((c & 1) ? WM_LINE_ROOM : -WM_LINE_ROOM), cy = y + ((c & 2) ? WM_LINE_ROOM : -WM_LINE_ROOM);
            if ((cx < 0) || (cy < 0) || !wm_passable(p, subtile_slab(coord_subtile(cx)), subtile_slab(coord_subtile(cy))) ||
                !wm_standable(cx, cy))
                return false;
        }
    }
    return true;
}

/** The goal of the carry now: the new home's middle, or the old one's when called off (engine slabs). */
static TbBool wm_goal(PlayerNumber p, MapSlabCoord *gx, MapSlabCoord *gy)
{
    if (wm[p].state == WMig_Going)
        return world_engine_slab(wm[p].to_x, wm[p].to_y, gx, gy);
    return world_engine_slab(wm[p].from_x, wm[p].from_y, gx, gy);
}

/******************************************************************************/
/* The Imps */

static struct Thing *wm_imp(PlayerNumber p, int k)
{
    if (wm[p].imp[k] == 0)
        return INVALID_THING;
    struct Thing *imp = thing_get(wm[p].imp[k]);
    if (!thing_exists(imp) || !thing_is_creature(imp) || (imp->owner != p) || (imp->creation_turn != wm[p].imp_made[k]))
        return INVALID_THING;
    return imp;
}

/** Is the Imp in a state to carry (not fighting, fleeing, held, knocked out or dying)? */
static TbBool wm_imp_can_carry(const struct Thing *imp)
{
    if (!thing_exists(imp) || creature_is_dying(imp) || creature_is_being_unconscious(imp) || thing_is_picked_up(imp) ||
        creature_is_kept_in_custody(imp) || creature_is_fleeing_combat(imp) ||
        creature_under_spell_effect(imp, CSAfF_Chicken) || creature_under_spell_effect(imp, CSAfF_Freeze))
        return false;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(imp);
    if (creature_control_invalid(cctrl) || (cctrl->combat_flags != 0) || creature_under_spell_effect(imp, CSAfF_Fear))
        return false;
    return (get_creature_state_besides_interruptions(imp) != CrSt_CreatureCombatFlee) &&
        (imp->active_state != CrSt_CreatureCombatFlee) && (imp->continue_state != CrSt_CreatureCombatFlee);
}

/* the Imps that stepped up onto the pedestal with the Soul Container (their corner there is no floor to walk on): this
   machine's, found again after a load */
static uint8_t wm_hop[PLAYERS_MAX][4];

static void wm_release(PlayerNumber p, int k, const char *why)
{
    wm_hop[p][k] = 0;
    struct Thing *imp = wm_imp(p, k);
    wm[p].imp[k] = 0;
    wm[p].imp_made[k] = 0;
    if (!thing_exists(imp))
        return;
    SYNCLOG("World: migration of player %d: the Imp (index %d) at corner %d lets go (%s)", (int)p, (int)imp->index, k, why);
    if (wm_imp_can_carry(imp) && (imp->active_state == CrSt_MoveToPosition))
    {
        cleanup_current_thing_state(imp);
        set_start_state(imp);
    }
}

static void wm_release_all(PlayerNumber p, const char *why)
{
    for (int k = 0; k < 4; k++)
        wm_release(p, k, why);
    wm[p].carriers = 0;
    wm[p].carried = 0;
}

/** Can an Imp stand at this map place (floor, not the raised pedestal or a wall)? */
static TbBool wm_standable(MapCoord x, MapCoord y)
{
    const MapSubtlCoord sx = coord_subtile(x), sy = coord_subtile(y);
    if ((sx < 0) || (sy < 0) || (sx >= game.map_subtiles_x) || (sy >= game.map_subtiles_y))
        return false;
    return get_floor_filled_subtiles_at(sx, sy) <= 1;
}

/** The place of corner k (0-3) round the Heart: as near as WM_CORNER where an Imp can stand there, else pushed out
 *  along its diagonal until it can (over the pedestal: its corners). */
static void wm_corner(const struct Thing *heart, int k, struct Coord3d *pos)
{
    const int sx = (k & 1) ? 1 : -1, sy = (k & 2) ? 1 : -1;
    MapCoord d = WM_CORNER;
    for (int step = 0; step < 16; step++, d += COORD_PER_STL / 2)
    {
        pos->x.val = heart->mappos.x.val + sx * d;
        pos->y.val = heart->mappos.y.val + sy * d;
        if (wm_standable(pos->x.val, pos->y.val))
            break;
    }
    pos->z.val = 0;
    pos->z.val = get_floor_height_at(pos);
}

/** Corner k right at the Soul Container (WM_CORNER from its middle), standable or not; z on the floor there. */
static void wm_corner_close(const struct Thing *heart, int k, struct Coord3d *pos)
{
    pos->x.val = heart->mappos.x.val + ((k & 1) ? WM_CORNER : -WM_CORNER);
    pos->y.val = heart->mappos.y.val + ((k & 2) ? WM_CORNER : -WM_CORNER);
    pos->z.val = 0;
    pos->z.val = get_floor_height_at(pos);
}

/** An enemy creature (heroes, a keeper not allied; not held, knocked out or dying) within `slabs` of the thing that
 *  can walk to it (wm_enemy_reaches), or NULL. (Held to its corner, an Imp's own sense of danger never runs: the user, 2026-11-19, "the 2 imps in the back
 *  of the Soul Container don't flee at the presence of enemies. They seem to get stuck.") */
#define WM_FEAR_SLABS 4

/** A slab no enemy walks through to the Imps: rock, earth, gold, any wall (fortified too), a door (it has to be broken
 *  first), lava for a creature that does not fly. */
static TbBool wm_fear_wall(MapSlabCoord x, MapSlabCoord y, TbBool flier)
{
    if ((x < 0) || (y < 0) || (x >= game.map_tiles_x) || (y >= game.map_tiles_y))
        return true;
    const struct SlabMap *slb = get_slabmap_block(x, y);
    if (slabmap_block_invalid(slb) || slab_kind_is_door(slb->kind))
        return true;
    if (slb->kind == SlbT_LAVA)
        return !flier;
    const struct SlabConfigStats *slabst = get_slab_kind_stats(slb->kind);
    return (slabst->block_flags & (SlbAtFlg_Filled | SlbAtFlg_Digable | SlbAtFlg_Valuable)) != 0;
}

/** Can the enemy walk to the thing in at most 2 x slabs + 2 slab steps over open slabs (the four sides)? A hero on the
 *  far side of a wall is no danger to the Imps (the user, 2026-11-21: "The raid's Tunneller was on the other side of a
 *  fortified wall. Imps shouldn't flee then"). */
#define WM_FEAR_BOX 32
static TbBool wm_enemy_reaches(const struct Thing *thing, const struct Thing *enemy, int slabs)
{
    const MapSlabCoord tx = subtile_slab(thing->mappos.x.stl.num), ty = subtile_slab(thing->mappos.y.stl.num);
    const MapSlabCoord ex = subtile_slab(enemy->mappos.x.stl.num), ey = subtile_slab(enemy->mappos.y.stl.num);
    const int r = slabs + 3, w = 2 * r + 1;
    if ((w > WM_FEAR_BOX) || (labs((long)ex - tx) > r) || (labs((long)ey - ty) > r))
        return false;
    if ((ex == tx) && (ey == ty))
        return true;
    const TbBool flier = ((enemy->movement_flags & TMvF_Flying) != 0);
    uint8_t steps[WM_FEAR_BOX * WM_FEAR_BOX];
    int16_t queue[WM_FEAR_BOX * WM_FEAR_BOX];
    memset(steps, 0xFF, sizeof(steps));
    int head = 0, tail = 0;
    const int start = (ey - ty + r) * w + (ex - tx + r), goal = r * w + r;
    steps[start] = 0;
    queue[tail++] = (int16_t)start;
    const int most = 2 * slabs + 2;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while (head < tail)
    {
        const int c = queue[head++];
        if (steps[c] >= most)
            continue;
        const int cx = c % w, cy = c / w;
        for (int d = 0; d < 4; d++)
        {
            const int nx = cx + dx[d], ny = cy + dy[d];
            if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= w))
                continue;
            const int n = ny * w + nx;
            if (steps[n] != 0xFF)
                continue;
            if (n == goal)
                return true;
            if (wm_fear_wall(tx - r + nx, ty - r + ny, flier))
                continue;
            steps[n] = (uint8_t)(steps[c] + 1);
            queue[tail++] = (int16_t)n;
        }
    }
    return false;
}

static const struct Thing *wm_enemy_near(PlayerNumber p, const struct Thing *thing, int slabs)
{
    const MapSubtlCoord r = slabs * STL_PER_SLB;
    const MapSubtlCoord cx = thing->mappos.x.stl.num, cy = thing->mappos.y.stl.num;
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
                    !creature_is_kept_in_custody(t) && !creature_is_being_unconscious(t) && !creature_is_dying(t) &&
                    wm_enemy_reaches(thing, t, slabs))
                    return t;
            }
        }
    return NULL;
}

/** The Imp at corner k takes fright: it lets go and runs six slabs along the Heart's way, to the side away from the
 *  enemy (its own flee place would be its Heart, where the enemy is). */
static void wm_frighten(PlayerNumber p, int k, struct Thing *imp, const struct Thing *enemy)
{
    struct Coord3d away = imp->mappos;
    TbBool found = false;
    if ((wm_path[p] != NULL) && (wm_path_len[p] > 0))
    {
        MapCoordDelta best = -1;
        for (int side = -1; side <= 1; side += 2)
        {
            const int w = max(0, min(wm_path_at[p] + side * 6, wm_path_len[p] - 1));
            MapSlabCoord x, y;
            if (!world_engine_slab(wm_path[p][2 * w], wm_path[p][2 * w + 1], &x, &y))
                continue;
            struct Coord3d c;
            c.x.val = subtile_coord_center(slab_subtile_center(x));
            c.y.val = subtile_coord_center(slab_subtile_center(y));
            c.z.val = 0;
            const MapCoordDelta d = get_chessboard_distance(&c, &enemy->mappos);
            if (d > best)
            {
                best = d;
                away = c;
                found = true;
            }
        }
    }
    if (!found)
    {
        /* no way known: straight away from the enemy */
        away.x.val = imp->mappos.x.val + (imp->mappos.x.val - enemy->mappos.x.val);
        away.y.val = imp->mappos.y.val + (imp->mappos.y.val - enemy->mappos.y.val);
    }
    wm_release(p, k, "an enemy is near: it flees");
    cleanup_current_thing_state(imp);
    if (setup_person_move_to_coord(imp, &away, NavRtF_Default))
        imp->continue_state = CrSt_ImpDoingNothing;
    else
        set_start_state(imp);
}

/** Free Imps take the empty corners (the nearest first). */
static void wm_fill_corners(PlayerNumber p, const struct Thing *heart)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    if (wm_enemy_near(p, heart, WM_FEAR_SLABS + 1) != NULL)
        return; /* (no Imp goes to it while an enemy is at it: the task waits till they are driven off) */
    for (int k = 0; k < 4; k++)
    {
        if (thing_exists(wm_imp(p, k)))
            continue;
        wm[p].imp[k] = 0;
        struct Coord3d at;
        wm_corner(heart, k, &at);
        struct Thing *best = INVALID_THING;
        MapCoordDelta best_d = INT32_MAX;
        long i = dungeon->digger_list_start;
        unsigned long guard = 0;
        while ((i != 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *imp = thing_get(i);
            const struct CreatureControl *cctrl = creature_control_get_from_thing(imp);
            if (creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (!wm_imp_can_carry(imp) || !thing_is_creature_digger(imp))
                continue;
            TbBool taken = false;
            for (int o = 0; o < 4; o++)
                if (wm[p].imp[o] == imp->index)
                    taken = true;
            if (taken)
                continue;
            const MapCoordDelta d = get_chessboard_distance(&imp->mappos, &at);
            if ((d < best_d) && (wm_enemy_near(p, imp, WM_FEAR_SLABS + 2) == NULL))
            {
                best_d = d;
                best = imp;
            }
        }
        if (!thing_exists(best))
            return; /* no free Imp now: asked again in a moment */
        wm[p].imp[k] = best->index;
        wm[p].imp_made[k] = best->creation_turn;
        SYNCLOG("World: migration of player %d: the Imp (index %d) goes to corner %d", (int)p, (int)best->index, k);
    }
}

/** Sends the Imp to its corner, or holds it there. */
static void wm_imp_to(struct Thing *imp, const struct Coord3d *at)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(imp);
    /* (a creature walks to the middle of a subtile: the same subtile is the same place) */
    if ((imp->active_state == CrSt_MoveToPosition) && (cctrl->moveto_pos.x.stl.num == at->x.stl.num) &&
        (cctrl->moveto_pos.y.stl.num == at->y.stl.num))
        return; /* on its way there */
    if ((imp->active_state != CrSt_MoveToPosition) && (imp->mappos.x.stl.num == at->x.stl.num) &&
        (imp->mappos.y.stl.num == at->y.stl.num))
        return; /* standing at it */
    cleanup_current_thing_state(imp);
    if (setup_person_move_to_coord(imp, at, NavRtF_Default))
        imp->continue_state = CrSt_ImpDoingNothing;
    else if ((game.play_gameturn % 200) == 0)
    {
        /* (so a log says it: such an Imp is free meanwhile and the Imps' own work takes it away) */
        const struct SlabMap *slb = get_slabmap_for_subtile(at->x.stl.num, at->y.stl.num);
        WARNLOG("World: migration: Imp %d at (%d,%d) can't walk to its corner (%d,%d): floor height %d, slab kind %d",
            (int)imp->index, (int)imp->mappos.x.stl.num, (int)imp->mappos.y.stl.num, (int)at->x.stl.num, (int)at->y.stl.num,
            (int)get_floor_filled_subtiles_at(at->x.stl.num, at->y.stl.num), slabmap_block_invalid(slb) ? -1 : (int)slb->kind);
    }
}

/******************************************************************************/
/* The rooms */

static TbBool wm_is_flame(const struct Thing *thing)
{
    switch (thing->model)
    {
    case ObjMdl_HeartFlameRed: case ObjMdl_HeartFlameBlue: case ObjMdl_HeartFlameGreen: case ObjMdl_HeartFlameYellow:
    case ObjMdl_HeartFlameWhite: case ObjMdl_HeartFlamePurple: case ObjMdl_HeartFlameBlack: case ObjMdl_HeartFlameOrange:
        return true;
    default:
        return false;
    }
}

/** The old home's flames go out (its pedestal stays). */
static int wm_douse(MapSlabCoord cx, MapSlabCoord cy)
{
    const int r = WORLD_MIGRATE_ROOM_SIZE / 2;
    int doused = 0;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Object);
    long i = slist->index;
    unsigned long guard = 0;
    while ((i != 0) && (guard++ < THINGS_COUNT))
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!wm_is_flame(thing))
            continue;
        const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num), y = subtile_slab(thing->mappos.y.stl.num);
        if ((abs(x - cx) <= r) && (abs(y - cy) <= r))
        {
            destroy_object(thing);
            doused++;
        }
    }
    return doused;
}

/** The new home, as a start dungeon's: the 3x3 Dungeon Heart pedestal (its flames come with its slabs) in the middle
 *  of the 5x5, claimed floor round it; a second Soul Container that came with the slabs is taken away, the keeper's
 *  own is the Heart. */
static void wm_build_home(PlayerNumber p, struct Thing *heart, MapSlabCoord cx, MapSlabCoord cy)
{
    const int r = WORLD_MIGRATE_ROOM_SIZE / 2;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            replace_slab_from_script(cx + i, cy + j, SlbT_DUNGHEART);
    const struct StructureList *slist = get_list_for_thing_class(TCls_Object);
    long i = slist->index;
    unsigned long guard = 0;
    while ((i != 0) && (guard++ < THINGS_COUNT))
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if ((thing == heart) || (thing->model != heart->model))
            continue;
        const MapSlabCoord x = subtile_slab(thing->mappos.x.stl.num), y = subtile_slab(thing->mappos.y.stl.num);
        if ((abs(x - cx) <= r) && (abs(y - cy) <= r))
        {
            SYNCLOG("World: migration of player %d: a second Soul Container (index %d) that came with the room is taken away",
                (int)p, (int)thing->index);
            destroy_object(thing);
        }
    }
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (!dungeon_invalid(dungeon))
    {
        dungeon->dnheart_idx = heart->index;
        /* (a backup heart the room's own Soul Containers were taken for is gone with them) */
        const struct Thing *backup = thing_get(dungeon->backup_heart_idx);
        if ((dungeon->backup_heart_idx != 0) && (!thing_exists(backup) || (backup == heart) || !thing_is_dungeon_heart(backup)))
            dungeon->backup_heart_idx = 0;
    }
}

/** The Heart sets down at (cx, cy)'s middle, at the height it had over its pedestal. */
static void wm_set_heart(PlayerNumber p, struct Thing *heart, MapSlabCoord cx, MapSlabCoord cy)
{
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(cx));
    pos.y.val = subtile_coord_center(slab_subtile_center(cy));
    pos.z.val = wm[p].from_z;
    move_thing_in_map(heart, &pos);
}

/** Is the camera near the Heart (in WM_CAMERA_SLABS)? */
#define WM_CAMERA_SLABS 16
static TbBool wm_camera_near(const struct DungeonCamera *cam, const struct Thing *heart)
{
    const long dx = labs((long)cam->x - (long)heart->mappos.x.val), dy = labs((long)cam->y - (long)heart->mappos.y.val);
    return (max(dx, dy) <= WM_CAMERA_SLABS * COORD_PER_SLB);
}

/******************************************************************************/
/* The packets (game state) */

static void wm_start(PlayerNumber p, MapSlabCoord tx, MapSlabCoord ty)
{
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    if (wm[p].state != WMig_None)
    {
        tell_keeper(p, "The Soul Container is on the move already.");
        return;
    }
    if (!wm_home_fits(p, tx, ty))
    {
        tell_keeper(p, "The Soul Container needs 5 by 5 slabs of your own claimed floor, with no room on it.");
        return;
    }
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (!wm_find_path(p, hx, hy, tx, ty))
    {
        tell_keeper(p, "The Imps can't carry the Soul Container there: no way through.");
        return;
    }
    int64_t wx, wy;
    world_slab_to_world(tx, ty, &wx, &wy);
    wm[p].to_x = (int32_t)wx;
    wm[p].to_y = (int32_t)wy;
    world_slab_to_world(hx, hy, &wx, &wy);
    wm[p].from_x = (int32_t)wx;
    wm[p].from_y = (int32_t)wy;
    wm[p].from_z = heart->mappos.z.val;
    wm[p].state = WMig_Going;
    {
        /* the camera's jump of a room placed (keeper_build_room: 240 for one of more slabs; the dungeon is game state,
           so a joined player's camera jumps too) */
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (!dungeon_invalid(dungeon))
            dungeon->camera_deviate_jump = 240;
    }
    wm[p].carriers = 0;
    wm[p].carried = 0;
    memset(wm[p].imp, 0, sizeof(wm[p].imp));
    memset(wm[p].imp_made, 0, sizeof(wm[p].imp_made));
    SYNCLOG("World: player %d's Soul Container is to be carried from slab (%d,%d) to (%d,%d), %d slabs of way",
        (int)p, (int)hx, (int)hy, (int)tx, (int)ty, wm_path_len[p]);
    tell_keeper(p, "Migration in progress: four Imps will carry the Soul Container to its new home.");
}

static void wm_cancel(PlayerNumber p)
{
    if (wm[p].state != WMig_Going)
        return;
    struct Thing *heart = get_player_soul_container(p);
    MapSlabCoord ox, oy;
    if (!thing_exists(heart) || !world_engine_slab(wm[p].from_x, wm[p].from_y, &ox, &oy))
        return;
    wm_path_clear(p);
    if ((subtile_slab(heart->mappos.x.stl.num) == ox) && (subtile_slab(heart->mappos.y.stl.num) == oy) &&
        (get_2d_distance(&heart->mappos, &(struct Coord3d){.x.val = subtile_coord_center(slab_subtile_center(ox)),
        .y.val = subtile_coord_center(slab_subtile_center(oy))}) < COORD_PER_STL))
    {
        /* it hasn't left yet */
        wm_set_heart(p, heart, ox, oy);
        wm_release_all(p, "called off");
        wm[p].state = WMig_None;
        tell_keeper(p, "Migration called off.");
        return;
    }
    wm[p].state = WMig_Returning;
    tell_keeper(p, "Migration called off: the Imps carry the Soul Container back.");
}

void world_migrate_packet(PlayerNumber p, int what, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!simulating() || !wm_player_ok(p) || !world_player_is_human(p))
        return;
    if (what == 1)
        wm_start(p, subtile_slab(stl_x), subtile_slab(stl_y));
    else if (what == 2)
        wm_cancel(p);
}

/******************************************************************************/
/* Every turn (the simulating machine) */

/** Has the keeper a Lair within reach of its Heart (its creatures' new rooms are being built)? */
static TbBool wm_lair_near(PlayerNumber p, const struct Thing *heart)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return false;
    for (RoomKind rkind = 0; rkind < game.conf.slab_conf.room_types_count; rkind++)
    {
        if (!room_role_matches(rkind, RoRoF_LairStorage))
            continue;
        long i = dungeon->room_list_start[rkind];
        unsigned long guard = 0;
        while ((i != 0) && (guard++ < ROOMS_COUNT))
        {
            const struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            i = room->next_of_owner;
            const long dx = labs((long)room->central_stl_x - (long)heart->mappos.x.stl.num);
            const long dy = labs((long)room->central_stl_y - (long)heart->mappos.y.stl.num);
            if (max(dx, dy) <= WORLD_MIGRATE_ROOM_REACH * STL_PER_SLB)
                return true;
        }
    }
    return false;
}

/** A creature with nothing to do (standing about, or wandering off with nothing to do at the end of it). */
static TbBool wm_idle(const struct Thing *thing)
{
    if ((thing->active_state == CrSt_CreatureDoingNothing) || (thing->active_state == CrSt_CreatureWantsAHome))
        return true;
    return (thing->active_state == CrSt_MoveToPosition) && (thing->continue_state == CrSt_CreatureDoingNothing);
}

/** The keeper's creatures go to the Heart: all of them while it is on the move (only_idle false), and after it has
 *  moved, until a Lair is built near it, the ones with nothing to do (the user, 2026-11-19: "follow the Soul Container
 *  to its new location and wait for their new rooms to be built"). */
/** A creature's place in the Soul Container's escort while it is carried (the user, 2026-11-19: "follow around it, not
 *  just behind it. They may move in front as well to protect it"): a slab of its way from three behind it to four ahead
 *  of it, by the creature's index, and a subtile across the way; where that is no floor, the slab's middle. */
static TbBool wm_escort_place(PlayerNumber p, const struct Thing *heart, const struct Thing *thing, struct Coord3d *pos)
{
    if ((wm_path[p] == NULL) || (wm_path_len[p] <= 0))
        return false;
    const int d = (int)(thing->index % 8) - 3;
    int k = wm_path_at[p] + d;
    k = max(0, min(k, wm_path_len[p] - 1));
    MapSlabCoord x, y;
    if (!world_engine_slab(wm_path[p][2 * k], wm_path[p][2 * k + 1], &x, &y))
        return false;
    pos->x.val = subtile_coord_center(slab_subtile_center(x));
    pos->y.val = subtile_coord_center(slab_subtile_center(y));
    pos->z.val = 0;
    const MapCoord ox = pos->x.val + (((thing->index / 8) % 3) - 1) * COORD_PER_STL;
    const MapCoord oy = pos->y.val + (((thing->index / 24) % 3) - 1) * COORD_PER_STL;
    if (wm_standable(ox, oy))
    {
        pos->x.val = ox;
        pos->y.val = oy;
    } else if (!wm_standable(pos->x.val, pos->y.val))
        *pos = heart->mappos;
    return true;
}

static void wm_gather(PlayerNumber p, const struct Thing *heart, TbBool only_idle, MapCoordDelta keep)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    int sent = 0;
    long i = dungeon->creatr_list_start;
    unsigned long guard = 0;
    while ((i != 0) && (guard++ < CREATURES_COUNT))
    {
        struct Thing *thing = thing_get(i);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (!wm_imp_can_carry(thing) || thing_is_creature_digger(thing) || (thing->alloc_flags & TAlF_IsControlled))
            continue;
        struct Coord3d pos;
        if (!only_idle && wm_escort_place(p, heart, thing, &pos))
        {
            /* the escort: each at its own place round the Soul Container, before and behind it */
            if (get_chessboard_distance(&thing->mappos, &pos) <= COORD_PER_SLB)
                continue;
            if ((thing->active_state == CrSt_MoveToPosition) && (get_chessboard_distance(&cctrl->moveto_pos, &pos) <= COORD_PER_SLB / 2))
                continue; /* on its way */
        } else
        {
            if (get_chessboard_distance(&thing->mappos, &heart->mappos) <= keep)
                continue;
            if (only_idle && !wm_idle(thing))
                continue;
            if ((thing->active_state == CrSt_MoveToPosition) &&
                (get_chessboard_distance(&cctrl->moveto_pos, &heart->mappos) <= 3 * COORD_PER_SLB))
                continue; /* on its way */
            pos = heart->mappos;
            /* a place round the Heart, by its index (not all on one spot) */
            pos.x.val += ((thing->index % 5) - 2) * COORD_PER_STL;
            pos.y.val += (((thing->index / 5) % 5) - 2) * COORD_PER_STL;
            if (!wm_standable(pos.x.val, pos.y.val))
                pos = heart->mappos;
        }
        cleanup_current_thing_state(thing);
        if (setup_person_move_to_coord(thing, &pos, NavRtF_Default))
        {
            thing->continue_state = CrSt_CreatureDoingNothing;
            sent++;
        } else
            set_start_state(thing);
    }
    if ((sent > 0) && ((game.play_gameturn % 400) < 40))
        SYNCLOG("World: migration of player %d: %d creatures %s the Soul Container", (int)p, sent, only_idle ? "wait by" : "follow");
}

static void wm_follow(PlayerNumber p, const struct Thing *heart)
{
    wm_gather(p, heart, false, WM_FOLLOW_SLABS * COORD_PER_SLB);
}

static void wm_arrive(PlayerNumber p, struct Thing *heart, MapSlabCoord gx, MapSlabCoord gy)
{
    if (wm[p].state == WMig_Going)
    {
        /* the Imps step out to the room's corners first: the middle becomes the pedestal */
        const int r = WORLD_MIGRATE_ROOM_SIZE / 2;
        for (int k = 0; k < 4; k++)
        {
            struct Thing *imp = wm_imp(p, k);
            if (!thing_exists(imp))
                continue;
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(gx + ((k & 1) ? r : -r)));
            pos.y.val = subtile_coord_center(slab_subtile_center(gy + ((k & 2) ? r : -r)));
            pos.z.val = 0;
            pos.z.val = get_floor_height_at(&pos);
            move_thing_in_map(imp, &pos);
            reset_interpolation_of_thing(imp);
        }
        wm_build_home(p, heart, gx, gy);
        wm_set_heart(p, heart, gx, gy);
        MapSlabCoord ox, oy;
        int doused = 0;
        if (world_engine_slab(wm[p].from_x, wm[p].from_y, &ox, &oy))
            doused = wm_douse(ox, oy);
        wm[p].moved = 1;
        {
            /* set down as a room is placed: the camera's jump, when the keeper's camera is near the new home (its
               sounds: world_migrate_local_turn, on the keeper's own machine) */
            struct Dungeon *dungeon = get_players_num_dungeon(p);
            const struct UserState *ustate = get_player_primary_user_state(get_player(p));
            if (!dungeon_invalid(dungeon) && !user_state_invalid(ustate) && wm_camera_near(&ustate->dungeon_camera, heart))
                dungeon->camera_deviate_jump = 240;
        }
        SYNCLOG("World: player %d's Soul Container is in its new home at slab (%d,%d); %d flames of the old one put out",
            (int)p, (int)gx, (int)gy, doused);
        wm_release_all(p, "home");
        wm[p].state = WMig_None;
        wm_path_clear(p);
        wm_follow(p, heart);
        tell_keeper(p, "The Soul Container is in its new home.");
        return;
    }
    wm_set_heart(p, heart, gx, gy);
    SYNCLOG("World: player %d's Soul Container is back on its old pedestal", (int)p);
    wm_release_all(p, "back home");
    wm[p].state = WMig_None;
    wm_path_clear(p);
    tell_keeper(p, "The Soul Container is back on its old pedestal.");
}

static void wm_turn_one(PlayerNumber p)
{
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
    {
        wm_release_all(p, "no Heart");
        wm[p].state = WMig_None;
        wm_path_clear(p);
        return;
    }
    MapSlabCoord gx, gy;
    if (!wm_goal(p, &gx, &gy))
        return; /* (its goal is not in the loaded window now: wait) */
    /* the way: found again when there is none (a load, a shift) or now and then (walls dug, doors built) */
    /* after a failed search, the next one waits 50 turns (audit 2026-11-21: a blocked way was searched again every turn,
       up to 1024x1024 slabs each time) */
    static GameTurn failed_at[PLAYERS_MAX];
    const TbBool wait_after_fail = (failed_at[p] != 0) && (game.play_gameturn >= failed_at[p]) &&
        (game.play_gameturn - failed_at[p] < 50);
    if ((wm_path[p] == NULL) && wait_after_fail)
    {
        wm[p].carried = 0;
        return;
    }
    if ((wm_path[p] == NULL) || ((game.play_gameturn - wm_path_made[p]) > 400))
    {
        failed_at[p] = 0;
        if (!wm_find_path(p, subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), gx, gy))
        {
            failed_at[p] = game.play_gameturn;
            if ((game.play_gameturn % 400) == 0)
                tell_keeper(p, "The Imps can't get the Soul Container through: the way is blocked.");
            wm[p].carried = 0;
            return;
        }
    }
    /* who holds it */
    for (int k = 0; k < 4; k++)
    {
        struct Thing *imp = wm_imp(p, k);
        if ((wm[p].imp[k] != 0) && !thing_exists(imp))
            wm_release(p, k, "gone");
        else if (thing_exists(imp) && !wm_imp_can_carry(imp))
            wm_release(p, k, "frightened off or held");
    }
    if ((game.play_gameturn % 4) == (p % 4))
    {
        /* an Imp with an enemy near takes fright, and all four do when an enemy is near the Soul Container itself
           (looked at every fourth turn) */
        const struct Thing *at_heart = wm_enemy_near(p, heart, WM_FEAR_SLABS + 1);
        for (int k = 0; k < 4; k++)
        {
            struct Thing *imp = wm_imp(p, k);
            const struct Thing *enemy = !thing_exists(imp) ? NULL : (at_heart != NULL) ? at_heart : wm_enemy_near(p, imp, WM_FEAR_SLABS);
            if (enemy != NULL)
                wm_frighten(p, k, imp, enemy);
        }
    }
    if ((game.play_gameturn % 8) == (p % 8))
        wm_fill_corners(p, heart);
    int at = 0;
    long slowest = 0;
    MapCoordDelta away[4] = {0, 0, 0, 0}; /* each Imp from the place it is sent to (the progress line) */
    for (int k = 0; k < 4; k++)
    {
        struct Thing *imp = wm_imp(p, k);
        if (!thing_exists(imp))
            continue;
        /* its corner right at the Soul Container; over the pedestal (no floor to walk to there) the Imp walks to the
           pedestal's corner, then steps up onto its corner, and goes along with it until there is floor again */
        struct Coord3d close, target;
        wm_corner_close(heart, k, &close);
        const struct SlabMap *under = get_slabmap_for_subtile(close.x.stl.num, close.y.stl.num);
        const TbBool pedestal = !slabmap_block_invalid(under) && (under->kind == SlbT_DUNGHEART) && (slabmap_owner(under) == p);
        if (!wm_standable(close.x.val, close.y.val) && !pedestal)
        {
            /* no floor there and no pedestal (a door's post, a wall's edge): its corner comes in towards the Soul
               Container till there is floor - never out, round to the other side of the wall */
            /* down to 0, the Soul Container's own place (2026-11-21: the steps stopped at 32; with the Container near the
               far edge of its subtile every step was still on a Treasury pillar, the Imp was sent onto the pillar, could
               not walk there, went reinforcing walls, and the Container waited in the air till it was dropped) */
            for (MapCoord d = WM_CORNER - 64; ; d = max(d - 64, 0))
            {
                struct Coord3d in = close;
                in.x.val = heart->mappos.x.val + ((k & 1) ? d : -d);
                in.y.val = heart->mappos.y.val + ((k & 2) ? d : -d);
                if (wm_standable(in.x.val, in.y.val))
                {
                    close = in;
                    break;
                }
                if (d == 0)
                    break;
            }
        }
        if (wm_standable(close.x.val, close.y.val) || !pedestal)
        {
            wm_hop[p][k] = 0;
            target = close;
            wm_imp_to(imp, &target);
        } else if (wm_hop[p][k])
        {
            target = close;
            if (get_chessboard_distance(&imp->mappos, &close) > 8)
                move_thing_in_map(imp, &close);
        } else
        {
            wm_corner(heart, k, &target);
            wm_imp_to(imp, &target);
            if (get_chessboard_distance(&imp->mappos, &target) <= WM_AT)
            {
                wm_hop[p][k] = 1;
                cleanup_current_thing_state(imp);
                internal_set_thing_state(imp, CrSt_ImpDoingNothing);
                move_thing_in_map(imp, &close);
                target = close;
            }
        }
        const MapCoordDelta d = get_chessboard_distance(&imp->mappos, &target);
        away[k] = d;
        if (d <= WM_AT)
            at++;
        const long speed = get_creature_speed(imp);
        if ((slowest == 0) || (speed < slowest))
            slowest = speed;
    }
    if ((game.play_gameturn % 200) == 0)
    {
        char line[256];
        int n = 0;
        for (int k = 0; (k < 4) && (n < (int)sizeof(line) - 40); k++)
        {
            struct Thing *imp = wm_imp(p, k);
            if (thing_exists(imp))
                n += snprintf(line + n, sizeof(line) - n, " [%d: %ld away, state %d]", (int)imp->index, (long)away[k],
                    (int)imp->active_state);
            else
                n += snprintf(line + n, sizeof(line) - n, " [none]");
        }
        SYNCLOG("World: migration of player %d: Heart at (%d,%d) %s, way %d of %d;%s", (int)p, (int)heart->mappos.x.stl.num,
            (int)heart->mappos.y.stl.num, wm[p].carried ? "carried" : "down", wm_path_at[p], wm_path_len[p], line);
    }
    /* up once all four are at their corners; down again only when one lets go (frightened off, fighting, picked up,
       knocked out, dead: wm_release); one that is behind is waited for */
    int held = 0;
    for (int k = 0; k < 4; k++)
        if (thing_exists(wm_imp(p, k)))
            held++;
    const uint8_t was = wm[p].carried;
    wm[p].carriers = (uint8_t)held;
    wm[p].carried = (uint8_t)((held == 4) && (was || (at == 4)));
    if (was && !wm[p].carried)
        tell_keeper(p, "An Imp let go: the Soul Container is down!");
    struct Coord3d pos = heart->mappos;
    if (wm[p].carried && (at < 4))
        pos.z.val = get_floor_height_at(&pos) + WM_LIFT; /* (up, not moving: waiting for the one behind) */
    else if (wm[p].carried)
    {
        /* along the way, a little slower than the slowest of them walks */
        const struct Coord3d goal = {.x.val = subtile_coord_center(slab_subtile_center(gx)),
            .y.val = subtile_coord_center(slab_subtile_center(gy))};
        struct Coord3d next = goal;
        while (wm_path_at[p] < wm_path_len[p])
        {
            MapSlabCoord nx, ny;
            if (!world_engine_slab(wm_path[p][2 * wm_path_at[p]], wm_path[p][2 * wm_path_at[p] + 1], &nx, &ny))
                break;
            next.x.val = subtile_coord_center(slab_subtile_center(nx));
            next.y.val = subtile_coord_center(slab_subtile_center(ny));
            if ((get_chessboard_distance(&heart->mappos, &next) > 16) || (wm_path_at[p] + 1 >= wm_path_len[p]))
                break;
            wm_path_at[p]++;
        }
        if (wm_path_len[p] == 0)
            next = goal;
        else
        {
            /* straight where it can go straight (the user, 2026-11-19: "Allow the Soul Container move diagonally as
               well ... It seems to take sharp turns only"): it makes for the farthest point of its way, up to eight
               slabs on, that it can reach in a straight line with all four Imps' corners on floor; the points it
               passes on the way count as reached */
            int aim = wm_path_at[p];
            for (int j = min(wm_path_at[p] + 8, wm_path_len[p] - 1); j > wm_path_at[p]; j--)
            {
                MapSlabCoord jx, jy;
                if (!world_engine_slab(wm_path[p][2 * j], wm_path[p][2 * j + 1], &jx, &jy))
                    continue;
                struct Coord3d c;
                c.x.val = subtile_coord_center(slab_subtile_center(jx));
                c.y.val = subtile_coord_center(slab_subtile_center(jy));
                c.z.val = 0;
                if (wm_line_clear(p, &heart->mappos, &c))
                {
                    aim = j;
                    next = c;
                    break;
                }
            }
            MapCoordDelta nearest = INT32_MAX;
            int at_k = wm_path_at[p];
            for (int k = wm_path_at[p]; k <= aim; k++)
            {
                MapSlabCoord kx, ky;
                if (!world_engine_slab(wm_path[p][2 * k], wm_path[p][2 * k + 1], &kx, &ky))
                    continue;
                struct Coord3d c;
                c.x.val = subtile_coord_center(slab_subtile_center(kx));
                c.y.val = subtile_coord_center(slab_subtile_center(ky));
                c.z.val = 0;
                const MapCoordDelta d = get_chessboard_distance(&heart->mappos, &c);
                if (d < nearest)
                {
                    nearest = d;
                    at_k = k;
                }
            }
            if ((at_k > wm_path_at[p]) && (at_k < aim))
                wm_path_at[p] = at_k;
        }
        long step = max(4L, min(20L, slowest / 2));
        const long ddx = (long)next.x.val - (long)pos.x.val, ddy = (long)next.y.val - (long)pos.y.val;
        const long dist = LbSqrL(ddx * ddx + ddy * ddy);
        if (dist <= step)
        {
            pos.x.val = next.x.val;
            pos.y.val = next.y.val;
        } else
        {
            pos.x.val += ddx * step / dist;
            pos.y.val += ddy * step / dist;
        }
        pos.z.val = get_floor_height_at(&pos) + WM_LIFT;
        if ((pos.x.val == goal.x.val) && (pos.y.val == goal.y.val))
        {
            move_thing_in_map(heart, &pos);
            wm_arrive(p, heart, gx, gy);
            return;
        }
    } else
    {
        /* down: it lies on the floor where it is (over its pedestal: where it floats) */
        MapSlabCoord ox, oy;
        const TbBool home = world_engine_slab(wm[p].from_x, wm[p].from_y, &ox, &oy) &&
            (subtile_slab(heart->mappos.x.stl.num) == ox) && (subtile_slab(heart->mappos.y.stl.num) == oy);
        pos.z.val = home ? wm[p].from_z : get_floor_height_at(&pos);
    }
    if ((pos.x.val != heart->mappos.x.val) || (pos.y.val != heart->mappos.y.val) || (pos.z.val != heart->mappos.z.val))
        move_thing_in_map(heart, &pos);
    if ((game.play_gameturn % 20) == (p % 20))
        wm_follow(p, heart);
}

static void wm_test_turn(void);

void world_migrate_turn(void)
{
    if (!simulating())
        return;
    wm_test_turn();
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (wm[p].state != WMig_None)
            wm_turn_one(p);
        else if (wm[p].moved && ((game.play_gameturn % 40) == ((p + 20) % 40)))
        {
            /* moved, and no Lair near the new home yet: the idle creatures wait by the Heart */
            const struct Thing *heart = get_player_soul_container(p);
            if (thing_exists(heart) && !wm_lair_near(p, heart))
                wm_gather(p, heart, true, WM_WAIT_SLABS * COORD_PER_SLB);
        }
    }
}

TbBool world_migrate_room_too_far(const struct Thing *creatng, const struct Room *room)
{
    if (!world_mode_active() || !thing_exists(creatng) || (creatng->class_id != TCls_Creature) || room_is_invalid(room))
        return false;
    const PlayerNumber p = creatng->owner;
    if (!wm_player_ok(p) || (room->owner != p) || ((wm[p].moved == 0) && (wm[p].state == WMig_None)))
        return false;
    if (thing_is_creature_digger(creatng))
        return false;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return false;
    const long dx = labs((long)room->central_stl_x - (long)heart->mappos.x.stl.num);
    const long dy = labs((long)room->central_stl_y - (long)heart->mappos.y.stl.num);
    return (max(dx, dy) > WORLD_MIGRATE_ROOM_REACH * STL_PER_SLB);
}

void world_migrate_local_turn(void)
{
    /* this machine's keeper: its Soul Container set down in its new home (Going, then no migration and a moved Heart)
       sounds as a room placed, when the camera is near it */
    static int was = WMig_None;
    const PlayerNumber me = my_player_number;
    if (!world_mode_active() || netstate_dedicated() || !wm_player_ok(me))
        return;
    const int now = wm[me].state;
    if ((was == WMig_Going) && (now == WMig_None) && wm[me].moved)
    {
        const struct Thing *heart = get_player_soul_container(me);
        const struct UserState *ustate = get_local_user_state();
        if (thing_exists(heart) && !user_state_invalid(ustate) && wm_camera_near(&ustate->dungeon_camera, heart))
        {
            play_non_3d_sample(snd_tile_place);
            play_non_3d_sample(snd_larg_tile_down);
            play_non_3d_sample(snd_larg_tile_up);
            SYNCLOG("World: migration: the Soul Container is set down in view: its sounds");
        }
    }
    was = now;
}

TbBool world_migrate_quiet_room(PlayerNumber p, RoomKind rkind)
{
    if (!world_mode_active() || !wm_player_ok(p))
        return false;
    if (wm[p].state != WMig_None)
        return true;
    if (!wm[p].moved)
        return false;
    const struct Thing *heart = get_player_soul_container(p);
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (!thing_exists(heart) || dungeon_invalid(dungeon) || (rkind <= 0) || (rkind >= game.conf.slab_conf.room_types_count))
        return false;
    long i = dungeon->room_list_start[rkind];
    unsigned long guard = 0;
    while ((i != 0) && (guard++ < ROOMS_COUNT))
    {
        const struct Room *room = room_get(i);
        if (room_is_invalid(room))
            break;
        i = room->next_of_owner;
        const long dx = labs((long)room->central_stl_x - (long)heart->mappos.x.stl.num);
        const long dy = labs((long)room->central_stl_y - (long)heart->mappos.y.stl.num);
        if (max(dx, dy) <= WORLD_MIGRATE_ROOM_REACH * STL_PER_SLB)
            return false; /* (one of them is near: a creature that can't get there is worth telling) */
    }
    return true;
}

void world_migrate_seat_reset(PlayerNumber p)
{
    if (!wm_player_ok(p))
        return;
    if (wm[p].state != WMig_None)
        wm_release_all(p, "seat reset");
    memset(&wm[p], 0, sizeof(wm[p]));
    wm_path_clear(p);
}

/******************************************************************************/
/* This machine's placing (the panel's Migrate Dungeon) */

static TbBool wm_armed;

void world_migrate_arm(void)
{
    wm_armed = true;
    SYNCLOG("World: migration: placing the Soul Container (left click sets it down, right click calls it off)");
}

void world_migrate_disarm(void)
{
    wm_armed = false;
}

TbBool world_migrate_armed(void)
{
    return wm_armed && world_mode_active();
}

void world_migrate_request_cancel(void)
{
    set_players_packet_action(get_my_player(), PckA_WorldKeeperPanel, (2UL << 28), 0, 0, 0);
}

/** The slab under the pointer (the new home's middle), false off the map. */
static TbBool wm_pointed(MapSlabCoord *x, MapSlabCoord *y)
{
    /* (the block the 3D view finds under the pointer, as the Hand's) */
    if ((block_pointed_at_x < 0) || (block_pointed_at_y < 0) || (block_pointed_at_x >= game.map_subtiles_x) ||
        (block_pointed_at_y >= game.map_subtiles_y))
        return false;
    *x = subtile_slab(block_pointed_at_x);
    *y = subtile_slab(block_pointed_at_y);
    return true;
}

TbBool world_migrate_inputs(void)
{
    if (!world_migrate_armed())
        return false;
    if (get_local_view_type() != PVT_DungeonTop)
    {
        wm_armed = false;
        return false;
    }
    if (right_button_clicked || right_button_released || right_button_held)
    {
        if (right_button_released)
        {
            wm_armed = false;
            SYNCLOG("World: migration: placing called off");
        }
        /* (the held flag stays: it is what makes the release, the game's own input reads it) */
        right_button_clicked = 0;
        right_button_released = 0;
        return true;
    }
    if (left_button_clicked || left_button_held || left_button_released)
    {
        if (left_button_released)
        {
            MapSlabCoord x, y;
            if (wm_pointed(&x, &y))
            {
                if (wm_home_fits(my_player_number, x, y))
                {
                    /* the placing sounds of a room of many slabs (player_build_room_at), on this machine */
                    play_non_3d_sample(snd_tile_place);
                    play_non_3d_sample(snd_larg_tile_down);
                    play_non_3d_sample(snd_larg_tile_up);
                    wm_armed = false;
                    set_players_packet_action(get_my_player(), PckA_WorldKeeperPanel,
                        (1UL << 28) | (unsigned long)slab_subtile_center(x), (unsigned long)slab_subtile_center(y), 0, 0);
                    SYNCLOG("World: migration: the new home is to be at slab (%d,%d)", (int)x, (int)y);
                } else
                {
                    SYNCLOG("World: migration: slab (%d,%d) refused for the new home", (int)x, (int)y);
                    message_add(MsgType_Blank, 0, "The Soul Container needs 5 by 5 slabs of your own claimed floor, with no room on it.");
                }
            }
        }
        left_button_clicked = 0;
        left_button_released = 0;
        return true;
    }
    return false;
}

void world_migrate_cursor_box(void)
{
    /* the box is the game's cursor box: it stays on the screen until something clears it, and in the dungeon view the
       game itself does not (the user, 2026-11-19: after a placing, the box stayed until a click) - so it is cleared
       here the moment the placing ends, or the pointer leaves the map */
    static TbBool shown = false;
    MapSlabCoord x, y;
    if (!world_migrate_armed() || gameplay_cursor_is_over_gui() || !wm_pointed(&x, &y))
    {
        if (shown)
        {
            map_volume_box.visible = 0;
            shown = false;
        }
        return;
    }
    shown = true;
    const int r = WORLD_MIGRATE_ROOM_SIZE / 2;
    map_volume_box.visible = 1;
    map_volume_box.color = wm_home_fits(my_player_number, x, y) ? SLC_GREEN : SLC_RED;
    map_volume_box.beg_x = subtile_coord(slab_subtile(x - r, 0), 0);
    map_volume_box.beg_y = subtile_coord(slab_subtile(y - r, 0), 0);
    map_volume_box.end_x = subtile_coord(slab_subtile(x + r + 1, 0), 0);
    map_volume_box.end_y = subtile_coord(slab_subtile(y + r + 1, 0), 0);
    map_volume_box.floor_height_z = 1;
}

TbBool world_migrate_draw_hand(long cursor_x, long cursor_y)
{
    if (!world_migrate_armed())
        return false;
    const struct ObjectConfigStats *objst = get_object_model_stats(ObjMdl_SoulCountainer);
    if (objst == NULL)
        return false;
    const unsigned short anim = objst->sprite_anim_idx;
    const unsigned char frames = keepersprite_frames(anim);
    const unsigned char frame = (frames > 0) ? (unsigned char)((game.play_gameturn / 2) % frames) : 0;
    /* (about the Hand's size, standing on the pointer) */
    RendererSubmitKeeperHandSprite(cursor_x + scale_ui_value(8), cursor_y + scale_ui_value(22), anim, 0, frame,
        scale_ui_value(13), RendererGetDrawFlags());
    return true;
}

/******************************************************************************/
/* KFX_WORLD_MIGRATETEST=turn (see the file's head) */

/** Is the engine slab on ground that is loaded now (the test set-up writes only there)? */
static TbBool wm_loaded(MapSlabCoord x, MapSlabCoord y)
{
    int64_t wx, wy;
    MapSlabCoord ex, ey;
    world_slab_to_world(x, y, &wx, &wy);
    return world_engine_slab(wx, wy, &ex, &ey) && (ex == x) && (ey == y);
}

static void wm_test_slab(PlayerNumber p, MapSlabCoord x, MapSlabCoord y, SlabKind kind, int *skipped)
{
    if (!wm_loaded(x, y))
    {
        (*skipped)++;
        return;
    }
    if ((kind == SlbT_CLAIMED) || (kind == SlbT_PATH))
        place_slab_type_on_map(kind, slab_subtile_center(x), slab_subtile_center(y), p, 0);
    else
        replace_slab_from_script(x, y, kind);
}

static void wm_test_turn(void)
{
    static long at = -2, cancel_after = -1, distance = 14, creatures = 0, raiders = 0, south = 0;
    static PlayerNumber raid_p = -1;   /* the keeper whose carry the raiders wait for */
    static MapSlabCoord raid_x, raid_y; /* their pocket's middle (8 slabs north of the tunnel's middle) */
    static TbBool prep_only = false;
    static TbBool walled = false; /* raiders < 0: their pocket is 3 slabs north, behind a fortified wall, no way out */
    static PlayerNumber tp = -1;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_MIGRATETEST");
        at = -1;
        if (v != NULL)
            sscanf(v, "%ld,%ld,%ld,%ld,%ld", &at, &distance, &creatures, &raiders, &south); /* (the same as MIGRATEPREP) */
        const char *pv = getenv("KFX_WORLD_MIGRATEPREP");
        if ((at < 0) && (pv != NULL))
        {
            /* "turn[,distance[,creatures[,raiders]]]" */
            sscanf(pv, "%ld,%ld,%ld,%ld,%ld", &at, &distance, &creatures, &raiders, &south);
            prep_only = true;
        }
        distance = max(8L, min(distance, 120L));
        creatures = max(0L, min(creatures, 60L));
        walled = (raiders < 0);
        raiders = max(0L, min(labs(raiders), 20L));
        south = max(-30L, min(south, 30L));
        const char *c = getenv("KFX_WORLD_MIGRATECANCEL");
        cancel_after = (c != NULL) ? atol(c) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_MIGRATETEST=%ld (cancel after %ld)", at, cancel_after);
    }
    if (at < 0)
        return;
    if ((raid_p >= 0) && (wm[raid_p].state == WMig_Going))
    {
        /* the ambush: once the carried Soul Container is within 8 slabs of the pocket's column, the pocket is dug out
           (unclaimed path, with a way down to the tunnel) and the raiders come out of it, making for the tunnel */
        const struct Thing *heart = get_player_soul_container(raid_p);
        if (thing_exists(heart) && (subtile_slab(heart->mappos.x.stl.num) >= raid_x - 8))
        {
            int skipped = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++)
                    wm_test_slab(game.neutral_player_num, raid_x + i, raid_y + j, SlbT_PATH, &skipped);
            if (walled)
            {
                /* a fortified wall between the pocket and the tunnel: the heroes are 3 slabs from it and cannot get out */
                for (int i = -2; i <= 2; i++)
                    wm_test_slab(raid_p, raid_x + i, raid_y + 2, SlbT_WALLDRAPE, &skipped);
            } else
            for (MapSlabCoord y = raid_y + 2; y < raid_y + 8; y++)
                wm_test_slab(game.neutral_player_num, raid_x, y, SlbT_PATH, &skipped);
            static const char *const heroes[6] = {"KNIGHT", "ARCHER", "BARBARIAN", "WIZARD", "MONK", "SAMURAI"};
            int made = 0;
            struct Coord3d goal;
            goal.x.val = subtile_coord_center(slab_subtile_center(raid_x));
            goal.y.val = subtile_coord_center(slab_subtile_center(raid_y + 8));
            goal.z.val = 0;
            for (int i = 0; i < raiders; i++)
            {
                const long model = get_id(creature_desc, heroes[i % 6]);
                if (model <= 0)
                    continue;
                struct Coord3d pos;
                pos.x.val = subtile_coord_center(slab_subtile(raid_x - 1 + (i % 3), 1));
                pos.y.val = subtile_coord_center(slab_subtile(raid_y - 1 + ((i / 3) % 3), 1));
                pos.z.val = 0;
                struct Thing *thing = create_creature(&pos, (ThingModel)model, PLAYER_GOOD);
                if (thing_is_invalid(thing))
                    continue;
                set_creature_level(thing, (CrtrExpLevel)(2 + (i % 4)));
                if (!walled && setup_person_move_to_coord(thing, &goal, NavRtF_Default))
                    thing->continue_state = CrSt_GoodDoingNothing;
                made++;
            }
            SYNCLOG("World: migration test: %d hero raiders come out %s at slab (%d,%d)", made,
                walled ? "3 slabs north of the tunnel, behind a fortified wall" : "8 slabs north of the tunnel", (int)raid_x, (int)raid_y);
            raid_p = -1;
        }
    }
    if ((cancel_after >= 0) && (tp >= 0) && ((long)game.play_gameturn == at + cancel_after))
    {
        SYNCLOG("World: migration test: called off");
        world_migrate_packet(tp, 2, 0, 0);
    }
    {
        /* the camera to the Heart once it is home (for a screenshot of the new home) */
        static TbBool cammed = false;
        static long done_at = -1;
        if (!cammed && (tp >= 0) && ((long)game.play_gameturn > at) && (wm[tp].state == WMig_None) && !netstate_dedicated())
        {
            cammed = world_local_camera_to_heart();
            done_at = (long)game.play_gameturn;
            SYNCLOG("World: migration test: done at turn %ld; the camera to the Heart", (long)game.play_gameturn);
        }
        /* where the creatures are: at the end of the carry, then every 30 seconds (in 12 slabs of the Heart, of all) */
        if ((tp >= 0) && (((long)game.play_gameturn == at + 300) || ((long)game.play_gameturn == at + 700) || ((done_at >= 0) && ((((long)game.play_gameturn - done_at) % 600) == 0))))
        {
            const struct Thing *heart = get_player_soul_container(tp);
            const struct Dungeon *dungeon = get_players_num_dungeon(tp);
            int near = 0, all = 0, ahead = 0;
            long i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start;
            unsigned long guard = 0;
            while ((i != 0) && thing_exists(heart) && (guard++ < CREATURES_COUNT))
            {
                const struct Thing *thing = thing_get(i);
                const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
                if (creature_control_invalid(cctrl))
                    break;
                i = cctrl->players_next_creature_idx;
                all++;
                if (get_chessboard_distance(&thing->mappos, &heart->mappos) <= 12 * COORD_PER_SLB)
                {
                    near++;
                    if (thing->mappos.x.val > heart->mappos.x.val + COORD_PER_STL)
                        ahead++; /* (east of it: the test's way runs east) */
                }
                else if (all <= 12)
                    SYNCLOG("World: migration test: far: %s state %s (continue %s), %ld slabs off, lair room %d", thing_model_name(thing),
                        creature_state_code_name(thing->active_state), creature_state_code_name(thing->continue_state),
                        (long)(get_chessboard_distance(&thing->mappos, &heart->mappos) / COORD_PER_SLB), (int)cctrl->lair_room_id);
            }
            SYNCLOG("World: migration test: turn %ld, %d of the %d creatures within 12 slabs of the Heart (%d of them ahead of it)",
                (long)game.play_gameturn, near, all, ahead);
        }
    }
    if ((long)game.play_gameturn != at)
        return;
    PlayerNumber p = my_player_number;
    for (PlayerNumber q = 0; netstate_dedicated() && (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
        if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
        {
            p = q;
            break;
        }
    struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    tp = p;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const MapSlabCoord tx = hx + distance, ty = hy + south;
    /* a claimed corridor from the heart room's east side, and the 5x5 at its end */
    int skipped = 0;
    if (south != 0)
    {
        /* the new home that many slabs south too, with open claimed ground all the way between: the way across it
           runs diagonally */
        for (MapSlabCoord x = hx + 3; x <= tx + 2; x++)
            for (MapSlabCoord y = min(hy - 2, ty - 2); y <= max(hy + 2, ty + 2); y++)
                wm_test_slab(p, x, y, SlbT_CLAIMED, &skipped);
    } else
    for (MapSlabCoord x = hx + 3; x <= tx + 2; x++)
        for (int j = -2; j <= 2; j++)
        {
            if ((x < tx - 2) && (j != 0))
                continue;
            wm_test_slab(p, x, hy + j, SlbT_CLAIMED, &skipped);
        }
    reveal_map_area(p, slab_subtile(hx, 0), slab_subtile(tx + 4, 2), slab_subtile(min(hy, ty) - 4, 0), slab_subtile(max(hy, ty) + 4, 2));
    if (creatures > 0)
    {
        /* a bigger dungeon: south-west of the Heart a claimed hall with a Treasure Room and a Library (3x3), a Lair, a
           Hatchery and a Training Room (5x5), joined to the Heart's room, and the creatures in it; with the new home
           far enough east, every one of these rooms is more than WORLD_MIGRATE_ROOM_REACH slabs from it */
        for (MapSlabCoord y = hy + 3; y <= hy + 12; y++)
            for (MapSlabCoord x = hx - 31; x <= hx; x++)
                if ((y >= hy + 5) || (x == hx))
                    wm_test_slab(p, x, y, SlbT_CLAIMED, &skipped);
        static const struct { int x0, w; SlabKind kind; } rooms[5] = {
            {-30, 3, SlbT_TREASURE}, {-26, 3, SlbT_LIBRARY}, {-22, 5, SlbT_LAIR}, {-16, 5, SlbT_GARDEN}, {-10, 5, SlbT_TRAINING}};
        for (int r = 0; r < 5; r++)
            for (int j = 0; j < rooms[r].w; j++)
                for (int i = 0; i < rooms[r].w; i++)
                    wm_test_slab(p, hx + rooms[r].x0 + i, hy + 6 + j, rooms[r].kind, &skipped);
        reveal_map_area(p, slab_subtile(hx - 32, 0), slab_subtile(hx + 1, 2), slab_subtile(hy, 0), slab_subtile(hy + 13, 2));
        static const char *const kinds[10] = {"FLY", "BUG", "SPIDER", "DEMONSPAWN", "TROLL", "SORCEROR", "ORC", "HELL_HOUND",
            "DARK_MISTRESS", "BILE_DEMON"};
        int made = 0;
        for (int i = 0; i < creatures; i++)
        {
            const long model = get_id(creature_desc, kinds[i % 10]);
            if (model <= 0)
                continue;
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(hx - 28 + (i % 25)));
            pos.y.val = subtile_coord_center(slab_subtile_center(hy + 11 + (i / 25) % 2));
            pos.z.val = 0;
            struct Thing *thing = create_creature(&pos, (ThingModel)model, p);
            if (thing_is_invalid(thing))
                continue;
            set_creature_level(thing, (CrtrExpLevel)(i % 5));
            made++;
        }
        player_add_offmap_gold(p, 20000); /* (their wages; off the map, as the game counts gold not in a Treasure Room) */
        SYNCLOG("World: migration test: a bigger dungeon: 5 rooms south-west of the Heart and %d creatures (levels 1-5)", made);
    }
    /* four Imps at the least */
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    for (int n = dungeon_invalid(dungeon) ? 4 : dungeon->num_active_diggers; n < 4; n++)
    {
        struct Coord3d pos = heart->mappos;
        pos.x.val += 2 * COORD_PER_SLB;
        pos.z.val = 0;
        create_owned_special_digger(pos.x.val, pos.y.val, p);
    }
    SYNCLOG("World: migration test: player %d's Heart at slab (%d,%d); the new home made at (%d,%d), fits %d (%d slabs not "
        "loaded, left alone)", (int)p, (int)hx, (int)hy, (int)tx, (int)ty, (int)wm_home_fits(p, tx, ty), skipped);
    if (raiders > 0)
    {
        raid_p = p;
        raid_x = hx + distance / 2;
        raid_y = walled ? hy - 3 : hy - 8;
        SYNCLOG("World: migration test: %ld hero raiders wait %s north of the tunnel's middle, slab (%d,%d)", raiders,
            walled ? "3 slabs (behind a fortified wall)" : "8 slabs", (int)raid_x, (int)raid_y);
    }
    if (prep_only)
    {
        tp = -1; /* (the ground only: the player migrates by hand) */
        SYNCLOG("World: migration test: the ground is ready, nothing ordered (KFX_WORLD_MIGRATEPREP)");
        return;
    }
    world_migrate_packet(p, 1, slab_subtile_center(tx), slab_subtile_center(ty));
}

/******************************************************************************/
/* MIG1 (an extra of the world save): each keeper's migration. The Imps are found again after a load (their indexes
   are not kept): the Heart is down until they are at their corners again. */
size_t world_migrate_save_size(void)
{
    uint32_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if ((wm[p].state != WMig_None) || wm[p].moved)
            n++;
    return (n == 0) ? 0 : (4 + n * 8 * 4);
}

void world_migrate_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *at = buf + 4;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if ((wm[p].state == WMig_None) && !wm[p].moved)
            continue;
        const int32_t v[8] = {(int32_t)p, (int32_t)wm[p].state | ((int32_t)wm[p].moved << 8), wm[p].to_x, wm[p].to_y,
            wm[p].from_x, wm[p].from_y, wm[p].from_z, 0};
        memcpy(at, v, sizeof(v));
        at += sizeof(v);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_migrate_load(const uint8_t *buf, size_t len)
{
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if (n > (len - 4) / 32) /* (no overflow on 32-bit builds: audit 2026-11-21) */
        return false;
    for (uint32_t k = 0; k < n; k++)
    {
        int32_t v[8];
        memcpy(v, buf + 4 + k * 32, sizeof(v));
        if ((v[0] < 0) || (v[0] >= PLAYERS_MAX))
            continue;
        struct WorldMigration *m = &wm[v[0]];
        memset(m, 0, sizeof(*m));
        m->state = (uint8_t)(v[1] & 0xFF);
        if (m->state > WMig_Returning)
            m->state = WMig_None;
        m->moved = (uint8_t)((v[1] >> 8) & 1);
        m->to_x = v[2];
        m->to_y = v[3];
        m->from_x = v[4];
        m->from_y = v[5];
        m->from_z = v[6];
    }
    return true;
}

void world_migrate_reset(void)
{
    memset(wm, 0, sizeof(wm));
    memset(wm_hop, 0, sizeof(wm_hop));
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        wm_path_clear(p);
    wm_armed = false;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif

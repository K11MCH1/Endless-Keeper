/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rivals.c
 *     Endless world: Rivals with Ambitions (2026-11-13; see world_rivals.h).
 * @par Purpose:
 *     Decisions (the user was away; the recommended choice each time):
 *     - How many at once: WORLD_RIVALS_MAX (8) in the world, two aimed at any one keeper; a rival woken beyond that
 *       dozes off as before.
 *     - Which ambition: by the rival's place in the world (its generator structure), so a rival keeps its character.
 *     - The Hoarder: every two minutes its computer digs a tunnel to the farthest Treasury of the keeper who woke it and
 *       attacks there (the computer's own attack: it sends its usual share of creatures).
 *     - The Portal-grabber: it digs to the neutral portal nearest the keeper's heart (within 80 slabs) and claims it;
 *       then the next one.
 *     - The Warlord: a day's countdown (shown on its marker); then it digs to the keeper's heart and comes with every
 *       creature it has; then the countdown starts again.
 *     - It is aimed only at the keeper who woke it (the human whose creatures or ground came within its reach first),
 *       and ends when that keeper has no heart any more, or when the rival is destroyed or let go.
 *     - Its marker: on the full-screen map of the keeper it is aimed at, a label at its heart ("Hoarder", "Portal-
 *       grabber", "Warlord 12:34"); the label lives in the game state, so a joined player sees it.
 *     Saved: in the game state (game.world_rivals) and in XTR2 as RIV1.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_rivals.h"
#include "world_stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "config_compp.h"
#include "config_terrain.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_computer.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "post_inc.h"

enum RvKind { RvK_None = 0, RvK_Hoarder, RvK_PortalGrabber, RvK_Warlord };
static const char *const rv_names[4] = {"", "Hoarder", "Portal-grabber", "Warlord"};
#define RV_PER_TARGET 2
#define RV_HOARD_EVERY (2 * 60 * 20)
#define RV_GRAB_EVERY (60 * 20)
#define RV_WAR_COUNTDOWN (20 * 60 * 20)     /* a day of the world (12 + 8 minutes) */
#define RV_PORTAL_REACH 80

/* game.world_rivals[i]: kind (0 free), keeper index, the slot it plays as, the keeper it is aimed at, its heart (engine
   slabs, for the marker), the turn of its next move */

static long rv_dist2(MapSlabCoord ax, MapSlabCoord ay, MapSlabCoord bx, MapSlabCoord by)
{
    return (long)(ax - bx) * (ax - bx) + (long)(ay - by) * (ay - by);
}

TbBool world_rivals_holds(int k)
{
    if (!world_mode_active())
        return false;
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
        if ((game.world_rivals[i].kind != RvK_None) && (game.world_rivals[i].keeper == k))
            return true;
    return false;
}

void world_rivals_keeper_gone(int k)
{
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
        if ((game.world_rivals[i].kind != RvK_None) && (game.world_rivals[i].keeper == k))
        {
            SYNCLOG("World: the %s (keeper %d) gives up its ambition: it is gone", rv_names[game.world_rivals[i].kind % 4], k);
            memset(&game.world_rivals[i], 0, sizeof(game.world_rivals[i]));
        }
}

void world_rivals_keeper_woke(int k, PlayerNumber slot, PlayerNumber waker, int64_t heart_wx, int64_t heart_wy)
{
    if (!world_mode_active() || (waker < 0) || (waker >= PLAYERS_MAX) || !world_player_is_human(waker) || world_rivals_holds(k))
        return;
    int free_i = -1, on_target = 0;
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
    {
        if (game.world_rivals[i].kind == RvK_None)
        {
            if (free_i < 0)
                free_i = i;
        } else if (game.world_rivals[i].target == waker)
            on_target++;
    }
    if ((free_i < 0) || (on_target >= RV_PER_TARGET))
    {
        SYNCLOG("World: keeper %d wakes with no ambition (%s)", k, (free_i < 0) ? "enough rivals hold one" :
            "two rivals are after that keeper already");
        return;
    }
    MapSlabCoord hx, hy;
    if (!world_engine_slab(heart_wx, heart_wy, &hx, &hy))
        return;
    const uint64_t h = (uint64_t)(heart_wx * 0x9E3779B1LL) ^ (uint64_t)(heart_wy * 0x85EBCA77LL) ^ world_seed();
    struct WorldRival *rv = &game.world_rivals[free_i];
    memset(rv, 0, sizeof(*rv));
    rv->kind = (uint8_t)(1 + (h % 3));
    {
        /* test switch KFX_WORLD_RIVALKIND=1|2|3: every rival takes that ambition (Hoarder, Portal-grabber, Warlord) */
        const char *v = getenv("KFX_WORLD_RIVALKIND");
        if ((v != NULL) && (atoi(v) >= RvK_Hoarder) && (atoi(v) <= RvK_Warlord))
            rv->kind = (uint8_t)atoi(v);
    }
    rv->keeper = (int16_t)k;
    rv->slot = (int8_t)slot;
    rv->target = (int8_t)waker;
    rv->hx = hx;
    rv->hy = hy;
    rv->next = game.play_gameturn + ((rv->kind == RvK_Warlord) ? RV_WAR_COUNTDOWN : 200);
    SYNCLOG("World: keeper %d (player %d) wakes with an ambition: the %s, against player %d", k, (int)slot,
        rv_names[rv->kind], (int)waker);
}

/** The computer's own attack process (its completion sends the creatures), or -1. */
static int rv_attack_process(struct Computer2 *comp)
{
    for (int i = 0; i < COMPUTER_PROCESSES_COUNT; i++)
    {
        if (flag_is_set(comp->processes[i].flags, ComProc_ListEnd))
            break;
        if (comp->processes[i].func_complete == 14) /* completed_attack1 */
            return i;
    }
    return -1;
}

static struct Coord3d rv_slab_pos(MapSlabCoord x, MapSlabCoord y)
{
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(x));
    pos.y.val = subtile_coord_center(slab_subtile_center(y));
    pos.z.val = 0;
    return pos;
}

static void rv_act(struct WorldRival *rv)
{
    struct Computer2 *comp = get_computer_player(rv->slot);
    struct Thing *heart = find_players_dungeon_heart(rv->slot);
    struct Thing *target_heart = find_players_dungeon_heart(rv->target);
    if ((comp == NULL) || !thing_exists(heart) || !thing_exists(target_heart))
        return;
    const MapSlabCoord thx = subtile_slab(target_heart->mappos.x.stl.num), thy = subtile_slab(target_heart->mappos.y.stl.num);
    const struct Coord3d start = heart->mappos;
    switch (rv->kind)
    {
    case RvK_Hoarder:
    {
        /* the Treasury of the target farthest from its heart */
        struct Room *best = NULL;
        long best_d = -1;
        for (int i = 1; i < ROOMS_COUNT; i++)
        {
            struct Room *room = room_get(i);
            if (!room_exists(room) || (room->owner != rv->target) || !room_role_matches(room->kind, RoRoF_GoldStorage))
                continue;
            const long d = rv_dist2(slb_num_decode_x(room->slabs_list), slb_num_decode_y(room->slabs_list), thx, thy);
            if (d > best_d)
            {
                best_d = d;
                best = room;
            }
        }
        const int cp = rv_attack_process(comp);
        if ((best == NULL) || (cp < 0))
        {
            rv->next = game.play_gameturn + RV_HOARD_EVERY / 4; /* no Treasury yet (or no attack process): look again later */
            return;
        }
        const struct Coord3d end = rv_slab_pos(slb_num_decode_x(best->slabs_list), slb_num_decode_y(best->slabs_list));
        if (create_task_dig_to_attack(comp, start, end, rv->target, cp))
            SYNCLOG("World: the Hoarder (player %d) tunnels to player %d's Treasury at slab (%d,%d)", (int)rv->slot,
                (int)rv->target, (int)end.x.stl.num / 3, (int)end.y.stl.num / 3);
        rv->next = game.play_gameturn + RV_HOARD_EVERY;
        break;
    }
    case RvK_PortalGrabber:
    {
        struct Room *best = NULL;
        long best_d = (long)RV_PORTAL_REACH * RV_PORTAL_REACH + 1;
        for (int i = 1; i < ROOMS_COUNT; i++)
        {
            struct Room *room = room_get(i);
            if (!room_exists(room) || (room->owner != game.neutral_player_num) ||
                !room_role_matches(room->kind, RoRoF_CrPoolSpawn))
                continue;
            const long d = rv_dist2(slb_num_decode_x(room->slabs_list), slb_num_decode_y(room->slabs_list), thx, thy);
            if (d < best_d)
            {
                best_d = d;
                best = room;
            }
        }
        if (best == NULL)
        {
            rv->next = game.play_gameturn + RV_GRAB_EVERY;
            return;
        }
        const struct Coord3d end = rv_slab_pos(slb_num_decode_x(best->slabs_list), slb_num_decode_y(best->slabs_list));
        const int cp = rv_attack_process(comp);
        if ((cp >= 0) && create_task_dig_to_entrance(comp, start, end, cp, best->index))
            SYNCLOG("World: the Portal-grabber (player %d) digs to the neutral portal at slab (%d,%d), %ld slabs from"
                " player %d's heart", (int)rv->slot, (int)end.x.stl.num / 3, (int)end.y.stl.num / 3,
                (long)sqrt((double)best_d), (int)rv->target);
        rv->next = game.play_gameturn + RV_GRAB_EVERY * 3;
        break;
    }
    case RvK_Warlord:
    {
        const int cp = rv_attack_process(comp);
        if (cp < 0)
            return;
        comp->processes[cp].process_configuration_value_2 = 100; /* everything it has */
        const struct Coord3d end = target_heart->mappos;
        if (create_task_dig_to_attack(comp, start, end, rv->target, cp))
            SYNCLOG("World: the Warlord (player %d) comes for player %d's heart with everything", (int)rv->slot,
                (int)rv->target);
        rv->next = game.play_gameturn + RV_WAR_COUNTDOWN;
        break;
    }
    default:
        break;
    }
}

void world_rivals_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % 32) != 19))
        return;
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
    {
        struct WorldRival *rv = &game.world_rivals[i];
        if (rv->kind == RvK_None)
            continue;
        struct PlayerInfo *player = get_player(rv->slot);
        if ((rv->slot < 0) || !player_exists(player) || (player->victory_state == VicS_LostLevel) ||
            !world_player_is_human(rv->target) || !thing_exists(find_players_dungeon_heart(rv->target)))
        {
            SYNCLOG("World: the %s (player %d) gives up its ambition", rv_names[rv->kind % 4], (int)rv->slot);
            memset(rv, 0, sizeof(*rv));
            continue;
        }
        struct Thing *heart = find_players_dungeon_heart(rv->slot);
        if (thing_exists(heart))
        {
            rv->hx = subtile_slab(heart->mappos.x.stl.num);
            rv->hy = subtile_slab(heart->mappos.y.stl.num);
        }
        if ((GameTurnDelta)(game.play_gameturn - rv->next) >= 0)
            rv_act(rv);
    }
}

TbBool world_rivals_map_label(int i, MapSlabCoord *ex, MapSlabCoord *ey, char *text, size_t size)
{
    if (!world_mode_active() || (i < 0) || (i >= WORLD_RIVALS_MAX))
        return false;
    const struct WorldRival *rv = &game.world_rivals[i];
    if ((rv->kind == RvK_None) || (rv->target != my_player_number))
        return false;
    *ex = rv->hx;
    *ey = rv->hy;
    if (rv->kind == RvK_Warlord)
    {
        const long s = max(0L, (long)(GameTurnDelta)(rv->next - game.play_gameturn)) / 20;
        snprintf(text, size, "Warlord %ld:%02ld", s / 60, s % 60);
    } else
        snprintf(text, size, "%s", rv_names[rv->kind % 4]);
    return true;
}

/* Saving (XTR2 record RIV1): the rivals as they are, turns as how long until their next move */
#pragma pack(1)
struct RvSaved {
    uint8_t kind;
    int8_t slot, target;
    int16_t keeper;
    int32_t hx, hy;
    int32_t next_in;
};
#pragma pack()

void world_rivals_reset(void)
{
    memset(game.world_rivals, 0, sizeof(game.world_rivals));
}

size_t world_rivals_save_size(void)
{
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
        if (game.world_rivals[i].kind != RvK_None)
            return WORLD_RIVALS_MAX * sizeof(struct RvSaved);
    return 0;
}

void world_rivals_save(uint8_t *buf)
{
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
    {
        const struct WorldRival *rv = &game.world_rivals[i];
        struct RvSaved s;
        memset(&s, 0, sizeof(s));
        s.kind = rv->kind;
        s.slot = rv->slot;
        s.target = rv->target;
        s.keeper = rv->keeper;
        s.hx = rv->hx;
        s.hy = rv->hy;
        s.next_in = (int32_t)(GameTurnDelta)(rv->next - game.play_gameturn);
        memcpy(buf + (size_t)i * sizeof(s), &s, sizeof(s));
    }
}

TbBool world_rivals_load(const uint8_t *buf, size_t len)
{
    world_rivals_reset();
    if (len < WORLD_RIVALS_MAX * sizeof(struct RvSaved))
        return false;
    for (int i = 0; i < WORLD_RIVALS_MAX; i++)
    {
        struct RvSaved s;
        memcpy(&s, buf + (size_t)i * sizeof(s), sizeof(s));
        struct WorldRival *rv = &game.world_rivals[i];
        rv->kind = (s.kind <= RvK_Warlord) ? s.kind : RvK_None;
        rv->slot = s.slot;
        rv->target = s.target;
        rv->keeper = s.keeper;
        rv->hx = s.hx;
        rv->hy = s.hy;
        rv->next = game.play_gameturn + (GameTurn)(GameTurnDelta)s.next_in;
    }
    return true;
}

/******************************************************************************/
// Endless Keeper: A Prisoner's Fate.
/******************************************************************************/
/** @file world_prisoners.c
 *     A captive hero gets a real choice beside converting it, starving it into a Skeleton or sacrificing it. Its
 *     jailer picks it up and drops it:
 *     - on their own Treasury Room: Ransom. Gold now (three times its wage, at least 100), and the hero comes back
 *       with that keeper's next raid, a level higher;
 *     - on their own Library: Interrogation. It gives up the nearest hero stronghold (direction and distance; its
 *       heart and portal are revealed when they are on the map), then dies under the questioning.
 *     The heroes waiting to come back are the world save's "PRF1" extra.
 */
/******************************************************************************/
#include "pre_inc.h"
#include <limits.h>
#include "world_prisoners.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_groups.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "map_data.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "room_data.h"
#include "server_admin.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_stats.h"
#include "player_instances.h"
#include "thing_factory.h"
#include "creature_states_hero.h"
#include "map_blocks.h"
#include "thing_navigate.h"
#include "world_outpost.h"
#include "world_stream.h"
#include "world_wild.h"
#include "world_caretaker.h"
#include "creature_states_prisn.h"
#include "power_hand.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#define PF_QUEUE_MAX 64
#define PF_RANSOM_WAGES 3
#define PF_RANSOM_MIN 100
#define PF_RETURN_PER_RAID 4
#define PF_LEVEL_MAX 100
#define PF_SAVE_MAGIC 0x31465250u /* "PRF1" */
#define PF_GANG_MAGIC 0x31474E47u /* "GNG1": the chain gang, after the waiting heroes */
/* Chain Gangs: a captive hero dropped by its jailer on their own Workshop is put in irons and works for them as their
   imps do (digging, mining, carrying), unconverted. It works only while one of its keeper's own creatures stands
   guard near it; without one it slacks (a quarter of its speed) and is ten times as likely to slip its irons. Loose,
   it is a hero again. The keeper's Prisons set the gang's size (their capacity); a gang takes no wage. */
#define PF_GANG_MAX 64
#define PF_GANG_CHECK 32          /* the gang is looked at one turn in this many */
#define PF_GUARD_SLABS 8          /* a guard of its keeper within this many slabs keeps it working */
#define PF_LOOSE_WATCHED 400      /* a check's chance of slipping its irons, one in this many: guarded ... */
#define PF_LOOSE_SLACKING 40      /* ... and unguarded */

#pragma pack(1)
struct PfReturn {
    uint16_t model;     /* 0: free */
    uint8_t owner;      /* the keeper it was ransomed by: it comes back with that keeper's raid */
    uint8_t level;      /* the level it comes back at (1-based) */
};
#pragma pack()

static struct PfReturn pf_queue[PF_QUEUE_MAX];
static void pf_tell(PlayerNumber plyr_idx, const char *text);

#pragma pack(1)
struct PfGang {
    uint16_t thing;      /* 0: free */
    uint32_t made_turn;  /* the thing's creation turn (a reused index is not the same captive) */
    uint16_t model;
    uint8_t owner;       /* its jailer */
    uint8_t slack;       /* unguarded at the last check */
};
#pragma pack()
static struct PfGang pf_gang[PF_GANG_MAX];
int world_prisoners_gang_size = 0; /* (read by the digger checks before they look the thing up) */

/* each creature's gang slot (1 + the slot; 0: none), by its control slot: rebuilt by pf_gang_count() after every
   change (audit 2026-11-13: the digger checks scanned all 64 slots on each call while any gang existed) */
static uint8_t pf_slot_of[CREATURES_COUNT];

static struct PfGang *pf_gang_of(const struct Thing *thing)
{
    const CctrlIndex c = thing->ccontrol_idx;
    if ((c <= 0) || (c >= CREATURES_COUNT) || (pf_slot_of[c] == 0))
        return NULL;
    struct PfGang *g = &pf_gang[pf_slot_of[c] - 1];
    return ((g->thing == thing->index) && (g->made_turn == (uint32_t)thing->creation_turn)) ? g : NULL;
}

TbBool world_prisoners_chained(const struct Thing *thing)
{
    return (world_prisoners_gang_size > 0) && (thing->class_id == TCls_Creature) && (pf_gang_of(thing) != NULL);
}

static void pf_gang_count(void)
{
    memset(pf_slot_of, 0, sizeof(pf_slot_of));
    for (int s = 0; s < PF_GANG_MAX; s++)
    {
        const struct Thing *gt = (pf_gang[s].thing != 0) ? thing_get(pf_gang[s].thing) : INVALID_THING;
        if (thing_exists(gt) && (gt->class_id == TCls_Creature) && (gt->ccontrol_idx > 0) && (gt->ccontrol_idx < CREATURES_COUNT))
            pf_slot_of[gt->ccontrol_idx] = (uint8_t)(s + 1);
    }
    int n = 0;
    for (int i = 0; i < PF_GANG_MAX; i++)
        if (pf_gang[i].thing != 0)
            n++;
    world_prisoners_gang_size = n;
}

/** The captives a keeper's Prisons can hold: the gang's size. */
static int pf_prison_capacity(PlayerNumber plyr_idx)
{
    const struct Dungeon *dungeon = get_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return 0;
    int cap = 0;
    for (RoomKind rkind = 0; rkind < game.conf.slab_conf.room_types_count; rkind++)
    {
        if (!room_role_matches(rkind, RoRoF_Prison))
            continue;
        long i = dungeon->room_list_start[rkind];
        for (int k = 0; (i != 0) && (k < ROOMS_COUNT); k++)
        {
            const struct Room *room = room_get(i);
            if (room_is_invalid(room))
                break;
            cap += room->total_capacity;
            i = room->next_of_owner;
        }
    }
    return cap;
}

static int pf_gang_of_keeper(PlayerNumber plyr_idx)
{
    int n = 0;
    for (int i = 0; i < PF_GANG_MAX; i++)
        if ((pf_gang[i].thing != 0) && (pf_gang[i].owner == plyr_idx))
            n++;
    return n;
}

/** Returns false when the gang is full: the captive stays a captive (the drop goes on as usual). */
static TbBool pf_chain(PlayerNumber jailer, struct Thing *thing)
{
    const int cap = pf_prison_capacity(jailer), have = pf_gang_of_keeper(jailer);
    int slot = -1;
    for (int i = 0; (i < PF_GANG_MAX) && (slot < 0); i++)
        if (pf_gang[i].thing == 0)
            slot = i;
    char text[160];
    if ((have >= cap) || (slot < 0))
    {
        SYNCLOG("Prisoners: Chain Gangs: player %d can't chain a %s: the gang is %d, the Prisons hold %d", (int)jailer,
            thing_model_name(thing), have, cap);
        snprintf(text, sizeof(text), "Your Prisons can send out a gang of %d: the %s stays a captive.", cap,
            creature_code_name(thing->model));
        pf_tell(jailer, text);
        return false;
    }
    pf_gang[slot].thing = thing->index;
    pf_gang[slot].made_turn = (uint32_t)thing->creation_turn;
    pf_gang[slot].model = (uint16_t)thing->model;
    pf_gang[slot].owner = (uint8_t)jailer;
    pf_gang[slot].slack = 0;
    pf_gang_count();
    change_creature_owner(thing, jailer); /* (marked first: it goes on its keeper's list of diggers) */
    if (creature_is_being_unconscious(thing))
        thing->health = max(thing->health, (HitPoints)1);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    cctrl->max_speed = calculate_correct_creature_maxspeed(thing);
    SYNCLOG("Prisoners: Chain Gangs: player %d put a %s of level %d in irons (gang %d of %d)", (int)jailer,
        thing_model_name(thing), cctrl->exp_level + 1, have + 1, cap);
    snprintf(text, sizeof(text), "The %s is put in irons and works for you. Keep a guard near it.", creature_code_name(thing->model));
    pf_tell(jailer, text);
    return true;
}

/** A captive out of its irons is a hero again. */
/* Captives out of their irons, making for the nearest land the heroes hold (the page); not saved (after a load a
   loose captive is a plain hero) */
#define PF_FREE_MAX 16
#define PF_FREE_TURNS 2400      /* it leaves the map after this long on its way, wherever it is */
#define PF_FREE_ARRIVED 4       /* slabs from the heroes' heart: arrived, it goes in (leaves the map) */
struct PfFree { ThingIndex thing; int32_t born; int64_t tx, ty; GameTurn since; };
static struct PfFree pf_free[PF_FREE_MAX];

static void pf_free_add(struct Thing *thing)
{
    int64_t wx, wy, x0, y0, ex, ey, hx, hy, gx, gy;
    int odx, ody;
    world_slab_to_world(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num), &wx, &wy);
    if (!world_outpost_nearest(wx, wy, &x0, &y0, &ex, &ey, &odx, &ody, &hx, &hy, &gx, &gy))
    {
        SYNCLOG("Prisoners: Chain Gangs: no heroes' land near: the loose %s fights where it is", thing_model_name(thing));
        return;
    }
    for (int i = 0; i < PF_FREE_MAX; i++)
        if (pf_free[i].thing == 0)
        {
            pf_free[i].thing = thing->index;
            pf_free[i].born = thing->creation_turn;
            pf_free[i].tx = hx;
            pf_free[i].ty = hy;
            pf_free[i].since = game.play_gameturn;
            SYNCLOG("Prisoners: Chain Gangs: the loose %s makes for the heroes' land at world slab (%lld,%lld)", thing_model_name(thing),
                (long long)hx, (long long)hy);
            return;
        }
}

/** Every 32 turns: each loose captive walks on toward the heroes' land (a fight first, if one comes), and leaves the
 *  map once there or after PF_FREE_TURNS. */
static void pf_free_turn(void)
{
    for (int i = 0; i < PF_FREE_MAX; i++)
    {
        struct PfFree *f = &pf_free[i];
        if (f->thing == 0)
            continue;
        struct Thing *thing = thing_get(f->thing);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->creation_turn != f->born) ||
            (thing->owner != PLAYER_GOOD) || (thing->health < 0))
        {
            memset(f, 0, sizeof(*f));
            continue;
        }
        if (creature_is_kept_in_custody(thing) || creature_is_being_unconscious(thing) || thing_is_picked_up(thing) ||
            thing_is_dragged_or_pulled(thing))
        {
            memset(f, 0, sizeof(*f)); /* caught again: it is a captive like any other */
            continue;
        }
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (cctrl->combat_flags != 0)
            continue; /* a fight first */
        MapSlabCoord sx, sy;
        const TbBool on_map = world_engine_slab(f->tx, f->ty, &sx, &sy);
        const MapSlabCoord tx = subtile_slab(thing->mappos.x.stl.num), ty = subtile_slab(thing->mappos.y.stl.num);
        const long d = on_map ? max(labs((long)sx - tx), labs((long)sy - ty)) : LONG_MAX;
        if ((d <= PF_FREE_ARRIVED) || ((GameTurnDelta)(game.play_gameturn - f->since) > PF_FREE_TURNS))
        {
            SYNCLOG("Prisoners: Chain Gangs: the loose %s %s", thing_model_name(thing), (d <= PF_FREE_ARRIVED) ?
                "reaches the heroes' land and is gone" : "is gone into the dark");
            memset(f, 0, sizeof(*f));
            world_remove_thing_quietly(thing);
            continue;
        }
        if (get_creature_state_besides_interruptions(thing) == CrSt_MoveToPosition)
            continue; /* on its way */
        struct Coord3d pos;
        if (on_map)
        {
            pos.x.val = subtile_coord_center(slab_subtile_center(sx));
            pos.y.val = subtile_coord_center(slab_subtile_center(sy));
        } else
        {
            /* the heroes' land is off the loaded map: 15 slabs its way */
            int64_t wx, wy;
            world_slab_to_world(tx, ty, &wx, &wy);
            const int64_t dx = f->tx - wx, dy = f->ty - wy;
            const int64_t m = max(llabs(dx), llabs(dy));
            const MapSlabCoord nx = tx + (MapSlabCoord)((m > 0) ? dx * 15 / m : 0), ny = ty + (MapSlabCoord)((m > 0) ? dy * 15 / m : 0);
            pos.x.val = subtile_coord_center(slab_subtile_center(nx));
            pos.y.val = subtile_coord_center(slab_subtile_center(ny));
        }
        pos.z.val = 0;
        setup_person_move_to_coord(thing, &pos, NavRtF_Default);
    }
}

static void pf_loose(struct PfGang *gang, struct Thing *thing)
{
    const PlayerNumber jailer = gang->owner;
    change_creature_owner(thing, PLAYER_GOOD); /* (still marked: it leaves its keeper's list of diggers) */
    memset(gang, 0, sizeof(*gang));
    pf_gang_count();
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    cctrl->max_speed = calculate_correct_creature_maxspeed(thing);
    cctrl->party.objective = CHeroTsk_AttackEnemies; /* (it fights what stops it on its way) */
    SYNCLOG("Prisoners: Chain Gangs: a %s of player %d's gang slipped its irons", thing_model_name(thing), (int)jailer);
    pf_free_add(thing); /* and makes for the nearest land the heroes hold (the page) */
    char text[160];
    snprintf(text, sizeof(text), "A %s of your gang has slipped its irons!", creature_code_name(thing->model));
    pf_tell(jailer, text);
}

/** A guard: one of the keeper's own creatures (not a digger, not in irons) near the captive. */
static TbBool pf_guarded(const struct Thing *thing, PlayerNumber owner)
{
    const struct Dungeon *dungeon = get_dungeon(owner);
    if (dungeon_invalid(dungeon))
        return false;
    long i = dungeon->creatr_list_start;
    for (int k = 0; (i != 0) && (k < CREATURES_COUNT); k++)
    {
        const struct Thing *other = thing_get(i);
        const struct CreatureControl *octrl = creature_control_get_from_thing(other);
        if (thing_is_invalid(other) || creature_control_invalid(octrl))
            break;
        i = octrl->players_next_creature_idx;
        if ((other == thing) || (other->health < 0) || thing_is_picked_up(other) || creature_is_being_unconscious(other) ||
            world_prisoners_chained(other))
            continue;
        if ((abs(coord_slab(other->mappos.x.val) - coord_slab(thing->mappos.x.val)) <= PF_GUARD_SLABS) &&
            (abs(coord_slab(other->mappos.y.val) - coord_slab(thing->mappos.y.val)) <= PF_GUARD_SLABS))
            return true;
    }
    return false;
}

static void pf_gang_turn(void)
{
    if ((world_prisoners_gang_size == 0) || ((game.play_gameturn % PF_GANG_CHECK) != 5) ||
        (netstate_is_client() && netstate_thin_mode()))
        return;
    for (int g = 0; g < PF_GANG_MAX; g++)
    {
        struct PfGang *gang = &pf_gang[g];
        if (gang->thing == 0)
            continue;
        struct Thing *thing = thing_get(gang->thing);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || ((uint32_t)thing->creation_turn != gang->made_turn) ||
            (thing->owner != gang->owner) || (thing->health < 0))
        {
            memset(gang, 0, sizeof(*gang)); /* (it died, or is gone some other way) */
            pf_gang_count();
            continue;
        }
        if (thing_is_picked_up(thing))
            continue;
        const uint8_t slack = pf_guarded(thing, gang->owner) ? 0 : 1;
        if (slack != gang->slack)
        {
            gang->slack = slack;
            creature_control_get_from_thing(thing)->max_speed = calculate_correct_creature_maxspeed(thing);
            SYNCLOG("Prisoners: Chain Gangs: a %s of player %d's gang is %s", thing_model_name(thing), (int)gang->owner,
                slack ? "unguarded and slacks" : "guarded and works");
        }
        /* (while its keeper is away and the caretaker keeps the dungeon, nobody slips: the dungeon waits for its keeper) */
        if (!world_caretaker_active(gang->owner) && (GAME_RANDOM(slack ? PF_LOOSE_SLACKING : PF_LOOSE_WATCHED) == 0))
            pf_loose(gang, thing);
    }
}

uint8_t world_prisoners_freeze(const struct Thing *thing)
{
    if (world_prisoners_gang_size == 0)
        return 0;
    struct PfGang *gang = pf_gang_of(thing);
    if (gang == NULL)
        return 0;
    const uint8_t flags = (uint8_t)(1 | (gang->slack ? 2 : 0));
    SYNCLOG("Prisoners: Chain Gangs: a %s of player %d's gang freezes in its irons", thing_model_name(thing), (int)gang->owner);
    memset(gang, 0, sizeof(*gang)); /* (the thing goes; the frozen record keeps the irons) */
    pf_gang_count();
    return flags;
}

void world_prisoners_thaw(struct Thing *thing, uint8_t flags)
{
    if (!(flags & 1) || !thing_exists(thing) || (thing->class_id != TCls_Creature))
        return;
    int slot = -1;
    for (int i = 0; (i < PF_GANG_MAX) && (slot < 0); i++)
        if (pf_gang[i].thing == 0)
            slot = i;
    if (slot < 0)
        return; /* (no room in the table: it stays its keeper's creature, out of the gang) */
    pf_gang[slot].thing = thing->index;
    pf_gang[slot].made_turn = (uint32_t)thing->creation_turn;
    pf_gang[slot].model = (uint16_t)thing->model;
    pf_gang[slot].owner = (uint8_t)thing->owner;
    pf_gang[slot].slack = (flags & 2) ? 1 : 0;
    pf_gang_count();
    /* it was made as an ordinary creature of its keeper: file it with the diggers again */
    if ((thing->alloc_flags & TAlF_InDungeonList) != 0)
        remove_first_creature(thing);
    set_first_creature(thing);
    creature_control_get_from_thing(thing)->max_speed = calculate_correct_creature_maxspeed(thing);
    SYNCLOG("Prisoners: Chain Gangs: a %s of player %d's gang thaws in its irons (index %d)", thing_model_name(thing),
        (int)thing->owner, (int)thing->index);
}

long world_prisoners_speed(const struct Thing *thing, long speed)
{
    if (world_prisoners_gang_size == 0)
        return speed;
    const struct PfGang *gang = pf_gang_of(thing);
    return ((gang != NULL) && gang->slack) ? speed / 4 : speed;
}

GoldAmount world_prisoners_pay(const struct Thing *thing, GoldAmount pay)
{
    return ((world_prisoners_gang_size > 0) && (pf_gang_of(thing) != NULL)) ? 0 : pay;
}

static void pf_tell(PlayerNumber plyr_idx, const char *text)
{
    if (is_my_player_number(plyr_idx) && !netstate_dedicated()) /* (a dedicated server's own number is a joined player's) */
    {
        message_add(MsgType_Blank, 0, text);
        return;
    }
    if (!netstate_is_server() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    const NetUserId u = get_player_primary_user(get_player(plyr_idx));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static const char *pf_direction(int64_t dx, int64_t dy)
{
    static const char *const names[8] = {"east", "south-east", "south", "south-west", "west", "north-west", "north",
        "north-east"};
    const double a = atan2((double)dy, (double)dx); /* y grows southwards */
    int k = (int)floor(a / (M_PI / 4) + 0.5);
    return names[(k + 8) % 8];
}

static void pf_ransom(PlayerNumber jailer, struct Thing *thing)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const int level = cctrl->exp_level + 1;
    const GoldAmount wage = compute_creature_max_pay(crconf->pay, cctrl->exp_level);
    const GoldAmount gold = max(wage * PF_RANSOM_WAGES, (GoldAmount)PF_RANSOM_MIN);
    player_add_offmap_gold(jailer, gold);
    int slot = -1;
    for (int i = 0; (i < PF_QUEUE_MAX) && (slot < 0); i++)
        if (pf_queue[i].model == 0)
            slot = i;
    if (slot >= 0)
    {
        pf_queue[slot].model = (uint16_t)thing->model;
        pf_queue[slot].owner = (uint8_t)jailer;
        pf_queue[slot].level = (uint8_t)min(level + 1, PF_LEVEL_MAX);
    }
    SYNCLOG("Prisoners: player %d ransomed a %s of level %d for %ld gold; %s", (int)jailer, thing_model_name(thing), level,
        (long)gold, (slot >= 0) ? "it comes back with the next raid, a level higher" : "the queue is full: it won't come back");
    char text[160];
    snprintf(text, sizeof(text), "The %s is ransomed for %ld gold. It will come back with a raid, a level higher.",
        creature_code_name(thing->model), (long)gold);
    pf_tell(jailer, text);
    create_effect(&thing->mappos, TngEff_Explosion3, jailer);
    delete_thing_structure(thing, 0);
}

static void pf_interrogate(PlayerNumber jailer, struct Thing *thing)
{
    int64_t wx = 0, wy = 0;
    world_slab_to_world(coord_slab(thing->mappos.x.val), coord_slab(thing->mappos.y.val), &wx, &wy);
    int64_t x0, y0, ex, ey, hx, hy, gx, gy;
    int odx, ody;
    char text[200];
    if (world_outpost_nearest(wx, wy, &x0, &y0, &ex, &ey, &odx, &ody, &hx, &hy, &gx, &gy))
    {
        const long dist = (long)floor(sqrt((double)(hx - wx) * (hx - wx) + (double)(hy - wy) * (hy - wy)) + 0.5);
        int revealed = 0;
        const int64_t spots[2][2] = {{hx, hy}, {gx, gy}};
        for (int s = 0; s < 2; s++)
        {
            MapSlabCoord sx, sy;
            if (world_engine_slab(spots[s][0], spots[s][1], &sx, &sy))
            {
                const int r = (s == 0) ? 4 : 2;
                reveal_map_area(jailer, slab_subtile(max(sx - r, 0), 0), slab_subtile(sx + r, 2), slab_subtile(max(sy - r, 0), 0),
                    slab_subtile(sy + r, 2));
                revealed++;
            }
        }
        SYNCLOG("Prisoners: player %d interrogated a %s: the nearest stronghold's heart is %ld slabs %s at world (%lld,%lld); "
            "%d of its heart and portal revealed", (int)jailer, thing_model_name(thing), dist, pf_direction(hx - wx, hy - wy),
            (long long)hx, (long long)hy, revealed);
        snprintf(text, sizeof(text), "Under questioning, the %s gives up a hero stronghold %ld slabs to the %s%s.",
            creature_code_name(thing->model), dist, pf_direction(hx - wx, hy - wy), (revealed > 0) ? ": see your map" : "");
    } else
    {
        SYNCLOG("Prisoners: player %d interrogated a %s: no stronghold near", (int)jailer, thing_model_name(thing));
        snprintf(text, sizeof(text), "Under questioning, the %s knows of nothing you haven't found.", creature_code_name(thing->model));
    }
    pf_tell(jailer, text);
    kill_creature(thing, INVALID_THING, jailer, CrDed_NoUnconscious); /* (it does not survive the questioning) */
}

TbBool world_prisoners_dropped(struct Dungeon *dungeon, struct Thing *thing)
{
    if (!world_mode_active() || dungeon_invalid(dungeon) || !thing_is_creature(thing) || (thing->owner != PLAYER_GOOD) ||
        world_prisoners_chained(thing) ||
        world_wild_creature(thing) || (thing->health < 0))
        return false;
    const PlayerNumber jailer = dungeon->owner;
    if (!world_player_is_human(jailer))
        return false;
    const struct Room *room = get_room_thing_is_on(thing);
    if (room_is_invalid(room) || (room->owner != jailer))
        return false;
    if (room_role_matches(room->kind, RoRoF_CratesManufctr))
        return pf_chain(jailer, thing);
    else if (room_role_matches(room->kind, RoRoF_GoldStorage))
        pf_ransom(jailer, thing);
    else if (room_role_matches(room->kind, RoRoF_Research))
        pf_interrogate(jailer, thing);
    else
        return false;
    return true;
}

int world_prisoners_join_raid(PlayerNumber plyr_idx, struct Thing *leader, MapSlabCoord sx, MapSlabCoord sy)
{
    if (!thing_exists(leader))
        return 0;
    int made = 0;
    for (int i = 0; (i < PF_QUEUE_MAX) && (made < PF_RETURN_PER_RAID); i++)
    {
        struct PfReturn *ret = &pf_queue[i];
        if ((ret->model == 0) || (ret->owner != plyr_idx))
            continue;
        if (!creature_count_below_map_limit(0))
            break;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(sx)) + (MapCoord)(((made % 3) - 1) * 64);
        pos.y.val = subtile_coord_center(slab_subtile_center(sy)) + (MapCoord)(((made / 3) - 1) * 64);
        pos.z.val = 0;
        struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, ret->model, PLAYER_GOOD);
        if (thing_is_invalid(thing))
            continue;
        /* as the raid's own heroes come: through the ceiling, then after the leader */
        thing->mappos.z.val = get_ceiling_height(&thing->mappos);
        create_effect(&thing->mappos, TngEff_CeilingBreach, thing->owner);
        initialise_thing_state(thing, CrSt_CreatureHeroEntering);
        set_flag(thing->rendering_flags, TRF_Invisible);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        cctrl->countdown = 40 + 8 * made;
        init_creature_level(thing, (CrtrExpLevel)(ret->level - 1));
        cctrl->party.original_objective = CHeroTsk_AttackEnemies; /* (marks raid heroes) */
        cctrl->party.objective = CHeroTsk_AttackEnemies;
        cctrl->party.target_plyr_idx = plyr_idx;
        add_creature_to_group(thing, leader);
        SYNCLOG("Prisoners: a ransomed %s comes back with player %d's raid at level %d", thing_model_name(thing), (int)plyr_idx,
            (int)ret->level);
        memset(ret, 0, sizeof(*ret));
        made++;
    }
    return made;
}

/** Test hook KFX_WORLD_PRISONTEST="T": at T a one-slab Treasury Room and a one-slab Library beside the local keeper's
 *  heart, and two Knights of level 5 as if carried out of the Prison: one dropped on each (the hand's drop calls the
 *  same function). Then the next raid should bring the first one back at level 6. "T,1": the rooms and a Prison are
 *  built the stock way, the Knights are put in the Prison (the stock arrival there), and at T+20 and T+30 each is
 *  taken in the keeper's hand and dropped on its room (the hand's own pickup and drop). */
static void pf_test_by_hand(long t)
{
    static ThingIndex knights[2];
    static MapSlabCoord rx[2], ry[2];
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 0)
    {
        static const char *const rooms[3] = {"TREASURE", "RESEARCH", "PRISON"};
        struct Room *made[3] = {NULL, NULL, NULL};
        for (int k = 0; k < 3; k++)
        {
            const MapSlabCoord x = hx - 2 + 2 * k, y = hy + 5;
            for (int dy = 3; dy <= 4; dy++)
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(hy + dy), my_player_number, 0);
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(y), my_player_number, 0);
            made[k] = place_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(x), slab_subtile_center(y));
            reveal_map_area(my_player_number, slab_subtile(x, 0), slab_subtile(x, 2), slab_subtile(hy + 3, 0), slab_subtile(y, 2));
            if (k < 2)
            {
                rx[k] = x;
                ry[k] = y;
            } else
            {
                /* a second Prison slab: one captive a slab */
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x + 1), slab_subtile_center(y), my_player_number, 0);
                place_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(x + 1), slab_subtile_center(y));
                reveal_map_area(my_player_number, slab_subtile(x + 1, 0), slab_subtile(x + 1, 2), slab_subtile(y, 0), slab_subtile(y, 2));
            }
        }
        for (int k = 0; k < 1; k++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(hx + 2));
            pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
            pos.z.val = get_floor_height_at(&pos);
            struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
            if (thing_is_invalid(knight))
                continue;
            init_creature_level(knight, 4);
            move_thing_in_map(knight, &pos);
            creature_arrived_at_prison(knight); /* (as when a captive is carried in) */
            knights[k] = knight->index;
        }
        SYNCLOG("Prisoners test: by hand: Treasury %s, Library %s, Prison %s; the Knights kept in the Prison: %d %d",
            room_is_invalid(made[0]) ? "MISSING" : "built", room_is_invalid(made[1]) ? "MISSING" : "built",
            room_is_invalid(made[2]) ? "MISSING" : "built", (int)creature_is_kept_in_prison(thing_get(knights[0])),
            (int)creature_is_kept_in_prison(thing_get(knights[1])));
        return;
    }
    if (t == 25)
    {
        /* the second captive, once the first has left the Prison */
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx + 2));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
        if (!thing_is_invalid(knight))
        {
            init_creature_level(knight, 4);
            move_thing_in_map(knight, &pos);
            creature_arrived_at_prison(knight);
            knights[1] = knight->index;
            SYNCLOG("Prisoners test: by hand: the second Knight kept in the Prison: %d", (int)creature_is_kept_in_prison(knight));
        }
        return;
    }
    if ((t != 20) && (t != 30))
        return;
    const int k = (t == 20) ? 0 : 1;
    struct Thing *knight = thing_get(knights[k]);
    if (!thing_exists(knight))
        return;
    struct PlayerInfo *player = get_player(my_player_number);
    const TbBool may = thing_is_pickable_by_hand(player, knight);
    if (!place_thing_in_power_hand(knight, my_player_number))
    {
        SYNCLOG("Prisoners test: by hand: the Knight can't be taken in the hand (pickable %d)", (int)may);
        return;
    }
    get_player_primary_user_state(player)->thing_under_hand = 0;
    const GoldAmount before = get_dungeon(my_player_number)->total_money_owned;
    const short res = dump_first_held_thing_on_map(my_player_number, slab_subtile_center(rx[k]), slab_subtile_center(ry[k]), true);
    SYNCLOG("Prisoners test: by hand: a Knight from the Prison (pickable by the hand %d) dropped on the %s: %d; it is %s; treasury %ld -> %ld",
        (int)may, (k == 0) ? "Treasury" : "Library", (int)res, (thing_exists(knight) && (knight->class_id == TCls_Creature) &&
        (knight->model == (ThingModel)get_id(creature_desc, "KNIGHT"))) ? "still here" : "gone", (long)before,
        (long)get_dungeon(my_player_number)->total_money_owned);
}
static void pf_test_hook(void)
{
    static long base = -2;
    static int by_hand = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_PRISONTEST");
        base = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%d", &base, &by_hand) >= 1))
            SYNCLOG("Prisoners test: KFX_WORLD_PRISONTEST at turn %ld%s", base, by_hand ? ", by hand" : "");
        else
            base = -1;
    }
    if (by_hand && (base >= 0) && ((long)game.play_gameturn >= base))
    {
        pf_test_by_hand((long)game.play_gameturn - base);
        return;
    }
    if ((base < 0) || ((long)game.play_gameturn != base))
        return;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    static const char *const rooms[2] = {"TREASURE", "RESEARCH"};
    static const SlabKind slabs[2] = {SlbT_TREASURE, SlbT_LIBRARY};
    for (int k = 0; k < 2; k++)
    {
        const MapSlabCoord rx = hx + ((k == 0) ? -1 : 1), ry = hy + 5;
        for (int dy = 3; dy <= 4; dy++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(rx), slab_subtile_center(hy + dy), my_player_number, 0);
        place_slab_type_on_map(slabs[k], slab_subtile_center(rx), slab_subtile_center(ry), my_player_number, 0);
        struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(rx),
            slab_subtile_center(ry));
        if (!room_is_invalid(room))
            do_room_integration(room);
        reveal_map_area(my_player_number, slab_subtile(rx, 0), slab_subtile(rx, 2), slab_subtile(hy + 3, 0), slab_subtile(ry, 2));
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(rx));
        pos.y.val = subtile_coord_center(slab_subtile_center(ry));
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
        if (thing_is_invalid(knight))
            continue;
        init_creature_level(knight, 4);
        move_thing_in_map(knight, &pos); /* (as a drop: on the room's slab, not moved to a free spot beside it) */
        const GoldAmount before = get_dungeon(my_player_number)->total_money_owned;
        const TbBool taken = world_prisoners_dropped(get_dungeon(my_player_number), knight);
        SYNCLOG("Prisoners test: %s room %s; a Knight dropped on it: %s; treasury %ld -> %ld", rooms[k],
            room_is_invalid(room) ? "MISSING" : "built", taken ? "taken" : "NOT taken", (long)before,
            (long)get_dungeon(my_player_number)->total_money_owned);
    }
}

/** Test hook KFX_WORLD_GANGTEST="T": at T a one-slab Prison and a one-slab Workshop beside the local keeper's heart,
 *  and a Tunneller and a Knight of level 3 dropped on the Workshop (the same function the hand's drop calls); a third
 *  dropped when the Prison is full is refused. T+300 logs the gang: owner, digger, speed, wage; then every one of the
 *  keeper's other creatures is moved far away so the gang is unguarded. */
static void pf_gang_test_hook(void)
{
    static long base = -2;
    static int freeze = 0; /* "T,1": the Tunneller is taken far off at T+20, its chunk frozen at T+60 and loaded at T+400 */
    static ThingIndex members[3];
    static MapSlabCoord fx, fy;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_GANGTEST");
        base = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%d", &base, &freeze) < 1))
            base = -1;
        if (base >= 0)
            SYNCLOG("Prisoners test: KFX_WORLD_GANGTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 0)
    {
        static const char *const rooms[2] = {"PRISON", "WORKSHOP"};
        static const SlabKind slabs[2] = {SlbT_PRISON, SlbT_WORKSHOP};
        for (int k = 0; k < 2; k++)
        {
            const MapSlabCoord rx = hx + ((k == 0) ? -1 : 1), ry = hy + 5;
            for (int dy = 3; dy <= 4; dy++)
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(rx), slab_subtile_center(hy + dy), my_player_number, 0);
            place_slab_type_on_map(slabs[k], slab_subtile_center(rx), slab_subtile_center(ry), my_player_number, 0);
            struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, rooms[k]), slab_subtile_center(rx),
                slab_subtile_center(ry));
            if (!room_is_invalid(room))
                do_room_integration(room);
            reveal_map_area(my_player_number, slab_subtile(rx, 0), slab_subtile(rx, 2), slab_subtile(hy + 3, 0), slab_subtile(ry, 2));
            SYNCLOG("Prisoners test: %s room %s, capacity %d", rooms[k], room_is_invalid(room) ? "MISSING" : "built",
                room_is_invalid(room) ? 0 : (int)room->total_capacity);
        }
        struct Coord3d gpos;
        gpos.x.val = subtile_coord_center(slab_subtile_center(hx + 1));
        gpos.y.val = subtile_coord_center(slab_subtile_center(hy + 4));
        gpos.z.val = get_floor_height_at(&gpos);
        struct Thing *guard = create_creature(&gpos, (ThingModel)get_id(creature_desc, "ORC"), my_player_number);
        SYNCLOG("Prisoners test: a guard %s beside the Workshop", thing_is_invalid(guard) ? "NOT made" : "made");
        static const char *const kinds[3] = {"TUNNELLER", "KNIGHT", "ARCHER"};
        for (int k = 0; k < 3; k++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(hx + 1));
            pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
            pos.z.val = get_floor_height_at(&pos);
            struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, kinds[k]), PLAYER_GOOD);
            if (thing_is_invalid(hero))
                continue;
            init_creature_level(hero, 2);
            move_thing_in_map(hero, &pos);
            members[k] = hero->index;
            const TbBool taken = world_prisoners_dropped(get_dungeon(my_player_number), hero);
            SYNCLOG("Prisoners test: a %s dropped on the Workshop: %s; owner now %d; chained %d", kinds[k], taken ? "taken" : "NOT taken",
                (int)hero->owner, (int)world_prisoners_chained(hero));
            if (!taken)
            {
                members[k] = 0;
                delete_thing_structure(hero, 0); /* (in play it stays a captive in the hand's drop; here it would fight) */
            }
        }
        return;
    }
    if ((freeze == 1) && (t == 20))
    {
        struct Thing *m = thing_get(members[0]);
        if (thing_exists(m) && world_test_far_spot(&fx, &fy))
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(fx));
            pos.y.val = subtile_coord_center(slab_subtile_center(fy));
            pos.z.val = get_floor_height_at(&pos);
            move_thing_in_map(m, &pos);
            SYNCLOG("Prisoners test: the Tunneller (index %d) taken far off, to engine slab (%d,%d)", (int)m->index, (int)fx, (int)fy);
        }
    }
    if ((freeze == 1) && (t == 60))
    {
        world_test_freeze_chunk(fx, fy);
        struct Thing *m = thing_get(members[0]);
        SYNCLOG("Prisoners test: its chunk frozen: the Tunneller %s; %d in chain gangs", (thing_exists(m) &&
            (m->class_id == TCls_Creature) && (m->model == (ThingModel)get_id(creature_desc, "TUNNELLER"))) ? "is STILL there" :
            "is gone", world_prisoners_gang_size);
    }
    if ((freeze == 1) && (t == 400))
    {
        MapSlabCoord sx, sy;
        world_test_far_spot(&sx, &sy); /* (its chunks load again) */
    }
    if ((freeze == 1) && (t == 460))
    {
        int found = 0;
        for (long i = 1; i < THINGS_COUNT; i++)
        {
            struct Thing *c = thing_get(i);
            if (!thing_exists(c) || (c->class_id != TCls_Creature) || (c->model != (ThingModel)get_id(creature_desc, "TUNNELLER")) ||
                (c->owner != my_player_number))
                continue;
            found++;
            SYNCLOG("Prisoners test: after the thaw: TUNNELLER index %d owner %d chained %d digger %d on the diggers list %d, wage %ld",
                (int)c->index, (int)c->owner, (int)world_prisoners_chained(c), (int)thing_is_creature_digger(c),
                (int)creature_is_for_dungeon_diggers_list(c), (long)calculate_correct_creature_pay(c));
        }
        SYNCLOG("Prisoners test: after the thaw: %d Tunnellers of the keeper; %d in chain gangs", found, world_prisoners_gang_size);
    }
    if (freeze == 1)
        return;
    if ((t == 300) || (t == 900))
    {
        for (int k = 0; k < 3; k++)
        {
            struct Thing *m = thing_get(members[k]);
            if ((members[k] == 0) || !thing_exists(m) || (m->class_id != TCls_Creature))
                continue;
            SYNCLOG("Prisoners test: T+%ld: %s owner %d chained %d digger %d on the diggers list %d, speed %ld, wage %ld, state %d",
                t, thing_model_name(m), (int)m->owner, (int)world_prisoners_chained(m), (int)thing_is_creature_digger(m),
                (int)creature_is_for_dungeon_diggers_list(m), (long)creature_control_get_from_thing(m)->max_speed,
                (long)calculate_correct_creature_pay(m), (int)get_creature_state_besides_interruptions(m));
        }
    }
    if (t == 310)
    {
        /* every other creature of the keeper far off: the gang is unguarded */
        const struct Dungeon *dungeon = get_dungeon(my_player_number);
        long i = dungeon->creatr_list_start;
        int moved = 0;
        for (int k = 0; (i != 0) && (k < CREATURES_COUNT); k++)
        {
            struct Thing *other = thing_get(i);
            const struct CreatureControl *octrl = creature_control_get_from_thing(other);
            if (thing_is_invalid(other) || creature_control_invalid(octrl))
                break;
            i = octrl->players_next_creature_idx;
            if (world_prisoners_chained(other))
                continue;
            struct Coord3d pos = heart->mappos;
            pos.x.val -= 20 * COORD_PER_STL;
            move_thing_in_map(other, &pos);
            moved++;
        }
        SYNCLOG("Prisoners test: %d other creatures moved far away", moved);
    }
    /* "T,2": with the others moved away (T+310), the Tunneller slips its irons at T+320 (as the roll would have it), and
       its way to the heroes' land is logged every 400 turns */
    if ((freeze == 2) && (t == 320))
    {
        struct Thing *m = thing_get(members[0]);
        struct PfGang *g = thing_exists(m) ? pf_gang_of(m) : NULL;
        if (g != NULL)
            pf_loose(g, m);
        SYNCLOG("Prisoners test: the Tunneller let loose: %s", (g != NULL) ? "yes" : "it was not in irons");
    }
    if ((freeze == 2) && (t > 320) && (((t - 320) % 400) == 0))
    {
        int n = 0;
        for (int i = 0; i < PF_FREE_MAX; i++)
            if (pf_free[i].thing != 0)
            {
                const struct Thing *f = thing_get(pf_free[i].thing);
                n++;
                SYNCLOG("Prisoners test: T+%ld: a loose %s at engine slab (%d,%d), state %d", t, thing_exists(f) ? thing_model_name(f) : "?",
                    thing_exists(f) ? (int)subtile_slab(f->mappos.x.stl.num) : -1, thing_exists(f) ? (int)subtile_slab(f->mappos.y.stl.num) : -1,
                    thing_exists(f) ? (int)get_creature_state_besides_interruptions(f) : -1);
            }
        if (n == 0)
            SYNCLOG("Prisoners test: T+%ld: no loose captive left", t);
    }
}

void world_prisoners_turn(void)
{
    pf_gang_turn();
    if ((game.play_gameturn % 32) == 17)
        pf_free_turn();
    pf_test_hook();
    pf_gang_test_hook();
}

void world_prisoners_reset(void)
{
    memset(pf_queue, 0, sizeof(pf_queue));
    memset(pf_gang, 0, sizeof(pf_gang));
    memset(pf_slot_of, 0, sizeof(pf_slot_of));
    memset(pf_free, 0, sizeof(pf_free));
    world_prisoners_gang_size = 0;
}

static int pf_count(void)
{
    int n = 0;
    for (int i = 0; i < PF_QUEUE_MAX; i++)
        if (pf_queue[i].model != 0)
            n++;
    return n;
}

size_t world_prisoners_save_size(void)
{
    if ((pf_count() == 0) && (world_prisoners_gang_size == 0))
        return 0;
    return 8 + sizeof(pf_queue) + ((world_prisoners_gang_size > 0) ? 8 + sizeof(pf_gang) : 0);
}

void world_prisoners_save(uint8_t *buf)
{
    const uint32_t head[2] = {PF_SAVE_MAGIC, (uint32_t)sizeof(pf_queue)};
    memcpy(buf, head, 8);
    memcpy(buf + 8, pf_queue, sizeof(pf_queue));
    if (world_prisoners_gang_size > 0)
    {
        uint8_t *g = buf + 8 + sizeof(pf_queue);
        const uint32_t ghead[2] = {PF_GANG_MAGIC, (uint32_t)sizeof(pf_gang)};
        memcpy(g, ghead, 8);
        memcpy(g + 8, pf_gang, sizeof(pf_gang));
    }
}

TbBool world_prisoners_load(const uint8_t *buf, size_t len)
{
    world_prisoners_reset();
    uint32_t head[2];
    if (len < 8)
        return false;
    memcpy(head, buf, 8);
    if ((head[0] != PF_SAVE_MAGIC) || (len < 8 + (size_t)head[1]))
        return false;
    memcpy(pf_queue, buf + 8, min((size_t)head[1], sizeof(pf_queue)));
    const size_t at = 8 + (size_t)head[1];
    uint32_t ghead[2];
    if (len >= at + 8)
    {
        memcpy(ghead, buf + at, 8);
        if ((ghead[0] == PF_GANG_MAGIC) && (len >= at + 8 + (size_t)ghead[1]))
            memcpy(pf_gang, buf + at + 8, min((size_t)ghead[1], sizeof(pf_gang)));
    }
    pf_gang_count();
    SYNCLOG("Prisoners: %d ransomed heroes waiting to come back, %d in chain gangs", pf_count(), world_prisoners_gang_size);
    return true;
}

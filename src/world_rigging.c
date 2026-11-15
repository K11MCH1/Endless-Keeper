/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rigging.c
 *     Endless world: rigging, devices linked to each other.
 * @par Purpose:
 *     See world_rigging.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_rigging.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "world_stream.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_traps.h"
#include "thing_doors.h"
#include "thing_stats.h"
#include "config_trapdoor.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "map_blocks.h"
#include "slab_data.h"
#include "config_creature.h"
#include "thing_creature.h"
#include "room_util.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* A link knows its two devices by thing (fast) and by place and kind: a chunk that freezes and comes back makes new
 * things of them, which are found again where they stand (2026-11-13; before, the links of a frozen dungeon were
 * dropped by the sweep for good). */
struct RigLink {
    ThingIndex trigger;
    int32_t trigger_born;
    ThingIndex target;
    int32_t target_born;
    uint8_t owner;
    uint8_t used;
    uint8_t target_class;
    uint8_t pad;
    ThingModel trigger_model, target_model;
    int32_t pad2;
    int64_t trigger_wx, trigger_wy, target_wx, target_wy; /* world slabs */
};
static struct RigLink rl[RIGGING_LINKS_MAX];
/* Counting doors: a door of the keeper's that locks once a number of heroes have passed it ("!count N" on it). Known by
 * place and kind as well as by thing (see struct RigLink). Saved as the XTR2 record "RGC1". */
#define RIG_COUNTERS_MAX 128
#define RIG_COUNT_SEEN 8
struct RigCounter {
    uint8_t used;
    uint8_t owner;
    uint8_t need;            /* heroes to pass before it locks */
    uint8_t count;           /* heroes passed so far */
    ThingIndex door;
    int32_t door_born;
    ThingModel door_model;
    uint8_t seen_at;         /* the next slot of seen[] */
    uint8_t pad;
    ThingIndex seen[RIG_COUNT_SEEN];     /* the last heroes counted (so one standing in it counts once) */
    int32_t seen_born[RIG_COUNT_SEEN];
    int64_t wx, wy;          /* the door's world slab */
};
static struct RigCounter rcn[RIG_COUNTERS_MAX];

static ThingIndex rig_pick[PLAYERS_MAX];      /* the Alarm a keeper picked (server memory) */
static int32_t rig_pick_born[PLAYERS_MAX];
static int rig_depth;                         /* a linked trap that is an Alarm itself: no endless chains */

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: rigging, to player %d: %s", (int)p, text);
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

static TbBool is_alarm(const struct Thing *t)
{
    static ThingModel alarm = -1;
    if (alarm < 0)
        alarm = (ThingModel)get_id(trap_desc, "ALARM");
    return thing_exists(t) && (t->class_id == TCls_Trap) && (t->model == alarm);
}

static TbBool alive(ThingIndex idx, int32_t born, struct Thing **out)
{
    struct Thing *t = thing_get(idx);
    if (!thing_exists(t) || (t->creation_turn != born))
        return false;
    *out = t;
    return true;
}

static void place_of(const struct Thing *t, int64_t *wx, int64_t *wy)
{
    world_slab_to_world(subtile_slab(t->mappos.x.stl.num), subtile_slab(t->mappos.y.stl.num), wx, wy);
}

/** One end of a link: 1 found (*out, and the link's thing brought up to date), 0 gone (its ground is loaded and it
 *  isn't there), -1 its ground isn't loaded (frozen: it is still there, as far as anyone knows). */
static int locate(ThingIndex *idx, int32_t *born, uint8_t cls, ThingModel model, uint8_t owner, int64_t wx, int64_t wy,
    struct Thing **out)
{
    struct Thing *t = thing_get(*idx);
    if (thing_exists(t) && (t->creation_turn == *born) && (t->class_id == cls) && (t->model == model))
    {
        *out = t;
        return 1;
    }
    MapSlabCoord x, y;
    if (!world_engine_slab(wx, wy, &x, &y))
        return -1;
    t = find_base_thing_on_mapwho(cls, model, slab_subtile_center(x), slab_subtile_center(y));
    if (thing_is_invalid(t) || (t->owner != owner))
        return 0;
    *idx = t->index;
    *born = t->creation_turn;
    *out = t;
    return 1;
}

/** Is this link's Alarm this thing (by thing, or by place and kind)? */
static TbBool link_trigger_is(struct RigLink *l, const struct Thing *traptng)
{
    if ((l->trigger == traptng->index) && (l->trigger_born == traptng->creation_turn))
        return true;
    int64_t wx, wy;
    place_of(traptng, &wx, &wy);
    if ((l->trigger_wx != wx) || (l->trigger_wy != wy) || (l->trigger_model != traptng->model) || (l->owner != traptng->owner))
        return false;
    l->trigger = traptng->index;
    l->trigger_born = traptng->creation_turn;
    return true;
}

/** The keeper's door or trap at or beside a subtile. */
static struct Thing *device_at(PlayerNumber p, MapSubtlCoord sx, MapSubtlCoord sy)
{
    for (int r = 0; r <= 1; r++)
        for (MapSubtlCoord y = sy - r; y <= sy + r; y++)
            for (MapSubtlCoord x = sx - r; x <= sx + r; x++)
            {
                if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                    continue;
                long i = get_mapwho_thing_index(get_map_block_at(x, y));
                unsigned long guard = 0;
                while ((i != 0) && (guard++ < THINGS_COUNT))
                {
                    struct Thing *t = thing_get(i);
                    if (thing_is_invalid(t))
                        break;
                    i = t->next_on_mapblk;
                    if (((t->class_id == TCls_Trap) || (t->class_id == TCls_Door)) && (t->owner == p))
                        return t;
                }
            }
    return INVALID_THING;
}

static void set_counter(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y, int need);

void world_rigging_packet(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    const int count_cmd = (int)(((uint32_t)stl_x >> 24) & 0xFF); /* "!count N": N + 1 in the top byte (0: plain "!rig") */
    stl_x = (MapSubtlCoord)((uint32_t)stl_x & 0x00FFFFFFu);
    if (count_cmd != 0)
    {
        set_counter(p, stl_x, stl_y, count_cmd - 1);
        return;
    }
    struct Thing *dev = device_at(p, stl_x, stl_y);
    if (!thing_exists(dev))
    {
        tell_keeper(p, "Point at one of your Alarm traps, then at one of your doors or traps.");
        return;
    }
    struct Thing *trigger = INVALID_THING;
    const TbBool picked = alive(rig_pick[p], rig_pick_born[p], &trigger) && is_alarm(trigger) && (trigger->owner == p);
    if (!picked || (dev == trigger))
    {
        if (!is_alarm(dev))
        {
            tell_keeper(p, "Rigging starts at an Alarm trap: point at one of yours first.");
            return;
        }
        rig_pick[p] = dev->index;
        rig_pick_born[p] = dev->creation_turn;
        tell_keeper(p, "Alarm picked: now \"!rig\" on a door or trap of yours to link it (again to unlink).");
        return;
    }
    /* linked already: unlink */
    int count = 0;
    for (int i = 0; i < RIGGING_LINKS_MAX; i++)
    {
        struct RigLink *l = &rl[i];
        if (!l->used || !link_trigger_is(l, trigger))
            continue;
        struct Thing *cur = INVALID_THING;
        if ((locate(&l->target, &l->target_born, l->target_class, l->target_model, l->owner, l->target_wx, l->target_wy, &cur) == 1)
            && (cur == dev))
        {
            memset(l, 0, sizeof(*l));
            tell_keeper(p, "Unlinked.");
            return;
        }
        count++;
    }
    if (count >= RIGGING_PER_TRIGGER)
    {
        tell_keeper(p, "An Alarm sets off eight devices at most.");
        return;
    }
    for (int i = 0; i < RIGGING_LINKS_MAX; i++)
    {
        struct RigLink *l = &rl[i];
        if (l->used)
            continue;
        l->used = 1;
        l->owner = (uint8_t)p;
        l->trigger = trigger->index;
        l->trigger_born = trigger->creation_turn;
        l->target = dev->index;
        l->target_born = dev->creation_turn;
        l->target_class = (uint8_t)dev->class_id;
        l->trigger_model = trigger->model;
        l->target_model = dev->model;
        place_of(trigger, &l->trigger_wx, &l->trigger_wy);
        place_of(dev, &l->target_wx, &l->target_wy);
        tell_keeper(p, (dev->class_id == TCls_Door) ? "Linked: the Alarm will lock this door." : "Linked: the Alarm will fire this trap.");
        return;
    }
    tell_keeper(p, "The world holds no more links.");
}

TbBool world_rigging_command(const char *text, MapCoord cursor_x, MapCoord cursor_y)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strncasecmp(text + 1, "count", 5) == 0)
    {
        /* "!count N" (1 to 20) with the cursor on a door of yours; "!count" or "!count 0" takes the count away */
        const char *arg = text + 6;
        if ((*arg != '\0') && (*arg != ' '))
            return false;
        int n = atoi(arg);
        n = max(0, min(n, RIGGING_COUNT_MAX));
        set_players_packet_action(get_my_player(), PckA_WorldRig,
            ((unsigned long)coord_subtile(cursor_x) & 0x00FFFFFFul) | ((unsigned long)(n + 1) << 24), (unsigned long)coord_subtile(cursor_y), 0, 0);
        return true;
    }
    if (strcasecmp(text + 1, "rig") != 0)
        return false;
    set_players_packet_action(get_my_player(), PckA_WorldRig, (unsigned long)coord_subtile(cursor_x),
        (unsigned long)coord_subtile(cursor_y), 0, 0);
    return true;
}

static void set_counter(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y, int need)
{
    struct Thing *door = device_at(p, stl_x, stl_y);
    if (!thing_exists(door) || (door->class_id != TCls_Door))
    {
        tell_keeper(p, "Point at one of your doors to count heroes through it.");
        return;
    }
    int64_t wx, wy;
    place_of(door, &wx, &wy);
    struct RigCounter *c = NULL, *free_c = NULL;
    for (int i = 0; (i < RIG_COUNTERS_MAX) && (c == NULL); i++)
    {
        if (!rcn[i].used)
        {
            if (free_c == NULL)
                free_c = &rcn[i];
            continue;
        }
        if ((rcn[i].wx == wx) && (rcn[i].wy == wy))
            c = &rcn[i];
    }
    if (need <= 0)
    {
        if (c != NULL)
            memset(c, 0, sizeof(*c));
        tell_keeper(p, "That door no longer counts heroes.");
        return;
    }
    if (c == NULL)
        c = free_c;
    if (c == NULL)
    {
        tell_keeper(p, "The world holds no more counting doors.");
        return;
    }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->owner = (uint8_t)p;
    c->need = (uint8_t)need;
    c->door = door->index;
    c->door_born = door->creation_turn;
    c->door_model = door->model;
    c->wx = wx;
    c->wy = wy;
    char text[96];
    snprintf(text, sizeof(text), "This door locks once %d hero%s passed it.", need, (need == 1) ? " has" : "es have");
    tell_keeper(p, text);
}

/** Every 8 turns (simulating machine): heroes in each counting door are counted, each once; at the number, it locks. */
static void count_heroes(void)
{
    for (int i = 0; i < RIG_COUNTERS_MAX; i++)
    {
        struct RigCounter *c = &rcn[i];
        if (!c->used)
            continue;
        struct Thing *door = INVALID_THING;
        const int found = locate(&c->door, &c->door_born, TCls_Door, c->door_model, c->owner, c->wx, c->wy, &door);
        if (found == 0)
        {
            memset(c, 0, sizeof(*c)); /* the door is gone */
            continue;
        }
        if (found < 0)
            continue; /* frozen */
        const MapSlabCoord sx = subtile_slab(door->mappos.x.stl.num), sy = subtile_slab(door->mappos.y.stl.num);
        for (MapSubtlCoord y = slab_subtile(sy, 0); y <= slab_subtile(sy, 2); y++)
            for (MapSubtlCoord x = slab_subtile(sx, 0); x <= slab_subtile(sx, 2); x++)
            {
                long j = get_mapwho_thing_index(get_map_block_at(x, y));
                unsigned long guard = 0;
                while ((j != 0) && (guard++ < THINGS_COUNT))
                {
                    struct Thing *t = thing_get(j);
                    if (thing_is_invalid(t))
                        break;
                    j = t->next_on_mapblk;
                    if (!thing_is_creature(t) || (t->owner != PLAYER_GOOD))
                        continue;
                    TbBool already = false;
                    for (int k = 0; (k < RIG_COUNT_SEEN) && !already; k++)
                        already = (c->seen[k] == t->index) && (c->seen_born[k] == t->creation_turn);
                    if (already)
                        continue;
                    c->seen[c->seen_at] = t->index;
                    c->seen_born[c->seen_at] = t->creation_turn;
                    c->seen_at = (uint8_t)((c->seen_at + 1) % RIG_COUNT_SEEN);
                    c->count++;
                }
            }
        if (c->count < c->need)
            continue;
        c->count = 0;
        if (!door->door.is_locked)
        {
            lock_door(door);
            char text[96];
            snprintf(text, sizeof(text), "A counting door of yours locks: %d hero%s passed it.", c->need, (c->need == 1) ? " has" : "es have");
            tell_keeper(c->owner, text);
            SYNCLOG("World: player %d's counting door locks after %d heroes", (int)c->owner, (int)c->need);
        }
    }
}

TbBool world_rigging_counting_door(const struct Thing *doortng)
{
    if (!world_mode_active() || thing_is_invalid(doortng) || (doortng->class_id != TCls_Door))
        return false;
    for (int i = 0; i < RIG_COUNTERS_MAX; i++)
        if (rcn[i].used && (rcn[i].door == doortng->index) && (rcn[i].door_born == doortng->creation_turn))
            return true;
    return false;
}

void world_rigging_triggered(struct Thing *traptng, struct Thing *creatng)
{
    if (!simulating() || !is_alarm(traptng) || (rig_depth > 0))
        return;
    rig_depth++;
    int locked = 0, fired = 0;
    for (int i = 0; i < RIGGING_LINKS_MAX; i++)
    {
        struct RigLink *l = &rl[i];
        struct Thing *t;
        if (!l->used || !link_trigger_is(l, traptng)
            || (locate(&l->target, &l->target_born, l->target_class, l->target_model, l->owner, l->target_wx, l->target_wy, &t) != 1))
            continue;
        if (t->owner != l->owner)
            continue; /* it changed hands */
        if (t->class_id == TCls_Door)
        {
            if (!t->door.is_locked)
            {
                lock_door(t);
                locked++;
            }
        } else if (t->class_id == TCls_Trap)
        {
            if (t->trap.num_shots <= 0)
                continue; /* spent or reloading: as the stock trigger, it does not fire */
            activate_trap(t, creatng);
            struct Dungeon *dungeon = get_dungeon(t->owner);
            if (!dungeon_invalid(dungeon))
                dungeon->trap_info.activated[t->trap.flag_number]++;
            process_trap_charge(t); /* a charge used, and the reload, as update_trap_trigger does */
            fired++;
        }
    }
    rig_depth--;
    if (locked + fired > 0)
        SYNCLOG("World: player %d's rigged Alarm goes off: %d door%s locked, %d trap%s fired", (int)traptng->owner, locked,
            (locked == 1) ? "" : "s", fired, (fired == 1) ? "" : "s");
}

/** KFX_WORLD_RIGTEST=turn (test switch): an Alarm, a Lightning trap and a door are made beside this machine's Heart,
 *  rigged through the "!rig" packet's function, and 100 turns later the Alarm is set off by a creature of the keeper;
 *  the door's lock and the Lightning trap's firing are logged. */
static void sweep_links(void);

static void count_heroes(void);

/** KFX_WORLD_RIGCOUNTSAVE=turn (test switch): a real door (on its own door slab between two walls) is built 8 slabs
 *  below and 5 east of this machine's Heart and made to count 2 heroes; 400 turns later (a save and load can come
 *  between) two Archers stand in it and it must lock. */
static void rig_count_save_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_RIGCOUNTSAVE");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at < 0) || (((long)game.play_gameturn != at) && ((long)game.play_gameturn != at + 400)))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord dx = subtile_slab(heart->mappos.x.stl.num) + 5, dy = subtile_slab(heart->mappos.y.stl.num) + 8;
    if ((long)game.play_gameturn == at)
    {
        for (int j = -1; j <= 1; j++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(dx), slab_subtile_center(dy + j), p, 0);
        place_slab_type_on_map(SlbT_WALLDRAPE, slab_subtile_center(dx - 1), slab_subtile_center(dy), p, 0);
        place_slab_type_on_map(SlbT_WALLDRAPE, slab_subtile_center(dx + 1), slab_subtile_center(dy), p, 0);
        player_place_door_without_check_at(slab_subtile_center(dx), slab_subtile_center(dy), p, (ThingModel)get_id(door_desc, "WOOD"), true);
        struct Thing *door = get_door_for_position(slab_subtile_center(dx), slab_subtile_center(dy));
        SYNCLOG("World: rig count test: a door built %d", (int)!thing_is_invalid(door));
        if (!thing_is_invalid(door))
            world_rigging_packet(p, (MapSubtlCoord)(((uint32_t)door->mappos.x.stl.num & 0x00FFFFFFu) | (3u << 24)), door->mappos.y.stl.num);
        return;
    }
    struct Thing *door = get_door_for_position(slab_subtile_center(dx), slab_subtile_center(dy));
    if (thing_is_invalid(door))
    {
        SYNCLOG("World: rig count test: the door is gone");
        return;
    }
    struct Coord3d hp = door->mappos;
    struct Thing *h1 = create_creature(&hp, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
    struct Thing *h2 = create_creature(&hp, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
    count_heroes();
    SYNCLOG("World: rig count test: after the save, two Archers in the door: locked %d", (int)door->door.is_locked);
    if (!thing_is_invalid(h1))
        delete_thing_structure(h1, 0);
    if (!thing_is_invalid(h2))
        delete_thing_structure(h2, 0);
}

static void rig_test(void)
{
    static long at = -2;
    static ThingIndex alarm = 0, door = 0;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_RIGTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_RIGTEST=%ld", at);
    }
    if (at < 0)
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    if ((long)game.play_gameturn == at)
    {
        struct Coord3d a = heart->mappos, b = heart->mappos, c = heart->mappos;
        a.y.val += 6 * COORD_PER_STL;
        b.y.val += 6 * COORD_PER_STL; b.x.val += 6 * COORD_PER_STL;
        c.y.val += 6 * COORD_PER_STL; c.x.val -= 6 * COORD_PER_STL;
        struct Thing *ta = create_trap(&a, (ThingModel)get_id(trap_desc, "ALARM"), p);
        struct Thing *tb = create_trap(&b, (ThingModel)get_id(trap_desc, "LIGHTNING"), p);
        struct Thing *tc = create_door(&c, (ThingModel)get_id(door_desc, "WOOD"), 0, p, false);
        if (!thing_is_invalid(tb))
            rearm_trap(tb); /* armed, as a built trap is (a linked trap fires only with a charge) */
        if (thing_is_invalid(ta) || thing_is_invalid(tb) || thing_is_invalid(tc))
        {
            SYNCLOG("World: rig test: could not make the devices (%d %d %d)", !thing_is_invalid(ta), !thing_is_invalid(tb), !thing_is_invalid(tc));
            return;
        }
        alarm = ta->index;
        door = tc->index;
        world_rigging_packet(p, ta->mappos.x.stl.num, ta->mappos.y.stl.num);
        world_rigging_packet(p, tb->mappos.x.stl.num, tb->mappos.y.stl.num);
        world_rigging_packet(p, ta->mappos.x.stl.num, ta->mappos.y.stl.num);
        world_rigging_packet(p, tc->mappos.x.stl.num, tc->mappos.y.stl.num);
        if (getenv("KFX_WORLD_RIGCOUNT") != NULL)
        {
            /* KFX_WORLD_RIGCOUNT=1 as well: the door counts 2 heroes; one Archer in it is counted on two looks (once), a
               second Archer locks it (the rigged Alarm is not set off in this case) */
            world_rigging_packet(p, (MapSubtlCoord)(((uint32_t)tc->mappos.x.stl.num & 0x00FFFFFFu) | (3u << 24)), tc->mappos.y.stl.num);
            struct Coord3d hp = tc->mappos;
            struct Thing *h1 = create_creature(&hp, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
            count_heroes();
            count_heroes();
            const int after_one = tc->door.is_locked;
            struct Thing *h2 = create_creature(&hp, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
            count_heroes();
            SYNCLOG("World: rig test: counting door: heroes made %d %d; locked after one (seen twice) %d, after two %d",
                (int)!thing_is_invalid(h1), (int)!thing_is_invalid(h2), after_one, (int)tc->door.is_locked);
            if (!thing_is_invalid(h1))
                delete_thing_structure(h1, 0);
            if (!thing_is_invalid(h2))
                delete_thing_structure(h2, 0);
            at = -1; /* the Alarm part is not run after this */
            return;
        }
        if (getenv("KFX_WORLD_RIGRELINK") != NULL)
        {
            /* KFX_WORLD_RIGRELINK=1 as well: the Alarm and the trap are made again as new things where they stood, as a
               chunk that froze and came back does, and the sweep runs; the links must hold */
            struct Coord3d pa = ta->mappos, pb = tb->mappos;
            const ThingModel ma = ta->model, mb = tb->model;
            destroy_trap(ta);
            destroy_trap(tb);
            struct Thing *na = create_trap(&pa, ma, p), *nb = create_trap(&pb, mb, p);
            if (!thing_is_invalid(na) && !thing_is_invalid(nb))
            {
                na->creation_turn += 1; /* as if made on another turn, so the old thing's turn no longer matches */
                nb->creation_turn += 1;
                alarm = na->index;
            }
            sweep_links();
            int n = 0;
            for (int i = 0; i < RIGGING_LINKS_MAX; i++)
                n += rl[i].used ? 1 : 0;
            SYNCLOG("World: rig test: the Alarm and the trap made again (things %d, %d); %d links after the sweep",
                thing_is_invalid(na) ? -1 : (int)na->index, thing_is_invalid(nb) ? -1 : (int)nb->index, n);
        }
    }
    if ((long)game.play_gameturn == at)
    {
        struct Thing *ta = thing_get(alarm), *tc = thing_get(door);
        if (!thing_exists(ta) || !thing_exists(tc))
        {
            SYNCLOG("World: rig test: a device is gone (alarm %d, door %d)", (int)thing_exists(ta), (int)thing_exists(tc));
            return;
        }
        const int was_locked = tc->door.is_locked;
        const struct Dungeon *dg = get_players_num_dungeon(p);
        struct Thing *who = dungeon_invalid(dg) ? INVALID_THING : thing_get(dg->digger_list_start); /* one of its imps sets it off */
        if (!thing_exists(who))
        {
            SYNCLOG("World: rig test: no imp to set the Alarm off");
            return;
        }
        activate_trap(ta, who);
        SYNCLOG("World: rig test: the Alarm goes off: the door was locked %d, is locked %d", was_locked, (int)tc->door.is_locked);
    }
}

void world_rigging_turn(void)
{
    if (!simulating())
        return;
    rig_test();
    rig_count_save_test();
    if ((game.play_gameturn % 8) == 5)
        count_heroes();
    if ((game.play_gameturn % 256) != 41)
        return;
    sweep_links();
}

static void sweep_links(void)
{
    for (int i = 0; i < RIGGING_LINKS_MAX; i++)
    {
        struct RigLink *l = &rl[i];
        struct Thing *t;
        if (!l->used)
            continue;
        /* a device is gone (its ground loaded, nothing there): the link goes; frozen ground keeps it */
        if ((locate(&l->trigger, &l->trigger_born, TCls_Trap, l->trigger_model, l->owner, l->trigger_wx, l->trigger_wy, &t) == 0)
            || (locate(&l->target, &l->target_born, l->target_class, l->target_model, l->owner, l->target_wx, l->target_wy, &t) == 0))
            memset(l, 0, sizeof(*l));
    }
}

/******************************************************************************/
void world_rigging_reset(void)
{
    memset(rl, 0, sizeof(rl));
    memset(rig_pick, 0, sizeof(rig_pick));
    memset(rig_pick_born, 0, sizeof(rig_pick_born));
    rig_depth = 0;
    memset(rcn, 0, sizeof(rcn));
}

/* the counting doors' record (RGC1): their count, then each used counter */
size_t world_rigging_counters_save_size(void)
{
    int n = 0;
    for (int i = 0; i < RIG_COUNTERS_MAX; i++)
        n += rcn[i].used ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct RigCounter) : 0;
}

void world_rigging_counters_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < RIG_COUNTERS_MAX; i++)
    {
        if (!rcn[i].used)
            continue;
        memcpy(buf + at, &rcn[i], sizeof(struct RigCounter));
        at += sizeof(struct RigCounter);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_rigging_counters_load(const uint8_t *buf, size_t len)
{
    memset(rcn, 0, sizeof(rcn));
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > RIG_COUNTERS_MAX) || (4 + (size_t)n * sizeof(struct RigCounter) > len))
        return false;
    for (uint32_t i = 0; i < n; i++)
        memcpy(&rcn[i], buf + 4 + (size_t)i * sizeof(struct RigCounter), sizeof(struct RigCounter));
    return true;
}

void world_rigging_counters_reset(void)
{
    memset(rcn, 0, sizeof(rcn));
}

size_t world_rigging_save_size(void)
{
    int n = 0;
    for (int i = 0; i < RIGGING_LINKS_MAX; i++)
        n += rl[i].used ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct RigLink) : 0;
}

void world_rigging_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < RIGGING_LINKS_MAX; i++)
    {
        if (!rl[i].used)
            continue;
        memcpy(buf + at, &rl[i], sizeof(struct RigLink));
        at += sizeof(struct RigLink);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_rigging_load(const uint8_t *buf, size_t len)
{
    world_rigging_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct RigLink) + 4 > len)
        return false;
    for (uint32_t i = 0; (i < n) && (i < RIGGING_LINKS_MAX); i++)
        memcpy(&rl[i], buf + 4 + (size_t)i * sizeof(struct RigLink), sizeof(struct RigLink));
    return true;
}

#ifdef __cplusplus
}
#endif

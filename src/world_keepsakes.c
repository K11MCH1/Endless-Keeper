/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keepsakes.c
 *     Endless world: keepsakes, loot for named creatures.
 * @par Purpose:
 *     See world_keepsakes.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_keepsakes.h"
#include "world_caretaker.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "world_stream.h"
#include "world_storage.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_stats.h"
#include "thing_creature.h"
#include "thing_navigate.h"
#include "room_data.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_objects.h"
#include "map_data.h"
#include "power_hand.h"
#include "gui_msgs.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "server_admin.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
enum KsStat { KS_Health = 0, KS_Strength, KS_Armour, KS_Speed, KS_COUNT };
static const char *const ks_names[KS_COUNT] = {"a heart-stone", "a war charm", "an iron tooth", "a hare's foot"};
static const char *const ks_stats[KS_COUNT] = {"health", "strength", "armour", "speed"};

struct Keepsake {
    uint8_t used;
    uint8_t stat;
    uint8_t pct;
    uint8_t owner;          /* the keeper it belongs to (PLAYER_NEUTRAL: nobody's, lying in the world) */
    uint8_t last_owner;     /* the keeper who had it last (an ally of theirs can't take it) */
    uint8_t pad;
    uint16_t bearer_md;     /* carried by the creature with this Misdeeds record (0: not carried) */
    ThingIndex obj;         /* lying as this object (when not carried) */
    int32_t obj_born;
    ThingIndex bearer;      /* the bearer's thing as last seen (found again by its record when gone) */
    int32_t bearer_born;
    /* where it lies, as last seen: in a chunk that freezes and comes back it is a new thing, found again there
       (2026-11-13; before, it was forgotten and came back as a plain object) */
    ThingModel obj_model;
    uint8_t sub_x, sub_y;   /* its subtile within its slab */
    int64_t wx, wy;         /* its world slab */
};
static struct Keepsake ks[WORLD_KEEPSAKES_MAX];

#define kview game.world_keepsake_view
#define kborn game.world_keepsake_born

static ThingModel ks_model(void)
{
    static ThingModel model = -1;
    if (model < 0)
        model = (ThingModel)get_id(object_desc, "POTION_GREEN"); /* stands in until keepsakes have art */
    return model;
}

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void ks_note_place(struct Keepsake *k, const struct Thing *obj)
{
    k->obj_model = obj->model;
    world_slab_to_world(subtile_slab(obj->mappos.x.stl.num), subtile_slab(obj->mappos.y.stl.num), &k->wx, &k->wy);
    k->sub_x = (uint8_t)(obj->mappos.x.stl.num % STL_PER_SLB);
    k->sub_y = (uint8_t)(obj->mappos.y.stl.num % STL_PER_SLB);
}

static struct Keepsake *ks_of_object(const struct Thing *thing)
{
    if (!thing_exists(thing) || (thing->class_id != TCls_Object))
        return NULL;
    for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
        if (ks[i].used && (ks[i].bearer_md == 0) && (ks[i].obj == thing->index) && (ks[i].obj_born == thing->creation_turn))
            return &ks[i];
    return NULL;
}

TbBool world_keepsake_is_object(const struct Thing *thing)
{
    return world_mode_active() && (ks_of_object(thing) != NULL);
}

/** What a creature carries, from the copy in struct Game (every machine): stat and percent, false when nothing. */
static TbBool carried(const struct Thing *creatng, int *stat, int *pct)
{
    if (!world_mode_active() || !thing_exists(creatng) || !thing_is_creature(creatng))
        return false;
    const CctrlIndex c = creatng->ccontrol_idx;
    if ((c <= 0) || (c >= CREATURES_COUNT) || (kview[c] == 0) || (kborn[c] != creatng->creation_turn))
        return false;
    *stat = (kview[c] >> 8) - 1;
    *pct = kview[c] & 0xFF;
    return (*stat >= 0) && (*stat < KS_COUNT);
}

static uint32_t ks_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

void world_keepsakes_drop(const struct Coord3d *pos, int chance, uint32_t salt, PlayerNumber last_owner)
{
    if (!simulating() || (pos == NULL))
        return;
    const uint32_t h = ks_hash(salt ^ ((uint32_t)game.play_gameturn * 2654435761U));
    if ((int)(h % 100) >= chance)
        return;
    struct Keepsake *k = NULL;
    for (int i = 0; (k == NULL) && (i < WORLD_KEEPSAKES_MAX); i++)
        if (!ks[i].used)
            k = &ks[i];
    if ((k == NULL) || (ks_model() <= 0))
        return;
    struct Coord3d at = *pos;
    at.z.val = 0;
    at.z.val = get_floor_height_at(&at);
    struct Thing *obj = create_object(&at, ks_model(), game.neutral_player_num, -1);
    if (thing_is_invalid(obj))
        return;
    memset(k, 0, sizeof(*k));
    k->used = 1;
    k->stat = (uint8_t)((h >> 8) % KS_COUNT);
    const char *force = getenv("KFX_WORLD_KEEPSAKESTAT"); /* test switch: every keepsake of this stat (0-3) */
    if ((force != NULL) && (atoi(force) >= 0) && (atoi(force) < KS_COUNT))
        k->stat = (uint8_t)atoi(force);
    k->pct = (uint8_t)(5 + (h >> 16) % 11); /* +5 to +15 % */
    k->owner = PLAYER_NEUTRAL;
    k->last_owner = (uint8_t)(((last_owner >= 0) && (last_owner < PLAYERS_MAX)) ? last_owner : PLAYER_NEUTRAL);
    k->obj = obj->index;
    k->obj_born = obj->creation_turn;
    ks_note_place(k, obj);
    world_storage_add_loot(obj, PLAYER_NEUTRAL);
    SYNCLOG("World: a keepsake drops at subtile (%d,%d): %s, +%d%% %s", (int)at.x.stl.num, (int)at.y.stl.num,
        ks_names[k->stat], (int)k->pct, ks_stats[k->stat]);
}

/** A creature's stats are worked out again when what it carries changes. */
static void restat(struct Thing *creatng)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return;
    cctrl->max_speed = calculate_correct_creature_maxspeed(creatng);
    const HitPoints was = cctrl->max_health;
    cctrl->max_health = calculate_correct_creature_max_health(creatng);
    if ((was > 0) && (creatng->health > cctrl->max_health))
        creatng->health = cctrl->max_health;
}

static void set_view(struct Thing *creatng, const struct Keepsake *k)
{
    const CctrlIndex c = creatng->ccontrol_idx;
    if ((c <= 0) || (c >= CREATURES_COUNT))
        return;
    const uint16_t v = (k != NULL) ? (uint16_t)(((k->stat + 1) << 8) | k->pct) : 0;
    if ((kview[c] == v) && (kborn[c] == creatng->creation_turn))
        return;
    kview[c] = v;
    kborn[c] = creatng->creation_turn;
    restat(creatng);
}

void world_keepsakes_hand_dropped(struct Dungeon *dungeon, struct Thing *objtng)
{
    if (!simulating() || dungeon_invalid(dungeon))
        return;
    struct Keepsake *k = ks_of_object(objtng);
    if (k == NULL)
        return;
    const PlayerNumber p = dungeon->owner;
    /* the named creature of this keeper it was dropped on (within a subtile), carrying none */
    const MapSubtlCoord cx = objtng->mappos.x.stl.num, cy = objtng->mappos.y.stl.num;
    struct Thing *bearer = INVALID_THING;
    for (MapSubtlCoord y = cy - 1; (y <= cy + 1) && thing_is_invalid(bearer); y++)
    {
        for (MapSubtlCoord x = cx - 1; (x <= cx + 1) && thing_is_invalid(bearer); x++)
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
                if (!thing_is_creature(t) || (t->owner != p) || creature_is_dying(t))
                    continue;
                const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
                int s, pc;
                if (creature_control_invalid(cctrl) || (cctrl->creature_name[0] == '\0') || (cctrl->misdeed_id == 0) || carried(t, &s, &pc))
                    continue;
                bearer = t;
                break;
            }
        }
    }
    if (thing_is_invalid(bearer))
        return;
    const struct CreatureControl *bctrl = creature_control_get_from_thing(bearer);
    k->bearer_md = bctrl->misdeed_id;
    k->bearer = bearer->index;
    k->bearer_born = bearer->creation_turn;
    k->owner = (uint8_t)p;
    k->last_owner = (uint8_t)p;
    k->obj = 0;
    k->obj_born = 0;
    world_storage_forget(objtng);
    delete_thing_structure(objtng, 0);
    set_view(bearer, k);
    SYNCLOG("World: player %d gives %s (+%d%% %s) to %s", (int)p, ks_names[k->stat], (int)k->pct, ks_stats[k->stat],
        bctrl->creature_name);
    if (is_my_player_number(p) && !netstate_dedicated())
        message_add_fmt(MsgType_Creature, bearer->model, "%s now carries %s: +%d%% %s", bctrl->creature_name,
            ks_names[k->stat], (int)k->pct, ks_stats[k->stat]);
    else if (network_is_active() && netstate_is_server())
    {
        /* a joined player hears it from the server */
        const NetUserId u = get_player_primary_user(get_player(p));
        if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        {
            char text[128];
            snprintf(text, sizeof(text), "%s now carries %s: +%d%% %s", bctrl->creature_name, ks_names[k->stat], (int)k->pct,
                ks_stats[k->stat]);
            server_admin_notice(u, text);
        }
    }
}

static struct Keepsake *ks_of_bearer(const struct Thing *creatng)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0))
        return NULL;
    for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
        if (ks[i].used && (ks[i].bearer_md == cctrl->misdeed_id))
            return &ks[i];
    return NULL;
}

void world_keepsakes_bearer_dying(struct Thing *creatng)
{
    if (!simulating() || !thing_exists(creatng))
        return;
    struct Keepsake *k = ks_of_bearer(creatng);
    if (k == NULL)
        return;
    struct Coord3d at = creatng->mappos;
    at.z.val = 0;
    at.z.val = get_floor_height_at(&at);
    struct Thing *obj = create_object(&at, ks_model(), game.neutral_player_num, -1);
    set_view(creatng, NULL);
    k->bearer_md = 0;
    k->bearer = 0;
    k->last_owner = (uint8_t)creatng->owner;
    k->owner = PLAYER_NEUTRAL;
    if (thing_is_invalid(obj))
    {
        memset(k, 0, sizeof(*k)); /* no room for the object: it is lost */
        return;
    }
    k->obj = obj->index;
    k->obj_born = obj->creation_turn;
    ks_note_place(k, obj);
    world_storage_add_loot(obj, PLAYER_NEUTRAL);
    SYNCLOG("World: a keepsake (%s) drops where its bearer of player %d died", ks_names[k->stat], (int)creatng->owner);
}

long world_keepsakes_speed(const struct Thing *creatng, long speed)
{
    int s, pc;
    return (carried(creatng, &s, &pc) && (s == KS_Speed)) ? speed * (100 + pc) / 100 : speed;
}

HitPoints world_keepsakes_max_health(const struct Thing *creatng, HitPoints health)
{
    int s, pc;
    return (carried(creatng, &s, &pc) && (s == KS_Health)) ? (HitPoints)((int64_t)health * (100 + pc) / 100) : health;
}

HitPoints world_keepsakes_adjust_damage(const struct Thing *victim, const struct Thing *attacker, HitPoints dmg)
{
    int s, pc;
    if (dmg <= 0)
        return dmg;
    if (carried(attacker, &s, &pc) && (s == KS_Strength))
        dmg = (HitPoints)((int64_t)dmg * (100 + pc) / 100);
    if (carried(victim, &s, &pc) && (s == KS_Armour))
        dmg = (HitPoints)((int64_t)dmg * 100 / (100 + pc));
    return dmg;
}

const char *world_keepsakes_label(const struct Thing *creatng)
{
    static char text[48];
    int s, pc;
    if (!carried(creatng, &s, &pc))
        return "";
    snprintf(text, sizeof(text), " (%s)", ks_names[s]);
    return text;
}

const char *world_keepsakes_line(const struct Thing *creatng)
{
    static char text[64];
    int s, pc;
    if (!carried(creatng, &s, &pc))
        return "";
    snprintf(text, sizeof(text), "Carries %s: +%d%% %s", ks_names[s], pc, ks_stats[s]);
    return text;
}

/** A bearer's thing, found again by its Misdeeds record when it was frozen and came back (or rose from the grave). */
static struct Thing *find_bearer(struct Keepsake *k)
{
    struct Thing *t = thing_get(k->bearer);
    if (thing_exists(t) && thing_is_creature(t) && (t->creation_turn == k->bearer_born))
    {
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (!creature_control_invalid(cctrl) && (cctrl->misdeed_id == k->bearer_md))
            return t;
    }
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (!creature_control_invalid(cctrl) && (cctrl->misdeed_id == k->bearer_md))
        {
            k->bearer = t->index;
            k->bearer_born = t->creation_turn;
            return t;
        }
    }
    return INVALID_THING;
}

/** KFX_WORLD_KEEPSAKETEST=turn (test switch): at that turn a keepsake drops beside this machine's keeper's Heart and
 *  an Orc named Grimtooth is made there; 300 turns later the keepsake (in storage by then) is given to him as by a
 *  hand drop; 300 turns after that he dies and it drops. Each step is logged with his stats. */
static void ks_test(void)
{
    static long at = -2;
    static ThingIndex orc = 0;
    static int32_t orc_born = 0;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_KEEPSAKETEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_KEEPSAKETEST=%ld", at);
    }
    if (at < 0)
        return;
    /* this machine's keeper, or on a dedicated server the first human keeper with a Heart (a joined player) */
    PlayerNumber p = my_player_number;
    for (PlayerNumber q = 0; netstate_dedicated() && (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
        if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
        {
            p = q;
            break;
        }
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    struct Thing *o = thing_get(orc);
    const TbBool orc_ok = thing_exists(o) && (o->creation_turn == orc_born);
    if ((long)game.play_gameturn == at)
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 6 * COORD_PER_STL;
        world_keepsakes_drop(&pos, 100, 12345, -1);
        struct Thing *t = create_creature(&pos, (ThingModel)get_id(creature_desc, "ORC"), p);
        if (!thing_is_invalid(t))
        {
            snprintf(creature_control_get_from_thing(t)->creature_name, CREATURE_NAME_MAX, "Grimtooth");
            orc = t->index;
            orc_born = t->creation_turn;
        }
        SYNCLOG("World: keepsake test: a keepsake dropped and Grimtooth made");
    } else if (((long)game.play_gameturn == at + 100) && (getenv("KFX_WORLD_KSRELINK") != NULL))
    {
        /* KFX_WORLD_KSRELINK=1 as well: the lying keepsake is made again as a new thing where it lies, as a chunk that
           froze and came back does; it must be found again and still be given at +300 */
        for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
        {
            struct Thing *obj = thing_get(ks[i].obj);
            if (!ks[i].used || (ks[i].bearer_md != 0) || !thing_exists(obj))
                continue;
            struct Coord3d pos = obj->mappos;
            const ThingModel m = obj->model;
            const PlayerNumber own = obj->owner;
            destroy_object(obj);
            struct Thing *n = create_object(&pos, m, own, -1);
            if (!thing_is_invalid(n))
                n->creation_turn += 1;
            SYNCLOG("World: keepsake test: the keepsake is made again as thing %d", thing_is_invalid(n) ? -1 : (int)n->index);
            break;
        }
    } else if (((long)game.play_gameturn == at + 300) && orc_ok)
    {
        struct CreatureControl *cctrl = creature_control_get_from_thing(o);
        SYNCLOG("World: keepsake test: before: Grimtooth (record %d) speed %ld, max health %ld", (int)cctrl->misdeed_id,
            (long)cctrl->max_speed, (long)cctrl->max_health);
        for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
        {
            if (!ks[i].used || (ks[i].bearer_md != 0))
                continue;
            struct Thing *obj = thing_get(ks[i].obj);
            if (!thing_exists(obj))
                continue;
            SYNCLOG("World: keepsake test: the keepsake belongs to player %d and lies %s", (int)ks[i].owner,
                (get_room_thing_is_on(obj) != NULL) ? "in a room" : "on the floor");
            if (getenv("KFX_WORLD_KEEPSAKEHAND") != NULL)
            {
                /* KFX_WORLD_KEEPSAKEHAND=1 as well: the stock Hand: into the keeper's hand, and dropped on him */
                const TbBool picked = place_thing_in_power_hand(obj, p);
                const short dropped = picked ? dump_first_held_thing_on_map(p, o->mappos.x.stl.num, o->mappos.y.stl.num, true) : 0;
                SYNCLOG("World: keepsake test: by the Hand: picked up %d, dropped %d", (int)picked, (int)dropped);
            } else
            {
                move_thing_in_map(obj, &o->mappos); /* as if the Hand dropped it on him */
                world_keepsakes_hand_dropped(get_players_num_dungeon(p), obj);
            }
            break;
        }
        SYNCLOG("World: keepsake test: after: Grimtooth speed %ld, max health %ld, label '%s', sheet '%s'",
            (long)cctrl->max_speed, (long)cctrl->max_health, world_keepsakes_label(o), world_keepsakes_line(o));
    } else if (((long)game.play_gameturn == at + 600) && orc_ok)
    {
        kill_creature(o, INVALID_THING, -1, CrDed_Default);
        SYNCLOG("World: keepsake test: Grimtooth dies");
    } else if ((long)game.play_gameturn == at + 700)
    {
        int lying = 0, carriedn = 0;
        for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
        {
            lying += (ks[i].used && (ks[i].bearer_md == 0)) ? 1 : 0;
            carriedn += (ks[i].used && (ks[i].bearer_md != 0)) ? 1 : 0;
        }
        SYNCLOG("World: keepsake test: %d keepsake(s) lying, %d carried", lying, carriedn);
    }
}

void world_keepsakes_turn(void)
{
    if (!simulating())
        return;
    ks_test();
    if ((game.play_gameturn % 32) != 19)
        return;
    for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
    {
        struct Keepsake *k = &ks[i];
        if (!k->used)
            continue;
        if (k->bearer_md != 0)
        {
            /* carried: its copy in struct Game follows the bearer (a frozen bearer is not on the map: kept) */
            struct Thing *t = find_bearer(k);
            if (thing_exists(t))
                set_view(t, k);
            continue;
        }
        struct Thing *obj = thing_get(k->obj);
        if (!thing_exists(obj) || (obj->creation_turn != k->obj_born))
        {
            /* frozen with its chunk (it waits), back as a new thing (found where it lay), or gone */
            MapSlabCoord ex, ey;
            if ((k->obj_model > 0) && !world_engine_slab(k->wx, k->wy, &ex, &ey))
                continue;
            obj = (k->obj_model > 0) ? find_base_thing_on_mapwho(TCls_Object, k->obj_model, slab_subtile(ex, k->sub_x),
                slab_subtile(ey, k->sub_y)) : INVALID_THING;
            if (thing_is_invalid(obj) || (ks_of_object(obj) != NULL))
            {
                memset(k, 0, sizeof(*k)); /* gone */
                continue;
            }
            k->obj = obj->index;
            k->obj_born = obj->creation_turn;
            world_storage_add_loot(obj, (PlayerNumber)k->owner);
            SYNCLOG("World: a keepsake (%s) is found again where it lay", ks_names[k->stat]);
        }
        if (!thing_is_picked_up(obj))
            ks_note_place(k, obj);
        if ((k->owner != PLAYER_NEUTRAL) || thing_is_picked_up(obj))
            continue;
        /* lying in the world: the first keeper whose creature is within two subtiles takes it home */
        const MapSubtlCoord cx = obj->mappos.x.stl.num, cy = obj->mappos.y.stl.num;
        PlayerNumber taker = -1;
        for (MapSubtlCoord y = cy - 2; (y <= cy + 2) && (taker < 0); y++)
        {
            for (MapSubtlCoord x = cx - 2; (x <= cx + 2) && (taker < 0); x++)
            {
                if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                    continue;
                long j = get_mapwho_thing_index(get_map_block_at(x, y));
                unsigned long guard = 0;
                while ((j != 0) && (guard++ < THINGS_COUNT))
                {
                    const struct Thing *t = thing_get(j);
                    if (thing_is_invalid(t))
                        break;
                    j = t->next_on_mapblk;
                    if (!thing_is_creature(t) || (t->owner >= PLAYERS_COUNT) || !world_player_is_human(t->owner) ||
                        world_caretaker_active(t->owner))
                        continue; /* (an away keeper's creatures take nothing home) */
                    if ((k->last_owner != PLAYER_NEUTRAL) && (k->last_owner != t->owner) && players_are_mutual_allies(k->last_owner, t->owner))
                        continue; /* never an ally of the keeper who had it */
                    taker = t->owner;
                    break;
                }
            }
        }
        if (taker < 0)
            continue;
        k->owner = (uint8_t)taker;
        world_storage_add_loot(obj, taker);
        SYNCLOG("World: player %d takes a keepsake (%s) home", (int)taker, ks_names[k->stat]);
    }
}

/******************************************************************************/
void world_keepsakes_reset(void)
{
    memset(ks, 0, sizeof(ks));
    memset(kview, 0, sizeof(kview));
    memset(kborn, 0, sizeof(kborn));
}

size_t world_keepsakes_save_size(void)
{
    int n = 0;
    for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
        n += ks[i].used ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct Keepsake) : 0;
}

void world_keepsakes_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < WORLD_KEEPSAKES_MAX; i++)
    {
        if (!ks[i].used)
            continue;
        memcpy(buf + at, &ks[i], sizeof(struct Keepsake));
        at += sizeof(struct Keepsake);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_keepsakes_load(const uint8_t *buf, size_t len)
{
    world_keepsakes_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct Keepsake) + 4 > len)
        return false;
    for (uint32_t i = 0; (i < n) && (i < WORLD_KEEPSAKES_MAX); i++)
        memcpy(&ks[i], buf + 4 + (size_t)i * sizeof(struct Keepsake), sizeof(struct Keepsake));
    return true; /* the copies in struct Game are made again at the next pass */
}

#ifdef __cplusplus
}
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_heresy.c
 *     Endless world: heresies.
 * @par Purpose:
 *     See world_heresy.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_heresy.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "world_stream.h"
#include "world_daynight.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "config_objects.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "map_blocks.h"
#include "room_util.h"
#include "world_caretaker.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static const char *const heresy_names[WHe_COUNT] = {"", "Open Graves", "Company Scrip", "Night Court", "One Kind"};
static const char *const heresy_lines[WHe_COUNT] = {"",
    "no Lairs are wanted, but sleep heals at half speed",
    "no wages, but everyone grows hungry twice as fast",
    "double work by night, half work by day",
    "the portals bring only the kind you name (or your most common), five levels up"};

struct HeresyKeeper {
    uint8_t tome[HERESY_TOMES_PER_KEEPER];      /* the heresy of each tome held (0: none) */
    uint8_t adopted[HERESY_TOMES_PER_KEEPER];   /* 1: adopted */
    ThingModel one_kind;                        /* One Kind: the kind chosen when it was adopted */
};
static struct HeresyKeeper hk[PLAYERS_MAX];

#define HERESY_LYING_MAX 64
/* A lying tome is known by its thing and by its place: in a chunk that freezes and comes back it is a new thing, found
 * again where it lies (2026-11-13; before, it was forgotten and came back as a plain potion). */
struct LyingTome {
    ThingIndex obj;
    int32_t born;
    uint8_t heresy;
    uint8_t sub_x, sub_y;   /* its subtile within its slab */
    uint8_t pad;
    int64_t wx, wy;         /* its world slab */
};
static struct LyingTome lt[HERESY_LYING_MAX];

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: heresy, to player %d: %s", (int)p, text);
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

TbBool world_heresy_has(PlayerNumber p, int heresy)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_MAX) || world_caretaker_active(p)) /* away: the usual rules */
        return false;
    for (int i = 0; i < HERESY_TOMES_PER_KEEPER; i++)
        if (hk[p].adopted[i] && (hk[p].tome[i] == heresy))
            return true;
    return false;
}

TbBool world_heresy_no_lair(PlayerNumber p)
{
    return world_heresy_has(p, WHe_OpenGraves);
}

int world_heresy_heal_pct(PlayerNumber p)
{
    return world_heresy_has(p, WHe_OpenGraves) ? -50 : 0;
}

GoldAmount world_heresy_pay(const struct Thing *creatng, GoldAmount pay)
{
    return (thing_exists(creatng) && world_heresy_has(creatng->owner, WHe_CompanyScrip)) ? 0 : pay;
}

int world_heresy_hunger_step(const struct Thing *creatng)
{
    return (thing_exists(creatng) && world_heresy_has(creatng->owner, WHe_CompanyScrip)) ? 2 : 1;
}

long world_heresy_work(const struct Thing *creatng, long value)
{
    if (!thing_exists(creatng) || !world_heresy_has(creatng->owner, WHe_NightCourt))
        return value;
    return world_is_day() ? value / 2 : value * 2;
}

void world_heresy_stronghold_destroyed(PlayerNumber by, const struct Coord3d *pos)
{
    if (!simulating() || (by < 0) || (by >= PLAYERS_MAX) || (pos == NULL))
        return;
    const struct Thing *heart = get_player_soul_container(by);
    if (!thing_exists(heart))
        return;
    const long dx = subtile_slab(pos->x.stl.num) - subtile_slab(heart->mappos.x.stl.num);
    const long dy = subtile_slab(pos->y.stl.num) - subtile_slab(heart->mappos.y.stl.num);
    const TbBool forced = (getenv("KFX_WORLD_HERESYTEST") != NULL);
    if (!forced && (dx * dx + dy * dy < (long)HERESY_FAR_SLABS * HERESY_FAR_SLABS))
        return; /* only far from home */
    const uint32_t h = (uint32_t)(game.play_gameturn * 2654435761U) ^ (uint32_t)(dx * 31 + dy);
    if (!forced && ((h >> 8) % 4 != 0))
        return;
    struct LyingTome *t = NULL;
    for (int i = 0; (t == NULL) && (i < HERESY_LYING_MAX); i++)
        if (lt[i].obj == 0)
            t = &lt[i];
    if (t == NULL)
        return;
    struct Coord3d at = *pos;
    at.z.val = 0;
    at.z.val = get_floor_height_at(&at);
    struct Thing *obj = create_object(&at, (ThingModel)get_id(object_desc, "POTION_PURPLE"), game.neutral_player_num, -1);
    if (thing_is_invalid(obj))
        return;
    t->obj = obj->index;
    t->born = obj->creation_turn;
    t->heresy = (uint8_t)(1 + (h >> 16) % (WHe_COUNT - 1));
    world_slab_to_world(subtile_slab(obj->mappos.x.stl.num), subtile_slab(obj->mappos.y.stl.num), &t->wx, &t->wy);
    t->sub_x = (uint8_t)(obj->mappos.x.stl.num % STL_PER_SLB);
    t->sub_y = (uint8_t)(obj->mappos.y.stl.num % STL_PER_SLB);
    SYNCLOG("World: a heretical tome lies where a stronghold fell");
}

/** The keeper's most common creature kind, imps aside (0: none). */
static ThingModel most_common_kind(PlayerNumber p)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return 0;
    ThingModel best = 0;
    int best_n = 0;
    for (ThingModel m = 1; m < game.conf.crtr_conf.model_count; m++)
    {
        if (flag_is_set(creature_stats_get(m)->model_flags, CMF_IsSpecDigger) || flag_is_set(creature_stats_get(m)->model_flags, CMF_IsSpectator)
            || (m == get_players_special_digger_model(p)))
            continue;
        const int n = dungeon->owned_creatures_of_model[m];
        if (n > best_n)
        {
            best = m;
            best_n = n;
        }
    }
    return best;
}

ThingModel world_heresy_portal_kind(PlayerNumber plyr_idx, ThingModel crmodel)
{
    if ((crmodel <= 0) || !world_mode_active() || !world_heresy_has(plyr_idx, WHe_OneKind))
        return crmodel;
    const ThingModel kind = hk[plyr_idx].one_kind;
    return (kind > 0) ? kind : crmodel;
}

void world_heresy_portal_arrived(struct Thing *creatng)
{
    if (!thing_exists(creatng) || !world_mode_active() || (creatng->owner >= PLAYERS_MAX)
        || !world_heresy_has(creatng->owner, WHe_OneKind) || (creatng->model != hk[creatng->owner].one_kind))
        return;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    const int to = min((int)cctrl->exp_level + HERESY_ONE_KIND_LEVELS, CREATURE_MAX_LEVEL - 1);
    set_creature_level(creatng, to);
    SYNCLOG("World: One Kind: player %d's portal brings %s at level %d", (int)creatng->owner, thing_model_name(creatng), to + 1);
}

static void tell_list(PlayerNumber p)
{
    char text[220];
    int n = snprintf(text, sizeof(text), "Your tomes:");
    int any = 0;
    for (int i = 0; i < HERESY_TOMES_PER_KEEPER; i++)
    {
        if (hk[p].tome[i] == 0)
            continue;
        any = 1;
        n += snprintf(text + n, sizeof(text) - n, " %d. %s%s (%s);", i + 1, heresy_names[hk[p].tome[i]],
            hk[p].adopted[i] ? " [kept]" : "", heresy_lines[hk[p].tome[i]]);
        if (n >= (int)sizeof(text) - 2)
            break;
    }
    tell_keeper(p, any ? text : "You hold no heretical tomes. They lie where strongholds fall, far from home.");
}

void world_heresy_packet(PlayerNumber p, int op, int n)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    const ThingModel named = (ThingModel)((n >> 8) & 0xFFFF); /* One Kind's kind, named in "take" (0: none) */
    n &= 0xFF;
    struct HeresyKeeper *k = &hk[p];
    if (op == 1)
    {
        tell_list(p);
        return;
    }
    const int i = n - 1;
    if ((i < 0) || (i >= HERESY_TOMES_PER_KEEPER) || (k->tome[i] == 0))
    {
        tell_keeper(p, "No such tome (\"!heresy\" lists yours).");
        return;
    }
    char text[160];
    if (op == 2)
    {
        int adopted = 0;
        for (int j = 0; j < HERESY_TOMES_PER_KEEPER; j++)
            adopted += k->adopted[j] ? 1 : 0;
        if (k->adopted[i])
            return;
        if (adopted >= HERESY_ADOPTED_MAX)
        {
            tell_keeper(p, "A dungeon holds two heresies at most: burn one at the Temple first.");
            return;
        }
        if (k->tome[i] == WHe_OneKind)
        {
            /* the kind is the keeper's most common creature (imps aside) at the moment it is adopted */
            ThingModel kind = most_common_kind(p);
            if (named > 0)
            {
                /* the kind the keeper named: only one its dungeon has (its portals brought it), never heroes or
                   anything a forged packet names */
                const struct Dungeon *dg = get_players_num_dungeon(p);
                if ((named >= game.conf.crtr_conf.model_count) || dungeon_invalid(dg)
                    || flag_is_set(creature_stats_get(named)->model_flags, CMF_IsSpecDigger)
                    || flag_is_set(creature_stats_get(named)->model_flags, CMF_IsSpectator)
                    || (named == get_players_special_digger_model(p)) || (dg->owned_creatures_of_model[named] <= 0))
                {
                    tell_keeper(p, "One Kind takes a kind your dungeon already has: name one of your own creatures.");
                    return;
                }
                kind = named;
            }
            if (kind <= 0)
            {
                tell_keeper(p, "One Kind needs a kind: \"!heresy take N <kind>\" (or creatures of yours to choose from).");
                return;
            }
            k->one_kind = kind;
            k->adopted[i] = 1;
            snprintf(text, sizeof(text), "Your dungeon keeps One Kind: the portals bring only %s now, five levels up.",
                get_string(creature_stats_get(kind)->namestr_idx));
            tell_keeper(p, text);
            return;
        }
        k->adopted[i] = 1;
        snprintf(text, sizeof(text), "Your dungeon keeps %s: %s.", heresy_names[k->tome[i]], heresy_lines[k->tome[i]]);
        tell_keeper(p, text);
        return;
    }
    if (op == 3)
    {
        const struct Dungeon *dungeon = get_players_num_dungeon(p);
        if (dungeon_invalid(dungeon) || (dungeon->room_list_start[RoK_TEMPLE] == 0))
        {
            tell_keeper(p, "A tome is burned at the Temple: you have none.");
            return;
        }
        snprintf(text, sizeof(text), "%s burns at your Temple, and your dungeon is as it was.", heresy_names[k->tome[i]]);
        k->tome[i] = 0;
        k->adopted[i] = 0;
        tell_keeper(p, text);
    }
}

TbBool world_heresy_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strncasecmp(text + 1, "heresy", 6) != 0)
        return false;
    const char *arg = text + 7;
    if ((*arg != '\0') && (*arg != ' '))
        return false;
    while (*arg == ' ')
        arg++;
    int op = 1, n = 0;
    if (strncasecmp(arg, "take", 4) == 0)
    {
        op = 2;
        n = atoi(arg + 4);
        /* One Kind: "!heresy take N <kind>" names the kind (a creature's name as the game writes it, e.g. Troll) */
        const char *kind_arg = arg + 4;
        while (*kind_arg == ' ')
            kind_arg++;
        while ((*kind_arg >= '0') && (*kind_arg <= '9'))
            kind_arg++;
        while (*kind_arg == ' ')
            kind_arg++;
        if (*kind_arg != '\0')
        {
            for (ThingModel m = 1; m < game.conf.crtr_conf.model_count; m++)
            {
                const char *name = get_string(creature_stats_get(m)->namestr_idx);
                if ((name != NULL) && (strcasecmp(name, kind_arg) == 0))
                {
                    n |= (int)m << 8;
                    break;
                }
            }
        }
    } else if (strncasecmp(arg, "burn", 4) == 0)
    {
        op = 3;
        n = atoi(arg + 4);
    }
    set_players_packet_action(get_my_player(), PckA_WorldHeresy, (unsigned long)op, (unsigned long)n, 0, 0);
    return true;
}

/** KFX_WORLD_HERESYTEST=turn (test switch): a stronghold "falls" beside this machine's Heart (the tome always drops),
 *  and 100 turns later the keeper lists, takes and burns tomes; the wages and hunger of a creature are logged. */
static void heresy_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_HERESYTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_HERESYTEST=%ld", at);
    }
    if (at < 0)
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    if ((long)game.play_gameturn == at)
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 6 * COORD_PER_STL;
        world_heresy_stronghold_destroyed(p, &pos);
        if (getenv("KFX_WORLD_TOMERELINK") != NULL)
        {
            /* KFX_WORLD_TOMERELINK=1 as well: the tome is made again as a new thing where it lies, as a chunk that froze
               and came back does; it must still be found and taken */
            for (int i = 0; i < HERESY_LYING_MAX; i++)
            {
                struct Thing *o = thing_get(lt[i].obj);
                if ((lt[i].obj == 0) || !thing_exists(o))
                    continue;
                struct Coord3d at2 = o->mappos;
                const ThingModel m = o->model;
                destroy_object(o);
                struct Thing *n = create_object(&at2, m, game.neutral_player_num, -1);
                if (!thing_is_invalid(n))
                    n->creation_turn += 1;
                SYNCLOG("World: heresy test: the lying tome is made again as thing %d", thing_is_invalid(n) ? -1 : (int)n->index);
            }
        }
    } else if ((long)game.play_gameturn == at + 100)
    {
        world_heresy_packet(p, 1, 0);
        /* the test keeper takes whatever it found as Company Scrip, to read wages */
        if (hk[p].tome[0] != 0)
            hk[p].tome[0] = (getenv("KFX_WORLD_ONEKIND") != NULL) ? WHe_OneKind : WHe_CompanyScrip; /* (KFX_WORLD_ONEKIND=1: One Kind) */
        if (getenv("KFX_WORLD_ONEKIND") != NULL)
        {
            /* One Kind chooses the most common kind: two Trolls and an Orc beside the Heart first */
            static const char *const kinds[3] = {"TROLL", "TROLL", "ORC"};
            for (int c = 0; c < 3; c++)
            {
                struct Coord3d cpos = heart->mappos;
                cpos.y.val += 6 * COORD_PER_STL;
                create_creature(&cpos, (ThingModel)get_id(creature_desc, kinds[c]), p);
            }
        }
        if (getenv("KFX_WORLD_ONEKINDNAME") != NULL)
        {
            /* KFX_WORLD_ONEKINDNAME=<kind> as well: taken through the chat command, naming the kind */
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "!heresy take 1 %s", getenv("KFX_WORLD_ONEKINDNAME"));
            world_heresy_command(cmd);
        } else
            world_heresy_packet(p, 2, 1);
        SYNCLOG("World: heresy test: Company Scrip %d, Night Court %d, Open Graves %d", (int)world_heresy_has(p, WHe_CompanyScrip),
            (int)world_heresy_has(p, WHe_NightCourt), (int)world_heresy_has(p, WHe_OpenGraves));
    } else if (((long)game.play_gameturn == at + 150) && (getenv("KFX_WORLD_HERESYTEMPLE") != NULL))
    {
        /* KFX_WORLD_HERESYTEMPLE=1 as well: a 3x3 Temple below the Heart before the burning */
        const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
            {
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1 + dx), slab_subtile_center(hy + 6 + dy), p, 0);
                replace_slab_from_script(hx - 1 + dx, hy + 6 + dy, SlbT_TEMPLE);
            }
        SYNCLOG("World: heresy test: a Temple is laid below the Heart");
    } else if ((long)game.play_gameturn == at + 200)
    {
        world_heresy_packet(p, 3, 1);
        SYNCLOG("World: heresy test: after burning, Company Scrip %d", (int)world_heresy_has(p, WHe_CompanyScrip));
    }
}

void world_heresy_turn(void)
{
    if (!simulating())
        return;
    heresy_test();
    if ((game.play_gameturn % 16) != 11)
        return;
    for (int i = 0; i < HERESY_LYING_MAX; i++)
    {
        struct LyingTome *t = &lt[i];
        if (t->obj == 0)
            continue;
        struct Thing *obj = thing_get(t->obj);
        if (!thing_exists(obj) || (obj->creation_turn != t->born))
        {
            /* frozen with its chunk (it waits), or back as a new thing (found where it lies), or gone */
            MapSlabCoord ex, ey;
            if (!world_engine_slab(t->wx, t->wy, &ex, &ey))
                continue;
            obj = find_base_thing_on_mapwho(TCls_Object, (ThingModel)get_id(object_desc, "POTION_PURPLE"),
                slab_subtile(ex, t->sub_x), slab_subtile(ey, t->sub_y));
            if (thing_is_invalid(obj))
            {
                memset(t, 0, sizeof(*t));
                continue;
            }
            t->obj = obj->index;
            t->born = obj->creation_turn;
            SYNCLOG("World: a lying heretical tome is found again where it lay");
        }
        /* the first keeper whose creature is within two slabs takes it */
        const MapSubtlCoord cx = obj->mappos.x.stl.num, cy = obj->mappos.y.stl.num;
        PlayerNumber taker = -1;
        for (MapSubtlCoord y = cy - 6; (y <= cy + 6) && (taker < 0); y++)
            for (MapSubtlCoord x = cx - 6; (x <= cx + 6) && (taker < 0); x++)
            {
                if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                    continue;
                long j = get_mapwho_thing_index(get_map_block_at(x, y));
                unsigned long guard = 0;
                while ((j != 0) && (guard++ < THINGS_COUNT))
                {
                    const struct Thing *c = thing_get(j);
                    if (thing_is_invalid(c))
                        break;
                    j = c->next_on_mapblk;
                    if (thing_is_creature(c) && (c->owner < PLAYERS_COUNT) && (c->owner < PLAYERS_MAX) && world_player_is_human(c->owner))
                    {
                        taker = c->owner;
                        break;
                    }
                }
            }
        if (taker < 0)
            continue;
        struct HeresyKeeper *k = &hk[taker];
        int slot = -1;
        for (int s = 0; (slot < 0) && (s < HERESY_TOMES_PER_KEEPER); s++)
            if (k->tome[s] == 0)
                slot = s;
        if (slot < 0)
            continue; /* its shelf is full: it lies */
        k->tome[slot] = t->heresy;
        k->adopted[slot] = 0;
        delete_thing_structure(obj, 0);
        memset(t, 0, sizeof(*t));
        char text[160];
        snprintf(text, sizeof(text), "Your creatures found a heretical tome: %s (\"!heresy take %d\" to keep it).",
            heresy_names[k->tome[slot]], slot + 1);
        tell_keeper(taker, text);
    }
}

/******************************************************************************/
void world_heresy_reset(void)
{
    memset(hk, 0, sizeof(hk));
    memset(lt, 0, sizeof(lt));
}

size_t world_heresy_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int i = 0; i < HERESY_TOMES_PER_KEEPER; i++)
            if (hk[p].tome[i] != 0)
                return 8 + sizeof(hk) + sizeof(lt);
    for (int i = 0; i < HERESY_LYING_MAX; i++)
        if (lt[i].obj != 0)
            return 8 + sizeof(hk) + sizeof(lt);
    return 0;
}

void world_heresy_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX, m = HERESY_LYING_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, &m, 4);
    memcpy(buf + 8, hk, sizeof(hk));
    memcpy(buf + 8 + sizeof(hk), lt, sizeof(lt));
}

TbBool world_heresy_load(const uint8_t *buf, size_t len)
{
    world_heresy_reset();
    uint32_t n = 0, m = 0;
    if (len < 8)
        return false;
    memcpy(&n, buf, 4);
    memcpy(&m, buf + 4, 4);
    if ((n != PLAYERS_MAX) || (m != HERESY_LYING_MAX) || (8 + sizeof(hk) + sizeof(lt) > len))
        return false;
    memcpy(hk, buf + 8, sizeof(hk));
    memcpy(lt, buf + 8 + sizeof(hk), sizeof(lt));
    return true;
}

void world_heresy_seat_reset(PlayerNumber p)
{
    if ((p >= 0) && (p < PLAYERS_MAX))
        memset(&hk[p], 0, sizeof(hk[p]));
}

#ifdef __cplusplus
}
#endif

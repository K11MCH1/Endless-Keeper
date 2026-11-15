/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_auction.c
 *     Endless world: The Auction House (2026-11-13; see world_auction.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - In-game commands on a dedicated server (the stall next to the portal is not drawn yet):
 *       "!sell creature <kind> <price>" puts your best creature of that kind up, "!sell hero <kind> <price>" a hero of
 *       that kind from your Prison; "!market" lists what is for sale; "!buy <number>" buys; "!unsell <number>" takes
 *       yours back. Set prices, no bidding.
 *     - Prices: 200 to 2,000 gold for each level of what is sold. A keeper buys at most 20,000 gold of goods a day
 *       (the world's day). A seller needs a portal; a buyer a portal (creatures) or a Prison with room (heroes).
 *     - Delivery: a bought creature becomes the buyer's and steps out of the buyer's portal; a bought hero is put in
 *       the buyer's Prison. Nothing on offer is taken from the seller until it is bought: it goes on working.
 *     - Spellbooks and special boxes are not built yet.
 *     Only human keepers.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_auction.h"
#include "world_stream.h"
#include "world_daynight.h"
#include <string.h>
#include <strings.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "map_blocks.h"
#include "net_game.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "room_data.h"
#include "server_admin.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "config_strings.h"
#include "post_inc.h"

/** A creature kind's name as players read it (Troll), else its code name (TROLL). */
static const char *kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *s = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((s != NULL) && (s[0] != '\0')) ? s : creature_code_name(model);
}

#define AU_MAX 64
#define AU_PRICE_MIN 200       /* gold for each level */
#define AU_PRICE_MAX 2000
#define AU_DAY_LIMIT 20000     /* gold a keeper may spend in a day */

enum AuKind { Au_None = 0, Au_Creature, Au_Hero };

struct AuOffer {
    int16_t seller;
    uint8_t kind;
    uint8_t pad;
    int32_t thing, thing_turn;
    int32_t price;
};
static struct AuOffer offers[AU_MAX];
struct AuSpent {
    int32_t day;
    int32_t gold;
};
static struct AuSpent spent[PLAYERS_MAX];

static void au_tell(PlayerNumber p, const char *text)
{
    if (!netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static const char *au_name(PlayerNumber p)
{
    const char *n = server_admin_player_name(p);
    return ((n != NULL) && (n[0] != '\0')) ? n : "a keeper";
}

static struct Thing *au_thing(const struct AuOffer *o)
{
    struct Thing *t = thing_get(o->thing);
    if (!thing_exists(t) || !thing_is_creature(t) || (t->creation_turn != o->thing_turn) || creature_is_dying(t))
        return INVALID_THING;
    return t;
}

static TbBool au_listed(const struct Thing *t)
{
    for (int i = 0; i < AU_MAX; i++)
        if ((offers[i].kind != Au_None) && (offers[i].thing == t->index) && (offers[i].thing_turn == t->creation_turn))
            return true;
    return false;
}

static TbBool au_in_prison_of(const struct Thing *t, PlayerNumber holder)
{
    const struct Room *room = get_room_thing_is_on(t);
    return room_exists(room) && (room->owner == holder) && room_role_matches(room->kind, RoRoF_Prison) &&
        creature_is_kept_in_prison(t);
}

/** Is the offer still good: the creature the seller's (working, not carried), or the hero still in the seller's Prison? */
static TbBool au_valid(const struct AuOffer *o, struct Thing **out)
{
    struct Thing *t = au_thing(o);
    if (thing_is_invalid(t) || thing_is_picked_up(t) || ((t->alloc_flags & TAlF_IsControlled) != 0))
        return false;
    if (o->kind == Au_Creature)
    {
        if ((t->owner != o->seller) || creature_is_being_unconscious(t) || creature_is_kept_in_custody(t))
            return false;
    } else if ((t->owner != PLAYER_GOOD) || !au_in_prison_of(t, o->seller))
        return false;
    if (out != NULL)
        *out = t;
    return true;
}

static struct Room *au_prison(PlayerNumber p)
{
    for (int i = 1; i < ROOMS_COUNT; i++)
    {
        struct Room *room = room_get(i);
        if (room_exists(room) && (room->owner == p) && room_role_matches(room->kind, RoRoF_Prison) &&
            (room->used_capacity < room->total_capacity))
            return room;
    }
    return INVALID_ROOM;
}

/** The thing to sell: the seller's best creature of the kind (not on offer already), or a hero of the kind in its
 *  Prison. */
static struct Thing *au_pick(PlayerNumber seller, int kind, ThingModel model)
{
    struct Thing *best = INVALID_THING;
    int best_lv = -1;
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *t = thing_get(i);
        if (thing_is_invalid(t))
            break;
        i = t->next_of_class;
        if ((t->model != model) || au_listed(t) || thing_is_picked_up(t) || creature_is_dying(t) ||
            ((t->alloc_flags & TAlF_IsControlled) != 0))
            continue;
        if (kind == Au_Creature)
        {
            if ((t->owner != seller) || creature_is_being_unconscious(t) || creature_is_kept_in_custody(t))
                continue;
        } else if ((t->owner != PLAYER_GOOD) || !au_in_prison_of(t, seller))
            continue;
        const int lv = creature_control_get_from_thing(t)->exp_level;
        if (lv > best_lv)
        {
            best = t;
            best_lv = lv;
        }
    }
    return best;
}

static long au_spent_today(PlayerNumber p)
{
    if (spent[p].day != (int32_t)world_day_number())
    {
        spent[p].day = (int32_t)world_day_number();
        spent[p].gold = 0;
    }
    return spent[p].gold;
}

static const char *au_sell(PlayerNumber seller, int kind, const char *model_name, long price, int *slot_out)
{
    const ThingModel model = (ThingModel)get_id(creature_desc, model_name);
    if (model <= 0)
        return "no creature of that kind (use its name, as TROLL or KNIGHT)";
    if (room_is_invalid(pick_random_room_of_role(seller, RoRoF_CrPoolSpawn)))
        return "you need a portal to sell from";
    struct Thing *t = au_pick(seller, kind, model);
    if (thing_is_invalid(t))
        return (kind == Au_Creature) ? "you have no such creature free to sell" : "you hold no such hero in your Prison";
    const long lv = creature_control_get_from_thing(t)->exp_level + 1;
    if ((price < AU_PRICE_MIN * lv) || (price > AU_PRICE_MAX * lv))
        return "the price must be 200 to 2,000 gold for each level";
    int slot = -1;
    for (int i = 0; (i < AU_MAX) && (slot < 0); i++)
        if (offers[i].kind == Au_None)
            slot = i;
    if (slot < 0)
        return "the market is full";
    offers[slot].seller = (int16_t)seller;
    offers[slot].kind = (uint8_t)kind;
    offers[slot].thing = t->index;
    offers[slot].thing_turn = t->creation_turn;
    offers[slot].price = (int32_t)price;
    if (slot_out != NULL)
        *slot_out = slot;
    SYNCLOG("World auction: player %d sells a %s (level %ld) for %ld gold (offer %d)", (int)seller, thing_model_name(t), lv,
        price, slot + 1);
    return NULL;
}

static const char *au_buy(PlayerNumber buyer, int slot)
{
    if ((slot < 0) || (slot >= AU_MAX) || (offers[slot].kind == Au_None))
        return "no such offer";
    struct AuOffer *o = &offers[slot];
    if (o->seller == buyer)
        return "that offer is your own";
    struct Thing *t = INVALID_THING;
    if (!au_valid(o, &t))
    {
        memset(o, 0, sizeof(*o));
        return "that offer is gone";
    }
    if (au_spent_today(buyer) + o->price > AU_DAY_LIMIT)
        return "you can buy only 20,000 gold of goods a day";
    if (get_dungeon(buyer)->total_money_owned < o->price)
        return "you have not the gold";
    struct Coord3d pos;
    if (o->kind == Au_Creature)
    {
        struct Room *portal = pick_random_room_of_role(buyer, RoRoF_CrPoolSpawn);
        if (room_is_invalid(portal) || !find_random_valid_position_for_thing_in_room(t, portal, &pos))
            return "you need a portal for it to come through";
    } else
    {
        struct Room *prison = au_prison(buyer);
        if (room_is_invalid(prison) || !find_random_valid_position_for_thing_in_room(t, prison, &pos))
            return "you need a Prison with room for it";
    }
    if (take_money_from_dungeon(buyer, o->price, 1) < 0)
        return "you have not the gold";
    spent[buyer].gold += o->price;
    player_add_offmap_gold(o->seller, o->price);
    if (o->kind == Au_Creature)
    {
        initialise_thing_state(t, CrSt_CreatureDoingNothing);
        change_creature_owner(t, buyer);
        move_thing_in_map(t, &pos);
        set_start_state(t);
    } else
    {
        initialise_thing_state(t, CrSt_CreatureArrivedAtPrison);
        move_thing_in_map(t, &pos);
    }
    char msg[200];
    snprintf(msg, sizeof(msg), "%s buys your %s for %ld gold.", au_name(buyer), kind_name(t->model), (long)o->price);
    au_tell(o->seller, msg);
    SYNCLOG("World auction: player %d buys player %d's %s for %ld gold (%s)", (int)buyer, (int)o->seller,
        thing_model_name(t), (long)o->price, (o->kind == Au_Creature) ? "through its portal" : "into its Prison");
    memset(o, 0, sizeof(*o));
    return NULL;
}

TbBool world_auction_command(int user, PlayerNumber me, const char *word, const char *args)
{
    if (!world_mode_active())
        return false;
    char a1[32] = "", a2[64] = "", a3[32] = "";
    sscanf(args, "%31s %63s %31s", a1, a2, a3);
    char msg[220];
    if ((me < 0) || (me >= PLAYERS_COUNT))
    {
        server_admin_notice(user, "Only a seated keeper trades.");
        return true;
    }
    if (strcasecmp(word, "market") == 0)
    {
        int n = 0;
        for (int i = 0; i < AU_MAX; i++)
        {
            struct Thing *t = INVALID_THING;
            if (offers[i].kind == Au_None)
                continue;
            if (!au_valid(&offers[i], &t))
            {
                memset(&offers[i], 0, sizeof(offers[i]));
                continue;
            }
            snprintf(msg, sizeof(msg), "%d: %s%s (%s) level %d from %s, %ld gold", i + 1,
                (offers[i].kind == Au_Hero) ? "captive " : "", kind_name(t->model), creature_code_name(t->model),
                creature_control_get_from_thing(t)->exp_level + 1, au_name(offers[i].seller), (long)offers[i].price);
            server_admin_notice(user, msg);
            n++;
        }
        if (n == 0)
            server_admin_notice(user, "Nothing for sale. !sell creature|hero <kind> <price> puts something up.");
        snprintf(msg, sizeof(msg), "You may spend %ld more gold today.", AU_DAY_LIMIT - au_spent_today(me));
        server_admin_notice(user, msg);
        return true;
    }
    if (strcasecmp(word, "sell") == 0)
    {
        const int kind = (strcasecmp(a1, "creature") == 0) ? Au_Creature : ((strcasecmp(a1, "hero") == 0) ? Au_Hero : Au_None);
        if ((kind == Au_None) || (a2[0] == '\0'))
        {
            server_admin_notice(user, "!sell creature <kind> <price>, or !sell hero <kind> <price> (a hero from your Prison).");
            return true;
        }
        int slot = -1;
        const char *why = au_sell(me, kind, a2, atol(a3), &slot);
        if (why != NULL)
            snprintf(msg, sizeof(msg), "Not for sale: %s.", why);
        else
            snprintf(msg, sizeof(msg), "Offer %d is up at your stall: any keeper may buy it with !buy %d.", slot + 1, slot + 1);
        server_admin_notice(user, msg);
        return true;
    }
    if (strcasecmp(word, "buy") == 0)
    {
        const char *why = au_buy(me, atoi(a1) - 1);
        if (why != NULL)
            snprintf(msg, sizeof(msg), "No sale: %s.", why);
        else
            snprintf(msg, sizeof(msg), "Bought: it comes through your portal (a hero to your Prison).");
        server_admin_notice(user, msg);
        return true;
    }
    if (strcasecmp(word, "unsell") == 0)
    {
        const int slot = atoi(a1) - 1;
        if ((slot >= 0) && (slot < AU_MAX) && (offers[slot].kind != Au_None) && (offers[slot].seller == me))
        {
            memset(&offers[slot], 0, sizeof(offers[slot]));
            server_admin_notice(user, "Taken back from your stall.");
        } else
            server_admin_notice(user, "No such offer of yours.");
        return true;
    }
    return false;
}

/** Test hook KFX_WORLD_AUCTIONTEST=T (server): at T the first two human keepers get what they lack (a portal each, a
 *  Prison each, a Troll for the first and a Knight in the first's Prison); at T+20 the first puts both up and the
 *  second buys both. */
static void au_test(void)
{
    static long at = -1;
    static int cmd = 0;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_AUCTIONTEST");
        at = (v != NULL) ? atol(v) : 0;
        const char *c = getenv("KFX_WORLD_AUCTIONCMD");
        if ((c != NULL) && (atol(c) > 0))
        {
            at = atol(c);
            cmd = 1;
        }
    }
    if ((at <= 0) || (((long)game.play_gameturn != at) && ((long)game.play_gameturn != at + 20) &&
        ((long)game.play_gameturn != at + 40)))
        return;
    PlayerNumber two[2] = {PLAYER_NONE, PLAYER_NONE};
    int n = 0;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (n < 2); p++)
        if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)))
            two[n++] = p;
    if (n < 2)
    {
        SYNCLOG("World auction test: fewer than two keepers");
        return;
    }
    if ((long)game.play_gameturn == at)
    {
        const RoomKind entk = (RoomKind)get_id(room_desc, "ENTRANCE");
        const RoomKind prisk = find_first_roomkind_with_role(RoRoF_Prison);
        for (int k = 0; k < 2; k++)
        {
            const PlayerNumber q = two[k];
            struct Thing *qh = get_player_soul_container(q);
            const MapSlabCoord hx = subtile_slab(qh->mappos.x.stl.num), hy = subtile_slab(qh->mappos.y.stl.num);
            const TbBool has_portal = !room_is_invalid(pick_random_room_of_role(q, RoRoF_CrPoolSpawn));
            for (int dy = -1; (dy <= 1) && !has_portal; dy++)
                for (int dx = -6; dx <= -4; dx++)
                {
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy), q, 0);
                    place_room(q, entk, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy));
                }
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = 4; dx <= 6; dx++)
                {
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy), q, 0);
                    place_room(q, prisk, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy));
                }
            player_add_offmap_gold(q, 10000);
        }
        struct Thing *ah = get_player_soul_container(two[0]);
        struct Coord3d pos = ah->mappos;
        pos.y.val += 3 * STL_PER_SLB * COORD_PER_STL;
        pos.z.val = 0;
        struct Thing *troll = create_creature(&pos, (ThingModel)get_id(creature_desc, "TROLL"), two[0]);
        if (thing_exists(troll))
            move_thing_in_map(troll, &pos);
        struct Room *prison = au_prison(two[0]);
        struct Thing *knight = create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
        if (thing_exists(knight) && !room_is_invalid(prison) && find_random_valid_position_for_thing_in_room(knight, prison, &pos))
        {
            initialise_thing_state(knight, CrSt_CreatureArrivedAtPrison);
            move_thing_in_map(knight, &pos);
        }
        SYNCLOG("World auction test: set up (troll %s, knight %s, prison %s)", thing_exists(troll) ? "made" : "failed",
            thing_exists(knight) ? "made" : "failed", room_is_invalid(prison) ? "none" : "made");
        return;
    }
    if (cmd)
    {
        /* the real path: typed commands through the server's handler */
        static const char *const lines[2][4] = {
            {"sell creature TROLL 500", "sell hero KNIGHT 900", "sell creature TROLL 99999", NULL},
            {"market", "buy 1", "buy 2", "market"},
        };
        const int k = ((long)game.play_gameturn == at + 20) ? 0 : 1;
        const NetUserId u = get_player_primary_user(get_player(two[k]));
        for (int i = 0; (i < 4) && (lines[k][i] != NULL); i++)
        {
            SYNCLOG("World auction test: player %d types \"!%s\"", (int)two[k], lines[k][i]);
            server_admin_player_command(u, lines[k][i]);
        }
        SYNCLOG("World auction test: gold now: player %d %ld, player %d %ld", (int)two[0],
            (long)get_dungeon(two[0])->total_money_owned, (int)two[1], (long)get_dungeon(two[1])->total_money_owned);
        return;
    }
    if ((long)game.play_gameturn == at + 20)
    {
        int s1 = -1, s2 = -1;
        const char *w1 = au_sell(two[0], Au_Creature, "TROLL", 500, &s1);
        const char *w2 = au_sell(two[0], Au_Hero, "KNIGHT", 900, &s2);
        SYNCLOG("World auction test: selling the Troll: %s; the Knight: %s; over the price limit: %s", (w1 != NULL) ? w1 : "up",
            (w2 != NULL) ? w2 : "up", au_sell(two[0], Au_Creature, "TROLL", 99999, NULL));
        return;
    }
    for (int i = 0; i < AU_MAX; i++)
    {
        if (offers[i].kind == Au_None)
            continue;
        const int kind = offers[i].kind;
        const struct Thing *t = au_thing(&offers[i]);
        const char *why = au_buy(two[1], i);
        SYNCLOG("World auction test: player %d buys offer %d (%s): %s; now owner %d, in its Prison %d", (int)two[1], i + 1,
            (kind == Au_Hero) ? "hero" : "creature", (why != NULL) ? why : "bought", thing_exists(t) ? (int)t->owner : -1,
            thing_exists(t) ? (int)au_in_prison_of(t, two[1]) : -1);
    }
}

void world_auction_turn(void)
{
    if (!world_mode_active())
        return;
    au_test();
}

/******************************************************************************/
/* The save (the AUC1 extra) */

void world_auction_player_reset(PlayerNumber p)
{
    for (int i = 0; i < AU_MAX; i++)
        if ((offers[i].kind != Au_None) && (offers[i].seller == p))
            memset(&offers[i], 0, sizeof(offers[i]));
    if ((p >= 0) && (p < PLAYERS_MAX))
        memset(&spent[p], 0, sizeof(spent[p]));
}

void world_auction_reset(void)
{
    memset(offers, 0, sizeof(offers));
    memset(spent, 0, sizeof(spent));
}

size_t world_auction_save_size(void)
{
    return 4 + sizeof(offers) + sizeof(spent);
}

void world_auction_save(uint8_t *buf)
{
    const uint32_t n = AU_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, offers, sizeof(offers));
    memcpy(buf + 4 + sizeof(offers), spent, sizeof(spent));
}

TbBool world_auction_load(const uint8_t *buf, size_t len)
{
    world_auction_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != AU_MAX) || (len < 4 + sizeof(offers) + sizeof(spent)))
        return false;
    memcpy(offers, buf + 4, sizeof(offers));
    memcpy(spent, buf + 4 + sizeof(offers), sizeof(spent));
    for (int i = 0; i < AU_MAX; i++)
        if ((offers[i].kind > Au_Hero) || (offers[i].seller < 0) || (offers[i].seller >= PLAYERS_COUNT))
            memset(&offers[i], 0, sizeof(offers[i]));
    return true;
}

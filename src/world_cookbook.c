/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_cookbook.c
 *     Endless world: the Unholy Cookbook.
 * @par Purpose:
 *     See world_cookbook.h. The recipes are drawn from the world's seed each time they are needed; nothing names them.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_cookbook.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "world_stream.h"
#include "world_daynight.h"
#include "world_wild.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "map_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "thing_stats.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_mood.h"
#include "config_creature.h"
#include "config_strings.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "map_blocks.h"
#include "room_util.h"
#include "creature_states.h"
#include "thing_physics.h"
#include "room_data.h"
#include "world_gen.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define known game.world_cookbook_known
#define lore game.world_cookbook_lore

/* the kinds a recipe is made of: keepers' creatures that a portal brings */
static const char *const ck_kinds[] = {"FLY", "BUG", "SPIDER", "DEMONSPAWN", "ORC", "TROLL", "DARK_MISTRESS", "SORCEROR",
    "BILE_DEMON", "SKELETON", "VAMPIRE", "HELL_HOUND", "TENTACLE", "GHOST"};
#define CK_KINDS ((int)(sizeof(ck_kinds) / sizeof(ck_kinds[0])))

enum CkResult { CkR_Portal = 0, CkR_Pack, CkR_Gold, CkR_Heal, CkR_Levels, CkR_Reveal, CkR_Imps, CkR_Calm, CkR_COUNT };
static const char *const ck_results[CkR_COUNT] = {"the nearest portal shown", "a wild pack that fights for you",
    "gold from below", "every creature healed", "three creatures a level up", "the land round your Heart shown",
    "two imps", "every creature calmed"};

struct CkMix {
    uint32_t turn;           /* when its first creature was sacrificed */
    uint8_t n;
    uint8_t kind[3];         /* indexes into ck_kinds */
};
static struct CkMix ck_mix[PLAYERS_MAX];

static uint64_t ck_mixhash(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

static void sort3(uint8_t k[3])
{
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (k[j] < k[i])
            {
                const uint8_t t = k[i];
                k[i] = k[j];
                k[j] = t;
            }
}

/** Recipe r of this world: three kinds (sorted), drawn from the seed; recipes differ from each other. Worked out once
 *  per seed. */
static void recipe(int r, uint8_t out[3])
{
    static uint64_t for_seed = 0;
    static TbBool made = false;
    static uint8_t table[COOKBOOK_RECIPES][3];
    const uint64_t seed = world_seed() ^ 0xC00CB00CULL;
    if (!made || (for_seed != seed))
    {
        for (int q = 0; q < COOKBOOK_RECIPES; q++)
        {
            for (int attempt = 0; attempt < 64; attempt++)
            {
                const uint64_t h = ck_mixhash(seed + (uint64_t)q * 0x9E3779B97F4A7C15ULL + (uint64_t)attempt * 7919);
                uint8_t k[3] = {(uint8_t)(h % CK_KINDS), (uint8_t)((h >> 16) % CK_KINDS), (uint8_t)((h >> 32) % CK_KINDS)};
                sort3(k);
                TbBool clash = false;
                for (int o = 0; (o < q) && !clash; o++)
                    clash = (table[o][0] == k[0]) && (table[o][1] == k[1]) && (table[o][2] == k[2]);
                memcpy(table[q], k, 3);
                if (!clash)
                    break;
            }
        }
        for_seed = seed;
        made = true;
    }
    memcpy(out, table[r], 3);
}

/** Recipe r as this machine knows it: worked out from the world's seed where the world is simulated (and put into
 *  the game state), read from the game state on a joined client (which has no seed). */
static void recipe_known_here(int r, uint8_t out[3])
{
    if (netstate_thin_mode() && !network_is_host())
    {
        const PlayerNumber me = my_player_number;
        if ((me >= 0) && (me < PLAYERS_MAX))
            memcpy(out, game.world_cookbook_view[me][r], 3);
        else
            memset(out, 0xFF, 3);
        return;
    }
    recipe(r, out);
}

_Static_assert(sizeof(game.world_cookbook_view[0]) == COOKBOOK_RECIPES * 3, "the game state holds a keeper's recipes");

/** The simulating machine: into the game state, for each keeper only the ingredients it knows (a recipe it has made,
 *  the slots it read on lore pages); the rest stays unknown (0xFF), so the stream carries no recipe nobody found. */
static void publish_recipes(void)
{
    for (int r = 0; r < COOKBOOK_RECIPES; r++)
    {
        uint8_t rc[3];
        recipe(r, rc);
        for (int p = 0; p < PLAYERS_MAX; p++)
            for (int i = 0; i < 3; i++)
                game.world_cookbook_view[p][r][i] = ((known[p] & (1u << r)) || (lore[p] & ((uint64_t)1 << (3 * r + i)))) ?
                    rc[i] : 0xFF;
    }
}

static int kind_index(ThingModel model)
{
    for (int i = 0; i < CK_KINDS; i++)
        if (get_id(creature_desc, ck_kinds[i]) == model)
            return i;
    return -1;
}


static void tell_keeper(PlayerNumber p, const char *text);

void world_cookbook_stronghold_destroyed(PlayerNumber by, const struct Coord3d *pos)
{
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()) || (by < 0) || (by >= PLAYERS_MAX)
        || (pos == NULL) || !world_player_is_human(by))
        return;
    if ((world_biome_at_engine_slab(subtile_slab(pos->x.stl.num), subtile_slab(pos->y.stl.num)) != WgB_Ruins)
        && (getenv("KFX_WORLD_COOKLOREANY") == NULL)) /* (test switch: any ground counts as the ruins) */
        return;
    /* a lore page from the ruins: one ingredient of a recipe the keeper has not made and not read yet */
    const uint32_t h = (uint32_t)game.play_gameturn * 2654435761u ^ (uint32_t)by * 40503u;
    for (int t = 0; t < COOKBOOK_RECIPES * 3; t++)
    {
        const int bit = (int)((h + (uint32_t)t * 7u) % (COOKBOOK_RECIPES * 3));
        const int r = bit / 3;
        if ((known[by] & (1u << r)) || (lore[by] & ((uint64_t)1 << bit)))
            continue;
        lore[by] |= (uint64_t)1 << bit;
        publish_recipes(); /* (its view in the game state at once) */
        uint8_t rc[3];
        recipe(r, rc);
        char text[128];
        snprintf(text, sizeof(text), "A lore page from the ruins: one of the hidden recipes takes a %s (see \"!cookbook\").",
            get_string(creature_stats_get((ThingModel)get_id(creature_desc, ck_kinds[rc[bit % 3]]))->namestr_idx));
        tell_keeper(by, text); /* (to the keeper only, never into the log) */
        SYNCLOG("World: player %d reads a lore page of the cookbook", (int)by);
        return;
    }
}

static void tell_keeper(PlayerNumber p, const char *text)
{
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

/** A random creature of the keeper (not an imp), by the turn and a salt. */
static struct Thing *some_creature(PlayerNumber p, uint32_t salt)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon) || (dungeon->num_active_creatrs <= 0))
        return INVALID_THING;
    int k = (int)((ck_mixhash((uint64_t)game.play_gameturn ^ salt) % (uint64_t)dungeon->num_active_creatrs));
    long i = dungeon->creatr_list_start, guard = 0;
    struct Thing *last = INVALID_THING;
    while ((i > 0) && (guard++ < CREATURES_COUNT))
    {
        struct Thing *t = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (!thing_exists(t) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        last = t;
        if (k-- <= 0)
            return t;
    }
    return last;
}

static void each_creature(PlayerNumber p, void (*fn)(struct Thing *))
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return;
    long i = dungeon->creatr_list_start, guard = 0;
    while ((i > 0) && (guard++ < CREATURES_COUNT))
    {
        struct Thing *t = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
        if (!thing_exists(t) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        fn(t);
    }
}

static void heal_one(struct Thing *t)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
    t->health = cctrl->max_health;
}

static void calm_one(struct Thing *t)
{
    for (AnnoyMotive m = 1; m < AngR_ListEnd; m++)
        anger_set_creature_anger(t, 0, m);
}

static void give_result(PlayerNumber p, int result, const struct Coord3d *pos)
{
    const struct Thing *heart = get_player_soul_container(p);
    switch (result)
    {
    case CkR_Portal:
    {
        int64_t wx, wy;
        MapSlabCoord ex, ey;
        if (world_nearest_free_portal(p, &wx, &wy) && world_engine_slab(wx, wy, &ex, &ey))
            reveal_map_area(p, slab_subtile(ex - 4, 0), slab_subtile(ex + 4, 2), slab_subtile(ey - 4, 0), slab_subtile(ey + 4, 2));
        break;
    }
    case CkR_Pack:
    {
        /* three creatures of a wild kind at the Temple, the keeper's own, near its best level */
        static const char *const pack[3] = {"SPIDER", "HELL_HOUND", "TROLL"};
        const int best = world_keeper_top_level(p);
        for (int i = 0; i < 3; i++)
        {
            struct Coord3d at = *pos;
            at.x.val += (i - 1) * COORD_PER_STL;
            struct Thing *t = create_creature(&at, (ThingModel)get_id(creature_desc, pack[i]), p);
            if (!thing_is_invalid(t))
                init_creature_level(t, (CrtrExpLevel)max(0, best - 2));
        }
        break;
    }
    case CkR_Gold:
        player_add_offmap_gold(p, 1500);
        break;
    case CkR_Heal:
        each_creature(p, heal_one);
        break;
    case CkR_Levels:
        for (int i = 0; i < 3; i++)
        {
            struct Thing *t = some_creature(p, 101 + i);
            if (thing_exists(t))
            {
                const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
                set_creature_level(t, (CrtrExpLevel)(cctrl->exp_level + 1));
            }
        }
        break;
    case CkR_Reveal:
        if (thing_exists(heart))
            reveal_map_area(p, heart->mappos.x.stl.num - 120, heart->mappos.x.stl.num + 120, heart->mappos.y.stl.num - 120,
                heart->mappos.y.stl.num + 120);
        break;
    case CkR_Imps:
        for (int i = 0; i < 2; i++)
        {
            struct Coord3d at = *pos;
            at.y.val += (i + 1) * COORD_PER_STL;
            create_creature(&at, get_players_special_digger_model(p), p);
        }
        break;
    case CkR_Calm:
        each_creature(p, calm_one);
        break;
    default:
        break;
    }
}

void world_cookbook_sacrificed(PlayerNumber p, ThingModel model, const struct Coord3d *pos)
{
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()) || (p < 0) || (p >= PLAYERS_COUNT)
        || (p >= PLAYERS_MAX) || !world_player_is_human(p) || (pos == NULL))
        return;
    struct CkMix *m = &ck_mix[p];
    if ((m->n > 0) && ((uint32_t)game.play_gameturn - m->turn > (uint32_t)world_day_turns()))
        m->n = 0; /* a day passed: the mix went cold */
    const int k = kind_index(model);
    if (k < 0)
    {
        m->n = 0; /* a creature no recipe uses spoils the mix */
        return;
    }
    if (m->n == 0)
        m->turn = (uint32_t)game.play_gameturn;
    m->kind[m->n++] = (uint8_t)k;
    if (m->n < 3)
        return;
    uint8_t mix[3] = {m->kind[0], m->kind[1], m->kind[2]};
    sort3(mix);
    m->n = 0;
    for (int r = 0; r < COOKBOOK_RECIPES; r++)
    {
        uint8_t rc[3];
        recipe(r, rc);
        if ((rc[0] != mix[0]) || (rc[1] != mix[1]) || (rc[2] != mix[2]))
            continue;
        const int result = r % CkR_COUNT;
        give_result(p, result, pos);
        const TbBool first = !(known[p] & (1u << r));
        known[p] |= (uint16_t)(1u << r);
        publish_recipes();
        SYNCLOG("World: player %d makes a recipe of the cookbook%s", (int)p, first ? " for the first time" : "");
        char text[128];
        snprintf(text, sizeof(text), "The Temple answers: %s.%s", ck_results[result], first ? " (written in your cookbook)" : "");
        tell_keeper(p, text);
        return;
    }
    /* a wrong mix: nothing, or a bite */
    if (ck_mixhash((uint64_t)game.play_gameturn * 31 + (uint64_t)p) % 3 == 0)
    {
        struct Thing *t = some_creature(p, 7);
        if (thing_exists(t))
        {
            const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
            t->health = max(1, t->health - cctrl->max_health * 3 / 10);
        }
        SYNCLOG("World: player %d's Temple mix bites", (int)p);
        tell_keeper(p, "The mix was wrong, and the Temple bites.");
    } else
    {
        SYNCLOG("World: player %d's Temple mix does nothing", (int)p);
        tell_keeper(p, "The mix was wrong: nothing happens.");
    }
}

TbBool world_cookbook_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    if (strcasecmp(text + 1, "cookbook") != 0)
        return false;
    const PlayerNumber p = my_player_number;
    if (p >= PLAYERS_MAX)
        return true;
    int n = 0;
    for (int r = 0; r < COOKBOOK_RECIPES; r++)
    {
        if (!(known[p] & (1u << r)))
            continue;
        uint8_t rc[3];
        recipe_known_here(r, rc);
        char line[160];
        snprintf(line, sizeof(line), "%s: %s, %s, %s", ck_results[r % CkR_COUNT],
            get_string(creature_stats_get((ThingModel)get_id(creature_desc, ck_kinds[rc[0]]))->namestr_idx),
            get_string(creature_stats_get((ThingModel)get_id(creature_desc, ck_kinds[rc[1]]))->namestr_idx),
            get_string(creature_stats_get((ThingModel)get_id(creature_desc, ck_kinds[rc[2]]))->namestr_idx));
        if (chat_box_active())
            chat_box_add(-1, line);
        else
            message_add(MsgType_Blank, 0, line);
        n++;
    }
    char head[96];
    {
        /* the ingredients read on lore pages, by recipe not yet made */
        for (int r = 0; r < COOKBOOK_RECIPES; r++)
        {
            if (known[p] & (1u << r))
                continue;
            char line[160];
            int m = 0;
            line[0] = '\0';
            uint8_t rc[3];
            recipe_known_here(r, rc);
            for (int i = 0; i < 3; i++)
                if (lore[p] & ((uint64_t)1 << (3 * r + i)))
                    m += snprintf(line + m, sizeof(line) - m, "%s%s", (m > 0) ? ", " : "Lore: a hidden recipe takes ",
                        get_string(creature_stats_get((ThingModel)get_id(creature_desc, ck_kinds[rc[i]]))->namestr_idx));
            if (m == 0)
                continue;
            if (chat_box_active())
                chat_box_add(-1, line);
            else
                message_add(MsgType_Blank, 0, line);
        }
    }
    snprintf(head, sizeof(head), "Your cookbook: %d of %d recipes known.", n, COOKBOOK_RECIPES);
    if (chat_box_active())
        chat_box_add(-1, head);
    else
        message_add(MsgType_Blank, 0, head);
    return true;
}

/** KFX_WORLD_COOKTEMPLE=turn (test switch): a real Temple: a 3x3 Temple is laid below this machine's Heart, and every
 *  100 turns one creature of recipe 0 appears on its altar and is sacrificed the stock way (its sacrifice state); after
 *  the third, whether recipe 0 is now known is logged. No kind is named. */
static void cook_temple_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_COOKTEMPLE");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_COOKTEMPLE=%ld", at);
    }
    if ((at < 0) || !world_mode_active() || ((long)game.play_gameturn < at))
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const long t = (long)game.play_gameturn - at;
    if (t == 0)
    {
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
            {
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1 + dx), slab_subtile_center(hy + 6 + dy), p, 0);
                replace_slab_from_script(hx - 1 + dx, hy + 6 + dy, SlbT_TEMPLE);
            }
        SYNCLOG("World: cook temple test: a Temple is laid below the Heart (known mask %04x)", (unsigned)known[p]);
        return;
    }
    if ((t == 100) || (t == 200) || (t == 300))
    {
        uint8_t rc[3];
        recipe(0, rc);
        struct Coord3d pos;
        memset(&pos, 0, sizeof(pos));
        pos.x.val = subtile_coord_center(slab_subtile_center(hx));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 7));
        struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, ck_kinds[rc[t / 100 - 1]]), p);
        if (thing_is_invalid(c))
        {
            SYNCLOG("World: cook temple test: creature %ld could not be made", t / 100);
            return;
        }
        c->mappos.z.val = get_thing_height_at(c, &c->mappos);
        initialise_thing_state(c, CrSt_CreatureSacrifice);
        SYNCLOG("World: cook temple test: creature %ld is on the altar (altar there %d)", t / 100,
            (int)subtile_has_sacrificial_on_top(pos.x.stl.num, pos.y.stl.num));
        return;
    }
    if (t == 450)
        SYNCLOG("World: cook temple test: recipe 0 known %d (mask %04x)", (int)((known[p] & 1) != 0), (unsigned)known[p]);
}

/** KFX_WORLD_COOKTEST=turn (test switch): this machine's keeper makes recipe 0 and recipe 1 (as three sacrifices each,
 *  through the same function the Temple calls), then a wrong mix; the results are logged without naming any kind. */
void world_cookbook_turn(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_COOKTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_COOKTEST=%ld", at);
    }
    cook_temple_test();
    {
        /* KFX_WORLD_COOKLORE=turn (test switch): two heroes' strongholds "fall" to this machine's keeper on the nearest
           Ancient Ruins ground in the loaded window, then one elsewhere; how many lore ingredients the keeper has read is
           logged (no ingredient is named) */
        static long lt = -2;
        if (lt == -2)
        {
            const char *v = getenv("KFX_WORLD_COOKLORE");
            lt = (v != NULL) ? atol(v) : -1;
        }
        const PlayerNumber me = my_player_number;
        const struct Thing *hh = get_player_soul_container(me);
        if ((lt >= 0) && ((long)game.play_gameturn == lt) && thing_exists(hh))
        {
            const MapSlabCoord hx = subtile_slab(hh->mappos.x.stl.num), hy = subtile_slab(hh->mappos.y.stl.num);
            struct Coord3d ruin, other;
            TbBool found_ruin = false, found_other = false;
            memset(&ruin, 0, sizeof(ruin));
            memset(&other, 0, sizeof(other));
            for (int d = 0; (d < 38) && (!found_ruin || !found_other); d++)
                for (int k = -d; (k <= d) && (!found_ruin || !found_other); k++)
                {
                    const MapSlabCoord cand[4][2] = {{hx + k, hy - d}, {hx + k, hy + d}, {hx - d, hy + k}, {hx + d, hy + k}};
                    for (int c = 0; c < 4; c++)
                    {
                        const int b = world_biome_at_engine_slab(cand[c][0], cand[c][1]);
                        struct Coord3d *dst = (b == WgB_Ruins) ? &ruin : &other;
                        TbBool *flag = (b == WgB_Ruins) ? &found_ruin : &found_other;
                        if (*flag || (b < 0))
                            continue;
                        dst->x.val = subtile_coord_center(slab_subtile_center(cand[c][0]));
                        dst->y.val = subtile_coord_center(slab_subtile_center(cand[c][1]));
                        *flag = true;
                    }
                }
            int before = 0, mid = 0, after = 0;
            for (int b = 0; b < 64; b++)
                before += (int)((lore[me] >> b) & 1);
            if (found_other)
                world_cookbook_stronghold_destroyed(me, &other);
            for (int b = 0; b < 64; b++)
                mid += (int)((lore[me] >> b) & 1);
            if (found_ruin)
            {
                world_cookbook_stronghold_destroyed(me, &ruin);
                world_cookbook_stronghold_destroyed(me, &ruin);
            }
            for (int b = 0; b < 64; b++)
                after += (int)((lore[me] >> b) & 1);
            SYNCLOG("World: cook lore test: ruins ground found %d, other %d; lore ingredients read %d -> %d (elsewhere) -> %d (two in the ruins)",
                (int)found_ruin, (int)found_other, before, mid, after);
        }
    }
    if (world_mode_active() && (!netstate_thin_mode() || network_is_host()) && ((game.play_gameturn % 64) == 13))
        publish_recipes();
    {
        /* KFX_WORLD_COOKDIGEST=turn (test switch): the recipes' digest at that turn (compare with a joined client's) */
        static long dg = -2;
        if (dg == -2)
        {
            const char *v = getenv("KFX_WORLD_COOKDIGEST");
            dg = (v != NULL) ? atol(v) : -1;
        }
        if ((dg >= 0) && ((long)game.play_gameturn == dg))
            SYNCLOG("World: cookbook digest %08x", (unsigned)world_cookbook_digest());
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    struct Coord3d pos = heart->mappos;
    pos.y.val += 6 * COORD_PER_STL;
    const GoldAmount gold0 = get_players_num_dungeon(p)->total_money_owned;
    for (int r = 0; r < 3; r++)
    {
        uint8_t rc[3];
        recipe(r, rc);
        for (int i = 0; i < 3; i++)
            world_cookbook_sacrificed(p, (ThingModel)get_id(creature_desc, ck_kinds[rc[i]]), &pos);
    }
    SYNCLOG("World: cook test: known mask %04x, gold %ld -> %ld", (unsigned)known[p], (long)gold0,
        (long)get_players_num_dungeon(p)->total_money_owned);
    /* a wrong mix (three Flies, unless that is a recipe of this world) */
    for (int i = 0; i < 3; i++)
        world_cookbook_sacrificed(p, (ThingModel)get_id(creature_desc, "FLY"), &pos);
    /* (the cookbook page is not shown here: on-screen lines go into the log, and the recipes must not) */
}

/** A digest of this world's recipes, for comparing two machines without writing a recipe down (tests). */
uint32_t world_cookbook_digest(void)
{
    uint32_t h = 2166136261u;
    for (int r = 0; r < COOKBOOK_RECIPES; r++)
        for (int i = 0; i < 3; i++)
            h = (h ^ game.world_cookbook_view[0][r][i]) * 16777619u; /* (player 0's view: published, or as streamed) */
    return h;
}

/******************************************************************************/
void world_cookbook_reset(void)
{
    memset(known, 0, sizeof(known));
    memset(ck_mix, 0, sizeof(ck_mix));
    memset(lore, 0, sizeof(lore));
}

size_t world_cookbook_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if ((known[p] != 0) || (lore[p] != 0))
            return 4 + sizeof(known) + sizeof(lore); /* (the lore pages after the known recipes: a record without them loads too) */
    return 0;
}

void world_cookbook_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, known, sizeof(known));
    memcpy(buf + 4 + sizeof(known), lore, sizeof(lore));
}

TbBool world_cookbook_load(const uint8_t *buf, size_t len)
{
    world_cookbook_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (4 + sizeof(known) > len))
        return false;
    memcpy(known, buf + 4, sizeof(known));
    if (4 + sizeof(known) + sizeof(lore) <= len)
        memcpy(lore, buf + 4 + sizeof(known), sizeof(lore));
    return true;
}

void world_cookbook_seat_reset(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    known[p] = 0;
    lore[p] = 0;
    memset(&ck_mix[p], 0, sizeof(ck_mix[p]));
}

#ifdef __cplusplus
}
#endif

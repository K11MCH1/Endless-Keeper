/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_champion.c
 *     Endless world: the keeper's Champion.
 * @par Purpose:
 *     See world_champion.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_champion.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "world_stream.h"
#include "world_progress.h"
#include "world_daynight.h"
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
#include "world_misdeeds.h"
#include "creature_states.h"
#include "magic_powers.h"
#include "config_creature.h"
#include "packets.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "config_settings.h"
#include "front_input.h"
#include "kjm_input.h"
#include "bflib_keybrd.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* who the Champions are: in struct Game (game.world_champion, world_champion_born), so joined players see the label */
#define champ game.world_champion
#define champ_born game.world_champion_born

struct ChampionKeeper {
    int32_t changed_day;     /* the dungeon's day the title last passed (0: never) */
    uint32_t back_turn;      /* a fallen Champion comes back then (0: none fallen) */
    ThingModel model;
    CrtrExpLevel exp_level;  /* the level it comes back at (one lower) */
    uint8_t misdeed[2];      /* its Misdeeds record, kept frozen while it is away (low, high byte; 0: none) */
    uint8_t live_md[2];      /* the living Champion's Misdeeds record (it follows the creature through a frozen chunk) */
    char name[CREATURE_NAME_MAX];
};
static struct ChampionKeeper ck[PLAYERS_MAX];

static TbBool simulating(void)
{
    return world_mode_active() && (!network_is_active() || netstate_is_server());
}

static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: champion, to player %d: %s", (int)p, text);
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

static struct Thing *champion_of(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX) || (champ[p] == 0))
        return INVALID_THING;
    struct Thing *t = thing_get(champ[p]);
    if (!thing_exists(t) || !thing_is_creature(t) || (t->creation_turn != champ_born[p]) || (t->owner != p))
        return INVALID_THING;
    return t;
}

TbBool world_champion_is(const struct Thing *creatng)
{
    if (!world_mode_active() || !thing_exists(creatng) || (creatng->owner >= PLAYERS_MAX))
        return false;
    return (champ[creatng->owner] == creatng->index) && (champ_born[creatng->owner] == creatng->creation_turn);
}

CrtrExpLevel world_champion_level(const struct Thing *creatng, CrtrExpLevel exp_level)
{
    if (!world_champion_is(creatng))
        return exp_level;
    const PlayerNumber p = creatng->owner;
    long add = world_player_level(p);
    const long days = world_seat_day(p);
    if (add > days)
        add = days; /* capped at the dungeon's age: a keeper who starts again begins with no giant */
    long level = (long)exp_level + add;
    if (level > CREATURE_MAX_LEVEL_LIMIT - 1)
        level = CREATURE_MAX_LEVEL_LIMIT - 1;
    return (CrtrExpLevel)level;
}

const char *world_champion_label(const struct Thing *creatng)
{
    return world_champion_is(creatng) ? " (Champion)" : "";
}

static void restat(struct Thing *t)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(t);
    if (creature_control_invalid(cctrl))
        return;
    cctrl->max_health = calculate_correct_creature_max_health(t);
    if (t->health > cctrl->max_health)
        t->health = cctrl->max_health;
}

/** The creature of this keeper at or next to a subtile. */
static struct Thing *creature_at(PlayerNumber p, MapSubtlCoord sx, MapSubtlCoord sy)
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
                    if (thing_is_creature(t) && (t->owner == p) && !thing_is_creature_digger(t) && !creature_is_dying(t))
                        return t;
                }
            }
    return INVALID_THING;
}

static void make_champion(PlayerNumber p, struct Thing *t)
{
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
        return;
    const long dx = subtile_slab(t->mappos.x.stl.num) - subtile_slab(heart->mappos.x.stl.num);
    const long dy = subtile_slab(t->mappos.y.stl.num) - subtile_slab(heart->mappos.y.stl.num);
    if ((labs(dx) > CHAMPION_HEART_SLABS) || (labs(dy) > CHAMPION_HEART_SLABS))
    {
        tell_keeper(p, "A Champion is made at the Dungeon Heart: bring the creature there.");
        return;
    }
    struct ChampionKeeper *k = &ck[p];
    const long day = world_seat_day(p);
    if (((champ[p] != 0) || (k->back_turn != 0)) && (k->changed_day > 0) && (day - k->changed_day < CHAMPION_CHANGE_DAYS))
    {
        tell_keeper(p, "The title of Champion passes once a week.");
        return;
    }
    struct Thing *old = champion_of(p);
    champ[p] = t->index;
    champ_born[p] = t->creation_turn;
    ck[p].live_md[0] = ck[p].live_md[1] = 0; /* (filled from its record by the turn) */
    k->changed_day = (int32_t)day;
    k->back_turn = 0;
    if (thing_exists(old))
        restat(old);
    restat(t);
    char text[96];
    snprintf(text, sizeof(text), "%s is your Champion now (+%d levels).", creature_own_name(t),
        (int)(world_champion_level(t, 0)));
    tell_keeper(p, text);
}

static void possess(PlayerNumber p)
{
    struct Thing *t = champion_of(p);
    if (!thing_exists(t))
    {
        tell_keeper(p, ck[p].back_turn ? "Your Champion lies out of action until tomorrow." : "You have no Champion.");
        return;
    }
    struct PlayerInfo *player = get_player(p);
    struct UserState *ustate = get_player_primary_user_state(player);
    if (user_state_invalid(ustate))
        return;
    /* the stock Possess's own checks: not held in the Hand, not a captive, and only from the dungeon view */
    if ((ustate->view_type != PVT_DungeonTop) || (ustate->instance_num != PI_Unset))
    {
        tell_keeper(p, "Return to your dungeon view first.");
        return;
    }
    if (thing_is_picked_up(t))
    {
        tell_keeper(p, "Your Champion is in your Hand: put it down first.");
        return;
    }
    if (creature_is_kept_in_custody_by_enemy(t) || !can_cast_power_on_thing(p, t, PwrK_POSSESS))
    {
        tell_keeper(p, "Your Champion can't be possessed now.");
        return;
    }
    ustate->influenced_thing_idx = t->index;
    ustate->influenced_thing_creation = t->creation_turn;
    ustate->first_person_dig_claim_mode = false;
    ustate->teleport_destination = 19;
    ustate->battleid = 1;
    set_user_instance(get_player_primary_user(player), PI_DirctCtrl, 0);
    SYNCLOG("World: player %d possesses its Champion", (int)p);
}

void world_champion_packet(PlayerNumber p, int what, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!simulating() || (p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
        return;
    if (what == 2)
    {
        possess(p);
        return;
    }
    struct Thing *t = creature_at(p, stl_x, stl_y);
    if (!thing_exists(t))
    {
        tell_keeper(p, "Point at one of your creatures (not an imp) at your Heart.");
        return;
    }
    make_champion(p, t);
}

/** The dungeon view's inputs: J possesses the Champion ("One key possesses it"), unless the player's own key settings
 *  give J to a game key. True when it was taken. */
TbBool world_champion_inputs(void)
{
    if (!world_mode_active() || netstate_dedicated())
        return false;
    const struct UserState *ustate = get_local_user_state();
    if (((ustate->init_flags & UsrIF_NewMPMessage) != 0) || chat_box_draws_typing() || !is_key_pressed(KC_J, KMod_NONE))
        return false;
    for (int i = 0; i < GAME_KEYS_COUNT; i++)
        if (settings.kbkeys[i].code == KC_J)
            return false;
    clear_key_pressed(KC_J);
    set_players_packet_action(get_my_player(), PckA_WorldChampion, (unsigned long)2 << 28, 0, 0, 0);
    return true;
}

TbBool world_champion_command(const char *text, MapCoord cursor_x, MapCoord cursor_y)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    int what;
    if (strcasecmp(text + 1, "champion") == 0)
        what = 1;
    else if (strcasecmp(text + 1, "possess") == 0)
        what = 2;
    else
        return false;
    set_players_packet_action(get_my_player(), PckA_WorldChampion, (unsigned long)coord_subtile(cursor_x) | ((unsigned long)what << 28),
        (unsigned long)coord_subtile(cursor_y), 0, 0);
    return true;
}

void world_champion_falls(struct Thing *creatng)
{
    if (!simulating() || !world_champion_is(creatng))
        return;
    const PlayerNumber p = creatng->owner;
    struct ChampionKeeper *k = &ck[p];
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    /* its Misdeeds record (name, vices) goes with it, not with its body: no Unfinished Business, no Bad Blood */
    const uint16_t md = world_misdeeds_freeze(creatng);
    if (md != 0)
        cctrl->misdeed_id = 0;
    k->misdeed[0] = (uint8_t)(md & 0xFF);
    k->misdeed[1] = (uint8_t)(md >> 8);
    k->model = creatng->model;
    k->exp_level = (CrtrExpLevel)((cctrl->exp_level > 0) ? cctrl->exp_level - 1 : 0);
    memcpy(k->name, cctrl->creature_name, sizeof(k->name));
    k->name[sizeof(k->name) - 1] = '\0';
    k->back_turn = (uint32_t)game.play_gameturn + (uint32_t)world_day_turns();
    champ[p] = 0;
    champ_born[p] = 0;
    k->live_md[0] = k->live_md[1] = 0;
    tell_keeper(p, "Your Champion has fallen: it is pulled back to your Heart, and wakes tomorrow one level lower.");
}

/** KFX_WORLD_CHAMPIONTEST=turn (test switch): an Orc named Ironjaw at this machine's Heart is made Champion (the
 *  keeper is given 3000 XP first); his max health is logged before and after; 200 turns later he is killed, and his
 *  return is logged. */
static void champion_test(void)
{
    static long at = -2;
    static ThingIndex orc = 0;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_CHAMPIONTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_CHAMPIONTEST=%ld", at);
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
    if ((long)game.play_gameturn == at)
    {
        world_add_player_xp(p, 3000);
        struct Coord3d pos = heart->mappos;
        pos.y.val += 6 * COORD_PER_STL;
        struct Thing *t = create_creature(&pos, (ThingModel)get_id(creature_desc, "ORC"), p);
        if (thing_is_invalid(t))
            return;
        init_creature_level(t, 3);
        snprintf(creature_control_get_from_thing(t)->creature_name, CREATURE_NAME_MAX, "Ironjaw");
        orc = t->index;
        const HitPoints before = creature_control_get_from_thing(t)->max_health;
        world_champion_packet(p, 1, t->mappos.x.stl.num, t->mappos.y.stl.num);
        SYNCLOG("World: champion test: armour %ld, defence %ld, dexterity %ld (an Orc of level 4 has %ld, %ld, %ld)",
            calculate_correct_creature_armour(t), calculate_correct_creature_defense(t), calculate_correct_creature_dexterity(t),
            compute_creature_max_armour(creature_stats_get_from_thing(t)->armour, 3),
            compute_creature_max_defense(creature_stats_get_from_thing(t)->defense, 3),
            compute_creature_max_dexterity(creature_stats_get_from_thing(t)->dexterity, 3));
        SYNCLOG("World: champion test: Ironjaw (level 4, keeper level %d, dungeon day %ld) max health %ld -> %ld, fights at level %d, champion %d",
            world_player_level(p), world_seat_day(p), (long)before, (long)creature_control_get_from_thing(t)->max_health,
            (int)world_champion_level(t, 3) + 1, (int)world_champion_is(t));
    } else if ((long)game.play_gameturn == at + 100)
    {
        world_champion_packet(p, 2, 0, 0); /* "!possess" */
        const struct UserState *ustate = get_player_primary_user_state(get_player(p));
        SYNCLOG("World: champion test: possessing: influenced thing %d (Ironjaw %d)", (int)ustate->influenced_thing_idx, (int)orc);
    } else if (((long)game.play_gameturn == at + 50) && (getenv("KFX_WORLD_CHAMPRELINK") != NULL))
    {
        /* KFX_WORLD_CHAMPRELINK=1 as well: Ironjaw goes the way of a chunk that freezes and comes back: his record is
           frozen, he is deleted, and made again as a new thing that gets the record back; he must still be Champion */
        struct Thing *t = thing_get(orc);
        if (thing_exists(t) && world_champion_is(t))
        {
            const uint16_t id = world_misdeeds_freeze(t);
            struct Coord3d pos = t->mappos;
            const ThingModel model = t->model;
            const CrtrExpLevel lvl = creature_control_get_from_thing(t)->exp_level;
            delete_thing_structure(t, 0);
            struct Thing *n = create_creature(&pos, model, p);
            if (!thing_is_invalid(n))
            {
                init_creature_level(n, lvl);
                world_misdeeds_on_delete(creature_control_get_from_thing(n));
                world_misdeeds_thaw(n, id);
                n->creation_turn += 1; /* as if made on another turn */
                orc = n->index;
            }
            SYNCLOG("World: champion test: Ironjaw frozen and back as thing %d (record %d); Champion now %d",
                thing_is_invalid(n) ? -1 : (int)n->index, (int)id, thing_is_invalid(n) ? -1 : (int)world_champion_is(n));
        }
    } else if (((long)game.play_gameturn == at + 90) && (getenv("KFX_WORLD_CHAMPRELINK") != NULL))
    {
        struct Thing *t = thing_get(orc);
        SYNCLOG("World: champion test: 40 turns later Ironjaw (thing %d) is Champion %d", (int)orc,
            thing_exists(t) ? (int)world_champion_is(t) : -1);
    } else if ((long)game.play_gameturn == at + 200)
    {
        struct Thing *t = thing_get(orc);
        if (thing_exists(t) && world_champion_is(t))
        {
            kill_creature(t, INVALID_THING, -1, CrDed_Default);
            SYNCLOG("World: champion test: Ironjaw is killed");
        }
    }
}

void world_champion_turn(void)
{
    if (!simulating())
        return;
    champion_test();
    if ((game.play_gameturn % 16) != 7)
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        struct ChampionKeeper *k = &ck[p];
        struct Thing *cur = champion_of(p);
        if ((champ[p] != 0) && thing_exists(cur) && (k->live_md[0] == 0) && (k->live_md[1] == 0))
        {
            const uint16_t id = creature_control_get_from_thing(cur)->misdeed_id; /* (the record may come after the title) */
            k->live_md[0] = (uint8_t)(id & 0xFF);
            k->live_md[1] = (uint8_t)(id >> 8);
        }
        if ((champ[p] != 0) && !thing_exists(cur))
        {
            /* frozen with its chunk (it waits), back as a new thing (found by its Misdeeds record), or gone (sold, ...) */
            const uint16_t id = (uint16_t)(k->live_md[0] | (k->live_md[1] << 8));
            struct Thing *found = INVALID_THING;
            const struct Dungeon *dungeon = get_players_num_dungeon(p);
            long i = ((id == 0) || dungeon_invalid(dungeon)) ? 0 : dungeon->creatr_list_start, guard = 0;
            while ((i > 0) && (guard++ < CREATURES_COUNT) && thing_is_invalid(found))
            {
                struct Thing *t = thing_get(i);
                if (!thing_exists(t))
                    break;
                const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
                if (creature_control_invalid(cctrl))
                    break;
                i = cctrl->players_next_creature_idx;
                if (cctrl->misdeed_id == id)
                    found = t;
            }
            if (!thing_is_invalid(found))
            {
                champ[p] = found->index;
                champ_born[p] = found->creation_turn;
                SYNCLOG("World: player %d's Champion is found again (back from a frozen chunk)", (int)p);
            } else if ((id == 0) || !world_misdeeds_frozen(id))
            {
                champ[p] = 0; /* gone some other way (sold, a dungeon started again) */
                champ_born[p] = 0;
                k->live_md[0] = k->live_md[1] = 0;
            }
        }
        if ((champ[p] != 0) && thing_exists(cur) && ((game.play_gameturn % 256) == 7))
            restat(cur); /* its health follows the keeper's level and the days as they grow */
        if ((k->back_turn == 0) || ((uint32_t)game.play_gameturn < k->back_turn))
            continue;
        const struct Thing *heart = get_player_soul_container(p);
        const uint16_t md = (uint16_t)(k->misdeed[0] | (k->misdeed[1] << 8));
        k->misdeed[0] = k->misdeed[1] = 0;
        if (!thing_exists(heart) || !world_player_is_human(p))
        {
            k->back_turn = 0;
            world_misdeeds_release(md);
            continue;
        }
        struct Coord3d pos = heart->mappos;
        pos.y.val += 5 * COORD_PER_STL;
        struct Thing *t = create_creature(&pos, k->model, p);
        k->back_turn = 0;
        if (thing_is_invalid(t))
        {
            world_misdeeds_release(md);
            continue;
        }
        init_creature_level(t, k->exp_level);
        if (md != 0)
        {
            struct CreatureControl *ncctrl = creature_control_get_from_thing(t);
            world_misdeeds_on_delete(ncctrl); /* a record of its own, if it got one, gives way */
            world_misdeeds_thaw(t, md);
            SYNCLOG("World: player %d's Champion comes back with its Misdeeds record", (int)p);
        }
        if (k->name[0] != '\0')
            snprintf(creature_control_get_from_thing(t)->creature_name, CREATURE_NAME_MAX, "%s", k->name);
        champ[p] = t->index;
        champ_born[p] = t->creation_turn;
        restat(t);
        SYNCLOG("World: player %d's Champion (%s) wakes at the Heart, level %d", (int)p, thing_model_name(t), (int)k->exp_level + 1);
        tell_keeper(p, "Your Champion wakes at your Heart, one level lower.");
    }
}

/******************************************************************************/
void world_champion_reset(void)
{
    memset(ck, 0, sizeof(ck));
    memset(champ, 0, sizeof(champ));
    memset(champ_born, 0, sizeof(champ_born));
}

void world_champion_seat_reset(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    world_misdeeds_release((uint16_t)(ck[p].misdeed[0] | (ck[p].misdeed[1] << 8)));
    memset(&ck[p], 0, sizeof(ck[p]));
    champ[p] = 0;
    champ_born[p] = 0;
}

size_t world_champion_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if ((champ[p] != 0) || (ck[p].back_turn != 0) || (ck[p].changed_day != 0))
            return 4 + sizeof(ck) + sizeof(champ) + sizeof(champ_born);
    return 0;
}

void world_champion_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, ck, sizeof(ck));
    memcpy(buf + 4 + sizeof(ck), champ, sizeof(champ));
    memcpy(buf + 4 + sizeof(ck) + sizeof(champ), champ_born, sizeof(champ_born));
}

TbBool world_champion_load(const uint8_t *buf, size_t len)
{
    world_champion_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n != PLAYERS_MAX) || (4 + sizeof(ck) + sizeof(champ) + sizeof(champ_born) > len))
        return false;
    memcpy(ck, buf + 4, sizeof(ck));
    memcpy(champ, buf + 4 + sizeof(ck), sizeof(champ));
    memcpy(champ_born, buf + 4 + sizeof(ck) + sizeof(champ), sizeof(champ_born));
    return true;
}

#ifdef __cplusplus
}
#endif

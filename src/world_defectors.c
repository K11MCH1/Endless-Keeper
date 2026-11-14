/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_defectors.c
 *     Endless world: Defectors (2026-11-13; see world_defectors.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - The cause: the creature's highest anger of the three a keeper can answer: wages not paid, hunger, no lair.
 *       A creature angry for anything else (another creature, slaps) leaves as before: nothing defects without a
 *       cause another keeper could answer.
 *     - Who takes it: a human keeper of the same world (not the one it left, not one whose dungeon the caretaker keeps,
 *       not one at the creature limit) with a portal, who offers what it went without: gold for four of its paydays,
 *       a Hatchery, or a Lair with room. When several do, the best offer: the most gold, the biggest Hatchery, the
 *       Lair with the most room.
 *     - The limit: a defector counts towards its new keeper's creature limit like any creature (a keeper at the limit
 *       is passed over). It comes from outside its new keeper's creature pool and takes nothing from it.
 *     - Nobody can stir up another keeper's creatures from a distance: only the creature's own anger, at its own
 *       keeper's portal, makes it defect.
 *     Saved: DEF1 (each defector's line: the keeper it left, why, the day).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_defectors.h"
#include "world_stream.h"
#include "world_caretaker.h"
#include "world_daynight.h"
#include "world_misdeeds.h"
#include "world_wild.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "room_util.h"
#include "room_list.h"
#include "slab_data.h"
#include "map_blocks.h"
#include "config_terrain.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_navigate.h"
#include "thing_stats.h"
#include "room_entrance.h"
#include "post_inc.h"

#define DF_MAX 512
#define DF_PAY_DAYS 4

enum DfCause { DfC_None = 0, DfC_Pay, DfC_Food, DfC_Lair };
static const char *const df_cause_words[4] = {"", "unpaid wages", "an empty Hatchery", "no lair of its own"};

#pragma pack(1)
struct DfLine {
    uint16_t rec;            /* its Misdeeds record (the line follows the record, as the Rap Sheet does) */
    uint8_t from, cause;     /* the player it left, DfCause */
    int32_t day;             /* the world's day it defected */
    char from_name[20];      /* that keeper's name then */
};
#pragma pack()
static struct DfLine df_lines[DF_MAX];
static int df_count;

static int df_cause_of(const struct Thing *creatng)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return DfC_None;
    int best = DfC_None;
    int32_t level = 0;
    const int reasons[3][2] = {{AngR_NotPaid, DfC_Pay}, {AngR_Hungry, DfC_Food}, {AngR_NoLair, DfC_Lair}};
    for (int k = 0; k < 3; k++)
        if (cctrl->annoyance_level[reasons[k][0]] > level)
        {
            level = cctrl->annoyance_level[reasons[k][0]];
            best = reasons[k][1];
        }
    /* the other anger (other creatures, slaps) must not be the main one */
    if ((best != DfC_None) && (cctrl->annoyance_level[AngR_Other] > level))
        return DfC_None;
    return best;
}

/** What a keeper offers a creature that left for this cause (0: nothing). */
static long df_offer(PlayerNumber q, const struct Thing *creatng, int cause)
{
    struct Dungeon *dungeon = get_players_num_dungeon(q);
    if (dungeon_invalid(dungeon))
        return 0;
    switch (cause)
    {
    case DfC_Pay:
    {
        const long need = DF_PAY_DAYS * (long)calculate_correct_creature_pay(creatng);
        return (dungeon->total_money_owned >= max(need, 1L)) ? (long)dungeon->total_money_owned : 0;
    }
    case DfC_Food:
    {
        struct Room *room = pick_random_room_of_role(q, RoRoF_FoodStorage);
        return room_is_invalid(room) ? 0 : (long)room->slabs_count;
    }
    case DfC_Lair:
    {
        struct Room *room = find_room_of_role_with_spare_capacity(q, RoRoF_LairStorage, 1);
        return room_is_invalid(room) ? 0 : (long)(room->total_capacity - room->used_capacity) + 1;
    }
    default:
        return 0;
    }
}

static struct DfLine *df_line_of(uint16_t rec)
{
    if (rec == 0)
        return NULL;
    for (int i = 0; i < df_count; i++)
        if (df_lines[i].rec == rec)
            return &df_lines[i];
    return NULL;
}

TbBool world_defectors_leaving(struct Thing *creatng)
{
    if (!world_mode_active() || !thing_exists(creatng) || (creatng->class_id != TCls_Creature))
        return false;
    const PlayerNumber from = creatng->owner;
    if ((from >= PLAYERS_MAX) || !world_player_is_human(from) || thing_is_creature_digger(creatng))
        return false;
    const int cause = df_cause_of(creatng);
    if (cause == DfC_None)
    {
        SYNCLOG("World: a %s leaves player %d with no cause another keeper could answer: it goes", thing_model_name(creatng),
            (int)from);
        return false;
    }
    /* the best offer among the other keepers */
    PlayerNumber to = PLAYER_NONE;
    long best = 0;
    for (PlayerNumber q = 0; (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
    {
        if ((q == from) || !world_player_is_human(q) || world_caretaker_active(q))
            continue;
        struct PlayerInfo *player = get_player(q);
        struct Dungeon *dungeon = get_players_num_dungeon(q);
        if (!player_exists(player) || dungeon_invalid(dungeon) || (player->victory_state != VicS_Undecided))
            continue;
        if (!thing_exists(find_players_dungeon_heart(q)))
            continue;
        if (dungeon->num_active_creatrs >= dungeon->max_creatures_attracted)
            continue; /* at its creature limit */
        if (room_is_invalid(pick_random_room_of_role(q, RoRoF_CrPoolSpawn)))
            continue; /* no portal to come out of */
        const long offer = df_offer(q, creatng, cause);
        if (offer > best)
        {
            best = offer;
            to = q;
        }
    }
    if (to == PLAYER_NONE)
    {
        SYNCLOG("World: a %s leaves player %d over %s; no other keeper offers it: it goes", thing_model_name(creatng),
            (int)from, df_cause_words[cause]);
        return false;
    }
    struct Room *portal = pick_random_room_of_role(to, RoRoF_CrPoolSpawn);
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(slb_num_decode_x(portal->slabs_list)));
    pos.y.val = subtile_coord_center(slab_subtile_center(slb_num_decode_y(portal->slabs_list)));
    pos.z.val = 0;
    struct Thing *newtng = create_creature(&pos, creatng->model, to);
    if (thing_is_invalid(newtng))
        return false;
    struct CreatureControl *octrl = creature_control_get_from_thing(creatng);
    struct CreatureControl *nctrl = creature_control_get_from_thing(newtng);
    /* it is no giant: never above its new keeper's best creature */
    const int level = min((int)octrl->exp_level + 1, max(1, world_keeper_top_level(to)));
    init_creature_level(newtng, (CrtrExpLevel)(level - 1));
    memcpy(nctrl->creature_name, octrl->creature_name, sizeof(nctrl->creature_name));
    /* its Rap Sheet comes with it */
    if (octrl->misdeed_id != 0)
    {
        world_misdeeds_on_delete(nctrl); /* (a record the new thing got meanwhile) */
        nctrl->misdeed_id = octrl->misdeed_id;
        octrl->misdeed_id = 0;
        struct DfLine *line = df_line_of(nctrl->misdeed_id);
        if ((line == NULL) && (df_count < DF_MAX))
            line = &df_lines[df_count++];
        if (line != NULL)
        {
            memset(line, 0, sizeof(*line));
            line->rec = nctrl->misdeed_id;
            line->from = (uint8_t)from;
            line->cause = (uint8_t)cause;
            line->day = (int32_t)world_day_number();
            snprintf(line->from_name, sizeof(line->from_name), "%s", get_player(from)->player_name);
        }
    }
    move_thing_in_map(newtng, &pos);
    create_effect(&newtng->mappos, TngEff_RoomSparkeMedium, to);
    SYNCLOG("World: a %s (level %d) defects from player %d over %s and comes out of player %d's portal at level %d",
        thing_model_name(creatng), (int)octrl->exp_level + 1, (int)from, df_cause_words[cause], (int)to, level);
    return true;
}

/** Test hook KFX_WORLD_DEFECTTEST=T: at turn T player 0's first creature (not an imp) is made angry over unpaid
 *  wages and sets off for its portal, as an angry creature does (it walks; it defects only if it gets there). */
static void df_test(void)
{
    static long at = -1;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_DEFECTTEST");
        at = (v != NULL) ? atol(v) : 0;
    }
    if ((at > 300) && (game.play_gameturn == (GameTurn)(at - 300)))
    {
        /* (a joiner has no portal yet so early: players 0 and 1 get one west of their hearts, 3x3) */
        const RoomKind entk = (RoomKind)get_id(room_desc, "ENTRANCE");
        for (PlayerNumber q = 0; q < 2; q++)
        {
            struct Thing *qh = find_players_dungeon_heart(q);
            if (!thing_exists(qh))
                continue;
            const MapSlabCoord hx = subtile_slab(qh->mappos.x.stl.num), hy = subtile_slab(qh->mappos.y.stl.num);
            struct Room *made = NULL;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -6; dx <= -4; dx++)
                {
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy), q, 0);
                    struct Room *r = place_room(q, entk, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy));
                    if (!room_is_invalid(r))
                        made = r;
                }
            SYNCLOG("World test: defect: player %d gets a portal (%s)", (int)q, (made != NULL) ? "made" : "failed");
        }
        /* KFX_WORLD_DEFECTCAUSE=food|lair: player 1 gets a Hatchery or a Lair east of its heart, 3x3 */
        const char *cause = getenv("KFX_WORLD_DEFECTCAUSE");
        if ((cause != NULL) && ((strcmp(cause, "food") == 0) || (strcmp(cause, "lair") == 0)))
        {
            const RoomKind rk = find_first_roomkind_with_role((strcmp(cause, "food") == 0) ? RoRoF_FoodStorage : RoRoF_LairStorage);
            struct Thing *qh = find_players_dungeon_heart(1);
            int made = 0;
            if (thing_exists(qh))
            {
                const MapSlabCoord hx = subtile_slab(qh->mappos.x.stl.num), hy = subtile_slab(qh->mappos.y.stl.num);
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = 4; dx <= 6; dx++)
                    {
                        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy), 1, 0);
                        if (!room_is_invalid(place_room(1, rk, slab_subtile_center(hx + dx), slab_subtile_center(hy + dy))))
                            made++;
                    }
            }
            SYNCLOG("World test: defect: player 1 gets a %s of %d slabs", cause, made);
        }
        /* (and no creature yet: a Troll by player 0's heart) */
        struct Thing *heart = find_players_dungeon_heart(PLAYER0);
        if (thing_exists(heart))
        {
            struct Coord3d pos = heart->mappos;
            pos.x.val += 3 * COORD_PER_STL * 3;
            pos.z.val = 0;
            struct Thing *troll = create_creature(&pos, (ThingModel)get_id(creature_desc, "TROLL"), PLAYER0);
            if (thing_exists(troll))
                move_thing_in_map(troll, &pos);
            SYNCLOG("World test: defect: player 0 gets a Troll (%s)", thing_exists(troll) ? "made" : "failed");
        }
    }
    static ThingIndex watched = 0;
    if ((at > 0) && (watched > 0) && ((game.play_gameturn == (GameTurn)(at + 300)) || (game.play_gameturn == (GameTurn)(at + 900))))
    {
        struct Thing *w = thing_get(watched);
        struct Room *portal = pick_random_room_of_role(PLAYER0, RoRoF_CrPoolLeave);
        SYNCLOG("World test: defect: Trolls in the pools: player 0 %d, player 1 %d",
            (int)creature_pool_count(0, (ThingModel)get_id(creature_desc, "TROLL")),
            (int)creature_pool_count(1, (ThingModel)get_id(creature_desc, "TROLL")));
        SYNCLOG("World test: defect: the creature %s, state %d, at slab (%d,%d); player 0 %s a portal", thing_exists(w) ?
            "is here" : "is gone", thing_exists(w) ? (int)get_creature_state_besides_interruptions(w) : -1,
            thing_exists(w) ? (int)subtile_slab(w->mappos.x.stl.num) : 0, thing_exists(w) ? (int)subtile_slab(w->mappos.y.stl.num) : 0,
            room_is_invalid(portal) ? "has no" : "has");
    }
    if ((at <= 0) || (game.play_gameturn != (GameTurn)at))
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    if (dungeon_invalid(dungeon))
        return;
    struct Thing *thing = INVALID_THING;
    long guard = 0;
    for (ThingIndex i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *c = thing_get(i);
        struct CreatureControl *cc = creature_control_get_from_thing(c);
        if (!thing_exists(c) || creature_control_invalid(cc))
            break;
        i = cc->players_next_creature_idx;
        if ((get_creature_model_flags(c) & CMF_IsSpectator) || (strcmp(thing_model_name(c), "creature FLOATING_SPIRIT") == 0))
            continue;
        thing = c;
        break;
    }
    if (!thing_exists(thing))
    {
        SYNCLOG("World test: defect: player 0 has no creature");
        return;
    }
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const char *cause = getenv("KFX_WORLD_DEFECTCAUSE");
    const int reason = ((cause != NULL) && (strcmp(cause, "food") == 0)) ? AngR_Hungry :
        (((cause != NULL) && (strcmp(cause, "lair") == 0)) ? AngR_NoLair : AngR_NotPaid);
    cctrl->annoyance_level[reason] = 100000;
    const TbBool sent = external_set_thing_state(thing, CrSt_CreatureLeavingDungeon);
    watched = thing->index;
    SYNCLOG("World test: defect: player 0's %s %d is angry (reason %d) and %s", thing_model_name(thing),
        (int)thing->index, reason, sent ? "leaves for its portal" : "can't set off");
}

/* The line for the creature each player looks at, worked out by the server into the game state */
void world_defectors_turn(void)
{
    if (!world_mode_active())
        return;
    df_test();
    if ((game.play_gameturn % 16) != 11)
        return;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        const ThingIndex idx = game.world_misdeed_view_thing[p];
        struct Thing *thing = thing_get(idx);
        const struct CreatureControl *cctrl = ((idx > 0) && thing_exists(thing) && (thing->class_id == TCls_Creature)) ?
            creature_control_get_from_thing(thing) : NULL;
        const struct DfLine *line = (cctrl != NULL) ? df_line_of(cctrl->misdeed_id) : NULL;
        char text[sizeof(game.world_defector_line[0])];
        if (line != NULL)
            snprintf(text, sizeof(text), "Came over from %s (%s), day %d", (line->from_name[0] != 0) ? line->from_name :
                "another keeper", df_cause_words[line->cause % 4], (int)line->day);
        else
            text[0] = 0;
        if (strcmp(text, game.world_defector_line[p]) != 0)
            memcpy(game.world_defector_line[p], text, sizeof(text));
        game.world_defector_line_thing[p] = (line != NULL) ? idx : 0;
    }
}

const char *world_defectors_rap_line(ThingIndex thing_idx)
{
    const PlayerNumber me = my_player_number;
    if (!world_mode_active() || (me >= PLAYERS_MAX) || (thing_idx == 0) || (game.world_defector_line_thing[me] != thing_idx) ||
        (game.world_defector_line[me][0] == 0))
        return NULL;
    return game.world_defector_line[me];
}

/* Saving (XTR2 record DEF1) */
void world_defectors_reset(void)
{
    memset(df_lines, 0, sizeof(df_lines));
    df_count = 0;
}

size_t world_defectors_save_size(void)
{
    return (df_count > 0) ? 4 + (size_t)df_count * sizeof(struct DfLine) : 0;
}

void world_defectors_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)df_count;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, df_lines, (size_t)n * sizeof(struct DfLine));
}

TbBool world_defectors_load(const uint8_t *buf, size_t len)
{
    world_defectors_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > DF_MAX) || (len < 4 + (size_t)n * sizeof(struct DfLine)))
        return false;
    memcpy(df_lines, buf + 4, (size_t)n * sizeof(struct DfLine));
    df_count = (int)n;
    return true;
}

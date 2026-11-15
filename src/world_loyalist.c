/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_loyalist.c
 *     Endless world: the last loyalist.
 * @par Purpose:
 *     See world_loyalist.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_loyalist.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "bflib_guibtns.h"
#include "world_stream.h"
#include "world_misdeeds.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "thing_stats.h"
#include "creature_control.h"
#include "config_creature.h"
#include "config_strings.h"
#include "gui_frontbtns.h"
#include "frontend.h"
#include "keeperfx.hpp"
#include "net_state.h"
#include "packets.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* the box's choices (this machine's keeper): survivors, then the veteran's body, then nobody */
#define WL_CHOICES 8
struct WlChoice {
    ThingIndex idx;            /* a survivor, or WORLD_LOYALIST_VETERAN, or 0: nobody */
    int32_t born;              /* its creation turn */
    char label[48];
};
static struct WlChoice wl_choices[WL_CHOICES];
static int wl_count, wl_pick;
char world_loyalist_label[64] = "Walks with you: nobody";

/* the simulating machine: the choice a keeper sent, and the loyalist on its way */
struct WlPending {
    uint32_t par1, par2;       /* from the packet */
    ThingModel model;          /* 0: none */
    CrtrExpLevel exp_level;    /* already halved */
    uint16_t md_id;            /* its Misdeeds record (0: none) */
    char name[CREATURE_NAME_MAX];
};
static struct WlPending wl_pending[PLAYERS_MAX];

/* the simulating machine: each keeper's creatures as they were when it was defeated. The dungeon's fall kills them a
 * few seconds later, often before the keeper has pressed Start again; a creature chosen from the box then still walks
 * along, as it was at the defeat (without its Misdeeds record, which went with its death). */
#define WL_SNAP 16
struct WlSnap {
    ThingIndex idx;
    int32_t born;
    ThingModel model;
    CrtrExpLevel exp_level;
    uint16_t md;             /* its Misdeeds record, kept frozen if it died after the defeat (0: none) */
    char name[CREATURE_NAME_MAX];
};
static struct WlSnap wl_snap[PLAYERS_MAX][WL_SNAP];

static uint8_t wl_snap_n[PLAYERS_MAX];
static uint8_t wl_lost[PLAYERS_MAX];

/** The records kept for a defeated keeper's noted creatures go (not chosen, or the notes start again). */
static void release_orphans(void);
static void release_snap_records(PlayerNumber p)
{
    for (int k = 0; (k < wl_snap_n[p]) && (k < WL_SNAP); k++)
        if (wl_snap[p][k].md != 0)
        {
            world_misdeeds_release(wl_snap[p][k].md);
            wl_snap[p][k].md = 0;
        }
}

/* the loyalists that haven't fought yet (one a keeper), saved */
struct WlResting {
    ThingIndex idx;
    int32_t born;
};
static struct WlResting wl_rest[PLAYERS_MAX];

static TbBool is_digger(const struct Thing *thing)
{
    return flag_is_set(get_creature_model_flags(thing), CMF_IsSpecDigger);
}

static const char *creature_kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    return get_string(crconf->namestr_idx);
}

/** The dead list's greatest veteran (model, level, name), false when there is none. */
static TbBool best_veteran(PlayerNumber p, struct CreatureStorage *out)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (dungeon_invalid(dungeon))
        return false;
    int best = -1;
    for (int i = 0; (i < dungeon->dead_creatures_count) && (i < DEAD_CREATURES_MAX_COUNT); i++)
    {
        const struct CreatureStorage *cs = &dungeon->dead_creatures[i];
        if ((cs->model <= 0) || (cs->model >= game.conf.crtr_conf.model_count))
            continue;
        if ((best < 0) || (cs->exp_level > dungeon->dead_creatures[best].exp_level))
            best = i;
    }
    if (best < 0)
        return false;
    *out = dungeon->dead_creatures[best];
    return true;
}

static void wl_label_update(void)
{
    if ((wl_pick < 0) || (wl_pick >= wl_count))
        snprintf(world_loyalist_label, sizeof(world_loyalist_label), "Walks with you: nobody");
    else
        snprintf(world_loyalist_label, sizeof(world_loyalist_label), "Walks with you: %s", wl_choices[wl_pick].label);
}

void world_loyalist_box_shown(void)
{
    wl_count = 0;
    wl_pick = 0;
    const PlayerNumber p = my_player_number;
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (!dungeon_invalid(dungeon))
    {
        /* the survivors: renamed ones first, then by level */
        struct { struct Thing *t; int score; } all[64];
        int n = 0;
        long i = dungeon->creatr_list_start, guard = 0;
        while ((i > 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing))
                break;
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (is_digger(thing) || creature_is_dying(thing) || flag_is_set(get_creature_model_flags(thing), CMF_IsSpectator))
                continue;
            const int score = ((cctrl->creature_name[0] != '\0') ? 1000 : 0) + cctrl->exp_level;
            if (n < 64)
            {
                all[n].t = thing;
                all[n].score = score;
                n++;
            } else
            {
                int worst = 0;
                for (int k = 1; k < n; k++)
                    if (all[k].score < all[worst].score)
                        worst = k;
                if (score > all[worst].score)
                {
                    all[worst].t = thing;
                    all[worst].score = score;
                }
            }
        }
        for (int k = 0; (k < n) && (wl_count < WL_CHOICES - 2); k++)
        {
            int best = k;
            for (int j = k + 1; j < n; j++)
                if (all[j].score > all[best].score)
                    best = j;
            const struct Thing *t = all[best].t;
            all[best] = all[k];
            const struct CreatureControl *cctrl = creature_control_get_from_thing(t);
            struct WlChoice *c = &wl_choices[wl_count++];
            c->idx = t->index;
            c->born = t->creation_turn;
            if (cctrl->creature_name[0] != '\0')
                snprintf(c->label, sizeof(c->label), "%s, %s %d", cctrl->creature_name, creature_kind_name(t->model),
                    (int)cctrl->exp_level + 1);
            else
                snprintf(c->label, sizeof(c->label), "%s, level %d", creature_kind_name(t->model), (int)cctrl->exp_level + 1);
        }
    }
    struct CreatureStorage vet;
    if (best_veteran(p, &vet))
    {
        struct WlChoice *c = &wl_choices[wl_count++];
        c->idx = WORLD_LOYALIST_VETERAN;
        c->born = 0;
        snprintf(c->label, sizeof(c->label), "the body of %s %d", creature_kind_name(vet.model), (int)vet.exp_level + 1);
    }
    struct WlChoice *none = &wl_choices[wl_count++];
    none->idx = 0;
    none->born = 0;
    snprintf(none->label, sizeof(none->label), "nobody");
    wl_label_update();
    char all_text[400];
    int at_c = 0;
    for (int k = 0; (k < wl_count) && (at_c < (int)sizeof(all_text) - 2); k++)
        at_c += snprintf(all_text + at_c, sizeof(all_text) - at_c, "%s%s", (k > 0) ? "; " : "", wl_choices[k].label);
    SYNCLOG("World: the Start again box offers %d loyalist choice(s): %s", wl_count, all_text);
}

void world_loyalist_button_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (wl_count > 0)
        wl_pick = (wl_pick + 1) % wl_count;
    wl_label_update();
}

void world_loyalist_choice(unsigned long *par1, unsigned long *par2)
{
    *par1 = 0;
    *par2 = 0;
    if ((wl_pick >= 0) && (wl_pick < wl_count))
    {
        *par1 = wl_choices[wl_pick].idx;
        *par2 = (uint32_t)wl_choices[wl_pick].born;
    }
    SYNCLOG("World: the last loyalist chosen: %s", world_loyalist_label);
}

/******************************************************************************/
void world_loyalist_choose(PlayerNumber plyr_idx, unsigned long par1, unsigned long par2)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    wl_pending[plyr_idx].par1 = (uint32_t)par1;
    wl_pending[plyr_idx].par2 = (uint32_t)par2;
}

void world_loyalist_depart(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX) || !world_mode_active())
        return;
    struct WlPending *w = &wl_pending[p];
    w->model = 0;
    w->md_id = 0;
    w->name[0] = '\0';
    if (w->par1 == WORLD_LOYALIST_VETERAN)
    {
        struct CreatureStorage vet;
        if (best_veteran(p, &vet))
        {
            w->model = vet.model;
            w->exp_level = (CrtrExpLevel)max(0, (vet.exp_level + 1) / 2 - 1);
            snprintf(w->name, sizeof(w->name), "%s", vet.creature_name);
            SYNCLOG("World: player %d's last loyalist: the body of its %s (level %d)", (int)p, thing_class_and_model_name(TCls_Creature, vet.model),
                (int)vet.exp_level + 1);
        }
    } else if ((w->par1 > 0) && (w->par1 < THINGS_COUNT))
    {
        struct Thing *thing = thing_get(w->par1);
        if (thing_exists(thing) && thing_is_creature(thing) && (thing->owner == p) && (thing->creation_turn == (int32_t)w->par2)
            && !is_digger(thing) && !creature_is_dying(thing) && !flag_is_set(get_creature_model_flags(thing), CMF_IsSpectator))
        {
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            w->model = thing->model;
            w->exp_level = (CrtrExpLevel)max(0, (cctrl->exp_level + 1) / 2 - 1);
            memcpy(w->name, cctrl->creature_name, sizeof(w->name));
            w->name[sizeof(w->name) - 1] = '\0';
            w->md_id = world_misdeeds_freeze(thing); /* its record goes with it */
            SYNCLOG("World: player %d's last loyalist: its %s%s%s (level %d), walking with it", (int)p, thing_model_name(thing),
                (w->name[0] != '\0') ? " " : "", w->name, (int)cctrl->exp_level + 1);
            world_remove_creature_quietly(thing);
        } else
        {
            /* fallen with the dungeon since the defeat? it follows all the same, as it was then */
            struct WlSnap *sn = NULL;
            for (int k = 0; (k < wl_snap_n[p]) && (k < WL_SNAP) && (sn == NULL); k++)
                if ((wl_snap[p][k].idx == (ThingIndex)w->par1) && (wl_snap[p][k].born == (int32_t)w->par2))
                    sn = &wl_snap[p][k];
            if (sn != NULL)
            {
                w->model = sn->model;
                w->exp_level = (CrtrExpLevel)max(0, (sn->exp_level + 1) / 2 - 1);
                memcpy(w->name, sn->name, sizeof(w->name));
                w->name[sizeof(w->name) - 1] = '\0';
                w->md_id = sn->md; /* its record, kept when it died */
                sn->md = 0;
                SYNCLOG("World: player %d's last loyalist fell with the dungeon; its %s%s%s (level %d) walks with it all the same",
                    (int)p, thing_class_and_model_name(TCls_Creature, sn->model), (w->name[0] != '\0') ? " " : "", w->name,
                    (int)sn->exp_level + 1);
            } else
                SYNCLOG("World: player %d's chosen loyalist is no longer there", (int)p);
        }
    }
    w->par1 = w->par2 = 0;
    release_snap_records(p);
}

void world_loyalist_dying(struct Thing *thing)
{
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()) || !thing_exists(thing))
        return;
    const PlayerNumber p = thing->owner;
    if ((p < 0) || (p >= PLAYERS_MAX) || !wl_lost[p])
        return;
    for (int k = 0; (k < wl_snap_n[p]) && (k < WL_SNAP); k++)
    {
        struct WlSnap *sn = &wl_snap[p][k];
        if ((sn->idx != thing->index) || (sn->born != thing->creation_turn) || (sn->md != 0))
            continue;
        /* a noted creature of a defeated keeper dies (the dungeon's fall): its record waits for the choice */
        const uint16_t id = world_misdeeds_freeze(thing);
        if (id != 0)
        {
            creature_control_get_from_thing(thing)->misdeed_id = 0;
            sn->md = id;
        }
        return;
    }
}

void world_loyalist_arrive(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    struct WlPending *w = &wl_pending[p];
    wl_rest[p].idx = 0;
    if (w->model <= 0)
        return;
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
    {
        SYNCLOG("World: player %d's last loyalist has no Heart to go to", (int)p);
        w->model = 0;
        return;
    }
    struct Coord3d pos = heart->mappos;
    pos.y.val += 5 * COORD_PER_STL;
    struct Thing *thing = create_creature(&pos, w->model, p);
    if (thing_is_invalid(thing))
    {
        SYNCLOG("World: player %d's last loyalist could not be made", (int)p);
        w->model = 0;
        return;
    }
    init_creature_level(thing, w->exp_level);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (w->name[0] != '\0')
        snprintf(cctrl->creature_name, sizeof(cctrl->creature_name), "%s", w->name);
    if (w->md_id != 0)
    {
        world_misdeeds_thaw(thing, w->md_id);
        SYNCLOG("World: player %d's last loyalist keeps its Misdeeds record", (int)p);
    }
    wl_rest[p].idx = thing->index;
    wl_rest[p].born = thing->creation_turn;
    SYNCLOG("World: player %d's last loyalist arrived at the new Heart: %s%s%s, level %d, resting until its first fight",
        (int)p, thing_model_name(thing), (w->name[0] != '\0') ? " " : "", w->name, (int)cctrl->exp_level + 1);
    w->model = 0;
}

TbBool world_loyalist_resting(const struct Thing *thing)
{
    if (!thing_exists(thing) || (thing->owner >= PLAYERS_MAX))
        return false;
    const struct WlResting *r = &wl_rest[thing->owner];
    return (r->idx == thing->index) && (r->born == thing->creation_turn);
}

/** KFX_WORLD_LOYALTEST=turn (test switch): at that turn this machine's keeper gets an Orc of level 8 named Gnasher,
 *  an Orc of level 4 and a Fly of level 2 beside its Heart (lose and start again with KFX_WORLD_TESTLOSE and
 *  KFX_WORLD_RESPAWN to see one walk along). */
static void loyal_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_LOYALTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_LOYALTEST=%ld", at);
    }
    if ((at >= 0) && ((long)game.play_gameturn == at + 100) && !netstate_dedicated())
    {
        /* the box's default choice, then Start again as its button does (lose first with KFX_WORLD_TESTLOSE) */
        const PlayerNumber me = my_player_number;
        world_loyalist_box_shown();
        const char *pick = getenv("KFX_WORLD_LOYALPICK"); /* test switch: the choice button clicked n times first */
        for (int k = (pick != NULL) ? atoi(pick) : 0; k > 0; k--)
            world_loyalist_button_click(NULL);
        unsigned long par1, par2;
        world_loyalist_choice(&par1, &par2);
        world_loyalist_choose(me, par1, par2);
        SYNCLOG("World: loyalist test: player %d starts again: %s", (int)me, world_seat_respawn(me) ? "ok" : "refused");
        return;
    }
    static ThingIndex second = 0;
    if ((at >= 0) && ((long)game.play_gameturn == at + 50) && (getenv("KFX_WORLD_LOYALKILL") != NULL))
    {
        /* test switch KFX_WORLD_LOYALKILL=1: the level 4 Orc dies, so a body is on the dead list */
        struct Thing *t = thing_get(second);
        if (thing_exists(t) && thing_is_creature(t))
        {
            kill_creature(t, INVALID_THING, -1, CrDed_NoEffects);
            SYNCLOG("World: loyalist test: the level 4 Orc dies");
        }
        return;
    }
    if ((at < 0) || ((long)game.play_gameturn != at))
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
    struct Coord3d pos = heart->mappos;
    pos.y.val += 6 * COORD_PER_STL;
    static const char *const kinds[3] = {"ORC", "ORC", "FLY"};
    static const int levels[3] = {8, 4, 2};
    for (int i = 0; i < 3; i++)
    {
        struct Thing *t = create_creature(&pos, (ThingModel)get_id(creature_desc, kinds[i]), p);
        if (thing_is_invalid(t))
            continue;
        init_creature_level(t, (CrtrExpLevel)(levels[i] - 1));
        if (i == 0)
            snprintf(creature_control_get_from_thing(t)->creature_name, CREATURE_NAME_MAX, "Gnasher");
        if (i == 1)
            second = t->index;
    }
    SYNCLOG("World: loyalist test: three creatures made for player %d", (int)p);
}

/** The simulating machine, each turn: a keeper just defeated has its creatures noted (see struct WlSnap). */
static void snapshot_the_defeated(void)
{
    if (network_is_active() && !netstate_is_server())
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        const struct PlayerInfo *player = get_player(p);
        const TbBool lost = player_exists(player) && world_player_is_human(p) && (player->victory_state == VicS_LostLevel);
        if (lost && !wl_lost[p])
        {
            release_snap_records(p);
            wl_snap_n[p] = 0;
            const struct Dungeon *dungeon = get_players_num_dungeon(p);
            long i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start, guard = 0;
            while ((i > 0) && (guard++ < CREATURES_COUNT))
            {
                const struct Thing *thing = thing_get(i);
                if (!thing_exists(thing))
                    break;
                const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
                if (creature_control_invalid(cctrl))
                    break;
                i = cctrl->players_next_creature_idx;
                if (is_digger(thing) || creature_is_dying(thing) || flag_is_set(get_creature_model_flags(thing), CMF_IsSpectator))
                    continue;
                /* the best WL_SNAP, as the box ranks them (named first, then by level) */
                const int score = ((cctrl->creature_name[0] != '\0') ? 1000 : 0) + cctrl->exp_level;
                int slot = -1;
                if (wl_snap_n[p] < WL_SNAP)
                    slot = wl_snap_n[p]++;
                else
                {
                    int worst = 0, worst_score = 1 << 30;
                    for (int k = 0; k < WL_SNAP; k++)
                    {
                        const int sk = ((wl_snap[p][k].name[0] != '\0') ? 1000 : 0) + wl_snap[p][k].exp_level;
                        if (sk < worst_score)
                        {
                            worst = k;
                            worst_score = sk;
                        }
                    }
                    if (score > worst_score)
                        slot = worst;
                }
                if (slot < 0)
                    continue;
                struct WlSnap *sn = &wl_snap[p][slot];
                sn->idx = thing->index;
                sn->born = thing->creation_turn;
                sn->model = thing->model;
                sn->exp_level = cctrl->exp_level;
                sn->md = 0;
                memcpy(sn->name, cctrl->creature_name, sizeof(sn->name));
                sn->name[sizeof(sn->name) - 1] = '\0';
            }
            char kinds[256];
            int kn = 0;
            kinds[0] = '\0';
            for (int k = 0; (k < wl_snap_n[p]) && (kn < (int)sizeof(kinds) - 40); k++)
                kn += snprintf(kinds + kn, sizeof(kinds) - kn, "%s%s %d", (k > 0) ? ", " : "",
                    thing_class_and_model_name(TCls_Creature, wl_snap[p][k].model), (int)wl_snap[p][k].exp_level + 1);
            SYNCLOG("World: player %d is defeated: %d of its creatures noted for the last loyalist (%s)", (int)p,
                (int)wl_snap_n[p], kinds);
        } else if (!lost && wl_lost[p])
        {
            release_snap_records(p);
            wl_snap_n[p] = 0;
        }
        wl_lost[p] = lost ? 1 : 0;
    }
}

void world_loyalist_turn(void)
{
    release_orphans();
    if (!world_mode_active())
        return;
    loyal_test();
    snapshot_the_defeated();
    if ((game.play_gameturn % 16) != 5)
        return;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        struct WlResting *r = &wl_rest[p];
        if (r->idx == 0)
            continue;
        const struct Thing *thing = thing_get(r->idx);
        if (!thing_exists(thing) || !thing_is_creature(thing) || (thing->creation_turn != r->born) || (thing->owner != p))
        {
            r->idx = 0; /* gone (or frozen with its chunk: it counts again when it is back) */
            continue;
        }
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!creature_control_invalid(cctrl) && (cctrl->combat_flags != 0))
        {
            SYNCLOG("World: player %d's last loyalist (%s) fights: it rests no more and counts toward the raids", (int)p,
                thing_model_name(thing));
            r->idx = 0;
        }
    }
}

/******************************************************************************/
/* records kept frozen for a defeat's notes, read back from a save: the notes themselves are not saved, so these are
   let go at the first turn after the load (Misdeeds has its records by then) */
static uint16_t wl_orphan_md[PLAYERS_MAX][WL_SNAP];
static TbBool wl_orphans;

static void release_orphans(void)
{
    if (!wl_orphans)
        return;
    wl_orphans = false;
    int n = 0;
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; k < WL_SNAP; k++)
            if (wl_orphan_md[p][k] != 0)
            {
                world_misdeeds_release(wl_orphan_md[p][k]);
                wl_orphan_md[p][k] = 0;
                n++;
            }
    SYNCLOG("World: the last loyalist: %d records kept for a defeat before the save are let go", n);
}

void world_loyalist_seat_freed(PlayerNumber p)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return;
    release_snap_records(p); /* a seat freed without Start again (left, deleted, /reset): its kept records go too */
    wl_snap_n[p] = 0;
}

static TbBool any_snap_records(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; (k < wl_snap_n[p]) && (k < WL_SNAP); k++)
            if (wl_snap[p][k].md != 0)
                return true;
    return false;
}

void world_loyalist_reset(void)
{
    memset(wl_orphan_md, 0, sizeof(wl_orphan_md));
    wl_orphans = false;
    memset(wl_pending, 0, sizeof(wl_pending));
    memset(wl_rest, 0, sizeof(wl_rest));
    memset(wl_snap_n, 0, sizeof(wl_snap_n)); /* the notes of a defeat: their things belong to the world left behind */
    memset(wl_lost, 0, sizeof(wl_lost));
}

size_t world_loyalist_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (wl_rest[p].idx != 0)
            return 4 + sizeof(wl_rest) + 4 + sizeof(wl_orphan_md);
    return any_snap_records() ? 4 + sizeof(wl_rest) + 4 + sizeof(wl_orphan_md) : 0;
}

void world_loyalist_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, wl_rest, sizeof(wl_rest));
    /* then the records kept frozen for the defeats' notes (older builds read only the part above) */
    uint16_t kept[PLAYERS_MAX][WL_SNAP];
    memset(kept, 0, sizeof(kept));
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; (k < wl_snap_n[p]) && (k < WL_SNAP); k++)
            kept[p][k] = wl_snap[p][k].md;
    const uint32_t m = WL_SNAP;
    memcpy(buf + 4 + sizeof(wl_rest), &m, 4);
    memcpy(buf + 8 + sizeof(wl_rest), kept, sizeof(kept));
}

TbBool world_loyalist_load(const uint8_t *buf, size_t len)
{
    memset(wl_rest, 0, sizeof(wl_rest));
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct WlResting) + 4 > len)
        return false;
    memcpy(wl_rest, buf + 4, sizeof(struct WlResting) * (size_t)min((uint32_t)PLAYERS_MAX, n));
    const size_t at = 4 + (size_t)n * sizeof(struct WlResting);
    uint32_t m = 0;
    if (len >= at + 4)
        memcpy(&m, buf + at, 4);
    if ((m > 0) && (len >= at + 4 + (size_t)n * m * 2))
    {
        for (uint32_t p = 0; (p < n) && (p < PLAYERS_MAX); p++)
            for (uint32_t k = 0; (k < m) && (k < WL_SNAP); k++)
            {
                memcpy(&wl_orphan_md[p][k], buf + at + 4 + ((size_t)p * m + k) * 2, 2);
                wl_orphans |= (wl_orphan_md[p][k] != 0);
            }
    }
    return true;
}

#ifdef __cplusplus
}
#endif

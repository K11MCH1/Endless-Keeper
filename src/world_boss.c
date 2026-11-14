/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_boss.c
 *     Endless world: World Bosses (2026-11-13; see world_boss.h).
 * @par Purpose:
 *     Decisions on the page's open points and on what this first version leaves out (the user was away; the
 *     recommended choice each time):
 *     - When: the countdown is 24 hours of game time, the first a day after the world began. It runs again only when
 *       a boss has been beaten (the user, 2026-11-14): until then the event stays ready, and a lost fight puts it back
 *       to ready with the same boss, whole again. The schedule is the world's, so everyone on a server sees one timer.
 *     - Bosses: a hero-owned creature of the game, by turns the Avatar, the Horned Reaper, a Dragon and a Giant, at
 *       3.5 times its size, three levels above the best creature that the keepers who joined OWN (not the best one
 *       sent: audit 2026-11-13, a keeper who sent its weakest creatures got a weak boss and the same loot). Its
 *       health is scaled to the team it meets (the user, 2026-11-14: "Scale to the team"): three times the combined
 *       health of the creatures that came (a quarter at first; then "Triple its health"; then "Quadrupple its health
 *       again"), and never less than if three of the keepers' healthiest creature had come. Its blows land half as
 *       often as the game would have them ("Decrease its hitting chance"), a blow by hand is 72% of the creature's own
 *       ("Decrease its melee damage by 20%"), and every three seconds it casts one of its spells that is ready, the
 *       rarest first ("Allow the Avatar to use its Spells": left to the game it only ever used its sword); its Word
 *       of Power comes back in ten seconds. (It was 25 times the creature's own health, then 37.5 times: 585,000 for a level 13 Avatar, against
 *       ten test creatures that dealt 190 a second.) It strikes as that creature does, with every ability it has:
 *       each blow at nine tenths of the creature's own, each ability twice as long to come back
 *       (world_boss_reset_turns). (Three times the blow until the user's play test of 2026-11-14: "they hit too
 *       hard. Decrease damage by 70%.")
 *     - How many: up to five creatures a keeper (imps can't go), and ten keepers an event (the user, 2026-11-14; it
 *       was four creatures at first, then any number for a day). The picker lists all of a keeper's creatures that
 *       may go, the best first, eight rows at a time (it scrolls), with none ticked: the player ticks the ones to
 *       send, and Join is greyed out and does nothing while none is ticked.
 *     - The round starts three minutes after the first keeper joined, or at once when every keeper who is playing has
 *       joined (so a keeper alone in a world goes to the fight the moment it clicks Join).
 *     - The world apart (the user, 2026-11-14: "an alternative empty world that Players and their selected
 *       creatures get transported to"): a sealed 31 x 31 slab room cut out of the rock of the world apart
 *       (world_stream.h), the far corner of the world plane where the generator makes nothing and nothing is kept:
 *       no keeper's ground, no structure, no wild pack or raid can ever be in the way, and no spot has to be
 *       looked for. It is cut for each fight and kept loaded while the fight is on; when the fight is over and
 *       everyone has gone, its ground freezes and is forgotten, so the next fight's arena is cut from fresh rock.
 *       A keeper at the fight sees the arena only: its camera stays inside it, and its full-screen map stays on it
 *       (the zoom works). (Before: a room 2,600 to 4,400 slabs from the world's start, clear of the keepers' starts
 *       and the generator's structures.)
 *     - The journey: when the round starts, every keeper who goes sees the game's loading screen (the red disc) for
 *       three seconds. Behind it the arena is cut and each keeper's creatures come in near a corner of their own;
 *       at its end the boss appears and the fight begins for everyone at once. The same screen, two seconds of it,
 *       takes a keeper home: when the 30 seconds after the boss fell are up, when all its creatures have fallen, or
 *       when the fight is lost. (A keeper who did not join sees no loading screen: its game goes on.)
 *     - The pull: the boss draws every creature at the fight as an enemy's Dungeon Heart does. A creature that isn't
 *       fighting walks up to the boss (the game's own combat begins when it senses the boss); none knows fear there,
 *       and one that leaves the arena (Teleport, a spell that calls creatures home) is put back. And the creatures
 *       draw the boss: with nobody to fight it goes for the nearest creature, and it lets go of one that gets more
 *       than three tiles away when another is closer (the user, 2026-11-14; it does not stand in the middle).
 *     - No clock, no health bar: a fight has no time limit and shows neither. It ends for a keeper when all its
 *       creatures have fallen (the keeper goes back to its Dungeon Heart; the others fight on), and for everyone when
 *       the boss falls. Only a fight in which nothing at all is hurt for ten minutes is given up (a guard against a
 *       stuck fight).
 *     - No Hand: the Hand can't pick up anything in the arena; spells work as anywhere.
 *     - Your dungeon meanwhile: no raids come for a dungeon whose keeper is at the fight. (Pausing the whole dungeon,
 *       as the caretaker does for an away player, is left for later.)
 *     - Loot: when the boss falls, everyone who sent creatures gets gold (by the Heart: the imps take it to the
 *       Treasury), two research books and an Increase Level box, which wait in their storage (world_storage.c: the
 *       Storage Room, else the room of the Dungeon Heart) when the 30 seconds are up. Each keeper's share is measured
 *       by its own best creature when the fight began (level L): 1,000 gold for each level of the boss that keeper
 *       alone would have met (L + 3; 5,000 at least), and books of grade 1 + L / 6 (the deepest, 5, from level 24).
 *       (Audit 2026-11-13: it was 5,000 gold plus 1,000 for every fight the world had had, without end, and the
 *       deepest books for everyone, so a new keeper who sent one creature beside a strong keeper's got them too.)
 *       Gathering a drop in the arena that only its owner sees is left for later: each player's loot is their own
 *       by being delivered to them.
 *       A keeper whose creatures all fell before the boss did has gone home and gets no share.
 *     - Losing: when every creature sent has died, the boss leaves and there is no loot; the event is ready again.
 *     - Survivors go back to their keeper's Dungeon Heart.
 *     Saved: BOS1 (the phase, the schedule, who joined with what, the boss, the arena).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_boss.h"
#include "world_stream.h"
#include "world_progress.h"
#include "world_storage.h"
#include "world_gen.h"
#include "world_wild.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "bflib_planar.h"
#include "bflib_mouse.h"
#include "bflib_datetm.h"
#include "config_strings.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "config_creature.h"
#include "config_objects.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_combt.h"
#include "creature_instances.h"
#include "dungeon_data.h"
#include "frontend.h"
#include "engine_redraw.h"
#include "game_legacy.h"
#include "globals.h"
#include "gui_draw.h"
#include "gui_frontmenu.h"
#include "gui_msgs.h"
#include "config_magic.h"
#include "sounds.h"
#include "local_camera.h"
#include "map_blocks.h"
#include "map_data.h"
#include "packets.h"
#include "player_data.h"
#include "player_instances.h"
#include "player_utils.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_shots.h"
#include "config_effects.h"
#include "engine_camera.h"
#include "thing_factory.h"
#include "server_admin.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_objects.h"
#include "thing_physics.h"
#include "vidfade.h"
#include "light_data.h"
#include "frontmenu_ingame_map.h"
#include "net_state.h"
#include "net_game.h"
#include "player_data.h"
#include "observer.h"
#include "frontend.h"
#include "kfx/renderer/RendererManager.h"
#include "world_caretaker.h"
#include "creature_graphics.h"
#include "kjm_input.h"
#include "sprites.h"
#include "custom_sprites.h"
#include "thing_stats.h"
#include "world_misdeeds.h"
#include "config_crtrstates.h"
#include "world_curtain.h"
#include "post_inc.h"

#define WB_EVERY_DEFAULT (24L * 60 * 60 * 20)  /* turns between two fights: 24 hours of game time */
#define WB_OPEN_TURNS (wb_open_turns())         /* how long the others may still join once a keeper has: 3 minutes (KFX_WORLD_BOSSOPEN=turns, tests) */
#define WB_QUIET_TURNS (10 * 60 * 20)           /* a fight in which nothing is hurt for this long is given up (a stuck fight) */
#define WB_LOOT_TURNS (30 * 20)                 /* after the boss falls, then home */
#define WB_TEAM_MAX 255                         /* creatures a keeper may send: as many as it ticks (a list holds this many) */
#define WB_VIEW_MARGIN 5                        /* a keeper's camera stays this many slabs inside the arena's walls */
#define WB_LOSE_SLABS 3                         /* the boss lets go of a creature that is further than this when another is closer */
#define WB_HIT_PERCENT 50                       /* the boss's blows land half as often as the game would have them (the
                                                   user, 2026-11-14: "Decrease its hitting chance") */
#define WB_SPELL_TURNS 60                       /* the boss casts one of its spells this often when one is ready (3 seconds) */
#define WB_PLAYERS_LIMIT 10                     /* keepers at one event (the user, 2026-11-14: "Limit the event to 10 players") */
#define WB_TEAM_LIMIT 5                         /* creatures a keeper may send ("Limit the amount of creatures a Player can
                                                   choose to 5"; any number for a day before that, four at first) */
#define WB_MELEE_PERCENT 72                     /* a blow by hand: 72% of the creature's own (90%, then "Decrease its melee
                                                   damage by 20%"); its spells stay at WB_DAMAGE_PERCENT */
#define WB_WOP_TURNS 200                        /* a boss's Word of Power comes back in ten seconds ("Set the Avatar's Word
                                                   of Power spells to a 10 seconds cooldown"; 40 seconds before) */
#define WB_WOP_REACH_STL 3                      /* a boss's Word of Power reaches a slab further on every side: two slabs
                                                   wider in all ("Increase the width of The Avatar's Word of Power's
                                                   sprite reach by 2 slabs") */
#define WB_WOP_KNOCK_SLABS 3                    /* and throws creatures three slabs further back ("Increase the knockback
                                                   of The Avatar's Word of Power by 3 slabs") */
#define WB_CLEAVE_PERCENT 50                    /* Cleave: this chance at each of the Avatar's blows by hand ... */
#define WB_CLEAVE_KNOCK_SLABS 2                 /* ... to throw the creatures in front of it this far back ... */
#define WB_CLEAVE_REACH (5 * STL_PER_SLB * COORD_PER_STL / 2) /* ... those within two and a half slabs of its middle ... */
#define WB_CLEAVE_ARC DEGREES_60                /* ... and within 60 degrees either side of where it faces (the reach and
                                                   the arc are my numbers: "the enemies in front of him") */
#define WB_BOSS_ABOVE 3                         /* the boss's level above the best creature of the keepers who joined */
#define WB_GOLD_PER_LEVEL 1000                  /* a share's gold for each level of the boss its keeper alone would meet */
#define WB_GOLD_MIN 5000
#define WB_GRADE_LEVELS 6                       /* a share's books: one grade deeper for every six levels of its keeper's best */
/* The arena's middle, world slabs: set when a fight starts, kept in the game state (every machine's camera goes
   there). It is one fixed place in the world apart (world_stream.h): the far corner of the world plane where the
   generator makes nothing and nothing is kept, 80,000 slabs inside it, in the middle of a chunk (the room and its
   walls then lie in three chunks by three). (The first version had this corner too, in generated ground, and hung
   after a load: the map-wide loops walked the box round every loaded page. They walk the loaded ground now. In
   between, the arena was cut 2,600 to 4,400 slabs from the world's start: a save made during such a fight still
   has its arena there, and finishes there.) */
#define WB_ARENA_X ((int64_t)game.world_boss_arena_x)
#define WB_ARENA_Y ((int64_t)game.world_boss_arena_y)
#define WB_APART_X (WORLD_APART_MIN + 80000 + 8)
#define WB_APART_Y (WORLD_APART_MIN + 80000 + 8)
#define WB_ARENA_HALF 15                        /* the room: 31 x 31 slabs */
#define WB_WALL 3                               /* rock round it */
#define WB_SIZE_PERCENT 350                     /* the boss's size */
#define WB_TEAM_PERCENT 300                     /* the boss's health: this percent of the combined health of the creatures
                                                   it meets (the user, 2026-11-14: "Scale to the team") ... */
#define WB_TEAM_FLOOR 3                         /* ... and never less than if this many of the keepers' best creature
                                                   had come (one weakling sent alone met a boss of straw) */
#define WB_DAMAGE_PERCENT 90                    /* each blow at nine tenths of the creature's own (three times as hard
                                                   until that test: "they hit too hard", "Decrease damage by 70%") */
#define WB_SLOW_TIMES 2                         /* and each attack's cooldown twice as long */

static const char *const wb_boss_kinds[4] = {"AVATAR", "HORNY", "DRAGON", "GIANT"};
static const char *const wb_boss_names[4] = {"the Avatar", "the Horned Reaper", "a Dragon", "a Giant"};

/* The server's state (saved; the phase, its end, who joined and the boss's kind and health are also in the game
   state, game.world_boss_*, for every machine's timer) */
static long wb_every = -1;
static TbBool wb_arena_cut;
static ThingIndex wb_boss_idx;
static ThingIndex wb_picks[PLAYERS_MAX][WB_TEAM_MAX]; /* the creatures each keeper sends (0: taken off the team) */
static uint16_t wb_pick_n[PLAYERS_MAX];               /* how many places of that list are used */
static ThingIndex wb_pend[PLAYERS_MAX][WB_TEAM_MAX];  /* a join that is still arriving, piece by piece */
static uint16_t wb_pend_n[PLAYERS_MAX];
static GameTurn wb_fight_start;
/* The journey (the user, 2026-11-14: "All Players get the loading screen with the red disc. Players and Creatures
   are transported to a different world where the Arena is drawn."): when a round starts, the keepers who go see the
   game's loading screen (world_curtain.c) for WB_TRAVEL_TURNS. WB_TRAVEL_LEAD turns into it, behind the screens, the
   arena is cut and their teams and views are taken there; at its end the boss appears and the fight begins for
   everyone at once. The phase is WBP_Fight from the journey's first turn (the keepers are away from then on) and
   world_boss_until its last; the boss's thing in the game state stays 0 until it is there. */
#define WB_TRAVEL_TURNS (3 * 20)
#define WB_TRAVEL_LEAD 4
#define WB_HOME_TURNS (2 * 20)                        /* the loading screen on the way home (this machine's own) */
static uint8_t wb_travel;                             /* 0: no journey; 1: the screens are up, the teams still at
                                                         home; 2: the teams are in the arena, the boss not yet */
/* The way home: a keeper's views are held at its Dungeon Heart until this turn. A joined machine sends where its view
   is with every packet, as a place modulo 170 slabs that the server reads as the one nearest its own copy of that
   camera: a packet sent from the arena that arrives after the server has taken the view home is read as a place a
   few dozen slabs from the Heart, and the player came home to black rock. The loading screen is up for as long, so
   nobody is panning. (On the way in the arena's box holds the views: wb_bound_cameras.) */
static GameTurn wb_home_pin[PLAYERS_MAX];
static long wb_quiet_sum;                             /* the health of everything at the fight, added up ... */
static GameTurn wb_quiet_since;                       /* ... and since when it has been the same */

static long wb_open_turns(void)
{
    static long open = -1;
    if (open < 0)
    {
        const char *v = getenv("KFX_WORLD_BOSSOPEN");
        open = ((v != NULL) && (atol(v) > 0)) ? atol(v) : 3 * 60 * 20;
    }
    return open;
}

/** The percent of its team's combined health that a boss gets (test switch KFX_WORLD_BOSSPERCENT=<n>: another
 *  percent, to try a longer or a shorter fight in a played game without a new build). */
static long wb_team_percent(void)
{
    static long percent = -1;
    if (percent < 0)
    {
        const char *v = getenv("KFX_WORLD_BOSSPERCENT");
        percent = ((v != NULL) && (atol(v) > 0)) ? atol(v) : WB_TEAM_PERCENT;
        if (v != NULL)
            SYNCLOG("World boss test: a boss has %ld%% of its team's health", percent);
    }
    return percent;
}

static long wb_every_turns(void)
{
    if (wb_every < 0)
    {
        const char *v = getenv("KFX_WORLD_BOSSEVERY");
        wb_every = ((v != NULL) && (atol(v) > 0)) ? atol(v) : WB_EVERY_DEFAULT;
        if (v != NULL)
            SYNCLOG("World boss test: a fight every %ld turns", wb_every);
    }
    return wb_every;
}

static TbBool wb_engine_box(MapSlabCoord *x1, MapSlabCoord *y1, MapSlabCoord *x2, MapSlabCoord *y2)
{
    MapSlabCoord cx, cy;
    if (!world_engine_slab(WB_ARENA_X, WB_ARENA_Y, &cx, &cy))
        return false;
    *x1 = cx - WB_ARENA_HALF;
    *y1 = cy - WB_ARENA_HALF;
    *x2 = cx + WB_ARENA_HALF;
    *y2 = cy + WB_ARENA_HALF;
    return true;
}

static uint8_t wb_got_books[PLAYERS_MAX], wb_got_box[PLAYERS_MAX]; /* loot collected in the arena */
static uint8_t wb_care[PLAYERS_MAX]; /* the caretaker keeps the dungeon because its keeper is at the fight */
static uint8_t wb_top[PLAYERS_MAX];  /* the level of each keeper's best creature when the fight began (its share's measure) */

/** The level a keeper's share is measured by: its best creature's when the fight began. */
static int wb_share_level(PlayerNumber p)
{
    return (wb_top[p] > 0) ? (int)wb_top[p] : world_keeper_top_level(p); /* (0: a fight saved before this was kept) */
}

static long wb_share_gold(PlayerNumber p)
{
    return max((long)WB_GOLD_MIN, (long)WB_GOLD_PER_LEVEL * (wb_share_level(p) + WB_BOSS_ABOVE));
}

static int wb_share_grade(PlayerNumber p)
{
    return min(5, 1 + wb_share_level(p) / WB_GRADE_LEVELS);
}

PlayerNumber world_boss_loot_owner(const struct Thing *thing)
{
    if (!world_mode_active() || (game.world_boss_phase != WBP_Loot) || !thing_exists(thing) || (thing->index == 0))
        return PLAYER_NONE;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; k < 4; k++)
            if (game.world_boss_loot[p][k] == thing->index)
                return p;
    return PLAYER_NONE;
}

TbBool world_boss_loot_hidden(const struct Thing *thing)
{
    const PlayerNumber o = world_boss_loot_owner(thing);
    return (o != PLAYER_NONE) && (o != my_player_number);
}

TbBool world_boss_collect(struct Thing *thing, PlayerNumber plyr_idx)
{
    const PlayerNumber o = world_boss_loot_owner(thing);
    if ((o == PLAYER_NONE) || (o != plyr_idx))
        return false;
    if (thing->model == world_progress_book_model())
        wb_got_books[o]++;
    else
        wb_got_box[o]++;
    for (int k = 0; k < 4; k++)
        if (game.world_boss_loot[o][k] == thing->index)
            game.world_boss_loot[o][k] = 0;
    SYNCLOG("World boss: player %d collects a %s", (int)o, thing_model_name(thing));
    delete_thing_structure(thing, 0);
    return true;
}

TbBool world_boss_take_gold(struct Thing *thing, PlayerNumber plyr_idx)
{
    /* (the pile went into the Hand as any gold does, and with no Treasury in the arena the Hand was stuck holding it;
       the user, 2026-11-14: "picking up gold should go straight to the Gold counter") */
    if (!thing_exists(thing) || (thing->class_id != TCls_Object) || !object_is_gold_pile(thing))
        return false;
    const PlayerNumber o = world_boss_loot_owner(thing);
    if ((o == PLAYER_NONE) || (o != plyr_idx))
        return false;
    const long gold = thing->valuable.gold_stored;
    struct Coord3d pos = thing->mappos;
    pos.z.val += 128;
    /* the floating number and the sound of gold picked up, as the Hand's own pick-up gives them */
    struct Thing *number = (gold > 0) ? create_price_effect(&pos, o, gold) : INVALID_THING;
    const struct PowerConfigStats *powerst = get_power_model_stats(PwrK_PICKUPGOLD);
    if (thing_exists(number) && (powerst != NULL))
        thing_play_sample(number, powerst->select_sound_idx, NORMAL_PITCH, 0, 3, 0, 2, FULL_LOUDNESS);
    if (gold > 0)
        player_add_offmap_gold(o, gold);
    for (int k = 0; k < 4; k++)
        if (game.world_boss_loot[o][k] == thing->index)
            game.world_boss_loot[o][k] = 0;
    const struct Dungeon *dungeon = get_players_num_dungeon(o);
    SYNCLOG("World boss: player %d takes its %ld gold: straight to its gold, now %ld", (int)o, gold,
        dungeon_invalid(dungeon) ? -1L : (long)dungeon->total_money_owned);
    delete_thing_structure(thing, 0);
    return true;
}

TbBool world_boss_blocks_pickup(const struct Thing *thing, PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (game.world_boss_phase < WBP_Fight) || !thing_exists(thing))
        return false;
    const PlayerNumber lo = world_boss_loot_owner(thing);
    if (lo != PLAYER_NONE)
        return (lo != plyr_idx); /* its own player's loot only */
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
        return false;
    const MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
    return (sx >= x1 - WB_WALL) && (sx <= x2 + WB_WALL) && (sy >= y1 - WB_WALL) && (sy <= y2 + WB_WALL);
}

long world_boss_damage_percent(const struct Thing *thing)
{
    if ((wb_boss_idx == 0) || (thing == NULL) || (thing->index != wb_boss_idx) || !world_mode_active() ||
        (game.world_boss_phase != WBP_Fight))
        return 100;
    return WB_DAMAGE_PERCENT;
}

long world_boss_melee_percent(const struct Thing *thing)
{
    if ((wb_boss_idx == 0) || (thing == NULL) || (thing->index != wb_boss_idx) || !world_mode_active() ||
        (game.world_boss_phase != WBP_Fight))
        return 100;
    return WB_MELEE_PERCENT;
}

long world_boss_reset_turns(const struct Thing *thing, int inst_idx, long turns)
{
    /* (the first version pushed the turn an ability was last used into the future, and took the pushed turn for a
       new use when it came round: after its first use every ability was put off for good, and the boss never struck
       again; the user saw it stand still with creatures in front of it) */
    if ((wb_boss_idx == 0) || (thing == NULL) || (thing->index != wb_boss_idx) || !world_mode_active() ||
        (game.world_boss_phase != WBP_Fight))
        return turns;
    static long wop = -2; /* Word of Power's number among the abilities */
    if (wop == -2)
        wop = get_id(instance_desc, "WORD_OF_POWER");
    if ((wop > 0) && (inst_idx == wop))
        return WB_WOP_TURNS;
    return turns * WB_SLOW_TIMES;
}

TbBool world_boss_no_fear(const struct Thing *thing)
{
    /* (a mark on the creatures, set once a turn, was not enough: the game's own combat takes such a mark off as it
       begins a fight and asks about fear in the same step, so the weak ran from the boss the moment they reached it) */
    if (!world_mode_active() || (game.world_boss_phase != WBP_Fight) || (thing == NULL) || (wb_boss_idx == 0))
        return false;
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
        return false;
    const MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
    return (sx >= x1) && (sx <= x2) && (sy >= y1) && (sy <= y2);
}

TbBool world_boss_takes_all(const struct Thing *victim)
{
    return (wb_boss_idx != 0) && (victim != NULL) && (victim->index == wb_boss_idx) && world_mode_active() &&
        (game.world_boss_phase == WBP_Fight);
}

long world_boss_release_attackers(struct Thing *victim, TbBool melee)
{
    if (!world_boss_takes_all(victim))
        return 0;
    /* (the attackers without a place are not in the boss's lists: they are the creatures at the fight that have the
       boss as their enemy in this kind of combat; those with a place were let go just before this is asked) */
    long num = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p] != 1)
            continue;
        for (int k = 0; k < (int)wb_pick_n[p]; k++)
        {
            struct Thing *thing = thing_get(wb_picks[p][k]);
            if ((wb_picks[p][k] == 0) || !thing_exists(thing) || (thing->class_id != TCls_Creature))
                continue;
            const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (creature_control_invalid(cctrl) || (cctrl->combat.battle_enemy_idx != victim->index) ||
                ((cctrl->combat_flags & (melee ? CmbtF_Melee : CmbtF_Ranged)) == 0))
                continue;
            if (melee ? remove_melee_attacker(thing, victim) : remove_ranged_attacker(thing, victim))
                num++;
        }
    }
    return num;
}

TbBool world_boss_truce(const struct Thing *thing1, const struct Thing *thing2)
{
    /* (found in a test on a server, 2026-11-14: two keepers who were not allied sent eight creatures each, and
       the sixteen fought each other to the death beside a boss that lost 83 health of 337,500) */
    if (!world_mode_active() || (game.world_boss_phase < WBP_Fight) || (thing1 == NULL) || (thing2 == NULL) ||
        (thing1->owner == thing2->owner))
        return false;
    const PlayerNumber p1 = thing1->owner, p2 = thing2->owner;
    if ((p1 < 0) || (p1 >= PLAYERS_MAX) || (p2 < 0) || (p2 >= PLAYERS_MAX) || (game.world_boss_joined[p1] != 1) ||
        (game.world_boss_joined[p2] != 1))
        return false;
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
        return false;
    const MapSlabCoord ax = subtile_slab(thing1->mappos.x.stl.num), ay = subtile_slab(thing1->mappos.y.stl.num);
    const MapSlabCoord bx = subtile_slab(thing2->mappos.x.stl.num), by = subtile_slab(thing2->mappos.y.stl.num);
    return (ax >= x1) && (ax <= x2) && (ay >= y1) && (ay <= y2) && (bx >= x1) && (bx <= x2) && (by >= y1) && (by <= y2);
}

TbBool world_boss_spares(const struct Thing *victim, PlayerNumber from)
{
    /* (the keepers at the fight need not be allied; whether a truce stops harm at all is the world's friendly-fire
       variable, world_truce.c, which asks this) */
    if (!world_mode_active() || (game.world_boss_phase < WBP_Fight) || (victim == NULL) ||
        (victim->class_id != TCls_Creature) || (from < 0) || (from == PLAYER_GOOD))
        return false;
    const PlayerNumber p = victim->owner;
    if ((p < 0) || (p >= PLAYERS_MAX) || (from == p) || (game.world_boss_joined[p] != 1))
        return false;
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
        return false;
    const MapSlabCoord sx = subtile_slab(victim->mappos.x.stl.num), sy = subtile_slab(victim->mappos.y.stl.num);
    return (sx >= x1) && (sx <= x2) && (sy >= y1) && (sy <= y2);
}

long world_boss_hit_percent(const struct Thing *shotng)
{
    if ((wb_boss_idx == 0) || (shotng == NULL) || ((ThingIndex)shotng->parent_idx != wb_boss_idx) || !world_mode_active() ||
        (game.world_boss_phase != WBP_Fight))
        return 100;
    return WB_HIT_PERCENT;
}

TbBool world_boss_no_flower(const struct Thing *thing)
{
    return world_mode_active() && (thing != NULL) && (game.world_boss_thing != 0) &&
        (thing->index == game.world_boss_thing) && (thing->class_id == TCls_Creature) &&
        (game.world_boss_phase == WBP_Fight);
}

TbBool world_boss_player_away(PlayerNumber plyr_idx)
{
    return world_mode_active() && (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX) && (game.world_boss_phase >= WBP_Fight) &&
        (game.world_boss_joined[plyr_idx] == 1);
}

/** Where a camera may be while its keeper is at the fight: the arena without WB_VIEW_MARGIN slabs along its walls
 *  (map coordinates). On a joined machine the arena's ground may not have come yet: its place is known all the same. */
static TbBool wb_view_box(MapCoord *bx1, MapCoord *by1, MapCoord *bx2, MapCoord *by2)
{
    MapSlabCoord cx, cy;
    if (!world_engine_slab(WB_ARENA_X, WB_ARENA_Y, &cx, &cy) && !world_engine_slab_unloaded(WB_ARENA_X, WB_ARENA_Y, &cx, &cy))
        return false;
    const int r = WB_ARENA_HALF - WB_VIEW_MARGIN;
    *bx1 = subtile_coord(slab_subtile(cx - r, 0), 0);
    *by1 = subtile_coord(slab_subtile(cy - r, 0), 0);
    *bx2 = subtile_coord(slab_subtile(cx + r, 2), COORD_PER_STL - 1);
    *by2 = subtile_coord(slab_subtile(cy + r, 2), COORD_PER_STL - 1);
    return true;
}

/** Is this machine's keeper at a fight (in the arena, from the fight's start until it goes home)? */
static TbBool wb_me_at_fight(void)
{
    const PlayerNumber me = my_player_number;
    return world_mode_active() && (game.world_boss_phase >= WBP_Fight) && (me >= 0) && (me < PLAYERS_MAX) &&
        (game.world_boss_joined[me] == 1) && !observer_is_active() && !netstate_dedicated();
}

TbBool world_boss_view_box(MapCoord *x1, MapCoord *y1, MapCoord *x2, MapCoord *y2)
{
    return wb_me_at_fight() && wb_view_box(x1, y1, x2, y2);
}

TbBool world_boss_map_locked(int64_t *wx, int64_t *wy)
{
    if (!wb_me_at_fight())
        return false;
    *wx = WB_ARENA_X;
    *wy = WB_ARENA_Y;
    return true;
}

/* A keeper's creatures are waiting in the world apart with no fight on (wb_sweep_apart): its ground stays loaded */
static TbBool wb_waiting_apart;

void world_boss_want_chunks(void (*want)(int64_t wx, int64_t wy, int r))
{
    if (!world_mode_active())
        return;
    if (game.world_boss_phase >= WBP_Fight)
        want(WB_ARENA_X, WB_ARENA_Y, 2);
    else if (wb_waiting_apart)
        want(WB_APART_X, WB_APART_Y, 2);
}

/** Where this fight's arena goes: the world apart. */
static TbBool wb_choose_arena(void)
{
    if (!world_apart_slab(WB_APART_X - WB_ARENA_HALF - WB_WALL - 16, WB_APART_Y - WB_ARENA_HALF - WB_WALL - 16))
        return false;
    game.world_boss_arena_x = (int32_t)WB_APART_X;
    game.world_boss_arena_y = (int32_t)WB_APART_Y;
    SYNCLOG("World boss: the arena goes to the world apart, world slab (%ld,%ld)", (long)WB_APART_X, (long)WB_APART_Y);
    return true;
}

/** Does an object of this kind already stand on this slab's middle? (A lost fight is fought again in the same arena:
 *  its lights are not put up twice.) */
static TbBool wb_has_object(ThingModel model, MapSlabCoord sx, MapSlabCoord sy)
{
    const struct Map *mapblk = get_map_block_at(slab_subtile_center(sx), slab_subtile_center(sy));
    long guard = 0;
    for (ThingIndex i = get_mapwho_thing_index(mapblk); (i != 0) && (guard++ < (long)THINGS_COUNT);)
    {
        const struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_on_mapblk;
        if ((thing->class_id == TCls_Object) && (thing->model == model))
            return true;
    }
    return false;
}

/** The arena: rock round a room of claimed-less path, cut on the map (its chunks are loaded first). */
static TbBool wb_cut_arena(void)
{
    world_load_chunks_round_world(WB_ARENA_X, WB_ARENA_Y, 2);
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
    {
        ERRORLOG("World boss: the arena's ground didn't load");
        return false;
    }
    for (MapSlabCoord y = y1 - WB_WALL; y <= y2 + WB_WALL; y++)
        for (MapSlabCoord x = x1 - WB_WALL; x <= x2 + WB_WALL; x++)
        {
            const TbBool inside = (x >= x1) && (x <= x2) && (y >= y1) && (y <= y2);
            struct SlabMap *slb = get_slabmap_block(x, y);
            if (slabmap_block_invalid(slb))
                continue;
            const SlabKind want = inside ? SlbT_PATH : SlbT_ROCK;
            if (slb->kind == want)
                continue;
            place_slab_type_on_map(want, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
            do_slab_efficiency_alteration(x, y);
        }
    {
        /* light: candlesticks along the walls and on a grid inside, temple statues in a ring round the middle (the first time) */
        const long candle = get_id(object_desc, "CANDLESTCK"), statue = get_id(object_desc, "TEMPLE_STATUE");
        int lights = 0;
        for (int k = 0; k <= 30; k += 6)
        {
            const MapSlabCoord at[4][2] = {{x1 + k, y1}, {x1 + k, y2}, {x1, y1 + k}, {x2, y1 + k}};
            for (int j = 0; (j < 4) && (candle > 0); j++)
            {
                struct Coord3d pos;
                pos.x.val = subtile_coord_center(slab_subtile_center(at[j][0]));
                pos.y.val = subtile_coord_center(slab_subtile_center(at[j][1]));
                pos.z.val = get_floor_height_at(&pos);
                if (!wb_has_object((ThingModel)candle, at[j][0], at[j][1]) &&
                    !thing_is_invalid(create_object(&pos, (ThingModel)candle, game.neutral_player_num, -1)))
                    lights++;
            }
        }
        /* and candlesticks on a grid inside, every 6 slabs: the middle, where the fight is, is lit too */
        for (int gy = 3; (gy < 30) && (candle > 0); gy += 6)
            for (int gx = 3; gx < 30; gx += 6)
            {
                struct Coord3d pos;
                pos.x.val = subtile_coord_center(slab_subtile_center(x1 + gx));
                pos.y.val = subtile_coord_center(slab_subtile_center(y1 + gy));
                pos.z.val = get_floor_height_at(&pos);
                if (!wb_has_object((ThingModel)candle, x1 + gx, y1 + gy) &&
                    !thing_is_invalid(create_object(&pos, (ThingModel)candle, game.neutral_player_num, -1)))
                    lights++;
            }
        const int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
        const int ring[4][2] = {{-5, -5}, {5, -5}, {-5, 5}, {5, 5}};
        for (int j = 0; (j < 4) && (statue > 0); j++)
        {
            struct Coord3d pos;
            pos.x.val = subtile_coord_center(slab_subtile_center(cx + ring[j][0]));
            pos.y.val = subtile_coord_center(slab_subtile_center(cy + ring[j][1]));
            pos.z.val = get_floor_height_at(&pos);
            if (!wb_has_object((ThingModel)statue, cx + ring[j][0], cy + ring[j][1]) &&
                !thing_is_invalid(create_object(&pos, (ThingModel)statue, game.neutral_player_num, -1)))
                lights++;
        }
        SYNCLOG("World boss: %d new lights in the arena", lights);
    }
    wb_arena_cut = true;
    /* its light, now: a dedicated server works light out only round its own view, so the arena's pages would go to
       the players unlit */
    light_render_area_now(slab_subtile(x1 - WB_WALL, 0), slab_subtile(y1 - WB_WALL, 0), slab_subtile(x2 + WB_WALL, 2),
        slab_subtile(y2 + WB_WALL, 2));
    SYNCLOG("World boss: the arena is cut at engine slabs (%d,%d)-(%d,%d)", (int)x1, (int)y1, (int)x2, (int)y2);
    return true;
}

/** The server moves the dungeon cameras of a player's users (game state): a camera packet holds a position relative
 *  to where the server has the camera, so a jump of thousands of slabs can't come from the player's machine (it came
 *  as 40 slabs, and the stream never sent the arena). The player's machine moves its own camera too. */
static void wb_users_camera(PlayerNumber p, MapCoord x, MapCoord y)
{
    for (NetUserId u = 0; u < MAX_NET_USERS; u++)
    {
        if (get_net_user_player_number(u) != p)
            continue;
        struct UserState *ustate = get_user_state(u);
        if (user_state_invalid(ustate))
            continue;
        ustate->dungeon_camera.x = x;
        ustate->dungeon_camera.y = y;
    }
}

static void wb_move_to(struct Thing *thing, MapSlabCoord sx, MapSlabCoord sy, int k)
{
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(sx)) + (MapCoord)(((k * 53) % 129) - 64);
    pos.y.val = subtile_coord_center(slab_subtile_center(sy)) + (MapCoord)(((k * 97) % 129) - 64);
    pos.z.val = 0;
    pos.z.val = get_thing_height_at(thing, &pos);
    move_thing_in_map(thing, &pos);
    reset_interpolation_of_thing(thing);
    create_effect(&thing->mappos, TngEff_RoomSparkeMedium, thing->owner);
}

/* the creation turn of each pick, so a thing index used again for another creature is not taken for it (not saved:
   after a load 0, which is not checked) */
static GameTurn wb_pick_turn[PLAYERS_MAX][WB_TEAM_MAX];

/** May this creature go to the fight: not held in an enemy's Prison or Torture Chamber, not knocked out (a captive
 *  keeps its owner, and the fight would carry it out of its cell and send it home). */
static TbBool wb_free_to_go(const struct Thing *thing)
{
    return !creature_is_kept_in_custody(thing) && !creature_is_being_unconscious(thing);
}

/** Can this keeper send this thing: a living creature of its own that is free to go, not an imp, and not the spirit
 *  a joined player's seat keeps as a creature. */
static TbBool wb_may_go(const struct Thing *thing, PlayerNumber p)
{
    return thing_exists(thing) && (thing->class_id == TCls_Creature) && (thing->owner == p) && !creature_is_dying(thing) &&
        !thing_is_creature_digger(thing) && ((get_creature_model_flags(thing) & CMF_IsSpectator) == 0) &&
        wb_free_to_go(thing);
}

/** Is this one of the creatures a player sent, still alive? */
static TbBool wb_pick_alive(PlayerNumber p, int k, struct Thing **out)
{
    struct Thing *thing = thing_get(wb_picks[p][k]);
    if ((wb_picks[p][k] == 0) || !thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->owner != p) ||
        creature_is_dying(thing) || ((wb_pick_turn[p][k] != 0) && (thing->creation_turn != wb_pick_turn[p][k])))
        return false;
    *out = thing;
    return true;
}

/** The greatest health among the creatures a keeper owns, sent or not (not its imps, not a joined seat's spirit). */
static long wb_keeper_top_health(PlayerNumber p)
{
    long top = 0;
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    long guard = 0;
    for (long i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        const struct Thing *thing = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!thing_exists(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (creature_is_dying(thing) || thing_is_creature_digger(thing) ||
            ((get_creature_model_flags(thing) & CMF_IsSpectator) != 0))
            continue;
        top = max(top, (long)cctrl->max_health);
    }
    return top;
}

/** Test switch KFX_WORLD_BOSSPLAIN=1: the boss's Word of Power and blows as the game has them (no wider reach, no
 *  further throw, no Cleave), to measure what the changes add. */
static TbBool wb_plain(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("KFX_WORLD_BOSSPLAIN") != NULL) ? 1 : 0;
    return (on != 0);
}

static TbBool wb_watching(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("KFX_WORLD_BOSSWATCH") != NULL) ? 1 : 0;
    return (on != 0);
}

/* (the watch, KFX_WORLD_BOSSWATCH: how far a throw carried the creatures it threw, at their furthest in the two
   seconds after it) */
#define WB_THROW_WATCH 24
static ThingIndex wb_tw_idx[WB_THROW_WATCH];
static MapCoordDelta wb_tw_dist[WB_THROW_WATCH], wb_tw_far[WB_THROW_WATCH];
static int wb_tw_n;
static GameTurn wb_tw_turn;
static struct Coord3d wb_tw_from;
static const char *wb_tw_what = "";

static void wb_throw_watch_turn(void)
{
    if (wb_tw_n == 0)
        return;
    for (int i = 0; i < wb_tw_n; i++)
    {
        const struct Thing *thing = thing_get(wb_tw_idx[i]);
        if (thing_exists(thing) && (thing->class_id == TCls_Creature))
            wb_tw_far[i] = max(wb_tw_far[i], get_2d_distance(&wb_tw_from, &thing->mappos));
    }
    if ((GameTurnDelta)(game.play_gameturn - wb_tw_turn) < 40)
        return;
    long sum = 0, most = 0;
    for (int i = 0; i < wb_tw_n; i++)
    {
        const long d = (long)(wb_tw_far[i] - wb_tw_dist[i]);
        sum += d;
        most = max(most, d);
    }
    SYNCLOG("World boss watch: %s threw %d creatures back: %ld subtiles on average, %ld at the most (hundredths: %ld, %ld)",
        wb_tw_what, wb_tw_n, sum / wb_tw_n / COORD_PER_STL, most / COORD_PER_STL, sum * 100 / wb_tw_n / COORD_PER_STL,
        most * 100 / COORD_PER_STL);
    wb_tw_n = 0;
}

/** Throws a creature back from a point by about `slabs` slabs: a push that the game's own sliding carries that far
 *  (a thing loses inertia/256 of its speed each turn: its floor inertia on the ground, its air inertia above it, so
 *  a push of v slides v * 256 / inertia in all). A creature walks back towards its enemy while it still slides:
 *  measured in a fight of fifty, a push worked out for 9 subtiles carried creatures 5.3 on average and one for 6
 *  carried them 3.7, so the push is five thirds of what the sliding alone would need. */
static void wb_throw_back(struct Thing *thing, const struct Coord3d *from, int slabs, const char *what)
{
    if (slabs > 0) /* (0: the watch only measures, for the test switch KFX_WORLD_BOSSPLAIN) */
    {
        const long dist = (long)slabs * STL_PER_SLB * COORD_PER_STL;
        const TbBool in_air = (thing->mappos.z.val > thing->floor_height);
        const long inertia = max(1L, (long)(in_air ? thing->inertia_air : thing->inertia_floor));
        const long speed = max(1L, dist * inertia * 5 / (256 * 3));
        const long angle = get_angle_xy_to(from, &thing->mappos);
        thing->veloc_push_add.x.val += distance_with_angle_to_coord_x(speed, angle);
        thing->veloc_push_add.y.val += distance_with_angle_to_coord_y(speed, angle);
        thing->state_flags |= TF1_PushAdd;
        thing->pushed_by_player = PLAYER_GOOD;
        thing->last_turn_pushed = game.play_gameturn;
    }
    if (wb_watching() && ((wb_tw_n == 0) || ((wb_tw_turn == game.play_gameturn) && (wb_tw_what == what))) &&
        (wb_tw_n < WB_THROW_WATCH))
    {
        if (wb_tw_n == 0)
        {
            wb_tw_turn = game.play_gameturn;
            wb_tw_from = *from;
            wb_tw_what = what;
        }
        wb_tw_idx[wb_tw_n] = thing->index;
        wb_tw_dist[wb_tw_n] = get_2d_distance(from, &thing->mappos);
        wb_tw_far[wb_tw_n] = wb_tw_dist[wb_tw_n];
        wb_tw_n++;
    }
}

long world_boss_corpse_size(const struct Thing *thing)
{
    return world_boss_takes_all(thing) ? (long)thing->sprite_size : 0;
}

MapCoordDelta world_boss_wop_reach(const struct Thing *caster)
{
    return (world_boss_takes_all(caster) && !wb_plain()) ? (MapCoordDelta)(WB_WOP_REACH_STL * COORD_PER_STL) : 0;
}

long world_boss_effect_spread_percent(const struct Thing *efftng)
{
    /* (by the game state, so that a joined player's machine draws the same flames) */
    if (!world_mode_active() || (efftng == NULL) || (game.world_boss_thing == 0) ||
        (efftng->parent_idx != game.world_boss_thing) || (game.world_boss_phase != WBP_Fight) || wb_plain())
        return 100;
    const struct EffectConfigStats *effcst = get_effect_model_stats(efftng->model);
    if (effcst->area_affect_type != AAffT_WOPDamage)
        return 100;
    const long range = max(1L, (long)get_shot_model_stats(ShM_WordOfPower)->area_range);
    return 100 * (range + WB_WOP_REACH_STL) / range;
}

void world_boss_wop_cast(struct Thing *caster, const struct Coord3d *pos, MapCoordDelta reach)
{
    if (!world_boss_takes_all(caster) || (wb_plain() && !wb_watching()))
        return;
    int thrown = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p] != 1)
            continue;
        for (int k = 0; k < (int)wb_pick_n[p]; k++)
        {
            struct Thing *thing;
            if (!wb_pick_alive(p, k, &thing) || thing_is_picked_up(thing) || (get_2d_distance(pos, &thing->mappos) > reach))
                continue;
            wb_throw_back(thing, pos, wb_plain() ? 0 : WB_WOP_KNOCK_SLABS, "Word of Power");
            thrown++;
        }
    }
    if (wb_watching())
        SYNCLOG("World boss watch: the boss's Word of Power reaches %ld subtiles and throws %d creatures", (long)reach /
            COORD_PER_STL, thrown);
}

void world_boss_cleave(struct Thing *shooter, struct Thing *shotng, const struct Thing *target)
{
    if (!world_boss_takes_all(shooter) || ((game.world_boss_kind % 4) != 0) || wb_plain() || (shotng == NULL))
        return;
    if ((long)THING_RANDOM(shotng, 100) >= WB_CLEAVE_PERCENT)
        return;
    int thrown = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p] != 1)
            continue;
        for (int k = 0; k < (int)wb_pick_n[p]; k++)
        {
            struct Thing *thing;
            if (!wb_pick_alive(p, k, &thing) || thing_is_picked_up(thing) ||
                (get_2d_distance(&shooter->mappos, &thing->mappos) > WB_CLEAVE_REACH))
                continue;
            /* in front of it: the creature the blow is for, and those within the arc of where it faces */
            if ((thing != target) && (get_angle_difference(shooter->move_angle_xy, get_angle_xy_to(&shooter->mappos,
                &thing->mappos)) > WB_CLEAVE_ARC))
                continue;
            wb_throw_back(thing, &shooter->mappos, WB_CLEAVE_KNOCK_SLABS, "Cleave");
            thrown++;
        }
    }
    if (wb_watching())
        SYNCLOG("World boss watch: the boss cleaves: %d creatures thrown back", thrown);
}

/** Where a keeper's team comes into the arena: every keeper has a place of its own near a corner (the user,
 *  2026-11-14: "Players/enemy keepers should spawn in different spots near the edge of the arena's corners"). The
 *  first four keepers take the four corners (opposite ones first), the next four a place seven slabs along the north
 *  and south walls from each corner, the last four seven slabs along the west and east walls: twelve places, and an
 *  event holds ten keepers. A keeper's number is its place among the keepers at the event, by player. */
static void wb_spot(PlayerNumber p, MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2, MapSlabCoord *sx,
    MapSlabCoord *sy)
{
    int n = 0;
    for (PlayerNumber q = 0; (q < p) && (q < PLAYERS_MAX); q++)
        n += game.world_boss_joined[q] ? 1 : 0;
    const int corner = n % 4, ring = (n / 4) % 3;
    const TbBool east = (corner == 1) || (corner == 2), south = (corner == 1) || (corner == 3);
    MapSlabCoord x = east ? x2 - 3 : x1 + 3, y = south ? y2 - 3 : y1 + 3;
    if (ring == 1)
        x += east ? -7 : 7;
    else if (ring == 2)
        y += south ? -7 : 7;
    *sx = x;
    *sy = y;
}

/** A keeper's creature `k` (of its team) goes to its keeper's place: the team stands in a block of three by two. */
static void wb_move_to_spot(struct Thing *thing, PlayerNumber p, int k, MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2,
    MapSlabCoord y2)
{
    MapSlabCoord sx, sy;
    wb_spot(p, x1, y1, x2, y2, &sx, &sy);
    wb_move_to(thing, sx + (k % 3) - 1, sy + ((k / 3) % 2) - 1, k);
}

/** A keeper's join is whole: the creatures of the list that may go are its team. The first keeper to join starts the
 *  wait for the others (WB_OPEN_TURNS). */
static void wb_commit_join(PlayerNumber p)
{
    int n = 0;
    memset(wb_picks[p], 0, sizeof(wb_picks[p]));
    memset(wb_pick_turn[p], 0, sizeof(wb_pick_turn[p]));
    for (int k = 0; (k < (int)wb_pend_n[p]) && (n < WB_TEAM_LIMIT); k++)
    {
        const ThingIndex idx = wb_pend[p][k];
        struct Thing *thing = thing_get(idx);
        if ((idx == 0) || !wb_may_go(thing, p))
            continue;
        TbBool twice = false;
        for (int j = 0; (j < n) && !twice; j++)
            twice = (wb_picks[p][j] == idx);
        if (twice)
            continue;
        wb_pick_turn[p][n] = thing->creation_turn;
        wb_picks[p][n++] = idx;
    }
    const int asked = (int)wb_pend_n[p];
    wb_pick_n[p] = (uint16_t)n;
    wb_pend_n[p] = 0;
    int others = 0;
    for (PlayerNumber q = 0; q < PLAYERS_MAX; q++)
        others += ((q != p) && game.world_boss_joined[q]) ? 1 : 0;
    const TbBool was = (game.world_boss_joined[p] != 0);
    if (!was && (others >= WB_PLAYERS_LIMIT))
    {
        /* the event is full: this keeper stays out */
        memset(wb_picks[p], 0, sizeof(wb_picks[p]));
        memset(wb_pick_turn[p], 0, sizeof(wb_pick_turn[p]));
        wb_pick_n[p] = 0;
        SYNCLOG("World boss: player %d cannot join: the event is full (%d keepers)", (int)p, WB_PLAYERS_LIMIT);
        return;
    }
    game.world_boss_joined[p] = (n > 0) ? 1 : 0;
    if ((n > 0) && !was && (others == 0))
        game.world_boss_until = game.play_gameturn + WB_OPEN_TURNS; /* the first to join: the others have this long */
    SYNCLOG("World boss: player %d %s with %d creatures (%d asked for)", (int)p, (n > 0) ? "joins" : "doesn't join", n,
        asked);
}

/** A join comes in pieces, since a packet has room for three creatures: two thing indexes in par1 and one in the low
 *  half of par2, the piece's place in the list in par2's bits 16-23 and the list's length in bits 24-31. The pieces
 *  come in order; the first empties the list and the last makes the join (a piece out of order is dropped, and the
 *  player's machine sends the join again). Length 0 takes the join back. */
void world_boss_packet_join(PlayerNumber plyr_idx, uint32_t par1, uint32_t par2)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (game.world_boss_phase != WBP_Open))
        return;
    const int at = (int)((par2 >> 16) & 0xFF), total = (int)((par2 >> 24) & 0xFF);
    if (total == 0)
    {
        wb_pend_n[plyr_idx] = 0;
        wb_commit_join(plyr_idx);
        return;
    }
    if (at == 0)
        wb_pend_n[plyr_idx] = 0;
    if (at != (int)wb_pend_n[plyr_idx])
    {
        SYNCLOG("World boss: a piece of player %d's join came out of order (place %d, %d expected): dropped", (int)plyr_idx,
            at, (int)wb_pend_n[plyr_idx]);
        return;
    }
    const ThingIndex in[3] = {(ThingIndex)(par1 & 0xFFFF), (ThingIndex)(par1 >> 16), (ThingIndex)(par2 & 0xFFFF)};
    for (int k = 0; (k < 3) && ((int)wb_pend_n[plyr_idx] < total); k++)
        wb_pend[plyr_idx][wb_pend_n[plyr_idx]++] = in[k];
    if ((int)wb_pend_n[plyr_idx] >= total)
        wb_commit_join(plyr_idx);
}

/** Test switch KFX_WORLD_BOSSKEEPERS: the world's computer keepers take part as players do, for a session with ten
 *  keepers and one person at the mouse (the user, 2026-11-14: "I meant AI keepers, not real players"). =1: each sends
 *  the five best creatures of its own dungeon. =2: each gets the test team the players get (KFX_WORLD_BOSSTEAM) and
 *  sends the five best of that team only, so that all the teams are the same. Without the switch a computer keeper
 *  never joins. */
static int wb_keepers_play(void)
{
    static int mode = -1;
    if (mode < 0)
    {
        const char *v = getenv("KFX_WORLD_BOSSKEEPERS");
        mode = (v != NULL) ? max(0, min(atoi(v), 2)) : 0;
    }
    return mode;
}

static GameTurn wb_team_turn[PLAYERS_MAX]; /* the turn each computer keeper's test team was made (0: it has none) */

/** An awake computer keeper of the world that has its Dungeon Heart. */
static TbBool wb_computer_keeper(PlayerNumber p)
{
    return world_player_is_keeper(p) && !world_keeper_is_passive(p) && thing_exists(find_players_dungeon_heart(p));
}

/** Would this computer keeper come (the test switch): awake, with a Heart, and with a creature it may send? */
static TbBool wb_keeper_will_come(PlayerNumber p, int mode)
{
    if ((mode == 0) || !wb_computer_keeper(p) || ((mode == 2) && (wb_team_turn[p] == 0)))
        return false;
    struct Dungeon *dungeon = get_players_num_dungeon(p);
    long guard = 0;
    for (long i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (!thing_exists(thing) || creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (wb_may_go(thing, p) && !thing_is_picked_up(thing) &&
            ((mode != 2) || (thing->creation_turn == wb_team_turn[p])))
            return true;
    }
    return false;
}

#define WB_KEEPER_GAP 40 /* turns between two computer keepers being ready (two seconds) */

/** The computer keepers' part in an event that is ready (the test switch above). Each awake one joins as soon as a
 *  player is ready, with its five best creatures (the highest levels; of one level, the healthiest), while the event
 *  has room (WB_PLAYERS_LIMIT keepers). They are ready one after another, two seconds apart, the first two seconds
 *  after the player, so that the player's window counts them in ("Players ready: 3 of 10") and the last of them
 *  starts the fight, as the last player to click Ready does; the round waits for them (wb_all_joined). A computer
 *  keeper gets no share of the loot, its dungeon is not held and nothing is shown to it. When no player is ready any
 *  more (Ready was clicked again), the computer keepers are out again: a fight never begins without a player. */
static void wb_keepers_join(void)
{
    const int mode = wb_keepers_play();
    if (mode == 0)
        return;
    int humans = 0, joined = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!game.world_boss_joined[p])
            continue;
        joined++;
        humans += world_player_is_human(p) ? 1 : 0;
    }
    static GameTurn next_at; /* the turn the next computer keeper is ready (0: no player is ready) */
    if (humans == 0)
        next_at = 0;
    else if (next_at == 0)
        next_at = game.play_gameturn + WB_KEEPER_GAP;
    const TbBool may = (humans > 0) && ((GameTurnDelta)(game.play_gameturn - next_at) >= 0);
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!world_player_is_keeper(p))
            continue;
        if (humans == 0)
        {
            if (game.world_boss_joined[p])
            {
                SYNCLOG("World boss: no player is joined any more: computer keeper %d is out again", (int)p);
                game.world_boss_joined[p] = 0;
                wb_pick_n[p] = 0;
            }
            continue;
        }
        if (!may || game.world_boss_joined[p] || (joined >= WB_PLAYERS_LIMIT) || !wb_computer_keeper(p) ||
            ((mode == 2) && (wb_team_turn[p] == 0)))
            continue;
        ThingIndex pick[WB_TEAM_LIMIT];
        int level[WB_TEAM_LIMIT];
        long health[WB_TEAM_LIMIT];
        int n = 0;
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        long guard = 0;
        for (long i = dungeon_invalid(dungeon) ? 0 : dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
        {
            struct Thing *thing = thing_get(i);
            const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            if (!thing_exists(thing) || creature_control_invalid(cctrl))
                break;
            i = cctrl->players_next_creature_idx;
            if (!wb_may_go(thing, p) || thing_is_picked_up(thing) ||
                ((mode == 2) && (thing->creation_turn != wb_team_turn[p])))
                continue;
            const int l = (int)cctrl->exp_level + 1;
            const long h = (long)cctrl->max_health;
            int k = n;
            while ((k > 0) && ((level[k - 1] < l) || ((level[k - 1] == l) && (health[k - 1] < h))))
                k--;
            if (k >= WB_TEAM_LIMIT)
                continue; /* weaker than the five it has */
            for (int j = min(n, WB_TEAM_LIMIT - 1); j > k; j--)
            {
                pick[j] = pick[j - 1];
                level[j] = level[j - 1];
                health[j] = health[j - 1];
            }
            pick[k] = thing->index;
            level[k] = l;
            health[k] = h;
            n = min(n + 1, WB_TEAM_LIMIT);
        }
        if (n == 0)
            continue; /* (it has no creature to send yet: asked again next turn) */
        for (int k = 0; k < n; k++)
            wb_pend[p][k] = pick[k];
        wb_pend_n[p] = (uint16_t)n;
        SYNCLOG("World boss: computer keeper %d sends its %d best creatures (levels %d to %d)", (int)p, n, level[n - 1],
            level[0]);
        wb_commit_join(p);
        joined += game.world_boss_joined[p] ? 1 : 0;
        next_at = game.play_gameturn + WB_KEEPER_GAP;
        break; /* (one at a time) */
    }
}

static void wb_give_abilities(struct Thing *boss);

/** The event stays ready, with nobody joined (a fight could not begin: its keepers pick again). */
static void wb_stay_ready(const char *why)
{
    SYNCLOG("World boss: %s: the event stays ready", why);
    wb_travel = 0;
    memset(game.world_boss_joined, 0, sizeof(game.world_boss_joined));
    memset(wb_pick_n, 0, sizeof(wb_pick_n));
    memset(wb_pend_n, 0, sizeof(wb_pend_n));
    game.world_boss_phase = WBP_Open;
    game.world_boss_until = game.play_gameturn;
}

/** Who goes, and what they are measured by: the keepers that joined and have a creature of their team that can still
 *  go. `prune` (when the round starts): a creature that died, lies in the Hand, or was captured or knocked out since
 *  it was picked stays where it is and is off the team (it is not fetched later, and does not count as alive); a
 *  keeper with none left has not joined; and each keeper's best creature is noted (its share's measure). Returns how
 *  many keepers go. */
static int wb_census(TbBool prune, int *best_out, int64_t *team_health_out, long *top_health_out)
{
    int players = 0, best = 1;
    int64_t team_health = 0; /* the health of every creature that goes, added up */
    long top_health = 0;     /* the greatest health of one creature the keepers own */
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (prune)
            wb_top[p] = 0;
        if (!game.world_boss_joined[p] || (!prune && (game.world_boss_joined[p] != 1)))
            continue;
        int n = 0;
        for (int k = 0; k < (int)wb_pick_n[p]; k++)
        {
            struct Thing *thing;
            if (!wb_pick_alive(p, k, &thing) || (prune && (thing_is_picked_up(thing) || !wb_free_to_go(thing))))
            {
                if (prune)
                    wb_picks[p][k] = 0;
                continue;
            }
            n++;
            best = max(best, (int)creature_control_get_from_thing(thing)->exp_level + 1);
            team_health += (int64_t)creature_control_get_from_thing(thing)->max_health;
        }
        if (n == 0)
        {
            if (prune)
            {
                SYNCLOG("World boss: none of player %d's creatures can go: it has not joined", (int)p);
                game.world_boss_joined[p] = 0;
                wb_pick_n[p] = 0;
            }
            continue;
        }
        players++;
        /* the best creature this keeper owns, sent or not */
        if (prune)
            wb_top[p] = (uint8_t)min(world_keeper_top_level(p), 255);
        best = max(best, (int)wb_top[p]);
        top_health = max(top_health, wb_keeper_top_health(p));
    }
    *best_out = best;
    *team_health_out = team_health;
    *top_health_out = top_health;
    return players;
}

static void wb_end(TbBool won);
static void wb_bound_cameras(void);

/** A round starts: the journey begins. The keepers who go are away from this turn on, and their machines put up the
 *  loading screen; the arena, the teams and the boss follow behind it (wb_journey_turn). */
static void wb_start_fight(void)
{
    int best;
    int64_t team_health;
    long top_health;
    const int players = wb_census(true, &best, &team_health, &top_health);
    if (players == 0)
    {
        wb_stay_ready("nobody is left to fight");
        return;
    }
    if (!wb_choose_arena())
    {
        ERRORLOG("World boss: no place for an arena");
        wb_stay_ready("there is no arena");
        return;
    }
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (game.world_boss_joined[p])
            game.world_boss_joined[p] = 1;
    {
        /* the views go now (each machine takes its own view there this turn too; the ground comes with the teams) */
        MapCoord bx1, by1, bx2, by2;
        if (wb_view_box(&bx1, &by1, &bx2, &by2))
            for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
                if ((game.world_boss_joined[p] == 1) && world_player_is_human(p))
                    wb_users_camera(p, (bx1 + bx2) / 2, (by1 + by2) / 2);
    }
    wb_boss_idx = 0;
    game.world_boss_kind = (uint8_t)(game.world_boss_number % 4);
    game.world_boss_health = 100;
    game.world_boss_thing = 0;
    game.world_boss_hp = 0;
    game.world_boss_hp_max = 0;
    wb_travel = 1;
    game.world_boss_phase = WBP_Fight;
    game.world_boss_until = game.play_gameturn + WB_TRAVEL_TURNS;
    SYNCLOG("World boss: the round starts: %d players are on their way to the arena (%d turns behind the loading screen)",
        players, WB_TRAVEL_TURNS);
}

/** The journey, behind the loading screens: the arena is cut in the world apart, and each keeper's creatures come in
 *  at its own place near a corner (wb_spot), its view with them. False: there is no arena (the round is off). */
static TbBool wb_teams_arrive(void)
{
    if ((game.world_boss_arena_x == 0 && game.world_boss_arena_y == 0) || !wb_cut_arena())
        return false;
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
        return false;
    int moved = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p] != 1)
            continue;
        if (world_player_is_human(p)) /* (a computer keeper, test switch KFX_WORLD_BOSSKEEPERS, only sends creatures) */
        {
            reveal_map_area(p, slab_subtile(x1 - 1, 0), slab_subtile(x2 + 1, 2), slab_subtile(y1 - 1, 0),
                slab_subtile(y2 + 1, 2));
            /* the home dungeon is held as an away player's is: no training, research or experience, no alarms, the
               dread held (world_caretaker_hold; no computer takes the seat: the keeper casts spells in the arena) */
            if (!world_caretaker_active(p))
            {
                world_caretaker_hold(p, true);
                wb_care[p] = 1;
            }
            wb_users_camera(p, subtile_coord_center(slab_subtile_center((x1 + x2) / 2)),
                subtile_coord_center(slab_subtile_center((y1 + y2) / 2)));
        }
        for (int k = 0; k < (int)wb_pick_n[p]; k++)
        {
            struct Thing *thing;
            if (!wb_pick_alive(p, k, &thing))
                continue;
            cleanup_current_thing_state(thing);
            wb_move_to_spot(thing, p, k, x1, y1, x2, y2);
            set_start_state(thing);
            moved++;
        }
        {
            MapSlabCoord sx, sy;
            wb_spot(p, x1, y1, x2, y2, &sx, &sy);
            SYNCLOG("World boss: player %d's creatures come in %d slabs from the west wall and %d from the north wall "
                "(the arena is %d slabs square)", (int)p, (int)(sx - x1), (int)(sy - y1), 2 * WB_ARENA_HALF + 1);
        }
    }
    SYNCLOG("World boss: %d creatures are in the arena; the boss comes in %ld turns", moved,
        (long)max((GameTurnDelta)(game.world_boss_until - game.play_gameturn), (GameTurnDelta)0));
    return true;
}

/** The journey's end: the boss appears in the arena's middle, and the fight is on. False: it could not be made. */
static TbBool wb_boss_arrives(void)
{
    int best;
    int64_t team_health;
    long top_health;
    const int players = wb_census(false, &best, &team_health, &top_health);
    MapSlabCoord x1, y1, x2, y2;
    if ((players == 0) || !wb_engine_box(&x1, &y1, &x2, &y2))
        return false;
    const int kind = (int)(game.world_boss_number % 4);
    const long model = get_id(creature_desc, wb_boss_kinds[kind]);
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center((x1 + x2) / 2));
    pos.y.val = subtile_coord_center(slab_subtile_center((y1 + y2) / 2));
    pos.z.val = 0;
    struct Thing *boss = (model > 0) ? create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature,
        (ThingModel)model, PLAYER_GOOD) : INVALID_THING;
    if (thing_is_invalid(boss))
    {
        ERRORLOG("World boss: couldn't make %s", wb_boss_kinds[kind]);
        return false;
    }
#ifdef CREATURE_MAX_LEVEL_LIMIT
    const int cap = CREATURE_MAX_LEVEL_LIMIT;
#else
    const int cap = CREATURE_MAX_LEVEL;
#endif
    /* test switch KFX_WORLD_BOSSWEAK=1: a level 1 boss with a hundredth of its health (to see a fight won) */
    const TbBool weak = (getenv("KFX_WORLD_BOSSWEAK") != NULL);
    init_creature_level(boss, (CrtrExpLevel)(weak ? 0 : min(best + WB_BOSS_ABOVE, cap) - 1));
    struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    /* Its health is scaled to the team it meets: WB_TEAM_PERCENT of the combined health of the creatures that came,
       so a small team and a big one have a fight of much the same length. Never less than if WB_TEAM_FLOOR of the
       keepers' healthiest creature had come: sent alone, a weakling would meet a boss of straw that one Lightning
       fells, for the same loot (the level and the loot are measured by what the keepers own for the same reason). */
    const int64_t measure = max(team_health, (int64_t)WB_TEAM_FLOOR * top_health);
    bctrl->max_health = weak ? max(1L, (long)bctrl->max_health / 100) :
        (HitPoints)max((int64_t)1, min(measure * wb_team_percent() / 100, (int64_t)INT32_MAX));
    {
        /* test switch KFX_WORLD_BOSSHEALTH=<n>: the boss at its real level and strength, with n health (a fight a
           small test team can win, to see the whole event in a played game) */
        const char *hv = getenv("KFX_WORLD_BOSSHEALTH");
        if ((hv != NULL) && (atol(hv) > 0))
            bctrl->max_health = atol(hv);
    }
    boss->health = bctrl->max_health;
    boss->sprite_size = (unsigned short)(game.conf.crtr_conf.sprite_size * WB_SIZE_PERCENT / 100);
    wb_give_abilities(boss);
    wb_boss_idx = boss->index;
    game.world_boss_kind = (uint8_t)kind;
    game.world_boss_health = 100;
    game.world_boss_thing = boss->index;
    game.world_boss_hp = (int32_t)boss->health;
    game.world_boss_hp_max = (int32_t)bctrl->max_health;
    int creatures = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; (game.world_boss_joined[p] == 1) && (k < (int)wb_pick_n[p]); k++)
        {
            struct Thing *thing;
            creatures += wb_pick_alive(p, k, &thing) ? 1 : 0;
        }
    wb_travel = 0;
    wb_fight_start = game.play_gameturn;
    wb_quiet_sum = -1;
    wb_quiet_since = game.play_gameturn;
    game.world_boss_until = game.play_gameturn; /* (a fight has no clock) */
    SYNCLOG("World boss: fight %lu: %s at level %d (%ld health: %ld%% of %lld, the team's combined health is %lld and %d "
        "of the keepers' healthiest creature make %lld) against %d creatures of %d players (the best creature they own: "
        "level %d)", (unsigned long)game.world_boss_number + 1, wb_boss_names[kind], (int)bctrl->exp_level + 1,
        (long)boss->health, wb_team_percent(), (long long)measure, (long long)team_health, WB_TEAM_FLOOR,
        (long long)((int64_t)WB_TEAM_FLOOR * top_health), creatures, players, best);
    server_admin_log("world", "A World Boss fight begins: %s against %d players", wb_boss_names[kind], players);
    return true;
}

/** A turn of the journey (the phase is WBP_Fight, wb_travel says how far it is). */
static void wb_journey_turn(void)
{
    const GameTurnDelta left = (GameTurnDelta)(game.world_boss_until - game.play_gameturn);
    if ((wb_travel == 1) && (left <= WB_TRAVEL_TURNS - WB_TRAVEL_LEAD))
    {
        if (wb_teams_arrive())
            wb_travel = 2;
        else
        {
            SYNCLOG("World boss: there is no arena: everyone stays home, and the event is ready again");
            wb_end(false);
            return;
        }
    }
    wb_bound_cameras();
    if ((wb_travel == 2) && (left <= 0) && !wb_boss_arrives())
    {
        SYNCLOG("World boss: the boss could not be made: everyone goes home, and the event is ready again");
        wb_end(false);
    }
}

/** The boss fell: each player who joined gets a share in the arena, seen by that player only: a gold pile, two deep
 *  research books and an Increase Level box, in a row of its own along the west wall. */
static void wb_drop_loot(void)
{
    memset(game.world_boss_loot, 0, sizeof(game.world_boss_loot));
    memset(wb_got_books, 0, sizeof(wb_got_books));
    memset(wb_got_box, 0, sizeof(wb_got_box));
    MapSlabCoord x1, y1, x2, y2;
    if (!wb_engine_box(&x1, &y1, &x2, &y2))
        return;
    const ThingModel book = world_progress_book_model();
    const long box_model = get_id(object_desc, "SPECBOX_INCLEV");
    int row = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if ((game.world_boss_joined[p] != 1) || !world_player_is_human(p))
            continue; /* (a keeper whose creatures all fell has gone home: no share; nor has a computer keeper one) */
        const MapSlabCoord ly = y1 + 2 + (row * 4) % (2 * WB_ARENA_HALF - 3), lx = x1 + 4;
        row++;
        int n = 0;
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(lx));
        pos.y.val = subtile_coord_center(slab_subtile_center(ly));
        pos.z.val = 0;
        struct Thing *gold = drop_gold_pile(wb_share_gold(p), &pos);
        if (thing_exists(gold))
            game.world_boss_loot[p][n++] = gold->index;
        world_progress_drop_books(slab_subtile_center(lx + 2), slab_subtile_center(ly), wb_share_grade(p), -1, 2);
        const struct StructureList *olist = get_list_for_thing_class(TCls_Object);
        long guard = 0;
        for (ThingIndex i = olist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT) && (n < 3);)
        {
            struct Thing *obj = thing_get(i);
            if (thing_is_invalid(obj))
                break;
            i = obj->next_of_class;
            if ((obj->model == book) && (obj->creation_turn == game.play_gameturn) &&
                (abs((int)subtile_slab(obj->mappos.x.stl.num) - (int)(lx + 2)) <= 2) &&
                (abs((int)subtile_slab(obj->mappos.y.stl.num) - (int)ly) <= 2))
                game.world_boss_loot[p][n++] = obj->index;
        }
        pos.x.val = subtile_coord_center(slab_subtile_center(lx + 4));
        struct Thing *box = (box_model > 0) ? create_object(&pos, (ThingModel)box_model, game.neutral_player_num, -1) : INVALID_THING;
        if (thing_exists(box) && (n < 4))
            game.world_boss_loot[p][n++] = box->index;
        SYNCLOG("World boss: player %d's share lies in the arena: %d things (its best creature was level %d: %ld gold, books "
            "of grade %d)", (int)p, n, wb_share_level(p), wb_share_gold(p), wb_share_grade(p));
    }
}

/** A keeper's creatures leave the arena: what is left of its team goes to its Dungeon Heart, its camera with it, and
 *  its dungeon is its own again. Returns how many came home. */
static int wb_send_home(PlayerNumber p)
{
    struct Thing *heart = find_players_dungeon_heart(p);
    if (thing_exists(heart))
    {
        wb_users_camera(p, heart->mappos.x.val, heart->mappos.y.val);
        wb_home_pin[p] = game.play_gameturn + WB_HOME_TURNS;
    }
    int home = 0;
    for (int k = 0; k < (int)wb_pick_n[p]; k++)
    {
        struct Thing *thing;
        if (!wb_pick_alive(p, k, &thing))
            continue;
        if (thing_exists(heart))
        {
            cleanup_current_thing_state(thing);
            wb_move_to(thing, subtile_slab(heart->mappos.x.stl.num) + 2, subtile_slab(heart->mappos.y.stl.num), k);
            set_start_state(thing);
            home++;
        }
    }
    if (wb_care[p])
    {
        world_caretaker_hold(p, false);
        wb_care[p] = 0;
    }
    return home;
}

/** The fight is over. Won (the 30 seconds after the boss fell are up): the survivors go home, what each keeper
 *  collected waits in its storage (gold went to the Treasury when the Hand took it), what nobody collected is lost,
 *  and the countdown to the next boss begins. Lost (every creature sent has fallen): the boss leaves, there is no
 *  loot, and the event is ready again with the same boss, whole again: the countdown runs again only when a boss has
 *  been beaten. */
static void wb_end(TbBool won)
{
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p] != 1)
            continue;
        struct Thing *heart = find_players_dungeon_heart(p);
        const int home = wb_send_home(p);
        if (won && thing_exists(heart) && (wb_got_books[p] + wb_got_box[p] > 0))
        {
            const MapSubtlCoord bx = heart->mappos.x.stl.num - 3, by = heart->mappos.y.stl.num;
            const ThingModel book = world_progress_book_model();
            if (wb_got_books[p] > 0)
                world_progress_drop_books(bx, by, wb_share_grade(p), -1, wb_got_books[p]);
            int stored = 0;
            const struct StructureList *olist = get_list_for_thing_class(TCls_Object);
            long guard = 0;
            for (ThingIndex i = olist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
            {
                struct Thing *obj = thing_get(i);
                if (thing_is_invalid(obj))
                    break;
                i = obj->next_of_class;
                if ((obj->model == book) && (obj->creation_turn == game.play_gameturn) &&
                    (abs((int)obj->mappos.x.stl.num - (int)bx) <= 6) && (abs((int)obj->mappos.y.stl.num - (int)by) <= 6) &&
                    world_storage_add_loot(obj, p))
                    stored++;
            }
            const long box_model = get_id(object_desc, "SPECBOX_INCLEV");
            for (int k = 0; (k < wb_got_box[p]) && (box_model > 0); k++)
            {
                struct Coord3d bpos = heart->mappos;
                bpos.y.val += 3 * COORD_PER_STL;
                bpos.z.val = 0;
                struct Thing *box = create_object(&bpos, (ThingModel)box_model, game.neutral_player_num, -1);
                if (thing_exists(box) && world_storage_add_loot(box, p))
                    stored++;
            }
            SYNCLOG("World boss: player %d collected %d books and %d boxes (%d in its storage); %d creatures come home",
                (int)p, (int)wb_got_books[p], (int)wb_got_box[p], stored, home);
        } else
            SYNCLOG("World boss: %d of player %d's creatures come home%s", home, (int)p, won ? " (no loot collected)" : "");
        /* what lies uncollected is lost */
        for (int k = 0; k < 4; k++)
        {
            struct Thing *obj = thing_get(game.world_boss_loot[p][k]);
            if ((game.world_boss_loot[p][k] != 0) && thing_exists(obj) && (obj->class_id == TCls_Object) &&
                !thing_is_picked_up(obj))
                delete_thing_structure(obj, 0);
            game.world_boss_loot[p][k] = 0;
        }
    }
    struct Thing *boss = thing_get(wb_boss_idx);
    if ((wb_boss_idx > 0) && thing_exists(boss) && (boss->class_id == TCls_Creature) && !creature_is_dying(boss))
        world_remove_thing_quietly(boss);
    wb_boss_idx = 0;
    wb_travel = 0;
    game.world_boss_thing = 0;
    game.world_boss_hp = 0;
    game.world_boss_hp_max = 0;
    memset(wb_picks, 0, sizeof(wb_picks));
    memset(wb_pick_n, 0, sizeof(wb_pick_n));
    memset(wb_pend_n, 0, sizeof(wb_pend_n));
    memset(wb_pick_turn, 0, sizeof(wb_pick_turn));
    memset(wb_top, 0, sizeof(wb_top));
    memset(game.world_boss_joined, 0, sizeof(game.world_boss_joined));
    if (won)
    {
        game.world_boss_number++;
        game.world_boss_phase = WBP_Waiting;
        game.world_boss_until = game.play_gameturn + (GameTurn)wb_every_turns();
        SYNCLOG("World boss: the countdown to fight %lu begins: %ld turns", (unsigned long)game.world_boss_number + 1,
            wb_every_turns());
    } else
    {
        game.world_boss_phase = WBP_Open;
        game.world_boss_until = game.play_gameturn;
    }
}

/** Test switch KFX_WORLD_BOSSKNOCK=T: at T player 0's highest-level creature (not an imp) is knocked out, so a join
 *  just after shows that a knocked-out (or captive) creature is not sent. */
static void wb_knock_test(void)
{
    static long at = -1;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_BOSSKNOCK");
        at = (v != NULL) ? atol(v) : 0;
    }
    if ((at <= 0) || ((long)game.play_gameturn != at))
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    if (dungeon_invalid(dungeon))
        return;
    struct Thing *best = INVALID_THING;
    int best_lv = -1;
    long guard = 0;
    for (long i = dungeon->creatr_list_start; (i > 0) && (guard++ < CREATURES_COUNT);)
    {
        struct Thing *t = thing_get(i);
        struct CreatureControl *c = creature_control_get_from_thing(t);
        if (!thing_exists(t) || creature_control_invalid(c))
            break;
        i = c->players_next_creature_idx;
        if (!thing_is_creature_digger(t) && (c->exp_level > best_lv))
        {
            best = t;
            best_lv = c->exp_level;
        }
    }
    if (thing_exists(best))
        make_creature_unconscious(best);
    SYNCLOG("World boss test: player 0's %s (index %d) knocked out before the join", thing_exists(best) ?
        thing_model_name(best) : "nothing", thing_exists(best) ? (int)best->index : 0);
}

/** With the watch switch on (KFX_WORLD_BOSSWATCH): every use of an ability by the boss, by whoever started it (the
 *  game's own fighting or the spell timer), with the turns since it last used that one. */
static void wb_watch_uses(const struct Thing *boss)
{
    static int on = -1;
    static GameTurn last[INSTANCE_TYPES_MAX];
    static ThingIndex of_boss;
    if (on < 0)
        on = (getenv("KFX_WORLD_BOSSWATCH") != NULL) ? 1 : 0;
    if (!on)
        return;
    const struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    if (of_boss != boss->index)
    {
        /* another boss: its abilities' turns as they stand are where the count starts */
        of_boss = boss->index;
        memcpy(last, bctrl->instance_use_turn, sizeof(last));
        return;
    }
    for (int32_t i = 1; (i < game.conf.crtr_conf.instances_count) && (i < INSTANCE_TYPES_MAX); i++)
    {
        if (bctrl->instance_use_turn[i] == last[i])
            continue;
        const struct InstanceInfo *inst_inf = creature_instance_info_get(i);
        if (inst_inf->reset_time >= 40) /* (not the hand weapon or Drain: they would fill the log) */
            SYNCLOG("World boss watch: the boss uses %s, %ld turns after the last time", creature_instance_code_name(i),
                (last[i] == 0) ? -1L : (long)(GameTurnDelta)(bctrl->instance_use_turn[i] - last[i]));
        last[i] = bctrl->instance_use_turn[i];
    }
}

/** Test switch KFX_WORLD_BOSSWATCH=n: every n turns of a fight, what the boss and each creature sent are doing. */
static void wb_watch_test(const struct Thing *boss)
{
    static long every = -2;
    if (every == -2)
    {
        const char *v = getenv("KFX_WORLD_BOSSWATCH");
        every = (v != NULL) ? atol(v) : -1;
    }
    if ((every <= 0) || ((game.play_gameturn % every) != 0))
        return;
    const struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    SYNCLOG("World boss watch: the boss: %s then %s, combat flags %02x, instance %d, health %ld of %ld, at (%d,%d), opponents %d melee %d ranged",
        creature_state_code_name(boss->active_state), creature_state_code_name(boss->continue_state), (unsigned)bctrl->combat_flags,
        (int)bctrl->instance_id, (long)boss->health, (long)bctrl->max_health, (int)boss->mappos.x.stl.num, (int)boss->mappos.y.stl.num,
        (int)bctrl->opponents_melee_count, (int)bctrl->opponents_ranged_count);
    int alive = 0, fighting = 0, waiting = 0, walking = 0, fleeing = 0;
    long nearest = -1, farthest = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p] != 1)
            continue;
        for (int k = 0; k < (int)wb_pick_n[p]; k++)
        {
            struct Thing *thing;
            if (!wb_pick_alive(p, k, &thing))
                continue;
            const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            const long dist = (long)(get_chessboard_distance(&thing->mappos, &boss->mappos) / COORD_PER_STL);
            alive++;
            if ((cctrl->combat_flags & CmbtF_Waiting) != 0)
                waiting++;
            else if (cctrl->combat_flags != 0)
                fighting++;
            else if (creature_is_fleeing_combat(thing))
                fleeing++;
            else if (thing->active_state == CrSt_MoveToPosition)
                walking++;
            nearest = (nearest < 0) ? dist : min(nearest, dist);
            farthest = max(farthest, dist);
            if ((alive <= 12) || ((thing->alloc_flags & TAlF_IsControlled) != 0))
                SYNCLOG("World boss watch: player %d's %s%s (level %d): %s then %s, combat flags %02x, instance %d, health %ld of %ld, "
                    "%ld subtiles from the boss", (int)p, ((thing->alloc_flags & TAlF_IsControlled) != 0) ? "taken-over " : "",
                    thing_model_name(thing), (int)cctrl->exp_level + 1,
                    creature_state_code_name(thing->active_state), creature_state_code_name(thing->continue_state),
                    (unsigned)cctrl->combat_flags, (int)cctrl->instance_id, (long)thing->health, (long)cctrl->max_health, dist);
        }
    }
    SYNCLOG("World boss watch: %d creatures alive: %d fighting, %d waiting for a place, %d walking, %d fleeing, %d other; "
        "%ld to %ld subtiles from the boss", alive, fighting, waiting, walking, fleeing, alive - fighting - waiting - walking -
        fleeing, max(nearest, 0L), farthest);
    {
        /* whom the boss is on, and how far that one and the nearest creature are */
        const struct Thing *enemy = (bctrl->combat_flags != 0) ? thing_get(bctrl->combat.battle_enemy_idx) : INVALID_THING;
        if (thing_exists(enemy))
            SYNCLOG("World boss watch: the boss fights player %d's %s, %ld subtiles away; the nearest creature is %ld subtiles away",
                (int)enemy->owner, thing_model_name(enemy), (long)(get_chessboard_distance(&boss->mappos, &enemy->mappos) /
                COORD_PER_STL), max(nearest, 0L));
        else
            SYNCLOG("World boss watch: the boss fights nobody (%s); the nearest creature is %ld subtiles away, its walk ends "
                "%ld subtiles from where it stands", creature_state_code_name(boss->active_state), max(nearest, 0L),
                (long)(get_chessboard_distance(&boss->mappos, &bctrl->moveto_pos) / COORD_PER_STL));
    }
    /* which abilities the boss has used since the last line */
    {
        static GameTurn last_seen[INSTANCE_TYPES_MAX];
        char used[200] = "";
        for (int32_t i = 1; (i < game.conf.crtr_conf.instances_count) && (i < INSTANCE_TYPES_MAX); i++)
        {
            if ((bctrl->instance_use_turn[i] == 0) || (bctrl->instance_use_turn[i] == last_seen[i]))
                continue;
            last_seen[i] = bctrl->instance_use_turn[i];
            const size_t len = strlen(used);
            snprintf(used + len, sizeof(used) - len, " %s", creature_instance_code_name(i));
        }
        if (used[0] != 0)
            SYNCLOG("World boss watch: the boss used:%s", used);
    }
}

/** Every keeper who is playing has joined: nobody is left to wait for, and the round starts. (A keeper who is away, or
 *  whose co-op seat is empty, is not waited for.) */
/** Test switch KFX_WORLD_BOSSWAIT=1: the event counts on one keeper more, who is never ready, so that a round waits
 *  for its clock (KFX_WORLD_BOSSOPEN) even in a single-player game: the window can be seen with this keeper ready
 *  and the others not. */
static TbBool wb_wait_test(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("KFX_WORLD_BOSSWAIT") != NULL) ? 1 : 0;
    return (on != 0);
}

static TbBool wb_all_joined(void)
{
    if (wb_wait_test())
        return false;
    int joined = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p])
        {
            joined++;
            continue;
        }
        if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)) && !world_caretaker_active(p) &&
            !world_coop_seat_away(p))
            return false; /* this one could still join */
    }
    /* (test switch KFX_WORLD_BOSSKEEPERS: the computer keepers that will come are waited for as well) */
    const int mode = wb_keepers_play();
    if ((mode != 0) && (joined > 0) && (joined < WB_PLAYERS_LIMIT))
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            if (!game.world_boss_joined[p] && wb_keeper_will_come(p, mode))
                return false;
    return (joined > 0);
}

/** How many keepers the event counts on: those who are ready and those who could still be (the keepers a round is
 *  waited for, as wb_all_joined has it; with the test switch for computer keepers, those that will come), never more
 *  than an event holds. Every machine's window shows it ("Players ready: 2 of 4"). */
static int wb_count_wanted(void)
{
    const int mode = wb_keepers_play();
    int n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (game.world_boss_joined[p])
            n++;
        else if (world_player_is_human(p) && thing_exists(get_player_soul_container(p)) && !world_caretaker_active(p) &&
            !world_coop_seat_away(p))
            n++;
        else if (wb_keeper_will_come(p, mode))
            n++;
    }
    return min(n + (wb_wait_test() ? 1 : 0), WB_PLAYERS_LIMIT);
}

/** Test switch KFX_WORLD_BOSSTEAM="n,from,to[,turn]": at that turn (60 when not given) every keeper a player plays
 *  (in a single player game: the local keeper) gets n creatures beside its Heart, of ten kinds in turn, their levels
 *  spread evenly from `from` to `to` ("10,1,10": one of each level from 1 to 10), so that the picker and a fight can be
 *  tried in a new world, on a server too. With KFX_WORLD_BOSSKEEPERS=2 every computer keeper that takes part gets the
 *  same team at its Heart, once, when it has woken (a keeper's base is loaded after that turn). */
static void wb_team_test(void)
{
    static int count = -1, from = 1, to = 10, at = 60;
    if (count < 0)
    {
        const char *v = getenv("KFX_WORLD_BOSSTEAM");
        count = 0;
        if (v != NULL)
            sscanf(v, "%d,%d,%d,%d", &count, &from, &to, &at);
        count = max(0, min(count, 60));
        from = max(1, min(from, 100));
        to = max(from, min(to, 100));
    }
    if (count == 0)
        return;
    static const char *const kinds[10] = {"FLY", "BUG", "SPIDER", "DEMONSPAWN", "TROLL", "SORCEROR", "ORC", "HELL_HOUND",
        "DARK_MISTRESS", "BILE_DEMON"};
    const TbBool now = ((long)game.play_gameturn == (long)at);
    const TbBool keepers = (wb_keepers_play() == 2) && ((long)game.play_gameturn > (long)at) &&
        ((game.play_gameturn % 20) == 0);
    if (now)
        memset(wb_team_turn, 0, sizeof(wb_team_turn));
    if (!now && !keepers)
        return;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        const TbBool human = world_player_is_human(p);
        if (human ? !now : (!keepers || (wb_team_turn[p] != 0) || !wb_computer_keeper(p)))
            continue;
        const struct Thing *heart = human ? get_player_soul_container(p) : find_players_dungeon_heart(p);
        if (!thing_exists(heart))
            continue;
        if (!human)
            wb_team_turn[p] = game.play_gameturn;
        int made = 0;
        for (int i = 0; i < count; i++)
        {
            const long model = get_id(creature_desc, kinds[i % 10]);
            if (model <= 0)
                continue;
            struct Coord3d pos = heart->mappos;
            if (human)
            {
                pos.x.val += (2 + (i % 5)) * COORD_PER_STL;
                pos.y.val += (5 + (i / 5) % 4) * COORD_PER_STL;
            } else
            {
                /* (a computer keeper's base is not cleared in a ring: on the Heart's own room, south of the Heart) */
                pos.x.val += ((i % 5) - 2) * COORD_PER_STL;
                pos.y.val += (3 + (i / 5) % 2) * COORD_PER_STL;
            }
            struct Thing *thing = create_creature(&pos, (ThingModel)model, p);
            if (thing_is_invalid(thing))
                continue;
            const int level = (count > 1) ? from + (to - from) * i / (count - 1) : to;
            set_creature_level(thing, (CrtrExpLevel)(level - 1));
            made++;
            SYNCLOG("World boss test: a %s of level %d for player %d", thing_model_name(thing), level, (int)p);
        }
        SYNCLOG("World boss test: %d creatures of levels %d to %d for player %d", made, from, to, (int)p);
    }
}

/** A creature at the fight goes for the boss the way creatures go for an enemy's Dungeon Heart: it knows no fear
 *  there (world_boss_no_fear), and while it isn't fighting it walks up to the boss; the game's own combat begins when
 *  it senses the boss.
 *  (The first version ordered a fight to the death once a second from wherever the creature stood. From across the
 *  arena the order was dropped at once, so the creatures flashed red for a moment and wandered off again; and the
 *  boss, with enemies it could not sense, ran away.) A creature that left the arena (Teleport, a spell that calls
 *  creatures home) is put back: none escapes. */
static void wb_drive(struct Thing *thing, struct Thing *boss, int k, MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2,
    MapSlabCoord y2)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || thing_is_picked_up(thing))
        return;
    const MapSlabCoord sx = subtile_slab(thing->mappos.x.stl.num), sy = subtile_slab(thing->mappos.y.stl.num);
    if ((sx < x1) || (sx > x2) || (sy < y1) || (sy > y2))
    {
        SYNCLOG("World boss: player %d's %s left the arena: it is put back", (int)thing->owner, thing_model_name(thing));
        cleanup_current_thing_state(thing);
        wb_move_to_spot(thing, thing->owner, k, x1, y1, x2, y2); /* (at its keeper's place) */
        set_start_state(thing);
        return;
    }
    if ((thing->alloc_flags & TAlF_IsControlled) != 0)
        return; /* the player has taken it over: it goes where the player steers it (it was marched to the boss
                   under the player's hands; the user, 2026-11-14) */
    if (((game.play_gameturn + (GameTurn)k) % 10) != 0)
        return; /* twice a second each */
    if (creature_is_being_unconscious(thing) || creature_under_spell_effect(thing, CSAfF_Chicken) ||
        creature_under_spell_effect(thing, CSAfF_Freeze))
        return;
    if ((cctrl->combat_flags != 0) || (cctrl->instance_id != CrInst_NULL))
        return; /* fighting, or in the middle of a blow or a spell */
    if (get_chessboard_distance(&thing->mappos, &boss->mappos) < 3 * COORD_PER_STL)
        return; /* at the boss */
    if ((thing->active_state == CrSt_MoveToPosition) &&
        (get_chessboard_distance(&cctrl->moveto_pos, &boss->mappos) < 4 * COORD_PER_STL))
        return; /* on its way to it */
    cleanup_current_thing_state(thing);
    if (setup_person_move_to_coord(thing, &boss->mappos, NavRtF_Default))
        thing->continue_state = CrSt_CreatureDoingNothing;
    else
        set_start_state(thing);
}

/** The boss turns on this creature: it fights it if it senses it and can get at it, else it walks up to it. In a
 *  fight already, it changes its enemy and stays in the fight (ending the fight would let go of every creature that
 *  is fighting the boss, and they would all have to find it again). */
static void wb_boss_goes_for(struct Thing *boss, struct Thing *target)
{
    const struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    const CrAttackType how = creature_can_have_combat_with_creature(boss, target, get_combat_distance(boss, target), 1, 0);
    if (how > AttckT_Unset)
    {
        if (bctrl->combat_flags != 0)
        {
            if (change_current_combat(boss, target, how))
                return;
        } else if (set_creature_in_combat_to_the_death(boss, target, how))
            return;
    }
    if (bctrl->combat_flags != 0)
        set_start_state(boss); /* (it leaves the fight it was in) */
    if (setup_person_move_to_coord(boss, &target->mappos, NavRtF_Default))
        boss->continue_state = CrSt_GoodDoingNothing;
}

/** The creatures at the fight draw the boss as a magnet draws iron (the user, 2026-11-14: "He should walk to creatures
 *  and attack. If creatures flee a few tiles away, he targets another creature that is closer."). With nobody to
 *  fight it goes for the creature nearest to it (a hero left to itself stands about, or walks off looking for a
 *  dungeon). It keeps the creature it fights while that one stays near; once that one is more than WB_LOSE_SLABS
 *  tiles away and another creature is at least a tile closer, it turns on the closer one. Looked at four times a
 *  second, and never in the middle of a blow or a spell. */
static void wb_boss_hunt(struct Thing *boss, struct Thing *nearest, MapCoordDelta nearest_dist)
{
    struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    if (((game.play_gameturn % 5) != 0) || creature_control_invalid(bctrl) || (bctrl->instance_id != CrInst_NULL) ||
        creature_is_being_unconscious(boss) || thing_is_picked_up(boss) || creature_under_spell_effect(boss, CSAfF_Freeze) ||
        creature_under_spell_effect(boss, CSAfF_Chicken))
        return;
    const MapCoordDelta tile = STL_PER_SLB * COORD_PER_STL;
    if (bctrl->combat_flags != 0)
    {
        const struct Thing *now = thing_get(bctrl->combat.battle_enemy_idx);
        if (thing_exists(now) && (now->index == nearest->index))
            return; /* it is on the nearest one */
        const MapCoordDelta now_dist = thing_exists(now) ? get_chessboard_distance(&boss->mappos, &now->mappos) : INT32_MAX;
        if ((now_dist <= WB_LOSE_SLABS * tile) || (nearest_dist + tile > now_dist))
            return; /* the one it fights is still near, or nobody is clearly closer */
        wb_boss_goes_for(boss, nearest);
        return;
    }
    if ((boss->active_state == CrSt_MoveToPosition) &&
        (get_chessboard_distance(&bctrl->moveto_pos, &nearest->mappos) < 3 * COORD_PER_STL))
        return; /* on its way to it */
    wb_boss_goes_for(boss, nearest);
}

/** A World Boss has every ability of its kind, whatever its level. In an endless world a creature learns its third
 *  ability at level 10, its fourth at 20 and so on (world_spread_abilities), so the Avatar a team with a level 10
 *  creature meets, at level 13, had its sword, Armour and Heal and none of its spells (the user, 2026-11-14: "Allow
 *  the Avatar to use its Spells"). Not Invisibility: the creatures could not find a boss they cannot see, and the
 *  fight would stand still. Given again every few turns: a level gained in the fight would take them away. */
static void wb_give_abilities(struct Thing *boss)
{
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(boss);
    struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    if ((crconf == NULL) || creature_control_invalid(bctrl))
        return;
    for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
    {
        const int k = crconf->learned_instance_id[i];
        if ((k <= 0) || (k >= INSTANCE_TYPES_MAX))
            continue;
        const char *name = creature_instance_code_name(k);
        bctrl->instance_available[k] = ((name == NULL) || (strcmp(name, "INVISIBILITY") != 0));
    }
}

static GameTurn wb_next_spell; /* the turn from which the boss may cast its next spell (not saved: a load casts at once) */

/** The boss uses its spells (the user, 2026-11-14: "Allow the Avatar to use its Spells"). Left to the game, a creature
 *  that fights hand to hand takes the first of its abilities that is ready and reaches its enemy, and that is always
 *  its hand weapon: the Avatar never cast Freeze, Drain, its missile or Word of Power. Every WB_SPELL_TURNS the boss
 *  takes one of its other attacking abilities that is ready (the rarest first) and uses it, once the blow it is in
 *  the middle of has ended, on the nearest creature at the fight that the ability reaches and that it can see. (Abilities a creature
 *  uses on itself, Armour or Heal, are the game's own doing as before.) */
static void wb_boss_casts(struct Thing *boss)
{
    struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
    if (((GameTurnDelta)(game.play_gameturn - wb_next_spell) < 0) || creature_control_invalid(bctrl) ||
        creature_is_being_unconscious(boss) || thing_is_picked_up(boss) ||
        creature_under_spell_effect(boss, CSAfF_Freeze) || creature_under_spell_effect(boss, CSAfF_Chicken))
        return;
    /* its hand weapon: the ability the game takes for a blow at arm's length */
    CrInstance weapon = get_best_melee_offensive_weapon(boss, 0);
    if (weapon < 0)
        weapon = -weapon; /* (not ready this turn) */
    CrInstance ready[24];
    int n = 0;
    for (int32_t i = 1; (i < game.conf.crtr_conf.instances_count) && (n < 24); i++)
    {
        const struct InstanceInfo *inst_inf = creature_instance_info_get(i);
        if ((inst_inf->range_min < 0) || (i == weapon) || ((inst_inf->instance_property_flags & (InstPF_RangedAttack |
            InstPF_RangedDebuff | InstPF_MeleeAttack)) == 0))
            continue; /* not an attack, or the hand weapon */
        if (creature_instance_is_available(boss, i) && creature_instance_has_reset(boss, i))
            ready[n++] = (CrInstance)i;
    }
    if (n == 0)
        return;
    /* the rarest first: the ability that takes longest to come back is the one used when it is ready (Freeze, then
       Word of Power, then the missile; Drain, back almost at once, fills the gaps) */
    for (int a = 1; a < n; a++)
        for (int b = a; (b > 0) && (world_boss_reset_turns(boss, ready[b - 1], creature_instance_info_get(ready[b - 1])->reset_time) <
            world_boss_reset_turns(boss, ready[b], creature_instance_info_get(ready[b])->reset_time)); b--)
        {
            const CrInstance t = ready[b - 1];
            ready[b - 1] = ready[b];
            ready[b] = t;
        }
    for (int j = 0; j < n; j++)
    {
        const CrInstance inst = ready[j];
        const struct InstanceInfo *inst_inf = creature_instance_info_get(inst);
        struct Thing *target = INVALID_THING;
        MapCoordDelta target_dist = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        {
            if (game.world_boss_joined[p] != 1)
                continue;
            for (int k = 0; k < (int)wb_pick_n[p]; k++)
            {
                struct Thing *thing;
                if (!wb_pick_alive(p, k, &thing) || creature_is_being_unconscious(thing) || thing_is_picked_up(thing))
                    continue;
                const MapCoordDelta dist = get_combat_distance(boss, thing);
                if ((dist < inst_inf->range_min) || (dist > inst_inf->range_max))
                    continue;
                if (thing_is_invalid(target) || (dist < target_dist))
                {
                    target = thing;
                    target_dist = dist;
                }
            }
        }
        if (thing_is_invalid(target) || !creature_can_see_combat_path(boss, target, target_dist))
            continue;
        if (bctrl->instance_id != CrInst_NULL)
        {
            /* In the middle of a blow: a spell waits for the blow to end, and the hand weapon is held back so that
               this blow is the last before it. (A boss in a crowd swings without a pause: left to that it found a
               free moment for a spell about once in six seconds, and for Word of Power once in a fight.) */
            if ((weapon > 0) && (weapon < INSTANCE_TYPES_MAX))
                bctrl->instance_use_turn[weapon] = game.play_gameturn;
            return;
        }
        creature_turn_to_face(boss, &target->mappos);
        set_creature_instance(boss, inst, target->index, 0);
        wb_next_spell = game.play_gameturn + WB_SPELL_TURNS;
        if (getenv("KFX_WORLD_BOSSWATCH") != NULL) /* (the watch: each spell as it is cast) */
            SYNCLOG("World boss watch: the boss casts %s at player %d's %s, %ld subtiles away", creature_instance_code_name(inst),
                (int)target->owner, thing_model_name(target), (long)(target_dist / COORD_PER_STL));
        return;
    }
}

/** The cameras of the keepers at the fight stay inside the arena (the game state's; each machine bounds its own view
 *  as well, local_camera.c). */
static void wb_bound_cameras(void)
{
    MapCoord bx1, by1, bx2, by2;
    if (!wb_view_box(&bx1, &by1, &bx2, &by2))
        return;
    for (NetUserId u = 0; u < MAX_NET_USERS; u++)
    {
        const PlayerNumber p = get_net_user_player_number(u);
        if ((p < 0) || (p >= PLAYERS_MAX) || (game.world_boss_joined[p] != 1))
            continue;
        struct UserState *ustate = get_user_state(u);
        if (user_state_invalid(ustate))
            continue;
        ustate->dungeon_camera.x = max(bx1, min(ustate->dungeon_camera.x, bx2));
        ustate->dungeon_camera.y = max(by1, min(ustate->dungeon_camera.y, by2));
    }
}

/** Nobody stays in the world apart when no fight is on. Every team is sent home when its fight ends; this finds a
 *  keeper's creature only when its keeper's Dungeon Heart was not on the map then (a party's dungeon freezes while
 *  its player is away). Such a creature waits where it is, and the arena's ground with it (that ground is not kept:
 *  what stands on it when it freezes is gone), until the Heart is on the map again; then it goes there. A hero or a
 *  neutral creature left there is removed. */
static void wb_sweep_apart(void)
{
    if ((game.play_gameturn % 16) != 5)
        return;
    MapSlabCoord ax, ay;
    if ((game.world_boss_phase >= WBP_Fight) || !world_engine_slab(WB_APART_X, WB_APART_Y, &ax, &ay))
    {
        wb_waiting_apart = false; /* a fight keeps the ground itself; ground that isn't loaded holds nobody */
        return;
    }
    int waiting = 0;
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!world_apart_thing(thing) || thing_is_picked_up(thing) || creature_is_dying(thing))
            continue;
        if ((thing->owner == PLAYER_GOOD) || is_neutral_thing(thing))
        {
            SYNCLOG("World boss: a %s of player %d was left in the arena with no fight on: removed", thing_model_name(thing),
                (int)thing->owner);
            world_remove_thing_quietly(thing);
            continue;
        }
        struct Thing *heart = find_players_dungeon_heart(thing->owner);
        if (!thing_exists(heart))
        {
            waiting++; /* its keeper's Heart isn't on the map: it waits */
            continue;
        }
        SYNCLOG("World boss: a %s of player %d was left in the arena with no fight on: it goes to its Dungeon Heart",
            thing_model_name(thing), (int)thing->owner);
        cleanup_current_thing_state(thing);
        wb_move_to(thing, subtile_slab(heart->mappos.x.stl.num) + 2, subtile_slab(heart->mappos.y.stl.num), 0);
        set_start_state(thing);
    }
    if ((waiting > 0) && !wb_waiting_apart)
        SYNCLOG("World boss: %d creatures wait in the arena for their keeper's Dungeon Heart to be on the map again", waiting);
    wb_waiting_apart = (waiting > 0);
}

void world_boss_turn(void)
{
    if (!world_mode_active())
        return;
    wb_knock_test();
    wb_team_test();
    wb_sweep_apart();
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        /* the way home: the views stay at the Heart while the loading screen is up */
        if (wb_home_pin[p] == 0)
            continue;
        struct Thing *heart = find_players_dungeon_heart(p);
        if (((GameTurnDelta)(wb_home_pin[p] - game.play_gameturn) <= 0) || (game.world_boss_joined[p] == 1) || !thing_exists(heart))
        {
            wb_home_pin[p] = 0;
            continue;
        }
        wb_users_camera(p, heart->mappos.x.val, heart->mappos.y.val);
    }
    if (game.world_boss_until == 0)
        game.world_boss_until = game.play_gameturn + (GameTurn)wb_every_turns(); /* a new world (or one saved before) */
    const TbBool due = ((GameTurnDelta)(game.play_gameturn - game.world_boss_until) >= 0);
    switch (game.world_boss_phase)
    {
    case WBP_Waiting:
        if (due)
        {
            /* ready, and it stays ready until a fight is won: no clock runs until a keeper joins */
            game.world_boss_phase = WBP_Open;
            game.world_boss_until = game.play_gameturn;
            memset(game.world_boss_joined, 0, sizeof(game.world_boss_joined));
            memset(wb_pick_n, 0, sizeof(wb_pick_n));
            memset(wb_pend_n, 0, sizeof(wb_pend_n));
            SYNCLOG("World boss: fight %lu is ready: players may join", (unsigned long)game.world_boss_number + 1);
            server_admin_log("world", "A World Boss is ready to fight");
        }
        break;
    case WBP_Open:
    {
        /* the round starts WB_OPEN_TURNS after the first keeper joined (world_boss_until, set by that join), or at
           once when every keeper who is playing has joined */
        wb_keepers_join(); /* (test switch KFX_WORLD_BOSSKEEPERS) */
        if (((game.play_gameturn % 4) == 0) || (game.world_boss_wanted == 0))
            game.world_boss_wanted = (uint8_t)wb_count_wanted();
        int joined = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            joined += game.world_boss_joined[p] ? 1 : 0;
        {
            /* (the log says it when it changes: what every window shows as "Players ready") */
            static int said_ready = -1, said_wanted = -1;
            static long said_event = -1;
            if ((said_ready != joined) || (said_wanted != (int)game.world_boss_wanted) ||
                (said_event != (long)game.world_boss_number))
            {
                if ((joined > 0) || (said_ready > 0))
                    SYNCLOG("World boss: %d of %d players are ready", joined, max((int)game.world_boss_wanted, joined));
                said_ready = joined;
                said_wanted = (int)game.world_boss_wanted;
                said_event = (long)game.world_boss_number;
            }
        }
        /* (or when the event is full: nobody else can join) */
        if ((joined > 0) && (due || wb_all_joined() || (joined >= WB_PLAYERS_LIMIT)))
            wb_start_fight();
        break;
    }
    case WBP_Fight:
    {
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            if (wb_care[p] && (game.world_boss_joined[p] == 1) && !world_caretaker_active(p))
                world_caretaker_hold(p, true); /* (a load: the hold isn't in the caretaker's own state) */
        if (wb_travel != 0)
        {
            wb_journey_turn(); /* the loading screens are up: the teams, then the boss, arrive */
            break;
        }
        wb_bound_cameras();
        struct Thing *boss = thing_get(wb_boss_idx);
        const TbBool boss_alive = (wb_boss_idx > 0) && thing_exists(boss) && (boss->class_id == TCls_Creature) &&
            !creature_is_dying(boss);
        if (!boss_alive)
        {
            SYNCLOG("World boss: the boss has fallen: 30 seconds, then everyone goes home");
            if (wb_watching())
            {
                /* (the watch: the body it left, and the size that body is drawn at) */
                MapSlabCoord bx1, by1, bx2, by2;
                const struct StructureList *dlist = get_list_for_thing_class(TCls_DeadCreature);
                long guard = 0;
                for (ThingIndex i = dlist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT) &&
                    wb_engine_box(&bx1, &by1, &bx2, &by2);)
                {
                    const struct Thing *body = thing_get(i);
                    if (thing_is_invalid(body))
                        break;
                    i = body->next_of_class;
                    const MapSlabCoord sx = subtile_slab(body->mappos.x.stl.num), sy = subtile_slab(body->mappos.y.stl.num);
                    if ((body->owner == PLAYER_GOOD) && (sx >= bx1) && (sx <= bx2) && (sy >= by1) && (sy <= by2))
                        SYNCLOG("World boss watch: the boss's body (%s) is drawn at size %d (a boss alive: %d, the game's "
                            "own for a creature: %d)", thing_model_name(body), (int)body->sprite_size,
                            (int)(game.conf.crtr_conf.sprite_size * WB_SIZE_PERCENT / 100), (int)game.conf.crtr_conf.sprite_size);
                }
            }
            wb_drop_loot();
            server_admin_log("world", "The World Boss has fallen");
            game.world_boss_health = 0;
            game.world_boss_thing = 0;
            game.world_boss_hp = 0;
            game.world_boss_phase = WBP_Loot;
            game.world_boss_until = game.play_gameturn + WB_LOOT_TURNS;
            break;
        }
        struct CreatureControl *bctrl = creature_control_get_from_thing(boss);
        {
            /* test switch KFX_WORLD_BOSSPOSSESS=n (single player): n turns into the fight this machine's keeper takes
               over the first creature it sent, as the Possess spell does, and nobody steers it: the watch shows
               whether it is left standing where the player has it */
            static long at = -2;
            if (at == -2)
            {
                const char *v = getenv("KFX_WORLD_BOSSPOSSESS");
                at = (v != NULL) ? atol(v) : -1;
            }
            const PlayerNumber me = my_player_number;
            if ((at >= 0) && ((long)(GameTurnDelta)(game.play_gameturn - wb_fight_start) == at) && (me >= 0) && (me < PLAYERS_MAX))
                for (int k = 0; k < (int)wb_pick_n[me]; k++)
                {
                    struct Thing *thing;
                    if (!wb_pick_alive(me, k, &thing))
                        continue;
                    const TbBool ok = control_creature_as_controller(get_my_user(), thing);
                    SYNCLOG("World boss test: this player takes over its %s (index %d): %s", thing_model_name(thing),
                        (int)thing->index, ok ? "done" : "refused");
                    break;
                }
        }
        wb_watch_test(boss);
        wb_watch_uses(boss);
        wb_throw_watch_turn();
        game.world_boss_health = (uint8_t)max(1L, min(100L, (long)boss->health * 100 / max(1L, (long)bctrl->max_health)));
        game.world_boss_thing = boss->index;
        game.world_boss_hp = (int32_t)max(0L, (long)boss->health);
        game.world_boss_hp_max = (int32_t)max(1L, (long)bctrl->max_health);
        if ((game.play_gameturn % 8) == 0)
        {
            boss->sprite_size = (unsigned short)(game.conf.crtr_conf.sprite_size * WB_SIZE_PERCENT / 100);
            wb_give_abilities(boss);
        }
        MapSlabCoord x1, y1, x2, y2;
        const TbBool box = wb_engine_box(&x1, &y1, &x2, &y2);
        /* the boss stands its ground (nothing in the arena knows fear, world_boss_no_fear; it would still back off
           from an enemy it cannot sense) and stays in its arena */
        if (creature_is_fleeing_combat(boss))
            set_start_state(boss);
        if (box && !thing_is_picked_up(boss))
        {
            const MapSlabCoord bx = subtile_slab(boss->mappos.x.stl.num), by = subtile_slab(boss->mappos.y.stl.num);
            if ((bx < x1) || (bx > x2) || (by < y1) || (by > y2))
            {
                SYNCLOG("World boss: the boss left the arena: it is put back");
                cleanup_current_thing_state(boss);
                wb_move_to(boss, (x1 + x2) / 2, (y1 + y2) / 2, 0);
                set_start_state(boss);
            }
        }
        int keepers = 0, left = 0;
        long sum = (long)boss->health;
        struct Thing *nearest = INVALID_THING; /* the creature at the fight that is nearest to the boss */
        MapCoordDelta nearest_dist = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        {
            if (game.world_boss_joined[p] != 1)
                continue;
            int alive = 0;
            for (int k = 0; k < (int)wb_pick_n[p]; k++)
            {
                struct Thing *thing;
                if (!wb_pick_alive(p, k, &thing))
                    continue;
                alive++;
                sum += (long)thing->health;
                if (box)
                    wb_drive(thing, boss, k, x1, y1, x2, y2);
                if (!creature_is_being_unconscious(thing) && !thing_is_picked_up(thing))
                {
                    const MapCoordDelta dist = get_chessboard_distance(&boss->mappos, &thing->mappos);
                    if (thing_is_invalid(nearest) || (dist < nearest_dist))
                    {
                        nearest = thing;
                        nearest_dist = dist;
                    }
                }
            }
            if (alive == 0)
            {
                /* all its creatures have fallen: this keeper goes back to its Dungeon Heart; the others fight on */
                SYNCLOG("World boss: every creature of player %d has fallen: it goes home", (int)p);
                wb_send_home(p);
                wb_pick_n[p] = 0;
                game.world_boss_joined[p] = 2;
                continue;
            }
            keepers++;
            left += alive;
        }
        {
            /* (the log: how the fight stands, every fifteen seconds; a played game's log had nothing between the
               fight's start and its end) */
            const long age = (long)(GameTurnDelta)(game.play_gameturn - wb_fight_start);
            if ((age > 0) && ((age % 300) == 0))
                SYNCLOG("World boss: %ld seconds into the fight: the boss has %ld of %ld health (%ld%%); %d creatures of %d "
                    "keepers are left", age / 20, (long)boss->health, (long)bctrl->max_health, (long)boss->health * 100 /
                    max(1L, (long)bctrl->max_health), left, keepers);
        }
        if (box && !thing_is_invalid(nearest))
        {
            wb_boss_hunt(boss, nearest, nearest_dist);
            wb_boss_casts(boss);
        }
        if (sum != wb_quiet_sum)
        {
            wb_quiet_sum = sum;
            wb_quiet_since = game.play_gameturn;
        }
        const TbBool stuck = ((GameTurnDelta)(game.play_gameturn - wb_quiet_since) >= WB_QUIET_TURNS);
        if ((keepers == 0) || stuck)
        {
            SYNCLOG("World boss: %s: the boss leaves, no loot; the event is ready again", (keepers == 0) ?
                "every creature sent has fallen" : "nothing has been hurt for ten minutes");
            server_admin_log("world", "The World Boss fight is lost");
            wb_end(false);
        }
        break;
    }
    case WBP_Loot:
        wb_bound_cameras();
        if (due)
            wb_end(true);
        break;
    }
}

/******************************************************************************/
/* This machine: the timer, the join button and window, the camera */

/** A line of text at (x, y), w wide, px high; align 0 left, 1 centre, 2 right; in the font's own colours, or all in
 *  one colour (colour >= 0: a greyed-out button's text). */
static void wb_text_col(int x, int y, int w, int px, int align, const char *text, int colour)
{
    const unsigned long flg_mem = RendererGetDrawFlags();
    LbTextSetFont(winfont);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags((align == 1) ? Lb_TEXT_HALIGN_CENTER : (align == 2) ? Lb_TEXT_HALIGN_RIGHT : Lb_TEXT_HALIGN_LEFT);
    if (colour >= 0)
    {
        RendererAddDrawFlags(Lb_TEXT_ONE_COLOR);
        RendererSetDrawColour((TbPixel)colour);
    }
    LbTextSetWindow(x, y, w, 2 * px + 4);
    const int line = max(LbTextLineHeight(), 1);
    LbTextDrawResized(0, 0, max(36 * px / line, 4), text);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

static void wb_text(int x, int y, int w, int px, int align, const char *text)
{
    wb_text_col(x, y, w, px, align, text, -1);
}

/** The y for wb_text that puts the letters themselves on the middle of a box (the font has empty space above them:
 *  their middle is about 4/3 of the size below the top of the text window; measured on screen). */
static int wb_text_y(int box_y, int box_h, int px)
{
    return box_y + box_h / 2 - px * 4 / 3;
}

static void wb_clock(char *out, size_t size, long turns)
{
    const long s = max(0L, turns) / 20;
    if (s >= 3600)
        snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

/** A box in the game's own UI stone (the texture behind the in-game menus), darkened by half when dim, in a thin
 *  frame of the given colour. */
static void wb_stone_box(int x, int y, int w, int h, TbPixel frame, TbBool dim)
{
    const int u = max(1, units_per_pixel / 16);
    draw_slab64k_background(x, y, w, h);
    if (dim)
    {
        RendererAddDrawFlags(Lb_SPRITE_TRANSPAR4);
        LbDrawBox(x, y, w, h, colours[0][0][0]);
        RendererClearDrawFlags(Lb_SPRITE_TRANSPAR4);
    }
    LbDrawBox(x, y, w, u, frame);
    LbDrawBox(x, y + h - u, w, u, frame);
    LbDrawBox(x, y, u, h, frame);
    LbDrawBox(x + w - u, y, u, h, frame);
}

static int wb_strip_box[4];             /* the timer strip on the screen (x, y, w, h); w 0: not drawn */
static TbBool wb_strip_hover;           /* the mouse is over the strip while it can be clicked */
static long wb_tapped_event = -1;       /* the event (world_boss_number) whose strip this player has tapped */
static TbBool wb_sent;
static GameTurn wb_sent_turn;           /* when this player's join was sent */

/** The strip can be clicked: the countdown is over (players may join) and this player hasn't joined. */
/** How many keepers have joined the event. */
static int wb_joined_count(void)
{
    int n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        n += game.world_boss_joined[p] ? 1 : 0;
    return n;
}

static TbBool wb_leaving;               /* this player clicked Ready again: its join is being taken back */
static TbBool wb_leave_sent;
static int wb_leave_tries;
static GameTurn wb_leave_turn;

/** Is this machine's keeper ready (its join has gone to the server, or is on its way, and is not being taken back)? */
static TbBool wb_i_am_ready(void)
{
    const PlayerNumber me = my_player_number;
    const TbBool mine = (me >= 0) && (me < PLAYERS_MAX) && (game.world_boss_joined[me] != 0);
    return (mine || wb_sent) && !wb_leaving && (game.world_boss_phase == WBP_Open);
}

/** The World Boss button can be clicked while the event is ready: it opens the window (and shuts it again). A keeper
 *  who is ready opens it to be not ready any more; one who is not cannot open it while the event is full. */
static TbBool wb_strip_clickable(void)
{
    const PlayerNumber me = my_player_number;
    const TbBool joined = (me >= 0) && (me < PLAYERS_MAX) && game.world_boss_joined[me];
    return world_mode_active() && (game.world_boss_phase == WBP_Open) && !observer_is_active() && !netstate_dedicated() &&
        (joined || wb_sent || (wb_joined_count() < WB_PLAYERS_LIMIT));
}

/** The boss's health, for the keepers at the fight: its name and a long thin bar at the top of the view, as the
 *  Soulslike games draw a boss's (the user, 2026-11-14: "design a health bar that appears at the top of the screen
 *  like Soulslike games such as Elden Ring do"). The red part is its health now; the pale part behind it is what it
 *  has just lost, which closes up after a moment; the number at the right is the health it has left (the first
 *  version counted the damage dealt there; the user, 2026-11-14: "The number to the right should reflect the HP
 *  left"). Read from the game state (world_boss_hp, world_boss_hp_max), so a joined player's machine draws the same. */
static void wb_draw_health_bar(void)
{
    static const char *const titles[4] = {"The Avatar", "The Horned Reaper", "The Dragon", "The Giant"};
    static long shown_event = -1;   /* the fight the trail belongs to */
    static long ghost;              /* where the pale part ends, in health */
    static long last_hp;
    static TbClockMSec trail_since, last_frame;
    const long full = (long)game.world_boss_hp_max;
    if (full <= 0)
        return;
    const long hp = max(0L, min((long)game.world_boss_hp, full));
    const TbClockMSec now = LbTimerClock();
    if (shown_event != (long)game.world_boss_number * 4 + 1 + (long)(full & 3))
    {
        /* another fight (or the same boss again, whole): no trail yet */
        shown_event = (long)game.world_boss_number * 4 + 1 + (long)(full & 3);
        ghost = hp;
        last_hp = hp;
        trail_since = last_frame = now;
    }
    if ((hp < last_hp) && (ghost <= last_hp))
    {
        ghost = last_hp;     /* a new trail begins where the health stood */
        trail_since = now;
    }
    last_hp = hp;
    if (ghost < hp)
        ghost = hp;
    /* the trail stands for half a second, then closes at a seventh of the bar a second */
    const long dt = (long)min((TbClockMSec)250, now - last_frame);
    last_frame = now;
    if ((ghost > hp) && (now - trail_since > 500))
        ghost = max(hp, ghost - max(1L, (long)((int64_t)full * dt / 7000)));

    const int upp = units_per_pixel;
    const int u = max(1, upp / 16);
    const int view_x = engine_window_reserved_left();
    const int view_w = MyScreenWidth - view_x;
    const int bar_w = view_w * 56 / 100;
    const int bar_x = view_x + (view_w - bar_w) / 2;
    const int px = 7 * upp / 16;                 /* the lettering's size */
    const int name_y = 5 * upp / 16;
    const int bar_y = name_y + 17 * upp / 16;
    const int bar_h = max(3 * u, 6 * upp / 16);
    const TbPixel black = colours[0][0][0], gold = colours[11][8][3], gold_dim = colours[6][4][2];
    const TbPixel pit = colours[3][0][0], red = colours[12][1][1], red_dark = colours[8][0][0], red_lit = colours[15][5][4];
    const TbPixel pale = colours[14][11][6];
    /* the frame: a dark surround, a gold line above and below, and a point at each end */
    LbDrawBox(bar_x - 2 * u, bar_y - 2 * u, bar_w + 4 * u, bar_h + 4 * u, black);
    LbDrawBox(bar_x - 2 * u, bar_y - 2 * u, bar_w + 4 * u, u, gold);
    LbDrawBox(bar_x - 2 * u, bar_y + bar_h + u, bar_w + 4 * u, u, gold);
    for (int k = 0; k < 4; k++)
    {
        const int h = bar_h + 4 * u - 2 * k * u;
        if (h <= 0)
            break;
        const TbPixel col = (k == 0) ? gold : gold_dim;
        LbDrawBox(bar_x - (3 + k) * u, bar_y - 2 * u + k * u, u, h, col);
        LbDrawBox(bar_x + bar_w + (2 + k) * u, bar_y - 2 * u + k * u, u, h, col);
    }
    LbDrawBox(bar_x, bar_y, bar_w, bar_h, pit);
    const int fill_w = (int)((int64_t)bar_w * hp / full);
    const int ghost_w = (int)((int64_t)bar_w * ghost / full);
    if (ghost_w > fill_w)
        LbDrawBox(bar_x + fill_w, bar_y, ghost_w - fill_w, bar_h, pale);
    if (fill_w > 0)
    {
        LbDrawBox(bar_x, bar_y, fill_w, bar_h, red);
        LbDrawBox(bar_x, bar_y, fill_w, u, red_lit);
        LbDrawBox(bar_x, bar_y + bar_h - max(u, bar_h / 3), fill_w, max(u, bar_h / 3), red_dark);
    }
    /* its name above the bar's left end; the health it has left above its right end */
    wb_text(bar_x, name_y, bar_w, px, 0, titles[game.world_boss_kind % 4]);
    char left[24];
    snprintf(left, sizeof(left), "%ld", hp);
    wb_text(bar_x, name_y, bar_w, px, 2, left);
}

/** The game's own sparkle (the "spangle" a newly unlocked room, spell or trap button gets: button sprites
 *  GBS_guisymbols_new_function_1..8, eight frames, spangle_button in frontend.cpp), drawn small at both ends of a box so
 *  its text stays readable. */
static void wb_sparkle(int bx, int by, int bw, int bh, long t)
{
    const struct TbSprite *first = get_button_sprite(GBS_guisymbols_new_function_1);
    if ((first == NULL) || (first->SHeight <= 0))
        return;
    const int size = bh * 8 / 5;                               /* a little taller than the strip */
    const int units = max(size * 16 / first->SHeight, 1);
    const struct TbSprite *spr = get_button_sprite(GBS_guisymbols_new_function_1 + (int)((t >> 1) & 7));
    if (spr == NULL)
        return;
    const int sw = spr->SWidth * units / 16, sh = spr->SHeight * units / 16;
    const int cy = by + bh / 2 - sh / 2;
    LbSpriteDrawResized(bx - sw / 2, cy, units, spr);          /* centred on the left edge ... */
    LbSpriteDrawResized(min(bx + bw - sw / 2, MyScreenWidth - sw), cy, units, spr); /* ... and on the right edge (inside the screen) */
}

/** Half of the game's button bar: its left end piece at (x, y), then its middle piece again and again up to the
 *  right edge of the screen, where it is cut square (no right end piece). lit: the glowing bar that goes over a
 *  button under the mouse. Returns the width of the left end piece on the screen. */
static int wb_half_bar(int x, int y, int units, TbBool lit)
{
    const struct TbSprite *left = get_button_sprite(lit ? GBS_frontend_button_sta_l : GBS_frontend_button_std_l);
    const struct TbSprite *mid = get_button_sprite(lit ? GBS_frontend_button_sta_c : GBS_frontend_button_std_c);
    if ((left == NULL) || (mid == NULL) || (mid->SWidth <= 0))
        return 0;
    LbSpriteDrawResized(x, y, units, left);
    const int cap = (left->SWidth * units + 8) / 16;
    const int step = max(mid->SWidth * units / 16, 1);
    for (int at = x + cap; at < MyScreenWidth; at += step)
        LbSpriteDrawResized(at, y, units, mid);
    return cap;
}

void world_boss_draw(int x, int y, int w)
{
    wb_strip_box[2] = 0; /* (not drawn: its button goes too) */
    if (!world_mode_active() || (game.world_boss_until == 0))
        return;
    const PlayerNumber me = my_player_number;
    const uint8_t mine = ((me >= 0) && (me < PLAYERS_MAX)) ? game.world_boss_joined[me] : 0;
    const long left = (long)(GameTurnDelta)(game.world_boss_until - game.play_gameturn);
    char clock[24], text[64];
    wb_clock(clock, sizeof(clock), left);
    const int kind = game.world_boss_kind % 4;
    switch (game.world_boss_phase)
    {
    case WBP_Waiting: snprintf(text, sizeof(text), "World Boss %s", clock); break;
    case WBP_Open:
    {
        /* ready, with no clock, until a keeper joins; then the others have until the clock runs out */
        int joined = 0;
        for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
            joined += game.world_boss_joined[p] ? 1 : 0;
        if (wb_i_am_ready() && (joined > 0))
            snprintf(text, sizeof(text), "Ready: starts %s", clock);
        else if (wb_i_am_ready())
            snprintf(text, sizeof(text), "Ready...");
        else if (joined >= WB_PLAYERS_LIMIT)
            snprintf(text, sizeof(text), "World Boss full %s", clock);
        else if (joined > 0)
            snprintf(text, sizeof(text), "World Boss ready %s", clock);
        else
            snprintf(text, sizeof(text), "World Boss ready");
        break;
    }
    case WBP_Fight:
        if (mine == 1)
        {
            wb_draw_health_bar(); /* at the fight: no clock; the boss's health at the top of the view */
            return;
        }
        snprintf(text, sizeof(text), "World Boss fight");
        break;
    default:
        if (mine == 1)
            snprintf(text, sizeof(text), "%s falls! Home %s", (kind == 0) ? "Avatar" : (kind == 1) ? "Reaper" :
                (kind == 2) ? "Dragon" : "Giant", clock);
        else
            snprintf(text, sizeof(text), "The World Boss has fallen");
        break;
    }
    /* The World Boss button (the user's design, 2026-11-14): half a button and half a square. Its left end is the
       left end of the game's own button bar; its right end is cut square and lies against the right edge of the
       screen, so the button stands out from that edge like a tab. Lit, as the game's buttons are, while the mouse is
       on it and it can be clicked. */
    (void)x;
    const int upp = units_per_pixel;
    const int bh = 16 * upp / 16;
    const struct TbSprite *body = get_button_sprite(GBS_frontend_button_std_c);
    const int bu = ((body != NULL) && (body->SHeight > 0)) ? max((bh * 16 + body->SHeight / 2) / body->SHeight, 1) : 16;
    const int tw = max(w, 128 * upp / 16);
    const int tx = MyScreenWidth - tw;
    const TbBool clickable = wb_strip_clickable();
    wb_strip_box[0] = tx; wb_strip_box[1] = y; wb_strip_box[2] = tw; wb_strip_box[3] = bh;
    const int cap = wb_half_bar(tx, y, bu, false);
    if (clickable && wb_strip_hover)
        wb_half_bar(tx - 6 * bu / 16, y - 6 * bu / 16, bu, true);
    /* the lettering on the middle of the button's flat part, across and up and down */
    const int px = 6 * upp / 16;
    wb_text(tx + cap / 2, wb_text_y(y, bh, px), tw - cap / 2 - 2 * upp / 16, px, 1, text);
    /* from the moment the countdown reaches 0 (the event opens) until this player taps the button: a small sparkle at
       its ends, kept off the text so it stays readable */
    if (clickable && (wb_tapped_event != (long)game.world_boss_number))
        wb_sparkle(tx, y, tw, bh, (long)game.play_gameturn);
}

/* The join window: every creature of this machine's keeper that may go (not imps), the best first. Eight rows show at
   a time and the list scrolls (the mouse wheel, or the bar at its right); any number may be ticked. */
#define WB_ROWS 8
static ThingIndex wb_list_thing[WB_TEAM_MAX];
static uint8_t wb_list_level[WB_TEAM_MAX];
static uint8_t wb_list_on[WB_TEAM_MAX];
static int wb_list_n;                  /* creatures listed */
static int wb_scroll;                  /* the list's entry in the top row */
static char wb_title[] = "World Boss Event";
static char wb_fight_text[] = "Ready";
static char wb_cancel_text[] = "Cancel";
static char wb_close_text[] = "Close";

/** A creature kind's name as players read it (Bile Demon), else its code name. */
static const char *wb_kind_name(ThingModel model)
{
    const struct CreatureModelConfig *crconf = creature_stats_get(model);
    const char *name = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    return ((name != NULL) && (name[0] != 0)) ? name : creature_code_name(model);
}

/** The list's entry a row shows (-1: an empty row). */
static int wb_row_entry(int row)
{
    const int e = wb_scroll + row;
    return ((row >= 0) && (row < WB_ROWS) && (e < wb_list_n)) ? e : -1;
}

/** How many creatures are ticked. */
static int wb_ticked(void)
{
    int n = 0;
    for (int k = 0; k < wb_list_n; k++)
        n += wb_list_on[k] ? 1 : 0;
    return n;
}

static void wb_scroll_by(int rows)
{
    wb_scroll = max(0, min(wb_scroll + rows, wb_list_n - WB_ROWS));
}

/** A text cut to a width, as wb_text draws it at this size: whole when it fits, else its start and "...". */
static const char *wb_fit(const char *text, int w, int px, char *buf, size_t size)
{
    LbTextSetFont(winfont);
    const int units = max(36 * px / max(LbTextLineHeight(), 1), 4);
    if (LbTextStringWidthM(text, units) <= w)
        return text;
    snprintf(buf, size, "%s", text);
    for (size_t len = strlen(buf); len > 1; len--)
    {
        snprintf(buf + len - 1, size - (len - 1), "...");
        if (LbTextStringWidthM(buf, units) <= w)
            break;
    }
    return buf;
}

static void wb_fill_rows(void)
{
    wb_list_n = 0;
    wb_scroll = 0;
    memset(wb_list_thing, 0, sizeof(wb_list_thing));
    memset(wb_list_on, 0, sizeof(wb_list_on)); /* nothing is ticked: the player picks */
    struct Dungeon *dungeon = get_players_num_dungeon(my_player_number);
    if (dungeon_invalid(dungeon))
        return;
    long i = dungeon->creatr_list_start;
    long guard = 0;
    while ((i > 0) && (guard++ < CREATURES_COUNT))
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing))
            break;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if (!wb_may_go(thing, my_player_number))
            continue; /* a captive or a knocked-out creature can't be sent */
        const int l = min((int)cctrl->exp_level + 1, 255);
        if (wb_list_n >= WB_TEAM_MAX)
        {
            if (l <= (int)wb_list_level[WB_TEAM_MAX - 1])
                continue;
            wb_list_n = WB_TEAM_MAX - 1; /* the list is full: the weakest listed makes room */
        }
        int k = wb_list_n++;
        while ((k > 0) && ((int)wb_list_level[k - 1] < l))
        {
            wb_list_thing[k] = wb_list_thing[k - 1];
            wb_list_level[k] = wb_list_level[k - 1];
            k--;
        }
        wb_list_thing[k] = thing->index;
        wb_list_level[k] = (uint8_t)l;
    }
}

static int wb_hover_row = -1; /* the picker's row under the mouse (-1: none) */

static void wb_flat_button(struct GuiButton *gbtn)
{
    if ((gbtn->flags & LbBtnF_Enabled) == 0)
        return;
    const TbBool over = (gbtn->flags & LbBtnF_MouseOver) != 0;
    const int row = (int)gbtn->content.lval;
    const int u = max(1, units_per_pixel / 16);
    const int px = gbtn->height * 5 / 12;
    if (row == 103)
    {
        wb_hover_row = -1; /* (the title is drawn first: the rows below say which one the mouse is over) */
        wb_text(gbtn->scr_pos_x, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), gbtn->width, px, 1, wb_title);
        return;
    }
    if (row == 107)
    {
        /* how many are ticked, of how many listed */
        char count[48];
        if (wb_list_n == 0)
            snprintf(count, sizeof(count), "No creatures to send");
        else
            snprintf(count, sizeof(count), "%d of %d selected", wb_ticked(), min(WB_TEAM_LIMIT, wb_list_n));
        wb_text(gbtn->scr_pos_x, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), gbtn->width, px, 0, count);
        return;
    }
    if (row == 108)
    {
        /* how many keepers are ready, of how many the event counts on (the user, 2026-11-14: "an indicator that shows
           how may players have Ready'd above the current buttons in the middle of the window") */
        char ready[48];
        const int n = wb_joined_count();
        snprintf(ready, sizeof(ready), "Players ready: %d of %d", n, max(max((int)game.world_boss_wanted, n), 1));
        wb_text(gbtn->scr_pos_x, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), gbtn->width, px, 1, ready);
        return;
    }
    if ((row == 100) || (row == 101) || (row == 105))
    {
        /* the game's own button bar (as the in-game Options menu's buttons), lit while the mouse is on it */
        const int bu = simple_button_sprite_height_units_per_px(gbtn, GBS_frontend_button_std_c, 100);
        const int ticked = wb_ticked();
        const TbBool ready = wb_i_am_ready();
        /* Ready is greyed out, and does nothing, while nothing is ticked; a keeper who is ready has its team (the
           list is not changed until it is not ready any more) */
        const TbBool dull = ((row == 100) && !ready && ((ticked == 0) || wb_leaving)) ||
            ((row == 105) && ((wb_list_n == 0) || ready || wb_leaving));
        const char *text = (row == 100) ? wb_fight_text : (row == 101) ? (ready ? wb_close_text : wb_cancel_text) :
            ((ticked > 0) && (ticked >= min(WB_TEAM_LIMIT, wb_list_n))) ? "Select none" : "Select best 5";
        char fit[32];
        const int tw = gbtn->width - 6 * u;
        draw_bar64k(gbtn->scr_pos_x, gbtn->scr_pos_y, bu, gbtn->width);
        if (dull)
        {
            /* greyed out: the button as it is, bar and lettering, under one dark veil; no light under the mouse.
               (Grey lettering on a bar darkened twice came out lilac in the game's palette: "This looks ugly".) */
            wb_text(gbtn->scr_pos_x + 3 * u, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), tw, px, 1,
                wb_fit(text, tw, px, fit, sizeof(fit)));
            RendererAddDrawFlags(Lb_SPRITE_TRANSPAR4);
            LbDrawBox(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, colours[0][0][0]);
            RendererClearDrawFlags(Lb_SPRITE_TRANSPAR4);
            return;
        }
        if (over || ((row == 100) && ready))
        {
            /* (Ready stays lit while this keeper is ready: "stays highlighted when clicked") */
            /* the lit bar is the bar with a glowing rim round it: it goes that rim up and to the left of the bar and
               is that much wider, as the game's own buttons have it (draw_load_button), at this bar's own scale.
               (It was drawn at the bar's own place, and sat down and to the right of it: "the highlight effect does
               not match with the button".) */
            const int rim = 6 * bu / 16;
            draw_lit_bar64k(gbtn->scr_pos_x - rim, gbtn->scr_pos_y - rim, bu, gbtn->width + rim);
        }
        wb_text(gbtn->scr_pos_x + 3 * u, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), tw, px, 1,
            wb_fit(text, tw, px, fit, sizeof(fit)));
        return;
    }
    /* a creature row: the stone, bright with a gold edge when ticked, darker when not */
    const int e = wb_row_entry(row);
    if (e < 0)
        return;
    const struct Thing *thing = thing_get(wb_list_thing[e]);
    const TbBool here = wb_may_go(thing, my_player_number);
    if (!here)
        wb_list_on[e] = 0; /* it died or was captured while the window was open: it can't be sent */
    const TbBool ticked = (wb_list_on[e] != 0);
    wb_stone_box(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height,
        ticked ? colours[15][12][3] : over ? colours[9][6][2] : colours[4][3][2], !ticked && !over);
    if (over)
        wb_hover_row = row;
    /* the creature's icon (as the Creature Menu shows it), its name, and its level at the right edge */
    int icon_w = 0;
    const long spr_idx = thing_exists(thing) ? get_creature_model_graphics(thing->model, CGI_HandSymbol) : 0;
    if (spr_idx > 0)
    {
        const struct TbSprite *spr = get_panel_sprite(spr_idx);
        if ((spr != NULL) && (spr->SHeight > 0) && (spr->SWidth > 0))
        {
            /* sized as the Creature Menu's buttons size it (the sprite's width to 138% of a square the row's height;
               its bottom at the row.s bottom: the portrait is there, the rest is clear) */
            const int sq = gbtn->height - 2 * u;
            const int iu = max(((sq * 138 / 100) * 16 + spr->SWidth / 2) / spr->SWidth, 1);
            icon_w = sq;
            /* the portrait is low in the sprite, with a clear margin under it: its middle is about 68% down the sprite
               (measured on screen), and that middle goes on the row's middle */
            const int sh = spr->SHeight * iu / 16;
            draw_gui_panel_sprite_left(gbtn->scr_pos_x + 3 * u, gbtn->scr_pos_y + gbtn->height / 2 - sh * 68 / 100, iu, spr_idx);
        }
    }
    const int tx = gbtn->scr_pos_x + max(icon_w, gbtn->height - 8 * u) + 8 * u;
    char lv[24];
    snprintf(lv, sizeof(lv), "level %d", (int)wb_list_level[e]);
    const int lw = gbtn->width / 4;
    char fit[64];
    const int name_w = gbtn->scr_pos_x + gbtn->width - lw - 6 * u - tx - 4 * u; /* (clear of the level at the right) */
    /* the name its keeper gave it, else its kind (the row's icon shows the kind too) */
    const char *given = here ? world_misdeeds_shown_name(thing) : NULL;
    const char *name = !here ? "(gone)" : (given != NULL) ? given : wb_kind_name(thing->model);
    wb_text(tx, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), name_w, px, 0, wb_fit(name, name_w, px, fit, sizeof(fit)));
    wb_text(gbtn->scr_pos_x + gbtn->width - lw - 6 * u, wb_text_y(gbtn->scr_pos_y, gbtn->height, px), lw, px, 2, lv);
}

/* The bar at the list's right: an arrow at each end (a row up, a row down) and between them a thumb that shows where
   the eight rows are in the list (a click above or below it: a page). Not drawn while the whole list fits. */
static void wb_arrow(int x, int y, int a, TbBool up, TbBool lit)
{
    const TbPixel col = lit ? colours[15][12][3] : colours[5][4][3];
    const int rows = max(a / 3, 2);
    const int top = y + (a - rows) / 2;
    for (int i = 0; i < rows; i++)
    {
        const int half = (up ? i : rows - 1 - i) * max(a / 2 - 2, 1) / max(rows - 1, 1) + 1;
        LbDrawBox(x + a / 2 - half, top + i, 2 * half, 1, col);
    }
}

/** The thumb's place in the bar: its top (from the bar's top) and its height. */
static void wb_thumb(const struct GuiButton *gbtn, int *top, int *height)
{
    const int a = gbtn->width, track = max(gbtn->height - 2 * a, 1);
    *height = max(track * WB_ROWS / max(wb_list_n, 1), min(track, 6));
    *top = a + (track - *height) * wb_scroll / max(wb_list_n - WB_ROWS, 1);
}

static void wb_scroll_draw(struct GuiButton *gbtn)
{
    if (wb_list_n <= WB_ROWS)
        return;
    const int u = max(1, units_per_pixel / 16);
    const TbBool over = (gbtn->flags & LbBtnF_MouseOver) != 0;
    wb_stone_box(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, colours[4][3][2], true);
    int top, height;
    wb_thumb(gbtn, &top, &height);
    LbDrawBox(gbtn->scr_pos_x + 2 * u, gbtn->scr_pos_y + top, gbtn->width - 4 * u, height, over ? colours[15][12][3] :
        colours[9][6][2]);
    wb_arrow(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, true, wb_scroll > 0);
    wb_arrow(gbtn->scr_pos_x, gbtn->scr_pos_y + gbtn->height - gbtn->width, gbtn->width, false,
        wb_scroll < wb_list_n - WB_ROWS);
}

static void wb_scroll_click(struct GuiButton *gbtn)
{
    if (wb_list_n <= WB_ROWS)
        return;
    const int my = (int)GetMouseY() - gbtn->pos_y;
    int top, height;
    wb_thumb(gbtn, &top, &height);
    if (my < gbtn->width)
        wb_scroll_by(-1);
    else if (my >= gbtn->height - gbtn->width)
        wb_scroll_by(1);
    else if (my < top)
        wb_scroll_by(-WB_ROWS);
    else if (my >= top + height)
        wb_scroll_by(WB_ROWS);
}

/* The tooltip of the creature the mouse is over: its stats on one page, its vices on the other; the mouse wheel turns
   the page. Drawn by the menu's last button, so it lies over the rows. */
#define WB_TIP_PAGES 2
#define WB_TIP_WAIT_MS 2000  /* the mouse stays this long over a creature before its tooltip opens */
static int wb_tip_row = -1;  /* the row the page belongs to (a new row starts at the first page) */
static int wb_tip_page = 0;
static TbClockMSec wb_tip_since;     /* when the mouse came over that row */
static TbBool wb_tip_forced = false; /* the test switch KFX_WORLD_BOSSTIP: no waiting */

/** The row whose tooltip is on the screen now (-1: none): the one the mouse has been over for WB_TIP_WAIT_MS. */
static int wb_tip_shown_row(void)
{
    if (wb_tip_forced)
        return wb_hover_row;
    if ((wb_hover_row < 0) || (wb_hover_row != wb_tip_row) || (LbTimerClock() - wb_tip_since < WB_TIP_WAIT_MS))
        return -1;
    return wb_hover_row;
}

static void wb_tip_maintain(struct GuiButton *gbtn)
{
    (void)gbtn;
    static int tip_scroll = -1;
    if ((wb_hover_row != wb_tip_row) || (wb_scroll != tip_scroll))
    {
        /* another row under the mouse (or the list moved under it): its tooltip waits again */
        wb_tip_row = wb_hover_row;
        tip_scroll = wb_scroll;
        wb_tip_page = 0;
        wb_tip_since = LbTimerClock();
    }
    if (!wheel_scrolled_up && !wheel_scrolled_down)
        return;
    /* the wheel in the window: with a tooltip open it turns the tooltip's page, else it moves the list (never the
       camera's zoom) */
    if (wb_tip_shown_row() >= 0)
        wb_tip_page = (wb_tip_page + (wheel_scrolled_down ? 1 : WB_TIP_PAGES - 1)) % WB_TIP_PAGES;
    else
        wb_scroll_by(wheel_scrolled_down ? 1 : -1);
    wheel_scrolled_up = false;
    wheel_scrolled_down = false;
    clear_key_pressed(KC_MOUSEWHEEL_UP);
    clear_key_pressed(KC_MOUSEWHEEL_DOWN);
}

static void wb_tip_draw(struct GuiButton *gbtn)
{
    {
        /* test switch KFX_WORLD_BOSSTIP=row: that row counts as under the mouse, and the page turns every 40 turns
           (screenshots of both pages, with nobody at the mouse) */
        const char *v = getenv("KFX_WORLD_BOSSTIP");
        if (v != NULL)
        {
            wb_hover_row = atoi(v);
            wb_tip_row = wb_hover_row;
            wb_tip_page = (int)((game.play_gameturn / 40) % WB_TIP_PAGES);
            wb_tip_forced = true;
        }
    }
    const int e = wb_row_entry(wb_tip_shown_row());
    if (e < 0)
        return;
    const struct Thing *thing = thing_get(wb_list_thing[e]);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
        return;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl))
        return;
    const int u = max(1, units_per_pixel / 16);
    const int px = 10 * u, line = 2 * px + 2 * u;
    /* the lines of this page */
    char lines[8][64];
    int n = 0;
    if (wb_tip_page == 0)
    {
        snprintf(lines[n++], 64, "Stats");
        snprintf(lines[n++], 64, "Level %d", (int)cctrl->exp_level + 1);
        snprintf(lines[n++], 64, "Health %ld / %ld", (long)thing->health, (long)cctrl->max_health);
        snprintf(lines[n++], 64, "Strength %ld", calculate_correct_creature_strength(thing));
        snprintf(lines[n++], 64, "Armour %ld", calculate_correct_creature_armour(thing));
        snprintf(lines[n++], 64, "Defence %ld", calculate_correct_creature_defense(thing));
        snprintf(lines[n++], 64, "Dexterity %ld", calculate_correct_creature_dexterity(thing));
        snprintf(lines[n++], 64, "Speed %ld", calculate_correct_creature_maxspeed(thing));
    } else
    {
        snprintf(lines[n++], 64, "Vices");
        const char *names[5], *does[5];
        const int v = world_misdeeds_vices_of(thing, names, does, 3);
        if (v < 0)
            snprintf(lines[n++], 64, "Open its Rap Sheet to see them");
        else if (v == 0)
            snprintf(lines[n++], 64, "None");
        for (int i = 0; i < v; i++)
        {
            snprintf(lines[n++], 64, "%s", names[i]);
            snprintf(lines[n++], 64, "  %s", does[i]);
        }
    }
    (void)gbtn;
    /* the window: below the mouse cursor (clear of the pointer), kept on the screen */
    const int w = 200 * u, h = n * line + 2 * line + 10 * u;
    int x = (int)GetMouseX() - w / 4, y = (int)GetMouseY() + 22 * u;
    x = max(0, min(x, MyScreenWidth - w));
    if (y + h > MyScreenHeight)
        y = max((int)GetMouseY() - h - 6 * u, 0); /* no room below: above the cursor */
    wb_stone_box(x, y, w, h, colours[4][3][2], true);                    /* solid stone, darkened for the text ... */
    draw_round_slab64k(x, y, units_per_pixel, w, h, ROUNDSLAB64K_DARK); /* ... in the game's own message frame */
    for (int i = 0; i < n; i++)
    {
        char fit[64];
        wb_text(x + 8 * u, y + 5 * u + i * line, w - 16 * u, px, (i == 0) ? 1 : 0, wb_fit(lines[i], w - 16 * u, px, fit, sizeof(fit)));
    }
    /* the pages: "<  1 / 2  >", and how to turn them */
    char pages[24];
    snprintf(pages, sizeof(pages), "<   %d / %d   >", wb_tip_page + 1, WB_TIP_PAGES);
    wb_text(x + 8 * u, y + h - 2 * line - 3 * u, w - 16 * u, px, 1, pages);
    wb_text(x + 8 * u, y + h - line - 3 * u, w - 16 * u, px * 4 / 5, 1, "Mouse scroll to next page");
}

static void wb_row_click(struct GuiButton *gbtn)
{
    const int e = wb_row_entry((int)gbtn->content.lval);
    if ((e < 0) || !wb_may_go(thing_get(wb_list_thing[e]), my_player_number))
        return;
    if (wb_i_am_ready() || wb_leaving)
        return; /* ready: the team is as it was sent (Ready again, and it can be changed) */
    if (wb_list_on[e])
        wb_list_on[e] = 0;
    else if (wb_ticked() < WB_TEAM_LIMIT)
        wb_list_on[e] = 1; /* five at most: a sixth is not ticked until one is unticked */
}

static void wb_row_maintain(struct GuiButton *gbtn)
{
    if (wb_row_entry((int)gbtn->content.lval) >= 0)
        gbtn->flags |= LbBtnF_Enabled;
    else
        gbtn->flags &= ~LbBtnF_Enabled;
}

/** "Select best 5" ticks the five best creatures listed (the list has the best first) and unticks the others; with
 *  five ticked it reads "Select none" and unticks them all. */
static void wb_all_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (wb_i_am_ready() || wb_leaving)
        return;
    const int ticked = wb_ticked();
    const TbBool none = ((ticked > 0) && (ticked >= min(WB_TEAM_LIMIT, wb_list_n)));
    int n = 0;
    for (int k = 0; k < wb_list_n; k++)
    {
        wb_list_on[k] = (!none && (n < WB_TEAM_LIMIT) && wb_may_go(thing_get(wb_list_thing[k]), my_player_number)) ? 1 : 0;
        n += wb_list_on[k] ? 1 : 0;
    }
}

/* This machine's join on its way to the server: three creatures a packet (world_boss_packet_join), one packet a turn */
static ThingIndex wb_queue[WB_TEAM_MAX];
static int wb_queue_n, wb_queue_pos, wb_queue_tries;

static void wb_send_join(void)
{
    wb_queue_n = 0;
    for (int k = 0; (k < wb_list_n) && (wb_queue_n < WB_TEAM_MAX); k++)
        if (wb_list_on[k] && (wb_list_thing[k] != 0))
            wb_queue[wb_queue_n++] = wb_list_thing[k];
    wb_queue_pos = 0;
    wb_queue_tries = 0;
    wb_sent = (wb_queue_n > 0);
    wb_sent_turn = game.play_gameturn;
    wb_leaving = false;
    SYNCLOG("World boss: this player is ready with %d creatures", wb_queue_n);
}

/** The next piece of this machine's join goes out with this turn's packet, when no other order is in it. When all of
 *  it has gone and the server still doesn't have this keeper as joined two seconds later (a piece was lost to another
 *  order of the same turn), it is sent again, twice at most; then the player may pick again. */
static void wb_pump_join(void)
{
    const PlayerNumber me = my_player_number;
    if ((me < 0) || (me >= PLAYERS_MAX))
        return;
    if (wb_leaving)
    {
        /* not ready any more: the join is taken back (a piece with length 0), sent when no other order is in this
           turn's packet, and again if the server still has this keeper as ready two seconds later */
        if (game.world_boss_phase != WBP_Open)
        {
            wb_leaving = false; /* (too late: the fight has begun) */
            return;
        }
        if (!wb_leave_sent)
        {
            if (get_players_packet_action(get_my_player()) != PckA_None)
                return;
            set_players_packet_action(get_my_player(), PckA_WorldBossJoin, 0, 0, 0, 0);
            wb_leave_sent = true;
            wb_leave_turn = game.play_gameturn;
            return;
        }
        const long age = (long)(GameTurnDelta)(game.play_gameturn - wb_leave_turn);
        if (!game.world_boss_joined[me] && (age >= 6))
            wb_leaving = false;
        else if (age > 40)
        {
            if (wb_leave_tries++ < 2)
                wb_leave_sent = false;
            else
                wb_leaving = false;
        }
        return;
    }
    if (!wb_sent || (wb_queue_n == 0) || (game.world_boss_phase != WBP_Open))
        return;
    if (wb_queue_pos >= wb_queue_n)
    {
        if (game.world_boss_joined[me] || ((GameTurnDelta)(game.play_gameturn - wb_sent_turn) <= 40))
            return;
        if (wb_queue_tries >= 2)
        {
            SYNCLOG("World boss: this player's join was not taken: it may pick again");
            wb_sent = false;
            wb_queue_n = 0;
            return;
        }
        wb_queue_tries++;
        wb_queue_pos = 0;
        SYNCLOG("World boss: this player's join is sent again");
    }
    if (get_players_packet_action(get_my_player()) != PckA_None)
        return;
    const int at = wb_queue_pos;
    ThingIndex t[3] = {0, 0, 0};
    for (int k = 0; (k < 3) && (at + k < wb_queue_n); k++)
        t[k] = wb_queue[at + k];
    const uint32_t par1 = (uint32_t)t[0] | ((uint32_t)t[1] << 16);
    const uint32_t par2 = (uint32_t)t[2] | ((uint32_t)at << 16) | ((uint32_t)wb_queue_n << 24);
    set_players_packet_action(get_my_player(), PckA_WorldBossJoin, par1, par2, 0, 0);
    wb_queue_pos = min(at + 3, wb_queue_n);
    wb_sent_turn = game.play_gameturn;
}

/** Ready (the user, 2026-11-14: "replace the Join button for a 'Ready', that also stays highlighted when clicked.
 *  Clicking again cancels being Ready'd."): the team that is ticked goes to the server and this keeper is ready; the
 *  window stays, with the button lit. Clicked again, the keeper is not ready any more. The fight begins when the
 *  last keeper the event counts on is ready (or three minutes after the first was). */
static void wb_fight_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if ((game.world_boss_phase != WBP_Open) || wb_leaving)
        return;
    if (wb_i_am_ready())
    {
        wb_leaving = true;
        wb_leave_sent = false;
        wb_leave_tries = 0;
        wb_leave_turn = game.play_gameturn;
        wb_sent = false; /* (no more pieces of the join go out) */
        SYNCLOG("World boss: this player is not ready any more");
        return;
    }
    if (wb_ticked() == 0)
        return; /* nothing is ticked: Ready does nothing */
    wb_send_join();
}

static void wb_cancel_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    turn_off_menu(GMnu_WORLD_BOSS_PICK);
}

static void wb_join_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    if (!wb_strip_clickable())
        return;
    wb_tapped_event = (long)game.world_boss_number; /* the sparkle stops */
    if (menu_is_active(GMnu_WORLD_BOSS_PICK))
    {
        turn_off_menu(GMnu_WORLD_BOSS_PICK); /* (the button opens the window and shuts it) */
        return;
    }
    wb_fill_rows();
    if (wb_i_am_ready())
    {
        /* a keeper who is ready sees the team it sent */
        for (int k = 0; k < wb_list_n; k++)
            for (int q = 0; q < wb_queue_n; q++)
                if (wb_queue[q] == wb_list_thing[k])
                    wb_list_on[k] = 1;
    }
    turn_on_menu(GMnu_WORLD_BOSS_PICK);
}

/** The strip's button: laid over the timer strip (drawn by world_boss_draw under the day/night dial). */
static void wb_strip_maintain(struct GuiButton *gbtn)
{
    struct GuiMenu *gmnu = get_active_menu(gbtn->gmenu_idx);
    if (wb_strip_box[2] > 0)
    {
        /* both of the button's places: where it is drawn (scr_pos) and where the mouse is tested against it (pos).
           Only the first was set, so the click area stayed where the menu was made, off the screen, and no mouse
           could click the strip (the tests clicked it by calling its handler). */
        gbtn->scr_pos_x = wb_strip_box[0];
        gbtn->scr_pos_y = wb_strip_box[1];
        gbtn->pos_x = wb_strip_box[0];
        gbtn->pos_y = wb_strip_box[1];
        gbtn->width = wb_strip_box[2];
        gbtn->height = wb_strip_box[3];
        gbtn->flags |= LbBtnF_Visible | LbBtnF_Enabled;
        if (gmnu != NULL)
        {
            gmnu->pos_x = wb_strip_box[0];
            gmnu->pos_y = wb_strip_box[1];
            gmnu->width = wb_strip_box[2];
            gmnu->height = wb_strip_box[3];
        }
    } else
    {
        gbtn->scr_pos_x = -4000;
        gbtn->pos_x = -4000;
        gbtn->flags &= ~(LbBtnF_Visible | LbBtnF_Enabled);
    }
    wb_strip_hover = ((gbtn->flags & LbBtnF_MouseOver) != 0);
    {
        /* test switch KFX_WORLD_BOSSCLICK=1: once for each event, says whether a mouse at the strip's middle is over
           its button (the test the game's input makes), since an unattended run has no mouse to click with */
        static int on = -1;
        static long said = -1;
        if (on < 0)
            on = (getenv("KFX_WORLD_BOSSCLICK") != NULL) ? 1 : 0;
        if (on && (wb_strip_box[2] > 0) && (said != (long)game.world_boss_number))
        {
            said = (long)game.world_boss_number;
            SYNCLOG("World boss test: the strip is at (%d,%d), %d x %d; a mouse at its middle is over its button: %d",
                wb_strip_box[0], wb_strip_box[1], wb_strip_box[2], wb_strip_box[3], (int)check_if_pos_is_over_button(gbtn,
                wb_strip_box[0] + wb_strip_box[2] / 2, wb_strip_box[1] + wb_strip_box[3] / 2));
        }
    }
}

static void wb_strip_button(struct GuiButton *gbtn)
{
    (void)gbtn; /* (nothing: the strip itself is drawn with the dial) */
}

static void wb_pick_background(struct GuiMenu *gmnu)
{
    draw_ornate_slab64k(gmnu->pos_x, gmnu->pos_y, units_per_pixel, gmnu->width, gmnu->height); /* (as the in-game menus) */
}

static struct GuiButtonInit world_boss_bar_buttons[] = {
  /* (button flag 0, here and on the rows and Join: 1 closes a button's menu at every click, which shut the picker the
     moment a row was clicked, so that nothing could be ticked or unticked) */
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wb_join_click, NULL, NULL, 0, 0, 0, 0, 0, 200, 24, wb_strip_button, 0, GUIStr_Empty, 0, {.lval = 102}, 0, wb_strip_maintain },
  {              -1, BID_DEFAULT, 0, 0, NULL,          NULL, NULL, 0, 0, 0, 0, 0,   0,  0, NULL,           0, 0,            0, {0},           0, NULL },
};

struct GuiMenu world_boss_bar_menu =
 { GMnu_WORLD_BOSS_BAR, 0, 1, world_boss_bar_buttons, 9999, 66, 200, 24, NULL, 0, NULL, NULL, 0, 0, 0,};

/* The window, from the top: the title; a line with how many are ticked and "Select best 5"; eight rows with the bar
   at their right; how many players are ready; Ready and Cancel */
#define WB_ROW_BTN(r) {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wb_row_click, NULL, NULL, 0, 30, 62 + 28 * (r), 30, 62 + 28 * (r), 270, 24, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = (r)}, 0, wb_row_maintain }
static struct GuiButtonInit world_boss_pick_buttons[] = {
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL, NULL, NULL, 0, 30, 8, 30, 8, 292, 26, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = 103}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,            NULL, NULL, 0,  30,  36,  30,  36, 180, 20, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = 107}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wb_all_click,    NULL, NULL, 0, 218,  36, 218,  36, 104, 20, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = 105}, 0, NULL },
  WB_ROW_BTN(0), WB_ROW_BTN(1), WB_ROW_BTN(2), WB_ROW_BTN(3), WB_ROW_BTN(4), WB_ROW_BTN(5), WB_ROW_BTN(6), WB_ROW_BTN(7),
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wb_scroll_click, NULL, NULL, 0, 304,  62, 304,  62,  18, 220, wb_scroll_draw, 0, GUIStr_Empty, 0, {.lval = 106}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,            NULL, NULL, 0,  30, 290,  30, 290, 292, 22, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = 108}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, wb_fight_click,  NULL, NULL, 0,  46, 318,  46, 318, 124, 28, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = 100}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, wb_cancel_click, NULL, NULL, 0, 182, 318, 182, 318, 124, 28, wb_flat_button, 0, GUIStr_Empty, 0, {.lval = 101}, 0, NULL },
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 0, NULL,            NULL, NULL, 0,   0,   0,   0,   0,   1,  1, wb_tip_draw,    0, GUIStr_Empty, 0, {.lval = 104}, 0, wb_tip_maintain },
  {              -1, BID_DEFAULT, 0, 0, NULL,            NULL, NULL, 0,   0,   0,   0,   0,   0,  0, NULL,           0, 0,            0, {0},            0, NULL },
};

struct GuiMenu world_boss_pick_menu =
 { GMnu_WORLD_BOSS_PICK, 0, 1, world_boss_pick_buttons, POS_GAMECTR, POS_GAMECTR, 352, 358, wb_pick_background, 0, NULL, NULL, 0, 1, 0,};

/** Test hook KFX_WORLD_BOSSJOIN (a run with nobody at the mouse): this machine joins as soon as a fight is ready.
 *  =1: with its best four; =<n> (2 or more): with its best n; =all: with every creature it may send; =worst: with
 *  the weakest listed alone (the boss must still be measured by its best). */
static void wb_test_join(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("KFX_WORLD_BOSSJOIN") != NULL) ? 1 : 0;
    static int pick = -1;
    if (pick < 0)
        pick = (getenv("KFX_WORLD_BOSSPICK") != NULL) ? 1 : 0; /* test: the picker opens by itself (a screenshot) */
    static long tap = -2;
    if (tap == -2)
        tap = (getenv("KFX_WORLD_BOSSTAP") != NULL) ? atol(getenv("KFX_WORLD_BOSSTAP")) : -1;
    if ((tap >= 0) && (game.play_gameturn == (GameTurn)tap))
        wb_join_click(NULL);
    if (pick && (game.world_boss_phase == WBP_Open) && !wb_sent && !menu_is_active(GMnu_WORLD_BOSS_PICK))
    {
        wb_fill_rows();
        turn_on_menu(GMnu_WORLD_BOSS_PICK);
    }
    if (on && (game.world_boss_phase == WBP_Loot) && ((game.play_gameturn % 10) == 0) &&
        (my_player_number < PLAYERS_MAX))
    {
        /* the test also collects its share, as a click of the Hand on a book or a box does (the packet) */
        for (int k = 0; k < 4; k++)
        {
            struct Thing *obj = thing_get(game.world_boss_loot[my_player_number][k]);
            if ((game.world_boss_loot[my_player_number][k] == 0) || !thing_exists(obj))
                continue;
            /* (the gold as a click of the Hand on the pile does: the pick-up packet) */
            set_players_packet_action(get_my_player(), object_is_gold_pile(obj) ? PckA_UsePwrHandPick : PckA_UseSpecialBox,
                obj->index, 0, 0, 0);
            SYNCLOG("World boss test: this player's Hand takes its %s", thing_model_name(obj));
            break;
        }
    }
    if (!on || wb_sent || (game.world_boss_phase != WBP_Open))
        return;
    /* (once for each time the event is ready: a lost fight makes it ready again, and the test would join for ever) */
    static long joined_event = -1;
    static int joins = 0;
    if (joined_event != (long)game.world_boss_number)
    {
        joined_event = (long)game.world_boss_number;
        joins = 0;
    }
    const char *again = getenv("KFX_WORLD_BOSSAGAIN"); /* =<n>: after a lost fight the test joins again, n times */
    if (joins > ((again != NULL) ? atoi(again) : 0))
        return;
    wb_fill_rows();
    if (wb_list_n == 0)
        return;
    joins++;
    const char *v = getenv("KFX_WORLD_BOSSJOIN");
    const int best = min((strcmp(v, "all") == 0) ? wb_list_n : (atoi(v) >= 2) ? atoi(v) : 4, WB_TEAM_LIMIT);
    for (int k = 0; k < wb_list_n; k++)
        wb_list_on[k] = (k < best) ? 1 : 0; /* (the picker starts with nothing ticked: the test ticks its best) */
    if (strcmp(v, "worst") == 0)
    {
        memset(wb_list_on, 0, sizeof(wb_list_on));
        wb_list_on[wb_list_n - 1] = 1;
        SYNCLOG("World boss test: this player sends only its weakest listed creature (level %d; its best is level %d)",
            (int)wb_list_level[wb_list_n - 1], (int)wb_list_level[0]);
    }
    wb_send_join();
}

/** A button of the timer strip or the picker, for the mouse test: 'S' the strip, 'R' the picker's row `row`, 'J' its
 *  Join button, 'C' its Cancel button, 'A' its "Select all" button, 'B' the bar at the list's right. */
static struct GuiButton *wb_test_button(char what, int row)
{
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
    {
        struct GuiButton *gbtn = &active_buttons[i];
        if ((gbtn->flags & LbBtnF_Active) == 0)
            continue;
        if (what == 'S')
        {
            if (gbtn->maintain_call == wb_strip_maintain)
                return gbtn;
            continue;
        }
        if (what == 'B')
        {
            if (gbtn->draw_call == wb_scroll_draw)
                return gbtn;
            continue;
        }
        if (gbtn->draw_call != wb_flat_button)
            continue;
        const long v = gbtn->content.lval;
        if (((what == 'R') && (v == row)) || ((what == 'J') && (v == 100)) || ((what == 'C') && (v == 101)) ||
            ((what == 'A') && (v == 105)))
            return gbtn;
    }
    return NULL;
}

/** Test switch KFX_WORLD_BOSSMOUSE="S,R0,R0,R2,J" (a run with nobody at the mouse, at normal speed): the game plays
 *  a mouse itself, through its own mouse feed, so that the buttons are clicked the way a player clicks them and not
 *  by calling their handlers (that is how a strip no mouse could click passed its tests). From 40 turns after the
 *  event opens, one step every 20 turns: S the timer strip, R<n> the picker's row n, J its Join button, C its Cancel
 *  button, A its "Select all" button, D and U the arrows of the bar at the list's right (a row down, a row up), P the
 *  bar's foot just above the down arrow (a page down), H<n> the mouse held still over row n (no click; HC, HJ, HS:
 *  over Cancel, Join, the World Boss button), W nothing. A
 *  step moves the mouse to the button's middle and presses the left button, lets go 3 turns later, and 11 turns later
 *  says what the picker holds. */
static void wb_mouse_test(void)
{
    static const char *steps = NULL;
    static int read = 0;
    static long open_turn = -1;
    if (!read)
    {
        steps = getenv("KFX_WORLD_BOSSMOUSE");
        read = 1;
    }
    if (steps == NULL)
        return;
    if ((open_turn < 0) && (game.world_boss_phase != WBP_Open))
        return;
    if (open_turn < 0)
        open_turn = (long)game.play_gameturn;
    const long t = (long)game.play_gameturn - open_turn - 40;
    if (t < 0)
        return;
    const char *sp = steps;
    for (long i = 0; (i < t / 20) && (sp != NULL); i++)
    {
        sp = strchr(sp, ',');
        if (sp != NULL)
            sp++;
    }
    if ((sp == NULL) || (*sp == 0))
        return;
    const char what = *sp;
    const int row = ((what == 'R') || (what == 'H')) ? atoi(sp + 1) : -1;
    const TbBool bar = (what == 'D') || (what == 'U') || (what == 'P');
    struct TbPoint none = {0, 0};
    if ((t % 20) == 0)
    {
        /* (H followed by a letter: the mouse held still over that button, HC over Cancel, HJ over Join, HS over the
           World Boss button, for a screenshot of the lit bar) */
        const char held = ((what == 'H') && (sp[1] >= 'A') && (sp[1] <= 'Z')) ? sp[1] : 0;
        struct GuiButton *gbtn = wb_test_button(held ? held : (what == 'H') ? 'R' : bar ? 'B' : what, row);
        if ((what == 'W') || (gbtn == NULL))
        {
            SYNCLOG("World boss mouse test: step %c%d: %s", what, max(row, 0), (what == 'W') ? "wait" : "no such button on screen");
            return;
        }
        const int x = gbtn->pos_x + gbtn->width / 2;
        int y = gbtn->pos_y + gbtn->height / 2;
        if (what == 'U')
            y = gbtn->pos_y + gbtn->width / 2;
        else if (what == 'D')
            y = gbtn->pos_y + gbtn->height - gbtn->width / 2;
        else if (what == 'P')
            y = gbtn->pos_y + gbtn->height - gbtn->width - 2;
        LbMouseSetPositionInitial(x / max((int)pixel_size, 1), y / max((int)pixel_size, 1));
        /* the buttons under that point, in the order the input looks at them (the last one takes the click) */
        char under[160] = "";
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            const struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) == 0) || !check_if_pos_is_over_button(b, x, y))
                continue;
            const size_t len = strlen(under);
            snprintf(under + len, sizeof(under) - len, " #%d(menu %d, value %ld, flags %02x)", i, (int)b->gmenu_idx,
                ((b->draw_call == wb_flat_button) || (b->draw_call == wb_scroll_draw)) ? b->content.lval : -1L,
                (unsigned)b->flags);
        }
        SYNCLOG("World boss mouse test: step %c%d: the mouse goes to (%d,%d)%s; under it:%s", what, max(row, 0), x, y,
            (what == 'H') ? " and stays" : " and presses", under);
        if (what != 'H')
            mouseControl(MActn_LBUTTONDOWN, &none);
    }
    if (((t % 20) == 3) && (what != 'H') && (what != 'W'))
        mouseControl(MActn_LBUTTONUP, &none);
    if ((t % 20) == 14)
    {
        char ticks[WB_ROWS + 1];
        for (int k = 0; k < WB_ROWS; k++)
        {
            const int e = wb_row_entry(k);
            ticks[k] = (e < 0) ? '-' : wb_list_on[e] ? 'X' : '.';
        }
        ticks[WB_ROWS] = 0;
        const PlayerNumber me = my_player_number;
        SYNCLOG("World boss mouse test: after %c%d: picker open %d, %d creatures listed, %d ticked, top row %d, rows [%s], "
            "tooltip on row %d, join sent %d, joined %d, phase %d", what, max(row, 0), (int)menu_is_active(GMnu_WORLD_BOSS_PICK),
            wb_list_n, wb_ticked(), wb_scroll, ticks, wb_tip_shown_row(), (int)wb_sent,
            (int)((me >= 0) && (me < PLAYERS_MAX) && game.world_boss_joined[me]), (int)game.world_boss_phase);
    }
}

static TbBool was_at_fight = false;   /* this machine's keeper was in the arena last turn */
static TbBool camera_away = false;    /* its view is at the arena */
static TbBool camera_settled = false; /* moved again once the arena's ground was here (a camera moved to ground this
                                         machine didn't have yet was clamped to the ground it had) */
static TbBool was_journey = false;    /* this machine's keeper was on its way to the arena last turn */

void world_boss_local_turn(void)
{
    if (!world_mode_active() || netstate_dedicated())
        return;
    const PlayerNumber me = my_player_number;
    const uint8_t mine = ((me >= 0) && (me < PLAYERS_MAX)) ? game.world_boss_joined[me] : 0;
    const uint8_t phase = game.world_boss_phase;
    const TbBool at_fight = (phase >= WBP_Fight) && (mine == 1); /* in the arena, until this keeper goes home */
    /* the timer strip is a button while players may join and this one hasn't */
    const TbBool want_bar = wb_strip_clickable();
    if (!want_bar)
        wb_strip_hover = false;
    if (want_bar && !menu_is_active(GMnu_WORLD_BOSS_BAR))
        turn_on_menu(GMnu_WORLD_BOSS_BAR);
    else if (!want_bar && menu_is_active(GMnu_WORLD_BOSS_BAR))
        turn_off_menu(GMnu_WORLD_BOSS_BAR);
    if ((phase != WBP_Open) && menu_is_active(GMnu_WORLD_BOSS_PICK))
        turn_off_menu(GMnu_WORLD_BOSS_PICK);
    if (phase != WBP_Open)
    {
        /* (a lost fight makes the event ready again: this player may then be ready again) */
        wb_sent = false;
        wb_queue_n = 0;
        wb_leaving = false;
    }
    wb_pump_join();
    wb_test_join();
    wb_mouse_test();
    {
        /* The loading screen (world_curtain.c). On the way in: from the round's start until the boss is in the arena
           (the game state's boss thing is 0 until then), and after that for as long as this machine has not got the
           arena's ground yet (a joined machine gets it from the server). On the way home: WB_HOME_TURNS from the turn
           this keeper goes home (its fight is over, won or lost), and until its view is back at its Dungeon Heart. */
        const TbBool journey = at_fight && (phase == WBP_Fight) && (game.world_boss_thing == 0);
        if (journey && !was_journey)
        {
            world_curtain_raise(max((long)(GameTurnDelta)(game.world_boss_until - game.play_gameturn), 1L));
            SYNCLOG("World boss: this player is on its way to the arena");
        }
        if (!at_fight && was_at_fight)
            world_curtain_raise(WB_HOME_TURNS);
        world_curtain_hold(journey || (at_fight && camera_away && !camera_settled) || (!at_fight && camera_away));
        was_journey = journey;
    }
    /* the camera goes to the arena when this keeper's fight begins ... */
    if (at_fight && !was_at_fight)
    {
        MapSlabCoord cx, cy;
        if (world_engine_slab(WB_ARENA_X, WB_ARENA_Y, &cx, &cy) || world_engine_slab_unloaded(WB_ARENA_X, WB_ARENA_Y, &cx, &cy))
        {
            world_local_camera_to(subtile_coord_center(slab_subtile_center(cx)), subtile_coord_center(slab_subtile_center(cy)));
            camera_away = true;
            camera_settled = false;
        }
        else
            return; /* not here yet: next turn */
    }
    if (at_fight && camera_away && !camera_settled)
    {
        MapSlabCoord cx, cy;
        if (world_engine_slab(WB_ARENA_X, WB_ARENA_Y, &cx, &cy))
        {
            world_local_camera_to(subtile_coord_center(slab_subtile_center(cx)), subtile_coord_center(slab_subtile_center(cy)));
            camera_settled = true;
            {
                /* test switch KFX_WORLD_BOSSZOOM="zoom,dx,dy" (zoom 520 far to 12000 near; dx, dy slabs from the
                   arena's middle): this machine's view of the arena, for a screenshot that shows the boss large */
                const char *v = getenv("KFX_WORLD_BOSSZOOM");
                struct UserState *ustate = get_user_state(get_my_user());
                long zoom = 0;
                int dx = 0, dy = 0;
                if ((v != NULL) && (sscanf(v, "%ld,%d,%d", &zoom, &dx, &dy) >= 1) && (zoom > 0) && !user_state_invalid(ustate))
                {
                    struct DungeonCamera pose = ustate->dungeon_camera;
                    pose.zoom[false] = (int32_t)zoom;
                    pose.x += dx * STL_PER_SLB * COORD_PER_STL;
                    pose.y += dy * STL_PER_SLB * COORD_PER_STL;
                    ustate->dungeon_camera = pose;
                    sync_local_camera_pose(get_my_user(), &pose);
                    SYNCLOG("World boss test: this machine's view zooms to %ld, %d slabs east and %d south of the middle", zoom, dx, dy);
                }
            }
            /* (test: the arena drawn black on a joined machine; its lights and map panel worked out again) */
            if (getenv("KFX_WORLD_BOSSLIGHT") != NULL)
            {
                light_stat_refresh();
                panel_map_update(slab_subtile(cx - WB_ARENA_HALF - WB_WALL, 0), slab_subtile(cy - WB_ARENA_HALF - WB_WALL, 0),
                    (2 * (WB_ARENA_HALF + WB_WALL) + 1) * STL_PER_SLB, (2 * (WB_ARENA_HALF + WB_WALL) + 1) * STL_PER_SLB);
                SYNCLOG("World boss test: lights and map refreshed");
            }
        }
    }
    /* ... and home when it is over for this keeper: the 30 seconds after the boss fell are up, or all its creatures
       have fallen (the fight may go on for other keepers) */
    if (!at_fight && was_at_fight && (phase != WBP_Waiting))
    {
        message_add_fmt(MsgType_Blank, 0, "Your creatures have fallen to the World Boss");
        SYNCLOG("World boss: this player's creatures have all fallen: its view goes home");
    }
    {
        /* (for the loading screen's time this machine's view is put back at the Heart whenever it is found
           elsewhere: the server may have read a late packet of this machine's as a place near home, see wb_home_pin) */
        static GameTurn home_until = 0;
        if (!at_fight && was_at_fight)
            home_until = game.play_gameturn + WB_HOME_TURNS;
        if (at_fight)
            home_until = 0;
        const struct UserState *mine_state = get_user_state(get_my_user());
        const struct Thing *heart = ((me >= 0) && (me < PLAYERS_MAX)) ? find_players_dungeon_heart(me) : INVALID_THING;
        const TbBool homing = !at_fight && (home_until != 0) && ((GameTurnDelta)(home_until - game.play_gameturn) > 0);
        const TbBool off = homing && thing_exists(heart) && !user_state_invalid(mine_state) &&
            ((labs((long)mine_state->dungeon_camera.x - (long)heart->mappos.x.val) > COORD_PER_STL) ||
             (labs((long)mine_state->dungeon_camera.y - (long)heart->mappos.y.val) > COORD_PER_STL));
        if (!at_fight && (camera_away || off))
        {
            if (world_local_camera_to_heart())
                camera_away = false;
        }
    }
    if (at_fight && ((game.play_gameturn % 100) == 0) && ((getenv("KFX_WORLD_BOSSJOIN") != NULL) ||
        (getenv("KFX_WORLD_BOSSWATCH") != NULL)))
    {
        /* test log (with the join or the watch switch): does this machine have the arena's ground and the boss, and
           is its view inside the arena? */
        MapSlabCoord ax, ay;
        const TbBool page = world_engine_slab(WB_ARENA_X, WB_ARENA_Y, &ax, &ay);
        const struct UserState *ustate = get_user_state(get_my_user());
        int crs = 0;
        const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
        long guard = 0;
        for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT);)
        {
            struct Thing *c = thing_get(i);
            if (thing_is_invalid(c))
                break;
            i = c->next_of_class;
            if (page && (abs((int)subtile_slab(c->mappos.x.stl.num) - (int)ax) < 20) &&
                (abs((int)subtile_slab(c->mappos.y.stl.num) - (int)ay) < 20))
                crs++;
        }
        SYNCLOG("World boss test: this machine: the arena's page %s (its middle at slab (%d,%d)), %d creatures in it, camera "
            "at slab (%d,%d)", page ? "here" : "missing", (int)ax, (int)ay, crs, (int)(ustate->dungeon_camera.x / COORD_PER_STL / 3),
            (int)(ustate->dungeon_camera.y / COORD_PER_STL / 3));
    }
    was_at_fight = at_fight;
}

/******************************************************************************/
/* Saving (XTR2 record BOS1): a head of fixed size, then each keeper's team (pick_n[p] thing indexes, 16 bits each,
   keeper by keeper) */
#define WB_SAVE_VERSION 0xB2
#pragma pack(1)
struct WbSaved {
    uint8_t version;                   /* WB_SAVE_VERSION (the first layout began with the phase, 0 to 3: it is not
                                          read, and the countdown starts anew) */
    uint8_t phase, kind, health, arena_cut;
    uint32_t number;
    int32_t until_in;                  /* turns from now */
    int32_t fight_age;                 /* turns since the fight began */
    uint32_t boss;
    uint8_t joined[PLAYERS_MAX];
    int32_t arena_x, arena_y;
    uint8_t care[PLAYERS_MAX], got_books[PLAYERS_MAX], got_box[PLAYERS_MAX];
    uint16_t loot[PLAYERS_MAX][4];
    uint8_t top[PLAYERS_MAX];
    uint16_t pick_n[PLAYERS_MAX];
};
#pragma pack()

void world_boss_reset(void)
{
    wb_arena_cut = false;
    wb_boss_idx = 0;
    wb_travel = 0;
    wb_waiting_apart = false;
    memset(wb_home_pin, 0, sizeof(wb_home_pin));
    world_curtain_reset();
    memset(wb_picks, 0, sizeof(wb_picks));
    memset(wb_pick_n, 0, sizeof(wb_pick_n));
    memset(wb_pend_n, 0, sizeof(wb_pend_n));
    memset(wb_pick_turn, 0, sizeof(wb_pick_turn));
    wb_fight_start = 0;
    wb_quiet_sum = -1;
    wb_quiet_since = game.play_gameturn;
    wb_sent = false;
    wb_queue_n = 0;
    was_at_fight = false;
    was_journey = false;
    camera_away = false;
    camera_settled = false;
    game.world_boss_phase = WBP_Waiting;
    game.world_boss_until = 0;
    game.world_boss_number = 0;
    game.world_boss_kind = 0;
    game.world_boss_health = 0;
    game.world_boss_thing = 0;
    game.world_boss_hp = 0;
    game.world_boss_hp_max = 0;
    game.world_boss_wanted = 0;
    game.world_boss_arena_x = 0;
    game.world_boss_arena_y = 0;
    memset(game.world_boss_joined, 0, sizeof(game.world_boss_joined));
    memset(game.world_boss_loot, 0, sizeof(game.world_boss_loot));
    memset(wb_got_books, 0, sizeof(wb_got_books));
    memset(wb_got_box, 0, sizeof(wb_got_box));
    memset(wb_care, 0, sizeof(wb_care));
    memset(wb_top, 0, sizeof(wb_top));
}

size_t world_boss_save_size(void)
{
    if (game.world_boss_until == 0)
        return 0;
    size_t n = sizeof(struct WbSaved);
    for (int p = 0; p < PLAYERS_MAX; p++)
        n += sizeof(uint16_t) * min((size_t)wb_pick_n[p], (size_t)WB_TEAM_MAX);
    return n;
}

void world_boss_save(uint8_t *buf)
{
    struct WbSaved s;
    memset(&s, 0, sizeof(s));
    s.version = WB_SAVE_VERSION;
    s.phase = game.world_boss_phase;
    s.kind = game.world_boss_kind;
    s.health = game.world_boss_health;
    s.arena_cut = (uint8_t)((wb_arena_cut ? 1 : 0) | ((wb_travel & 3) << 1)); /* (and the journey's step) */
    s.number = game.world_boss_number;
    s.until_in = (int32_t)(GameTurnDelta)(game.world_boss_until - game.play_gameturn);
    s.fight_age = (int32_t)(GameTurnDelta)(game.play_gameturn - wb_fight_start);
    s.boss = wb_boss_idx;
    s.arena_x = game.world_boss_arena_x;
    s.arena_y = game.world_boss_arena_y;
    memcpy(s.care, wb_care, sizeof(s.care));
    memcpy(s.got_books, wb_got_books, sizeof(s.got_books));
    memcpy(s.got_box, wb_got_box, sizeof(s.got_box));
    memcpy(s.loot, game.world_boss_loot, sizeof(s.loot));
    memcpy(s.top, wb_top, sizeof(s.top));
    memcpy(s.joined, game.world_boss_joined, sizeof(s.joined));
    for (int p = 0; p < PLAYERS_MAX; p++)
        s.pick_n[p] = (uint16_t)min((int)wb_pick_n[p], WB_TEAM_MAX);
    memcpy(buf, &s, sizeof(s));
    uint8_t *at = buf + sizeof(s);
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int k = 0; k < (int)s.pick_n[p]; k++)
        {
            const uint16_t idx = (uint16_t)wb_picks[p][k];
            memcpy(at, &idx, sizeof(idx));
            at += sizeof(idx);
        }
}

TbBool world_boss_load(const uint8_t *buf, size_t len)
{
    world_boss_reset();
    struct WbSaved s;
    if ((len < sizeof(s)) || (buf[0] != WB_SAVE_VERSION))
        return false;
    memcpy(&s, buf, sizeof(s));
    size_t teams = 0;
    for (int p = 0; p < PLAYERS_MAX; p++)
    {
        if (s.pick_n[p] > WB_TEAM_MAX)
            return false;
        teams += s.pick_n[p];
    }
    if (len < sizeof(s) + teams * sizeof(uint16_t))
        return false;
    game.world_boss_phase = (s.phase <= WBP_Loot) ? s.phase : WBP_Waiting;
    game.world_boss_kind = s.kind;
    game.world_boss_health = s.health;
    wb_arena_cut = ((s.arena_cut & 1) != 0);
    wb_travel = (uint8_t)((s.arena_cut >> 1) & 3);
    if ((wb_travel > 2) || (s.phase != WBP_Fight))
        wb_travel = 0;
    game.world_boss_number = s.number;
    game.world_boss_until = game.play_gameturn + (GameTurn)(GameTurnDelta)s.until_in;
    if (game.world_boss_until == 0)
        game.world_boss_until = 1;
    wb_fight_start = game.play_gameturn - (GameTurn)(GameTurnDelta)s.fight_age;
    wb_boss_idx = (ThingIndex)s.boss;
    game.world_boss_thing = ((game.world_boss_phase == WBP_Fight) && (wb_travel == 0)) ? wb_boss_idx : 0; /* (its health: the next turn) */
    game.world_boss_arena_x = s.arena_x;
    game.world_boss_arena_y = s.arena_y;
    memcpy(wb_care, s.care, sizeof(wb_care));
    memcpy(wb_got_books, s.got_books, sizeof(wb_got_books));
    memcpy(wb_got_box, s.got_box, sizeof(wb_got_box));
    memcpy(wb_top, s.top, sizeof(wb_top));
    memcpy(game.world_boss_loot, s.loot, sizeof(game.world_boss_loot));
    memcpy(game.world_boss_joined, s.joined, sizeof(s.joined));
    /* (the creation turns of the teams are not saved: not checked after a load) */
    const uint8_t *at = buf + sizeof(s);
    for (int p = 0; p < PLAYERS_MAX; p++)
    {
        wb_pick_n[p] = s.pick_n[p];
        for (int k = 0; k < (int)s.pick_n[p]; k++)
        {
            uint16_t idx;
            memcpy(&idx, at, sizeof(idx));
            at += sizeof(idx);
            wb_picks[p][k] = (ThingIndex)idx;
        }
    }
    wb_quiet_sum = -1;
    wb_quiet_since = game.play_gameturn;
    return true;
}

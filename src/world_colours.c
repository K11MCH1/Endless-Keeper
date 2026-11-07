/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_colours.c
 *     Endless world: the colours each machine shows its player (see world_colours.h). Presentation only: nothing
 *     here is game state, so machines in a network game may show the same keeper in different colours.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_colours.h"

#include <string.h>

#include "world_stream.h"
#include "world_pages.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "creature_control.h"
#include "config_cubes.h"
#include "map_data.h"
#include "slab_data.h"
#include "game_legacy.h"
#include "keeperfx.hpp"
#include "config_spritecolors.h"
#include "config_objects.h"
#include "thing_objects.h"
#include "frontmenu_ingame_map.h"
#include "gui_msgs.h"
#include "post_inc.h"

/******************************************************************************/
/* colours others get, in the order they are met; the local player is red */
static const unsigned char display_cycle[] = {1, 2, 3, 6, 7, 8}; /* blue, green, yellow, purple, black, orange */
#define DISPLAY_CYCLE_LEN ((int)(sizeof(display_cycle) / sizeof(display_cycle[0])))
#define COLOUR_RED 0

#define MET_MAX 1024
static int32_t met_ids[MET_MAX];      /* world_player_identity of each keeper met, in order */
static int met_count = 0;
static TbBool table_on = false;       /* colour_table differs from the stock colours */
static unsigned char colour_table[COLOURS_COUNT];

static TbBool colours_active(void)
{
    return world_mode_active() && (my_player_number < PLAYERS_COUNT) &&
        (my_player_number != PLAYER_GOOD) && (my_player_number != PLAYER_NEUTRAL);
}

static int met_index(int32_t id)
{
    for (int i = 0; i < met_count; i++)
        if (met_ids[i] == id)
            return i;
    return -1;
}

/* the turn each keeper was first seen defeated (0: not defeated), for the player panel */
static GameTurn defeated_turn[PLAYERS_MAX];
#define PANEL_DEFEATED_TURNS 2400 /* a defeated keeper leaves the player panel 2 minutes after its defeat */
#define DEFEAT_WATCH_TURNS 64     /* the defeats a machine sees in its first turns of a world (a load, a join) are old */
static int defeat_watch;

void world_colours_reset(void)
{
    memset(defeated_turn, 0, sizeof(defeated_turn));
    defeat_watch = 0;
    met_count = 0;
    table_on = false;
    for (int c = 0; c < COLOURS_COUNT; c++)
        colour_table[c] = (unsigned char)c;
}

unsigned char world_display_colour(PlayerNumber plyr_idx)
{
    if (!colours_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return get_player_color_idx(plyr_idx);
    if (plyr_idx == my_player_number)
        return COLOUR_RED;
    if ((plyr_idx == PLAYER_GOOD) || (plyr_idx == PLAYER_NEUTRAL) || (plyr_idx == game.neutral_player_num))
        return get_player_color_idx(plyr_idx);
    const int i = met_index(world_player_identity(plyr_idx));
    if (i < 0)
        return get_player_color_idx(PLAYER_NEUTRAL); /* not met yet: shown as nobody's */
    return display_cycle[i % DISPLAY_CYCLE_LEN];
}

static void refresh_table(void)
{
    unsigned char before[COLOURS_COUNT];
    memcpy(before, colour_table, sizeof(before));
    const TbBool was_on = table_on;
    for (int c = 0; c < COLOURS_COUNT; c++)
        colour_table[c] = (unsigned char)c;
    table_on = colours_active();
    if (!table_on)
        return;
    /* a true colour belongs to the player that has it (dungeon->color_idx), the first one when shared */
    TbBool done[COLOURS_COUNT];
    memset(done, 0, sizeof(done));
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        const unsigned char c = get_player_color_idx(p);
        if ((c >= COLOURS_COUNT) || done[c])
            continue;
        done[c] = true;
        colour_table[c] = world_display_colour(p);
    }
    /* the minimap's player colours follow */
    if ((was_on != table_on) || (memcmp(before, colour_table, sizeof(before)) != 0))
        setup_panel_colors();
}

unsigned char world_display_colour_of_colour(unsigned char colour_idx)
{
    if (!table_on || (colour_idx >= COLOURS_COUNT))
        return colour_idx;
    return colour_table[colour_idx];
}

unsigned short world_display_cube(unsigned short cube_id)
{
    if (!table_on)
        return cube_id;
    const struct CubeConfigStats *cube = get_cube_model_stats(cube_id);
    if ((cube->ownershipGroup == 0) || (cube->ownershipGroup >= CUBE_OWNERSHIP_GROUPS) || (cube->owner < 0) ||
        (cube->owner >= COLOURS_COUNT))
        return cube_id;
    const unsigned char shown = colour_table[(unsigned char)cube->owner];
    if (shown == (unsigned char)cube->owner)
        return cube_id;
    const unsigned short other = game.conf.cube_conf.cube_bits[cube->ownershipGroup][shown];
    return (other != 0) ? other : cube_id;
}

/** Is any of a player's creatures revealed to the local player? */
static TbBool creature_seen(PlayerNumber plyr_idx)
{
    const struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return false;
    const ThingIndex lists[2] = {dungeon->creatr_list_start, dungeon->digger_list_start};
    for (int l = 0; l < 2; l++)
    {
        long i = lists[l];
        long guard = 0;
        while ((i > 0) && (guard++ < CREATURES_COUNT))
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing))
                break;
            const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            i = creature_control_invalid(cctrl) ? 0 : cctrl->players_next_creature_idx;
            if (thing_revealed(thing, my_player_number))
                return true;
        }
    }
    return false;
}

/** Has the local player met this keeper (its colour shows)? Always outside endless worlds. */
TbBool world_player_met(PlayerNumber plyr_idx)
{
    if (!colours_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT) || (plyr_idx == my_player_number)
     || (plyr_idx == PLAYER_GOOD) || (plyr_idx == PLAYER_NEUTRAL) || (plyr_idx == game.neutral_player_num))
        return true;
    return met_index(world_player_identity(plyr_idx)) >= 0;
}

/** Is this keeper in the player panel? A defeated one leaves it 2 minutes after its defeat (endless worlds). */
TbBool world_player_listed(PlayerNumber plyr_idx)
{
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (plyr_idx == my_player_number))
        return true;
    return (defeated_turn[plyr_idx] == 0) || ((GameTurnDelta)(game.play_gameturn - defeated_turn[plyr_idx]) < PANEL_DEFEATED_TURNS);
}

/** A keeper's defeat, on every machine (2026-11-07): the dead face, then "<nickname> was defeated!". */
static void announce_defeat(PlayerNumber plyr_idx)
{
    if ((plyr_idx == PLAYER_GOOD) || (plyr_idx == PLAYER_NEUTRAL) || (plyr_idx == game.neutral_player_num) || !player_is_keeper(plyr_idx))
        return;
    const struct PlayerInfo *player = get_player(plyr_idx);
    if (player->player_name[0] != '\0')
        message_add_fmt(MsgType_Player, plyr_idx, "%s was defeated!", player->player_name);
    else
        message_add_fmt(MsgType_Player, plyr_idx, "Keeper %d was defeated!", (int)plyr_idx + 1);
}

static void track_defeats(void)
{
    const TbBool news = (defeat_watch >= DEFEAT_WATCH_TURNS);
    if (!news)
        defeat_watch++;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        /* only the players this world has (9, or 16 with players 10-15) */
        const TbBool defeated = (p < PLAYERS_COUNT) && player_exists(get_player(p)) && (get_player(p)->victory_state == VicS_LostLevel);
        if (!defeated)
            defeated_turn[p] = 0;
        else if (defeated_turn[p] == 0)
        {
            defeated_turn[p] = (game.play_gameturn != 0) ? game.play_gameturn : 1;
            if (news)
                announce_defeat(p);
        }
    }
}

void world_colours_turn(void)
{
    if (world_mode_active())
        track_defeats();
    if (!colours_active())
    {
        if (table_on)
            refresh_table();
        return;
    }
    if ((game.play_gameturn % 8) != 0)
    {
        if (!table_on)
            refresh_table();
        return;
    }
    /* the keepers not met yet */
    PlayerBitFlags open = pmask_none();
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        if ((p == my_player_number) || (p == PLAYER_GOOD) || (p == PLAYER_NEUTRAL) || (p == game.neutral_player_num))
            continue;
        if (!player_exists(get_player(p)) || (met_index(world_player_identity(p)) >= 0))
            continue;
        pmask_add(&open, p);
    }
    PlayerBitFlags found = pmask_none();
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        if (pmask_has(open, p) && creature_seen(p))
            pmask_add(&found, p);
    /* their ground: a slab of theirs whose centre the local player has seen */
    if (pmask_any(pmask_andnot(open, found)))
    {
        const int mine = my_player_number;
        if (world_paged)
        {
            for (struct WorldPage *pg = wp_first(); (pg != NULL) && (pmask_any(pmask_andnot(open, found))); pg = pg->next)
            {
                for (int i = 0; i < WP_CHUNK_SLAB_CELLS; i++)
                {
                    const PlayerNumber o = slabmap_owner(&pg->slab[i]);
                    if ((o < 0) || (o >= PLAYERS_COUNT) || !pmask_has(pmask_andnot(open, found), o))
                        continue;
                    const int sx = i % WP_CHUNK_SLABS, sy = i / WP_CHUNK_SLABS;
                    if (pmask_has(pg->map[(sy * STL_PER_SLB + 1) * WP_CHUNK_STL + sx * STL_PER_SLB + 1].revealed, mine))
                        pmask_add(&found, o);
                }
            }
        } else
        {
            for (MapSlabCoord sy = 0; (sy < game.map_tiles_y) && (pmask_any(pmask_andnot(open, found))); sy++)
            {
                for (MapSlabCoord sx = 0; sx < game.map_tiles_x; sx++)
                {
                    const PlayerNumber o = slabmap_owner(get_slabmap_block(sx, sy));
                    if ((o < 0) || (o >= PLAYERS_COUNT) || !pmask_has(pmask_andnot(open, found), o))
                        continue;
                    if (subtile_revealed(slab_subtile_center(sx), slab_subtile_center(sy), my_player_number))
                        pmask_add(&found, o);
                }
            }
        }
    }
    /* met now, in player order */
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (met_count < MET_MAX); p++)
    {
        if (!pmask_has(found, p))
            continue;
        met_ids[met_count++] = world_player_identity(p);
        SYNCLOG("World colours: player %d met %s, shown as colour %d", (int)my_player_number,
            player_code_name(p), (int)display_cycle[(met_count - 1) % DISPLAY_CYCLE_LEN]);
    }
    refresh_table();
}

unsigned short world_display_thing_sprite(const struct Thing *thing)
{
    if (!table_on || (thing->class_id != TCls_Object))
        return thing->anim_sprite;
    const ThingModel base = get_coloured_object_base_model(thing->model);
    if (base == 0)
        return thing->anim_sprite;
    const unsigned char truecol = get_player_color_idx(thing->owner);
    if (truecol >= COLOURS_COUNT)
        return thing->anim_sprite;
    const unsigned char shown = colour_table[truecol];
    if (shown == truecol)
        return thing->anim_sprite;
    const ThingModel model = get_coloured_object_model_for_colour(base, shown);
    if ((model <= 0) || (model == thing->model))
        return thing->anim_sprite;
    const struct ObjectConfigStats *objst = get_object_model_stats(model);
    return (objst->sprite_anim_idx != 0) ? objst->sprite_anim_idx : thing->anim_sprite;
}

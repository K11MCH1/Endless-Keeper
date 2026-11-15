/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_anniversary.c
 *     Endless world: anniversaries of the Heart.
 * @par Purpose:
 *     See world_anniversary.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_anniversary.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "world_stream.h"
#include "world_daynight.h"
#include "game_legacy.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "thing_stats.h"
#include "config_objects.h"
#include "map_data.h"
#include "gui_msgs.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "world_caretaker.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static const long anniv_days[WORLD_ANNIVERSARIES] = {7, 30, 100, 365};
/* The gifts: special boxes beside the Heart (two at most each). Chosen for the night of 2026-11-13: a level for every
   creature first, a healing and a level a month in, the Multiply box at a hundred days, both at a year. */
static const char *const anniv_gifts[WORLD_ANNIVERSARIES][2] = {
    {"SPECBOX_INCLEV", NULL},
    {"SPECBOX_HEALALL", "SPECBOX_INCLEV"},
    {"SPECBOX_MULTPLY", NULL},
    {"SPECBOX_MULTPLY", "SPECBOX_INCLEV"},
};
static const char *const anniv_words[WORLD_ANNIVERSARIES] = {"a week", "a month", "a hundred days", "a year"};
/* where the anniversaries' flames burn, in subtiles from the Heart */
static const signed char anniv_flame_at[WORLD_ANNIVERSARIES][2] = {{-2, -2}, {2, -2}, {-2, 2}, {2, 2}};

struct AnnivSeat {
    uint8_t marked;                          /* bit i: anniversary i marked */
    uint8_t pad;
    uint16_t flame[WORLD_ANNIVERSARIES];     /* the flames given (thing indices, 0: none) */
};
static struct AnnivSeat anv[PLAYERS_MAX];

long world_anniversary_day(int i)
{
    return ((i >= 0) && (i < WORLD_ANNIVERSARIES)) ? anniv_days[i] : 0;
}

int world_anniversaries_marked(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return 0;
    int n = 0;
    for (int i = 0; i < WORLD_ANNIVERSARIES; i++)
        n += (anv[plyr_idx].marked >> i) & 1;
    return n;
}

/** Is this one of the Heart's flames (any keeper colour)? */
static TbBool is_heart_flame(const struct Thing *thing)
{
    if (!thing_exists(thing) || (thing->class_id != TCls_Object))
        return false;
    const char *name = object_code_name(thing->model);
    return (name != NULL) && (strncmp(name, "HEARTFLAME", 10) == 0);
}

void world_anniversary_seat_started(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    struct AnnivSeat *a = &anv[plyr_idx];
    for (int i = 0; i < WORLD_ANNIVERSARIES; i++)
    {
        struct Thing *f = (a->flame[i] != 0) ? thing_get(a->flame[i]) : INVALID_THING;
        if (is_heart_flame(f) && (f->owner == plyr_idx))
            destroy_object(f);
    }
    if (a->marked != 0)
        SYNCLOG("World: player %d's dungeon starts again: its anniversaries are lost", (int)plyr_idx);
    memset(a, 0, sizeof(*a));
}

/** A line for every keeper: the chat (or the messages where there is no chat box). */
static void announce(const char *text)
{
    if (network_is_active())
    {
        if (netstate.my_id != SERVER_ID)
            return;
        server_admin_notice(-1, text);
        server_admin_chat(-1, -1, "Server", text);
        if (netstate_dedicated())
            return;
    }
    if (chat_box_active())
        chat_box_add(-1, text);
    else
        message_add(MsgType_Blank, 0, text);
}

/** One more flame on the Heart, as the ones it has (their model and height). */
static uint16_t light_flame(PlayerNumber plyr_idx, const struct Thing *heart, int i)
{
    const MapSubtlCoord hx = heart->mappos.x.stl.num, hy = heart->mappos.y.stl.num;
    const struct Thing *like = INVALID_THING;
    for (MapSubtlCoord y = hy - 4; (y <= hy + 4) && thing_is_invalid(like); y++)
    {
        for (MapSubtlCoord x = hx - 4; (x <= hx + 4) && thing_is_invalid(like); x++)
        {
            if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                continue;
            long k = get_mapwho_thing_index(get_map_block_at(x, y));
            unsigned long guard = 0;
            while ((k != 0) && (guard++ < THINGS_COUNT))
            {
                const struct Thing *t = thing_get(k);
                if (thing_is_invalid(t))
                    break;
                k = t->next_on_mapblk;
                if (is_heart_flame(t) && (t->owner == plyr_idx))
                {
                    like = t;
                    break;
                }
            }
        }
    }
    if (thing_is_invalid(like))
        return 0; /* a Heart without flames (some maps): nothing to add to */
    struct Coord3d pos = heart->mappos;
    pos.x.val += anniv_flame_at[i][0] * COORD_PER_STL;
    pos.y.val += anniv_flame_at[i][1] * COORD_PER_STL;
    pos.z.val = like->mappos.z.val;
    struct Thing *f = create_object(&pos, like->model, plyr_idx, -1);
    return thing_is_invalid(f) ? 0 : (uint16_t)f->index;
}

static void give_gift(PlayerNumber plyr_idx, const struct Thing *heart, int i)
{
    for (int g = 0; g < 2; g++)
    {
        if (anniv_gifts[i][g] == NULL)
            continue;
        const ThingModel model = (ThingModel)get_id(object_desc, anniv_gifts[i][g]);
        if (model <= 0)
            continue;
        struct Coord3d pos = heart->mappos;
        pos.x.val += (g == 0 ? -1 : 1) * COORD_PER_STL;
        pos.y.val += 5 * COORD_PER_STL; /* in the Heart's room, in front of the Heart */
        pos.z.val = 0;
        struct Thing *box = create_object(&pos, model, plyr_idx, -1);
        if (!thing_is_invalid(box))
            SYNCLOG("World: player %d's anniversary gift: %s", (int)plyr_idx, anniv_gifts[i][g]);
    }
}

/** KFX_WORLD_ANNIVTEST=1 (test switch): log each keeper's age and marks every 1000 turns. */
static TbBool anniv_test(void)
{
    static int on = -1;
    if (on < 0)
    {
        on = (getenv("KFX_WORLD_ANNIVTEST") != NULL) ? 1 : 0;
        if (on)
            SYNCLOG("World: test switch KFX_WORLD_ANNIVTEST");
    }
    return on;
}

void world_anniversary_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % 32) != 11))
        return;
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!world_player_is_human(p) || world_caretaker_active(p)) /* away: marked when the keeper is back */
            continue;
        const struct Thing *heart = get_player_soul_container(p);
        if (!thing_exists(heart))
            continue;
        const long day = world_seat_day(p);
        struct AnnivSeat *a = &anv[p];
        if (anniv_test() && ((game.play_gameturn % 1024) == 11))
            SYNCLOG("World: anniversary test: player %d's dungeon is on its day %ld, %d marked", (int)p, day,
                world_anniversaries_marked(p));
        /* the newest anniversary reached: a dungeon that passed several at once (a world saved before this) gets
           the newest one's gift and line; the older ones are marked quietly */
        int newest = -1;
        for (int i = 0; i < WORLD_ANNIVERSARIES; i++)
            if ((day >= anniv_days[i]) && !(a->marked & (1u << i)))
                newest = i;
        if (newest < 0)
            continue;
        for (int i = 0; i <= newest; i++)
        {
            if (a->marked & (1u << i))
                continue;
            a->marked |= (uint8_t)(1u << i);
            if (a->flame[i] == 0)
            {
                a->flame[i] = light_flame(p, heart, i);
                SYNCLOG("World: player %d's Heart %s", (int)p, (a->flame[i] != 0) ? "has one more flame" : "has no flame to add to");
            }
        }
        give_gift(p, heart, newest);
        char text[160];
        snprintf(text, sizeof(text), "%s's Heart has beaten for %s (day %ld)", server_admin_player_name(p),
            anniv_words[newest], anniv_days[newest]);
        announce(text);
        SYNCLOG("World: player %d's dungeon marks its anniversary of %ld days (day %ld)", (int)p, anniv_days[newest], day);
        server_admin_player_event(p, "%s", text);
    }
}

/******************************************************************************/
void world_anniversary_reset(void)
{
    memset(anv, 0, sizeof(anv));
}

size_t world_anniversary_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        if (anv[p].marked != 0)
            return 4 + sizeof(anv);
    return 0;
}

void world_anniversary_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, anv, sizeof(anv));
}

TbBool world_anniversary_load(const uint8_t *buf, size_t len)
{
    world_anniversary_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct AnnivSeat) + 4 > len)
        return false;
    memcpy(anv, buf + 4, sizeof(struct AnnivSeat) * (size_t)min((uint32_t)PLAYERS_MAX, n));
    return true;
}

#ifdef __cplusplus
}
#endif

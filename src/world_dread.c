/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dread.c
 *     Endless world: the Ledger of Dread (2026-11-13).
 * @par Purpose:
 *     A keeper's dread is the level their raids are sized from. Before it, raids read the keeper's best creature
 *     alone: one Dragon made every raid as hard as a whole army of Dragons, and a full Treasury or a trapped dungeon
 *     counted for nothing. The dread weighs several things, none of them worth starving:
 *     - the army (most of it): half the best creature's level, plus half the average of the ten best (an empty
 *       place counts 0, so a lone champion is not an army), plus up to 2 for more than ten creatures;
 *     - gold held: up to +2 (+1 at 20,000, +2 at 60,000);
 *     - traps and doors standing: up to +1.5 (one twentieth each);
 *     - the record of the last raids: -2 to +2 (a raid beaten cleanly raises it, a raid that hurt the keeper lowers
 *       it).
 *     The four extras together add at most 4, and never more than the army's own part. A keeper whose army has fallen below two fifths of its recent peak
 *     gets mercy: the raid is sized from the army alone, and as few parties as its tier allows.
 *     The server works the dread out; it lives in the game state (game.world_dread), so the meter of a joined
 *     player shows it too. Saved: the record, the peak army and the raid tallies (record "LDG1").
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_dread.h"
#include "world_daynight.h"
#include "world_stream.h"
#include "world_caretaker.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "config_creature.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "frontend.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_data.h"
#include "player_utils.h"
#include "player_instances.h"
#include "config_players.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "vidfade.h"
#include "kfx/renderer/RendererManager.h"
#include "post_inc.h"

#define DREAD_TOP 10                 /* the army's best this many count in its average */
#define DREAD_EXTRAS_MAX 4.0         /* the extras (army size, gold, defences, record) add at most this */
#define DREAD_MERCY_FRACTION 0.4     /* mercy below this share of the army's recent peak */
#define DREAD_MERCY_MIN_PEAK 10      /* ... once the peak was worth something (summed levels) */
#define DREAD_PEAK_KEEP 0.9          /* each new day the peak keeps this much of itself */
#define DREAD_RECORD_MAX 4           /* the record in half levels: -4..4 */

/* What the ledger keeps per keeper (saved). */
struct DreadLedger {
    int8_t record;          /* half levels, -DREAD_RECORD_MAX..DREAD_RECORD_MAX */
    uint8_t pad[3];
    uint32_t peak;          /* the army's summed levels at their recent peak */
    int32_t peak_day;       /* the world's day the peak last decayed on */
    uint16_t raid_made;     /* the raid on now: heroes made, heroes that left alive, the keeper's creatures lost */
    uint16_t raid_left;
    uint16_t raid_lost;
    uint16_t pad2;
    uint32_t gold_avg;      /* gold held, averaged slowly (rises over about 2 minutes, falls over about 7) */
};
static struct DreadLedger dread[PLAYERS_MAX];

/* What the last count found (worked out every 64 turns, and when a raid is made) */
struct DreadParts {
    int best, count;
    uint32_t sum;           /* summed levels of the whole army */
    double army, size, gold, defence, record, total;
    TbBool mercy;
    long traps, doors;
    GoldAmount money;
};

static int dread_log_level = -1;

static TbBool dread_test_log(void)
{
    if (dread_log_level < 0)
        dread_log_level = (getenv("KFX_WORLD_DREADLOG") != NULL) ? 1 : 0;
    return (dread_log_level > 0);
}

/** The army: imps (the digger list) don't count, as for the best level before. */
static void dread_count_army(PlayerNumber plyr_idx, struct DreadParts *dp)
{
    int top[DREAD_TOP];
    memset(top, 0, sizeof(top));
    dp->best = 1;
    dp->count = 0;
    dp->sum = 0;
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
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
        const int lv = cctrl->exp_level + 1;
        dp->count++;
        dp->sum += (uint32_t)lv;
        if (lv > dp->best)
            dp->best = lv;
        /* keep the ten best, highest first */
        int k = DREAD_TOP - 1;
        if (lv <= top[k])
            continue;
        while ((k > 0) && (top[k - 1] < lv))
        {
            top[k] = top[k - 1];
            k--;
        }
        top[k] = lv;
    }
    int topsum = 0;
    for (int k = 0; k < DREAD_TOP; k++)
        topsum += top[k];
    dp->army = 0.5 * dp->best + 0.5 * (double)topsum / DREAD_TOP;
}

/** Traps with shots left and doors standing, of every keeper, in one walk of each list. */
static void dread_count_defences(long *traps, long *doors)
{
    const ThingClass classes[2] = {TCls_Trap, TCls_Door};
    for (int c = 0; c < 2; c++)
    {
        long *out = (c == 0) ? traps : doors;
        const struct StructureList *slist = get_list_for_thing_class(classes[c]);
        long i = slist->index;
        unsigned long k = 0;
        while (i > 0)
        {
            struct Thing *thing = thing_get(i);
            if (thing_is_invalid(thing))
                break;
            i = thing->next_of_class;
            if ((thing->owner < PLAYERS_MAX) && ((c == 1) || (thing->trap.num_shots > 0)))
                out[thing->owner]++;
            if (++k > slist->count)
                break;
        }
    }
}

static void dread_work_out(PlayerNumber plyr_idx, long traps, long doors, struct DreadParts *dp)
{
    memset(dp, 0, sizeof(*dp));
    dread_count_army(plyr_idx, dp);
    /* the gold part reads a slow average, not the gold of this instant: lifting the hoards with the Hand (they leave
       the count while held) moved it by two levels in seconds (audit 2026-11-13) */
    dp->money = (GoldAmount)dread[plyr_idx].gold_avg;
    dp->traps = traps;
    dp->doors = doors;
    dp->size = min(2.0, max(0.0, (dp->count - DREAD_TOP) / 20.0));
    dp->gold = min(2.0, log2(1.0 + max(0.0, (double)dp->money) / 20000.0));
    dp->defence = min(1.5, (traps + doors) / 20.0);
    const struct DreadLedger *lg = &dread[plyr_idx];
    dp->record = lg->record / 2.0;
    double extras = dp->size + dp->gold + dp->defence + dp->record;
    /* the extras never more than DREAD_EXTRAS_MAX, nor more than the army itself: a full Treasury guarded by imps
       and level 1 creatures doesn't bring raids three times their level */
    const double most = min(DREAD_EXTRAS_MAX, max(1.0, dp->army));
    if (extras > most)
        extras = most;
    dp->mercy = (lg->peak >= DREAD_MERCY_MIN_PEAK) && ((double)dp->sum < DREAD_MERCY_FRACTION * lg->peak);
    if (dp->mercy)
        extras = min(extras, 0.0); /* the floor of mercy: nothing on top of what is left of the army */
    dp->total = dp->army + extras;
}

static int dread_level_of(const struct DreadParts *dp)
{
    int lv = (int)floor(dp->total + 0.5);
#ifdef CREATURE_MAX_LEVEL_LIMIT
    const int cap = CREATURE_MAX_LEVEL_LIMIT;
#else
    const int cap = CREATURE_MAX_LEVEL;
#endif
    return max(1, min(lv, cap));
}

/** The peak: the army's summed levels at their best, losing a tenth each day. */
static void dread_note_peak(PlayerNumber plyr_idx, const struct DreadParts *dp)
{
    struct DreadLedger *lg = &dread[plyr_idx];
    const long day = world_day_number();
    if ((lg->peak_day > 0) && (day - lg->peak_day > 60))
        lg->peak_day = (int32_t)(day - 60); /* (0.9 to the 60th is next to nothing: no need to go further) */
    while ((lg->peak_day > 0) && (lg->peak_day < day))
    {
        lg->peak = (uint32_t)(lg->peak * DREAD_PEAK_KEEP);
        lg->peak_day++;
    }
    lg->peak_day = (int32_t)day;
    if (dp->sum > lg->peak)
        lg->peak = dp->sum;
}

/** Every 64 turns: the slow average of the gold a keeper holds. */
static void dread_note_gold(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    struct DreadLedger *lg = &dread[plyr_idx];
    const int64_t now = max((int64_t)0, (int64_t)dungeon->total_money_owned);
    const int64_t avg = lg->gold_avg;
    const int64_t next = (now > avg) ? avg + (now - avg + 31) / 32 : avg - (avg - now + 127) / 128;
    lg->gold_avg = (uint32_t)min(next, (int64_t)UINT32_MAX);
}

static void dread_publish(PlayerNumber plyr_idx, const struct DreadParts *dp)
{
    const int lv = dread_level_of(dp);
    game.world_dread[plyr_idx] = (uint8_t)min(lv, 255);
    game.world_dread_flags[plyr_idx] = dp->mercy ? 1 : 0;
}

static void dread_log(const char *why, PlayerNumber plyr_idx, const struct DreadParts *dp)
{
    SYNCLOG("World: dread of player %d %s: %d (army %.1f from best %d and %d creatures, %u levels in all; size +%.1f, "
        "gold %ld +%.1f, %ld traps and %ld doors +%.1f, record %+.1f%s)", (int)plyr_idx, why, dread_level_of(dp),
        dp->army, dp->best, dp->count, (unsigned)dp->sum, dp->size, (long)dp->money, dp->gold, dp->traps, dp->doors,
        dp->defence, dp->record, dp->mercy ? "; mercy: the army has fallen far below its peak" : "");
}

int world_dread_raid_level(PlayerNumber plyr_idx, TbBool *mercy)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
    {
        if (mercy != NULL)
            *mercy = false;
        return 1;
    }
    if (world_caretaker_active(plyr_idx) && (game.world_dread[plyr_idx] > 0))
    {
        /* the caretaker keeps the dungeon: its dread is held where it was when its keeper left (audit 2026-11-13) */
        SYNCLOG("World: dread of player %d for a raid: %d, held while the caretaker keeps the dungeon", (int)plyr_idx,
            (int)game.world_dread[plyr_idx]);
        if (mercy != NULL)
            *mercy = ((game.world_dread_flags[plyr_idx] & 1) != 0);
        return game.world_dread[plyr_idx];
    }
    long traps[PLAYERS_MAX], doors[PLAYERS_MAX];
    memset(traps, 0, sizeof(traps));
    memset(doors, 0, sizeof(doors));
    dread_count_defences(traps, doors);
    struct DreadParts dp;
    dread_work_out(plyr_idx, traps[plyr_idx], doors[plyr_idx], &dp);
    dread_note_peak(plyr_idx, &dp);
    dread_publish(plyr_idx, &dp);
    dread_log("for a raid", plyr_idx, &dp);
    if (mercy != NULL)
        *mercy = dp.mercy;
    return dread_level_of(&dp);
}

void world_dread_raid_made(PlayerNumber plyr_idx, int n)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    struct DreadLedger *lg = &dread[plyr_idx];
    lg->raid_made = (uint16_t)min(n, 65535);
    lg->raid_left = 0;
    lg->raid_lost = 0;
}

void world_dread_raider_left(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    if (dread[plyr_idx].raid_left < 65535)
        dread[plyr_idx].raid_left++;
}

void world_dread_creature_died(const struct Thing *thing)
{
    if (!world_mode_active() || !thing_exists(thing) || (thing->class_id != TCls_Creature))
        return;
    const PlayerNumber p = thing->owner;
    if ((p < 0) || (p >= PLAYERS_MAX) || !world_player_is_human(p) || (dread[p].raid_made == 0) ||
        thing_is_creature_digger(thing))
        return; /* (imps don't count in the army, nor as its losses) */
    if (dread[p].raid_lost < 65535)
        dread[p].raid_lost++;
}

/** How the raid went: beaten cleanly (none of its heroes got away and the keeper lost at most half as many
 *  creatures as heroes came) raises the record by a half level; a raid that hurt (the keeper lost more creatures
 *  than heroes came, or lost some while half the heroes got away) lowers it; anything between (heroes that went
 *  home without a fight among it) draws it a step towards nothing. */
void world_dread_raid_over(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    struct DreadLedger *lg = &dread[plyr_idx];
    if (lg->raid_made == 0)
        return;
    const int made = lg->raid_made, left = lg->raid_left, lost = lg->raid_lost;
    const char *how;
    if ((left == 0) && (2 * lost <= made) && world_caretaker_active(plyr_idx))
    {
        how = "beaten cleanly by the caretaker: the record stays";
    } else if ((left == 0) && (2 * lost <= made))
    {
        lg->record = (int8_t)min(lg->record + 1, DREAD_RECORD_MAX);
        how = "beaten cleanly: the record rises";
    } else if ((lost > made) || ((2 * left >= made) && (lost > 0)))
    {
        lg->record = (int8_t)max(lg->record - 1, -DREAD_RECORD_MAX);
        how = "it hurt: the record falls";
    } else
    {
        lg->record = (int8_t)((lg->record > 0) ? lg->record - 1 : (lg->record < 0) ? lg->record + 1 : 0);
        how = "even: the record eases";
    }
    SYNCLOG("World: dread of player %d: the raid is over (%d heroes came, %d left alive, %d of the keeper's creatures"
        " lost): %s (record %+.1f)", (int)plyr_idx, made, left, lost, how, lg->record / 2.0);
    lg->raid_made = lg->raid_left = lg->raid_lost = 0;
}

void world_dread_seat_started(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    memset(&dread[plyr_idx], 0, sizeof(dread[plyr_idx]));
    game.world_dread[plyr_idx] = 0;
    game.world_dread_flags[plyr_idx] = 0;
}

/** Test hook KFX_WORLD_DREADCULL=<turn>: at that turn four in five of player 0's creatures (not imps) die, so that
 *  the next raid shows the floor of mercy. */
static void dread_cull_hook(void)
{
    static long at = -1;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_DREADCULL");
        at = (v != NULL) ? atol(v) : 0;
    }
    if ((at <= 0) || (game.play_gameturn != (GameTurn)at))
        return;
    struct Dungeon *dungeon = get_players_num_dungeon(PLAYER0);
    if (dungeon_invalid(dungeon))
        return;
    long i = dungeon->creatr_list_start;
    long guard = 0;
    int n = 0, killed = 0;
    while ((i > 0) && (guard++ < CREATURES_COUNT))
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing))
            break;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
            break;
        i = cctrl->players_next_creature_idx;
        if ((n++ % 5) != 0)
        {
            kill_creature(thing, INVALID_THING, -1, CrDed_NoUnconscious);
            killed++;
        }
    }
    SYNCLOG("World test: the dread cull killed %d of player 0's %d creatures", killed, n);
}

void world_dread_turn(void)
{
    if (!world_mode_active())
        return;
    dread_cull_hook();
    if ((game.play_gameturn % 64) != 21)
        return;
    long traps[PLAYERS_MAX], doors[PLAYERS_MAX];
    memset(traps, 0, sizeof(traps));
    memset(doors, 0, sizeof(doors));
    dread_count_defences(traps, doors);
    const TbBool log_now = dread_test_log() && ((game.play_gameturn % 2048) == 21);
    for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
    {
        if (!world_player_is_human(p))
        {
            game.world_dread[p] = 0;
            game.world_dread_flags[p] = 0;
            continue;
        }
        if (world_caretaker_active(p) && (game.world_dread[p] > 0))
            continue; /* held while its keeper is away */
        dread_note_gold(p);
        struct DreadParts dp;
        dread_work_out(p, traps[p], doors[p], &dp);
        dread_note_peak(p, &dp);
        dread_publish(p, &dp);
        if (log_now)
            dread_log("now", p, &dp);
    }
}

/******************************************************************************/
/* The meter */

/** Text in a box (align 0 left, 1 centre, 2 right); px: about the capitals' height. */
static void dread_text(int x, int y, int w, int px, int align, const char *text)
{
    const unsigned long flg_mem = RendererGetDrawFlags();
    LbTextSetFont(winfont);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags((align == 1) ? Lb_TEXT_HALIGN_CENTER : (align == 2) ? Lb_TEXT_HALIGN_RIGHT : Lb_TEXT_HALIGN_LEFT);
    LbTextSetWindow(x, y, w, 2 * px + 4);
    const int line = max(LbTextLineHeight(), 1);
    LbTextDrawResized(0, 0, max(36 * px / line, 4), text);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

/** A small skull, drawn in boxes (u: one of its pixels): a dome, two eye holes, teeth. */
static void dread_skull(int x, int y, int u, unsigned char col, unsigned char hole)
{
    LbDrawBox(x + u, y, 4 * u, u, col);
    LbDrawBox(x, y + u, 6 * u, 3 * u, col);
    LbDrawBox(x + u, y + 4 * u, 4 * u, u, col);
    LbDrawBox(x + u, y + 2 * u, u, u, hole);
    LbDrawBox(x + 4 * u, y + 2 * u, u, u, hole);
    LbDrawBox(x + u, y + 5 * u, u, u, col);
    LbDrawBox(x + 3 * u, y + 5 * u, u, u, col);
}

void world_dread_draw(int x, int y, int w)
{
    if (!world_mode_active())
        return;
    const PlayerNumber me = my_player_number;
    if ((me < 0) || (me >= PLAYERS_MAX) || (game.world_dread[me] == 0))
        return;
    const int lv = game.world_dread[me];
    const TbBool mercy = (game.world_dread_flags[me] & 1) != 0;
    int next = 0;
    const int tier = world_raid_tier_of(lv, &next);
    const int tiers = world_raid_tiers();
    const int upp = units_per_pixel;
    const int u = max(1, upp / 16);
    const int h = 22 * upp / 16;
    /* as wide as the skulls need (wider than the dial), its right edge under the dial's */
    const int need = tiers * 6 * u + (tiers - 1) * 2 * u + 8 * u;
    if (w < need)
    {
        x -= need - w;
        w = need;
    }
    /* the panel: flat and dark, a thin rim */
    LbDrawBox(x, y, w, h, colours[1][1][1]);
    LbDrawBox(x + u, y + u, w - 2 * u, h - 2 * u, colours[3][2][2]);
    LbDrawBox(x + 2 * u, y + 2 * u, w - 4 * u, h - 4 * u, colours[1][1][1]);
    /* one skull per raid tier, lit up to the keeper's */
    const int sk = 6 * u, gap = 2 * u;
    const int row = tiers * sk + (tiers - 1) * gap;
    int sx = x + (w - row) / 2;
    const int sy = y + 4 * u;
    for (int t = 0; t < tiers; t++)
    {
        unsigned char col = colours[4][3][3];
        if (t <= tier)
            col = mercy ? colours[9][11][13] : (t == tiers - 1) ? colours[15][4][2] : colours[13][12][10];
        dread_skull(sx, sy, u, col, colours[1][1][1]);
        sx += sk + gap;
    }
    /* the bar to the next tier (full at the last) */
    const int bx = x + 4 * u, by = y + h - 6 * u, bw = w - 8 * u, bh = 2 * u;
    LbDrawBox(bx, by, bw, bh, colours[2][2][2]);
    int fill = bw;
    if (next > 0)
    {
        int lo = world_raid_tier_min(tier);
        fill = (next > lo) ? bw * (lv - lo) / (next - lo) : bw;
    }
    LbDrawBox(bx, by, max(u, min(fill, bw)), bh, mercy ? colours[9][11][13] : colours[13][3][2]);
    char text[24];
    snprintf(text, sizeof(text), mercy ? "Mercy %d" : "Dread %d", lv);
    dread_text(x, y + h + u, w, 6 * upp / 16, 1, text);
}

/******************************************************************************/
/* Saving (XTR2 record LDG1): the players with something in their ledger. */
#pragma pack(1)
struct DreadSaved {
    uint32_t plyr;
    struct DreadLedger lg;
};
#pragma pack()

static TbBool dread_wanted(PlayerNumber p)
{
    const struct DreadLedger *lg = &dread[p];
    return (lg->record != 0) || (lg->peak != 0) || (lg->raid_made != 0);
}

void world_dread_reset(void)
{
    memset(dread, 0, sizeof(dread));
    memset(game.world_dread, 0, sizeof(game.world_dread));
    memset(game.world_dread_flags, 0, sizeof(game.world_dread_flags));
}

size_t world_dread_save_size(void)
{
    size_t n = 0;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
        if (dread_wanted(p))
            n++;
    return (n > 0) ? 4 + n * sizeof(struct DreadSaved) : 0;
}

void world_dread_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *at = buf + 4;
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (!dread_wanted(p))
            continue;
        struct DreadSaved s;
        memset(&s, 0, sizeof(s));
        s.plyr = (uint32_t)p;
        s.lg = dread[p];
        memcpy(at, &s, sizeof(s));
        at += sizeof(s);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_dread_load(const uint8_t *buf, size_t len)
{
    world_dread_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((n > PLAYERS_MAX) || (len < 4 + (size_t)n * sizeof(struct DreadSaved)))
        return false;
    for (uint32_t k = 0; k < n; k++)
    {
        struct DreadSaved s;
        memcpy(&s, buf + 4 + (size_t)k * sizeof(s), sizeof(s));
        if (s.plyr >= PLAYERS_MAX)
            continue;
        dread[s.plyr] = s.lg;
        if (dread[s.plyr].record > DREAD_RECORD_MAX)
            dread[s.plyr].record = DREAD_RECORD_MAX;
        if (dread[s.plyr].record < -DREAD_RECORD_MAX)
            dread[s.plyr].record = -DREAD_RECORD_MAX;
    }
    return true;
}

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_feats.c
 *     Endless world: the Keeper's Rap Sheet.
 * @par Purpose:
 *     See world_feats.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "world_feats.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "world_stream.h"
#include "world_progress.h"
#include "world_daynight.h"
#include "world_colours.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_utils.h"
#include "player_instances.h"
#include "local_camera.h"
#include "gui_parchment.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "creature_control.h"
#include "creature_states.h"
#include "world_caretaker.h"
#include "config_magic.h"
#include "map_data.h"
#include "gui_msgs.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "frontend.h"
#include "chat_box.h"
#include "server_admin.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "power_hand.h"
#include "magic_powers.h"
#include "thing_creature.h"
#include "config_creature.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct FeatInfo {
    const char *name;
    const char *line;      /* what is counted */
    uint32_t tier[WORLD_FEAT_TIERS];
};
static const struct FeatInfo feats[WFt_COUNT] = {
    {"Poultry in motion", "Heroes turned into chickens",          {1, 5, 15, 40, 100}},
    {"Frequent flyer",    "Creatures dropped into battle",        {10, 50, 200, 600, 2000}},
    {"Night shift",       "Outposts cleared between dusk and dawn", {1, 3, 8, 20, 50}},
    {"Heartbreaker",      "Keepers' Hearts destroyed",            {1, 3, 8, 20, 50}},
    {"Slave driver",      "Creatures slapped",                    {25, 150, 600, 2000, 6000}},
    {"Bone idle",         "Skeletons raised in the Prison",       {1, 5, 20, 60, 150}},
    {"Portal hunter",     "Portals claimed",                      {1, 3, 6, 12, 25}},
    {"Clean hands",       "Raids beaten without casting a spell", {1, 3, 10, 25, 60}},
    {"Old bones",         "Days your Heart has beaten",           {7, 30, 100, 200, 365}},
};
/* the XP each tier pays (a destroyed keeper's Heart is 1000) */
static const uint32_t tier_xp[WORLD_FEAT_TIERS] = {100, 250, 500, 1000, 2000};
static const char *const tier_names[WORLD_FEAT_TIERS + 1] = {"-", "I", "II", "III", "IV", "V"};

/* the last turn each keeper cast a spell (for Clean hands; saved) */
static uint32_t ft_last_spell[PLAYERS_MAX];

#define fcount game.world_feat_count
#define ftier game.world_feat_tier

static TbBool counts_for(PlayerNumber plyr_idx)
{
    return world_mode_active() && (plyr_idx >= 0) && (plyr_idx < PLAYERS_COUNT) && (plyr_idx < PLAYERS_MAX)
        && world_player_is_human(plyr_idx) && !world_caretaker_active(plyr_idx); /* away: nothing counts (Old bones
                                                                                       catches up on return) */
}

static int tier_of(int feat, uint32_t count)
{
    int t = 0;
    while ((t < WORLD_FEAT_TIERS) && (count >= feats[feat].tier[t]))
        t++;
    return t;
}

/** A count changed: the tiers it reached pay their XP (on the simulating machine; the XP and tier reach the others
 *  in the game state). */
static void check_tiers(PlayerNumber p, int feat)
{
    const int now = tier_of(feat, fcount[p][feat]);
    while (ftier[p][feat] < now)
    {
        const int t = ftier[p][feat]++;
        SYNCLOG("World: player %d's feat %s reached tier %s (%u): %u XP", (int)p, feats[feat].name, tier_names[t + 1],
            (unsigned)fcount[p][feat], (unsigned)tier_xp[t]);
        world_add_player_xp(p, tier_xp[t]);
    }
}

void world_feats_add(PlayerNumber plyr_idx, int feat, uint32_t n)
{
    if (!counts_for(plyr_idx) || (feat < 0) || (feat >= WFt_COUNT) || (n == 0))
        return;
    if (network_is_active() && !netstate_is_server())
        return; /* a thin client: the server counts */
    uint32_t *c = &fcount[plyr_idx][feat];
    *c = (*c > UINT32_MAX - n) ? UINT32_MAX : (*c + n);
    check_tiers(plyr_idx, feat);
}

/* Against farming (self-check 2026-11-13): a creature counts toward a feat at most once in a while (a slap, a drop),
   or once in its life (a hero turned into a chicken). By control slot and creation turn; not saved (a load lets one
   more count, no more). */
enum { FtOnce_Slap = 0, FtOnce_Drop, FtOnce_Chicken, FtOnce_COUNT };
static uint32_t ft_once_at[FtOnce_COUNT][CREATURES_COUNT];   /* 1 + the turn it last counted (0: never) */
static int32_t ft_once_born[FtOnce_COUNT][CREATURES_COUNT];

static TbBool counts_once(const struct Thing *thing, int slot, uint32_t gap)
{
    const CctrlIndex i = thing->ccontrol_idx;
    if ((i <= 0) || (i >= CREATURES_COUNT))
        return false;
    const uint32_t now = (uint32_t)game.play_gameturn;
    if ((ft_once_at[slot][i] != 0) && (ft_once_born[slot][i] == thing->creation_turn)
        && ((gap == 0) || (now - (ft_once_at[slot][i] - 1) < gap)))
        return false;
    ft_once_at[slot][i] = now + 1;
    ft_once_born[slot][i] = thing->creation_turn;
    return true;
}

void world_feats_chickened(struct Thing *victim, PlayerNumber caster)
{
    if (thing_exists(victim) && (victim->owner == PLAYER_GOOD) && counts_for(caster) && counts_once(victim, FtOnce_Chicken, 0))
        world_feats_add(caster, WFt_Poultry, 1); /* each hero once */
}

/** "Into battle": an enemy creature stands within four subtiles of where it landed. */
static TbBool enemy_near(const struct Thing *creatng, PlayerNumber plyr_idx)
{
    const MapSubtlCoord cx = creatng->mappos.x.stl.num, cy = creatng->mappos.y.stl.num;
    for (MapSubtlCoord y = cy - 4; y <= cy + 4; y++)
    {
        for (MapSubtlCoord x = cx - 4; x <= cx + 4; x++)
        {
            if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                continue;
            long i = get_mapwho_thing_index(get_map_block_at(x, y));
            unsigned long guard = 0;
            while ((i != 0) && (guard++ < THINGS_COUNT))
            {
                const struct Thing *t = thing_get(i);
                if (thing_is_invalid(t))
                    break;
                i = t->next_on_mapblk;
                if ((t != creatng) && thing_is_creature(t) && (t->owner != plyr_idx) && players_are_enemies(plyr_idx, t->owner)
                    && !creature_is_kept_in_custody(t) && !creature_is_being_unconscious(t) && !creature_is_dying(t)
                    && !thing_is_dragged_or_pulled(t))
                    return true; /* (a captive, an unconscious, dying or dragged enemy is no battle) */
            }
        }
    }
    return false;
}

void world_feats_dropped(struct Thing *creatng, PlayerNumber plyr_idx)
{
    if (counts_for(plyr_idx) && thing_exists(creatng) && thing_is_creature(creatng) && enemy_near(creatng, plyr_idx)
        && counts_once(creatng, FtOnce_Drop, 600))
        world_feats_add(plyr_idx, WFt_Flyer, 1); /* a creature once in 30 seconds */
}

void world_feats_slapped(struct Thing *creatng, PlayerNumber plyr_idx)
{
    if (counts_for(plyr_idx) && thing_exists(creatng) && thing_is_creature(creatng) && counts_once(creatng, FtOnce_Slap, 1200))
        world_feats_add(plyr_idx, WFt_SlaveDriver, 1); /* a creature once a minute */
}

void world_feats_power_used(PlayerNumber plyr_idx, int pwkind)
{
    if (!counts_for(plyr_idx))
        return;
    switch (pwkind)
    {
    case PwrK_None: case PwrK_HAND: case PwrK_SLAP: case PwrK_PICKUPCRTR: case PwrK_PICKUPGOLD: case PwrK_PICKUPFOOD:
        return; /* the Hand is not a spell */
    default:
        ft_last_spell[plyr_idx] = (uint32_t)game.play_gameturn;
        break;
    }
}

/* the turn the keeper's last raid landed, +1 (0: unknown, as after a load: then the day's first turn counts) */
static uint32_t ft_raid_landed[PLAYERS_MAX];

void world_feats_raid_landed(PlayerNumber plyr_idx)
{
    if ((plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX))
        ft_raid_landed[plyr_idx] = (uint32_t)game.play_gameturn + 1;
}

void world_feats_raid_over(PlayerNumber plyr_idx, TbBool beaten, long landed_day)
{
    if (!counts_for(plyr_idx) || !beaten || !thing_exists(get_player_soul_container(plyr_idx)))
        return;
    long landed_turn = (max(landed_day, 1L) - 1) * world_day_turns();
    if ((plyr_idx < PLAYERS_MAX) && (ft_raid_landed[plyr_idx] > (uint32_t)landed_turn))
        landed_turn = (long)ft_raid_landed[plyr_idx] - 1; /* the landing itself, not the day's first turn */
    if ((long)ft_last_spell[plyr_idx] >= landed_turn)
    {
        SYNCLOG("World: player %d beat a raid, but cast a spell during it", (int)plyr_idx);
        return;
    }
    world_feats_add(plyr_idx, WFt_CleanHands, 1);
}

/** KFX_WORLD_FEATTEST=1 (test switch): every 500 turns, this machine's keeper does a little of every feat that has
 *  a hook (a slap counted, a chicken counted, ...) through world_feats_add, and the counts are logged. */
static TbBool feat_test(void)
{
    static int on = -1;
    if (on < 0)
    {
        on = (getenv("KFX_WORLD_FEATTEST") != NULL) ? 1 : 0;
        if (on)
            SYNCLOG("World: test switch KFX_WORLD_FEATTEST");
    }
    return on;
}

static void feats_real_test(void);

void world_feats_turn(void)
{
    feats_real_test();
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()))
        return;
    if ((game.play_gameturn % 64) == 13)
    {
        for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (p < PLAYERS_MAX); p++)
        {
            if (!counts_for(p) || !thing_exists(get_player_soul_container(p)))
                continue;
            const uint32_t day = (uint32_t)max(world_seat_day(p), 0L);
            if (day > fcount[p][WFt_OldBones])
            {
                fcount[p][WFt_OldBones] = day;
                check_tiers(p, WFt_OldBones);
            }
        }
    }
    if (feat_test() && ((game.play_gameturn % 500) == 250))
    {
        const PlayerNumber p = my_player_number;
        world_feats_add(p, WFt_SlaveDriver, 30);
        {
            /* the anti-farming limit: three slaps on one creature through the slap hook count once */
            const struct Dungeon *dg = get_players_num_dungeon(p);
            struct Thing *t = dungeon_invalid(dg) ? INVALID_THING : thing_get(dg->creatr_list_start);
            if (thing_exists(t))
            {
                const uint32_t before = fcount[p][WFt_SlaveDriver];
                for (int k = 0; k < 3; k++)
                    world_feats_slapped(t, p);
                SYNCLOG("World: feat test: three slaps on one %s counted %u", thing_model_name(t), (unsigned)(fcount[p][WFt_SlaveDriver] - before));
            }
        }
        world_feats_add(p, WFt_Poultry, 2);
        world_feats_add(p, WFt_Flyer, 12);
        char line[256];
        int n = 0;
        for (int f = 0; f < WFt_COUNT; f++)
            n += snprintf(line + n, sizeof(line) - n, " %u/%d", (unsigned)fcount[p][f], (int)ftier[p][f]);
        int top[3];
        world_feats_top3(p, top);
        SYNCLOG("World: feat test: player %d counts/tiers%s; top %s, %s, %s; level %d", (int)p, line,
            (top[0] >= 0) ? feats[top[0]].name : "-", (top[1] >= 0) ? feats[top[1]].name : "-",
            (top[2] >= 0) ? feats[top[2]].name : "-", world_player_level(p));
        SYNCLOG("World: feat test: the player panel tooltip reads \"Number of creatures: <name>%s\"", world_feats_top3_text(p));
    }
}

/** KFX_WORLD_FEATREAL=turn (test switch): the real actions, through the game's own paths: two of this machine's Flies,
 *  one slapped three times and one once (slap_thing); an Archer of the heroes beside the Heart, a Fly dropped on it by
 *  the Hand (dump_first_held_thing_on_map), and the Archer turned into a chicken by the Chicken power cast for free
 *  (magic_use_power_direct). The changes of Slave driver, Frequent flyer and Poultry in motion are logged. */
static void feats_real_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_FEATREAL");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !world_mode_active())
        return;
    const PlayerNumber p = my_player_number;
    const struct Thing *heart = get_player_soul_container(p);
    if (((int)p >= PLAYERS_MAX) || !thing_exists(heart))
        return;
    struct PlayerInfo *player = get_player(p);
    struct Coord3d pos = heart->mappos;
    pos.y.val += 6 * COORD_PER_STL;
    const uint32_t slap0 = fcount[p][WFt_SlaveDriver], fly0 = fcount[p][WFt_Flyer], hen0 = fcount[p][WFt_Poultry];
    struct Thing *a = create_creature(&pos, (ThingModel)get_id(creature_desc, "FLY"), p);
    struct Thing *b = create_creature(&pos, (ThingModel)get_id(creature_desc, "FLY"), p);
    struct Coord3d hp = pos;
    hp.x.val += 2 * COORD_PER_STL;
    struct Thing *hero = create_creature(&hp, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
    if (thing_is_invalid(a) || thing_is_invalid(b) || thing_is_invalid(hero))
        return;
    const NetUserId u = get_player_primary_user(player);
    for (int i = 0; i < 3; i++)
        slap_thing(player, u, a);
    slap_thing(player, u, b);
    if (place_thing_in_power_hand(b, p))
        dump_first_held_thing_on_map(p, hero->mappos.x.stl.num, hero->mappos.y.stl.num, true);
    magic_use_power_direct(p, PwrK_CHICKEN, 0, hero->mappos.x.stl.num, hero->mappos.y.stl.num, hero, PwMod_CastForFree);
    SYNCLOG("World: feat real test: Slave driver +%u (two creatures slapped, one of them three times: 2 expected), "
        "Frequent flyer +%u (1 expected), Poultry in motion +%u (1 expected)", (unsigned)(fcount[p][WFt_SlaveDriver] - slap0),
        (unsigned)(fcount[p][WFt_Flyer] - fly0), (unsigned)(fcount[p][WFt_Poultry] - hen0));
}

void world_feats_present(void)
{
    static uint8_t seen[WORLD_FEAT_SLOTS];
    static PlayerNumber watched = -2;
    if (!world_mode_active() || (my_player_number >= PLAYERS_MAX))
        return;
    const PlayerNumber p = my_player_number;
    const TbBool news = (watched == p);
    for (int f = 0; f < WFt_COUNT; f++)
    {
        const int t = ftier[p][f];
        if (news && (t > seen[f]) && (t <= WORLD_FEAT_TIERS))
            message_add_fmt(MsgType_Player, p, "Feat: %s %s (%s) +%u XP", feats[f].name, tier_names[t], feats[f].line,
                (unsigned)tier_xp[t - 1]);
        seen[f] = (uint8_t)t;
    }
    watched = p;
}

void world_feats_seat_reset(PlayerNumber plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    memset(fcount[plyr_idx], 0, sizeof(fcount[plyr_idx]));
    memset(ftier[plyr_idx], 0, sizeof(ftier[plyr_idx]));
    ft_last_spell[plyr_idx] = 0;
}

int world_feats_tier(PlayerNumber plyr_idx, int feat)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX) || (feat < 0) || (feat >= WFt_COUNT))
        return 0;
    return ftier[plyr_idx][feat];
}

const char *world_feats_name(int feat)
{
    return ((feat >= 0) && (feat < WFt_COUNT)) ? feats[feat].name : "";
}

/** How far toward its next tier a feat is, in thousandths (1000 at the top tier). */
static int progress_of(PlayerNumber p, int f)
{
    const int t = ftier[p][f];
    if (t >= WORLD_FEAT_TIERS)
        return 1000;
    const uint32_t lo = (t > 0) ? feats[f].tier[t - 1] : 0, hi = feats[f].tier[t];
    const uint32_t c = fcount[p][f];
    return (c <= lo) ? 0 : (int)((uint64_t)(c - lo) * 1000 / (hi - lo));
}

void world_feats_top3(PlayerNumber plyr_idx, int out[3])
{
    out[0] = out[1] = out[2] = -1;
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return;
    for (int k = 0; k < 3; k++)
    {
        int best = -1, best_score = 0;
        for (int f = 0; f < WFt_COUNT; f++)
        {
            if ((f == out[0]) || (f == out[1]) || (ftier[plyr_idx][f] == 0))
                continue;
            const int score = ftier[plyr_idx][f] * 2000 + progress_of(plyr_idx, f);
            if (score > best_score)
            {
                best = f;
                best_score = score;
            }
        }
        out[k] = best;
    }
}

const char *world_feats_top3_text(PlayerNumber plyr_idx)
{
    static char text[96];
    text[0] = '\0';
    if (!world_mode_active() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return text;
    int top[3];
    world_feats_top3(plyr_idx, top);
    int n = 0;
    for (int k = 0; (k < 3) && (top[k] >= 0) && (n < (int)sizeof(text) - 1); k++)
        n += snprintf(text + n, sizeof(text) - n, "%s%s %s", (k > 0) ? ", " : " (", feats[top[k]].name, tier_names[ftier[plyr_idx][top[k]]]);
    if ((n > 0) && (n < (int)sizeof(text) - 1))
        snprintf(text + n, sizeof(text) - n, ")");
    return text;
}

/******************************************************************************/
/* the Rap Sheet window */
#define FT_W 440
#define FT_H 300

static char ft_close[] = "Close";

static void ft_close_click(struct GuiButton *gbtn)
{
    (void)gbtn;
    turn_off_menu(GMnu_KEEPER_FEATS);
}

static void ft_background(struct GuiMenu *gmnu);

static struct GuiButtonInit world_feats_buttons[] = {
  {LbBtnT_NormalBtn, BID_DEFAULT, 0, 1, ft_close_click, NULL, NULL, 0, 999, FT_H - 40, 999, FT_H - 40, 110, 30, gui_area_text, 1, GUIStr_Empty, 0, {.str = ft_close}, 0, NULL },
  {              -1, BID_DEFAULT, 0, 0, NULL,           NULL, NULL, 0,   0,        0,   0,        0,   0,  0, NULL,          0, 0,            0, {0},              0, NULL },
};

struct GuiMenu world_feats_menu =
 { GMnu_KEEPER_FEATS, 0, 1, world_feats_buttons, POS_GAMECTR, POS_GAMECTR, FT_W, FT_H, ft_background, 0, NULL, NULL, 0, 1, 0,};

/** A text remap that tints the font: 1 gold (a feat had), 2 grey (not yet); built once from the palette. */
static const unsigned char *ft_tint(int which)
{
    static unsigned char maps[2][256];
    static TbBool made = false;
    if ((which < 1) || (which > 2))
        return NULL;
    if (!made)
    {
        for (int i = 0; i < 256; i++)
        {
            const unsigned char *c = &engine_palette[3 * i];
            const int lum = (c[0] + c[1] + c[2]) / 3;
            maps[0][i] = LbPaletteFindColour(engine_palette, (unsigned char)min(63, lum + 8),
                (unsigned char)min(63, lum * 4 / 5 + 4), (unsigned char)(lum * 2 / 7));
            maps[1][i] = LbPaletteFindColour(engine_palette, (unsigned char)(lum * 2 / 3), (unsigned char)(lum * 2 / 3),
                (unsigned char)(lum * 2 / 3));
        }
        made = true;
    }
    return maps[which - 1];
}

static void ft_text(int x, int y, int w, int px, int align, const char *text, int tint)
{
    const unsigned long flg_mem = RendererGetDrawFlags();
    LbTextSetFont(winfont);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags((align == 1) ? Lb_TEXT_HALIGN_CENTER : (align == 2) ? Lb_TEXT_HALIGN_RIGHT : Lb_TEXT_HALIGN_LEFT);
    LbTextSetWindow(x, y, w, 2 * px + 4);
    const int line = max(LbTextLineHeight(), 1);
    const unsigned char *old_remap = LbTextGetRemap();
    const unsigned char *map = ft_tint(tint);
    if (map != NULL)
        LbTextSetRemap(map);
    LbTextDrawResized(0, 0, max(36 * px / line, 4), text);
    if (map != NULL)
        LbTextSetRemap(old_remap);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

static TbPixel ft_colour(int r, int g, int b)
{
    return LbPaletteFindColour(engine_palette, (unsigned char)r, (unsigned char)g, (unsigned char)b);
}

static void ft_background(struct GuiMenu *gmnu)
{
    static int drawn_logged = 0;
    if (!drawn_logged) { drawn_logged = 1; SYNCLOG("World: the Rap Sheet is drawn (%d x %d)", (int)gmnu->width, (int)gmnu->height); }
    const int upp = max((gmnu->width * 16 + FT_W / 2) / FT_W, 1);
#define S(v) ((v) * upp / 16)
    static TbPixel panel, edge, bar_bg, bar_fg;
    const int gold = 1, dim = 2;
    if (panel == 0)
    {
        panel = ft_colour(5, 5, 7);
        edge = ft_colour(22, 20, 16);
        bar_bg = ft_colour(12, 11, 10);
        bar_fg = ft_colour(46, 34, 10);
    }
    const int x = gmnu->pos_x, y = gmnu->pos_y, w = gmnu->width, h = gmnu->height;
    /* a flat dark panel with a thin frame */
    LbDrawBox(x, y, w, h, panel);
    LbDrawBox(x, y, w, max(S(1), 1), edge);
    LbDrawBox(x, y + h - max(S(1), 1), w, max(S(1), 1), edge);
    LbDrawBox(x, y, max(S(1), 1), h, edge);
    LbDrawBox(x + w - max(S(1), 1), y, max(S(1), 1), h, edge);
    const PlayerNumber p = my_player_number;
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_MAX))
        return;
    char text[128];
    ft_text(x + S(12), y + S(8), w - S(24), max(S(8), 7), 0, "The Keeper's Rap Sheet", gold);
    snprintf(text, sizeof(text), "Player level %d", world_player_level(p));
    ft_text(x + S(12), y + S(10), w - S(24), max(S(6), 6), 2, text, dim);
    /* the feats: name and tier, what is counted, a bar to the next tier */
    const int col_w = S(200);
    const int row_h = S(28);
    int top_y = y + S(30);
    for (int f = 0; f < WFt_COUNT; f++)
    {
        const int cx = x + S(12) + (f % 2) * (col_w + S(16));
        const int cy = top_y + (f / 2) * row_h;
        const int t = ftier[p][f];
        snprintf(text, sizeof(text), "%s", feats[f].name);
        ft_text(cx, cy, col_w, max(S(6), 6), 0, text, (t > 0) ? gold : dim);
        ft_text(cx, cy, col_w, max(S(6), 6), 2, tier_names[t], (t > 0) ? gold : dim);
        if (t < WORLD_FEAT_TIERS)
            snprintf(text, sizeof(text), "%s: %u / %u", feats[f].line, (unsigned)fcount[p][f], (unsigned)feats[f].tier[t]);
        else
            snprintf(text, sizeof(text), "%s: %u", feats[f].line, (unsigned)fcount[p][f]);
        ft_text(cx, cy + S(9), col_w, max(S(5), 6), 0, text, dim);
        const int bar_w = col_w, bar_h = max(S(2), 2);
        LbDrawBox(cx, cy + S(20), bar_w, bar_h, bar_bg);
        LbDrawBox(cx, cy + S(20), bar_w * progress_of(p, f) / 1000, bar_h, bar_fg);
    }
    /* the other keepers: their top three */
    int ky = top_y + ((WFt_COUNT + 1) / 2) * row_h + S(2);
    ft_text(x + S(12), ky, w - S(24), max(S(5), 5), 0, "Other keepers", gold);
    ky += S(10);
    int shown = 0;
    for (PlayerNumber o = 0; (o < PLAYERS_COUNT) && (o < PLAYERS_MAX) && (ky < y + h - S(48)); o++)
    {
        if ((o == p) || !world_player_is_human(o) || !world_player_listed(o) || !player_exists(get_player(o)))
            continue;
        int top[3];
        world_feats_top3(o, top);
        const char *name = server_admin_player_name(o);
        int n = snprintf(text, sizeof(text), "%s (level %d):", name, world_player_level(o));
        if (top[0] < 0)
            snprintf(text + n, sizeof(text) - n, " no feats yet");
        for (int k = 0; (k < 3) && (top[k] >= 0) && (n < (int)sizeof(text)); k++)
            n += snprintf(text + n, sizeof(text) - n, "%s %s %s", (k > 0) ? "," : "", feats[top[k]].name,
                tier_names[ftier[o][top[k]]]);
        ft_text(x + S(12), ky, w - S(24), max(S(5), 5), 0, text, dim);
        ky += S(9);
        shown++;
    }
    if (shown == 0)
        ft_text(x + S(12), ky, w - S(24), max(S(5), 5), 0, "No other keeper here", dim);
#undef S
}

TbBool world_feats_command(const char *text)
{
    if ((text == NULL) || !world_mode_active() || ((text[0] != '!') && (text[0] != '/')))
        return false;
    const char *s = text + 1;
    if ((strcasecmp(s, "feats") != 0) && (strcasecmp(s, "rapsheet") != 0))
        return false;
    if (menu_is_active(GMnu_KEEPER_FEATS))
        turn_off_menu(GMnu_KEEPER_FEATS);
    else
    {
        if (get_local_view_type() == PVT_MapScreen)
            zoom_from_parchment_map();
        turn_on_menu(GMnu_KEEPER_FEATS);
        SYNCLOG("World: the Rap Sheet opens");
    }
    return true;
}

void world_feats_menu_turn(void)
{
    /* KFX_WORLD_FEATSPAGE=turn (test switch): the Rap Sheet opens itself at that turn */
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_FEATSPAGE");
        at = (v != NULL) ? atol(v) : -1;
    }
    if ((at >= 0) && ((long)game.play_gameturn >= at) && world_mode_active())
    {
        at = -1;
        world_feats_command("!feats");
    }
}

/******************************************************************************/
void world_feats_reset(void)
{
    memset(fcount, 0, sizeof(fcount));
    memset(ftier, 0, sizeof(ftier));
    memset(ft_last_spell, 0, sizeof(ft_last_spell));
    memset(ft_raid_landed, 0, sizeof(ft_raid_landed));
    memset(ft_once_at, 0, sizeof(ft_once_at));
    memset(ft_once_born, 0, sizeof(ft_once_born));
}

size_t world_feats_save_size(void)
{
    for (int p = 0; p < PLAYERS_MAX; p++)
        for (int f = 0; f < WORLD_FEAT_SLOTS; f++)
            if ((fcount[p][f] != 0) || (ft_last_spell[p] != 0))
                return 8 + sizeof(fcount) + sizeof(ftier) + sizeof(ft_last_spell);
    return 0;
}

void world_feats_save(uint8_t *buf)
{
    const uint32_t n = PLAYERS_MAX, slots = WORLD_FEAT_SLOTS;
    memcpy(buf, &n, 4);
    memcpy(buf + 4, &slots, 4);
    size_t at = 8;
    memcpy(buf + at, fcount, sizeof(fcount));
    at += sizeof(fcount);
    memcpy(buf + at, ftier, sizeof(ftier));
    at += sizeof(ftier);
    memcpy(buf + at, ft_last_spell, sizeof(ft_last_spell));
}

TbBool world_feats_load(const uint8_t *buf, size_t len)
{
    world_feats_reset();
    uint32_t n = 0, slots = 0;
    if (len < 8)
        return false;
    memcpy(&n, buf, 4);
    memcpy(&slots, buf + 4, 4);
    if ((slots == 0) || (slots > 255) || ((size_t)n * slots * 5 + (size_t)n * 4 + 8 > len))
        return false;
    const uint8_t *c = buf + 8, *t = c + (size_t)n * slots * 4, *ls = t + (size_t)n * slots;
    for (uint32_t p = 0; p < n; p++)
    {
        for (uint32_t f = 0; f < slots; f++)
        {
            if ((p >= PLAYERS_MAX) || (f >= WORLD_FEAT_SLOTS))
                continue;
            memcpy(&fcount[p][f], c + ((size_t)p * slots + f) * 4, 4);
            ftier[p][f] = t[(size_t)p * slots + f];
        }
        if (p < PLAYERS_MAX)
            memcpy(&ft_last_spell[p], ls + (size_t)p * 4, 4);
    }
    return true;
}

#ifdef __cplusplus
}
#endif

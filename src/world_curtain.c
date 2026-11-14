/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_curtain.c
 *     Endless world: the loading screen as a curtain over a running game (see world_curtain.h).
 * @par Purpose:
 *     The picture is the one the game shows while a level loads (data/loading64.raw and its palette). A level's
 *     loading screen sets that palette for the whole screen; a running game keeps its own. The OpenGL renderer
 *     draws a picture with a palette of its own beside the game's, so there the picture has its true colours. The
 *     software renderer has one palette for the screen: there each of the picture's colours is matched to the
 *     nearest of the game's palette, once, when the curtain goes up (black and the disc's reds are in it; its pale
 *     highlights come out a little pinker). It is drawn as the frame's one picture, as the full-screen map's
 *     parchment is, at any size: whole, in the middle, black beside it.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_curtain.h"

#include <stdlib.h>
#include <string.h>

#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_video.h"
#include "front_simple.h"
#include "game_legacy.h"
#include "vidmode.h"
#include "kfx/renderer/RendererManager.h"
#include "post_inc.h"

static TbBool curtain_on;            /* it is up (as far as its turns go) */
static TbBool curtain_held;          /* something asked this turn that it stay up */
static GameTurn curtain_held_turn;   /* the turn that was asked in */
static GameTurn curtain_since;       /* the turn it went up */
static TbClockMSec curtain_since_clock; /* and the clock then */
static GameTurn curtain_min_until;   /* up at least until this turn */
static unsigned char *curtain_img;   /* the loading picture (NULL: not loaded) ... */
static unsigned char *curtain_near;  /* ... the same in the game's palette (the software renderer) ... */
static unsigned char curtain_pal[256 * 4]; /* ... and its own palette, as the renderer takes one: B, G, R, 255 */
static int curtain_w, curtain_h;
static TbBool curtain_failed;        /* the picture could not be loaded: there is no curtain */

static void curtain_free(void)
{
    free(curtain_img);
    free(curtain_near);
    curtain_img = NULL;
    curtain_near = NULL;
    curtain_w = curtain_h = 0;
}

static TbBool curtain_load(void)
{
    if (curtain_img != NULL)
        return true;
    if (curtain_failed || (engine_palette == NULL))
        return false;
    unsigned char *raw = NULL, *pal = NULL;
    int w = 0, h = 0;
    if (!load_loading_picture(&raw, &pal, &w, &h) || (w <= 0) || (h <= 0))
    {
        WARNLOG("World curtain: the loading picture could not be loaded: no curtain");
        curtain_failed = true;
        return false;
    }
    for (int i = 0; i < 256; i++)
    {
        curtain_pal[4 * i + 0] = RendererPaletteChannel8(pal[3 * i + 2]);
        curtain_pal[4 * i + 1] = RendererPaletteChannel8(pal[3 * i + 1]);
        curtain_pal[4 * i + 2] = RendererPaletteChannel8(pal[3 * i + 0]);
        curtain_pal[4 * i + 3] = 255;
    }
    const size_t cells = (size_t)w * (size_t)h;
    if (RendererGetActiveType() != RENDERER_OPENGL)
    {
        /* one palette for the whole screen: each of the picture's colours becomes the game palette's nearest */
        const unsigned char *game_pal = RendererGetActivePalette();
        if (game_pal == NULL)
            game_pal = engine_palette;
        unsigned char remap[256];
        for (int i = 0; i < 256; i++)
            remap[i] = LbPaletteFindColour(game_pal, pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]);
        curtain_near = (unsigned char *)malloc(cells);
        if (curtain_near == NULL)
        {
            free(raw);
            free(pal);
            curtain_failed = true;
            return false;
        }
        for (size_t i = 0; i < cells; i++)
            curtain_near[i] = remap[raw[i]];
    }
    free(pal);
    curtain_img = raw;
    curtain_w = w;
    curtain_h = h;
    return true;
}

void world_curtain_raise(long min_turns)
{
    const GameTurn now = game.play_gameturn;
    if (!world_curtain_up())
    {
        if (!curtain_load())
            return;
        curtain_on = true;
        curtain_since = now;
        curtain_since_clock = LbTimerClock();
        curtain_min_until = now;
        SYNCLOG("World curtain: the loading screen goes up");
    }
    if ((GameTurnDelta)(now + (GameTurn)max(min_turns, 0L) - curtain_min_until) > 0)
        curtain_min_until = now + (GameTurn)max(min_turns, 0L);
}

void world_curtain_hold(TbBool hold)
{
    curtain_held = hold;
    curtain_held_turn = game.play_gameturn;
}

TbBool world_curtain_up(void)
{
    if (!curtain_on)
        return false;
    const GameTurn now = game.play_gameturn;
    const GameTurnDelta age = (GameTurnDelta)(now - curtain_since);
    /* a hold counts for the turn it was asked in and the next (the asking is done once a turn; frames come oftener) */
    const TbBool held = curtain_held && ((GameTurnDelta)(now - curtain_held_turn) >= 0) &&
        ((GameTurnDelta)(now - curtain_held_turn) <= 1);
    /* its longest, in turns and by the clock: the turns stand still in a paused game (another player's pause, on a
       server), and nothing can be clicked behind the curtain */
    const TbBool too_long = (age < 0) || (age > WORLD_CURTAIN_MAX_TURNS) ||
        (LbTimerClock() - curtain_since_clock > WORLD_CURTAIN_MAX_MS);
    if (!too_long && (held || ((GameTurnDelta)(now - curtain_min_until) < 0)))
        return true;
    SYNCLOG("World curtain: the loading screen comes down after %ld turns%s", (long)age, too_long ? " (its longest)" : "");
    curtain_on = false;
    curtain_held = false;
    curtain_free();
    return false;
}

void world_curtain_draw(void)
{
    if (curtain_img == NULL)
        return;
    set_pointer_graphic_none();
    /* whole, in the middle, as large as the screen takes (the level loading screen's own layout) */
    const int sw = MyScreenWidth, sh = MyScreenHeight;
    int dw = sw, dh = (int)((int64_t)sw * curtain_h / curtain_w);
    if (dh > sh)
    {
        dh = sh;
        dw = (int)((int64_t)sh * curtain_w / curtain_h);
    }
    struct RendererPresentImageDesc d = {0};
    d.format = PRESENT_FORMAT_INDEXED8;
    d.kind = PRESENT_KIND_OPAQUE; /* fills its place and clears the rest of the screen */
    if (curtain_near != NULL)
    {
        d.palette = PRESENT_PALETTE_GAME;
        d.src = curtain_near;
    } else
    {
        d.palette = PRESENT_PALETTE_EMBEDDED;
        d.embedded_palette = curtain_pal;
        d.src = curtain_img;
    }
    d.dst_x = (sw - dw) / 2;
    d.dst_y = (sh - dh) / 2;
    d.dst_w = dw;
    d.dst_h = dh;
    d.src_pitch = curtain_w;
    d.src_w = curtain_w;
    d.src_h = curtain_h;
    RendererPresentImage(&d);
}

void world_curtain_reset(void)
{
    curtain_on = false;
    curtain_held = false;
    curtain_free();
}

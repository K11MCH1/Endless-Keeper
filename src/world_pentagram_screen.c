/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_screen.c
 *     Endless worlds: The Pentagram's own screen (the Keeper panel's Upgrades opens it).
 * @par Purpose:
 *     The user's design (2026-11-27, ideas\PENTAGRAM-2026-11-27.md): a full-screen web over a pentagram carved into
 *     the dungeon floor (lava channels, a turning ring of runes, braziers at the star's points, the beating Heart in
 *     the middle), in the Molten look. The game keeps running behind it, its sounds muted. Drag to pan, wheel to zoom;
 *     a click marks a point as pending and Confirm sends them (world_pentagram_send_learn); right-click takes a pending
 *     point back. The top bar: Return, the points, Search, Reset Tree, zoom. The Foci on the left fly to their Focus.
 *     Small screens (or touch): the Foci list folds, and a node's card docks at the foot with a Learn button.
 *     Painted with ui_paint.c on a canvas the size of the screen (true colour on OpenGL/Vulkan, the palette version in
 *     software): the web as a base layer repainted when the view or the web changes, the hover, the card and the
 *     Heart as parts.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_pentagram.h"
#include "world_pentagram_data.h"
#include "world_pentagram_screen.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "bflib_mouse.h"
#include "bflib_keybrd.h"
#include "bflib_sndlib.h"
#include "bflib_vidraw.h"
#include "bflib_planar.h"
#include "kjm_input.h"
#include "game_legacy.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "vidmode.h"
#include "net_state.h"
#include "ui_paint.h"
#include "ui_molten.h"
#include "world_stream.h"
#include "world_progress.h"
#include "world_daynight.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "bflib_video.h"
#include "front_simple.h"
#include "world_keeper_panel.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WPS_HEART_R 78
static const float wps_rad[5] = {78.0f, 14.0f, 31.0f, 40.0f, 45.0f}; /* by WpgKind */

static struct {
    TbBool open;
    float s, ox, oy;                 /* screen = world * s + (ox, oy) */
    TbBool fitted;
    int hover;                       /* the node under the pointer (-1: none) */
    int selected;                    /* the compact layout's chosen node */
    uint8_t pend[WPG_NODES];         /* pending points by node */
    uint8_t order[400];              /* the pending points in the order they were asked for */
    int norder;
    TbBool dragging, dragged;
    int grab_mx, grab_my;
    float grab_ox, grab_oy;
    char search[32];
    TbBool search_on;
    GameTurn reset_armed;
    SoundVolume volume_kept;
    unsigned long crack_started;     /* the Heart's cracking (ms; 0: none) */
    TbBool compact;
    float u;                          /* the chrome's scale */
    int sw, sh;
    float zt, zax, zay;               /* a zoom easing in: the scale it goes to, about the screen point (zt 0: none) */
} wps;

static struct UmHost wps_host;

/******************************************************************************/
/* The web with the pending points on top (the rules as world_pentagram.c has them, on a local copy) */

static int wps_pts(int node)
{
    return world_pentagram_points(my_player_number, node) + wps.pend[node];
}

static TbBool wps_has(int node)
{
    return (node == WPG_HEART_NODE) || (wps_pts(node) > 0);
}

static int wps_spent(void)
{
    int n = 0;
    for (int i = 0; i < WPG_NODES; i++)
        n += wps_pts(i);
    return n;
}

static int wps_pending(void)
{
    return wps.norder;
}

static int wps_free(void)
{
    return world_pentagram_total(my_player_number) - wps_spent();
}

static int wps_focus_pts(int focus)
{
    int n = 0;
    for (int i = 0; i < WPG_NODES; i++)
        if ((wpg_nodes[i].focus == focus) && (wpg_nodes[i].kind != WpgK_Key))
            n += wps_pts(i);
    return n;
}

static int wps_group_pick(int group)
{
    for (int i = 0; i < WPG_NODES; i++)
        if ((wpg_nodes[i].kind == WpgK_Key) && (wpg_nodes[i].group == group) && (wps_pts(i) > 0))
            return i;
    return -1;
}

static TbBool wps_joined(int node)
{
    for (int e = 0; e < WPG_EDGES; e++)
    {
        const int a = wpg_edges[e][0], b = wpg_edges[e][1];
        if (((a == node) && wps_has(b)) || ((b == node) && wps_has(a)))
            return true;
    }
    return false;
}

static TbBool wps_gate_open(const struct WpgNode *n)
{
    if (n->duo[0] != 255)
        return (wps_focus_pts(n->duo[0]) >= WPG_DUO_GATE_POINTS) && (wps_focus_pts(n->duo[1]) >= WPG_DUO_GATE_POINTS);
    return wps_focus_pts(n->group) >= WPG_GATE_POINTS;
}

static TbBool wps_all_learnt(void)
{
    for (int i = 1; i < WPG_NODES; i++)
    {
        const struct WpgNode *n = &wpg_nodes[i];
        if ((n->kind == WpgK_Key) ? (wps_group_pick(n->group) < 0) : (wps_pts(i) < n->max))
            return false;
    }
    return true;
}

static TbBool wps_can_learn(int node)
{
    if ((node < 0) || (node >= WPG_NODES) || (wps_free() <= 0))
        return false;
    const struct WpgNode *n = &wpg_nodes[node];
    if (node == WPG_HEART_NODE)
        return (wps_pts(node) == 0) && wps_all_learnt();
    if (n->kind == WpgK_Key)
        return (wps_group_pick(n->group) < 0) && wps_gate_open(n) && wps_joined(node);
    const int p = wps_pts(node);
    return (p < n->max) && ((p > 0) || wps_joined(node));
}

static void wps_learn(int node)
{
    if (!wps_can_learn(node) || (wps.norder >= (int)sizeof(wps.order)))
        return;
    wps.pend[node]++;
    wps.order[wps.norder++] = (uint8_t)node;
}

/** Takes the last pending point of a node back, unless something pending hangs on it. */
static void wps_unpend(int node)
{
    if ((node < 0) || (node >= WPG_NODES) || (wps.pend[node] == 0))
        return;
    wps.pend[node]--;
    /* still joined, and every keystone's Focus still has its points? */
    TbBool ok = true;
    for (int i = 1; (i < WPG_NODES) && ok; i++)
    {
        if (wps.pend[i] == 0)
            continue;
        const struct WpgNode *n = &wpg_nodes[i];
        if ((n->kind == WpgK_Key) && !wps_gate_open(n))
            ok = false;
        else if ((world_pentagram_points(my_player_number, i) == 0) && !wps_joined(i))
            ok = false;
    }
    if (wps.pend[WPG_HEART_NODE] && !wps_all_learnt())
        ok = false;
    if (!ok)
    {
        wps.pend[node]++;
        return;
    }
    for (int k = wps.norder - 1; k >= 0; k--)
        if (wps.order[k] == node)
        {
            memmove(wps.order + k, wps.order + k + 1, (size_t)(wps.norder - k - 1));
            wps.norder--;
            break;
        }
}

static void wps_confirm(void)
{
    if (wps.norder <= 0)
        return;
    if (wps.pend[WPG_HEART_NODE])
        wps.crack_started = LbTimerClock(); /* the Heart cracks */
    world_pentagram_send_learn(wps.order, wps.norder);
    memset(wps.pend, 0, sizeof(wps.pend));
    wps.norder = 0;
}

/** Test hook (KFX_WORLD_PENTAGRAMTEST mode "netall"): every point the keeper may spend, chosen on the screen as a
 *  player would (pass after pass over the web, a keystone pair's first or, reverse, its second; the Heart last), then
 *  confirmed: the packets go as from the Confirm button. Returns the points chosen. */
int world_pentagram_screen_test_all(TbBool reverse)
{
    world_pentagram_screen_show();
    for (int pass = 0; pass < 64; pass++)
    {
        const int before = wps.norder;
        for (int k = 1; k < WPG_NODES; k++)
        {
            const int i = reverse ? (WPG_NODES - k) : k;
            while (wps_can_learn(i))
                wps_learn(i);
        }
        if (wps_can_learn(WPG_HEART_NODE))
            wps_learn(WPG_HEART_NODE);
        if (wps.norder == before)
            break;
    }
    const int n = wps.norder;
    wps_confirm();
    return n;
}

static void wps_cancel(void)
{
    memset(wps.pend, 0, sizeof(wps.pend));
    wps.norder = 0;
}

/******************************************************************************/
/* Opening and closing */

TbBool world_pentagram_screen_open(void)
{
    return wps.open && world_mode_active() && !netstate_dedicated();
}

void world_pentagram_screen_show(void)
{
    if (!world_mode_active() || netstate_dedicated() || wps.open)
        return;
    memset(&wps.pend, 0, sizeof(wps.pend));
    wps.norder = 0;
    wps.open = true;
    wps.fitted = false;
    wps.hover = wps.selected = -1;
    wps.search[0] = 0;
    wps.search_on = false;
    wps.reset_armed = 0;
    /* the game's sounds go quiet while it is open (notifications still speak) */
    wps.volume_kept = GetCurrentSoundMasterVolume();
    SetSoundMasterVolume(0);
    SYNCLOG("World: the Pentagram opens (%d of %d points spent)", world_pentagram_spent(my_player_number),
        world_pentagram_total(my_player_number));
}

void world_pentagram_screen_close(void)
{
    if (!wps.open)
        return;
    wps.open = false;
    SetSoundMasterVolume(wps.volume_kept);
    um_host_forget(&wps_host);
    SYNCLOG("World: the Pentagram closes");
}

/******************************************************************************/
/* The view */

static void wps_layout(void)
{
    wps.sw = (int)MyScreenWidth;
    wps.sh = (int)MyScreenHeight;
    wps.u = max(0.62f, min(1.6f, min((float)wps.sw / 1920.0f, (float)wps.sh / 1080.0f)));
    wps.compact = (wps.sw < 1100) || (wps.sh < 640);
}

static float wps_top(void)
{
    return 84.0f * wps.u;
}

static void wps_fit(void)
{
    const float top = wps_top(), bottom = wps.compact ? 10.0f : 70.0f * wps.u;
    const float w = (float)wps.sw, h = (float)wps.sh - top - bottom;
    wps.s = min(w, h) / 2900.0f;
    wps.ox = w / 2.0f;
    wps.oy = top + h / 2.0f;
    wps.fitted = true;
    wps.zt = 0.0f;
}

static float wps_min_s(void)
{
    const float top = wps_top();
    return min((float)wps.sw, (float)wps.sh - top) / 2900.0f * 0.8f;
}

static unsigned long wps_zoom_ms;
/** A wheel step: the zoom eases to its new scale over about a tenth of a second (wps_animate, each frame). */
static void wps_zoom_at(float f, float mx, float my)
{
    wps_zoom_ms = LbTimerClock();
    const float from = (wps.zt > 0.0f) ? wps.zt : wps.s;
    wps.zt = max(wps_min_s(), min(wps_min_s() * 7.0f, from * f));
    wps.zax = mx;
    wps.zay = my;
}

void world_pentagram_screen_pinch(float log_scale, long screen_x, long screen_y)
{
    if (!world_pentagram_screen_open() || !isfinite(log_scale))
        return;
    /* (straight, no easing: the web follows the fingers; the sharp repaint waits for them to rest, as for the wheel) */
    wps_zoom_ms = LbTimerClock();
    wps.zt = 0.0f;
    const float ns = max(wps_min_s(), min(wps_min_s() * 7.0f, wps.s * expf(log_scale)));
    wps.ox = (float)screen_x - ((float)screen_x - wps.ox) * ns / wps.s;
    wps.oy = (float)screen_y - ((float)screen_y - wps.oy) * ns / wps.s;
    wps.s = ns;
}

static void wps_fly_to(float wx, float wy)
{
    wps.zt = 0.0f;
    wps.s = wps_min_s() * 2.4f;
    wps.ox = (float)wps.sw / 2.0f - wx * wps.s;
    wps.oy = wps_top() + ((float)wps.sh - wps_top()) / 2.0f - wy * wps.s;
}

#define SX(x) ((x) * wps.s + wps.ox)
#define SY(y) ((y) * wps.s + wps.oy)

/** The node under a screen point (-1: none). */
static int wps_node_at(int mx, int my)
{
    if ((float)my < wps_top())
        return -1;
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < WPG_NODES; i++)
    {
        const struct WpgNode *n = &wpg_nodes[i];
        const float dx = (float)mx - SX(n->x), dy = (float)my - SY(n->y);
        const float r = (wps_rad[n->kind] + 6.0f) * wps.s;
        const float d = dx * dx + dy * dy;
        if ((d <= r * r) && (d < best_d))
        {
            best = i;
            best_d = d;
        }
    }
    return best;
}

/******************************************************************************/
/* Painting */

#define C_LAVA     UIC(255, 140, 50, 255)
#define C_LAVA_DIM UIC(226, 64, 26, 255)
#define C_GROOVE   UIC(8, 4, 3, 255)
#define C_LIT      UIC(255, 178, 74, 255)
#define C_CAN      UIC(255, 190, 110, 180)
#define C_LOCKED   UIC(255, 214, 170, 77)
#define C_PEND     UIC(255, 238, 180, 255)

/** A straight band from (x0, y0) to (x1, y1), `w` pixels wide. */
static inline void wps_blend(unsigned char *p, UiColour col, float cov);

/** A straight band (a thick line, square ends), row by row: only the pixels it covers and their edge (the canvas's
 *  path fill went over the band's whole box: the star's long channels cost a pass each). */
static void wps_band(struct UiCanvas *c, float x0, float y0, float x1, float y1, float w, UiColour col)
{
    const float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if ((l < 0.5f) || (c->px == NULL))
        return;
    const float ux = dx / l, uy = dy / l, hw = w * 0.5f;
    /* the band's corners, and the rows it spans */
    const float nx = -uy * hw, ny = ux * hw;
    const float qx[4] = {x0 + nx, x1 + nx, x1 - nx, x0 - nx}, qy[4] = {y0 + ny, y1 + ny, y1 - ny, y0 - ny};
    float ymin = qy[0], ymax = qy[0];
    for (int k = 1; k < 4; k++) { ymin = min(ymin, qy[k]); ymax = max(ymax, qy[k]); }
    const int ya = max(c->clip_y0, (int)floorf(ymin) - 1), yb = min(c->clip_y1, (int)ceilf(ymax) + 1);
    for (int y = ya; y < yb; y++)
    {
        /* the row's x range: where the quad's edges cross the row's top and bottom, one pixel more each side */
        float xa = 1e30f, xb = -1e30f;
        for (int k = 0; k < 4; k++)
        {
            const int k2 = (k + 1) & 3;
            const float ay = qy[k], by = qy[k2];
            for (int e = 0; e < 2; e++)
            {
                const float yy = (float)y + (float)e;
                if (((ay <= yy) && (by >= yy)) || ((by <= yy) && (ay >= yy)))
                {
                    const float tt = (fabsf(by - ay) < 1e-6f) ? 0.0f : (yy - ay) / (by - ay);
                    const float xx = qx[k] + (qx[k2] - qx[k]) * tt;
                    xa = min(xa, xx); xb = max(xb, xx);
                    if (fabsf(by - ay) < 1e-6f) { xa = min(xa, qx[k2]); xb = max(xb, qx[k2]); }
                }
            }
            if ((ay >= (float)y) && (ay <= (float)y + 1.0f)) { xa = min(xa, qx[k]); xb = max(xb, qx[k]); }
        }
        if (xb < xa)
            continue;
        const int ia = max(c->clip_x0, (int)floorf(xa) - 1), ib = min(c->clip_x1, (int)ceilf(xb) + 1);
        const float py = (float)y + 0.5f - y0;
        unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)max(ia, 0)) * 4;
        for (int x = ia; x < ib; x++, p += 4)
        {
            const float px = (float)x + 0.5f - x0;
            const float along = px * ux + py * uy, across = fabsf(-px * uy + py * ux);
            const float cov = min(1.0f, max(0.0f, hw + 0.5f - across)) * min(1.0f, max(0.0f, min(along + 0.5f, l - along + 0.5f)));
            if (cov > 0.0f)
                wps_blend(p, col, cov);
        }
    }
}

static inline void wps_blend(unsigned char *p, UiColour col, float cov);

static UiColour wps_mix(UiColour a, UiColour b, float k)
{
    UiColour out = 0;
    for (int sh = 0; sh < 32; sh += 8)
    {
        const float va = (float)((a >> sh) & 0xFF), vb = (float)((b >> sh) & 0xFF);
        out |= (UiColour)((unsigned)(va + (vb - va) * k + 0.5f) & 0xFF) << sh;
    }
    return out;
}

/** A disc, its colour going from c0 at the top to c1 at the bottom, row by row: the inside of an opaque one is written
 *  straight, only its edge blended (the canvas's rounded-box fill blended every pixel: the floor alone cost a pass). */
static void wps_circle(struct UiCanvas *c, float cx, float cy, float r, UiColour c0, UiColour c1)
{
    if ((c->px == NULL) || (r <= 0.0f))
        return;
    const float ro = r + 1.0f, ri = max(0.0f, r - 1.0f);
    const int y0 = max(c->clip_y0, (int)floorf(cy - ro)), y1 = min(c->clip_y1, (int)ceilf(cy + ro));
    for (int y = y0; y < y1; y++)
    {
        const float dy = (float)y + 0.5f - cy, ady = fabsf(dy);
        if (ady >= ro)
            continue;
        const UiColour col = wps_mix(c0, c1, min(1.0f, max(0.0f, (dy + r) / (2.0f * r))));
        const TbBool opaque = ((col >> 24) == 0xFF);
        const uint32_t word = (uint32_t)((col >> 16) & 0xFF) | ((uint32_t)((col >> 8) & 0xFF) << 8) |
            ((uint32_t)(col & 0xFF) << 16) | 0xFF000000u;
        const float xo = sqrtf(ro * ro - dy * dy), xi = (ady < ri) ? sqrtf(ri * ri - dy * dy) : 0.0f;
        const int xa = max(c->clip_x0, (int)floorf(cx - xo)), xb = min(c->clip_x1, (int)ceilf(cx + xo));
        const int ia = (xi > 0.0f) ? (int)ceilf(cx - xi) : xb, ib = (xi > 0.0f) ? (int)floorf(cx + xi) : xb;
        unsigned char *row = c->px + (size_t)y * (size_t)c->w * 4;
        for (int x = xa; x < xb; x++)
        {
            unsigned char *p = row + (size_t)x * 4;
            if ((x >= ia) && (x < ib))
            {
                /* the inside */
                if (opaque)
                {
                    const int n = min(ib, xb) - x;
                    uint32_t *q = (uint32_t *)p;
                    for (int k = 0; k < n; k++)
                        q[k] = word;
                    x += n - 1;
                } else
                    wps_blend(p, col, 1.0f);
                continue;
            }
            const float dx = (float)x + 0.5f - cx;
            const float cov = min(1.0f, max(0.0f, r + 0.5f - sqrtf(dx * dx + dy * dy)));
            if (cov > 0.0f)
                wps_blend(p, col, cov);
        }
    }
}

static inline void wps_blend(unsigned char *p, UiColour col, float cov)
{
    const float a = (float)(col >> 24) * cov;
    if (a <= 0.0f)
        return;
    const float k = a / 255.0f, ia = 1.0f - k;
    p[0] = (unsigned char)((float)((col >> 16) & 0xFF) * k + (float)p[0] * ia + 0.5f);
    p[1] = (unsigned char)((float)((col >> 8) & 0xFF) * k + (float)p[1] * ia + 0.5f);
    p[2] = (unsigned char)((float)(col & 0xFF) * k + (float)p[2] * ia + 0.5f);
    p[3] = (unsigned char)(a + (float)p[3] * ia + 0.5f);
}

/** A ring of radius r and half-width hw (soft: a glow fading out from r), touching only the pixels near it, row by row
 *  (the canvas's own stroke goes over every pixel of the ring's square: the screen's big rings cost a whole pass each). */
static void wps_annulus(struct UiCanvas *c, float cx, float cy, float r, float hw, UiColour col, TbBool soft)
{
    if ((c->px == NULL) || (hw <= 0.0f) || (r <= 0.0f))
        return;
    const float ro = r + hw + 1.0f, ri = max(0.0f, r - hw - 1.0f);
    const int y0 = max(c->clip_y0, (int)floorf(cy - ro)), y1 = min(c->clip_y1, (int)ceilf(cy + ro));
    for (int y = y0; y < y1; y++)
    {
        const float dy = (float)y + 0.5f - cy, ady = fabsf(dy);
        if (ady >= ro)
            continue;
        const float xo = sqrtf(ro * ro - dy * dy), xi = (ady < ri) ? sqrtf(ri * ri - dy * dy) : 0.0f;
        for (int side = 0; side < 2; side++)
        {
            int xa, xb;
            if (xi <= 0.0f)
            {
                if (side == 1)
                    break;
                xa = (int)floorf(cx - xo); xb = (int)ceilf(cx + xo);
            } else if (side == 0)
            {
                xa = (int)floorf(cx - xo); xb = (int)ceilf(cx - xi);
            } else
            {
                xa = (int)floorf(cx + xi); xb = (int)ceilf(cx + xo);
            }
            xa = max(xa, c->clip_x0);
            xb = min(xb, c->clip_x1);
            unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)max(xa, 0)) * 4;
            for (int x = xa; x < xb; x++, p += 4)
            {
                const float dx = (float)x + 0.5f - cx;
                const float e = fabsf(sqrtf(dx * dx + dy * dy) - r);
                float cov;
                if (soft)
                {
                    const float f = max(0.0f, 1.0f - e / hw);
                    cov = f * f;
                } else
                    cov = min(1.0f, max(0.0f, hw + 0.5f - e));
                if (cov > 0.0f)
                    wps_blend(p, col, cov);
            }
        }
    }
}

static void wps_ring(struct UiCanvas *c, float cx, float cy, float r, float w, UiColour col)
{
    wps_annulus(c, cx, cy, r, w * 0.5f, col, false);
}

static void wps_octagon(struct UiCanvas *c, float cx, float cy, float r, UiColour col)
{
    float xy[16];
    for (int k = 0; k < 8; k++)
    {
        const float a = (float)(M_PI / 8.0 + k * M_PI / 4.0);
        xy[2 * k] = cx + r * cosf(a);
        xy[2 * k + 1] = cy + r * sinf(a);
    }
    const int cnt = 8;
    ui_fill_path(c, xy, &cnt, 1, col);
}

/** An icon fitted in a box of `box` pixels round (cx, cy), greyed when `grey`. */
static void wps_icon(struct UiCanvas *c, int icon, float cx, float cy, float box, TbBool grey)
{
    int w = 0, h = 0;
    const unsigned char *px = world_pentagram_icon(icon, &w, &h);
    if ((px == NULL) || (w <= 0) || (h <= 0))
        return;
    const float k = min(box / (float)w, box / (float)h);
    const int dw = max(1, (int)(w * k + 0.5f)), dh = max(1, (int)(h * k + 0.5f));
    if (grey)
        ui_image_gain(c, (int)(cx - dw / 2.0f), (int)(cy - dh / 2.0f), dw, dh, px, w, h, 0.45f);
    else
        ui_image(c, (int)(cx - dw / 2.0f), (int)(cy - dh / 2.0f), dw, dh, px, w, h, 0.0f);
}

/* the runes: little glyphs of straight strokes (unit box, y down), drawn upright to the ring as it turns */
static const float wps_rune_segs[12][8] = {
    {0.5f, 0, 0.5f, 1, 0.5f, 0.2f, 0.9f, 0.45f}, {0.2f, 0, 0.2f, 1, 0.2f, 0, 0.8f, 0.5f},
    {0.5f, 0, 0.5f, 1, 0.1f, 0.3f, 0.9f, 0.3f}, {0.2f, 0, 0.2f, 1, 0.8f, 0, 0.8f, 1},
    {0.5f, 0, 0.5f, 1, 0.5f, 0.5f, 0.9f, 0.1f}, {0.2f, 0, 0.8f, 1, 0.8f, 0, 0.2f, 1},
    {0.5f, 0, 0.5f, 1, 0.2f, 0.6f, 0.8f, 0.6f}, {0.3f, 0, 0.3f, 1, 0.3f, 0.5f, 0.8f, 0.9f},
    {0.5f, 0, 0.5f, 1, 0.1f, 0.1f, 0.5f, 0.5f}, {0.2f, 0.1f, 0.8f, 0.5f, 0.8f, 0.5f, 0.2f, 0.9f},
    {0.5f, 0, 0.5f, 1, 0.5f, 0.0f, 0.9f, 0.3f}, {0.2f, 0, 0.2f, 1, 0.2f, 0.5f, 0.8f, 0.5f},
};

static void wps_runes(struct UiCanvas *c, float cx, float cy, float r, float size, float turn, int count, UiColour col)
{
    for (int k = 0; k < count; k++)
    {
        const float a = turn + (float)(2.0 * M_PI * k / count);
        const float ux = cosf(a), uy = sinf(a);      /* outward */
        const float vx = -uy, vy = ux;              /* along the ring */
        const float bx = cx + ux * r, by = cy + uy * r;
        const float *g = wps_rune_segs[(k * 7 + 3) % 12];
        for (int s = 0; s < 2; s++)
        {
            /* glyph x along the ring, glyph y from outside to inside */
            const float gx0 = (g[4 * s] - 0.5f) * size * 0.6f, gy0 = (0.5f - g[4 * s + 1]) * size;
            const float gx1 = (g[4 * s + 2] - 0.5f) * size * 0.6f, gy1 = (0.5f - g[4 * s + 3]) * size;
            wps_band(c, bx + vx * gx0 + ux * gy0, by + vy * gx0 + uy * gy0, bx + vx * gx1 + ux * gy1, by + vy * gx1 + uy * gy1,
                max(1.2f, size * 0.12f), col);
        }
    }
}

/* the screen's ground (one colour: the cached web is laid on it at any place; the palette's nearest colour, so that the
   game UI's box under the web on OpenGL/Vulkan matches it exactly) */
static UiColour wps_bg_col = UIC(17, 11, 8, 255);
static TbPixel wps_bg_index;
#define WPS_BG wps_bg_col

/** The runes, turning slowly clockwise (one turn in 4 minutes), and the Heart's own ring the other way: drawn over the
 *  cached web on every repaint of the screen (they turn a step every 2 s). */
static void wps_paint_runes(struct UiCanvas *c)
{
    const float s = wps.s, hx = SX(0), hy = SY(0);
    const unsigned long now = (LbTimerClock() / 2000UL) * 2000UL;
    /* each 2 s step well under half the runes' spacing, so that the eye follows them the way they go (2026-11-28: the
       outer ring's 3-degree step against its 3.75-degree spacing read as a turn backwards, the inner ring's way): the
       outer ring 1 degree clockwise a step (96 runes, 3.75 degrees apart), the inner 4 degrees counter-clockwise (28,
       12.9 degrees apart) */
    const float t = (float)(now % 720000UL) / 720000.0f * (float)(2.0 * M_PI);
    const float t2 = (float)(now % 180000UL) / 180000.0f * (float)(2.0 * M_PI);
    wps_runes(c, hx, hy, (WPG_RING + 35) * s, max(6.0f, 30.0f * s), t, 96, UIC(255, 110, 50, 120));
    wps_runes(c, hx, hy, 118.0f * s, max(4.0f, 19.0f * s), -t2, 28, UIC(255, 90, 40, 140));
}

static void wps_fill_bg(struct UiCanvas *c)
{
    if (c->px == NULL)
        return;
    const uint32_t word = (uint32_t)((WPS_BG >> 16) & 0xFF) | ((uint32_t)((WPS_BG >> 8) & 0xFF) << 8) |
        ((uint32_t)(WPS_BG & 0xFF) << 16) | 0xFF000000u;
    uint32_t *q = (uint32_t *)c->px;
    const size_t n = (size_t)c->w * (size_t)c->h;
    for (size_t k = 0; k < n; k++)
        q[k] = word;
}

static void wps_paint_background(struct UiCanvas *c)
{
    const float s = wps.s, hx = SX(0), hy = SY(0);
    wps_fill_bg(c);
    /* the floor inside the ring: a little warmer than round it */
    wps_circle(c, hx, hy, (WPG_RING + 70) * s, UIC(30, 19, 14, 255), UIC(18, 11, 8, 255));
    /* the circles: dark grooves with lava */
    wps_ring(c, hx, hy, WPG_RING * s, max(2.0f, 14.0f * s), C_GROOVE);
    wps_ring(c, hx, hy, (WPG_RING + 70) * s, max(2.0f, 10.0f * s), C_GROOVE);
    wps_ring(c, hx, hy, 152.0f * s, max(2.0f, 10.0f * s), C_GROOVE);
    wps_ring(c, hx, hy, 232.0f * s, max(1.0f, 6.0f * s), C_GROOVE);
    if (!um_host_flat())
        wps_annulus(c, hx, hy, WPG_RING * s, max(3.0f, 10.0f * s), UIC(255, 90, 30, 60), true);
    wps_ring(c, hx, hy, WPG_RING * s, max(1.5f, 3.5f * s), C_LAVA);
    wps_ring(c, hx, hy, 152.0f * s, max(1.2f, 3.0f * s), C_LAVA);
    /* the star: five lava channels in grooves */
    float tx[5], ty[5];
    for (int k = 0; k < 5; k++)
    {
        const double a = (-90.0 + 72.0 * k) * M_PI / 180.0;
        tx[k] = SX((float)(WPG_RING * cos(a)));
        ty[k] = SY((float)(WPG_RING * sin(a)));
    }
    static const int order[6] = {0, 2, 4, 1, 3, 0};
    for (int pass = 0; pass < 3; pass++)
        for (int k = 0; k < 5; k++)
        {
            const int a = order[k], b = order[k + 1];
            const float w = (pass == 0) ? 40.0f : (pass == 1) ? 26.0f : 9.0f;
            const UiColour col = (pass == 0) ? C_GROOVE : (pass == 1) ? UIC(36, 16, 9, 255) : C_LAVA_DIM;
            wps_band(c, tx[a], ty[a], tx[b], ty[b], max(1.5f, w * s), col);
            if (pass == 2)
                wps_band(c, tx[a], ty[a], tx[b], ty[b], max(1.0f, 4.0f * s), C_LAVA);
        }
    /* braziers at the points */
    for (int k = 0; k < 5; k++)
    {
        if (!um_host_flat())
            ui_glow_rrect(c, tx[k] - 30 * s, ty[k] - 30 * s, tx[k] + 30 * s, ty[k] + 30 * s, 30 * s, 90 * s, UIC(255, 140, 50, 70));
        ui_fill_rrect(c, tx[k] - 30 * s, ty[k] + 4 * s, tx[k] + 30 * s, ty[k] + 26 * s, 4 * s, UIC(42, 26, 18, 255), UIC(20, 12, 8, 255), false);
        wps_circle(c, tx[k], ty[k] - 14 * s, 14 * s, UIC(255, 210, 120, 230), UIC(255, 120, 40, 230));
    }
    /* the Foci's names round the ring */
    for (int f = 0; f < WPG_FOCI; f++)
    {
        const int got = wps_focus_pts(f) + ((wps_group_pick(f) >= 0) ? 1 : 0);
        const float px = max(10.0f, 36.0f * s);
        char up[32];
        snprintf(up, sizeof(up), "%s", wpg_foci_names[f]);
        for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
        ui_text_box(c, UiF_Head, px, SX(wpg_labels[f][0]) - 400, SY(wpg_labels[f][1]) - px, 800, px, 1,
            got ? UIC(255, 214, 140, 255) : UIC(222, 178, 88, 140), px * 0.2f, true, up);
        char num[16];
        snprintf(num, sizeof(num), "%d", got);
        ui_text_box(c, UiF_Num, px * 0.85f, SX(wpg_labels[f][0]) - 100, SY(wpg_labels[f][1]) + px * 0.2f, 200, px, 1,
            got ? C_LIT : UIC(222, 178, 88, 110), 0, false, num);
    }
}

/** A link's state: 2 both ends held (molten), 1 one end (a way open), 0 none. */
static void wps_paint_web(struct UiCanvas *c)
{
    const float s = wps.s;
    for (int e = 0; e < WPG_EDGES; e++)
    {
        const struct WpgNode *a = &wpg_nodes[wpg_edges[e][0]], *b = &wpg_nodes[wpg_edges[e][1]];
        const TbBool ha = wps_has(wpg_edges[e][0]), hb = wps_has(wpg_edges[e][1]);
        const float dx = (float)(b->x - a->x), dy = (float)(b->y - a->y), l = sqrtf(dx * dx + dy * dy);
        const float ra = wps_rad[a->kind] + 4.0f, rb = wps_rad[b->kind] + 4.0f;
        if (l <= ra + rb)
            continue;
        const float x0 = SX(a->x + dx / l * ra), y0 = SY(a->y + dy / l * ra), x1 = SX(b->x - dx / l * rb), y1 = SY(b->y - dy / l * rb);
        if (ha && hb)
        {
            if (!um_host_flat())
                wps_band(c, x0, y0, x1, y1, max(3.0f, 14.0f * s), UIC(255, 120, 40, 60));
            wps_band(c, x0, y0, x1, y1, max(2.0f, 6.0f * s), C_LIT);
        }
        else if (ha || hb)
            wps_band(c, x0, y0, x1, y1, max(1.5f, 3.5f * s), UIC(255, 190, 110, 110));
        else
            wps_band(c, x0, y0, x1, y1, max(1.0f, 3.0f * s), UIC(255, 214, 170, 40));
    }
    /* the gates */
    for (int g = 0; g < WPG_GATES; g++)
    {
        const struct WpgGate *gt = &wpg_gates[g];
        char txt[40];
        TbBool ok;
        float w = 84.0f;
        if (gt->duo[0] != 255)
        {
            const int pa = wps_focus_pts(gt->duo[0]), pb = wps_focus_pts(gt->duo[1]);
            ok = (pa >= WPG_DUO_GATE_POINTS) && (pb >= WPG_DUO_GATE_POINTS);
            snprintf(txt, sizeof(txt), ok ? "%d + %d" : "%d+%d / %d+%d", ok ? WPG_DUO_GATE_POINTS : min(pa, WPG_DUO_GATE_POINTS),
                ok ? WPG_DUO_GATE_POINTS : min(pb, WPG_DUO_GATE_POINTS), WPG_DUO_GATE_POINTS, WPG_DUO_GATE_POINTS);
            w = 112.0f;
        } else
        {
            const int fp = wps_focus_pts(gt->group);
            ok = fp >= WPG_GATE_POINTS;
            snprintf(txt, sizeof(txt), ok ? "%d" : "%d / %d", ok ? WPG_GATE_POINTS : fp, WPG_GATE_POINTS);
        }
        const float gx = SX(gt->x), gy = SY(gt->y), hw = w * 0.5f * s, hh = 15.0f * s;
        ui_fill_rrect(c, gx - hw, gy - hh, gx + hw, gy + hh, hh, UIC(18, 11, 8, 255), UIC(18, 11, 8, 255), false);
        ui_stroke_rrect(c, gx - hw, gy - hh, gx + hw, gy + hh, hh, max(1.0f, 2.0f * s), ok ? C_LIT : UIC(255, 214, 170, 70));
        ui_text_box(c, UiF_Num, max(8.0f, 16.0f * s), gx - hw, gy - hh, 2 * hw, 2 * hh, 1, ok ? UIC(255, 208, 138, 255) : UIC(200, 184, 160, 255), 0, false, txt);
    }
    /* the nodes */
    for (int i = 1; i < WPG_NODES; i++)
    {
        const struct WpgNode *n = &wpg_nodes[i];
        const float r = wps_rad[n->kind] * s, cx = SX(n->x), cy = SY(n->y);
        if ((cx < -60) || (cy < -60) || (cx > c->w + 60) || (cy > c->h + 60))
            continue;
        const TbBool has = wps_has(i), can = wps_can_learn(i), pend = (wps.pend[i] > 0) && (world_pentagram_points(my_player_number, i) == 0);
        const TbBool other = (n->kind == WpgK_Key) && (wps_group_pick(n->group) >= 0) && !has;
        const TbBool grey = !has && !can;
        const UiColour ring = pend ? C_PEND : has ? C_LIT : can ? C_CAN : C_LOCKED;
        const float rw = max(1.0f, (has ? 3.5f : can ? 2.5f : 2.0f) * s);
        TbBool found = false;
        if (wps.search[0] != 0)
        {
            char a[64], b[32];
            snprintf(a, sizeof(a), "%s", n->name);
            snprintf(b, sizeof(b), "%s", wps.search);
            for (char *q = a; *q; q++) *q = (char)tolower((unsigned char)*q);
            for (char *q = b; *q; q++) *q = (char)tolower((unsigned char)*q);
            found = (strstr(a, b) != NULL);
        }
        if (has && !pend && !um_host_flat())
            ui_glow_rrect(c, cx - r, cy - r, cx + r, cy + r, r, max(2.0f, 10.0f * s), UIC(255, 140, 40, 70));
        if (n->kind == WpgK_Key)
        {
            wps_octagon(c, cx, cy, r, other ? UIC(255, 214, 170, 30) : ring);
            wps_octagon(c, cx, cy, r - rw, has ? UIC(184, 54, 26, 255) : UIC(30, 21, 16, 255));
            if (n->icon >= 0)
                wps_icon(c, n->icon, cx, cy, 54.0f * s, grey || other);
        }
        else
        {
            wps_circle(c, cx, cy, r, has ? UIC(122, 52, 20, 255) : UIC(74, 52, 40, 255), has ? UIC(22, 11, 7, 255) : UIC(18, 12, 9, 255));
            wps_ring(c, cx, cy, r, rw, ring);
            if (n->kind == WpgK_Minor)
                wps_circle(c, cx, cy, max(1.5f, 5.0f * s), has ? UIC(255, 210, 122, 255) : can ? UIC(255, 190, 110, 128) : UIC(255, 214, 170, 46),
                    has ? UIC(255, 210, 122, 255) : can ? UIC(255, 190, 110, 128) : UIC(255, 214, 170, 46));
            else if (n->icon >= 0)
                wps_icon(c, n->icon, cx, cy, ((n->kind == WpgK_Core) ? 54.0f : 42.0f) * s, grey);
            if (n->kind == WpgK_Core)
                wps_ring(c, cx, cy, r + 8 * s, max(1.0f, 2.0f * s), has ? UIC(255, 200, 110, 190) : UIC(255, 214, 170, 46));
            if (n->max > 1)
            {
                char b[8];
                snprintf(b, sizeof(b), "%d/%d", wps_pts(i), n->max);
                const float bw = 19.0f * s, bh = 9.5f * s, by = cy + r;
                const TbBool p = wps_pts(i) > 0;
                ui_fill_rrect(c, cx - bw, by - bh, cx + bw, by + bh, bh, p ? UIC(122, 46, 16, 255) : UIC(20, 13, 10, 255), p ? UIC(122, 46, 16, 255) : UIC(20, 13, 10, 255), false);
                ui_stroke_rrect(c, cx - bw, by - bh, cx + bw, by + bh, bh, max(1.0f, 1.5f * s), p ? C_LIT : UIC(255, 214, 170, 64));
                ui_text_box(c, UiF_Num, max(7.0f, 14.0f * s), cx - bw, by - bh, 2 * bw, 2 * bh, 1, p ? UIC(255, 233, 200, 255) : UIC(170, 156, 132, 255), 0, false, b);
            }
        }
        if (found)
            wps_ring(c, cx, cy, r + 10 * s, max(1.5f, 3.0f * s), UIC(120, 220, 255, 230));
    }
}

/* the Heart's portrait, smoothly enlarged once (the user: the high definition picture, not pixels) */
static unsigned char *wps_reaper_px;
static int wps_reaper_size;

static const unsigned char *wps_reaper_picture(int size)
{
    if ((wps_reaper_px != NULL) && (wps_reaper_size == size))
        return wps_reaper_px;
    int w = 0, h = 0;
    const unsigned char *src = world_pentagram_icon(wpg_nodes[WPG_HEART_NODE].icon, &w, &h);
    if ((src == NULL) || (size <= 0))
        return NULL;
    free(wps_reaper_px);
    wps_reaper_px = (unsigned char *)malloc((size_t)size * size * 4);
    if (wps_reaper_px == NULL)
        return NULL;
    wps_reaper_size = size;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
        {
            const float fx = ((float)x + 0.5f) * w / size - 0.5f, fy = ((float)y + 0.5f) * h / size - 0.5f;
            const int x0 = max(0, min(w - 1, (int)floorf(fx))), y0 = max(0, min(h - 1, (int)floorf(fy)));
            const int x1 = min(w - 1, x0 + 1), y1 = min(h - 1, y0 + 1);
            const float ax = fx - floorf(fx), ay = fy - floorf(fy);
            for (int ch = 0; ch < 4; ch++)
            {
                const float v = (1 - ay) * ((1 - ax) * src[(y0 * w + x0) * 4 + ch] + ax * src[(y0 * w + x1) * 4 + ch]) +
                    ay * ((1 - ax) * src[(y1 * w + x0) * 4 + ch] + ax * src[(y1 * w + x1) * 4 + ch]);
                wps_reaper_px[((size_t)y * size + x) * 4 + ch] = (unsigned char)max(0.0f, min(255.0f, v + 0.5f));
            }
        }
    return wps_reaper_px;
}

/** The Heart: beating, or once unlocked the Reaper; the crack and the flash between. */
static void wps_paint_heart(struct UiCanvas *c)
{
    const float s = wps.s, hx = SX(0), hy = SY(0), r = WPS_HEART_R * s;
    const TbBool reaper = world_pentagram_points(my_player_number, WPG_HEART_NODE) > 0;
    const unsigned long since = wps.crack_started ? (unsigned long)LbTimerClock() - wps.crack_started : 100000UL;
    if (!um_host_flat())
        ui_glow_rrect(c, hx - r, hy - r, hx + r, hy + r, r, 110 * s, reaper ? UIC(255, 90, 20, 70) : UIC(255, 40, 20, 80));
    if (reaper && (since > 900))
    {
        wps_circle(c, hx, hy, r, UIC(90, 20, 8, 255), UIC(12, 3, 2, 255));
        wps_ring(c, hx, hy, r + 9 * s, max(2.0f, 5.0f * s), C_LAVA);
        const int size = max(8, (int)(2 * (r - 4 * s)));
        const unsigned char *pic = wps_reaper_picture(size);
        if (pic != NULL)
            ui_image(c, (int)(hx - size / 2.0f), (int)(hy - size / 2.0f), size, size, pic, size, size, size / 2.0f);
        wps_ring(c, hx, hy, r - 4 * s, max(1.0f, 2.0f * s), UIC(255, 200, 120, 150));
    } else
    {
        /* the beat (the launcher's heart rhythm) */
        const float ph = (float)(LbTimerClock() % 1100UL) / 1100.0f;
        const float k = (ph < 0.12f) ? 1.0f + 0.07f * ph / 0.12f : (ph < 0.24f) ? 1.07f - 0.08f * (ph - 0.12f) / 0.12f :
            (ph < 0.36f) ? 0.99f + 0.05f * (ph - 0.24f) / 0.12f : 1.04f - 0.04f * min(1.0f, (ph - 0.36f) / 0.3f);
        const float rr = r * k;
        wps_circle(c, hx, hy, rr, UIC(255, 120, 90, 255), UIC(70, 4, 4, 255));
        wps_circle(c, hx - 16 * s, hy - 24 * s, 16 * s, UIC(255, 220, 200, 90), UIC(255, 220, 200, 0));
        wps_ring(c, hx, hy, rr, max(1.0f, 3.0f * s), UIC(255, 200, 150, 140));
        if (wps_can_learn(WPG_HEART_NODE) || wps.pend[WPG_HEART_NODE])
            wps_ring(c, hx, hy, r + 8 * s, max(1.5f, 3.0f * s), wps.pend[WPG_HEART_NODE] ? C_PEND : C_CAN);
        if (since <= 1900)
        {
            /* the crack: bright lines out from the middle, then the flash */
            unsigned int seed = 7;
            const float grow = min(1.0f, since / 700.0f);
            for (int k2 = 0; k2 < 8; k2++)
            {
                float ang = (float)(k2 * 45) * (float)M_PI / 180.0f, rad = 0, px = hx, py = hy;
                while (rad < (WPS_HEART_R + 6) * grow)
                {
                    seed = seed * 1103515245u + 12345u;
                    rad += 12.0f + (float)((seed >> 16) % 10);
                    ang += ((float)((seed >> 8) % 100) / 100.0f - 0.5f) * 0.7f;
                    const float nx = hx + cosf(ang) * min(rad, WPS_HEART_R + 6.0f) * s, ny = hy + sinf(ang) * min(rad, WPS_HEART_R + 6.0f) * s;
                    wps_band(c, px, py, nx, ny, max(1.5f, 3.5f * s), UIC(255, 243, 200, 255));
                    px = nx;
                    py = ny;
                }
            }
            if (since > 600)
            {
                const float f = min(1.0f, (since - 600) / 1200.0f);
                const float fr = r * (1.0f + 3.0f * f);
                wps_circle(c, hx, hy, fr, UIC(255, 255, 240, (int)(230 * (1 - f))), UIC(255, 120, 40, 0));
            }
        }
    }
    if (wps.hover == WPG_HEART_NODE)
        wps_ring(c, hx, hy, r + 15 * s, max(1.5f, 3.0f * s), UM_GOLD_HI);
}

/** The card of a node (NULL height: measured only). Returns its height. */
/** Words laid in rows no wider than `width` (a word longer than a row stands alone); returns the rows. */
#define WPS_WRAP_ROWS 8
static int wps_wrap(int font, float px, float width, const char *text, char out[][200])
{
    int n = 0;
    char row[200] = "";
    const char *p = text;
    while ((*p != 0) && (n < WPS_WRAP_ROWS))
    {
        while (*p == ' ')
            p++;
        if (*p == 0)
            break;
        const char *e = p;
        while ((*e != 0) && (*e != ' '))
            e++;
        char word[120];
        const size_t wl = min((size_t)(e - p), sizeof(word) - 1);
        memcpy(word, p, wl);
        word[wl] = 0;
        char trial[200];
        snprintf(trial, sizeof(trial), (row[0] != 0) ? "%s %s" : "%s%s", row, word);
        if ((row[0] != 0) && (ui_text_width(font, px, 0, trial) > width))
        {
            snprintf(out[n++], 200, "%s", row);
            snprintf(row, sizeof(row), "%s", word);
        } else
            snprintf(row, sizeof(row), "%s", trial);
        p = e;
    }
    if ((row[0] != 0) && (n < WPS_WRAP_ROWS))
        snprintf(out[n++], 200, "%s", row);
    return n;
}

/** Wrapped rows from yy (their tops), drawn unless measuring; returns the height they take. */
static float wps_rows(struct UiCanvas *c, int font, float px, float x, float yy, float width, UiColour col, const char *text,
    TbBool draw)
{
    char rows[WPS_WRAP_ROWS][200];
    const int n = wps_wrap(font, px, width, text, rows);
    const float lh = px * 1.35f;
    for (int k = 0; (k < n) && draw; k++)
        ui_text(c, font, px, x, yy + k * lh + px * 1.02f, col, 0, false, rows[k]);
    return n * lh;
}

/** The card of a node (measure: only its height). Returns its height. Every line wraps inside it (the user,
 *  2026-11-27: the text went past the card). */
static float wps_paint_card(struct UiCanvas *c, int node, float x, float y, float w, TbBool learn_button, TbBool measure)
{
    const float u = wps.u, pad = 16 * u;
    const struct WpgNode *n = &wpg_nodes[node];
    const struct UmStyle st = {u, um_host_flat()};
    const float body = max(12.0f, 16 * u), small = max(11.0f, 13 * u), head = max(14.0f, 25 * u);
    const float inner = w - 2 * pad, text_x = x + pad + 18 * u, text_w = inner - 18 * u;
    const TbBool draw = !measure;
    const TbBool reaper = (node == WPG_HEART_NODE);
    const int p = (n->kind == WpgK_Key || reaper) ? (wps_has(node) && !(reaper && world_pentagram_points(my_player_number, node) == 0) ? 1 : 0) : wps_pts(node);
    char kind[200];
    static const char *const kinds[5] = {"The last point", "Minor skill", "Major skill", "Focus core", "Keystone (choose one of two)"};
    snprintf(kind, sizeof(kind), "%s  -  %s  -  %d/%d", n->where, kinds[n->kind], p, n->max);
    /* (the design notes' "Touches:" line, the game systems a skill works through, is not shown: the user, 2026-11-28) */
    const char *why = wps_can_learn(node) ? (learn_button ? "" : "Click: learn (pending until Confirm). Right-click: take a pending point back")
        : ((wps.pend[node] > 0) ? "Pending: Confirm to spend it" : world_pentagram_why(my_player_number, node));
    /* the height first (the same rows as drawn), then the card and its rows */
    float h = 0;
    for (int pass = 0; pass < (draw ? 2 : 1); pass++)
    {
        const TbBool paint = (pass == 1); /* (pass 0 measures) */
        if (paint)
        {
            um_card(c, &st, x, y, x + w, y + h);
            if (!st.flat)
                ui_fill_rrect(c, x, y, x + w, y + h, 12 * u, UIC(36, 24, 19, 240), UIC(16, 11, 9, 245), false);
        }
        float yy = y + pad;
        yy += wps_rows(c, UiF_Head, head, x + pad, yy, inner, UM_GOLD_HI, n->name, paint) + 4 * u;
        yy += wps_rows(c, UiF_Body, small, x + pad, yy, inner, UM_DIM, kind, paint) + 8 * u;
        for (int k = 0; k < 3; k++)
        {
            if (n->lines[k][0] == 0)
                continue;
            const TbBool had = k < p, next = (k == p) && wps_can_learn(node);
            const UiColour col = had ? UM_TEXT : next ? UM_GOLD_HI : UM_DIM;
            if (paint)
            {
                /* the point's dot, level with its first row */
                const float dy = yy + body * 0.66f;
                wps_circle(c, x + pad + 5 * u, dy, 5 * u, had ? C_LIT : UIC(0, 0, 0, 0), had ? C_LIT : UIC(0, 0, 0, 0));
                wps_ring(c, x + pad + 5 * u, dy, 5 * u, max(1.0f, 1.5f * u), col);
            }
            yy += wps_rows(c, UiF_Body, body, text_x, yy, text_w, col, n->lines[k], paint) + 4 * u;
        }
        yy += 6 * u;
        if ((why != NULL) && (why[0] != 0))
            yy += wps_rows(c, UiF_Body, small, x + pad, yy, inner, UM_GOLD, why, paint) + 4 * u;
        if (learn_button)
        {
            if (paint)
            {
                const TbBool can = wps_can_learn(node);
                um_button(c, &st, x + pad, yy + 6 * u, x + w - pad, yy + 50 * u, can ? UmB_Primary : UmB_Normal, can ? UmS_Normal : UmS_Off,
                    20 * u, can ? "Learn - 1 point" : "Learn");
            }
            yy += 54 * u;
        }
        if (!paint)
            h = yy + pad - y;
    }
    return h;
}

/* the chrome's buttons, as last painted (screen pixels) */
enum WpsBtn { WpsB_Back = 0, WpsB_Search, WpsB_Reset, WpsB_ZoomOut, WpsB_ZoomIn, WpsB_Fit, WpsB_Confirm, WpsB_Cancel, WpsB_Learn,
    WpsB_Foci, WpsB_Rest, WpsB_Count };
static float wps_btn[WpsB_Count][4];
static float wps_foci_rows[WPG_FOCI][4];
static float wps_foci_r[4];   /* the Foci card, and the node card, as last painted (the Heart's own picture avoids them) */
static float wps_card_r[4];

static TbBool wps_in(const float *r, int mx, int my)
{
    return (mx >= r[0]) && (mx < r[2]) && (my >= r[1]) && (my < r[3]);
}

static void wps_set_btn(int b, float x0, float y0, float x1, float y1)
{
    wps_btn[b][0] = x0; wps_btn[b][1] = y0; wps_btn[b][2] = x1; wps_btn[b][3] = y1;
}

static void wps_paint_chrome(struct UiCanvas *c, int mx, int my)
{
    const float u = wps.u, top = wps_top(), pad = 18 * u;
    const struct UmStyle st = {u, um_host_flat()};
    memset(wps_btn, 0, sizeof(wps_btn));
    /* the top strip */
    ui_fill_rrect(c, 0, 0, (float)c->w, top, 0, UIC(30, 18, 14, 235), UIC(10, 7, 6, 245), false);
    ui_fill_rrect(c, 0, top - 1, (float)c->w, top, 0, UIC(255, 214, 170, 40), UIC(255, 214, 170, 40), false);
    const float bh = 52 * u, by = (top - bh) / 2;
    const char *back = wps.compact ? "< Profile" : "< Return to Profile";
    const float bw = ui_text_width(UiF_Head, 21 * u, 0, back) + 40 * u;
    wps_set_btn(WpsB_Back, pad, by, pad + bw, by + bh);
    um_button(c, &st, pad, by, pad + bw, by + bh, UmB_Normal, wps_in(wps_btn[WpsB_Back], mx, my) ? UmS_Hot : UmS_Normal, 21 * u, back);
    /* the right: search, reset, zoom */
    float rx = (float)c->w - pad;
    const float rb = 44 * u, ry = (top - rb) / 2;
    if (!wps.compact)
    {
        static const char *const zl[3] = {"[ ]", "+", "-"};
        static const int zb[3] = {WpsB_Fit, WpsB_ZoomIn, WpsB_ZoomOut};
        for (int k = 0; k < 3; k++)
        {
            wps_set_btn(zb[k], rx - rb, ry, rx, ry + rb);
            um_button(c, &st, rx - rb, ry, rx, ry + rb, UmB_Normal, wps_in(wps_btn[zb[k]], mx, my) ? UmS_Hot : UmS_Normal, 20 * u, zl[k]);
            rx -= rb + 10 * u;
        }
    }
    char reset[64];
    const long cost = world_pentagram_reset_cost(my_player_number);
    if ((wps.reset_armed != 0) && (game.play_gameturn - wps.reset_armed < 100))
        snprintf(reset, sizeof(reset), "Click again: %ld gold", cost);
    else
        snprintf(reset, sizeof(reset), wps.compact ? "Reset" : "Reset Tree - %ld gold", cost);
    const float rw = ui_text_width(UiF_Head, 17 * u, 0, reset) + 32 * u;
    wps_set_btn(WpsB_Reset, rx - rw, ry, rx, ry + rb);
    um_button(c, &st, rx - rw, ry, rx, ry + rb, UmB_Normal, (cost <= 0) ? UmS_Off : wps_in(wps_btn[WpsB_Reset], mx, my) ? UmS_Hot : UmS_Normal, 17 * u, reset);
    rx -= rw + 10 * u;
    const float sw = wps.compact ? rb : 220 * u;
    wps_set_btn(WpsB_Search, rx - sw, ry, rx, ry + rb);
    ui_fill_rrect(c, rx - sw, ry, rx, ry + rb, rb / 2, UIC(30, 18, 14, 230), UIC(10, 7, 6, 240), false);
    ui_stroke_rrect(c, rx - sw, ry, rx, ry + rb, rb / 2, 1, wps.search_on ? UM_GOLD_HI : UIC(255, 214, 170, 50));
    char stext[48];
    snprintf(stext, sizeof(stext), wps.search_on ? "%s_" : ((wps.search[0] != 0) ? "%s" : (wps.compact ? "?" : "Search skills...")), wps.search);
    ui_text_box(c, UiF_Body, max(11.0f, 15 * u), rx - sw + 16 * u, ry, sw - 24 * u, rb, wps.compact ? 1 : 0,
        (wps.search[0] || wps.search_on) ? UM_TEXT : UM_DIM, 0, false, stext);
    rx -= sw + 10 * u;
    /* the middle: the points */
    const float mid_l = pad + bw + 16 * u, mid_r = rx - 16 * u;
    char info[96];
    const int total = world_pentagram_total(my_player_number);
    snprintf(info, sizeof(info), wps.compact ? "Points %d of %d - Level %d" : "Ability Points - %d of %d spent - Level %d",
        wps_spent(), total, world_player_level(my_player_number));
    const float info_w = ui_text_width(UiF_Body, max(11.0f, 15 * u), 0, info);
    const float title_w = wps.compact ? 0 : ui_text_width(UiF_Head, 15 * u, 4.5f * u, "THE PENTAGRAM") + 16 * u;
    const float disc = 58 * u;
    float x = (mid_l + mid_r) / 2 - (title_w + disc + 16 * u + info_w) / 2;
    if (!wps.compact)
        ui_text(c, UiF_Head, 15 * u, x, top / 2 + 6 * u, UM_GOLD, 4.5f * u, false, "THE PENTAGRAM");
    x += title_w;
    if (!st.flat)
        ui_glow_rrect(c, x, top / 2 - disc / 2, x + disc, top / 2 + disc / 2, disc / 2, 14 * u, UIC(255, 90, 24, 110));
    wps_circle(c, x + disc / 2, top / 2, disc / 2, UIC(255, 176, 80, 255), UIC(92, 10, 7, 255));
    char fr[12];
    snprintf(fr, sizeof(fr), "%d", wps_free());
    ui_text_box(c, UiF_Num, 30 * u, x, top / 2 - disc / 2, disc, disc, 1, UIC(255, 244, 222, 255), 0, true, fr);
    x += disc + 16 * u;
    ui_text(c, UiF_Body, max(11.0f, 15 * u), x, top / 2 - 2 * u, UM_TEXT, 0, false, info);
    {
        const int lv = world_player_level(my_player_number);
        const uint32_t xp = world_player_xp(my_player_number);
        const uint32_t a = world_player_level_xp(lv), b = world_player_level_xp(lv + 1);
        const float frac = (b > a) ? (float)(xp - a) / (float)(b - a) : 1.0f;
        um_bar(c, &st, x, top / 2 + 8 * u, x + min(260 * u, info_w), top / 2 + 17 * u, frac);
    }
    /* the Foci */
    memset(wps_foci_rows, 0, sizeof(wps_foci_rows));
    memset(wps_foci_r, 0, sizeof(wps_foci_r));
    if (!wps.compact)
    {
        const float fx = pad, fy = top + pad, fw = 250 * u, row = 34 * u;
        const float fh = 12 * u + 26 * u + WPG_FOCI * row + 12 * u;
        wps_foci_r[0] = fx; wps_foci_r[1] = fy; wps_foci_r[2] = fx + fw; wps_foci_r[3] = fy + fh;
        um_card(c, &st, fx, fy, fx + fw, fy + fh);
        ui_text(c, UiF_Head, 13 * u, fx + 18 * u, fy + 28 * u, UM_GOLD, 3 * u, false, "FOCI");
        for (int f = 0; f < WPG_FOCI; f++)
        {
            const float ry0 = fy + 38 * u + f * row;
            wps_foci_rows[f][0] = fx + 8 * u; wps_foci_rows[f][1] = ry0; wps_foci_rows[f][2] = fx + fw - 8 * u; wps_foci_rows[f][3] = ry0 + row;
            if (wps_in(wps_foci_rows[f], mx, my))
                ui_fill_rrect(c, wps_foci_rows[f][0], ry0, wps_foci_rows[f][2], ry0 + row, 8 * u, UIC(255, 226, 196, 16), UIC(255, 226, 196, 16), false);
            const int got = wps_focus_pts(f) + ((wps_group_pick(f) >= 0) ? 1 : 0);
            ui_text_box(c, UiF_Head, max(12.0f, 17 * u), fx + 18 * u, ry0, fw - 80 * u, row, 0, got ? UM_TEXT : UM_DIM, 0, false, wpg_foci_names[f]);
            char v[16];
            snprintf(v, sizeof(v), (wps_group_pick(f) >= 0) ? "%d *" : "%d", got);
            ui_text_box(c, UiF_Num, max(12.0f, 17 * u), fx + fw - 70 * u, ry0, 52 * u, row, 2, got ? UM_GOLD_HI : UM_DIM, 0, false, v);
        }
    }
    /* Day of Rest (learnt): declare tomorrow a safe day; once every 5 world days */
    {
        const int wait = world_pentagram_rest_wait(my_player_number);
        if (wait >= 0)
        {
            char rest[48];
            const struct WorldPentagram *s = world_pentagram_state(my_player_number);
            const long today = world_day_number();
            if ((s != NULL) && (s->rest_day == (uint32_t)(today + 1)))
                snprintf(rest, sizeof(rest), "Day of Rest: tomorrow");
            else if ((s != NULL) && (s->rest_day == (uint32_t)today))
                snprintf(rest, sizeof(rest), "Day of Rest: today");
            else if (wait > 0)
                snprintf(rest, sizeof(rest), "Day of Rest in %d day%s", wait, (wait == 1) ? "" : "s");
            else
                snprintf(rest, sizeof(rest), "Declare a Day of Rest");
            const float fs = max(12.0f, 16 * u), rh = 44 * u;
            const float rsw = ui_text_width(UiF_Head, fs, 0, rest) + 32 * u;
            /* under the Foci (the compact layout: over the bottom left) */
            const float x0 = pad;
            const float y0 = wps.compact ? ((float)c->h - pad - rh) : (top + pad + 12 * u + 26 * u + WPG_FOCI * 34 * u + 12 * u + 14 * u);
            wps_set_btn(WpsB_Rest, x0, y0, x0 + rsw, y0 + rh);
            um_button(c, &st, x0, y0, x0 + rsw, y0 + rh, UmB_Normal, (wait != 0) ? UmS_Off :
                wps_in(wps_btn[WpsB_Rest], mx, my) ? UmS_Hot : UmS_Normal, fs, rest);
        }
    }
    /* pending points */
    if (wps_pending() > 0)
    {
        char conf[32];
        snprintf(conf, sizeof(conf), "Confirm %d point%s", wps_pending(), (wps_pending() == 1) ? "" : "s");
        const float cw = ui_text_width(UiF_Head, 19 * u, 0, conf) + 44 * u, xw = ui_text_width(UiF_Head, 19 * u, 0, "Cancel") + 36 * u;
        const float y1 = (float)c->h - pad, y0 = y1 - 50 * u, x0 = ((float)c->w - (cw + 12 * u + xw)) / 2;
        if (!(wps.compact && (wps.selected >= 0)))
        {
            wps_set_btn(WpsB_Confirm, x0, y0, x0 + cw, y1);
            um_button(c, &st, x0, y0, x0 + cw, y1, UmB_Primary, wps_in(wps_btn[WpsB_Confirm], mx, my) ? UmS_Hot : UmS_Normal, 19 * u, conf);
            wps_set_btn(WpsB_Cancel, x0 + cw + 12 * u, y0, x0 + cw + 12 * u + xw, y1);
            um_button(c, &st, x0 + cw + 12 * u, y0, x0 + cw + 12 * u + xw, y1, UmB_Normal, wps_in(wps_btn[WpsB_Cancel], mx, my) ? UmS_Hot : UmS_Normal, 19 * u, "Cancel");
        }
    }
}

/** What is drawn over the cached web: the runes, the Heart (when it has no picture of its own), the hovered node's ring,
 *  the chrome and the card. */
static void wps_paint_over(struct UiCanvas *c, TbBool heart, TbBool runes, int mx, int my)
{
    if (runes)
        wps_paint_runes(c);
    if (heart)
        wps_paint_heart(c);
    /* the hovered node's ring */
    if ((wps.hover > 0) && (wps.hover < WPG_NODES))
    {
        const struct WpgNode *n = &wpg_nodes[wps.hover];
        wps_ring(c, SX(n->x), SY(n->y), (wps_rad[n->kind] + 6) * wps.s, max(1.5f, 2.5f * wps.s), UM_GOLD_HI);
    }
    wps_paint_chrome(c, mx, my);
    /* the card: by the hovered node, or docked at the foot (compact); none for the Heart until it is unlocked */
    const int node = wps.compact ? wps.selected : wps.hover;
    memset(wps_btn[WpsB_Learn], 0, sizeof(wps_btn[WpsB_Learn]));
    memset(wps_card_r, 0, sizeof(wps_card_r));
    if ((node >= 0) && !((node == WPG_HEART_NODE) && (world_pentagram_points(my_player_number, WPG_HEART_NODE) == 0)))
    {
        if (wps.compact)
        {
            const float pad = 18 * wps.u, w = (float)c->w - 2 * pad;
            const float h = wps_paint_card(c, node, pad, 0, w, true, true);
            const float y = (float)c->h - pad - h;
            wps_paint_card(c, node, pad, y, w, true, false);
            wps_card_r[0] = pad; wps_card_r[1] = y; wps_card_r[2] = pad + w; wps_card_r[3] = y + h;
            wps_set_btn(WpsB_Learn, pad + 16 * wps.u, y + h - 16 * wps.u - 50 * wps.u, pad + w - 16 * wps.u, y + h - 16 * wps.u);
        } else
        {
            const struct WpgNode *n = &wpg_nodes[node];
            const float w = 400 * wps.u, r = (wps_rad[n->kind] + 14) * wps.s;
            const float h = wps_paint_card(c, node, 0, 0, w, false, true);
            float x = SX(n->x) + r + 12;
            if (x + w > c->w - 10)
                x = SX(n->x) - r - 12 - w;
            float y = SY(n->y) - 60 * wps.u;
            y = max(wps_top() + 6, min(y, (float)c->h - h - 10));
            wps_paint_card(c, node, x, y, w, false, false);
            wps_card_r[0] = x; wps_card_r[1] = y; wps_card_r[2] = x + w; wps_card_r[3] = y + h;
        }
    }
}

/* Speed (the user, 2026-11-27: "zooming in and panning is very slow"): the floor and the web are painted once into a
   cache at a scale (the whole pentagram when it fits WPS_CACHE_MAX_PX, else the view and a quarter more round it);
   panning copies from it, a zoom shows it stretched until the wheel has rested WPS_ZOOM_REST ms, then paints it again
   sharp. The screen's picture is then only the cache, the runes, the chrome and the card, repainted when one of them
   changes; the beating Heart has a small picture of its own over it (the Spare slot: the Reaper's icon is not shown
   while this screen is open) unless the chrome or a card lies over it. */
#define WPS_CACHE_MAX_PX (2900 * 2900)
#define WPS_WORLD_HALF 1500.0f
#define WPS_ZOOM_REST 180UL

static struct UiCanvas wps_cache;
static float wps_cache_s, wps_cache_ox, wps_cache_oy; /* cache pixel = world * s + (ox, oy) */
static float wps_cache_r[4];                           /* the world rectangle it holds */
static char wps_cache_key[200];
static unsigned long wps_cache_gen;
static struct UmHost wps_heart_host;

static uint32_t wps_pend_hash(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < WPG_NODES; i++)
        h = (h ^ wps.pend[i]) * 16777619u;
    return h;
}

/** The cache's world rectangle at scale s (false: none fits). */
static void wps_cache_rect_for(float s, float *r)
{
    const float half = WPS_WORLD_HALF;
    if ((2.0f * half * s) * (2.0f * half * s) <= (float)WPS_CACHE_MAX_PX)
    {
        r[0] = -half; r[1] = -half; r[2] = half; r[3] = half;
        return;
    }
    const float vx0 = (0 - wps.ox) / s, vy0 = (0 - wps.oy) / s, vx1 = ((float)wps.sw - wps.ox) / s, vy1 = ((float)wps.sh - wps.oy) / s;
    const float mxw = (vx1 - vx0) * 0.15f, myw = (vy1 - vy0) * 0.15f;
    r[0] = max(-half, vx0 - mxw); r[1] = max(-half, vy0 - myw);
    r[2] = min(half, vx1 + mxw); r[3] = min(half, vy1 + myw);
    if (r[2] <= r[0]) r[2] = r[0] + 1;
    if (r[3] <= r[1]) r[3] = r[1] + 1;
}

/** Does the cache hold what the screen shows now (at its own scale)? */
static TbBool wps_cache_covers(void)
{
    const float half = WPS_WORLD_HALF;
    const float vx0 = max(-half, (0 - wps.ox) / wps.s), vy0 = max(-half, (wps_top() - wps.oy) / wps.s);
    const float vx1 = min(half, ((float)wps.sw - wps.ox) / wps.s), vy1 = min(half, ((float)wps.sh - wps.oy) / wps.s);
    if ((vx1 <= vx0) || (vy1 <= vy0))
        return true; /* (nothing of the pentagram on the screen) */
    return (vx0 >= wps_cache_r[0]) && (vy0 >= wps_cache_r[1]) && (vx1 <= wps_cache_r[2]) && (vy1 <= wps_cache_r[3]);
}

static void wps_cache_paint(void)
{
    float r[4];
    wps_cache_rect_for(wps.s, r);
    const int cw = max(1, (int)ceilf((r[2] - r[0]) * wps.s)), ch = max(1, (int)ceilf((r[3] - r[1]) * wps.s));
    if (!ui_canvas_size_raw(&wps_cache, cw, ch))
        return;
    const float ks = wps.s, kox = wps.ox, koy = wps.oy;
    const int ksw = wps.sw, ksh = wps.sh;
    wps.ox = -r[0] * wps.s;
    wps.oy = -r[1] * wps.s;
    wps_cache_s = wps.s; wps_cache_ox = wps.ox; wps_cache_oy = wps.oy;
    memcpy(wps_cache_r, r, sizeof(wps_cache_r));
    const unsigned long tb = LbTimerClock();
    wps_paint_background(&wps_cache);
    const unsigned long tw = LbTimerClock();
    wps_paint_web(&wps_cache);
    static int logs = 0;
    if (logs < 12)
    {
        logs++;
        SYNCLOG("Pentagram screen: web cached at scale %.3f, %dx%d, %lu ms (floor %lu)", (double)ks, cw, ch,
            (unsigned long)(LbTimerClock() - tb), (unsigned long)(tw - tb));
    }
    wps.s = ks; wps.ox = kox; wps.oy = koy; wps.sw = ksw; wps.sh = ksh;
    wps_cache_gen++;
}

/** The cache onto the screen's canvas (stretched when the zoom is not its own). */
/** A cached picture (pixel = world * s + (ox, oy)) stretched onto the screen's canvas, the nearest pixel. */
static void wps_stretch(struct UiCanvas *c, const struct UiCanvas *from, float fs, float fox, float foy)
{
    if ((from->px == NULL) || (fs <= 0.0f))
        return;
    uint32_t *dst = (uint32_t *)c->px;
    const uint32_t *src = (const uint32_t *)from->px;
    const int cw = from->w, ch = from->h;
    const float k = fs / wps.s;
    const int64_t kfix = (int64_t)(k * 65536.0f);
    for (int y = 0; y < c->h; y++)
    {
        const int sy = (int)floorf(((float)y + 0.5f - wps.oy) * k + foy);
        if ((sy < 0) || (sy >= ch))
            continue;
        const uint32_t *row = src + (size_t)sy * cw;
        /* only the columns that fall inside the picture */
        const float xa = (0.0f - foy * 0.0f - fox) / k + wps.ox, xb = ((float)cw - fox) / k + wps.ox;
        const int x0 = max(0, (int)floorf(xa)), x1 = min(c->w, (int)ceilf(xb));
        int64_t sxf = (int64_t)((((((float)x0 + 0.5f) - wps.ox) * k) + fox) * 65536.0f);
        uint32_t *out = dst + (size_t)y * c->w;
        for (int x = x0; x < x1; x++, sxf += kfix)
        {
            const int sx = (int)(sxf >> 16);
            if ((sx >= 0) && (sx < cw))
                out[x] = row[sx];
        }
    }
}

/* The whole pentagram, smaller (2026-11-27: "it cuts out while zooming out"): while a zoom eases, the sharp cache may
   hold only the view it was painted for; this picture of all of it goes under it, so nothing is cut off. */
#define WPS_OVERVIEW_PX 1600
static struct UiCanvas wps_ov;
static float wps_ov_s, wps_ov_ox, wps_ov_oy;
static char wps_ov_key[200];

static void wps_overview_paint(const char *key)
{
    if ((wps_ov.px != NULL) && (strcmp(key, wps_ov_key) == 0))
        return;
    const float s = (float)WPS_OVERVIEW_PX / (2.0f * WPS_WORLD_HALF);
    if (!ui_canvas_size_raw(&wps_ov, WPS_OVERVIEW_PX, WPS_OVERVIEW_PX))
        return;
    const float ks = wps.s, kox = wps.ox, koy = wps.oy;
    wps.s = s;
    wps.ox = WPS_WORLD_HALF * s;
    wps.oy = WPS_WORLD_HALF * s;
    wps_ov_s = wps.s; wps_ov_ox = wps.ox; wps_ov_oy = wps.oy;
    wps_paint_background(&wps_ov);
    wps_paint_web(&wps_ov);
    wps.s = ks; wps.ox = kox; wps.oy = koy;
    snprintf(wps_ov_key, sizeof(wps_ov_key), "%s", key);
}

/** The cache onto the screen's canvas (stretched, over the whole pentagram's smaller picture, when the zoom is not
 *  its own). */
static void wps_cache_blit(struct UiCanvas *c)
{
    wps_fill_bg(c);
    if (wps_cache.px == NULL)
        return;
    uint32_t *dst = (uint32_t *)c->px;
    const uint32_t *src = (const uint32_t *)wps_cache.px;
    const int cw = wps_cache.w, ch = wps_cache.h;
    if (fabsf(wps.s - wps_cache_s) < 1e-6f * wps.s)
    {
        const int dx = (int)floorf(wps.ox - wps_cache_ox + 0.5f), dy = (int)floorf(wps.oy - wps_cache_oy + 0.5f);
        const int x0 = max(0, dx), x1 = min(c->w, dx + cw);
        if (x1 <= x0)
            return;
        for (int y = max(0, dy); y < min(c->h, dy + ch); y++)
            memcpy(dst + (size_t)y * c->w + x0, src + (size_t)(y - dy) * cw + (x0 - dx), (size_t)(x1 - x0) * 4);
        return;
    }
    wps_stretch(c, &wps_ov, wps_ov_s, wps_ov_ox, wps_ov_oy);
    wps_stretch(c, &wps_cache, wps_cache_s, wps_cache_ox, wps_cache_oy);
}

static TbBool wps_rect_meets(const float *a, const float *b)
{
    return (a[2] > a[0]) && (b[2] > b[0]) && (a[0] < b[2]) && (b[0] < a[2]) && (a[1] < b[3]) && (b[1] < a[3]);
}

/** The pointer's chrome target (0: none), as the chrome's buttons were last painted. */
static int wps_hot(int mx, int my)
{
    for (int b = 0; b < WpsB_Count; b++)
        if (wps_in(wps_btn[b], mx, my))
            return 1 + b;
    for (int f = 0; f < WPG_FOCI; f++)
        if (wps_in(wps_foci_rows[f], mx, my))
            return 100 + f;
    return 0;
}

/** Test hook (KFX_WORLD_PENTAGRAMTEST mode "motion"): pans the view 9 pixels a turn, and every 10th turn zooms in
 *  (its first 60 turns) or out; the frames' costs are logged. */
static int wps_motion_log;
void world_pentagram_screen_test_motion(long step)
{
    if (!wps.open)
        return;
    if (step == 0)
        wps_motion_log = 160;
    wps.ox -= 9.0f;
    if ((step % 10) == 5)
        wps_zoom_at((step < 60) ? 1.15f : 1.0f / 1.15f, wps.sw / 2.0f, wps.sh / 2.0f);
}

/** Each frame (not each game turn): a drag follows the pointer, a zoom eases on. */
static unsigned long wps_last_ms;
static void wps_animate(int mx, int my, unsigned long now)
{
    if (wps.dragging && wps.dragged && ((float)wps.grab_my > wps_top()))
    {
        wps.ox = wps.grab_ox + (float)(mx - wps.grab_mx);
        wps.oy = wps.grab_oy + (float)(my - wps.grab_my);
    }
    if ((wps.zt > 0.0f) && (fabsf(wps.zt - wps.s) > 1e-6f * wps.zt))
    {
        const float dt = (float)min(100UL, now - wps_last_ms);
        float ns = wps.s + (wps.zt - wps.s) * (1.0f - expf(-dt / 40.0f));
        if (fabsf(ns - wps.zt) < 0.004f * wps.zt)
            ns = wps.zt;
        wps.ox = wps.zax - (wps.zax - wps.ox) * ns / wps.s;
        wps.oy = wps.zay - (wps.zay - wps.oy) * ns / wps.s;
        wps.s = ns;
        wps_zoom_ms = now;
        if (ns == wps.zt)
            wps.zt = 0.0f;
    }
    wps_last_ms = now;
}

/* OpenGL and Vulkan (2026-11-27, "the camera panning is still rough"): the web is a picture of its own that the GPU
   moves (panning changes where it is drawn, not the picture: nothing is painted or uploaded), on the game UI's box of
   the ground's colour, with the chrome a picture of its own over it (repainted only when what it shows changes) and
   the Heart's over that. The software renderer keeps the one picture (wps_host). */
static struct UmHost wps_web_host, wps_ov_host, wps_chrome_host;

void world_pentagram_screen_draw(void)
{
    if (!world_pentagram_screen_open())
        return;
    wps_layout();
    if (!wps.fitted)
        wps_fit();
    const int mx = GetMouseX(), my = GetMouseY();
    const unsigned long now = LbTimerClock();
    if ((wps_bg_index == 0) && (engine_palette != NULL))
    {
        wps_bg_index = LbPaletteFindColour(engine_palette, 17 >> 2, 11 >> 2, 8 >> 2);
        {
            const unsigned char *e = engine_palette + 3 * wps_bg_index;
            wps_bg_col = UIC((e[0] << 2) | (e[0] >> 4), (e[1] << 2) | (e[1] >> 4), (e[2] << 2) | (e[2] >> 4), 255);
        }
    }
    wps_animate(mx, my, now);
    /* the cache: painted again when the web changes, a zoom has rested, or the view left it */
    char ckey[200];
    snprintf(ckey, sizeof(ckey), "%d %d %d %u %s %d %d", (int)world_pentagram_state(my_player_number)->version, wps.norder,
        world_pentagram_total(my_player_number), (unsigned)wps_pend_hash(), wps.search, (int)um_host_flat(), (int)wps.compact);
    const TbBool same_scale = (wps_cache.px != NULL) && (fabsf(wps.s - wps_cache_s) < 1e-6f * wps.s);
    if ((wps_cache.px == NULL) || (strcmp(ckey, wps_cache_key) != 0) ||
        (!same_scale && ((now - wps_zoom_ms) >= WPS_ZOOM_REST)) || (same_scale && !wps_cache_covers()))
    {
        wps_cache_paint();
        snprintf(wps_cache_key, sizeof(wps_cache_key), "%s", ckey);
    }
    {
        char okey[220];
        snprintf(okey, sizeof(okey), "%s|%dx%d", ckey, wps.sw, wps.sh);
        wps_overview_paint(okey);
    }
    /* the Heart's own picture: when nothing of the chrome or a card lies over it */
    const float hs = wps.s, hr = WPS_HEART_R * hs;
    const unsigned long since = wps.crack_started ? now - wps.crack_started : 100000UL;
    const float hhalf = ceilf(((since <= 1900) ? 4.0f * hr : hr + 110.0f * hs) + 4.0f);
    const float hrect[4] = {SX(0) - hhalf, SY(0) - hhalf, SX(0) + hhalf, SY(0) + hhalf};
    const float strip[4] = {0, 0, (float)wps.sw, wps_top()};
    TbBool apart = !wps_rect_meets(hrect, strip) && !wps_rect_meets(hrect, wps_foci_r) && !wps_rect_meets(hrect, wps_card_r);
    for (int b = 0; (b < WpsB_Count) && apart; b++)
        apart = !wps_rect_meets(hrect, wps_btn[b]);
    apart = apart && (hrect[2] > 0) && (hrect[3] > 0) && (hrect[0] < wps.sw) && (hrect[1] < wps.sh);
    const unsigned long beat = now / 40UL;
    if (!um_host_flat())
    {
        /* the ground: the game UI's box (under every picture) */
        UIRenderer_SubmitSolidBox(0, 0, wps.sw, wps.sh, wps_bg_index);
        /* the whole pentagram's smaller picture under the web (seen only where the sharp one does not reach while a
           zoom eases), and the web with its runes: both moved and scaled by the GPU (nothing is painted or uploaded
           for a pan or a zoom; the sharp web is painted again once a zoom has rested) */
        if (wps_ov.px != NULL)
        {
            wps_ov_host.name = "pentagram_overview";
            wps_ov_host.slot = UmSl_KeeperPanel; /* (the Keeper panel closes as it opens) */
            if (um_host_begin(&wps_ov_host, wps_ov.w, wps_ov.h, wps_ov_key))
            {
                memcpy(wps_ov_host.canvas.px, wps_ov.px, (size_t)wps_ov.w * (size_t)wps_ov.h * 4);
                um_host_painted(&wps_ov_host, now);
            }
            const float k = wps.s / wps_ov_s;
            um_host_show_sized(&wps_ov_host, (int)floorf(wps.ox - wps_ov_ox * k + 0.5f), (int)floorf(wps.oy - wps_ov_oy * k + 0.5f),
                (int)floorf(wps_ov.w * k + 0.5f), (int)floorf(wps_ov.h * k + 0.5f));
        }
        if (wps_cache.px != NULL)
        {
            wps_web_host.name = "pentagram_web";
            wps_web_host.slot = UmSl_Library;
            char wkey[64];
            snprintf(wkey, sizeof(wkey), "%lu %lu", wps_cache_gen, now / 2000UL);
            if (um_host_begin(&wps_web_host, wps_cache.w, wps_cache.h, wkey))
            {
                memcpy(wps_web_host.canvas.px, wps_cache.px, (size_t)wps_cache.w * (size_t)wps_cache.h * 4);
                const float ks = wps.s, kox = wps.ox, koy = wps.oy;
                wps.s = wps_cache_s; wps.ox = wps_cache_ox; wps.oy = wps_cache_oy;
                wps_paint_runes(&wps_web_host.canvas);
                wps.s = ks; wps.ox = kox; wps.oy = koy;
                um_host_painted(&wps_web_host, now);
            }
            const float k = wps.s / wps_cache_s;
            const int wx = (int)floorf(wps.ox - wps_cache_ox * k + 0.5f), wy = (int)floorf(wps.oy - wps_cache_oy * k + 0.5f);
            if (fabsf(k - 1.0f) < 1e-6f)
                um_host_show(&wps_web_host, wx, wy);
            else
                um_host_show_sized(&wps_web_host, wx, wy, (int)floorf(wps_cache.w * k + 0.5f), (int)floorf(wps_cache.h * k + 0.5f));
        }
        /* the chrome, the hovered node's ring and card (and the Heart when something lies over it) */
        const TbBool tied = (wps.hover >= 0) || (wps.compact && (wps.selected >= 0)) || !apart; /* (things on the web) */
        wps_chrome_host.name = "pentagram_chrome";
        wps_chrome_host.slot = UmSl_LibraryTip;
        char ckey2[240];
        snprintf(ckey2, sizeof(ckey2), "%d %d %d %d %d %ld %d %d %d %d %lu %d %lu %d %.5f %.1f %.1f", wps.hover, wps.selected,
            wps_hot(mx, my), wps.norder, (int)wps.search_on, (long)world_pentagram_spent(my_player_number),
            (int)(game.play_gameturn / 20), (int)(wps.reset_armed != 0), world_pentagram_rest_wait(my_player_number), (int)apart,
            apart ? 0UL : beat, (int)strlen(wps.search), apart ? 0UL : wps.crack_started, (int)tied, tied ? (double)wps.s : 0.0,
            tied ? (double)wps.ox : 0.0, tied ? (double)wps.oy : 0.0);
        if (um_host_begin(&wps_chrome_host, wps.sw, wps.sh, ckey2))
        {
            wps_paint_over(&wps_chrome_host.canvas, !apart, false, mx, my);
            um_host_painted(&wps_chrome_host, now);
        }
        um_host_show(&wps_chrome_host, 0, 0);
    } else
    {
    /* the screen's picture */
    wps_host.name = "pentagram";
    wps_host.slot = UmSl_KeeperPanel; /* (the Keeper panel closes as it opens) */
    char key[240];
    snprintf(key, sizeof(key), "%lu %.5f %.1f %.1f %lu %d %d %d %d %d %ld %d %d %d %d %lu %d %lu", wps_cache_gen, (double)wps.s,
        (double)wps.ox, (double)wps.oy, now / 2000UL, wps.hover, wps.selected, wps_hot(mx, my), wps.norder, (int)wps.search_on,
        (long)world_pentagram_spent(my_player_number), (int)(game.play_gameturn / 20), (int)(wps.reset_armed != 0),
        world_pentagram_rest_wait(my_player_number), (int)apart, apart ? 0UL : beat, (int)strlen(wps.search),
        apart ? 0UL : wps.crack_started);
    if (um_host_begin(&wps_host, wps.sw, wps.sh, key))
    {
        const unsigned long t0 = LbTimerClock();
        wps_cache_blit(&wps_host.canvas);
        const unsigned long t1 = LbTimerClock();
        wps_paint_over(&wps_host.canvas, !apart, true, mx, my);
        um_host_painted(&wps_host, now);
        if (wps_motion_log > 0)
        {
            wps_motion_log--;
            SYNCLOG("Pentagram screen: frame: blit %lu ms, over %lu ms (scale %.3f%s)", t1 - t0, (unsigned long)(LbTimerClock() - t1),
                (double)wps.s, (fabsf(wps.s - wps_cache_s) < 1e-6f * wps.s) ? "" : ", stretched");
        }
    }
    um_host_show(&wps_host, 0, 0);
    }
    if (!apart)
        return;
    const int hsz = (int)(2 * hhalf);
    wps_heart_host.name = "pentagram_heart";
    wps_heart_host.slot = UmSl_Spare;
    char hkey[160];
    snprintf(hkey, sizeof(hkey), "%d %.5f %lu %lu %d %d %d %d", hsz, (double)wps.s, beat, wps.crack_started,
        (int)(world_pentagram_points(my_player_number, WPG_HEART_NODE) > 0), (int)(wps.hover == WPG_HEART_NODE),
        (int)wps_can_learn(WPG_HEART_NODE), (int)wps.pend[WPG_HEART_NODE]);
    if (um_host_begin(&wps_heart_host, hsz, hsz, hkey))
    {
        const float kox = wps.ox, koy = wps.oy;
        wps.ox = hhalf;
        wps.oy = hhalf;
        wps_paint_heart(&wps_heart_host.canvas);
        wps.ox = kox;
        wps.oy = koy;
        um_host_painted(&wps_heart_host, now);
    }
    um_host_show(&wps_heart_host, (int)floorf(SX(0) - hhalf), (int)floorf(SY(0) - hhalf));
}

/******************************************************************************/
/* Input: everything goes to the screen while it is open */

/** A typed key as a letter or digit (0: none). */
static char wps_key_char(TbKeyCode k)
{
    static const TbKeyCode keys[] = {KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G, KC_H, KC_I, KC_J, KC_K, KC_L, KC_M, KC_N, KC_O,
        KC_P, KC_Q, KC_R, KC_S, KC_T, KC_U, KC_V, KC_W, KC_X, KC_Y, KC_Z, KC_SPACE, KC_APOSTROPHE};
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz '";
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (keys[i] == k)
            return chars[i];
    return 0;
}

TbBool world_pentagram_screen_inputs(void)
{
    if (!world_pentagram_screen_open())
        return false;
    wps_layout();
    if (!wps.fitted)
        wps_fit();
    const int mx = GetMouseX(), my = GetMouseY();
    if (lbKeyOn[KC_ESCAPE])
    {
        clear_key_pressed(KC_ESCAPE);
        if (wps.search_on)
            wps.search_on = false;
        else
            world_pentagram_screen_close();
        return true;
    }
    /* typing into Search */
    if (wps.search_on)
    {
        if (lbInkey != 0)
        {
            const char ch = wps_key_char(lbInkey);
            size_t n = strlen(wps.search);
            if ((lbInkey == KC_BACK) && (n > 0))
                wps.search[n - 1] = 0;
            else if ((ch >= 32) && (ch < 127) && (n + 1 < sizeof(wps.search)))
            {
                wps.search[n] = ch;
                wps.search[n + 1] = 0;
            }
            lbInkey = 0;
        }
    }
    if (wheel_scrolled_up || wheel_scrolled_down)
    {
        wps_zoom_at(wheel_scrolled_up ? 1.15f : 1.0f / 1.15f, (float)mx, (float)my);
        wheel_scrolled_up = wheel_scrolled_down = false;
    }
    /* dragging pans */
    if (lbDisplay.MLeftButton && !wps.dragging && left_button_held)
    {
        wps.dragging = true;
        wps.dragged = false;
        wps.grab_mx = mx;
        wps.grab_my = my;
        wps.grab_ox = wps.ox;
        wps.grab_oy = wps.oy;
    }
    if (wps.dragging)
    {
        if ((abs(mx - wps.grab_mx) + abs(my - wps.grab_my)) > 4)
            wps.dragged = true;
        if (wps.dragged && ((float)wps.grab_my > wps_top()))
        {
            wps.ox = wps.grab_ox + (float)(mx - wps.grab_mx);
            wps.oy = wps.grab_oy + (float)(my - wps.grab_my);
        }
        if (!lbDisplay.MLeftButton)
            wps.dragging = false;
    }
    if (!wps.compact)
        wps.hover = wps.dragged ? -1 : wps_node_at(mx, my); /* (none while the web is dragged: the chrome stays still) */
    {
        /* test switch KFX_WORLD_PENTAGRAMHOVER=<node name>: that node's card is shown, as under the pointer */
        static int forced = -2;
        if (forced == -2)
        {
            const char *v = getenv("KFX_WORLD_PENTAGRAMHOVER");
            forced = -1;
            for (int i = 0; (v != NULL) && (i < WPG_NODES) && (forced < 0); i++)
                if (strcmp(wpg_nodes[i].name, v) == 0)
                    forced = i;
        }
        if (forced >= 0)
        {
            wps.hover = forced;
            wps.selected = forced;
        }
    }
    if (left_button_released)
    {
        const TbBool was_drag = wps.dragged;
        left_button_released = 0;
        left_button_clicked = 0;
        wps.dragged = false;
        if (!was_drag)
        {
            if (wps_in(wps_btn[WpsB_Back], mx, my))
            {
                /* back to the Player Profile (the Keeper panel the Upgrades tab opened this from) */
                world_pentagram_screen_close();
                world_keeper_panel_show();
            }
            else if (wps_in(wps_btn[WpsB_ZoomIn], mx, my))
                wps_zoom_at(1.25f, wps.sw / 2.0f, wps.sh / 2.0f);
            else if (wps_in(wps_btn[WpsB_ZoomOut], mx, my))
                wps_zoom_at(1.0f / 1.25f, wps.sw / 2.0f, wps.sh / 2.0f);
            else if (wps_in(wps_btn[WpsB_Fit], mx, my))
                wps_fit();
            else if (wps_in(wps_btn[WpsB_Search], mx, my))
                wps.search_on = !wps.search_on;
            else if (wps_in(wps_btn[WpsB_Reset], mx, my))
            {
                if ((wps.reset_armed != 0) && (game.play_gameturn - wps.reset_armed < 100))
                {
                    wps_cancel();
                    world_pentagram_send_reset();
                    wps.reset_armed = 0;
                } else if (world_pentagram_reset_cost(my_player_number) > 0)
                    wps.reset_armed = game.play_gameturn;
            }
            else if (wps_in(wps_btn[WpsB_Rest], mx, my))
            {
                if (world_pentagram_rest_wait(my_player_number) == 0)
                    world_pentagram_send_rest();
            }
            else if (wps_in(wps_btn[WpsB_Confirm], mx, my))
                wps_confirm();
            else if (wps_in(wps_btn[WpsB_Cancel], mx, my))
                wps_cancel();
            else if (wps.compact && wps_in(wps_btn[WpsB_Learn], mx, my))
                wps_learn(wps.selected);
            else
            {
                TbBool taken = false;
                for (int f = 0; (f < WPG_FOCI) && !taken; f++)
                    if (wps_in(wps_foci_rows[f], mx, my))
                    {
                        for (int i = 0; i < WPG_NODES; i++)
                            if ((wpg_nodes[i].focus == f) && (wpg_nodes[i].kind == WpgK_Core))
                                wps_fly_to(wpg_nodes[i].x, wpg_nodes[i].y);
                        taken = true;
                    }
                if (!taken)
                {
                    const int node = wps_node_at(mx, my);
                    if (wps.compact)
                    {
                        wps.selected = node;
                        if (node == WPG_HEART_NODE)
                            wps_learn(node); /* (the Heart has no card before it is unlocked) */
                    } else if (node >= 0)
                        wps_learn(node);
                }
            }
        }
    }
    if (right_button_released)
    {
        right_button_released = 0;
        right_button_clicked = 0;
        const int node = wps_node_at(mx, my);
        if (node >= 0)
            wps_unpend(node);
    }
    return true;
}

#ifdef __cplusplus
}
#endif

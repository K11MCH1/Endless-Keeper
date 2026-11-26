// Endless Keeper launcher: the effects of the modern looks (see fx.h).
#include "fx.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

bool fx_motion = true;

static SDL_Renderer *g_r = nullptr;
static SDL_Texture *g_round[2] = {nullptr, nullptr}, *g_box[2] = {nullptr, nullptr}; /* [0] drawn over, [1] adds light */
static float g_time = 0, g_dt = 0.016f;
static std::map<ImGuiID, float> g_follow;

static const int MASK = 64, MARGIN = 24; /* the masks' size, and the box mask's soft rim */

static ImTextureRef ref(SDL_Texture *t)
{
    return ImTextureRef((ImTextureID)(intptr_t)t);
}

static float clamp01(float v)
{
    return (v < 0) ? 0 : ((v > 1) ? 1 : v);
}

/** White with a soft alpha: a round spot, or a box with a soft rim. */
static SDL_Texture *mask_texture(bool box, bool additive)
{
    static unsigned char px[MASK * MASK * 4];
    for (int y = 0; y < MASK; y++)
        for (int x = 0; x < MASK; x++)
        {
            float a;
            if (box)
            {
                const float dx = std::max(std::max(MARGIN - (x + 0.5f), (x + 0.5f) - (MASK - MARGIN)), 0.0f);
                const float dy = std::max(std::max(MARGIN - (y + 0.5f), (y + 0.5f) - (MASK - MARGIN)), 0.0f);
                a = clamp01(1.0f - sqrtf(dx * dx + dy * dy) / MARGIN);
                a = a * a * (3.0f - 2.0f * a);
                a *= a;
            } else
            {
                a = clamp01(1.0f - hypotf(x + 0.5f - MASK * 0.5f, y + 0.5f - MASK * 0.5f) / (MASK * 0.5f));
                a = a * a * (3.0f - 2.0f * a);
                a *= a;
            }
            unsigned char *p = px + (y * MASK + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    SDL_Texture *t = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, MASK, MASK);
    if (t != nullptr)
    {
        SDL_UpdateTexture(t, nullptr, px, MASK * 4);
        SDL_SetTextureBlendMode(t, additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}

void fx_init(SDL_Renderer *renderer)
{
    g_r = renderer;
    for (int i = 0; i < 2; i++)
    {
        g_round[i] = mask_texture(false, i == 1);
        g_box[i] = mask_texture(true, i == 1);
    }
}

void fx_frame(float dt)
{
    g_dt = std::min(dt, 0.1f);
    g_time += g_dt;
}

float fx_time()
{
    return g_time;
}

float fx_ease_out(float t)
{
    t = 1.0f - clamp01(t);
    return 1.0f - t * t * t;
}

float fx_heartbeat()
{
    if (!fx_motion)
        return 0.35f;
    const float p = fmodf(g_time, 1.7f);
    const float a = (p - 0.10f) / 0.085f, b = (p - 0.40f) / 0.11f;
    return clamp01(expf(-a * a) + 0.62f * expf(-b * b));
}

float fx_follow(ImGuiID id, float target, float seconds)
{
    auto it = g_follow.find(id);
    if ((it == g_follow.end()) || !fx_motion || (seconds <= 0))
    {
        g_follow[id] = target;
        return target;
    }
    it->second += (target - it->second) * (1.0f - expf(-g_dt * 3.0f / seconds));
    if (fabsf(target - it->second) < 0.004f)
        it->second = target;
    return it->second;
}

/** A soft rim round a rounded box, outside it only: strongest at the box's outline, gone `spread` further out, and
    pulled down by `drop` on its way (a shadow falls). With a texture it is drawn through that (light that adds). */
static void rim(ImDrawList *dl, SDL_Texture *tex, ImVec2 a, ImVec2 b, float rounding, float spread, ImU32 col, float drop)
{
    if (((col & IM_COL32_A_MASK) == 0) || (spread <= 0) || (b.x <= a.x) || (b.y <= a.y))
        return;
    dl->PathRect(a, b, std::max(1.0f, std::min(rounding, std::min(b.x - a.x, b.y - a.y) * 0.5f)));
    static std::vector<ImVec2> pts, out;
    pts.assign(dl->_Path.Data, dl->_Path.Data + dl->_Path.Size);
    dl->PathClear();
    const int n = (int)pts.size();
    if (n < 3)
        return;
    out.resize(n);
    for (int i = 0; i < n; i++)
    {
        /* which way is out at this point: between its two edges' own (the outline goes clockwise) */
        const ImVec2 p0 = pts[(i + n - 1) % n], p1 = pts[i], p2 = pts[(i + 1) % n];
        ImVec2 e0(p1.x - p0.x, p1.y - p0.y), e1(p2.x - p1.x, p2.y - p1.y);
        const float l0 = sqrtf(e0.x * e0.x + e0.y * e0.y), l1 = sqrtf(e1.x * e1.x + e1.y * e1.y);
        if (l0 > 1e-4f) { e0.x /= l0; e0.y /= l0; }
        if (l1 > 1e-4f) { e1.x /= l1; e1.y /= l1; }
        ImVec2 m(e0.y + e1.y, -(e0.x + e1.x));
        const float lm = sqrtf(m.x * m.x + m.y * m.y);
        if (lm > 1e-4f) { m.x /= lm; m.y /= lm; } else { m = ImVec2(e0.y, -e0.x); }
        const float along = std::max(0.5f, m.x * e0.y - m.y * e0.x); /* (a corner's point goes further, to keep the rim's width) */
        out[i] = ImVec2(m.x / along, m.y / along);
    }
    static const float at[4] = {0.0f, 0.22f, 0.52f, 1.0f}, weight[4] = {1.0f, 0.58f, 0.22f, 0.0f};
    const ImVec2 uv = (tex != nullptr) ? ImVec2(0.5f, 0.5f) : dl->_Data->TexUvWhitePixel;
    const float alpha = (float)((col >> IM_COL32_A_SHIFT) & 0xFF);
    if (tex != nullptr)
        dl->PushTexture(ref(tex));
    dl->PrimReserve(n * 18, n * 4);
    const unsigned base = dl->_VtxCurrentIdx;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            dl->PrimWriteVtx(ImVec2(pts[i].x + out[i].x * spread * at[k], pts[i].y + out[i].y * spread * at[k] + drop * at[k]), uv,
                (col & ~IM_COL32_A_MASK) | ((ImU32)(alpha * weight[k] + 0.5f) << IM_COL32_A_SHIFT));
    for (int i = 0; i < n; i++)
    {
        const unsigned p = base + i * 4, q = base + ((i + 1) % n) * 4;
        for (int k = 0; k < 3; k++)
        {
            dl->PrimWriteIdx((ImDrawIdx)(p + k));
            dl->PrimWriteIdx((ImDrawIdx)(q + k));
            dl->PrimWriteIdx((ImDrawIdx)(q + k + 1));
            dl->PrimWriteIdx((ImDrawIdx)(p + k));
            dl->PrimWriteIdx((ImDrawIdx)(q + k + 1));
            dl->PrimWriteIdx((ImDrawIdx)(p + k + 1));
        }
    }
    if (tex != nullptr)
        dl->PopTexture();
}

void fx_shadow(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float spread, ImU32 col)
{
    rim(dl, nullptr, a, b, rounding, spread, col, spread * 0.25f);
}

void fx_halo(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float spread, ImU32 col)
{
    if (g_box[1] != nullptr)
        rim(dl, g_box[1], a, b, rounding, spread, col, 0.0f);
}

void fx_glow(ImDrawList *dl, ImVec2 c, float rx, float ry, ImU32 col)
{
    if ((g_round[1] != nullptr) && ((col & IM_COL32_A_MASK) != 0))
        dl->AddImage(ref(g_round[1]), ImVec2(c.x - rx, c.y - ry), ImVec2(c.x + rx, c.y + ry), ImVec2(0, 0), ImVec2(1, 1), col);
}

void fx_blot(ImDrawList *dl, ImVec2 c, float rx, float ry, ImU32 col)
{
    if ((g_round[0] != nullptr) && ((col & IM_COL32_A_MASK) != 0))
        dl->AddImage(ref(g_round[0]), ImVec2(c.x - rx, c.y - ry), ImVec2(c.x + rx, c.y + ry), ImVec2(0, 0), ImVec2(1, 1), col);
}

void fx_light_gradient(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom)
{
    if (g_box[1] == nullptr)
        return;
    const ImVec2 uv(0.5f, 0.5f); /* the mask's middle: plain white */
    dl->PushTexture(ref(g_box[1]));
    dl->PrimReserve(6, 4);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteIdx(i0);
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 1));
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
    dl->PrimWriteIdx(i0);
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 3));
    dl->PrimWriteVtx(a, uv, top);
    dl->PrimWriteVtx(ImVec2(b.x, a.y), uv, top);
    dl->PrimWriteVtx(b, uv, bottom);
    dl->PrimWriteVtx(ImVec2(a.x, b.y), uv, bottom);
    dl->PopTexture();
}

/** Colours the vertices from v0 on along p0-p1 from c0 to c1; a vertex's own alpha (the smoothed edge) is kept. */
static void shade(ImDrawList *dl, int v0, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1)
{
    const ImVec2 d(p1.x - p0.x, p1.y - p0.y);
    const float inv = 1.0f / std::max(d.x * d.x + d.y * d.y, 1e-6f);
    for (int i = v0; i < dl->VtxBuffer.Size; i++)
    {
        ImDrawVert &v = dl->VtxBuffer[i];
        const float t = clamp01(((v.pos.x - p0.x) * d.x + (v.pos.y - p0.y) * d.y) * inv);
        ImU32 out = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            float ch = ((c0 >> shift) & 0xFF) + (((c1 >> shift) & 0xFF) - (float)((c0 >> shift) & 0xFF)) * t;
            if (shift == IM_COL32_A_SHIFT)
                ch *= ((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            out |= ((ImU32)(ch + 0.5f) & 0xFF) << shift;
        }
        v.col = out;
    }
}

void fx_rect_gradient(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool horizontal, ImDrawFlags flags)
{
    if (((c0 | c1) & IM_COL32_A_MASK) == 0)
        return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    shade(dl, v0, a, horizontal ? ImVec2(b.x, a.y) : ImVec2(a.x, b.y), c0, c1);
}

void fx_line_fade(ImDrawList *dl, ImVec2 a, float width, ImU32 col, float thickness)
{
    const ImU32 none = col & ~IM_COL32_A_MASK;
    const float end = width * 0.22f, y1 = a.y + thickness;
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + end, y1), none, col, col, none);
    dl->AddRectFilled(ImVec2(a.x + end, a.y), ImVec2(a.x + width - end, y1), col);
    dl->AddRectFilledMultiColor(ImVec2(a.x + width - end, a.y), ImVec2(a.x + width, y1), col, none, none, col);
}

float fx_text_spaced(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float spacing)
{
    float x = 0;
    const char *end = text + strlen(text);
    for (const char *p = text; p < end;)
    {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0)
            break;
        if (dl != nullptr)
            dl->AddText(font, size, ImVec2(pos.x + x, pos.y), col, p, p + n);
        x += font->CalcTextSizeA(size, 4000.0f, 0.0f, p, p + n).x + spacing;
        p += n;
    }
    return (x > 0) ? x - spacing : 0;
}

int fx_group_begin(ImDrawList *dl)
{
    return dl->VtxBuffer.Size;
}

void fx_group_end(ImDrawList *dl, int start, float alpha, ImVec2 offset)
{
    if ((alpha >= 0.999f) && (offset.x == 0) && (offset.y == 0))
        return;
    alpha = clamp01(alpha);
    for (int i = start; i < dl->VtxBuffer.Size; i++)
    {
        ImDrawVert &v = dl->VtxBuffer[i];
        v.pos.x += offset.x;
        v.pos.y += offset.y;
        const ImU32 a = (ImU32)(((v.col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

/******************************************************************************/
/* the blurred backdrop: the frame goes into a picture of its own; in the middle of drawing it (after the background)
   it is halved four times and smeared, and the boxes are filled with that */

static const int MIPS = 4;
static SDL_Texture *g_scene = nullptr, *g_mip[MIPS + 1] = {};
static int g_scene_w = 0, g_scene_h = 0, g_mip_w = 0, g_mip_h = 0;
static bool g_target_failed = false, g_target_on = false; /* on: this frame is being drawn into the picture */

static void target_free()
{
    if (g_scene != nullptr)
        SDL_DestroyTexture(g_scene);
    g_scene = nullptr;
    for (SDL_Texture *&t : g_mip)
    {
        if (t != nullptr)
            SDL_DestroyTexture(t);
        t = nullptr;
    }
}

static SDL_Texture *target_texture(int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
    if (t != nullptr)
    {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}

void fx_target_begin(int width, int height)
{
    if ((g_r == nullptr) || g_target_failed || (width < 64) || (height < 64))
        return;
    if ((g_scene != nullptr) && ((width != g_scene_w) || (height != g_scene_h)))
        target_free();
    if (g_scene == nullptr)
    {
        g_scene_w = width;
        g_scene_h = height;
        g_scene = target_texture(width, height);
        bool ok = (g_scene != nullptr);
        for (int i = 0; ok && (i <= MIPS); i++)
        {
            const int shift = std::min(i + 1, MIPS);
            g_mip_w = std::max(4, width >> shift);
            g_mip_h = std::max(4, height >> shift);
            g_mip[i] = target_texture(g_mip_w, g_mip_h);
            ok = (g_mip[i] != nullptr);
        }
        if (!ok)
        {
            /* no such textures here: the looks do without the blur */
            target_free();
            g_target_failed = true;
            return;
        }
        SDL_SetTextureBlendMode(g_mip[MIPS], SDL_BLENDMODE_BLEND); /* (drawn with rounded, smoothed corners) */
    }
    SDL_SetRenderTarget(g_r, g_scene);
    g_target_on = true;
}

void fx_target_end()
{
    if (!g_target_on)
        return;
    g_target_on = false;
    SDL_SetRenderTarget(g_r, nullptr);
    SDL_RenderTexture(g_r, g_scene, nullptr, nullptr);
}

static void backdrop_callback(const ImDrawList *, const ImDrawCmd *)
{
    if (!g_target_on)
        return;
    SDL_Texture *src = g_scene;
    for (int i = 0; i < MIPS; i++)
    {
        SDL_SetRenderTarget(g_r, g_mip[i]);
        SDL_RenderTexture(g_r, src, nullptr, nullptr);
        src = g_mip[i];
    }
    /* nine copies a texel and a half apart, each weighing the same (the n-th at 1/n over the ones before) */
    SDL_SetRenderTarget(g_r, g_mip[MIPS]);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    static const float at[9][2] = {{0, 0}, {-1.5f, 0}, {1.5f, 0}, {0, -1.5f}, {0, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}, {-1.5f, 1.5f}, {1.5f, 1.5f}};
    for (int n = 0; n < 9; n++)
    {
        const SDL_FRect dst = {at[n][0], at[n][1], (float)g_mip_w, (float)g_mip_h};
        SDL_SetTextureAlphaModFloat(src, 1.0f / (float)(n + 1));
        SDL_RenderTexture(g_r, src, nullptr, &dst);
    }
    SDL_SetTextureAlphaModFloat(src, 1.0f);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(g_r, g_scene);
}

void fx_backdrop_capture(ImDrawList *background)
{
    if (!g_target_on)
        return;
    background->AddCallback(backdrop_callback, nullptr);
    background->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

bool fx_backdrop_draw(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImU32 tint)
{
    if (!g_target_on)
        return false;
    dl->AddImageRounded(ref(g_mip[MIPS]), a, b, ImVec2(a.x / g_scene_w, a.y / g_scene_h), ImVec2(b.x / g_scene_w, b.y / g_scene_h), tint, rounding);
    return true;
}

void fx_shutdown()
{
    target_free();
    for (int i = 0; i < 2; i++)
    {
        if (g_round[i] != nullptr)
            SDL_DestroyTexture(g_round[i]);
        if (g_box[i] != nullptr)
            SDL_DestroyTexture(g_box[i]);
        g_round[i] = g_box[i] = nullptr;
    }
}

/******************************************************************************/
/* particles */

struct Particle {
    float x, y, vx, vy, age, life, size, phase;
};
static std::vector<Particle> g_drift, g_sparks;
static int g_drift_kind = 0;
static float g_spark_due = 0;

static float rnd()
{
    return (float)rand() / ((float)RAND_MAX + 1.0f);
}

static float rnd(float lo, float hi)
{
    return lo + (hi - lo) * rnd();
}

/** An ember's colour as it cools: yellow-white, orange, dark red. */
static ImU32 ember_colour(float t, float alpha)
{
    const float r = 255.0f, g = (t < 0.5f) ? 214.0f - 220.0f * t : 104.0f - 150.0f * (t - 0.5f), b = std::max(0.0f, 130.0f - 330.0f * t);
    return IM_COL32((int)r, (int)std::max(g, 20.0f), (int)b, (int)(255.0f * clamp01(alpha)));
}

static Particle spawn(int kind, float w, float h, float s, bool anywhere)
{
    Particle p;
    p.phase = rnd(0.0f, 6.283f);
    p.age = 0;
    if (kind == FX_EMBERS)
    {
        /* embers rise off the bottom edge (the lava the picture stands on) and cool on their way up */
        p.x = rnd(-0.02f, 1.02f) * w;
        p.y = h + rnd(4.0f, 30.0f) * s;
        p.vx = rnd(-8.0f, 8.0f) * s;
        p.vy = -rnd(22.0f, 70.0f) * s;
        p.life = rnd(4.0f, 9.0f);
        p.size = rnd(1.0f, 2.6f) * s;
        if (anywhere)
        {
            p.age = rnd(0.0f, p.life);
            p.y += p.vy * p.age;
        }
    } else
    {
        /* gold dust hangs in the air */
        p.x = rnd() * w;
        p.y = rnd() * h;
        p.vx = rnd(-7.0f, 7.0f) * s;
        p.vy = rnd(-6.0f, 3.0f) * s;
        p.life = rnd(6.0f, 13.0f);
        p.size = rnd(0.8f, 1.9f) * s;
        if (anywhere)
            p.age = rnd(0.0f, p.life);
    }
    return p;
}

void fx_particles(ImDrawList *dl, int kind, float w, float h, float s)
{
    if (!fx_motion || (kind == 0))
    {
        g_drift.clear();
        g_drift_kind = 0;
        return;
    }
    const bool fresh = (kind != g_drift_kind);
    if (fresh)
        g_drift.clear();
    g_drift_kind = kind;
    const size_t want = (size_t)std::max(36.0f, std::min(200.0f, w * h / (s * s) / (1180.0f * 720.0f) * ((kind == FX_EMBERS) ? 80.0f : 56.0f)));
    while (g_drift.size() < want)
        g_drift.push_back(spawn(kind, w, h, s, fresh || (g_drift.size() < want / 2)));
    for (Particle &p : g_drift)
    {
        p.age += g_dt;
        if ((p.age >= p.life) || (p.y < -20.0f) || (p.x < -40.0f) || (p.x > w + 40.0f))
            p = spawn(kind, w, h, s, false);
        const float t = p.age / p.life;
        const float sway = sinf(p.phase + g_time * ((kind == FX_EMBERS) ? 1.3f : 0.5f)) * ((kind == FX_EMBERS) ? 14.0f : 5.0f) * s;
        p.x += (p.vx + sway) * g_dt;
        p.y += p.vy * g_dt;
        if (kind == FX_EMBERS)
        {
            p.vy *= 1.0f - 0.10f * g_dt; /* they slow as they cool */
            const float alpha = clamp01(t * 6.0f) * clamp01((1.0f - t) * 1.6f) * (0.72f + 0.28f * sinf(p.phase * 3.0f + g_time * 7.0f));
            fx_glow(dl, ImVec2(p.x, p.y), p.size * 5.0f, p.size * 5.0f, ember_colour(t, alpha * 0.34f));
            fx_glow(dl, ImVec2(p.x, p.y), p.size * 1.5f, p.size * 1.5f, ember_colour(t * 0.6f, alpha));
        } else
        {
            const float alpha = clamp01(t * 4.0f) * clamp01((1.0f - t) * 3.0f) * (0.45f + 0.55f * (0.5f + 0.5f * sinf(p.phase * 2.0f + g_time * 1.7f)));
            fx_glow(dl, ImVec2(p.x, p.y), p.size * 4.5f, p.size * 4.5f, IM_COL32(255, 206, 120, (int)(46.0f * alpha)));
            fx_glow(dl, ImVec2(p.x, p.y), p.size * 1.4f, p.size * 1.4f, IM_COL32(255, 232, 176, (int)(190.0f * alpha)));
        }
    }
}

void fx_sparks(ImVec2 a, ImVec2 b, float rate, float s)
{
    if (!fx_motion)
        return;
    g_spark_due += rate * g_dt;
    while ((g_spark_due >= 1.0f) && (g_sparks.size() < 90))
    {
        g_spark_due -= 1.0f;
        Particle p;
        p.x = rnd(a.x, b.x);
        p.y = rnd(a.y, b.y);
        p.vx = rnd(-14.0f, 14.0f) * s;
        p.vy = -rnd(34.0f, 92.0f) * s;
        p.age = 0;
        p.life = rnd(0.9f, 2.0f);
        p.size = rnd(0.9f, 2.0f) * s;
        p.phase = rnd(0.0f, 6.283f);
        g_sparks.push_back(p);
    }
    if (g_spark_due > 1.0f)
        g_spark_due = 0;
}

void fx_sparks_draw(ImDrawList *dl)
{
    if (!fx_motion)
        g_sparks.clear();
    for (size_t i = 0; i < g_sparks.size();)
    {
        Particle &p = g_sparks[i];
        p.age += g_dt;
        if (p.age >= p.life)
        {
            g_sparks[i] = g_sparks.back();
            g_sparks.pop_back();
            continue;
        }
        const float t = p.age / p.life;
        p.x += (p.vx + sinf(p.phase + g_time * 2.4f) * 10.0f) * g_dt;
        p.y += p.vy * g_dt;
        p.vy *= 1.0f - 0.5f * g_dt;
        const float alpha = clamp01(t * 8.0f) * (1.0f - t) * (1.0f - t);
        fx_glow(dl, ImVec2(p.x, p.y), p.size * 4.5f, p.size * 4.5f, ember_colour(t, alpha * 0.4f));
        fx_glow(dl, ImVec2(p.x, p.y), p.size * 1.4f, p.size * 1.4f, ember_colour(t * 0.5f, alpha));
        i++;
    }
}

/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkWorldViewRenderer.cpp
 *     The 3D world for Vulkan: GLWorldViewRenderer's render-thread draws, call for call, with the same shaders
 *     (VkProgram) and the same fixed-function state. The game-thread capture (DrawIsometricView, sprites,
 *     shadows, lens commands) is GLWorldViewRenderer's own, inherited.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkWorldViewRenderer.h"
#include "kfx/renderer/vulkan/VkProgram.h"
#include "kfx/renderer/ITileAtlas.h"
#include "kfx/renderer/RendererSettings.h"
#include "kfx/renderer/RendererManager.h"
#include "kfx/profiling/KfxProfiling.h"
#include "engine_buckets.h"
#include "engine_render.h"
#include "engine_textures.h"
#include "creature_graphics.h"
#include "bflib_video.h"
#include "bflib_basics.h"
#include "globals.h"
#include "vidmode.h"
#include "player_data.h"
#include "world_colours.h"
#include <algorithm>
#include <cstring>
#include "sprite_recolour.h"
#include "post_inc.h"

static constexpr int k_kspr_decode_dim = 256;
static uint8_t s_vk_kspr_decode_buf[k_kspr_decode_dim * k_kspr_decode_dim * 2];

// GLWorldViewRenderer.cpp's file-local helpers, the same code (they are static there).
static bool vk_decoded_sprite_darkens(int w, int h)
{
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = s_vk_kspr_decode_buf + y * k_kspr_decode_dim * 2;
        for (int x = 0; x < w; ++x)
            if (row[x * 2 + 1] != 0 && row[x * 2] >= 50 && row[x * 2] <= 56)
                return true;
    }
    return false;
}

static void vk_decode_keeper_rle(uint8_t* dst, const uint8_t* data, int w, int h)
{
    if (!data || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y)
        memset(dst + y * k_kspr_decode_dim * 2, 0, k_kspr_decode_dim * 2);
    const signed char* sp     = reinterpret_cast<const signed char*>(data);
    const signed char* sp_end = sp + (ptrdiff_t)w * h * 3 + h;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = dst + y * k_kspr_decode_dim * 2;
        int x = 0;
        while (true) {
            if (sp >= sp_end) {
                WARNLOG("decode_keeper_rle: ran past expected data end at row %d", y);
                return;
            }
            signed char cmd = *sp++;
            if (cmd == 0) break;
            if (cmd < 0) {
                x += (int)(-cmd);
            } else {
                int count = (int)cmd;
                for (int i = 0; i < count; ++i) {
                    if (sp >= sp_end) {
                        WARNLOG("decode_keeper_rle: pixel data past expected end");
                        return;
                    }
                    if (x < w) {
                        row[x * 2]     = (uint8_t)(*sp);
                        row[x * 2 + 1] = 255;
                    }
                    ++sp;
                    ++x;
                }
            }
        }
    }
}

namespace {
struct ShadowVkVertex { float x, y, u, v, layer, z_ndc, darken; };
}

struct VkWorldViewRenderer::Impl {
    VkCore* core = nullptr;
    VkProgram world, shadow;
    VkProgram kspr, kspr_glow, kspr_outline, kspr_edge;          // per-sprite fallback (single texture)
    VkProgram kspr_array, kspr_array_glow, kspr_array_outline, kspr_array_edge;
    VkProgram kspr_inst, kspr_inst_outline, kspr_inst_edge;
    VkProgram lens_mist, lens_remap, lens_overlay, lens_copy;
    uint8_t l_world = 0, l_shadow = 0, l_kspr[4] = {}, l_karr[4] = {}, l_inst = 0, l_inst_ol = 0, l_inst_edge = 0;
    uint8_t l_lens[4] = {};
    bool cleared_this_frame = false;   // the sprite atlas was cleared mid-frame: later uploads go in draw order
    bool depth_off = false;            // the cursor's sprites: OpenGL's depth test is off for them
    std::vector<ShadowVkVertex> shadow_verts;
};

/** The program's uniforms by name: shortcuts. */
static inline void set_f(VkProgram& p, const char* n, float x) { p.Set1f(n, x); }

static void mat3_from_xform(const PaletteTransform& xf, float out[9])
{
    // glUniformMatrix3fv(..., GL_TRUE, &m[0][0]): m is row-major; std140 wants columns
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            out[c * 3 + r] = xf.m[r][c];
}

VkWorldViewRenderer::~VkWorldViewRenderer()
{
    delete m_vk;
    m_vk = nullptr;
}

static VkVertexLayout world_layout()
{
    VkVertexLayout l;
    l.stride = sizeof(WorldVertex);
    l.attrs = {
        { "a_pos",      VK_FORMAT_R32G32B32_SFLOAT, 0,                  0 },
        { "a_uv",       VK_FORMAT_R32G32_SFLOAT,    3 * sizeof(float),  0 },
        { "a_shade",    VK_FORMAT_R32_SFLOAT,       5 * sizeof(float),  0 },
        { "a_stl",      VK_FORMAT_R32G32_SFLOAT,    6 * sizeof(float),  0 },
        { "a_camera_z", VK_FORMAT_R32_SFLOAT,       8 * sizeof(float),  0 },
        { "a_layer",    VK_FORMAT_R32_SFLOAT,       9 * sizeof(float),  0 },
        { "aWorldPos",  VK_FORMAT_R32G32B32_SFLOAT, 10 * sizeof(float), 0 },
        { "a_tile",     VK_FORMAT_R32_SFLOAT,       13 * sizeof(float), 0 },
    };
    return l;
}

bool VkWorldViewRenderer::init_gl_resources()
{
    if (m_initialized)
        return true;
    if (m_resource_mapper == nullptr || vk()->Core() == nullptr)
        return false;
    delete m_vk;
    m_vk = new Impl();
    m_vk->core = vk()->Core();
    if (!compile_world_shaders())
    {
        ERRORLOG("VkWorldViewRenderer: world pipelines failed");
        return false;
    }
    push_shade_uniforms();
    {
        GpuTextureDesc desc;
        desc.width = MAX_SUBTILES_X;
        desc.height = MAX_SUBTILES_Y;
        desc.format = GpuTextureFormat::R16UI;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.debug_name = "world_lightmap";
        m_lightmap_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
        if (m_resource_mapper->ResolveTexture(m_lightmap_tex_handle) == nullptr)
        {
            ERRORLOG("VkWorldViewRenderer: lightmap texture failed");
            return false;
        }
    }
    if (!init_keeper_sprite_shader())
    {
        ERRORLOG("VkWorldViewRenderer: keeper-sprite pipelines failed");
        return false;
    }
    if (!init_shadow_shader())
        WARNLOG("VkWorldViewRenderer: shadow pipeline unavailable -- creature shadows disabled");
    if (!init_lens_shaders())
        WARNLOG("VkWorldViewRenderer: lens pipelines unavailable -- possession lens distortion disabled");
    m_initialized = true;
    SYNCLOG("VkWorldViewRenderer: initialised");
    return true;
}

void VkWorldViewRenderer::free_gl_resources()
{
    GLWorldViewRenderer::free_gl_resources();
    if (m_vk != nullptr)
    {
        VkProgram* all[] = { &m_vk->world, &m_vk->shadow, &m_vk->kspr, &m_vk->kspr_glow, &m_vk->kspr_outline,
                             &m_vk->kspr_edge, &m_vk->kspr_array, &m_vk->kspr_array_glow, &m_vk->kspr_array_outline,
                             &m_vk->kspr_array_edge, &m_vk->kspr_inst, &m_vk->kspr_inst_outline, &m_vk->kspr_inst_edge,
                             &m_vk->lens_mist, &m_vk->lens_remap, &m_vk->lens_overlay, &m_vk->lens_copy };
        for (VkProgram* p : all)
            p->Destroy();
    }
}

bool VkWorldViewRenderer::compile_world_shaders()
{
    if (!m_vk->world.Create(m_vk->core, "world"))
        return false;
    m_vk->l_world = m_vk->world.AddLayout(world_layout());
    m_shader_handle = 0x7F00000000000201ull; // tells the shared code the world shader exists
    m_vk->world.Set1f("u_time", 0.0f);
    m_vk->world.Set1f("u_missing_tile", 0.0f);
    return true;
}

void VkWorldViewRenderer::push_shade_uniforms()
{
    if (m_vk == nullptr) return;
    VkProgram& w = m_vk->world;
    w.Set1f("u_fullbright",  g_renderer_settings.shade_fullbright);
    w.Set1f("u_ambient",     g_renderer_settings.shade_ambient);
    w.Set1f("u_shade_scale", g_renderer_settings.shade_scale);
    w.Set1f("u_shade_gamma", g_renderer_settings.shade_gamma);
    w.Set1i("u_lighting_mode", g_renderer_settings.lighting_mode);
    w.Set1i("u_darkness_mode", g_renderer_settings.darkness_mode);
    w.Set1f("u_fog_speed",   g_renderer_settings.fog_speed);
    w.Set1f("u_fog_density", g_renderer_settings.fog_density);
    w.Set1i("u_tile_filter", g_renderer_settings.tile_filter);
    m_tile_filter_applied = -1;
}

void VkWorldViewRenderer::RefreshFadeTableAndSettings()
{
    if (m_vk != nullptr)
    {
        VkImageObj* fade = vk()->TextureImage(m_fade_tex_handle);
        if (fade != nullptr)
            m_vk->core->UploadImage(*fade, 0, 0, 0, 256, 64, pixmap.fade_tables, 256, 1);
    }
    push_shade_uniforms();
}

/******************************************************************************/
/* shadows                                                                    */
/******************************************************************************/

bool VkWorldViewRenderer::init_shadow_shader()
{
    if (!m_vk->shadow.Create(m_vk->core, "shadow"))
        return false;
    VkVertexLayout l;
    l.stride = sizeof(ShadowVkVertex);
    l.attrs = {
        { "a_pos",    VK_FORMAT_R32G32_SFLOAT, 0,                 0 },
        { "a_uv",     VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float), 0 },
        { "a_layer",  VK_FORMAT_R32_SFLOAT,    4 * sizeof(float), 0 },
        { "a_z_ndc",  VK_FORMAT_R32_SFLOAT,    5 * sizeof(float), 0 },
        { "a_darken", VK_FORMAT_R32_SFLOAT,    6 * sizeof(float), 0 },
    };
    // the shader's own attribute names are matched; any it does not have are skipped
    m_vk->l_shadow = m_vk->shadow.AddLayout(l);
    m_shadow_shader_handle = 0x7F00000000000202ull;
    return true;
}

void VkWorldViewRenderer::draw_shadows_gpu()
{
    if (m_rt_shadow_cmds.empty() || m_vk == nullptr || !m_vk->shadow.Valid())
        return;
    std::vector<ShadowVkVertex>& sv = m_vk->shadow_verts;
    sv.clear();
    sv.reserve(m_rt_shadow_cmds.size() * 6);
    for (const auto& cmd : m_rt_shadow_cmds)
    {
        const int layer = resolve_atlas_layer(cmd.draw_idx, cmd.data, cmd.src_w, cmd.src_h);
        if (layer < 0)
            continue;
        const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int t : tri)
            sv.push_back({ cmd.x[t], cmd.y[t], cmd.u[t], cmd.v[t], (float)layer, cmd.z_ndc, cmd.darken });
    }
    if (sv.empty())
        return;
    VkProgram& p = m_vk->shadow;
    p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
    p.SetTexture("u_sprite", vk()->TextureImage(m_kspr_sprite_array_handle));
    VkPipeState st;
    st.Blend(VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_COLOR);
    st.Depth(true, false, VK_COMPARE_OP_LESS_OR_EQUAL); // shadows test against tile depth, don't occlude each other
    st.layout_id = m_vk->l_shadow;
    if (!p.Bind(st))
        return;
    m_vk->core->BindVertices(sv.data(), sv.size() * sizeof(ShadowVkVertex));
    vkCmdDraw(m_vk->core->Cmd(), (uint32_t)sv.size(), 1, 0, 0);
}

/******************************************************************************/
/* keeper sprites                                                             */
/******************************************************************************/

static VkVertexLayout kspr_layout()
{
    // 6 vertices x (pos, local px, map, src, flip) = 11 floats each
    VkVertexLayout l;
    l.stride = 11 * sizeof(float);
    l.attrs = {
        { "a_pos",   VK_FORMAT_R32G32_SFLOAT,       0,                  0 },
        { "a_local", VK_FORMAT_R32G32_SFLOAT,       2 * sizeof(float),  0 },
        { "a_map",   VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float),  0 },
        { "a_src",   VK_FORMAT_R32G32_SFLOAT,       8 * sizeof(float),  0 },
        { "a_flip",  VK_FORMAT_R32_SFLOAT,          10 * sizeof(float), 0 },
    };
    return l;
}

bool VkWorldViewRenderer::init_keeper_sprite_shader()
{
    Impl& v = *m_vk;
    const VkVertexLayout kl = kspr_layout();
    struct { VkProgram* p; const char* name; uint8_t* layout; } progs[] = {
        { &v.kspr, "kspr", &v.l_kspr[0] }, { &v.kspr_glow, "kspr_glow", &v.l_kspr[1] },
        { &v.kspr_outline, "kspr_outline", &v.l_kspr[2] }, { &v.kspr_edge, "kspr_edge", &v.l_kspr[3] },
        { &v.kspr_array, "kspr_array", &v.l_karr[0] }, { &v.kspr_array_glow, "kspr_array_glow", &v.l_karr[1] },
        { &v.kspr_array_outline, "kspr_array_outline", &v.l_karr[2] }, { &v.kspr_array_edge, "kspr_array_edge", &v.l_karr[3] },
    };
    for (auto& e : progs)
    {
        if (!e.p->Create(v.core, e.name))
            return false;
        *e.layout = e.p->AddLayout(kl);
    }
    // the shared code tests these handles; here they only say the programs exist
    m_kspr_shader_handle            = 0x7F00000000000210ull;
    m_kspr_glow_shader_handle       = 0x7F00000000000211ull;
    m_kspr_outline_shader_handle    = 0x7F00000000000212ull;
    m_kspr_edge_shader_handle       = 0x7F00000000000213ull;
    m_kspr_atlas_shader_handle      = 0x7F00000000000214ull;
    m_kspr_atlas_glow_shader_handle = 0x7F00000000000215ull;
    m_kspr_atlas_outline_shader_handle = 0x7F00000000000216ull;
    m_kspr_atlas_edge_shader_handle = 0x7F00000000000217ull;

    {
        static const uint8_t s_zero_256x256[256 * 256 * 2] = {};
        GpuTextureDesc desc;
        desc.width = 256;
        desc.height = 256;
        desc.format = GpuTextureFormat::RG8;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.initial_pixels.assign(s_zero_256x256, s_zero_256x256 + sizeof(s_zero_256x256));
        desc.debug_name = "kspr_sprite";
        m_kspr_sprite_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
        if (m_resource_mapper->ResolveTexture(m_kspr_sprite_tex_handle) == nullptr)
            return false;
    }
    // the sprite decode atlas: 2048 layers, fewer if the device allows fewer (as OpenGL does)
    {
        int want = k_kspr_atlas_layers;
        const char* forced = getenv("KFX_GL_KSPR_LAYERS");
        const int max_layers = (int)v.core->props.limits.maxImageArrayLayers;
        if (forced != NULL && atoi(forced) > 0)
            want = atoi(forced);
        else if (max_layers > 0 && want > max_layers)
            want = max_layers;
        m_kspr_atlas_layers = want;
        m_kspr_sprite_array_handle = kInvalidGpuResource;
        for (int layers = m_kspr_atlas_layers; layers >= 256; layers /= 2)
        {
            GpuTextureDesc array_desc;
            array_desc.width = k_kspr_decode_dim;
            array_desc.height = k_kspr_decode_dim;
            array_desc.array_layers = layers;
            array_desc.format = GpuTextureFormat::RG8;
            array_desc.min_filter = GpuTextureFilter::Nearest;
            array_desc.mag_filter = GpuTextureFilter::Nearest;
            array_desc.wrap = GpuTextureWrap::Clamp;
            array_desc.debug_name = "kspr_sprite_array";
            const GpuResourceHandle handle = m_resource_mapper->RequestCreateTexture(array_desc);
            if (m_resource_mapper->ResolveTexture(handle) != nullptr)
            {
                m_kspr_sprite_array_handle = handle;
                m_kspr_atlas_layers = layers;
                break;
            }
            WARNLOG("VkWorldViewRenderer: keeper-sprite atlas of %d layers refused, trying %d", layers, layers / 2);
        }
        if (m_kspr_sprite_array_handle == kInvalidGpuResource)
        {
            WARNLOG("VkWorldViewRenderer: no keeper-sprite atlas, the per-sprite path draws");
            m_kspr_atlas_shader_handle = kInvalidGpuResource;
            m_kspr_atlas_glow_shader_handle = kInvalidGpuResource;
        }
        else
            SYNCLOG("VkWorldViewRenderer: keeper-sprite decode atlas ready (%d layers, 256x256 RG8)", m_kspr_atlas_layers);
    }
    if (m_kspr_atlas_shader_handle != kInvalidGpuResource && m_kspr_clut_tex_handle == kInvalidGpuResource)
    {
        std::vector<uint8_t> zero_clut((size_t)256 * k_clut_rows * 4, 0);
        GpuTextureDesc desc;
        desc.width = 256;
        desc.height = k_clut_rows;
        desc.format = GpuTextureFormat::RGBA8;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.initial_pixels = std::move(zero_clut);
        desc.debug_name = "kspr_clut";
        m_kspr_clut_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
        m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle);
        m_kspr_clut_used = 1;
    }
    if (m_kspr_sprite_array_handle != kInvalidGpuResource && !init_keeper_sprite_instancing())
        WARNLOG("VkWorldViewRenderer: sprite instancing unavailable, using the per-sprite path");
    return true;
}

bool VkWorldViewRenderer::init_keeper_sprite_instancing()
{
    Impl& v = *m_vk;
    if (!v.kspr_inst.Create(v.core, "kspr_inst"))
        return false;
    static_assert(sizeof(KsprInstance) == 60, "KsprInstance layout changed -- update attrib offsets");
    VkVertexLayout il;
    il.stride = 2 * sizeof(float);          // the unit quad
    il.instance_stride = sizeof(KsprInstance);
    il.attrs = {
        { "a_corner", VK_FORMAT_R32G32_SFLOAT,       0,  0 },
        { "a_rect",   VK_FORMAT_R32G32B32A32_SFLOAT, 0,  1 },
        { "a_src",    VK_FORMAT_R32G32_SFLOAT,       16, 1 },
        { "a_misc",   VK_FORMAT_R32G32B32A32_SFLOAT, 40, 1 },
        { "a_flags",  VK_FORMAT_R32_UINT,            56, 1 },
        { "a_map",    VK_FORMAT_R32G32B32A32_SFLOAT, 24, 1 },
    };
    v.l_inst = v.kspr_inst.AddLayout(il);
    m_kspr_inst_shader_handle = 0x7F00000000000220ull;

    static_assert(sizeof(KsprOutlineInstance) == 68, "KsprOutlineInstance layout changed -- update attrib offsets");
    VkVertexLayout ol;
    ol.stride = 2 * sizeof(float);
    ol.instance_stride = sizeof(KsprOutlineInstance);
    ol.attrs = {
        { "a_corner", VK_FORMAT_R32G32_SFLOAT,       0,  0 },
        { "a_rect",   VK_FORMAT_R32G32B32A32_SFLOAT, 0,  1 },
        { "a_src",    VK_FORMAT_R32G32_SFLOAT,       16, 1 },
        { "a_lzf",    VK_FORMAT_R32G32B32_SFLOAT,    40, 1 },
        { "a_color",  VK_FORMAT_R32G32B32A32_SFLOAT, 52, 1 },
        { "a_map",    VK_FORMAT_R32G32B32A32_SFLOAT, 24, 1 },
    };
    if (v.kspr_inst_outline.Create(v.core, "kspr_inst_outline"))
    {
        v.l_inst_ol = v.kspr_inst_outline.AddLayout(ol);
        m_kspr_inst_outline_shader_handle = 0x7F00000000000221ull;
    }
    if (v.kspr_inst_edge.Create(v.core, "kspr_inst_edge"))
    {
        v.l_inst_edge = v.kspr_inst_edge.AddLayout(ol);
        m_kspr_inst_edge_shader_handle = 0x7F00000000000222ull;
    }
    SYNCLOG("VkWorldViewRenderer: instanced keeper-sprite path ready");
    return true;
}

// Uploads the decoded sprite into an atlas layer: before the frame's draws, or in draw order once the atlas was
// cleared this frame (a reused layer may already be drawn from earlier in the frame).
static void upload_sprite_layer(VkCore& core, VkImageObj& arr, int layer, int src_w, bool inline_upload)
{
    if (inline_upload)
        core.UploadImageInline(arr, 0, 0, layer, src_w, k_kspr_decode_dim, s_vk_kspr_decode_buf, k_kspr_decode_dim * 2, 2);
    else
        core.UploadImage(arr, 0, 0, layer, src_w, k_kspr_decode_dim, s_vk_kspr_decode_buf, k_kspr_decode_dim * 2, 2);
}

void VkWorldViewRenderer::execute_preload_atlas()
{
    if (m_vk == nullptr) return;
    VkImageObj* arr = vk()->TextureImage(m_kspr_sprite_array_handle);
    if (!arr || m_kspr_atlas_shader_handle == kInvalidGpuResource)
    {
        ERRORLOG("execute_preload_atlas: resources not ready -- preload skipped");
        return;
    }
    ensure_clut_valid();
    int preloaded = 0;
    for (int i = 0; i < KEEPSPRITE_LENGTH && m_kspr_atlas_used < m_kspr_atlas_layers / 2; i++)
    {
        if (!keepsprite[i] || !*keepsprite[i]) continue;
        if ((size_t)i >= creature_table_length) continue;
        const struct KeeperSprite& ks = creature_table[i];
        if (ks.SWidth <= 0 || ks.SHeight <= 0 || ks.SWidth > k_kspr_decode_dim || ks.SHeight > k_kspr_decode_dim) continue;
        if (m_kspr_atlas_map.count(i)) continue;
        memset(s_vk_kspr_decode_buf, 0, sizeof(s_vk_kspr_decode_buf));
        vk_decode_keeper_rle(s_vk_kspr_decode_buf, *keepsprite[i], ks.SWidth, ks.SHeight);
        const int layer = m_kspr_atlas_used++;
        upload_sprite_layer(*m_vk->core, *arr, layer, ks.SWidth, m_vk->cleared_this_frame);
        m_kspr_atlas_map[i] = { layer, ks.SWidth, vk_decoded_sprite_darkens(ks.SWidth, ks.SHeight) };
        preloaded++;
    }
    for (int i = 0; i < KEEPERSPRITE_ADD_NUM && m_kspr_atlas_used < m_kspr_atlas_layers / 2; i++)
    {
        if (!keepersprite_add[i]) continue;
        const struct KeeperSprite& ks = creature_table_add[i];
        if (ks.SWidth <= 0 || ks.SHeight <= 0 || ks.SWidth > k_kspr_decode_dim || ks.SHeight > k_kspr_decode_dim) continue;
        const int32_t sprite_id = KEEPERSPRITE_ADD_OFFSET + i;
        if (m_kspr_atlas_map.count(sprite_id)) continue;
        memset(s_vk_kspr_decode_buf, 0, sizeof(s_vk_kspr_decode_buf));
        vk_decode_keeper_rle(s_vk_kspr_decode_buf, keepersprite_add[i], ks.SWidth, ks.SHeight);
        const int layer = m_kspr_atlas_used++;
        upload_sprite_layer(*m_vk->core, *arr, layer, ks.SWidth, m_vk->cleared_this_frame);
        m_kspr_atlas_map[sprite_id] = { layer, ks.SWidth, vk_decoded_sprite_darkens(ks.SWidth, ks.SHeight) };
        preloaded++;
    }
    if (m_kspr_atlas_used > m_kspr_atlas_peak)
        m_kspr_atlas_peak = m_kspr_atlas_used;
    SYNCLOG("VkWorldViewRenderer: atlas preload complete -- %d sprites uploaded (%d/%d layers used)",
            preloaded, m_kspr_atlas_used, m_kspr_atlas_layers);
}

void VkWorldViewRenderer::ensure_clut_valid()
{
    if (m_vk == nullptr) return;
    VkImageObj* clut = vk()->TextureImage(m_kspr_clut_tex_handle);
    if (!clut) return;
    if (memcmp(m_rt_palette, m_kspr_clut_palette_snap, sizeof(m_rt_palette)) == 0) return;
    memcpy(m_kspr_clut_palette_snap, m_rt_palette, sizeof(m_rt_palette));
    uint8_t row[256 * 4];
    for (int i = 0; i < 256; i++) {
        row[i*4+0] = RendererPaletteChannel8(m_rt_palette[i*3+0]);
        row[i*4+1] = RendererPaletteChannel8(m_rt_palette[i*3+1]);
        row[i*4+2] = RendererPaletteChannel8(m_rt_palette[i*3+2]);
        row[i*4+3] = 255;
    }
    m_vk->core->UploadImage(*clut, 0, 0, 0, 256, 1, row, sizeof(row), 4);
    m_kspr_clut_remaps.clear();
    m_kspr_clut_used = 1;
}

int VkWorldViewRenderer::resolve_atlas_layer(int32_t sprite_id, const unsigned char* data, int src_w, int src_h,
                                             bool* darkens)
{
    if (darkens) *darkens = false;
    if (m_vk == nullptr) return -1;
    VkImageObj* arr = vk()->TextureImage(m_kspr_sprite_array_handle);
    VkImageObj* clut = vk()->TextureImage(m_kspr_clut_tex_handle);
    if (!arr || m_kspr_atlas_shader_handle == kInvalidGpuResource || !clut)
        return -1;
    if (sprite_id < 0)
        return -1;
    auto it = m_kspr_atlas_map.find(sprite_id);
    if (it != m_kspr_atlas_map.end())
    {
        m_kspr_atlas_hits++;
        if (darkens) *darkens = it->second.darkens;
        return it->second.layer;
    }
    if (m_kspr_atlas_used >= m_kspr_atlas_layers)
        return -1;
    memset(s_vk_kspr_decode_buf, 0, sizeof(s_vk_kspr_decode_buf));
    vk_decode_keeper_rle(s_vk_kspr_decode_buf, data, src_w, src_h);
    const int atlas_layer = m_kspr_atlas_used++;
    if (m_kspr_atlas_used > m_kspr_atlas_peak)
        m_kspr_atlas_peak = m_kspr_atlas_used;
    upload_sprite_layer(*m_vk->core, *arr, atlas_layer, src_w, m_vk->cleared_this_frame);
    const bool sprite_darkens = vk_decoded_sprite_darkens(src_w, src_h);
    m_kspr_atlas_map[sprite_id] = { atlas_layer, src_w, sprite_darkens };
    if (darkens) *darkens = sprite_darkens;
    m_kspr_atlas_misses++;
    return atlas_layer;
}

float VkWorldViewRenderer::resolve_clut_v(const unsigned char* remap)
{
    float clut_v = 0.5f / (float)k_clut_rows;
    if (m_vk == nullptr) return clut_v;
    VkImageObj* clut = vk()->TextureImage(m_kspr_clut_tex_handle);
    if (!remap || !clut)
        return clut_v;
    for (size_t i = 0; i < m_kspr_clut_remaps.size(); ++i)
        if (memcmp(m_kspr_clut_remaps[i].data(), remap, 256) == 0)
            return (float(i + 1) + 0.5f) / (float)k_clut_rows;
    if (m_kspr_clut_used >= k_clut_rows)
        return clut_v;
    const int row_idx = m_kspr_clut_used++;
    uint8_t row_data[256 * 4];
    for (int ci = 0; ci < 256; ci++) {
        int ri = remap[ci];
        row_data[ci*4+0] = RendererPaletteChannel8(m_rt_palette[ri*3+0]);
        row_data[ci*4+1] = RendererPaletteChannel8(m_rt_palette[ri*3+1]);
        row_data[ci*4+2] = RendererPaletteChannel8(m_rt_palette[ri*3+2]);
        row_data[ci*4+3] = 255;
    }
    sprite_recolour_row_pal6(remap, m_rt_palette, row_data); /* Endless: a registered recolour in true colour */
    // a new row: nothing drawn earlier this frame reads it
    m_vk->core->UploadImage(*clut, 0, row_idx, 0, 256, 1, row_data, sizeof(row_data), 4);
    std::array<uint8_t, 256> remap_copy;
    memcpy(remap_copy.data(), remap, remap_copy.size());
    m_kspr_clut_remaps.push_back(remap_copy);
    return (float(row_idx) + 0.5f) / (float)k_clut_rows;
}

void VkWorldViewRenderer::flush_keeper_sprite_instances()
{
    if (m_kspr_instances.empty() && m_kspr_outline_instances.empty())
        return;
    if (m_vk == nullptr) return;
    Impl& v = *m_vk;
    VkCore& core = *v.core;
    static const float k_unit_quad[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    VkImageObj* clut = vk()->TextureImage(m_kspr_clut_tex_handle);
    VkImageObj* arr = vk()->TextureImage(m_kspr_sprite_array_handle);
    // premultiplied alpha: one blend for normal sprites (rgb*a, a) and additive glow (rgb, 0); depth test, no write
    VkPipeState base;
    base.Blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    base.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    if (!m_kspr_outline_instances.empty())
    {
        const bool use_edge = (g_renderer_settings.creature_outline_mode == RENDERER_OUTLINE_EDGE);
        VkProgram& p = use_edge ? v.kspr_inst_edge : v.kspr_inst_outline;
        if (p.Valid())
        {
            p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
            p.SetTexture("u_sprite", arr);
            VkPipeState st = base;
            st.Depth(true, false, v.depth_off ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_GREATER);
            st.layout_id = use_edge ? v.l_inst_edge : v.l_inst_ol;
            if (p.Bind(st))
            {
                core.BindVertices(k_unit_quad, sizeof(k_unit_quad), 0);
                core.BindVertices(m_kspr_outline_instances.data(), m_kspr_outline_instances.size() * sizeof(KsprOutlineInstance), 1);
                vkCmdDraw(core.Cmd(), 4, (uint32_t)m_kspr_outline_instances.size(), 0, 0);
            }
        }
    }
    if (!m_kspr_instances.empty() && v.kspr_inst.Valid())
    {
        VkProgram& p = v.kspr_inst;
        p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
        float m9[9];
        mat3_from_xform(m_rt_palette_xform, m9);
        p.SetMat3(p.Uniform("u_palette_xform"), m9);
        p.SetTexture("u_sprite", arr);
        p.SetTexture("u_clut", clut);
        size_t first = 0;
        for (const KsprInstanceRun& run : m_kspr_instance_runs)
        {
            VkPipeState st = base;
            st.Depth(true, false, v.depth_off ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS_OR_EQUAL);
            if (run.darken)
                st.color_op = st.alpha_op = VK_BLEND_OP_REVERSE_SUBTRACT;
            st.layout_id = v.l_inst;
            if (p.Bind(st))
            {
                core.BindVertices(k_unit_quad, sizeof(k_unit_quad), 0);
                core.BindVertices(m_kspr_instances.data() + first, run.count * sizeof(KsprInstance), 1);
                vkCmdDraw(core.Cmd(), 4, (uint32_t)run.count, 0, 0);
            }
            first += run.count;
        }
    }
    m_kspr_instances.clear();
    m_kspr_instance_runs.clear();
    m_kspr_outline_instances.clear();
}

int VkWorldViewRenderer::render_keepersprite_gpu(const IRWorldKeeperSpriteCmd& cmd)
{
    if (m_vk == nullptr) return 1;
    Impl& v = *m_vk;
    VkCore& core = *v.core;
    const unsigned char* data = cmd.data;
    const int src_w = cmd.src_w;
    const int src_h = cmd.src_h;
    int32_t content_h = cmd.content_h;
    const unsigned int draw_flags = cmd.draw_flags;
    const unsigned char* remap = cmd.remap_enabled ? cmd.remap_table : nullptr;
    const float z_ndc = cmd.z_ndc;
    VkImageObj* sprite_tex = vk()->TextureImage(m_kspr_sprite_tex_handle);
    VkImageObj* arr = vk()->TextureImage(m_kspr_sprite_array_handle);
    VkImageObj* clut = vk()->TextureImage(m_kspr_clut_tex_handle);
    VkImageObj* pal = vk()->TextureImage(m_palette_tex_handle);
    if (!sprite_tex || !pal)
        return 1;
    if (src_w <= 0 || src_h <= 0 || src_w > k_kspr_decode_dim || src_h > k_kspr_decode_dim)
        return 1;
    if (cmd.dst_w <= 0 || cmd.dst_h <= 0) return 1;
    if (content_h <= 0 || content_h > src_h) content_h = src_h;

    const bool additive = (draw_flags & Lb_SPRITE_ALPHA_ADDITIVE) != 0;
    const bool use_remap = remap && (draw_flags & Lb_SPRITE_REMAP) && !additive;
    const int atlas_layer = resolve_atlas_layer(cmd.sprite_id, data, src_w, src_h);
    float clut_v = 0.5f / (float)k_clut_rows;
    if (atlas_layer >= 0 && use_remap && clut)
        clut_v = resolve_clut_v(remap);

    const float vx0 = (float)cmd.dst_x, vy0 = (float)cmd.dst_y;
    const float vw  = (float)cmd.dst_w, vh  = (float)cmd.dst_h;
    const float mpx = (float)cmd.phase_x, mpy = (float)cmd.phase_y;
    const float msx = (float)cmd.step_x,  msy = (float)cmd.step_y;
    const float sw  = (float)src_w, sh = (float)content_h;
    const float flip = (draw_flags & Lb_SPRITE_FLIP_HORIZ) ? 1.0f : 0.0f;
    const float sv[6][11] = {
        { vx0,      vy0,      0.0f, 0.0f, mpx, mpy, msx, msy, sw, sh, flip },
        { vx0 + vw, vy0,      vw,   0.0f, mpx, mpy, msx, msy, sw, sh, flip },
        { vx0 + vw, vy0 + vh, vw,   vh,   mpx, mpy, msx, msy, sw, sh, flip },
        { vx0,      vy0,      0.0f, 0.0f, mpx, mpy, msx, msy, sw, sh, flip },
        { vx0 + vw, vy0 + vh, vw,   vh,   mpx, mpy, msx, msy, sw, sh, flip },
        { vx0,      vy0 + vh, 0.0f, vh,   mpx, mpy, msx, msy, sw, sh, flip },
    };
    const float alpha = draw_flags_source_weight(draw_flags);
    float m9[9];
    mat3_from_xform(m_rt_palette_xform, m9);
    auto draw6 = [&](VkProgram& p, const VkPipeState& st) {
        if (!p.Bind(st)) return;
        core.BindVertices(sv, sizeof(sv));
        vkCmdDraw(core.Cmd(), 6, 1, 0, 0);
    };
    auto decode_upload = [&]() {
        vk_decode_keeper_rle(s_vk_kspr_decode_buf, data, src_w, src_h);
        if (use_remap)
            for (int y = 0; y < src_h; ++y) {
                uint8_t* row = s_vk_kspr_decode_buf + y * k_kspr_decode_dim * 2;
                for (int x = 0; x < src_w; ++x)
                    if (row[x * 2 + 1] != 0) row[x * 2] = remap[row[x * 2]];
            }
        // one reusable texture, overwritten per sprite: in draw order
        core.UploadImageInline(*sprite_tex, 0, 0, 0, src_w, src_h, s_vk_kspr_decode_buf, k_kspr_decode_dim * 2, 2);
    };

    bool uploaded = false;
    const bool wants_outline = cmd.wants_outline != 0;
    if (g_renderer_settings.creature_outline_mode != RENDERER_OUTLINE_NONE && !additive && wants_outline)
    {
        float oc_r = 0.9f, oc_g = 0.9f, oc_b = 0.9f;
        if (cmd.owner >= 0)
        {
            unsigned char color_idx = world_display_colour((PlayerNumber)cmd.owner);
            if (color_idx < 9)
            {
                uint8_t pal_idx = player_room_colours[color_idx];
                oc_r = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 0]) / 255.0f;
                oc_g = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 1]) / 255.0f;
                oc_b = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 2]) / 255.0f;
            }
        }
        const float oc_a = g_renderer_settings.creature_outline_alpha;
        const float outline_z = z_ndc + 0.002f;
        const bool use_edge = (g_renderer_settings.creature_outline_mode == RENDERER_OUTLINE_EDGE);
        VkPipeState st;
        st.Blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        st.Depth(true, false, VK_COMPARE_OP_GREATER);
        if (atlas_layer >= 0)
        {
            VkProgram& p = use_edge ? v.kspr_array_edge : v.kspr_array_outline;
            p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
            p.Set1f("u_z_ndc", outline_z);
            p.Set1f("u_layer", (float)atlas_layer);
            p.Set4f(p.Uniform("u_outline_color"), oc_r, oc_g, oc_b, oc_a);
            p.SetTexture("u_sprite", arr);
            st.layout_id = use_edge ? v.l_karr[3] : v.l_karr[2];
            draw6(p, st);
        }
        else
        {
            VkProgram& p = use_edge ? v.kspr_edge : v.kspr_outline;
            decode_upload();
            uploaded = true;
            p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
            p.Set1f("u_z_ndc", outline_z);
            p.Set4f(p.Uniform("u_outline_color"), oc_r, oc_g, oc_b, oc_a);
            p.SetTexture("u_sprite", sprite_tex);
            st.layout_id = use_edge ? v.l_kspr[3] : v.l_kspr[2];
            draw6(p, st);
        }
    }

    VkPipeState st;
    st.Depth(true, false, VK_COMPARE_OP_LESS_OR_EQUAL);
    if (atlas_layer >= 0)
    {
        if (additive)
        {
            VkProgram& p = v.kspr_array_glow;
            p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
            p.Set1f("u_z_ndc", z_ndc);
            p.Set1f("u_layer", (float)atlas_layer);
            p.SetMat3(p.Uniform("u_palette_xform"), m9);
            p.Set1i("u_darken", 0);
            p.SetTexture("u_sprite", arr);
            st.Blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
            st.layout_id = v.l_karr[1];
            draw6(p, st);
            p.Set1i("u_darken", 1);
            VkPipeState sd = st;
            sd.color_op = sd.alpha_op = VK_BLEND_OP_REVERSE_SUBTRACT;
            draw6(p, sd);
        }
        else
        {
            VkProgram& p = v.kspr_array;
            p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
            p.Set1f("u_alpha", alpha);
            p.Set1f("u_z_ndc", z_ndc);
            p.Set1f("u_layer", (float)atlas_layer);
            p.Set1f("u_clut_v", clut_v);
            p.SetTexture("u_clut", clut);
            p.SetTexture("u_sprite", arr);
            st.Blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
            st.layout_id = v.l_karr[0];
            draw6(p, st);
        }
        return 1;
    }
    if (!uploaded)
        decode_upload();
    if (additive)
    {
        VkProgram& p = v.kspr_glow;
        p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
        p.Set1f("u_z_ndc", z_ndc);
        p.SetMat3(p.Uniform("u_palette_xform"), m9);
        p.Set1i("u_darken", 0);
        p.SetTexture("u_sprite", sprite_tex);
        st.Blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
        st.layout_id = v.l_kspr[1];
        draw6(p, st);
        p.Set1i("u_darken", 1);
        VkPipeState sd = st;
        sd.color_op = sd.alpha_op = VK_BLEND_OP_REVERSE_SUBTRACT;
        draw6(p, sd);
    }
    else
    {
        VkProgram& p = v.kspr;
        p.Set2f("u_viewport", (float)m_draw_screen_w, (float)m_draw_screen_h);
        p.Set1f("u_alpha", alpha);
        p.Set1f("u_z_ndc", z_ndc);
        p.SetTexture("u_palette", pal);
        p.SetTexture("u_sprite", sprite_tex);
        st.Blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        st.layout_id = v.l_kspr[0];
        draw6(p, st);
    }
    return 1;
}

void VkWorldViewRenderer::DrawCursorKeeperSprites()
{
    if (m_rt_cursor_kspr_ir.empty() || m_vk == nullptr)
        return;
    ensure_clut_valid();
    const int saved_draw_w = m_draw_screen_w;
    const int saved_draw_h = m_draw_screen_h;
    const float saved_z    = m_current_sprite_z;
    m_draw_screen_w    = m_full_screen_w;
    m_draw_screen_h    = m_full_screen_h;
    m_current_sprite_z = -1.0f;
    m_vk->core->SetViewport(0.0f, 0.0f, (float)m_full_screen_w, (float)m_full_screen_h);
    // OpenGL draws these with the depth test off (both the outline and the sprite pass): depth_off below
    for (const IRWorldKeeperSpriteCmd& cmd : m_rt_cursor_kspr_ir)
        append_keeper_sprite_instance(cmd);
    m_vk->depth_off = true;
    flush_keeper_sprite_instances();
    m_vk->depth_off = false;
    m_draw_screen_w    = saved_draw_w;
    m_draw_screen_h    = saved_draw_h;
    m_current_sprite_z = saved_z;
}

/******************************************************************************/
/* the world pass                                                             */
/******************************************************************************/

void VkWorldViewRenderer::gpu_execute_passes(int vp_x, int vp_y_gl, int screen_w, int screen_h,
                                             const std::vector<WorldVertex>& tile_verts)
{
    KFX_ZONE_COLOR("VkWorldViewRenderer::gpu_execute_passes", KFX_COLOR_RENDER_GPU);
    if (m_vk == nullptr) return;
    Impl& v = *m_vk;
    VkCore& core = *v.core;
    v.cleared_this_frame = false;
    m_draw_screen_w = screen_w;
    m_draw_screen_h = screen_h;
    m_kspr_atlas_hits = 0;
    m_kspr_atlas_misses = 0;
    ensure_clut_valid();

    VkImageObj* lightmap = vk()->TextureImage(m_lightmap_tex_handle);
    if (!lightmap) return;
    core.SetViewport((float)vp_x, (float)vp_y_gl, (float)screen_w, (float)screen_h);
    // the lightmap snapshot (taken in FlipBuffers on the game thread)
    core.UploadImage(*lightmap, 0, 0, 0, k_lightmap_w, k_lightmap_h, m_rt_lightmap, k_lightmap_w * 2, 2);

    VkProgram& w = v.world;
    w.SetTexture("u_lightmap", lightmap);
    w.SetTexture("u_fade_table", vk()->TextureImage(m_fade_tex_handle));
    w.SetTexture("u_palette", vk()->TextureImage(m_palette_tex_handle));
    bool tiles_bound = false;
    for (const auto& cmd : m_rt_draw_cmds)
    {
        if (cmd.type == DrawCmd::CMD_TILES)
        {
            if (cmd.vert_count <= 0 || (size_t)(cmd.vert_start + cmd.vert_count) > tile_verts.size())
                continue;
            VkImageObj* atlas_img = (m_atlas && m_atlas->IsInitialized()) ? vk()->TextureImage(m_atlas->GetAtlasTextureArray()) : nullptr;
            w.Set1f("u_missing_tile", atlas_img == nullptr ? 1.0f : 0.0f);
            // the filter OpenGL sets on the atlas texture is the sampler here
            w.SetTexture("u_tile_atlas", atlas_img,
                         core.GetSampler(g_renderer_settings.tile_filter == RENDERER_FILTER_LINEAR, false));
            m_tile_filter_applied = g_renderer_settings.tile_filter;
            // tiles: drawn in order with depth writes and no depth test (GL_ALWAYS)
            VkPipeState st;
            st.Depth(true, true, VK_COMPARE_OP_ALWAYS);
            st.layout_id = v.l_world;
            if (!w.Bind(st))
                continue;
            if (!tiles_bound)
            {
                // the frame's vertices once, then every batch draws a range of them
                VkRingSlice s = core.RingAlloc(tile_verts.size() * sizeof(WorldVertex), 16);
                if (s.ptr == nullptr)
                    return;
                memcpy(s.ptr, tile_verts.data(), tile_verts.size() * sizeof(WorldVertex));
                vkCmdBindVertexBuffers(core.Cmd(), 0, 1, &s.buffer, &s.offset);
                tiles_bound = true;
            }
            vkCmdDraw(core.Cmd(), (uint32_t)cmd.vert_count, 1, (uint32_t)cmd.vert_start, 0);
        }
        else if (cmd.type == DrawCmd::CMD_PRELOAD_KSPR_ATLAS)
        {
            execute_preload_atlas();
            tiles_bound = false; // an inline upload may have restarted the pass (vertex bindings are kept, but be safe)
        }
        else if (cmd.type == DrawCmd::CMD_CLEAR_KSPR_ATLAS)
        {
            execute_clear_atlas();
            v.cleared_this_frame = true;
        }
        else if (cmd.type == DrawCmd::CMD_UPDATE_ANIMATED_TILES)
        {
            if (m_atlas)
                m_atlas->UpdateAnimatedTiles();
        }
    }

    draw_shadows_gpu();

    bool any_sprites = false;
    for (const auto& cmd : m_rt_draw_cmds)
        if (cmd.type == DrawCmd::CMD_IR_KEEPER_SPRITES) { any_sprites = true; break; }
    if (any_sprites)
    {
        // scissor to the world viewport: sprites must not bleed into the sidebar
        core.SetScissor(vp_x, vp_y_gl, screen_w, screen_h);
        core.SetScissorEnabled(true);
        auto sort_range = [&](const DrawCmd& cmd) -> const std::vector<int>& {
            m_kspr_sorted_idx.clear();
            const int end = cmd.sprite_ir_start + cmd.sprite_ir_count;
            for (int i = cmd.sprite_ir_start; i < end; ++i)
                m_kspr_sorted_idx.push_back(i);
            std::stable_sort(m_kspr_sorted_idx.begin(), m_kspr_sorted_idx.end(),
                             [this](int a, int b) {
                                 const uint64_t ka = m_rt_kspr_ir[a].sort_key;
                                 const uint64_t kb = m_rt_kspr_ir[b].sort_key;
                                 if ((ka >> 32) != (kb >> 32))
                                     return (ka >> 32) > (kb >> 32);
                                 return (uint32_t)ka < (uint32_t)kb;
                             });
            return m_kspr_sorted_idx;
        };
        const bool use_instancing = (m_kspr_inst_shader_handle != kInvalidGpuResource)
                                  && (m_kspr_sprite_array_handle != kInvalidGpuResource);
        if (use_instancing)
        {
            m_kspr_instances.clear();
            for (const auto& cmd : m_rt_draw_cmds)
            {
                if (cmd.type != DrawCmd::CMD_IR_KEEPER_SPRITES) continue;
                for (int i : sort_range(cmd))
                    append_keeper_sprite_instance(m_rt_kspr_ir[i]);
            }
            flush_keeper_sprite_instances();
        }
        else
        {
            for (const auto& cmd : m_rt_draw_cmds)
            {
                if (cmd.type != DrawCmd::CMD_IR_KEEPER_SPRITES) continue;
                for (int i : sort_range(cmd))
                    DrawKeeperSpriteGL(m_rt_kspr_ir[i]);
            }
        }
        core.SetScissorEnabled(false);
    }
    core.SetViewport(0.0f, 0.0f, (float)m_full_screen_w, (float)m_full_screen_h);
    m_rt_draw_cmds.clear();
    m_rt_kspr_ir.clear();
    m_rt_shadow_cmds.clear();
    m_cmd_vert_start = 0;
}

/******************************************************************************/
/* possession lens                                                            */
/******************************************************************************/

bool VkWorldViewRenderer::init_lens_shaders()
{
    Impl& v = *m_vk;
    VkVertexLayout l;
    l.stride = 4 * sizeof(float);
    l.attrs = {
        { "a_pos", VK_FORMAT_R32G32_SFLOAT, 0,                 0 },
        { "a_uv",  VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float), 0 },
    };
    bool any_ok = false;
    VkProgram* progs[4] = { &v.lens_mist, &v.lens_remap, &v.lens_overlay, &v.lens_copy };
    const char* names[4] = { "lens_mist", "lens_remap", "lens_overlay", "lens_copy" };
    GpuResourceHandle* handles[4] = { &m_lens_shader_mist_handle, &m_lens_shader_remap_handle,
                                      &m_lens_shader_overlay_handle, &m_lens_shader_copy_handle };
    for (int i = 0; i < 4; i++)
    {
        if (progs[i]->Create(v.core, names[i]))
        {
            v.l_lens[i] = progs[i]->AddLayout(l);
            *handles[i] = 0x7F00000000000230ull + (GpuResourceHandle)i;
            any_ok = true;
        }
        else
            WARNLOG("VkWorldViewRenderer: lens pipeline '%s' unavailable", names[i]);
    }
    return any_ok;
}

void VkWorldViewRenderer::upload_lens_textures_if_dirty()
{
    if (m_vk == nullptr) return;
    VkCore& core = *m_vk->core;
    switch (m_rt_lens_cmd.type)
    {
    case LensPixelEffectType::Mist:
        if (m_rt_lens_cmd.mist_pixels.size() != 256 * 256)
            break;
        if (m_lens_mist_tex_handle == kInvalidGpuResource)
        {
            GpuTextureDesc desc;
            desc.width = 256;
            desc.height = 256;
            desc.format = GpuTextureFormat::R8;
            desc.min_filter = GpuTextureFilter::Nearest;
            desc.mag_filter = GpuTextureFilter::Nearest;
            desc.wrap = GpuTextureWrap::Clamp;
            desc.debug_name = "lens_mist";
            m_lens_mist_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
            m_lens_mist_uploaded_version = 0xFFFFFFFFu;
        }
        if (m_rt_lens_cmd.mist_version != m_lens_mist_uploaded_version)
        {
            VkImageObj* img = vk()->TextureImage(m_lens_mist_tex_handle);
            if (img)
            {
                core.UploadImage(*img, 0, 0, 0, 256, 256, m_rt_lens_cmd.mist_pixels.data(), 256, 1);
                m_lens_mist_uploaded_version = m_rt_lens_cmd.mist_version;
            }
        }
        break;
    case LensPixelEffectType::Displacement:
    case LensPixelEffectType::Flyeye:
        if (m_rt_lens_cmd.remap_w <= 0 || m_rt_lens_cmd.remap_h <= 0 ||
            (size_t)(m_rt_lens_cmd.remap_w * m_rt_lens_cmd.remap_h * 2) != m_rt_lens_cmd.remap_pixels.size())
            break;
        if (m_rt_lens_cmd.remap_version != m_lens_remap_uploaded_version)
        {
            VkImageObj* img = vk()->TextureImage(m_lens_remap_tex_handle);
            if (img)
            {
                core.UploadImage(*img, 0, 0, 0, m_rt_lens_cmd.remap_w, m_rt_lens_cmd.remap_h,
                                 m_rt_lens_cmd.remap_pixels.data(), (uint32_t)m_rt_lens_cmd.remap_w * 4, 4);
                m_lens_remap_uploaded_version = m_rt_lens_cmd.remap_version;
            }
        }
        break;
    case LensPixelEffectType::Overlay:
        if (m_rt_lens_cmd.overlay_w <= 0 || m_rt_lens_cmd.overlay_h <= 0 ||
            (size_t)(m_rt_lens_cmd.overlay_w * m_rt_lens_cmd.overlay_h) != m_rt_lens_cmd.overlay_pixels.size())
            break;
        if (m_rt_lens_cmd.overlay_version != m_lens_overlay_uploaded_version)
        {
            VkImageObj* img = vk()->TextureImage(m_lens_overlay_tex_handle);
            if (img)
            {
                core.UploadImage(*img, 0, 0, 0, m_rt_lens_cmd.overlay_w, m_rt_lens_cmd.overlay_h,
                                 m_rt_lens_cmd.overlay_pixels.data(), (uint32_t)m_rt_lens_cmd.overlay_w, 1);
                m_lens_overlay_uploaded_version = m_rt_lens_cmd.overlay_version;
            }
        }
        break;
    default:
        break;
    }
}

bool VkWorldViewRenderer::BeginLensCapture(const float* clear_rgba)
{
    if (!m_rt_lens_cmd.active || m_vk == nullptr)
        return false;
    VkImageObj* color = vk()->TargetColor(m_lens_scene_rt_handle);
    VkImageObj* depth = vk()->TargetDepth(m_lens_scene_rt_handle);
    if (color == nullptr)
        return false;
    m_vk->core->BeginPass(color, depth, clear_rgba, true);
    return true;
}

void VkWorldViewRenderer::ResolveLensComposite()
{
    if (m_vk == nullptr) return;
    Impl& v = *m_vk;
    VkCore& core = *v.core;
    vk()->BindScreen();
    upload_lens_textures_if_dirty();
    VkImageObj* scene = vk()->TargetColor(m_lens_scene_rt_handle);
    if (scene == nullptr || m_rt_lens_cmd.viewport_w <= 0 || m_rt_lens_cmd.viewport_h <= 0)
        return;
    const float vp_x = (float)m_rt_lens_cmd.viewport_x;
    const float vp_w = (float)m_rt_lens_cmd.viewport_w;
    const float vp_h = (float)m_rt_lens_cmd.viewport_h;
    const int dest_vp_y_gl = m_full_screen_h - m_rt_lens_cmd.viewport_y - m_rt_lens_cmd.viewport_h;
    core.SetViewport((float)m_rt_lens_cmd.viewport_x, (float)dest_vp_y_gl, vp_w, vp_h);
    static const float k_quad[] = {
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
    };
    VkImageObj* index_img = vk()->TextureImage(m_palette_index_tex_handle);
    VkImageObj* pal = vk()->TextureImage(m_palette_tex_handle);
    VkImageObj* fade = vk()->TextureImage(m_fade_tex_handle);
    const bool have_index_tables = index_img && pal && fade;
    auto draw = [&](VkProgram& p, uint8_t layout) -> bool {
        VkPipeState st;
        st.layout_id = layout;
        if (!p.Bind(st)) return false;
        core.BindVertices(k_quad, sizeof(k_quad));
        vkCmdDraw(core.Cmd(), 6, 1, 0, 0);
        return true;
    };
    bool drawn = false;
    switch (m_rt_lens_cmd.type)
    {
    case LensPixelEffectType::Mist:
    {
        VkImageObj* mist = vk()->TextureImage(m_lens_mist_tex_handle);
        if (v.lens_mist.Valid() && mist && have_index_tables)
        {
            VkProgram& p = v.lens_mist;
            p.SetTexture("u_scene", scene);
            p.SetTexture("u_mist", mist);
            p.SetTexture("u_palette", pal);
            p.SetTexture("u_index_lookup", index_img);
            p.SetTexture("u_fade_table", fade);
            p.Set1f("u_src_x", vp_x);
            p.Set2f("u_view_size", vp_w, vp_h);
            p.Set2f("u_pos", m_rt_lens_cmd.mist_pos_x, m_rt_lens_cmd.mist_pos_y);
            p.Set2f("u_sec", m_rt_lens_cmd.mist_sec_x, m_rt_lens_cmd.mist_sec_y);
            p.Set1f("u_lightness", (float)m_rt_lens_cmd.mist_lightness);
            drawn = draw(p, v.l_lens[0]);
        }
        break;
    }
    case LensPixelEffectType::Displacement:
    case LensPixelEffectType::Flyeye:
    {
        VkImageObj* remap = vk()->TextureImage(m_lens_remap_tex_handle);
        if (v.lens_remap.Valid() && remap)
        {
            VkProgram& p = v.lens_remap;
            p.SetTexture("u_scene", scene);
            p.SetTexture("u_remap", remap);
            p.Set1f("u_src_x", vp_x);
            p.Set2f("u_view_size", vp_w, vp_h);
            drawn = draw(p, v.l_lens[1]);
        }
        break;
    }
    case LensPixelEffectType::Overlay:
    {
        VkImageObj* overlay = vk()->TextureImage(m_lens_overlay_tex_handle);
        if (v.lens_overlay.Valid() && overlay && have_index_tables)
        {
            VkProgram& p = v.lens_overlay;
            p.SetTexture("u_scene", scene);
            p.SetTexture("u_overlay", overlay);
            p.SetTexture("u_palette", pal);
            p.SetTexture("u_index_lookup", index_img);
            p.Set1f("u_src_x", vp_x);
            p.Set2f("u_view_size", vp_w, vp_h);
            p.Set1i("u_alpha", (int)(m_rt_lens_cmd.overlay_alpha * 256.0f + 0.5f));
            drawn = draw(p, v.l_lens[2]);
        }
        break;
    }
    default:
        break;
    }
    if (!drawn && v.lens_copy.Valid())
    {
        VkProgram& p = v.lens_copy;
        p.SetTexture("u_scene", scene);
        p.Set1f("u_src_x", vp_x);
        p.Set2f("u_view_size", vp_w, vp_h);
        p.Set1f("u_blur", m_rt_lens_cmd.blur_radius);
        draw(p, v.l_lens[3]);
    }
    // everything drawn after the lens (UI, overlays) expects the full screen
    core.SetViewport(0.0f, 0.0f, (float)m_full_screen_w, (float)m_full_screen_h);
}

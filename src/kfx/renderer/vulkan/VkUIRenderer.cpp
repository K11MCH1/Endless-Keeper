/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkUIRenderer.cpp
 *     UI quads, text and the sprite atlas for Vulkan: GLUIRenderer's draws, call for call, with the same shaders
 *     (VkProgram) and the same fixed-function state (blend, depth, scissor).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkUIRenderer.h"
#include "kfx/renderer/vulkan/VkProgram.h"
#include "kfx/renderer/ir/UICommands.h"
#include "kfx/renderer/ir/TextCommands.h"
#include "bflib_basics.h"
#include "globals.h"
#include <cstring>
#include "post_inc.h"

/******************************************************************************/
/* sprite atlas                                                               */
/******************************************************************************/

// Called with m_mutex held (GLSpriteAtlas::FlushPendingGL()); tex_id is the mapper's image id.
void VkSpriteAtlas::flush_dirty(unsigned int tex_id)
{
    if (m_dirty_y_min > m_dirty_y_max) return;
    const int h = m_dirty_y_max - m_dirty_y_min;
    if (h <= 0) return;
    VkResourceMapper* vm = static_cast<VkResourceMapper*>(m_resource_mapper);
    VkImageObj* img = vm ? vm->Image(tex_id) : nullptr;
    if (img != nullptr)
        vm->Core()->UploadImage(*img, 0, m_dirty_y_min, 0, k_atlas_w, h,
                                m_pixels.data() + (size_t)m_dirty_y_min * k_atlas_w, (uint32_t)k_atlas_w, 1);
    m_dirty_y_min = k_atlas_h;
    m_dirty_y_max = -1;
}

/******************************************************************************/
/* UI                                                                         */
/******************************************************************************/

struct VkUIRenderer::Impl {
    VkProgram sprite, colored, remap, clut, solid;
    uint8_t layout_sprite = 0, layout_colored = 0, layout_remap = 0, layout_clut = 0, layout_solid = 0;
    VkImageObj slab;     // R8, repeat
    VkImageObj minimap;  // R8, the size of the last minimap
    VkCore* core = nullptr;
    bool blend = false;  // OpenGL's GL_BLEND as the draws leave it
};

static VkVertexLayout ui_vertex_layout()
{
    // x,y (pos), u,v, r,g,b,a (colour), z (NDC depth) -- GLUIRenderer's UIVertexAttribs()
    VkVertexLayout l;
    l.stride = 9 * sizeof(float);
    l.attrs = {
        { "a_pos",   VK_FORMAT_R32G32_SFLOAT,       0,                 0 },
        { "a_uv",    VK_FORMAT_R32G32_SFLOAT,       2 * sizeof(float), 0 },
        { "a_color", VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float), 0 },
        { "a_z",     VK_FORMAT_R32_SFLOAT,          8 * sizeof(float), 0 },
    };
    return l;
}

bool VkUIRenderer::Init()
{
    if (m_resource_mapper == nullptr || vk()->Core() == nullptr)
        return false;
    delete m_vk;
    m_vk = new Impl();
    Impl& v = *m_vk;
    v.core = vk()->Core();
    if (!v.sprite.Create(v.core, "ui_sprite") || !v.colored.Create(v.core, "ui_colored") || !v.remap.Create(v.core, "ui_remap")
        || !v.clut.Create(v.core, "ui_clut") || !v.solid.Create(v.core, "ui_solid"))
        return false;
    const VkVertexLayout l = ui_vertex_layout();
    v.layout_sprite = v.sprite.AddLayout(l);
    v.layout_colored = v.colored.AddLayout(l);
    v.layout_remap = v.remap.AddLayout(l);
    v.layout_clut = v.clut.AddLayout(l);
    v.layout_solid = v.solid.AddLayout(l);
    // GLUIRenderer's shared code picks a program by these handles; here they only tell the programs apart
    m_shader_sprite_handle = 0x7F00000000000001ull;
    m_shader_sprite_colored_handle = 0x7F00000000000002ull;
    m_shader_remap_handle = 0x7F00000000000003ull;
    m_shader_clut_handle = 0x7F00000000000004ull;
    m_shader_solid_handle = 0x7F00000000000005ull;

    if (m_clut_tex_handle == kInvalidGpuResource)
    {
        std::vector<uint8_t> initial((size_t)256 * k_clut_rows * 4, 0);
        GpuTextureDesc clut_tex_desc;
        clut_tex_desc.width = 256;
        clut_tex_desc.height = k_clut_rows;
        clut_tex_desc.format = GpuTextureFormat::RGBA8;
        clut_tex_desc.min_filter = GpuTextureFilter::Nearest;
        clut_tex_desc.mag_filter = GpuTextureFilter::Nearest;
        clut_tex_desc.wrap = GpuTextureWrap::Clamp;
        clut_tex_desc.initial_pixels = std::move(initial);
        clut_tex_desc.debug_name = "ui_clut";
        m_clut_tex_handle = m_resource_mapper->RequestCreateTexture(clut_tex_desc);
    }
    return true;
}

void VkUIRenderer::Shutdown()
{
    if (m_atlas)
        m_atlas->SetLivenessQuery(nullptr);
    if (m_vk != nullptr)
    {
        m_vk->sprite.Destroy();
        m_vk->colored.Destroy();
        m_vk->remap.Destroy();
        m_vk->clut.Destroy();
        m_vk->solid.Destroy();
        if (m_vk->core)
        {
            m_vk->core->DestroyImage(m_vk->slab);
            m_vk->core->DestroyImage(m_vk->minimap);
        }
        delete m_vk;
        m_vk = nullptr;
    }
}

void VkUIRenderer::SetClip(int x, int y, int w, int h)
{
    if (m_vk == nullptr) return;
    if (w < 0) { m_vk->core->SetScissorEnabled(false); return; }
    m_vk->core->SetScissor(x, y, w, h);
    m_vk->core->SetScissorEnabled(true);
}

// The fixed-function state the UI draws use: blend as OpenGL left it, depth test (LEQUAL, no write) for the
// world-overlay layer.
static VkPipeState ui_state(bool blend, bool depth_test, uint8_t layout)
{
    VkPipeState st;
    if (blend)
        st.Blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    if (depth_test)
        st.Depth(true, false, VK_COMPARE_OP_LESS_OR_EQUAL);
    st.layout_id = layout;
    return st;
}

void VkUIRenderer::draw_textured_quad(GpuResourceHandle shader_handle, float x, float y, float w, float h,
                                      float u0, float v0, float u1, float v1,
                                      float r, float g, float b, float a,
                                      float remap_row)
{
    if (m_vk == nullptr) return;
    Impl& v = *m_vk;
    VkProgram* p = nullptr;
    uint8_t layout = 0;
    if (shader_handle == m_shader_clut_handle) { p = &v.clut; layout = v.layout_clut; }
    else if (shader_handle == m_shader_remap_handle) { p = &v.remap; layout = v.layout_remap; }
    else if (shader_handle == m_shader_sprite_colored_handle) { p = &v.colored; layout = v.layout_colored; }
    else { p = &v.sprite; layout = v.layout_sprite; }
    const float verts[6 * 9] = {
        x,     y,     u0, v0,  r, g, b, a, 0.0f,
        x + w, y,     u1, v0,  r, g, b, a, 0.0f,
        x + w, y + h, u1, v1,  r, g, b, a, 0.0f,
        x,     y,     u0, v0,  r, g, b, a, 0.0f,
        x + w, y + h, u1, v1,  r, g, b, a, 0.0f,
        x,     y + h, u0, v1,  r, g, b, a, 0.0f,
    };
    p->Set2f("u_screen_size", (float)m_screen_w, (float)m_screen_h);
    p->SetTexture("u_sprite_atlas", m_atlas ? vk()->TextureImage(m_atlas->GetTexture()) : nullptr);
    if (p == &v.sprite || p == &v.remap)
        p->SetTexture("u_palette", vk()->TextureImage(m_palette_tex_handle));
    if (p == &v.clut)
    {
        p->SetTexture("u_clut", vk()->TextureImage(m_clut_tex_handle));
        p->Set1f("u_clut_v", remap_row);
    }
    if (p == &v.remap)
    {
        p->SetTexture("u_fade_table", vk()->TextureImage(m_fade_table_tex_handle));
        p->Set1f("u_remap_row", remap_row);
    }
    // glyphs and the pointer: drawn with the state the UI passes leave (blend on, no depth test)
    if (!p->Bind(ui_state(true, false, layout)))
        return;
    v.core->BindVertices(verts, sizeof(verts));
    vkCmdDraw(v.core->Cmd(), 6, 1, 0, 0);
}

void VkUIRenderer::draw_solid_quad(float x, float y, float w, float h, float r, float g, float b, float a)
{
    if (m_vk == nullptr) return;
    Impl& v = *m_vk;
    const float verts[6 * 9] = {
        x,     y,     0, 0,  r, g, b, a, 0.0f,
        x + w, y,     0, 0,  r, g, b, a, 0.0f,
        x + w, y + h, 0, 0,  r, g, b, a, 0.0f,
        x,     y,     0, 0,  r, g, b, a, 0.0f,
        x + w, y + h, 0, 0,  r, g, b, a, 0.0f,
        x,     y + h, 0, 0,  r, g, b, a, 0.0f,
    };
    v.solid.Set2f("u_screen_size", (float)m_screen_w, (float)m_screen_h);
    if (!v.solid.Bind(ui_state(true, false, v.layout_solid)))
        return;
    v.core->BindVertices(verts, sizeof(verts));
    vkCmdDraw(v.core->Cmd(), 6, 1, 0, 0);
}

void VkUIRenderer::ensure_clut_valid()
{
    if (m_vk == nullptr || !m_frame_palette) return;
    VkImageObj* clut = vk()->TextureImage(m_clut_tex_handle);
    if (!clut) return;
    if (std::memcmp(m_frame_palette, m_clut_palette_snap, sizeof(m_clut_palette_snap)) == 0)
        return;
    std::memcpy(m_clut_palette_snap, m_frame_palette, sizeof(m_clut_palette_snap));
    uint8_t row[256 * 4];
    std::memcpy(row, m_frame_palette, sizeof(row));
    m_vk->core->UploadImage(*clut, 0, 0, 0, 256, 1, row, sizeof(row), 4);
    m_clut_remaps.clear();
    m_clut_used = 1;
}

int VkUIRenderer::resolve_clut_row(const unsigned char* cmap)
{
    if (!cmap || m_vk == nullptr)
        return 0;
    VkImageObj* clut = vk()->TextureImage(m_clut_tex_handle);
    if (!clut)
        return 0;
    ensure_clut_valid();
    for (size_t i = 0; i < m_clut_remaps.size(); ++i)
        if (std::memcmp(m_clut_remaps[i].data(), cmap, 256) == 0)
            return (int)i + 1;
    if (m_clut_used >= k_clut_rows)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            WARNLOG("VkUIRenderer: %d colour remaps in one frame: the rest draw uncoloured", k_clut_rows - 1);
        }
        return 0;
    }
    const int row_idx = m_clut_used++;
    uint8_t row[256 * 4];
    for (int i = 0; i < 256; ++i)
    {
        const unsigned int palette_index = cmap[i];
        row[i * 4 + 0] = m_frame_palette ? m_frame_palette[palette_index * 4 + 0] : 255;
        row[i * 4 + 1] = m_frame_palette ? m_frame_palette[palette_index * 4 + 1] : 255;
        row[i * 4 + 2] = m_frame_palette ? m_frame_palette[palette_index * 4 + 2] : 255;
        row[i * 4 + 3] = 255;
    }
    // a new row: no draw recorded before it in this frame reads it, so it can go before the frame's draws
    m_vk->core->UploadImage(*clut, 0, row_idx, 0, 256, 1, row, sizeof(row), 4);
    std::array<uint8_t, 256> copy;
    std::memcpy(copy.data(), cmap, copy.size());
    m_clut_remaps.push_back(copy);
    return row_idx;
}

void VkUIRenderer::FlushPendingSlabUpload()
{
    std::vector<uint8_t> pending;
    int dim = 0;
    {
        std::lock_guard<std::mutex> guard(m_slab_mutex);
        if (m_slab_pending_dim <= 0) return;
        pending.swap(m_slab_pending);
        dim = m_slab_pending_dim;
        m_slab_pending_dim = 0;
    }
    if (m_vk == nullptr) return;
    Impl& v = *m_vk;
    if (v.slab.image == VK_NULL_HANDLE)
    {
        if (!v.core->CreateImage(v.slab, dim, dim, 1, VK_FORMAT_R8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "slab_tile"))
            return;
        v.slab.sampler = v.core->GetSampler(false, true);
        v.core->InitSampledImage(v.slab);
        m_slab_dim = 0;
    }
    if (dim != m_slab_dim && m_slab_dim != 0)
    {
        WARNLOG("VkUIRenderer::FlushPendingSlabUpload: slab dimension changed at runtime (%d -> %d) -- not supported, dropping update",
                m_slab_dim, dim);
        return;
    }
    // index 0 -> 1: the sprite shader treats index 0 as transparent
    const int total = dim * dim;
    std::vector<uint8_t> slab_buf((size_t)total);
    for (int k = 0; k < total; ++k)
        slab_buf[(size_t)k] = pending[(size_t)k] ? pending[(size_t)k] : 1;
    v.core->UploadImage(v.slab, 0, 0, 0, dim, dim, slab_buf.data(), (uint32_t)dim, 1);
    m_slab_dim = dim;
}

bool VkUIRenderer::UploadMinimap(int slot, int size)
{
    if (slot < 0 || slot > 1 || size <= 0 || m_vk == nullptr) return false;
    const auto& buf = m_minimap_cpu_buf[slot];
    if (buf.size() != (size_t)size * (size_t)size) return false;
    Impl& v = *m_vk;
    if (v.minimap.image == VK_NULL_HANDLE || v.minimap.width != size)
    {
        v.core->DestroyImage(v.minimap);
        if (!v.core->CreateImage(v.minimap, size, size, 1, VK_FORMAT_R8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "minimap"))
            return false;
        v.minimap.sampler = v.core->GetSampler(false, false);
        v.core->InitSampledImage(v.minimap);
        m_minimap_tex_size = size;
    }
    // one minimap a frame, drawn after this: its upload can go before the frame's draws
    v.core->UploadImage(v.minimap, 0, 0, 0, size, size, buf.data(), (uint32_t)size, 1);
    return true;
}

void VkUIRenderer::FlushQuadRun(const std::vector<UIQuad>& run, PassType pass, int remap_row)
{
    if (run.empty() || m_vk == nullptr) return;
    Impl& v = *m_vk;
    VkProgram* p = nullptr;
    uint8_t layout = 0;
    VkImageObj* tex0 = nullptr;
    VkImageObj* atlas = m_atlas ? vk()->TextureImage(m_atlas->GetTexture()) : nullptr;
    bool bind_palette = false, bind_fade = false, bind_clut = false;
    switch (pass)
    {
    case PASS_SPRITE:  p = &v.sprite;  layout = v.layout_sprite;  tex0 = atlas; bind_palette = true; break;
    case PASS_SOLID:   p = &v.solid;   layout = v.layout_solid;   break;
    case PASS_COLORED: p = &v.colored; layout = v.layout_colored; tex0 = atlas; break;
    case PASS_REMAP:   p = &v.remap;   layout = v.layout_remap;   tex0 = atlas; bind_palette = true; bind_fade = true; break;
    case PASS_CLUT:    p = &v.clut;    layout = v.layout_clut;    tex0 = atlas; bind_palette = true; bind_clut = true; break;
    case PASS_SLAB:
        FlushPendingSlabUpload();
        if (v.slab.image == VK_NULL_HANDLE) return;
        p = &v.sprite; layout = v.layout_sprite; tex0 = &v.slab; bind_palette = true;
        break;
    case PASS_MINIMAP:
        if (v.minimap.image == VK_NULL_HANDLE) return;
        p = &v.sprite; layout = v.layout_sprite; tex0 = &v.minimap; bind_palette = true;
        break;
    }
    if (p == nullptr) return;

    std::vector<float> verts;
    verts.reserve(run.size() * 6 * 9);
    for (const UIQuad& q : run)
    {
        const float vv[6][9] = {
            { q.x0, q.y0, q.u0, q.v0, q.r, q.g, q.b, q.a, q.ndc_z },
            { q.x1, q.y0, q.u1, q.v0, q.r, q.g, q.b, q.a, q.ndc_z },
            { q.x1, q.y1, q.u1, q.v1, q.r, q.g, q.b, q.a, q.ndc_z },
            { q.x0, q.y0, q.u0, q.v0, q.r, q.g, q.b, q.a, q.ndc_z },
            { q.x1, q.y1, q.u1, q.v1, q.r, q.g, q.b, q.a, q.ndc_z },
            { q.x0, q.y1, q.u0, q.v1, q.r, q.g, q.b, q.a, q.ndc_z },
        };
        for (const auto& row : vv)
            for (float f : row)
                verts.push_back(f);
    }
    p->Set2f("u_screen_size", (float)m_screen_w, (float)m_screen_h);
    if (tex0)
        p->SetTexture("u_sprite_atlas", tex0);
    if (bind_palette)
        p->SetTexture("u_palette", vk()->TextureImage(m_palette_tex_handle));
    if (bind_fade)
    {
        p->SetTexture("u_fade_table", vk()->TextureImage(m_fade_table_tex_handle));
        p->Set1f("u_remap_row", (float)remap_row);
    }
    if (bind_clut)
    {
        p->SetTexture("u_clut", vk()->TextureImage(m_clut_tex_handle));
        p->Set1f("u_clut_v", ((float)remap_row + 0.5f) / (float)k_clut_rows);
    }
    if (!p->Bind(ui_state(v.blend, m_depth_test, layout)))
        return;
    v.core->BindVertices(verts.data(), verts.size() * sizeof(float));
    vkCmdDraw(v.core->Cmd(), (uint32_t)(run.size() * 6), 1, 0, 0);
}

void VkUIRenderer::FlushQuadLayer(std::vector<UIQuad>& quads, bool depth_test)
{
    if (quads.empty() || m_vk == nullptr) { quads.clear(); return; }
    m_vk->blend = true;
    m_depth_test = depth_test;
    std::vector<UIQuad> run;
    PassType run_pass = PASS_SPRITE;
    int run_remap_row = -1;
    for (const UIQuad& q : quads)
    {
        PassType p = classify(q.mode);
        bool remap_changed = ((p == PASS_REMAP || p == PASS_CLUT) && q.remap_row != run_remap_row);
        if (!run.empty() && (p != run_pass || remap_changed))
        {
            FlushQuadRun(run, run_pass, run_remap_row);
            run.clear();
        }
        if (run.empty()) { run_pass = p; run_remap_row = q.remap_row; }
        run.push_back(q);
    }
    if (!run.empty())
        FlushQuadRun(run, run_pass, run_remap_row);
    m_depth_test = false;
    m_vk->blend = false;
    quads.clear();
}

void VkUIRenderer::DrawWorldSpriteLayerRT()
{
    if (m_vk == nullptr) { m_quads[(int)IRUILayer::WorldOverlay].clear(); return; }
    // guards against bleeding into the overhead map / zoom box regions (OpenGL coordinates: bottom-left origin)
    const bool scissor = m_game_vp_set && m_game_vp_w > 0 && m_game_vp_h > 0;
    if (scissor)
        SetClip(m_game_vp_x, m_screen_h - m_game_vp_y - m_game_vp_h, m_game_vp_w, m_game_vp_h);
    FlushQuadLayer(m_quads[(int)IRUILayer::WorldOverlay], /*depth_test=*/true);
    if (scissor)
        SetClip(0, 0, -1, -1);
}

void VkUIRenderer::DrawWorldOverlayFlatLayerRT()
{
    if (m_vk == nullptr) { m_quads[(int)IRUILayer::WorldOverlayFlat].clear(); return; }
    const bool scissor = m_game_vp_set && m_game_vp_w > 0 && m_game_vp_h > 0;
    if (scissor)
        SetClip(m_game_vp_x, m_screen_h - m_game_vp_y - m_game_vp_h, m_game_vp_w, m_game_vp_h);
    FlushQuadLayer(m_quads[(int)IRUILayer::WorldOverlayFlat], /*depth_test=*/false);
    if (scissor)
        SetClip(0, 0, -1, -1);
}

void VkUIRenderer::DrawGameUIQuadsInterleaved(std::vector<UIQuad>& quads, const TextCommandBuffers& text,
                                              GLTextRenderer* text_renderer)
{
    if (m_vk == nullptr) { quads.clear(); return; }
    m_vk->blend = true;
    m_depth_test = false;
    const size_t qn = quads.size();
    const size_t tn = (text_renderer ? (size_t)text.draws.Size() : 0);
    size_t qi = 0, ti = 0;
    std::vector<UIQuad> run;
    PassType run_pass = PASS_SPRITE;
    int run_remap_row = -1;
    auto flush_run = [&]() {
        if (!run.empty()) { FlushQuadRun(run, run_pass, run_remap_row); run.clear(); }
    };
    while (qi < qn || ti < tn)
    {
        const bool take_quad = (ti >= tn) || (qi < qn && quads[qi].seq <= text.draws.Data()[ti].seq);
        if (take_quad)
        {
            const UIQuad& q = quads[qi++];
            PassType p = classify(q.mode);
            bool remap_changed = ((p == PASS_REMAP || p == PASS_CLUT) && q.remap_row != run_remap_row);
            if (!run.empty() && (p != run_pass || remap_changed))
                flush_run();
            if (run.empty()) { run_pass = p; run_remap_row = q.remap_row; }
            run.push_back(q);
        }
        else
        {
            flush_run();
            text_renderer->DrawGlyphs(text.draws.Data()[ti++], text);
        }
    }
    flush_run();
    m_vk->blend = false;
    quads.clear();
}

/******************************************************************************/
/* text                                                                       */
/******************************************************************************/

void VkTextRenderer::DrawGlyphs(const IRTextDrawCmd& cmd, const TextCommandBuffers& text)
{
    if (!m_ui || cmd.glyph_count == 0)
        return;
    if ((size_t)cmd.glyph_first + cmd.glyph_count > text.glyphs.Size())
        return;
    VkUIRenderer* ui = static_cast<VkUIRenderer*>(m_ui);
    const int screen_h = m_ui->GetScreenHeight();
    const int sx = cmd.clip_x;
    const int sy = screen_h - (cmd.clip_y + cmd.clip_h); // top-left origin -> OpenGL's bottom-left
    const int sw = cmd.clip_w;
    const int sh = cmd.clip_h;
    const bool scissor_valid = (sw > 0) && (sh > 0);
    if (scissor_valid)
        ui->SetClip(sx, sy, sw, sh);
    const IRTextGlyph* glyphs = text.glyphs.Data() + cmd.glyph_first;
    for (uint32_t i = 0; i < cmd.glyph_count; ++i)
    {
        const IRTextGlyph& glyph = glyphs[i];
        float r = 1.0f, g = 1.0f, b = 1.0f;
        if (glyph.kind != IRTextGlyphKind::PaletteSprite)
            m_ui->PaletteColour(glyph.colour, &r, &g, &b);
        switch (glyph.kind)
        {
        case IRTextGlyphKind::PaletteSprite:
            m_ui->DrawGlyphQuad(glyph.sprite, glyph.x, glyph.y, glyph.units_per_px, 1.0f, 1.0f, 1.0f, glyph.alpha, true, cmd.cmap);
            break;
        case IRTextGlyphKind::ColourSprite:
            m_ui->DrawGlyphQuad(glyph.sprite, glyph.x, glyph.y, glyph.units_per_px, r, g, b, glyph.alpha, false);
            break;
        case IRTextGlyphKind::SolidRect:
            m_ui->DrawSolidRect(glyph.x, glyph.y, glyph.w, glyph.h, r, g, b, glyph.alpha);
            break;
        }
    }
    if (scissor_valid)
        ui->SetClip(0, 0, -1, -1);
}

/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkPasses.cpp
 *     Vulkan versions of the full-screen passes and atlases.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkPasses.h"
#include "kfx/renderer/vulkan/VkProgram.h"
#include "kfx/renderer/TileAtlasPacker.h"
#include "kfx/renderer/RendererManager.h"
#include "engine_textures.h"   // block_ptrs, TEXTURE_VARIATIONS_COUNT
#include "bflib_basics.h"
#include <cstdlib>
#include <cstring>
#include "post_inc.h"

/******************************************************************************/
/* palette index lookup                                                       */
/******************************************************************************/

void VkPaletteIndexLookup::upload()
{
    VkResourceMapper* vm = static_cast<VkResourceMapper*>(m_mapper);
    VkImageObj* img = vm ? vm->TextureImage(m_tex_handle) : nullptr;
    if (img == nullptr || vm->Core() == nullptr)
        return;
    vm->Core()->UploadImage(*img, 0, 0, 0, img->width, img->height, m_table.data(), (uint32_t)img->width, 1);
}

/******************************************************************************/
/* tile atlas                                                                 */
/******************************************************************************/

// GLTileAtlas::Init() without its texture unbind.
bool VkTileAtlas::Init()
{
    if (m_initialized)
        return true;
    if (block_ptrs[0] == nullptr || !level_textures_ready)
        return false;
    if (!m_r8_scratch)
    {
        m_r8_scratch = (uint8_t*)malloc((size_t)k_atlas_w * k_atlas_h);
        if (!m_r8_scratch)
        {
            ERRORLOG("VkTileAtlas::Init: failed to allocate the scratch buffer");
            return false;
        }
    }
    if (m_resource_mapper == nullptr)
        return false;
    GpuTextureDesc desc;
    desc.width = k_atlas_w;
    desc.height = k_atlas_h;
    desc.array_layers = TEXTURE_VARIATIONS_COUNT;
    desc.format = GpuTextureFormat::R8;
    desc.min_filter = GpuTextureFilter::Nearest;
    desc.mag_filter = GpuTextureFilter::Nearest;
    desc.wrap = GpuTextureWrap::Clamp;
    desc.debug_name = "tile_atlas_array";
    m_texture_handle = m_resource_mapper->RequestCreateTexture(desc);
    if (m_resource_mapper->ResolveTexture(m_texture_handle) == nullptr)
    {
        ERRORLOG("VkTileAtlas::Init: tile atlas array texture realization failed");
        return false;
    }
    for (int v = 0; v < TEXTURE_VARIATIONS_COUNT; v++)
        BuildVariation(v);
    m_initialized = true;
    SYNCLOG("VkTileAtlas: loaded %d variations (%dx%dx%d R8 texture array)",
            TEXTURE_VARIATIONS_COUNT, k_atlas_w, k_atlas_h, TEXTURE_VARIATIONS_COUNT);
    return true;
}

void VkTileAtlas::Rebuild()
{
    if (!m_initialized || block_ptrs[0] == nullptr)
        return;
    for (int v = 0; v < TEXTURE_VARIATIONS_COUNT; v++)
        BuildVariation(v);
}

void VkTileAtlas::UploadFull(int variation)
{
    VkResourceMapper* vm = static_cast<VkResourceMapper*>(m_resource_mapper);
    VkImageObj* img = vm ? vm->TextureImage(m_texture_handle) : nullptr;
    if (img == nullptr)
        return;
    vm->Core()->UploadImage(*img, 0, 0, variation, k_atlas_w, k_atlas_h, m_r8_scratch, (uint32_t)k_atlas_w, 1);
}

void VkTileAtlas::UploadAnimatedStrip(int variation, int y_offset, int h_pixels)
{
    VkResourceMapper* vm = static_cast<VkResourceMapper*>(m_resource_mapper);
    VkImageObj* img = vm ? vm->TextureImage(m_texture_handle) : nullptr;
    if (img == nullptr)
        return;
    vm->Core()->UploadImage(*img, 0, y_offset, variation, k_atlas_w, h_pixels, m_r8_scratch, (uint32_t)k_atlas_w, 1);
}

/******************************************************************************/
/* image presents                                                             */
/******************************************************************************/

struct VkImagePresentPass::Impl {
    VkProgram blit, transparent, coverage, zoom;
    uint8_t lb = 0, lt = 0, lc = 0, lz = 0;
    VkCore* core = nullptr;
};

static VkVertexLayout rawimage_layout()
{
    VkVertexLayout l;
    l.stride = 4 * sizeof(float);
    l.attrs = {
        { "a_pos", VK_FORMAT_R32G32_SFLOAT, 0,                 0 },
        { "a_uv",  VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float), 0 },
    };
    return l;
}

VkImagePresentPass::~VkImagePresentPass()
{
    delete m_vk;
    m_vk = nullptr;
}

bool VkImagePresentPass::CompileShaders()
{
    if (m_resource_mapper == nullptr || vk()->Core() == nullptr)
        return false;
    delete m_vk;
    m_vk = new Impl();
    m_vk->core = vk()->Core();
    if (!m_vk->blit.Create(m_vk->core, "rawimage_blit") || !m_vk->transparent.Create(m_vk->core, "rawimage_transparent")
        || !m_vk->coverage.Create(m_vk->core, "rawimage_coverage") || !m_vk->zoom.Create(m_vk->core, "rawimage_zoom"))
        return false;
    const VkVertexLayout l = rawimage_layout();
    m_vk->lb = m_vk->blit.AddLayout(l);
    m_vk->lt = m_vk->transparent.AddLayout(l);
    m_vk->lc = m_vk->coverage.AddLayout(l);
    m_vk->lz = m_vk->zoom.AddLayout(l);
    // IsReady() and the shared Submit() test these handles; the draws use the programs above
    m_shader_handle = 0x7F00000000000101ull;
    m_transparent_shader_handle = 0x7F00000000000102ull;
    m_coverage_shader_handle = 0x7F00000000000103ull;
    m_zoom_shader_handle = 0x7F00000000000104ull;
    return true;
}

void VkImagePresentPass::Shutdown()
{
    GLImagePresentPass::Shutdown();
    if (m_vk != nullptr)
    {
        m_vk->blit.Destroy();
        m_vk->transparent.Destroy();
        m_vk->coverage.Destroy();
        m_vk->zoom.Destroy();
    }
}

bool VkImagePresentPass::init_quad() { return true; }

void VkImagePresentPass::upload_opaque_texture(size_t idx)
{
    if (m_vk == nullptr || idx >= m_rt_cmds.size() || idx >= m_opaque_slots.size()) return;
    VkImageObj* img = vk()->TextureImage(m_opaque_slots[idx].tex_handle);
    if (!img) return;
    m_vk->core->UploadImage(*img, 0, 0, 0, m_rt_cmds[idx].src_w, m_rt_cmds[idx].src_h, m_rt_cmds[idx].pixels.data(),
                            (uint32_t)m_rt_cmds[idx].src_w, 1);
}

void VkImagePresentPass::upload_overlay_texture()
{
    if (m_vk == nullptr) return;
    VkImageObj* img = vk()->TextureImage(m_overlay_tex_handle);
    if (!img) return;
    m_vk->core->UploadImage(*img, 0, 0, 0, m_rt_overlay_cmd.src_w, m_rt_overlay_cmd.src_h, m_rt_overlay_cmd.pixels.data(),
                            (uint32_t)m_rt_overlay_cmd.src_w, 1);
}

void VkImagePresentPass::upload_overlay_coverage_texture()
{
    if (m_vk == nullptr) return;
    VkImageObj* img = vk()->TextureImage(m_coverage_tex_handle);
    if (!img) return;
    m_vk->core->UploadImage(*img, 0, 0, 0, m_rt_overlay_cmd.src_w, m_rt_overlay_cmd.src_h, m_rt_overlay_cmd.coverage.data(),
                            (uint32_t)m_rt_overlay_cmd.src_w, 1);
}

void VkImagePresentPass::upload_zoom_texture()
{
    if (m_vk == nullptr) return;
    if (!m_rt_zoom_cmd.pixels.empty())
    {
        m_rt_zoom_pixels.swap(m_rt_zoom_cmd.pixels);
        m_rt_zoom_pixels_w = m_rt_zoom_cmd.src_w;
        m_rt_zoom_pixels_h = m_rt_zoom_cmd.src_h;
    }
    if (m_rt_zoom_pixels.empty())
        return;
    VkImageObj* img = vk()->TextureImage(m_zoom_tex_handle);
    if (!img) return;
    m_vk->core->UploadImage(*img, 0, 0, 0, m_rt_zoom_pixels_w, m_rt_zoom_pixels_h, m_rt_zoom_pixels.data(),
                            (uint32_t)m_rt_zoom_pixels_w, 1);
    m_rt_zoom_pixels.clear();
}

void VkImagePresentPass::upload_embedded_palette(const std::vector<unsigned char>& embedded_palette)
{
    if (m_vk == nullptr) return;
    if (m_embedded_palette_tex_handle == kInvalidGpuResource)
    {
        GpuTextureDesc desc;
        desc.width = 256;
        desc.height = 1;
        desc.format = GpuTextureFormat::RGBA8;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.debug_name = "fmv_embedded_palette";
        m_embedded_palette_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
    }
    VkImageObj* img = vk()->TextureImage(m_embedded_palette_tex_handle);
    if (!img || embedded_palette.size() < 256u * 4u) return;
    // the frame's palette is BGRA (OpenGL let the driver swap it with GL_BGRA)
    unsigned char rgba[256 * 4];
    for (int i = 0; i < 256; i++)
    {
        rgba[i * 4 + 0] = embedded_palette[i * 4 + 2];
        rgba[i * 4 + 1] = embedded_palette[i * 4 + 1];
        rgba[i * 4 + 2] = embedded_palette[i * 4 + 0];
        rgba[i * 4 + 3] = embedded_palette[i * 4 + 3];
    }
    // in draw order: several presents a frame may each bring their own palette
    m_vk->core->UploadImageInline(*img, 0, 0, 0, 256, 1, rgba, sizeof(rgba), 4);
}

void VkImagePresentPass::draw_quad(GLuint program, GLuint image_tex, GLuint palette_tex,
                                   int dst_x, int dst_y, int dst_w, int dst_h,
                                   int screen_w, int screen_h, GLuint coverage_tex)
{
    if (m_vk == nullptr) return;
    // here `program` is one of the handles set in CompileShaders() (low bits), the textures are image ids
    VkProgram* p = &m_vk->blit;
    uint8_t layout = m_vk->lb;
    VkPipeState st;
    const unsigned which = (unsigned)(program & 0xFFF);
    if (which == 0x102) { p = &m_vk->transparent; layout = m_vk->lt; st.Blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA); }
    else if (which == 0x103) { p = &m_vk->coverage; layout = m_vk->lc; st.Blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA); }
    st.layout_id = layout;
    const float x0 = (float)dst_x, y0 = (float)dst_y;
    const float x1 = (float)(dst_x + dst_w), y1 = (float)(dst_y + dst_h);
    const float verts[] = {
        x0, y0,  0.0f, 0.0f,
        x1, y0,  1.0f, 0.0f,
        x1, y1,  1.0f, 1.0f,
        x0, y0,  0.0f, 0.0f,
        x1, y1,  1.0f, 1.0f,
        x0, y1,  0.0f, 1.0f,
    };
    p->Set2f("u_screen_size", (float)screen_w, (float)screen_h);
    p->SetTexture("u_image", vk()->Image(image_tex));
    p->SetTexture("u_palette", vk()->Image(palette_tex));
    if (coverage_tex != 0)
        p->SetTexture("u_coverage", vk()->Image(coverage_tex));
    if (!p->Bind(st))
        return;
    m_vk->core->BindVertices(verts, sizeof(verts));
    vkCmdDraw(m_vk->core->Cmd(), 6, 1, 0, 0);
}

void VkImagePresentPass::Resolve(int screen_w, int screen_h)
{
    if (m_vk == nullptr || !IsActiveRT() || screen_w <= 0 || screen_h <= 0)
        return;
    VkImageObj* pal = vk()->TextureImage(m_palette_tex_handle);
    const GLTexture* pal_tex = m_resource_mapper->ResolveTexture(m_palette_tex_handle);
    if (pal == nullptr || pal_tex == nullptr)
        return;
    VkCore& core = *m_vk->core;
    core.SetViewport(0.0f, 0.0f, (float)screen_w, (float)screen_h);
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (m_rt_zoom_cmd.active)
    {
        upload_zoom_texture();
        VkImageObj* zoom_img = vk()->TextureImage(m_zoom_tex_handle);
        if (zoom_img != nullptr)
        {
            core.ClearColor(black);
            const float verts[] = {
                0.0f,            0.0f,             0.0f, 0.0f,
                (float)screen_w, 0.0f,             1.0f, 0.0f,
                (float)screen_w, (float)screen_h,  1.0f, 1.0f,
                0.0f,            0.0f,             0.0f, 0.0f,
                (float)screen_w, (float)screen_h,  1.0f, 1.0f,
                0.0f,            (float)screen_h,  0.0f, 1.0f,
            };
            VkProgram& z = m_vk->zoom;
            z.Set2f("u_screen_size", (float)screen_w, (float)screen_h);
            z.Set2f("u_zoom_center_map", m_rt_zoom_cmd.center_map_x, m_rt_zoom_cmd.center_map_y);
            z.Set2f("u_zoom_screen_center", m_rt_zoom_cmd.screen_cx, m_rt_zoom_cmd.screen_cy);
            z.Set1f("u_zoom_scale", m_rt_zoom_cmd.scale);
            z.Set2f("u_src_size", (float)m_rt_zoom_cmd.src_w, (float)m_rt_zoom_cmd.src_h);
            z.SetTexture("u_image", zoom_img);
            z.SetTexture("u_palette", pal);
            VkPipeState st;
            st.layout_id = m_vk->lz;
            if (z.Bind(st))
            {
                core.BindVertices(verts, sizeof(verts));
                vkCmdDraw(core.Cmd(), 6, 1, 0, 0);
            }
        }
    }
    else if (!m_rt_cmds.empty())
    {
        core.ClearColor(black);
        for (size_t i = 0; i < m_rt_cmds.size() && i < m_opaque_slots.size(); ++i)
        {
            upload_opaque_texture(i);
            const GLTexture* tex = m_resource_mapper->ResolveTexture(m_opaque_slots[i].tex_handle);
            if (tex == nullptr)
                continue;
            GLuint palette_tex = pal_tex->id;
            if (m_rt_cmds[i].palette == PRESENT_PALETTE_EMBEDDED)
            {
                upload_embedded_palette(m_rt_cmds[i].embedded_palette);
                const GLTexture* embedded_tex = m_resource_mapper->ResolveTexture(m_embedded_palette_tex_handle);
                if (embedded_tex != nullptr)
                    palette_tex = embedded_tex->id;
            }
            draw_quad((GLuint)(m_shader_handle & 0xFFFFFFFF), tex->id, palette_tex,
                      m_rt_cmds[i].dst_x, m_rt_cmds[i].dst_y, m_rt_cmds[i].dst_w, m_rt_cmds[i].dst_h, screen_w, screen_h);
        }
    }
}

void VkImagePresentPass::ResolveOverlay(int screen_w, int screen_h)
{
    if (m_vk == nullptr || !m_rt_overlay_cmd.active || screen_w <= 0 || screen_h <= 0)
        return;
    const GLTexture* pal_tex = m_resource_mapper->ResolveTexture(m_palette_tex_handle);
    if (pal_tex == nullptr)
        return;
    const bool use_coverage = !m_rt_overlay_cmd.coverage.empty();
    m_vk->core->SetViewport(0.0f, 0.0f, (float)screen_w, (float)screen_h);
    upload_overlay_texture();
    const GLTexture* overlay_tex = m_resource_mapper->ResolveTexture(m_overlay_tex_handle);
    GLuint coverage_tex_id = 0;
    if (use_coverage)
    {
        upload_overlay_coverage_texture();
        const GLTexture* coverage_tex = m_resource_mapper->ResolveTexture(m_coverage_tex_handle);
        coverage_tex_id = coverage_tex ? coverage_tex->id : 0;
    }
    if (overlay_tex != nullptr && (!use_coverage || coverage_tex_id != 0))
        draw_quad((GLuint)((use_coverage ? m_coverage_shader_handle : m_transparent_shader_handle) & 0xFFFFFFFF),
                  overlay_tex->id, pal_tex->id, m_rt_overlay_cmd.dst_x, m_rt_overlay_cmd.dst_y,
                  m_rt_overlay_cmd.dst_w, m_rt_overlay_cmd.dst_h, screen_w, screen_h, coverage_tex_id);
}

/******************************************************************************/
/* parchment transition                                                       */
/******************************************************************************/

struct VkMapFadePass::Impl {
    VkProgram prog;
    uint8_t layout = 0;
    VkCore* core = nullptr;
};

VkMapFadePass::~VkMapFadePass() { delete m_vk; m_vk = nullptr; }

bool VkMapFadePass::CompileShaders()
{
    if (m_resource_mapper == nullptr || vk()->Core() == nullptr)
        return false;
    delete m_vk;
    m_vk = new Impl();
    m_vk->core = vk()->Core();
    if (!m_vk->prog.Create(m_vk->core, "mapfade"))
        return false;
    VkVertexLayout l;
    l.stride = 4 * sizeof(float);
    l.attrs = {
        { "a_pos", VK_FORMAT_R32G32_SFLOAT, 0,                 0 },
        { "a_uv",  VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float), 0 },
    };
    m_vk->layout = m_vk->prog.AddLayout(l);
    m_shader_handle = 0x7F00000000000301ull; // IsReady()
    GpuTextureDesc ghost_desc;
    ghost_desc.width = 256;
    ghost_desc.height = 256;
    ghost_desc.format = GpuTextureFormat::R8;
    ghost_desc.min_filter = GpuTextureFilter::Nearest;
    ghost_desc.mag_filter = GpuTextureFilter::Nearest;
    ghost_desc.wrap = GpuTextureWrap::Clamp;
    ghost_desc.debug_name = "mapfade_ghost";
    m_ghost_tex_handle = m_resource_mapper->RequestCreateTexture(ghost_desc);
    return m_resource_mapper->ResolveTexture(m_ghost_tex_handle) != nullptr;
}

void VkMapFadePass::Shutdown()
{
    GLMapFadePass::Shutdown();
    if (m_vk != nullptr)
        m_vk->prog.Destroy();
}

bool VkMapFadePass::init_quad() { return true; }

void VkMapFadePass::CaptureParchmentFrame(int w, int h)
{
    if (m_vk == nullptr || w <= 0 || h <= 0) return;
    VkImageObj* dst = vk()->TargetColor(m_parchment_rt_handle);
    VkImageObj* src = vk()->ScreenColor();
    if (dst == nullptr || src == nullptr) return;
    m_vk->core->CopyImage(*src, *dst, w, h);
    vk()->BindScreen();
}

void VkMapFadePass::CaptureWorldFrame(int w, int h)
{
    if (m_vk == nullptr || w <= 0 || h <= 0) return;
    VkImageObj* dst = vk()->TextureImage(m_tex_world_handle);
    VkImageObj* src = vk()->ScreenColor();
    if (dst == nullptr || src == nullptr) return;
    m_vk->core->CopyImage(*src, *dst, w, h);
    vk()->BindScreen();
}

void VkMapFadePass::ResolveComposite(int screen_w, int screen_h)
{
    if (m_vk == nullptr) return;
    VkCore& core = *m_vk->core;
    VkImageObj* ghost = vk()->TextureImage(m_ghost_tex_handle);
    if (ghost != nullptr && m_rt_cmd.ghost_table.size() == 256u * 256u)
        core.UploadImage(*ghost, 0, 0, 0, 256, 256, m_rt_cmd.ghost_table.data(), 256, 1);
    VkImageObj* parchment = vk()->TargetColor(m_parchment_rt_handle);
    VkImageObj* world = vk()->TextureImage(m_tex_world_handle);
    VkImageObj* pal = vk()->TextureImage(m_palette_tex_handle);
    VkImageObj* fade = vk()->TextureImage(m_fade_table_tex_handle);
    VkImageObj* index_img = vk()->TextureImage(m_palette_index_tex_handle);
    if (!parchment || !world || !ghost || !pal || !fade || !index_img)
        return;
    vk()->BindScreen();
    core.SetViewport(0.0f, 0.0f, (float)screen_w, (float)screen_h);
    static const float k_quad[] = {
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
    };
    VkProgram& p = m_vk->prog;
    p.SetTexture("u_parchment", parchment);
    p.SetTexture("u_world", world);
    p.SetTexture("u_palette", pal);
    p.SetTexture("u_index_lookup", index_img);
    p.SetTexture("u_fade_table", fade);
    p.SetTexture("u_ghost", ghost);
    p.Set1f("u_step", m_rt_cmd.step);
    p.Set1f("u_warp_scale", m_rt_cmd.warp_scale);
    VkPipeState st;
    st.layout_id = m_vk->layout;
    if (!p.Bind(st))
        return;
    core.BindVertices(k_quad, sizeof(k_quad));
    vkCmdDraw(core.Cmd(), 6, 1, 0, 0);
}

/******************************************************************************/
/* zoom-box tiles                                                             */
/******************************************************************************/

struct VkZoomBoxTilesPass::Impl {
    VkProgram prog;
    uint8_t layout = 0;
    VkCore* core = nullptr;
};

VkZoomBoxTilesPass::~VkZoomBoxTilesPass() { delete m_vk; m_vk = nullptr; }

bool VkZoomBoxTilesPass::CompileShaders()
{
    if (m_resource_mapper == nullptr || vk()->Core() == nullptr)
        return false;
    delete m_vk;
    m_vk = new Impl();
    m_vk->core = vk()->Core();
    if (!m_vk->prog.Create(m_vk->core, "zoombox_tiles"))
        return false;
    VkVertexLayout l;
    l.stride = 5 * sizeof(float);
    l.attrs = {
        { "a_pos", VK_FORMAT_R32G32_SFLOAT,    0,                 0 },
        { "a_uvw", VK_FORMAT_R32G32B32_SFLOAT, 2 * sizeof(float), 0 },
    };
    m_vk->layout = m_vk->prog.AddLayout(l);
    m_shader_handle = 0x7F00000000000401ull; // IsReady()
    return true;
}

void VkZoomBoxTilesPass::Shutdown()
{
    GLZoomBoxTilesPass::Shutdown();
    if (m_vk != nullptr)
        m_vk->prog.Destroy();
}

void VkZoomBoxTilesPass::Resolve(int screen_w, int screen_h)
{
    if (!IsActiveRT() || m_vk == nullptr || !m_tile_atlas || !m_tile_atlas->IsInitialized())
        return;
    VkImageObj* atlas_img = vk()->TextureImage(m_tile_atlas->GetAtlasTextureArray());
    VkImageObj* pal = vk()->TextureImage(m_palette_tex_handle);
    if (!atlas_img || !pal || screen_w <= 0 || screen_h <= 0)
        return;
    m_vertex_scratch.clear();
    m_vertex_scratch.reserve((size_t)m_rt_cmd.tiles_x * (size_t)m_rt_cmd.tiles_y * 6 * 5);
    for (int ty = 0; ty < m_rt_cmd.tiles_y; ++ty)
    {
        for (int tx = 0; tx < m_rt_cmd.tiles_x; ++tx)
        {
            const uint16_t tile_id = m_rt_cmd.tile_block_ids[(size_t)ty * m_rt_cmd.tiles_x + tx];
            if (tile_id == 0xFFFF)
                continue;
            const int variation  = tile_id / TEXTURE_BLOCKS_COUNT;
            const int tile_local = tile_id % TEXTURE_BLOCKS_COUNT;
            float u0, v0, u1, v1;
            TileAtlasPacker::GetTileUV(tile_local, &u0, &v0, &u1, &v1);
            const float layer = (float)variation;
            const float x0 = (float)(m_rt_cmd.dst_x + tx * m_rt_cmd.tile_w);
            const float y0 = (float)(m_rt_cmd.dst_y + ty * m_rt_cmd.tile_h);
            const float x1 = x0 + (float)m_rt_cmd.tile_w;
            const float y1 = y0 + (float)m_rt_cmd.tile_h;
            const float verts[6][5] = {
                { x0, y0, u0, v0, layer },
                { x1, y0, u1, v0, layer },
                { x1, y1, u1, v1, layer },
                { x0, y0, u0, v0, layer },
                { x1, y1, u1, v1, layer },
                { x0, y1, u0, v1, layer },
            };
            for (const auto& row : verts)
                for (float f : row)
                    m_vertex_scratch.push_back(f);
        }
    }
    if (m_vertex_scratch.empty())
        return;
    VkProgram& p = m_vk->prog;
    p.Set2f("u_screen_size", (float)screen_w, (float)screen_h);
    p.SetTexture("u_atlas", atlas_img);
    p.SetTexture("u_palette", pal);
    VkPipeState st;
    st.layout_id = m_vk->layout;
    if (!p.Bind(st))
        return;
    m_vk->core->BindVertices(m_vertex_scratch.data(), m_vertex_scratch.size() * sizeof(float));
    vkCmdDraw(m_vk->core->Cmd(), (uint32_t)(m_vertex_scratch.size() / 5), 1, 0, 0);
}

/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file RendererVulkan.cpp
 *     The Vulkan backend. Mirrors RendererOpenGL.cpp: the game thread fills one of two frame slots with IR,
 *     PresentFrame() seals it and wakes the render thread, which replays it through RenderGraph::Execute()
 *     into a screen image at game size, then blits that into the window's present rect.
 *     The launcher's background (attract mode) has no swapchain: its frames are read back into shared memory.
 */
/******************************************************************************/
#include "../../world_attract.h"
#include "pre_inc.h"
#include "kfx/renderer/RendererVulkan.h"
#include "bflib_basics.h"
#include "globals.h"
#include "bflib_video.h"
#include "kfx/platform/IPlatform.h"
#include "kfx/platform/IWindowSystem.h"
#include "kfx/renderer/vulkan/VkCore.h"
#include "kfx/renderer/vulkan/VkResourceMapper.h"
#include "kfx/renderer/vulkan/VkUIRenderer.h"
#include "kfx/renderer/vulkan/VkWorldViewRenderer.h"
#include "kfx/renderer/vulkan/VkPasses.h"
#include "kfx/renderer/vulkan/VkProgram.h"
#include "kfx/renderer/opengl/GLCursorLayer.h"
#include "bflib_vidraw.h"
#include "kfx/renderer/GpuResourceHandle.h"
#include "kfx/renderer/GpuResourceDesc.h"
#include "kfx/renderer/RendererFrameCounter.h"
#include "kfx/renderer/RenderTaskProducerRegistry.h"
#include "kfx/renderer/ir/UICommands.h"
#include "kfx/renderer/ir/TextCommands.h"
#include "kfx/renderer/ir/WorldCommands.h"
#include "kfx/renderer/RenderThreadManager.h"
#include "kfx/renderer/RenderGraph.h"
#include "kfx/renderer/RendererManager.h"
#include "kfx/profiling/KfxProfiling.h"
#include "engine_textures.h"
#include "vidmode.h"
#include "front_simple.h"
#include "bflib_mouse.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#ifdef KFX_ANDROID
#include "AndroidRuntime.hpp" // the Android app's hooks (engine kit)
#endif
#include <cstring>
#include <string>
#include <vector>
#include "post_inc.h"

struct VkFrameData {
    UICommandBuffers    ui_cmds;
    TextCommandBuffers  text_cmds;
    WorldCommandBuffers world_cmds;
    uint32_t            frame_seq = 0;

    UICommandBuffers    parchment_ui_cmds;
    TextCommandBuffers  parchment_text_cmds;
    UICommandBuffers    swipe_cmds;
    UICommandBuffers    tooltip_ui_cmds;
    TextCommandBuffers  tooltip_text_cmds;

    int32_t screen_w = 0;
    int32_t screen_h = 0;
    GpuResourceHandle screen_rt = kInvalidGpuResource;

    int32_t drawable_w = 0;
    int32_t drawable_h = 0;
    int32_t present_x = 0;
    int32_t present_y = 0;
    int32_t present_w = 0;
    int32_t present_h = 0;

    uint64_t sealed_frame_number = 0;

    bool          palette_dirty = false;
    unsigned char palette_rgba[256 * 4] = {};
    unsigned char clear_index = 0;

    GLCursorSnapshot cursor_snapshot;

    // Android: the touch overlay / FPS counter, drawn by SDL's software renderer over transparent black
    // (premultiplied), screen_w x screen_h RGBA, top row first
    bool overlay_on = false;
    std::vector<unsigned char> overlay_rgba;
};

static void frame_clear_colour(const VkFrameData& fd, float* rgba)
{
    const unsigned char* c = &fd.palette_rgba[fd.clear_index * 4];
    rgba[0] = c[0] / 255.0f;
    rgba[1] = c[1] / 255.0f;
    rgba[2] = c[2] / 255.0f;
    rgba[3] = 1.0f;
}

struct RendererVulkan::Impl {
    // the core and the mapper first: everything below releases into them when destroyed
    VkCore             core;
    VkResourceMapper   resource_mapper;

    VkSpriteAtlas      atlas;
    VkUIRenderer       ui;
    VkTextRenderer     text;
    GLCursorLayer      cursor;
    VkTileAtlas        world_atlas;
    VkWorldViewRenderer world;
    VkMapFadePass      mapfade;
    VkImagePresentPass imgpresent;
    VkZoomBoxTilesPass zoomboxtiles;

    std::vector<unsigned char> huge_sprite_pixels;
    std::vector<unsigned char> huge_sprite_coverage;

    GpuResourceHandle  palette_tex_handle    = kInvalidGpuResource;
    GpuResourceHandle  fade_table_tex_handle = kInvalidGpuResource;
    VkPaletteIndexLookup palette_index_lookup;

    bool fade_tables_refreshed = false;
    unsigned int world_atlas_generation = 0;

    RenderThreadManager thread_mgr;
    VkFrameData frames[2];
    int write_idx  = 0;
    int render_idx = 0;

    bool init_ok = false;
    bool core_ok = false;
    std::string fail_reason;
    SDL_Window* window = nullptr;
    bool want_present = true;
    bool lost_reported = false;

    bool fg_lens_captured = false;

    std::string screenshot_path;
    int         screenshot_fmt = 0;

    GpuResourceHandle screen_rt_handle = kInvalidGpuResource;
    int32_t           screen_rt_w = 0;
    int32_t           screen_rt_h = 0;

    // Android overlay: game thread draws, render thread lays it over the frame
    SDL_Surface*  overlay_surface = nullptr;
    SDL_Renderer* overlay_renderer = nullptr;
    VkProgram     overlay_prog;
    uint8_t       overlay_layout = 0;
    VkImageObj    overlay_img;

    void EnsureScreenTarget(int32_t w, int32_t h);
    void DrawAndroidOverlay(VkFrameData& fd);
};

// Game thread. The Android app's touch overlay (KfxAndroidDrawOverlay) draws with an SDL_Renderer; the software
// renderer path hands it the window's. Here it gets a software renderer over a transparent picture of the frame's
// size, which the render thread lays over the frame.
void RendererVulkan::Impl::DrawAndroidOverlay(VkFrameData& fd)
{
    fd.overlay_on = false;
#ifdef KFX_ANDROID
    const int w = fd.screen_w, h = fd.screen_h;
    if (w <= 0 || h <= 0)
        return;
    if (overlay_surface == nullptr || overlay_surface->w != w || overlay_surface->h != h)
    {
        if (overlay_renderer) SDL_DestroyRenderer(overlay_renderer);
        if (overlay_surface) SDL_DestroySurface(overlay_surface);
        overlay_renderer = nullptr;
        overlay_surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        if (overlay_surface != nullptr)
            overlay_renderer = SDL_CreateSoftwareRenderer(overlay_surface);
        if (overlay_renderer == nullptr)
        {
            static bool warned = false;
            if (!warned) { warned = true; WARNLOG("RendererVulkan: no overlay renderer: %s", SDL_GetError()); }
            return;
        }
    }
    SDL_SetRenderDrawBlendMode(overlay_renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(overlay_renderer, 0, 0, 0, 0);
    SDL_RenderClear(overlay_renderer);
    SDL_SetRenderDrawBlendMode(overlay_renderer, SDL_BLENDMODE_BLEND);
    KfxAndroidDrawOverlay(overlay_renderer);
    SDL_FlushRenderer(overlay_renderer);
    // only a picture with something on it is handed over (the overlay is there while possessing or with the FPS)
    const unsigned char* px = (const unsigned char*)overlay_surface->pixels;
    bool any = false;
    for (int y = 0; y < h && !any; y++)
    {
        const uint32_t* row = (const uint32_t*)(px + (size_t)y * overlay_surface->pitch);
        for (int x = 0; x < w; x++)
            if (row[x] != 0) { any = true; break; }
    }
    if (!any)
        return;
    fd.overlay_rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(fd.overlay_rgba.data() + (size_t)y * w * 4, px + (size_t)y * overlay_surface->pitch, (size_t)w * 4);
    fd.overlay_on = true;
#else
    (void)fd;
#endif
}

// Game thread.
void RendererVulkan::Impl::EnsureScreenTarget(int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0)
        return;
    if (screen_rt_handle != kInvalidGpuResource && screen_rt_w == w && screen_rt_h == h)
        return;
    GpuRenderTargetDesc desc;
    desc.width = w;
    desc.height = h;
    desc.attachments = {
        { GpuTextureFormat::RGBA8, false },
        { GpuTextureFormat::Depth24Stencil8, true },
    };
    desc.debug_name = "screen";
    if (screen_rt_handle == kInvalidGpuResource)
        screen_rt_handle = resource_mapper.RequestCreateRenderTarget(desc);
    else
        screen_rt_handle = resource_mapper.RequestReloadRenderTarget(screen_rt_handle, desc);
    screen_rt_w = w;
    screen_rt_h = h;
}

bool RendererVulkan_Probe(std::string& reason, std::string& device)
{
    return VkCore_Probe(reason, device);
}

RendererVulkan::RendererVulkan() = default;

RendererVulkan::~RendererVulkan()
{
    Shutdown();
}

bool RendererVulkan::Init()
{
    IWindowSystem* ws = GetPlatform()->GetWindowSystem();
    if (ws == nullptr || !ws->HasWindow())
    {
        m_fail_reason = "no window";
        ERRORLOG("RendererVulkan::Init: no window");
        return false;
    }
    m_impl = new Impl();
    m_impl->window = (SDL_Window*)ws->GetNativeWindow();
#ifdef KFX_ANDROID
    if (m_impl->window != nullptr)
        KfxAndroidConfigureWindow(m_impl->window); // the panel's refresh rate, as the software path asks for it
#endif
    // the launcher's background is never shown: no swapchain, its frames are read back
    m_impl->want_present = !world_attract_active();

    m_impl->thread_mgr.Start(
        [this]{ render_thread_init(); },
        [this]{ render_thread_work(); },
        [this]{ render_thread_present(); },
        [this]{ render_thread_cleanup(); });

    if (!m_impl->init_ok)
    {
        m_fail_reason = m_impl->fail_reason.empty() ? "initialisation failed" : m_impl->fail_reason;
        m_impl->thread_mgr.Stop();
        delete m_impl;
        m_impl = nullptr;
        return false;
    }
    if (!world_attract_active())
        ws->ShowWindow();
    return true;
}

void RendererVulkan::Shutdown()
{
    if (m_impl != nullptr)
    {
        m_impl->thread_mgr.Stop();
        if (m_impl->overlay_renderer) SDL_DestroyRenderer(m_impl->overlay_renderer);
        if (m_impl->overlay_surface) SDL_DestroySurface(m_impl->overlay_surface);
        delete m_impl;
        m_impl = nullptr;
    }
}

// Render thread: everything that touches Vulkan lives here.
void RendererVulkan::render_thread_init()
{
    Impl& im = *m_impl;
    if (!im.core.Init(im.window, im.want_present, im.fail_reason))
    {
        im.init_ok = false;
        return;
    }
    im.core_ok = true;
    KFX_GPU_CTX_CREATE();
    im.resource_mapper.SetCore(&im.core);

    im.atlas.SetResourceMapper(&im.resource_mapper);
    im.ui.SetResourceMapper(&im.resource_mapper);
    if (!im.atlas.Init() || !im.ui.Init())
    {
        im.fail_reason = "UI renderer/atlas init failed";
        im.init_ok = false;
        return;
    }

    unsigned char black_palette[256 * 4] = {};
    for (int i = 0; i < 256; ++i)
        black_palette[i * 4 + 3] = 255;
    GpuTextureDesc palette_desc;
    palette_desc.width = 256;
    palette_desc.height = 1;
    palette_desc.format = GpuTextureFormat::RGBA8;
    palette_desc.min_filter = GpuTextureFilter::Nearest;
    palette_desc.mag_filter = GpuTextureFilter::Nearest;
    palette_desc.wrap = GpuTextureWrap::Clamp;
    palette_desc.initial_pixels.assign(black_palette, black_palette + sizeof(black_palette));
    palette_desc.debug_name = "palette";
    im.palette_tex_handle = im.resource_mapper.RequestCreateTexture(palette_desc);
    if (im.resource_mapper.ResolveTexture(im.palette_tex_handle) == nullptr)
    {
        im.fail_reason = "palette texture failed";
        im.init_ok = false;
        return;
    }
    GpuTextureDesc fade_table_desc;
    fade_table_desc.width = 256;
    fade_table_desc.height = 64;
    fade_table_desc.format = GpuTextureFormat::R8;
    fade_table_desc.min_filter = GpuTextureFilter::Nearest;
    fade_table_desc.mag_filter = GpuTextureFilter::Nearest;
    fade_table_desc.initial_pixels.assign(pixmap.fade_tables, pixmap.fade_tables + (256 * 64));
    fade_table_desc.debug_name = "fade_table";
    im.fade_table_tex_handle = im.resource_mapper.RequestCreateTexture(fade_table_desc);
    if (im.resource_mapper.ResolveTexture(im.fade_table_tex_handle) == nullptr)
    {
        im.fail_reason = "fade table texture failed";
        im.init_ok = false;
        return;
    }
    if (!im.palette_index_lookup.Init(&im.resource_mapper))
    {
        im.fail_reason = "palette index lookup failed";
        im.init_ok = false;
        return;
    }

    im.ui.SetAtlas(&im.atlas);
    im.ui.SetPaletteTexture(im.palette_tex_handle);
    im.ui.SetFadeTableTexture(im.fade_table_tex_handle);
    im.ui.SetScreenSize((int)RendererPhysicalWidth(), (int)lbDisplay.PhysicalScreenHeight);
    im.text.SetUIRenderer(&im.ui);
    im.cursor.SetUIRenderer(&im.ui);
    im.cursor.SetWorldRenderer(&im.world);

    im.world_atlas.SetResourceMapper(&im.resource_mapper);
    im.world.SetAtlas(&im.world_atlas);
    im.world.SetResourceMapper(&im.resource_mapper);
    im.world.SetFadeTexture(im.fade_table_tex_handle);
    im.world.SetPaletteTexture(im.palette_tex_handle);
    im.world.SetPaletteIndexTexture(im.palette_index_lookup.GetTexture());
    im.world.SetScreenSize((int)RendererPhysicalWidth(), (int)lbDisplay.PhysicalScreenHeight);
    if (!im.world.CompileShaders())
    {
        im.fail_reason = "world view pipelines failed";
        im.init_ok = false;
        return;
    }

    im.mapfade.SetResourceMapper(&im.resource_mapper);
    im.mapfade.SetPaletteTexture(im.palette_tex_handle);
    im.mapfade.SetFadeTableTexture(im.fade_table_tex_handle);
    im.mapfade.SetPaletteIndexTexture(im.palette_index_lookup.GetTexture());
    if (!im.mapfade.CompileShaders())
        WARNLOG("RendererVulkan::Init: parchment transition unavailable -- transition disabled");

    im.imgpresent.SetResourceMapper(&im.resource_mapper);
    im.imgpresent.SetPaletteTexture(im.palette_tex_handle);
    if (!im.imgpresent.CompileShaders())
        WARNLOG("RendererVulkan::Init: image presents unavailable -- FMV/splash/backgrounds will not be drawn");

    im.zoomboxtiles.SetResourceMapper(&im.resource_mapper);
    im.zoomboxtiles.SetPaletteTexture(im.palette_tex_handle);
    im.zoomboxtiles.SetTileAtlas(&im.world_atlas);
    if (!im.zoomboxtiles.CompileShaders())
        WARNLOG("RendererVulkan::Init: zoom-box tiles unavailable -- zoom box terrain will not be drawn");

    for (VkFrameData& fd : im.frames)
    {
        fd.ui_cmds.shared_seq   = &fd.frame_seq;
        fd.text_cmds.shared_seq = &fd.frame_seq;
        fd.parchment_ui_cmds.shared_seq   = &fd.frame_seq;
        fd.parchment_text_cmds.shared_seq = &fd.frame_seq;
        fd.tooltip_ui_cmds.shared_seq   = &fd.frame_seq;
        fd.tooltip_text_cmds.shared_seq = &fd.frame_seq;
    }
    im.ui.SetUICommandBuffers(&im.frames[0].ui_cmds);
    im.text.SetTextCommandBuffers(&im.frames[0].text_cmds);
    im.world.SetWorldCommandBuffers(&im.frames[0].world_cmds);
    im.write_idx = 0;
#ifdef KFX_ANDROID
    if (im.overlay_prog.Create(&im.core, "overlay_rgba"))
    {
        VkVertexLayout l;
        l.stride = 4 * sizeof(float);
        l.attrs = {
            { "a_pos", VK_FORMAT_R32G32_SFLOAT, 0,                 0 },
            { "a_uv",  VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float), 0 },
        };
        im.overlay_layout = im.overlay_prog.AddLayout(l);
    }
#endif
    im.init_ok = true;
    SYNCLOG("RendererVulkan: ready on '%s'%s", im.core.device_name.c_str(),
            im.want_present ? "" : " (no swapchain: the launcher's background)");
}

void RendererVulkan::render_thread_work()
{
    KFX_ZONE_COLOR("RendererVulkan::render_thread_work", KFX_COLOR_RENDER_GPU);
    Impl& im = *m_impl;
    VkFrameData& fd = im.frames[im.render_idx];

    if (!im.core.BeginFrame())
    {
        if (im.core.device_lost && !im.lost_reported)
        {
            im.lost_reported = true;
            ERRORLOG("RendererVulkan: the GPU device was lost; nothing more can be drawn this run");
        }
        return;
    }
    {
        KFX_ZONE_COLOR("RendererVulkan::FrameHousekeeping", KFX_COLOR_RENDER_GPU);
        im.resource_mapper.ProcessDeferredDestroys(fd.sealed_frame_number);
        if (fd.palette_dirty)
        {
            fd.palette_dirty = false;
            VkImageObj* pal = im.resource_mapper.TextureImage(im.palette_tex_handle);
            if (pal != nullptr)
                im.core.UploadImage(*pal, 0, 0, 0, 256, 1, fd.palette_rgba, 256 * 4, 4);
            im.palette_index_lookup.SetPalette(fd.palette_rgba);
        }
        im.ui.SetFramePalette(fd.palette_rgba);
        im.atlas.FlushPendingGL();

        if (!im.world_atlas.IsInitialized() && block_ptrs[0] != nullptr)
            im.world_atlas.Init();
        else if (im.world_atlas_generation != level_textures_generation)
            im.world_atlas.Rebuild();
        im.world_atlas_generation = level_textures_generation;

        if (!im.fade_tables_refreshed && fade_tables_ready)
        {
            im.world.RefreshFadeTableAndSettings();
            im.palette_index_lookup.RefreshBase(engine_palette);
            im.palette_index_lookup.SetPalette(fd.palette_rgba);
            im.fade_tables_refreshed = true;
        }
    }

    RenderGraph::Execute(*this);

    VkImageObj* screen = im.resource_mapper.ScreenColor();
    if (screen != nullptr && im.want_present)
    {
        KFX_ZONE_COLOR("RendererVulkan::PresentScreenTarget", KFX_COLOR_RENDER_GPU);
        im.core.BlitToSwapchain(*screen, fd.present_x, fd.present_y, fd.present_w, fd.present_h,
                                fd.drawable_w, fd.drawable_h,
#ifdef KFX_ANDROID
                                KfxAndroidVsync() != 0
#else
                                vsync_enabled != 0
#endif
                                );
    }
    im.core.SubmitFrame();
}

void RendererVulkan::render_thread_present()
{
    if (!m_impl->core_ok)
        return;
    {
        KFX_ZONE_COLOR("RendererVulkan::Present", KFX_COLOR_RENDER_GPU);
        m_impl->core.Present();
    }
    KFX_GPU_COLLECT();
    KFX_FRAMEMARK();
}

void RendererVulkan::render_thread_cleanup()
{
    Impl& im = *m_impl;
    if (im.core_ok)
    {
        vkDeviceWaitIdle(im.core.device);
        im.ui.Shutdown();
        im.world.Shutdown();
        im.mapfade.Shutdown();
        im.imgpresent.Shutdown();
        im.zoomboxtiles.Shutdown();
        im.resource_mapper.ShutdownAll();
        im.overlay_prog.Destroy();
        im.core.DestroyImage(im.overlay_img);
        VkProgram_FreeDummies(&im.core);
    }
    im.core.Shutdown();
    im.core_ok = false;
}

/******************************************************************************/
/* frame graph phases                                                         */
/******************************************************************************/

void RendererVulkan::FGClearFrame()
{
    Impl& im = *m_impl;
    VkFrameData& fd = im.frames[im.render_idx];
    im.world.SetScreenSize(fd.screen_w, fd.screen_h);
    im.ui.SetScreenSize(fd.screen_w, fd.screen_h);
    im.resource_mapper.SetScreenTarget(fd.screen_rt);
    im.core.last_pass = "clear";
    float clear_rgba[4];
    frame_clear_colour(fd, clear_rgba);
    im.core.SetViewport(0.0f, 0.0f, (float)fd.screen_w, (float)fd.screen_h);
    im.core.SetScissorEnabled(false);
    VkImageObj* color = im.resource_mapper.ScreenColor();
    if (color != nullptr)
        im.core.BeginPass(color, im.resource_mapper.ScreenDepth(), clear_rgba, true);
    im.ui.BuildQuadsFromIR(fd.ui_cmds);
}

void RendererVulkan::FGBeginWorldCapture()
{
    float clear_rgba[4];
    frame_clear_colour(m_impl->frames[m_impl->render_idx], clear_rgba);
    m_impl->core.last_pass = "lens capture";
    m_impl->fg_lens_captured = m_impl->world.BeginLensCapture(clear_rgba);
}

void RendererVulkan::FGExecuteWorld()
{
    m_impl->core.last_pass = "world";
    m_impl->world.GPURenderNow(m_impl->frames[m_impl->render_idx].world_cmds);
}

void RendererVulkan::FGFlushSwipeOverlay()
{
    static const TextCommandBuffers s_no_text;
    m_impl->core.last_pass = "swipe";
    m_impl->ui.DrawFromIR(m_impl->frames[m_impl->render_idx].swipe_cmds, s_no_text, nullptr);
}

void RendererVulkan::FGResolveWorldCapture()
{
    if (m_impl->fg_lens_captured)
    {
        m_impl->core.last_pass = "lens composite";
        m_impl->world.ResolveLensComposite();
    }
}

void RendererVulkan::FGCaptureMapFadeWorld()
{
    Impl& im = *m_impl;
    if (!im.mapfade.IsCapturePendingRT())
        return;
    VkFrameData& fd = im.frames[im.render_idx];
    im.core.last_pass = "map fade world capture";
    im.mapfade.CaptureWorldFrame(fd.screen_w, fd.screen_h);
    // the parchment draws that follow need a clean screen, as the parchment view has
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    VkImageObj* color = im.resource_mapper.ScreenColor();
    if (color != nullptr)
        im.core.BeginPass(color, im.resource_mapper.ScreenDepth(), black, true);
}

void RendererVulkan::FGResolveMapFade()
{
    Impl& im = *m_impl;
    if (!im.mapfade.IsActiveRT())
        return;
    VkFrameData& fd = im.frames[im.render_idx];
    im.core.last_pass = "map fade";
    if (im.mapfade.IsCapturePendingRT())
    {
        im.ui.DrawFromIR(fd.parchment_ui_cmds, fd.parchment_text_cmds, &im.text);
        im.mapfade.CaptureParchmentFrame(fd.screen_w, fd.screen_h);
    }
    im.mapfade.ResolveComposite(fd.screen_w, fd.screen_h);
}

void RendererVulkan::FGExecuteImagePresents()
{
    VkFrameData& fd = m_impl->frames[m_impl->render_idx];
    m_impl->core.last_pass = "image presents";
    m_impl->imgpresent.Resolve(fd.screen_w, fd.screen_h);
}

void RendererVulkan::FGExecuteImagePresentOverlay()
{
    VkFrameData& fd = m_impl->frames[m_impl->render_idx];
    m_impl->core.last_pass = "image present overlay";
    m_impl->imgpresent.ResolveOverlay(fd.screen_w, fd.screen_h);
}

void RendererVulkan::FGDrawZoomBoxes()
{
    VkFrameData& fd = m_impl->frames[m_impl->render_idx];
    m_impl->core.last_pass = "zoom box";
    m_impl->zoomboxtiles.Resolve(fd.screen_w, fd.screen_h);
}

void RendererVulkan::FGDrawWorldSpriteLayer()
{
    m_impl->core.last_pass = "world sprite layer";
    m_impl->ui.DrawWorldSpriteLayerRT();
}

void RendererVulkan::FGDrawWorldOverlayFlatLayer()
{
    m_impl->core.last_pass = "world flat layer";
    m_impl->ui.DrawWorldOverlayFlatLayerRT();
}

void RendererVulkan::FGDrawGameUI()
{
    m_impl->core.last_pass = "game UI";
    m_impl->ui.DrawGameUILayerRT(m_impl->frames[m_impl->render_idx].text_cmds, &m_impl->text);
}

void RendererVulkan::FGDrawFrontOverlay()
{
    m_impl->core.last_pass = "front overlay";
    m_impl->ui.DrawFrontOverlay();
}

void RendererVulkan::FGExecuteCursor()
{
    Impl& im = *m_impl;
    VkFrameData& fd = im.frames[im.render_idx];
    im.core.last_pass = "cursor";
    im.world.DrawCursorKeeperSprites();
    im.ui.DrawFromIR(fd.tooltip_ui_cmds, fd.tooltip_text_cmds, &im.text);
    if (!world_attract_active()) // (the launcher's background has no pointer)
        im.cursor.DrawSnapshot(fd.cursor_snapshot);
}

void RendererVulkan::FGDrawDevToolsOverlay()
{
    Impl& im = *m_impl;
    VkFrameData& fd = im.frames[im.render_idx];
    if (!fd.overlay_on || !im.overlay_prog.Valid() || fd.screen_w <= 0 || fd.screen_h <= 0)
        return;
    if (fd.overlay_rgba.size() != (size_t)fd.screen_w * fd.screen_h * 4)
        return;
    im.core.last_pass = "android overlay";
    if (im.overlay_img.image == VK_NULL_HANDLE || im.overlay_img.width != fd.screen_w || im.overlay_img.height != fd.screen_h)
    {
        im.core.DestroyImage(im.overlay_img);
        if (!im.core.CreateImage(im.overlay_img, fd.screen_w, fd.screen_h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "android_overlay"))
            return;
        im.overlay_img.sampler = im.core.GetSampler(false, false);
        im.core.InitSampledImage(im.overlay_img);
    }
    // one a frame, drawn after this: its upload can go before the frame's draws
    im.core.UploadImage(im.overlay_img, 0, 0, 0, fd.screen_w, fd.screen_h, fd.overlay_rgba.data(), (uint32_t)fd.screen_w * 4, 4);
    im.resource_mapper.BindScreen();
    im.core.SetViewport(0.0f, 0.0f, (float)fd.screen_w, (float)fd.screen_h);
    im.core.SetScissorEnabled(false);
    const float w = (float)fd.screen_w, h = (float)fd.screen_h;
    const float verts[] = {
        0, 0, 0, 0,   w, 0, 1, 0,   w, h, 1, 1,
        0, 0, 0, 0,   w, h, 1, 1,   0, h, 0, 1,
    };
    im.overlay_prog.Set2f("u_screen_size", w, h);
    im.overlay_prog.SetTexture("u_image", &im.overlay_img);
    VkPipeState st;
    st.Blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    st.layout_id = im.overlay_layout;
    if (!im.overlay_prog.Bind(st))
        return;
    im.core.BindVertices(verts, sizeof(verts));
    vkCmdDraw(im.core.Cmd(), 6, 1, 0, 0);
}

bool RendererVulkan::ScheduleScreenshot(const char* path, int fmt)
{
    if (m_impl == nullptr || !path || (fmt != 1 && fmt != 2))
        return false;
    m_impl->screenshot_path = path;
    m_impl->screenshot_fmt  = fmt;
    return true;
}

static void save_screenshot(const uint8_t* rgba, int w, int h, const std::string& path, int fmt)
{
    // the screen image is bottom row first (OpenGL's order): flip rows for a top-down file
    const size_t row_bytes = (size_t)w * 4;
    std::vector<unsigned char> flipped(row_bytes * (size_t)h);
    for (int row = 0; row < h; ++row)
        memcpy(flipped.data() + (size_t)row * row_bytes, rgba + (size_t)(h - 1 - row) * row_bytes, row_bytes);
    SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, flipped.data(), (int)row_bytes);
    if (surf != nullptr)
    {
        bool ok = (fmt == 1) ? IMG_SavePNG(surf, path.c_str()) : SDL_SaveBMP(surf, path.c_str());
        if (!ok)
            ERRORLOG("Screenshot save failed (%s): %s", path.c_str(), SDL_GetError());
        SDL_DestroySurface(surf);
    }
}

void RendererVulkan::FGCaptureScreenshot()
{
    Impl& im = *m_impl;
    VkImageObj* screen = im.resource_mapper.ScreenColor();
    VkFrameData& fd = im.frames[im.render_idx];
    if (screen == nullptr || fd.screen_w <= 0 || fd.screen_h <= 0)
    {
        im.screenshot_path.clear();
        return;
    }
    im.core.last_pass = "readback";
    if (world_attract_active())
    {
        // the launcher's background: every frame goes to the launcher
        im.core.Readback(*screen, [](const uint8_t* rgba, int w, int h) {
            world_attract_frame(rgba, w, h, 1);
        });
    }
    if (im.screenshot_path.empty())
        return;
    const std::string path = im.screenshot_path;
    const int fmt = im.screenshot_fmt;
    im.core.Readback(*screen, [path, fmt](const uint8_t* rgba, int w, int h) {
        save_screenshot(rgba, w, h, path, fmt);
    });
    im.screenshot_path.clear();
}

/******************************************************************************/
/* game thread                                                                */
/******************************************************************************/

void RendererVulkan::SetDisplayPalette(const unsigned char* rgb8)
{
    if (m_impl == nullptr || rgb8 == nullptr) return;
    VkFrameData& fd = m_impl->frames[m_impl->write_idx];
    for (int i = 0; i < 256; ++i) {
        fd.palette_rgba[i * 4 + 0] = rgb8[i * 3 + 0];
        fd.palette_rgba[i * 4 + 1] = rgb8[i * 3 + 1];
        fd.palette_rgba[i * 4 + 2] = rgb8[i * 3 + 2];
        fd.palette_rgba[i * 4 + 3] = 255;
    }
    fd.palette_dirty = true;
}

void RendererVulkan::ClearScreen(unsigned char colour)
{
    if (m_impl == nullptr) return;
    m_impl->frames[m_impl->write_idx].clear_index = colour;
}

void RendererVulkan::PresentFrame()
{
    if (m_impl == nullptr || !GetPlatform()->GetWindowSystem()->HasWindow())
        return;
    Impl& im = *m_impl;

    RenderTaskProducerRegistry_ProduceAll(RendererFrameCounter_Current());
    im.thread_mgr.WaitForCompletion();

    if (RendererIsFadeCachePreserved() && !RendererConsumeForceUIFlip())
    {
        // blocking palette fade: keep re-drawing the last real frame, only its palette changes (as OpenGL does)
        VkFrameData& write_fd  = im.frames[im.write_idx];
        VkFrameData& render_fd = im.frames[im.render_idx];
        if (write_fd.palette_dirty)
        {
            memcpy(render_fd.palette_rgba, write_fd.palette_rgba, sizeof(render_fd.palette_rgba));
            render_fd.palette_dirty = true;
            write_fd.palette_dirty = false;
        }
        RendererFrameCounter_Advance();
        im.thread_mgr.Signal();
        return;
    }

    const int filled = im.write_idx;
    const int next   = 1 - filled;

    VkFrameData& next_fd = im.frames[next];
    next_fd.ui_cmds.Reset();
    next_fd.text_cmds.Reset();
    next_fd.world_cmds.Reset();
    next_fd.parchment_ui_cmds.Reset();
    next_fd.parchment_text_cmds.Reset();
    next_fd.swipe_cmds.Reset();
    next_fd.tooltip_ui_cmds.Reset();
    next_fd.tooltip_text_cmds.Reset();
    next_fd.frame_seq = 0;
    next_fd.palette_dirty = false;
    memcpy(next_fd.palette_rgba, im.frames[filled].palette_rgba, sizeof(next_fd.palette_rgba));
    next_fd.clear_index = im.frames[filled].clear_index;

    im.world.FlipBuffers();
    im.mapfade.FlipBuffers();
    im.imgpresent.FlipBuffers();
    im.zoomboxtiles.FlipBuffers();

    im.ui.SetUICommandBuffers(&next_fd.ui_cmds);
    im.text.SetTextCommandBuffers(&next_fd.text_cmds);
    im.world.SetWorldCommandBuffers(&next_fd.world_cmds);

    VkFrameData& filled_fd = im.frames[filled];
    filled_fd.screen_w = (int32_t)RendererPhysicalWidth();
    filled_fd.screen_h = (int32_t)lbDisplay.PhysicalScreenHeight;
    im.EnsureScreenTarget(filled_fd.screen_w, filled_fd.screen_h);
    filled_fd.screen_rt = im.screen_rt_handle;
    {
        const IWindowSystem* ws = GetPlatform()->GetWindowSystem();
        int dw = 0, dh = 0, px = 0, py = 0, pw = 0, ph = 0;
        ws->GetDrawableSize(&dw, &dh);
        ws->GetPresentRect(&px, &py, &pw, &ph);
        filled_fd.drawable_w = dw;
        filled_fd.drawable_h = dh;
        filled_fd.present_x = px;
        filled_fd.present_y = py;
        filled_fd.present_w = pw;
        filled_fd.present_h = ph;
    }
    filled_fd.sealed_frame_number = RendererFrameCounter_Current();
    im.DrawAndroidOverlay(filled_fd);

    LbMouseOnBeginSwap();
    filled_fd.cursor_snapshot = im.cursor.Snapshot();
    LbMouseOnEndSwap();

    im.render_idx = filled;
    im.write_idx  = next;
    RendererFrameCounter_Advance();
    im.thread_mgr.Signal();
#ifdef KFX_ANDROID
    KfxAndroidFramePresented(); // frame pacing without vsync, the FPS counter, the second screen
#endif
}

bool RendererVulkan::BeginFrame()
{
    return m_impl != nullptr && m_impl->init_ok;
}

bool RendererVulkan::CanPresent() const
{
    return m_impl != nullptr && m_impl->thread_mgr.IsWorkDone();
}

void RendererVulkan::EndFrame()
{
}

bool RendererVulkan::PresentImage(const struct RendererPresentImageDesc* desc)
{
    if (m_impl == nullptr || !m_impl->imgpresent.IsReady())
        return false;
    m_impl->imgpresent.Submit(desc);
    return true;
}

void RendererVulkan::PresentHugeSprite(const struct TbHugeSprite* spr, int32_t sp_len,
                                       int32_t x_shift, int32_t y_shift, int32_t units_per_px)
{
    if (m_impl == nullptr || !m_impl->imgpresent.IsReady())
        return;
    const int32_t w = RendererPhysicalWidth();
    const int32_t h = lbDisplay.PhysicalScreenHeight;
    if (w <= 0 || h <= 0)
        return;
    // two buffers pre-filled differently recover the RLE's per-pixel opacity (as RendererOpenGL does)
    const size_t npixels = (size_t)w * (size_t)h;
    std::vector<unsigned char>& pixels = m_impl->huge_sprite_pixels;
    std::vector<unsigned char>& coverage = m_impl->huge_sprite_coverage;
    pixels.assign(npixels, 0x00);
    coverage.assign(npixels, 0xFF);
    LbHugeSpriteDraw(spr, sp_len, pixels.data(), w, h, x_shift, y_shift, units_per_px);
    LbHugeSpriteDraw(spr, sp_len, coverage.data(), w, h, x_shift, y_shift, units_per_px);
    for (size_t i = 0; i < npixels; i++)
        coverage[i] = (pixels[i] == coverage[i]) ? 0xFF : 0x00;
    struct RendererPresentImageDesc desc = {};
    desc.dst_w = w;
    desc.dst_h = h;
    desc.src = pixels.data();
    desc.src_pitch = w;
    desc.src_w = w;
    desc.src_h = h;
    desc.kind = PRESENT_KIND_TRANSPARENT;
    desc.coverage = coverage.data();
    m_impl->imgpresent.Submit(&desc);
}

void RendererVulkan::SubmitZoomBoxTiles(const uint16_t* tile_block_ids, int tiles_x, int tiles_y,
                                        int dst_x, int dst_y, int tile_w, int tile_h)
{
    if (m_impl == nullptr || !m_impl->zoomboxtiles.IsReady())
        return;
    m_impl->zoomboxtiles.Submit(tile_block_ids, tiles_x, tiles_y, dst_x, dst_y, tile_w, tile_h);
}

bool RendererVulkan::SubmitLandviewZoom(const unsigned char* src_buf, int src_w, int src_h,
                                        float center_map_x, float center_map_y,
                                        float screen_cx,    float screen_cy,
                                        float scale)
{
    if (m_impl == nullptr || !m_impl->imgpresent.IsReady())
        return false;
    m_impl->imgpresent.SubmitZoom(src_buf, src_w, src_h, center_map_x, center_map_y, screen_cx, screen_cy, scale);
    return true;
}

class ITextRenderer* RendererVulkan::GetTextRenderer() { return m_impl ? &m_impl->text : nullptr; }
class IUIRenderer* RendererVulkan::GetUIRenderer() { return m_impl ? &m_impl->ui : nullptr; }
class ICursorLayer* RendererVulkan::GetCursorLayer() { return m_impl ? &m_impl->cursor : nullptr; }
class IWorldViewRenderer* RendererVulkan::GetWorldViewRenderer() { return m_impl ? &m_impl->world : nullptr; }

void RendererVulkan::SubmitMapFadeStep(bool starting, float display_step, float warp_scale,
                                       const unsigned char* ghost_table)
{
    if (m_impl == nullptr) return;
    m_impl->mapfade.SubmitStep(starting, display_step, warp_scale, ghost_table);
}

void RendererVulkan::BeginOverlayCapture(OverlayCaptureKind kind)
{
    if (m_impl == nullptr) return;
    VkFrameData& fd = m_impl->frames[m_impl->write_idx];
    switch (kind)
    {
    case OVERLAY_CAPTURE_PARCHMENT:
        m_impl->ui.SetUICommandBuffers(&fd.parchment_ui_cmds);
        m_impl->text.SetTextCommandBuffers(&fd.parchment_text_cmds);
        break;
    case OVERLAY_CAPTURE_SWIPE:
        m_impl->ui.SetUICommandBuffers(&fd.swipe_cmds);
        break;
    case OVERLAY_CAPTURE_TOOLTIP:
        m_impl->ui.SetUICommandBuffers(&fd.tooltip_ui_cmds);
        m_impl->text.SetTextCommandBuffers(&fd.tooltip_text_cmds);
        break;
    }
}

void RendererVulkan::EndOverlayCapture(OverlayCaptureKind kind)
{
    if (m_impl == nullptr) return;
    VkFrameData& fd = m_impl->frames[m_impl->write_idx];
    m_impl->ui.SetUICommandBuffers(&fd.ui_cmds);
    if (kind == OVERLAY_CAPTURE_PARCHMENT || kind == OVERLAY_CAPTURE_TOOLTIP)
        m_impl->text.SetTextCommandBuffers(&fd.text_cmds);
}

bool RendererVulkan::MapFadeSupportsNativeResolution() const
{
    return (m_impl != nullptr) && m_impl->mapfade.IsReady();
}

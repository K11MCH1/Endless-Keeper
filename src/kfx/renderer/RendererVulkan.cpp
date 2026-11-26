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
#include "kfx/renderer/UIImage.h"
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
#include "game_legacy.h"
#include "keeperfx.hpp"
#include "bflib_mouse.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#ifdef KFX_ANDROID
#include "AndroidRuntime.hpp" // the Android app's hooks (engine kit)
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
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
    // (premultiplied), RGBA, top row first: only the band of rows [overlay_y0, overlay_y0 + overlay_rows) that changed
    bool overlay_on = false;
    int32_t overlay_y0 = 0;
    int32_t overlay_rows = 0;
    std::vector<unsigned char> overlay_rgba;

    // Endless Keeper: the true-colour window of this frame (UIImage.h)
    UIImageFrame ui_image[UI_IMAGE_SLOTS];
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
    int32_t overlay_drawn_y0 = 0, overlay_drawn_y1 = 0;   // the surface's rows drawn on last time (cleared next)
    int32_t overlay_img_y0 = 0, overlay_img_y1 = 0;       // the image's rows that may not be empty

    // Endless Keeper: the true-colour window (UIImage.h), drawn with overlay_prog. Game thread: the source.
    UIImageSource ui_image_src[UI_IMAGE_SLOTS];
    VkImageObj    ui_image_img[UI_IMAGE_SLOTS];
    const void*   ui_image_uploaded[UI_IMAGE_SLOTS] = {}; // the copy now in each image
    uint32_t      ui_image_uploaded_gen[UI_IMAGE_SLOTS] = {}; // (and its generation: copies are reused)
    void DrawUIImages(VkFrameData& fd, int first, int last);
    void DrawUIImage(VkFrameData& fd, int slot);

    // Android: what a frame costs, logged every 5 s; the refresh rate asked again for each new swapchain
    uint32_t refresh_serial = 0;
    uint64_t t_start = 0, t_frames = 0, t_wait_ns = 0, t_overlay_ns = 0, t_pace_ns = 0;
    uint64_t t_busy_polls = 0;                 // the game loop found the render thread busy (it sleeps 1 ms each)

    // Android performance hint (ADPF, 2026-11-24): the game and render threads take turns on each frame, so neither
    // looks busy and the phone kept their cores at 0.8-1.5 GHz until a touch raised them (Vulkan fell to 60-90 FPS
    // on a 144 Hz RedMagic). A hint session tells Android the frame's deadline and how long each frame really took.
    int32_t render_tid = 0;
    bool hint_wanted = false;     // the phone has the API and no test switch turned it off
    int hint_quiet_frames = 0;    // frames in a row without the world (menus): the session closes after a few
    void* hint_session = nullptr;
    int64_t hint_target_ns = 0;
    uint64_t hint_last_ns = 0, hint_last_wait_ns = 0;
    uint64_t t_hint_work_ns = 0;
    void AndroidHintStart();
    bool AndroidHintOpen();
    void AndroidHintFrame(bool world_drawn);
    void AndroidHintStop();
    std::atomic<uint64_t> t_render_ns{0};      // render thread: building and submitting the frames (not presenting)
    void AndroidTiming();

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
        SDL_SetRenderDrawBlendMode(overlay_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(overlay_renderer, 0, 0, 0, 0);
        SDL_RenderClear(overlay_renderer);
        SDL_FlushRenderer(overlay_renderer);
        overlay_drawn_y0 = overlay_drawn_y1 = 0;
        overlay_img_y0 = overlay_img_y1 = 0; // (the render thread makes a new, empty image of this size)
    }
    unsigned char* px = (unsigned char*)overlay_surface->pixels;
    const size_t pitch = (size_t)overlay_surface->pitch;
    // 2026-11-24: only the rows drawn on last time are cleared and only the changed rows go to the GPU (before: the
    // whole screen, 12 MB at 2608x1200, cleared, copied and uploaded every frame the FPS counter was on)
    if (overlay_drawn_y1 > overlay_drawn_y0)
        memset(px + (size_t)overlay_drawn_y0 * pitch, 0, (size_t)(overlay_drawn_y1 - overlay_drawn_y0) * pitch);
    SDL_SetRenderDrawBlendMode(overlay_renderer, SDL_BLENDMODE_BLEND);
    KfxAndroidDrawOverlay(overlay_renderer);
    SDL_FlushRenderer(overlay_renderer);
    int y0 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
    {
        const uint32_t* row = (const uint32_t*)(px + (size_t)y * pitch);
        uint32_t any = 0;
        for (int x = 0; x < w; x++)
            any |= row[x];
        if (any != 0)
        {
            if (y0 < 0) y0 = y;
            y1 = y + 1;
        }
    }
    if (y0 < 0)
    {
        overlay_drawn_y0 = overlay_drawn_y1 = 0;
        return; // nothing on it (the overlay is there while possessing or with the FPS counter on)
    }
    overlay_drawn_y0 = y0;
    overlay_drawn_y1 = y1;
    // the band uploaded also wipes what the image still holds from before
    const int b0 = (overlay_img_y1 > overlay_img_y0) ? std::min(y0, overlay_img_y0) : y0;
    const int b1 = (overlay_img_y1 > overlay_img_y0) ? std::max(y1, overlay_img_y1) : y1;
    fd.overlay_rgba.resize((size_t)w * (b1 - b0) * 4);
    for (int y = b0; y < b1; y++)
        memcpy(fd.overlay_rgba.data() + (size_t)(y - b0) * w * 4, px + (size_t)y * pitch, (size_t)w * 4);
    fd.overlay_y0 = b0;
    fd.overlay_rows = b1 - b0;
    overlay_img_y0 = y0;
    overlay_img_y1 = y1;
    fd.overlay_on = true;
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
    {
        // test switches (2026-11-24): an empty file of that name in the game folder brings the old way back
        auto has = [](const char* name) { FILE* f = fopen(name, "rb"); if (f) fclose(f); return f != nullptr; };
        const bool legacy_rotation = has("vk_test_no_prerotate");
        const bool legacy_pacing = has("vk_test_game_limiter");
        m_impl->core.allow_prerotate = !legacy_rotation;
        m_impl->core.vsync_pacing = !legacy_pacing && KfxAndroidVsync() != 0;
        SYNCLOG("RendererVulkan: Android pre-rotation %s, frames paced by %s", legacy_rotation ? "off (test switch)" : "on",
                m_impl->core.vsync_pacing ? "the screen (vsync)" : legacy_pacing ? "the game's limiter (test switch)" : "the game's limiter (no vsync)");
    }
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
#ifdef KFX_ANDROID
    if (m_impl->core.vsync_pacing && m_impl->want_present)
    {
        RendererSetPacesFramesItself(1);
        redetect_screen_refresh_rate_for_draw();
    }
    if (m_impl->want_present)
    {
        FILE* off = fopen("vk_test_no_hint", "rb"); // (test switch: an empty file of that name)
        if (off != nullptr)
        {
            fclose(off);
            SYNCLOG("RendererVulkan: Android performance hint off (test switch)");
        }
        else
            m_impl->AndroidHintStart();
    }
#endif
    return true;
}

void RendererVulkan::Shutdown()
{
    if (m_impl != nullptr)
        m_impl->AndroidHintStop();
    if (RendererPacesFramesItself())
    {
        RendererSetPacesFramesItself(0);
        redetect_screen_refresh_rate_for_draw();
    }
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
#ifdef KFX_ANDROID
    im.render_tid = (int32_t)gettid();
#endif
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
    // (all platforms since 2026-11-25: the true-colour windows use it too, UIImage.h)
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
    im.init_ok = true;
    SYNCLOG("RendererVulkan: ready on '%s'%s", im.core.device_name.c_str(),
            im.want_present ? "" : " (no swapchain: the launcher's background)");
}

void RendererVulkan::render_thread_work()
{
    KFX_ZONE_COLOR("RendererVulkan::render_thread_work", KFX_COLOR_RENDER_GPU);
    Impl& im = *m_impl;
    VkFrameData& fd = im.frames[im.render_idx];
#ifdef KFX_ANDROID
    struct WorkTimer {
        std::atomic<uint64_t>& sum; uint64_t t0; VkCore& core; uint64_t w0;
        WorkTimer(std::atomic<uint64_t>& s, VkCore& c) : sum(s), t0(SDL_GetTicksNS()), core(c),
            w0(c.wait_fence_ns.load() + c.wait_acquire_ns.load()) {}
        ~WorkTimer() { // (the fence and acquire waits are counted apart)
            const uint64_t waited = core.wait_fence_ns.load() + core.wait_acquire_ns.load() - w0;
            const uint64_t spent = SDL_GetTicksNS() - t0;
            sum += spent > waited ? spent - waited : 0;
        }
    } work_timer(im.t_render_ns, im.core);
#endif

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
        for (VkImageObj& img : im.ui_image_img)
            im.core.DestroyImage(img);
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
    m_impl->DrawUIImages(m_impl->frames[m_impl->render_idx], UI_IMAGE_UNDER, UI_IMAGE_SLOTS); // (the pictures under the game's UI)
    m_impl->ui.DrawGameUILayerRT(m_impl->frames[m_impl->render_idx].text_cmds, &m_impl->text);
}

void RendererVulkan::FGDrawFrontOverlay()
{
    m_impl->core.last_pass = "front overlay";
    m_impl->ui.DrawFrontOverlay();
    m_impl->DrawUIImages(m_impl->frames[m_impl->render_idx], 0, UI_IMAGE_UNDER);
}

bool RendererVulkan::SubmitUIImage(int slot, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* rgba, uint32_t gen)
{
    if (m_impl == nullptr || !m_impl->init_ok || slot < 0 || slot >= UI_IMAGE_SLOTS)
        return false;
    return m_impl->ui_image_src[slot].Submit(m_impl->frames[m_impl->write_idx].ui_image[slot], x, y, w, h, rgba, gen);
}

// Render thread: the true-colour window over the UI (uploaded when it is a new copy; an upload goes in the upload
// command buffer, before the frame's draws, as the Android overlay's).
void RendererVulkan::Impl::DrawUIImages(VkFrameData& fd, int first, int last)
{
    bool bound = (first < UI_IMAGE_UNDER); // (the front overlay draws into the screen already)
    for (int slot = first; slot < last; slot++)
    {
        if (!bound && fd.ui_image[slot].on)
        {
            /* before the game UI's layer: the screen as the target (an overhead map or a picture-in-picture capture
               may have had another) */
            resource_mapper.BindScreen();
            core.SetViewport(0.0f, 0.0f, (float)fd.screen_w, (float)fd.screen_h);
            bound = true;
        }
        DrawUIImage(fd, slot);
    }
}

void RendererVulkan::Impl::DrawUIImage(VkFrameData& fd, int slot)
{
    const UIImageFrame& pic = fd.ui_image[slot];
    if (!pic.on || !pic.rgba || pic.w <= 0 || pic.h <= 0 || pic.rgba->size() != (size_t)pic.w * (size_t)pic.h * 4)
        return;
    if (!overlay_prog.Valid() || fd.screen_w <= 0 || fd.screen_h <= 0)
        return;
    core.last_pass = "true-colour window";
    if (ui_image_img[slot].image == VK_NULL_HANDLE || ui_image_img[slot].width != pic.w || ui_image_img[slot].height != pic.h)
    {
        core.DestroyImage(ui_image_img[slot]);
        ui_image_uploaded[slot] = nullptr;
        if (!core.CreateImage(ui_image_img[slot], pic.w, pic.h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "ui_rgba"))
            return;
        ui_image_img[slot].sampler = core.GetSampler(false, false);
        core.InitSampledImage(ui_image_img[slot]);
    }
    if ((ui_image_uploaded[slot] != pic.rgba.get()) || (ui_image_uploaded_gen[slot] != pic.gen))
    {
        core.UploadImage(ui_image_img[slot], 0, 0, 0, pic.w, pic.h, pic.rgba->data(), (uint32_t)pic.w * 4, 4);
        ui_image_uploaded[slot] = pic.rgba.get();
        ui_image_uploaded_gen[slot] = pic.gen;
    }
    core.SetScissorEnabled(false);
    const float x0 = (float)pic.x, y0 = (float)pic.y, x1 = (float)(pic.x + pic.w), y1 = (float)(pic.y + pic.h);
    const float verts[] = {
        x0, y0, 0, 0,   x1, y0, 1, 0,   x1, y1, 1, 1,
        x0, y0, 0, 0,   x1, y1, 1, 1,   x0, y1, 0, 1,
    };
    overlay_prog.Set2f("u_screen_size", (float)fd.screen_w, (float)fd.screen_h);
    overlay_prog.SetTexture("u_image", &ui_image_img[slot]);
    VkPipeState st;
    st.Blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    st.layout_id = overlay_layout;
    if (!overlay_prog.Bind(st))
        return;
    core.BindVertices(verts, sizeof(verts));
    vkCmdDraw(core.Cmd(), 6, 1, 0, 0);
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
    if (fd.overlay_rows <= 0 || fd.overlay_y0 < 0 || fd.overlay_y0 + fd.overlay_rows > fd.screen_h
        || fd.overlay_rgba.size() != (size_t)fd.screen_w * fd.overlay_rows * 4)
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
    im.core.UploadImage(im.overlay_img, 0, fd.overlay_y0, 0, fd.screen_w, fd.overlay_rows, fd.overlay_rgba.data(),
                        (uint32_t)fd.screen_w * 4, 4);
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
#ifdef KFX_ANDROID
    const uint64_t t_wait0 = SDL_GetTicksNS();
    im.thread_mgr.WaitForCompletion();
    im.t_wait_ns += SDL_GetTicksNS() - t_wait0;
#else
    im.thread_mgr.WaitForCompletion();
#endif

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
    for (UIImageFrame& f : next_fd.ui_image)
        f.on = false;
    next_fd.palette_dirty = false;
    memcpy(next_fd.palette_rgba, im.frames[filled].palette_rgba, sizeof(next_fd.palette_rgba));
    next_fd.clear_index = im.frames[filled].clear_index;

    [[maybe_unused]] const bool world_drawn = im.world.HasPendingCommands(); // (the level's 3D view this frame; menus have none)
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
#ifdef KFX_ANDROID
    const uint64_t t_ov0 = SDL_GetTicksNS();
    im.DrawAndroidOverlay(filled_fd);
    im.t_overlay_ns += SDL_GetTicksNS() - t_ov0;
#else
    im.DrawAndroidOverlay(filled_fd);
#endif

    LbMouseOnBeginSwap();
    filled_fd.cursor_snapshot = im.cursor.Snapshot();
    LbMouseOnEndSwap();

    im.render_idx = filled;
    im.write_idx  = next;
    RendererFrameCounter_Advance();
    im.thread_mgr.Signal();
#ifdef KFX_ANDROID
    // a new swapchain (the first one too: Init asked before there was one) may lose the refresh rate asked for
    const uint32_t serial = im.core.swapchain_serial.load();
    if (serial != im.refresh_serial && im.window != nullptr)
    {
        im.refresh_serial = serial;
        KfxAndroidConfigureWindow(im.window);
    }
    const uint64_t t_pace0 = SDL_GetTicksNS();
    KfxAndroidFramePresented(); // frame pacing without vsync, the FPS counter, the second screen
    im.t_pace_ns += SDL_GetTicksNS() - t_pace0;
    im.AndroidHintFrame(world_drawn);
    im.AndroidTiming();
#endif
}


#ifdef KFX_ANDROID
namespace {
// libandroid's APerformanceHint_* (API 33+), looked up at run time so the engine needs no newer API level to build
struct HintApi {
    void* (*getManager)() = nullptr;
    void* (*createSession)(void*, const int32_t*, size_t, int64_t) = nullptr;
    int (*updateTarget)(void*, int64_t) = nullptr;
    int (*reportActual)(void*, int64_t) = nullptr;
    void (*closeSession)(void*) = nullptr;
    bool ok = false;
};
const HintApi& hint_api()
{
    static HintApi api;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (lib != nullptr)
        {
            api.getManager = (void* (*)())dlsym(lib, "APerformanceHint_getManager");
            api.createSession = (void* (*)(void*, const int32_t*, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
            api.updateTarget = (int (*)(void*, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
            api.reportActual = (int (*)(void*, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
            api.closeSession = (void (*)(void*))dlsym(lib, "APerformanceHint_closeSession");
            api.ok = api.getManager && api.createSession && api.updateTarget && api.reportActual && api.closeSession;
        }
    }
    return api;
}
/** The frame's deadline: a bit under the screen's refresh (or the player's lower cap). */
int64_t hint_target_for(SDL_Window* window)
{
    float hz = 0.0f;
    const SDL_DisplayMode* mode = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
    if (mode != nullptr)
        hz = mode->refresh_rate;
    if (fps_limit_current > 0 && (hz <= 0.0f || fps_limit_current < hz))
        hz = (float)fps_limit_current;
    if (hz < 20.0f)
        hz = 60.0f;
    // 85% of the frame: with the target at the full frame Android kept the clocks where frames took 7.0-7.3 ms of
    // 6.94 (135-138 FPS at 144 Hz); a margin lets it raise them enough for every refresh
    return (int64_t)(0.85e9 / hz);
}
/** The cores' clocks now, "little/mid prime" in MHz (empty where Android does not let an app read them). */
std::string cpu_clocks()
{
    std::string out;
    for (int policy = 0; policy < 12; policy++)
    {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", policy);
        FILE* f = fopen(path, "r");
        if (f == nullptr)
            continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1)
        {
            if (!out.empty()) out += "/";
            out += std::to_string(khz / 1000);
        }
        fclose(f);
    }
    return out.empty() ? std::string("not readable") : out + " MHz";
}
}
#endif

void RendererVulkan::Impl::AndroidHintStart()
{
#ifdef KFX_ANDROID
    if (!hint_api().ok)
    {
        SYNCLOG("RendererVulkan: no Android performance hint (libandroid has no APerformanceHint)");
        return;
    }
    hint_wanted = true;
    SYNCLOG("RendererVulkan: Android performance hint while a level is drawn (game thread %d, render thread %d)",
            (int)gettid(), (int)render_tid);
#endif
}

/** Only while the world is drawn (2026-11-24): in the menus the frames are far below the target, Android then holds
 *  the threads' cores down, and a Continue Game started from there loaded in 5.4 s instead of 0.7-1.6 s. */
bool RendererVulkan::Impl::AndroidHintOpen()
{
#ifdef KFX_ANDROID
    const HintApi& api = hint_api();
    void* manager = api.getManager();
    const int32_t tids[2] = { (int32_t)gettid(), render_tid };
    hint_target_ns = hint_target_for(window);
    if (manager != nullptr && render_tid > 0)
        hint_session = api.createSession(manager, tids, 2, hint_target_ns);
    if (hint_session == nullptr)
    {
        SYNCLOG("RendererVulkan: Android performance hint not given (this phone made no session)");
        hint_wanted = false;
        return false;
    }
    hint_last_ns = 0;
    return true;
#else
    return false;
#endif
}

/** Game thread, once a frame: how long the frame took, less the time the render thread only waited (for the GPU, or
 *  for a free image when the screen sets the pace). Taking turns, both threads' work lies on the frame's path. */
void RendererVulkan::Impl::AndroidHintFrame(bool world_drawn)
{
#ifdef KFX_ANDROID
    if (!hint_wanted)
        return;
    if (world_drawn)
    {
        hint_quiet_frames = 0;
        if (hint_session == nullptr && !AndroidHintOpen())
            return;
    }
    else if (hint_session != nullptr && ++hint_quiet_frames >= 10)
    {
        AndroidHintStop();
        return;
    }
    if (hint_session == nullptr)
        return;
    const uint64_t now = SDL_GetTicksNS();
    const uint64_t waited = core.wait_total_ns.load();
    if (hint_last_ns != 0)
    {
        const uint64_t span = now - hint_last_ns;
        const uint64_t idle = waited - hint_last_wait_ns;
        int64_t work = (int64_t)(span > idle ? span - idle : 0);
        if (work < 100000) work = 100000; // (a report must be above zero)
        hint_api().reportActual(hint_session, work);
        t_hint_work_ns += (uint64_t)work;
    }
    hint_last_ns = now;
    hint_last_wait_ns = waited;
    if ((t_frames & 63) == 0)
    {
        const int64_t target = hint_target_for(window);
        if (target != hint_target_ns)
        {
            hint_target_ns = target;
            hint_api().updateTarget(hint_session, target);
        }
    }
#endif
}

void RendererVulkan::Impl::AndroidHintStop()
{
#ifdef KFX_ANDROID
    if (hint_session != nullptr)
        hint_api().closeSession(hint_session);
    hint_session = nullptr;
#endif
}

/** Android (2026-11-24, Vulkan slower than OpenGL ES on one phone): every 5 s, where a frame's time went. */
void RendererVulkan::Impl::AndroidTiming()
{
#ifdef KFX_ANDROID
    const uint64_t now = SDL_GetTicksNS();
    if (t_start == 0)
    {
        t_start = now;
        core.wait_fence_ns = 0; core.wait_acquire_ns = 0; core.wait_present_ns = 0;
        return;
    }
    t_frames++;
    if (now - t_start < 5000000000ull)
        return;
    const double n = (double)t_frames, ms = 1e-6 / n;
    float hz = 0.0f;
    const SDL_DisplayMode* mode = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
    if (mode != nullptr)
        hz = mode->refresh_rate;
    const int turns = core.swap_rotation.load();
    char hint_buf[64];
    if (hint_session != nullptr)
        snprintf(hint_buf, sizeof(hint_buf), "work %.2f of %.2f ms", t_hint_work_ns * ms, hint_target_ns * 1e-6);
    else
        snprintf(hint_buf, sizeof(hint_buf), hint_wanted ? "off (no level drawn)" : "none");
    const std::string hint_line = hint_buf;
    SYNCLOG("Vulkan timing: %.1f FPS over %.1f s; ms a frame: waiting for the render thread %.2f, overlay %.2f, app pacing %.2f;"
            " render thread busy %.2f, waits: frame fence %.2f, image acquire %.2f, present %.2f; render thread found busy %.2f"
            " times a frame; display %.1f Hz, draw limit %d; %s; hint %s; CPU %s",
            n * 1e9 / (double)(now - t_start), (now - t_start) * 1e-9, t_wait_ns * ms, t_overlay_ns * ms, t_pace_ns * ms,
            t_render_ns.exchange(0) * ms,
            core.wait_fence_ns.exchange(0) * ms, core.wait_acquire_ns.exchange(0) * ms, core.wait_present_ns.exchange(0) * ms,
            t_busy_polls / n, hz, (int)fps_limit_current,
            turns < 0 ? "no swapchain" : turns == 0 ? "not pre-rotated" : turns == 1 ? "pre-rotated 90"
            : turns == 2 ? "pre-rotated 180" : "pre-rotated 270",
            hint_line.c_str(), cpu_clocks().c_str());
    t_start = now;
    t_frames = t_wait_ns = t_overlay_ns = t_pace_ns = t_busy_polls = t_hint_work_ns = 0;
#endif
}

bool RendererVulkan::BeginFrame()
{
    return m_impl != nullptr && m_impl->init_ok;
}

bool RendererVulkan::CanPresent() const
{
    if (m_impl == nullptr)
        return false;
    const bool done = m_impl->thread_mgr.IsWorkDone();
#ifdef KFX_ANDROID
    if (!done)
        m_impl->t_busy_polls++;
#endif
    return done;
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

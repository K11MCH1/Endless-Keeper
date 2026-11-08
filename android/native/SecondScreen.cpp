// The level map on a second built-in screen (Retroid Pocket Duo). GameActivity's SecondScreen
// shows a presentation window there and hands its surface and touches to this file. After
// each presented frame KeeperFX's own map drawing (the parchment, the overhead map and the
// magnifier) runs again into a private 8-bit buffer (640x480 on the Retroid, shown at twice the
// size): the software renderer draws wherever lbDisplay points, so lbDisplay, the UI scale and
// the parchment image are swapped in for the call and put back. The map is laid out as large
// as whole pixels per slab allow; a poster thread takes the buffer to the surface through the
// game palette, so the game never waits for that screen.
// On that screen a finger is the hovering mouse of KeeperFX's map screen: while it is down on
// the map the magnifier shows what is under it (above the finger, where the hand does not
// cover it), and a quick tap is the click, flying the camera there as the event buttons do.
// KeeperFX's panel map button in the corner switches between that layout (Large) and Fill,
// which zooms the picture until the map's top and bottom touch the screen's edges.
//
#include "AndroidRuntime.hpp"
#include "bflib_basics.h"
#include "bflib_planar.h"
#include "bflib_video.h"
#include "kfx/renderer/RendererManager.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "keeperfx.hpp"
#include "globals.h"
#include "gui_parchment.h"
#include "custom_sprites.h"
#include "sprites.h"
#include "local_camera.h"
#include "player_data.h"
#include "engine_camera.h"
#include "map_data.h"
#include "game_legacy.h"
#include <android/native_window_jni.h>
#include <jni.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// gui_parchment.c, without header declarations: draw_2d_map's three parts (it fixes the
// layout) and draw_zoom_box's two (it takes the mouse and the layout from the main screen).
extern "C" {
void draw_overhead_map(const struct TbRect *map_area, long block_size, PlayerNumber plyr_idx);
void draw_overhead_things(const struct TbRect *map_area, long block_size, PlayerNumber plyr_idx);
void draw_overhead_room_icons(const struct TbRect *map_area, long block_size, PlayerNumber plyr_idx);
void draw_zoom_box_terrain(long scrtop_x, long scrtop_y, int stl_x, int stl_y, PlayerNumber plyr_idx, long draw_tiles_x, long draw_tiles_y, int subtile_size);
void draw_zoom_box_things(long scrtop_x, long scrtop_y, int stl_x, int stl_y, PlayerNumber plyr_idx, long draw_tiles_x, long draw_tiles_y, int subtile_size);
}

namespace {
constexpr int ParchmentW = 640, ParchmentH = 480; // the hi-res parchment image
constexpr uint64_t TapMs = 350;
std::mutex guard; // the window and the touch queue: Java threads against the game thread
ANativeWindow* window = nullptr;
int windowW = 0, windowH = 0;
bool blank = false;
struct Touch { int action; float x, y; int w, h; };
std::vector<Touch> touches;
int frameW = 0, frameH = 0; // the draw buffer: a whole fraction of the screen, at least the parchment's size
std::vector<TbPixel> frame, parchment(ParchmentW * ParchmentH);
bool parchmentTried = false, parchmentReady = false;
std::atomic<uint64_t> levelAt{0};
std::atomic<bool> fill{false};
uint64_t drawnAt = 0;
struct TbRect mapArea{}, iconArea{};
long mapBlock = 0;
// The finger on the screen, in frame pixels.
bool fingerDown = false, fingerMoved = false, iconPressed = false;
float fingerX = 0, fingerY = 0, downX = 0, downY = 0;
uint64_t downAt = 0;
uint64_t costNs = 0; int costCount = 0;

bool inside(const TbRect& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

bool inLevel() {
    uint64_t at = levelAt.load();
    return at && SDL_GetTicks() - at < 1000 && get_my_player() != nullptr && local_state.camera.ready;
}

// The map as large as whole pixels per slab allow, with a little parchment around it.
void layoutMap() {
    mapBlock = std::max(1L, std::min<long>(frameW * 94 / 100 / game.map_tiles_x, frameH * 94 / 100 / game.map_tiles_y));
    mapArea.left = (frameW - mapBlock * game.map_tiles_x) / 2;
    mapArea.top = (frameH - mapBlock * game.map_tiles_y) / 2;
    mapArea.right = mapArea.left + mapBlock * game.map_tiles_x;
    mapArea.bottom = mapArea.top + mapBlock * game.map_tiles_y;
}

// How the frame lands on a surface of this size: screen = frame * scale + offset. Large shows
// the whole frame, centred; Fill scales the map's height to the screen's, centred on the map.
struct View {
    float scale, ox, oy;
    TbRect visible; // the frame pixels on screen
};
View view(int w, int h) {
    View v;
    if (fill && mapArea.bottom > mapArea.top) {
        v.scale = static_cast<float>(h) / (mapArea.bottom - mapArea.top);
        v.ox = w / 2.0f - v.scale * (mapArea.left + mapArea.right) / 2.0f;
        v.oy = -v.scale * mapArea.top;
    } else {
        v.scale = std::min(static_cast<float>(w) / frameW, static_cast<float>(h) / frameH);
        v.ox = (w - frameW * v.scale) / 2; v.oy = (h - frameH * v.scale) / 2;
    }
    v.visible.left = std::max(0L, static_cast<long>(std::ceil(-v.ox / v.scale)));
    v.visible.top = std::max(0L, static_cast<long>(std::ceil(-v.oy / v.scale)));
    v.visible.right = std::min<long>(frameW, static_cast<long>((w - v.ox) / v.scale));
    v.visible.bottom = std::min<long>(frameH, static_cast<long>((h - v.oy) / v.scale));
    return v;
}

// KeeperFX's magnifier at the finger, as draw_zoom_box does at the mouse.
void drawMagnifier(PlayerNumber plyr, const TbRect& visible) {
    long draw_tiles = 13, subtile_unscaled = 8;
    switch (get_local_minimap_zoom()) { // draw_zoom_box's sizes per minimap zoom
    case 128: draw_tiles = 6; subtile_unscaled = 18; break;
    case 256: draw_tiles = 9; subtile_unscaled = 12; break;
    case 512: draw_tiles = 12; subtile_unscaled = 9; break;
    case 1024: draw_tiles = 18; subtile_unscaled = 6; break;
    case 2048: draw_tiles = 36; subtile_unscaled = 3; break;
    }
    const int subtile_size = scale_value_for_resolution(subtile_unscaled);
    long box = draw_tiles * subtile_size, gap = scale_value_for_resolution(24);
    long scrtop_x = std::clamp(static_cast<long>(fingerX) + gap, visible.left, std::max(visible.left, visible.right - box));
    long scrtop_y = static_cast<long>(fingerY) - gap - box; // above the finger, or below when there is no room
    if (scrtop_y < visible.top) scrtop_y = std::min(static_cast<long>(fingerY) + gap, std::max(visible.top, visible.bottom - box));
    int stl_x = static_cast<int>(STL_PER_SLB * (fingerX - mapArea.left) / mapBlock) - draw_tiles / 2;
    int stl_y = static_cast<int>(STL_PER_SLB * (fingerY - mapArea.top) / mapBlock) - draw_tiles / 2;
    if (stl_x < -draw_tiles/2 || stl_x >= game.map_subtiles_x+1-draw_tiles/2 || stl_y < -draw_tiles/2 || stl_y >= game.map_subtiles_y+1-draw_tiles/2)
        return;
    RendererSetZoomBoxScreenRect(scrtop_x, scrtop_y, scrtop_x + box, scrtop_y + box);
    draw_zoom_box_terrain(scrtop_x, scrtop_y, stl_x, stl_y, plyr, draw_tiles, draw_tiles, subtile_size);
    draw_zoom_box_things(scrtop_x, scrtop_y, stl_x, stl_y, plyr, draw_tiles, draw_tiles, subtile_size);
    RendererClearZoomBoxScreenRect();
    int units = (74 * units_per_pixel) / get_button_sprite(GBS_parchment_map_frame_deco_b_tl)->SWidth;
    int beg_x = scrtop_x - scale_value_for_resolution(20), beg_y = scrtop_y - scale_value_for_resolution(24);
    int end_x = scrtop_x - scale_value_for_resolution(46) + box, end_y = scrtop_y - scale_value_for_resolution(58) + box;
    UIRenderer_SubmitButtonSprite(beg_x, beg_y, units, get_button_sprite(GBS_parchment_map_frame_deco_b_tl));
    UIRenderer_SubmitButtonSprite(end_x, beg_y, units, get_button_sprite(GBS_parchment_map_frame_deco_b_tr));
    UIRenderer_SubmitButtonSprite(beg_x, end_y, units, get_button_sprite(GBS_parchment_map_frame_deco_b_bl));
    UIRenderer_SubmitButtonSprite(end_x, end_y, units, get_button_sprite(GBS_parchment_map_frame_deco_b_br));
}

// Brightness of a palette entry (6-bit channels, weighted like the eye: 0-378).
int brightness(const unsigned char* pal, int i) { return 2 * pal[i*3] + 3 * pal[i*3+1] + pal[i*3+2]; }

// The palette's brightest and darkest entries.
void paletteExtremes(int& light, int& dark) {
    const unsigned char* pal = RendererGetActivePalette();
    int best = -1, worst = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int b = brightness(pal, i);
        if (b > best) { best = b; light = i; }
        if (b < worst) { worst = b; dark = i; }
    }
}

// The camera's centre: a light square between dark ones, a few slabs across.
void drawCameraMark(const struct Camera* camera) {
    int light = 0, dark = 0;
    paletteExtremes(light, dark);
    long cx = mapArea.left + mapBlock * camera->mappos.x.val / (COORD_PER_STL * STL_PER_SLB);
    long cy = mapArea.top + mapBlock * camera->mappos.y.val / (COORD_PER_STL * STL_PER_SLB);
    long half = std::max(4L, mapBlock * 5 / 2), width = std::max(1L, mapBlock / 4);
    auto plot = [&](long x, long y, int colour) {
        if (x >= 0 && x < frameW && y >= 0 && y < frameH) frame[y * frameW + x] = static_cast<TbPixel>(colour);
    };
    auto box = [&](long r, int colour) {
        for (long d = -r; d <= r; d++) {
            plot(cx + d, cy - r, colour); plot(cx + d, cy + r, colour);
            plot(cx - r, cy + d, colour); plot(cx + r, cy + d, colour);
        }
    };
    box(half + width, dark);
    for (long r = half; r > half - width; r--) box(r, light);
    box(half - width, dark);
}

// KeeperFX's parchment, overhead map, camera mark, magnifier and map button into `frame`.
void drawMap() {
    auto* player = get_my_player();
    layoutMap();
    View v = view(windowW, windowH);
    TbDisplayStruct display = lbDisplay;
    unsigned short scale = units_per_pixel, flags = RendererGetDrawFlags();
    unsigned char* sharedParchment = hires_parchment;
    int sharedLoaded = parchment_loaded;
    lbDisplay.WScreen = lbDisplay.GraphicsWindowPtr = frame.data();
    lbDisplay.PhysicalScreenWidth = lbDisplay.GraphicsScreenWidth = lbDisplay.GraphicsWindowWidth = frameW;
    lbDisplay.PhysicalScreenHeight = lbDisplay.GraphicsScreenHeight = lbDisplay.GraphicsWindowHeight = frameH;
    lbDisplay.GraphicsWindowX = lbDisplay.GraphicsWindowY = 0;
    units_per_pixel = std::max(frameW, frameH) / 40; // vidmode.c's rule
    // The engine's parchment buffer is shared scratch, reloaded whenever its map screen opens;
    // the second screen keeps its own copy, loaded once.
    hires_parchment = parchment.data();
    if (!parchmentTried) {
        parchmentTried = true;
        parchment_loaded = 0;
        reload_parchment_file(true);
        parchmentReady = parchment_loaded != 0;
        LbSyncLog("AndroidSecondScreen parchment=%d frame=%dx%d\n", static_cast<int>(parchmentReady), frameW, frameH);
    }
    parchment_loaded = parchmentReady;
    if (parchmentReady) draw_map_parchment();
    else std::fill(frame.begin(), frame.end(), 0);
    draw_overhead_map(&mapArea, mapBlock, player->id_number);
    draw_overhead_things(&mapArea, mapBlock, player->id_number);
    draw_overhead_room_icons(&mapArea, mapBlock, player->id_number);
    if (const auto* camera = get_local_active_camera()) drawCameraMark(camera);
    if (fingerDown && !iconPressed && inside(mapArea, fingerX, fingerY)) drawMagnifier(player->id_number, v.visible);
    // The panel's map button in the visible top-right corner, a tenth of the screen tall,
    // pressed while a finger is on it.
    const struct TbSprite* icon = get_panel_sprite(iconPressed ? GPS_rpanel_rpanel_btn_bigmap_act : GPS_rpanel_rpanel_btn_bigmap_std);
    float iconH = windowH / 10.0f / v.scale, margin = windowH / 60.0f / v.scale;
    int iconUnits = std::max(1, static_cast<int>(16 * iconH / std::max<int>(1, icon->SHeight)));
    int spriteW = icon->SWidth * iconUnits / 16, spriteH = icon->SHeight * iconUnits / 16;
    iconArea.right = v.visible.right - static_cast<long>(margin); iconArea.left = iconArea.right - spriteW;
    iconArea.top = v.visible.top + static_cast<long>(margin); iconArea.bottom = iconArea.top + spriteH;
    // Only the scroll goes on the parchment: the sprite is a corner piece of the side panel,
    // and its stone rim and face looked like a hole cut in the parchment. The sprite is drawn
    // on its own, and the light pixels of its middle (the scroll) are copied.
    if (spriteW > 0 && spriteH > 0) {
        int light = 0, dark = 0;
        paletteExtremes(light, dark);
        static std::vector<TbPixel> scratch;
        scratch.assign(static_cast<size_t>(spriteW) * spriteH, static_cast<TbPixel>(dark));
        lbDisplay.WScreen = lbDisplay.GraphicsWindowPtr = scratch.data();
        lbDisplay.PhysicalScreenWidth = lbDisplay.GraphicsScreenWidth = lbDisplay.GraphicsWindowWidth = spriteW;
        lbDisplay.PhysicalScreenHeight = lbDisplay.GraphicsScreenHeight = lbDisplay.GraphicsWindowHeight = spriteH;
        UIRenderer_SubmitPanelSpriteRaw(0, 0, iconUnits, icon, 0);
        const unsigned char* pal = RendererGetActivePalette();
        for (int y = spriteH * 25 / 100; y < spriteH * 78 / 100; y++)
            for (int x = spriteW * 20 / 100; x < spriteW * 80 / 100; x++) {
                TbPixel p = scratch[static_cast<size_t>(y) * spriteW + x];
                long fx = iconArea.left + x, fy = iconArea.top + y;
                // The scroll's tones measure 146-241 here, the face behind it 22-94.
                if (brightness(pal, p) >= 120 && fx >= 0 && fx < frameW && fy >= 0 && fy < frameH) frame[fy * frameW + fx] = p;
            }
    }
    hires_parchment = sharedParchment;
    parchment_loaded = sharedLoaded;
    units_per_pixel = scale;
    RendererSetDrawFlags(flags);
    lbDisplay = display;
}

// A finished picture for the poster thread, which waits for the screen's buffer and converts
// there, off the game thread.
struct Job {
    ANativeWindow* window = nullptr; // acquired for the job
    unsigned generation = 0;
    bool black = true;
    std::vector<TbPixel> pixels;
    int w = 0, h = 0;
    uint32_t colours[256];
    View v{};
};
std::mutex jobGuard;
std::condition_variable jobReady;
Job job;
bool jobPending = false;
std::thread poster;
unsigned windowGeneration = 0;

// A job to the surface: the frame through its colours, or black.
void write(const Job& j) {
    static unsigned configured = 0;
    if (j.generation != configured) {
        ANativeWindow_setBuffersGeometry(j.window, 0, 0, WINDOW_FORMAT_RGBX_8888);
        configured = j.generation;
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(j.window, &buffer, nullptr) != 0) return;
    auto* rows = static_cast<uint32_t*>(buffer.bits);
    size_t stride = buffer.stride;
    const uint32_t black = 0xFF000000u;
    if (j.black) {
        for (int y = 0; y < buffer.height; y++) std::fill(rows + y * stride, rows + y * stride + buffer.width, black);
        ANativeWindow_unlockAndPost(j.window);
        return;
    }
    const View& v = j.v;
    int k = static_cast<int>(v.scale), ox = static_cast<int>(v.ox), oy = static_cast<int>(v.oy);
    if (k >= 1 && v.scale == k && v.ox == ox && v.oy == oy && ox >= 0 && oy >= 0 && ox + j.w * k <= buffer.width && oy + j.h * k <= buffer.height) {
        // Whole pixels: each frame row is widened once and copied to the rows below it.
        for (int y = 0; y < buffer.height; y++)
            if (y < oy || y >= oy + j.h * k) std::fill(rows + y * stride, rows + y * stride + buffer.width, black);
        for (int sy = 0; sy < j.h; sy++) {
            uint32_t* row = rows + (oy + static_cast<size_t>(sy) * k) * stride;
            const TbPixel* src = j.pixels.data() + static_cast<size_t>(sy) * j.w;
            std::fill(row, row + ox, black);
            uint32_t* out = row + ox;
            for (int x = 0; x < j.w; x++) { uint32_t c = j.colours[src[x]]; for (int r = 0; r < k; r++) *out++ = c; }
            std::fill(out, row + buffer.width, black);
            for (int r = 1; r < k; r++) std::copy(row, row + buffer.width, row + r * stride);
        }
    } else {
        // Any scale (Fill): nearest frame pixel; a row repeating the one above is copied.
        std::vector<int> column(buffer.width); // source column, or -1 outside the frame
        for (int x = 0; x < buffer.width; x++) {
            int sx = static_cast<int>(std::floor((x - v.ox) / v.scale));
            column[x] = sx < 0 || sx >= j.w ? -1 : sx;
        }
        int previous = -1;
        for (int y = 0; y < buffer.height; y++) {
            uint32_t* row = rows + y * stride;
            int sy = static_cast<int>(std::floor((y - v.oy) / v.scale));
            if (sy < 0 || sy >= j.h) { std::fill(row, row + buffer.width, black); previous = -1; continue; }
            if (sy == previous) { std::copy(row - stride, row - stride + buffer.width, row); continue; }
            const TbPixel* src = j.pixels.data() + static_cast<size_t>(sy) * j.w;
            for (int x = 0; x < buffer.width; x++) row[x] = column[x] < 0 ? black : j.colours[src[column[x]]];
            previous = sy;
        }
    }
    ANativeWindow_unlockAndPost(j.window);
}

void posterLoop() {
    Job local;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(jobGuard);
            jobReady.wait(lock, [] { return jobPending; });
            std::swap(local, job);
            jobPending = false;
        }
        write(local);
        ANativeWindow_release(local.window);
        local.window = nullptr;
    }
}

// Hands `frame` (or black) to the poster; a picture it has not taken yet is replaced.
// Game thread, holding `guard`.
void queue(bool black) {
    std::lock_guard<std::mutex> lock(jobGuard);
    if (job.window) ANativeWindow_release(job.window);
    ANativeWindow_acquire(window);
    job.window = window; job.generation = windowGeneration; job.black = black;
    if (!black) {
        job.pixels = frame; job.w = frameW; job.h = frameH;
        job.v = view(windowW, windowH);
        const unsigned char* pal = RendererGetActivePalette();
        for (int i = 0; i < 256; i++)
            job.colours[i] = 0xFF000000u | RendererPaletteChannel8(pal[i*3]) | RendererPaletteChannel8(pal[i*3+1]) << 8 | RendererPaletteChannel8(pal[i*3+2]) << 16;
    }
    jobPending = true;
    jobReady.notify_one();
    if (!poster.joinable()) poster = std::thread(posterLoop);
}

void applyTouches() {
    std::vector<Touch> pending;
    pending.swap(touches);
    bool level = inLevel();
    for (const auto& t : pending) {
        if (!level || t.w <= 0 || t.h <= 0) { fingerDown = iconPressed = false; continue; }
        View v = view(t.w, t.h);
        fingerX = (t.x - v.ox) / v.scale; fingerY = (t.y - v.oy) / v.scale;
        uint64_t now = SDL_GetTicks();
        if (t.action == 0) { // ACTION_DOWN
            fingerDown = true; fingerMoved = false; downAt = now; downX = fingerX; downY = fingerY;
            iconPressed = inside(iconArea, fingerX, fingerY);
        } else if (std::hypot(fingerX - downX, fingerY - downY) * v.scale > t.h * .025f) {
            fingerMoved = true;
        }
        if (t.action != 1 && t.action != 3) continue; // ACTION_UP, ACTION_CANCEL end the touch
        bool up = t.action == 1;
        if (iconPressed) {
            if (up && inside(iconArea, fingerX, fingerY)) fill = !fill;
        } else if (up && !fingerMoved && now - downAt < TapMs && mapBlock > 0 && inside(mapArea, fingerX, fingerY)) {
            // A tap is the map screen's click: fly the camera there (looking around with the
            // magnifier moves nothing).
            auto* player = get_my_player();
            int viewType = get_local_view_type();
            if (viewType == PVT_DungeonTop || viewType == PVT_MapScreen) {
                auto coord = [&](float p, long start, long subtiles) {
                    long c = static_cast<long>((p - start) * COORD_PER_STL * STL_PER_SLB / mapBlock);
                    return static_cast<MapCoord>(std::clamp(c, 0L, subtiles * COORD_PER_STL - 1));
                };
                MapCoord x = coord(fingerX, mapArea.left, game.map_subtiles_x), y = coord(fingerY, mapArea.top, game.map_subtiles_y);
                move_local_camera_to_position(x, y);
                if (KfxAndroidInputDiagnostics()) LbSyncLog("AndroidSecondScreen tap map=%.0f,%.0f camera=%d,%d\n", fingerX, fingerY, static_cast<int>(x), static_cast<int>(y));
            }
        }
        fingerDown = iconPressed = false;
    }
}
}

// A game turn of a level ran (KfxAndroidApplyCamera); the map shows while these keep coming.
extern "C" void KfxAndroidSecondScreenLevelTick(void) { levelAt = SDL_GetTicks(); }

// After each presented frame, on the game thread.
extern "C" void KfxAndroidSecondScreenFrame(void) {
    std::lock_guard<std::mutex> lock(guard);
    if (!window) { touches.clear(); return; }
    bool touched = !touches.empty();
    applyTouches();
    if (!inLevel()) {
        if (!blank) { queue(true); blank = true; }
        return;
    }
    uint64_t now = SDL_GetTicks();
    if (!blank && !touched && now - drawnAt < 50) return; // a game turn's pace (20 a second), or at once for a finger
    drawnAt = now; blank = false;
    uint64_t start = SDL_GetTicksNS();
    drawMap();
    queue(false);
    costNs += SDL_GetTicksNS() - start;
    if (++costCount == 300) {
        LbSyncLog("AndroidSecondScreen game_thread_ms=%.2f fill=%d\n", costNs / 1e6 / costCount, static_cast<int>(fill.load()));
        costNs = 0; costCount = 0;
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_keeperrecomped_game_SecondScreen_surface(JNIEnv* env, jclass, jobject surface) {
    std::lock_guard<std::mutex> lock(guard);
    if (window) ANativeWindow_release(window);
    window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    windowGeneration++; blank = false; fingerDown = iconPressed = false;
    if (!window) return;
    // Draw at a whole fraction of the screen no smaller than the parchment (640x480 on the
    // Retroid's 1280x960): the software renderer's cost grows with the pixels, and at full
    // size an update took 13 ms of the game thread.
    windowW = ANativeWindow_getWidth(window); windowH = ANativeWindow_getHeight(window);
    int k = std::max(1, std::min(windowW / ParchmentW, windowH / ParchmentH));
    frameW = std::max(ParchmentW, windowW / k) & ~3;
    frameH = std::max(ParchmentH, windowH / k);
    frame.assign(static_cast<size_t>(frameW) * frameH, 0);
    mapArea = {}; // laid out again for this frame size
    LbSyncLog("AndroidSecondScreen surface=%dx%d frame=%dx%d\n", windowW, windowH, frameW, frameH);
}

extern "C" JNIEXPORT void JNICALL Java_org_keeperrecomped_game_SecondScreen_touch(JNIEnv*, jclass, jint action, jfloat x, jfloat y, jint w, jint h) {
    std::lock_guard<std::mutex> lock(guard);
    if (touches.size() < 256) touches.push_back({action, x, y, w, h});
}

// The size choice (Large or Fill), kept by the Java side between runs.
extern "C" JNIEXPORT void JNICALL Java_org_keeperrecomped_game_SecondScreen_setFill(JNIEnv*, jclass, jboolean on) { fill = on; }
extern "C" JNIEXPORT jboolean JNICALL Java_org_keeperrecomped_game_SecondScreen_isFill(JNIEnv*, jclass) { return fill ? JNI_TRUE : JNI_FALSE; }

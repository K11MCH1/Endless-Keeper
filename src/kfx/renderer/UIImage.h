/******************************************************************************/
// Endless Keeper - a true-colour picture laid over the UI
/******************************************************************************/
/** @file UIImage.h
 *     A window painted in true colour on the CPU (ui_paint.c: the Keeper panel, the Library's window), handed to the
 *     OpenGL and Vulkan renderers as one premultiplied RGBA picture and drawn over the game's UI, under the pointer
 *     (2026-11-25, the user: "Option B and do it in the style of the new Launcher design").
 * @par Design:
 *     The game thread hands the picture over each frame it is shown (RendererSubmitUIImage, in a slot of its own); a
 *     frame that did not is drawn without it. Its pixels are copied only when its generation number changes: both
 *     frame slots share the copy, and the render thread uploads it only then. The software renderer has none of this
 *     (the windows draw their palette version themselves).
 */
/******************************************************************************/
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

/** Pictures a frame may hold, drawn in slot order (a window, its tooltip over it). Slots from UI_IMAGE_UNDER on are
 *  drawn under the game's UI instead, at the start of its layer (the side panel's glass: the game's minimap and
 *  tooltips over it, as over the panel art); the others over it, after the front overlay. */
#define UI_IMAGE_SLOTS 16
#define UI_IMAGE_UNDER 10

struct UIImageFrame {
    bool on = false;
    int32_t x = 0, y = 0, w = 0, h = 0;
    uint32_t gen = 0;
    std::shared_ptr<const std::vector<uint8_t>> rgba; // premultiplied, top row first, w*h*4 bytes
};

/** Game thread: what a renderer keeps between frames to share an unchanged picture's copy. */
struct UIImageSource {
    std::shared_ptr<const std::vector<uint8_t>> last;
    uint32_t last_gen = 0;
    int32_t last_w = 0, last_h = 0;
    /* (copies to reuse: one no frame slot holds any more is written over, not made anew - 20 MB a change at 4K;
       two frame slots hold one each at most) */
    std::shared_ptr<std::vector<uint8_t>> pool[3];

    /** Puts the picture into a frame's slot (the slot of the frame being drawn now). */
    bool Submit(UIImageFrame& slot, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* rgba, uint32_t gen)
    {
        if (w <= 0 || h <= 0 || rgba == nullptr)
            return false;
        if (!last || gen != last_gen || w != last_w || h != last_h)
        {
            const size_t size = (size_t)w * (size_t)h * 4;
            std::shared_ptr<std::vector<uint8_t>> copy;
            for (auto& p : pool)
            {
                if (!p)
                {
                    p = std::make_shared<std::vector<uint8_t>>(size);
                    copy = p;
                    break;
                }
                if (p.use_count() == 1 && p.get() != last.get())
                {
                    p->resize(size);
                    copy = p;
                    break;
                }
            }
            if (!copy)
                copy = std::make_shared<std::vector<uint8_t>>(size); // (all held: a new one, as before)
            memcpy(copy->data(), rgba, size);
            last = copy;
            last_gen = gen;
            last_w = w;
            last_h = h;
        }
        slot.on = true;
        slot.x = x;
        slot.y = y;
        slot.w = w;
        slot.h = h;
        slot.gen = gen;
        slot.rgba = last;
        return true;
    }
};

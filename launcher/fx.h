// Endless Keeper launcher: the effects of the modern looks - soft shadows and glows, gradients, the blurred picture
// behind the boxes, particles, and values that follow their targets smoothly.
#pragma once
#include "imgui.h"
struct SDL_Renderer;

/** False: the still look (no particles, pulses or slides): the player's setting, or Windows' "show animations" off. */
extern bool fx_motion;

void fx_init(SDL_Renderer *renderer);
void fx_shutdown();
/** Once a frame, before anything is drawn. */
void fx_frame(float dt);
float fx_time();

float fx_ease_out(float t);
/** The Dungeon Heart's beat (two pulses, then a rest): 0 at rest, 1 on the beat. The one rhythm all the glows keep. */
float fx_heartbeat();
/** A value that follows its target (it is there after about `seconds`); the first time, and without motion, at once. */
float fx_follow(ImGuiID id, float target, float seconds);

/** A soft dark shadow round a rounded box, `spread` wide and falling a little (nothing under the box itself). */
void fx_shadow(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float spread, ImU32 col);
/** The same in light: it adds to what is behind it. */
void fx_halo(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float spread, ImU32 col);
/** A round glow that adds light / a round soft blot (drawn over). The radii may differ: an ellipse. */
void fx_glow(ImDrawList *dl, ImVec2 centre, float rx, float ry, ImU32 col);
void fx_blot(ImDrawList *dl, ImVec2 centre, float rx, float ry, ImU32 col);
/** A box filled with light that adds, one colour at its top and one at its bottom. */
void fx_light_gradient(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom);
/** A rounded box filled with a gradient, alpha too (top to bottom, or left to right). */
void fx_rect_gradient(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool horizontal = false, ImDrawFlags flags = 0);
/** A level line that fades out at both ends. */
void fx_line_fade(ImDrawList *dl, ImVec2 a, float width, ImU32 col, float thickness);
/** Text with room between its letters. Returns its width (dl null: only measured). */
float fx_text_spaced(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float spacing);

/** What is drawn between the two is faded and moved as one piece (an entrance). */
int fx_group_begin(ImDrawList *dl);
void fx_group_end(ImDrawList *dl, int start, float alpha, ImVec2 offset);

/** The frame is drawn into a picture of its own, so the part behind a box can be blurred: begin before the clear, end
    after the last draw (it puts the picture in the window). Without it (no such textures) all else still works. */
void fx_target_begin(int width, int height);
void fx_target_end();
/** In the background's draw list, after the background: what is drawn so far becomes the blurred backdrop. */
void fx_backdrop_capture(ImDrawList *background);
/** The blurred backdrop inside a box. False when there is none (the caller fills the box darker instead). */
bool fx_backdrop_draw(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImU32 tint);

enum { FX_EMBERS = 1, FX_DUST = 2 };
/** The drifting particles of the whole window (behind the boxes). */
void fx_particles(ImDrawList *dl, int kind, float width, float height, float scale);
/** Sparks off a strip (the Play button's top edge): `rate` a second. Drawn by fx_sparks_draw. */
void fx_sparks(ImVec2 a, ImVec2 b, float rate, float scale);
void fx_sparks_draw(ImDrawList *dl);

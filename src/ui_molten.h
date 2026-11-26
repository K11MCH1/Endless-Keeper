/******************************************************************************/
// Endless Keeper - the launcher's "Molten" look for in-game windows
/******************************************************************************/
/** @file ui_molten.h
 *     The pieces of a window in the launcher's Molten look (test/launcher-modern, launcher/launcher.cpp: panel(),
 *     button_molten(), the ImGui style), painted with ui_paint.h: the window's dark glass, cards, buttons, tabs, bars,
 *     frames, rules. The user chose it 2026-11-25 for the Keeper panel and the Library's window.
 *     `flat` gives the software renderer's version: square, solid, no shadows or glows (it is then reduced to the
 *     game's palette). Coordinates are canvas pixels; `u` is the window's scale (pixels per design unit).
 */
/******************************************************************************/
#ifndef UI_MOLTEN_H
#define UI_MOLTEN_H

#include "ui_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UmStyle {
    float u;        /* pixels per design unit */
    TbBool flat;    /* the software renderer's version */
};

/* the launcher's colours */
#define UM_TEXT     UIC(226, 212, 186, 255)
#define UM_BRIGHT   UIC(240, 228, 204, 255)
#define UM_DIM      UIC(160, 146, 122, 255)
#define UM_SOFT     UIC(200, 186, 160, 255)
#define UM_GOLD     UIC(222, 178, 88, 255)
#define UM_GOLD_HI  UIC(255, 224, 140, 255)
#define UM_LIGHT    UIC(255, 232, 190, 255)
#define UM_EMBER    UIC(255, 150, 70, 255)

enum UmButtonKind { UmB_Normal = 0, UmB_Primary, UmB_Danger };
enum UmState { UmS_Normal = 0, UmS_Hot, UmS_Down, UmS_Off, UmS_Armed };

/** The window: soft shadow, dark glass, warm light along its top, a faint edge. The margin round it the shadow needs
 *  (pixels) is um_window_margin(). */
void um_window(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1);
float um_window_margin(const struct UmStyle *st);
/** A card inside it (a little lighter, a thin edge). */
void um_card(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1);
/** A button with its label in the book face (`px` its size, made smaller to fit). */
void um_button(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, int kind, int state,
    float px, const char *label);
/** A tab: the chosen one lit. */
void um_tab(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, TbBool chosen,
    TbBool hot, float px, const char *label);
/** A level bar `frac` full. */
void um_bar(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float frac);
/** A frame round a picture (portrait, icon): gold, brighter when lit. */
void um_frame(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float radius,
    TbBool lit);
/** A well for a picture: the dark inside of a frame. */
void um_well(struct UiCanvas *c, const struct UmStyle *st, float x0, float y0, float x1, float y1, float radius);
/** A level rule across the window. */
void um_rule(struct UiCanvas *c, const struct UmStyle *st, float x0, float y, float x1);
/** A small heading in the book face, gold. */
void um_heading(struct UiCanvas *c, const struct UmStyle *st, float x, float y, float h, float px, const char *text);

/** A panel icon (the game's GUI panel sprite spr_idx) as a true-colour picture, its baked drop shadow taken off (its
 *  near-black pixels: a well has no ground for it to fall on) and cropped to the picture itself, so it can be put in
 *  the middle of a well (2026-11-25 for the Library's window, the user: "none of the game icons seem to be centered").
 *  Kept for the game's run. NULL: none. */
const unsigned char *um_icon(long spr_idx, int *w, int *h);
/** Draws it in the box (x0, y0)-(x1, y1) as large as `fill` of the box allows, at a whole number of its pixels when that
 *  is near (kept sharp), in the middle, its pixels brighter by `gain` (1: as they are). */
void um_icon_draw(struct UiCanvas *c, long spr_idx, float x0, float y0, float x1, float y1, float fill, float gain);

/** A window painted on a canvas and shown each frame: on OpenGL/Vulkan as a true-colour picture over the UI, in
 *  software reduced to the palette and copied into the screen. Painted again only when its key (all that it shows)
 *  changes. */
/** The picture slots (UIImage.h): over the game's UI 0-9, under it 10-15 (each group drawn in this order) */
enum UmSlot {
    UmSl_KeeperPanel = 0, UmSl_Library = 1, UmSl_LibraryTip = 2, UmSl_Compass = 3, UmSl_Menu = 4, UmSl_Menu2 = 5,
    UmSl_Front = 6, UmSl_Front2 = 7, UmSl_Tooltip = 8, UmSl_Spare = 9,
    UmSl_SideGlass = 10, UmSl_Side = 11, UmSl_Gold = 12, UmSl_Events = 13, UmSl_Dial = 14, UmSl_Boss = 15
};

/** A part of a window: where it is on its canvas (pixels, with room for its glow or shadow) and a hash of what it
 *  shows. */
struct UmPart {
    int x0, y0, x1, y1;
    uint32_t hash;
};
#define UM_PARTS 48

struct UmHost {
    struct UiCanvas canvas;
    unsigned char *idx;         /* the software version's palette indices */
    size_t idx_cap;
    TbBool idx_stale;
    uint32_t gen;
    char key[1536];
    unsigned long paints;
    unsigned long paint_ms;     /* the last painting's time */
    const char *name;           /* for the log and the test switch's files */
    int slot;                   /* its picture slot on OpenGL/Vulkan (one a window shown at once; higher over lower) */
    struct UiCanvas glass;      /* the window's glass and its shadow: changes with its size only */
    char glass_key[128];
    struct UiCanvas base;       /* what changes seldom (the glass, cards, labels), painted under the rest */
    char base_key[256];
    struct UmPart parts[UM_PARTS]; /* the live parts as last painted */
    int nparts;
    int dirty[4];               /* the canvas's rectangle painted last (x0, y0, x1, y1) */
    int shown_x, shown_y;       /* where it was shown last */
};
/** Whether this frame's version is the software renderer's (flat). Test switch KFX_UI_TCDUMP=<folder>: never (the
 *  true-colour version is painted in software too, and each painting saved there as <name>_<n>.rgba: int32 width,
 *  int32 height, premultiplied RGBA; where it is shown is logged; nothing of it reaches the screen). */
TbBool um_host_flat(void);
/** The canvas made cw x ch; true when it has to be painted (its key changed). The caller paints it, then calls
 *  um_host_painted. */
TbBool um_host_begin(struct UmHost *h, int cw, int ch, const char *key);
void um_host_painted(struct UmHost *h, unsigned long started_ms);
/** Within a painting: true when the base (h->base, cw x ch like the canvas) has to be painted for its key; then (in
 *  any case) um_host_base_down lays it on the canvas, and the live part is painted over it. */
TbBool um_host_base_begin(struct UmHost *h, const char *key);
void um_host_base_down(struct UmHost *h);
/** Within a base painting: true when the glass (h->glass, the canvas's size) has to be painted for its key; then (in
 *  any case) um_host_glass_down lays it as the base's ground. */
TbBool um_host_glass_begin(struct UmHost *h, const char *key);
void um_host_glass_down(struct UmHost *h);
/** A hash of formatted text (a part's state). */
uint32_t um_hash(const char *fmt, ...);
/** Adds a part in design units (x, y, w, h, with `room` units round it) on a canvas at scale u with margin m. */
void um_part(struct UmPart *parts, int *n, float u, float m, float x, float y, float w, float h, float room, uint32_t hash);
/** The live part's painting by parts: `full` (the base is new) lays all the base down; else only the parts that
 *  changed since the last painting are restored from the base. False: nothing changed. True: the canvas is clipped
 *  to what must be painted; paint the live part, then um_host_parts_end. */
TbBool um_host_parts(struct UmHost *h, const struct UmPart *parts, int n, TbBool full);
void um_host_parts_end(struct UmHost *h);
/** Shows the canvas this frame with its top left corner at (x, y) on the screen. */
void um_host_show(struct UmHost *h, int x, int y);
void um_host_forget(struct UmHost *h);

#ifdef __cplusplus
}
#endif
#endif

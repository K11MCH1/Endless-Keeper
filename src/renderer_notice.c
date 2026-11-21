/******************************************************************************/
// Endless Keeper
/******************************************************************************/
/** @file renderer_notice.c
 *     The startup notice: which renderer the game started on, top right, for 4 seconds.
 * @par Purpose
 *     Vulkan is the default and falls back to OpenGL (then software) where it cannot run; the player sees
 *     which one this run uses ("Vulkan", or "OpenGL - Vulkan unavailable"). The reason is in the log.
 *     (User's decision, 2026-11-21.)
 */
/******************************************************************************/
#include "pre_inc.h"
#include "bflib_basics.h"
#include "bflib_video.h"
#include "bflib_sprfnt.h"
#include "bflib_datetm.h"
#include "frontend.h"
#include "net_state.h"
#include "world_attract.h"
#include "kfx/renderer/RendererManager.h"
#include <stdio.h>
#include "post_inc.h"

#define NOTICE_MS 4000

static TbClockMSec notice_start = 0;
static TbBool notice_done = false;

void renderer_notice_draw(const char *renderer_name, const char *fallback_note)
{
    if (notice_done || renderer_name == NULL)
        return;
    if (world_attract_active() || netstate_dedicated())
    {
        notice_done = true; /* the launcher's background and a dedicated server show nobody a notice */
        return;
    }
    /* the menus' small font, or the game's when the menus' fonts are not loaded (a level started directly) */
    struct TbSpriteSheet *font = (frontend_font[1] != NULL) ? frontend_font[1] : winfont;
    if (font == NULL || MyScreenWidth <= 0 || MyScreenHeight <= 0)
        return; /* the clock starts once it can be drawn */
    const TbClockMSec now = LbTimerClock();
    if (notice_start == 0)
        notice_start = now;
    if (now - notice_start > NOTICE_MS)
    {
        notice_done = true;
        return;
    }
    char text[96];
    if (fallback_note != NULL && fallback_note[0] != '\0')
        snprintf(text, sizeof(text), "%s - %s", renderer_name, fallback_note);
    else
        snprintf(text, sizeof(text), "%s", renderer_name);

    const TbBool opened = !RendererIsFrameOpen();
    if (opened && !RendererBeginFrame())
        return;
    const unsigned short old_flags = RendererGetDrawFlags();
    LbTextSetFont(font);
    /* the same size on every screen: a line about 1/36 of the screen's height (16 units = 1:1) */
    int line_px = LbTextLineHeight();
    if (line_px <= 0)
        line_px = 16;
    int units = (MyScreenHeight * 16) / (36 * line_px);
    if (units < 16)
        units = 16;
    const int margin = (MyScreenHeight / 90 > 2) ? MyScreenHeight / 90 : 2;
    LbTextSetWindow(0, 0, MyScreenWidth - margin, MyScreenHeight);
    RendererSetDrawFlags(Lb_TEXT_HALIGN_RIGHT);
    LbTextDrawResized(0, margin, units, text);
    RendererSetDrawFlags(old_flags);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    if (opened)
        RendererEndFrame();
}

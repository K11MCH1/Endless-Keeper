/******************************************************************************/
// Endless Keeper - screen safe area for the UI
/******************************************************************************/
/** @file ui_safe_area.c
 *     See ui_safe_area.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "ui_safe_area.h"
#include "bflib_video.h"
#include "frontend.h"
#include "kfx/platform/PlatformManager.h"
#include <stdio.h>
#include <stdlib.h>
#include "post_inc.h"

int ui_safe_left = 0;
int ui_safe_top = 0;
int ui_safe_right = 0;
int ui_safe_bottom = 0;

TbBool ui_safe_area_refresh(void)
{
    int l = 0, t = 0, r = 0, b = 0;
    // test switch KFX_TEST_SAFE_AREA=left,top,right,bottom (game pixels): a phone's corners and camera hole on a PC
    const char *fake = getenv("KFX_TEST_SAFE_AREA");
    if ((fake == NULL) || (sscanf(fake, "%d,%d,%d,%d", &l, &t, &r, &b) != 4))
    {
        l = t = r = b = 0;
        PlatformManager_GetGameSafeInsets(&l, &t, &r, &b);
    }
    // never more than a quarter of the screen from one side (a broken report must not squeeze the UI away)
    const int max_x = MyScreenWidth / 4, max_y = MyScreenHeight / 4;
    l = l < 0 ? 0 : (l > max_x ? max_x : l);
    r = r < 0 ? 0 : (r > max_x ? max_x : r);
    t = t < 0 ? 0 : (t > max_y ? max_y : t);
    b = b < 0 ? 0 : (b > max_y ? max_y : b);
    if (l == ui_safe_left && t == ui_safe_top && r == ui_safe_right && b == ui_safe_bottom)
        return false;
    ui_safe_left = l;
    ui_safe_top = t;
    ui_safe_right = r;
    ui_safe_bottom = b;
    SYNCLOG("UI safe area: insets left %d, top %d, right %d, bottom %d of %dx%d", l, t, r, b, (int)MyScreenWidth, (int)MyScreenHeight);
    return true;
}

void ui_safe_area_changed(void)
{
    if (!ui_safe_area_refresh())
        return;
    if (frontend_menu_state == FeSt_INITIAL) // (a level is shown: its menus were placed with the old insets)
        reinit_all_menus();
}

int ui_safe_right_edge(void)
{
    return (int)MyScreenWidth - ui_safe_right;
}

int ui_safe_bottom_edge(void)
{
    return (int)MyScreenHeight - ui_safe_bottom;
}

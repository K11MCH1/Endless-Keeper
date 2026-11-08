#include "AndroidRuntime.hpp"
#include "TouchGestures.hpp"
#include "bflib_mouse.h"
#include "bflib_mshandler.hpp"
#include "bflib_video.h"
#include "kfx/platform/WindowSystemSDL.h"
#include "kfx/renderer/RendererManager.h"
#include "keeperfx.hpp"
#include "local_camera.h"
#include "player_data.h"
#include "engine_camera.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "bflib_basics.h"
#include "engine_render.h"
#include "thing_list.h"
#include "frontend.h"
#include "gui_frontbtns.h"
#include "bflib_inputctrl.h"
#include "bflib_guibtns.h"
#include "gui_frontmenu.h"
#include "packets.h"
#include "roomspace.h"
#include "thing_creature.h"
#include "globals.h"
#include "engine_lenses.h"
#include "creature_control.h"
#include "frontmenu_net.h"
#include "observer.h"
#include "bflib_keybrd.h"
#include "config_settings.h"
#include "config_terrain.h"
#include "front_input.h"
#include "room_data.h"
#include <jni.h>
// Endless Keeper (2026-11-09): the endless world's full-screen map scrolls with one finger and zooms with a pinch
extern "C" TbBool world_mode_active(void);
extern "C" void parchment_zoom(int dir, long screen_x, long screen_y);
static bool endlessMap() { return get_local_view_type() == PVT_MapScreen && world_mode_active(); }
static float mapZoomAcc = 0;
#include <atomic>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// GUI callbacks without a public header (frontmenu_ingame_tabs.c).
extern "C" {
void pick_up_creature_doing_activity(struct GuiButton *gbtn);
void pick_up_next_creature(struct GuiButton *gbtn);
void gui_area_instance_button(struct GuiButton *gbtn);
}

using namespace keeper::touch;
static Gestures gestures;
static float panX = 0, panY = 0, zoomDelta = 0, rotationDelta = 0;
static uint64_t lastDrain = 0;
static Uint32 presentationGeneration = 0;
static uint64_t inputSnapshotAt = 0;
static int inputThing = 0;
static std::atomic<int> menuRequest{0};
static std::atomic<bool> diagnosticSnapshot{false};
extern "C" JNIEXPORT void JNICALL Java_org_keeperrecomped_game_GameActivity_requestMenu(JNIEnv*,jclass,jint action) { menuRequest.store(action); if (KfxAndroidInputDiagnostics()) LbSyncLog("AndroidMenu request=%d\n", static_cast<int>(action)); }
extern "C" JNIEXPORT void JNICALL Java_org_keeperrecomped_game_TouchProbe_snapshot(JNIEnv*,jclass) { diagnosticSnapshot.store(true); }
extern "C" Uint32 KfxAndroidPresentationGeneration(void) { return presentationGeneration; }
// Android Back behaves exactly like Escape (in-game Options, frontend "back"): inject the
// key so KeeperFX's own Escape handling runs; release it a few frames later so the
// per-frame key latch sees it held.
static int escapeReleaseTicks = 0;
static bool pushKey(SDL_Scancode scancode, SDL_Keycode key, bool down) {
    SDL_Event ev; SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.timestamp = SDL_GetTicksNS();
    ev.key.windowID = SDL_GetWindowID(GetSDLWindowSystem()->GetSDLWindow());
    ev.key.scancode = scancode;
    ev.key.key = key;
    ev.key.down = down;
    return SDL_PushEvent(&ev);
}
static void pushEscape(bool down) {
    bool pushed = pushKey(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, down);
    if (KfxAndroidInputDiagnostics()) LbSyncLog("AndroidMenu escape %s pushed=%d\n", down ? "down" : "up", static_cast<int>(pushed));
}
// A key press held like a real tap of the key. In a level the engine samples keys once
// per game turn (20 per second), so a press of a few frames would go unseen.
struct KeyRelease { SDL_Scancode scancode; SDL_Keycode key; uint64_t at; };
static std::vector<KeyRelease> keyReleases;
static void tapKey(SDL_Scancode scancode, SDL_Keycode key) {
    if (pushKey(scancode, key, true)) keyReleases.push_back({scancode, key, SDL_GetTicks() + 120});
}

static bool rotateWithFingers = true;
static float panSensitivity = 1; // the launcher's camera movement sensitivity
extern "C" void KfxAndroidConfigureTouch(float slop, float pinch, float rotate, int rotate_with_fingers, float pan_sensitivity) {
    gestures.configure({slop, pinch, rotate, 450});
    rotateWithFingers = rotate_with_fingers != 0;
    panSensitivity = pan_sensitivity;
}
static SDL_FPoint gamePosition(float x, float y) {
    int ww = 0, wh = 0;
    GetSDLWindowSystem()->GetWindowSize(&ww, &wh);
    int rx=0, ry=0, rw=ww, rh=wh;
    GetSDLWindowSystem()->GetPresentRect(&rx, &ry, &rw, &rh);
    int dw=ww,dh=wh;
    SDL_GetWindowSizeInPixels(GetSDLWindowSystem()->GetSDLWindow(),&dw,&dh);
    float sx=ww>0?static_cast<float>(dw)/ww:1, sy=wh>0?static_cast<float>(dh)/wh:1;
    float gx = rw > 0 ? (x*sx-rx)*RendererPhysicalWidth()/rw : x;
    float gy = rh > 0 ? (y*sy-ry)*RendererPhysicalHeight()/rh : y;
    return {gx,gy};
}
// Inverse of gamePosition: engine screen coordinates to window coordinates.
static SDL_FPoint windowPosition(float gx, float gy) {
    int ww = 0, wh = 0;
    GetSDLWindowSystem()->GetWindowSize(&ww, &wh);
    int rx=0, ry=0, rw=ww, rh=wh;
    GetSDLWindowSystem()->GetPresentRect(&rx, &ry, &rw, &rh);
    int dw=ww,dh=wh;
    SDL_GetWindowSizeInPixels(GetSDLWindowSystem()->GetSDLWindow(),&dw,&dh);
    float sx=ww>0?static_cast<float>(dw)/ww:1, sy=wh>0?static_cast<float>(dh)/wh:1;
    float x = RendererPhysicalWidth() > 0 ? (gx*rw/RendererPhysicalWidth()+rx)/sx : gx;
    float y = RendererPhysicalHeight() > 0 ? (gy*rh/RendererPhysicalHeight()+ry)/sy : gy;
    return {x,y};
}
static SDL_Point cursor{0,0};
static void setCursor(int gx, int gy) {
    cursor = {std::clamp(gx, 0, std::max(0, static_cast<int>(RendererPhysicalWidth())-1)),
              std::clamp(gy, 0, std::max(0, static_cast<int>(RendererPhysicalHeight())-1))};
    pointerHandler.SetMousePosition(cursor.x, cursor.y);
}
static void position(float x, float y) {
    auto point=gamePosition(x,y);
    setCursor(static_cast<int>(point.x), static_cast<int>(point.y));
}
static void button(unsigned action) { TbPoint delta{0,0}; mouseControl(action, &delta); }
// A tap is a short real click. In a level the button must be seen held for a game turn:
// digging tags on a held press, so a same-frame press and release tagged nothing.
static uint64_t clickReleaseAt = 0;
static GameTurn clickTurn = 0;
static void releaseClick() {
    if (!clickReleaseAt) return;
    clickReleaseAt = 0;
    button(MActn_LBUTTONUP);
}
static void click() {
    releaseClick();
    button(MActn_LBUTTONDOWN);
    clickReleaseAt = SDL_GetTicks() + 80;
    clickTurn = get_gameturn();
}
// Picking up acts on release, so with a creature under the hand click at once, before it
// walks away; anything else gets the held click.
static void tap() {
    const auto* player = get_my_player();
    if (player && get_local_view_type() == PVT_DungeonTop && local_state.local_thing_under_hand > 0) {
        releaseClick();
        button(MActn_LBUTTONDOWN); button(MActn_LBUTTONUP);
    } else click();
}
static void stepClick(uint64_t now) {
    // Wait for a game turn, but never longer than a quarter second (menus, pause).
    if (clickReleaseAt && now >= clickReleaseAt && (get_gameturn() != clickTurn || now >= clickReleaseAt + 170)) releaseClick();
}
static bool inEngineWindow(SDL_FPoint p) {
    return p.x>=local_state.engine_window_x && p.x<local_state.engine_window_x+local_state.engine_window_width
        && p.y>=local_state.engine_window_y && p.y<local_state.engine_window_y+local_state.engine_window_height;
}
static GuiButton* buttonAt(int gx, int gy) {
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++) {
        auto* gbtn = &active_buttons[i];
        if ((gbtn->flags & LbBtnF_Active) == 0 || !active_menus[static_cast<unsigned>(gbtn->gmenu_idx)].is_turned_on) continue;
        if (check_if_pos_is_over_button(gbtn, gx, gy)) return gbtn;
    }
    return nullptr;
}

// Tap and hold on a creature-panel count keeps picking creatures up, one per game
// turn at most (each pickup is a packet action).
static bool leftHeld = false;
static uint64_t nextPickupAt = 0;
static GameTurn lastPickupTurn = 0;
static void repeatPickup(uint64_t now) {
    if (!leftHeld || now < nextPickupAt || get_gameturn() == lastPickupTurn) return;
    const auto* player = get_my_player();
    if (!player || get_local_view_type() != PVT_DungeonTop) return;
    auto* gbtn = buttonAt(cursor.x, cursor.y);
    if (!gbtn || (gbtn->flags & LbBtnF_Enabled) == 0
     || (gbtn->click_event != pick_up_creature_doing_activity && gbtn->click_event != pick_up_next_creature)) return;
    gbtn->click_event(gbtn);
    lastPickupTurn = get_gameturn();
    nextPickupAt = now + 110;
}

// The hand in the dungeon. Taps pick creatures up as clicks do, also with some in the hand.
// To drop, a finger held where the creatures should go and a second finger: a tap of it
// drops one (a right click, which drops on release), a hold keeps dropping, one per game
// turn, until it lifts. A tap on a creature with an empty hand waits for the double-tap
// window: a second tap on it slaps it, otherwise it is picked up. Both go by the creature's
// thing index (the engine's own pick and slap actions), so one that walked off during the
// wait is still the one taken. Two-finger taps and holds neither slap nor drop: they are
// kept free for other uses.
static bool dropHeld = false, rightIgnored = false;
static uint64_t nextDropAt = 0;
static GameTurn lastDropTurn = 0;
static const UserState* handState() {
    auto* player = get_my_player();
    if (!player || player == &local_observer_player || !local_state.camera.ready || get_local_view_type() != PVT_DungeonTop || a_menu_window_is_active()) return nullptr;
    const auto* ustate = get_local_user_state_prediction();
    return user_state_invalid(ustate) || ustate->work_state != PSt_CtrlDungeon ? nullptr : ustate;
}
static bool handHoldsThings() {
    if (!handState()) return false;
    const auto* dungeon = get_players_dungeon(get_my_player());
    return !dungeon_invalid(dungeon) && dungeon->num_things_in_hand > 0;
}
static bool handOverThing() {
    const auto* ustate = handState();
    return ustate && ustate->primary_cursor_state == CSt_PowerHand && local_state.local_thing_under_hand > 0 && !handHoldsThings();
}
static bool dropContext(float x, float y) { return handHoldsThings() && inEngineWindow(gamePosition(x,y)); }
static void dropOne() { releaseClick(); button(MActn_RBUTTONDOWN); button(MActn_RBUTTONUP); }
static void repeatDrop(uint64_t now) {
    if (!dropHeld || now < nextDropAt || get_gameturn() == lastDropTurn || !handHoldsThings()) return;
    dropOne();
    lastDropTurn = get_gameturn();
    nextDropAt = now + 110;
}
struct CreatureTap { bool active = false, second = false; ThingIndex thing = 0; float x = 0, y = 0; uint64_t at = 0; };
static CreatureTap creatureTap;
// Packet actions take one slot per game turn, so quick taps on several creatures queue.
enum class ThingAct { Pick, Slap };
struct QueuedAct { ThingAct act; ThingIndex thing; };
static std::vector<QueuedAct> thingActs;
static GameTurn thingActTurn = 0;
static void queueThingAct(ThingAct act) {
    if (thingActs.empty()) thingActTurn = get_gameturn();
    thingActs.push_back({act, creatureTap.thing});
    creatureTap = {};
}
static void stepThingAct() {
    if (thingActs.empty()) return;
    if (!handState() || get_gameturn() > thingActTurn + 3) { thingActs.clear(); return; }
    if (get_local_packet()->action != PckA_None) return; // the slot frees with the next turn's packet
    auto next = thingActs.front();
    thingActs.erase(thingActs.begin());
    thingActTurn = get_gameturn();
    if (next.act == ThingAct::Pick) set_packet_action(get_local_packet(), PckA_UsePwrHandPick, next.thing, 0, 0, 0);
    else set_packet_power_on_thing(get_local_packet(), PwrK_SLAP, next.thing);
    if (KfxAndroidInputDiagnostics()) LbSyncLog("AndroidHand %s thing=%d turn=%lu\n", next.act == ThingAct::Pick ? "pick" : "slap",
                                                static_cast<int>(next.thing), static_cast<unsigned long>(get_gameturn()));
}

// Double tap and hold drags a dig rectangle: the second touch presses the left button
// in KeeperFX's drag-placement highlight mode and lifting tags the box. In dig context
// a lone tap waits for the double-tap window, so the first tap of a double tap never
// tags its tile (the tile under the press decides tag versus untag).
constexpr uint64_t doubleTapMs = 260;
struct PendingTap { bool active = false; float x = 0, y = 0; uint64_t at = 0; bool room = false; };
static PendingTap pendingTap;
enum class DigDrag { None, Arming, Pressed };
static DigDrag digDrag = DigDrag::None;
static SDL_FPoint digStart{}, digLatest{};
static bool digReleased = false;
static GameTurn digTurn = 0, digReleaseTurn = 0, restoreTurn = 0;
static int restoreMode = -1, restoreTries = 0;
// A hold on a wall, or in the dungeon with creatures in the hand, presses only once the
// finger slides (marking walls along the way) or lifts (a click), so a second finger can
// still turn it into the dig square before a wall under it is marked (which would make the
// square unmark instead), or into dropping before it clicks anything. A drop hold keeps its
// spot, cursor included, until the finger slides three times as far: a held finger rolls
// when another one taps.
static bool holdDeferred = false, holdDrop = false;
static GameTurn holdPinTurn = 0;
static bool digContext(float x, float y) {
    auto* player = get_my_player();
    if (!player || !local_state.camera.ready || get_local_view_type() != PVT_DungeonTop || a_menu_window_is_active()) return false;
    const auto* ustate = get_local_user_state_prediction();
    if (user_state_invalid(ustate) || ustate->work_state != PSt_CtrlDungeon || ustate->primary_cursor_state != CSt_PickAxe
     || ustate->thing_under_hand != 0 || local_state.local_thing_under_hand != 0) return false;
    return inEngineWindow(gamePosition(x,y));
}
static bool packetActionFree() { return get_local_packet()->action == PckA_None; }
static void startDigDrag(float x, float y) {
    auto* player = get_my_player();
    const auto* ustate = get_local_user_state_prediction();
    digDrag = DigDrag::Arming; digStart = digLatest = {x,y}; digReleased = false; digTurn = get_gameturn();
    if (restoreMode < 0 && ustate->roomspace_highlight_mode != drag_placement_mode) restoreMode = ustate->roomspace_highlight_mode;
    restoreTurn = 0;
    if (ustate->roomspace_highlight_mode != drag_placement_mode && packetActionFree())
        set_players_packet_action(player, PckA_SetRoomspaceHighlight, drag_placement_mode, 1, 0, 0);
}
static void stepDigDrag() {
    auto* player = get_my_player();
    if (!player) return;
    const auto* ustate = get_local_user_state_prediction();
    GameTurn turn = get_gameturn();
    if (digDrag == DigDrag::Arming) {
        bool ready = ustate->roomspace_highlight_mode == drag_placement_mode;
        if (!ready && turn > digTurn + 2 && turn <= digTurn + 12 && packetActionFree())
            set_players_packet_action(player, PckA_SetRoomspaceHighlight, drag_placement_mode, 1, 0, 0);
        if (ready || turn > digTurn + 12) {
            position(digStart.x, digStart.y);
            button(MActn_LBUTTONDOWN);
            digDrag = DigDrag::Pressed; digTurn = turn;
            digReleaseTurn = turn + 1;
        }
    } else if (digDrag == DigDrag::Pressed) {
        // Hold the press on the start slab for a turn so the box starts there.
        if (turn > digTurn) position(digLatest.x, digLatest.y);
        if (digReleased && turn >= digReleaseTurn) {
            button(MActn_LBUTTONUP);
            digDrag = DigDrag::None; restoreTurn = turn + 3; restoreTries = 0;
        }
    } else if (restoreMode >= 0 && restoreTurn && turn >= restoreTurn) {
        if (ustate->roomspace_highlight_mode == restoreMode || restoreTries >= 4) { restoreMode = -1; restoreTurn = 0; }
        else if (packetActionFree()) {
            set_players_packet_action(player, PckA_SetRoomspaceHighlight, restoreMode, 1, 0, 0);
            restoreTries++; restoreTurn = turn + 3;
        }
    }
}

// Building a room takes KeeperFX's two one-click keys. A double tap fills the dug-out space
// under the finger (Shift-click, Best Room Space). A second finger while one is held turns
// the hold into the fixed-size square (Ctrl, Square Room Space): dragging the second finger
// up grows it, down shrinks it, and lifting the held finger builds it; a hold alone still
// builds its one tile on release. The configured key is held until the engine is in that
// key's mode, the click lasts a game turn, and the key lets go a few turns later (at once,
// the drag mode's build-on-release would follow the click). As in dig mode a lone tap
// waits for the double-tap window; bridges, which those keys paint instead, keep plain
// taps and holds. Digging has the same square (Ctrl in KeeperFX's dig highlight): a
// second finger during a hold on a wall shows it, and lifting the held finger marks every
// wall in it (or unmarks them, when the wall under the finger is already marked).
enum class RoomClick { None, Arming, Pressed, Releasing };
static RoomClick roomClick = RoomClick::None;
static long roomKey = Gkey_BestRoomSpace;
static bool roomDig = false; // the square or click is the dig highlight, not a room
static SDL_FPoint roomAt{}, holdAt{};
static GameTurn roomTurn = 0, squareLeftUpTurn = 0;
// The square follows the held finger; the size finger's height sets its width, which is
// kept for the next square. The hold's pressed button stays down until the engine is in
// box mode, where a release builds nothing (the drag mode would build the hold's tile).
static bool squareActive = false, sizeFingerDown = false, squareLeftDown = false;
static SDL_FingerID squareFinger = 0, sizeFinger = 0, lastFingerDown = 0;
static float sizeAnchorY = 0;
static int squareSize = DEFAULT_USER_ROOMSPACE_WIDTH;
static std::vector<SDL_FingerID> spentFingers; // still down after their square ended
static bool roomContext(float x, float y) {
    auto* player = get_my_player();
    if (!player || !local_state.camera.ready || get_local_view_type() != PVT_DungeonTop || a_menu_window_is_active()) return false;
    const auto* ustate = get_local_user_state_prediction();
    if (user_state_invalid(ustate) || ustate->work_state != PSt_BuildRoom
     || room_role_matches(ustate->chosen_room_kind, RoRoF_PassLava|RoRoF_PassWater|RoRoF_PassAbyss)) return false;
    return inEngineWindow(gamePosition(x,y));
}
static void holdRoomKey(long key, bool down) {
    unsigned code = settings.kbkeys[key].code;
    if (code != KC_UNASSIGNED && code < KC_LIST_END) lbKeyOn[code] = down;
}
static const UserState* roomState() {
    auto* player = get_my_player();
    const auto* ustate = player ? get_local_user_state_prediction() : nullptr;
    return !ustate || user_state_invalid(ustate) || ustate->work_state != (roomDig ? PSt_CtrlDungeon : PSt_BuildRoom) ? nullptr : ustate;
}
static bool roomKeyReady(const UserState* ustate) {
    if (roomKey != Gkey_SquareRoomSpace) return ustate->roomspace_mode == roomspace_detection_mode;
    if (roomDig) return ustate->roomspace_highlight_mode == roomspace_detection_mode && ustate->roomspace_width == squareSize;
    return ustate->roomspace_mode == box_placement_mode && ustate->roomspace_width == squareSize;
}
static void logRoom(const char* stage) {
    const auto* ustate = roomState();
    if (!ustate || !KfxAndroidInputDiagnostics()) return;
    const auto* dungeon = get_players_dungeon(get_my_player());
    LbSyncLog("AndroidRoom %s dig=%d turn=%lu mode=%d highlight=%d width=%d size=%d kind=%d slabs=%ld tasks=%d\n", stage, static_cast<int>(roomDig),
              static_cast<unsigned long>(get_gameturn()), ustate->roomspace_mode, ustate->roomspace_highlight_mode, ustate->roomspace_width,
              squareSize, ustate->chosen_room_kind, get_room_slabs_count(my_player_number, ustate->chosen_room_kind),
              dungeon_invalid(dungeon) ? -1 : dungeon->task_count);
}
static void releaseSquareHold(const UserState* ustate) {
    if (!squareLeftDown || (ustate && ustate->roomspace_mode != box_placement_mode && get_gameturn() <= roomTurn + 12)) return;
    button(MActn_LBUTTONUP); squareLeftDown = false; squareLeftUpTurn = get_gameturn();
}
static void stopRoomClick() {
    if (roomClick == RoomClick::Pressed) button(MActn_LBUTTONUP);
    if (roomClick != RoomClick::None) holdRoomKey(roomKey, false);
    roomClick = RoomClick::None;
}
static void startRoomClick(long key, bool dig, float x, float y) {
    stopRoomClick();
    roomClick = RoomClick::Arming; roomKey = key; roomDig = dig; roomAt = {x,y}; roomTurn = get_gameturn();
    holdRoomKey(key, true);
    logRoom(key == Gkey_SquareRoomSpace ? "square-build" : "fill");
}
static void stepRoomClick() {
    if (roomClick == RoomClick::None) return;
    const auto* ustate = roomState();
    if (!ustate) { releaseSquareHold(nullptr); stopRoomClick(); return; } // out of building, a release builds nothing
    GameTurn turn = get_gameturn();
    position(roomAt.x, roomAt.y);
    releaseSquareHold(ustate);
    if (roomClick == RoomClick::Arming) {
        // Without the mode in a few turns (a rebound key) the tap still builds as a tap does.
        if (!squareLeftDown && turn > squareLeftUpTurn && (roomKeyReady(ustate) || turn > roomTurn + 12)) {
            logRoom("press");
            button(MActn_LBUTTONDOWN); roomClick = RoomClick::Pressed; roomTurn = turn;
        }
    } else if (roomClick == RoomClick::Pressed) {
        if (turn > roomTurn) { button(MActn_LBUTTONUP); roomClick = RoomClick::Releasing; roomTurn = turn; }
    } else if (!squareLeftDown && turn > roomTurn + 2 && turn > squareLeftUpTurn + 2) {
        holdRoomKey(roomKey, false); roomClick = RoomClick::None;
        logRoom("done");
    }
}
// The held finger leaves the gesture recogniser: it and the size finger are routed here.
// The size finger's first touch starts the square; a later tap of it cancels the square.
constexpr uint64_t squareTapMs = 450; // the recogniser's hold time
static int sizeTouches = 0;
static uint64_t sizeDownAt = 0;
static SDL_FPoint sizeStart{};
static bool sizeMoved = false;
static void sizeFingerDownAt(SDL_FingerID id, float x, float y) {
    sizeFinger = id; sizeFingerDown = true; sizeAnchorY = y;
    sizeTouches++; sizeDownAt = SDL_GetTicks(); sizeStart = {x,y}; sizeMoved = false;
}
static void startSquare(SDL_FingerID id, float x, float y, bool dig) {
    stopRoomClick();
    roomDig = dig;
    // A room hold has pressed the button; a dig hold has not pressed yet.
    squareActive = true; squareFinger = lastFingerDown; squareLeftDown = !dig; leftHeld = false; holdDeferred = false;
    sizeTouches = 0; sizeFingerDownAt(id, x, y);
    roomAt = holdAt; roomTurn = get_gameturn();
    holdRoomKey(Gkey_SquareRoomSpace, true);
    gestures.cancel(); gestures.take(); // drops the recogniser's held-button release
    logRoom("square");
}
// Cancelled: the held finger builds nothing when it lifts, and the key lets go only after
// the hold's button is up in box mode (as after a build).
static void cancelSquare() {
    squareActive = false;
    spentFingers.push_back(squareFinger);
    roomClick = RoomClick::Releasing; roomKey = Gkey_SquareRoomSpace; roomTurn = get_gameturn();
    logRoom("cancel");
}
static void endSquare(bool build) {
    if (!squareActive) return;
    squareActive = false;
    if (sizeFingerDown) spentFingers.push_back(sizeFinger);
    if (build) { startRoomClick(Gkey_SquareRoomSpace, roomDig, roomAt.x, roomAt.y); return; }
    releaseSquareHold(nullptr);
    holdRoomKey(Gkey_SquareRoomSpace, false);
}
static void squareFingerEvent(const SDL_Event* e, float x, float y) {
    SDL_FingerID id = e->tfinger.fingerID;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (sizeFingerDown) spentFingers.push_back(id);
        else sizeFingerDownAt(id, x, y);
        return;
    }
    if (id == squareFinger) {
        roomAt = {x,y};
        if (e->type == SDL_EVENT_FINGER_UP) endSquare(true);
    } else if (sizeFingerDown && id == sizeFinger) {
        if (std::hypot(x-sizeStart.x, y-sizeStart.y) > gestures.slop()) sizeMoved = true;
        if (e->type == SDL_EVENT_FINGER_UP && sizeTouches > 1 && !sizeMoved && SDL_GetTicks()-sizeDownAt < squareTapMs) {
            sizeFingerDown = false;
            cancelSquare();
            return;
        }
        // One size step per 6% of the screen height; at a limit the step starts again from here.
        int w=0,h=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
        float step = std::max(24.0f, h*.06f);
        int steps = static_cast<int>((sizeAnchorY-y)/step);
        if (steps != 0) {
            int size = std::clamp(squareSize+steps, MIN_USER_ROOMSPACE_WIDTH, MAX_USER_ROOMSPACE_WIDTH);
            sizeAnchorY = size == squareSize+steps ? sizeAnchorY-steps*step : y;
            squareSize = size;
            logRoom("size");
        }
        if (e->type == SDL_EVENT_FINGER_UP) sizeFingerDown = false;
    }
}
static void stepSquare() {
    if (!squareActive) return;
    const auto* ustate = roomState();
    if (!ustate || a_menu_window_is_active()) { endSquare(false); return; }
    local_state.roomspace_size = squareSize; // what the engine sends while the key is held
    position(roomAt.x, roomAt.y);
    releaseSquareHold(ustate);
}

// Dropping: the held finger (creatures in the hand) leaves the gesture recogniser and marks
// where they go, the spot it was held at; each tap of a second finger drops one there,
// holding it keeps dropping.
static bool dropMode = false, dropFingerDown = false, dropFingerHeld = false;
static SDL_FingerID dropHoldFinger = 0, dropFinger = 0;
static uint64_t dropFingerAt = 0;
static void dropFingerDownAt(SDL_FingerID id) {
    dropFinger = id; dropFingerDown = true; dropFingerHeld = false; dropFingerAt = SDL_GetTicks();
}
static void startDropMode(SDL_FingerID id) {
    dropMode = true; dropHoldFinger = lastFingerDown; holdDeferred = false; leftHeld = false;
    gestures.cancel(); gestures.take(); // the hold has pressed nothing yet
    dropFingerDownAt(id);
}
static void endDropMode(bool holdFingerUp) {
    if (!dropMode) return;
    dropMode = dropHeld = false;
    if (!holdFingerUp) spentFingers.push_back(dropHoldFinger);
    if (dropFingerDown) spentFingers.push_back(dropFinger);
    dropFingerDown = false;
}
static void dropFingerEvent(const SDL_Event* e) {
    SDL_FingerID id = e->tfinger.fingerID;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (dropFingerDown) spentFingers.push_back(id);
        else dropFingerDownAt(id);
        return;
    }
    if (id == dropHoldFinger) {
        if (e->type == SDL_EVENT_FINGER_UP) endDropMode(true);
    } else if (dropFingerDown && id == dropFinger && e->type == SDL_EVENT_FINGER_UP) {
        if (dropFingerHeld) dropHeld = false;
        else { position(holdAt.x, holdAt.y); dropOne(); }
        dropFingerDown = dropFingerHeld = false;
    }
}
static void stepDropMode(uint64_t now) {
    if (!dropMode) return;
    if (!handHoldsThings()) { endDropMode(false); return; } // the hand is empty (or the view changed)
    position(holdAt.x, holdAt.y);
    if (dropFingerDown && !dropFingerHeld && now-dropFingerAt >= squareTapMs) { dropFingerHeld = dropHeld = true; nextDropAt = 0; }
}

// The LAN lobby keeps text input on for its chat line, which on a phone kept the keyboard
// over the lobby (and its Start Game button). The lobby starts text input without the
// keyboard; a tap on the chat line or on an edit box shows it (the engine only asks SDL
// when text input is off, so an edit box tapped in the lobby got no keyboard), and a tap
// elsewhere in the lobby hides it.
extern "C" void KfxAndroidStartQuietTextInput(void) {
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    LbStartTextInput();
    SDL_ResetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD);
}
static bool textFieldAt(int gx, int gy) {
    // Every button under the finger: the lobby's Lobby Name box lies on top of a settings
    // row and the panel behind it.
    for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++) {
        auto* gbtn = &active_buttons[i];
        if ((gbtn->flags & LbBtnF_Active) == 0 || (gbtn->flags & LbBtnF_Enabled) == 0
         || !active_menus[static_cast<unsigned>(gbtn->gmenu_idx)].is_turned_on || !check_if_pos_is_over_button(gbtn, gx, gy)) continue;
        if (gbtn->gbtype == LbBtnT_EditBox || gbtn->draw_call == frontnet_draw_current_message) return true;
    }
    return false;
}
static void textKeyboard(float x, float y) {
    SDL_Window* window = GetSDLWindowSystem()->GetSDLWindow();
    auto p = gamePosition(x,y);
    if (textFieldAt(static_cast<int>(p.x), static_cast<int>(p.y))) SDL_StartTextInput(window);
    else if (frontend_menu_state == FeSt_NET_START && SDL_ScreenKeyboardShown(window))
        SDL_StopTextInput(window); // the lobby restarts it quietly
}

static void drain() {
    for (const auto& a : gestures.take()) {
        if (digDrag != DigDrag::None && (a.kind == Kind::Hover || a.kind == Kind::DragUp)) {
            digLatest = {a.x,a.y};
            if (a.kind == Kind::DragUp) digReleased = true;
            continue;
        }
        if (roomClick != RoomClick::None && (a.kind == Kind::Hover || a.kind == Kind::DragUp)) continue;
        // A second touch after a creature tap that is no tap (a drag, a hold, two fingers):
        // the first tap stays a pick-up.
        if (creatureTap.second && a.kind != Kind::Hover && a.kind != Kind::LeftClick) queueThingAct(ThingAct::Pick);
        position(a.x,a.y);
        if (a.kind == Kind::LeftDown) holdAt = {a.x,a.y};
        else if (leftHeld && a.kind == Kind::Hover) {
            if (get_gameturn() <= holdPinTurn) position(holdAt.x, holdAt.y); // a slid dig hold starts on its wall
            else holdAt = {a.x,a.y};
        } else if (holdDeferred && holdDrop) position(holdAt.x, holdAt.y);
        if (a.kind!=Kind::Hover && KfxAndroidInputDiagnostics()) {
            LbSyncLog("AndroidInput action=%d x=%.1f y=%.1f dx=%.4f dy=%.4f cursor=%ld,%ld target=%d\n",static_cast<int>(a.kind),a.x,a.y,a.dx,a.dy,static_cast<long>(lbDisplay.MMouseX),static_cast<long>(lbDisplay.MMouseY),local_state.local_thing_under_hand);
            inputSnapshotAt=SDL_GetTicks()+350;
            inputThing=local_state.local_thing_under_hand;
        }
        switch (a.kind) {
        case Kind::LeftClick:
            if (creatureTap.second) {
                // The same creature (or a near miss on bare floor) slaps; another one means the
                // first tap was a pick-up and this tap goes on below.
                int under = local_state.local_thing_under_hand;
                if (under <= 0 || under == creatureTap.thing) { queueThingAct(ThingAct::Slap); break; }
                queueThingAct(ThingAct::Pick);
            }
            if (handOverThing() && inEngineWindow(gamePosition(a.x,a.y))) {
                if (creatureTap.active) queueThingAct(ThingAct::Pick); // an earlier tap still waiting
                creatureTap = {true, false, static_cast<ThingIndex>(local_state.local_thing_under_hand), a.x, a.y, SDL_GetTicks()};
                break;
            }
            if (digContext(a.x,a.y)) { pendingTap = {true, a.x, a.y, SDL_GetTicks()}; break; }
            if (roomContext(a.x,a.y)) { pendingTap = {true, a.x, a.y, SDL_GetTicks(), true}; break; }
            textKeyboard(a.x,a.y);
            tap(); break;
        case Kind::RightClick:
            if (handHoldsThings() || handOverThing()) break; // no drop, no slap
            releaseClick(); button(MActn_RBUTTONDOWN); button(MActn_RBUTTONUP); break;
        case Kind::LeftDown:
            if (digContext(a.x,a.y)) { holdDeferred = true; holdDrop = false; break; }
            if (dropContext(a.x,a.y)) { holdDeferred = holdDrop = true; break; }
            releaseClick(); button(MActn_LBUTTONDOWN); leftHeld = true; nextPickupAt = 0; break;
        case Kind::LeftUp:
            if (holdDeferred) { holdDeferred = false; tap(); break; } // lifted in place: a click
            button(MActn_LBUTTONUP); leftHeld = false; break;
        case Kind::Hover:
            if (holdDeferred && std::hypot(a.x-holdAt.x, a.y-holdAt.y) > gestures.slop()*(holdDrop ? 3 : 1)) {
                holdDeferred = false;
                position(holdAt.x, holdAt.y);
                releaseClick(); button(MActn_LBUTTONDOWN); leftHeld = true; nextPickupAt = 0;
                holdPinTurn = get_gameturn();
            }
            break;
        case Kind::RightDown:
            if (handHoldsThings() || handOverThing()) { rightIgnored = true; break; }
            button(MActn_RBUTTONDOWN); break;
        case Kind::RightUp:
            if (rightIgnored) { rightIgnored = false; break; }
            button(MActn_RBUTTONUP); break;
        case Kind::Pan: panX -= a.dx; panY -= a.dy; break;
        case Kind::Zoom:
            if (endlessMap()) {
                // The map's zoom levels are a fifth to a third apart: one level for each such pinch, round
                // the point between the fingers (that point stays under them).
                constexpr float step = .22f;
                mapZoomAcc += a.dx;
                auto at = gamePosition(a.x, a.y);
                while (std::fabs(mapZoomAcc) >= step) {
                    int dir = mapZoomAcc > 0 ? 1 : -1;
                    parchment_zoom(dir, static_cast<long>(at.x) / pixel_size, static_cast<long>(at.y) / pixel_size);
                    mapZoomAcc -= dir*step;
                }
                break;
            }
            zoomDelta += a.dx; break;
        case Kind::Rotate: if (endlessMap()) break; rotationDelta += a.dx; break;
        default: break;
        }
    }
}
// Possess Creature on touch: each finger takes one role when it lands. Left of the
// screen is a floating analog stick, the right side drags the view, and on-screen
// buttons attack and leave. The side panel stays tappable (spells, next page).
enum class Role { None, Stick, Look, Attack, Exit, Panel };
struct PossessFinger { Role role; SDL_FPoint origin, last; };
static std::map<SDL_FingerID, PossessFinger> possessFingers;
static float stickX = 0, stickY = 0, lookX = 0, lookY = 0;
static bool attackDown = false, possessing = false;
struct PossessLayout { float w, h, panel; SDL_FPoint stick, attack, exit; float stickR, attackR, exitR; };
static bool possessionActive() {
    auto* player = get_my_player();
    if (!player || !local_state.camera.ready || get_local_view_type() != PVT_CreatureContrl || a_menu_window_is_active()) return false;
    const auto* ustate = get_local_user_state();
    if ((ustate->init_flags & UsrIF_CreaturePassengerMode) != 0) return false;
    auto* thing = thing_get(ustate->controlled_thing_idx);
    if (!thing_is_creature(thing)) return false;
    // The level-start heart zoom (and a defeated keeper) fly an invisible spectator creature.
    return (creature_control_get_from_thing(thing)->creature_state_flags & TF2_Spectator) == 0;
}
static PossessLayout possessLayout() {
    int w=0,h=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
    float u = static_cast<float>(std::max(1,std::min(w,h)));
    float panel = local_state.engine_window_x > 0 ? windowPosition(local_state.engine_window_x, 0).x : 0;
    PossessLayout l{static_cast<float>(w), static_cast<float>(h), panel};
    l.stickR = .15f*u; l.attackR = .12f*u; l.exitR = .065f*u;
    l.stick = {panel + .25f*u, h - .27f*u};
    l.attack = {w - .2f*u, h - .25f*u};
    l.exit = {w - .1f*u, .26f*u};
    return l;
}
static float distanceTo(SDL_FPoint a, float x, float y) { return std::hypot(a.x-x, a.y-y); }
static void centreCursor() {
    setCursor(local_state.engine_window_x + local_state.engine_window_width/2, local_state.engine_window_y + local_state.engine_window_height/2);
}
static void setAttack(bool down) {
    if (attackDown == down) return;
    attackDown = down;
    if (down) centreCursor();
    button(down ? MActn_LBUTTONDOWN : MActn_LBUTTONUP);
}
static void possessionPanelTap(float x, float y) {
    auto p = gamePosition(x,y);
    auto* gbtn = buttonAt(static_cast<int>(p.x), static_cast<int>(p.y));
    if (!gbtn || (gbtn->flags & LbBtnF_Enabled) == 0) return;
    if (gbtn->draw_call == gui_area_instance_button) {
        // Spells are chosen with the number keys; the key path also turns to the right page.
        long slot = gbtn->content.lval;
        if (slot < 0 || slot > 9) return;
        tapKey(slot == 9 ? SDL_SCANCODE_0 : static_cast<SDL_Scancode>(SDL_SCANCODE_1 + slot), slot == 9 ? SDLK_0 : static_cast<SDL_Keycode>(SDLK_1 + slot));
        return;
    }
    if (gbtn->click_event == nullptr && (gbtn->flags & LbBtnF_Clickable) == 0 && gbtn->parent_menu == nullptr) return;
    // Click the button directly: the mouse cursor never leaves the view, so it neither attacks nor turns.
    do_button_click_actions(gbtn, &gbtn->button_state_left_pressed, gbtn->click_event);
    do_button_release_actions(gbtn, &gbtn->button_state_left_pressed, gbtn->click_event);
}
static void possessionDown(SDL_FingerID id, float x, float y) {
    auto l = possessLayout();
    Role role = Role::Look;
    bool stickTaken = std::any_of(possessFingers.begin(), possessFingers.end(), [](const auto& f) { return f.second.role == Role::Stick; });
    if (x < l.panel) role = Role::Panel;
    else if (distanceTo(l.attack,x,y) <= l.attackR*1.15f) role = Role::Attack;
    else if (distanceTo(l.exit,x,y) <= l.exitR*1.4f) role = Role::Exit;
    else if (x < l.w*.5f) role = stickTaken ? Role::None : Role::Stick;
    possessFingers[id] = {role, {x,y}, {x,y}};
    if (role == Role::Attack) setAttack(true);
}
static void possessionMove(PossessFinger& f, float x, float y) {
    if (f.role == Role::Stick) {
        float r = possessLayout().stickR, dx = x-f.origin.x, dy = y-f.origin.y, length = std::hypot(dx,dy);
        float amount = std::clamp((length/r - .12f)/.88f, 0.0f, 1.0f);
        stickX = length > 0 ? dx/length*amount : 0;
        stickY = length > 0 ? dy/length*amount : 0;
    } else if (f.role == Role::Look) {
        lookX += x-f.last.x; lookY += y-f.last.y;
    }
    f.last = {x,y};
}
static void possessionUp(SDL_FingerID id, float x, float y) {
    auto it = possessFingers.find(id);
    if (it == possessFingers.end()) return;
    auto& f = it->second;
    possessionMove(f, x, y);
    auto l = possessLayout();
    if (f.role == Role::Stick) stickX = stickY = 0;
    else if (f.role == Role::Attack) setAttack(false);
    else if (f.role == Role::Exit && distanceTo(l.exit,x,y) <= l.exitR*1.6f) { button(MActn_RBUTTONDOWN); button(MActn_RBUTTONUP); }
    else if (f.role == Role::Panel && distanceTo(f.origin,x,y) <= gestures.slop()) possessionPanelTap(x,y);
    possessFingers.erase(it);
}
static void endPossessionInput() {
    setAttack(false);
    possessFingers.clear();
    stickX = stickY = lookX = lookY = 0;
}
// Possession (without an open menu window) switches every new finger to the controls.
static void updatePossession() {
    bool active = possessionActive();
    if (active == possessing) return;
    possessing = active;
    if (active) {
        gestures.cancel(); drain(); panX=panY=zoomDelta=rotationDelta=0;
        pendingTap.active = false; creatureTap = {}; holdDeferred = dropHeld = false;
        centreCursor();
    } else endPossessionInput();
}
// A possessed creature takes digital moves: a game turn sends a move only once an axis has
// added up to 1 (get_creature_control_nonaction_inputs), so values below 1 pulse the moves
// and the creature walks. The stick snaps to the keyboard's eight directions (a diagonal
// was ~0.7 per axis, always a walk) and runs at full strength near its rim; inside, the
// creature walks, faster the further it is pushed.
constexpr float stickRun = .8f;
extern "C" void KfxAndroidStickAxes(float* x, float* y) {
    *x = *y = 0;
    float amount = std::hypot(stickX, stickY);
    if (!possessing || amount < .15f) return;
    float strength = amount >= stickRun ? 1.f : .45f + .35f*(amount-.15f)/(stickRun-.15f);
    constexpr float sin22 = .3827f; // sectors of 45 degrees around each direction
    if (std::fabs(stickX) > amount*sin22) *x = stickX > 0 ? strength : -strength;
    if (std::fabs(stickY) > amount*sin22) *y = stickY > 0 ? strength : -strength;
}
extern "C" void KfxAndroidPossessionLook(float* x, float* y) {
    // A drag across half the window is one full-strength mouse sweep.
    int w=0,h=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
    *x = possessing && w > 0 ? lookX/(w*.5f)*2.4f : 0;
    *y = possessing && h > 0 ? lookY/(h*.5f)*2.4f : 0;
    lookX = lookY = 0;
}
static void disc(SDL_Renderer* r, SDL_FPoint c, float inner, float outer, SDL_FColor colour) {
    constexpr int segments = 48;
    std::vector<SDL_Vertex> v; std::vector<int> index;
    if (inner <= 0) v.push_back({c, colour, {0,0}});
    for (int i = 0; i <= segments; i++) {
        float a = i*6.2831853f/segments, cs = std::cos(a), sn = std::sin(a);
        v.push_back({{c.x+cs*outer, c.y+sn*outer}, colour, {0,0}});
        if (inner > 0) v.push_back({{c.x+cs*inner, c.y+sn*inner}, colour, {0,0}});
    }
    for (int i = 0; i < segments; i++) {
        if (inner <= 0) { index.insert(index.end(), {0, i+1, i+2}); continue; }
        int o = i*2; index.insert(index.end(), {o, o+1, o+2, o+1, o+3, o+2});
    }
    SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), index.data(), static_cast<int>(index.size()));
}
static void bar(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, float width, SDL_FColor colour) {
    float dx = b.x-a.x, dy = b.y-a.y, length = std::max(.001f, std::hypot(dx,dy));
    float nx = -dy/length*width*.5f, ny = dx/length*width*.5f;
    SDL_Vertex v[4] = {{{a.x+nx,a.y+ny},colour,{0,0}}, {{a.x-nx,a.y-ny},colour,{0,0}}, {{b.x+nx,b.y+ny},colour,{0,0}}, {{b.x-nx,b.y-ny},colour,{0,0}}};
    int index[6] = {0,1,2,1,3,2};
    SDL_RenderGeometry(r, nullptr, v, 4, index, 6);
}
extern "C" void KfxAndroidDrawTouchOverlay(SDL_Renderer* r) {
    if (!possessing) return;
    int w=0,h=0,ow=0,oh=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
    if (w <= 0 || !SDL_GetCurrentRenderOutputSize(r,&ow,&oh)) return;
    float k = static_cast<float>(ow)/w;
    auto at = [k](SDL_FPoint p) { return SDL_FPoint{p.x*k, p.y*k}; };
    float sx=1,sy=1; SDL_BlendMode blend;
    SDL_GetRenderScale(r,&sx,&sy); SDL_GetRenderDrawBlendMode(r,&blend);
    SDL_SetRenderScale(r,1,1); SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    auto l = possessLayout();
    SDL_FPoint base = l.stick;
    for (const auto& f : possessFingers) if (f.second.role == Role::Stick) base = f.second.origin;
    const SDL_FColor light{1,.93f,.8f,.45f}, shade{0,0,0,.28f};
    disc(r, at(base), 0, l.stickR*k, shade);
    disc(r, at(base), l.stickR*k*.94f, l.stickR*k, light);
    // Faint ring where walking turns into running (stickRun of the knob's travel).
    float runAt = .12f + .88f*stickRun;
    disc(r, at(base), l.stickR*k*(runAt-.02f), l.stickR*k*(runAt+.01f), SDL_FColor{1,.93f,.8f,.18f});
    disc(r, at({base.x+stickX*l.stickR, base.y+stickY*l.stickR}), 0, l.stickR*.42f*k, SDL_FColor{1,.93f,.8f,.5f});
    disc(r, at(l.attack), 0, l.attackR*k, attackDown ? SDL_FColor{.85f,.12f,.05f,.75f} : SDL_FColor{.6f,.08f,.03f,.5f});
    disc(r, at(l.attack), l.attackR*k*.93f, l.attackR*k, light);
    // Three claw marks for attack, a cross for leaving the creature.
    SDL_FPoint c = at(l.attack); float a = l.attackR*k*.45f;
    for (int i = -1; i <= 1; i++) bar(r, {c.x-a*.55f+i*a*.5f, c.y+a}, {c.x+a*.55f+i*a*.5f, c.y-a}, a*.2f, light);
    disc(r, at(l.exit), 0, l.exitR*k, shade);
    disc(r, at(l.exit), l.exitR*k*.88f, l.exitR*k, light);
    c = at(l.exit); a = l.exitR*k*.42f;
    bar(r, {c.x-a,c.y-a}, {c.x+a,c.y+a}, a*.3f, light); bar(r, {c.x-a,c.y+a}, {c.x+a,c.y-a}, a*.3f, light);
    SDL_SetRenderScale(r,sx,sy); SDL_SetRenderDrawBlendMode(r,blend);
}

extern "C" int KfxAndroidTouchEvent(const SDL_Event* e) {
    if (e->type==SDL_EVENT_RENDER_DEVICE_RESET || e->type==SDL_EVENT_RENDER_TARGETS_RESET || e->type==SDL_EVENT_RENDER_DEVICE_LOST) {
        presentationGeneration++;
        return 0;
    }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_FINGER_CANCELED) {
        gestures.cancel(); drain(); panX=panY=zoomDelta=rotationDelta=0;
        pendingTap.active = false; releaseClick(); endSquare(false); releaseSquareHold(nullptr); stopRoomClick(); endDropMode(true);
        spentFingers.clear(); endPossessionInput(); // cancelled fingers send no lift; their ids come back
        creatureTap = {}; thingActs.clear(); holdDeferred = dropHeld = rightIgnored = false;
        return 0;
    }
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) && KfxAndroidInputDiagnostics())
        LbSyncLog("AndroidKey %s scancode=%d key=0x%x menu_active=%d show_gui=%d\n", e->type == SDL_EVENT_KEY_DOWN ? "down" : "up",
                  static_cast<int>(e->key.scancode), static_cast<unsigned>(e->key.key), static_cast<int>(a_menu_window_is_active()), static_cast<int>(local_state.show_gui));
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_MOTION) return 0;
    int w=0,h=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
    float x=e->tfinger.x*w, y=e->tfinger.y*h;
    auto possessed = possessFingers.find(e->tfinger.fingerID);
    if (possessed != possessFingers.end()) {
        if (e->type == SDL_EVENT_FINGER_UP) possessionUp(e->tfinger.fingerID,x,y);
        else possessionMove(possessed->second,x,y);
        return 1;
    }
    auto spent = std::find(spentFingers.begin(), spentFingers.end(), e->tfinger.fingerID);
    if (spent != spentFingers.end()) {
        if (e->type == SDL_EVENT_FINGER_UP) spentFingers.erase(spent);
        return 1;
    }
    if (squareActive) { squareFingerEvent(e,x,y); return 1; }
    if (dropMode) { dropFingerEvent(e); return 1; }
    if (e->type == SDL_EVENT_FINGER_DOWN && leftHeld && roomContext(holdAt.x,holdAt.y)) { startSquare(e->tfinger.fingerID,x,y,false); return 1; }
    if (e->type == SDL_EVENT_FINGER_DOWN && holdDeferred && digContext(holdAt.x,holdAt.y)) { startSquare(e->tfinger.fingerID,x,y,true); return 1; }
    if (e->type == SDL_EVENT_FINGER_DOWN && holdDeferred && dropContext(holdAt.x,holdAt.y)) { startDropMode(e->tfinger.fingerID); return 1; }
    if (e->type == SDL_EVENT_FINGER_DOWN) lastFingerDown = e->tfinger.fingerID;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        releaseClick(); // a tap still held must not turn into a drag under the next finger
        updatePossession();
        if (possessing) { possessionDown(e->tfinger.fingerID,x,y); return 1; }
        // Menus and the sidebar retain normal mouse dragging; world drags pan. The endless world's map is
        // dragged like a mouse drags it (it follows the finger), so it gets the left button instead.
        const auto* player = get_my_player();
        mapZoomAcc = 0;
        bool world = player && !endlessMap() && (get_local_view_type() == PVT_DungeonTop || get_local_view_type() == PVT_MapScreen);
        gestures.setPanEnabled(world && inEngineWindow(gamePosition(x,y)));
        gestures.down(static_cast<int64_t>(e->tfinger.fingerID),x,y,SDL_GetTicks());
        if (pendingTap.active && SDL_GetTicks()-pendingTap.at < doubleTapMs
         && std::hypot(x-pendingTap.x, y-pendingTap.y) <= std::max(48.0f, gestures.slop()*3) && gestures.captureAsDrag()) {
            pendingTap.active = false;
            if (pendingTap.room) startRoomClick(Gkey_BestRoomSpace,false,x,y); else startDigDrag(x,y);
        }
        if (creatureTap.active && !creatureTap.second && SDL_GetTicks()-creatureTap.at < doubleTapMs
         && std::hypot(x-creatureTap.x, y-creatureTap.y) <= std::max(48.0f, gestures.slop()*3)) creatureTap.second = true;
    } else if (e->type == SDL_EVENT_FINGER_UP) gestures.up(static_cast<int64_t>(e->tfinger.fingerID),x,y,SDL_GetTicks());
    else gestures.move(static_cast<int64_t>(e->tfinger.fingerID),x,y,SDL_GetTicks());
    drain(); return 1;
}
extern "C" void KfxAndroidTouchTick(void) {
    gestures.tick(SDL_GetTicks());drain();
    uint64_t now = SDL_GetTicks();
    updatePossession();
    if (pendingTap.active && now-pendingTap.at >= doubleTapMs) {
        pendingTap.active = false;
        position(pendingTap.x, pendingTap.y);
        tap();
    }
    if (creatureTap.active && now-creatureTap.at >= (creatureTap.second ? 2000 : doubleTapMs)) queueThingAct(ThingAct::Pick);
    stepThingAct();
    stepClick(now);
    stepDigDrag();
    stepSquare();
    stepRoomClick();
    repeatPickup(now);
    stepDropMode(now);
    repeatDrop(now);
    for (auto it = keyReleases.begin(); it != keyReleases.end();) {
        if (now < it->at) { ++it; continue; }
        pushKey(it->scancode, it->key, false);
        it = keyReleases.erase(it);
    }
    if (diagnosticSnapshot.exchange(false) && KfxAndroidInputDiagnostics()) { inputSnapshotAt=SDL_GetTicks();inputThing=local_state.local_thing_under_hand; }
    if (escapeReleaseTicks > 0 && --escapeReleaseTicks == 0) pushEscape(false);
    int request=menuRequest.exchange(0);
    if (request==1) {
        if (escapeReleaseTicks == 0) { pushEscape(true); escapeReleaseTicks = 3; }
    } else if (request && local_state.camera.ready && KfxAndroidCheatsEnabled()) {
        if(request==3) toggle_instance_cheat_menu(); else toggle_main_cheat_menu();
    }
    if (inputSnapshotAt && SDL_GetTicks()>=inputSnapshotAt && local_state.camera.ready) {
        inputSnapshotAt=0;
        auto* player=get_my_player();
        const auto* dungeon=get_players_dungeon(player);
        const auto* camera=get_local_active_camera();
        const auto* thing=thing_get(inputThing);
        LbSyncLog("AndroidInput snapshot hand=%d thing=%d health=%ld camera=%ld,%ld zoom=%d yaw=%d\n",dungeon_invalid(dungeon)?-1:dungeon->num_things_in_hand,inputThing,thing_is_invalid(thing)?-1L:static_cast<long>(thing->health),static_cast<long>(camera->mappos.x.val),static_cast<long>(camera->mappos.y.val),camera->zoom,camera->rotation_angle_x);
        const auto* controlled=thing_get(get_local_user_state()->controlled_thing_idx);
        if (thing_is_creature(controlled)) {
            const auto* cctrl=creature_control_get_from_thing(controlled);
            LbSyncLog("AndroidPossess thing=%d active_instance=%d instance=%d attack=%d stick=%.2f,%.2f\n",get_local_user_state()->controlled_thing_idx,
                      static_cast<int>(cctrl->active_instance_id),static_cast<int>(cctrl->instance_id),static_cast<int>(attackDown),stickX,stickY);
        }
        // map_to_screen projects through rotpers, which stays null until the first 3D frame.
        int id=rotpers ? get_thing_class_list_head(TCls_Creature) : 0;
        for(int count=0;id>0 && count<16;count++) {
            auto* creature=thing_get(id);if(thing_is_invalid(creature))break;
            auto pos=creature->mappos;pos.z.val+=creature->clipbox_size_z/2;
            int32_t sx=0,sy=0;
            if (map_to_screen(&pos,&sx,&sy)) LbSyncLog("AndroidCreature id=%d owner=%d model=%d screen=%d,%d norm=%.4f,%.4f health=%ld\n",id,creature->owner,creature->model,sx,sy,sx/static_cast<double>(RendererPhysicalWidth()),sy/static_cast<double>(RendererPhysicalHeight()),static_cast<long>(creature->health));
            id=creature->next_of_class;
        }
    }
}
extern "C" void KfxAndroidCameraInputs(float* x, float* y, float* z, float* r) {
    uint64_t now=SDL_GetTicks();
    float dt=lastDrain ? std::clamp((now-lastDrain)/1000.0f,.001f,.1f) : 1.f/60.f;
    lastDrain=now;
    // Convert drag displacement to a bounded camera velocity, independent of FPS. The
    // sensitivity scales both the velocity and its bound (1 = the keyboard's top speed).
    int w=0,h=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
    float unit=std::max(320,std::min(w,h)), s=panSensitivity;
    *x=std::clamp(s*panX/(unit*dt),-s,s); *y=std::clamp(s*panY/(unit*dt),-s,s);
    *z=zoomDelta; *r=rotationDelta;
    panX=panY=zoomDelta=rotationDelta=0;
}
extern "C" void KfxAndroidApplyCamera(void) {
    float x=0,y=0,z=0,r=0;
    KfxAndroidCameraInputs(&x,&y,&z,&r);
    if (!local_state.camera.ready) return;
    KfxAndroidSecondScreenLevelTick(); // only levels update cameras
    // A spectator's camera follows the watched player, which overwrote every touch pan and
    // zoom, until keys or the mouse detach it (get_observer_inputs); touch detaches it too.
    if ((x != 0 || y != 0 || z != 0 || r != 0) && observer_is_following_user()) detach_observer_camera();
    auto* player=get_my_player();
    int view=get_local_view_type();
    if (view != PVT_DungeonTop && view != PVT_MapScreen) return;
    if (x != 0 || y != 0) {
        local_state.camera_movement_x=x;
        local_state.camera_movement_y=y;
    }
    int index=static_cast<int>(get_local_active_camera()-local_state.camera.current);
    if (index<0 || index>=CamIV_EndList) return;
    auto* camera=&local_state.camera.destination[index];
    // A rising angle turns the map counterclockwise on screen, against a clockwise twist.
    if (rotateWithFingers) r = -r;
    if (r != 0) camera->rotation_angle_x=(camera->rotation_angle_x+static_cast<int>(std::lround(r*2048.0/360.0))) & 2047;
    if (z != 0) {
        bool front=camera->view_mode==PVM_FrontView;
        int lo=static_cast<int>(scale_camera_zoom_to_screen(front?FRONTVIEW_CAMERA_ZOOM_MIN:CAMERA_ZOOM_MIN));
        int hi=static_cast<int>(scale_camera_zoom_to_screen(front?FRONTVIEW_CAMERA_ZOOM_MAX:CAMERA_ZOOM_MAX));
        camera->zoom=std::clamp(static_cast<int>(std::lround(camera->zoom*std::exp(z*.9f))),lo,hi);
    }
}

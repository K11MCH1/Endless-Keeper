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
#include "power_hand.h"
#include "map_data.h"
#include "game_legacy.h"
#include "player_instances.h"
#include "gui_parchment.h"
#include "gui_boxmenu.h"
#include <cstring>
#include <jni.h>
// Endless Keeper (2026-11-09): the endless world's full-screen map scrolls with one finger and zooms with a pinch
extern "C" TbBool world_mode_active(void);
extern "C" void parchment_pinch(float log_scale, long screen_x, long screen_y);
extern "C" int point_is_over_gui_menu(long x, long y); // gui_frontmenu.c (no header has it)
extern "C" void world_parchment_magnifier(TbBool held);
extern "C" TbBool world_parchment_magnifier_icon_at(long x, long y);
extern "C" TbBool KfxAndroidPointerPlace(struct Coord3d *pos, unsigned char *context, ThingIndex *under); // front_input.c
static bool endlessMap() { return get_local_view_type() == PVT_MapScreen && world_mode_active(); }
// Endless Keeper (2026-11-28): the Pentagram's screen (Player Upgrades) - one finger drags the web as the mouse does,
// a pinch zooms it round the fingers; nothing of the dungeon's camera or the hand while it is open.
extern "C" TbBool world_pentagram_screen_open(void);
extern "C" void world_pentagram_screen_pinch(float log_scale, long screen_x, long screen_y);
static bool pentagramOpen() { return world_pentagram_screen_open() != 0; }
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
// (2026-11-24, the user: every tap came late, icons too.) A tap on a button or menu (they act on release) is let go
// after two frames, enough for the game to see the press; one on the world is let go once a game turn has seen it,
// with no time floor.
static uint64_t clickReleaseAt = 0;
static GameTurn clickTurn = 0;
static bool clickQuick = false;
static uint64_t clickFrame = 0;
static uint64_t frameCount = 0;
static void releaseClick() {
    if (!clickReleaseAt) return;
    clickReleaseAt = 0;
    button(MActn_LBUTTONUP);
}
static void click(bool quick = false) {
    releaseClick();
    button(MActn_LBUTTONDOWN);
    clickReleaseAt = SDL_GetTicks() + 1;
    clickTurn = get_gameturn();
    clickQuick = quick;
    clickFrame = frameCount;
}
static bool pointerOnGui(); // (below)
// Picking up acts on release, so with a creature under the hand click at once, before it
// walks away; anything else gets the held click.
static void tap() {
    const auto* player = get_my_player();
    if (player && get_local_view_type() == PVT_DungeonTop && local_state.local_thing_under_hand > 0) {
        releaseClick();
        button(MActn_LBUTTONDOWN); button(MActn_LBUTTONUP);
    } else click(pointerOnGui());
}
static void stepClick(uint64_t now) {
    if (!clickReleaseAt) return;
    // On a button or menu: two frames (the press is seen whichever side of the game's input it landed on).
    if (clickQuick) { if (frameCount >= clickFrame + 2) releaseClick(); return; }
    // On the world: until a game turn has seen it (digging tags on a held press), never longer than a quarter second
    // (pause). It had a 0.08 s floor on top before 2026-11-24.
    if (frameCount > clickFrame && (get_gameturn() != clickTurn || now >= clickReleaseAt + 250)) releaseClick();
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
/** The pointer is on a button or a menu box (the main menu's, a level's panels, icons and dialogs), not the world. */
static bool pointerOnGui() {
    return buttonAt(cursor.x, cursor.y) != nullptr || point_is_over_gui_menu(cursor.x, cursor.y) >= 0;
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
static void dropOne() { releaseClick(); button(MActn_RBUTTONDOWN); button(MActn_RBUTTONUP); }
// A two-finger press with something in the hand or a creature under it neither drops nor slaps.
static bool rightIgnored = false;

// ---- The hand's rules (2026-11-19), after M3RT1N99's KeeperFX Android port (touch_hand.c, touch_keeper.c;
// github.com/M3RT1N99/keeperfx, android-v1.1, GPL-2). A tap stays a click: it picks up what is under it. With
// something in the hand, a finger that rests a quarter second on ground that takes a drop lets the hand's things fall
// there one at a time for as long as it stays down (it may travel after that); one that travels sooner moves the camera
// (2026-11-24: a pan dropped the creature where the swipe began). With an empty hand a quick stroke that starts on an
// own creature slaps it and the own creatures it crosses after it; one that starts anywhere else moves the camera, as
// a slow one does (2026-11-22: quick one-finger pans slapped instead). A slap stroke never presses the left button, so
// the walls it crosses are not marked for digging (theirs turned into a dig drag on a wall). The clicks are right clicks at the finger, a mouse's own, and every click on its
// way through the input lag counts: a drop goes out only for something that will still be in the hand when it
// arrives, a slap only when nothing will be.
namespace hand {
constexpr int MaxFlights = 32, SlapMemory = 8;
constexpr long long SwipeMs = 250;
// The swipe slap is off for now (the user, 2026-11-24): a quick stroke from an own creature pans like any other.
constexpr bool SlapsOn = false;
constexpr float HoldMs = 250, RainMs = 100, SlapMs = 150;
struct Sees { bool in_force = false, known = false; int hand = 0; bool droppable = false, wall = false, door = false, thing = false;
              ThingIndex creature = 0; long long turn = 0; int lag = 0; float x = 0, y = 0; };
enum class Verdict { Wait, Today, Hand };
enum class Click { None, Drop, Slap };
struct Flight { long long turn; bool pick; };
struct Slapped { ThingIndex creature; long long ms; };
enum class Phase { None, Deciding, Today, Armed };
struct Stroke { Phase phase = Phase::None; bool readStart = false, open = false, moved = false, swipe = false, slappedOnce = false,
                fromCreature = false; // the stroke began on an own creature (read before the finger moved)
                long long downMs = 0, movedMs = 0; double nextDropTurn = 0; float slapX = 0, slapY = 0; Slapped slapped[SlapMemory]{}; };
static Flight flights[MaxFlights];
static int flightCount = 0;
static long long lastRightTurn = -1, lastPacketTurn = 0;
static Stroke stroke;
static void ageFlights(long long turn, int lag) {
    int kept = 0;
    for (int i = 0; i < flightCount; i++) {
        long long age = turn - flights[i].turn;
        if (age < 0 || age > lag) continue; // arrived, or lost to a lag change
        flights[kept++] = flights[i];
    }
    flightCount = kept;
}
static void addFlight(long long turn, bool pick) {
    if (flightCount >= MaxFlights) { std::memmove(&flights[0], &flights[1], sizeof(flights[0]) * (MaxFlights - 1)); flightCount = MaxFlights - 1; }
    flights[flightCount++] = {turn, pick};
}
static int countFlights(bool pick) { int n = 0; for (int i = 0; i < flightCount; i++) if (flights[i].pick == pick) n++; return n; }
static void notePacket(long long turn, int lag, bool rightReleased, bool picking) {
    ageFlights(turn, lag);
    lastPacketTurn = turn;
    if (rightReleased) { addFlight(turn, false); lastRightTurn = turn; }
    if (picking) addFlight(turn, true);
}
static void begin(long long ms) { stroke = {}; stroke.phase = Phase::Deciding; stroke.downMs = ms; }
static void moved(long long ms) { if (!stroke.moved) { stroke.moved = true; stroke.movedMs = ms; } }
static void end() { stroke = {}; }
static long long packetTurn(const Sees& s) { return lastPacketTurn == s.turn ? s.turn + 1 : s.turn; }
static Verdict arm(bool swipe, double firstDropTurn) { stroke.phase = Phase::Armed; stroke.swipe = swipe; stroke.nextDropTurn = firstDropTurn; return Verdict::Hand; }
static Verdict decide(const Sees& s, long long now, float longPressMs) {
    if (stroke.phase == Phase::Armed) return Verdict::Hand;
    if (stroke.phase != Phase::Deciding) return Verdict::Today;
    if (!s.in_force) { stroke.phase = Phase::Today; return Verdict::Today; }
    if (!s.known) return Verdict::Wait;
    ageFlights(s.turn, s.lag);
    const int willHold = s.hand - countFlights(false) + countFlights(true);
    if (!stroke.readStart) {
        stroke.readStart = true; stroke.open = willHold > 0 ? s.droppable : (!s.wall && !s.door);
        stroke.fromCreature = !stroke.moved && s.creature != 0;
    }
    if (!stroke.open) { stroke.phase = Phase::Today; return Verdict::Today; }
    if (stroke.moved) {
        if (willHold > 0) { // rested the quarter second first (the decision came late): drops; moved sooner: a pan
            if (stroke.movedMs - stroke.downMs >= HoldMs && s.droppable) return arm(false, static_cast<double>(packetTurn(s)));
            stroke.phase = Phase::Today; return Verdict::Today;
        }
        if (SlapsOn && stroke.fromCreature && stroke.movedMs - stroke.downMs < SwipeMs) return arm(true, static_cast<double>(packetTurn(s)));
        stroke.phase = Phase::Today; return Verdict::Today; // a pan
    }
    const long long held = now - stroke.downMs;
    if (willHold > 0 && s.droppable) return held >= HoldMs ? arm(false, static_cast<double>(packetTurn(s))) : Verdict::Wait;
    if (held < longPressMs) return Verdict::Wait;
    if (willHold > 0 || s.thing || s.wall || s.door) { stroke.phase = Phase::Today; return Verdict::Today; } // the ordinary long press
    // Bare floor and an empty hand: held like this the finger waits for what another finger takes from the panel.
    return arm(false, static_cast<double>(packetTurn(s)));
}
static bool slapDue(ThingIndex creature, long long now) {
    int oldest = 0, slot = -1;
    for (int i = 0; i < SlapMemory; i++) {
        if (stroke.slapped[i].creature == creature) slot = i;
        if (stroke.slapped[i].ms < stroke.slapped[oldest].ms) oldest = i;
    }
    if (slot >= 0 && now - stroke.slapped[slot].ms < SlapMs) return false;
    if (slot < 0) slot = oldest;
    stroke.slapped[slot] = {creature, now};
    return true;
}
static Click clickDue(const Sees& s, long long now, int whipTurns, float turnMs, float restPx) {
    if (stroke.phase != Phase::Armed || !s.in_force || !s.known) return Click::None;
    ageFlights(s.turn, s.lag);
    const long long turn = packetTurn(s);
    if (s.hand - countFlights(false) > 0) {
        if (!s.droppable || static_cast<double>(turn) < stroke.nextDropTurn) return Click::None;
        // In turns, the game's own measure of clicks; what a rate between turns carries over stays.
        const double period = RainMs / turnMs, soonest = static_cast<double>(turn) + std::floor(period);
        stroke.nextDropTurn = std::max(stroke.nextDropTurn + period, soonest);
        return Click::Drop;
    }
    if (!stroke.swipe || countFlights(true) > 0 || s.creature == 0) return Click::None;
    const long long sinceRight = turn - lastRightTurn;
    if (lastRightTurn >= 0 && sinceRight >= 0 && sinceRight < whipTurns) return Click::None;
    // A finger that has come to rest on a creature has said what it wanted: slaps take health.
    if (stroke.slappedOnce && std::hypot(s.x - stroke.slapX, s.y - stroke.slapY) < restPx) return Click::None;
    if (!slapDue(s.creature, now)) return Click::None;
    stroke.slappedOnce = true; stroke.slapX = s.x; stroke.slapY = s.y;
    return Click::Slap;
}
}
extern "C" void KfxAndroidNotePacket(const struct Packet *pckt) {
    const bool gui = (pckt->control_flags & PCtr_Gui) != 0;
    const bool rightReleased = !gui && (pckt->control_flags & PCtr_RBtnRelease) != 0;
    const bool picking = pckt->action == PckA_UsePwrHandPick || (!gui && (pckt->control_flags & PCtr_LBtnRelease) != 0);
    hand::notePacket(static_cast<long long>(get_gameturn()), game.input_lag_turns, rightReleased, picking);
}
// The finger the rules are asked about: the first one on the map. While they are deciding, a move past the tap
// distance is held back from the recogniser (it would pan); once they say the finger drops or slaps it leaves the
// recogniser altogether, and the pointer follows it.
static void drain();
struct GestFinger { float x, y; bool world; };
static std::map<SDL_FingerID, GestFinger> gestFingers; // the fingers the recogniser has
struct HandFinger { bool active = false, armed = false, movedFar = false; SDL_FingerID id = 0; float x = 0, y = 0, x0 = 0, y0 = 0; };
static HandFinger handFinger;
static bool gripping = false; // two fingers hold the map (below)
static void endGrip() { gripping = false; }
constexpr float handLongPressMs = 400; // before the recogniser's hold (450), which would press the left button
/** A window in the middle of the view is under the finger: the Keeper panel, the Library's window, the creature window,
 *  the World Boss window. A finger on one works its buttons (and drags the Keeper panel by its top strip); it never pans
 *  or turns the camera behind it (2026-11-25, the user: "The mouse panning triggers behind the panels while swiping
 *  around on the panels"). */
static bool windowOverViewAt(float x, float y) {
    const auto p = gamePosition(x, y);
    for (int idx = 0; idx < ACTIVE_MENUS_COUNT; idx++) {
        const struct GuiMenu& m = active_menus[idx];
        if (m.visual_state != 2 || !m.is_turned_on) continue;
        if (m.ident != GMnu_KEEPER_PANEL && m.ident != GMnu_LEVEL_WINDOW && m.ident != GMnu_CREATURE_WINDOW && m.ident != GMnu_WORLD_BOSS_PICK) continue;
        if (p.x >= m.pos_x && p.x < m.pos_x + m.width && p.y >= m.pos_y && p.y < m.pos_y + m.height) return true;
    }
    return false;
}

static bool handInForce(float x, float y) {
    auto* player = get_my_player();
    if (!player || player == &local_observer_player || !local_state.camera.ready || observer_is_active()) return false;
    if (get_local_view_type() != PVT_DungeonTop || get_local_map_fade() != PVT_None) return false;
    if ((game.operation_flags & GOF_Paused) != 0 || player->victory_state == VicS_LostLevel) return false;
    const auto* ustate = get_local_user_state_prediction();
    if (user_state_invalid(ustate) || ustate->work_state != PSt_CtrlDungeon) return false;
    if ((get_local_user_state()->init_flags & (UsrIF_KeyboardInputDisabled | UsrIF_MouseInputDisabled)) != 0) return false;
    if (a_menu_window_is_active() || pentagramOpen()) return false;
    auto p = gamePosition(x,y);
    return inEngineWindow(p) && !buttonAt(static_cast<int>(p.x), static_cast<int>(p.y))
        && !point_is_over_gui_box(static_cast<ScreenCoord>(p.x), static_cast<ScreenCoord>(p.y));
}
static hand::Sees handSees(float x, float y) {
    hand::Sees s;
    s.turn = static_cast<long long>(get_gameturn()); s.lag = game.input_lag_turns;
    auto p = gamePosition(x,y); s.x = p.x; s.y = p.y;
    s.in_force = handInForce(x,y);
    if (!s.in_force) return s;
    struct Coord3d pos; unsigned char context = 0; ThingIndex under = 0;
    if (!KfxAndroidPointerPlace(&pos, &context, &under)) return s;
    const auto* dungeon = get_players_dungeon(get_my_player());
    if (dungeon_invalid(dungeon)) return s;
    s.known = true;
    s.hand = dungeon->num_things_in_hand;
    s.wall = context == CSt_PickAxe;
    s.door = context == CSt_DoorKey;
    const bool onPath = s.hand > 0 ? is_creature_droppable_on_path(thing_get(dungeon->things_in_hand[0])) : true;
    s.droppable = can_drop_thing_here(coord_subtile(pos.x.val), coord_subtile(pos.y.val), my_player_number, onPath);
    const auto* thing = thing_get(under);
    s.thing = under != 0;
    if (under != 0 && thing_is_creature(thing) && thing->owner == my_player_number && thing_slappable(thing, my_player_number)) s.creature = under;
    return s;
}
static void startHandStroke(SDL_FingerID id, float x, float y) {
    handFinger = {true, false, false, id, x, y, x, y};
    hand::begin(static_cast<long long>(SDL_GetTicks()));
    position(x,y);
}
static void endHandStroke() { handFinger = {}; hand::end(); }
static void armHand() {
    handFinger.armed = true;
    gestures.cancel(); gestures.take(); // the finger leaves the recogniser: nothing it held is clicked
    if (leftHeld) { button(MActn_LBUTTONUP); leftHeld = false; }
    gestFingers.erase(handFinger.id);
    position(handFinger.x, handFinger.y);
    if (KfxAndroidInputDiagnostics()) LbSyncLog("AndroidHand armed swipe=%d turn=%lu\n", static_cast<int>(hand::stroke.swipe), static_cast<unsigned long>(get_gameturn()));
}
// True: the event was the hand's.
static bool handFingerEvent(const SDL_Event* e, float x, float y) {
    if (!handFinger.active || e->type == SDL_EVENT_FINGER_DOWN || e->tfinger.fingerID != handFinger.id) return false;
    handFinger.x = x; handFinger.y = y;
    if (!handFinger.movedFar && std::hypot(x-handFinger.x0, y-handFinger.y0) > gestures.slop()) {
        handFinger.movedFar = true;
        hand::moved(static_cast<long long>(SDL_GetTicks()));
    }
    if (handFinger.armed) {
        if (e->type == SDL_EVENT_FINGER_UP) endHandStroke();
        else position(x,y);
        return true;
    }
    if (e->type == SDL_EVENT_FINGER_UP) { endHandStroke(); return false; } // a tap or a short press: the recogniser's
    if (handFinger.movedFar) { position(x,y); return true; }              // held back until the rules have spoken
    return false;
}
static void stepHand(uint64_t now) {
    if (!handFinger.active) return;
    if (!handFinger.armed) {
        auto verdict = hand::decide(handSees(handFinger.x, handFinger.y), static_cast<long long>(now), handLongPressMs);
        if (verdict == hand::Verdict::Hand) armHand();
        else if (verdict == hand::Verdict::Today) {
            const bool heldBack = handFinger.movedFar;
            const SDL_FingerID id = handFinger.id; const float x = handFinger.x, y = handFinger.y;
            endHandStroke();
            if (heldBack) { gestures.move(static_cast<int64_t>(id), x, y, now); drain(); } // the move it was not given
        }
        return;
    }
    position(handFinger.x, handFinger.y);
    const float turnMs = 1000.0f / static_cast<float>(std::max<int32_t>(turns_per_second, 1));
    const float restPx = static_cast<float>(RendererPhysicalHeight()) * 6.0f / 720.0f;
    auto click = hand::clickDue(handSees(handFinger.x, handFinger.y), static_cast<long long>(now),
                                static_cast<int>(player_instance_info[PI_Whip].length_turns), turnMs, restPx);
    if (click == hand::Click::None) return;
    dropOne();
    if (KfxAndroidInputDiagnostics()) LbSyncLog("AndroidHand %s turn=%lu\n", click == hand::Click::Drop ? "drop" : "slap", static_cast<unsigned long>(get_gameturn()));
}

// Double tap and hold drags a dig rectangle: the second touch presses the left button
// in KeeperFX's drag-placement highlight mode and lifting tags the box. In dig context
// a lone tap waits for the double-tap window, so the first tap of a double tap never
// tags its tile (the tile under the press decides tag versus untag).
constexpr uint64_t doubleTapMs = 260;
struct PendingTap { bool active = false; float x = 0, y = 0; uint64_t at = 0; bool room = false; bool sent = false; };
static PendingTap pendingTap;
enum class DigDrag { None, Arming, Pressed };
static DigDrag digDrag = DigDrag::None;
static SDL_FPoint digStart{}, digLatest{};
static bool digReleased = false;
static GameTurn digTurn = 0, digReleaseTurn = 0, restoreTurn = 0;
static int restoreMode = -1, restoreTries = 0;
// A hold on a wall presses only once the finger slides (marking walls along the way) or
// lifts (a click), so a second finger can still turn it into the dig square before a wall
// under it is marked (which would make the square unmark instead).
static bool holdDeferred = false;
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
        if (clickReleaseAt) return; // a click still held (the one putting a double tap's first tap back)
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
        position(a.x,a.y);
        if (a.kind == Kind::LeftDown) holdAt = {a.x,a.y};
        else if (leftHeld && a.kind == Kind::Hover) {
            if (get_gameturn() <= holdPinTurn) position(holdAt.x, holdAt.y); // a slid dig hold starts on its wall
            else holdAt = {a.x,a.y};
        }
        if (a.kind!=Kind::Hover && KfxAndroidInputDiagnostics()) {
            LbSyncLog("AndroidInput action=%d x=%.1f y=%.1f dx=%.4f dy=%.4f cursor=%ld,%ld target=%d\n",static_cast<int>(a.kind),a.x,a.y,a.dx,a.dy,static_cast<long>(lbDisplay.MMouseX),static_cast<long>(lbDisplay.MMouseY),local_state.local_thing_under_hand);
            inputSnapshotAt=SDL_GetTicks()+350;
            inputThing=local_state.local_thing_under_hand;
        }
        switch (a.kind) {
        case Kind::LeftClick:
            // A tap on a creature picks it up at once (the hand's rules, below).
            if (handOverThing() && inEngineWindow(gamePosition(a.x,a.y))) { tap(); break; }
            // A dig tap goes at once (it waited the double-tap window before 2026-11-24); should it be the first of a
            // double tap, it is taken back when the second one comes (finger down, below), before the dig box starts.
            if (digContext(a.x,a.y)) { tap(); pendingTap = {true, a.x, a.y, SDL_GetTicks(), false, true}; break; }
            if (roomContext(a.x,a.y)) { pendingTap = {true, a.x, a.y, SDL_GetTicks(), true}; break; }
            textKeyboard(a.x,a.y);
            tap(); break;
        case Kind::RightClick:
            if (handHoldsThings() || handOverThing()) break; // no drop, no slap
            releaseClick(); button(MActn_RBUTTONDOWN); button(MActn_RBUTTONUP); break;
        case Kind::LeftDown:
            if (digContext(a.x,a.y)) { holdDeferred = true; break; }
            releaseClick(); button(MActn_LBUTTONDOWN); leftHeld = true; nextPickupAt = 0; break;
        case Kind::LeftUp:
            if (holdDeferred) { holdDeferred = false; tap(); break; } // lifted in place: a click
            button(MActn_LBUTTONUP); leftHeld = false; break;
        case Kind::Hover:
            if (holdDeferred && std::hypot(a.x-holdAt.x, a.y-holdAt.y) > gestures.slop()) {
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
        case Kind::Pan: if (gripping) break; panX -= a.dx; panY -= a.dy; break;
        case Kind::Zoom:
            if (gripping || endlessMap() || pentagramOpen()) break; // the map's and the Pentagram's pinches go by the fingers (mapFrame, pentagramFrame)
            zoomDelta += a.dx; break;
        case Kind::Rotate: if (endlessMap() || gripping || pentagramOpen()) break; rotationDelta += a.dx; break;
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
    // Endless (2026-11-24): inside the screen safe area (rounded corners, the camera hole; GameActivity sets it)
    SDL_Rect safe{0, 0, w, h};
    auto* win = static_cast<SDL_Window*>(GetSDLWindowSystem()->GetNativeWindow());
    if (!win || !SDL_GetWindowSafeArea(win, &safe) || safe.w <= 0 || safe.h <= 0) safe = SDL_Rect{0, 0, w, h};
    const float sl = static_cast<float>(safe.x), st = static_cast<float>(safe.y);
    const float sr = static_cast<float>(w - safe.x - safe.w), sb = static_cast<float>(h - safe.y - safe.h);
    l.stick = {std::max(panel, sl) + .25f*u, h - sb - .27f*u};
    l.attack = {w - sr - .2f*u, h - sb - .25f*u};
    l.exit = {w - sr - .1f*u, st + .26f*u};
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
        pendingTap.active = false; holdDeferred = false; endHandStroke(); gestFingers.clear();
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
// The possession controls as one list of triangles (2026-11-27): the SDL_Renderer path draws it with one
// SDL_RenderGeometry call, the Vulkan renderer takes it as it is and draws it on the GPU (on the software picture
// it cost 10.5 ms a frame on a 2260x1200 phone).
static std::vector<SDL_Vertex> touchVerts;
static std::vector<int> touchIndex;
static void disc(SDL_FPoint c, float inner, float outer, SDL_FColor colour) {
    constexpr int segments = 48;
    const int base = static_cast<int>(touchVerts.size());
    if (inner <= 0) touchVerts.push_back({c, colour, {0,0}});
    for (int i = 0; i <= segments; i++) {
        float a = i*6.2831853f/segments, cs = std::cos(a), sn = std::sin(a);
        touchVerts.push_back({{c.x+cs*outer, c.y+sn*outer}, colour, {0,0}});
        if (inner > 0) touchVerts.push_back({{c.x+cs*inner, c.y+sn*inner}, colour, {0,0}});
    }
    for (int i = 0; i < segments; i++) {
        if (inner <= 0) { touchIndex.insert(touchIndex.end(), {base, base+i+1, base+i+2}); continue; }
        int o = base+i*2; touchIndex.insert(touchIndex.end(), {o, o+1, o+2, o+1, o+3, o+2});
    }
}
static void bar(SDL_FPoint a, SDL_FPoint b, float width, SDL_FColor colour) {
    float dx = b.x-a.x, dy = b.y-a.y, length = std::max(.001f, std::hypot(dx,dy));
    float nx = -dy/length*width*.5f, ny = dx/length*width*.5f;
    const int o = static_cast<int>(touchVerts.size());
    touchVerts.insert(touchVerts.end(), {{{a.x+nx,a.y+ny},colour,{0,0}}, {{a.x-nx,a.y-ny},colour,{0,0}}, {{b.x+nx,b.y+ny},colour,{0,0}}, {{b.x-nx,b.y-ny},colour,{0,0}}});
    touchIndex.insert(touchIndex.end(), {o, o+1, o+2, o+1, o+3, o+2});
}
// ow: the width of the picture drawn on, in pixels. False when nothing is to be drawn.
static bool touchOverlayGeometry(int ow) {
    touchVerts.clear(); touchIndex.clear();
    if (!possessing) return false;
    int w=0,h=0; GetSDLWindowSystem()->GetWindowSize(&w,&h);
    if (w <= 0 || ow <= 0) return false;
    float k = static_cast<float>(ow)/w;
    auto at = [k](SDL_FPoint p) { return SDL_FPoint{p.x*k, p.y*k}; };
    auto l = possessLayout();
    SDL_FPoint base = l.stick;
    for (const auto& f : possessFingers) if (f.second.role == Role::Stick) base = f.second.origin;
    const SDL_FColor light{1,.93f,.8f,.45f}, shade{0,0,0,.28f};
    disc(at(base), 0, l.stickR*k, shade);
    disc(at(base), l.stickR*k*.94f, l.stickR*k, light);
    // Faint ring where walking turns into running (stickRun of the knob's travel).
    float runAt = .12f + .88f*stickRun;
    disc(at(base), l.stickR*k*(runAt-.02f), l.stickR*k*(runAt+.01f), SDL_FColor{1,.93f,.8f,.18f});
    disc(at({base.x+stickX*l.stickR, base.y+stickY*l.stickR}), 0, l.stickR*.42f*k, SDL_FColor{1,.93f,.8f,.5f});
    disc(at(l.attack), 0, l.attackR*k, attackDown ? SDL_FColor{.85f,.12f,.05f,.75f} : SDL_FColor{.6f,.08f,.03f,.5f});
    disc(at(l.attack), l.attackR*k*.93f, l.attackR*k, light);
    // Three claw marks for attack, a cross for leaving the creature.
    SDL_FPoint c = at(l.attack); float a = l.attackR*k*.45f;
    for (int i = -1; i <= 1; i++) bar({c.x-a*.55f+i*a*.5f, c.y+a}, {c.x+a*.55f+i*a*.5f, c.y-a}, a*.2f, light);
    disc(at(l.exit), 0, l.exitR*k, shade);
    disc(at(l.exit), l.exitR*k*.88f, l.exitR*k, light);
    c = at(l.exit); a = l.exitR*k*.42f;
    bar({c.x-a,c.y-a}, {c.x+a,c.y+a}, a*.3f, light); bar({c.x-a,c.y+a}, {c.x+a,c.y-a}, a*.3f, light);
    return !touchIndex.empty();
}
extern "C" void KfxAndroidDrawTouchOverlay(SDL_Renderer* r) {
    int ow=0,oh=0;
    if (!possessing || !SDL_GetCurrentRenderOutputSize(r,&ow,&oh) || !touchOverlayGeometry(ow)) return;
    float sx=1,sy=1; SDL_BlendMode blend;
    SDL_GetRenderScale(r,&sx,&sy); SDL_GetRenderDrawBlendMode(r,&blend);
    SDL_SetRenderScale(r,1,1); SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(r, nullptr, touchVerts.data(), static_cast<int>(touchVerts.size()), touchIndex.data(), static_cast<int>(touchIndex.size()));
    SDL_SetRenderScale(r,sx,sy); SDL_SetRenderDrawBlendMode(r,blend);
}
// The Vulkan renderer's way in: the controls for a picture out_w pixels wide (game thread, like the drawing).
extern "C" int KfxAndroidTouchOverlayGeometry(int out_w, const SDL_Vertex** verts, int* nverts, const int** indices, int* nindices) {
    if (!touchOverlayGeometry(out_w)) return 0;
    *verts = touchVerts.data(); *nverts = static_cast<int>(touchVerts.size());
    *indices = touchIndex.data(); *nindices = static_cast<int>(touchIndex.size());
    return 1;
}

// ---- Two fingers hold the map (2026-11-19), after M3RT1N99's KeeperFX Android port (touch_gesture.c, touch_camera.c,
// GPL-2). The ground point under the fingers' midpoint stays under it: moving, zooming and turning are one transform
// round it, worked out again every drawn frame and written to the camera the frame shows (its previous and its
// destination pose alike, so the turn-to-turn smoothing has nothing left to chase). Small gates keep a drag from
// zooming and a pinch from turning; a gate that opens hands over what it held back within a moment, so nothing
// jumps. Before, a pinch and a twist were one or the other, went into the camera once a game turn and were chased
// by the smoothing: the jitter. The zoom stays within the game's own limits (the user's nearest to CAMERA_ZOOM_MAX).
namespace grip {
struct Fingers { float ax, ay, bx, by; };
struct View { float cx, cy, pxPerZoom, squash, lift, angleUnits, yawStep, limitX, limitY, zoomMin, zoomMax; };
struct Pose { float x, y, zoom, yaw; };
struct Tuning { float panSlop, zoomGate, rotateGate, catchUpMs, stepTwist; };
struct Gesture {
    Pose origin{}; float groundX = 0, groundY = 0, height = 0, mid0x = 0, mid0y = 0, dist0 = 1, prevAngle = 0, twist = 0;
    bool panOpen = false, zoomOpen = false, rotateOpen = false;
    float panLeftX = 0, panLeftY = 0, zoomLeft = 0, twistLeft = 0, stepRef = 0; int steps = 0;
};
constexpr float Pi = 3.14159265358979323846f;
static float yawRadians(const View& v, float yaw) { return yaw * 2.0f * Pi / v.angleUnits; }
static void screenToGround(const View& v, const Pose& c, float height, float sx, float sy, float& gx, float& gy) {
    const float scale = c.zoom * v.pxPerZoom;
    const float ux = (sx - v.cx) / scale, uy = ((sy - v.cy) / scale + v.lift * height) / v.squash;
    const float yaw = yawRadians(v, c.yaw), cs = std::cos(yaw), sn = std::sin(yaw);
    gx = c.x + ux * cs - uy * sn;
    gy = c.y + ux * sn + uy * cs;
}
static void midpoint(const Fingers& f, float& x, float& y) { x = (f.ax + f.bx) * .5f; y = (f.ay + f.by) * .5f; }
static float spread(const Fingers& f) { return std::max(1.0f, std::hypot(f.bx - f.ax, f.by - f.ay)); }
static float angle(const Fingers& f) { return std::atan2(f.by - f.ay, f.bx - f.ax); }
static void take(Gesture& g, const Fingers& f, const View& v, const Pose& c, float height) {
    g = {};
    g.origin = c; g.height = height;
    midpoint(f, g.mid0x, g.mid0y);
    g.dist0 = spread(f); g.prevAngle = angle(f);
    screenToGround(v, c, height, g.mid0x, g.mid0y, g.groundX, g.groundY);
}
static float catchUp(float left, float keep, float negligible) { left *= keep; return std::fabs(left) < negligible ? 0.0f : left; }
static void follow(Gesture& g, const Fingers& f, float dtMs, const View& v, const Tuning& t, float twistSign, Pose& c) {
    float mx, my; midpoint(f, mx, my);
    const float dist = spread(f), a = angle(f);
    float turn = a - g.prevAngle;
    if (turn > Pi) turn -= 2 * Pi; else if (turn < -Pi) turn += 2 * Pi;
    g.prevAngle = a;
    g.twist += turn * twistSign;
    const float travelX = mx - g.mid0x, travelY = my - g.mid0y, logZoom = std::log(dist / g.dist0);
    const float keep = std::exp(-dtMs / t.catchUpMs);
    if (g.panOpen) { g.panLeftX = catchUp(g.panLeftX, keep, .25f); g.panLeftY = catchUp(g.panLeftY, keep, .25f); }
    else { g.panOpen = std::hypot(travelX, travelY) >= t.panSlop; g.panLeftX = travelX; g.panLeftY = travelY; }
    if (g.zoomOpen) g.zoomLeft = catchUp(g.zoomLeft, keep, .0005f);
    else { g.zoomOpen = std::fabs(dist - g.dist0) >= t.zoomGate; g.zoomLeft = logZoom; }
    if (g.rotateOpen) g.twistLeft = catchUp(g.twistLeft, keep, .0005f);
    else { g.rotateOpen = std::fabs(g.twist) >= t.rotateGate; g.twistLeft = g.twist; }
    const float zoom = g.origin.zoom * std::exp(logZoom - g.zoomLeft);
    const float limited = std::clamp(zoom, v.zoomMin, v.zoomMax);
    // Pulling past a limit moves the reference along: the way back starts at once.
    if (g.zoomOpen && limited != zoom) g.dist0 = dist / std::exp(std::log(limited / g.origin.zoom) + g.zoomLeft);
    c.zoom = std::round(limited);
    float yaw = g.origin.yaw;
    if (v.yawStep > 0) { // the front view turns in quarter steps
        while (g.twist - g.stepRef >= t.stepTwist) { g.stepRef += t.stepTwist; g.steps++; }
        while (g.twist - g.stepRef <= -t.stepTwist) { g.stepRef -= t.stepTwist; g.steps--; }
        yaw = g.origin.yaw - static_cast<float>(g.steps) * v.yawStep;
    } else yaw -= (g.twist - g.twistLeft) * v.angleUnits / (2 * Pi);
    c.yaw = std::round(yaw);
    const float heldX = mx - g.panLeftX, heldY = my - g.panLeftY;
    Pose atOrigin = c; atOrigin.x = atOrigin.y = 0;
    float ox, oy; screenToGround(v, atOrigin, g.height, heldX, heldY, ox, oy);
    const float x = g.groundX - ox, y = g.groundY - oy;
    c.x = std::clamp(x, 0.0f, v.limitX); c.y = std::clamp(y, 0.0f, v.limitY);
    // The map's edge stops the map, not the fingers: they slide over the ground from here.
    if (c.x != x || c.y != y) screenToGround(v, c, g.height, heldX, heldY, g.groundX, g.groundY);
}
}
static grip::Gesture gripGesture;
static grip::Pose gripLeft{}; // what the grip last left in the camera, to notice when something else moved it
static int gripLeftIdx = -1;
static uint64_t gripLastNs = 0;
static grip::Pose poseOf(const struct Camera* cam) {
    return {static_cast<float>(cam->mappos.x.val), static_cast<float>(cam->mappos.y.val), static_cast<float>(cam->zoom), static_cast<float>(cam->rotation_angle_x)};
}
static grip::View viewOf(const struct Camera* cam, int idx) {
    grip::View v{};
    const bool front = idx == CamIV_FrontView;
    const float scale = static_cast<float>(units_per_pixel_best) / 16.0f / 65536.0f;
    v.cx = static_cast<float>(local_state.engine_window_x) + static_cast<float>(local_state.engine_window_width / 2);
    v.cy = static_cast<float>(local_state.engine_window_y) + static_cast<float>(local_state.engine_window_height / 2);
    v.angleUnits = static_cast<float>(DEGREES_360);
    v.limitX = static_cast<float>(game.map_subtiles_x * COORD_PER_STL - 1);
    v.limitY = static_cast<float>(game.map_subtiles_y * COORD_PER_STL - 1);
    const int32_t zoomMin = user_zoom_min(get_local_user_state(), front);
    if (front) { // the front view scales by an eighth of the zoom, doesn't foreshorten, shows height at a half
        v.pxPerZoom = scale / 8.0f; v.squash = 1.0f; v.lift = .5f; v.yawStep = static_cast<float>(DEGREES_90);
        v.zoomMin = static_cast<float>(std::max<int32_t>(FRONTVIEW_CAMERA_ZOOM_MIN, zoomMin)); v.zoomMax = static_cast<float>(FRONTVIEW_CAMERA_ZOOM_MAX);
    } else {
        const float tilt = static_cast<float>(-cam->rotation_angle_y) * 2.0f * grip::Pi / static_cast<float>(DEGREES_360);
        v.pxPerZoom = scale; v.squash = std::sin(tilt); v.lift = std::cos(tilt); v.yawStep = 0;
        v.zoomMin = static_cast<float>(zoomMin); v.zoomMax = static_cast<float>(CAMERA_ZOOM_MAX);
    }
    if (std::fabs(v.squash) < .05f) v.squash = .05f;
    return v;
}
// The height of what the player sees at a pixel: the first column a ray from the camera meets.
static float surfaceHeightAt(const grip::View& v, const grip::Pose& p, float sx, float sy) {
    for (int z = 8; z > 0; z--) {
        const float height = static_cast<float>(z * COORD_PER_STL);
        float gx, gy; grip::screenToGround(v, p, height, sx, sy, gx, gy);
        if (gx < 0 || gy < 0 || gx > v.limitX || gy > v.limitY) continue;
        const struct Map* mapblk = get_map_block_at(coord_subtile(static_cast<long>(gx)), coord_subtile(static_cast<long>(gy)));
        if (!map_block_invalid(mapblk) && get_mapblk_filled_subtiles(mapblk) >= z) return height;
    }
    return 0;
}
static void showPose(int idx, const grip::Pose& p) {
    gripLeft = {static_cast<float>(std::lround(p.x)), static_cast<float>(std::lround(p.y)), p.zoom, static_cast<float>(std::lround(p.yaw) & ANGLE_MASK)};
    gripLeftIdx = idx;
    struct Camera* cams[2] = {&local_state.camera.destination[idx], &local_state.camera.previous[idx]};
    for (auto* cam : cams) {
        cam->mappos.x.val = static_cast<MapCoord>(gripLeft.x);
        cam->mappos.y.val = static_cast<MapCoord>(gripLeft.y);
        cam->zoom = static_cast<int32_t>(gripLeft.zoom);
        cam->rotation_angle_x = static_cast<int32_t>(gripLeft.yaw);
        cam->velocity_x = cam->velocity_y = 0;
        cam->velocity_rotation = 0;
    }
}
static bool movedBySomethingElse(const struct Camera* cam, int idx) {
    const auto p = poseOf(cam);
    return idx != gripLeftIdx || p.x != gripLeft.x || p.y != gripLeft.y || p.zoom != gripLeft.zoom || p.yaw != gripLeft.yaw;
}
static void cameraFrame() {
    const uint64_t nowNs = SDL_GetTicksNS();
    const float dtMs = std::clamp(gripLastNs ? static_cast<float>(nowNs - gripLastNs) / 1e6f : 16.0f, 1.0f, 100.0f);
    gripLastNs = nowNs;
    bool two = gestFingers.size() == 2 && !possessing && !squareActive && digDrag == DigDrag::None && roomClick == RoomClick::None && !endlessMap() && !pentagramOpen();
    for (const auto& f : gestFingers) two = two && f.second.world;
    if (two && (!local_state.camera.ready || get_local_view_type() != PVT_DungeonTop || get_local_map_fade() != PVT_None || a_menu_window_is_active())) two = false;
    const int idx = two ? static_cast<int>(get_local_active_camera() - local_state.camera.current) : -1;
    if (two && ((idx != CamIV_Isometric && idx != CamIV_FrontView) || local_state.camera.move_cam != nullptr)) two = false;
    if (two) {
        const auto* ustate = get_local_user_state();
        if (player_instance_controls_camera(ustate->instance_num) || (ustate->init_flags & (UsrIF_KeyboardInputDisabled | UsrIF_MouseInputDisabled)) != 0) two = false;
    }
    if (!two) { gripping = false; return; }
    // A spectator's camera follows the watched player until the fingers detach it.
    if (observer_is_following_user()) detach_observer_camera();
    auto a = gestFingers.begin(), b = std::next(a);
    const auto pa = gamePosition(a->second.x, a->second.y), pb = gamePosition(b->second.x, b->second.y);
    const grip::Fingers fingers{pa.x, pa.y, pb.x, pb.y};
    const struct Camera* cam = &local_state.camera.destination[idx];
    const grip::View view = viewOf(cam, idx);
    const float shortSide = static_cast<float>(std::max<long>(1, std::min(RendererPhysicalWidth(), RendererPhysicalHeight())));
    const grip::Tuning tuning{shortSide * .014f, shortSide * .02f, .10f, 33.0f, .5f};
    if (!gripping || movedBySomethingElse(cam, idx)) {
        // From what is on screen, not from the destination the smoothing may still be on its way to.
        const grip::Pose shown = poseOf(&local_state.camera.current[idx]);
        float mx, my; grip::midpoint(fingers, mx, my);
        grip::take(gripGesture, fingers, view, shown, surfaceHeightAt(view, shown, mx, my));
        gripping = true;
        // The finger midpoint's pan and the recogniser's pinch/twist from before the grip are dropped.
        panX = panY = zoomDelta = rotationDelta = 0;
    }
    grip::Pose pose = gripGesture.origin;
    grip::follow(gripGesture, fingers, dtMs, view, tuning, rotateWithFingers ? 1.0f : -1.0f, pose);
    showPose(idx, pose);
}

// ---- The endless world's full-screen map (2026-11-19, the user: its pinch jittered, and wanted a magnifier to hold).
// A pinch zooms every drawn frame by the fingers' own spread, round the point between them, to any whole number of
// pixels per slab (it stepped between ten levels after a set amount of pinching, and a twist could take it over).
// The magnifying glass icon under the hint line: held, the magnifier shows, up and left of the finger, and follows it.
static bool mapPinchOpen = false, mapPinchTracking = false;
static float mapPinchStart = 0, mapPinchLast = 0;
static void mapFrame() {
    if (!endlessMap() || gestFingers.size() != 2) { mapPinchTracking = mapPinchOpen = false; return; }
    auto a = gestFingers.begin(), b = std::next(a);
    const auto pa = gamePosition(a->second.x, a->second.y), pb = gamePosition(b->second.x, b->second.y);
    const float spread = std::max(1.0f, std::hypot(pb.x - pa.x, pb.y - pa.y));
    if (!mapPinchTracking) { mapPinchTracking = true; mapPinchOpen = false; mapPinchStart = mapPinchLast = spread; return; }
    const float shortSide = static_cast<float>(std::max<long>(1, std::min(RendererPhysicalWidth(), RendererPhysicalHeight())));
    if (!mapPinchOpen) {
        if (std::fabs(spread - mapPinchStart) < shortSide * .02f) return; // a two-finger tap must not zoom
        mapPinchOpen = true;
    }
    parchment_pinch(std::log(spread / mapPinchLast), static_cast<long>((pa.x + pb.x) * .5f) / pixel_size, static_cast<long>((pa.y + pb.y) * .5f) / pixel_size);
    mapPinchLast = spread;
}
// The Pentagram's screen: a pinch zooms every drawn frame by the fingers' own spread, round the point between them, as
// the map's (a two-finger tap does not zoom).
static bool pentPinchOpen = false, pentPinchTracking = false;
static float pentPinchStart = 0, pentPinchLast = 0;
static void pentagramFrame() {
    if (!pentagramOpen() || gestFingers.size() != 2) { pentPinchTracking = pentPinchOpen = false; return; }
    auto a = gestFingers.begin(), b = std::next(a);
    const auto pa = gamePosition(a->second.x, a->second.y), pb = gamePosition(b->second.x, b->second.y);
    const float spread = std::max(1.0f, std::hypot(pb.x - pa.x, pb.y - pa.y));
    if (!pentPinchTracking) { pentPinchTracking = true; pentPinchOpen = false; pentPinchStart = pentPinchLast = spread; return; }
    const float shortSide = static_cast<float>(std::max<long>(1, std::min(RendererPhysicalWidth(), RendererPhysicalHeight())));
    if (!pentPinchOpen) {
        if (std::fabs(spread - pentPinchStart) < shortSide * .02f) return;
        pentPinchOpen = true;
    }
    world_pentagram_screen_pinch(std::log(spread / pentPinchLast), static_cast<long>((pa.x + pb.x) * .5f) / pixel_size,
        static_cast<long>((pa.y + pb.y) * .5f) / pixel_size);
    pentPinchLast = spread;
}
static bool magnifierHeld = false;
static SDL_FingerID magnifierFinger = 0;
static void endMagnifier() { if (magnifierHeld) world_parchment_magnifier(false); magnifierHeld = false; }
// True: the event was the magnifier's.
static bool magnifierEvent(const SDL_Event* e, float x, float y) {
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (magnifierHeld || !endlessMap() || !gestFingers.empty() || a_menu_window_is_active()) return false;
        auto p = gamePosition(x,y);
        if (!world_parchment_magnifier_icon_at(static_cast<long>(p.x), static_cast<long>(p.y))) return false;
        magnifierHeld = true; magnifierFinger = e->tfinger.fingerID;
        world_parchment_magnifier(true);
        position(x,y);
        return true;
    }
    if (!magnifierHeld || e->tfinger.fingerID != magnifierFinger) return false;
    if (e->type == SDL_EVENT_FINGER_UP || !endlessMap()) endMagnifier();
    else position(x,y);
    return true;
}

extern "C" int KfxAndroidTouchEvent(const SDL_Event* e) {
    if (e->type==SDL_EVENT_RENDER_DEVICE_RESET || e->type==SDL_EVENT_RENDER_TARGETS_RESET || e->type==SDL_EVENT_RENDER_DEVICE_LOST) {
        presentationGeneration++;
        return 0;
    }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_FINGER_CANCELED) {
        gestures.cancel(); drain(); panX=panY=zoomDelta=rotationDelta=0;
        pendingTap.active = false; releaseClick(); endSquare(false); releaseSquareHold(nullptr); stopRoomClick();
        spentFingers.clear(); endPossessionInput(); // cancelled fingers send no lift; their ids come back
        holdDeferred = rightIgnored = false; endHandStroke(); gestFingers.clear(); endGrip(); endMagnifier();
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
    if (magnifierEvent(e, x, y)) return 1;
    if (squareActive) { squareFingerEvent(e,x,y); return 1; }
    if (handFingerEvent(e, x, y)) return 1;
    if (e->type == SDL_EVENT_FINGER_DOWN && leftHeld && roomContext(holdAt.x,holdAt.y)) { startSquare(e->tfinger.fingerID,x,y,false); return 1; }
    if (e->type == SDL_EVENT_FINGER_DOWN && holdDeferred && digContext(holdAt.x,holdAt.y)) { startSquare(e->tfinger.fingerID,x,y,true); return 1; }
    if (e->type == SDL_EVENT_FINGER_DOWN) lastFingerDown = e->tfinger.fingerID;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        releaseClick(); // a tap still held must not turn into a drag under the next finger
        updatePossession();
        if (possessing) { possessionDown(e->tfinger.fingerID,x,y); return 1; }
        // Menus and the sidebar retain normal mouse dragging; world drags pan. The endless world's map is
        // dragged like a mouse drags it (it follows the finger), so it gets the left button instead.
        const auto* player = get_my_player();
        bool world = player && !endlessMap() && !pentagramOpen() && (get_local_view_type() == PVT_DungeonTop || get_local_view_type() == PVT_MapScreen);
        if (windowOverViewAt(x, y)) world = false; // (a window over the view: its own finger, no pan, no grip)
        gestures.setPanEnabled(world && inEngineWindow(gamePosition(x,y)));
        // The first finger on the map is asked about by the hand's rules (it may come to drop or slap).
        if (gestFingers.empty() && handInForce(x,y)) startHandStroke(e->tfinger.fingerID, x, y);
        else endHandStroke(); // a second finger: the first one's stroke is part of something else now
        gestFingers[e->tfinger.fingerID] = {x, y, world && inEngineWindow(gamePosition(x,y))};
        gestures.down(static_cast<int64_t>(e->tfinger.fingerID),x,y,SDL_GetTicks());
        if (pendingTap.active && SDL_GetTicks()-pendingTap.at < doubleTapMs
         && std::hypot(x-pendingTap.x, y-pendingTap.y) <= std::max(48.0f, gestures.slop()*3) && gestures.captureAsDrag()) {
            pendingTap.active = false;
            if (pendingTap.room) startRoomClick(Gkey_BestRoomSpace,false,x,y);
            else {
                // The first tap marked (or unmarked) its wall: the same click again puts it back, so the wall's own
                // mark decides marking or unmarking for the box, as before. (The box waits for it to be let go.)
                if (pendingTap.sent) { position(pendingTap.x, pendingTap.y); click(); }
                startDigDrag(x,y);
            }
        }
    } else if (e->type == SDL_EVENT_FINGER_UP) {
        gestFingers.erase(e->tfinger.fingerID);
        gestures.up(static_cast<int64_t>(e->tfinger.fingerID),x,y,SDL_GetTicks());
    } else {
        auto it = gestFingers.find(e->tfinger.fingerID);
        if (it != gestFingers.end()) { it->second.x = x; it->second.y = y; }
        gestures.move(static_cast<int64_t>(e->tfinger.fingerID),x,y,SDL_GetTicks());
    }
    drain(); return 1;
}
extern "C" void KfxAndroidTouchTick(void) {
    gestures.tick(SDL_GetTicks());drain();
    uint64_t now = SDL_GetTicks();
    updatePossession();
    if (pendingTap.active && now-pendingTap.at >= doubleTapMs) {
        pendingTap.active = false;
        if (!pendingTap.sent) { position(pendingTap.x, pendingTap.y); tap(); }
    }
    frameCount++;
    stepHand(now);
    cameraFrame();
    mapFrame();
    pentagramFrame();
    if (magnifierHeld && !endlessMap()) endMagnifier();
    stepClick(now);
    stepDigDrag();
    stepSquare();
    stepRoomClick();
    repeatPickup(now);
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

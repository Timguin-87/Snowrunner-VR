#include "xr/xr_input.h"
#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace xrinput {
namespace {

// The HP Reverb G2's controllers have their own interaction profile, behind an
// extension; without it they would fall back to whatever the runtime emulates.
constexpr const char* kHpExtension = "XR_EXT_hp_mixed_reality_controller";
bool g_hpEnabled = false;

std::atomic<bool> g_enabled{true};

XrInstance  g_instance = XR_NULL_HANDLE;
XrActionSet g_set      = XR_NULL_HANDLE;
bool        g_attached = false;

enum ActionId {
    kLT, kRT, kLG, kRG,          // float: triggers, grips
    kLS, kRS,                    // vector2f: sticks
    kL3, kR3, kA, kB, kX, kY,    // boolean
    kStart, kBack,
    kActionCount
};
XrAction g_act[kActionCount] = {};

struct ActionDef { ActionId id; const char* name; const char* label; XrActionType type; };
const ActionDef kActions[] = {
    { kLT,    "left_trigger",  "Left trigger (LT)",   XR_ACTION_TYPE_FLOAT_INPUT },
    { kRT,    "right_trigger", "Right trigger (RT)",  XR_ACTION_TYPE_FLOAT_INPUT },
    { kLG,    "left_grip",     "Left grip (LB)",      XR_ACTION_TYPE_FLOAT_INPUT },
    { kRG,    "right_grip",    "Right grip (RB)",     XR_ACTION_TYPE_FLOAT_INPUT },
    { kLS,    "left_stick",    "Left stick",          XR_ACTION_TYPE_VECTOR2F_INPUT },
    { kRS,    "right_stick",   "Right stick",         XR_ACTION_TYPE_VECTOR2F_INPUT },
    { kL3,    "left_click",    "Left stick click (L3)",  XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kR3,    "right_click",   "Right stick click (R3)", XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kA,     "button_a",      "A",                   XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kB,     "button_b",      "B",                   XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kX,     "button_x",      "X",                   XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kY,     "button_y",      "Y",                   XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kStart, "button_start",  "Start",               XR_ACTION_TYPE_BOOLEAN_INPUT },
    { kBack,  "button_back",   "Back",                XR_ACTION_TYPE_BOOLEAN_INPUT },
};

// --- bindings, one table per interaction profile ---------------------------
//
// Every path is one the profile really has (checked against the OpenXR
// registry, xr.xml): a single unknown path makes the runtime reject that
// profile's whole suggestion. Booleans bound to float paths and the reverse
// are legal -- the runtime thresholds or converts.
struct Bind { ActionId id; const char* path; };

#define LH "/user/hand/left/input/"
#define RH "/user/hand/right/input/"
const Bind kTouch[] = {
    { kLT, LH "trigger/value" },   { kRT, RH "trigger/value" },
    { kLG, LH "squeeze/value" },   { kRG, RH "squeeze/value" },
    { kLS, LH "thumbstick" },      { kRS, RH "thumbstick" },
    { kL3, LH "thumbstick/click" },{ kR3, RH "thumbstick/click" },
    { kX,  LH "x/click" },         { kY,  LH "y/click" },
    { kA,  RH "a/click" },         { kB,  RH "b/click" },
    { kStart, LH "menu/click" },
};
// The G2 has a menu button on each controller; the right one is Back.
const Bind kHpG2[] = {
    { kLT, LH "trigger/value" },   { kRT, RH "trigger/value" },
    { kLG, LH "squeeze/value" },   { kRG, RH "squeeze/value" },
    { kLS, LH "thumbstick" },      { kRS, RH "thumbstick" },
    { kL3, LH "thumbstick/click" },{ kR3, RH "thumbstick/click" },
    { kX,  LH "x/click" },         { kY,  LH "y/click" },
    { kA,  RH "a/click" },         { kB,  RH "b/click" },
    { kStart, LH "menu/click" },   { kBack, RH "menu/click" },
};
// Index: A/B on both hands, so the left pair is X/Y. No menu button reaches
// applications, so Start/Back are a firm squeeze of each trackpad.
const Bind kIndex[] = {
    { kLT, LH "trigger/value" },   { kRT, RH "trigger/value" },
    { kLG, LH "squeeze/value" },   { kRG, RH "squeeze/value" },
    { kLS, LH "thumbstick" },      { kRS, RH "thumbstick" },
    { kL3, LH "thumbstick/click" },{ kR3, RH "thumbstick/click" },
    { kX,  LH "a/click" },         { kY,  LH "b/click" },
    { kA,  RH "a/click" },         { kB,  RH "b/click" },
    { kStart, LH "trackpad/force" },{ kBack, RH "trackpad/force" },
};
// Original WMR controllers: no face buttons, so the trackpad clicks stand in
// for A (right) and B (left).
const Bind kWmr[] = {
    { kLT, LH "trigger/value" },   { kRT, RH "trigger/value" },
    { kLG, LH "squeeze/click" },   { kRG, RH "squeeze/click" },
    { kLS, LH "thumbstick" },      { kRS, RH "thumbstick" },
    { kL3, LH "thumbstick/click" },{ kR3, RH "thumbstick/click" },
    { kA,  RH "trackpad/click" },  { kB,  LH "trackpad/click" },
    { kStart, LH "menu/click" },   { kBack, RH "menu/click" },
};
// Vive wands: no sticks either; the trackpads are the sticks.
const Bind kVive[] = {
    { kLT, LH "trigger/value" },   { kRT, RH "trigger/value" },
    { kLG, LH "squeeze/click" },   { kRG, RH "squeeze/click" },
    { kLS, LH "trackpad" },        { kRS, RH "trackpad" },
    { kL3, LH "trackpad/click" },  { kR3, RH "trackpad/click" },
    { kStart, LH "menu/click" },   { kBack, RH "menu/click" },
};
#undef LH
#undef RH

XrPath to_path(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

bool suggest(const char* profile, const Bind* b, size_t n)
{
    std::vector<XrActionSuggestedBinding> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back({ g_act[b[i].id], to_path(b[i].path) });
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = to_path(profile);
    sb.countSuggestedBindings = (uint32_t)v.size();
    sb.suggestedBindings = v.data();
    const XrResult r = xrSuggestInteractionProfileBindings(g_instance, &sb);
    if (XR_FAILED(r)) {
        VRLOG("controllers: %s bindings rejected (%d)", profile, (int)r);
        return false;
    }
    return true;
}

// --- per-frame state -------------------------------------------------------

struct Raw {
    float lt = 0, rt = 0, lg = 0, rg = 0;
    XrVector2f ls{}, rs{};
    bool l3 = false, r3 = false, a = false, b = false, x = false, y = false;
    bool start = false, back = false;
};

float read_float(XrSession s, ActionId id, bool& active)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act[id];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive) {
        active = true;
        return st.currentState;
    }
    return 0.0f;
}

bool read_bool(XrSession s, ActionId id, bool& active)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act[id];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive) {
        active = true;
        return st.currentState != XR_FALSE;
    }
    return false;
}

XrVector2f read_vec(XrSession s, ActionId id, bool& active)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act[id];
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive) {
        active = true;
        return st.currentState;
    }
    return XrVector2f{0.0f, 0.0f};
}

SHORT to_axis(float v)
{
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return (SHORT)lroundf(v * 32767.0f);
}

BYTE to_trigger(float v)
{
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (BYTE)lroundf(v * 255.0f);
}

// A digital press from an analog value, with hysteresis so a grip held at the
// threshold does not chatter.
bool latch(bool was, float v) { return was ? v > 0.45f : v > 0.55f; }

// L3 AS A MODIFIER. How long a hold has to last before the game sees L3, how
// long a tap is shown to it, and where on the right stick a D-pad press begins.
constexpr ULONGLONG kL3HoldMs  = 200;
constexpr ULONGLONG kL3PulseMs = 100;

// THE MENU BUTTON IS START ON A TAP AND BACK ON A HOLD. A Quest controller has
// one menu button the game can have -- the right one belongs to the runtime --
// and nothing else that could be Back (Select / View). Released within
// kMenuHoldMs it reaches the game as Start, shown for kMenuPulseMs after the
// release; held that long it becomes Back for as long as it stays down, and
// no Start is sent at all. Start therefore arrives on release, not on press.
constexpr ULONGLONG kMenuHoldMs  = 500;
constexpr ULONGLONG kMenuPulseMs = 100;

XINPUT_GAMEPAD build_pad(const Raw& in)
{
    static bool lb = false, rb = false;
    static bool up = false, down = false, left = false, right = false;
    static bool l3Prev = false, l3Modifier = false;
    static ULONGLONG l3Down = 0, l3PulseUntil = 0;
    static bool menuPrev = false, menuWithL3 = false;
    static ULONGLONG menuDown = 0, startPulseUntil = 0;
    const ULONGLONG now = GetTickCount64();

    XINPUT_GAMEPAD p{};
    float rsx = in.rs.x, rsy = in.rs.y;
    const bool menu = in.start;
    bool start = false, back = in.back;

    // A press that BEGINS with L3 held is the L3 combination, Back at once;
    // any other press goes through the tap/hold rule above.
    if (menu && !menuPrev) { menuDown = now; menuWithL3 = in.l3; }

    if (in.l3 && !l3Prev) { l3Down = now; l3Modifier = false; }
    if (in.l3) {
        // The right stick becomes the D-pad and stops moving the camera; the
        // menu button becomes Back. Either use marks the hold as a modifier.
        up    = latch(up,    rsy);
        down  = latch(down, -rsy);
        right = latch(right, rsx);
        left  = latch(left, -rsx);
        if (up)    p.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (down)  p.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (left)  p.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (right) p.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (p.wButtons || menu) l3Modifier = true;
        rsx = rsy = 0.0f;
    } else {
        up = down = left = right = false;
    }

    if (menu) {
        if (menuWithL3 || now - menuDown >= kMenuHoldMs) back = true;
    } else if (menuPrev && !menuWithL3 && now - menuDown < kMenuHoldMs) {
        startPulseUntil = now + kMenuPulseMs;
    }
    if (now < startPulseUntil) start = true;
    menuPrev = menu;

    // What the game sees of L3 itself: a hold that touched nothing else, after
    // kL3HoldMs; a quick tap, as a short press after release; never a hold that
    // was used as the modifier.
    bool l3Out = false;
    if (in.l3)
        l3Out = !l3Modifier && now - l3Down >= kL3HoldMs;
    else if (l3Prev && !l3Modifier && now - l3Down < kL3HoldMs)
        l3PulseUntil = now + kL3PulseMs;
    if (now < l3PulseUntil) l3Out = true;
    l3Prev = in.l3;

    lb = latch(lb, in.lg);
    rb = latch(rb, in.rg);
    if (in.a)  p.wButtons |= XINPUT_GAMEPAD_A;
    if (in.b)  p.wButtons |= XINPUT_GAMEPAD_B;
    if (in.x)  p.wButtons |= XINPUT_GAMEPAD_X;
    if (in.y)  p.wButtons |= XINPUT_GAMEPAD_Y;
    if (lb)    p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (rb)    p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (l3Out) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (in.r3) p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (start) p.wButtons |= XINPUT_GAMEPAD_START;
    if (back)  p.wButtons |= XINPUT_GAMEPAD_BACK;

    p.bLeftTrigger  = to_trigger(in.lt);
    p.bRightTrigger = to_trigger(in.rt);
    p.sThumbLX = to_axis(in.ls.x);
    p.sThumbLY = to_axis(in.ls.y);
    p.sThumbRX = to_axis(rsx);
    p.sThumbRY = to_axis(rsy);
    return p;
}

// Published by poll() on the render thread, read through the XInput hook on
// whatever thread the game polls from.
SRWLOCK        g_lock = SRWLOCK_INIT;
XINPUT_GAMEPAD g_pad{};
bool           g_seen = false;      // a controller has delivered input this session
XINPUT_GAMEPAD g_lastOut{};
DWORD          g_packet = 1;

SHORT larger(SHORT a, SHORT b) { return std::abs((int)a) >= std::abs((int)b) ? a : b; }

} // namespace

bool wants_extension(const char* name) { return name && strcmp(name, kHpExtension) == 0; }

void note_extension_enabled(const char* name)
{
    if (wants_extension(name)) g_hpEnabled = true;
}

bool init(XrInstance instance, XrSession session)
{
    if (g_attached) return true;
    g_instance = instance;

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "gamepad");
    strcpy_s(si.localizedActionSetName, "Gamepad");
    if (XR_FAILED(xrCreateActionSet(instance, &si, &g_set))) {
        VRLOG("controllers: xrCreateActionSet failed -- no controller mapping");
        return false;
    }
    for (const ActionDef& d : kActions) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ai.actionName, d.name);
        strcpy_s(ai.localizedActionName, d.label);
        ai.actionType = d.type;
        if (XR_FAILED(xrCreateAction(g_set, &ai, &g_act[d.id]))) {
            VRLOG("controllers: xrCreateAction(%s) failed -- no controller mapping", d.name);
            xrDestroyActionSet(g_set);
            g_set = XR_NULL_HANDLE;
            return false;
        }
    }

    int accepted = 0;
    accepted += suggest("/interaction_profiles/oculus/touch_controller", kTouch, _countof(kTouch));
    accepted += suggest("/interaction_profiles/valve/index_controller",  kIndex, _countof(kIndex));
    accepted += suggest("/interaction_profiles/microsoft/motion_controller", kWmr, _countof(kWmr));
    accepted += suggest("/interaction_profiles/htc/vive_controller",     kVive,  _countof(kVive));
    if (g_hpEnabled)
        accepted += suggest("/interaction_profiles/hp/mixed_reality_controller", kHpG2, _countof(kHpG2));

    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &g_set;
    if (XR_FAILED(xrAttachSessionActionSets(session, &at))) {
        VRLOG("controllers: xrAttachSessionActionSets failed -- no controller mapping");
        xrDestroyActionSet(g_set);
        g_set = XR_NULL_HANDLE;
        return false;
    }
    g_attached = true;
    VRLOG("controllers: gamepad mapping ready, %d controller profile(s) bound%s",
          accepted, g_enabled.load() ? "" : " (switched OFF in the settings)");
    return true;
}

void poll(XrSession session, bool focused)
{
    if (!g_attached) return;

    Raw raw;
    bool active = false;
    if (focused) {
        XrActiveActionSet aas{g_set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &aas;
        if (XR_SUCCEEDED(xrSyncActions(session, &sync))) {
            raw.lt = read_float(session, kLT, active);
            raw.rt = read_float(session, kRT, active);
            raw.lg = read_float(session, kLG, active);
            raw.rg = read_float(session, kRG, active);
            raw.ls = read_vec(session, kLS, active);
            raw.rs = read_vec(session, kRS, active);
            raw.l3 = read_bool(session, kL3, active);
            raw.r3 = read_bool(session, kR3, active);
            raw.a  = read_bool(session, kA, active);
            raw.b  = read_bool(session, kB, active);
            raw.x  = read_bool(session, kX, active);
            raw.y  = read_bool(session, kY, active);
            raw.start = read_bool(session, kStart, active);
            raw.back  = read_bool(session, kBack, active);
        }
    }
    // Unfocused (the runtime's own menu is up, the headset is off): neutral.
    // Still "connected" once seen -- a pad that vanishes makes games pause.
    const XINPUT_GAMEPAD pad = build_pad(raw);

    AcquireSRWLockExclusive(&g_lock);
    const bool firstSight = active && !g_seen;
    g_pad = pad;
    if (active) g_seen = true;
    ReleaseSRWLockExclusive(&g_lock);
    if (firstSight)
        VRLOG("controllers: VR controllers active -- presented to the game as XInput pad 0");
}

void shutdown()
{
    AcquireSRWLockExclusive(&g_lock);
    g_seen = false;
    g_pad = XINPUT_GAMEPAD{};
    ReleaseSRWLockExclusive(&g_lock);
    if (g_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(g_set);   // destroys its actions with it
        g_set = XR_NULL_HANDLE;
    }
    g_attached = false;
}

bool merge_into(DWORD& result, XINPUT_STATE* state)
{
    if (!state || !g_enabled.load()) return false;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_seen) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const XINPUT_GAMEPAD vr = g_pad;
    XINPUT_GAMEPAD out = (result == ERROR_SUCCESS) ? state->Gamepad : XINPUT_GAMEPAD{};
    out.wButtons     |= vr.wButtons;
    out.bLeftTrigger  = (BYTE)(std::max)(out.bLeftTrigger,  vr.bLeftTrigger);
    out.bRightTrigger = (BYTE)(std::max)(out.bRightTrigger, vr.bRightTrigger);
    out.sThumbLX = larger(out.sThumbLX, vr.sThumbLX);
    out.sThumbLY = larger(out.sThumbLY, vr.sThumbLY);
    out.sThumbRX = larger(out.sThumbRX, vr.sThumbRX);
    out.sThumbRY = larger(out.sThumbRY, vr.sThumbRY);
    // A packet number that changes exactly when what we hand out changes --
    // games read an unchanged number as "no new input".
    if (memcmp(&out, &g_lastOut, sizeof(out)) != 0) {
        g_lastOut = out;
        ++g_packet;
    }
    state->Gamepad = out;
    state->dwPacketNumber = g_packet;
    ReleaseSRWLockExclusive(&g_lock);
    result = ERROR_SUCCESS;
    return true;
}

bool present()
{
    if (!g_enabled.load()) return false;
    AcquireSRWLockShared(&g_lock);
    const bool seen = g_seen;
    ReleaseSRWLockShared(&g_lock);
    return seen;
}

bool enabled() { return g_enabled.load(); }

void set_enabled(bool on)
{
    if (g_enabled.exchange(on) == on) return;
    VRLOG("controllers: VR controllers as gamepad -> %s", on ? "ON" : "OFF");
}

} // namespace xrinput

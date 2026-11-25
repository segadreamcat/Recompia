// SPDX-License-Identifier: GPL-3.0-or-later
// Tracked controller poses and pad actions, sampled only by the headset frame loop.
#include "xr/xr_internal.h"
#include "nxrt/config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
namespace nxrt::xr::detail {
// ---- actions ----------------------------------------------------------------------------------------------------------------------
XrPath path(const char* p) {
    XrPath out = XR_NULL_PATH;
    s().f.StringToPath(s().instance, p, &out);
    return out;
}

bool make_action(XrActionType type, const char* name, const char* label, XrAction* out) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    std::snprintf(ci.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(ci.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", label);
    return ok(s().f.CreateAction(s().set, &ci, out), "xrCreateAction");
}

bool make_actions() {
    State& st = s();
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(si.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "play");
    std::snprintf(si.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "Play");
    if (!ok(st.f.CreateActionSet(st.instance, &si, &st.set), "xrCreateActionSet")) return false;
    const struct { int i; const char* n; } bools[] = {{kA, "a"}, {kB, "b"}, {kX, "x"}, {kY, "y"}, {kMenu, "menu"}, {kLClick, "lclick"}, {kRClick, "rclick"}};
    for (auto& b : bools)
        if (!make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, b.n, b.n, &st.act_bool[b.i])) return false;
    const struct { int i; const char* n; } floats[] = {{kLTrig, "ltrig"}, {kRTrig, "rtrig"}, {kLGrip, "lgrip"}, {kRGrip, "rgrip"}};
    for (auto& fl : floats)
        if (!make_action(XR_ACTION_TYPE_FLOAT_INPUT, fl.n, fl.n, &st.act_float[fl.i])) return false;
    if (!make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "move", &st.act_vec[kLStick])) return false;
    if (!make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "turn", &st.act_vec[kRStick])) return false;

    for (int hand = 0; hand < 2; ++hand) {
        if (!make_action(XR_ACTION_TYPE_POSE_INPUT, hand ? "right_aim" : "left_aim", hand ? "Right aim" : "Left aim", &st.aim[hand])) return false;
        if (!make_action(XR_ACTION_TYPE_POSE_INPUT, hand ? "right_grip" : "left_grip", hand ? "Right grip" : "Left grip", &st.grip[hand])) return false;
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = st.aim[hand];
        ci.poseInActionSpace.orientation.w = 1.f;
        if (!ok(st.f.CreateActionSpace(st.session, &ci, &st.aim_space[hand]), "xrCreateActionSpace")) return false;
        if (!make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, hand ? "right_vibration" : "left_vibration", hand ? "Right vibration" : "Left vibration", &st.vibration[hand])) return false;
        ci.action = st.grip[hand];
        if (!ok(st.f.CreateActionSpace(st.session, &ci, &st.grip_space[hand]), "xrCreateActionSpace (grip)")) return false;
    }
    // Oculus Touch (Quest through Link, Air Link, Virtual Desktop or Steam Link)
    const XrActionSuggestedBinding touch[] = {
        {st.aim[0], path("/user/hand/left/input/aim/pose")},
        {st.aim[1], path("/user/hand/right/input/aim/pose")},
        {st.grip[0], path("/user/hand/left/input/grip/pose")},
        {st.grip[1], path("/user/hand/right/input/grip/pose")},
        {st.vibration[0], path("/user/hand/left/output/haptic")},
        {st.vibration[1], path("/user/hand/right/output/haptic")},
        {st.act_bool[kA], path("/user/hand/right/input/a/click")},
        {st.act_bool[kB], path("/user/hand/right/input/b/click")},
        {st.act_bool[kX], path("/user/hand/left/input/x/click")},
        {st.act_bool[kY], path("/user/hand/left/input/y/click")},
        {st.act_bool[kMenu], path("/user/hand/left/input/menu/click")},
        {st.act_bool[kLClick], path("/user/hand/left/input/thumbstick/click")},
        {st.act_bool[kRClick], path("/user/hand/right/input/thumbstick/click")},
        {st.act_float[kLTrig], path("/user/hand/left/input/trigger/value")},
        {st.act_float[kRTrig], path("/user/hand/right/input/trigger/value")},
        {st.act_float[kLGrip], path("/user/hand/left/input/squeeze/value")},
        {st.act_float[kRGrip], path("/user/hand/right/input/squeeze/value")},
        {st.act_vec[kLStick], path("/user/hand/left/input/thumbstick")},
        {st.act_vec[kRStick], path("/user/hand/right/input/thumbstick")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = touch;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof touch / sizeof touch[0]);
    ok(st.f.SuggestInteractionProfileBindings(st.instance, &sb), "xrSuggestInteractionProfileBindings (touch)");

    // Valve Index (and controllers SteamVR presents as Index): A/B on both hands, the left B as the menu
    const XrActionSuggestedBinding index[] = {
        {st.aim[0], path("/user/hand/left/input/aim/pose")},
        {st.aim[1], path("/user/hand/right/input/aim/pose")},
        {st.grip[0], path("/user/hand/left/input/grip/pose")},
        {st.grip[1], path("/user/hand/right/input/grip/pose")},
        {st.vibration[0], path("/user/hand/left/output/haptic")},
        {st.vibration[1], path("/user/hand/right/output/haptic")},
        {st.act_bool[kMenu], path("/user/hand/left/input/b/click")},
        {st.act_bool[kA], path("/user/hand/right/input/a/click")},
        {st.act_bool[kB], path("/user/hand/right/input/b/click")},
        {st.act_bool[kX], path("/user/hand/left/input/a/click")},
        {st.act_bool[kY], path("/user/hand/left/input/b/click")},
        {st.act_bool[kLClick], path("/user/hand/left/input/thumbstick/click")},
        {st.act_bool[kRClick], path("/user/hand/right/input/thumbstick/click")},
        {st.act_float[kLTrig], path("/user/hand/left/input/trigger/value")},
        {st.act_float[kRTrig], path("/user/hand/right/input/trigger/value")},
        {st.act_float[kLGrip], path("/user/hand/left/input/squeeze/value")},
        {st.act_float[kRGrip], path("/user/hand/right/input/squeeze/value")},
        {st.act_vec[kLStick], path("/user/hand/left/input/thumbstick")},
        {st.act_vec[kRStick], path("/user/hand/right/input/thumbstick")},
    };
    sb.interactionProfile = path("/interaction_profiles/valve/index_controller");
    sb.suggestedBindings = index;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof index / sizeof index[0]);
    ok(st.f.SuggestInteractionProfileBindings(st.instance, &sb), "xrSuggestInteractionProfileBindings (index)");

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &st.set;
    return ok(st.f.AttachSessionActionSets(st.session, &ai), "xrAttachSessionActionSets");
}

int32_t to_stick(float v) { return static_cast<int32_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f)); }

void read_actions() {
    State& st = s();
    XrActiveActionSet as{st.set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (!st.actions_ready || st.f.SyncActions(st.session, &si) != XR_SUCCESS) { clear_pad(); return; }
    auto b = [&](int i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.act_bool[i];
        XrActionStateBoolean v{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(st.f.GetActionStateBoolean(st.session, &gi, &v)) && v.isActive && v.currentState;
    };
    auto fl = [&](int i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.act_float[i];
        XrActionStateFloat v{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(st.f.GetActionStateFloat(st.session, &gi, &v)) && v.isActive ? v.currentState : 0.f;
    };
    auto vec = [&](int i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.act_vec[i];
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(st.f.GetActionStateVector2f(st.session, &gi, &v)) && v.isActive) return v.currentState;
        return XrVector2f{0, 0};
    };
    for (int h = 0; h < 2; ++h) {
        st.triggers[h].store(fl(h ? kRTrig : kLTrig), std::memory_order_relaxed);
        st.squeezes[h].store(fl(h ? kRGrip : kLGrip), std::memory_order_relaxed);
    }
    for (int h = 0; h < 2; ++h) {
        const float amplitude = st.haptic_amplitude[h].exchange(0);
        if (amplitude > 0 && st.vibration[h]) {
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO}; info.action = st.vibration[h];
            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION}; vibration.amplitude = amplitude;
            vibration.duration = static_cast<XrDuration>(st.haptic_seconds[h].load() * 1e9); vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
            st.f.ApplyHapticFeedback(st.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        }
    }
    uint64_t btns = 0;
    if (b(kA)) btns |= hid::btn(hid::kBtnA);
    if (b(kB)) btns |= hid::btn(hid::kBtnB);
    if (b(kX)) btns |= hid::btn(hid::kBtnX);
    if (b(kY)) btns |= hid::btn(hid::kBtnY);
    if (b(kMenu)) btns |= hid::btn(hid::kBtnPlus);
    const bool lc = b(kLClick), rc = b(kRClick);
    if (lc && rc) btns |= hid::btn(hid::kBtnMinus);
    else if (rc && !st.shortcuts_enabled.load()) btns |= hid::btn(hid::kBtnStickR);
    if (fl(kLTrig) > 0.5f) btns |= hid::btn(hid::kBtnZL);
    if (fl(kRTrig) > 0.5f) btns |= hid::btn(hid::kBtnZR);
    if (fl(kLGrip) > 0.5f) btns |= hid::btn(hid::kBtnL);
    if (fl(kRGrip) > 0.5f) btns |= hid::btn(hid::kBtnR);
    XrVector2f l = vec(kLStick), r = vec(kRStick);
    const uint64_t now = qpc::now_ns();
    if (lc && !st.left_click_was) { st.left_click_ns = now; st.left_click_used = false; }
    if (rc && !st.right_click_was) { st.right_click_ns = now; st.right_click_used = false; }
    if (lc && (rc || std::fabs(l.x) > .5f || std::fabs(l.y) > .5f)) st.left_click_used = true;
    if (lc && rc) st.right_click_used = true;
    if (st.shortcuts_enabled.load(std::memory_order_relaxed)) {
        if (!lc && st.left_click_was && !st.left_click_used && now-st.left_click_ns < 450000000ull)
            st.commands.fetch_or(kScreenshot, std::memory_order_relaxed);
        if (!rc && st.right_click_was && !st.right_click_used && now-st.right_click_ns < 450000000ull)
            st.commands.fetch_or(kTogglePerspective, std::memory_order_relaxed);
    }
    st.left_click_was = lc; st.right_click_was = rc;
    if (lc && !rc) {
        if (l.x < -0.5f) btns |= hid::btn(hid::kBtnLeft);
        if (l.x >  0.5f) btns |= hid::btn(hid::kBtnRight);
        if (l.y < -0.5f) btns |= hid::btn(hid::kBtnDown);
        if (l.y >  0.5f) btns |= hid::btn(hid::kBtnUp);
        l = {0, 0};
    }
    // Round dead zone and outer edge: tilt is measured as a whole (diagonals keep their length), and anything past [vr] stick_full of the
    // way out counts as a full push. Touch sticks seldom report 1.0, and the game picks running or walking from the stick's length, so
    // without the edge a full push sits on that threshold and the character keeps switching between the two.
    static const float full = std::clamp(static_cast<float>(nxrt::config_get_double("vr", "stick_full", 0.85)), 0.5f, 1.0f);
    auto radial = [](XrVector2f v) {
        constexpr float inner = 0.15f;
        const float len = std::sqrt(v.x * v.x + v.y * v.y);
        if (len <= inner) return XrVector2f{0, 0};
        const float out = std::min((len - inner) / (full - inner), 1.f);
        return XrVector2f{v.x / len * out, v.y / len * out};
    };
    l = radial(l);
    r = radial(r);
    st.pad_buttons.store(btns, std::memory_order_relaxed);
    st.pad_lx.store(to_stick(l.x), std::memory_order_relaxed);
    st.pad_ly.store(to_stick(l.y), std::memory_order_relaxed);
    st.pad_rx.store(to_stick(r.x), std::memory_order_relaxed);
    st.pad_ry.store(to_stick(r.y), std::memory_order_relaxed);
}

void clear_pad() {
    State& st = s();
    st.left_click_was = st.right_click_was = false;
    st.commands.store(0);
    for (int h = 0; h < 2; ++h) { st.triggers[h].store(0); st.squeezes[h].store(0); st.haptic_amplitude[h].store(0); }
    st.pad_buttons.store(0);
    st.pad_lx.store(0), st.pad_ly.store(0), st.pad_rx.store(0), st.pad_ry.store(0);
}


} // namespace nxrt::xr::detail

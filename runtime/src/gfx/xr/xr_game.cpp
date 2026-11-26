// SPDX-License-Identifier: GPL-3.0-or-later
// Camera-driven eye selection and durable metadata. Presentation may repeat or outnumber actual camera updates.
#include "xr/xr_internal.h"
#include <algorithm>
#include <cstring>
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
namespace nxrt::xr {
using namespace detail;
EyeView eye_view() {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    if (!st.running.load() || !st.world_mode.load() || !st.tracking[0].valid || !st.tracking[1].valid) return {};
    if (st.cur_eye != 0) {
        st.cur_eye = 0;
        const uint64_t frame = ++st.next_id;
        for (int e = 0; e < 2; ++e) {
            st.cur_views[e] = st.tracking[e]; st.cur_views[e].frame = frame;
            st.cur_hands[e] = st.hands[e];
        }
    } else st.cur_eye = 1;
    return st.cur_views[st.cur_eye];
}

Tag game_present() {
    State& st = s();
    if (!active()) return {};
    std::lock_guard<std::mutex> lk(st.gm);
    Tag now = st.camera;
    if (!st.world_mode.load() || !now.world) { now = {}; now.eye = 0; }
    return now;
}

void set_world_active(bool active) {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    if (st.world_mode.exchange(active) != active) {
        st.cur_eye = -1; st.camera = {};
        for (auto& camera : st.cameras) camera = {};
        st.camera_sequence = 0;
        NXLOG_I(kTag, "camera mode: %s", active ? "world" : "panel");
    }
}

void note_world_view(const EyeView& view) {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    st.camera = {};
    st.camera.eye = view.eye; st.camera.frame = view.frame; st.camera.view = view;
    st.camera.world = view.valid && st.world_mode.load();
}

void note_rendered_fov(float left, float right, float up, float down) {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    if (!st.camera.world) return;
    st.camera.fov[0] = left; st.camera.fov[1] = right; st.camera.fov[2] = up; st.camera.fov[3] = down;
    st.camera.fov_ok = true;
}

EyeView hand_aim() {
    std::lock_guard<std::mutex> lk(s().gm);
    return s().running.load() ? s().cur_hands[1] : EyeView{};
}
EyeView head_view() { std::lock_guard<std::mutex> lock(s().gm); return running() ? s().tracking[0] : EyeView{}; }
HandView hand_view(int hand) {
    State& st = s();
    if (hand < 0 || hand > 1 || !running()) return {};
    std::lock_guard<std::mutex> lk(st.gm);
    return {st.hands[hand], st.grips[hand], st.triggers[hand].load(), st.squeezes[hand].load()};
}
void hold_panel(int hand, float width_m) {
    State& st = s();
    st.panel_hand_width.store(std::clamp(width_m, .08f, 1.5f));
    st.panel_hand_ns.store(qpc::now_ns());
    st.panel_hand.store(hand < 0 || hand > 1 ? -1 : hand);
}
void haptic(int hand, float amplitude, float seconds) {
    if(hand < 0 || hand > 1 || !running()) return;
    s().haptic_seconds[hand].store(std::clamp(seconds, .005f, .25f));
    s().haptic_amplitude[hand].store(std::clamp(amplitude, 0.f, 1.f));
}
void spatial_input(uint64_t buttons, uint64_t capture, uint64_t until_ns) {
    std::lock_guard<std::mutex> lock(s().gm);
    s().spatial_buttons.store(buttons); s().spatial_capture.store(capture); s().spatial_until.store(until_ns);
}
void set_shortcuts_enabled(bool enabled) { s().shortcuts_enabled.store(enabled, std::memory_order_relaxed); }
void set_sharpness(float amount) { s().sharpness.store(std::clamp(amount, 0.f, 1.f), std::memory_order_relaxed); }
uint32_t take_commands() { return s().commands.exchange(0, std::memory_order_relaxed); }
Pad pad() {
    State& st = s(); Pad p;
    if (!running()) return p;
    p.active = true; p.buttons = st.pad_buttons.load();
    { std::lock_guard<std::mutex> lock(st.gm);
      if (qpc::now_ns() < st.spatial_until.load()) p.buttons = (p.buttons & ~st.spatial_capture.load()) | st.spatial_buttons.load(); }
    p.lx = st.pad_lx.load(); p.ly = st.pad_ly.load(); p.rx = st.pad_rx.load(); p.ry = st.pad_ry.load();
    return p;
}
} // namespace nxrt::xr

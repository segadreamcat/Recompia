// SPDX-License-Identifier: GPL-3.0-or-later
// Camera-driven eye selection and durable metadata. Presentation may repeat or outnumber actual camera updates.
#include "xr/xr_internal.h"
#include <algorithm>
#include <cstring>
#include "nxrt/config.h"
#include "nxrt/log.h"
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
Pad pad() {
    State& st = s(); Pad p;
    if (!running()) return p;
    p.active = true; p.buttons = st.pad_buttons.load();
    p.lx = st.pad_lx.load(); p.ly = st.pad_ly.load(); p.rx = st.pad_rx.load(); p.ry = st.pad_ry.load();
    return p;
}
} // namespace nxrt::xr

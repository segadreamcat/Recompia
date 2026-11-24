// SPDX-License-Identifier: GPL-3.0-or-later
// Game-frame eye alternation. Presents own their tracking snapshots, including across the engine's camera-to-picture delay.
#include "xr/xr_internal.h"
#include <algorithm>
#include <cstring>
#include "nxrt/config.h"
namespace nxrt::xr {
using namespace detail;
// ---- the game thread -------------------------------------------------------------------------------------------------------------
EyeView eye_view() {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    if (st.cur_eye < 0 || !st.running.load(std::memory_order_relaxed)) return {};
    return st.cur_views[st.cur_eye];
}

// The game's picture lags its camera by [vr] frame_lag presents (the engine records a frame's draws while the next one is simulated):
// the present that shows an eye is the one frame_lag presents after the frame whose camera placed it.
int frame_lag() {
    static const int v = static_cast<int>(std::clamp<int64_t>(nxrt::config_get_int("vr", "frame_lag", 1), 0, 3));
    return v;
}

Tag game_present() {
    State& st = s();
    if (!active()) return {};
    std::lock_guard<std::mutex> lk(st.gm);
    Tag now;   // what this frame's camera did
    now.eye = st.cur_eye;
    now.frame = st.cur_frame;
    if (st.cur_eye >= 0) now.view = st.cur_views[st.cur_eye];
    now.world = st.world_this_frame;
    now.fov_ok = st.fov_this_frame;
    std::memcpy(now.fov, st.fov_frame, sizeof now.fov);
    st.fov_this_frame = false;
    st.world_this_frame = false;
    // the picture presented now is the camera of frame_lag frames ago
    for (int i = State::kHist - 1; i > 0; --i) st.hist[i] = st.hist[i - 1];
    st.hist[0] = now;
    const Tag t = st.hist[frame_lag()];
    // The next game frame uses the other eye, or snapshots the newest tracking for a new pair without waiting.
    if (st.cur_eye == 0) {
        st.cur_eye = 1;
        st.wait_due = false;
    } else {
        st.cur_eye = -1;
        st.wait_due = true;
    }
    return t;
}

void game_present_done() {
    State& st = s();
    if (!active()) return;
    {
        std::lock_guard<std::mutex> lk(st.gm);
        if (!st.wait_due) return;
        st.wait_due = false;
    }
    std::lock_guard<std::mutex> lk(st.gm);
    if (!st.running.load() || !st.tracking[0].valid || !st.tracking[1].valid) { st.cur_eye = -1; return; }
    st.cur_frame = ++st.next_id;
    st.cur_eye = 0;
    for (int e = 0; e < 2; ++e) {
        st.cur_views[e] = st.tracking[e];
        st.cur_views[e].frame = st.cur_frame;
        st.cur_hands[e] = st.hands[e];
    }
}

void note_rendered_fov(float tan_left, float tan_right, float tan_up, float tan_down) {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    st.fov_frame[0] = tan_left, st.fov_frame[1] = tan_right, st.fov_frame[2] = tan_up, st.fov_frame[3] = tan_down;
    st.fov_this_frame = true;
}

void note_world_view() { std::lock_guard<std::mutex> lk(s().gm); s().world_this_frame = true; }

EyeView hand_aim() {
    std::lock_guard<std::mutex> lk(s().gm);
    return s().running.load() ? s().cur_hands[1] : EyeView{};
}

Pad pad() {
    State& st = s();
    Pad p;
    if (!running()) return p;
    p.active = true;
    p.buttons = st.pad_buttons.load(std::memory_order_relaxed);
    p.lx = st.pad_lx.load(std::memory_order_relaxed);
    p.ly = st.pad_ly.load(std::memory_order_relaxed);
    p.rx = st.pad_rx.load(std::memory_order_relaxed);
    p.ry = st.pad_ry.load(std::memory_order_relaxed);
    return p;
}


} // namespace nxrt::xr

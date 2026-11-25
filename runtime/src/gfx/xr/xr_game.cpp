// SPDX-License-Identifier: GPL-3.0-or-later
// Camera-driven eye selection and durable metadata. Presentation may repeat or outnumber actual camera updates.
#include "xr/xr_internal.h"
#include <algorithm>
#include <cstring>
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/key_state.h"
#include <cmath>
namespace nxrt::xr {
using namespace detail;
EyeView eye_view() {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    for(int h=0; h<2; ++h){st.tracking[h]=EyeView{};st.tracking[h].valid=true;st.tracking[h].pos[0]=h?.032f:-.032f;st.tracking[h].pos[1]=1.65f;st.tracking[h].quat[0]=-.130526f;st.tracking[h].quat[3]=.991445f;st.tracking[h].center[1]=1.65f;st.tracking[h].tan_left=-1.43f;st.tracking[h].tan_right=1.43f;st.tracking[h].tan_up=1.43f;st.tracking[h].tan_down=-1.43f;st.tracking[h].eye=h;}
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
EyeView head_view() { EyeView h;h.valid=true;h.pos[1]=h.center[1]=1.65f;h.quat[0]=-.130526f;h.quat[3]=.991445f;return h; }
HandView hand_view(int hand) {
    HandView h;h.grip.valid=h.aim.valid=true;h.grip.pos[0]=hand?.20f:-.18f;h.grip.pos[1]=1.34f;h.grip.pos[2]=-.42f;
    if(!hand){h.grip.quat[0]=.70710678f;h.grip.quat[2]=-.70710678f;h.grip.quat[3]=0;}else{for(float& v:h.grip.quat)v=.5f;}
    if(hand){
      static uint64_t sw=0,wh=0;uint64_t now=qpc::now_ns();
      if(key_state::down_by_name("F7")){if(!sw)sw=now;float t=static_cast<float>((now-sw)*1e-9);h.trigger=.8f;h.grip.pos[0]=-.215f+std::min(t,.8f)*.15f;h.grip.pos[1]=1.27f;h.grip.pos[2]=-.26f;}else sw=0;
      if(key_state::down_by_name("F8")){if(!wh)wh=now;float t=static_cast<float>((now-wh)*1e-9);h.squeeze=1;h.grip.pos[2]=-.08f-std::clamp((t-.3f)*2.f,0.f,.5f);}else wh=0;
    }
    h.aim=h.grip;h.aim.quat[0]=-.1f;h.aim.quat[1]=h.aim.quat[2]=0;h.aim.quat[3]=.994987f;return h;
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
    static thread_local uint64_t prior=0; if(p.buttons!=prior){NXLOG_I("vr","diagnostic pad 0x%llx",static_cast<unsigned long long>(p.buttons));prior=p.buttons;}
    p.lx = st.pad_lx.load(); p.ly = st.pad_ly.load(); p.rx = st.pad_rx.load(); p.ry = st.pad_ry.load();
    return p;
}
} // namespace nxrt::xr

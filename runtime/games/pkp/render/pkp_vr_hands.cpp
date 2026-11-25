// SPDX-License-Identifier: GPL-3.0-or-later
// First-person view models and wrist interaction. Native character animation
// and gameplay remain authoritative.
#include "render/pkp_vr_hands.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "input/pkp_play.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/xr.h"
#include "nxrt/ui_layer.h"
#include "ui/pkp_menu.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
namespace nxrt::games::pkp {
namespace {
constexpr float kPi = 3.14159265359f;
struct V {
    float x = 0, y = 0, z = 0;
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V a) { return std::sqrt(dot(a, a)); }
V norm(V a) {
    float l = length(a);
    return l > .0001f ? a * (1 / l) : V{0, 1, 0};
}
V vec(const float *p) { return {p[0], p[1], p[2]}; }
V rotate(const float *q, V v) {
    V u{q[0], q[1], q[2]};
    V t = cross(u, v) * 2;
    return v + t * q[3] + cross(u, t);
}
struct Color {
    float r, g, b;
};
constexpr Color purple{.60f, .25f, .82f}, green{.14f, .65f, .19f}, black{.035f, .04f, .06f};
struct State {
    uint64_t base = 0, time = 0, epoch = 0, pulse_until = 0, pulse = 0, cooldown = 0, cast_start = 0, vine_start = 0;
    std::atomic<bool> playing{false};
    bool touching = false, swiped = false, whip_armed = false;
    uint64_t grip_start = 0;
    bool grip_was = false, grip_cast = false;
    float swipe_x = 0, pull = 0;
    V previous, vine_origin, vine_direction;
    uint16_t action = 0;
    xr::SpatialScene scene;
} g;
void triangle(V a, V b, V c, Color color, float picture = 0, V ua = {}, V ub = {}, V uc = {}) {
    auto &s = g.scene;
    if (s.count + 3 > xr::SpatialScene::kMaxVertices)
        return;
    const V p[] = {a, b, c}, uv[] = {ua, ub, uc};
    for (int i = 0; i < 3; ++i) {
        auto &v = s.vertices[s.count++];
        v = {{p[i].x, p[i].y, p[i].z}, {color.r, color.g, color.b, 1}, {uv[i].x, uv[i].y}, picture};
    }
}
void tube(V a, V b, float ra, float rb, Color color, bool cap = true) {
    const V dir = norm(b - a), u = norm(cross(dir, std::fabs(dir.y) < .9f ? V{0, 1, 0} : V{1, 0, 0})), v = cross(dir, u);
    for (int i = 0; i < 10; ++i) {
        float t0 = 2 * kPi * i / 10, t1 = 2 * kPi * (i + 1) / 10;
        V n0 = u * std::cos(t0) + v * std::sin(t0), n1 = u * std::cos(t1) + v * std::sin(t1);
        const float light = .72f + .28f * std::max(0.f, dot(n0, norm(V{-.4f, 1, .3f})));
        Color shade{color.r * light, color.g * light, color.b * light};
        triangle(a + n0 * ra, b + n0 * rb, b + n1 * rb, shade);
        triangle(a + n0 * ra, b + n1 * rb, a + n1 * ra, shade);
        if(cap) triangle(b, b + n1 * rb, b + n0 * rb, shade);
    }
}
void ball(const xr::EyeView &pose, V center, V radius, Color color) {
    auto point = [&](int ring, int side) {
        float p = kPi * ring / 6, t = 2 * kPi * side / 10;
        return vec(pose.pos) + rotate(pose.quat, center + V{radius.x * std::sin(p) * std::cos(t), radius.y * std::cos(p),
                                                            radius.z * std::sin(p) * std::sin(t)});
    };
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x) {
            Color c = color;
            float f = .75f + .25f * (1.f - y / 6.f);
            c.r *= f;
            c.g *= f;
            c.b *= f;
            triangle(point(y, x), point(y + 1, x), point(y + 1, x + 1), c);
            triangle(point(y, x), point(y + 1, x + 1), point(y, x + 1), c);
        }
}
void hand(const xr::HandView &h, int side, const xr::EyeView &head) {
    auto pose = h.grip;
    // OpenXR grip X is the palm normal, Y runs toward the wrist, and -Z runs from little finger toward thumb.
    // Orient the view model's finger/forearm axis to grip Y; the back of the left hand is grip -X.
    const float bone[4] = {-.5f, side ? -.5f : .5f, side ? -.5f : .5f, .5f};
    const float* q=h.grip.quat;
    pose.quat[0]=q[3]*bone[0]+q[0]*bone[3]+q[1]*bone[2]-q[2]*bone[1];
    pose.quat[1]=q[3]*bone[1]-q[0]*bone[2]+q[1]*bone[3]+q[2]*bone[0];
    pose.quat[2]=q[3]*bone[2]+q[0]*bone[1]-q[1]*bone[0]+q[2]*bone[3];
    pose.quat[3]=q[3]*bone[3]-q[0]*bone[0]-q[1]*bone[1]-q[2]*bone[2];
    const float sign = side ? 1.f : -1.f;
    V head_forward = rotate(head.quat, {0, 0, -1});
    head_forward.y = 0;
    head_forward = norm(head_forward);
    const V right{-head_forward.z, 0, head_forward.x};
    const V shoulder = vec(head.center) + right * (sign * .19f) + V{0, -.24f, 0} + head_forward * -.07f;
    const V wrist = vec(pose.pos) + rotate(h.grip.quat, {0, .065f, 0});
    V elbow = (shoulder + wrist) * .5f + right * (sign * .09f) + V{0, -.13f, .05f};
    // Soft arms stretch with room-scale reach; no movement or collision is
    // applied to the native character.
    tube(shoulder, elbow, .052f, .043f, purple);
    tube(elbow, wrist, .043f, .033f, purple);
    ball(pose, {0, -.008f, -.015f}, {.042f, .026f, .058f}, purple);
    for (int i = 0; i < 4; ++i) {
        float curl = std::clamp(i == 0 ? h.trigger : h.squeeze, 0.f, 1.f);
        float x = (-.027f + i * .018f) * sign;
        ball(pose, {x, -.009f - .019f * curl, -.071f + .025f * curl}, {.011f, .012f, .030f - .010f * curl}, purple);
    }
    ball(pose, {-sign * .043f, -.013f, -.026f}, {.017f, .018f, .029f}, purple);
}
struct Watch {
    V center, right, up, normal;
};
Watch watch_pose(const xr::EyeView &p) {
    return {vec(p.pos) + rotate(p.quat, {-.045f, .10f, 0}), rotate(p.quat, {0, 0, -1}), rotate(p.quat, {0, -1, 0}), rotate(p.quat, {-1, 0, 0})};
}
void watch(const Watch &w, bool touched) {
    const V center = w.center;
    const float radius = .073f;
    tube(center - w.normal * .018f, center, radius * .94f, radius, black, false);
    for(int i=0;i<36;++i){
        float a=2*kPi*i/36,b=2*kPi*(i+1)/36;
        V p=w.right*std::cos(a)+w.up*std::sin(a),q=w.right*std::cos(b)+w.up*std::sin(b);
        triangle(center+p*radius,center+q*radius,center+q*(radius*.91f),black);
        triangle(center+p*radius,center+q*(radius*.91f),center+p*(radius*.91f),black);
    }
    const Color white{1, 1, 1};
    // Native move wheel, including its icon, name and selection animation, on a
    // circular wrist face.
    for (int i = 0; i < 36; ++i) {
        float a = 2 * kPi * i / 36, b = 2 * kPi * (i + 1) / 36;
        V p = w.right * std::cos(a) + w.up * std::sin(a), q = w.right * std::cos(b) + w.up * std::sin(b);
        triangle(center + w.normal * .001f, center + p * (radius * .91f) + w.normal * .001f,
                 center + q * (radius * .91f) + w.normal * .001f, white, 1, {.5f, .5f, 0},
                 {.5f + .5f * std::cos(a), .5f - .5f * std::sin(a), 0}, {.5f + .5f * std::cos(b), .5f - .5f * std::sin(b), 0});
    }
    if (touched)
        ball(xr::EyeView{}, {center.x, center.y, center.z}, {.006f, .006f, .006f}, green);
}
void pulse(uint64_t buttons, uint64_t now) {
    g.pulse = buttons;
    g.pulse_until = now + 120000000ull;
}
void reset_interaction() {
    g.touching = g.swiped = g.whip_armed = false;
    g.time = g.pulse_until = g.grip_start = 0;
    g.grip_was = g.grip_cast = false;
    xr::spatial_input(0, 0, 0);
}
void frame(bool playing, double, play::GameFrame *) { g.playing = playing; }
void game_frame(::Ctx *) {
    if (!xr::wanted())
        return;
    const uint64_t now = qpc::now_ns();
    g.scene.count = 0;
    std::fill(std::begin(g.scene.picture_rect), std::end(g.scene.picture_rect), 0.f);
    const auto head = xr::head_view();
    const auto left = xr::hand_view(0), right = xr::hand_view(1);
    const bool active =
        xr::running() && head.valid && g.playing && settings::get_bool(settings::Key::VrFirstPerson) && !menu_has_selection();
    if (!active) {
        reset_interaction();
        xr::set_spatial_scene(g.scene);
        ui_layer::set_background_region({});
        return;
    }
    if (head.reference_epoch != g.epoch) {
        g.epoch = head.reference_epoch;
        reset_interaction();
        g.vine_start = 0;
    }
    if (settings::get_bool(settings::Key::VrHands)) {
        if (left.grip.valid)
            hand(left, 0, head);
        if (right.grip.valid)
            hand(right, 1, head);
    }
    const bool wrist = settings::get_bool(settings::Key::VrWrist) && left.grip.valid;
    bool within_reach = false;
    float x = 0;
    Watch w{};
    if (wrist) {
        w = watch_pose(left.grip);
        if (right.grip.valid) {
            V finger = vec(right.grip.pos) + rotate(right.grip.quat, {.005f, -.10f, -.025f});
            V delta = finger - w.center;
            x = dot(delta, w.right);
            within_reach = std::fabs(x) < .14f && std::fabs(dot(delta, w.up)) < .09f && std::fabs(dot(delta, w.normal)) < .085f &&
                           dot(w.normal, norm(vec(head.center) - w.center)) > .15f;
        }
        const bool pressed = right.trigger > .35f;
        if (within_reach && pressed && !g.touching) {
            g.touching = true;
            g.swiped = false;
            g.swipe_x = x;
        }
        if (!pressed || !right.grip.valid || !within_reach) {
            g.touching = false;
            g.swiped = false;
        }
        if (g.touching && !g.swiped && std::fabs(x - g.swipe_x) > .045f) {
            pulse(hid::btn(x > g.swipe_x ? hid::kBtnRight : hid::kBtnLeft), now);
            g.swiped = true;
            g.cooldown = now + 500000000ull;
            xr::haptic(1, .10f, .02f);
            NXLOG_I("vr", "wrist swipe: %s", x > g.swipe_x ? "next move" : "previous move");
        }
        watch(w, g.touching);
        // Native 16:9 HUD wheel bounds in the square render, transformed by the
        // user's whole-HUD inset.
        const float scale = static_cast<float>(settings::get_double(settings::Key::VrHudScale));
        const float native[] = {.80f, .65f, .19f, .16f};
        g.scene.picture_rect[0] = .5f + (native[0] - .5f) * scale;
        g.scene.picture_rect[1] = .5f + (native[1] - .5f) * scale;
        g.scene.picture_rect[2] = native[2] * scale;
        g.scene.picture_rect[3] = native[3] * scale;
    } else {
        g.touching = g.swiped = false;
    }
    uint64_t capture = g.touching ? hid::btn(hid::kBtnZR) : 0;
    const uint64_t manager = guest::deref(g.base + layout::kPlayerManager), player = guest::deref(manager + layout::kManagerPlayer),
                   component = guest::deref(player + layout::kAction);
    const uint16_t action = guest::ok(component, layout::kActionId + 2) ? guest::rd<uint16_t>(component + layout::kActionId) : 0;
    if (action == 878 && g.action != 878 && right.aim.valid) {
        g.vine_start = now;
        g.vine_origin = vec(right.grip.valid ? right.grip.pos : right.aim.pos);
        g.vine_direction = rotate(right.aim.quat, {0, 0, -1});
    }
    if(g.action!=action) NXLOG_I("vr","diagnostic native action %u",action);
    g.action = action;
    if (g.vine_start && now - g.vine_start < 700000000ull) {
        float t = static_cast<float>((now - g.vine_start) * 1e-9);
        float reach = 1.8f * std::sin(kPi * std::min(t / .7f, 1.f));
        V last = g.vine_origin;
        for (int i = 1; i <= 14; ++i) {
            float u = i / 14.f;
            V p = g.vine_origin + g.vine_direction * (reach * u) +
                  V{.06f * std::sin(u * 12 - t * 18) * u, -.16f * u * u, .025f * std::cos(u * 10) * u};
            tube(last, p, .014f * (1 - u * .65f), .014f * (1 - u * .7f), green);
            last = p;
        }
    }
    const bool whip_controls = settings::get_bool(settings::Key::VrWhipCast) && play_camera_free() && right.grip.valid && !g.touching;
    if (whip_controls) {
        capture |= hid::btn(hid::kBtnR);
        const bool held = right.squeeze > .65f;
        if(held && !g.grip_was) { g.grip_start = now; g.grip_cast = false; }
        if(!held && g.grip_was && !g.grip_cast && now-g.grip_start<300000000ull) pulse(hid::btn(hid::kBtnR), now);
        g.grip_was=held;
    } else { g.grip_was=false; }
    if (right.grip.valid) {
        const V relative = vec(right.grip.pos) - vec(head.center);
        V forward = rotate(head.quat, {0, 0, -1});
        forward.y = 0;
        forward = norm(forward);
        const float reach = dot(relative, forward);
        const double dt = g.time ? static_cast<double>(now - g.time) * 1e-9 : 0;
        const bool allowed =
            whip_controls && now >= g.cooldown && right.squeeze > .65f;
        if (!allowed) {
            g.whip_armed = false;
            g.cast_start = 0;
        } else {
            if (!g.whip_armed && reach < .16f) {
                g.whip_armed = true;
                g.pull = reach;
                g.cast_start = now;
            }
            if (g.whip_armed) {
                g.pull = std::min(g.pull, reach);
                const float speed = dt > .003 && dt < .1 ? dot(relative - g.previous, forward) / static_cast<float>(dt) : 0;
                if (now - g.cast_start > 1200000000ull)
                    g.whip_armed = false;
                else if (reach - g.pull > .20f && speed > 1.6f) {
                    pulse(hid::btn(hid::kBtnZR), now);
                    g.cooldown = now + 900000000ull;
                    g.whip_armed = false;
                    g.grip_cast = true; xr::haptic(1, .18f, .03f);
                    NXLOG_I("vr", "gesture cast: forward whip");
                }
            }
        }
        g.previous = relative;
        g.time = now;
    } else {
        g.time = 0;
        g.whip_armed = false;
    }
    xr::spatial_input(now < g.pulse_until ? g.pulse : 0, capture, now + 150000000ull);
    xr::set_spatial_scene(g.scene);
    const float* r=g.scene.picture_rect;
    const float margin=r[2]>0?.003f:0;
    ui_layer::set_background_region({r[0]-margin,r[1]-margin,r[2]+2*margin,r[3]+2*margin});
}
void install(const FeatureInstall &in) { g.base = in.base; }
void remove() {
    g.scene.count = 0;
    xr::set_spatial_scene(g.scene);
    ui_layer::set_background_region({});
    reset_interaction();
    g.playing = false;
    g.vine_start = 0;
}
} // namespace
const Feature kVrHandsFeature = {.name = "vr_hands", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};
} // namespace nxrt::games::pkp

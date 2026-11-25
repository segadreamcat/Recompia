// SPDX-License-Identifier: GPL-3.0-or-later
// First-person view models and wrist interaction. Native character animation
// and gameplay remain authoritative.
#include "render/pkp_vr_hands.h"
#include "render/pkp_vr_glove_mesh.inc"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "input/pkp_play.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/config.h"
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
constexpr Color green{.14f, .65f, .19f}, black{.035f, .04f, .06f};
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
// The character's purple, as the game draws it (sRGB 184, 134, 212), in the linear space the eye images are written in.
constexpr Color kDitto{.479f, .238f, .658f};
float vr_value(const char* key, double fallback, double lo, double hi) {
    return static_cast<float>(std::clamp(nxrt::config_get_double("vr", key, fallback), lo, hi));
}
// [vr] hand_size: wrist to fingertip in metres. hand_roll: degrees the thumbs turn up from palm-down (a relaxed controller grip).
// hand_pitch: degrees the fingers tip down from the pointing ray.
float hand_size() { static const float v = vr_value("hand_size", 0.19, 0.08, 0.40); return v; }
float hand_roll() { static const float v = vr_value("hand_roll", 45.0, -90.0, 90.0) * kPi / 180.f; return v; }
float hand_pitch() { static const float v = vr_value("hand_pitch", -12.0, -60.0, 60.0) * kPi / 180.f; return v; }

// Where a glove is: the aim pose orients it (fingers along the pointing ray), the grip pose places the palm on the handle.
struct HandFrame {
    bool valid = false;
    int side = 0;
    V origin;
    float quat[4] = {0, 0, 0, 1};
    float size = .19f, roll_c = 1, roll_s = 0, pitch_c = 1, pitch_s = 0;
};
HandFrame hand_frame(const xr::HandView &h, int side) {
    HandFrame f;
    if (!h.grip.valid) return f;
    const xr::EyeView &orient = h.aim.valid ? h.aim : h.grip;
    f.valid = true;
    f.side = side;
    f.size = hand_size();
    std::copy(std::begin(orient.quat), std::end(orient.quat), f.quat);
    const float roll = side ? -hand_roll() : hand_roll(), pitch = hand_pitch();
    f.roll_c = std::cos(roll), f.roll_s = std::sin(roll), f.pitch_c = std::cos(pitch), f.pitch_s = std::sin(pitch);
    f.origin = vec(h.grip.pos) + rotate(f.quat, V{0, .02f, .30f * f.size});
    return f;
}
// A hand-local direction (fingers +Y, palm +Z) in the aim pose's frame: X -> -X, +Y -> -Z (forward), +Z -> -Y (palm down), then the roll
// about the pointing ray and the pitch about the hand's side axis.
V hand_dir(const HandFrame &f, V p) {
    V a{-p.x, -p.z, -p.y};
    a = {a.x * f.roll_c - a.y * f.roll_s, a.x * f.roll_s + a.y * f.roll_c, a.z};
    a = {a.x, a.y * f.pitch_c - a.z * f.pitch_s, a.y * f.pitch_s + a.z * f.pitch_c};
    return rotate(f.quat, a);
}
V hand_point(const HandFrame &f, V p) { return f.origin + hand_dir(f, p * f.size); }

// A cartoon glove. The index finger closes with the trigger, the other fingers with the grip, about the knuckle line. Lit from above.
void glove(const HandFrame &f, const xr::HandView &h) {
    const vr_glove::Mesh &mesh = f.side ? vr_glove::kRight : vr_glove::kLeft;
    const float index_close = std::clamp(h.trigger, 0.f, 1.f) * 1.4f, close = std::clamp(h.squeeze, 0.f, 1.f) * 1.4f;
    constexpr float kKnuckle = .47f;
    const V light = norm(V{-.4f, 1, .3f});
    auto &s = g.scene;
    if (s.count + mesh.index_count > xr::SpatialScene::kMaxVertices)
        return;
    V world[1024];
    Color shade[1024];
    const uint32_t n = std::min<uint32_t>(mesh.vertex_count, 1024);
    for (uint32_t i = 0; i < n; ++i) {
        const auto &v = mesh.vertices[i];
        V p{v.pos[0], v.pos[1], v.pos[2]}, nrm{v.normal[0], v.normal[1], v.normal[2]};
        const float bend = v.curl * (v.index ? index_close : close);
        if (bend > 0) {   // toward the palm (+Z) about the knuckle line
            const float c = std::cos(bend), sn = std::sin(bend), y = p.y - kKnuckle;
            p = {p.x, kKnuckle + y * c - p.z * sn, y * sn + p.z * c};
            nrm = {nrm.x, nrm.y * c - nrm.z * sn, nrm.y * sn + nrm.z * c};
        }
        world[i] = hand_point(f, p);
        const float lit = v.shade * (.55f + .55f * std::max(0.f, dot(hand_dir(f, nrm), light)));
        shade[i] = {kDitto.r * lit, kDitto.g * lit, kDitto.b * lit};
    }
    for (uint32_t t = 0; t + 2 < mesh.index_count; t += 3) {
        const uint16_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
        if (a >= n || b >= n || c >= n)
            continue;
        for (uint16_t k : {a, b, c}) {
            auto &out = s.vertices[s.count++];
            out = {{world[k].x, world[k].y, world[k].z}, {shade[k].r, shade[k].g, shade[k].b, 1}, {0, 0}, 0};
        }
    }
}
// The right index fingertip, straight or pulled in by the trigger.
V index_tip(const HandFrame &f, float trigger) {
    const float *t = (f.side ? vr_glove::kRight : vr_glove::kLeft).index_tip;
    V p{t[0], t[1], t[2]};
    const float a = std::clamp(trigger, 0.f, 1.f) * 1.4f, c = std::cos(a), sn = std::sin(a), y = p.y - .47f;
    return hand_point(f, {p.x, .47f + y * c - p.z * sn, y * sn + p.z * c});
}
// A square smartwatch on the back of the left glove's wrist: a band around the wrist, a rounded case and a dark screen with the native move
// wheel on it; the native "Nearby Habitats" box floats just past the watch. Face axes: right toward the fingers, up toward the little
// finger (across the wrist, the long side), normal out of the back of the hand (readable with the forearm across the body).
struct Watch {
    V center, right, up, normal;
    float half_w = .0175f, half_h = .0205f;   // case half-sizes along right and up
};
// [vr] watch_size: the case's long side in metres (an Apple Watch is about 0.041).
float watch_size() { static const float v = vr_value("watch_size", 0.041, 0.020, 0.090); return v; }
// Wrist cross-section of the left glove, hand-local units: centre and half-widths across (X) and through (Z) the wrist.
constexpr float kWristY = .05f, kWristCx = -.044f, kWristCz = -.005f, kWristRx = .25f, kWristRz = .20f;
// Native HUD rects (x, y, w, h of the 16:9 HUD in the square picture, before the VR HUD inset): the move wheel and the Nearby Habitats box.
constexpr float kWheelRect[] = {.80f, .65f, .19f, .16f}, kHabitatRect[] = {.0f, .27f, .28f, .15f};
Watch watch_pose(const HandFrame &f) {
    Watch w;
    w.half_h = watch_size() * .5f;
    w.half_w = w.half_h * .85f;
    const float lift = .012f / f.size;   // the case sits on the band, just off the back of the wrist
    w.center = hand_point(f, {kWristCx, kWristY, kWristCz - kWristRz - lift});
    w.right = norm(hand_dir(f, {0, 1, 0}));
    w.up = norm(hand_dir(f, {1, 0, 0}));
    w.normal = norm(hand_dir(f, {0, 0, -1}));
    return w;
}
// A rounded rectangle in the face plane, as a fan; picture != 0 maps it onto that live picture (u right, v down).
void rounded(const Watch &w, V center, float hw, float hh, float corner, Color color, float picture = 0) {
    constexpr int kArc = 6;
    V ring[4 * (kArc + 1)];
    int n = 0;
    const float cx[] = {hw - corner, -(hw - corner), -(hw - corner), hw - corner}, cy[] = {hh - corner, hh - corner, -(hh - corner), -(hh - corner)};
    for (int q = 0; q < 4; ++q)
        for (int k = 0; k <= kArc; ++k) {
            const float a = (q + static_cast<float>(k) / kArc) * kPi * .5f;
            ring[n++] = {cx[q] + corner * std::cos(a), cy[q] + corner * std::sin(a), 0};
        }
    auto at = [&](V p) { return center + w.right * p.x + w.up * p.y; };
    auto uv = [&](V p) { return V{.5f + p.x / (2 * hw), .5f - p.y / (2 * hh), 0}; };
    for (int i = 0; i < n; ++i) {
        const V a = ring[i], b = ring[(i + 1) % n];
        triangle(at({}), at(a), at(b), color, picture, uv({}), uv(a), uv(b));
    }
}
void watch(const HandFrame &f, const Watch &w, bool touched, bool habitats) {
    const Color band{.05f, .05f, .07f}, bezel{.12f, .12f, .14f}, screen{.01f, .01f, .012f}, white{1, 1, 1};
    // band: an elliptical strip around the wrist, slightly outside the glove
    constexpr int kBand = 24;
    const float half = w.half_w * .9f / f.size, grow = 1.08f;
    for (int i = 0; i < kBand; ++i) {
        const float a = 2 * kPi * i / kBand, b = 2 * kPi * (i + 1) / kBand;
        auto at = [&](float t, float dy) {
            return hand_point(f, {kWristCx + kWristRx * grow * std::cos(t), kWristY + dy, kWristCz + kWristRz * grow * std::sin(t)});
        };
        const float light = .7f + .3f * std::max(0.f, -std::sin(a));
        const Color col{band.r * light, band.g * light, band.b * light};
        triangle(at(a, -half), at(b, -half), at(b, half), col);
        triangle(at(a, -half), at(b, half), at(a, half), col);
    }
    // case: back plate, rounded front, then the screen and the wheel on it
    const float corner = w.half_w * .42f;
    rounded(w, w.center - w.normal * .007f, w.half_w, w.half_h, corner, bezel);
    rounded(w, w.center, w.half_w, w.half_h, corner, bezel);
    const float sw = w.half_w * .86f, sh = w.half_h * .88f;
    rounded(w, w.center + w.normal * .0004f, sw, sh, corner * .8f, screen);
    const float wheel_h = std::min(sh, sw / 1.19f);
    rounded(w, w.center + w.normal * .0008f, sw, wheel_h, corner * .5f, white, 1);
    if (touched)   // a small light at the edge while a finger is on the screen
        ball(xr::EyeView{}, w.center + w.up * (sh * .85f) + w.normal * .002f, {.002f, .002f, .002f}, green);
    // the Nearby Habitats box, floating past the 12 o'clock side; only the box's own pixels are drawn (picture 2)
    if (habitats) {
        const float card_w = vr_value("habitat_card_width", 0.075, 0.03, 0.20) * .5f;
        const float card_h = card_w * (kHabitatRect[3] / kHabitatRect[2]);
        const V at = w.center + w.normal * .012f + w.up * (w.half_h + card_h + .006f);
        rounded(w, at, card_w, card_h, .001f, white, 2);
    }
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
        ui_layer::set_background_region(0, {}), ui_layer::set_background_region(1, {});
        return;
    }
    if (head.reference_epoch != g.epoch) {
        g.epoch = head.reference_epoch;
        reset_interaction();
        g.vine_start = 0;
    }
    const HandFrame left_frame = hand_frame(left, 0), right_frame = hand_frame(right, 1);
    if (settings::get_bool(settings::Key::VrHands)) {
        if (left_frame.valid)
            glove(left_frame, left);
        if (right_frame.valid)
            glove(right_frame, right);
    }
    const bool wrist = settings::get_bool(settings::Key::VrWrist) && left_frame.valid;
    Watch w{};
    if (wrist) {
        // Smartwatch scrolling: rest the right index fingertip on the face and slide it toward the fingers (next move) or the elbow
        // (previous move); every 2 cm of slide is one step, with a tick on the right controller.
        w = watch_pose(left_frame);
        bool on_face = false;
        float x = 0;
        if (right_frame.valid) {
            const V delta = index_tip(right_frame, right.trigger) - w.center;
            const float depth = dot(delta, w.normal);
            x = dot(delta, w.right);
            const float y = dot(delta, w.up);
            on_face = depth > -.015f && depth < .025f && std::fabs(x) < w.half_w * 1.5f && std::fabs(y) < w.half_h * 1.5f &&
                      dot(w.normal, norm(vec(head.center) - w.center)) > .1f;
        }
        if (on_face && !g.touching) {
            g.touching = true;
            g.swipe_x = x;
            xr::haptic(1, .05f, .01f);
        } else if (!on_face) {
            g.touching = false;
        }
        constexpr float kStep = .02f;
        if (g.touching && std::fabs(x - g.swipe_x) > kStep && now >= g.cooldown) {
            const bool next = x > g.swipe_x;
            pulse(hid::btn(next ? hid::kBtnRight : hid::kBtnLeft), now);
            g.swipe_x += next ? kStep : -kStep;
            g.cooldown = now + 180000000ull;
            g.whip_armed = false;
            xr::haptic(1, .12f, .02f);
            NXLOG_I("vr", "wrist swipe: %s", next ? "next move" : "previous move");
        }
        watch(left_frame, w, g.touching, true);
        // the HUD rects in the square render, moved by the user's whole-HUD inset
        const float scale = static_cast<float>(settings::get_double(settings::Key::VrHudScale));
        for (int k = 0; k < 2; ++k) {
            const float *native = k ? kHabitatRect : kWheelRect;
            float *r = g.scene.picture_rect + 4 * k;
            r[0] = .5f + (native[0] - .5f) * scale;
            r[1] = .5f + (native[1] - .5f) * scale;
            r[2] = native[2] * scale;
            r[3] = native[3] * scale;
        }
    } else {
        g.touching = false;
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
            if (!g.whip_armed && reach < .28f) {
                g.whip_armed = true;
                g.pull = reach;
                g.cast_start = now;
            }
            if (g.whip_armed) {
                g.pull = std::min(g.pull, reach);
                const float speed = dt > .003 && dt < .1 ? dot(relative - g.previous, forward) / static_cast<float>(dt) : 0;
                if (now - g.cast_start > 1200000000ull)
                    g.whip_armed = false;
                else if (reach - g.pull > .15f && speed > 1.2f) {
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
    for (int k = 0; k < 2; ++k) {   // the lifted HUD pieces are painted over with the world behind them in the eye images
        const float *r = g.scene.picture_rect + 4 * k;
        const float margin = r[2] > 0 ? .003f : 0;
        ui_layer::set_background_region(k, {r[0] - margin, r[1] - margin, r[2] + 2 * margin, r[3] + 2 * margin});
    }
}
void install(const FeatureInstall &in) { g.base = in.base; }
void remove() {
    g.scene.count = 0;
    xr::set_spatial_scene(g.scene);
    ui_layer::set_background_region(0, {}), ui_layer::set_background_region(1, {});
    reset_interaction();
    g.playing = false;
    g.vine_start = 0;
}
} // namespace
const Feature kVrHandsFeature = {.name = "vr_hands", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};
} // namespace nxrt::games::pkp

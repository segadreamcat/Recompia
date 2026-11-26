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
#include <cstdio>
#include <string>
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
// The character's selected move: a skill id (the move wheel's ids) at +0x1a of its move state at character + 0x3950 (0x2fa9a4 reads it,
// the wheel's next / previous at 0x2f9030 change it). Cut is skill 5 (its action is 883).
constexpr uint64_t kSelectedSkill = 0x3950 + 0x1a;
constexpr uint8_t kCutSkill = 5, kLeafageSkill = 1;   // Leafage is skill 1 (its action is 878)
constexpr int kVineN = 16;   // points along a Leafage vine
struct State {
    uint64_t base = 0, time = 0, epoch = 0, pulse_until = 0, pulse = 0, cooldown = 0, cast_start = 0, vine_start = 0;
    std::atomic<bool> playing{false};
    bool touching = false, swiped = false, whip_armed = false;
    uint64_t grip_start = 0;
    bool grip_was = false, grip_cast = false;
    float swipe_x = 0, pull = 0;
    V previous, vine_origin, vine_direction;
    uint16_t action = 0;
    uint8_t skill = 0;
    // The floating HUD: where it stands (local space) and which way it faces; it follows the head lazily
    bool ui_placed = false;
    V ui_center, ui_target;
    float ui_yaw = 0, ui_target_yaw = 0;
    float morph[2] = {};        // per hand: 0 the glove, 1 the Cut blade; animated between, Ditto style
    uint64_t raised_until[2] = {};   // a blade stays a moment after its grip opens, so a swing keeps it
    uint64_t grip_tap_start[2] = {};   // with Cut or Leafage selected a grip grows the claw or vine; a quick tap still sends L or R
    bool grip_tap_was[2] = {};
    // the Leafage vines: a loose chain from each hand, swung around by the hand and wiggling on its own (Verlet)
    V vine[2][kVineN], vine_prev[2][kVineN];
    bool vine_live[2] = {};
    uint64_t lash_ns[2] = {};   // a whip just used Leafage: the vine lashes out along the swing
    V lash_dir[2];
    V swing_prev[2];
    uint64_t swing_time[2] = {}, cut_cooldown = 0;
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
// squash < 1 pulls the whole glove in toward the middle of the palm (the start of a Ditto morph).
void glove(const HandFrame &f, const xr::HandView &h, float squash = 1) {
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
        if (squash < 1) {
            const V mid{0, .40f, .05f};
            p = mid + (p - mid) * squash;
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
// Wrist gear on the back of each glove's wrist. Left: a round smartwatch with a Poke Ball bezel (red top half, white bottom half, a black
// seam) and the native move wheel on its face. Right: a plain band carrying the native "Nearby Habitats" box. Face axes: right along the
// forearm (toward the fingers on the left wrist, toward the elbow on the right, so both read with the arm across the body), up across
// the wrist, normal out of the back of the hand.
struct Watch {
    V center, right, up, normal;
    float radius = .017f;   // face radius; the bezel ring goes out to radius * kBezel
};
constexpr float kBezel = 1.25f;
// [vr] watch_size: the watch's outer diameter, bezel included, in metres.
float watch_size() { static const float v = vr_value("watch_size", 0.056, 0.020, 0.100); return v; }
// Wrist cross-section of each glove, hand-local units: centre and half-widths across (X) and through (Z) the wrist.
constexpr float kWristY = .05f, kWristRx = .25f, kWristRz = .20f;
constexpr float kWristCx[2] = {-.044f, .046f}, kWristCz[2] = {-.004f, -.0065f};
// Native HUD rects (x, y, w, h of the 16:9 HUD in the square picture, before the VR HUD inset): the move wheel and the Nearby Habitats box.
constexpr float kWheelRect[] = {.80f, .65f, .19f, .16f};
Watch watch_pose(const HandFrame &f, int side) {
    Watch w;
    w.radius = watch_size() * .5f / kBezel;
    const float lift = .010f / f.size;   // the case sits on the band, just off the back of the wrist
    w.center = hand_point(f, {kWristCx[side], kWristY, kWristCz[side] - kWristRz - lift});
    const float flip = side ? -1.f : 1.f;
    w.right = norm(hand_dir(f, {0, flip, 0}));
    w.up = norm(hand_dir(f, {flip, 0, 0}));
    w.normal = norm(hand_dir(f, {0, 0, -1}));
    return w;
}
void band(const HandFrame &f, int side, float width, float grow = 1.08f) {
    const Color band{.05f, .05f, .07f};
    constexpr int kBand = 24;
    const float half = width * .5f / f.size;
    for (int i = 0; i < kBand; ++i) {
        const float a = 2 * kPi * i / kBand, b = 2 * kPi * (i + 1) / kBand;
        auto at = [&](float t, float dy) {
            return hand_point(f, {kWristCx[side] + kWristRx * grow * std::cos(t), kWristY + dy, kWristCz[side] + kWristRz * grow * std::sin(t)});
        };
        const float light = .7f + .3f * std::max(0.f, -std::sin(a));
        const Color col{band.r * light, band.g * light, band.b * light};
        triangle(at(a, -half), at(b, -half), at(b, half), col);
        triangle(at(a, -half), at(b, half), at(a, half), col);
    }
}
void watch(const HandFrame &f, const Watch &w, bool touched) {
    const Color red{.80f, .03f, .03f}, white{.92f, .92f, .92f}, black{.02f, .02f, .02f}, side_col{.06f, .06f, .07f}, face{1, 1, 1};
    band(f, 0, .018f);
    const float outer = w.radius * kBezel, seam = w.radius * .05f;
    tube(w.center - w.normal * .006f, w.center, outer, outer, side_col, false);
    constexpr int kRing = 48;
    const V z = w.normal * .0005f;
    for (int i = 0; i < kRing; ++i) {
        const float a = 2 * kPi * i / kRing, b = 2 * kPi * (i + 1) / kRing;
        const V p = w.right * std::cos(a) + w.up * std::sin(a), q = w.right * std::cos(b) + w.up * std::sin(b);
        const Color ring = std::sin(a + kPi / kRing) >= 0 ? red : white;   // top half red, bottom half white
        triangle(w.center + p * outer, w.center + q * outer, w.center + q * w.radius, ring);
        triangle(w.center + p * outer, w.center + q * w.radius, w.center + p * w.radius, ring);
        triangle(w.center + z, w.center + p * w.radius + z, w.center + q * w.radius + z, face, 1, {.5f, .5f, 0},
                 {.5f + .5f * std::cos(a), .5f - .5f * std::sin(a), 0}, {.5f + .5f * std::cos(b), .5f - .5f * std::sin(b), 0});
    }
    // the black seam across the bezel, on both sides of the face
    for (const float s : {1.f, -1.f}) {
        const V in = w.center + w.right * (s * w.radius) + z * .5f, out = w.center + w.right * (s * outer) + z * .5f;
        triangle(in - w.up * seam, out - w.up * seam, out + w.up * seam, black);
        triangle(in - w.up * seam, out + w.up * seam, in + w.up * seam, black);
    }
    if (touched)   // a small light at the edge while a finger is on the face
        ball(xr::EyeView{}, w.center + w.up * (w.radius * .8f) + w.normal * .002f, {.003f, .003f, .003f}, green);
}
// Cut: each glove becomes a pale, curved scythe blade (the move borrows Scyther's), from the back of the wrist past the fingertips.
// grow 0..1 draws it part way out of the hand, still Ditto purple at the start (the end of a Ditto morph).
Color mix(Color a, Color b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
void blade(const HandFrame &f, float grow = 1) {
    // Scyther's scythe: a broad, flat sickle rising from the back of the wrist over the fingers and hooking down toward the palm side;
    // a pale silver face, a green ridge along its outer edge and a green cuff where it leaves the arm.
    const Color face = mix(kDitto, {.90f, .92f, .95f}, grow), shade = mix(kDitto, {.70f, .74f, .80f}, grow),
                ridge = mix(kDitto, {.42f, .72f, .30f}, grow), cuff = mix(kDitto, {.36f, .62f, .26f}, grow);
    static const float size = vr_value("blade_size", 1.7, 0.5, 3.0);
    const float k = size * std::max(grow, .05f);
    const float flip = f.side ? 1.f : -1.f;
    constexpr int kSeg = 16;
    // the sickle's two edges as curves from the wrist to the tip (hand units, y along the fingers, z toward the palm): the outer back
    // edge arcs high over the fingers, the inner cutting edge less, and they meet at the point, which hooks down past the fingertips
    auto curve = [&](V b, V m, V e, float t) { return (b * ((1 - t) * (1 - t)) + m * (2 * t * (1 - t)) + e * (t * t)) * k; };
    const V tip{flip * .05f, .62f, .62f};
    auto outer = [&](float t) { return curve({0, 0, -.14f}, {flip * .03f, 1.55f, -.02f}, tip, t); };
    auto inner = [&](float t) { return curve({0, .22f, -.02f}, {flip * .04f, 1.0f, .16f}, tip, t); };
    V po{}, pi{}, pr{};
    for (int seg = 0; seg <= kSeg; ++seg) {
        const float t = static_cast<float>(seg) / kSeg;
        const V o = outer(t), in = inner(t);
        const V d = o - in;   // the green ridge just outside the outer edge, narrowing to nothing at the point
        const float dl = std::sqrt(dot(d, d));
        const V rim = dl > 1e-4f ? o + d * (.045f * k * std::min(1.f, dl / (.12f * k)) / dl) : o;
        const V a = hand_point(f, o), b = hand_point(f, in), r = hand_point(f, rim);
        if (seg) {
            const Color col = seg % 2 ? face : mix(face, shade, .35f);   // faint bands, like the blade's sheen
            triangle(po, a, b, col), triangle(po, b, pi, col);
            triangle(pr, r, a, ridge), triangle(pr, a, po, ridge);
        }
        po = a, pi = b, pr = r;
    }
    tube(hand_point(f, {0, -.04f, -.10f}), hand_point(f, V{0, .12f, -.14f} * std::max(k, .3f)), .07f * f.size, .06f * f.size, cuff, true);
}
// The game's 2D layer (conversations, menus, prompts, talk bubbles) floats in the world on a large sheet in front of the player instead
// of being painted on the eyes: only the pixels the 2D layer drew are shown (picture 2), the sheet stays where it is while the head looks
// around and glides back in front once the head has turned well away. The move wheel, already on the watch, is left out of it.
// [vr] ui_distance and ui_width in metres, ui_follow the head turn (degrees) that brings it back in front.
void hud_sheet(const xr::EyeView &head, uint64_t now, const float *hud, const float *hole) {
    static uint64_t last = now;
    const float dt = std::min(static_cast<float>(now - last) * 1e-9f, .1f);
    last = now;
    const float dist = vr_value("ui_distance", 1.5, 0.6, 4.0), width = vr_value("ui_width", 1.7, 0.5, 5.0);
    const float follow = vr_value("ui_follow", 40, 10, 120) * kPi / 180;
    V fwd = rotate(head.quat, {0, 0, -1});
    fwd.y = 0;
    if (fwd.x * fwd.x + fwd.z * fwd.z < 1e-4f) fwd = {0, 0, -1};
    fwd = norm(fwd);
    const float yaw = std::atan2(fwd.x, -fwd.z);
    const V eye = vec(head.center);
    const V want = eye + fwd * dist + V{0, vr_value("ui_height", 0.0, -1.0, 1.0), 0};   // level with the eyes
    float turn = yaw - g.ui_yaw;
    while (turn > kPi) turn -= 2 * kPi;
    while (turn < -kPi) turn += 2 * kPi;
    const V off = g.ui_center - eye;
    const float away = std::sqrt(off.x * off.x + off.z * off.z);
    if (!g.ui_placed) {
        g.ui_placed = true;
        g.ui_center = g.ui_target = want;
        g.ui_yaw = g.ui_target_yaw = yaw;
    } else if (std::fabs(turn) > follow || std::fabs(away - dist) > .6f) {
        g.ui_target = want;
        g.ui_target_yaw = yaw;
    }
    // glide toward the target (about a third of a second)
    const float a = std::min(1.f, dt * 6);
    g.ui_center = g.ui_center + (g.ui_target - g.ui_center) * a;
    float dy = g.ui_target_yaw - g.ui_yaw;
    while (dy > kPi) dy -= 2 * kPi;
    while (dy < -kPi) dy += 2 * kPi;
    g.ui_yaw += dy * a;
    const V right{std::cos(g.ui_yaw), 0, std::sin(g.ui_yaw)}, up{0, 1, 0};
    const float hw = width * .5f, hh = hw * hud[3] / hud[2] * 1.f;   // the band's own shape (16:9 at full HUD)
    // the sheet in band coordinates (u right, v down), cut around the hole the watch's wheel leaves
    auto rect = [&](float u0, float v0, float u1, float v1) {
        if (u1 <= u0 || v1 <= v0) return;
        auto at = [&](float u, float v) { return g.ui_center + right * ((u - .5f) * 2 * hw) + up * ((.5f - v) * 2 * hh); };
        const Color white{1, 1, 1};
        triangle(at(u0, v0), at(u0, v1), at(u1, v1), white, 2, {u0, v0, 0}, {u0, v1, 0}, {u1, v1, 0});
        triangle(at(u0, v0), at(u1, v1), at(u1, v0), white, 2, {u0, v0, 0}, {u1, v1, 0}, {u1, v0, 0});
    };
    if (hole && hole[2] > 0) {
        const float u0 = std::clamp((hole[0] - hud[0]) / hud[2], 0.f, 1.f), u1 = std::clamp((hole[0] + hole[2] - hud[0]) / hud[2], 0.f, 1.f);
        const float v0 = std::clamp((hole[1] - hud[1]) / hud[3], 0.f, 1.f), v1 = std::clamp((hole[1] + hole[3] - hud[1]) / hud[3], 0.f, 1.f);
        rect(0, 0, 1, v0), rect(0, v1, 1, 1), rect(0, v0, u0, v1), rect(u1, v0, 1, v1);
    } else {
        rect(0, 0, 1, 1);
    }
}
// The game's whole 2D layer: all of the square picture (its 2D reaches above and below the 16:9 middle, the conversation box among it),
// moved by the user's HUD inset. Lifting all of it leaves no edge of the painted-back world inside the view.
void hud_band(float out[4]) {
    const float scale = static_cast<float>(settings::get_double(settings::Key::VrHudScale));
    out[0] = out[1] = .5f - .5f * scale, out[2] = out[3] = scale;
}
// Leafage: a long vine grows from the hand, Ditto purple where it leaves the hand and leaf green toward the tip. It is a loose chain:
// the hand drags its root around, the rest follows with a little lag and a wiggle of its own, so a fast whip of the hand cracks it.
// [vr] vine_length in metres.
void vine(const HandFrame &f, int h, float grow, uint64_t now) {
    static const float length = vr_value("vine_length", 1.3, 0.3, 3.0);
    static uint64_t last[2] = {};
    const float dt = last[h] ? std::clamp(static_cast<float>(now - last[h]) * 1e-9f, .001f, .05f) : .011f;
    last[h] = now;
    V *p = g.vine[h], *q = g.vine_prev[h];
    const V root = hand_point(f, {0, .55f, 0}), out = norm(hand_dir(f, {0, 1, 0}));
    // a lash: for about half a second after a whip the vine stretches out and its tip is flung along the swing
    const float lt = g.lash_ns[h] ? static_cast<float>(now - g.lash_ns[h]) * 1e-9f / .5f : 1.f;
    const float lash = lt < 1 ? std::sin(kPi * lt) : 0.f;
    const float seg = length * std::max(grow, .05f) * (1 + .7f * lash) / (kVineN - 1);
    if (!g.vine_live[h]) {
        for (int i = 0; i < kVineN; ++i) p[i] = q[i] = root + out * (seg * i);
        g.vine_live[h] = true;
    }
    const float t = static_cast<float>(now % 100000000000ull) * 1e-9f;
    const V side = norm(cross(out, std::fabs(out.y) < .9f ? V{0, 1, 0} : V{1, 0, 0})), lift = cross(out, side);
    for (int i = 1; i < kVineN; ++i) {   // move: keep momentum (damped), sag a little, wiggle like it is alive
        const float u = static_cast<float>(i) / (kVineN - 1);
        const V wiggle = side * (std::sin(t * 7 + u * 9 + h * 2) * 2.2f * u) + lift * (std::cos(t * 5.5f + u * 7) * 1.6f * u);
        V acc = V{0, -2.f, 0} + wiggle * (1 - lash) + out * 2.f;   // a pull outward keeps it reaching away from the hand
        if (lash > 0) acc = acc + g.lash_dir[h] * (60.f * u * (1 - lt));   // the crack runs out toward the tip
        const V now_p = p[i];
        p[i] = p[i] + (p[i] - q[i]) * .975f + acc * (dt * dt);   // little damping: it keeps swinging, like a vine
        q[i] = now_p;
    }
    p[0] = q[0] = root;
    for (int pass = 0; pass < 4; ++pass)   // keep each link its length
        for (int i = 1; i < kVineN; ++i) {
            V d = p[i] - p[i - 1];
            const float l = std::sqrt(dot(d, d));
            if (l < 1e-5f) d = out * seg;
            else d = d * (seg / l);
            p[i] = p[i - 1] + d;
        }
    for (int i = 1; i < kVineN; ++i) {   // draw: thick and purple at the hand, thin and green at the tip
        const float u0 = static_cast<float>(i - 1) / (kVineN - 1), u1 = static_cast<float>(i) / (kVineN - 1);
        const Color col = mix(kDitto, {.30f, .78f, .25f}, std::min(1.f, u1 * 1.6f));
        tube(p[i - 1], p[i], .016f * (1 - u0 * .7f), .016f * (1 - u1 * .7f), col, i == kVineN - 1);
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
void game_frame(::Ctx *c) {
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
        // menus and conversations (the game does not count those as play): no hands, but the 2D layer still floats in the world
        const bool sheet = xr::running() && head.valid && settings::get_bool(settings::Key::VrFirstPerson) &&
                           settings::get_bool(settings::Key::VrFloatingHud);
        static int was = -1;
        if (was != (sheet ? 1 : 0)) NXLOG_I("vr", "floating HUD %s (menu or conversation)", sheet ? "on" : "off");
        was = sheet ? 1 : 0;
        g.scene.count = 0;
        std::fill(std::begin(g.scene.picture_rect), std::end(g.scene.picture_rect), 0.f);
        if (sheet) {
            float hud[4];
            hud_band(hud);
            std::copy(hud, hud + 4, g.scene.picture_rect + 4);
            hud_sheet(head, now, hud, nullptr);
        } else {
            g.ui_placed = false;
        }
        xr::set_spatial_scene(g.scene);
        ui_layer::set_background_region(0, {});
        const float *r = g.scene.picture_rect + 4;
        ui_layer::set_background_region(1, r[2] > 0 ? ui_layer::Region{r[0], r[1], r[2], r[3]} : ui_layer::Region{});
        return;
    }
    if (head.reference_epoch != g.epoch) {
        g.epoch = head.reference_epoch;
        reset_interaction();
        g.vine_start = 0;
    }
    const HandFrame left_frame = hand_frame(left, 0), right_frame = hand_frame(right, 1);
    const uint64_t character = guest::deref(guest::deref(g.base + layout::kPlayerManager) + layout::kManagerPlayer);
    const uint8_t skill = guest::ok(character + kSelectedSkill, 1) ? guest::rd<uint8_t>(character + kSelectedSkill) : 0;
    if (skill != g.skill) NXLOG_I("vr", "selected move %u", skill);
    g.skill = skill;
    const bool cutting = skill == kCutSkill, leafage = skill == kLeafageSkill;
    const bool morphing = cutting || leafage;   // a move whose hands morph while a grip is squeezed
    {   // the morph: with Cut selected, squeezing a grip melts that hand into a purple blob and a blade grows out of it, and back
        // a moment after letting go (so a swing that ends with the grip open keeps its blade); about a third of a second each way
        static uint64_t last = now;
        const float dt = std::min(static_cast<float>(now - last) * 1e-9f, .1f);
        last = now;
        for (int h = 0; h < 2; ++h) {
            const xr::HandView &hv = h ? right : left;
            if (morphing && hv.squeeze > .6f) g.raised_until[h] = now + 300000000ull;
            const bool claws = morphing && now < g.raised_until[h];
            g.morph[h] = std::clamp(g.morph[h] + (claws ? dt : -dt) / .35f, 0.f, 1.f);
        }
    }
    if (settings::get_bool(settings::Key::VrHands)) {
        for (int h = 0; h < 2; ++h) {
            const HandFrame &f = h ? right_frame : left_frame;
            const float m = g.morph[h];
            if (m < .5f || !leafage) g.vine_live[h] = false;
            if (!f.valid) continue;
            if (m <= 0) {
                glove(f, h ? right : left);
                continue;
            }
            if (m < .5f) glove(f, h ? right : left, 1 - 1.6f * m);
            else if (cutting) blade(f, (m - .5f) * 2);
            else if (leafage) vine(f, h, (m - .5f) * 2, now);
            // the blob: swells while the hand melts, shrinks as the blade forms
            const float blob = (m < .5f ? m * 2 : (1 - m) * 2) * .30f * f.size;
            if (blob > .002f) {
                xr::EyeView at{};
                at.pos[0] = 0, at.pos[1] = 0, at.pos[2] = 0;
                ball(at, hand_point(f, {0, .35f, .02f}), {blob, blob * .9f, blob}, kDitto);
            }
        }
    }
    const bool wrist = settings::get_bool(settings::Key::VrWrist) && left_frame.valid;
    std::fill(std::begin(g.scene.picture_rect), std::end(g.scene.picture_rect), 0.f);
    // the HUD rects in the square render, moved by the user's whole-HUD inset; a rect is lifted only while its wrist is drawn
    const auto lift = [&](int k, const float *native) {
        const float scale = static_cast<float>(settings::get_double(settings::Key::VrHudScale));
        float *r = g.scene.picture_rect + 4 * k;
        r[0] = .5f + (native[0] - .5f) * scale;
        r[1] = .5f + (native[1] - .5f) * scale;
        r[2] = native[2] * scale;
        r[3] = native[3] * scale;
    };
    const bool sheet = settings::get_bool(settings::Key::VrFloatingHud);
    Watch w{};
    if (wrist) {
        // Smartwatch scrolling: rest the right index fingertip on the face and slide it toward the fingers (next move) or the elbow
        // (previous move); every 2 cm of slide is one step, with a tick on the right controller.
        w = watch_pose(left_frame, 0);
        bool on_face = false;
        float x = 0;
        if (right_frame.valid) {
            const V delta = index_tip(right_frame, right.trigger) - w.center;
            const float depth = dot(delta, w.normal);
            x = dot(delta, w.right);
            const float y = dot(delta, w.up);
            on_face = depth > -.015f && depth < .025f && x * x + y * y < (w.radius * 1.7f) * (w.radius * 1.7f) &&
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
        watch(left_frame, w, g.touching);
        lift(0, kWheelRect);
    } else {
        g.touching = false;
    }
    uint64_t capture = g.touching ? hid::btn(hid::kBtnZR) : 0;
    for (int h = 0; h < 2; ++h) {   // with Cut or Leafage selected the grips grow claws or vines; a quick tap is still L or R
        const xr::HandView &hv = h ? right : left;
        if (!morphing || !hv.grip.valid) {
            g.grip_tap_was[h] = false;
            continue;
        }
        capture |= hid::btn(h ? hid::kBtnR : hid::kBtnL);
        const bool held = hv.squeeze > .5f;
        if (held && !g.grip_tap_was[h]) g.grip_tap_start[h] = now;
        if (!held && g.grip_tap_was[h] && now - g.grip_tap_start[h] < 300000000ull) pulse(hid::btn(h ? hid::kBtnR : hid::kBtnL), now);
        g.grip_tap_was[h] = held;
    }
    // Cut by swinging: a fast swing of either hand while it is a blade uses the move.
    for (int h = 0; h < 2; ++h) {
        const xr::HandView &hv = h ? right : left;
        if (!morphing || g.morph[h] < .5f || !hv.grip.valid) {
            g.swing_time[h] = 0;
            continue;
        }
        const V p = vec(hv.grip.pos);
        const double dt = g.swing_time[h] ? static_cast<double>(now - g.swing_time[h]) * 1e-9 : 0;
        if (dt > .003 && dt < .1 && now >= g.cut_cooldown) {
            const V v = (p - g.swing_prev[h]) * static_cast<float>(1 / dt);
            const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (speed > vr_value("cut_speed", 1.8, 0.8, 5.0)) {
                pulse(hid::btn(hid::kBtnZR), now);
                if (leafage) g.lash_ns[h] = now, g.lash_dir[h] = v * (1 / speed);
                g.cut_cooldown = now + 450000000ull;
                xr::haptic(h, .35f, .05f);
                NXLOG_I("vr", "gesture %s: %s swing %.1f m/s", cutting ? "cut" : "leafage", h ? "right" : "left", static_cast<double>(speed));
            }
        }
        g.swing_prev[h] = p;
        g.swing_time[h] = now;
    }
    const uint64_t manager = guest::deref(g.base + layout::kPlayerManager), player = guest::deref(manager + layout::kManagerPlayer),
                   component = guest::deref(player + layout::kAction);
    const uint16_t action = guest::ok(component, layout::kActionId + 2) ? guest::rd<uint16_t>(component + layout::kActionId) : 0;
    if(g.action!=action) NXLOG_I("vr","diagnostic native action %u",action);
    g.action = action;
    const bool whip_controls =
        settings::get_bool(settings::Key::VrWhipCast) && play_camera_free() && right.grip.valid && !g.touching && !morphing;
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
    {
        static int was = -1;
        if (was != (sheet ? 1 : 0)) NXLOG_I("vr", "floating HUD %s (play)", sheet ? "on" : "off");
        was = sheet ? 1 : 0;
    }
    if (sheet) {   // the rest of the 2D layer floats in the world, minus the wheel the watch already shows
        float hud[4];
        hud_band(hud);
        std::copy(hud, hud + 4, g.scene.picture_rect + 4);
        hud_sheet(head, now, hud, wrist ? g.scene.picture_rect : nullptr);
    } else {
        g.ui_placed = false;
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

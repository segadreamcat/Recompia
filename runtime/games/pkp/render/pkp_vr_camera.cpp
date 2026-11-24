// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR camera: the player's render view put at the character's eyes and turned by the headset, one eye per camera update (nxrt/xr.h).
//
// Two hooks, both on sites the game build already has (no Set up again):
//   view   0x7100088e04 (pkp_shoulder_camera.cpp has the details): the follow camera's view block (x20) right before its view matrix is
//          built from eye, look-at and up. Player 0's eye goes to the character (the player's position plus [vr] head_height) plus the
//          headset eye's offset, turned by the follow camera's yaw (the right stick still turns, Skyrim style: the head only looks around).
//          Look-at and up come from the headset's orientation, so pitch and roll are the head's own.
//   proj   0x71008cc124 (pkp_aspect.cpp): a camera's view-projection is handed to the renderer (x20 = the camera). For player 0's view block
//          the projection rows (+192) get the headset eye's own asymmetric field of view and the view-projection (+256) is rebuilt from the
//          view rows (+128), so the picture is exactly what the compositor expects for that eye.
// The game does not rewrite the view block every frame; as in the shoulder camera, the hook keeps what it wrote and takes the block as the
// game's when it differs. Keep the shoulder camera at 0 / 0 while playing in VR (both write the same block).
#include "render/pkp_vr_camera.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_view_offset.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/xr.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "vr";
constexpr uint64_t kViewWrite = 0x88e04;      // mid: x19 = the follow camera, x20 = its view block
constexpr uint64_t kCamSubmit = 0x8cc124;     // mid: x20 = the camera whose view-projection goes to the renderer
constexpr uint64_t kViewRoot = 0x3483e18;     // GOT -> the qword holding the view blocks' root (pkp_view_projection.cpp)
constexpr uint64_t kViewFirst = 0x49b0;       // player 0's view block: root + this
constexpr uint64_t kCameraPlayer = 0x8;       // follow camera: the player index (signed byte)
constexpr uint64_t kEye = 0x10, kBlockBytes = 0x150;
constexpr size_t kView = 128, kProj = 192, kViewProj = 256;

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

// q * v * q^-1 for a unit quaternion (x, y, z, w)
V3 rotate(const float q[4], V3 v) {
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float tx = 2.f * (qy * v.z - qz * v.y), ty = 2.f * (qz * v.x - qx * v.z), tz = 2.f * (qx * v.y - qy * v.x);
    return {v.x + qw * tx + (qy * tz - qz * ty), v.y + qw * ty + (qz * tx - qx * tz), v.z + qw * tz + (qx * ty - qy * tx)};
}
// About +Y by the angle whose sine and cosine are given (OpenXR and, as assumed by the shoulder camera too, the game: right-handed, Y up)
V3 yaw(V3 v, float sn, float cs) { return {cs * v.x + sn * v.z, v.y, -sn * v.x + cs * v.z}; }

struct State {
    uint64_t base = 0;
    bool valid = false;
    float game[12] = {};       // original eye, look-at and up, restored together when VR stops
    float written[12] = {};    // eye, look-at, up as this hook wrote them
    float yaw_s = 0, yaw_c = 1;
    std::atomic<int> eye_in_view{-1};   // the eye the view block holds (the projection hook uses the same eye's field of view)
    xr::EyeView eye;                    // that eye (game thread only)
    std::atomic<uint64_t> placed{0}, projected{0}, other_cams{0};
    std::atomic<uint64_t> last_other{0};
    bool logged = false;
    // one OpenXR frame's camera: both eyes from the same character position and yaw (the game moves between the two eye frames)
    bool latched = false;
    uint64_t latched_frame = 0, reference_epoch = 0;
    bool origin_valid = false;
    float origin[3] = {};
    std::mutex aim_mutex;
    bool aim_valid = false;
    V3 aim_eye, aim_dir;
    uint64_t aim_ns = 0;
    V3 anchor;
    float game_eye[3] = {};
    uint64_t diag_ns = 0;
    float diag_p[6] = {};
};
State g;

using guest::rd;

uint64_t view_block0() {
    const uint64_t got = rd<uint64_t>(g.base + kViewRoot);
    const uint64_t root = guest::ok(got, 8) ? rd<uint64_t>(got) : 0;
    return root ? root + kViewFirst : 0;
}

// The tangents a row-vector projection (clip = p * P) covers, handed to the compositor (nxrt/xr.h note_rendered_fov).
void note_fov_from(const float* P) {
    const float sx = P[0], sy = P[5], p23 = P[11];
    if (!(std::fabs(sx) > 1e-4f) || !(std::fabs(sy) > 1e-4f) || !(std::fabs(p23) > 0.5f)) return;
    const float cx = -P[8] / (sx * p23), cy = -P[9] / (sy * p23);
    const float hx = 1.f / std::fabs(sx), hy = 1.f / std::fabs(sy);
    if (!std::isfinite(cx) || !std::isfinite(cy)) return;
    xr::note_rendered_fov(cx - hx, cx + hx, cy + hy, cy - hy);
}

int hook_view(::Ctx* c, uint64_t, void*) {
    if (!guest::ok(c->x[19], kCameraPlayer + 1) || rd<int8_t>(c->x[19] + kCameraPlayer) != 0) return 0;   // split screen's other players keep the game's camera
    const uint64_t view = c->x[20];
    if (!guest::ok(view, kBlockBytes)) return 0;
    xr::EyeView e = xr::eye_view();
    const xr::EyeView raw_eye = e;
    if (e.valid && xr::mirror_x()) {   // [vr] mirror_x: a left-handed world
        e.pos[0] = -e.pos[0];
        e.quat[1] = -e.quat[1], e.quat[2] = -e.quat[2];
        const float l = e.tan_left;
        e.tan_left = -e.tan_right, e.tan_right = -l;
    }
    if (!e.valid) {
        if (g.valid) {   // VR stopped: the game's own view again
            float cur[12];
            std::memcpy(cur, reinterpret_cast<const void*>(view + kEye), sizeof cur);
            if (std::memcmp(cur, g.written, sizeof cur) == 0) std::memcpy(reinterpret_cast<void*>(view + kEye), g.game, sizeof g.game);
            g.valid = false;
        }
        g.eye_in_view.store(-1, std::memory_order_relaxed);
        g.latched = false;
        g.origin_valid = false;
        { std::lock_guard<std::mutex> lk(g.aim_mutex); g.aim_valid = false; }
        view_offset::set(0, 0, 0, 0);
        return 0;
    }

    float cur[12];
    std::memcpy(cur, reinterpret_cast<const void*>(view + kEye), sizeof cur);
    if (!g.valid || std::memcmp(cur, g.written, sizeof cur) != 0) std::memcpy(g.game, cur, sizeof g.game);   // the game wrote a view
    for (float f : g.game)
        if (!std::isfinite(f)) return 0;

    // the left eye takes the follow camera's yaw (its horizontal forward taken to OpenXR's -Z) and the character's position; the right eye
    // of the same headset frame reuses them, so the two eyes differ only by the headset's own eye offset
    if (!g.origin_valid || e.reference_epoch != g.reference_epoch) {
        std::memcpy(g.origin, e.center, sizeof g.origin);
        g.reference_epoch = e.reference_epoch;
        g.origin_valid = true;
        g.latched = false;
    }
    if (!g.latched || e.frame != g.latched_frame) {
        const float fx = g.game[4] - g.game[0], fz = g.game[6] - g.game[2];
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-3f) {
            // yaw(0, 0, -1) = (-sin, 0, -cos) must equal (fx, fz) / fl
            g.yaw_s = -fx / fl;
            g.yaw_c = -fz / fl;
        }
        // The follow look-at is an animated focus point, including camera collision and scripted offsets. Anchor to the
        // character's established virtual position getter instead; its override also handles the save-data position mode.
        const uint64_t manager = guest::deref(g.base + layout::kPlayerManager);
        const uint64_t player = guest::deref(manager + layout::kManagerPlayer);
        const uint64_t vtable = guest::deref(player);
        const uint64_t getter = guest::deref(vtable + 2 * sizeof(uint64_t));
        if (!getter || !guest::ok(getter, 4)) return 0;
        const uint64_t pos = guest::call(c, getter, player, 0, 0, 0, 0, (c->x[31] - 0x400) & ~uint64_t{15});
        float point[3];
        if (!guest::read(pos, point, sizeof point)) return 0;
        for (float value : point) if (!std::isfinite(value)) return 0;
        g.anchor = V3{point[0], point[1] + xr::head_height(), point[2]};
        g.latched_frame = e.frame;
        g.game_eye[0] = g.game[0], g.game_eye[1] = g.game[1], g.game_eye[2] = g.game[2];
        g.latched = true;
    }
    const float s = xr::world_scale();
    const V3 anchor = g.anchor;
    const V3 eye = add(anchor, yaw(scale(V3{e.pos[0] - g.origin[0], e.pos[1] - g.origin[1], e.pos[2] - g.origin[2]}, s), g.yaw_s, g.yaw_c));
    const V3 fwd = yaw(rotate(e.quat, V3{0, 0, -1}), g.yaw_s, g.yaw_c);
    const V3 up = yaw(rotate(e.quat, V3{0, 1, 0}), g.yaw_s, g.yaw_c);
    const V3 look = add(eye, scale(fwd, 100.f));
    const xr::EyeView hand = xr::hand_aim();
    V3 aim_eye = add(anchor, yaw(scale(V3{e.center[0] - g.origin[0], e.center[1] - g.origin[1], e.center[2] - g.origin[2]}, s), g.yaw_s, g.yaw_c));
    V3 aim_dir = fwd;
    if (hand.valid) {
        aim_eye = add(anchor, yaw(scale(V3{hand.pos[0] - g.origin[0], hand.pos[1] - g.origin[1], hand.pos[2] - g.origin[2]}, s), g.yaw_s, g.yaw_c));
        aim_dir = yaw(rotate(hand.quat, V3{0, 0, -1}), g.yaw_s, g.yaw_c);
    }
    {
        std::lock_guard<std::mutex> lk(g.aim_mutex);
        g.aim_eye = aim_eye; g.aim_dir = aim_dir; g.aim_ns = qpc::now_ns(); g.aim_valid = true;
    }


    float out[12];
    std::memcpy(out, cur, sizeof out);   // keeps each vec4's w
    out[0] = eye.x, out[1] = eye.y, out[2] = eye.z;
    out[4] = look.x, out[5] = look.y, out[6] = look.z;
    out[8] = up.x, out[9] = up.y, out[10] = up.z;
    std::memcpy(reinterpret_cast<void*>(view + kEye), out, sizeof out);
    std::memcpy(g.written, out, sizeof out);
    g.valid = true;
    g.eye = e;
    g.eye_in_view.store(e.eye, std::memory_order_release);
    xr::note_world_view(raw_eye);
    // camera aim casts its ray from the follow camera's eye plus this offset (pkp_view_offset.h)
    view_offset::set(0, eye.x - g.game[0], eye.y - g.game[1], eye.z - g.game[2]);
    {   // the field of view the picture gets, as far as known now (the projection hook refines it): last frame's projection of this view
        const float* P = reinterpret_cast<const float*>(view + 0xc0);
        note_fov_from(P);
    }
    const uint64_t now = qpc::now_ns();
    if (now - g.diag_ns > 3000000000ull) {
        g.diag_ns = now;
        NXLOG_I(kTag, "diag: placed %llu, projected %llu, other cameras %llu (last 0x%llx, view 0x%llx, block0 0x%llx); P before: %.3f %.3f %.3f %.3f %.3f %.3f; "
                "eye %d tan L %.3f R %.3f U %.3f D %.3f; game eye (%.0f %.0f %.0f) look (%.0f %.0f %.0f)",
                static_cast<unsigned long long>(g.placed.load()), static_cast<unsigned long long>(g.projected.load()),
                static_cast<unsigned long long>(g.other_cams.load()), static_cast<unsigned long long>(g.last_other.load()),
                static_cast<unsigned long long>(view), static_cast<unsigned long long>(view_block0()), g.diag_p[0], g.diag_p[1], g.diag_p[2],
                g.diag_p[3], g.diag_p[4], g.diag_p[5], e.eye, e.tan_left, e.tan_right, e.tan_up, e.tan_down, g.game[0], g.game[1], g.game[2],
                g.game[4], g.game[5], g.game[6]);
    }
    g.placed.fetch_add(1, std::memory_order_relaxed);
    if (!g.logged) {
        g.logged = true;
        NXLOG_I(kTag, "first VR eye placed: eye %d at (%.1f, %.1f, %.1f), scale %.1f units/m, head height %.1f", e.eye,
                static_cast<double>(eye.x), static_cast<double>(eye.y), static_cast<double>(eye.z), static_cast<double>(s),
                static_cast<double>(xr::head_height()));
    }
    return 0;
}

int hook_proj(::Ctx* c, uint64_t, void*) {
    if (g.eye_in_view.load(std::memory_order_acquire) < 0) return 0;
    const uint64_t cam = c->x[20];
    if (!cam || cam != view_block0() || !guest::ok(cam, kViewProj + 64)) {
        g.other_cams.fetch_add(1, std::memory_order_relaxed);
        g.last_other.store(cam, std::memory_order_relaxed);
        return 0;
    }
    const xr::EyeView& e = g.eye;
    float* P = reinterpret_cast<float*>(cam + kProj);
    const float* V = reinterpret_cast<const float*>(cam + kView);
    const float p23 = P[11];
    g.diag_p[0] = P[0], g.diag_p[1] = P[5], g.diag_p[2] = P[8], g.diag_p[3] = P[9], g.diag_p[4] = P[10], g.diag_p[5] = P[11];
    if (!(std::fabs(p23) > 0.5f) || !(std::fabs(P[5]) > 1e-6f)) return 0;   // not a perspective projection
    const float rl = e.tan_right - e.tan_left, ud = e.tan_up - e.tan_down;
    if (!(rl > 1e-3f) || !(ud > 1e-3f)) return 0;
    const float sx = 2.f / rl, cx = (e.tan_right + e.tan_left) * 0.5f;
    const float sy = (P[5] > 0 ? 2.f : -2.f) / ud, cy = (e.tan_up + e.tan_down) * 0.5f;   // keeps the game's clip-space Y direction
    // row-vector projection (clip = p * P): x_ndc = sx * (t - cx) for the tangent t = x / depth; the z row carries the off-centre terms
    P[0] = sx, P[1] = 0, P[2] = 0, P[3] = 0;
    P[4] = 0, P[5] = sy, P[6] = 0, P[7] = 0;
    P[8] = -sx * cx * p23;
    P[9] = -sy * cy * p23;
    // P[10], P[11], P[12..15]: the game's depth mapping stays
    float VP[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            VP[i * 4 + j] = V[i * 4 + 0] * P[0 * 4 + j] + V[i * 4 + 1] * P[1 * 4 + j] + V[i * 4 + 2] * P[2 * 4 + j] + V[i * 4 + 3] * P[3 * 4 + j];
    for (float f : VP)
        if (!std::isfinite(f)) return 0;
    std::memcpy(reinterpret_cast<void*>(cam + kViewProj), VP, sizeof VP);
    note_fov_from(P);
    g.projected.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// Reuse the established active-follow-camera layout from camera aim. Camera hooks can update less often than presents.
void game_frame(::Ctx*) {
    const uint64_t manager = guest::deref(g.base + layout::kCameraManager);
    bool world = false;
    if (guest::ok(manager, layout::kActiveCameraIndex + 1)) {
        const int8_t index = rd<int8_t>(manager + layout::kActiveCameraIndex);
        if (index >= 0) world = guest::deref(manager + layout::kActiveCameraTable + static_cast<uint64_t>(index) * 8) == manager + layout::kFollowCamera;
    }
    xr::set_world_active(world);
    if (!world) g.eye_in_view.store(-1, std::memory_order_relaxed);
}

void install(const FeatureInstall& in) {
    g.base = in.base;
    in.hooks.add_mid(in.base + kViewWrite, &hook_view);
    in.hooks.add_mid(in.base + kCamSubmit, &hook_proj);
    NXLOG_I(kTag, "VR camera: hooks main+0x%llx (view), main+0x%llx (projection); active while an OpenXR session runs",
            static_cast<unsigned long long>(kViewWrite), static_cast<unsigned long long>(kCamSubmit));
}

void remove() {
    xr::set_world_active(false);
    view_offset::set(0, 0, 0, 0);
    g.valid = false;
    g.latched = g.origin_valid = false;
    { std::lock_guard<std::mutex> lk(g.aim_mutex); g.aim_valid = false; }
    g.eye_in_view.store(-1);
    g.placed.store(0), g.projected.store(0);
    g.logged = false;
}

void status_text(char* buf, size_t n) {
    std::snprintf(buf, n, " vr_placed=%llu vr_projected=%llu", static_cast<unsigned long long>(g.placed.load()),
                  static_cast<unsigned long long>(g.projected.load()));
}

}  // namespace

bool vr_camera::aim_ray(float eye[3], float direction[3]) {
    if (!xr::running()) return false;
    std::lock_guard<std::mutex> lk(g.aim_mutex);
    if (!g.aim_valid || qpc::now_ns() - g.aim_ns > 250000000ull) return false;
    eye[0] = g.aim_eye.x; eye[1] = g.aim_eye.y; eye[2] = g.aim_eye.z;
    direction[0] = g.aim_dir.x; direction[1] = g.aim_dir.y; direction[2] = g.aim_dir.z;
    return true;
}

const Feature kVrCameraFeature = {.name = "vr_camera", .install = &install, .remove = &remove, .game_frame = &game_frame, .status_text = &status_text};

}  // namespace nxrt::games::pkp

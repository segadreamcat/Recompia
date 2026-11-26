// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR camera: the player's render view put at the character's eyes and turned by the headset, one eye per camera update (nxrt/xr.h).
//
// The follow-camera hook changes the source camera before its view matrix and render copies are built. The engine's
// own projection builder receives a symmetric lens that encloses the headset FOV. Every scene pass therefore uses the
// same lens; OpenXR receives that actual symmetric FOV and handles the headset's asymmetric visible region.
// The submit hook only observes the completed camera. Patching its projection here is too late: some passes already
// copied the old lens, which makes props and terrain separate as the head tilts (the same constraint as pkp_aspect.cpp).
#include "render/pkp_vr_camera.h"
#include "render/pkp_vr_avatar.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "input/pkp_play.h"
#include "ui/pkp_menu.h"
#include "nxrt/ui_layer.h"
#include "core/pkp_view_offset.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/xr.h"

namespace nxrt::games::pkp {
namespace { std::atomic<bool> g_hold_world{false}; }
namespace {

constexpr const char* kTag = "vr";
constexpr uint64_t kViewWrite = 0x88e04;      // mid: x19 = the follow camera, x20 = its view block
constexpr uint64_t kCamSubmit = 0x8cc124;     // mid: x20 = the camera whose view-projection goes to the renderer
constexpr uint64_t kViewRoot = 0x3483e18;     // GOT -> the qword holding the view blocks' root (pkp_view_projection.cpp)
constexpr uint64_t kViewFirst = 0x49b0;       // player 0's view block: root + this
constexpr uint64_t kCameraPlayer = 0x8;       // follow camera: the player index (signed byte)
constexpr uint64_t kEye = 0x10, kBlockBytes = 0x15c;
// Established camera builder and lens fields from pkp_aspect.cpp; rebuild at the source, before the view handover.
constexpr uint64_t kProjectionBuild = 0x1f12114, kProjectionFields = 0x1f12130, kCameraRebuild = 0x1f11e78;
constexpr size_t kFov = 328;
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
    float display_aspect = 16.f / 9.f;
    float game_lens[2] = {}, written_lens[2] = {};
    float written[12] = {};    // eye, look-at, up as this hook wrote them
    float yaw_s = 0, yaw_c = 1;
    std::atomic<int> eye_in_view{-1};   // the eye the view block holds (the projection hook uses the same eye's field of view)
    std::atomic<uint64_t> placed{0}, projected{0}, other_cams{0};
    std::atomic<uint64_t> last_other{0};
    bool logged = false;
    // one OpenXR frame's camera: both eyes from the same character position and yaw (the game moves between the two eye frames)
    std::atomic<bool> playing{false};
    std::atomic<bool> selection_menu{false};
    bool scene_seen = false, scripted = false, world = false;
    bool first_person = true;
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
    V3 placed_eye, placed_right, placed_up, placed_back;
    uint64_t audited = 0;
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

int place_view(::Ctx* c, uint64_t view, bool follow) {
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
        g.eye_in_view.store(-1, std::memory_order_relaxed);
        if (g.valid) {   // VR stopped: the game's own view again
            float cur[12];
            std::memcpy(cur, reinterpret_cast<const void*>(view + kEye), sizeof cur);
            if (std::memcmp(cur, g.written, sizeof cur) == 0) std::memcpy(reinterpret_cast<void*>(view + kEye), g.game, sizeof g.game);
            if (std::memcmp(reinterpret_cast<const void*>(view + kFov), g.written_lens, sizeof g.written_lens) == 0) {
                std::memcpy(reinterpret_cast<void*>(view + kFov), g.game_lens, sizeof g.game_lens);
                guest::call(c, g.base + kProjectionBuild, view, 0, 0, 0, 0, (c->x[31] - 0x400) & ~uint64_t{15});
            }
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
    const bool first = follow && settings::get_bool(settings::Key::VrFirstPerson);
    if (first != g.first_person) { g.first_person = first; g.latched = false; g.origin_valid = false; }
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
        g.anchor = first ? V3{point[0], point[1] + xr::head_height(), point[2]} : V3{g.game[0], g.game[1], g.game[2]};
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
    g.placed_eye = eye; g.placed_up = up; g.placed_back = scale(fwd, -1.f);
    g.placed_right = yaw(rotate(e.quat, V3{1, 0, 0}), g.yaw_s, g.yaw_c);
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
    // Use camera fields, not a late matrix override. The game may rebuild/copy P for separate object and terrain passes.
    float lens[2];
    std::memcpy(lens, reinterpret_cast<const void*>(view + kFov), sizeof lens);
    if (!g.valid || std::memcmp(lens, g.written_lens, sizeof lens) != 0) std::memcpy(g.game_lens, lens, sizeof lens);
    const float hx = std::fmax(std::fabs(e.tan_left), std::fabs(e.tan_right));
    // Keep the native lens aspect. Separate scene paths derive horizontal scale from the display aspect even when
    // the source camera requests another shape. A wider symmetric render FOV encloses the headset without that split;
    // the compositor receives the actual rendered FOV and maps its visible region to the headset.
    const float aspect = g.display_aspect;
    const float hy = std::fmax(std::fmax(std::fabs(e.tan_down), std::fabs(e.tan_up)), hx / aspect);
    g.written_lens[0] = 2.f * std::atan(hy);
    g.written_lens[1] = aspect;
    std::memcpy(reinterpret_cast<void*>(view + kFov), g.written_lens, sizeof g.written_lens);
    guest::call(c, g.base + kProjectionBuild, view, 0, 0, 0, 0, (c->x[31] - 0x400) & ~uint64_t{15});
    g.valid = true;
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

int hook_view(::Ctx* c, uint64_t, void*) {
    if (!guest::ok(c->x[19], kCameraPlayer + 1) || rd<int8_t>(c->x[19] + kCameraPlayer) != 0) return 0;
    return place_view(c, c->x[20], true);
}

int hook_proj(::Ctx* c, uint64_t, void*) {
    // Scripted cameras use the same primary view handover. Retain their authored position and add the tracked view before renderer copies.
    if (g.scripted && g.world && c->x[20] == view_block0() && xr::running()) {
        place_view(c, c->x[20], false);
        if (g.eye_in_view.load(std::memory_order_acquire) >= 0)
            guest::call(c, g.base + kCameraRebuild, c->x[20], 1, 1, 0, 0, (c->x[31]-0x800) & ~uint64_t{15});
    }
    if (g.eye_in_view.load(std::memory_order_acquire) < 0) return 0;
    const uint64_t cam = c->x[20];
    if (!cam || cam != view_block0() || !guest::ok(cam, kViewProj + 64)) {
        g.other_cams.fetch_add(1, std::memory_order_relaxed);
        g.last_other.store(cam, std::memory_order_relaxed);
        return 0;
    }
    const float* P = reinterpret_cast<const float*>(cam + kProj);
    if (++g.audited % 300 == 1) {
        const float* V = reinterpret_cast<const float*>(cam + kView);
        const float ex = -(V[0] * V[12] + V[1] * V[13] + V[2] * V[14]);
        const float ey = -(V[4] * V[12] + V[5] * V[13] + V[6] * V[14]);
        const float ez = -(V[8] * V[12] + V[9] * V[13] + V[10] * V[14]);
        NXLOG_I(kTag, "view audit: eye error (%.4f %.4f %.4f), right error (%.4f %.4f %.4f), up error (%.4f %.4f %.4f), back error (%.4f %.4f %.4f)",
            ex-g.placed_eye.x, ey-g.placed_eye.y, ez-g.placed_eye.z,
            V[0]-g.placed_right.x, V[4]-g.placed_right.y, V[8]-g.placed_right.z,
            V[1]-g.placed_up.x, V[5]-g.placed_up.y, V[9]-g.placed_up.z,
            V[2]-g.placed_back.x, V[6]-g.placed_back.y, V[10]-g.placed_back.z);
    }
    g.diag_p[0] = P[0], g.diag_p[1] = P[5], g.diag_p[2] = P[8], g.diag_p[3] = P[9], g.diag_p[4] = P[10], g.diag_p[5] = P[11];
    note_fov_from(P);
    xr::note_rendered_camera(reinterpret_cast<const float*>(cam + kView), reinterpret_cast<const float*>(cam + kViewProj));
    g.projected.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// Keep the VR FOV on every projection build, including builds made before the follow-view write; the native lens aspect
// is retained so independent scene paths that derive their horizontal scale from the display agree with this camera.
int hook_projection_fields(::Ctx* c, uint64_t, void*) {
    if (!g.valid || !xr::running() || g.eye_in_view.load(std::memory_order_relaxed) < 0) return 0;
    const uint64_t cam = c->x[0];
    if (cam != view_block0() || !guest::ok(cam, kBlockBytes)) return 0;
    std::memcpy(reinterpret_cast<void*>(cam + kFov), g.written_lens, sizeof g.written_lens);
    return 0;
}

// Reuse the established active-follow-camera layout from camera aim. Camera hooks can update less often than presents.
void game_frame(::Ctx* c) {
    if (!xr::wanted()) return;
    const uint64_t manager = guest::deref(g.base + layout::kCameraManager);
    bool world = false, follow = false, scene = false;
    if (guest::ok(manager, layout::kActiveCameraIndex + 1)) {
        const int8_t index = rd<int8_t>(manager + layout::kActiveCameraIndex);
        if (index >= 0 && index < 14) {
            const uint64_t camera = guest::deref(manager + layout::kActiveCameraTable + static_cast<uint64_t>(index) * 8);
            scene = guest::ok(camera, 8);
            follow = camera == manager + layout::kFollowCamera;
        }
        if (follow && g.playing.load(std::memory_order_relaxed)) g.scene_seen = true;
        world = scene && g.scene_seen;
    }
    const uint64_t players = guest::deref(g.base + layout::kPlayerManager);
    const uint64_t player = guest::deref(players + layout::kManagerPlayer);
    const uint8_t input_flags = guest::ok(player, layout::kInputLock + 1) ? rd<uint8_t>(player + layout::kInputLock) : 0;
    const bool locked = (input_flags & layout::kInputLockBit) != 0;
    // The reading-panel preference applies to selectable screens and tutorial popups too. Opening the bag must not force
    // a mode change when the player chose to stay in the world.
    if (world && settings::get_bool(settings::Key::VrAutoReading) && !g_hold_world.load(std::memory_order_relaxed) &&
        ((follow && (input_flags & 0x08)) || g.selection_menu.load(std::memory_order_relaxed) ||
         !g.playing.load(std::memory_order_relaxed) || !play_camera_free() || locked)) world = false;
    g.scripted = scene && !follow;
    vr_avatar::update(c, world && follow && xr::running() && settings::get_bool(settings::Key::VrFirstPerson));
    const bool restore = g.world && !world && g.valid;
    g.world = world;
    xr::set_world_active(world);
    ui_layer::set_scale(world && xr::running() ? static_cast<float>(settings::get_double(settings::Key::VrHudScale)) : 1.f);
    if (restore && guest::ok(view_block0(), kBlockBytes)) {
        place_view(c, view_block0(), false);
        guest::call(c, g.base + kCameraRebuild, view_block0(), 1, 1, 0, 0, (c->x[31]-0x800) & ~uint64_t{15});
    }
    if (!world) g.eye_in_view.store(-1, std::memory_order_relaxed);
}

void frame(bool playing, double, play::GameFrame*) {
    g.playing.store(playing, std::memory_order_relaxed);
    g.selection_menu.store(menu_has_selection(), std::memory_order_relaxed);
}

void install(const FeatureInstall& in) {
    g.base = in.base;
    const auto display = play_display_live();
    g.display_aspect = static_cast<float>(display.aspect > 0 ? display.aspect : static_cast<double>(display.render_w) / display.render_h);
    in.hooks.add_mid(in.base + kViewWrite, &hook_view);
    in.hooks.add_mid(in.base + kCamSubmit, &hook_proj);
    in.hooks.add_mid(in.base + kProjectionFields, &hook_projection_fields);
    NXLOG_I(kTag, "VR camera: hooks main+0x%llx (view), main+0x%llx (lens audit); source camera lens rebuilt before view copies",
            static_cast<unsigned long long>(kViewWrite), static_cast<unsigned long long>(kCamSubmit));
}

void remove() {
    xr::set_world_active(false);
    ui_layer::set_scale(1.f);
    g.scene_seen = g.scripted = g.world = false;
    view_offset::set(0, 0, 0, 0);
    g.valid = false;
    g.playing.store(false, std::memory_order_relaxed);
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

bool vr_camera::owns_projection(uint64_t camera) {
    return g.valid && g.eye_in_view.load(std::memory_order_relaxed) >= 0 && camera == view_block0();
}

void vr_camera::hold_world(bool on) { g_hold_world.store(on, std::memory_order_relaxed); }
bool vr_camera::reference_point(const float point[3], uint64_t epoch, float world[3]) {
    if (!g.valid || !g.world || !g.first_person || !g.latched || epoch != g.reference_epoch) return false;
    const V3 p = add(g.anchor, yaw(scale(V3{point[0]-g.origin[0], point[1]-g.origin[1], point[2]-g.origin[2]}, xr::world_scale()), g.yaw_s, g.yaw_c));
    world[0]=p.x; world[1]=p.y; world[2]=p.z; return true;
}

bool vr_camera::aim_ray(float eye[3], float direction[3]) {
    if (!xr::running()) return false;
    std::lock_guard<std::mutex> lk(g.aim_mutex);
    if (!g.aim_valid || qpc::now_ns() - g.aim_ns > 250000000ull) return false;
    eye[0] = g.aim_eye.x; eye[1] = g.aim_eye.y; eye[2] = g.aim_eye.z;
    direction[0] = g.aim_dir.x; direction[1] = g.aim_dir.y; direction[2] = g.aim_dir.z;
    return true;
}

const Feature kVrCameraFeature = {.name = "vr_camera", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame, .status_text = &status_text};

}  // namespace nxrt::games::pkp

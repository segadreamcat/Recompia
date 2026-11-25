// SPDX-License-Identifier: GPL-3.0-or-later
// VR application shortcuts and local diagnostic captures; game-side settings and files stay out of the OpenXR transport.
#include "input/pkp_vr_controls.h"
#include "core/pkp_settings.h"
#include "nxrt/key_state.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/qpc.h"
#include "nxrt/screenshot.h"
#include "nxrt/xr.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace nxrt::games::pkp {
namespace {
struct State {
    bool view_key = false, shot_key = false;
    uint64_t checked_ns = 0, requested_ns = 0;
    std::filesystem::path pending;
    bool eye = false;   // the pending shot is the headset's left eye (xr::request_eye_shot), else the flat game picture
} g;
void capture() {
    if (!g.pending.empty()) return;
    std::error_code ec;
    const auto dir = std::filesystem::current_path(ec) / "vr-screenshots";
    if (ec) return;
    std::filesystem::create_directories(dir, ec);
    if (ec) { NXLOG_W("vr", "cannot create screenshot folder: %s", ec.message().c_str()); return; }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = dir / ("VR-" + std::to_string(ms) + ".png");
    // What the headset shows (hands, watch and all) while VR runs; the flat game picture otherwise, or when no eye image comes (a menu).
    g.eye = xr::running() && xr::request_eye_shot(path.string().c_str());
    if (!g.eye && !screenshot::request(path.string().c_str(), true)) { notices::post("vr-capture", "Screenshot busy; try again.", 2); return; }
    g.pending = path; g.requested_ns = qpc::now_ns();
    NXLOG_I("vr", "screenshot requested: %s", path.string().c_str());
}
void game_frame(::Ctx*) {
    if (!xr::wanted()) return;
    const bool enabled = settings::get_bool(settings::Key::VrShortcuts);
    xr::set_shortcuts_enabled(enabled);
    xr::set_sharpness(static_cast<float>(settings::get_double(settings::Key::VrSharpness)));
    uint32_t commands = xr::take_commands();
    const bool view_key = key_state::down_by_name("F11"), shot_key = key_state::down_by_name("F12");
    if (enabled) {
        if (view_key && !g.view_key) commands |= xr::kTogglePerspective;
        if (shot_key && !g.shot_key) commands |= xr::kScreenshot;
    } else commands = 0;
    g.view_key = view_key; g.shot_key = shot_key;
    if (commands & xr::kTogglePerspective) {
        const bool first = !settings::get_bool(settings::Key::VrFirstPerson);
        settings::set_bool(settings::Key::VrFirstPerson, first);
        notices::post("vr-view", first ? "VR: first person" : "VR: third person", 2);
        NXLOG_I("vr", "perspective: %s", first ? "first person" : "third person");
    }
    if (commands & xr::kScreenshot) {   // a buzz in the left hand says the shot was taken
        NXLOG_I("vr", "screenshot shortcut");
        xr::haptic(0, .6f, .08f);
        capture();
    }
    const uint64_t now = qpc::now_ns();
    if (!g.pending.empty() && g.eye && xr::eye_shot_state() == 3) {   // no headset eye image came: take the flat picture instead
        g.eye = false;
        if (!screenshot::request(g.pending.string().c_str(), true)) { notices::post("vr-capture", "Screenshot busy; try again.", 2); g.pending.clear(); }
    }
    if (!g.pending.empty() && now-g.checked_ns > 250000000ull) {
        g.checked_ns = now;
        std::error_code ec;
        if (std::filesystem::file_size(g.pending, ec) > 0 && !ec && now-g.requested_ns > 250000000ull) {
            std::ofstream latest(g.pending.parent_path()/"latest.txt"); latest << g.pending.filename().string() << '\n';
            notices::post("vr-capture", "Screenshot saved in vr-screenshots.", 3);
            NXLOG_I("vr", "screenshot saved: %s", g.pending.string().c_str());
            g.pending.clear();
        } else if (now-g.requested_ns > 10000000000ull) {
            notices::post("vr-capture", "Screenshot did not finish; try again.", 3); g.pending.clear();
        }
    }
}
void remove() { g = {}; }
}
const Feature kVrControlsFeature = {.name="vr_controls", .remove=&remove, .game_frame=&game_frame};
}

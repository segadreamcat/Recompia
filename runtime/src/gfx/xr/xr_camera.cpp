// SPDX-License-Identifier: GPL-3.0-or-later
// Associate an image with the actual source camera found in its scene uniforms, independent of CPU/render frame delay.
#include "xr/xr_internal.h"
#include "xr/xr_vk.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "nxrt/log.h"

namespace nxrt::xr {
using namespace detail;
namespace {
thread_local Tag observed;
thread_local bool seen = false;
bool equal_matrix(const float* a, const float* b) {
    if (std::memcmp(a, b, 16 * sizeof(float)) == 0) return true;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > 1e-6f * std::max(1.f, std::fabs(b[i]))) return false;
    return true;
}
}
void note_rendered_camera(const float view[16], const float vp[16]) {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    if (!st.camera.world || !st.camera.fov_ok) return;
    auto& entry = st.cameras[st.camera_sequence++ % State::kCameraHistory];
    entry.tag = st.camera;
    std::memcpy(entry.view, view, sizeof entry.view);
    std::memcpy(entry.vp, vp, sizeof entry.vp);
}
void render_note_camera(const float view[16], const float vp[16]) {
    if (seen) return;
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    const uint64_t count = std::min<uint64_t>(st.camera_sequence, State::kCameraHistory);
    for (uint64_t i = 0; i < count; ++i) {
        const auto& entry = st.cameras[(st.camera_sequence - 1 - i) % State::kCameraHistory];
        if (entry.tag.world && equal_matrix(view, entry.view) && equal_matrix(vp, entry.vp)) {
            observed = entry.tag; seen = true; return;
        }
    }
}
Tag render_resolve_tag(Tag intent) {
    Tag result = intent.world ? (seen ? observed : Tag{}) : intent;
    static thread_local uint64_t matched = 0, skipped = 0, different = 0;
    if (intent.world) {
        if (seen) { ++matched; if (observed.eye != intent.eye || observed.frame != intent.frame) ++different; }
        else ++skipped;
        if ((matched + skipped) % 300 == 0)
            NXLOG_I(kTag, "camera/image match: %llu matched, %llu skipped, %llu differed from present-time camera; actual eye %d pair %llu",
                static_cast<unsigned long long>(matched), static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(different),
                result.eye, static_cast<unsigned long long>(result.frame));
    }
    seen = false; observed = {};
    return result;
}
} // namespace nxrt::xr

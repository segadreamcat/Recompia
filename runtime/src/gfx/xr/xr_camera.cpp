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
thread_local uint64_t draws = 0, changes = 0, mismatches = 0, lens_mismatch = 0;
thread_local float previous_view[16] = {}, previous_vp[16] = {};
thread_local bool previous = false, audit_image = true;
thread_local uint64_t wanted_right = 0, image_sequence = 0;
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
    if (seen && !audit_image) return;
    ++draws;
    if (previous && std::memcmp(previous_view, view, sizeof previous_view) == 0 && std::memcmp(previous_vp, vp, sizeof previous_vp) == 0) return;
    std::memcpy(previous_view, view, sizeof previous_view);
    std::memcpy(previous_vp, vp, sizeof previous_vp);
    previous = true;
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    const uint64_t count = std::min<uint64_t>(st.camera_sequence, State::kCameraHistory);
    bool same_view = false;
    const State::CameraTag* match = nullptr;
    for (uint64_t i = 0; i < count; ++i) {
        const auto& entry = st.cameras[(st.camera_sequence - 1 - i) % State::kCameraHistory];
        const bool view_matches = entry.tag.world && equal_matrix(view, entry.view);
        same_view |= view_matches;
        if (!view_matches || !equal_matrix(vp, entry.vp)) continue;
        if (!match) match = &entry;
        // A stationary camera has identical matrices in several source frames. Prefer the sibling of the captured left
        // eye when it has exactly these matrices, so CPU progress cannot turn a valid stationary pair into mismatched IDs.
        if (entry.tag.eye == 1 && entry.tag.frame == wanted_right) { match = &entry; break; }
        if (entry.tag.eye == 0 || !wanted_right) break;
    }
    if (match) {
        if (seen && observed.eye != match->tag.eye) {
            ++changes;
            if (changes <= 12) NXLOG_W(kTag, "different eye cameras in one image: first eye %d, later eye %d", observed.eye, match->tag.eye);
        }
        if (!seen) observed = match->tag;
        seen = true; return;
    }
    ++mismatches;
    if (same_view) {
        ++lens_mismatch;
        if (lens_mismatch <= 12) NXLOG_W(kTag, "same scene view with a different projection: sx %.3f sy %.3f, VP diagonal %.3f %.3f %.3f %.3f",
            view[0]*vp[0]+view[4]*vp[4]+view[8]*vp[8], view[1]*vp[1]+view[5]*vp[5]+view[9]*vp[9], vp[0],vp[5],vp[10],vp[15]);
    }
}
Tag render_resolve_tag(Tag intent) {
    Tag result = intent.world ? (seen ? observed : Tag{}) : intent;
    if (!intent.world) wanted_right = 0;
    else if (seen && result.eye == 0) wanted_right = result.frame;
    else if (seen && result.eye == 1 && result.frame == wanted_right) wanted_right = 0;
    static thread_local uint64_t matched = 0, skipped = 0, different = 0;
    if (intent.world) {
        if (seen) { ++matched; if (observed.eye != intent.eye || observed.frame != intent.frame) ++different; }
        else ++skipped;
        if ((matched + skipped) % 300 == 0)
            NXLOG_I(kTag, "camera/image match: %llu matched, %llu skipped, %llu differed from present-time camera; actual eye %d pair %llu",
                static_cast<unsigned long long>(matched), static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(different),
                result.eye, static_cast<unsigned long long>(result.frame));
        if ((matched + skipped) % 300 == 0) NXLOG_I(kTag, "scene camera audit: %llu G-buffer draws, %llu unmatched distinct cameras, %llu eye changes inside audited images, %llu same-view lens mismatches",
            static_cast<unsigned long long>(draws), static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(changes), static_cast<unsigned long long>(lens_mismatch));
    }
    seen = false; observed = {}; previous = false;
    audit_image = (++image_sequence % 60) == 0;
    return result;
}
} // namespace nxrt::xr

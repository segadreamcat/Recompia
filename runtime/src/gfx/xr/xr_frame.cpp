// SPDX-License-Identifier: GPL-3.0-or-later
// Headset-paced frame pump. A slow game reuses its last complete stereo pair with that pair's actual camera poses.
#include "xr/xr_internal.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::xr::detail {
namespace {
void poll_events() {
    State& st = s();
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (st.f.PollEvent(st.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev)->state;
            st.session_state = state;
            NXLOG_I(kTag, "session state %d", static_cast<int>(state));
            if (state == XR_SESSION_STATE_READY) {
                std::lock_guard<std::mutex> lk(st.fm);
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (ok(st.f.BeginSession(st.session, &bi), "xrBeginSession")) {
                    st.session_begun.store(true);
                    st.running.store(true);
                }
            } else if (state == XR_SESSION_STATE_STOPPING) {
                st.running.store(false);
                clear_pad();
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                st.running.store(false);
                st.exiting.store(true);
                clear_pad();
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto* change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&ev);
            if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                std::lock_guard<std::mutex> lk(st.gm);
                st.recenter_time = change->changeTime;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            st.running.store(false);
            st.exiting.store(true);
            clear_pad();
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    // A render may already own an image when STOPPING arrives. Let its submit and release finish before ending the session.
    if (st.session_state == XR_SESSION_STATE_STOPPING && st.session_begun.load()) {
        std::lock_guard<std::mutex> lk(st.fm);
        if (!st.acquired) {
            ok(st.f.EndSession(st.session), "xrEndSession");
            st.session_begun.store(false);
            st.published_pair = -1;
            st.panel_ns = 0;
            std::lock_guard<std::mutex> gl(st.gm);
            st.cur_eye = -1;
            for (auto& t : st.hist) t = {};
        }
    }
}

void locate_tracking(XrTime time) {
    State& st = s();
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = st.local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    if (XR_FAILED(st.f.LocateViews(st.session, &li, &vs, 2, &count, views)) || count != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) || !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) return;
    EyeView hands[2];
    if (st.actions_ready) for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.aim[h];
        XrActionStatePose action{XR_TYPE_ACTION_STATE_POSE};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(st.f.GetActionStatePose(st.session, &gi, &action)) && action.isActive &&
            XR_SUCCEEDED(st.f.LocateSpace(st.aim_space[h], st.local, time, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            XrView v{XR_TYPE_VIEW}; v.pose = loc.pose;
            hands[h] = to_eye(v, -1);
        }
    }
    std::lock_guard<std::mutex> lk(st.gm);
    if (st.recenter_time && time >= st.recenter_time) { ++st.reference_epoch; st.recenter_time = 0; }
    for (int e = 0; e < 2; ++e) {
        st.tracking[e] = to_eye(views[e], e);
        st.tracking[e].reference_epoch = st.reference_epoch;
        st.tracking[e].center[0] = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
        st.tracking[e].center[1] = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        st.tracking[e].center[2] = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
        st.hands[e] = hands[e];
    }
}

void submit_layers(const XrFrameState& fs, bool& panel_mode, XrPosef& panel_pose) {
    State& st = s();
    // Get tracking before fm: the game and image transport never need both locks at once.
    EyeView head;
    { std::lock_guard<std::mutex> lk(st.gm); head = st.tracking[0]; }
    std::lock_guard<std::mutex> lk(st.fm);
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1] = {};
    const uint64_t now = qpc::now_ns();
    // A single skipped camera hook must not replace the 3D world by a flat screenshot. Sustained menu frames may do so.
    const bool menu = st.panel_ns && (st.published_pair < 0 || (st.panel_ns > st.published_ns && now - st.published_ns > 250000000ull));
    if (fs.shouldRender && menu && st.panel.sc) {
        if ((!panel_mode || st.panel_epoch != head.reference_epoch) && head.valid) {
            st.panel_epoch = head.reference_epoch;
            const float x = head.quat[0], y = head.quat[1], z = head.quat[2], w = head.quat[3];
            const float angle = std::atan2(2.f * (w * y + x * z), 1.f - 2.f * (x * x + y * y));
            panel_pose = {};
            panel_pose.orientation.y = std::sin(angle * 0.5f);
            panel_pose.orientation.w = std::cos(angle * 0.5f);
            panel_pose.position = {head.center[0] - std::sin(angle) * panel_distance(), head.center[1], head.center[2] - std::cos(angle) * panel_distance()};
        }
        panel_mode = true;
        quad.space = st.local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.pose = panel_pose;
        quad.size = {panel_width(), panel_width() * static_cast<float>(st.panel.h) / static_cast<float>(st.panel.w)};
        quad.subImage.swapchain = st.panel.sc;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(st.panel.w), static_cast<int32_t>(st.panel.h)}};
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        ei.layerCount = 1;
    } else if (fs.shouldRender && st.published_pair >= 0) {
        panel_mode = false;
        for (int e = 0; e < 2; ++e) {
            const Tag& t = st.published_tags[e];
            const Swap& sw = st.swap[st.published_pair][e];
            std::memcpy(&views[e].pose.position, t.view.pos, sizeof t.view.pos);
            std::memcpy(&views[e].pose.orientation, t.view.quat, sizeof t.view.quat);
            views[e].fov = {std::atan(t.fov_ok ? t.fov[0] : t.view.tan_left), std::atan(t.fov_ok ? t.fov[1] : t.view.tan_right),
                            std::atan(t.fov_ok ? t.fov[2] : t.view.tan_up), std::atan(t.fov_ok ? t.fov[3] : t.view.tan_down)};
            views[e].subImage.swapchain = sw.sc;
            views[e].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(sw.w), static_cast<int32_t>(sw.h)}};
        }
        projection.space = st.local;
        projection.viewCount = 2;
        projection.views = views;
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        ei.layerCount = 1;
    }
    ei.layers = ei.layerCount ? layers : nullptr;
    std::lock_guard<std::mutex> queue_lock(*st.vk_queue_mutex);
    if (ok(st.f.EndFrame(st.session, &ei), "xrEndFrame")) ++st.frames_ended;
}
} // namespace

EyeView to_eye(const XrView& v, int eye) {
    EyeView e;
    e.valid = true; e.eye = eye;
    std::memcpy(e.pos, &v.pose.position, sizeof e.pos);
    std::memcpy(e.quat, &v.pose.orientation, sizeof e.quat);
    e.tan_left = std::tan(v.fov.angleLeft); e.tan_right = std::tan(v.fov.angleRight);
    e.tan_up = std::tan(v.fov.angleUp); e.tan_down = std::tan(v.fov.angleDown);
    return e;
}

void frame_loop() {
    State& st = s();
    bool panel_mode = false;
    XrPosef panel_pose{}; panel_pose.orientation.w = 1.f; panel_pose.position.z = -panel_distance();
    uint64_t log_ns = qpc::now_ns(), last_frames = 0, last_pairs = 0;
    while (!st.stop.load() && !st.exiting.load() && !st.failed.load()) {
        poll_events();
        if (!st.running.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (!ok(st.f.WaitFrame(st.session, &wi, &fs), "xrWaitFrame")) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        XrResult begin_result;
        { std::lock_guard<std::mutex> queue_lock(*st.vk_queue_mutex); begin_result = st.f.BeginFrame(st.session, &bi); }
        if (!ok(begin_result, "xrBeginFrame")) continue;
        read_actions();
        // Camera work precedes presentation by at least two game frames. Submission always retains this actual render pose.
        locate_tracking(fs.predictedDisplayTime + fs.predictedDisplayPeriod);
        submit_layers(fs, panel_mode, panel_pose);
        const uint64_t now = qpc::now_ns();
        if (now - log_ns >= 5000000000ull) {
            std::lock_guard<std::mutex> lk(st.fm);
            const double seconds = static_cast<double>(now - log_ns) * 1e-9;
            NXLOG_I(kTag, "frame pump %.1f Hz, new stereo pairs %.1f/s, mode %s, image age %.1f ms", (st.frames_ended - last_frames) / seconds,
                    (st.pairs_published - last_pairs) / seconds, panel_mode ? "panel" : "world", st.published_ns ? (now - st.published_ns) * 1e-6 : 0.0);
            log_ns = now; last_frames = st.frames_ended; last_pairs = st.pairs_published;
        }
    }
    clear_pad();
}
} // namespace nxrt::xr::detail

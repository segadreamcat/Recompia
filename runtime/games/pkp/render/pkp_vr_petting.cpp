// SPDX-License-Identifier: GPL-3.0-or-later
// Tracked contact feedback. Creature state, friendship and native interaction controls are left to the game.
#include "render/pkp_vr_petting.h"
#include "render/pkp_vr_camera.h"
#include "core/pkp_resident_census.h"
#include "core/pkp_settings.h"
#include "ui/pkp_menu.h"
#include "nxrt/qpc.h"
#include "nxrt/xr.h"
#include <algorithm>
#include <atomic>
#include <cmath>
namespace nxrt::games::pkp {
namespace {
struct State {
    uint64_t base = 0, sampled = 0, epoch = 0, feedback[2] = {}, touching[2] = {};
    std::atomic<bool> playing{false};
    bool previous_valid[2] = {};
    float previous[2][3] = {};
    resident_census::ActorPosition actors[64];
    size_t count = 0;
} g;
void frame(bool playing, double, play::GameFrame *) { g.playing = playing; }
void clear() {
    g.count = 0;
    g.touching[0] = g.touching[1] = 0;
    g.previous_valid[0] = g.previous_valid[1] = false;
}
void game_frame(::Ctx *c) {
    if (!xr::wanted())
        return;
    if (!xr::running() || !g.playing || menu_has_selection() || !settings::get_bool(settings::Key::VrPetting) ||
        !settings::get_bool(settings::Key::VrFirstPerson)) {
        clear();
        return;
    }
    const auto head = xr::head_view();
    if (!head.valid) {
        clear();
        return;
    }
    if (head.reference_epoch != g.epoch) {
        g.epoch = head.reference_epoch;
        clear();
    }
    const uint64_t now = qpc::now_ns();
    if (now - g.sampled > 50000000ull) {
        g.sampled = now;
        g.count = resident_census::actor_positions(c, g.base, g.actors, 64);
    }
    for (int hand = 0; hand < 2; ++hand) {
        const auto pose = xr::hand_view(hand);
        float point[3];
        if (!pose.grip.valid || !vr_camera::reference_point(pose.grip.pos, pose.grip.reference_epoch, point)) {
            g.touching[hand] = 0;
            g.previous_valid[hand] = false;
            continue;
        }
        uint64_t contact = 0;
        float nearest = 1e9f;
        for (size_t i = 0; i < g.count; ++i) {
            const auto &actor = g.actors[i];
            // A forgiving contact capsule around a live creature. Geometry feedback is approximate, not a mesh collision query.
            float dx = point[0] - actor.position[0], dz = point[2] - actor.position[2];
            float dy = point[1] - std::clamp(point[1], actor.position[1] + 20.f, actor.position[1] + 95.f);
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < 35.f * 35.f && d < nearest) {
                nearest = d;
                contact = actor.actor;
            }
        }
        float moved = 0;
        for (int j = 0; j < 3; ++j) {
            const float d = pose.grip.pos[j] - g.previous[hand][j];
            moved += d * d;

        }
        if (contact && now - g.feedback[hand] > 160000000ull &&
            (contact != g.touching[hand] || (g.previous_valid[hand] && moved > .0001f)) && pose.trigger < .2f && pose.squeeze < .5f) {
            for (int j=0; j<3; ++j) g.previous[hand][j]=pose.grip.pos[j];
            xr::haptic(hand, .13f, .035f);
            g.feedback[hand] = now;
        }
        g.touching[hand] = contact;
        g.previous_valid[hand] = true;
    }
}
void install(const FeatureInstall &in) { g.base = in.base; }
void remove() { clear(); g.base=g.sampled=g.epoch=0; g.playing.store(false); }
} // namespace
const Feature kVrPettingFeature = {
    .name = "vr_petting", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};
} // namespace nxrt::games::pkp

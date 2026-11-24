// SPDX-License-Identifier: GPL-3.0-or-later
// The character's native fade component hides every body part together without changing gameplay visibility reasons.
#include "render/pkp_vr_avatar.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {
constexpr uint64_t kFade = 0x1860, kFadeTarget = 0x27caac, kVisibilityReasons = 0x241c;
struct State { uint64_t base = 0, owned = 0; uint8_t alpha = 255; } g;
void install(const FeatureInstall& in) { g = {}; g.base = in.base; }
void remove() {
    if (guest::ok(g.owned, kFade + 16)) {
        guest::wr<uint8_t>(g.owned + kFade + 12, 0);
        guest::wr<uint8_t>(g.owned + kFade + 13, g.alpha);
    }
    g = {};
}
void fade(::Ctx* c, uint64_t player, uint8_t alpha) {
    const ::Ctx saved = *c;
    c->v[0].f[0] = 0.f;
    guest::call(c, g.base + kFadeTarget, player + kFade, alpha, 0, 0, 0, (c->x[31] - 0x400) & ~uint64_t{15});
    *c = saved;
}
bool native_hidden(uint64_t player) {
    return (guest::rd<uint8_t>(player + kVisibilityReasons) & 0xfc) ||
        guest::rd<uint8_t>(player + kVisibilityReasons + 1) ||
        (guest::rd<uint8_t>(player + kVisibilityReasons + 2) & 3);
}
}
void vr_avatar::update(::Ctx* c, bool hide) {
    const uint64_t mgr = guest::deref(g.base + layout::kPlayerManager);
    const uint64_t player = guest::deref(mgr + layout::kManagerPlayer);
    if (!guest::ok(player, kVisibilityReasons + 3)) { g.owned = 0; return; }
    if (g.owned && g.owned != player) g.owned = 0;
    if (hide) {
        if (!g.owned) {
            g.owned = player; g.alpha = guest::rd<uint8_t>(player + kFade + 13);
            fade(c, player, 0);
            NXLOG_I("vr", "first-person avatar: native body fade enabled");
        } else if (guest::rd<uint8_t>(player + kFade + 13) && (guest::rd<uint8_t>(player + kFade + 12) != 2 || guest::rd<uint8_t>(player + kFade + 14) != 0)) fade(c, player, 0);
    } else if (g.owned) {
        if (!native_hidden(player)) fade(c, player, g.alpha);
        g.owned = 0;
    }
}
const Feature kVrAvatarFeature = {.name="vr_avatar", .install=&install, .remove=&remove};
}

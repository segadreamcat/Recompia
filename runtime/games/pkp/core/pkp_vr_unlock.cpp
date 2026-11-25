// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR testing aid: with [vr] test_unlock_all = 1 every ability bit (ids 1..0xf9, layout::kSaveAbilities) is turned on in the loaded
// save once the world is up, again after each return to the title. Off by default and with no Settings row: a normal game never runs it.
// The bits are the save's own, so the next save keeps them; back the save up before testing with it.
#include "core/pkp_vr_unlock.h"

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {

constexpr uint64_t kPermissions = 0x8c;   // game state + : the permission mask's low 32 bits, 0 at the title (set once the world is up)
constexpr uint32_t kFirstId = 1, kLastId = 0xf9;

uint64_t g_base = 0;
bool g_done = false;

bool wanted() {
    static const bool v = nxrt::config_get_bool("vr", "test_unlock_all", false);
    return v;
}

void game_frame(::Ctx*) {
    if (!wanted()) return;
    const uint64_t state = guest::deref(g_base + layout::kGameState);
    if (!state || !guest::ok(state + kPermissions, 4) || !guest::rd<uint32_t>(state + kPermissions)) {
        g_done = false;   // the title or a load: apply again once the world is back
        return;
    }
    if (g_done) return;
    const uint64_t root = guest::deref(g_base + layout::kSaveRoot);
    if (!root || !guest::ok(root + layout::kSaveAbilities, 0x20)) return;
    uint8_t* bits = reinterpret_cast<uint8_t*>(root + layout::kSaveAbilities);
    unsigned added = 0;
    for (uint32_t id = kFirstId; id <= kLastId; ++id) {
        uint8_t& byte = bits[id >> 3];
        const uint8_t bit = static_cast<uint8_t>(1u << (id & 7));
        if (!(byte & bit)) byte |= bit, ++added;
    }
    g_done = true;
    NXLOG_I("vr", "test_unlock_all: %u abilities turned on (ids %u..%u)", added, kFirstId, kLastId);
}

void install(const FeatureInstall& in) {
    g_base = in.base;
    g_done = false;
    if (wanted()) NXLOG_W("vr", "test_unlock_all is on: every ability is turned on in the loaded save (testing only)");
}

void remove() { g_done = false; }

}  // namespace

const Feature kVrUnlockFeature = {.name = "vr_unlock", .install = &install, .remove = &remove, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp

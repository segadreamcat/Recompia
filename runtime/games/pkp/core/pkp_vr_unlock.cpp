// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR testing aid: with [vr] test_unlock_all = 1 every ability (ids 1..0xf9, layout::kSaveAbilities: moves such as Water Gun, and
// unlocks such as the creature list) is turned on in the loaded save once the world is up, again after each return to the title. Each
// goes through the game's own quest effect 0x19 "set" (0x1227fc, as co-op applies a shared ability), so whatever the game does when a
// step grants one runs too; a bit still off after it is set directly. Off by default and with no Settings row: a normal game never
// runs it. The bits are the save's own, so the next save keeps them; back the save up before testing with it.
#include "core/pkp_vr_unlock.h"

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

#include <cstring>

namespace nxrt::games::pkp {
namespace {

constexpr uint64_t kPermissions = 0x8c;   // game state + : the permission mask's low 32 bits, 0 at the title (set once the world is up)
constexpr uint32_t kFirstId = 1, kLastId = 0xf9;
constexpr uint64_t kAbilityToggleFn = 0x1227fc;   // quest effect 0x19: command +0 = 0x19, u16 id at +2, u16 value at +4 (1 sets)
constexpr unsigned kPerFrame = 16;                // abilities granted per game frame

alignas(8) uint8_t g_command[0x20] = {};   // the effect command, guest-readable
uint32_t g_next = kFirstId;

uint64_t g_base = 0;
bool g_done = false;

bool wanted() {
    static const bool v = nxrt::config_get_bool("vr", "test_unlock_all", false);
    return v;
}

void game_frame(::Ctx* c) {
    if (!wanted()) return;
    const uint64_t state = guest::deref(g_base + layout::kGameState);
    if (!state || !guest::ok(state + kPermissions, 4) || !guest::rd<uint32_t>(state + kPermissions)) {
        g_done = false;   // the title or a load: apply again once the world is back
        g_next = kFirstId;
        return;
    }
    if (g_done) return;
    const uint64_t root = guest::deref(g_base + layout::kSaveRoot);
    if (!root || !guest::ok(root + layout::kSaveAbilities, 0x20)) return;
    uint8_t* bits = reinterpret_cast<uint8_t*>(root + layout::kSaveAbilities);
    static unsigned granted, forced;
    if (g_next == kFirstId) granted = forced = 0;
    for (unsigned n = 0; n < kPerFrame && g_next <= kLastId; ++g_next) {
        const uint32_t id = g_next;
        uint8_t& byte = bits[id >> 3];
        const uint8_t bit = static_cast<uint8_t>(1u << (id & 7));
        if (byte & bit) continue;
        std::memset(g_command, 0, sizeof g_command);
        g_command[0] = 0x19;
        const uint16_t args[2] = {static_cast<uint16_t>(id), 1};
        std::memcpy(g_command + 2, args, sizeof args);
        guest::call(c, g_base + kAbilityToggleFn, reinterpret_cast<uint64_t>(g_command));
        ++n, ++granted;
        if (!(byte & bit)) byte |= bit, ++forced;
    }
    if (g_next <= kLastId) return;
    g_next = kFirstId;
    g_done = true;
    NXLOG_I("vr", "test_unlock_all: %u abilities granted through the game (%u set directly after), ids %u..%u", granted, forced, kFirstId, kLastId);
}

void install(const FeatureInstall& in) {
    g_base = in.base;
    g_done = false;
    if (wanted()) NXLOG_W("vr", "test_unlock_all is on: every ability is turned on in the loaded save (testing only)");
}

void remove() { g_done = false, g_next = kFirstId; }

}  // namespace

const Feature kVrUnlockFeature = {.name = "vr_unlock", .install = &install, .remove = &remove, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp

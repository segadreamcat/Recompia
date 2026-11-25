// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR testing aid: with [vr] test_unlock_all = 1 every move is put on the move wheel (fill_wheel) and every ability (ids 1..0xf9, layout::kSaveAbilities: moves such as Water Gun, and
// unlocks such as the creature list) is turned on in the loaded save once the world is up, again after each return to the title. Each
// goes through the game's own quest effect 0x19 "set" (0x1227fc, as co-op applies a shared ability), so whatever the game does when a
// step grants one runs too; a bit still off after it is set directly. Off by default and with no Settings row: a normal game never
// runs it. The bits are the save's own, so the next save keeps them; back the save up before testing with it.
#include "core/pkp_vr_unlock.h"

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

#include <algorithm>
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
bool g_wheel_done = false;

// The move wheel: 8 skill ids (1..60, 0 = empty) for player 0 at save root + kWheel; learning a move is what puts it there, so the
// ability bits alone leave the wheel as it was. The skill table of the master data (root + kSkillTable + 0x10 on data versions past
// 0x11b; its vfunc 0x38(0) gives the rows, their vfunc 0x30(id) a row whose +8 is a 0x30-byte record) gives each wheel move its slot:
// record +0x1e is the wheel position 1..8 (0 for the moves the game runs by context, such as Surf), +0x26 the move's own id (0 for
// the upgraded variants). Water Gun, Leafage, Rock Smash, Cut, Rototiller and the rest, in the game's own wheel order.
// The wheel also shows a slot only while the player owns that move: a byte per skill id at save root + kOwned + player * 0x3e0, at +0x2e8
// or +0x325 (which one the game reads depends on its mode), with +0x362 / +0x39f as the other way to count as owned.
constexpr uint64_t kWheel = 0x6d84ec, kMasterData = 0x35618f0, kSkillTable = 0x8d8;
constexpr uint64_t kOwned = 0x250c50, kOwnedA = 0x2e8, kOwnedB = 0x325;
constexpr uint32_t kSkillIds = 60;

// Puts every wheel move the skill table lists on the wheel: in its own slot when that is free, else in a free one.
void fill_wheel(::Ctx* c, uint64_t root) {
    if (!guest::ok(root + kWheel, 8)) return;
    const uint64_t db = guest::deref(g_base + kMasterData);
    if (!db || !guest::ok(db + 0x2b70, 4)) return;
    const uint32_t ver = guest::rd<uint32_t>(db + 0x2b70);
    const uint64_t table = guest::deref(db + (ver > 0x11b ? kSkillTable : 0) + 0x10);
    if (!table || !guest::ok(table, 8)) return;
    const uint64_t rows = guest::call(c, guest::deref(guest::deref(table) + 0x38), table, 0);
    if (!rows || !guest::ok(rows, 8)) return;
    const uint64_t get = guest::deref(guest::deref(rows) + 0x30);
    uint8_t want[8] = {};
    for (uint32_t id = 1; id <= kSkillIds; ++id) {
        const uint64_t row = guest::call(c, get, rows, id);
        if (!row || !guest::ok(row, 0x10)) continue;
        const uint64_t rec = guest::deref(row + 8);
        if (!rec || !guest::ok(rec, 0x30)) continue;
        const uint8_t pos = guest::rd<uint8_t>(rec + 0x1e);
        if (pos >= 1 && pos <= 8 && guest::rd<uint8_t>(rec + 0x26) == id && !want[pos - 1]) want[pos - 1] = static_cast<uint8_t>(id);
    }
    unsigned owned = 0;
    for (const uint8_t id : want) {   // player 0, the local player
        for (const uint64_t at : {root + kOwned + kOwnedA + id, root + kOwned + kOwnedB + id}) {
            if (!id || !guest::ok(at, 1) || guest::rd<uint8_t>(at)) continue;
            guest::wr<uint8_t>(at, 1);
            ++owned;
        }
    }
    if (owned) NXLOG_I("vr", "test_unlock_all: %u move ownership bytes set", owned);
    uint8_t* wheel = reinterpret_cast<uint8_t*>(root + kWheel);
    unsigned added = 0;
    for (int k = 0; k < 8; ++k) {
        const uint8_t id = want[k];
        if (!id || std::find(wheel, wheel + 8, id) != wheel + 8) continue;
        uint8_t* slot = !wheel[k] ? wheel + k : std::find(wheel, wheel + 8, uint8_t{0});
        if (slot == wheel + 8) break;
        *slot = id, ++added;
    }
    g_wheel_done = true;
    if (added)
        NXLOG_I("vr", "test_unlock_all: %u moves put on the wheel (now %u %u %u %u %u %u %u %u)", added, wheel[0], wheel[1], wheel[2], wheel[3],
                wheel[4], wheel[5], wheel[6], wheel[7]);
}

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
        g_wheel_done = false;
        // At the title the save is read in well before the world starts; turning the bits on then means the world is built with
        // every ability in place (a move list made when the stage starts sees them). Only once a save is there (some bit already on).
        const uint64_t root = guest::deref(g_base + layout::kSaveRoot);
        if (root && guest::ok(root + layout::kSaveAbilities, 0x20)) {
            uint8_t* bits = reinterpret_cast<uint8_t*>(root + layout::kSaveAbilities);
            bool any = false;
            for (int i = 0; i < 0x20; ++i) any |= bits[i] != 0;
            unsigned added = 0;
            for (uint32_t id = kFirstId; any && id <= kLastId; ++id) {
                const uint8_t bit = static_cast<uint8_t>(1u << (id & 7));
                if (!(bits[id >> 3] & bit)) bits[id >> 3] |= bit, ++added;
            }
            if (added) NXLOG_I("vr", "test_unlock_all: %u abilities turned on in the save read in at the title", added);
            if (any) fill_wheel(c, root);
        }
        return;
    }
    if (g_done) return;
    const uint64_t root = guest::deref(g_base + layout::kSaveRoot);
    if (!root || !guest::ok(root + layout::kSaveAbilities, 0x20)) return;
    if (!g_wheel_done) fill_wheel(c, root);
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

void remove() { g_done = false, g_wheel_done = false, g_next = kFirstId; }

}  // namespace

const Feature kVrUnlockFeature = {.name = "vr_unlock", .install = &install, .remove = &remove, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp

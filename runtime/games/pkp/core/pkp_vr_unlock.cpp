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

#include <cstdio>
#include <cstring>
#include <string>

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
bool g_dumped = false;

// The move wheel: 8 skill ids (1..60, 0 = empty) for player 0 at save root + kWheel. Skills come from the master data's skill table
// (master data root + kSkillTable + 0x10 on data versions past 0x11b): its vfunc 0x38(0) gives the rows, their vfunc 0x30(id) a row
// whose +8 is the record.
constexpr uint64_t kWheel = 0x6d84ec, kMasterData = 0x35618f0, kSkillTable = 0x8d8;

std::string hex_bytes(uint64_t a, size_t n) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", guest::rd<uint8_t>(a + i));
        out += b;
        if ((i & 3) == 3 && i + 1 < n) out += ' ';
    }
    return out;
}

// Testing aid: logs the wheel and every skill row once, so the moves can be put on the wheel by id.
void dump_skills(::Ctx* c, uint64_t root) {
    g_dumped = true;
    NXLOG_I("vr", "skill dump: wheel %s", hex_bytes(root + kWheel, 8).c_str());
    const uint64_t db = guest::deref(g_base + kMasterData);
    if (!db || !guest::ok(db + 0x2b70, 4)) return;
    const uint32_t ver = guest::rd<uint32_t>(db + 0x2b70);
    const uint64_t table = guest::deref(db + (ver > 0x11b ? kSkillTable : 0) + 0x10);
    NXLOG_I("vr", "skill dump: data version 0x%x, table 0x%llx", ver, static_cast<unsigned long long>(table));
    if (!table || !guest::ok(table, 8)) return;
    const uint64_t rows = guest::call(c, guest::deref(guest::deref(table) + 0x38), table, 0);
    if (!rows || !guest::ok(rows, 8)) return;
    const uint64_t get = guest::deref(guest::deref(rows) + 0x30);
    for (uint32_t id = 1; id <= 60; ++id) {
        const uint64_t row = guest::call(c, get, rows, id);
        if (!row || !guest::ok(row, 0x10)) continue;
        const uint64_t rec = guest::deref(row + 8);
        if (!rec || !guest::ok(rec, 0x60)) continue;
        std::string strs;
        for (uint64_t o = 0; o < 0x60; o += 8) {   // pointer fields that lead to text
            const uint64_t q = guest::rd<uint64_t>(rec + o);
            if (!guest::ok(q, 4)) continue;
            char t[48] = {};
            size_t n = 0;
            while (n + 1 < sizeof t && guest::ok(q + n, 1)) {
                const char ch = guest::rd<char>(q + n);
                if (ch < 32 || ch > 126) break;
                t[n++] = ch;
            }
            if (n >= 3) strs += " +" + std::to_string(o) + "=\"" + t + "\"";
        }
        NXLOG_I("vr", "skill dump: id %u rec %s%s", id, hex_bytes(rec, 0x60).c_str(), strs.c_str());
    }
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
        }
        return;
    }
    if (g_done) return;
    const uint64_t root = guest::deref(g_base + layout::kSaveRoot);
    if (!root || !guest::ok(root + layout::kSaveAbilities, 0x20)) return;
    if (!g_dumped) dump_skills(c, root);
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

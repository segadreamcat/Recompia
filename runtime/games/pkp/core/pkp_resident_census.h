// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only creature actor census used by the resident cap feature.
#pragma once
#include <cstdint>
#include <cstddef>
struct Ctx;
namespace nxrt::games::pkp::resident_census {
struct ActorPosition { uint64_t actor; float position[3]; };
// Current live actor positions via their native getter; excludes unlinked actors and invalid/off-map positions.
size_t actor_positions(::Ctx* ctx, uint64_t main_base, ActorPosition* out, size_t capacity);
void start(std::uint64_t main_base);
void tick();
void stop();
}

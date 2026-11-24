// SPDX-License-Identifier: GPL-3.0-or-later
// The game's 2D shown or hidden (nxrt/ui_layer.h): the latest request.
#include "nxrt/ui_layer.h"

#include <atomic>
#include <algorithm>

#include "nxrt/game.h"
#include "nxrt/game_render.h"

namespace nxrt::ui_layer {

namespace {
std::atomic<bool> g_hidden{false};
std::atomic<float> g_scale{1.f};
}  // namespace

bool supported() {
    const game::Render* r = game::info().render;
    return r && r->ui_after_picture;
}

void set_visible(bool v) {
    if (!v && !supported()) return;
    g_hidden.store(!v, std::memory_order_relaxed);
}

void set_scale(float value) { g_scale.store(supported() ? std::clamp(value, .2f, 1.f) : 1.f, std::memory_order_relaxed); }
float scale() { return g_scale.load(std::memory_order_relaxed); }

bool hidden() { return g_hidden.load(std::memory_order_relaxed); }

}  // namespace nxrt::ui_layer

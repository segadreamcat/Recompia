// SPDX-License-Identifier: GPL-3.0-or-later
// Identify the headset eye from the camera actually bound by the scene draw. Camera layout belongs to the game descriptor.
#include "nvn_replay_internal.h"
#include "nxrt/game.h"
#include "nxrt/game_render.h"
#include "xr/xr_vk.h"

namespace nxrt::nvn::be::gpu::replay {
void xr_gbuffer_note() {
    if (!xr::running()) return;
    static const game::CameraBlock* const camera = [] {
        const game::Render* render = game::info().render;
        return render ? render->camera : nullptr;
    }();
    if (!camera || camera->stage > 4 || camera->slot >= 16 || S.ncolor < camera->gbuffer_colors || !S.depth_tex || !S.stage[camera->stage]) return;
    const uint64_t bytes = (static_cast<uint64_t>(std::max(camera->view, camera->view_proj)) + 16) * sizeof(float);
    const GState::Bind& uniform = S.ubo[camera->stage][camera->slot];
    if (!uniform.addr || uniform.size < bytes) return;
    const uint8_t* data = host_ptr(uniform.addr);
    if (!data) return;
    xr::render_note_camera(reinterpret_cast<const float*>(data + camera->view * sizeof(float)),
                           reinterpret_cast<const float*>(data + camera->view_proj * sizeof(float)));
}
} // namespace nxrt::nvn::be::gpu::replay

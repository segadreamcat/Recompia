// SPDX-License-Identifier: GPL-3.0-or-later
// Reference-space meshes composited into a transport slot. Each slot owns its upload data until both queues finish.
#pragma once
#include "xr/xr_internal.h"
namespace nxrt::xr::detail {
void spatial_render(gfx::Device& device, VkCommandBuffer cb, int slot, int eye, StageImage& target, const Tag& tag, const VkImage* background, const VkRect2D* excluded, bool flip_y);   // ui_layer::kRegions each
void spatial_shutdown();
}

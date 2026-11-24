// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR: the graphics half of the OpenXR output (nxrt/xr.h has the overview). Called by the Vulkan device (instance and device
// creation) and by the NVN back end's present (the render thread).
#pragma once
#include <volk.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "nxrt/xr.h"

namespace nxrt::gfx { class Device; }

namespace nxrt::xr {

// vk_device.cpp, before vkCreateInstance: starts OpenXR when [vr] enabled = 1 and adds the instance extensions the runtime asks for.
void vk_instance_extensions(std::vector<const char*>& exts);
// After vkCreateInstance: the physical device the headset is attached to (VK_NULL_HANDLE: no preference).
VkPhysicalDevice vk_wanted_physical(VkInstance instance);
// Before vkCreateDevice: the device extensions the runtime asks for (only names the device offers are added by the caller's check).
void vk_device_extensions(std::vector<const char*>& exts);
// After vkCreateDevice: creates the session on this device and queue.
void vk_device_created(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family, uint32_t queue_index, std::mutex* queue_mutex);

// Before Vulkan device destruction: stop the frame pump and destroy OpenXR graphics resources.
void vk_shutdown(VkDevice device);
// Render-thread metadata travels with each present.
// The first matching scene draw identifies the actual eye. Missing matches skip capture rather than guessing an eye.
void render_note_camera(const float view[16], const float view_projection[16]);
Tag render_resolve_tag(Tag intent);
void render_set_tag(Tag t);
Tag render_tag();
// Records the copy of the presented picture (src, in SHADER_READ_ONLY_OPTIMAL, left in that layout) into the tagged eye's swapchain.
// crop: x, y, w, h in texels (w or h 0: the whole image). flip_y: the picture is stored bottom-up.
void render_record(gfx::Device& device, VkCommandBuffer cb, VkImage src, VkFormat src_format, uint32_t src_w, uint32_t src_h, const int32_t crop[4], bool flip_y);
// After the Vulkan submit worker has drained: signal image readiness on the game queue. OpenXR handoff belongs to the headset thread.
void render_after_submit();

}  // namespace nxrt::xr

// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR: the graphics half of the OpenXR output (nxrt/xr.h has the overview). Called by the Vulkan device (instance and device
// creation) and by the NVN back end's present (the render thread).
#pragma once
#include <volk.h>

#include <cstdint>
#include <vector>

#include "nxrt/xr.h"

namespace nxrt::xr {

// vk_device.cpp, before vkCreateInstance: starts OpenXR when [vr] enabled = 1 and adds the instance extensions the runtime asks for.
void vk_instance_extensions(std::vector<const char*>& exts);
// After vkCreateInstance: the physical device the headset is attached to (VK_NULL_HANDLE: no preference).
VkPhysicalDevice vk_wanted_physical(VkInstance instance);
// Before vkCreateDevice: the device extensions the runtime asks for (only names the device offers are added by the caller's check).
void vk_device_extensions(std::vector<const char*>& exts);
// After vkCreateDevice: creates the session on this device and queue.
void vk_device_created(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family, uint32_t queue_index);

// The render thread. The present path sets the tag of the present it runs (present_on_render_thread reads it back).
void render_set_tag(Tag t);
Tag render_tag();
// Records the copy of the presented picture (src, in SHADER_READ_ONLY_OPTIMAL, left in that layout) into the tagged eye's swapchain.
// crop: x, y, w, h in texels (w or h 0: the whole image). flip_y: the picture is stored bottom-up.
void render_record(VkCommandBuffer cb, VkImage src, VkFormat src_format, uint32_t src_w, uint32_t src_h, const int32_t crop[4], bool flip_y);
// After the present's submission: releases the swapchain image and, after the right eye, ends the OpenXR frame.
void render_after_submit();

}  // namespace nxrt::xr

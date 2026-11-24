// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan image transport. The compositor reads only a complete published eye pair; the next pair uses separate swapchains.
#include "xr/xr_internal.h"
#include <algorithm>
#include "nxrt/log.h"
#include "nxrt/qpc.h"
namespace nxrt::xr::detail {
bool is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
VkFormat unorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s().vk_physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void destroy_mid() {
    State& st = s();
    if (st.mid) vkDestroyImage(st.vk_device, st.mid, nullptr);
    if (st.mid_mem) vkFreeMemory(st.vk_device, st.mid_mem, nullptr);
    st.mid = VK_NULL_HANDLE;
    st.mid_mem = VK_NULL_HANDLE;
    st.mid_w = st.mid_h = 0;
}

bool ensure_mid(VkFormat fmt, uint32_t w, uint32_t h) {
    State& st = s();
    if (st.mid && st.mid_format == fmt && st.mid_w == w && st.mid_h == h) return true;
    vkDeviceWaitIdle(st.vk_device);   // a size change: the old image may still be in use
    destroy_mid();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(st.vk_device, &ci, nullptr, &st.mid) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(st.vk_device, st.mid, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(st.vk_device, &ai, nullptr, &st.mid_mem) != VK_SUCCESS ||
        vkBindImageMemory(st.vk_device, st.mid, st.mid_mem, 0) != VK_SUCCESS) {
        destroy_mid();
        return false;
    }
    st.mid_format = fmt;
    st.mid_w = w, st.mid_h = h;
    return true;
}

void destroy_swap(Swap& sw) {
    if (sw.sc) s().f.DestroySwapchain(sw.sc);
    sw = Swap{};
}

bool ensure_swap(Swap& sw, uint32_t w, uint32_t h) {
    State& st = s();
    if (sw.sc && sw.w == w && sw.h == h) return true;
    if (sw.acquired) return false;
    vkDeviceWaitIdle(st.vk_device);
    destroy_swap(sw);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = st.sc_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!ok(st.f.CreateSwapchain(st.session, &ci, &sw.sc), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    st.f.EnumerateSwapchainImages(sw.sc, 0, &n, nullptr);
    sw.images.assign(n, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    st.f.EnumerateSwapchainImages(sw.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(sw.images.data()));
    sw.w = w, sw.h = h;
    NXLOG_I(kTag, "eye swapchain %ux%u, %u images, format %lld", w, h, n, static_cast<long long>(st.sc_format));
    return true;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}


void destroy_images() {
    for (auto& pair : s().swap) for (auto& eye : pair) destroy_swap(eye);
    destroy_swap(s().panel);
    destroy_mid();
}
} // namespace nxrt::xr::detail
namespace nxrt::xr {
using namespace detail;
namespace { thread_local Tag t_render_tag; }
void render_set_tag(Tag t) {
    t_render_tag = t;
    if (t.eye != 0) return;
    std::lock_guard<std::mutex> lk(s().fm);
    s().writing_frame = t.frame;
    s().recorded[0] = s().recorded[1] = false;
}
Tag render_tag() { return t_render_tag; }
void render_record(VkCommandBuffer cb, VkImage src, VkFormat src_format, uint32_t src_w, uint32_t src_h, const int32_t crop[4], bool flip_y) {
    State& st = s();
    const Tag t = t_render_tag;
    if (t.eye < 0 || t.eye > 1 || !running()) return;
    std::lock_guard<std::mutex> lk(st.fm);
    if (!st.session_begun.load() || !st.running.load() || st.acquired) return;
    std::lock_guard<std::mutex> queue_lock(*st.vk_queue_mutex);
    if (t.world && (!t.view.valid || st.writing_frame != t.frame)) return;

    const int32_t cx = crop && crop[2] > 0 ? crop[0] : 0, cy = crop && crop[3] > 0 ? crop[1] : 0;
    const uint32_t w = crop && crop[2] > 0 ? static_cast<uint32_t>(crop[2]) : src_w;
    const uint32_t h = crop && crop[3] > 0 ? static_cast<uint32_t>(crop[3]) : src_h;
    if (!w || !h) return;
    Swap& sw = t.world ? st.swap[st.writing_pair][t.eye] : st.panel;
    if (!ensure_swap(sw, w, h)) return;
    const VkFormat sc_fmt = static_cast<VkFormat>(st.sc_format);
    // an sRGB picture is copied as it is stored; a UNORM one (sRGB-encoded values) goes in bit for bit through the UNORM partner
    const VkFormat mid_fmt = is_srgb(src_format) ? sc_fmt : unorm_of(sc_fmt);
    if (!ensure_mid(mid_fmt, w, h)) return;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo aqi{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!ok(st.f.AcquireSwapchainImage(sw.sc, &aqi, &idx), "xrAcquireSwapchainImage")) return;
    sw.acquired = true;
    st.acquired = &sw;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!ok(st.f.WaitSwapchainImage(sw.sc, &wi), "xrWaitSwapchainImage")) { give_up("eye swapchain wait failed"); return; }
    sw.waited = true;
    const VkImage dst = sw.images[idx].image;

    barrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cb, st.mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    const int32_t y0 = flip_y ? cy + static_cast<int32_t>(h) : cy, y1 = flip_y ? cy : cy + static_cast<int32_t>(h);
    blit.srcOffsets[0] = {cx, y0, 0};
    blit.srcOffsets[1] = {cx + static_cast<int32_t>(w), y1, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    barrier(cb, st.mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = cp.srcSubresource;
    cp.extent = {w, h, 1};
    vkCmdCopyImage(cb, st.mid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (t.world) { st.recorded[t.eye] = true; st.tags[t.eye] = t; }
    else st.panel_recorded = true;
}


void render_after_submit() {
    State& st = s();
    const Tag t = t_render_tag;
    t_render_tag = {};
    if (t.eye < 0) return;
    std::lock_guard<std::mutex> lk(st.fm);
    if (st.acquired) {
        Swap& sw = *st.acquired;
        if (!sw.waited) return;
        std::lock_guard<std::mutex> queue_lock(*st.vk_queue_mutex);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!ok(st.f.ReleaseSwapchainImage(sw.sc, &ri), "xrReleaseSwapchainImage")) { give_up("eye image release failed"); return; }
        sw.acquired = sw.waited = false;
        st.acquired = nullptr;
    }
    if (st.panel_recorded) { st.panel_ns = qpc::now_ns(); st.panel_recorded = false; }
    if (t.eye != 1 || t.frame != st.writing_frame || !st.recorded[0] || !st.recorded[1]) return;
    const auto& pair = st.swap[st.writing_pair];
    if (pair[0].w != pair[1].w || pair[0].h != pair[1].h) return;
    st.published_pair = st.writing_pair;
    st.writing_pair ^= 1;
    st.published_tags[0] = st.tags[0]; st.published_tags[1] = st.tags[1];
    st.published_ns = qpc::now_ns();
    st.recorded[0] = st.recorded[1] = false;
    if (++st.pairs_published == 1)
        NXLOG_I(kTag, "first complete stereo pair published (%ux%u per eye); independent compositor, exact rendered poses", pair[0].w, pair[0].h);
}
} // namespace nxrt::xr

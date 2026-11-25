// SPDX-License-Identifier: GPL-3.0-or-later
// Three-slot image handoff. Game queue writes private staging images; only the headset thread touches OpenXR images or its queue.
#include "xr/xr_internal.h"
#include "xr/xr_spatial.h"
#include "nxrt/ui_layer.h"
#include <algorithm>
#include <cmath>
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxrt::gfx { bool save_png_rgba(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h); }

namespace nxrt::xr::detail {
namespace {
uint32_t memory_type(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s().vk_physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    return UINT32_MAX;
}
VkFormat stage_format(VkFormat source) {
    const VkFormat target = static_cast<VkFormat>(s().sc_format);
    const bool srgb = source == VK_FORMAT_R8G8B8A8_SRGB || source == VK_FORMAT_B8G8R8A8_SRGB;
    if (!srgb && target == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
    if (!srgb && target == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
    return target;
}
void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags dest) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = source; b.dstAccessMask = dest; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
// The left-eye screenshot (request_eye_shot): the request, the readback recorded in render_record and the marker value it waits on.
struct EyeShot {
    std::mutex m;
    std::string path;
    uint64_t requested_ns = 0, ready = 0;
    bool recorded = false, busy = false;
    gfx::Device* device = nullptr;
    gfx::Buffer buffer;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, crop[4] = {};
    std::atomic<int> state{0};
} g_shot;
void destroy_stage(StageImage& image) {
    if (image.view) vkDestroyImageView(s().vk_device, image.view, nullptr);
    if (image.image) vkDestroyImage(s().vk_device, image.image, nullptr);
    if (image.memory) vkFreeMemory(s().vk_device, image.memory, nullptr);
    image = {};
}
bool ensure_stage(StageImage& image, VkFormat format, uint32_t w, uint32_t h, uint64_t written, uint64_t completed) {
    State& st = s();
    if (image.image && image.format == format && image.w == w && image.h == h) return true;
    if (written > completed) return false; // resizing never destroys an image whose game-queue write is still in flight
    destroy_stage(image);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (vkCreateImage(st.vk_device, &ci, nullptr, &image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(st.vk_device, image.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = memory_type(mr.memoryTypeBits);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(st.vk_device, &ai, nullptr, &image.memory) != VK_SUCCESS ||
        vkBindImageMemory(st.vk_device, image.image, image.memory, 0) != VK_SUCCESS) { destroy_stage(image); return false; }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=image.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=format;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(st.vk_device,&view,nullptr,&image.view)!=VK_SUCCESS){destroy_stage(image);return false;}
    image.format = format; image.w = w; image.h = h;
    return true;
}
void destroy_swap(Swap& swap) {
    if (swap.sc) s().f.DestroySwapchain(swap.sc);
    swap = {};
}
bool ensure_swap(Swap& swap, uint32_t w, uint32_t h) {
    State& st = s();
    if (swap.sc && swap.w == w && swap.h == h) return true;
    vkQueueWaitIdle(st.xr_queue);
    destroy_swap(swap);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = st.sc_format; ci.sampleCount = ci.faceCount = ci.arraySize = ci.mipCount = 1; ci.width = w; ci.height = h;
    if (!ok(st.f.CreateSwapchain(st.session, &ci, &swap.sc), "xrCreateSwapchain")) return false;
    uint32_t count = 0;
    if (!ok(st.f.EnumerateSwapchainImages(swap.sc, 0, &count, nullptr), "xrEnumerateSwapchainImages")) return false;
    swap.images.assign(count, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (!ok(st.f.EnumerateSwapchainImages(swap.sc, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(swap.images.data())), "xrEnumerateSwapchainImages")) return false;
    swap.w = w; swap.h = h;
    NXLOG_I(kTag, "headset swapchain %ux%u, %u images", w, h, count);
    return count != 0;
}
bool ensure_command(Stage& stage) {
    State& st = s();
    if (stage.pool) return vkResetCommandPool(st.vk_device, stage.pool, 0) == VK_SUCCESS;
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = st.queue_family;
    if (vkCreateCommandPool(st.vk_device, &ci, nullptr, &stage.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = stage.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    return vkAllocateCommandBuffers(st.vk_device, &ai, &stage.command) == VK_SUCCESS;
}
} // namespace

bool create_transport() {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; ci.pNext = &type;
    return vkCreateSemaphore(s().vk_device, &ci, nullptr, &s().ready) == VK_SUCCESS &&
           vkCreateSemaphore(s().vk_device, &ci, nullptr, &s().consumed) == VK_SUCCESS;
}

// Called only by the headset frame loop. No handoff mutex is held while waiting for OpenXR or submitting work.
void transport_frame() {
    State& st = s();
    int chosen = -1;
    {
        std::lock_guard<std::mutex> lk(st.fm);
        for (int i = 0; i < State::kStages; ++i)
            if (st.stages[i].state == ImageState::Ready && (chosen < 0 || st.stages[i].ready_value > st.stages[chosen].ready_value)) chosen = i;
        if (chosen < 0) return;
        for (int i = 0; i < State::kStages; ++i)
            if (i != chosen && st.stages[i].state == ImageState::Ready) st.stages[i].state = ImageState::Free;
        st.stages[chosen].state = ImageState::Reading;
        st.stages[chosen].consumed_value = ++st.consumed_counter;
    }
    Stage& stage = st.stages[chosen];
    const bool world = stage.world;
    const int count = world ? 2 : 1;
    const Tag tags[2] = {stage.tags[0], stage.tags[1]};
    const uint32_t width = stage.image[0].w, height = stage.image[0].h;
    std::lock_guard<std::mutex> queue_lock(*st.vk_queue_mutex);
    if (!ensure_command(stage)) { give_up("headset command allocation failed"); return; }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(stage.command, &begin) != VK_SUCCESS) { give_up("headset command begin failed"); return; }
    for (int e = 0; e < count; ++e) {
        StageImage& image = stage.image[e];
        Swap& swap = world ? st.swap[e] : st.panel;
        if (!ensure_swap(swap, image.w, image.h)) { give_up("headset swapchain creation failed"); return; }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!ok(st.f.AcquireSwapchainImage(swap.sc, &ai, &index), "xrAcquireSwapchainImage")) { give_up("headset image acquire failed"); return; }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        if (!ok(st.f.WaitSwapchainImage(swap.sc, &wi), "xrWaitSwapchainImage")) { give_up("headset image wait failed"); return; }
        const VkImage dest = swap.images[index].image;
        // The destination is completely overwritten, including on the first use of each runtime image.
        barrier(stage.command, dest, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.extent = {image.w, image.h, 1};
        vkCmdCopyImage(stage.command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(stage.command, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    if (vkEndCommandBuffer(stage.command) != VK_SUCCESS) { give_up("headset command end failed"); return; }
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = st.ready; wait.value = stage.ready_value; wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = st.consumed; signal.value = stage.consumed_value; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = stage.command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = submit.signalSemaphoreInfoCount = submit.commandBufferInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait; submit.pSignalSemaphoreInfos = &signal; submit.pCommandBufferInfos = &command;
    if (vkQueueSubmit2(st.xr_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) { give_up("headset transfer submit failed"); return; }
    for (int e = 0; e < count; ++e) {
        Swap& swap = world ? st.swap[e] : st.panel;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!ok(st.f.ReleaseSwapchainImage(swap.sc, &ri), "xrReleaseSwapchainImage")) { give_up("headset image release failed"); return; }
    }
    if (world) {
        st.published_tags[0] = tags[0]; st.published_tags[1] = tags[1];
        st.published_pair = 0; st.published_ns = qpc::now_ns();
        if (++st.pairs_published == 1) NXLOG_I(kTag, "first complete stereo pair published (%ux%u); camera-driven eyes, separate image transport", width, height);
    } else st.panel_ns = qpc::now_ns();
}

void destroy_images() {
    State& st = s();
    spatial_shutdown();
    st.sharpener.reset();
    st.sharpener_tried = st.sharpener_logged = false;
    for (auto& stage : st.stages) {
        for (auto& image : stage.image) destroy_stage(image);
        if (stage.pool) vkDestroyCommandPool(st.vk_device, stage.pool, nullptr);
    }
    for (auto& swap : st.swap) destroy_swap(swap);
    destroy_swap(st.panel);
    if (st.ready) vkDestroySemaphore(st.vk_device, st.ready, nullptr);
    if (st.consumed) vkDestroySemaphore(st.vk_device, st.consumed, nullptr);
}
} // namespace nxrt::xr::detail

namespace nxrt::xr {
using namespace detail;
namespace { thread_local Tag t_render_tag; }
void render_set_tag(Tag tag) { t_render_tag = render_resolve_tag(tag); }
Tag render_tag() { return t_render_tag; }

void render_record(gfx::Device& device, VkCommandBuffer cb, VkImage source, VkFormat source_format, uint32_t source_w, uint32_t source_h, const int32_t crop[4], bool flip_y, const VkImage* background, const VkRect2D* background_rect) {
    State& st = s(); const Tag tag = t_render_tag;
    if (tag.eye < 0 || tag.eye > 1 || !running()) return;
    uint64_t consumed = 0, completed = 0;
    if (vkGetSemaphoreCounterValue(st.vk_device, st.consumed, &consumed) != VK_SUCCESS ||
        vkGetSemaphoreCounterValue(st.vk_device, st.ready, &completed) != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lk(st.fm);
    st.capture_stage = st.capture_eye = -1;
    for (auto& stage : st.stages)
        if (stage.state == ImageState::Reading && stage.consumed_value <= consumed) stage.state = ImageState::Free;
    const int32_t x = crop && crop[2] > 0 ? crop[0] : 0, y = crop && crop[3] > 0 ? crop[1] : 0;
    const uint32_t w = crop && crop[2] > 0 ? static_cast<uint32_t>(crop[2]) : source_w;
    const uint32_t h = crop && crop[3] > 0 ? static_cast<uint32_t>(crop[3]) : source_h;
    if (!w || !h || (tag.world && (!tag.view.valid || tag.frame <= st.captured_frame))) return;
    if (tag.world && tag.fov_ok && (st.logged_eye_w != w || st.logged_eye_h != h)) {
        st.logged_eye_w = w; st.logged_eye_h = h;
        const float fx = tag.fov[1] - tag.fov[0], fy = tag.fov[2] - tag.fov[3];
        if (fx > 0.f && fy > 0.f) NXLOG_I(kTag, "eye detail: source %ux%u, headset-visible region about %.0fx%.0f pixels; rendered tangents %.3f %.3f %.3f %.3f",
            w, h, w * (tag.view.tan_right-tag.view.tan_left)/fx, h * (tag.view.tan_up-tag.view.tan_down)/fy, tag.fov[0], tag.fov[1], tag.fov[2], tag.fov[3]);
    }
    if (st.writing_stage >= 0) {
        Stage& previous = st.stages[st.writing_stage];
        if (!tag.world || previous.frame != tag.frame) { previous.state = ImageState::Free; st.writing_stage = -1; }
    }
    int slot = tag.world ? st.writing_stage : -1;
    if (slot < 0) {
        if (tag.world && tag.eye != 0) return;
        for (int i = 0; i < State::kStages; ++i) if (st.stages[i].state == ImageState::Free && st.stages[i].ready_value <= completed) { slot = i; break; }
        if (slot < 0) return; // no available transport slot: skip capture without stalling the game
        Stage& stage = st.stages[slot];
        if (stage.consumed_value) {
            VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            wait.semaphore = st.consumed; wait.value = stage.consumed_value; wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.waitSemaphoreInfoCount = 1; submit.pWaitSemaphoreInfos = &wait;
            std::lock_guard<std::mutex> queue_lock(*st.game_queue_mutex);
            if (vkQueueSubmit2(st.game_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) { give_up("staging reuse wait failed"); return; }
        }
        stage.state = ImageState::Writing; stage.world = tag.world; stage.frame = tag.frame;
        stage.written[0] = stage.written[1] = false;
        if (tag.world) st.writing_stage = slot;
    }
    Stage& stage = st.stages[slot];
    const int eye = tag.world ? tag.eye : 0;
    StageImage& image = stage.image[eye];
    if (!ensure_stage(image, stage_format(source_format), w, h, stage.ready_value, completed)) {
        if (!tag.world) stage.state = ImageState::Free;
        return;
    }
    barrier(cb, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cb, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {x, flip_y ? y + static_cast<int32_t>(h) : y, 0};
    blit.srcOffsets[1] = {x + static_cast<int32_t>(w), flip_y ? y : y + static_cast<int32_t>(h), 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    vkCmdBlitImage(cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    // The desktop output pass runs after this capture. Apply its existing RCAS filter here to affect actual headset pixels.
    const float sharpness = st.sharpness.load(std::memory_order_relaxed);
    if (tag.world && sharpness > 0.f) {
        if (!st.sharpener_tried) {
            st.sharpener_tried = true;
            st.sharpener = std::make_unique<extras::Fsr1Spatial>();
            if (!st.sharpener->init(device)) { st.sharpener.reset(); NXLOG_W(kTag, "eye sharpening unavailable; retaining the unfiltered image"); }
        }
        if (st.sharpener && st.sharpener->sharpen_enlarged(cb, image.image, {w, h}, {w, h}, false, 1.f, sharpness)) {
            const VkImage filtered = st.sharpener->output_image();
            barrier(cb, filtered, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit sharpened{}; sharpened.srcSubresource = sharpened.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            sharpened.srcOffsets[1] = sharpened.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
            vkCmdBlitImage(cb, filtered, VK_IMAGE_LAYOUT_GENERAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sharpened, VK_FILTER_NEAREST);
            barrier(cb, filtered, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            if (!st.sharpener_logged) { st.sharpener_logged = true; NXLOG_I(kTag, "eye sharpening: native %ux%u, RCAS %.2f before headset transfer", w, h, sharpness); }
        }
    }
    // Each lifted UI region in the eye image's coordinates (top-left, inside the crop).
    VkImage backgrounds[ui_layer::kRegions]{};
    VkRect2D excluded[ui_layer::kRegions]{};
    for (int r = 0; background && background_rect && r < ui_layer::kRegions; ++r) {
        if (!background[r]) continue;
        backgrounds[r] = background[r];
        excluded[r] = background_rect[r];
        excluded[r].offset.x -= x;
        excluded[r].offset.y = flip_y ? y + static_cast<int32_t>(h) - background_rect[r].offset.y - static_cast<int32_t>(excluded[r].extent.height)
                                      : background_rect[r].offset.y - y;
    }
    spatial_render(device, cb, slot, eye, image, tag, backgrounds, excluded, flip_y);
    if (tag.world && eye == 0) {   // a requested left-eye screenshot: copy the finished eye image out (it is in TRANSFER_SRC here)
        std::lock_guard<std::mutex> shot_lock(g_shot.m);
        const bool bgra = image.format == VK_FORMAT_B8G8R8A8_SRGB || image.format == VK_FORMAT_B8G8R8A8_UNORM;
        const bool rgba = image.format == VK_FORMAT_R8G8B8A8_SRGB || image.format == VK_FORMAT_R8G8B8A8_UNORM;
        if (!g_shot.path.empty() && !g_shot.busy && !g_shot.recorded && (bgra || rgba) &&
            device.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &g_shot.buffer,
                                 "vr eye shot") == VK_SUCCESS) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shot.buffer.buffer, 1, &region);
            VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.buffer = g_shot.buffer.buffer; hb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
            g_shot.recorded = true;
            g_shot.device = &device;
            g_shot.format = image.format;
            g_shot.w = w, g_shot.h = h;
            // the lens's visible region inside the wider rendered picture (tangents: left, right, up, down)
            uint32_t x0 = 0, x1 = w, y0 = 0, y1 = h;
            const float fx = tag.fov[1] - tag.fov[0], fy = tag.fov[2] - tag.fov[3];
            if (tag.fov_ok && fx > 0 && fy > 0) {
                auto clampu = [](float v, uint32_t n) { return static_cast<uint32_t>(std::clamp(v, 0.f, static_cast<float>(n))); };
                x0 = clampu((tag.view.tan_left - tag.fov[0]) / fx * w, w), x1 = clampu((tag.view.tan_right - tag.fov[0]) / fx * w, w);
                y0 = clampu((tag.fov[2] - tag.view.tan_up) / fy * h, h), y1 = clampu((tag.fov[2] - tag.view.tan_down) / fy * h, h);
                if (x1 <= x0 + 16 || y1 <= y0 + 16) x0 = 0, x1 = w, y0 = 0, y1 = h;
            }
            g_shot.crop[0] = x0, g_shot.crop[1] = y0, g_shot.crop[2] = x1 - x0, g_shot.crop[3] = y1 - y0;
        }
    }
    st.capture_stage = slot; st.capture_eye = eye; st.capture_tag = tag;
}

void render_after_submit() {
    State& st = s(); t_render_tag = {};
    if (st.capture_stage < 0) return;
    // This marker is queued after the present's command buffers, following submit_drain in the caller. The headset queue waits on it.
    const uint64_t ready = ++st.ready_counter;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = st.ready; signal.value = ready; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signal;
    { std::lock_guard<std::mutex> queue_lock(*st.game_queue_mutex);
      if (vkQueueSubmit2(st.game_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) { give_up("game image marker submit failed"); return; } }
    {   // a recorded left-eye screenshot: read it back on a worker once this marker has passed
        std::lock_guard<std::mutex> shot_lock(g_shot.m);
        if (g_shot.recorded && !g_shot.busy) {
            g_shot.recorded = false;
            g_shot.busy = true;
            g_shot.ready = ready;
            std::thread([vk = st.vk_device, sem = st.ready]() {
                std::string path;
                gfx::Buffer buffer;
                gfx::Device* device;
                VkFormat format;
                uint32_t w, h, crop[4];
                uint64_t value;
                {
                    std::lock_guard<std::mutex> l(g_shot.m);
                    path = g_shot.path, buffer = g_shot.buffer, device = g_shot.device, format = g_shot.format;
                    w = g_shot.w, h = g_shot.h, value = g_shot.ready;
                    std::copy(std::begin(g_shot.crop), std::end(g_shot.crop), crop);
                }
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1; wi.pSemaphores = &sem; wi.pValues = &value;
                bool ok = vkWaitSemaphores(vk, &wi, 5000000000ull) == VK_SUCCESS && buffer.mapped;
                if (ok) {
                    device->invalidate_buffer(buffer);
                    const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
                    const uint8_t* src = static_cast<const uint8_t*>(buffer.mapped);
                    std::vector<uint8_t> out(static_cast<size_t>(crop[2]) * crop[3] * 4);
                    for (uint32_t y = 0; y < crop[3]; ++y)
                        for (uint32_t x = 0; x < crop[2]; ++x) {
                            const uint8_t* p = src + (static_cast<size_t>(y + crop[1]) * w + x + crop[0]) * 4;
                            uint8_t* q = out.data() + (static_cast<size_t>(y) * crop[2] + x) * 4;
                            q[0] = bgra ? p[2] : p[0], q[1] = p[1], q[2] = bgra ? p[0] : p[2], q[3] = 255;
                        }
                    ok = gfx::save_png_rgba(path, out.data(), crop[2], crop[3]);
                    if (ok) NXLOG_I(kTag, "eye screenshot %ux%u written to %s", crop[2], crop[3], path.c_str());
                }
                if (!ok) NXLOG_W(kTag, "eye screenshot could not be written");
                std::lock_guard<std::mutex> l(g_shot.m);
                device->destroy_buffer(g_shot.buffer);
                g_shot.buffer = {};
                g_shot.path.clear();
                g_shot.busy = false;
                g_shot.state.store(ok ? 2 : 3);
            }).detach();
        }
    }
    std::lock_guard<std::mutex> lk(st.fm);
    Stage& stage = st.stages[st.capture_stage]; const int eye = st.capture_eye;
    stage.ready_value = ready; stage.written[eye] = true; stage.tags[eye] = st.capture_tag;
    if (!stage.world || (stage.written[0] && stage.written[1])) {
        stage.state = ImageState::Ready;
        if (stage.world) { st.captured_frame = stage.frame; st.writing_stage = -1; }
    }
    st.capture_stage = st.capture_eye = -1;
}
} // namespace nxrt::xr

namespace nxrt::xr {
bool request_eye_shot(const char* png_path) {
    using detail::g_shot;
    std::lock_guard<std::mutex> l(g_shot.m);
    if (!png_path || g_shot.busy || g_shot.recorded || !g_shot.path.empty()) {
        // a request no world-mode eye took within a second is dropped (a menu or the title is showing)
        if (!g_shot.busy && !g_shot.recorded && !g_shot.path.empty() && qpc::now_ns() - g_shot.requested_ns > 1000000000ull) {
            g_shot.path.clear();
            g_shot.state.store(3);
        }
        return false;
    }
    g_shot.path = png_path;
    g_shot.requested_ns = qpc::now_ns();
    g_shot.state.store(1);
    return true;
}
int eye_shot_state() {
    using detail::g_shot;
    std::lock_guard<std::mutex> l(g_shot.m);
    if (!g_shot.busy && !g_shot.recorded && !g_shot.path.empty() && qpc::now_ns() - g_shot.requested_ns > 1000000000ull) {
        g_shot.path.clear();
        g_shot.state.store(3);
    }
    return g_shot.state.load();
}
}  // namespace nxrt::xr


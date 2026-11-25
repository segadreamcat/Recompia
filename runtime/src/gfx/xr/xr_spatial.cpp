// SPDX-License-Identifier: GPL-3.0-or-later
// Application-owned tracked geometry and live picture patches, independent of
// the game's renderer and assets.
#include "xr/xr_spatial.h"
#include "nxrt/ui_layer.h"
#include "extras/vk_make.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
namespace nxrt::xr {
namespace {
using namespace detail;
#include "xr/xr_spatial_shaders.inc"
struct Vertex {
    float clip[4], color[4], uv[3];
};
struct Slot {
    gfx::Buffer vertices;
    gfx::Image picture, depth;
};
struct SpatialState {
    std::mutex mutex;
    SpatialScene pending;
    SpatialScene pairs[State::kStages];
    Slot slots[State::kStages * 2];
    gfx::Device *dev = nullptr;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[State::kStages * 2] = {};
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
} g;
void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
bool init(gfx::Device &device, VkFormat format) {
    if (g.dev)
        return g.pipeline && g.format == format;
    g.dev = &device;
    g.format = format;
    VkDevice d = device.device();
    g.vs = extras::make_module(d, kSpatialVert, sizeof kSpatialVert);
    g.fs = extras::make_module(d, kSpatialFrag, sizeof kSpatialFrag);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 1;
    sl.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(d, &sl, nullptr, &g.set_layout) != VK_SUCCESS)
        return false;
    vk_gauge_descriptor_set_layouts().add(1);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.set_layout;
    if (vkCreatePipelineLayout(d, &pl, nullptr, &g.layout) != VK_SUCCESS)
        return false;
    vk_gauge_pipeline_layouts().add(1);
    if (!extras::make_clamp_sampler(d, VK_FILTER_LINEAR, &g.sampler) || !extras::make_sampler_pool(d, State::kStages * 2, &g.pool))
        return false;
    VkDescriptorSetLayout layouts[State::kStages * 2];
    for (auto &l : layouts)
        l = g.set_layout;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool;
    ai.descriptorSetCount = State::kStages * 2;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(d, &ai, g.sets) != VK_SUCCESS)
        return false;
    vk_gauge_descriptor_sets().add(State::kStages * 2);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}, {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 32}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &ba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.depthAttachmentFormat=VK_FORMAT_D16_UNORM;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    pi.pDepthStencilState=&depth;
    pi.pColorBlendState = &bs;
    pi.pDynamicState = &ds;
    pi.layout = g.layout;
    if (!g.vs || !g.fs || vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pi, nullptr, &g.pipeline) != VK_SUCCESS)
        return false;
    vk_gauge_pipelines().add(1);
    NXLOG_I("vr", "tracked geometry renderer ready, %u vertices per pair", SpatialScene::kMaxVertices);
    return true;
}
std::array<float, 3> local(const float q[4], const float p[3]) {
    const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    const float tx = 2 * (y * p[2] - z * p[1]), ty = 2 * (z * p[0] - x * p[2]), tz = 2 * (x * p[1] - y * p[0]);
    return {p[0] + w * tx + y * tz - z * ty, p[1] + w * ty + z * tx - x * tz, p[2] + w * tz + x * ty - y * tx};
}
} // namespace
void set_spatial_scene(const SpatialScene &scene) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.pending.count = std::min(scene.count, SpatialScene::kMaxVertices) / 3 * 3;
    std::copy_n(scene.vertices, g.pending.count, g.pending.vertices);
    std::copy_n(scene.picture_rect, 8, g.pending.picture_rect);
}
namespace detail {
constexpr int32_t kPicture = 256;   // side of one band of the live picture image
void spatial_render(gfx::Device &device, VkCommandBuffer cb, int slot, int eye, StageImage &target, const Tag &tag, const VkImage *background, const VkRect2D *excluded, bool flip_y) {
    if (!tag.world)
        return;
    if (eye == 0) {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.pairs[slot] = g.pending;
    }
    const SpatialScene &scene = g.pairs[slot];
    if (!scene.count || !init(device, target.format))
        return;
    const int index = slot * 2 + eye;
    Slot &out = g.slots[index];
    const VkDevice d = device.device();
    if (!out.vertices && device.create_buffer(sizeof(Vertex) * SpatialScene::kMaxVertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                              gfx::MemoryUse::Upload, &out.vertices, "XR spatial vertices") != VK_SUCCESS)
        return;
    if (!out.picture) {
        gfx::ImageDesc ci;
        ci.width = kPicture;
        ci.height = kPicture * 3;   // bands: picture 1, picture 2, picture 2's background
        ci.format = target.format;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.name = "XR live picture patch";
        if (device.create_image(ci, &out.picture) != VK_SUCCESS)
            return;
    }
    if(!out.depth || out.depth.extent.width!=target.w || out.depth.extent.height!=target.h){
        if(out.depth)device.destroy_image(out.depth);
        gfx::ImageDesc ci;ci.width=target.w;ci.height=target.h;ci.format=VK_FORMAT_D16_UNORM;
        ci.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;ci.name="XR view-model depth";
        if(device.create_image(ci,&out.depth)!=VK_SUCCESS)return;
    }
    barrier(cb, out.picture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto band = [&](int n, VkImage src, const VkOffset3D &a, const VkOffset3D &b) {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = a;
        blit.srcOffsets[1] = b;
        blit.dstOffsets[0] = {0, kPicture * n, 0};
        blit.dstOffsets[1] = {kPicture, kPicture * (n + 1), 1};
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.picture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    };
    const auto from_target = [&](const float *rect, VkOffset3D &a, VkOffset3D &b) {
        if (!(rect[2] > 0 && rect[3] > 0)) return false;
        a = {static_cast<int32_t>(std::clamp(rect[0], 0.f, 1.f) * target.w), static_cast<int32_t>(std::clamp(rect[1], 0.f, 1.f) * target.h), 0};
        b = {static_cast<int32_t>(std::clamp(rect[0] + rect[2], 0.f, 1.f) * target.w),
             static_cast<int32_t>(std::clamp(rect[1] + rect[3], 0.f, 1.f) * target.h), 1};
        return b.x > a.x && b.y > a.y;
    };
    {
        VkClearColorValue white{{1, 1, 1, 1}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, out.picture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = out.picture.image;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    VkOffset3D a{}, b{};
    if (from_target(scene.picture_rect, a, b)) band(0, target.image, a, b);
    // Picture 2 and its background: with no captured background both bands hold the same pixels, so nothing of it is drawn.
    if (from_target(scene.picture_rect + 4, a, b)) {
        band(1, target.image, a, b);
        const VkRect2D &e = excluded[1];
        if (background && background[1] && e.extent.width && e.extent.height) {
            // the background holds the region with its margin; take just the box's pixels (rows run the other way when flipped)
            const int32_t w = static_cast<int32_t>(e.extent.width), h = static_cast<int32_t>(e.extent.height);
            const int32_t x0 = std::clamp(a.x - e.offset.x, 0, w), x1 = std::clamp(b.x - e.offset.x, 0, w);
            const int32_t y0 = std::clamp(a.y - e.offset.y, 0, h), y1 = std::clamp(b.y - e.offset.y, 0, h);
            if (x1 > x0 && y1 > y0)
                band(2, background[1], {x0, flip_y ? h - y0 : y0, 0}, {x1, flip_y ? h - y1 : y1, 1});
            else
                band(2, target.image, a, b);
        } else {
            band(2, target.image, a, b);
        }
    }
    barrier(cb, out.picture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    // The lifted UI regions are painted over with the world they covered.
    for (int r = 0; background && excluded && r < ui_layer::kRegions; ++r) {
        const VkRect2D &e = excluded[r];
        if (!background[r] || !e.extent.width || !e.extent.height || e.offset.x < 0 || e.offset.y < 0 || e.offset.x + e.extent.width > target.w ||
            e.offset.y + e.extent.height > target.h)
            continue;
        barrier(cb,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit bl{};bl.srcSubresource=bl.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        const int w=static_cast<int>(e.extent.width),h=static_cast<int>(e.extent.height);
        bl.srcOffsets[0]={0,flip_y?h:0,0};bl.srcOffsets[1]={w,flip_y?0:h,1};
        bl.dstOffsets[0]={e.offset.x,e.offset.y,0};bl.dstOffsets[1]={e.offset.x+w,e.offset.y+h,1};
        vkCmdBlitImage(cb,background[r],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_NEAREST);
        barrier(cb,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    }
    struct Tri {
        Vertex v[3];
        float distance;
    };
    std::vector<Tri> triangles;
    triangles.reserve(scene.count / 3);
    const float *f = tag.fov_ok ? tag.fov : nullptr;
    const float left = f ? f[0] : tag.view.tan_left, right = f ? f[1] : tag.view.tan_right, up = f ? f[2] : tag.view.tan_up,
                down = f ? f[3] : tag.view.tan_down;
    for (uint32_t i = 0; i + 2 < scene.count; i += 3) {
        Tri tri{};
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
            const auto &v = scene.vertices[i + k];
            float p[3] = {v.pos[0] - tag.view.pos[0], v.pos[1] - tag.view.pos[1], v.pos[2] - tag.view.pos[2]};
            const auto a = local(tag.view.quat, p);
            const float z = -a[2];
            if (!std::isfinite(z) || z < .025f) {
                valid = false;
                break;
            }
            auto &t = tri.v[k];
            t.clip[0] = (2 * a[0] - (right + left) * z) / (right - left);
            t.clip[1] = (-2 * a[1] + (up + down) * z) / (up - down);
            t.clip[2] = z - .02f;
            t.clip[3] = z;
            std::copy_n(v.color, 4, t.color);
            t.uv[0] = v.uv[0];
            t.uv[1] = v.picture > 1.5f ? (1 + v.uv[1]) / 3 : v.picture > .5f ? v.uv[1] / 3 : v.uv[1];   // the picture's band
            t.uv[2] = v.picture;
            tri.distance += z;
        }
        if (valid)
            triangles.push_back(tri);
    }
    std::stable_sort(triangles.begin(), triangles.end(), [](const Tri &a, const Tri &b) { return a.distance > b.distance; });
    auto *vertices = static_cast<Vertex *>(out.vertices.mapped);
    uint32_t count = 0;
    for (const auto &tri : triangles)
        for (const auto &v : tri.v)
            vertices[count++] = v;
    if (!count)
        return;
    VkDescriptorImageInfo image{g.sampler, out.picture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = g.sets[index];
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &image;
    vkUpdateDescriptorSets(d, 1, &wr, 0, nullptr);
    barrier(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkImageMemoryBarrier db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    db.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;db.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex=db.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    db.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.image=out.depth.image;db.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,nullptr,0,nullptr,1,&db);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};depth.imageView=out.depth.view;
    depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;depth.clearValue.depthStencil={1.f,0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.pDepthAttachment=&depth;
    ri.renderArea = {{0, 0}, {target.w, target.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.sets[index], 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &out.vertices.buffer, &offset);
    VkViewport vp{0, 0, static_cast<float>(target.w), static_cast<float>(target.h), 0, 1};
    VkRect2D sc{{0, 0}, {target.w, target.h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, count, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}
void spatial_shutdown() {
    if (!g.dev)
        return;
    VkDevice d = g.dev->device();
    for (auto &slot : g.slots) {
        if (slot.depth) g.dev->destroy_image(slot.depth);
        if (slot.vertices)
            g.dev->destroy_buffer(slot.vertices);
        if (slot.picture)
            g.dev->destroy_image(slot.picture);
        slot = {};
    }
    if (g.pipeline) {
        vkDestroyPipeline(d, g.pipeline, nullptr);
        vk_gauge_pipelines().sub(1);
    }
    if (g.layout) {
        vkDestroyPipelineLayout(d, g.layout, nullptr);
        vk_gauge_pipeline_layouts().sub(1);
    }
    if (g.pool) {
        vkDestroyDescriptorPool(d, g.pool, nullptr);
        vk_gauge_descriptor_pools().sub(1);
    }
    if (g.sets[0])
        vk_gauge_descriptor_sets().sub(State::kStages * 2);
    if (g.set_layout) {
        vkDestroyDescriptorSetLayout(d, g.set_layout, nullptr);
        vk_gauge_descriptor_set_layouts().sub(1);
    }
    if (g.sampler) {
        vkDestroySampler(d, g.sampler, nullptr);
        vk_gauge_samplers().sub(1);
    }
    if (g.vs) {
        vkDestroyShaderModule(d, g.vs, nullptr);
        vk_gauge_shader_modules().sub(1);
    }
    if (g.fs) {
        vkDestroyShaderModule(d, g.fs, nullptr);
        vk_gauge_shader_modules().sub(1);
    }
    g.dev = nullptr;
    g.pipeline = VK_NULL_HANDLE;
    g.layout = VK_NULL_HANDLE;
    g.pool = VK_NULL_HANDLE;
    g.set_layout = VK_NULL_HANDLE;
    g.sampler = VK_NULL_HANDLE;
    g.vs = g.fs = VK_NULL_HANDLE;
    std::fill(std::begin(g.sets), std::end(g.sets), VK_NULL_HANDLE);
}
} // namespace detail
} // namespace nxrt::xr

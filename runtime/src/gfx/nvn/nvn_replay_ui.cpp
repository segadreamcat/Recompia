// SPDX-License-Identifier: GPL-3.0-or-later
// The game's 2D left out of the picture while code asks for it (nxrt/ui_layer.h, #572): the textures the game presents, and the draw rule.
// Render thread only (the replay and the present both run there).
#include "nvn_replay_internal.h"
#include "nxrt/ui_layer.h"
#include <algorithm>
#include <cmath>

namespace nxrt::nvn::be::gpu {

namespace {
// The last textures presented. The game presents its window textures in turn (two or three), so these are exactly those within a few
// frames. Held by reference so a slot never names a texture object that was freed and made again at the same address.
struct Presented {
    static constexpr size_t kSlots = 4;
    Ref<Texture> tex[kSlots];
    struct Background { gfx::Image image; VkRect2D region{}; bool pending=true, captured=false; } background[kSlots][ui_layer::kRegions];
    size_t next = 0;
};
Presented g_presented;
}  // namespace

void replay_note_presented(Texture* t) {
    if (!t) return;
    for(size_t i=0;i<Presented::kSlots;++i)
        if(g_presented.tex[i].get()==t){for(auto& bg:g_presented.background[i])bg.pending=true;return;}
    const size_t index=g_presented.next++%Presented::kSlots;
    for(auto& bg:g_presented.background[index]){
        if(bg.image){device().wait_idle();device().destroy_image(bg.image);}
        bg={};
    }
    g_presented.tex[index]=Ref<Texture>::share(t);
}

bool replay_ui_background(Texture* t, VkImage* image, VkRect2D* region) {
    bool any=false;
    for(int r=0;r<ui_layer::kRegions;++r){
        image[r]=VK_NULL_HANDLE;region[r]={};
        const auto request=ui_layer::background_region(r);if(request.w<=0||request.h<=0)continue;
        for(size_t i=0;i<Presented::kSlots;++i){const auto& bg=g_presented.background[i][r];
            if(bg.captured&&bg.image&&shares_image(t,g_presented.tex[i].get())){image[r]=bg.image.image;region[r]=bg.region;any=true;break;}
        }
    }
    return any;
}

namespace replay {
void ui_capture_background(const ShaderInfo* vs) {
    if(!ui_is_draw(vs)||!F)return;
    Texture* target=S.color_tex[0].get();
    for(size_t i=0;i<Presented::kSlots;++i)for(int r=0;r<ui_layer::kRegions;++r){
        const auto request=ui_layer::background_region(r);if(request.w<=0||request.h<=0)continue;
        auto& bg=g_presented.background[i][r];
        if(!bg.pending||!shares_image(target,g_presented.tex[i].get()))continue;
        const auto extent=tex_image_extent(target,0);if(!extent.width||!extent.height)return;
        const int x0=std::clamp(static_cast<int>(std::floor(request.x*extent.width)),0,static_cast<int>(extent.width));
        const int x1=std::clamp(static_cast<int>(std::ceil((request.x+request.w)*extent.width)),x0,static_cast<int>(extent.width));
        const int y0=std::clamp(static_cast<int>(std::floor(request.y*extent.height)),0,static_cast<int>(extent.height));
        const int y1=std::clamp(static_cast<int>(std::ceil((request.y+request.h)*extent.height)),y0,static_cast<int>(extent.height));
        const uint32_t w=static_cast<uint32_t>(x1-x0),h=static_cast<uint32_t>(y1-y0);if(!w||!h)continue;
        const VkFormat format=tex_vk_format(target);
        if(!bg.image||bg.image.extent.width!=w||bg.image.extent.height!=h||bg.image.format!=format){
            if(bg.image){device().wait_idle();device().destroy_image(bg.image);}
            gfx::ImageDesc desc;desc.width=w;desc.height=h;desc.format=format;
            desc.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;desc.name="UI background patch";
            if(device().create_image(desc,&bg.image)!=VK_SUCCESS)continue;
            bg.captured=false;
        }
        end_pass();tex_transition(target,*F,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=bg.image.image;
        b.oldLayout=bg.captured?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask=bg.captured?VK_ACCESS_TRANSFER_READ_BIT:0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(F->cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        const int storage_y=window_origin_mode()==NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT?static_cast<int>(extent.height)-y1:y0;
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.srcOffset={x0,storage_y,0};copy.extent={w,h,1};
        vkCmdCopyImage(F->cb,tex_image(target),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,bg.image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(F->cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        bg.region={{x0,storage_y},{w,h}};bg.captured=true;bg.pending=false;
    }
}


bool ui_is_draw(const ShaderInfo* vs) {
    if (!vs || vs->refl.vertex_inputs().empty() || S.ncolor == 0 || !S.color_tex[0] || g_xfb.active) return false;
    Texture* target = S.color_tex[0].get();
    for (const Ref<Texture>& r : g_presented.tex)
        if (shares_image(target, r.get())) return true;
    return false;
}

bool ui_skip_draw(const ShaderInfo* vs) { return ui_layer::hidden() && ui_is_draw(vs); }

void ui_shutdown() {
    for(auto& slot:g_presented.background)for(auto& bg:slot)if(bg.image)device().destroy_image(bg.image);
    g_presented=Presented();
}

}  // namespace replay
}  // namespace nxrt::nvn::be::gpu

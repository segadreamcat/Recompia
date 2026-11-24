// SPDX-License-Identifier: GPL-3.0-or-later
// OpenXR implementation state. Tracking and game-camera snapshots use gm; published images and session transitions use fm.
#pragma once
#include "xr/xr_vk.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define XR_NO_PROTOTYPES 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
namespace nxrt::xr::detail {
inline constexpr const char* kTag = "vr";
struct Fns {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance CreateInstance = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrGetSystem GetSystem = nullptr;
    PFN_xrGetSystemProperties GetSystemProperties = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrCreateActionSpace CreateActionSpace = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrStringToPath StringToPath = nullptr;
    PFN_xrCreateActionSet CreateActionSet = nullptr;
    PFN_xrCreateAction CreateAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets AttachSessionActionSets = nullptr;
    PFN_xrSyncActions SyncActions = nullptr;
    PFN_xrGetActionStateBoolean GetActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat GetActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f GetActionStateVector2f = nullptr;
    PFN_xrGetActionStatePose GetActionStatePose = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrDestroyActionSet DestroyActionSet = nullptr;
    PFN_xrRequestExitSession RequestExitSession = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR = nullptr;
};

// ---- the controllers --------------------------------------------------------------------------------------------------------------
enum ActBool { kA, kB, kX, kY, kMenu, kLClick, kRClick, kBoolCount };
enum ActFloat { kLTrig, kRTrig, kLGrip, kRGrip, kFloatCount };
enum ActVec { kLStick, kRStick, kVecCount };


struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
    uint32_t w = 0, h = 0;
    bool acquired = false;
    bool waited = false;
};
struct State {
    bool wanted = false;
    std::atomic<bool> failed{false}, running{false}, session_begun{false}, exiting{false}, stop{false};
    void* lib = nullptr;
    Fns f;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    std::vector<std::string> inst_ext_names, dev_ext_names;
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkPhysicalDevice vk_physical = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    std::mutex* vk_queue_mutex = nullptr;
    int64_t sc_format = 0;
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    std::thread pump;
    XrActionSet set = XR_NULL_HANDLE;
    XrAction act_bool[kBoolCount] = {};
    XrAction act_float[kFloatCount] = {};
    XrAction act_vec[kVecCount] = {};
    XrAction aim[2] = {};
    XrSpace aim_space[2] = {};
    bool actions_ready = false;
    std::atomic<uint64_t> pad_buttons{0};
    std::atomic<int32_t> pad_lx{0}, pad_ly{0}, pad_rx{0}, pad_ry{0};

    std::mutex gm;
    EyeView tracking[2], hands[2], cur_views[2], cur_hands[2];
    uint64_t reference_epoch = 0, panel_epoch = 0;
    XrTime recenter_time = 0;
    static constexpr int kHist = 4;
    Tag hist[kHist];
    bool wait_due = false, world_this_frame = false, fov_this_frame = false;
    float fov_frame[4] = {};
    int cur_eye = -1;
    uint64_t cur_frame = 0, next_id = 0;

    std::mutex fm;
    // A complete pair remains untouched while the next pair is filled. No tracking-ring slots are referenced by queued presents.
    Swap swap[2][2], panel;
    int writing_pair = 0, published_pair = -1;
    uint64_t writing_frame = 0, published_ns = 0, panel_ns = 0;
    bool recorded[2] = {};
    Tag tags[2], published_tags[2];
    Swap* acquired = nullptr;
    bool panel_recorded = false;
    VkImage mid = VK_NULL_HANDLE;
    VkDeviceMemory mid_mem = VK_NULL_HANDLE;
    VkFormat mid_format = VK_FORMAT_UNDEFINED;
    uint32_t mid_w = 0, mid_h = 0;
    uint64_t pairs_published = 0, frames_ended = 0;
};
State& s();
bool ok(XrResult r, const char* what);
void give_up(const char* why);
bool make_actions();
void read_actions();
void clear_pad();
EyeView to_eye(const XrView& v, int eye);
void frame_loop();
void destroy_images();
} // namespace nxrt::xr::detail

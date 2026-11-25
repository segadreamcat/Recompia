// SPDX-License-Identifier: GPL-3.0-or-later
// OpenXR loader, Vulkan session creation, configuration and teardown. Frame submission and image transport are separate modules.
#include "xr/xr_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace nxrt::xr::detail {
State& s() { static State st; return st; }
bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    NXLOG_W(kTag, "%s failed (%d)", what, static_cast<int>(r));
    return false;
}
void give_up(const char* why) {
    if (s().failed.exchange(true)) return;
    s().running.store(false);
    NXLOG_W(kTag, "VR off: %s (the game runs on the monitor only)", why);
}
// ---- loading ----------------------------------------------------------------------------------------------------------------------
void* open_lib(const std::string& path) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void* lib_sym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

template <class T>
bool load_fn(XrInstance inst, const char* name, T* out) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(s().f.GetInstanceProcAddr(inst, name, &fn)) || !fn) {
        NXLOG_W(kTag, "the OpenXR runtime has no %s", name);
        return false;
    }
    *out = reinterpret_cast<T>(fn);
    return true;
}

bool load_loader() {
    State& st = s();
    std::string want = nxrt::config_get_str("vr", "loader", "");
#ifdef _WIN32
    const char* names[] = {"openxr_loader.dll"};
#else
    const char* names[] = {"libopenxr_loader.so.1", "libopenxr_loader.so"};
#endif
    if (!want.empty()) st.lib = open_lib(want);
    for (const char* n : names)
        if (!st.lib) st.lib = open_lib(n);
    if (!st.lib) return false;
    st.f.GetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(lib_sym(st.lib, "xrGetInstanceProcAddr"));
    if (!st.f.GetInstanceProcAddr) return false;
    return load_fn(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &st.f.EnumerateInstanceExtensionProperties) &&
           load_fn(XR_NULL_HANDLE, "xrCreateInstance", &st.f.CreateInstance);
}

bool load_instance_fns() {
    State& st = s();
    XrInstance i = st.instance;
    Fns& f = st.f;
    return load_fn(i, "xrDestroyInstance", &f.DestroyInstance) && load_fn(i, "xrGetSystem", &f.GetSystem) &&
           load_fn(i, "xrGetSystemProperties", &f.GetSystemProperties) &&
           load_fn(i, "xrEnumerateViewConfigurationViews", &f.EnumerateViewConfigurationViews) && load_fn(i, "xrCreateSession", &f.CreateSession) &&
           load_fn(i, "xrDestroySession", &f.DestroySession) && load_fn(i, "xrBeginSession", &f.BeginSession) &&
           load_fn(i, "xrEndSession", &f.EndSession) && load_fn(i, "xrPollEvent", &f.PollEvent) &&
           load_fn(i, "xrCreateReferenceSpace", &f.CreateReferenceSpace) && load_fn(i, "xrCreateActionSpace", &f.CreateActionSpace) &&
           load_fn(i, "xrLocateSpace", &f.LocateSpace) && load_fn(i, "xrWaitFrame", &f.WaitFrame) &&
           load_fn(i, "xrBeginFrame", &f.BeginFrame) && load_fn(i, "xrEndFrame", &f.EndFrame) &&
           load_fn(i, "xrLocateViews", &f.LocateViews) && load_fn(i, "xrEnumerateSwapchainFormats", &f.EnumerateSwapchainFormats) &&
           load_fn(i, "xrCreateSwapchain", &f.CreateSwapchain) && load_fn(i, "xrDestroySwapchain", &f.DestroySwapchain) &&
           load_fn(i, "xrEnumerateSwapchainImages", &f.EnumerateSwapchainImages) &&
           load_fn(i, "xrAcquireSwapchainImage", &f.AcquireSwapchainImage) &&
           load_fn(i, "xrWaitSwapchainImage", &f.WaitSwapchainImage) &&
           load_fn(i, "xrReleaseSwapchainImage", &f.ReleaseSwapchainImage) && load_fn(i, "xrStringToPath", &f.StringToPath) &&
           load_fn(i, "xrCreateActionSet", &f.CreateActionSet) && load_fn(i, "xrCreateAction", &f.CreateAction) &&
           load_fn(i, "xrSuggestInteractionProfileBindings", &f.SuggestInteractionProfileBindings) &&
           load_fn(i, "xrAttachSessionActionSets", &f.AttachSessionActionSets) && load_fn(i, "xrSyncActions", &f.SyncActions) &&
           load_fn(i, "xrGetActionStateBoolean", &f.GetActionStateBoolean) &&
           load_fn(i, "xrGetActionStateFloat", &f.GetActionStateFloat) &&
           load_fn(i, "xrGetActionStateVector2f", &f.GetActionStateVector2f) &&
           load_fn(i, "xrGetActionStatePose", &f.GetActionStatePose) &&
           load_fn(i, "xrDestroySpace", &f.DestroySpace) && load_fn(i, "xrDestroyActionSet", &f.DestroyActionSet) &&
           load_fn(i, "xrRequestExitSession", &f.RequestExitSession) &&
           load_fn(i, "xrApplyHapticFeedback", &f.ApplyHapticFeedback) &&
           load_fn(i, "xrGetVulkanInstanceExtensionsKHR", &f.GetVulkanInstanceExtensionsKHR) &&
           load_fn(i, "xrGetVulkanDeviceExtensionsKHR", &f.GetVulkanDeviceExtensionsKHR) &&
           load_fn(i, "xrGetVulkanGraphicsDeviceKHR", &f.GetVulkanGraphicsDeviceKHR) &&
           load_fn(i, "xrGetVulkanGraphicsRequirementsKHR", &f.GetVulkanGraphicsRequirementsKHR);
}

// Splits the runtime's space-separated extension list into names that live as long as the state.
void split_names(const std::string& list, std::vector<std::string>* out) {
    std::istringstream in(list);
    std::string n;
    while (in >> n) out->push_back(n);
}

// Starts OpenXR up to the system (instance, HMD). False: no VR this run.
bool start() {
    State& st = s();
    if (!load_loader()) {
        give_up("no OpenXR loader (put openxr_loader.dll next to the game executable, or set [vr] loader)");
        return false;
    }
    uint32_t n = 0;
    st.f.EnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    st.f.EnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    bool has_vk = false;
    for (auto& p : props) has_vk = has_vk || !std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (!has_vk) {
        give_up("the OpenXR runtime does not offer XR_KHR_vulkan_enable");
        return false;
    }
    const char* exts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", game::info().product);
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "nxrt");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    if (!ok(st.f.CreateInstance(&ici, &st.instance), "xrCreateInstance")) {
        give_up("no OpenXR instance (is SteamVR / Virtual Desktop / the Oculus app running?)");
        return false;
    }
    if (!load_instance_fns()) {
        give_up("the OpenXR runtime lacks a needed function");
        return false;
    }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(st.f.GetSystem(st.instance, &sgi, &st.system), "xrGetSystem")) {
        give_up("no headset found");
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(st.f.GetSystemProperties(st.instance, st.system, &sp)))
        NXLOG_I(kTag, "headset: %s (vendor 0x%x)", sp.systemName, static_cast<unsigned>(sp.vendorId));
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;
    if (XR_SUCCEEDED(st.f.EnumerateViewConfigurationViews(st.instance, st.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views)) && view_count == 2)
        NXLOG_I(kTag, "runtime recommends per-eye %ux%u / %ux%u pixels before lens distortion", views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
                views[1].recommendedImageRectWidth, views[1].recommendedImageRectHeight);
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    st.f.GetVulkanGraphicsRequirementsKHR(st.instance, st.system, &req);   // must be called before xrCreateSession
    uint32_t len = 0;
    st.f.GetVulkanInstanceExtensionsKHR(st.instance, st.system, 0, &len, nullptr);
    std::string list(len, '\0');
    if (len) st.f.GetVulkanInstanceExtensionsKHR(st.instance, st.system, len, &len, list.data());
    list.resize(std::strlen(list.c_str()));
    split_names(list, &st.inst_ext_names);
    NXLOG_I(kTag, "OpenXR up; Vulkan instance extensions it needs: %s", list.empty() ? "(none)" : list.c_str());
    return true;
}


} // namespace nxrt::xr::detail
namespace nxrt::xr {
using namespace detail;
// ---- configuration ---------------------------------------------------------------------------------------------------------------
float world_scale() {
    static const float v = static_cast<float>(nxrt::config_get_double("vr", "world_scale", 100.0));
    return v;
}
float head_height() {
    static const float v = static_cast<float>(nxrt::config_get_double("vr", "head_height", 120.0));
    return v;
}

float panel_distance() {
    static const float v = static_cast<float>(nxrt::config_get_double("vr", "panel_distance", 2.5));
    return v;
}
float panel_width() {
    static const float v = static_cast<float>(nxrt::config_get_double("vr", "panel_width", 3.2));
    return v;
}

bool mirror_x() {
    static const bool v = nxrt::config_get_bool("vr", "mirror_x", false);
    return v;
}

bool wanted() {
    static const bool v = nxrt::config_get_bool("vr", "enabled", false);
    return v;
}

bool active() { return s().session != XR_NULL_HANDLE && !s().failed; }
bool running() { return active() && s().running.load(std::memory_order_acquire); }

// ---- Vulkan bring-up -------------------------------------------------------------------------------------------------------------
void vk_instance_extensions(std::vector<const char*>& exts) {
    State& st = s();
    st.wanted = nxrt::config_get_bool("vr", "enabled", false);
    if (!st.wanted) return;
    NXLOG_I(kTag, "[vr] enabled: starting OpenXR");
    if (!start()) return;
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
    for (const std::string& name : st.inst_ext_names) {
        bool offered = false;
        for (auto& p : have) offered = offered || name == p.extensionName;
        bool listed = false;
        for (const char* e : exts) listed = listed || name == e;
        if (offered && !listed) exts.push_back(name.c_str());
        if (!offered) NXLOG_W(kTag, "Vulkan instance extension %s (asked for by OpenXR) is not offered", name.c_str());
    }
}

VkPhysicalDevice vk_wanted_physical(VkInstance instance) {
    State& st = s();
    if (st.failed || !st.instance) return VK_NULL_HANDLE;
    st.vk_instance = instance;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!ok(st.f.GetVulkanGraphicsDeviceKHR(st.instance, st.system, instance, &pd), "xrGetVulkanGraphicsDeviceKHR")) return VK_NULL_HANDLE;
    return pd;
}

void vk_device_extensions(std::vector<const char*>& exts) {
    State& st = s();
    if (st.failed || !st.instance) return;
    uint32_t len = 0;
    st.f.GetVulkanDeviceExtensionsKHR(st.instance, st.system, 0, &len, nullptr);
    std::string list(len, '\0');
    if (len) st.f.GetVulkanDeviceExtensionsKHR(st.instance, st.system, len, &len, list.data());
    list.resize(std::strlen(list.c_str()));
    st.dev_ext_names.clear();
    split_names(list, &st.dev_ext_names);
    NXLOG_I(kTag, "Vulkan device extensions OpenXR needs: %s", list.empty() ? "(none)" : list.c_str());
    for (const std::string& name : st.dev_ext_names) {
        bool listed = false;
        for (const char* e : exts) listed = listed || name == e;
        if (!listed) exts.push_back(name.c_str());
    }
}

void vk_device_created(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family, uint32_t queue_index, std::mutex* queue_mutex) {
    State& st = s();
    if (st.failed || !st.instance) return;
    st.vk_instance = instance;
    st.vk_physical = physical;
    st.vk_device = device;
    st.game_queue_mutex = queue_mutex;
    st.vk_queue_mutex = queue_index ? &st.xr_queue_mutex : queue_mutex;
    st.queue_family = queue_family;
    vkGetDeviceQueue(device, queue_family, 0, &st.game_queue);
    vkGetDeviceQueue(device, queue_family, queue_index, &st.xr_queue);
    NXLOG_I(kTag, "graphics queue 0, headset queue %u in family %u", queue_index, queue_family);
    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = instance;
    gb.physicalDevice = physical;
    gb.device = device;
    gb.queueFamilyIndex = queue_family;
    gb.queueIndex = queue_index;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &gb;
    ci.systemId = st.system;
    if (!ok(st.f.CreateSession(st.instance, &ci, &st.session), "xrCreateSession")) {
        st.session = XR_NULL_HANDLE;
        give_up("no OpenXR session on this GPU");
        return;
    }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.f;
    if (!ok(st.f.CreateReferenceSpace(st.session, &rs, &st.local), "xrCreateReferenceSpace")) {
        give_up("no LOCAL reference space");
        return;
    }
    st.actions_ready = make_actions();
    if (!st.actions_ready) NXLOG_W(kTag, "controllers unavailable (the gamepad and keyboard still work)");

    uint32_t n = 0;
    st.f.EnumerateSwapchainFormats(st.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    st.f.EnumerateSwapchainFormats(st.session, n, &n, formats.data());
    const int64_t prefer[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (int64_t p : prefer) {
        if (std::find(formats.begin(), formats.end(), p) != formats.end()) {
            st.sc_format = p;
            break;
        }
    }
    if (!st.sc_format) { give_up("no supported 8-bit eye format"); return; }
    if (!create_transport()) { give_up("could not create headset image transport"); return; }
    st.pump = std::thread(&frame_loop);
    NXLOG_I(kTag, "session created (queue family %u), eye format %lld; put the headset on", queue_family, static_cast<long long>(st.sc_format));
}


void vk_shutdown(VkDevice device) {
    State& st = s();
    if (!st.vk_device || st.vk_device != device) return;
    st.stop.store(true);
    if (st.session_begun.load()) st.f.RequestExitSession(st.session);
    if (st.pump.joinable()) st.pump.join();
    st.running.store(false);
    vkDeviceWaitIdle(device);
    destroy_images();
    for (auto& space : st.aim_space) if (space) { st.f.DestroySpace(space); space = XR_NULL_HANDLE; }
    for (auto& space : st.grip_space) if (space) { st.f.DestroySpace(space); space = XR_NULL_HANDLE; }
    if (st.local) { st.f.DestroySpace(st.local); st.local = XR_NULL_HANDLE; }
    if (st.session) { st.f.DestroySession(st.session); st.session = XR_NULL_HANDLE; }
    if (st.set) { st.f.DestroyActionSet(st.set); st.set = XR_NULL_HANDLE; }
    if (st.instance) { st.f.DestroyInstance(st.instance); st.instance = XR_NULL_HANDLE; }
    st.vk_device = VK_NULL_HANDLE;
    if (st.lib) {
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(st.lib));
#else
        dlclose(st.lib);
#endif
        st.lib = nullptr;
    }
}
} // namespace nxrt::xr

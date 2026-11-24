// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR: the OpenXR session, its frame loop, the eye swapchains and the controllers (nxrt/xr.h, src/gfx/xr/xr_vk.h).
// The loader is loaded at run time, so a PC without one (or with [vr] enabled = 0) runs exactly as before.
#include "xr/xr_vk.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define XR_NO_PROTOTYPES 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"

namespace nxrt::xr {
namespace {

constexpr const char* kTag = "vr";

// ---- the loader and the entry points ---------------------------------------------------------------------------------------------
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
    PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR = nullptr;
};

// ---- the controllers --------------------------------------------------------------------------------------------------------------
enum ActBool { kA, kB, kX, kY, kMenu, kLClick, kRClick, kBoolCount };
enum ActFloat { kLTrig, kRTrig, kLGrip, kRGrip, kFloatCount };
enum ActVec { kLStick, kRStick, kVecCount };

// ---- one OpenXR frame -----------------------------------------------------------------------------------------------------------
struct FrameData {
    uint64_t id = 0;
    XrTime time = 0;
    bool should_render = false;
    bool views_ok = false;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
};
constexpr int kRing = 4;

struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
    uint32_t w = 0, h = 0;
    bool acquired = false;
};

struct State {
    bool wanted = false;            // [vr] enabled
    bool failed = false;            // gave up: the game runs flat
    void* lib = nullptr;
    Fns f;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    std::vector<std::string> inst_ext_names, dev_ext_names;   // storage for the names handed to Vulkan

    // Vulkan
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkPhysicalDevice vk_physical = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    int64_t sc_format = 0;          // the eye swapchains' format (sRGB when offered)

    // session state (game thread writes; the render thread reads running)
    std::atomic<bool> running{false};
    std::atomic<bool> session_begun{false};
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    std::atomic<bool> exiting{false};

    // actions
    XrActionSet set = XR_NULL_HANDLE;
    XrAction act_bool[kBoolCount] = {};
    XrAction act_float[kFloatCount] = {};
    XrAction act_vec[kVecCount] = {};
    std::atomic<uint64_t> pad_buttons{0};
    std::atomic<int32_t> pad_lx{0}, pad_ly{0}, pad_rx{0}, pad_ry{0};

    // game thread: the eye of the frame being simulated
    std::mutex gm;
    int cur_eye = -1;
    uint64_t cur_frame = 0;
    uint64_t next_id = 0;
    EyeView cur_views[2];

    // frames handed from the game thread to the render thread
    std::mutex fm;
    FrameData ring[kRing];

    // render thread
    Swap swap[2];
    uint64_t begun_frame = 0;       // the frame xrBeginFrame was called for (0: none open)
    bool recorded[2] = {false, false};
    int acquired_eye = -1;
    VkImage mid = VK_NULL_HANDLE;   // the intermediate copy (format conversion and flip by blit, then a bit copy into the sRGB swapchain)
    VkDeviceMemory mid_mem = VK_NULL_HANDLE;
    VkFormat mid_format = VK_FORMAT_UNDEFINED;
    uint32_t mid_w = 0, mid_h = 0;
    uint64_t frames_ended = 0;
    bool logged_first_end = false;
};
State& s() {
    static State st;
    return st;
}
thread_local Tag t_render_tag;

bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    NXLOG_W(kTag, "%s failed (%d)", what, static_cast<int>(r));
    return false;
}

void give_up(const char* why) {
    State& st = s();
    if (st.failed) return;
    st.failed = true;
    st.running.store(false);
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
           load_fn(i, "xrGetSystemProperties", &f.GetSystemProperties) && load_fn(i, "xrCreateSession", &f.CreateSession) &&
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
        give_up("no OpenXR loader (put openxr_loader.dll next to pkp.exe, or set [vr] loader)");
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
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "PokopiaVR");
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

// ---- actions ----------------------------------------------------------------------------------------------------------------------
XrPath path(const char* p) {
    XrPath out = XR_NULL_PATH;
    s().f.StringToPath(s().instance, p, &out);
    return out;
}

bool make_action(XrActionType type, const char* name, const char* label, XrAction* out) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    std::snprintf(ci.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(ci.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", label);
    return ok(s().f.CreateAction(s().set, &ci, out), "xrCreateAction");
}

bool make_actions() {
    State& st = s();
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(si.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "play");
    std::snprintf(si.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "Play");
    if (!ok(st.f.CreateActionSet(st.instance, &si, &st.set), "xrCreateActionSet")) return false;
    const struct { int i; const char* n; } bools[] = {{kA, "a"}, {kB, "b"}, {kX, "x"}, {kY, "y"}, {kMenu, "menu"}, {kLClick, "lclick"}, {kRClick, "rclick"}};
    for (auto& b : bools)
        if (!make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, b.n, b.n, &st.act_bool[b.i])) return false;
    const struct { int i; const char* n; } floats[] = {{kLTrig, "ltrig"}, {kRTrig, "rtrig"}, {kLGrip, "lgrip"}, {kRGrip, "rgrip"}};
    for (auto& fl : floats)
        if (!make_action(XR_ACTION_TYPE_FLOAT_INPUT, fl.n, fl.n, &st.act_float[fl.i])) return false;
    if (!make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "move", &st.act_vec[kLStick])) return false;
    if (!make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "turn", &st.act_vec[kRStick])) return false;

    // Oculus Touch (Quest through Link, Air Link, Virtual Desktop or Steam Link)
    const XrActionSuggestedBinding touch[] = {
        {st.act_bool[kA], path("/user/hand/right/input/a/click")},
        {st.act_bool[kB], path("/user/hand/right/input/b/click")},
        {st.act_bool[kX], path("/user/hand/left/input/x/click")},
        {st.act_bool[kY], path("/user/hand/left/input/y/click")},
        {st.act_bool[kMenu], path("/user/hand/left/input/menu/click")},
        {st.act_bool[kLClick], path("/user/hand/left/input/thumbstick/click")},
        {st.act_bool[kRClick], path("/user/hand/right/input/thumbstick/click")},
        {st.act_float[kLTrig], path("/user/hand/left/input/trigger/value")},
        {st.act_float[kRTrig], path("/user/hand/right/input/trigger/value")},
        {st.act_float[kLGrip], path("/user/hand/left/input/squeeze/value")},
        {st.act_float[kRGrip], path("/user/hand/right/input/squeeze/value")},
        {st.act_vec[kLStick], path("/user/hand/left/input/thumbstick")},
        {st.act_vec[kRStick], path("/user/hand/right/input/thumbstick")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = touch;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof touch / sizeof touch[0]);
    ok(st.f.SuggestInteractionProfileBindings(st.instance, &sb), "xrSuggestInteractionProfileBindings (touch)");

    // Valve Index (and controllers SteamVR presents as Index): A/B on both hands, the left B as the menu
    const XrActionSuggestedBinding index[] = {
        {st.act_bool[kA], path("/user/hand/right/input/a/click")},
        {st.act_bool[kB], path("/user/hand/right/input/b/click")},
        {st.act_bool[kX], path("/user/hand/left/input/a/click")},
        {st.act_bool[kY], path("/user/hand/left/input/b/click")},
        {st.act_bool[kLClick], path("/user/hand/left/input/thumbstick/click")},
        {st.act_bool[kRClick], path("/user/hand/right/input/thumbstick/click")},
        {st.act_float[kLTrig], path("/user/hand/left/input/trigger/value")},
        {st.act_float[kRTrig], path("/user/hand/right/input/trigger/value")},
        {st.act_float[kLGrip], path("/user/hand/left/input/squeeze/value")},
        {st.act_float[kRGrip], path("/user/hand/right/input/squeeze/value")},
        {st.act_vec[kLStick], path("/user/hand/left/input/thumbstick")},
        {st.act_vec[kRStick], path("/user/hand/right/input/thumbstick")},
    };
    sb.interactionProfile = path("/interaction_profiles/valve/index_controller");
    sb.suggestedBindings = index;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof index / sizeof index[0]);
    ok(st.f.SuggestInteractionProfileBindings(st.instance, &sb), "xrSuggestInteractionProfileBindings (index)");

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &st.set;
    return ok(st.f.AttachSessionActionSets(st.session, &ai), "xrAttachSessionActionSets");
}

int32_t to_stick(float v) { return static_cast<int32_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f)); }

void read_actions() {
    State& st = s();
    XrActiveActionSet as{st.set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(st.f.SyncActions(st.session, &si))) return;
    auto b = [&](int i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.act_bool[i];
        XrActionStateBoolean v{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(st.f.GetActionStateBoolean(st.session, &gi, &v)) && v.isActive && v.currentState;
    };
    auto fl = [&](int i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.act_float[i];
        XrActionStateFloat v{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(st.f.GetActionStateFloat(st.session, &gi, &v)) && v.isActive ? v.currentState : 0.f;
    };
    auto vec = [&](int i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = st.act_vec[i];
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(st.f.GetActionStateVector2f(st.session, &gi, &v)) && v.isActive) return v.currentState;
        return XrVector2f{0, 0};
    };
    uint64_t btns = 0;
    if (b(kA)) btns |= hid::btn(hid::kBtnA);
    if (b(kB)) btns |= hid::btn(hid::kBtnB);
    if (b(kX)) btns |= hid::btn(hid::kBtnX);
    if (b(kY)) btns |= hid::btn(hid::kBtnY);
    if (b(kMenu)) btns |= hid::btn(hid::kBtnPlus);
    if (b(kLClick)) btns |= hid::btn(hid::kBtnMinus);
    if (b(kRClick)) btns |= hid::btn(hid::kBtnStickR);
    if (fl(kLTrig) > 0.5f) btns |= hid::btn(hid::kBtnZL);
    if (fl(kRTrig) > 0.5f) btns |= hid::btn(hid::kBtnZR);
    if (fl(kLGrip) > 0.5f) btns |= hid::btn(hid::kBtnL);
    if (fl(kRGrip) > 0.5f) btns |= hid::btn(hid::kBtnR);
    const XrVector2f l = vec(kLStick), r = vec(kRStick);
    st.pad_buttons.store(btns, std::memory_order_relaxed);
    st.pad_lx.store(to_stick(l.x), std::memory_order_relaxed);
    st.pad_ly.store(to_stick(l.y), std::memory_order_relaxed);
    st.pad_rx.store(to_stick(r.x), std::memory_order_relaxed);
    st.pad_ry.store(to_stick(r.y), std::memory_order_relaxed);
}

void clear_pad() {
    State& st = s();
    st.pad_buttons.store(0);
    st.pad_lx.store(0), st.pad_ly.store(0), st.pad_rx.store(0), st.pad_ry.store(0);
}

// ---- session events (game thread) -----------------------------------------------------------------------------------------------
void poll_events() {
    State& st = s();
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (st.f.PollEvent(st.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            st.session_state = sc->state;
            NXLOG_I(kTag, "session state %d", static_cast<int>(sc->state));
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                std::lock_guard<std::mutex> lk(st.fm);
                if (ok(st.f.BeginSession(st.session, &bi), "xrBeginSession")) {
                    st.session_begun.store(true);
                    st.running.store(true);
                }
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                std::lock_guard<std::mutex> lk(st.fm);
                st.running.store(false);
                ok(st.f.EndSession(st.session), "xrEndSession");
                st.session_begun.store(false);
                st.begun_frame = 0;
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                st.running.store(false);
                st.exiting.store(true);
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            st.running.store(false);
            st.exiting.store(true);
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

EyeView to_eye(const XrView& v, int eye) {
    EyeView e;
    e.valid = true;
    e.eye = eye;
    e.pos[0] = v.pose.position.x, e.pos[1] = v.pose.position.y, e.pos[2] = v.pose.position.z;
    e.quat[0] = v.pose.orientation.x, e.quat[1] = v.pose.orientation.y, e.quat[2] = v.pose.orientation.z, e.quat[3] = v.pose.orientation.w;
    e.tan_left = std::tan(v.fov.angleLeft);
    e.tan_right = std::tan(v.fov.angleRight);
    e.tan_up = std::tan(v.fov.angleUp);
    e.tan_down = std::tan(v.fov.angleDown);
    return e;
}

// ---- the render thread: images -----------------------------------------------------------------------------------------------------
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

FrameData* frame_slot(uint64_t id) {
    FrameData& fd = s().ring[id % kRing];
    return fd.id == id ? &fd : nullptr;
}

}  // namespace

// ---- configuration ---------------------------------------------------------------------------------------------------------------
float world_scale() {
    static const float v = static_cast<float>(nxrt::config_get_double("vr", "world_scale", 100.0));
    return v;
}
float head_height() {
    static const float v = static_cast<float>(nxrt::config_get_double("vr", "head_height", 60.0));
    return v;
}

bool mirror_x() {
    static const bool v = nxrt::config_get_bool("vr", "mirror_x", false);
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

void vk_device_created(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family, uint32_t queue_index) {
    State& st = s();
    if (st.failed || !st.instance) return;
    st.vk_instance = instance;
    st.vk_physical = physical;
    st.vk_device = device;
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
    if (!make_actions()) NXLOG_W(kTag, "controllers unavailable (the gamepad and keyboard still work)");

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
    if (!st.sc_format && !formats.empty()) st.sc_format = formats[0];
    NXLOG_I(kTag, "session created (queue family %u), eye format %lld; put the headset on", queue_family, static_cast<long long>(st.sc_format));
}

// ---- the game thread -------------------------------------------------------------------------------------------------------------
EyeView eye_view() {
    State& st = s();
    std::lock_guard<std::mutex> lk(st.gm);
    if (st.cur_eye < 0 || !st.running.load(std::memory_order_relaxed)) return {};
    return st.cur_views[st.cur_eye];
}

Tag game_present() {
    State& st = s();
    if (!active()) return {};
    Tag t;
    {
        std::lock_guard<std::mutex> lk(st.gm);
        t.eye = st.cur_eye;
        t.frame = st.cur_frame;
        if (st.cur_eye == 0) {   // the right eye of the same OpenXR frame comes next
            st.cur_eye = 1;
            return t;
        }
        st.cur_eye = -1;
    }
    poll_events();
    if (st.exiting.load()) {
        clear_pad();
        return t;
    }
    if (!st.running.load()) {
        clear_pad();
        return t;
    }
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(st.f.WaitFrame(st.session, &wi, &fs))) return t;
    read_actions();

    FrameData fd;
    fd.time = fs.predictedDisplayTime;
    fd.should_render = fs.shouldRender == XR_TRUE;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = st.local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    fd.views_ok = XR_SUCCEEDED(st.f.LocateViews(st.session, &li, &vs, 2, &nv, fd.views)) && nv == 2 &&
                  (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
    {
        std::lock_guard<std::mutex> lk(st.fm);
        fd.id = ++st.next_id;
        st.ring[fd.id % kRing] = fd;
    }
    std::lock_guard<std::mutex> lk(st.gm);
    st.cur_frame = fd.id;
    st.cur_eye = 0;
    if (fd.views_ok) {
        st.cur_views[0] = to_eye(fd.views[0], 0);
        st.cur_views[1] = to_eye(fd.views[1], 1);
    } else {
        st.cur_views[0] = st.cur_views[1] = EyeView{};
    }
    return t;
}

Pad pad() {
    State& st = s();
    Pad p;
    if (!running()) return p;
    p.active = true;
    p.buttons = st.pad_buttons.load(std::memory_order_relaxed);
    p.lx = st.pad_lx.load(std::memory_order_relaxed);
    p.ly = st.pad_ly.load(std::memory_order_relaxed);
    p.rx = st.pad_rx.load(std::memory_order_relaxed);
    p.ry = st.pad_ry.load(std::memory_order_relaxed);
    return p;
}

// ---- the render thread -----------------------------------------------------------------------------------------------------------
void render_set_tag(Tag t) {
    t_render_tag = t;
    State& st = s();
    if (t.eye != 0 || !running()) return;
    // the left eye opens the OpenXR frame
    std::lock_guard<std::mutex> lk(st.fm);
    if (!st.session_begun.load() || !frame_slot(t.frame)) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult r = st.f.BeginFrame(st.session, &bi);
    if (XR_FAILED(r)) {
        NXLOG_W(kTag, "xrBeginFrame failed (%d)", static_cast<int>(r));
        st.begun_frame = 0;
        return;
    }
    st.begun_frame = t.frame;
    st.recorded[0] = st.recorded[1] = false;
}

Tag render_tag() { return t_render_tag; }

void render_record(VkCommandBuffer cb, VkImage src, VkFormat src_format, uint32_t src_w, uint32_t src_h, const int32_t crop[4], bool flip_y) {
    State& st = s();
    const Tag t = t_render_tag;
    if (t.eye < 0 || t.eye > 1 || !running()) return;
    std::lock_guard<std::mutex> lk(st.fm);
    if (st.begun_frame != t.frame) return;
    const FrameData* fd = frame_slot(t.frame);
    if (!fd || !fd->should_render || !fd->views_ok) return;

    const int32_t cx = crop && crop[2] > 0 ? crop[0] : 0, cy = crop && crop[3] > 0 ? crop[1] : 0;
    const uint32_t w = crop && crop[2] > 0 ? static_cast<uint32_t>(crop[2]) : src_w;
    const uint32_t h = crop && crop[3] > 0 ? static_cast<uint32_t>(crop[3]) : src_h;
    if (!w || !h) return;
    Swap& sw = st.swap[t.eye];
    if (!ensure_swap(sw, w, h)) return;
    const VkFormat sc_fmt = static_cast<VkFormat>(st.sc_format);
    // an sRGB picture is copied as it is stored; a UNORM one (sRGB-encoded values) goes in bit for bit through the UNORM partner
    const VkFormat mid_fmt = is_srgb(src_format) ? sc_fmt : unorm_of(sc_fmt);
    if (!ensure_mid(mid_fmt, w, h)) return;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo aqi{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!ok(st.f.AcquireSwapchainImage(sw.sc, &aqi, &idx), "xrAcquireSwapchainImage")) return;
    sw.acquired = true;
    st.acquired_eye = t.eye;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!ok(st.f.WaitSwapchainImage(sw.sc, &wi), "xrWaitSwapchainImage")) return;
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
    st.recorded[t.eye] = true;
}

void render_after_submit() {
    State& st = s();
    const Tag t = t_render_tag;
    t_render_tag = Tag{};
    if (t.eye < 0) return;
    std::lock_guard<std::mutex> lk(st.fm);
    if (st.acquired_eye >= 0) {
        Swap& sw = st.swap[st.acquired_eye];
        if (sw.acquired) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ok(st.f.ReleaseSwapchainImage(sw.sc, &ri), "xrReleaseSwapchainImage");
            sw.acquired = false;
        }
        st.acquired_eye = -1;
    }
    if (t.eye != 1 || st.begun_frame != t.frame || !st.session_begun.load()) return;
    const FrameData* fd = frame_slot(t.frame);
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fd ? fd->time : 0;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    const bool both = fd && fd->should_render && fd->views_ok && st.recorded[0] && st.recorded[1] && st.swap[0].w == st.swap[1].w &&
                      st.swap[0].h == st.swap[1].h;
    if (both) {
        for (int e = 0; e < 2; ++e) {
            pv[e].pose = fd->views[e].pose;
            pv[e].fov = fd->views[e].fov;
            pv[e].subImage.swapchain = st.swap[e].sc;
            pv[e].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(st.swap[e].w), static_cast<int32_t>(st.swap[e].h)}};
            pv[e].subImage.imageArrayIndex = 0;
        }
        layer.space = st.local;
        layer.viewCount = 2;
        layer.views = pv;
        ei.layerCount = 1;
        ei.layers = layers;
    }
    const XrResult r = st.f.EndFrame(st.session, &ei);
    st.begun_frame = 0;
    if (XR_FAILED(r)) {
        NXLOG_W(kTag, "xrEndFrame failed (%d)", static_cast<int>(r));
        return;
    }
    ++st.frames_ended;
    if (both && !st.logged_first_end) {
        st.logged_first_end = true;
        NXLOG_I(kTag, "first stereo frame shown (%ux%u per eye)", st.swap[0].w, st.swap[0].h);
    }
}

}  // namespace nxrt::xr

// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR: OpenXR head-mounted display output, the game-facing half (no Vulkan types here; the graphics half is src/gfx/xr/xr_vk.h).
//
// Stereo is alternate-eye rendering (AER): the game renders the left eye in one frame and the right eye in the next, each from that eye's
// pose, and the two presents of one OpenXR frame are handed to the compositor together. The game therefore runs at twice the headset's rate
// (144 game frames for a 72 Hz headset). One OpenXR frame:
//   game thread   present boundary of the right eye (or the first one): xrWaitFrame, xrSyncActions, xrLocateViews -> the views of the frame
//   game thread   left-eye frame: the camera module asks eye_view(); the present boundary tags that present (eye 0)
//   game thread   right-eye frame: the same with eye 1
//   render thread the two tagged presents: xrBeginFrame on eye 0, the picture copied into each eye's swapchain, xrEndFrame after eye 1
// Everything is off unless [vr] enabled = 1 and an OpenXR runtime (SteamVR, Virtual Desktop's VDXR, Oculus) answers; then the game runs as
// before on the monitor only.
//
// Configuration ([vr], read once):
//   enabled = 0          1: start an OpenXR session
//   world_scale = 100    game world units per metre of head movement (a block is 100 units)
//   head_height = 60     world units from the follow camera's look-at point (the character) up to the eyes
//   mirror_x = 0         1: mirror the headset's left and right (if the eyes come out swapped: the game's world turns out left-handed)
//   loader =             the OpenXR loader to load (default openxr_loader.dll next to the exe, then the system's)
#pragma once
#include <cstdint>

namespace nxrt::xr {

// [vr] enabled and a session that has been created (it may not be running yet: the headset may be off or another app may be in front).
bool active();
// The session is running and the headset shows this app's frames: the camera module should place the eyes.
bool running();

// One eye of the OpenXR frame in progress, in the LOCAL reference space (metres; +Y up, -Z forward; recentred by the headset's own recenter).
struct EyeView {
    bool valid = false;
    int eye = -1;                       // 0 left, 1 right
    float pos[3] = {};
    float quat[4] = {0, 0, 0, 1};       // x, y, z, w
    float tan_left = -1, tan_right = 1, tan_up = 1, tan_down = -1;   // the eye's field of view as tangents (left and down negative)
};
// The game thread: the eye this game frame renders. Invalid while the session is not running.
EyeView eye_view();

// The game thread, at the game's present (the frame boundary): tags the frame just finished with the eye it rendered and moves on to the
// next eye, waiting for the next OpenXR frame after a right eye. Returns the tag the render thread hands to the graphics half.
struct Tag {
    int eye = -1;                       // -1: not a VR frame
    uint64_t frame = 0;                 // the OpenXR frame (1, 2, ...)
};
Tag game_present();

// Controllers, as Switch pad input (nxrt/input_source.h NpadButton bits; sticks -32767..32767, +Y up). All zero while not running.
struct Pad {
    bool active = false;
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad pad();

// [vr] settings the camera module reads.
float world_scale();
float head_height();
bool mirror_x();

}  // namespace nxrt::xr

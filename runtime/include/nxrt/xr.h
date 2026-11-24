// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR: OpenXR head-mounted display output, the game-facing half (no Vulkan types here; the graphics half is src/gfx/xr/xr_vk.h).
//
// The game renders alternate eyes. A dedicated headset frame loop submits the latest complete pair at the headset's cadence,
// retaining the exact pose used to render each image. Neither game nor render thread waits for xrWaitFrame. Images are handed
// back to OpenXR only after their graphics commands have actually been submitted to Vulkan.
//
// Configuration ([vr], read once):
//   enabled = 0          1: start an OpenXR session
//   world_scale = 100    game world units per metre of head movement (a block is 100 units)
//   head_height = 120    world units above the player's position
//   frame_lag = 1        presents between a frame's camera and the present that shows it (the engine draws one frame behind)
//   mirror_x = 0         1: mirror the headset's left and right (if the eyes come out swapped: the game's world turns out left-handed)
//   panel_distance = 2.5 metres to the flat panel that shows the title and menus (outside the world)
//   panel_width = 3.2    its width in metres
//   loader =             the OpenXR loader to load (default openxr_loader.dll next to the exe, then the system's)
#pragma once
#include <cstdint>

namespace nxrt::xr {

// [vr] enabled and a session that has been created (it may not be running yet: the headset may be off or another app may be in front).
bool active();
// [vr] enabled (whether or not a headset answered): for settings that must be chosen before the session exists.
bool wanted();
// The session is running and the headset shows this app's frames: the camera module should place the eyes.
bool running();

// One eye of the OpenXR frame in progress, in the LOCAL reference space (metres; +Y up, -Z forward; recentred by the headset's own recenter).
struct EyeView {
    bool valid = false;
    uint64_t frame = 0, reference_epoch = 0;
    float center[3] = {};
    int eye = -1;                       // 0 left, 1 right
    float pos[3] = {};
    float quat[4] = {0, 0, 0, 1};       // x, y, z, w
    float tan_left = -1, tan_right = 1, tan_up = 1, tan_down = -1;   // the eye's field of view as tangents (left and down negative)
};
// The game thread: the eye this game frame renders. Invalid while the session is not running.
EyeView eye_view();
// Right controller aim pose from the same tracking snapshot; invalid when tracking is unavailable.
EyeView hand_aim();

// The game thread, at the game's present (the frame boundary): tags the frame just finished with the eye it rendered and moves on to the
// next eye. Returns a complete camera snapshot that travels with the present to the graphics half.
struct Tag {
    int eye = -1;                       // -1: not a VR frame
    uint64_t frame = 0;                 // the rendered camera pair (1, 2, ...)
    EyeView view;                       // exact raw reference-space pose used by this image
    bool world = false;                 // the camera module placed this eye in the world (else: a menu or the title, shown on a flat panel)
    bool fov_ok = false;                // fov holds the field of view the picture was really rendered with (tangents: left, right, up, down)
    float fov[4] = {};
};
Tag game_present();
// The game thread, after the present is queued: snapshots the next pair's tracking after a right eye; never waits for the headset.
void game_present_done();
// The game thread: the camera module placed this frame's eye (the frame shows the world in stereo, not a menu).
void note_world_view();
// The game thread: the field of view this frame's picture was rendered with, read back from the projection (tangents, left and down
// negative). The compositor gets this one, so the picture matches even when the projection could not be set to the headset's own.
void note_rendered_fov(float tan_left, float tan_right, float tan_up, float tan_down);

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
float panel_distance();
float panel_width();

}  // namespace nxrt::xr

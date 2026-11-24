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
// Camera update: selects the next eye; both eyes of a pair use one tracking snapshot. Invalid outside the active world camera.
EyeView eye_view();
// Right controller aim pose from the same tracking snapshot; invalid when tracking is unavailable.
EyeView hand_aim();

// Image metadata is matched to the camera uniforms consumed by the scene draw. Presents never advance the camera eye.
struct Tag {
    int eye = -1;                       // -1: not a VR frame
    uint64_t frame = 0;                 // the rendered camera pair (1, 2, ...)
    EyeView view;                       // exact raw reference-space pose used by this image
    bool world = false;                 // the camera module placed this eye in the world (else: a menu or the title, shown on a flat panel)
    bool fov_ok = false;                // fov holds the field of view the picture was really rendered with (tangents: left, right, up, down)
    float fov[4] = {};
};
Tag game_present();
// The game camera module identifies whether its world camera is active, independently of how often that camera updates.
void set_world_active(bool active);
// The camera module placed this exact eye; metadata stays valid until the next camera update or mode change.
void note_world_view(const EyeView& view);
// The game thread: the field of view this frame's picture was rendered with, read back from the projection (tangents, left and down
// negative). The compositor gets this one, so the picture matches even when the projection could not be set to the headset's own.
void note_rendered_fov(float tan_left, float tan_right, float tan_up, float tan_down);
// Register the completed source camera. The renderer matches its actual uniforms instead of assuming a fixed frame delay.
void note_rendered_camera(const float view[16], const float view_projection[16]);

// Controllers, as Switch pad input (nxrt/input_source.h NpadButton bits; sticks -32767..32767, +Y up). All zero while not running.
struct Pad {
    bool active = false;
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad pad();
// Optional application commands from controller shortcuts, consumed by the game on its own frame thread.
enum Command : uint32_t { kTogglePerspective = 1, kScreenshot = 2 };
void set_shortcuts_enabled(bool enabled);
// Contrast-adaptive sharpening of world eye images before headset handoff; 0 disables it.
void set_sharpness(float amount);
uint32_t take_commands();

// [vr] settings the camera module reads.
float world_scale();
float head_height();
bool mirror_x();
float panel_distance();
float panel_width();

}  // namespace nxrt::xr

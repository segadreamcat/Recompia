// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR camera (pkp_vr_camera.cpp): first person, 6DOF, one eye per camera update (nxrt/xr.h). Does nothing unless an OpenXR session runs.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kVrCameraFeature;
namespace vr_camera {
// True while VR owns this source lens; other aspect hooks must leave it alone.
bool owns_projection(uint64_t camera);
// Right-hand aim in game coordinates; head aim while the controller is untracked. False outside a recently updated VR view.
bool aim_ray(float eye[3], float direction[3]);
// A tracked LOCAL point transformed with the current first-person anchor and yaw.
bool reference_point(const float point[3], uint64_t epoch, float world[3]);
}

}  // namespace nxrt::games::pkp

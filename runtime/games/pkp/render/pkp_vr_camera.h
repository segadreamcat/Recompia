// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR camera (pkp_vr_camera.cpp): first person, 6DOF, one eye per camera update (nxrt/xr.h). Does nothing unless an OpenXR session runs.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kVrCameraFeature;
namespace vr_camera {
// Right-hand aim in game coordinates; head aim while the controller is untracked. False outside a recently updated VR view.
bool aim_ray(float eye[3], float direction[3]);
}

}  // namespace nxrt::games::pkp

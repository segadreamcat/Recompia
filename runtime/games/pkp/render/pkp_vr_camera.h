// SPDX-License-Identifier: GPL-3.0-or-later
// PokopiaVR camera (pkp_vr_camera.cpp): first person, 6DOF, one eye per game frame (nxrt/xr.h). Does nothing unless an OpenXR session runs.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kVrCameraFeature;

}  // namespace nxrt::games::pkp

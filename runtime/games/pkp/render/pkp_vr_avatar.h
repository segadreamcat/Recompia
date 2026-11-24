// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/pkp_feature.h"
namespace nxrt::games::pkp {
extern const Feature kVrAvatarFeature;
namespace vr_avatar { void update(::Ctx* c, bool hide); }
}

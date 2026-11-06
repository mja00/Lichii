// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

constexpr bool CanDiscardPendingDraw(bool native, bool depth_enabled, u32 index_count,
                                    u32 vertex_count, bool discard_native = false) {
    // Native draws can produce persistent resources, even with depth enabled. Skipping them is
    // opt-in (EDEN_NX2_ASYNC_DISCARD=1): a possibly missing effect instead of a pipeline stall.
    if (native && !discard_native) {
        return false;
    }
    // Preserve the legacy async-shader heuristic.
    return depth_enabled || (index_count > 6 && vertex_count > 6);
}

} // namespace Vulkan

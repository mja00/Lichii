// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_gpu_breadcrumbs.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

void GpuBreadcrumbs::Initialize(const Device& device, const MemoryAllocator& allocator) {
    const char* value = std::getenv("EDEN_NX2_GPU_BREADCRUMBS");
    if (value == nullptr || value[0] == '0' || enabled) {
        return;
    }
    if (!device.IsAmdBufferMarkerSupported()) {
        LOG_WARNING(Render_Vulkan, "GPU breadcrumbs need VK_AMD_buffer_marker, which this driver "
                                   "lacks");
        return;
    }
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 256,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    buffer = allocator.CreateBuffer(ci, MemoryUsage::Download);
    if (buffer.Mapped().size() < 8) {
        LOG_WARNING(Render_Vulkan, "GPU breadcrumbs: no host-visible buffer");
        return;
    }
    std::memset(buffer.Mapped().data(), 0, 8);
    enabled = true;
    LOG_INFO(Render_Vulkan, "GPU breadcrumbs enabled");
}

void GpuBreadcrumbs::Dump(std::string_view reason) const {
    if (!enabled) {
        return;
    }
    buffer.Invalidate();
    u32 started = 0;
    u32 finished = 0;
    std::memcpy(&started, buffer.Mapped().data(), sizeof(started));
    std::memcpy(&finished, buffer.Mapped().data() + 4, sizeof(finished));
    const u32 recorded_now = recorded;
    LOG_CRITICAL(Render_Vulkan,
                 "GPU breadcrumbs ({}): recorded {}, last started {}, last finished {}", reason,
                 recorded_now, started, finished);
    // The work the GPU had started but not finished, plus a little context either side.
    const u32 first = finished > 4 ? finished - 4 : 1;
    const u32 last = std::min(recorded_now, std::max(started, finished) + 4);
    for (u32 sequence = first; sequence <= last && sequence - first < 64; ++sequence) {
        const Item& item = items[sequence & ItemMask];
        if (item.sequence != sequence) {
            continue;
        }
        const char* state = sequence <= finished ? "done"
                            : sequence <= started ? "RUNNING"
                                                  : "queued";
        LOG_CRITICAL(Render_Vulkan, "  #{} {} shader {:016x} fragment {:016x} size {}x{}x{}",
                     sequence, state, item.hash, item.fragment_hash, item.size[0], item.size[1],
                     item.size[2]);
    }
}

} // namespace Vulkan

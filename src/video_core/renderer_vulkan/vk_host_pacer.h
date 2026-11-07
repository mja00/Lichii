// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

/// Host frame pacer (vk_present_manager.h). On for Ocarina of Time, off for other programs, unless
/// EDEN_NX2_HOST_PACE is set (0 = off, anything else = on).
///
/// A swapchain shows a frame at the first display refresh after it is ready. A 60 fps game on a
/// 120 Hz display should then show every frame for two refreshes, but when its frames reach the
/// present thread unevenly (the GPU thread's work varies frame to frame) one lands just after a
/// refresh and stays three refreshes (25 ms), the next just before one and stays one (8 ms); with
/// mailbox two frames ready within one refresh drop the first. The pacer learns when each frame
/// was shown (VK_KHR_present_wait) and the display's refresh period, and holds the next frame
/// until shortly before the refresh `k` refreshes after the previous one, `k` being the game's
/// frame period in refreshes. A frame that is ready later is presented at once.
///
/// Used by the present thread only.
class HostPacer {
public:
    using Clock = std::chrono::steady_clock;

    /// `default_on`: whether this boot's program wants the pacer (the env switch overrides it).
    explicit HostPacer(bool default_on);

    /// Whether this boot wants the pacer at all (the switch; read once, in the constructor).
    [[nodiscard]] bool Enabled() const;

    /// A frame reached the present thread (for the game's frame period).
    void NoteQueued(Clock::time_point queued);

    /// Whether frames are held now (switch on, refresh period known).
    [[nodiscard]] bool Pacing() const {
        return pacing;
    }

    /// Re-reads the switch and the display's refresh period now and then. `allowed`: the device
    /// supports present ids and present wait, and frames are presented on the present thread.
    void Refresh(Clock::time_point now, bool allowed);

    /// When the next frame should be handed to the swapchain; `now` or earlier = at once.
    [[nodiscard]] Clock::time_point Target(Clock::time_point now);

    /// The frame was presented with `id` (0: not presented); `was_held` if Target made it wait.
    void NoteSubmitted(u64 id, Clock::time_point submitted_at, bool was_held);

    /// Target made the present thread wait `hold`.
    void NoteHold(std::chrono::nanoseconds hold);

    /// vkWaitForPresentKHR returned for `id` at `shown`, or the present was lost.
    void NoteShown(u64 id, Clock::time_point shown, bool was_lost);

    /// The last presented id, which the next Target wants shown first (0: none outstanding).
    [[nodiscard]] u64 Outstanding() const {
        return outstanding;
    }

private:
    struct Submitted {
        u64 id;
        Clock::time_point submitted;
        std::optional<Clock::time_point> intended; // the refresh it was held for
    };

    void Report(Clock::time_point now);

    bool enabled{}; ///< The switch, read once per boot
    bool pacing{};
    double refresh_ns{1e9 / 60.0};
    bool refresh_known{};
    Clock::time_point next_refresh_query{};

    std::optional<Clock::time_point> last_queued;
    double content_ns{1e9 / 60.0};

    std::optional<Clock::time_point> vblank; // refresh the last shown frame appeared at
    std::optional<Clock::time_point> last_shown;
    u64 last_shown_id{};
    std::optional<Clock::time_point> pending_intended;
    double lead_ns{2.0e6};
    u32 early_streak{}; ///< Frames in a row that could have been shown a refresh earlier
    u64 outstanding{};
    std::deque<Submitted> submitted;

    // Reported every 10 s.
    Clock::time_point report_start{};
    std::array<u32, 6> refreshes_shown{}; // 0 (same refresh), 1, 2, 3, 4, 5+
    u32 frames{}, held{}, missed{}, lost{}, caught_up{};
    double interval_sum_ms{}, interval_max_ms{}, hold_sum_ms{}, latency_sum_ms{};
};

/// The display refresh period: the compositor's measured one on Windows, `nominal_ns` (the period
/// the game asked for) elsewhere. nullopt while neither is known.
[[nodiscard]] std::optional<double> QueryRefreshPeriodNs(double nominal_ns);

} // namespace Vulkan

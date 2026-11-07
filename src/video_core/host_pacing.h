// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

/// What the game's present pacing (nx2_gsv.h) and the host frame pacer (vk_host_pacer.h) tell
/// each other about frame and refresh periods. Both run in one boot; the atomics carry a 0 while
/// the value is unknown.
namespace VideoCore::HostPacing {

/// The frame period the game asks for (nx2:vi2 present word 12 at the speed limit), 0 while
/// unknown or unpaced. The host frame pacer holds each frame to a whole number of refreshes of it.
inline std::atomic<long long> g_guest_frame_interval_ns{0};

/// The display refresh rate the game asks for, in Hz, 0 while unknown. Off Windows, where there is
/// no compositor to measure the real period, it is the host frame pacer's nominal period.
inline std::atomic<long long> g_guest_refresh_hz{0};

/// The display's refresh period while the host frame pacer runs, else 0. The game's present
/// pacing times its frames to whole multiples of it (genlock).
inline std::atomic<long long> g_host_refresh_ns{0};

} // namespace VideoCore::HostPacing

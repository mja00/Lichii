// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/logging.h"
#include "video_core/host_pacing.h"
#include "video_core/renderer_vulkan/vk_host_pacer.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <dwmapi.h>
// clang-format on
#endif

namespace Vulkan {
namespace {
using Ns = std::chrono::nanoseconds;
using Ms = std::chrono::duration<double, std::milli>;

Ns ToNs(double ns) {
    return Ns{static_cast<s64>(ns)};
}
} // Anonymous namespace

HostPacer::HostPacer(bool default_on) {
    // Read once per boot: the frontend applies nx2_switches.txt to the environment at startup,
    // before a game boots, and the switch is not meant to move under a running pacer.
    const char* const value = std::getenv("EDEN_NX2_HOST_PACE");
    if (value == nullptr) {
        enabled = default_on;
        return;
    }
    // EDEN_NX2_HOST_PACE=0 is off; a number reading as non-zero or a word like "on" is on.
    const bool numeric = (value[0] >= '0' && value[0] <= '9') || value[0] == '-';
    enabled = numeric ? std::atoi(value) != 0 : true;
}

std::optional<double> QueryRefreshPeriodNs(double nominal_ns) {
#ifdef _WIN32
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) &&
        info.rateRefresh.uiNumerator != 0 && info.rateRefresh.uiDenominator != 0) {
        const double nominal = 1e9 * info.rateRefresh.uiDenominator / info.rateRefresh.uiNumerator;
        // The compositor's measured period: a "120 Hz" display may run at 119.98, and the game's
        // frame clock follows this (genlock). The nominal rate when it looks off.
        LARGE_INTEGER frequency{};
        if (info.qpcRefreshPeriod != 0 && QueryPerformanceFrequency(&frequency) &&
            frequency.QuadPart != 0) {
            const double measured = 1e9 * static_cast<double>(info.qpcRefreshPeriod) /
                                    static_cast<double>(frequency.QuadPart);
            if (std::abs(measured - nominal) < nominal * 0.01) {
                return measured;
            }
        }
        return nominal;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return 1e9 / static_cast<double>(mode.dmDisplayFrequency);
    }
    // A fullscreen-exclusive or remote session can hide both from us; the game's rate will do.
    if (nominal_ns > 0) {
        return nominal_ns;
    }
#else
    // Nothing here reports the display's period, and the game's own rate is wrong on any display
    // that isn't 60 Hz (a 144 Hz one was paced as 60 Hz): stay off.
    (void)nominal_ns;
#endif
    return std::nullopt;
}

void HostPacer::NoteQueued(Clock::time_point queued) {
    if (last_queued) {
        // A load screen's or a stall's gap would take seconds to forget: clamp each sample.
        const double sample = std::clamp(static_cast<double>(Ns{queued - *last_queued}.count()),
                                         refresh_ns * 0.5, 100e6);
        content_ns += (sample - content_ns) * 0.05;
    }
    last_queued = queued;
}

bool HostPacer::Enabled() const {
    return enabled;
}

void HostPacer::Refresh(Clock::time_point now, bool allowed) {
    if (now >= next_refresh_query) {
        // The rate the game asks for (nx2:vi2 present word 12) is the fallback when the display's
        // can't be read; without either the pacer stays off.
        const long long hz =
            VideoCore::HostPacing::g_guest_refresh_hz.load(std::memory_order_relaxed);
        const double nominal = hz > 0 ? 1e9 / static_cast<double>(hz) : 0.0;
        if (const auto period = QueryRefreshPeriodNs(nominal);
            period && *period > 2e6 && *period < 50e6) {
            if (!refresh_known || std::abs(*period - refresh_ns) > refresh_ns * 0.01) {
                LOG_INFO(Render_Vulkan, "Host pacing: display refresh {:.4f} Hz", 1e9 / *period);
                vblank.reset();
            }
            refresh_ns = *period;
            refresh_known = true;
            next_refresh_query = now + std::chrono::seconds{2};
        } else {
            // The game has not told us its refresh rate yet: ask again soon instead of in 2 s.
            next_refresh_query =
                now + (refresh_known ? std::chrono::seconds{2} : std::chrono::milliseconds{200});
        }
    }
    pacing = allowed && refresh_known && enabled;
    VideoCore::HostPacing::g_host_refresh_ns.store(pacing ? static_cast<long long>(refresh_ns) : 0,
                                                   std::memory_order_relaxed);
}

HostPacer::Clock::time_point HostPacer::Target(Clock::time_point now) {
    pending_intended.reset();
    if (!pacing || !vblank || outstanding != 0) {
        return now;
    }
    // Refreshes per game frame, from the frame rate the game asks for: 2 for 60 fps at 120 Hz,
    // also while it runs at 55 (those frames then show for 2 or 3), 4 for 30 fps. Not from how
    // fast frames arrive: holding frames slows the game, and the pacer then locked itself to
    // the slower rate (a loading screen's 25 ms frames held Ocarina at 40 fps). Unknown: only
    // one frame per refresh.
    const long long guest_ns =
        VideoCore::HostPacing::g_guest_frame_interval_ns.load(std::memory_order_relaxed);
    const s64 k = guest_ns > 0
                      ? (std::max<s64>)(1, static_cast<s64>(std::floor(
                                               static_cast<double>(guest_ns) / refresh_ns + 0.02)))
                      : 1;
    auto intended = *vblank + ToNs(static_cast<double>(k) * refresh_ns);
    // A late frame moves the schedule a refresh later for good, and the frames after it then
    // wait that refresh in the queue (input lag). Once frames could have been shown a refresh
    // earlier for 2 s in a row, show one a refresh early (one short frame) to drop that wait.
    // Not at 30 fps and below (k > 2): there every frame must stay on screen equally long.
    if (k == 2 && intended - ToNs(refresh_ns + lead_ns + 1e6) > now) {
        if (++early_streak >= 120) {
            early_streak = 0;
            intended -= ToNs(refresh_ns);
            ++caught_up;
        }
    } else {
        early_streak = 0;
    }
    const auto target = intended - ToNs(lead_ns);
    if (target <= now) {
        return now;
    }
    pending_intended = intended;
    return target;
}

void HostPacer::NoteSubmitted(u64 id, Clock::time_point submitted_at, bool was_held) {
    if (id == 0) {
        outstanding = 0;
        pending_intended.reset();
        return;
    }
    submitted.push_back({id, submitted_at, was_held ? pending_intended : std::nullopt});
    while (submitted.size() > 16) {
        submitted.pop_front();
    }
    pending_intended.reset();
    outstanding = id;
}

void HostPacer::NoteShown(u64 id, Clock::time_point shown, bool was_lost) {
    while (!submitted.empty() && submitted.front().id < id) {
        submitted.pop_front();
    }
    std::optional<Submitted> entry;
    if (!submitted.empty() && submitted.front().id == id) {
        entry = submitted.front();
        submitted.pop_front();
    }
    if (outstanding != 0 && id >= outstanding) {
        outstanding = 0;
    }
    if (report_start == Clock::time_point{}) {
        report_start = shown;
    }
    if (was_lost) {
        ++lost;
        vblank.reset();
        last_shown.reset();
        return;
    }
    ++frames;
    if (entry) {
        latency_sum_ms += Ms{shown - entry->submitted}.count();
    }
    if (last_shown && last_shown_id + 1 == id) {
        const double interval = static_cast<double>(Ns{shown - *last_shown}.count());
        interval_sum_ms += interval / 1e6;
        interval_max_ms = (std::max)(interval_max_ms, interval / 1e6);
        const auto count = std::clamp<s64>(std::llround(interval / refresh_ns), 0, 5);
        ++refreshes_shown[static_cast<size_t>(count)];
    }
    // Follow the refresh grid: snap to the refresh this was shown at and correct the phase a
    // little, so the wake-up noise of the wait does not move the targets.
    if (vblank) {
        const double since = static_cast<double>(Ns{shown - *vblank}.count());
        const s64 refreshes = std::llround(since / refresh_ns);
        if (refreshes >= 1) {
            const auto predicted = *vblank + ToNs(static_cast<double>(refreshes) * refresh_ns);
            const double residual = static_cast<double>(Ns{shown - predicted}.count());
            vblank =
                std::abs(residual) < refresh_ns * 0.5 ? predicted + ToNs(residual * 0.1) : shown;
        }
    } else {
        vblank = shown;
    }
    if (entry && entry->intended) {
        if (shown > *entry->intended + ToNs(refresh_ns * 0.5)) {
            ++missed;
            lead_ns = (std::min)(lead_ns + 0.5e6, refresh_ns * 0.75);
        } else {
            lead_ns = (std::max)(lead_ns - 0.01e6, 0.5e6);
        }
    }
    last_shown = shown;
    last_shown_id = id;
    Report(shown);
}

void HostPacer::NoteHold(Ns hold) {
    ++held;
    hold_sum_ms += Ms{hold}.count();
}

void HostPacer::Report(Clock::time_point now) {
    if (now - report_start < std::chrono::seconds{10} || frames == 0) {
        return;
    }
    u32 intervals = 0;
    for (const u32 count : refreshes_shown) {
        intervals += count;
    }
    LOG_INFO(Render_Vulkan,
             "Host pacing {}: {} frames at {:.3f} Hz; shown for 1/2/3/4/5+ refreshes: "
             "{}/{}/{}/{}/{} (same refresh {}); interval mean {:.2f} ms, max {:.2f}; held {} "
             "(mean {:.2f} ms, missed {}), lead {:.2f} ms; present to shown {:.2f} ms; game "
             "frame {:.2f} ms; {} lost; caught up {}",
             pacing ? "on" : "off", frames, 1e9 / refresh_ns, refreshes_shown[1],
             refreshes_shown[2], refreshes_shown[3], refreshes_shown[4], refreshes_shown[5],
             refreshes_shown[0], intervals ? interval_sum_ms / intervals : 0.0, interval_max_ms,
             held, held ? hold_sum_ms / held : 0.0, missed, lead_ns / 1e6, latency_sum_ms / frames,
             content_ns / 1e6, lost, caught_up);
    report_start = now;
    refreshes_shown = {};
    frames = held = missed = lost = caught_up = 0;
    interval_sum_ms = interval_max_ms = hold_sum_ms = latency_sum_ms = 0;
}

} // namespace Vulkan

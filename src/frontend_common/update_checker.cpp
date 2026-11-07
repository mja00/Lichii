// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <charconv>
#include <compare>
#include <vector>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"

namespace UpdateChecker {

namespace {

// Two numbering schemes share the releases list: build-N (automatic builds) and
// lichii-oot-vX.Y (this fork's releases). A build only updates within its own scheme.
struct ReleaseVersion {
    bool fork_release{};
    std::vector<u64> parts;
    auto operator<=>(const ReleaseVersion&) const = default;
};

std::optional<u64> ParseNumber(std::string_view text) {
    u64 number{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return number;
}

std::optional<ReleaseVersion> ParseVersion(std::string_view tag) {
    constexpr std::string_view build_prefix = "build-";
    constexpr std::string_view fork_prefix = "lichii-oot-v";
    if (tag.starts_with(build_prefix)) {
        tag.remove_prefix(build_prefix.size());
        const auto number = ParseNumber(tag);
        if (!number || *number == 0) {
            return std::nullopt;
        }
        return ReleaseVersion{.fork_release = false, .parts = {*number}};
    }
    if (!tag.starts_with(fork_prefix)) {
        return std::nullopt;
    }
    tag.remove_prefix(fork_prefix.size());
    ReleaseVersion version{.fork_release = true, .parts = {}};
    while (true) {
        const size_t dot = tag.find('.');
        const auto part = ParseNumber(tag.substr(0, dot));
        if (!part) {
            return std::nullopt;
        }
        version.parts.push_back(*part);
        if (dot == std::string_view::npos) {
            break;
        }
        tag.remove_prefix(dot + 1);
    }
    // v2 and v2.0 are the same release.
    while (version.parts.size() > 1 && version.parts.back() == 0) {
        version.parts.pop_back();
    }
    return version;
}

} // namespace

std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view build_tag,
                                                 std::string_view platform) {
    auto newest = ParseVersion(build_tag);
    if (!newest) {
        return std::nullopt;
    }

    const Common::Net::Release* selected = nullptr;
    for (const auto& release : releases) {
        const auto version = ParseVersion(release.tag);
        if (version && version->fork_release == newest->fork_release && *version > *newest &&
            !release.GetPlatformAssets(platform).empty()) {
            newest = version;
            selected = &release;
        }
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

std::optional<Common::Net::Release> GetUpdate() {
    if (!ParseVersion(Common::g_build_release_tag) ||
        std::string_view{Common::g_build_release_platform}.empty()) {
        return std::nullopt;
    }

    auto update = SelectUpdate(Common::Net::GetReleases(), Common::g_build_release_tag,
                               Common::g_build_release_platform);
    if (update) {
        LOG_INFO(Frontend, "Received update {}", update->title);
    }
    return update;
}

} // namespace UpdateChecker

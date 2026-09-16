/**
 * @file    platform/desktop_shortcut.h
 * @brief   Writes a shortcut to the current user's desktop.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bd::platform {

// Writes a shortcut to the current user's desktop pointing at target. Returns
// false and fills error on failure. Windows only; elsewhere it reports that
// and returns false.
bool CreateDesktopShortcut(const std::filesystem::path &target,
                           std::string_view name, std::string &error);

// Repoints an existing desktop shortcut at target, so switching backends does
// not leave the shortcut starting the other executable. True and no work when
// no shortcut is on the desktop: one the user declined stays declined.
bool RetargetDesktopShortcut(const std::filesystem::path &target,
                             std::string_view name, std::string &error);

} // namespace bd::platform

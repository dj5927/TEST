/**
 * @file    platform/reboot.h
 * @brief   Warm reboot (host process relaunch) for restart-bound settings and
 * DLC changes.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace bd::platform {

// The active profile's reblue.toml once SetProfileContext has run, else
// <exe_dir>/reblue.toml.
std::filesystem::path ConfigFilePath();

// So warm reboot persists to the profile's config and re-passes --profile to
// the fresh instance. SpawnFreshInstance forwards nothing else. Call once
// during path setup.
void SetProfileContext(std::string profile_name,
                       std::filesystem::path config_path);

// True inside Steam's Game Mode session, where a relaunch spawned by the game
// is not the process Steam is tracking. There the warm reboot degrades to a
// clean exit and the user relaunches from Steam.
bool IsSteamGameMode();

// Registered once by the host app. Marshals the relaunch onto the UI thread,
// where the kernel state is reachable, as ReXApp::OnClosing does.
void SetWarmRebootHandler(std::function<void()> handler);

// Request a clean warm reboot. Safe to call from a guest thread and to call
// twice. Invokes the registered host handler, which relaunches the process.
void RequestWarmReboot();

// False if another copy is already running.
bool AcquireInstanceLock();

// Spawns exe in place of this process, forwarding the active profile and
// --repair if requested. True if it launched. The caller quits either way.
// Releases the instance lock first, so the replacement can take it before this
// process is gone.
bool SpawnReplacement(const std::filesystem::path &exe, bool repair);

// Spawns a fresh instance of this process, with --repair if requested. True
// if it launched. The caller quits either way.
bool RelaunchSelf(bool repair);

// Persist cvars, quiesce the guest and renderer, spawn a fresh instance, flush
// logs, exit this one. UI thread only, never returns. The quiesce runs first so
// the replacement never overlaps this process on the GPU, which does mean a
// failed spawn leaves nothing to fall back to. Under Game Mode the spawn is
// skipped and this is a settings-persisting exit.
//
// An empty exe restarts this executable. A backend switch passes the sibling
// built for the chosen renderer, which is what carries that choice across the
// restart now that a plain launch runs the backend it was built for.
[[noreturn]] void PerformWarmReboot(const std::function<void()> &quiesce,
                                    std::filesystem::path exe = {});

} // namespace bd::platform

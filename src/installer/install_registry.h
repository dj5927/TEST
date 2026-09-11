/**
 * @file    installer/install_registry.h
 * @brief   Persists the install location and disc fingerprints in the registry.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <rex/types.h>

namespace bd::installer {

// Bump when an older record can no longer be brought forward in memory.
// ReadInstallRegistry migrates what it can and stamps this on success, so a
// caller that sees anything else is holding a record it cannot use.
constexpr int kInstallSchemaVersion = 3;

// Blue Dragon ships on three DVDs, so every disc-indexed array here is this
// long.
inline constexpr int kDiscCount = 3;

// Windows alone ships one executable per backend, so it is the only platform
// that records a choice, offers one, or acts on one. Absent reads as D3D12.
enum class Renderer : u32 {
  D3D12 = 0,
  Vulkan = 1,
};
inline constexpr u32 kRendererCount = 2;

// Catalog keys, so the menu and the installer label a backend from one place.
constexpr const char *ToString(Renderer renderer) {
  switch (renderer) {
  case Renderer::D3D12:
    return "opt.renderer.d3d12";
  case Renderer::Vulkan:
    return "opt.renderer.vulkan";
  }
  return "";
}

#if defined(_WIN32)
constexpr const char *RendererExecutable(Renderer renderer) {
  return renderer == Renderer::Vulkan ? "reblue_vk.exe" : "reblue.exe";
}
#endif

// The backend this executable was built for, and so the one it runs: a plain
// launch never starts the other exe. Windows ships one per backend, every
// other platform is Vulkan.
constexpr Renderer kBuiltRenderer =
#if defined(REBLUE_D3D12)
    Renderer::D3D12;
#else
    Renderer::Vulkan;
#endif

struct InstallConfig {
  // Game files under {install_root}/game, user/DLC under {install_root}/user.
  std::filesystem::path install_root;
  std::array<std::string, kDiscCount> iso_fingerprints;
  int schema_version =
      0; // registry SchemaVersion, 0 if absent (pre-schema install)
  Renderer renderer = Renderer::D3D12;
  // REBLUE_VERSION_STRING of the build that wrote this record, stamped by
  // WriteInstallRegistry. Empty on a record written before it was recorded.
  std::string app_version;

  std::filesystem::path game_data_path() const { return install_root / "game"; }
  std::filesystem::path user_data_path() const { return install_root / "user"; }
};

// Reads the per-user install store, HKCU\Software\Zolaware\reblue\Install on
// Windows and $XDG_CONFIG_HOME/reblue/install.toml on Linux. Returns nullopt if
// absent or the recorded install_root/game/default.xex is missing. An older
// record comes back migrated, so schema_version reads as current whenever this
// build can use what it holds.
std::optional<InstallConfig> ReadInstallRegistry();

bool WriteInstallRegistry(const InstallConfig &config);

// Deletes the Install subkey. True on success or if absent.
bool ClearInstallRegistry();

} // namespace bd::installer

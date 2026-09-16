/**
 * @file    platform/native_window.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/native_window.h"

#include <format>

#include <SDL3/SDL_video.h>
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>

#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#endif

#include <rex/ui/window.h>

#include "core/android_diag.h"
#include "core/logging.h"
#include "platform/host_resources.h"

namespace bd::platform {

#if defined(_WIN32)

bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out) {
  out = static_cast<plume::RenderWindow>(window->GetNativeWindowHandle());
  if (!out) {
    BD_ERROR("Window has no native HWND yet");
    return false;
  }
  return true;
}

#elif defined(__APPLE__)

namespace {

void SetenvIfUnset(const char *name, const char *value) {
  if (const char *cur = std::getenv(name); !cur || !cur[0])
    setenv(name, value, 1);
}

// volk dlopen's libvulkan/libMoltenVK by leaf name, and dyld snapshots
// DYLD_LIBRARY_PATH at launch, so setting it in-process is too late: the run
// that needs it has to be a fresh exec. Priority 101 (the lowest the attribute
// allows) puts this ahead of every other initializer in the image, so the exec
// cannot tear down state one of them already built.
__attribute__((constructor(101))) void
ConfigureMoltenVKAndReexec(int /*argc*/, char **argv) {
  RaiseFDLimit();

  SetenvIfUnset("SDL_MAC_PRESS_AND_HOLD", "0");

  SetenvIfUnset("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1");
  // Style 3 is the single-Metal-queue mode.
  SetenvIfUnset("MVK_CONFIG_SEMAPHORE_SUPPORT_STYLE", "3");
  SetenvIfUnset("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1");

  if (std::getenv("REBLUE_MOLTENVK_RELAUNCHED"))
    return; // already relaunched

  char exe_buf[4096];
  uint32_t exe_size = sizeof(exe_buf);
  if (_NSGetExecutablePath(exe_buf, &exe_size) != 0)
    return;
  const std::string exe(exe_buf);
  const std::string exe_dir = exe.substr(0, exe.find_last_of('/'));

  const std::string dirs[] = {
      exe_dir + "/vulkan/lib", // SDK-staged runtime (rexglue_configure_target)
      "/opt/homebrew/lib",     // Homebrew molten-vk
      "/usr/local/lib",
  };
  std::string vk_path;
  for (const std::string &dir : dirs) {
    if (access((dir + "/libMoltenVK.dylib").c_str(), R_OK) != 0)
      continue;
    if (!vk_path.empty())
      vk_path += ':';
    vk_path += dir;
  }
  if (vk_path.empty())
    return; // nothing to add, volk will report the failure

  std::string dyld_path = vk_path;
  if (const char *existing = std::getenv("DYLD_LIBRARY_PATH");
      existing && *existing) {
    if (std::string(existing).find(vk_path) != std::string::npos)
      return;
    dyld_path += ':';
    dyld_path += existing;
  }
  setenv("DYLD_LIBRARY_PATH", dyld_path.c_str(), 1);
  setenv("REBLUE_MOLTENVK_RELAUNCHED", "1", 1);
  execv(exe.c_str(), argv);
  // execv only returns on failure, fall through and let startup continue.
}

} // namespace

bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out) {
  // The SDK owns the single SDL window, fetch it the same way the X11 path and
  // reblue_app.cpp's ApplyWindowSizeConstraints do.
  (void)window;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!sdl_window) {
    BD_ERROR("No SDL window available for the Vulkan surface");
    return false;
  }

  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
  out.window = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);

  // The view is a subview of the SDL window and dies with it, so it is cached
  // against the window it was made for rather than unconditionally: a
  // recreated window gets a fresh view instead of a dangling one.
  static SDL_Window *view_owner = nullptr;
  static SDL_MetalView metal_view = nullptr;
  if (!metal_view || view_owner != sdl_window) {
    metal_view = SDL_Metal_CreateView(sdl_window);
    view_owner = sdl_window;
  }
  if (!metal_view) {
    BD_ERROR("SDL_Metal_CreateView failed: {}", SDL_GetError());
    return false;
  }
  out.view = SDL_Metal_GetLayer(metal_view);

  if (!out.window || !out.view) {
    BD_ERROR(
        "SDL window exposed no NSWindow/CAMetalLayer for the Metal surface");
    return false;
  }
  return true;
}

#elif defined(__ANDROID__)

bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out) {
  // Current ReXGlue Android exposes the exact SDL_Window owned by this
  // rex::ui::Window. Do not guess via SDL_GetWindows()[0]: SDL may keep more
  // than one window around during Android activity/surface transitions, and a
  // swapchain created for the wrong SDL window can present successfully while
  // the visible SurfaceView remains black.
  if (!window) {
    BD_ERROR("GetNativeRenderWindow called with null window");
    return false;
  }

  auto *sdl_window =
      static_cast<SDL_Window *>(window->GetSDLWindowHandle());
  if (!sdl_window) {
    BD_ERROR("ReXGlue window has no SDL_Window handle yet");
    bd::AndroidDiag("native_window direct=null");
    return false;
  }

  // V019 experimented with a reduced Android buffer geometry. Reset the
  // native Surface to its default dimensions whenever a surface is acquired
  // (including after activity resume), so an already-created BufferQueue
  // cannot retain the reduced extent across an app update or lifecycle hop.
  {
    SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
    auto *native_window = static_cast<ANativeWindow *>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    if (native_window) {
      const int before_w = ANativeWindow_getWidth(native_window);
      const int before_h = ANativeWindow_getHeight(native_window);
      const int reset_result =
          ANativeWindow_setBuffersGeometry(native_window, 0, 0, 0);
      bd::AndroidDiag(std::format(
          "android_surface_reset before={}x{} result={} after={}x{}",
          before_w, before_h, reset_result, ANativeWindow_getWidth(native_window),
          ANativeWindow_getHeight(native_window)));
    }
  }

  out = sdl_window;

  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *first = (windows && count > 0) ? windows[0] : nullptr;
  const SDL_WindowID id = SDL_GetWindowID(sdl_window);
  const auto flags = static_cast<unsigned long long>(SDL_GetWindowFlags(sdl_window));
  bd::AndroidDiag(std::format(
      "native_window direct={} id={} flags=0x{:X} count={} first={} match_first={}",
      static_cast<void *>(sdl_window), static_cast<unsigned>(id), flags, count,
      static_cast<void *>(first), first == sdl_window));
  SDL_free(windows);
  return true;
}

#else

bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out) {
  // The SDK exposes no SDL_Window accessor and GetNativeWindowHandle is null
  // off Windows. The app owns a single window, fetched the same way
  // ApplyWindowSizeConstraints does. plume takes it whole so the surface
  // follows whichever video driver SDL picked.
  (void)window;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  out = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!out) {
    BD_ERROR("No SDL window available for the Vulkan surface");
    return false;
  }
  return true;
}

#endif

} // namespace bd::platform

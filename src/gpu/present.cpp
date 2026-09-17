/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <thread>

#include <plume_render_interface.h>

#include "gpu/gpu_profiling.h"

#include "core/android_diag.h"
#include "core/logging.h"
#include "engine/sofdec_player.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/output.h"
#include "gpu/settings.h"
#include "platform/native_window.h"

namespace bd::gpu {

namespace {

// setVsyncEnabled only assigns the bool the present path reads, so applying it
// every frame is free and stops a resize (which rebuilds the swapchain with
// vsync on) leaving a stale value.
void ApplyVsync(VideoState &s) {
  if (s.swap_chain) {
    s.swap_chain->setVsyncEnabled(Settings::Get().Vsync());
  }
}

constexpr i32 kIdleFPS = 30;

// Sleeps (no busy-wait) so consecutive presents sit at least 1000/bd_fps_limit
// ms apart, and 0 disables. Pacing the render thread back-pressures the guest
// main thread through the DrawEnd event. Runs even under vsync, which paces to
// the monitor instead, and a 120Hz panel ran the loop at 120 under a 60 cap.
void PaceFrame(bool idle = false) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  i32 fps = bd::engine::Settings::Get().FPSLimit();
  // The Sofdec movie clock advances from the per-frame delta inside BD's
  // 30Hz-gated logic, so it only runs at 1.0x when the engine ticks at 30Hz.
  if (bd::engine::SofdecPlayer::Playing())
    fps = 30;
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    std::this_thread::sleep_until(next);
    next += period;
  } else {
    next = now + period;
  }
}

// A recorded list must still keep the ring's per-slot fence discipline. A frame
// that opened none deliberately leaves the ring unrotated, so destroys stamped
// this frame carry to this slot's next reuse. Releases 'lock' when it drains.
void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

// Rebuild the swapchain and everything keyed to its back buffers.
void RebuildSwapChain(VideoState &s) {
  // Execute the open list, do not just end it: plume's CPU-side state tracker
  // updates on barriers() while the GPU transitions fire on execute, so
  // abandoning a list leaves the tracker claiming states the GPU never reached.
  SubmitOpenListLocked(s);
  // Every slot's back buffer / framebuffer reference must retire before
  // resize() destroys the old back buffer COMs, so wait every submitted slot's
  // fence.
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      BD_ERROR("Swap chain resize failed"); // minimized 0x0 is benign
  }
}

bool RecreateSwapChainForSurface(VideoState &s) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }

  s.framebuffers.clear();
  s.render_semaphores.clear();

  plume::RenderWindow render_window{};
  if (!s.host_window ||
      !bd::platform::GetNativeRenderWindow(s.host_window, render_window)) {
    BD_ERROR("Android surface restore: native render window unavailable");
    bd::AndroidDiag("lifecycle recreate failed native_window");
    return false;
  }

  s.swap_chain.reset();
  plume::RenderSwapChainDesc desc(render_window,
                                  plume::RenderFormat::B8G8R8A8_UNORM,
                                  kNumFrames + 1, false, kNumFrames);
  s.swap_chain = s.queue->createSwapChain(desc);
  if (!s.swap_chain || !s.swap_chain->resize() || s.swap_chain->isEmpty()) {
    BD_ERROR("Android surface restore: createSwapChain failed");
    bd::AndroidDiag("lifecycle recreate failed swapchain");
    return false;
  }

  for (u32 i = 0; i < kNumFrames; ++i)
    s.acquire_semaphores[i] = s.device->createCommandSemaphore();

  if (!BuildFramebuffers(s) || !BuildPresentSemaphores(s)) {
    BD_ERROR("Android surface restore: framebuffer/semaphore rebuild failed");
    bd::AndroidDiag("lifecycle recreate failed framebuffer");
    return false;
  }

  ApplyVsync(s);
  s.resize_requested.store(false, std::memory_order_release);
  bd::AndroidDiag(std::format("lifecycle recreate ok swap={}x{} images={}",
                              s.swap_chain->getWidth(),
                              s.swap_chain->getHeight(),
                              s.swap_chain->getTextureCount()));
  return true;
}

// BD renders its whole frame with RT[0] implicit, so the finished image lives
// in the back_buffer_surface placeholder and the frontBuffer handed to Swap is
// an empty guest surface that must not outrank it. 'chosen' reports the pick
// before the deferred resolve redirect, which the late-clear guard needs.
GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer,
                                  GuestTexture *&chosen) {
  GuestTexture *last_rt = s.last_drawn_rt[s.recording_slot()];
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  chosen = rt;
  // A deferred resolve dst still has its content in the source surface. Not for
  // an MSAA source: the gamma blit samples a Texture2D SRV while the descriptor
  // is a Texture2DMS view.
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

// Per-frame order: RT[0] -> COLOR_WRITE (+ late clear if no draw bound it)
// -> SHADER_READ, then back buffer -> COLOR_WRITE, fullscreen tri sampling
// RT[0]
// -> PRESENT.
void RecordPresentPass(VideoState &s, GuestTexture *rt, GuestTexture *chosen,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  BD_GPU_ZONE("RecordPresentPass");
  if (rt->layout != plume::RenderTextureLayout::COLOR_WRITE &&
      rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE));
    rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
    // discardTexture requires RT/DS state, which the barrier just set. Same as
    // BindDrawFramebuffer.
    if (needs_discard)
      s.command_list->discardTexture(rt->texture);
  }
  // Late clear only for the placeholder back buffer (no draws this frame).
  // Clearing last_resolved_dst/last_drawn_rt would erase what is being blitted.
  const bool clear_target_is_engine_content =
      (chosen == s.last_resolved_dst) ||
      (chosen == s.last_drawn_rt[s.recording_slot()]) || (rt != chosen);
  if (s.clear_pending && !clear_target_is_engine_content &&
      rt->layout == plume::RenderTextureLayout::COLOR_WRITE) {
    plume::RenderFramebuffer *rt_fb = GetFramebuffer(s, rt, nullptr);
    if (rt_fb) {
      s.command_list->setFramebuffer(rt_fb);
      s.command_list->clearColor(0, ArgbToRenderColor(s.clear_color_argb));
      s.command_list->setFramebuffer(nullptr);
    }
  }

  s.clear_pending = false;

  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(rt->texture,
                                  plume::RenderTextureLayout::SHADER_READ));
  rt->layout = plume::RenderTextureLayout::SHADER_READ;

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  u32 gamma_src_desc = rt->descriptorIndex;
  if (s.descriptor_compat_mode) {
    if (!BindCompatHostTextureLocked(s, rt, s.default_sampler.get())) {
      BD_ERROR("Present: failed to bind compatibility source descriptor");
      return;
    }
    gamma_src_desc = 0;
  }

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  // Fits what BD actually rendered rather than what the cvar asks for, so a
  // live aspect change or a resize cannot stretch the image: neither moves the
  // render rect. A Sofdec movie is prerendered 16:9 and BD stretches it
  // across that rect, so fitting the present to the design ratio squeezes it
  // back out. Stretch mode asked for the distortion and keeps it.
  const double present_aspect =
      (bd::engine::SofdecPlayer::Playing() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);
  if (fit_w != swap_w || fit_h != swap_h) {
    // Clear the whole back buffer so the uncovered edges show as black bars.
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
  // pow(color, guest scanout ramp exponent * kPresentGamma), then the guest TV
  // display correction curve at kPresentDisplayCorrection strength. Pipeline
  // layout + bindless sets were bound once at BeginCommandList.
  constexpr float kPresentGamma = 1.0f;             // guest ramp unscaled
  constexpr float kPresentDisplayCorrection = 1.0f; // full X360 scanout curve
  s.command_list->setPipeline(s.gamma_correction_pipeline.get());
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  } pc{gamma_src_desc, 0, s.guest_gamma * kPresentGamma,
       kPresentDisplayCorrection};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  // Overlays (the F3 menu) cover the whole window, not the letterboxed rect.
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  // Onto the back buffer while it is still bound + COLOR_WRITE. The hook
  // marshals to the UI thread (ImGui is not thread-safe) and records into this
  // list while this guest thread is parked, so it runs under the s.mutex we
  // already hold.
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

void Video::RequestClear(u32 flags, u32 color_argb, float depth, u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.clear_pending = true;
  s.clear_flags = flags;
  s.clear_color_argb = color_argb;
  s.clear_depth = depth;
  s.clear_stencil = stencil;
  // BD's frame model is Clear -> draws -> Swap, so the first Clear is the frame
  // boundary. Opening the list twice is safe, so the paired second Clear is a
  // no-op. Without it draws record into a closed list and vanish.
  s.frame_present_committed = false;
  BeginCommandList(s);

  // X360 Clear hits the bound RT immediately, while reblue defers so the next
  // draw folds it into a load op. A color clear whose RT then receives no draw
  // would float to an unrelated RT, so between passes with a real color target
  // clear now. The deferred path still covers depth, stencil and mid-pass.
  GuestTexture *rt = s.render_target;
  if (s.command_list_open && (flags & 0x1u) != 0 && rt && rt->texture &&
      !s.draw_framebuffer_bound) {
    // The clear wipes rt, so deferred resolves out of it must copy first. A
    // pending resolve into it is fully replaced, so that link just drops.
    MaterializeOutboundLocked(s, rt);
    DetachSourceSurfaceLocked(s, rt);
    const bool fresh = rt->layout == plume::RenderTextureLayout::UNKNOWN;
    if (rt->layout != plume::RenderTextureLayout::COLOR_WRITE) {
      plume::RenderTextureBarrier b(rt->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE);
      s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &b, 1);
      rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
    }
    if (fresh)
      s.command_list->discardTexture(rt->texture);
    if (plume::RenderFramebuffer *fb = GetFramebuffer(s, rt, nullptr)) {
      s.command_list->setFramebuffer(fb);
      s.command_list->clearColor(0, ArgbToRenderColor(color_argb));
      s.command_list->setFramebuffer(nullptr);
      s.clear_flags &= ~0x1u;
      if ((s.clear_flags & 0x30u) == 0)
        s.clear_pending = false;
    }
  }
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

void Video::NotifySurfaceLost() {
  auto &s = state();
  s.surface_available.store(false, std::memory_order_release);
  bd::AndroidDiag("video surface_available=false");
}

void Video::NotifySurfaceRestored() {
  auto &s = state();
  s.surface_available.store(true, std::memory_order_release);
  s.surface_recreate_requested.store(true, std::memory_order_release);
  bd::AndroidDiag("video surface_available=true recreate_requested=true");
}

void Video::PresentDiagnosticColor() {
#if defined(__ANDROID__)
  auto &s = state();
  std::unique_lock lock(s.mutex);

  bd::AndroidDiag("native_diag=PresentDiagnosticColor enter");
  if (!s.ready || !s.swap_chain || s.shutting_down.load(std::memory_order_acquire)) {
    bd::AndroidDiag("native_diag=PresentDiagnosticColor unavailable");
    return;
  }
  bd::AndroidDiag(std::format("native_diag=gpu backend='{}' device='{}'",
                              s.backend_info,
                              s.device ? s.device->getDescription().name : "<null>"));

  if (s.swap_chain->needsResize())
    RebuildSwapChain(s);
  if (s.framebuffers.empty()) {
    bd::AndroidDiag("native_diag=PresentDiagnosticColor no_framebuffers");
    return;
  }

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    bd::AndroidDiag("native_diag=PresentDiagnosticColor acquire_failed");
    return;
  }

  BeginCommandList(s);
  if (!s.command_list_open) {
    bd::AndroidDiag("native_diag=PresentDiagnosticColor no_command_list");
    return;
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.45f, 0.0f, 0.65f, 1.0f));
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  FrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  const bool present_ok = s.swap_chain->present(texture_index, signals, 1);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;

  u32 render_w = 0, render_h = 0;
  Output::RenderSize(render_w, render_h);
  bd::AndroidDiag(std::format(
      "native_diag=early_present result={} swap={}x{} render={}x{} image={}",
      present_ok, s.swap_chain->getWidth(), s.swap_chain->getHeight(), render_w,
      render_h, texture_index));

  lock.unlock();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
#endif
}

void Video::Present(GuestTexture *frontBuffer) {
  auto &s = state();
#if defined(__ANDROID__)
  static std::atomic<bool> s_first_present_trace{false};
  if (!s_first_present_trace.exchange(true, std::memory_order_acq_rel))
    bd::AndroidDiag("TRACE guest first Video::Present ENTER");
#endif
  // Before the lock: shutdown runs on the UI thread, so a Present that reached
  // the overlay hook would marshal into a thread no longer pumping, holding
  // s.mutex while it waits.
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  // One back buffer present per engine frame, and RequestClear reopens the
  // gate.
  if (s.frame_present_committed) {
    return;
  }

#if defined(__ANDROID__)
  if (!s.surface_available.load(std::memory_order_acquire)) {
    AbandonFrame(s, lock);
    return;
  }
  if (s.surface_recreate_requested.exchange(false,
                                             std::memory_order_acq_rel)) {
    if (!RecreateSwapChainForSurface(s)) {
      s.surface_recreate_requested.store(true, std::memory_order_release);
      AbandonFrame(s, lock);
      return;
    }
  }

  static std::atomic<u32> s_android_present_count{0};
  const u32 android_present_n =
      s_android_present_count.fetch_add(1, std::memory_order_relaxed);
  const bool android_diag_log =
      android_present_n < 12 || (android_present_n % 120) == 0;
  if (android_diag_log) {
    bd::AndroidDiag(std::format(
        "present #{} enter movie={} swap={}x{} fb_count={}",
        android_present_n, bd::engine::SofdecPlayer::Playing(),
        s.swap_chain ? s.swap_chain->getWidth() : 0,
        s.swap_chain ? s.swap_chain->getHeight() : 0,
        s.framebuffers.size()));
    BD_INFO("[android-diag] Present #{} enter movie={} swap={}x{} fb_count={}",
            android_present_n, bd::engine::SofdecPlayer::Playing(),
            s.swap_chain ? s.swap_chain->getWidth() : 0,
            s.swap_chain ? s.swap_chain->getHeight() : 0,
            s.framebuffers.size());
  }
#endif

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  // Empty after a failed or skipped resize (minimized), and indexing it is a
  // UAF.
  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  };
  PresentBreakdown pb;

  u32 texture_index = 0;
  {
    BD_CPU_ZONE("AcquireTexture");
    const auto t0 = Clock::now();
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
        AbandonFrame(s, lock);
        return;
      }
    }
    pb.acquire_ms = ms_since(t0);
#if defined(__ANDROID__)
    if (android_diag_log) {
      bd::AndroidDiag(std::format("present #{} acquire ok image={}",
                                  android_present_n, texture_index));
      BD_INFO("[android-diag] Present #{} acquire ok image={}",
              android_present_n, texture_index);
    }
#endif
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();

  GuestTexture *chosen = nullptr;
  GuestTexture *rt = SelectPresentSource(s, frontBuffer, chosen);
#if defined(__ANDROID__)
  if (android_diag_log) {
    bd::AndroidDiag(std::format(
        "present #{} source rt={} size={}x{} desc={} samples={} front={} front_size={}x{}",
        android_present_n, static_cast<void *>(rt), rt ? rt->width : 0,
        rt ? rt->height : 0,
        rt ? rt->descriptorIndex : kInvalidDescriptorIndex,
        rt ? static_cast<u32>(rt->sampleCount) : 0,
        static_cast<void *>(frontBuffer),
        frontBuffer ? frontBuffer->width : 0,
        frontBuffer ? frontBuffer->height : 0));
    BD_INFO("[android-diag] Present #{} source rt={} {}x{} desc={} samples={} front={} {}x{}",
            android_present_n, static_cast<void *>(rt), rt ? rt->width : 0,
            rt ? rt->height : 0,
            rt ? rt->descriptorIndex : kInvalidDescriptorIndex,
            rt ? static_cast<u32>(rt->sampleCount) : 0,
            static_cast<void *>(frontBuffer),
            frontBuffer ? frontBuffer->width : 0,
            frontBuffer ? frontBuffer->height : 0);
  }
#endif

  // An MSAA rt must never reach the gamma blit: its descriptor is a Texture2DMS
  // view, the blit shader samples a Texture2D. A correct frame ends on a
  // resolved single-sample surface, so an MSAA one here is already wrong
  // upstream, so skip the blit rather than crash the present.
  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      BD_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  // Reopens a closed list, so end() below always has one.
  BeginCommandList(s);
  RecordPresentPass(s, rt, chosen, back, back_fb);

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  FrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  // Indexed by the acquired swapchain image, not the frame-in-flight slot:
  // see the render_semaphores declaration on VideoState.
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  // NOT the frame just submitted: AdvanceAndWaitReused waits the slot about to
  // be reused, one frame old. That gap is the CPU/GPU overlap.
  {
    BD_CPU_ZONE("Submit");
    const auto t0 = Clock::now();
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
    pb.submit_ms = ms_since(t0);
  }
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  {
    BD_CPU_ZONE("PresentSwap");
    const auto t0 = Clock::now();
    // A removed device fails Present first, and the fence wait below still
    // returns (removal signals every fence), so without this the next D3D12
    // call is the one that reports the loss.
    const bool present_ok = s.swap_chain->present(texture_index, signals, 1);
#if defined(__ANDROID__)
    if (android_present_n == 0)
      bd::AndroidDiag(std::format(
          "TRACE guest first swapchain present result={}", present_ok));
    if (android_diag_log) {
      bd::AndroidDiag(std::format("present #{} queue result={}",
                                  android_present_n, present_ok));
      BD_INFO("[android-diag] Present #{} queue present result={}",
              android_present_n, present_ok);
    }
#endif
    if (!present_ok) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
    pb.present_ms = ms_since(t0);
  }
  const auto wait_t0 = Clock::now();
  {
    BD_CPU_ZONE("WaitFence");
    AdvanceAndWaitReused(s);
  }
  pb.fence_ms = ms_since(wait_t0);
  RecordGPUWait(pb.fence_ms);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  BD_FRAME_MARK();
  UpdateFrameStats();
  // The reused slot's fence has signalled, so resources released kNumFrames ago
  // can no longer be referenced by in-flight work. Drop the lock first, since
  // DrainSlot re-acquires it per entry.
  lock.unlock();
  {
    BD_CPU_ZONE("DrainSlot");
    const auto t0 = Clock::now();
    DrainSlot(s, reclaimed);
    pb.drain_ms = ms_since(t0);
  }
  {
    BD_CPU_ZONE("PaceFrame");
    const auto t0 = Clock::now();
    PaceFrame();
    pb.pace_ms = ms_since(t0);
  }
  RecordFrameSample(pb);
}

void Video::SkipPresent() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        BD_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  FrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  // Indexed by the acquired swapchain image, not the frame-in-flight slot:
  // see the render_semaphores declaration on VideoState.
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
  RecordBlankFrameSample();
  // No PaceFrame: the installer tick scheduler paces, and sleeping here would
  // only delay SDL event handling on the UI thread.
}

} // namespace bd::gpu

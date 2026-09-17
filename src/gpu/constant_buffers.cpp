/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/android_compat_slots.h"
#include "gpu/shaders/shader_cache.h"

namespace bd::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
  ConstantAllocation cachedVS{};
  ConstantAllocation cachedPS{};
  ConstantAllocation cachedShared{};
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, and the fetch constants almost never change between draws, so
// a 24-byte compare replaces them on the hot path. Sampler heap slots are never
// reclaimed, so a cached index stays valid until device teardown.
struct SamplerSlotCache {
  u32 fc[6]{};
  u32 sampler = 0;
  i32 aniso = -1;
  bool clamp3d = false;
  bool valid = false;
};

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[16];
  float shadowPcfScale = 1.0f;
  bool sharedBound = false;
  bool ready = false;
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

// Shrink the sun shadow PCF kernel inversely to the coverage box so its
// world-space penumbra stays constant as ShadowCoverageScale widens the light
// frustum, floored at one texel of the actual shadow map. Once per frame, not
// per draw, so a distance change applies without a restart (the dimension term
// lags a pending restart-gated change until the map is recreated).
void RecomputeShadowPcfScale(UploadState &s) {
#if defined(__ANDROID__)
  const f64 dist = std::clamp(ShadowCoverageScale(), 0.25, 4.0);
  const f64 dim = std::max(128.0, static_cast<f64>(ShadowMapDimension()));
#else
  const f64 dist = std::clamp(ShadowCoverageScale(), 1.0, 4.0);
  const f64 dim = std::max(512, Settings::Get().ShadowDimension());
#endif
  s.shadowPcfScale = static_cast<float>(std::max(1.0 / dist, 1024.0 / dim));
}

bool CreateChunk(UploadChunk &chunk) {
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  auto flags = plume::RenderBufferFlag::CONSTANT |
               plume::RenderBufferFlag::VERTEX |
               plume::RenderBufferFlag::INDEX;
  const bool needs_device_address = !bd::gpu::state().descriptor_ubo_mode;
  if (needs_device_address)
    flags = flags | plume::RenderBufferFlag::DEVICE_ADDRESSABLE;
  chunk.buffer = bd::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(kUploadChunkSize, flags),
      "cb-upload-chunk");
  if (!chunk.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    BD_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = needs_device_address ? chunk.buffer->getDeviceAddress() : 0;
  return true;
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      BD_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so BD's
// degenerate constants (e.g. the bloom/glare 0/0 weight normalization when
// intensity is zero) are harmless on hardware. Our recompiled D3D12 shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant propagates and blackens the
// post-fx composite. No BD shader reinterprets a float constant register as
// int, so the flush cannot corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  for (u32 i = 0; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  up.cachedVS = {};
  up.cachedPS = {};
  up.cachedShared = {};
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() { upload_state().sharedBound = false; }

// c50.xy is BD's NDC->UV half-scale (0.5 on hw). bd_blur_ps reconstructs its
// sample UV as uv = c50.xy*(ndc+1), so it MUST be 0.5. The guest derives it
// from sceneDim/1280x720*0.5, letting output res and supersampling leak in, and
// the pass oversamples until the source collapses into the top-left. bd_blur_ps
// is the ONLY pixel shader reading c50 as this screen->UV scale (verified
// across all 17 c50-using PS). The rest own c50 as material data, so a blanket
// pin would corrupt them (bd_lightshaft_ps's g_vLightShaftDiffuse -> gray
// god rays). bdCameraRefractionUvScaleHook pins device reg50 at the
// bdCameraRender writers, and this catches blur draws those writers miss.
constexpr u32 kScreenUVScaleRegByteOffset = 50 * 16;
constexpr u64 kBDBlurPSHash = 0xD94E164866C3B9BCull;
void PinScreenUVScaleReg(u8 *block) {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 h = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  if (h == kBDBlurPSHash) {
    auto *reg = reinterpret_cast<float *>(block + kScreenUVScaleRegByteOffset);
    reg[0] = 0.5f;
    reg[1] = 0.5f;
  }
}

ConstantAllocation UploadVertexShaderConstants(u32 device_guest,
                                               bool force_upload) {
  BD_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  FrameUpload &up = s.frames[s.cursor];
  if (!force_upload && up.cachedVS.size)
    return up.cachedVS;
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, vsFloatConstants),
                         kConstantBlockBytes);
  up.cachedVS = alloc;
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest,
                                              bool force_upload) {
  BD_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  FrameUpload &up = s.frames[s.cursor];
  if (!force_upload && up.cachedPS.size)
    return up.cachedPS;
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, psFloatConstants),
                         kConstantBlockBytes);
  PinScreenUVScaleReg(alloc.memory);
  up.cachedPS = alloc;
  return alloc;
}

ConstantAllocation UploadSharedConstants(u32 device_guest,
                                         const ConstantAllocation *vs_alloc,
                                         const ConstantAllocation *ps_alloc) {
  BD_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // bd_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: our SetTexture hook replaces BD's recompiled
  // body, so the engine's per-slot bound-texture shadow (device+0x2FF0+slot*4)
  // and GPU texture fetch constants (device+0x400+slot*0x18) are never written.
  // Video::SetTexture mirrors Xenos semantics (a null bind is ignored, since on
  // hardware it does not rebuild the fetch constant), so vs.textures holds the
  // last real texture per slot, exactly what the GPU would still be sampling.
  auto &vs = bd::gpu::state();
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  CompatDescriptorBundle *compat_bundle = nullptr;
  u32 compat_2d[16]{};
  u32 compat_3d[16]{};
  u32 compat_cube[16]{};
  u32 compat_sampler[16]{};
  u16 compat_2d_mask = 0;
  u16 compat_3d_mask = 0;
  u16 compat_cube_mask = 0;
  u16 compat_sampler_mask = 0;
  if (vs.descriptor_compat_mode) {
    compat_bundle = AllocateCompatDescriptorBundleLocked(vs);
    if (!compat_bundle) {
      BD_ERROR("descriptor compat: failed to allocate per-draw descriptor bundle");
      return {};
    }

    const GuestShader *ps = vs.pipelineState.pixelShader
                                ? vs.pipelineState.pixelShader
                                : vs.pixel_shader;
    const u64 ps_hash =
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    const AndroidCompatSlotMask *mask = FindAndroidCompatSlotMask(ps_hash);
    AndroidCompatSlotMask host_fallback{};
    if (!mask && ps && !ps->shaderCacheEntry) {
      // hcgPixelShaderCreateByHlsl creates the host 2D blit shader without a
      // guest cache entry. It only reads Tex0 and sampler0.
      host_fallback = {0, 0x0001, 0, 0, 0x0001};
      mask = &host_fallback;
    }
    if (!mask) {
      static u64 last_missing_hash = ~0ull;
      if (last_missing_hash != ps_hash) {
        last_missing_hash = ps_hash;
        BD_ERROR("descriptor compat: no slot mask for pixel shader 0x{:016X}",
                 ps_hash);
      }
      return {};
    }

    compat_2d_mask = mask->texture2D;
    compat_3d_mask = mask->texture3D;
    compat_cube_mask = mask->textureCube;
    compat_sampler_mask = mask->samplers;
    const auto dense = [](u16 bits, u32 capacity, u32 (&map)[16]) {
      u32 next = 0;
      for (u32 slot = 0; slot < 16; ++slot) {
        if ((bits & (u16(1) << slot)) != 0)
          map[slot] = next++;
      }
      return next <= capacity;
    };
    if (!dense(compat_2d_mask, kAndroidCompatTexture2DCapacity, compat_2d) ||
        !dense(compat_3d_mask, kAndroidCompatTexture3DCapacity, compat_3d) ||
        !dense(compat_cube_mask, kAndroidCompatTextureCubeCapacity,
               compat_cube) ||
        !dense(compat_sampler_mask, kAndroidCompatSamplerCapacity,
               compat_sampler)) {
      BD_ERROR("descriptor compat: compact capacity exceeded for PS "
               "0x{:016X}",
               ps_hash);
      return {};
    }

    // Recycled fixed descriptor arrays stay fully valid from their initial
    // fill. Reset only the dense entries this shader may actually access; the
    // resource walk below then overwrites entries backed by real resources.
    for (u32 i = 0; i < 16; ++i) {
      const u16 bit = u16(1) << i;
      if (compat_2d_mask & bit)
        compat_bundle->texture2D->setTexture(
            compat_2d[i], vs.null_textures[kNullTexture2DDescriptorIndex].get(),
            plume::RenderTextureLayout::SHADER_READ,
            vs.null_texture_views[kNullTexture2DDescriptorIndex].get());
      if (compat_3d_mask & bit)
        compat_bundle->texture3D->setTexture(
            compat_3d[i], vs.null_textures[kNullTexture3DDescriptorIndex].get(),
            plume::RenderTextureLayout::SHADER_READ,
            vs.null_texture_views[kNullTexture3DDescriptorIndex].get());
      if (compat_cube_mask & bit)
        compat_bundle->textureCube->setTexture(
            compat_cube[i], vs.null_textures[kNullTextureCubeDescriptorIndex].get(),
            plume::RenderTextureLayout::SHADER_READ,
            vs.null_texture_views[kNullTextureCubeDescriptorIndex].get());
      if (compat_sampler_mask & bit)
        compat_bundle->samplers->setSampler(compat_sampler[i],
                                            vs.default_sampler.get());
    }
  }
  for (u32 i = 0; i < 16; ++i) {
    if (compat_bundle) {
      // Preserve the X360 0..15 guest slot ABI in SharedConstants while the
      // Vulkan descriptor sets contain only the slots this shader can read.
      s.shared.samplerIndices[i] = compat_sampler[i];
      s.shared.texture2DIndices[i] = compat_2d[i];
      s.shared.texture3DIndices[i] = compat_3d[i];
      s.shared.textureCubeIndices[i] = compat_cube[i];
    } else {
      s.shared.samplerIndices[i] = 0;
      s.shared.texture2DIndices[i] = bd::gpu::kNullTexture2DDescriptorIndex;
      s.shared.texture3DIndices[i] = bd::gpu::kNullTexture3DDescriptorIndex;
      s.shared.textureCubeIndices[i] = bd::gpu::kNullTextureCubeDescriptorIndex;
    }

    bd::gpu::GuestTexture *tex = vs.textures[i];
    if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
        tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        tex->sourceSurface != vs.render_target &&
        tex->sourceSurface != vs.depth_stencil &&
        tex->sourceSurface->descriptorIndex !=
            bd::gpu::kInvalidDescriptorIndex) {
      tex = tex->sourceSurface;
    }
    if (tex && tex->texture && tex->textureView &&
        (compat_bundle ||
         tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex)) {
      switch (tex->viewDimension) {
      case plume::RenderTextureViewDimension::TEXTURE_3D:
        if (compat_bundle) {
          if ((compat_3d_mask & (u16(1) << i)) != 0)
            compat_bundle->texture3D->setTexture(
                compat_3d[i], tex->texture,
                plume::RenderTextureLayout::SHADER_READ,
                tex->textureView.get());
        } else {
          s.shared.texture3DIndices[i] = tex->descriptorIndex;
        }
        // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
        // volume as its slice-0 2D view too so tfetch2D samples the base layer.
        if (tex->companion2D && tex->companion2D->texture &&
            tex->companion2D->textureView &&
            (compat_bundle || tex->companion2D->descriptorIndex !=
                                  bd::gpu::kInvalidDescriptorIndex)) {
          if (compat_bundle) {
            if ((compat_2d_mask & (u16(1) << i)) != 0)
              compat_bundle->texture2D->setTexture(
                  compat_2d[i], tex->companion2D->texture,
                  plume::RenderTextureLayout::SHADER_READ,
                  tex->companion2D->textureView.get());
          } else {
            s.shared.texture2DIndices[i] = tex->companion2D->descriptorIndex;
          }
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_CUBE:
        if (compat_bundle) {
          if ((compat_cube_mask & (u16(1) << i)) != 0)
            compat_bundle->textureCube->setTexture(
                compat_cube[i], tex->texture,
                plume::RenderTextureLayout::SHADER_READ,
                tex->textureView.get());
        } else {
          s.shared.textureCubeIndices[i] = tex->descriptorIndex;
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_2D:
      case plume::RenderTextureViewDimension::UNKNOWN:
      default:
        if (compat_bundle) {
          if ((compat_2d_mask & (u16(1) << i)) != 0)
            compat_bundle->texture2D->setTexture(
                compat_2d[i], tex->texture,
                plume::RenderTextureLayout::SHADER_READ,
                tex->textureView.get());
        } else {
          s.shared.texture2DIndices[i] = tex->descriptorIndex;
        }
        // BD static reflection cubes bind as a 2D atlas yet the water/glass
        // shader cube-fetches the slot, so publish the sliced TextureCube
        // companion so tfetchCube resolves a real cube, not the null cube.
        if (tex->companionCube && tex->companionCube->texture &&
            tex->companionCube->textureView &&
            (compat_bundle || tex->companionCube->descriptorIndex !=
                                  bd::gpu::kInvalidDescriptorIndex)) {
          if (compat_bundle) {
            if ((compat_cube_mask & (u16(1) << i)) != 0)
              compat_bundle->textureCube->setTexture(
                  compat_cube[i], tex->companionCube->texture,
                  plume::RenderTextureLayout::SHADER_READ,
                  tex->companionCube->textureView.get());
          } else {
            s.shared.textureCubeIndices[i] = tex->companionCube->descriptorIndex;
          }
        }
        break;
      }

      // X360 stores sampler state in fetchConstants[N].dword[*]. The
      // SetSamplerState_* setters are unhooked and run their recompiled bodies,
      // so the address mode bits there are valid.
      if (device_p) {
        const auto &fc_be = device_p->fetchConstants[i];
        const u32 fc[6] = {
            u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
            u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
        };
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto desc = DecodeFromFetch(fc);

        // Shell fur volumes encode shell depth in W, and X360-default WRAP
        // wraps a z=0 fetch's second tap to the tip slice and halves density,
        // so force CLAMP to keep both taps on the dense base slice.
        if (clamp3d)
          desc.addressW = plume::RenderTextureAddressMode::CLAMP;

        if (compat_bundle) {
          if (auto *sampler = ResolveSamplerObjectLocked(desc)) {
            if ((compat_sampler_mask & (u16(1) << i)) != 0)
              compat_bundle->samplers->setSampler(compat_sampler[i], sampler);
          }
        } else {
          auto &sc = s.samplerSlots[i];
          if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
              std::memcmp(sc.fc, fc, sizeof(fc)) == 0) {
            s.shared.samplerIndices[i] = sc.sampler;
          } else {
            const u32 resolved = ResolveSlotLocked(desc);
            std::memcpy(sc.fc, fc, sizeof(fc));
            sc.sampler = resolved;
            sc.aniso = aniso_now;
            sc.clamp3d = clamp3d;
            sc.valid = true;
            s.shared.samplerIndices[i] = resolved;
          }
        }
      }
    }
  }

  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (device_guest) {
    const auto *device = bd::mem::at<const D3DDevice>(device_guest);
    for (u32 i = 0; i < 4; ++i) {
      s.shared.booleansArr[i] = device ? u32(device->vsBoolConstants[i]) : 0u;
      s.shared.booleansArr[4 + i] =
          device ? u32(device->psBoolConstants[i]) : 0u;
    }
  }

  s.shared.alphaThreshold = bd::gpu::Video::AlphaThreshold();
  s.shared.halfPixelOffsetX = 0.0f;
  s.shared.halfPixelOffsetY = 0.0f;
  s.shared.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  s.shared.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  s.shared.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  s.shared.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  s.shared.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  s.shared.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  s.shared.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  s.shared.shadowPcfScale = s.shadowPcfScale;

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  s.shared.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

  // UBO mode still needs a descriptor rebind every draw, but when the block is
  // byte-identical we can reuse the current frame-slot allocation instead of
  // copying another 352 bytes. Non-UBO preserves the original skip behavior.
  const bool shared_unchanged =
      s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0;
  FrameUpload &frame_up = s.frames[s.cursor];
  ConstantAllocation bound_shared{};
  if (shared_unchanged && vs.descriptor_ubo_mode &&
      frame_up.cachedShared.size) {
    bound_shared = frame_up.cachedShared;
  } else if (shared_unchanged && !vs.descriptor_ubo_mode) {
    if (compat_bundle)
      BindCompatDescriptorBundleLocked(vs, *compat_bundle);
    return {};
  } else {
    bound_shared = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
    if (!bound_shared.memory)
      return {};
    std::memcpy(bound_shared.memory, &s.shared, sizeof(SharedConstants));
    s.lastUploaded = s.shared;
    s.sharedBound = true;
    frame_up.cachedShared = bound_shared;
  }

  if (compat_bundle && vs.descriptor_ubo_mode) {
    if (!vs_alloc || !ps_alloc || !vs_alloc->size || !ps_alloc->size ||
        !vs_alloc->ref.ref || !ps_alloc->ref.ref || !bound_shared.ref.ref) {
      BD_ERROR("descriptor UBO: missing VS/PS/Shared constant allocation");
      return {};
    }

    const auto bind_ubo = [](plume::RenderDescriptorSet *set, u32 index,
                             const ConstantAllocation &a) {
      if (!set || !a.ref.ref || !a.size)
        return false;
      if (a.ref.offset > std::numeric_limits<u32>::max())
        return false;
      plume::RenderBufferStructuredView view(
          1, static_cast<u32>(a.ref.offset));
      set->setBuffer(index, a.ref.ref, a.size, &view);
      return true;
    };

    // Descriptor index 0..8 is Texture2D[9]. The following three single
    // ranges are set0 bindings 1/2/3: VS, PS and SharedConstants.
    if (!bind_ubo(compat_bundle->texture2D.get(), 9, *vs_alloc) ||
        !bind_ubo(compat_bundle->texture2D.get(), 10, *ps_alloc) ||
        !bind_ubo(compat_bundle->texture2D.get(), 11, bound_shared)) {
      BD_ERROR("descriptor UBO: failed to bind VS/PS/Shared constants");
      return {};
    }
  }

  if (compat_bundle)
    BindCompatDescriptorBundleLocked(vs, *compat_bundle);
  return bound_shared;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  BD_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    BD_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

} // namespace bd::gpu

/**
 * @file    engine/sofdec_player.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 */
#include "engine/sofdec_player.h"

#include <atomic>
#include <chrono>
#include <cstddef>

#include <rex/ppc.h>
#include <rex/types.h>

#include "engine/game.h"

namespace bd::engine {

namespace {

struct SofdecPlayer_t {
  /* 0x00 */ u8 _pad00[0x8C];
  /* 0x8C */ be_i32 status; // latched from mwPly by the present body
};
static_assert(offsetof(SofdecPlayer_t, status) == 0x8C);

constexpr i32 kNoStatus = -1;

// mwPly's status. Buffering counts as playing: the wait for the first frame is
// already the movie's time.
constexpr i32 kStatusPreparing = 1;
constexpr i32 kStatusAdvancing = 2;

// The present refreshes the deadline while the player reports playing and
// drops it as soon as the player reports anything else, leaving the hold as
// the backstop for a player freed without a last tick. Written on the render
// thread.
constexpr i64 kHoldNs = 250'000'000;
std::atomic<i64> g_untilNs{0};
std::atomic<u32> g_player{0};

i64 NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void OnPresent(u32 address) {
  const SofdecPlayer player(address);
  if (!player)
    return;
  const i32 status = player.Status();
  const bool playing = status == kStatusPreparing || status == kStatusAdvancing;
  g_player.store(address, std::memory_order_relaxed);
  g_untilNs.store(playing ? NowNs() + kHoldNs : 0, std::memory_order_relaxed);
}

} // namespace

bool SofdecPlayer::Playing() {
  return NowNs() < g_untilNs.load(std::memory_order_relaxed);
}

i32 SofdecPlayer::Status() const {
  const auto *self = Self<SofdecPlayer_t>();
  return self ? static_cast<i32>(self->status) : kNoStatus;
}

engine::SofdecPlayer Game::SofdecPlayer() const {
  return engine::SofdecPlayer(
      engine::SofdecPlayer::Playing() ? g_player.load(std::memory_order_relaxed)
                                      : 0);
}

} // namespace bd::engine

// SofdecPlayer__Update (render thread), after the status latch. r31 = the
// player.
void bdMoviePlaybackHook(PPCRegister &r31) { bd::engine::OnPresent(r31.u32); }

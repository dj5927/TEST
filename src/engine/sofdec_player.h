/**
 * @file    engine/sofdec_player.h
 * @brief   The mwPly wrapper a CRI::Sofdec::PlayTask owns, which is what
 *          plays a prerendered .sfd movie.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 */
#pragma once

#include <rex/types.h>

#include "engine/object.h"

namespace bd::engine {

class SofdecPlayer : public Object {
public:
  SofdecPlayer() = default;
  explicit SofdecPlayer(u32 address) : Object(address) {}

  // Sofdec has no teardown site, so playback is a deadline the present
  // refreshes while the player reports playing.
  static bool Playing();

  i32 Status() const; // mwPly status, 2 advances a frame, -1 when empty
};

} // namespace bd::engine

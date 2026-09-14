/**
 * @file    engine/menus/shop_mouse.cpp
 * @brief   Pointer support for the shop's per-row count arrows.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/memory_helpers.h"
#include "engine/d2anime/anime_mouse.h"
#include "engine/d2anime/d2anime.h"
#include "engine/virtual_buttons.h"
#include "reblue_init.h"

#include <rex/hook.h>
#include <rex/types.h>

REX_EXTERN(__imp__Shop__MainTask__UpdateCount);

namespace bd::engine {

namespace {

// Shop::MainTask keeps a menu per state in a table at +184, indexed by the
// state at +0x6C the same way its handlers reach it.
constexpr u32 kShopState = 0x6C;
constexpr u32 kShopStateMenus = 184;

// Where L_shp_buy_itmbtn.csv and its selling twin draw the two cur_01 arrows,
// in the row template's own coordinates. Each band spans both blink frames.
constexpr f32 kDownArrowX0 = 470.0f;
constexpr f32 kDownArrowX1 = 497.0f;
constexpr f32 kUpArrowX0 = 515.0f;
constexpr f32 kUpArrowX1 = 542.0f;

// The count moves on left and right, which a mouse has neither of, so a click
// on the arrows the row already draws is queued as the pad press the handler
// waits for. The guest keeps its own clamping, sound and redraw that way.
bool CountArrowClicked(u32 shopTask) {
  if (!MenuMouse::Get().PointerActive())
    return false;
  // Confirm is the click, since keybind_a carries LMB. The guest's own edge
  // keeps this to the frame the button went down.
  if (!CheckAction(GameAction::Confirm))
    return false;

  const u32 state = mem::try_load<u32>(shopTask + kShopState);
  const u32 menuVA =
      mem::try_load<u32>(shopTask + kShopStateMenus + state * sizeof(u32));
  if (!menuVA)
    return false;

  D2AnimeMenu menu(menuVA);
  if (!menu)
    return false;

  int row = -1;
  f32 x = 0.0f;
  if (!menu.PointerRowX(row, x) || row != menu.CursorIndex())
    return false;

  if (x >= kDownArrowX0 && x <= kDownArrowX1)
    PressButton(static_cast<int>(Button::Left));
  else if (x >= kUpArrowX0 && x <= kUpArrowX1)
    PressButton(static_cast<int>(Button::Right));
  else
    return false;
  return true;
}

} // namespace

} // namespace bd::engine

// Skipping the handler eats the confirm edge the same click would otherwise
// also be, so an arrow click steps the count instead of buying.
REX_HOOK_RAW(Shop__MainTask__UpdateCount) {
  const u32 shopTask = ctx.r3.u32;
  if (bd::engine::CountArrowClicked(shopTask))
    return;
  __imp__Shop__MainTask__UpdateCount(ctx, base);
}

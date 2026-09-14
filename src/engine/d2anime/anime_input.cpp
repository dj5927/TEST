/**
 * @file    engine/d2anime/anime_input.cpp
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include "engine/d2anime/anime_input.h"
#include "core/memory_helpers.h"
#include "engine/virtual_buttons.h"
#include "reblue_init.h"
#include <rex/types.h>

REX_IMPORT(__imp__bdInputCheckButton, InputCheckButton, u32(u32, u32, u32));
// The trailing argument is the caller's priority, which every in-game reader
// passes as zero.
REX_IMPORT(__imp__bdInputIsPressed, InputIsPressed, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__bdInputGetAnalogValue, InputGetAnalogValue,
           f64(u32, u32, u32, u32));

namespace bd::engine {

namespace {
constexpr u32 kInputManagerVA = 0x82DC9844; // -> the guest input manager
} // namespace

// The bdInputCheckButton hook only reaches guest callers. This one calls the
// original through REX_IMPORT, so without adding it here reblue's own menus
// would never see a host-synthesized press and the arrow keys would not move a
// cursor in them.
bool CheckButton(Button btn) {
  u32 inputMgr = bd::mem::load<u32>(kInputManagerVA);
  if (inputMgr && InputCheckButton(inputMgr, 0, static_cast<u32>(btn)) != 0)
    return true;
  return SynthesizedButton(btn);
}

bool ButtonHeld(Button btn) {
  u32 inputMgr = bd::mem::load<u32>(kInputManagerVA);
  if (inputMgr && InputIsPressed(inputMgr, 0, static_cast<u32>(btn), 0) != 0)
    return true;
  return SynthesizedButtonHeld(btn);
}

Button ActionButton(GameAction action) {
  const int btn = ActionMap::Get().Button(action);
  if (btn >= 0)
    return static_cast<Button>(btn);
  return action == GameAction::Cancel ? Button::B : Button::A;
}

bool CheckAction(GameAction action) {
  return CheckButton(ActionButton(action));
}

bool ActionHeld(GameAction action) { return ButtonHeld(ActionButton(action)); }

float StickValue(StickAxis axis) {
  u32 inputMgr = bd::mem::load<u32>(kInputManagerVA);
  if (!inputMgr)
    return 0.0f;
  return static_cast<float>(
      InputGetAnalogValue(inputMgr, 0, static_cast<u32>(axis), 0));
}

} // namespace bd::engine

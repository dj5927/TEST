/**
 * @file    engine/action_map.h
 * @brief   The engine's own action names and the button serving each one.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <optional>

#include <rex/types.h>

namespace bd::engine {

// The fifteen slots of the table bdGetCurrentMapPath (0x822D5090) selects, read
// by well over a hundred call sites: every menu, battle target selection, the
// gimmicks, the world map, the script system and both minigames.
//
// The names are the game's own. Each camp Config preset
// (d2anime\camp\cfg\L_key_01..04.csv) labels a controller diagram, and the
// labels move between anchors from preset to preset in exactly the pattern this
// table permutes its buttons, and that pattern pins a slot to an action.
enum class GameAction : int {
  Confirm = 0,       // しらべる/決定, the menu half
  Cancel = 1,        // キャンセル
  CampMenu = 2,      // キャンプメニュー
  FieldMenu = 3,     // フィールドメニュー
  Examine = 4,       // しらべる, the world half. Every gimmick reads this one
  Encounter = 5,     // エンカウント
  WorldMap = 6,      // ワールドマップ
  FieldSkill1 = 7,   // フィールドスキル１
  FieldSkill2 = 8,   // フィールドスキル２
  CenterCamera = 9,  // してんを自分のむきに
  DpadUp = 10,
  DpadDown = 11,
  DpadLeft = 12,
  DpadRight = 13,
  // Slot 14 carries a flag rather than a button: set, the direction readers
  // also accept the right stick. Zero in every shipped preset.
  RightStickAsDirection = 14,
};
inline constexpr int kGameActionCount = 15;

// Stable identifier, also the tail of the row's i18n key.
const char *ToString(GameAction action);

// Live view of the engine's table. Every accessor re-reads engine memory, so a
// change to the camp Config controller type option is picked up immediately and
// nothing here caches a mapping that the player can move.
class ActionMap {
public:
  static ActionMap &Get();

  // The pad button currently serving this action, as an anime_input Button
  // value. Negative when the table is unreachable or the slot carries a flag.
  int Button(GameAction action) const;

  // Which action a pad button serves. Confirm wins over Examine, which share a
  // button in every preset, because a prompt naming that button is far more
  // often a menu than a gimmick.
  std::optional<GameAction> ActionFor(int padButton) const;

private:
  ActionMap() = default;

  // Byte address of the fifteen-entry row for the player's controller type, or
  // zero when the engine image is not up yet.
  u32 Row() const;
};

} // namespace bd::engine

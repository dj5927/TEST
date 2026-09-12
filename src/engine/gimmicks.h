/**
 * @file    engine/gimmicks.h
 * @brief   How many of a map's gimmicks are still untouched: the search points
 *          the game calls Gmk::ReactGim, plus treasure chests and elemental
 *          barriers. Per map, or across the whole game.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <rex/types.h>

namespace bd::engine {

// The first eleven are Gmk::ReactGim's kind, in the order of the engine's own
// name table, and are what the search point CSVs spell NONE, MESS and so on.
// Chest and Barrier sit past that table.
enum class GimmickKind : u32 {
  Nothing,
  Message,
  Item,
  Gold,
  Heal,
  Damage,
  Status,
  Medal,
  Lock,
  Grass,
  Param,
  Chest,
  Barrier,
};
inline constexpr u32 kSearchKindCount = 11; // the ones a search point can be

const char *ToString(GimmickKind kind);

// bdFlagListLoad's own order.
enum class BarrierColor : u32 { Blue, Red, Green, White, Black };
inline constexpr u32 kBarrierColorCount = 5;

const char *ToString(BarrierColor color);

struct Tally {
  u32 total = 0;
  u32 remaining = 0;

  u32 Found() const { return total - remaining; }
  bool IsComplete() const { return remaining == 0; }
};

struct Marker {
  GimmickKind kind = GimmickKind::Nothing;
  bool collected = false;
  // False when a search point carries no flag: it respawns on every map load,
  // so collected is meaningless and it can never be counted down.
  bool trackable = false;
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
};

// Reads the baked table of what the game ships against the live flag array.
// Every query takes a map stem as bdStageNameBuild spells it ("dg05_01",
// "bi03d02"), or kEverywhere. A stem with no row tallies zero, including the
// one an off-field Stage().Name() gives.
//
// Points whose flag is -1 respawn on every map load and count in neither total
// nor remaining, so a floor made only of those reads as 0 of 0.
//
// The overworld is one script covering everything, so its chests file under
// "wd_world" rather than any wd_aNN. They still count in kEverywhere.
class Gimmicks {
public:
  static constexpr std::string_view kEverywhere = "*";

  static Gimmicks &Get();

  Gimmicks(const Gimmicks &) = delete;
  Gimmicks &operator=(const Gimmicks &) = delete;

  // True once the baked table parsed and the engine's flag array resolves.
  // Every tally is zero until then.
  bool IsReady() const;

  bool Has(std::string_view stem) const;

  Tally Points(std::string_view stem,
               std::optional<GimmickKind> kind = {}) const;
  Tally Chests(std::string_view stem) const;
  Tally Barriers(std::string_view stem,
                 std::optional<BarrierColor> color = {}) const;

  // Everything placed on one map, taken or not. The nine chests no script
  // places are absent, since nothing records where they would stand.
  std::vector<Marker> Markers(std::string_view stem) const;

private:
  Gimmicks();
  ~Gimmicks();

  struct Table;
  static std::unique_ptr<Table> ParseTable();
  static std::unique_ptr<Table> Parse(const u8 *data, size_t size,
                                      std::string_view origin);

  std::unique_ptr<Table> table_;
};

} // namespace bd::engine

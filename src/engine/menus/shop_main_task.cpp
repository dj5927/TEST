/**
 * @file    engine/menus/shop_main_task.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 */
#include "engine/menus/shop_main_task.h"

#include <cstddef>
#include <iterator>

#include "core/memory_helpers.h"

namespace bd::engine {

namespace {

struct ShopMainTask_t {
  /* 0x000 */ u8 _pad000[0x06C];
  /* 0x06C */ be_u32 state;
  /* 0x070 */ u8 _pad070[0x0B8 - 0x070];
  // Shop::MainTask::Init binds all ten at once, and its handlers reach a menu
  // by indexing this table with the state at 0x06C.
  /* 0x0B8 */ be_u32 stateMenus[10];
};
static_assert(offsetof(ShopMainTask_t, state) == 0x06C);
static_assert(offsetof(ShopMainTask_t, stateMenus) == 0x0B8);

} // namespace

AnimeMenu ShopMainTask::StateMenu() const {
  const auto *self = Self<ShopMainTask_t>();
  if (!self)
    return AnimeMenu();
  const u32 state = self->state;
  if (state >= std::size(self->stateMenus))
    return AnimeMenu();
  return AnimeMenu(self->stateMenus[state]);
}

} // namespace bd::engine

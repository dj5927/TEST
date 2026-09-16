/**
 * @file    engine/menus/title_menu.cpp
 * @brief   Title screen hooks - add "config" and "debug menu" entries.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/encoding.h"
#include "core/i18n.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/shutdown.h"
#include "engine/d2anime/anime_hittest.h"
#include "engine/d2anime/anime_mouse.h"
#include "engine/d2anime/d2anime.h"
#include "engine/menus/config_menu.h"
#include "engine/menus/config_menu_data.h"
#include "engine/menus/update_prompt.h"
#include "engine/settings.h"
#include "engine/language.h"
#include "engine/sfx.h"
#include "engine/state_layout.h"
#include "engine/virtual_buttons.h"
#include "gpu/gpu.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/ppc/stack.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

using bd::engine::Button;
using bd::engine::CheckButton;
using rex::memory::store_and_swap;

REXCVAR_DECLARE(i32, bd_opt_voice_type);

REX_IMPORT(__imp__bdColor4fToARGB, Color4fToARGB, u32(u32));
REX_IMPORT(__imp__bdTextCalcWidth, TextCalcWidth, f64(f64, u32, u32, u32, u32));
REX_IMPORT(__imp__SequenceHolder_FindSequenceByName, FindSequenceByName,
           u32(u32, u32));
REX_EXTERN(__imp__Visual__method_7E60);
REX_EXTERN(__imp__TitleTask_Update);
REX_EXTERN(__imp__TitleTask_Draw);
REX_EXTERN(__imp__TitleTask_OnChildComplete);
REX_EXTERN(__imp__TitleTask_PollInputStart);

namespace {

constexpr u32 kVisualRenderAddr = 0x82DC9848;
constexpr u32 kDebugMenuNameAddr = 0x82065130; // "DebugMenu" string

constexpr float kCursorBaseY = 528.0f;
constexpr float kEntrySpacing = 32.0f;
constexpr float kTextCursorOffset = 16.0f;


// TitleTask_t::state.
constexpr u32 kTitleStateMenu = 2; // navigable row list
constexpr u32 kTitleStateChildRunning = 4;

// Title screen navigable menu task. Local view: only the fields the mod
// hooks touch.
struct TitleTask_t {
  /* 0x000 */ u8 _pad000[0x78];
  /* 0x078 */ be_u32
      next_seq_id; // dispatch hook writes the "debug menu" sequence id here
  /* 0x07C */ u8 _pad07C[0x90 - 0x7C];
  /* 0x090 */ be_u32 state;  // kTitleState*, plus 6/7 for the back states
  /* 0x094 */ be_u32 cursor; // cursor row index
  /* 0x098 */ be_u32 has_save_data;
  /* 0x09C */ u8 _pad09C[0xA0 - 0x9C];
  /* 0x0A0 */ be_u32 child_task;
  /* 0x0A4 */ char child_name[6]; // task-name string, "Title\0" restored here
  /* 0x0AA */ u8 _pad0AA[0x104 - 0xAA];
  /* 0x104 */ be_u32 is_xbox_live;
  /* 0x108 */ u8 _pad108[0x114 - 0x108];
  /* 0x114 */ be_u32 language_cursor;
};
static_assert(offsetof(TitleTask_t, next_seq_id) == 0x078);
static_assert(offsetof(TitleTask_t, state) == 0x090);
static_assert(offsetof(TitleTask_t, cursor) == 0x094);
static_assert(offsetof(TitleTask_t, has_save_data) == 0x098);
static_assert(offsetof(TitleTask_t, child_task) == 0x0A0);
static_assert(offsetof(TitleTask_t, child_name) == 0x0A4);
static_assert(offsetof(TitleTask_t, is_xbox_live) == 0x104);
static_assert(offsetof(TitleTask_t, language_cursor) == 0x114);

TitleTask_t *Task(u32 titleTask) { return bd::mem::at<TitleTask_t>(titleTask); }

// Overlay at kVisualRenderAddr.
struct VisualRender_t {
  /* 0x0000 */ u8 _pad0000[0x1A38];
  /* 0x1A38 */ be_f32 screen_w; // screen width in pixels
};
static_assert(offsetof(VisualRender_t, screen_w) == 0x1A38);

bd::engine::ConfigMenu s_config_menu;
bool s_create_config = false;
bool s_config_closing = false;

// Screen-fade sequencing around the config child. Entering, the title fades
// to black and the config screen cuts in the instant it is up. Leaving, the
// veil snaps to black on the press and then lifts off the title in a fade.
// Either way it holds over the load, so neither the bare child-state backdrop
// nor a popping menu is ever visible.
enum class ConfigFade { None, TitleOut, Hold, Lift };
ConfigFade s_config_fade = ConfigFade::None;
constexpr f32 kFadeOutSeconds = 0.9f;
constexpr f32 kFadeInSeconds = 0.9f;
// A zero sweep lands the veil on the frame it is asked for.
constexpr f32 kInstant = 0.05f;

// Registered id of the engine "DebugMenu" sequence, resolved once at the title.
// It is only registered when devmode was on at boot.
constexpr u32 kSequenceNotRegistered = 0;
u32 s_debug_seq_id = kSequenceNotRegistered;
bool s_debug_resolved = false;

bool s_voice_picker_active = false;

// Drawn every frame, so the lookup and its UTF-16 conversion are cached.
const std::u16string &RowLabel(const char *key) {
  static std::map<std::string, std::u16string> cache;
  static u32 cachedLocale = ~0u;
  if (cachedLocale != bd::i18n::CurrentLocale()) {
    cachedLocale = bd::i18n::CurrentLocale();
    cache.clear();
  }
  auto it = cache.find(key);
  if (it == cache.end())
    it = cache.emplace(key, bd::Utf8ToU16(bd::i18n::Text(key))).first;
  return it->second;
}

bool DebugMenuAvailable() { return s_debug_seq_id != kSequenceNotRegistered; }

bool HasSaveData(u32 titleTask) { return Task(titleTask)->has_save_data != 0; }

// With saves the engine rows are Load=0, New=1, and NG+=2 when isXboxLive.
// Without saves the engine still draws New Game at row 1 (new_logo_xx loads
// unconditionally in TitleTask__Construct), and row 0 stays unused so the
// cursor never reaches the hidden Load row.
u32 ConfigIndex(u32 titleTask) {
  u32 idx = 2;
  if (HasSaveData(titleTask) && Task(titleTask)->is_xbox_live)
    idx++;
  return idx;
}

// "debug menu" sits one row below "config".
u32 DebugMenuIndex(u32 titleTask) { return ConfigIndex(titleTask) + 1; }

// "exit" is always the last row: below "debug menu" when present, else
// "config".
u32 ExitIndex(u32 titleTask) {
  return ConfigIndex(titleTask) + (DebugMenuAvailable() ? 2 : 1);
}

// Engine dpad handling in the menu state is gated on hasSaveData, so the
// no-save menu drives the cursor here. Valid rows: 1 (New Game) .. ExitIndex.
void NavigateNoSaveMenu(u32 titleTask) {
  auto *task = Task(titleTask);
  if (task->state != kTitleStateMenu)
    return;
  if (HasSaveData(titleTask))
    return;

  u32 cursor = task->cursor;
  u32 last = ExitIndex(titleTask);
  u32 next = cursor;
  if (CheckButton(Button::Down))
    next = cursor >= last ? 1 : cursor + 1;
  else if (CheckButton(Button::Up))
    next = cursor <= 1 ? last : cursor - 1;

  if (next != cursor) {
    task->cursor = next;
    sfx::Play(sfx::kCursor);
  }
}

// The title rows are not an AnimeMenu, so the hover engine never sees them.
// Their geometry is fixed and known: row i is a kEntrySpacing band centered on
// kCursorBaseY + i * kEntrySpacing, and every label is centered horizontally.
// The band tiles exactly, so a point maps to at most one row.
constexpr float kRowBandHalfWidth = 320.0f;

void HoverTitleRows(u32 titleTask) {
  auto *task = Task(titleTask);
  if (task->state != kTitleStateMenu)
    return;

  // Above the pointer gates, the way the battle's target step does it: this
  // says a list is taking menu input, not that the mouse is driving it. Nothing
  // else publishes it here, since these rows are not an AnimeMenu, which left
  // escape reading as the field's camp button and the arrow keys as the right
  // stick on the one screen that is nothing but a menu.
  bd::engine::MenuMouse::Get().MarkInputOwned();

  if (!bd::engine::MenuMouse::Get().PointerActive())
    return;

  f32 x = 0.0f;
  f32 y = 0.0f;
  if (!bd::engine::CursorInMenuSpace(x, y))
    return;

  const float centerX = bd::gpu::kDesignCanvasWidth * 0.5f;
  if (x < centerX - kRowBandHalfWidth || x > centerX + kRowBandHalfWidth)
    return;

  const float top = kCursorBaseY - kEntrySpacing * 0.5f;
  if (y < top)
    return;
  const int row = static_cast<int>((y - top) / kEntrySpacing);

  // Row 0 is Load, which the engine hides and skips without save data.
  const int first = HasSaveData(titleTask) ? 0 : 1;
  const int last = static_cast<int>(ExitIndex(titleTask));
  if (row < first || row > last)
    return;
  if (static_cast<u32>(row) == static_cast<u32>(task->cursor))
    return;

  task->cursor = static_cast<u32>(row);
  if (bd::engine::Settings::Get().MouseCursorSFX())
    sfx::Play(sfx::kCursor);
}

// Text top-left sits kTextCursorOffset above the cursor strip center.
float TitleRowTextY(u32 index) {
  return kCursorBaseY + index * kEntrySpacing - kTextCursorOffset;
}

// Draw a centered custom menu label at the given cursor row.
void DrawTitleLabel(PPCContext &ctx, u8 *base, u32 titleTask, u32 index,
                    std::u16string_view text) {
  u16 str[24];
  const int len =
      static_cast<int>(std::min<size_t>(text.size(), std::size(str) - 1));
  for (int i = 0; i < len; ++i)
    str[i] = static_cast<u16>(text[i]);
  str[len] = 0;

  float yPos = TitleRowTextY(index);

  rex::ppc::stack_guard guard(ctx);

  // bdColor4fToARGB reads big-endian floats in {r, g, b, a} order.
  u32 be_color[4];
  {
    float vals[] = {0.7f, 0.7f, 0.7f, 1.0f};
    for (int i = 0; i < 4; ++i) {
      u32 bits;
      std::memcpy(&bits, &vals[i], 4);
      be_color[i] = __builtin_bswap32(bits);
    }
  }
  u32 colorAddr = rex::ppc::stack_push(ctx, base, be_color, sizeof(be_color));
  u32 color = Color4fToARGB(colorAddr);

  u16 be_str[std::size(str)];
  for (int i = 0; i <= len; ++i)
    be_str[i] = __builtin_bswap16(str[i]);
  u32 strAddr = rex::ppc::stack_push(ctx, base, be_str, (len + 1) * 2);

  float textW = TextCalcWidth(24.0, color, strAddr, 1, -1);

  auto *visual =
      bd::mem::at<VisualRender_t>(bd::mem::load<u32>(kVisualRenderAddr));
  if (!visual)
    return;
  float xPos = (static_cast<float>(visual->screen_w) * 0.5f) - (textW * 0.5f);

  rex::CallFrame cf(ctx);
  cf.ctx.f1.f64 = (double)xPos;
  cf.ctx.f2.f64 = (double)yPos;
  cf.ctx.f3.f64 = 0.0;
  cf.ctx.f4.f64 = 24.0;
  cf.ctx.f5.f64 = 24.0;
  cf.ctx.r9.u32 = color;
  cf.ctx.r10.u32 = 1;
  // Callee reads ~18 u32s of stack params: zero them, then rewrite the string
  // (the zeroed param region can overlap the pushed string).
  for (int i = 0; i < 20; ++i)
    store_and_swap<u32>(base + cf.ctx.r1.u32 + 0x08 + i * 4, 0);
  for (int i = 0; i <= len; ++i)
    store_and_swap<u16>(base + strAddr + i * 2, str[i]);
  cf.ctx.r8.u32 = strAddr;
  __imp__Visual__method_7E60(cf, base);
}

} // namespace

// Extend the cursor wrap past the custom rows: +1 config, +1 "debug menu" when
// present, +1 "exit". Fires only when hasSaveData.
void bdTitleNavBoundsHook(PPCRegister &r30, PPCRegister &r31) {
  r30.u64 += DebugMenuAvailable() ? 3 : 2;
}

// Force the navigable menu state instead of the direct
// disc settings/language/DeviceSelect jump. Cursor is already 1 (New Game row),
// and hasSaveData stays 0 so the Load and New Game+ rows remain hidden and save
// creation reverts the title on its own.
bool bdTitleNoSaveMenuHook(PPCRegister &r11) {
  r11.u64 = kTitleStateMenu;
  return true;
}

// B in the back states computes next state (hasSaveData != 0) + 1, which is 1
// (press start) with no saves. Force the menu state so B returns to it.
namespace {
void ForceMenuOnNoSaveBack(PPCRegister &r11, PPCRegister &r31) {
  if (!Task(r31.u32)->has_save_data)
    r11.u64 = kTitleStateMenu;
}
} // namespace

void bdTitleNoSaveBackHook7(PPCRegister &r11, PPCRegister &r31) {
  ForceMenuOnNoSaveBack(r11, r31);
}

void bdTitleNoSaveBackHook6(PPCRegister &r11, PPCRegister &r31) {
  ForceMenuOnNoSaveBack(r11, r31);
}

// The three task hooks below stay raw: DrawTitleLabel and ConfigMenu::Update
// run guest calls on this hook's own stack, which only ctx carries.

// Draw the custom rows after the original draw, only in the menu state.
REX_HOOK_RAW(TitleTask_Draw) {
  u32 titleTask = ctx.r3.u32;
  __imp__TitleTask_Draw(ctx, base);

  if (Task(titleTask)->state != kTitleStateMenu)
    return;

  // First place our text is drawn, and bd_boot.ini is parsed by now.
  bd::i18n::SyncLocale();

  DrawTitleLabel(ctx, base, titleTask, ConfigIndex(titleTask),
                 RowLabel("title.config"));
  if (DebugMenuAvailable())
    DrawTitleLabel(ctx, base, titleTask, DebugMenuIndex(titleTask),
                   RowLabel("title.debug_menu"));
  DrawTitleLabel(ctx, base, titleTask, ExitIndex(titleTask),
                 RowLabel("title.exit"));
}

// A-button dispatch. "config" flags deferred creation and sets state 4
// (CHILD_RUNNING). "debug menu" writes the "DebugMenu" sequence id to
// next_seq_id so the engine fades out and transitions, the LoadGame idiom.
// "exit" hard-terminates the process.
bool bdTitleModsDispatchHook(PPCRegister &r31, PPCRegister &r11) {
  u32 titleTask = r31.u32;
  auto *task = Task(titleTask);
  u32 cursor = task->cursor;

  // A press mid-transition belongs to the fade, not to a row.
  if (s_config_fade != ConfigFade::None)
    return true;

  if (DebugMenuAvailable() && cursor == DebugMenuIndex(titleTask)) {
    task->next_seq_id = s_debug_seq_id;
    return true;
  }

  if (cursor == ConfigIndex(titleTask)) {
    // The menu state holds so the title keeps drawing under the veil. The
    // update hook flips to the child state once the veil lands.
    s_config_fade = ConfigFade::TitleOut;
    bd::ui::ScreenFade::Get().FadeTo(1.0f, kFadeOutSeconds);
    return true;
  }

  if (cursor == ExitIndex(titleTask)) {
    bd::RequestShutdown(bd::ShutdownReason::GuestExit); // never returns
  }

  return false;
}

REX_HOOK_RAW(TitleTask_OnChildComplete) {
  u32 titleTask = ctx.r3.u32;
  u32 childTask = ctx.r4.u32;

  if (s_config_menu.IsActive() && childTask == s_config_menu.TaskAddr()) {
    auto *task = Task(titleTask);
    memcpy(task->child_name, "Title\0", 6);
    task->child_task = 0;

    s_config_menu.Destroy();
    bd::engine::UnregisterVFS();

    // Skip the original: it would copy garbage from next_seq_id.
    return;
  }

  __imp__TitleTask_OnChildComplete(ctx, base);
}

REX_HOOK_RAW(TitleTask_Update) {
  u32 titleTask = ctx.r3.u32;
  auto *task = Task(titleTask);
  static float s_exit_hold_timer = 0.0f;

  // The stock New Game voice picker (state 7) polls its own D-pad helpers,
  // bypassing re:Blue's menu-arrow synthesis. Seed it from the persisted
  // profile voice type, then add only synthesized keyboard Up/Down here. The
  // guest still handles controller input and confirm/cancel normally.
  if (task->state == 7) {
    const int count = std::max(0, bd::engine::Language().VoiceCount());
    if (!s_voice_picker_active) {
      s_voice_picker_active = true;
      if (count > 0) {
        const int voiceType =
            std::clamp<int>(REXCVAR_GET(bd_opt_voice_type), 1, count);
        task->language_cursor = static_cast<u32>(voiceType - 1);
        BD_INFO("[voice-picker] seeded cursor from profile voice_type={} row={}",
                voiceType, voiceType - 1);
      }
    }

    if (count > 0) {
      const bool oldOwns = bd::engine::MenuOwnsInput();
      bd::engine::SetMenuOwnsInput(true);
      const bool down = bd::engine::SynthesizedButton(Button::Down);
      const bool up = bd::engine::SynthesizedButton(Button::Up);
      bd::engine::SetMenuOwnsInput(oldOwns);

      int cursor = static_cast<int>(u32(task->language_cursor));
      if (down) {
        cursor = (cursor + 1) % count;
        task->language_cursor = static_cast<u32>(cursor);
        BD_INFO("[voice-picker] keyboard down -> row {}", cursor);
      } else if (up) {
        cursor = (cursor + count - 1) % count;
        task->language_cursor = static_cast<u32>(cursor);
        BD_INFO("[voice-picker] keyboard up -> row {}", cursor);
      }
    }
  } else {
    s_voice_picker_active = false;
  }

  // Resolve the built-in "DebugMenu" sequence id once. Only registered when
  // debugMenuBoot was set at boot (devmode), so 0 keeps the row hidden.
  if (!s_debug_resolved) {
    u32 seqCtrl = bd::mem::load<u32>(bd::engine::addr::kSequenceControl);
    if (seqCtrl) {
      s_debug_resolved = true;
      s_debug_seq_id = FindSequenceByName(seqCtrl, kDebugMenuNameAddr);
    }
  }

  auto &fade = bd::ui::ScreenFade::Get();

  // The outgoing veil landed: only now does the title stop drawing its menu,
  // so the child state's bare backdrop is never visible.
  if (s_config_fade == ConfigFade::TitleOut && fade.IsOpaque()) {
    task->state = kTitleStateChildRunning;
    s_create_config = true;
    s_config_fade = ConfigFade::Hold;
  }
  if (s_config_fade == ConfigFade::Lift && fade.IsClear())
    s_config_fade = ConfigFade::None;

  // Handle pending close first so a reinstall can register fresh VFS state.
  if (s_config_closing) {
      s_config_closing = false;

      bool wantsRestart = s_config_menu.WantsRestart();
      s_config_menu.Destroy();

      task->child_task = 0;
      task->state = kTitleStateMenu;
      memcpy(task->child_name, "Title\0", 6);

      bd::engine::UnregisterVFS();

      if (wantsRestart) {
          // The rebuilt menu comes back up under the veil it went down under.
          s_create_config = true;
          s_config_fade = ConfigFade::Hold;
      }
      else {
          // INSTEAD of fading immediately, just prime the timer and stay in Hold.
          s_exit_hold_timer = 0.0f;
      }
  }

  if (s_create_config) {
    s_create_config = false;

    bd::engine::RegisterVFS();
    s_config_menu.Create(titleTask, bd::engine::ConfigMenu::Surface::Title,
                         &__imp__TitleTask_Update);

    if (!s_config_menu.IsActive()) {
      BD_ERROR("[config] Create failed, restoring the menu state");
      task->state = kTitleStateMenu;
      bd::engine::UnregisterVFS();
      fade.FadeTo(0.0f, kFadeInSeconds);
      s_config_fade = ConfigFade::Lift;
    } else {
      task->state = kTitleStateChildRunning;
    }
  }

  if (s_config_menu.IsActive()) {
    s_config_menu.Update(ctx, base);

    // The load is done: the finished screen cuts in from under the veil.
    if (s_config_fade == ConfigFade::Hold && s_config_menu.IsOnScreen()) {
      fade.FadeTo(0.0f, kInstant);
      s_config_fade = ConfigFade::None;
    }

    // The exit cuts to black on the press. The teardown above runs on the
    // next pass and the title fades up from under the veil it landed on.
    if (s_config_menu.IsClosing() && s_config_fade != ConfigFade::Hold) {
      fade.FadeTo(1.0f, kInstant);
      s_config_closing = true;
      s_config_fade = ConfigFade::Hold;
    }
  }
  else {
      // Held still under an opaque or landing veil: no input reaches the rows
      // it covers, and the swap happens between frames the player can see.
      if (s_config_fade == ConfigFade::TitleOut ||
          s_config_fade == ConfigFade::Hold) {

          // We are holding black, and the menu is gone (exit delay)
          if (s_config_fade == ConfigFade::Hold) {
              s_exit_hold_timer += ImGui::GetIO().DeltaTime;
              const float kExitHoldSeconds = 0.07f;

              if (s_exit_hold_timer >= kExitHoldSeconds) {
                  fade.FadeTo(0.0f, kFadeInSeconds); // Now start lifting the veil
                  s_config_fade = ConfigFade::Lift;
              }
          }
          return;
      }
      // The last frame before the original asks the content task to load
      // downloadable content, which is the deadline for answering about it.
      if (task->state == kTitleStateChildRunning && task->child_task == 0 &&
          bd::engine::UpdatePrompt::Get().Hold(titleTask))
        return;

      NavigateNoSaveMenu(titleTask);
      HoverTitleRows(titleTask);
      __imp__TitleTask_Update(ctx, base);
    }
  }
from pathlib import Path
p=Path(r'F:\BlueDragon_dev\reblue_engbase\src\engine\menus\title_menu.cpp')
s=p.read_text(encoding='utf-8')
# includes
if '#include "engine/language.h"' not in s:
    s=s.replace('#include "engine/settings.h"\n', '#include "engine/settings.h"\n#include "engine/language.h"\n')
if '#include <rex/cvar.h>' not in s:
    s=s.replace('#include <rex/hook.h>\n', '#include <rex/cvar.h>\n#include <rex/hook.h>\n')
# cvar declaration
marker='using rex::memory::store_and_swap;\n\n'
if 'REXCVAR_DECLARE(i32, bd_opt_voice_type);' not in s:
    s=s.replace(marker, marker+'REXCVAR_DECLARE(i32, bd_opt_voice_type);\n\n',1)
# remove temp automation vars block
start=s.find('// Temporary bring-up automation for the US-XEX/KR-data hybrid.')
if start!=-1:
    end=s.find('// Drawn every frame', start)
    s=s[:start]+s[end:]
# remove temp Start hook
start=s.find('REX_HOOK_RAW(TitleTask_PollInputStart) {')
if start!=-1:
    end=s.find('REX_HOOK_RAW(TitleTask_Update) {', start)
    s=s[:start]+s[end:]
# remove automation inside TitleTask_Update, from Bring-up comment to Resolve comment
start=s.find('  // Bring-up automation: once Press Start has completed')
if start!=-1:
    end=s.find('  // Resolve the built-in "DebugMenu" sequence id once.', start)
    s=s[:start]+s[end:]
# add voice picker state bool near debug vars
needle='u32 s_debug_seq_id = kSequenceNotRegistered;\nbool s_debug_resolved = false;\n\n'
if 'bool s_voice_picker_active' not in s:
    s=s.replace(needle, needle+'bool s_voice_picker_active = false;\n\n',1)
# inject voice picker fix right after static exit timer
needle='  static float s_exit_hold_timer = 0.0f;\n\n'
fix='''  static float s_exit_hold_timer = 0.0f;\n\n  // The stock New Game voice picker (state 7) polls its own D-pad helpers,\n  // bypassing re:Blue's menu-arrow synthesis. Seed it from the persisted\n  // profile voice type, then add only synthesized keyboard Up/Down here. The\n  // guest still handles controller input and confirm/cancel normally.\n  if (task->state == 7) {\n    const int count = std::max(0, bd::engine::Language().VoiceCount());\n    if (!s_voice_picker_active) {\n      s_voice_picker_active = true;\n      if (count > 0) {\n        const int voiceType =\n            std::clamp<int>(REXCVAR_GET(bd_opt_voice_type), 1, count);\n        task->language_cursor = static_cast<u32>(voiceType - 1);\n        BD_INFO("[voice-picker] seeded cursor from profile voice_type={} row={}",\n                voiceType, voiceType - 1);\n      }\n    }\n\n    if (count > 0) {\n      const bool oldOwns = bd::engine::MenuOwnsInput();\n      bd::engine::SetMenuOwnsInput(true);\n      const bool down = bd::engine::SynthesizedButton(Button::Down);\n      const bool up = bd::engine::SynthesizedButton(Button::Up);\n      bd::engine::SetMenuOwnsInput(oldOwns);\n\n      int cursor = static_cast<int>(u32(task->language_cursor));\n      if (down) {\n        cursor = (cursor + 1) % count;\n        task->language_cursor = static_cast<u32>(cursor);\n        BD_INFO("[voice-picker] keyboard down -> row {}", cursor);\n      } else if (up) {\n        cursor = (cursor + count - 1) % count;\n        task->language_cursor = static_cast<u32>(cursor);\n        BD_INFO("[voice-picker] keyboard up -> row {}", cursor);\n      }\n    }\n  } else {\n    s_voice_picker_active = false;\n  }\n\n'''
if '[voice-picker] seeded cursor' not in s:
    if needle not in s:
        raise SystemExit('update insertion marker not found')
    s=s.replace(needle,fix,1)
p.write_text(s,encoding='utf-8',newline='\n')
print('patched')

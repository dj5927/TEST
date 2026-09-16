from pathlib import Path
p=Path(r'F:\BlueDragon_dev\reblue_engbase\src\engine\menus\title_menu.cpp')
s=p.read_text(encoding='utf-8')
marker='''  auto *task = Task(titleTask);\n  static float s_exit_hold_timer = 0.0f;\n\n'''
insert='''  auto *task = Task(titleTask);\n  static float s_exit_hold_timer = 0.0f;\n\n  // Bring-up automation: once Press Start has completed and the normal title\n  // menu is live, select New Game and queue A for the next guest frame.\n  if (s_kr_test_started && !s_kr_test_new_game_queued &&\n      task->state == kTitleStateMenu) {\n    if (++s_kr_test_menu_frames >= 30) {\n      task->cursor = 1;\n      bd::engine::PressButton(static_cast<int>(Button::A));\n      s_kr_test_new_game_queued = true;\n      BD_INFO("[kr-hybrid-test] queued New Game A");\n    }\n  } else if (!s_kr_test_new_game_queued) {\n    s_kr_test_menu_frames = 0;\n  }\n\n  // The hybrid boot list is US/FR/JP/KR. Its selector is zero-based, so 3 is\n  // Korean. Delay the A edge to keep the New Game confirm from bleeding into\n  // the voice picker.\n  if (s_kr_test_new_game_queued && !s_kr_test_voice_queued &&\n      task->state == 7) {\n    if (++s_kr_test_voice_frames >= 30) {\n      task->language_cursor = 3;\n      bd::engine::PressButton(static_cast<int>(Button::A));\n      s_kr_test_voice_queued = true;\n      BD_INFO("[kr-hybrid-test] selected Korean voice and queued A");\n    }\n  } else if (!s_kr_test_voice_queued) {\n    s_kr_test_voice_frames = 0;\n  }\n\n'''
if '[kr-hybrid-test] queued New Game A' not in s:
    if marker not in s:
        raise SystemExit('update marker not found')
    s=s.replace(marker, insert, 1)
    p.write_text(s, encoding='utf-8', newline='\n')
    print('inserted title automation')
else:
    print('automation already present')

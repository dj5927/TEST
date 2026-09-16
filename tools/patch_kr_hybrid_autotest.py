from pathlib import Path
p=Path(r'F:\BlueDragon_dev\reblue_engbase\src\engine\menus\title_menu.cpp')
s=p.read_text(encoding='utf-8')
marker='''  __imp__TitleTask_OnChildComplete(ctx, base);\n}\n\nREX_HOOK_RAW(TitleTask_Update) {\n'''
insert='''  __imp__TitleTask_OnChildComplete(ctx, base);\n}\n\nREX_HOOK_RAW(TitleTask_PollInputStart) {\n  // Once the host update prompt is gone, synthesize exactly one Press Start.\n  // The original caller performs the normal sound/state/child-task side\n  // effects, so this is an input-only diagnostic.\n  if (!s_kr_test_started && !bd::engine::UpdatePrompt::Get().Active() &&\n      ++s_kr_test_start_polls >= 30) {\n    s_kr_test_started = true;\n    ctx.r3.u32 = 1;\n    BD_INFO("[kr-hybrid-test] synthesized Press Start");\n    return;\n  }\n  __imp__TitleTask_PollInputStart(ctx, base);\n}\n\nREX_HOOK_RAW(TitleTask_Update) {\n'''
if '[kr-hybrid-test] synthesized Press Start' not in s:
    if marker not in s:
        raise SystemExit('marker not found')
    s=s.replace(marker, insert, 1)
    p.write_text(s, encoding='utf-8', newline='\n')
    print('inserted Start hook')
else:
    print('Start hook already present')

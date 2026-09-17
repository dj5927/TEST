# Building the Android port

## Native side

The Android native host is still the normal re:Blue codebase, built as an
Android shared library instead of a desktop executable. The Android port also
contains mobile Vulkan compatibility work, touch controls, lifecycle handling,
external data/config paths, vibration support and Android diagnostics.

The generated recompiled guest code is still derived from the NTSC-U
`default.xex`. That XEX is required when regenerating the static recompilation,
but it is copyrighted game data and must never be committed or distributed.

The validated native build used:

```text
Android NDK: 28.2.13676358
ABI:         arm64-v8a
Platform:    android-28
Build type:  Release
Renderer:    Vulkan
```

After building, copy `libmain.so` and the matching `librexruntime.so` into the
Gradle wrapper's `android/app/src/main/jniLibs/arm64-v8a/` directory.

## Why Korean retail default.xex is not used

The Korean/Asian executable has a different guest code layout and entry point
from the NTSC-U executable used by re:Blue's static recompilation. Loading the
Korean XEX directly can route execution to addresses for which no recompiled
host function exists.

For that reason Korean support is data import, not executable replacement:

1. keep a normal NTSC-U re:Blue base;
2. import Korean language/text and XACT voice resources from the user's own
   Korean retail discs;
3. convert only the affected two-track regional Sofdec movies to the
   three-track topology expected by the NTSC-U runtime;
4. select Korean UI and the KR voice slot in `reblue.toml`.

See `KOREAN_RETAIL.md` and `../tools/prepare_korean_data.py`.

## Source-only repository rule

The following are intentionally excluded:

- retail disc images and extracted game data;
- `default.xex` from every region;
- `.ipk`, `.sfd`, XACT audio banks and fonts/textures;
- native `.so` build products;
- APK/AAB build products;
- signing keystores and passwords.


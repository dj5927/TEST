# re:Blue Android (preview)

Unofficial Android port work for [re:Blue](https://github.com/zolaware/reblue),
the native static-recompilation project for **Blue Dragon**.

This repository is intended to contain **code and tools only**. It does not
contain Blue Dragon disc images, `default.xex`, voice banks, movies, Korean
text assets, or any other retail game data. You must use your own legally
obtained Blue Dragon discs/images.

## Current goals

- Run the normal NTSC-U / English re:Blue data set on Android ARM64 + Vulkan.
- Keep the Android runtime language-neutral instead of forcing Korean data.
- Support touch controls, physical controllers, Android vibration, external
  game-data folders, and mobile Vulkan compatibility paths.
- Add an optional **Korean retail data import** path for owners of the Korean
  Blue Dragon discs.

## Important: Korean discs and the recompiled executable

re:Blue is statically recompiled from the NTSC-U executable. The Korean/Asian
retail `default.xex` has a different code layout and entry point, so replacing
the NTSC-U runtime XEX with the Korean XEX is **not** considered safe or
supported by this preview.

The known-good Korean configuration therefore uses:

1. a normal NTSC-U re:Blue base install, and
2. data imported from the user's own Korean retail Disc 1/2/3.

The import keeps the US-code runtime while adding Korean text/voice resources.
Nothing from the retail discs is redistributed here.

## Korean voice architecture

Korean retail `bd_boot.ini` exposes:

```ini
[Language] US TW KR
[Voice] JP KR
```

For normal dialogue, re:Blue can use the retail Korean XACT data directly:

- `snd_memory_kr`
- `snd_stream_kr`
- `pack/packmem_kr.ipk`

The Korean voice is the second entry in `[Voice]`, so the known-good setting is:

```toml
user_language = 7
bd_language = "kr"
bd_opt_voice_type = 2
bd_opt_subtitles = 1
```

### Cutscene movies

Korean retail Sofdec movies normally contain two audio streams:

- C1 = Japanese
- C2 = Korean

The NTSC-U re:Blue runtime expects the three-stream topology used by the US
disc. The included `tools/prepare_korean_data.py` converts only the affected
movies into a compatible three-stream layout while preserving Korean-retail
video/audio PES bodies:

- normal regional movie: C1=JP, C2=KR, C3=JP clone
- `BDopdemo.sfd`: C1=JP, C2=KR, C3=KR clone

Dropping the synthesized C3 stream from the converted file reproduces the
original Korean C1/C2/E0 packet-body sequence exactly.

The conversion logic has been dry-run against all three Korean retail disc
data sets. It identifies the same **69 unique regional movies** used by the
known-good Korean voice build; single-audio and silent SFDs are left unchanged.

## Korean retail import

The preview desktop installer now has a separate **Optional Korean Retail
Import** section. Keep the normal NTSC-U discs in **Install Sources**, then add
Korean retail Disc 1, Disc 2 and Disc 3 in the Korean import section.

When all three Korean discs are present, the installer:

1. installs the normal NTSC-U base without replacing its `default.xex`;
2. imports `pack/packmem_kr.ipk`, `snd_memory_kr` and `snd_stream_kr`;
3. extracts only Korean records from `!necessity`, `sca` and `sequence` into
   `mods/bd_asia_text`;
4. converts the 69 two-track Korean regional SFDs to the compatible
   three-stream topology;
5. exposes KR in `bd_boot.ini` and enables the Korean text mod; and
6. writes Korean UI/subtitle defaults with KR voice slot 2 into the profile.

The standalone preparation tool remains available for validation or existing
extracted data. It works on a `game` folder from the user's own Korean retail
discs:

```powershell
python tools\prepare_korean_data.py D:\BlueDragonKR\game
```

That command is a **dry run** and changes nothing. After reviewing the list:

```powershell
python tools\prepare_korean_data.py D:\BlueDragonKR\game --apply
```

See [docs/KOREAN_RETAIL.md](docs/KOREAN_RETAIL.md) for details.

## Status

This is a development preview, not an official re:Blue release. The generic
Android wrapper now builds as `com.reblue.android` and defaults to normal
NTSC-U / English data. A prepared Korean-data install is detected
automatically instead of being required by the APK.

The current preview has passed Java/Gradle compilation, native ARM64 linking,
APK assembly and APK-signature/package validation. Physical-device runtime QA
is still required before treating it as a general public release. Android
compatibility may vary by Vulkan driver/GPU.

See [android/README.md](android/README.md) and
[docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md) for build details.

## Upstream and license

The re:Blue source is BSD-3-Clause licensed. Keep the upstream copyright and
license notices when redistributing modified source or binaries.

- Upstream: https://github.com/zolaware/reblue
- ReXGlue SDK: https://github.com/rexglue/rexglue-sdk

Blue Dragon and all retail game assets remain property of their respective
rights holders and are not included in this project.

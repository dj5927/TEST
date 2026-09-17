# Korean retail data with re:Blue Android

## What is supported

The tested configuration is a **US-code re:Blue base** plus regional data read
from the user's own Korean Blue Dragon Disc 1/2/3.

This distinction matters because the Korean/Asian executable does not share
the NTSC-U code layout used to generate re:Blue's static recompilation.

## What re:Blue's installer already understands

Current re:Blue installer code already parses `bd_boot.ini` and recognizes
`kr` as a language code. For every language advertised by the discs it can
collect:

- `pack/packmem_<lang>.ipk`
- `snd_memory_<lang>/...`
- `snd_stream_<lang>/...`

Therefore a Korean-data import does not need a custom voice extractor for the
normal XACT dialogue banks. The Korean retail files can remain byte-identical.

## Why movies need one compatibility step

The Korean retail movie topology is normally:

```text
C1 = Japanese
C2 = Korean
```

The US-code runtime was authored around a three-audio-stream Sofdec system
layout. The compatibility conversion adds a synthesized C3 without transcoding
the Korean source audio or video.

### Normal regional movies

```text
C1 = Korean-retail C1 (JP)
C2 = Korean-retail C2 (KR)
C3 = C1 clone with PES stream id changed to C3
```

### BDopdemo.sfd

```text
C1 = Korean-retail JP/SFA
C2 = Korean-retail KR/AIX
C3 = C2 clone with PES stream id changed to C3
```

Pack/SCR headers and the Sofdec stream table are regenerated to describe the
three-stream file. The source C1/C2/E0 PES bodies remain unchanged.

## Validation completed on the Korean retail data

The preview preparation tool was dry-run against all three extracted Korean
retail discs:

| Disc | 2-track SFDs selected for conversion | Other SFDs left unchanged |
| --- | ---: | ---: |
| Disc 1 | 33 | 37 |
| Disc 2 | 16 | 32 |
| Disc 3 | 22 | 38 |

`BDopdemo.sfd` appears on multiple discs, so the unique regional conversion
set is **69 files**: 68 normal regional movies plus the BDop special case.

For every converted file the tool verifies that removing synthesized C3 from
the result reproduces the original Korean C1/C2/E0 packet-body sequence.

## Installer workflow

The preview installer implements this workflow:

1. Install/extract a normal NTSC-U re:Blue base from the user's own discs.
2. Use the separate **Optional Korean Retail Import** section.
3. Select Korean Disc 1, Disc 2 and Disc 3 images.
4. re:Blue copies Korean language and XACT voice resources from those discs.
5. The installer converts the 69 regional SFDs to the compatible three-stream
   form while extracting them.
6. The profile is configured for Korean UI/subtitles and voice index 2.
7. The NTSC-U `default.xex` used by re:Blue is kept; the Korean `default.xex`
   is not substituted into the US static-recompilation runtime.

The C++ installer path performs the import directly from the selected Korean
disc images. The Python tool remains as an independent dry-run/conversion
validator for already-extracted Korean data.

## Files that must never be committed to this repository

- Blue Dragon `.iso` files
- `default.xex` from any region
- extracted `pack/*.ipk`
- `snd_memory_*` / `snd_stream_*`
- `.sfd` movies
- Korean text/font/texture assets
- save data
- Android signing keystores or passwords

Only source code, build instructions and transformation logic belong here.

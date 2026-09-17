# re:Blue Android wrapper

This directory contains the Android application wrapper used by the unofficial
re:Blue Android port.

The wrapper itself contains no Blue Dragon game data and no signing secrets.
Native libraries are build products and are intentionally not committed.

## Runtime data profiles

The launcher is region-neutral by default:

- normal NTSC-U data -> English UI, voice slot 1
- prepared NTSC-U + Korean retail data -> Korean UI, the actual KR voice slot
  read from `bd_boot.ini`

The launcher refuses the known Korean retail `default.xex` hashes. re:Blue is
statically recompiled from the NTSC-U executable, so Korean retail data must be
imported on top of an NTSC-U base instead of replacing the executable.

## Native libraries

Before assembling an APK, place these ARM64 libraries in:

`app/src/main/jniLibs/arm64-v8a/`

- `libmain.so` - re:Blue compiled for Android
- `librexruntime.so` - matching ReXGlue runtime

Do not commit either file.

## Build

Requirements used by the validated preview build:

- JDK 17
- Android SDK 35
- Android NDK 28.2.13676358
- arm64-v8a
- minSdk 28 / targetSdk 35

For a debug APK:

```powershell
./gradlew.bat :app:assembleDebug
```

Release signing is optional. To sign locally, create
`release-signing.properties` in this directory with:

```properties
storeFile=your-private-key.jks
storePassword=...
keyAlias=...
keyPassword=...
```

The file and keystore are ignored by Git.

## Validated preview package

The locally validated package metadata is:

- application id: `com.reblue.android`
- version: `0.1.0-preview` (`versionCode=1`)
- ABI: `arm64-v8a`
- renderer: Vulkan

The APK is not committed to the source tree. Binary releases should be
published through GitHub Releases.


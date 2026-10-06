# Android build

This is the Android shell for Zero Hour Reforged.

Current native target:
- ABI: arm64-v8a
- Renderer: OpenGL ES 3.0
- Native entry point: libmain.so / SDL_main

Before the first build, vendor the third-party sources expected by Reforged:

    cd ../Code
    ./Tools/vendor.sh

Then open this directory in Android Studio and build the app module. The Gradle project reuses
SDL3's Android Java sources from the vendored SDL3 tree; no EA game data is packaged.

The APK currently expects the player's own Zero Hour installation to be supplied separately.

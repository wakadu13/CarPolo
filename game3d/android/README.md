# CarPolo 3D — Android packaging

The game is native C (raylib 5.5, `libmain.so`) started by `fr.carpolo.cactus3d.GameActivity`, a
tiny `NativeActivity` subclass that adds immersive fullscreen, keep-screen-on and a `vibrate()`
helper (called from C through `AndroidVibrate()` in `platform_android.h`, Android builds only).
Everything in `game3d/assets/` is packaged as APK assets (raylib reads them with relative paths,
e.g. `LoadFileData("world.bin")` on Android / `assets/world.bin` on desktop).

| | |
|---|---|
| applicationId | `fr.carpolo.cactus3d` ("CarPolo 3D"), minSdk 26, targetSdk 36, landscape |
| ABIs | arm64-v8a + armeabi-v7a (CI), arm64-v8a (local offline build) |
| signing | the shared dev key `android/app/carpolo-dev.keystore` (same as the web app) |

## Build in CI (reference)

`.github/workflows/android3d.yml` runs on every push touching `game3d/**` (or manually):
NDK (runner's `$ANDROID_NDK_LATEST_HOME`) + `build-tools;36.0.0` + `platforms;android-36` →
`game3d/android/build-apk.sh` → artifact `CarPolo3D-apk` and prerelease **`apk3d-latest`**
(deleted and recreated each run) with `CarPolo3D.apk`.

Download the published APK (public API, no auth needed):

```sh
id=$(curl -s https://api.github.com/repos/wakadu13/CarPolo/releases/tags/apk3d-latest \
     | python3 -c "import sys,json; print(json.load(sys.stdin)['assets'][0]['id'])")
curl -sL -H "Accept: application/octet-stream" -o CarPolo3D.apk \
     "https://api.github.com/repos/wakadu13/CarPolo/releases/assets/$id"
```
Phone link: `https://github.com/wakadu13/CarPolo/releases/download/apk3d-latest/CarPolo3D.apk`

## Build locally

With an NDK and SDK:
```sh
game3d/android/build-apk.sh --ndk $ANDROID_NDK_HOME --build-tools $ANDROID_HOME/build-tools/36.0.0 \
    --android-jar $ANDROID_HOME/platforms/android-36/android.jar        # → dist/CarPolo3D.apk
```

Without NDK/SDK (Debian/Ubuntu, e.g. when dl.google.com is unreachable), arm64-v8a only:
```sh
RAYLIB_SRC=/opt/src/raylib game3d/android/build-apk.sh --local         # RAYLIB_SRC optional
```
`local-toolchain/setup.sh` assembles that toolchain once into `~/.cache/carpolo-build/ndk-local`:
Ubuntu clang/lld, the old NDK platform tree from AOSP (`aosp-mirror/platform_development`, tag
`android-6.0.0_r1`: bionic API 21 headers + symbol lists → generated stub `.so`), current Khronos
EGL/GLES headers, `libgcc.a` from `libgcc-13-dev-arm64-cross` for compiler builtins. It links with
`--no-undefined`, so every symbol the game needs is checked against the API 21 libraries (all present
on Android 8+). It is a fallback: the CI (official NDK) APK is the reference build.

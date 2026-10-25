#!/usr/bin/env bash
# Builds the signed CarPolo 3D APK (raylib game in lib/<abi>/libmain.so, NativeActivity).
# Same script in GitHub Actions and on any Linux machine.
#
# Usage: game3d/android/build-apk.sh [options]
#   --ndk DIR            Android NDK (default: $ANDROID_NDK_HOME, $ANDROID_NDK_LATEST_HOME, $ANDROID_NDK_ROOT)
#   --toolchain FILE     use this CMake toolchain file instead of the NDK one (e.g. the offline
#                        toolchain made by game3d/android/local-toolchain/setup.sh)
#   --build-tools DIR    SDK build-tools dir (aapt2, zipalign, apksigner, d8). Default: $ANDROID_BUILD_TOOLS,
#                        else tools found in PATH (Debian/Ubuntu packages: aapt, zipalign, apksigner, dalvik-exchange)
#   --android-jar FILE   platform android.jar (aapt2 -I and javac classpath). Default: $ANDROID_JAR,
#                        else $ANDROID_HOME/platforms/android-36/android.jar
#   --aapt-include FILE  override the aapt2 -I file only   (e.g. /usr/share/android-framework-res/framework-res.apk)
#   --javac-cp FILE      override the javac classpath only (e.g. Robolectric's android-all jar)
#   --abis "LIST"        ABIs (default: "arm64-v8a armeabi-v7a")
#   --out FILE           output APK (default: dist/CarPolo3D.apk at the repo root)
# Env: KEYSTORE (default android/app/carpolo-dev.keystore), KS_PASS/KEY_ALIAS (carpolo),
#      VERSION_CODE (default: git commit count), RAYLIB_SRC (local raylib 5.5 source → no download),
#      BUILD_TYPE (Release).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME="$(cd "$HERE/.." && pwd)"          # game3d/
ROOT="$(cd "$GAME/.." && pwd)"          # repository root

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_ROOT:-}}}"
TOOLCHAIN_FILE=""
BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-}"
ANDROID_JAR="${ANDROID_JAR:-}"
AAPT_INCLUDE=""
JAVAC_CP=""
ABIS="${ABIS:-arm64-v8a armeabi-v7a}"
OUT="$ROOT/dist/CarPolo3D.apk"

while [ $# -gt 0 ]; do
  case "$1" in
    --ndk) NDK="$2"; shift 2 ;;
    --toolchain) TOOLCHAIN_FILE="$2"; shift 2 ;;
    --build-tools) BUILD_TOOLS="$2"; shift 2 ;;
    --android-jar) ANDROID_JAR="$2"; shift 2 ;;
    --aapt-include) AAPT_INCLUDE="$2"; shift 2 ;;
    --javac-cp) JAVAC_CP="$2"; shift 2 ;;
    --abis) ABIS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

PACKAGE="fr.carpolo.cactus3d"
MIN_SDK=26
TARGET_SDK=36
KEYSTORE="${KEYSTORE:-$ROOT/android/app/carpolo-dev.keystore}"
KS_PASS="${KS_PASS:-carpolo}"
KEY_ALIAS="${KEY_ALIAS:-carpolo}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- tools
tool() {  # tool NAME -> path (build-tools dir first, then PATH)
  if [ -n "$BUILD_TOOLS" ] && [ -x "$BUILD_TOOLS/$1" ]; then echo "$BUILD_TOOLS/$1"; return; fi
  command -v "$1" 2>/dev/null || true
}
AAPT2="$(tool aapt2)"; ZIPALIGN="$(tool zipalign)"; APKSIGNER="$(tool apksigner)"; D8="$(tool d8)"
[ -n "$AAPT2" ] || die "aapt2 not found (give --build-tools or install the 'aapt' package)"
[ -n "$ZIPALIGN" ] || die "zipalign not found"
[ -n "$APKSIGNER" ] || die "apksigner not found"
DX_JAR=/usr/share/java/com.android.dx.jar
[ -n "$D8" ] || [ -f "$DX_JAR" ] || die "neither d8 nor dx (dalvik-exchange) found"
command -v javac >/dev/null || die "javac (JDK 11+) is required"
command -v cmake >/dev/null || die "cmake is required"
[ -f "$KEYSTORE" ] || die "keystore not found: $KEYSTORE"

if [ -z "$ANDROID_JAR" ] && [ -n "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}" ]; then
  ANDROID_JAR="${ANDROID_HOME:-$ANDROID_SDK_ROOT}/platforms/android-$TARGET_SDK/android.jar"
fi
AAPT_INCLUDE="${AAPT_INCLUDE:-$ANDROID_JAR}"
JAVAC_CP="${JAVAC_CP:-$ANDROID_JAR}"
[ -f "$AAPT_INCLUDE" ] || die "no android.jar/framework-res for aapt2 (--android-jar or --aapt-include): '$AAPT_INCLUDE'"
[ -f "$JAVAC_CP" ] || die "no android.jar for javac (--android-jar or --javac-cp): '$JAVAC_CP'"

if [ -z "$TOOLCHAIN_FILE" ]; then
  [ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] || die "Android NDK not found (--ndk DIR or ANDROID_NDK_HOME)"
  TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
fi
STRIP="$(command -v llvm-strip || true)"
if [ -n "$NDK" ] && [ -d "$NDK/toolchains/llvm/prebuilt" ]; then
  s="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1 || true)"
  [ -n "$s" ] && STRIP="$s"
fi

VERSION_CODE="${VERSION_CODE:-$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 1)}"
[ "$VERSION_CODE" -ge 1 ] 2>/dev/null || VERSION_CODE=1
VERSION_NAME="1.0.$VERSION_CODE"

WORK="$GAME/build-android"
STAGE="$WORK/stage"
rm -rf "$STAGE" "$WORK/java" "$WORK/apk"
mkdir -p "$STAGE" "$WORK/java/classes" "$WORK/java/dex" "$WORK/apk" "$(dirname "$OUT")"

# ---------------------------------------------------------------- 1. native code
CMAKE_EXTRA=()
[ -n "${RAYLIB_SRC:-}" ] && CMAKE_EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_RAYLIB=$RAYLIB_SRC")
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
for abi in $ABIS; do
  log "Building libmain.so for $abi"
  bdir="$WORK/cmake-$abi"
  cmake -S "$GAME" -B "$bdir" "${GEN[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$MIN_SDK" -DANDROID_STL=none \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${CMAKE_EXTRA[@]}" > "$bdir.configure.log" 2>&1 \
    || { cat "$bdir.configure.log"; die "CMake configure failed for $abi"; }
  cmake --build "$bdir" --target main -j "$(nproc 2>/dev/null || echo 4)" \
    || die "native build failed for $abi"
  so="$(find "$bdir" -name libmain.so -type f | head -1)"
  [ -n "$so" ] || die "libmain.so not produced for $abi"
  mkdir -p "$STAGE/lib/$abi"
  if [ -n "$STRIP" ]; then "$STRIP" --strip-unneeded -o "$STAGE/lib/$abi/libmain.so" "$so"
  else cp "$so" "$STAGE/lib/$abi/libmain.so"; fi
done

# ---------------------------------------------------------------- 2. Java (GameActivity) -> dex
log "Compiling GameActivity"
mapfile -t JAVA_SOURCES < <(find "$HERE/java" -name '*.java')
javac -nowarn -Xlint:-options --release 8 -encoding UTF-8 -cp "$JAVAC_CP" \
  -d "$WORK/java/classes" "${JAVA_SOURCES[@]}"
mapfile -t CLASSES < <(find "$WORK/java/classes" -name '*.class')
if [ -n "$D8" ]; then
  "$D8" --release --min-api "$MIN_SDK" --lib "$JAVAC_CP" --output "$WORK/java/dex" "${CLASSES[@]}"
else
  java -cp "$DX_JAR" com.android.dx.command.Main --dex --min-sdk-version="$MIN_SDK" \
    --output="$WORK/java/dex/classes.dex" "$WORK/java/classes"
fi
cp "$WORK/java/dex/classes.dex" "$STAGE/classes.dex"

# ---------------------------------------------------------------- 3. resources, manifest, assets
log "Linking manifest + resources + assets"
"$AAPT2" compile --dir "$HERE/res" -o "$WORK/apk/res.zip"
ASSET_ARGS=()
if [ -d "$GAME/assets" ] && [ -n "$(ls -A "$GAME/assets" 2>/dev/null)" ]; then ASSET_ARGS=(-A "$GAME/assets"); fi
"$AAPT2" link -o "$WORK/apk/unaligned.apk" \
  -I "$AAPT_INCLUDE" \
  --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version "$MIN_SDK" --target-sdk-version "$TARGET_SDK" \
  --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
  "${ASSET_ARGS[@]}" \
  "$WORK/apk/res.zip"
# keep big binary assets uncompressed? world data compresses well: leave aapt2's choice.
(cd "$STAGE" && zip -q -r -X "$WORK/apk/unaligned.apk" classes.dex lib)

# ---------------------------------------------------------------- 4. align + sign
log "Aligning + signing"
"$ZIPALIGN" -f -p 4 "$WORK/apk/unaligned.apk" "$WORK/apk/aligned.apk"
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-type PKCS12 --ks-pass "pass:$KS_PASS" \
  --ks-key-alias "$KEY_ALIAS" --key-pass "pass:$KS_PASS" --min-sdk-version "$MIN_SDK" \
  --out "$OUT" "$WORK/apk/aligned.apk"

# ---------------------------------------------------------------- 5. checks
"$ZIPALIGN" -c -p 4 "$OUT" >/dev/null || die "APK is not aligned"
"$APKSIGNER" verify --min-sdk-version "$MIN_SDK" "$OUT" || die "signature check failed"
for abi in $ABIS; do
  unzip -l "$OUT" | grep -q " lib/$abi/libmain.so\$" || die "lib/$abi/libmain.so missing from the APK"
done
"$AAPT2" dump badging "$OUT" | grep -E "^(package|sdkVersion|targetSdkVersion|launchable-activity|native-code)" || true
log "OK: $OUT ($(du -h "$OUT" | cut -f1), versionCode $VERSION_CODE, ABIs: $ABIS)"

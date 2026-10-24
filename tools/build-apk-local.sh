#!/usr/bin/env bash
# Builds a signed, installable dist/CarPolo.apk WITHOUT the Android SDK or Gradle.
#
# The reference build is Gradle (android/) on GitHub Actions. This script is a
# fallback for machines where the Android SDK can't be installed: it uses the
# Debian/Ubuntu packaged tools (aapt2, dx, zipalign, apksigner, framework-res.apk
# for resource linking) and Robolectric's "android-all" jar from Maven Central as
# the compile-time framework classes (android.jar equivalent).
#
# Usage:  tools/build-apk-local.sh            -> dist/CarPolo.apk
# Env:    WWW_DIR=...   game folder to package (default: www/)
#         CARPOLO_CACHE=...  download cache (default: ~/.cache/carpolo-build)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/android/app"
WWW="${WWW_DIR:-$ROOT/www}"
CACHE="${CARPOLO_CACHE:-$HOME/.cache/carpolo-build}"
DIST="$ROOT/dist"
WORK="$DIST/.build"
OUT="$DIST/CarPolo.apk"

PACKAGE="fr.carpolo.game"
MIN_SDK=26
TARGET_SDK=36
ANDROID_ALL_VERSION="16-robolectric-13921718" # API 36 framework classes + resources
MAVEN_MIRRORS=(
  "https://repo1.maven.org/maven2"
  "https://maven-central.storage-download.googleapis.com/maven2"
)

KEYSTORE="$APP/carpolo-dev.keystore"
KS_PASS="carpolo"
KEY_ALIAS="carpolo"

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ -f "$WWW/index.html" ] || die "no index.html in $WWW (the game must be in www/)"

# --- 1. Build tools (Debian/Ubuntu packages) ---------------------------------
DX_JAR=/usr/share/java/com.android.dx.jar
# Framework resources (API 33) to link against; public resource IDs are stable,
# so this is fine for an app compiled for API 36.
FRAMEWORK_RES=/usr/share/android-framework-res/framework-res.apk
if ! command -v aapt2 >/dev/null || ! command -v zipalign >/dev/null \
   || ! command -v apksigner >/dev/null || [ ! -f "$DX_JAR" ] || [ ! -f "$FRAMEWORK_RES" ]; then
  command -v apt-get >/dev/null || die "needs the Debian/Ubuntu packages: aapt zipalign apksigner dalvik-exchange android-framework-res"
  SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO="sudo"
  log "Installing aapt2 / zipalign / apksigner / dx / framework-res (apt)"
  $SUDO apt-get update -qq
  DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq --no-install-recommends \
    aapt zipalign apksigner dalvik-exchange android-framework-res >/dev/null
fi
command -v javac >/dev/null || die "javac (JDK 11+) is required"

# --- 2. Framework classes for javac (android.jar equivalent, API 36) ---------
FRAMEWORK="$CACHE/android-all-$ANDROID_ALL_VERSION.jar"
if [ ! -f "$FRAMEWORK" ]; then
  mkdir -p "$CACHE"
  path="org/robolectric/android-all/$ANDROID_ALL_VERSION/android-all-$ANDROID_ALL_VERSION.jar"
  for base in "${MAVEN_MIRRORS[@]}"; do
    log "Downloading framework jar (~200 MB, once) from $base"
    if curl -fsSL --retry 3 -o "$FRAMEWORK.part" "$base/$path" \
       && expected="$(curl -fsSL --retry 3 "$base/$path.sha1" | cut -c1-40)" \
       && [ "$(sha1sum "$FRAMEWORK.part" | cut -c1-40)" = "$expected" ]; then
      mv "$FRAMEWORK.part" "$FRAMEWORK"
      break
    fi
    rm -f "$FRAMEWORK.part"
  done
  [ -f "$FRAMEWORK" ] || die "could not download $path"
fi

# --- 3. Version (same rule as Gradle: number of git commits) -----------------
VERSION_CODE="$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 1)"
[ "$VERSION_CODE" -ge 1 ] 2>/dev/null || VERSION_CODE=1
VERSION_NAME="1.0.$VERSION_CODE"

rm -rf "$WORK"
mkdir -p "$WORK/gen" "$WORK/classes" "$DIST"

# --- 4. Resources + manifest + assets ----------------------------------------
log "Compiling resources"
aapt2 compile --dir "$APP/src/main/res" -o "$WORK/res.zip"

# Gradle takes the package name from `namespace`; aapt2 wants it in the manifest.
sed "s#<manifest #<manifest package=\"$PACKAGE\" #" \
  "$APP/src/main/AndroidManifest.xml" > "$WORK/AndroidManifest.xml"

log "Linking resources + packaging assets from ${WWW#"$ROOT"/}/"
aapt2 link -o "$WORK/app.unaligned.apk" \
  -I "$FRAMEWORK_RES" \
  --manifest "$WORK/AndroidManifest.xml" \
  --min-sdk-version "$MIN_SDK" --target-sdk-version "$TARGET_SDK" \
  --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
  --java "$WORK/gen" \
  -A "$WWW" \
  "$WORK/res.zip"

# --- 5. Java -> dex ----------------------------------------------------------
log "Compiling Java"
mapfile -t SOURCES < <(find "$APP/src/main/java" "$WORK/gen" -name '*.java')
javac -nowarn -Xlint:-options --release 8 -encoding UTF-8 \
  -cp "$FRAMEWORK" -d "$WORK/classes" "${SOURCES[@]}"

log "Dexing"
java -cp "$DX_JAR" com.android.dx.command.Main --dex \
  --min-sdk-version="$MIN_SDK" --output="$WORK/classes.dex" "$WORK/classes"
(cd "$WORK" && zip -q -X app.unaligned.apk classes.dex)

# --- 6. Align + sign ---------------------------------------------------------
log "Aligning + signing"
zipalign -f -p 4 "$WORK/app.unaligned.apk" "$WORK/app.aligned.apk"
apksigner sign \
  --ks "$KEYSTORE" --ks-type PKCS12 --ks-pass "pass:$KS_PASS" \
  --ks-key-alias "$KEY_ALIAS" --key-pass "pass:$KS_PASS" \
  --min-sdk-version "$MIN_SDK" \
  --out "$OUT" "$WORK/app.aligned.apk"

# --- 7. Checks ---------------------------------------------------------------
zipalign -c -p 4 "$OUT" >/dev/null || die "APK is not aligned"
apksigner verify --min-sdk-version "$MIN_SDK" "$OUT" || die "signature check failed"
unzip -l "$OUT" | grep -q ' assets/index.html$' || die "assets/index.html missing from the APK"
aapt2 dump badging "$OUT" | grep -E "^(package|sdkVersion|targetSdkVersion|launchable-activity)" || true

log "OK: ${OUT#"$ROOT"/} ($(du -h "$OUT" | cut -f1), versionCode $VERSION_CODE)"

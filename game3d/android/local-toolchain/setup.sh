#!/usr/bin/env bash
# Offline-ish Android arm64 C toolchain WITHOUT the NDK download (dl.google.com unreachable):
#   - compiler/linker: the distro's clang + lld (Debian/Ubuntu packages clang, lld, llvm)
#   - sysroot: the old NDK "platforms" tree kept in AOSP's development repo (tag android-6.0.0_r1,
#     public mirror github.com/aosp-mirror/platform_development): bionic + android/*.h, EGL,
#     GLES2/3, OpenSLES headers, per-library symbol lists (→ stub .so we generate) and crt sources
#   - compiler builtins: libgcc.a from Ubuntu's libgcc-13-dev-arm64-cross
# Result: $OUT/android-arm64.cmake (CMake toolchain file) + $OUT/ndk/sources/android/native_app_glue.
# API level of the headers is 21 (binaries run on Android 5.0+, we ship minSdk 26).
#
# Usage: game3d/android/local-toolchain/setup.sh   (prints the toolchain file path)
set -euo pipefail
CACHE="${CARPOLO_CACHE:-$HOME/.cache/carpolo-build}"
OUT="$CACHE/ndk-local"
AOSP="${AOSP_DEVELOPMENT:-$CACHE/aosp-development}"
API=21
TRIPLE=aarch64-linux-android$API
log() { printf '\033[1;36m==>\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

for t in clang ld.lld llvm-ar; do command -v $t >/dev/null || die "$t missing (apt-get install clang lld llvm)"; done
LIBGCC="$(ls /usr/lib/gcc-cross/aarch64-linux-gnu/*/libgcc.a 2>/dev/null | sort -V | tail -1 || true)"
if [ -z "$LIBGCC" ]; then
  log "Installing libgcc for aarch64 (apt)"
  SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO=sudo
  DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq --no-install-recommends libgcc-13-dev-arm64-cross >/dev/null
  LIBGCC="$(ls /usr/lib/gcc-cross/aarch64-linux-gnu/*/libgcc.a | sort -V | tail -1)"
fi

# --- 1. old NDK platform tree (sparse, blobless clone of one tag) --------------------------
if [ ! -f "$AOSP/ndk/platforms/android-21/arch-arm64/symbols/libc.so.functions.txt" ]; then
  log "Fetching the NDK platforms tree from aosp-mirror/platform_development (android-6.0.0_r1)"
  rm -rf "$AOSP"; mkdir -p "$AOSP"
  git -C "$AOSP" init -q
  git -C "$AOSP" remote add origin https://github.com/aosp-mirror/platform_development
  git -C "$AOSP" config core.sparseCheckout true
  printf '/ndk/platforms/android-*/include/\n/ndk/platforms/android-21/arch-arm64/\n/ndk/sources/android/native_app_glue/\n' \
    > "$AOSP/.git/info/sparse-checkout"
  git -C "$AOSP" fetch -q --depth 1 --filter=blob:none origin tag android-6.0.0_r1
  git -C "$AOSP" checkout -q android-6.0.0_r1
fi
P="$AOSP/ndk/platforms"

rm -rf "$OUT"; mkdir -p "$OUT/sysroot/usr/include" "$OUT/sysroot/usr/lib" "$OUT/build"
SYS="$OUT/sysroot"

# --- 2. headers: android-3 → android-21 overlays, then arm64 arch headers ------------------
log "Assembling headers (API $API)"
for lvl in 3 4 5 8 9 12 13 14 15 16 17 18 19 21; do
  [ -d "$P/android-$lvl/include" ] && cp -a "$P/android-$lvl/include/." "$SYS/usr/include/"
done
cp -a "$P/android-21/arch-arm64/include/." "$SYS/usr/include/"
# Old (FreeBSD-style) cdefs.h hides C99 math etc. when a file defines _POSIX_C_SOURCE (raylib does);
# modern bionic exposes everything: do the same.
python3 - "$SYS/usr/include/sys/cdefs.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read(); i = s.rstrip().rfind('#endif')
s = s[:i] + ('#undef __POSIX_VISIBLE\n#define __POSIX_VISIBLE 200809\n#undef __XSI_VISIBLE\n#define __XSI_VISIBLE 700\n'
             '#undef __BSD_VISIBLE\n#define __BSD_VISIBLE 1\n#undef __ISO_C_VISIBLE\n#define __ISO_C_VISIBLE 2011\n') + s[i:]
open(p, 'w').write(s)
PY
# Modern bionic's <sys/time.h> also declares select() (miniaudio relies on it)
python3 - "$SYS/usr/include/sys/time.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read(); i = s.rstrip().rfind('#endif')
s = s[:i] + '#include <sys/select.h>\n' + s[i:]
open(p, 'w').write(s)
PY
# Current Khronos EGL/GLES headers (raylib needs EGL 1.5 names such as EGL_OPENGL_ES3_BIT); their
# eglplatform.h has an __ANDROID__ branch. Debian/Ubuntu: libegl-dev, libgles-dev.
for d in EGL KHR GLES2 GLES3; do
  [ -d "/usr/include/$d" ] || die "/usr/include/$d missing (apt-get install libegl-dev libgles-dev)"
  rm -rf "$SYS/usr/include/$d"; cp -a "/usr/include/$d" "$SYS/usr/include/$d"
done
# Constant added in API 25 that miniaudio uses
echo '#ifndef SL_ANDROID_RECORDING_PRESET_UNPROCESSED
#define SL_ANDROID_RECORDING_PRESET_UNPROCESSED ((SLuint32) 0x00000005)
#endif' >> "$SYS/usr/include/SLES/OpenSLES_AndroidConfiguration.h"
# clang >= 13 defines __ANDROID_MIN_SDK_VERSION__; the old api-level.h wants __ANDROID_API__
cat > "$SYS/usr/include/android/api-level.h" <<H
#ifndef ANDROID_API_LEVEL_H
#define ANDROID_API_LEVEL_H
#ifndef __ANDROID_API__
#define __ANDROID_API__ $API
#endif
#endif
H

# --- 3. stub shared libraries from the symbol lists -----------------------------------------
log "Generating stub libraries"
CFLAGS_T=(--target=$TRIPLE -fPIC -nostdlib -O0)
for lib in libc libm libdl liblog libandroid libEGL libGLESv2 libGLESv3 libOpenSLES libz libjnigraphics; do
  f="$P/android-21/arch-arm64/symbols/$lib.so.functions.txt"
  v="$P/android-21/arch-arm64/symbols/$lib.so.variables.txt"
  [ -f "$f" ] || continue
  src="$OUT/build/$lib.c"
  : > "$src"
  while read -r s; do [ -n "$s" ] && echo "void $s(void){}" >> "$src"; done < "$f"
  [ -f "$v" ] && while read -r s; do [ -n "$s" ] && echo "int $s[8];" >> "$src"; done < "$v"
  clang "${CFLAGS_T[@]}" -fno-builtin -w -shared -fuse-ld=lld -Wl,-soname,$lib.so \
    -o "$SYS/usr/lib/$lib.so" "$src"
done
# raylib links "atomic": aarch64 atomics are inline, an empty archive is enough
llvm-ar rcs "$SYS/usr/lib/libatomic.a"
cp "$LIBGCC" "$SYS/usr/lib/libgcc.a"

# --- 4. crt objects for shared libraries -----------------------------------------------------
log "Building crtbegin_so.o / crtend_so.o"
# (bionic's crtbegin_so.c needs __dso_handle_so.h, not in that tree: same content inlined here)
cat > "$OUT/build/crtbegin_so.c" <<'C'
extern void __cxa_finalize(void *);
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;
__attribute__((visibility("hidden"), destructor)) void __on_dlclose(void) { __cxa_finalize(&__dso_handle); }
extern int __cxa_atexit(void (*)(void *), void *, void *);
__attribute__((visibility("hidden"))) int atexit(void (*func)(void))
{
    return __cxa_atexit((void (*)(void *))func, (void *)0, &__dso_handle);
}
C
clang --target=$TRIPLE -fPIC -O2 -ffreestanding -c "$OUT/build/crtbegin_so.c" -o "$SYS/usr/lib/crtbegin_so.o"
printf '\t.section .note.GNU-stack,"",%%progbits\n' > "$OUT/build/crtend_so.S"
clang --target=$TRIPLE -c "$OUT/build/crtend_so.S" -o "$SYS/usr/lib/crtend_so.o"

# --- 5. native_app_glue in an NDK-like layout (raylib's CMake looks in ${ANDROID_NDK}/sources) --
mkdir -p "$OUT/ndk/sources/android"
cp -a "$AOSP/ndk/sources/android/native_app_glue" "$OUT/ndk/sources/android/"

# --- 6. CMake toolchain file ----------------------------------------------------------------
cat > "$OUT/android-arm64.cmake" <<T
# Generated by game3d/android/local-toolchain/setup.sh — arm64-v8a only.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(ANDROID TRUE)
set(ANDROID_ABI arm64-v8a)
set(ANDROID_NDK "$OUT/ndk")
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET $TRIPLE)
set(CMAKE_ASM_COMPILER_TARGET $TRIPLE)
set(CMAKE_SYSROOT "$SYS")
set(CMAKE_C_FLAGS_INIT "-DANDROID -fPIC -ffunction-sections -fdata-sections -funwind-tables -fstack-protector-strong -Wno-unused-command-line-argument")
# -nostdlib: link our crt objects + stubs explicitly (no glibc), builtins from libgcc.a
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -nostdlib $SYS/usr/lib/crtbegin_so.o -L$SYS/usr/lib")
set(CMAKE_C_STANDARD_LIBRARIES "-lc -lm -ldl $SYS/usr/lib/libgcc.a $SYS/usr/lib/crtend_so.o")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
T
log "Toolchain ready"
echo "$OUT/android-arm64.cmake"

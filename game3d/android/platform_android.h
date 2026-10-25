// Android-only helpers for the native game (implemented in android_bridge.c, compiled into
// libmain.so only for Android builds). Use them under #ifdef PLATFORM_ANDROID, e.g.
//
//   #if defined(PLATFORM_ANDROID)
//   #include "platform_android.h"
//   #endif
//   ...
//   #if defined(PLATFORM_ANDROID)
//   AndroidVibrate(120);
//   #endif
#ifndef CARPOLO_PLATFORM_ANDROID_H
#define CARPOLO_PLATFORM_ANDROID_H

// Short haptic pulse (milliseconds). Safe to call from the game thread; no-op if unavailable.
void AndroidVibrate(int milliseconds);

#endif

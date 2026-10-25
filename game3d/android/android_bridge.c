// JNI bridge from the native game to fr.carpolo.cactus3d.GameActivity (Android only).
#include <jni.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include "platform_android.h"

struct android_app *GetAndroidApp(void);   // provided by raylib (rcore_android.c)

void AndroidVibrate(int milliseconds)
{
    struct android_app *app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->vm || !app->activity->clazz) return;

    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    jclass cls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (cls) {
        jmethodID m = (*env)->GetStaticMethodID(env, cls, "vibrate", "(I)V");
        if (m) (*env)->CallStaticVoidMethod(env, cls, m, (jint)milliseconds);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (attached) (*vm)->DetachCurrentThread(vm);
}

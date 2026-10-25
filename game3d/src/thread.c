// Portable threads: Win32 (Windows) or pthreads (Linux, macOS, Android)
#include "thread.h"
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct cp_thread { HANDLE h; cp_thread_fn fn; void *arg; };
struct cp_lock { CRITICAL_SECTION cs; CONDITION_VARIABLE cv; };

static DWORD WINAPI trampoline(LPVOID p)
{
    cp_thread *t = (cp_thread *)p;
    t->fn(t->arg);
    return 0;
}

cp_thread *cp_thread_create(cp_thread_fn fn, void *arg)
{
    cp_thread *t = (cp_thread *)calloc(1, sizeof(cp_thread));
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
void cp_thread_join(cp_thread *t) { if (!t) return; WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); free(t); }
cp_lock *cp_lock_create(void)
{
    cp_lock *l = (cp_lock *)calloc(1, sizeof(cp_lock));
    if (!l) return NULL;
    InitializeCriticalSection(&l->cs);
    InitializeConditionVariable(&l->cv);
    return l;
}
void cp_lock_destroy(cp_lock *l) { if (!l) return; DeleteCriticalSection(&l->cs); free(l); }
void cp_lock_acquire(cp_lock *l) { EnterCriticalSection(&l->cs); }
void cp_lock_release(cp_lock *l) { LeaveCriticalSection(&l->cs); }
void cp_lock_wait(cp_lock *l) { SleepConditionVariableCS(&l->cv, &l->cs, INFINITE); }
void cp_lock_signal(cp_lock *l) { WakeConditionVariable(&l->cv); }
void cp_lock_broadcast(cp_lock *l) { WakeAllConditionVariable(&l->cv); }

#else
#include <pthread.h>

struct cp_thread { pthread_t t; };
struct cp_lock { pthread_mutex_t m; pthread_cond_t c; };

cp_thread *cp_thread_create(cp_thread_fn fn, void *arg)
{
    cp_thread *t = (cp_thread *)calloc(1, sizeof(cp_thread));
    if (!t) return NULL;
    if (pthread_create(&t->t, NULL, fn, arg) != 0) { free(t); return NULL; }
    return t;
}
void cp_thread_join(cp_thread *t) { if (!t) return; pthread_join(t->t, NULL); free(t); }
cp_lock *cp_lock_create(void)
{
    cp_lock *l = (cp_lock *)calloc(1, sizeof(cp_lock));
    if (!l) return NULL;
    pthread_mutex_init(&l->m, NULL);
    pthread_cond_init(&l->c, NULL);
    return l;
}
void cp_lock_destroy(cp_lock *l) { if (!l) return; pthread_mutex_destroy(&l->m); pthread_cond_destroy(&l->c); free(l); }
void cp_lock_acquire(cp_lock *l) { pthread_mutex_lock(&l->m); }
void cp_lock_release(cp_lock *l) { pthread_mutex_unlock(&l->m); }
void cp_lock_wait(cp_lock *l) { pthread_cond_wait(&l->c, &l->m); }
void cp_lock_signal(cp_lock *l) { pthread_cond_signal(&l->c); }
void cp_lock_broadcast(cp_lock *l) { pthread_cond_broadcast(&l->c); }
#endif

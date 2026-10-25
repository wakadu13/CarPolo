// Minimal portable threading for the background chunk builder.
// Implemented in thread.c with Win32 or POSIX threads (windows.h stays out of raylib code).
#ifndef CARPOLO_THREAD_H
#define CARPOLO_THREAD_H

#include <stdbool.h>

typedef struct cp_thread cp_thread;
typedef struct cp_lock cp_lock;          // mutex + condition variable
typedef void *(*cp_thread_fn)(void *);

cp_thread *cp_thread_create(cp_thread_fn fn, void *arg);   // NULL on failure
void       cp_thread_join(cp_thread *t);                   // also frees t
cp_lock   *cp_lock_create(void);
void       cp_lock_destroy(cp_lock *l);
void       cp_lock_acquire(cp_lock *l);
void       cp_lock_release(cp_lock *l);
void       cp_lock_wait(cp_lock *l);                       // wait on the condition (lock held)
void       cp_lock_signal(cp_lock *l);
void       cp_lock_broadcast(cp_lock *l);

#endif

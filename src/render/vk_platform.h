/*
 * Host primitives for the Vulkan back end: the window handle type, a mutex,
 * a condition variable, detached threads, a monotonic clock and sleeps.
 *
 * POSIX: plain pthreads and clock_gettime. Windows: slim reader/writer locks,
 * condition variables, CreateThread and QueryPerformanceCounter, so the
 * Windows build needs no winpthread DLL and stays buildable with MSVC (the
 * same mapping as reference/SSX3-recomp-koen src/vk/vkr_thread.h, GPL-2.0+).
 * Only included by vk_backend.c and the window files it includes.
 */
#ifndef SSX3_VK_PLATFORM_H
#define SSX3_VK_PLATFORM_H

#include <stddef.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

typedef HWND               VkbWindow;
typedef SRWLOCK            vkb_mutex;
typedef CONDITION_VARIABLE vkb_cond;
#define VKB_MUTEX_INIT     SRWLOCK_INIT

static inline void vkb_mutex_init(vkb_mutex *m)   { InitializeSRWLock(m); }
static inline void vkb_cond_init(vkb_cond *c)     { InitializeConditionVariable(c); }
static inline void vkb_lock(vkb_mutex *m)         { AcquireSRWLockExclusive(m); }
static inline void vkb_unlock(vkb_mutex *m)       { ReleaseSRWLockExclusive(m); }
static inline void vkb_cond_wait(vkb_cond *c, vkb_mutex *m)
{
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline void vkb_cond_signal(vkb_cond *c)   { WakeConditionVariable(c); }

/* Monotonic seconds. */
static inline double vkb_now(void)
{
    static double inv;
    LARGE_INTEGER t;
    if (!inv) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv = 1.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
}

/* Wait on c for at most `s` seconds (rounded up to whole milliseconds). */
static inline void vkb_cond_wait_s(vkb_cond *c, vkb_mutex *m, double s)
{
    DWORD ms = (DWORD)(s * 1000.0 + 0.999);
    SleepConditionVariableSRW(c, m, ms ? ms : 1, 0);
}

/* Sleep `s` seconds. Sleep() rounds to the timer tick (15.6 ms unless someone
 * raised the resolution), far too coarse for the presenter's frame cap, so a
 * high-resolution waitable timer (Windows 10 1803+) does the wait; older
 * systems fall back to a plain timer, then to Sleep. */
#ifdef _MSC_VER
#define VKB_THREAD_LOCAL __declspec(thread)
#else
#define VKB_THREAD_LOCAL _Thread_local
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static inline void vkb_sleep_s(double s)
{
    static VKB_THREAD_LOCAL HANDLE timer;
    static VKB_THREAD_LOCAL int tried;
    LARGE_INTEGER due;

    if (s <= 0)
        return;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    due.QuadPart = -(LONGLONG)(s * 1e7);       /* relative, 100 ns units */
    if (!due.QuadPart)
        due.QuadPart = -1;
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep((DWORD)(s * 1000.0));
}

typedef struct { void *(*fn)(void *); void *arg; } vkb_thread_start_;

static DWORD WINAPI vkb_thread_tramp_(LPVOID p)
{
    vkb_thread_start_ s = *(vkb_thread_start_ *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

/* Start fn(arg) on a detached thread with `stack` bytes (0: the default).
 * Returns 1 on success. */
static inline int vkb_thread_start(void *(*fn)(void *), void *arg, size_t stack)
{
    vkb_thread_start_ *s = (vkb_thread_start_ *)malloc(sizeof *s);
    HANDLE th;
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    th = CreateThread(NULL, stack, vkb_thread_tramp_, s,
                      stack ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, NULL);
    if (!th) {
        free(s);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

#else /* POSIX */
#include <pthread.h>
#include <time.h>
#include <SDL.h>

typedef SDL_Window        *VkbWindow;
typedef pthread_mutex_t    vkb_mutex;
typedef pthread_cond_t     vkb_cond;
#define VKB_MUTEX_INIT     PTHREAD_MUTEX_INITIALIZER

static inline void vkb_mutex_init(vkb_mutex *m)   { pthread_mutex_init(m, NULL); }
static inline void vkb_cond_init(vkb_cond *c)     { pthread_cond_init(c, NULL); }
static inline void vkb_lock(vkb_mutex *m)         { pthread_mutex_lock(m); }
static inline void vkb_unlock(vkb_mutex *m)       { pthread_mutex_unlock(m); }
static inline void vkb_cond_wait(vkb_cond *c, vkb_mutex *m) { pthread_cond_wait(c, m); }
static inline void vkb_cond_signal(vkb_cond *c)   { pthread_cond_signal(c); }

static inline double vkb_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline void vkb_cond_wait_s(vkb_cond *c, vkb_mutex *m, double s)
{
    struct timespec ts;
    long ns;
    clock_gettime(CLOCK_REALTIME, &ts);
    ns = ts.tv_nsec + (long)(s * 1e9);
    ts.tv_sec += ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    pthread_cond_timedwait(c, m, &ts);
}

static inline void vkb_sleep_s(double s)
{
    struct timespec ts;
    if (s <= 0)
        return;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static inline int vkb_thread_start(void *(*fn)(void *), void *arg, size_t stack)
{
    pthread_attr_t attr;
    pthread_t th;
    int rc;

    pthread_attr_init(&attr);
    if (stack)
        pthread_attr_setstacksize(&attr, stack);
    rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}
#endif

#endif /* SSX3_VK_PLATFORM_H */

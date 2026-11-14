#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stddef.h>

extern int goNilDeref(void);

void test_hooks_segv(void)
{
    volatile int *p = 0;
    *p = 1;
}

static void *ill_thread(void *arg)
{
    (void)arg;
#if defined(__x86_64__)
    __asm__("ud2");
#elif defined(__aarch64__)
    __asm__(".inst 0x00000000");
#endif
    return NULL;
}

static void *segv_thread(void *arg)
{
    (void)arg;
    test_hooks_segv();
    return NULL;
}

static void *raise_segv_thread(void *arg)
{
    (void)arg;
    raise(SIGSEGV);
    return NULL;
}

static void *segv_after_barrier_thread(void *arg)
{
    pthread_barrier_wait(arg);
    test_hooks_segv();
    return NULL;
}

static void *go_nil_then_segv_thread(void *arg)
{
    (void)arg;
    goNilDeref();
    volatile int *p = (int *)0x10;
    *p = 1;
    return NULL;
}

static void *go_nil_thread(void *arg)
{
    *(int *)arg = goNilDeref();
    return NULL;
}

static int run_thread(void *(*start)(void *), void *arg)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, start, arg) != 0) {
        return -1;
    }
    return pthread_join(thread, NULL);
}

int test_hooks_segv_on_c_thread(void)
{
    return run_thread(segv_thread, NULL);
}

int test_hooks_go_nil_on_c_thread(void)
{
    int recovered = 0;
    if (run_thread(go_nil_thread, &recovered) != 0) {
        return -1;
    }
    return recovered;
}

int test_hooks_go_nil_then_segv_on_c_thread(void)
{
    return run_thread(go_nil_then_segv_thread, NULL);
}

void test_hooks_abort(void)
{
    abort();
}

int test_hooks_raise_segv_on_c_thread(void)
{
    return run_thread(raise_segv_thread, NULL);
}

int test_hooks_segv_on_two_c_threads(void)
{
    pthread_barrier_t barrier;
    pthread_t threads[2];
    if (pthread_barrier_init(&barrier, NULL, 2) != 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (pthread_create(&threads[i], NULL, segv_after_barrier_thread, &barrier) != 0) {
            return -1;
        }
    }
    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
    }
    return 0;
}

int test_hooks_ill_on_c_thread(void)
{
    return run_thread(ill_thread, NULL);
}

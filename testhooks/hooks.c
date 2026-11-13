#include <pthread.h>
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

int test_hooks_ill_on_c_thread(void)
{
    return run_thread(ill_thread, NULL);
}

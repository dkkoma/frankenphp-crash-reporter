#define _GNU_SOURCE
#include <dlfcn.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

__attribute__((noinline)) static int recurse(int depth)
{
    volatile char frame[256];
    static int (*volatile next)(int) = recurse;
    frame[0] = (char)depth;
    return next(depth + 1) + frame[0];
}

__attribute__((noinline)) static void write_to_null(void)
{
    volatile int *p = 0;
    *p = 1;
}

__attribute__((noinline)) static void overwrite_return_address_then_fault(void)
{
    static void (*volatile fault)(void) = write_to_null;
    void **frame = __builtin_frame_address(0);
    frame[1] = (void *)0x1000;
    fault();
    __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) static void overflow_stack_buffer(void)
{
    static uintptr_t unmapped_addresses[32];
    static volatile size_t len = sizeof unmapped_addresses;
    char buf[16];
    for (size_t i = 0; i < COUNT(unmapped_addresses); i++) {
        unmapped_addresses[i] = 0x1000;
    }
    memcpy(buf, unmapped_addresses, len);
    __asm__ volatile("" ::"r"(buf) : "memory");
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    if (strcmp(name, "segv") == 0) {
        volatile int *p = 0;
        *p = 1;
    }
    if (strcmp(name, "protected") == 0) {
        volatile int *page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        *page = 1;
    }
    if (strcmp(name, "wildcall") == 0) {
        void (*volatile unmapped)(void) = (void (*)(void))0x10;
        unmapped();
    }
    if (strcmp(name, "abort") == 0) {
        abort();
    }
    if (strcmp(name, "stack_overflow") == 0) {
        recurse(0);
    }
    if (strcmp(name, "overwritten_return_address") == 0) {
        overwrite_return_address_then_fault();
    }
    if (strcmp(name, "stack_buffer_overflow") == 0) {
        overflow_stack_buffer();
    }
    if (strcmp(name, "probe") == 0) {
        int (*probe)(void *) = dlsym(RTLD_DEFAULT, "previous_handler_probe");
        if (probe) {
            probe((void *)0x20);
        }
    }
    int (*real)(const char *, struct passwd *, char *, size_t, struct passwd **) = dlsym(RTLD_NEXT, "getpwnam_r");
    return real(name, pwd, buf, buflen, result);
}

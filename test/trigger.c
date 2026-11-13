#define _GNU_SOURCE
#include <dlfcn.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    if (strcmp(name, "segv") == 0) {
        volatile int *p = 0;
        *p = 1;
    }
    if (strcmp(name, "wildcall") == 0) {
        void (*volatile unmapped)(void) = (void (*)(void))0x10;
        unmapped();
    }
    if (strcmp(name, "abort") == 0) {
        abort();
    }
    int (*real)(const char *, struct passwd *, char *, size_t, struct passwd **) = dlsym(RTLD_NEXT, "getpwnam_r");
    return real(name, pwd, buf, buflen, result);
}

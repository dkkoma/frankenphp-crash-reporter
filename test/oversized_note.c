#define _GNU_SOURCE
#include <dlfcn.h>
#include <grp.h>
#include <string.h>

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result)
{
    if (strcmp(name, "segv") == 0) {
        volatile int *p = 0;
        *p = 1;
    }
    int (*real)(const char *, struct group *, char *, size_t, struct group **) = dlsym(RTLD_NEXT, "getgrnam_r");
    return real(name, grp, buf, buflen, result);
}

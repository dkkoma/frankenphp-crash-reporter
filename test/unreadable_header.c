#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

void make_own_elf_header_unreadable_then_fault(void)
{
    Dl_info self;
    if (!dladdr(&make_own_elf_header_unreadable_then_fault, &self)) {
        return;
    }
    if (mprotect(self.dli_fbase, (size_t)sysconf(_SC_PAGESIZE), PROT_NONE) != 0) {
        return;
    }
    volatile int *p = 0;
    *p = 1;
}

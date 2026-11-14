#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void say(const char *message)
{
    ssize_t written = write(STDERR_FILENO, message, strlen(message));
    (void)written;
}

static int make_writable(void *addr)
{
    uintptr_t page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)addr & ~(page_size - 1));
    return addr != NULL && mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0;
}

static void siginfo_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    if (sig == SIGSEGV && info->si_code > 0 && make_writable(info->si_addr)) {
        say("previous handler recovered from the fault\n");
        return;
    }
    if (sig == SIGSEGV && info->si_code > 0 && info->si_addr == NULL) {
        say("previous handler got the original fault\n");
    } else {
        say("previous handler got something else\n");
    }
    _exit(77);
}

static void plain_handler(int sig)
{
    say(sig == SIGSEGV ? "plain previous handler got SIGSEGV\n" : "plain previous handler got something else\n");
    _exit(78);
}

__attribute__((constructor)) static void install(void)
{
    const char *kind = getenv("PREVIOUS_HANDLER");
    struct sigaction action;
    memset(&action, 0, sizeof action);
    if (kind && strcmp(kind, "ignore") == 0) {
        action.sa_handler = SIG_IGN;
    } else if (kind && strcmp(kind, "plain") == 0) {
        action.sa_handler = plain_handler;
    } else {
        action.sa_sigaction = siginfo_handler;
        action.sa_flags = SA_SIGINFO;
    }
    sigaction(SIGSEGV, &action, NULL);
}

#define _GNU_SOURCE
#include <setjmp.h>
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
    if (sig == SIGABRT) {
        say("previous handler got SIGABRT\n");
        _exit(79);
    }
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

static __thread sigjmp_buf probe_failed;
static __thread volatile int probing;

static void probing_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)info;
    (void)ucontext;
    if (sig == SIGSEGV && probing) {
        say("previous handler jumps out of the probe\n");
        siglongjmp(probe_failed, 1);
    }
    say("previous handler got a fault outside a probe\n");
    _exit(77);
}

int previous_handler_probe(void *addr)
{
    probing = 1;
    if (sigsetjmp(probe_failed, 1)) {
        probing = 0;
        return 0;
    }
    (void)*(volatile char *)addr;
    probing = 0;
    return 1;
}

static void returning_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
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
    } else if (kind && strcmp(kind, "return") == 0) {
        action.sa_sigaction = returning_handler;
        action.sa_flags = SA_SIGINFO;
        sigaction(SIGABRT, &action, NULL);
    } else if (kind && strcmp(kind, "default_with_siginfo") == 0) {
        action.sa_handler = SIG_DFL;
        action.sa_flags = SA_SIGINFO;
        sigaction(SIGBUS, &action, NULL);
        return;
    } else if (kind && strcmp(kind, "probe") == 0) {
        action.sa_sigaction = probing_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    } else {
        action.sa_sigaction = siginfo_handler;
        action.sa_flags = SA_SIGINFO;
        sigaction(SIGABRT, &action, NULL);
    }
    sigaction(SIGSEGV, &action, NULL);
}

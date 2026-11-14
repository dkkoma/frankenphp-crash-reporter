#include <signal.h>
#include <string.h>
#include <unistd.h>

static void say(const char *message)
{
    ssize_t written = write(STDERR_FILENO, message, strlen(message));
    (void)written;
}

static void handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    if (sig == SIGSEGV && info->si_code > 0 && info->si_addr == NULL) {
        say("previous handler got the original fault\n");
    } else {
        say("previous handler got something else\n");
    }
    _exit(77);
}

__attribute__((constructor)) static void install(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
}

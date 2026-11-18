//go:build linux && cgo && (amd64 || arm64)

#include "crash_report.h"

#include <setjmp.h>
#include <unwind.h>

#define TLS __thread __attribute__((tls_model("initial-exec")))

static const int recoverable_in_go[] = {SIGSEGV, SIGBUS, SIGFPE};
static const int always_fatal[] = {SIGILL, SIGABRT};

enum { NOT_INSTALLED, INSTALLED, DISABLED };

extern const ElfW(Ehdr) __ehdr_start __attribute__((visibility("hidden")));

static struct sigaction go_actions[NSIG];
static struct sigaction previous_actions[NSIG];
static atomic_int state = NOT_INSTALLED;

static TLS int first_signal;
static TLS int first_code;
static TLS void *first_fault_addr;
static TLS uintptr_t first_pc;
static TLS uintptr_t first_return_address;
static TLS void *first_frames[MAX_FRAMES];
static TLS int first_frame_count;
static TLS int first_callers_start;
static TLS int captured;
static TLS int handed_back_to_go;
static TLS int last_reported_signal;
static TLS uintptr_t last_reported_pc;
static TLS volatile sig_atomic_t faults_recoverable;
static TLS sigjmp_buf recover_from_fault;

static int report(void)
{
    if (!lock_reports()) {
        return 0;
    }
    struct crash crash = {
        .signal = first_signal,
        .code = first_code,
        .fault_addr = first_fault_addr,
        .pc = first_pc,
        .return_address = first_return_address,
        .frames = first_frames,
        .frame_count = first_frame_count,
        .callers_start = first_callers_start,
    };
    write_report(&crash, NULL);
    unlock_reports();
    return 1;
}

static _Unwind_Reason_Code record_frame(struct _Unwind_Context *context, void *unused)
{
    (void)unused;
    if (first_frame_count == MAX_FRAMES) {
        return _URC_END_OF_STACK;
    }
    first_frames[first_frame_count++] = (void *)_Unwind_GetIP(context);
    return _URC_NO_REASON;
}

static void walk_stack(void)
{
    sigset_t faults, blocked;
    sigemptyset(&faults);
    sigaddset(&faults, SIGSEGV);
    sigaddset(&faults, SIGBUS);
    first_frame_count = 0;
    if (sigsetjmp(recover_from_fault, 1) == 0) {
        faults_recoverable = 1;
        pthread_sigmask(SIG_UNBLOCK, &faults, &blocked);
        _Unwind_Backtrace(record_frame, NULL);
        pthread_sigmask(SIG_SETMASK, &blocked, NULL);
    }
    faults_recoverable = 0;
    if (first_frame_count > 0 && first_frames[first_frame_count - 1] == NULL) {
        first_frame_count--;
    }
}

static void capture(int sig, siginfo_t *info, void *ucontext)
{
    first_signal = sig;
    first_code = info->si_code;
    first_fault_addr = info->si_addr;
    first_pc = context_pc(ucontext);
    walk_stack();
    first_callers_start = find_callers_start(first_frames, first_frame_count, first_pc);
    first_return_address = first_callers_start < 0 ? context_return_address(ucontext) : 0;
}

static void report_non_go_fault(int sig, siginfo_t *info, void *ucontext)
{
    if (faults_recoverable) {
        siglongjmp(recover_from_fault, 1);
    }
    if (handed_back_to_go) {
        die_by_default(sig);
    }
    if (atomic_load(&state) != DISABLED) {
        if (!captured) {
            capture(sig, info, ucontext);
        }
        if ((first_signal != last_reported_signal || first_pc != last_reported_pc) && report()) {
            last_reported_signal = first_signal;
            last_reported_pc = first_pc;
        }
    }
    if (call_action(&previous_actions[sig], sig, info, ucontext)) {
        return;
    }
    if (info->si_code <= 0 || !go_actions[sig].sa_sigaction) {
        die_by_default(sig);
    }
    handed_back_to_go = 1;
    siginfo_t sent_by_process = *info;
    sent_by_process.si_code = SI_USER;
    go_actions[sig].sa_sigaction(sig, &sent_by_process, ucontext);
    die_by_default(sig);
}

static void capture_then_go(int sig, siginfo_t *info, void *ucontext)
{
    if (faults_recoverable) {
        siglongjmp(recover_from_fault, 1);
    }
    if (!handed_back_to_go) {
        capture(sig, info, ucontext);
        captured = 1;
    }
    go_actions[sig].sa_sigaction(sig, info, ucontext);
    captured = 0;
}

static void report_then_go(int sig, siginfo_t *info, void *ucontext)
{
    capture(sig, info, ucontext);
    report();
    go_actions[sig].sa_sigaction(sig, info, ucontext);
}

__attribute__((constructor)) static void install_before_go_runtime(void)
{
    const char *enabled = getenv("FRANKENPHP_CRASH_REPORTER");
    if (!enabled || strcmp(enabled, "true") != 0) {
        return;
    }
    executable_base = (uintptr_t)&__ehdr_start;
    executable_is_position_independent = __ehdr_start.e_type != ET_EXEC;
    walk_stack();
    for (size_t i = 0; i < COUNT(recoverable_in_go); i++) {
        struct sigaction ours;
        memset(&ours, 0, sizeof ours);
        ours.sa_sigaction = report_non_go_fault;
        ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigfillset(&ours.sa_mask);
        int sig = recoverable_in_go[i];
        if (sigaction(sig, &ours, &previous_actions[sig]) != 0) {
            atomic_store(&state, DISABLED);
            return;
        }
    }
    atomic_store(&state, INSTALLED);
}

static int is_ours(const struct sigaction *action)
{
    return action->sa_sigaction == report_non_go_fault || action->sa_sigaction == capture_then_go ||
           action->sa_sigaction == report_then_go;
}

static int read_go_action(int sig)
{
    struct sigaction *go_action = &go_actions[sig];
    return sigaction(sig, NULL, go_action) == 0 && (go_action->sa_flags & SA_SIGINFO) &&
           (go_action->sa_flags & SA_ONSTACK) && !is_ours(go_action);
}

static int chain_before_go(int sig, void (*handler)(int, siginfo_t *, void *))
{
    struct sigaction ours = go_actions[sig];
    ours.sa_sigaction = handler;
    return sigaction(sig, &ours, NULL) == 0;
}

enum { ARMED, ERR_NOT_INSTALLED, ERR_GO_HANDLERS };

int crash_reporter_arm(void)
{
    if (atomic_load(&state) != INSTALLED) {
        return ERR_NOT_INSTALLED;
    }
    int ok = 1;
    for (size_t i = 0; i < COUNT(recoverable_in_go); i++) {
        ok = ok && read_go_action(recoverable_in_go[i]);
    }
    for (size_t i = 0; i < COUNT(always_fatal); i++) {
        ok = ok && read_go_action(always_fatal[i]);
    }
    for (size_t i = 0; ok && i < COUNT(recoverable_in_go); i++) {
        ok = chain_before_go(recoverable_in_go[i], capture_then_go);
    }
    for (size_t i = 0; ok && i < COUNT(always_fatal); i++) {
        ok = chain_before_go(always_fatal[i], report_then_go);
    }
    if (!ok) {
        atomic_store(&state, DISABLED);
        return ERR_GO_HANDLERS;
    }
    return ARMED;
}

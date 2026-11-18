// The signal handler of the extension. It is installed after the Go runtime
// has started, so it follows what the os/signal documentation asks of non-Go
// code that installs a handler for a synchronous signal: it records the Go
// handler, decides from the PC whether the signal hit Go code, and calls the
// Go handler with the original arguments. Only a signal outside Go code is
// reported; the Go handler decides how the process ends in every case.
//
// This file includes no PHP header. It cannot use initial-exec thread-local
// storage, since the extension is loaded with dlopen, so all its state is
// global and used only while holding the report lock.

#include "crash_report.h"
#include "php_frames.h"

#include <dlfcn.h>
#include <elf.h>
#include <setjmp.h>
#include <unwind.h>

static const int handled[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

static struct sigaction previous_actions[NSIG];
// Whether on_signal is in the chain for the signal. disarm leaves it there
// when another handler was installed after it, and that handler may still
// call it; the next arm then keeps the saved Go handler.
static int installed[NSIG];
static atomic_int armed;
static uintptr_t go_text_start;
static uintptr_t go_text_end;

static volatile pid_t guarded_tid;
static sigjmp_buf guard;

static pid_t last_reported_tid;
static int last_reported_signal;
static uintptr_t last_reported_pc;
static uint64_t last_reported_at;

// The same fault again within this time is the instruction faulting again
// after a handler returned. A later one is a new crash.
#define REFAULT_WINDOW_NS (1000ULL * 1000 * 1000)

struct walk {
    void *frames[MAX_FRAMES];
    volatile int count;
};

static struct walk walk_state;
static struct php_frames php_state;

static pid_t current_tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 * 1000 * 1000 + (uint64_t)now.tv_nsec;
}

// Runs fn with SIGSEGV and SIGBUS unblocked. A fault inside it returns here
// instead of ending the process. The caller holds the report lock.
static void run_guarded(void (*fn)(void *), void *arg)
{
    sigset_t faults, blocked;
    sigemptyset(&faults);
    sigaddset(&faults, SIGSEGV);
    sigaddset(&faults, SIGBUS);
    if (sigsetjmp(guard, 1) == 0) {
        guarded_tid = current_tid();
        pthread_sigmask(SIG_UNBLOCK, &faults, &blocked);
        fn(arg);
        pthread_sigmask(SIG_SETMASK, &blocked, NULL);
    }
    guarded_tid = 0;
}

static _Unwind_Reason_Code record_frame(struct _Unwind_Context *context, void *arg)
{
    struct walk *walk = arg;
    if (walk->count == MAX_FRAMES) {
        return _URC_END_OF_STACK;
    }
    walk->frames[walk->count] = (void *)_Unwind_GetIP(context);
    walk->count++;
    return _URC_NO_REASON;
}

static void walk_stack(void *arg)
{
    _Unwind_Backtrace(record_frame, arg);
}

static void collect_php_frames(void *arg)
{
    crash_reporter_collect_php_frames(arg);
}

static int in_go_code(uintptr_t pc)
{
    return pc >= go_text_start && pc < go_text_end;
}

static void append_php_frames(void)
{
    if (!php_state.context) {
        return;
    }
    append_str(",\"php_frames\":[");
    int shown = 0;
    for (; shown < php_state.count; shown++) {
        const struct php_frame *frame = &php_state.frames[shown];
        size_t len_without_frame = out_len;
        if (shown > 0) {
            append(",", 1);
        }
        append("{", 1);
        if (frame->function_len > 0) {
            append_str("\"function\":\"");
            if (frame->scope_len > 0) {
                append_json_chars(frame->scope, frame->scope_len);
                append_str("::");
            }
            append_json_chars(frame->function, frame->function_len);
            append("\"", 1);
        }
        if (frame->file_len > 0) {
            if (frame->function_len > 0) {
                append(",", 1);
            }
            append_str("\"file\":");
            append_json_str(frame->file, frame->file_len);
            append_str(",\"line\":");
            append_dec((long)frame->line);
        }
        append("}", 1);
        // Leave room for some callers.
        if (out_len > MAX_LINE - MAX_CLOSING - 1024) {
            out_len = len_without_frame;
            break;
        }
    }
    append_str("]");
    int omitted = php_state.count - shown + php_state.omitted;
    if (omitted > 0) {
        append_str(",\"php_frames_omitted\":");
        append_dec(omitted);
    }
}

static void report(int sig, siginfo_t *info, void *ucontext, uintptr_t pc)
{
    if (!lock_reports()) {
        return;
    }
    pid_t tid = current_tid();
    uint64_t now = monotonic_ns();
    if (tid == last_reported_tid && sig == last_reported_signal && pc == last_reported_pc &&
        now - last_reported_at < REFAULT_WINDOW_NS) {
        unlock_reports();
        return;
    }

    walk_state.count = 0;
    run_guarded(walk_stack, &walk_state);
    int frame_count = walk_state.count;
    if (frame_count > 0 && walk_state.frames[frame_count - 1] == NULL) {
        frame_count--;
    }

    php_state.context = 0;
    php_state.count = 0;
    php_state.omitted = 0;
    run_guarded(collect_php_frames, &php_state);

    struct crash crash = {
        .signal = sig,
        .code = info->si_code,
        .fault_addr = info->si_addr,
        .pc = pc,
        .frames = walk_state.frames,
        .frame_count = frame_count,
        .callers_start = find_callers_start(walk_state.frames, frame_count, pc),
    };
    crash.return_address = crash.callers_start < 0 ? context_return_address(ucontext) : 0;
    write_report(&crash, append_php_frames);

    last_reported_tid = tid;
    last_reported_signal = sig;
    last_reported_pc = pc;
    last_reported_at = now;
    unlock_reports();
}

static void on_signal(int sig, siginfo_t *info, void *ucontext)
{
    if (guarded_tid != 0 && guarded_tid == current_tid()) {
        siglongjmp(guard, 1);
    }
    uintptr_t pc = context_pc(ucontext);
    if (atomic_load(&armed) && !in_go_code(pc)) {
        report(sig, info, ucontext, pc);
    }
    if (!call_action(&previous_actions[sig], sig, info, ucontext)) {
        die_by_default(sig);
    }
}

struct executable {
    uintptr_t bias;
    uintptr_t header;
};

static int first_object(struct dl_phdr_info *info, size_t size, void *arg)
{
    (void)size;
    struct executable *exe = arg;
    exe->bias = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && ph->p_offset == 0) {
            exe->header = info->dlpi_addr + ph->p_vaddr;
            break;
        }
    }
    return 1;
}

static int read_at(int fd, void *buf, size_t len, off_t offset)
{
    return pread(fd, buf, len, offset) == (ssize_t)len;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

// The Go linker lays out runtime.pcHeader and runtime.moduledata as below
// (runtime/symtab.go). Only the leading fields that have not changed since
// Go 1.18 are used, and each is checked against the others.
struct pc_header {
    uint32_t magic;
    uint8_t pad1, pad2, min_lc, ptr_size;
    uintptr_t nfunc;
    uintptr_t nfiles;
    uintptr_t unused;
    uintptr_t funcname_offset;
};

struct go_slice {
    uintptr_t ptr, len, cap;
};

struct module_data {
    uintptr_t pc_header;
    struct go_slice funcnametab, cutab, filetab, pctab, pclntable, ftab;
    uintptr_t findfunctab;
    uintptr_t minpc, maxpc;
    uintptr_t text, etext;
};

static int module_data_matches(const struct module_data *md, uintptr_t header_addr)
{
    const struct pc_header *header = (const struct pc_header *)header_addr;
    return md->pc_header == header_addr && md->funcnametab.ptr == header_addr + header->funcname_offset &&
           md->ftab.len == header->nfunc + 1 && md->text != 0 && md->text <= md->minpc && md->minpc < md->maxpc &&
           md->maxpc <= md->etext;
}

// Looks for runtime.firstmoduledata in [start, start+size) and, if found,
// sets the range of Go code from it.
static int find_module_data(uintptr_t start, uintptr_t size, uintptr_t header_addr)
{
    uintptr_t first = (start + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    uintptr_t last = start + size - sizeof(struct module_data);
    for (uintptr_t p = first; p <= last; p += sizeof(uintptr_t)) {
        const struct module_data *md = (const struct module_data *)p;
        if (module_data_matches(md, header_addr)) {
            go_text_start = md->text;
            go_text_end = md->etext;
            return 1;
        }
    }
    return 0;
}

// Finds the code of the Go program, [runtime.text, runtime.etext), from the
// section headers of the executable: the pclntab section gives the address of
// runtime.pcHeader, and runtime.firstmoduledata, in a data section, is the
// one structure that points to it. This works on a stripped executable, which
// has section headers but no symbol table.
static int find_go_text(uintptr_t bias)
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    int found = 0;
    ElfW(Ehdr) eh;
    ElfW(Shdr) *sections = NULL;
    char *names = NULL;
    if (!read_at(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_shentsize != sizeof(ElfW(Shdr)) || eh.e_shnum == 0 || eh.e_shstrndx >= eh.e_shnum) {
        goto done;
    }
    sections = calloc(eh.e_shnum, sizeof *sections);
    if (!sections || !read_at(fd, sections, eh.e_shnum * sizeof *sections, (off_t)eh.e_shoff)) {
        goto done;
    }
    const ElfW(Shdr) *strtab = &sections[eh.e_shstrndx];
    names = calloc(1, strtab->sh_size + 1);
    if (!names || !read_at(fd, names, strtab->sh_size, (off_t)strtab->sh_offset)) {
        goto done;
    }

    uintptr_t header_addr = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        const ElfW(Shdr) *s = &sections[i];
        if (s->sh_name < strtab->sh_size && (s->sh_flags & SHF_ALLOC) && ends_with(names + s->sh_name, ".gopclntab") &&
            s->sh_size >= sizeof(struct pc_header)) {
            header_addr = bias + s->sh_addr;
            break;
        }
    }
    if (!header_addr) {
        goto done;
    }
    const struct pc_header *header = (const struct pc_header *)header_addr;
    if ((header->magic & 0xfffffff0) != 0xfffffff0 || header->pad1 != 0 || header->pad2 != 0 ||
        header->ptr_size != sizeof(uintptr_t)) {
        goto done;
    }

    // The Go linker puts runtime.firstmoduledata in .noptrdata; look there
    // first, then in every other writable section.
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (int i = 0; i < eh.e_shnum && !found; i++) {
            const ElfW(Shdr) *s = &sections[i];
            if (!(s->sh_flags & SHF_ALLOC) || !(s->sh_flags & SHF_WRITE) || s->sh_type == SHT_NOBITS ||
                s->sh_size < sizeof(struct module_data) || s->sh_name >= strtab->sh_size ||
                ends_with(names + s->sh_name, ".noptrdata") != (pass == 0)) {
                continue;
            }
            found = find_module_data(bias + s->sh_addr, s->sh_size, header_addr);
        }
    }

done:
    free(names);
    free(sections);
    close(fd);
    return found;
}

static int is_ours(const struct sigaction *action)
{
    return (action->sa_flags & SA_SIGINFO) && action->sa_sigaction == on_signal;
}

// The Go runtime installs its handler, which is Go code, with SA_SIGINFO and
// SA_ONSTACK for each of these signals. Anything else, such as the handlers of
// the Go module of this repository or of another extension, means the process
// is not what this extension expects.
static int is_go_handler(const struct sigaction *action)
{
    return (action->sa_flags & SA_SIGINFO) && (action->sa_flags & SA_ONSTACK) &&
           in_go_code((uintptr_t)action->sa_sigaction);
}

HIDDEN int crash_reporter_ext_arm(void)
{
    struct executable exe = {0};
    dl_iterate_phdr(first_object, &exe);
    if (!exe.header || memcmp(((const ElfW(Ehdr) *)exe.header)->e_ident, ELFMAG, SELFMAG) != 0) {
        return ERR_EXECUTABLE;
    }
    executable_base = exe.header;
    executable_is_position_independent = ((const ElfW(Ehdr) *)exe.header)->e_type != ET_EXEC;

    if (!find_go_text(exe.bias)) {
        return ERR_GO_TEXT;
    }

    struct sigaction current[COUNT(handled)];
    memset(current, 0, sizeof current);
    for (size_t i = 0; i < COUNT(handled); i++) {
        int sig = handled[i];
        if (installed[sig]) {
            continue;
        }
        if (sigaction(sig, NULL, &current[i]) != 0 || !is_go_handler(&current[i])) {
            // The Go module installs its handlers in front of the Go runtime's.
            const char *go_module = getenv("FRANKENPHP_CRASH_REPORTER");
            return go_module && strcmp(go_module, "true") == 0 ? ERR_GO_MODULE_ENABLED : ERR_GO_HANDLERS;
        }
    }

    // Keep the extension loaded after PHP unloads it: another handler may
    // have saved this one and call it later.
    Dl_info self;
    if (dladdr((void *)on_signal, &self) && self.dli_fname) {
        dlopen(self.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    }

    // Walk the stack once here, so that the unwinder does not initialize
    // itself inside the handler.
    walk_state.count = 0;
    walk_stack(&walk_state);

    for (size_t i = 0; i < COUNT(handled); i++) {
        int sig = handled[i];
        if (installed[sig]) {
            continue;
        }
        previous_actions[sig] = current[i];
        struct sigaction ours = current[i];
        ours.sa_sigaction = on_signal;
        ours.sa_flags |= SA_SIGINFO | SA_ONSTACK;
        if (sigaction(sig, &ours, NULL) != 0) {
            crash_reporter_ext_disarm();
            return ERR_INSTALL;
        }
        installed[sig] = 1;
    }
    atomic_store(&armed, 1);
    return ARMED;
}

HIDDEN void crash_reporter_ext_disarm(void)
{
    atomic_store(&armed, 0);
    for (size_t i = 0; i < COUNT(handled); i++) {
        int sig = handled[i];
        struct sigaction current;
        if (installed[sig] && sigaction(sig, NULL, &current) == 0 && is_ours(&current) &&
            sigaction(sig, &previous_actions[sig], NULL) == 0) {
            installed[sig] = 0;
        }
    }
}

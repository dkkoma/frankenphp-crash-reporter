//go:build linux && cgo && (amd64 || arm64)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <elf.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define TLS __thread __attribute__((tls_model("initial-exec")))
#define MAX_FRAMES 64
#define MAX_MODULES 16
#define MAX_LINE PIPE_BUF
#define MAX_BUILD_ID 64

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const int recoverable_in_go[] = {SIGSEGV, SIGBUS, SIGFPE};
static const int always_fatal[] = {SIGILL, SIGABRT};

enum { NOT_INSTALLED, INSTALLED, DISABLED };

static struct sigaction go_actions[NSIG];
static atomic_int state = NOT_INSTALLED;
static atomic_int reported;
static atomic_int report_written;
static atomic_int reporter_tid;

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

static char maps[1 << 20];
static size_t maps_len;
static char out[32768];
static size_t out_len;

static uintptr_t context_pc(void *ucontext)
{
    ucontext_t *uc = ucontext;
#if defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#else
    (void)uc;
    return 0;
#endif
}

static int is_readable(uintptr_t addr, size_t len)
{
    char buf[16];
    struct iovec local = {.iov_base = buf, .iov_len = len};
    struct iovec remote = {.iov_base = (void *)addr, .iov_len = len};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return n == (ssize_t)len || (n < 0 && errno != EFAULT);
}

static uintptr_t context_return_address(void *ucontext)
{
    ucontext_t *uc = ucontext;
#if defined(__x86_64__)
    uintptr_t value = 0;
    struct iovec local = {.iov_base = &value, .iov_len = sizeof value};
    struct iovec remote = {.iov_base = (void *)uc->uc_mcontext.gregs[REG_RSP], .iov_len = sizeof value};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof value) {
        return 0;
    }
    return value;
#elif defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.regs[30];
#else
    (void)uc;
    return 0;
#endif
}

static void append(const char *s, size_t n)
{
    if (out_len >= sizeof out - 1) {
        return;
    }
    size_t room = sizeof out - 1 - out_len;
    if (n > room) {
        n = room;
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void append_str(const char *s)
{
    append(s, strlen(s));
}

#define JSON_ESCAPED_MAX 6

static size_t json_escape(unsigned char c, char *escaped)
{
    if (c == '"' || c == '\\') {
        escaped[0] = '\\';
        escaped[1] = (char)c;
        return 2;
    }
    if (c < 0x20) {
        return 0;
    }
    if (c < 0x80) {
        escaped[0] = (char)c;
        return 1;
    }
    memcpy(escaped, "\\u00", 4);
    escaped[4] = "0123456789abcdef"[c >> 4];
    escaped[5] = "0123456789abcdef"[c & 0xf];
    return 6;
}

static void append_json_chars(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char escaped[JSON_ESCAPED_MAX];
        append(escaped, json_escape((unsigned char)s[i], escaped));
    }
}

static void append_json_str(const char *s, size_t n)
{
    append("\"", 1);
    append_json_chars(s, n);
    append("\"", 1);
}

static void append_dec(long v)
{
    char buf[24];
    int i = sizeof buf;
    int negative = v < 0;
    unsigned long u = negative ? -(unsigned long)v : (unsigned long)v;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (negative) {
        buf[--i] = '-';
    }
    append(buf + i, sizeof buf - i);
}

static void append_hex(uintptr_t v)
{
    char buf[2 + 2 * sizeof v];
    int i = sizeof buf;
    do {
        buf[--i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    append(buf + i, sizeof buf - i);
}

static void load_maps(void)
{
    maps_len = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    ssize_t n;
    while (maps_len < sizeof maps && (n = read(fd, maps + maps_len, sizeof maps - maps_len)) > 0) {
        maps_len += (size_t)n;
    }
    close(fd);
}

static const char *parse_hex(const char *p, const char *end, uintptr_t *v)
{
    *v = 0;
    for (; p < end; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            *v = *v << 4 | (uintptr_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            *v = *v << 4 | (uintptr_t)(c - 'a' + 10);
        } else {
            break;
        }
    }
    return p;
}

struct mapping {
    uintptr_t start;
    uintptr_t end;
    uintptr_t offset;
    const char *path;
    size_t path_len;
};

static const char *next_mapping(const char *p, struct mapping *m)
{
    const char *end = maps + maps_len;
    if (p >= end) {
        return NULL;
    }
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol) {
        eol = end;
    }
    const char *q = parse_hex(p, eol, &m->start);
    if (q < eol) {
        q++;
    }
    q = parse_hex(q, eol, &m->end);
    for (int field = 0; field < 2 && q < eol; field++) {
        while (q < eol && *q != ' ') {
            q++;
        }
        while (q < eol && *q == ' ') {
            q++;
        }
    }
    q = parse_hex(q, eol, &m->offset);
    for (int field = 0; field < 2 && q < eol; field++) {
        while (q < eol && *q == ' ') {
            q++;
        }
        while (q < eol && *q != ' ') {
            q++;
        }
    }
    while (q < eol && *q == ' ') {
        q++;
    }
    m->path = q;
    m->path_len = (size_t)(eol - q);
    return eol + 1;
}

struct module {
    const char *path;
    size_t path_len;
    uintptr_t base;
    uintptr_t base_end;
};

static int find_module(uintptr_t addr, struct module *mod)
{
    struct mapping m;
    const char *found_path = NULL;
    size_t found_len = 0;
    for (const char *p = maps; (p = next_mapping(p, &m));) {
        if (addr >= m.start && addr < m.end) {
            found_path = m.path;
            found_len = m.path_len;
            break;
        }
    }
    if (!found_path || found_len == 0 || found_path[0] != '/') {
        return 0;
    }
    int has_base = 0;
    for (const char *p = maps; (p = next_mapping(p, &m)) && m.start <= addr;) {
        if (m.offset == 0 && m.path_len == found_len && memcmp(m.path, found_path, found_len) == 0) {
            mod->path = found_path;
            mod->path_len = found_len;
            mod->base = m.start;
            mod->base_end = m.end;
            has_base = 1;
        }
    }
    return has_base;
}

static int in_header_mapping(const struct module *mod, uintptr_t addr, uint64_t len)
{
    return addr >= mod->base && addr <= mod->base_end && len <= mod->base_end - addr;
}

static const ElfW(Ehdr) *elf_header(const struct module *mod)
{
    if (!in_header_mapping(mod, mod->base, sizeof(ElfW(Ehdr)))) {
        return NULL;
    }
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)mod->base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        return NULL;
    }
    return eh;
}

static uintptr_t load_bias(const struct module *mod)
{
    const ElfW(Ehdr) *eh = elf_header(mod);
    return eh && eh->e_type == ET_EXEC ? 0 : mod->base;
}

static uint64_t note_align(uint64_t size)
{
    return (size + 3) & ~(uint64_t)3;
}

static const unsigned char *find_build_id(const struct module *mod, size_t *len)
{
    const ElfW(Ehdr) *eh = elf_header(mod);
    if (!eh || eh->e_phoff > mod->base_end - mod->base) {
        return NULL;
    }
    uintptr_t ph_addr = mod->base + eh->e_phoff;
    if (!in_header_mapping(mod, ph_addr, (uint64_t)eh->e_phnum * sizeof(ElfW(Phdr)))) {
        return NULL;
    }
    const ElfW(Phdr) *ph = (const ElfW(Phdr) *)ph_addr;
    uintptr_t bias = load_bias(mod);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_NOTE) {
            continue;
        }
        uintptr_t note = bias + ph[i].p_vaddr;
        if (!in_header_mapping(mod, note, ph[i].p_memsz)) {
            continue;
        }
        uint64_t left = ph[i].p_memsz;
        while (left >= sizeof(ElfW(Nhdr))) {
            const ElfW(Nhdr) *nh = (const ElfW(Nhdr) *)note;
            uint64_t name_size = note_align(nh->n_namesz);
            uint64_t desc_size = note_align(nh->n_descsz);
            left -= sizeof *nh;
            if (name_size > left || desc_size > left - name_size) {
                break;
            }
            const char *name = (const char *)(note + sizeof *nh);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                if (nh->n_descsz > MAX_BUILD_ID) {
                    return NULL;
                }
                *len = nh->n_descsz;
                return (const unsigned char *)name + name_size;
            }
            note += sizeof *nh + name_size + desc_size;
            left -= name_size + desc_size;
        }
    }
    return NULL;
}

static void append_build_id(const struct module *mod)
{
    size_t len;
    const unsigned char *id = find_build_id(mod, &len);
    if (!id) {
        append_str("null");
        return;
    }
    append("\"", 1);
    for (size_t i = 0; i < len; i++) {
        append(&"0123456789abcdef"[id[i] >> 4], 1);
        append(&"0123456789abcdef"[id[i] & 0xf], 1);
    }
    append("\"", 1);
}

struct location {
    struct module module;
    uintptr_t value;
};

static struct location pc_location;
static struct location return_location;
static struct location caller_locations[MAX_FRAMES];
static int caller_count;

static void locate(uintptr_t addr, struct location *loc)
{
    if (find_module(addr, &loc->module)) {
        loc->value = addr - load_bias(&loc->module);
    } else {
        loc->module.path = NULL;
        loc->value = addr;
    }
}

static void append_location(const struct location *loc, struct module *modules, int *module_count)
{
    append("\"", 1);
    if (loc->module.path) {
        append_json_chars(loc->module.path, loc->module.path_len);
        append("+", 1);
    }
    append_hex(loc->value);
    append("\"", 1);
    if (!loc->module.path) {
        return;
    }
    for (int i = 0; i < *module_count; i++) {
        if (modules[i].base == loc->module.base) {
            return;
        }
    }
    if (*module_count < MAX_MODULES) {
        modules[(*module_count)++] = loc->module;
    }
}

static void write_all(const char *buf, size_t len)
{
    int waits_left = 10;
    for (size_t written = 0; written < len;) {
        ssize_t n = write(STDERR_FILENO, buf + written, len - written);
        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == EAGAIN && waits_left-- > 0) {
            struct pollfd fd = {.fd = STDERR_FILENO, .events = POLLOUT};
            poll(&fd, 1, 100);
        } else {
            return;
        }
    }
}

static void wait_for_report_written(void)
{
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
    for (int i = 0; i < 100 && !atomic_load(&report_written); i++) {
        nanosleep(&interval, NULL);
    }
}

static void format_report(const char *thread_name, int callers_shown)
{
    struct module modules[MAX_MODULES];
    int module_count = 0;
    out_len = 0;

    append_str("{\"severity\":\"CRITICAL\",\"message\":\"fatal signal\",\"signal\":");
    append_dec(first_signal);
    append_str(",\"code\":");
    append_dec(first_code);
    append_str(",\"fault_addr\":");
    if (first_code > 0) {
        append("\"", 1);
        append_hex((uintptr_t)first_fault_addr);
        append("\"", 1);
    } else {
        append_str("null");
    }
    append_str(",\"process\":");
    append_json_str(program_invocation_short_name, strlen(program_invocation_short_name));
    append_str(",\"tid\":");
    append_dec(atomic_load(&reporter_tid));
    append_str(",\"thread\":");
    append_json_str(thread_name, strlen(thread_name));
    append_str(",\"pc\":");
    append_location(&pc_location, modules, &module_count);
    if (first_callers_start < 0) {
        append_str(",\"return_address\":");
        if (first_return_address) {
            append_location(&return_location, modules, &module_count);
        } else {
            append_str("null");
        }
    }
    append_str(",\"callers\":[");
    for (int i = 0; i < callers_shown; i++) {
        if (i > 0) {
            append(",", 1);
        }
        append_location(&caller_locations[i], modules, &module_count);
    }
    append_str("]");
    if (callers_shown < caller_count) {
        append_str(",\"callers_omitted\":");
        append_dec(caller_count - callers_shown);
    }
    append_str(",\"build_ids\":{");
    for (int i = 0; i < module_count; i++) {
        if (i > 0) {
            append(",", 1);
        }
        append_json_str(modules[i].path, modules[i].path_len);
        append(":", 1);
        append_build_id(&modules[i]);
    }
    append_str("}}");
    out[out_len++] = '\n';
}

static void report(void)
{
    int tid = (int)syscall(SYS_gettid);
    if (atomic_exchange(&reported, 1)) {
        if (atomic_load(&reporter_tid) != tid) {
            wait_for_report_written();
        }
        return;
    }
    atomic_store(&reporter_tid, tid);
    load_maps();

    char thread_name[17] = {0};
    prctl(PR_GET_NAME, thread_name, 0, 0, 0);

    locate(first_pc, &pc_location);
    locate(first_return_address, &return_location);
    caller_count = 0;
    for (int i = first_callers_start < 0 ? first_frame_count : first_callers_start; i < first_frame_count; i++) {
        locate((uintptr_t)first_frames[i], &caller_locations[caller_count++]);
    }

    int callers_shown = caller_count;
    format_report(thread_name, callers_shown);
    while (out_len > MAX_LINE && callers_shown > 0) {
        format_report(thread_name, --callers_shown);
    }

    write_all(out, out_len);
    atomic_store(&report_written, 1);
}

static void capture(int sig, siginfo_t *info, void *ucontext)
{
    first_signal = sig;
    first_code = info->si_code;
    first_fault_addr = info->si_addr;
    first_pc = context_pc(ucontext);
    first_frame_count = is_readable(first_pc, 16) ? backtrace(first_frames, MAX_FRAMES) : 0;
    first_callers_start = -1;
    for (int i = 0; i + 1 < first_frame_count; i++) {
        if ((uintptr_t)first_frames[i] == first_pc) {
            first_callers_start = i + 1;
            break;
        }
    }
    first_return_address = first_callers_start < 0 ? context_return_address(ucontext) : 0;
}

static void die_by_default(int sig)
{
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    raise(sig);
    kill(getpid(), sig);
    struct timespec grace = {.tv_sec = 1, .tv_nsec = 0};
    nanosleep(&grace, NULL);
    _exit(128 + sig);
}

static void report_non_go_fault(int sig, siginfo_t *info, void *ucontext)
{
    if (handed_back_to_go) {
        die_by_default(sig);
    }
    if (atomic_load(&state) != DISABLED) {
        if (!captured) {
            capture(sig, info, ucontext);
        }
        report();
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
    for (size_t i = 0; i < COUNT(recoverable_in_go); i++) {
        struct sigaction current;
        if (sigaction(recoverable_in_go[i], NULL, &current) != 0 || (current.sa_flags & SA_SIGINFO) ||
            current.sa_handler != SIG_DFL) {
            return;
        }
    }
    void *warm_up[1];
    backtrace(warm_up, 1);
    for (size_t i = 0; i < COUNT(recoverable_in_go); i++) {
        struct sigaction ours;
        memset(&ours, 0, sizeof ours);
        ours.sa_sigaction = report_non_go_fault;
        ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigfillset(&ours.sa_mask);
        if (sigaction(recoverable_in_go[i], &ours, NULL) != 0) {
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

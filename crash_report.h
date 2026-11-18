// Code shared by the Go module (reporter.c) and the PHP extension (ext/):
// resolving addresses to modules, formatting the report line and writing it.
// Everything here is async-signal-safe. The buffers are shared, so a caller
// formats and writes a report only while it holds the lock (lock_reports).

#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
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

#define MAX_FRAMES 64
#define MAX_NAME 256
#define MAX_LINE PIPE_BUF
#define MAX_CLOSING 32

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SHARED static __attribute__((unused))

static atomic_flag reporting = ATOMIC_FLAG_INIT;

// Address of the ELF header of the executable, and whether the executable is
// position-independent. Set once at install time.
static uintptr_t executable_base;
static int executable_is_position_independent;

static char maps[1 << 20];
static size_t maps_len;
static char out[32768];
static size_t out_len;

SHARED uintptr_t context_pc(void *ucontext)
{
    ucontext_t *uc = ucontext;
#if defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#endif
}

SHARED uintptr_t context_return_address(void *ucontext)
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
#endif
}

SHARED void append(const char *s, size_t n)
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

SHARED void append_str(const char *s)
{
    append(s, strlen(s));
}

SHARED void append_json_chars(const char *s, size_t n)
{
    size_t start = n > MAX_NAME ? n - MAX_NAME : 0;
    if (start > 0) {
        append_str("...");
    }
    for (size_t i = start; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char safe = c < 0x20 || c >= 0x7f || c == '"' || c == '\\' ? '?' : (char)c;
        append(&safe, 1);
    }
}

SHARED void append_json_str(const char *s, size_t n)
{
    append("\"", 1);
    append_json_chars(s, n);
    append("\"", 1);
}

SHARED void append_dec(long v)
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

SHARED void append_hex(uintptr_t v)
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

SHARED void load_maps(void)
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

SHARED const char *parse_hex(const char *p, const char *end, uintptr_t *v)
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

SHARED const char *next_mapping(const char *p, struct mapping *m)
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
};

SHARED int find_module(uintptr_t addr, struct module *mod)
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
            has_base = 1;
        }
    }
    return has_base;
}

struct location {
    struct module module;
    uintptr_t value;
};

static struct location pc_location;
static struct location return_location;
static struct location caller_locations[MAX_FRAMES];
static int caller_count;

SHARED void locate(uintptr_t addr, struct location *loc)
{
    if (find_module(addr, &loc->module)) {
        int at_link_address = !executable_is_position_independent && loc->module.base == executable_base;
        loc->value = at_link_address ? addr : addr - loc->module.base;
    } else {
        loc->module.path = NULL;
        loc->value = addr;
    }
}

SHARED void append_location(const struct location *loc)
{
    append("\"", 1);
    if (loc->module.path) {
        append_json_chars(loc->module.path, loc->module.path_len);
        append("+", 1);
    }
    append_hex(loc->value);
    append("\"", 1);
}

SHARED void write_all(const char *buf, size_t len)
{
    size_t written = 0;
    for (int attempts = 10; written < len && attempts > 0; attempts--) {
        struct pollfd fd = {.fd = STDERR_FILENO, .events = POLLOUT};
        if (poll(&fd, 1, 100) <= 0) {
            continue;
        }
        ssize_t n = write(STDERR_FILENO, buf + written, len - written);
        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
            return;
        }
    }
}

// What a report says about one signal. frames are the return addresses walked
// from inside the handler; callers_start is the index of the first caller of
// pc among them, or -1 when pc is not among them.
struct crash {
    int signal;
    int code;
    void *fault_addr;
    uintptr_t pc;
    uintptr_t return_address;
    void *const *frames;
    int frame_count;
    int callers_start;
};

SHARED int find_callers_start(void *const *frames, int frame_count, uintptr_t pc)
{
    for (int i = 0; i + 1 < frame_count; i++) {
        if ((uintptr_t)frames[i] == pc) {
            return i + 1;
        }
    }
    return -1;
}

// append_extra, if not NULL, adds fields between pc (or return_address) and
// callers. It must leave room for the callers and the closing characters.
SHARED void format_report(const struct crash *crash, const char *thread_name, void (*append_extra)(void))
{
    out_len = 0;

    append_str("{\"severity\":\"CRITICAL\",\"message\":\"fatal signal\",\"signal\":");
    append_dec(crash->signal);
    append_str(",\"code\":");
    append_dec(crash->code);
    append_str(",\"fault_addr\":");
    if (crash->code > 0) {
        append("\"", 1);
        append_hex((uintptr_t)crash->fault_addr);
        append("\"", 1);
    } else {
        append_str("null");
    }
    append_str(",\"tid\":");
    append_dec(syscall(SYS_gettid));
    append_str(",\"thread\":");
    append_json_str(thread_name, strlen(thread_name));
    append_str(",\"pc\":");
    append_location(&pc_location);
    if (crash->callers_start < 0) {
        append_str(",\"return_address\":");
        if (crash->return_address) {
            append_location(&return_location);
        } else {
            append_str("null");
        }
    }
    if (append_extra) {
        append_extra();
    }
    append_str(",\"callers\":[");
    int callers_shown = 0;
    for (; callers_shown < caller_count; callers_shown++) {
        size_t len_without_caller = out_len;
        if (callers_shown > 0) {
            append(",", 1);
        }
        append_location(&caller_locations[callers_shown]);
        if (out_len > MAX_LINE - MAX_CLOSING) {
            out_len = len_without_caller;
            break;
        }
    }
    append_str("]");
    if (callers_shown < caller_count) {
        append_str(",\"callers_omitted\":");
        append_dec(caller_count - callers_shown);
    }
    append_str("}");
    out[out_len++] = '\n';
}

// Serializes reports. Gives up after about a second.
SHARED int lock_reports(void)
{
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
    for (int waits = 0; atomic_flag_test_and_set(&reporting); waits++) {
        if (waits == 100) {
            return 0;
        }
        nanosleep(&interval, NULL);
    }
    return 1;
}

SHARED void unlock_reports(void)
{
    atomic_flag_clear(&reporting);
}

// Formats and writes the line. The caller holds the lock.
SHARED void write_report(const struct crash *crash, void (*append_extra)(void))
{
    load_maps();

    char thread_name[17] = {0};
    prctl(PR_GET_NAME, thread_name, 0, 0, 0);

    locate(crash->pc, &pc_location);
    locate(crash->return_address, &return_location);
    caller_count = 0;
    for (int i = crash->callers_start < 0 ? crash->frame_count : crash->callers_start; i < crash->frame_count; i++) {
        locate((uintptr_t)crash->frames[i], &caller_locations[caller_count++]);
    }

    format_report(crash, thread_name, append_extra);

    write_all(out, out_len);
}

SHARED void die_by_default(int sig)
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

#endif

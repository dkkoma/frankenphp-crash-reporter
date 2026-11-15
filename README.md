# frankenphp-crash-reporter

A Go module that makes [FrankenPHP](https://frankenphp.dev/) print one JSON line
to stderr when the process is about to die from a fatal signal (SIGSEGV, SIGBUS,
SIGFPE, SIGILL, SIGABRT) raised outside Go code — in PHP, in an extension, or in
any other C code running in the process.

## Motivation

PHP runs on threads that FrankenPHP creates in C. When one of them crashes, the
Go runtime does not treat the signal as its own: it restores the default action
and raises the signal again. The process dies with no output at all, and the
only trace left is the exit status (for example 139 for SIGSEGV).

With this module linked in and enabled, the same crash leaves a line like this
before the process dies:

```json
{"severity":"CRITICAL","message":"fatal signal","signal":11,"code":1,"fault_addr":"0x0","tid":64,"thread":"php-14","pc":"/usr/local/lib/libphp.so+0x6c75a8","callers":["/usr/local/lib/libphp.so+0xa2a7db","/usr/local/bin/frankenphp+0x19d5d82","/usr/lib/x86_64-linux-gnu/libc.so.6+0x928bb"]}
```

The way the process ends (signal, exit status, Go's own crash output) is
unchanged, except when FrankenPHP runs as PID 1 (see
[FrankenPHP as PID 1](#frankenphp-as-pid-1)).

## Requirements

- Linux, amd64 or arm64
- glibc 2.35 or later, with libgcc from GCC 12 or later. With older versions
  the stack walk takes a lock of the dynamic loader inside the signal handler.
  musl is not tested
- cgo, which a FrankenPHP build already requires

## Tested FrankenPHP versions

[![Test](https://github.com/dkkoma/frankenphp-crash-reporter/actions/workflows/test.yml/badge.svg)](https://github.com/dkkoma/frankenphp-crash-reporter/actions/workflows/test.yml)

The [test workflow](.github/workflows/test.yml) runs every day against the
latest FrankenPHP 1.x release, on amd64 and arm64. The summary of each run shows
the version it tested. Older releases are not tested.

The reporter depends on how the Go runtime and FrankenPHP handle signals, so a
release the workflow has not passed on may behave differently.

## Installation

Add the module when building FrankenPHP with
[xcaddy](https://github.com/caddyserver/xcaddy):

```sh
xcaddy build \
    --output frankenphp \
    --with github.com/dunglas/frankenphp/caddy \
    --with github.com/dkkoma/frankenphp-crash-reporter
```

There is no Caddyfile directive and no Caddy module to configure. The package
only needs to be linked, so a blank import works as well:

```go
import _ "github.com/dkkoma/frankenphp-crash-reporter"
```

## Usage

Start FrankenPHP with `FRANKENPHP_CRASH_REPORTER=true`. Any other value, or no
value, leaves the reporter off and the process untouched.

At startup the reporter prints one line that tells whether it is active:

```json
{"severity":"INFO","message":"crash reporter armed"}
```

If its startup checks fail, it stays off and says why:

```json
{"severity":"WARNING","message":"crash reporter not armed: ..."}
```

## Output

The reporter prints nothing while the process runs normally. On a fatal signal
outside Go code it writes one line. When several threads crash at the same
time, only the first one is reported.

| Field | Meaning |
|---|---|
| `signal` | Signal number (11 SIGSEGV, 7 SIGBUS, 8 SIGFPE, 4 SIGILL, 6 SIGABRT) |
| `code` | `si_code`. Positive values come from the kernel (a bad memory access, for example). Zero or negative values mean the signal was sent with `raise` / `kill`; `abort()` gives -6 |
| `fault_addr` | For SIGSEGV and SIGBUS, the address the instruction tried to access. For SIGILL and SIGFPE, the address of the instruction. `null` when `code` is zero or negative. On amd64 an access to a non-canonical address (`0x4141414141414141`, for example) gives `code` 128 and `0x0` here, which is not a NULL dereference |
| `tid` / `thread` | ID and name of the crashed thread. FrankenPHP names its PHP threads `php-N` |
| `pc` | Where the crashed instruction is, as `module path+address`. The address is the one `addr2line` takes: the offset from the start of the module for a shared library or a position-independent executable, the virtual address for any other executable |
| `return_address` | Present only when the callers cannot be walked from `pc`. It is the value at the top of the stack on amd64 and the link register on arm64, or `null` if it could not be read. After a call through a NULL or corrupted function pointer, that is the return address of the call. In the other cases (code without unwind information such as JIT-compiled code, or a stack that cannot be walked) it is only a hint and may not be a return address at all |
| `callers` | Return addresses of the calling frames, nearest first, in the same form as `pc`. Empty when they cannot be walked from `pc` |
| `callers_omitted` | Present only when callers were dropped from the far end to keep the line within 4096 bytes |

The line is at most 4096 bytes (`PIPE_BUF`) and is written with a single
`write(2)`, so it does not interleave with log lines from other threads when
stderr is a pipe.

If stderr does not become writable within about a second (a full pipe that
nobody reads, for example), the line is dropped and the process ends without
it.

At most 64 frames of the stack are walked, so `callers` never holds more than
that, and `callers_omitted` does not count the frames beyond them. When the
stack of the crashed thread is corrupted, `callers` holds the frames that
could be read before the corrupted part.

In a module path or a thread name, a `"`, a `\`, a control
character and a byte outside ASCII are each written as `?`. A path longer
than 256 bytes is cut at the front and starts with `...`.

## Finding the crash site

1. Get the module named in `pc` from the exact build the crashed process ran,
   with debug information. The offsets mean nothing for any other build, and
   the report does not say which build it was: take it from the deployment
   (for a container, the image digest rather than a tag).

2. Pass the offsets to `addr2line`. `callers` and `return_address` are return
   addresses, so subtract 1 to get the line of the call. Pass `pc` as it is.

   ```sh
   addr2line -f -i -C -e libphp.so 0x6c75a8 0xa2a7da
   ```

   ```
   zif_posix_getpwnam
   /usr/src/php/ext/posix/posix.c:986
   ZEND_DO_ICALL_SPEC_RETVAL_UNUSED_HANDLER
   /usr/src/php/Zend/zend_vm_execute.h:1360
   ```

When the crash is inside PHP's memory allocator (`zend_mm_*`), the heap was
usually corrupted earlier, and the crash site is not necessarily the cause.

## How it works

The reporter never decides by itself whether a signal hit Go code. It relies
on two behaviors of the Go runtime:

- If a handler for SIGSEGV, SIGBUS or SIGFPE is already installed when the Go
  runtime starts, the runtime saves it and calls it when one of these signals
  is raised by non-Go code. Signals raised by Go code become panics as usual.
  The `os/signal` documentation describes this under
  ["Non-Go programs that call Go code"](https://pkg.go.dev/os/signal#hdr-Non_Go_programs_that_call_Go_code),
  and it applies in the same way to a handler installed by a C constructor
  of a cgo program.
- A SIGSEGV, SIGBUS or SIGFPE sent with `kill` is not treated as a program
  error, so the runtime handles it itself. This is not documented; it is how
  the runtime is implemented (`sigfwdgo` in `runtime/signal_unix.go`).

The tests run against the latest FrankenPHP every day, so a change in either
behavior shows up there.

**SIGSEGV, SIGBUS, SIGFPE.** A C constructor installs the reporting handler
before the Go runtime starts. When the package is initialized, a second handler
is placed in front of the Go one. It only records where the signal happened and
calls the Go handler:

- Go code: the Go handler turns the signal into a panic. Nothing is printed and
  the panic stays recoverable.
- Non-Go code: the Go handler calls the reporting handler, which writes the
  line and then calls the Go handler again with the signal marked as sent by
  `kill`. The runtime now handles it itself and ends the process the way it
  does without the reporter.

If a handler for one of these signals was already installed when the program
started (by a preloaded library, for example), the reporting handler calls it
after writing the line instead of going back to the Go handler. That is the
handler the Go runtime would have called without the reporter.

**SIGILL, SIGABRT.** The Go runtime never recovers from these, so no decision is
needed. A handler placed in front of the Go one writes the line and calls the
Go handler.

The handlers do not allocate, take locks, or call `dladdr`. Modules are
resolved from `/proc/self/maps`. The stack is walked once at install time so that the
lazy initialization of the unwinder does not happen inside a handler.

| Crash | Report | How the process ends |
|---|---|---|
| C code on a PHP thread or any other thread not created by Go | yes | default action of the signal (exit status 128 + signal) |
| C code called from a goroutine | yes | Go's fatal error output with the goroutine traceback, exit status 2. The output shows `sigcode=0` instead of the code of the fault |
| Go code (recoverable panic) | no | keeps running |
| Unrecovered Go panic | no | Go's panic output |
| Unrecovered Go panic with `GOTRACEBACK=crash` | yes, for the SIGABRT Go sends to itself | Go's panic output, then SIGABRT |

## FrankenPHP as PID 1

When FrankenPHP is PID 1 of a container (there is no init process in front of
it), the kernel discards a signal the process sends to itself while the action
of that signal is the default one. The Go runtime ends the process that way,
so the outcome of a crash outside Go code differs from the rest of this
document:

| Crash | Without the reporter | With the reporter |
|---|---|---|
| SIGSEGV, SIGBUS, SIGFPE | The process does not end. The crashed thread repeats the faulting instruction, its request hangs, and the other threads keep serving | The line is printed, and about a second later the process exits with status 128 + signal number. It is an exit, not a death by signal, so there is no core dump |
| `abort()` | The process ends, but not by SIGABRT: glibc falls back to an instruction that raises another fatal signal | The line is printed, and the process ends the same way |
| SIGILL | The process does not end, as above | The line is printed once, and the process does not end |

With an init process (`docker run --init`, for example) FrankenPHP is not
PID 1 and the process ends the same way with and without the reporter.

## Limitations

- SIGKILL cannot be caught, so an out-of-memory kill leaves no line.
- A stack overflow on a thread that has no alternate signal stack leaves no
  line, because the handler has no stack to run on. The Go runtime gives an
  alternate signal stack to every thread that has called into Go, which
  includes FrankenPHP's PHP threads. A thread created in C that never calls
  into Go has none unless it sets one up.
- If a handler that was installed before the program started returns instead
  of ending the process, the line is printed although the process keeps
  running. A later fault is reported again, unless it is the same signal at
  the same instruction on the same thread as the last one reported there.
  With such a handler, threads that crash at the same time may each be
  reported, and their lines may be mixed up.
- After the process survives a signal that was sent with `kill` to a thread
  not created by Go, the Go runtime puts its own handler back in front of
  this one for that signal. From then on, a report for SIGSEGV, SIGBUS or
  SIGFPE has no `callers`, and SIGILL and SIGABRT are not reported.
- If another library installs a handler for the same signals after the Go
  runtime has started, it replaces both the Go handler and this one.
- If the program subscribes to SIGSEGV, SIGBUS or SIGFPE with
  `os/signal.Notify`, or ignores them with `os/signal.Ignore`, the process ends
  differently once the reporter is enabled. A fault in C code called from a
  goroutine ends with the default action of the signal, without Go's
  traceback. With `os/signal.Notify`, a fault on a thread not created by Go
  also ends with the default action; without the reporter that thread faults
  in a loop and the process keeps running.
- When Go code crashes in a way the runtime does not recover from, only the
  runtime's own output is printed, except for SIGILL and SIGABRT.

## Testing

The test builds FrankenPHP with this module, crashes it in several ways, and
checks the output and the exit status. It needs only Docker:

```sh
docker build --pull --progress plain -t crash-reporter-test -f test/Dockerfile .
sh test/pid1.sh crash-reporter-test
```

The second command crashes FrankenPHP running as PID 1 of a container.

This tests the latest FrankenPHP 1.x release. To test another one, pass
`--build-arg FRANKENPHP_VERSION=<version>`.

The workflow builds the reporter both with `-O0` and with `-O2`
(`--build-arg CGO_OPTIMIZATION=-O0`); the default of the Dockerfile is `-O2`.

The `testhooks` directory is a separate Go module that exposes HTTP endpoints
to crash the process from Go and from C code inside the FrankenPHP binary. It is
linked only into the test binary.

## License

MIT

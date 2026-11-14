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
{"severity":"CRITICAL","message":"fatal signal","signal":11,"code":1,"fault_addr":"0x0","process":"frankenphp","tid":64,"thread":"php-14","pc":"/usr/local/lib/libphp.so+0x6c75a8","callers":["/usr/local/lib/libphp.so+0xa2a7db","/usr/local/bin/frankenphp+0x19d5d82","/usr/lib/x86_64-linux-gnu/libc.so.6+0x928bb"],"build_ids":{"/usr/local/lib/libphp.so":"89ae9f66750b0eb02f6d054072e8ece9474d669e","/usr/local/bin/frankenphp":"...","/usr/lib/x86_64-linux-gnu/libc.so.6":"..."}}
```

The way the process ends (signal, exit status, Go's own crash output) is
unchanged.

## Requirements

- Linux, amd64 or arm64
- glibc (the module uses `backtrace()` from `<execinfo.h>`, which musl does not provide)
- cgo, which a FrankenPHP build already requires

## Tested FrankenPHP versions

| FrankenPHP | amd64 | arm64 |
|---|---|---|
| 1.13.5 | ✅ | ✅ |

These are the versions the [test workflow](.github/workflows/test.yml) runs
against. The reporter depends on how the Go runtime and FrankenPHP handle
signals, so a version that is not listed may behave differently.

On other platforms the package compiles to nothing.

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
{"severity":"INFO","message":"crash reporter armed","process":"frankenphp"}
```

If its startup checks fail, it stays off and says why:

```json
{"severity":"WARNING","message":"crash reporter not armed: ...","process":"frankenphp"}
```

## Output

The reporter prints nothing while the process runs normally. On a fatal signal
outside Go code it writes one line. When several threads crash at the same
time, only the first one is reported.

| Field | Meaning |
|---|---|
| `signal` | Signal number (11 SIGSEGV, 7 SIGBUS, 8 SIGFPE, 4 SIGILL, 6 SIGABRT) |
| `code` | `si_code`. Positive values come from the kernel (a bad memory access, for example). Zero or negative values mean the signal was sent with `raise` / `kill`; `abort()` gives -6 |
| `fault_addr` | The address the instruction tried to access. `null` when `code` is zero or negative |
| `process` | Name of the process |
| `tid` / `thread` | ID and name of the crashed thread. FrankenPHP names its PHP threads `php-N` |
| `pc` | Where the crashed instruction is, as `module path+offset from the module start` |
| `return_address` | Present only when the callers cannot be walked from `pc`, which happens after a call through a NULL or corrupted function pointer. It is the return address of that call (top of the stack on amd64, the link register on arm64), or `null` if it could not be read |
| `callers` | Return addresses of the calling frames, nearest first, in the same form as `pc`. Empty when they cannot be walked from `pc` |
| `callers_omitted` | Present only when callers were dropped from the far end to keep the line within 4096 bytes |
| `build_ids` | GNU build ID of every module that appears in `pc`, `return_address` and `callers` |

The line is at most 4096 bytes (`PIPE_BUF`) and is written with a single
`write(2)`, so it does not interleave with log lines from other threads when
stderr is a pipe.

## Finding the crash site

1. Get a copy of the module named in `pc` that has debug information, and check
   that its build ID matches the one in `build_ids`:

   ```sh
   readelf -n libphp.so | grep 'Build ID'
   ```

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
on two behaviors of the Go runtime that are documented for cgo programs
(see ["Go programs that use cgo or SWIG"](https://pkg.go.dev/os/signal#hdr-Go_programs_that_use_cgo_or_SWIG)):

- If a handler for SIGSEGV, SIGBUS or SIGFPE is already installed when the Go
  runtime starts, the runtime saves it and calls it when one of these signals
  is raised by non-Go code. Signals raised by Go code become panics as usual.
- A SIGSEGV, SIGBUS or SIGFPE sent with `kill` is not treated as a program
  error, so the runtime handles it itself.

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
resolved from `/proc/self/maps`, and build IDs are read from the ELF headers
already mapped in memory. `backtrace()` is called once at install time so that
its lazy initialization does not happen inside a handler.

| Crash | Report | How the process ends |
|---|---|---|
| C code on a PHP thread or any other thread not created by Go | yes | default action of the signal (exit status 128 + signal) |
| C code called from a goroutine | yes | Go's fatal error output with the goroutine traceback, exit status 2 |
| Go code (recoverable panic) | no | keeps running |
| Unrecovered Go panic | no | Go's panic output |
| Unrecovered Go panic with `GOTRACEBACK=crash` | yes, for the SIGABRT Go sends to itself | Go's panic output, then SIGABRT |

## Limitations

- SIGKILL cannot be caught, so an out-of-memory kill leaves no line.
- A stack overflow on a thread that has no alternate signal stack leaves no
  line, because the handler has no stack to run on. PHP threads are such
  threads.
- If a handler that was installed before the program started recovers from a
  fault instead of ending the process, the line is printed although the
  process keeps running. A later fault is reported again.
- If another library installs a handler for the same signals after the Go
  runtime has started, it replaces both the Go handler and this one.
- If the program subscribes to SIGSEGV, SIGBUS or SIGFPE with
  `os/signal.Notify`, or ignores them with `os/signal.Ignore`, the process ends
  differently once the reporter is enabled. A fault in C code called from a
  goroutine ends with the default action of the signal, without Go's
  traceback. A fault on a thread not created by Go also ends with the default
  action; without the reporter that thread faults in a loop and the process
  keeps running.
- When Go code crashes in a way the runtime does not recover from, only the
  runtime's own output is printed, except for SIGILL and SIGABRT.

## Testing

The test builds FrankenPHP with this module, crashes it in several ways, and
checks the output and the exit status. It needs only Docker:

```sh
docker build --progress plain -f test/Dockerfile .
```

To test against another FrankenPHP version, pass
`--build-arg FRANKENPHP_VERSION=<version>`.

The `testhooks` directory is a separate Go module that exposes HTTP endpoints
to crash the process from Go and from C code inside the FrankenPHP binary. It is
linked only into the test binary.

## License

MIT

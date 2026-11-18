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

The reporter only adds this line. The process ends the way it does without
the reporter: by the same signal, with the same exit status.

When FrankenPHP runs as PID 1 of a container, a crash ends differently, with
and without the reporter; see [FrankenPHP as PID 1](#frankenphp-as-pid-1).
A few uncommon setups are covered under [Special cases](#special-cases).

The same reporter is also available as an experimental PHP extension, which
needs no custom FrankenPHP build and adds the PHP call stack to the line; see
[PHP extension (experimental)](#php-extension-experimental).

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
outside Go code it writes one line.

| Field | Meaning |
|---|---|
| `signal` | Signal number (11 SIGSEGV, 7 SIGBUS, 8 SIGFPE, 4 SIGILL, 6 SIGABRT) |
| `code` | `si_code`. Positive values come from the kernel (a bad memory access, for example). Zero or negative values mean the signal was sent with `raise` / `kill`; `abort()` gives -6 |
| `fault_addr` | For SIGSEGV and SIGBUS, the address the instruction tried to access. For SIGILL and SIGFPE, the address of the instruction. `null` when `code` is zero or negative |
| `tid` / `thread` | ID and name of the crashed thread. FrankenPHP names its PHP threads `php-N` |
| `pc` | Where the crashed instruction is, as `module path+address`. The address is the one `addr2line` takes |
| `callers` | Return addresses of the calling frames, nearest first, in the same form as `pc` |
| `return_address` | Present only when the callers cannot be walked from `pc`; `callers` is then empty. See below |
| `callers_omitted` | Present only when callers were dropped from the far end to keep the line within 4096 bytes |

The line is at most 4096 bytes (`PIPE_BUF`) and is written with a single
`write(2)`, so it does not interleave with log lines from other threads when
stderr is a pipe.

Details that matter when reading a line:

- **`fault_addr` of `0x0` with `code` 128** is not a NULL dereference. On
  amd64 the kernel reports an access to a non-canonical address
  (`0x4141414141414141`, for example) this way.
- **`pc` of the FrankenPHP binary** is a virtual address unless the binary was
  built as a position-independent executable. For a shared library, and for a
  position-independent executable, it is the offset from the start of the
  module. Either way it is what `addr2line` takes.
- **`return_address`** is the value at the top of the stack on amd64 and the
  link register on arm64, or `null` if it could not be read. After a call
  through a NULL or corrupted function pointer, it is the return address of
  that call. In the other cases (code without unwind information such as
  JIT-compiled code, or a stack that cannot be walked) it is only a hint and
  may not be a return address at all.
- **`callers`** holds at most 64 frames, and `callers_omitted` does not count
  the frames beyond them. When the stack of the crashed thread is corrupted,
  it holds the frames that could be read before the corrupted part.
- **Module paths and thread names**: a `"`, a `\`, a control character and a
  byte outside ASCII are each written as `?`. A path longer than 256 bytes is
  cut at the front and starts with `...`.
- **Several threads crashing at the same time** write their lines one after
  another. The first thread usually ends the process before the others get to
  write, so expect one line.

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

**SIGILL, SIGABRT.** The Go runtime does not turn these into panics, so no
decision is needed. A handler placed in front of the Go one writes the line
and calls the Go handler.

The handlers do not allocate or call `dladdr`. The only lock they take is
their own, which serializes the reports, and a thread waits for it for about
a second at most. Modules are resolved from `/proc/self/maps`. The stack is
walked once at install time so that the lazy initialization of the unwinder
does not happen inside a handler.

| Crash | Report | How the process ends |
|---|---|---|
| C code on a PHP thread or any other thread not created by Go | yes | default action of the signal (exit status 128 + signal) |
| C code called from a goroutine | yes | Go's fatal error output with the goroutine traceback, exit status 2. The output shows `sigcode=0` instead of the code of the fault |
| Go code, recovered panic | no | keeps running |
| Go code, unrecovered panic | no | Go's panic output |

## FrankenPHP as PID 1

When FrankenPHP is PID 1 of a container (there is no init process in front of
it), the kernel discards a signal the process sends to itself while the action
of that signal is the default one. The Go runtime ends the process that way,
so the outcome of a crash outside Go code differs:

| Crash | Without the reporter | With the reporter |
|---|---|---|
| SIGSEGV, SIGBUS, SIGFPE | The process does not end. The crashed thread repeats the faulting instruction, its request hangs, and the other threads keep serving | The line is printed, and about a second later the process exits with status 128 + signal number. It is an exit, not a death by signal, so there is no core dump |
| `abort()` with glibc 2.41 or later | The process ends, but not by SIGABRT: glibc falls back to an instruction that raises another fatal signal | The line is printed, and the process ends the same way |
| `abort()` with an older glibc, on arm64 | The process does not end | The line is printed, and the process does not end |
| `abort()` with an older glibc, on amd64 | The process does not end | The line is printed, followed by a line for the SIGSEGV glibc falls back to (`"code":128`). The process then exits as in the first row |
| SIGILL | The process does not end, as above | The line is printed once, and the process does not end |

With an init process (`docker run --init`, for example) FrankenPHP is not
PID 1 and the process ends the same way with and without the reporter.

## What is not reported

- SIGKILL cannot be caught, so an out-of-memory kill leaves no line.
- A crash in Go code is left to the Go runtime, as in the table above.
- If stderr does not become writable within about a second (a full pipe that
  nobody reads, for example), the line is dropped and the process ends without
  it.
- A stack overflow on a thread that has no alternate signal stack leaves no
  line, because the handler has no stack to run on. The Go runtime gives an
  alternate signal stack to every thread that has called into Go, which
  includes FrankenPHP's PHP threads. A thread created in C that never calls
  into Go has none unless it sets one up.
- If another library installs a handler for the same signals after the Go
  runtime has started, the report depends on that handler. The line is
  written when it calls the handler that was installed before it. It is not
  written when it ends the process itself, or when it restores the default
  action and raises the signal again.

## Special cases

Most deployments can skip this section. It lists the setups in which the
reporter behaves differently from the description above.

### `GOTRACEBACK=crash`

With this setting the Go runtime finishes a crash by sending SIGABRT to
itself, and that SIGABRT is reported like any other:

- An unrecovered Go panic, which is otherwise not reported, leaves a line for
  the SIGABRT.
- A fault in C code called from a goroutine leaves two lines: one for the
  fault and one for the SIGABRT.

### A signal handler installed before the program started

A preloaded library can install handlers for these signals before the Go
runtime starts. The Go runtime calls such a handler for a signal raised by
non-Go code, and so does the reporter, after writing the line.

- If that handler returns instead of ending the process, the line has been
  printed although the process keeps running. A later fault is reported
  again, unless it is the same signal at the same instruction on the same
  thread as the last one reported there.
- After the process survives a SIGILL or SIGABRT on a thread not created by
  Go, or any of the five signals sent with `kill` to such a thread, the Go
  runtime puts its own handler back in front of this one for that signal.
  From then on SIGSEGV, SIGBUS and SIGFPE are reported without `callers`,
  and SIGILL and SIGABRT are not reported.

### `os/signal.Notify` and `os/signal.Ignore` on these signals

- If the program subscribes to SIGSEGV, SIGBUS or SIGFPE with
  `os/signal.Notify`, or ignores them with `os/signal.Ignore`, the process ends
  differently once the reporter is enabled. A fault in C code called from a
  goroutine ends with the default action of the signal, without Go's
  traceback. With `os/signal.Notify`, a fault on a thread not created by Go
  also ends with the default action; without the reporter that thread faults
  in a loop and the process keeps running.
- If the program subscribes to SIGILL or SIGABRT with `os/signal.Notify`, one
  sent with `kill` goes to the channel and the process keeps running, but the
  line is printed.

## PHP extension (experimental)

The `ext` directory holds the reporter as a PHP extension. It loads into an
official FrankenPHP Docker image like any other extension, so no custom build
is needed, and it adds the PHP call stack to the line.

### How it differs from the Go module

The extension installs its handlers when PHP starts (`MINIT`), after the Go
runtime has started. The Go runtime then no longer forwards signals to it;
instead it is in front of the Go handler, and does what the `os/signal`
documentation asks of non-Go code that installs a handler for a synchronous
signal (["Go programs that use cgo or SWIG"](https://pkg.go.dev/os/signal#hdr-Go_programs_that_use_cgo_or_SWIG)):

- It saves the Go handler it replaces, and stays off if the handler in place
  is not Go code.
- It decides from the PC whether the signal hit Go code. Only a signal outside
  the code of the Go program is reported.
- It always calls the Go handler afterwards, with the original arguments. The
  Go runtime then handles the signal and ends the process exactly as it does
  without the extension.

It does not rely on how the Go runtime treats a signal sent with `kill`, as
the Go module does. What it relies on instead is the layout of two internal
structures of the Go runtime: to find the code of the Go program
(`runtime.text` to `runtime.etext`) in a stripped executable, it reads the
pclntab header and `runtime.firstmoduledata`. Each field it uses is checked
against the others at startup, and the extension stays off if they do not
match.

| Crash | Go module | Extension |
|---|---|---|
| C code on a PHP thread or any other thread not created by Go | reported | reported, with `php_frames` on a PHP thread |
| C code called from a goroutine | reported, then Go's fatal error output | same |
| Go code | not reported | not reported |
| Unrecovered Go panic with `GOTRACEBACK=crash` | the SIGABRT of the Go runtime is reported | not reported |
| SIGSEGV, SIGBUS, SIGFPE on a PHP thread, FrankenPHP as PID 1 | reported, then the process exits about a second later with status 128 + signal number | reported once; the process does not end, as without a reporter |
| `abort()` on a PHP thread, FrankenPHP as PID 1 | see [FrankenPHP as PID 1](#frankenphp-as-pid-1) | reported; the process ends as without a reporter. With an older glibc on amd64, the SIGSEGV glibc falls back to is reported too, and the process does not end |

A configuration reload through the admin API restarts PHP, and the extension
is armed again. If another handler was installed after the extension, the
extension stays in the chain behind it and keeps the Go handler it saved.

A handler installed before the program started and `os/signal.Notify` on
these signals are not tested with the extension yet.

### Requirements

- The requirements of the Go module above
- FrankenPHP. Under any other SAPI the extension does nothing
- Built against the same PHP (version and ZTS) as the FrankenPHP it is loaded
  into

Do not enable the Go module at the same time: when the Go module is armed, its
handlers are in front of the Go runtime's, and the extension stays off and says
so. With `FRANKENPHP_CRASH_REPORTER=true` set but no Go module in the
executable, the extension is armed as usual.

### Installation

Build it with the `phpize` of the PHP that FrankenPHP runs, from a checkout of
the whole repository (the extension includes `crash_report.h` from the root):

```sh
cd ext
phpize
./configure --enable-crash-reporter
make
make install
```

Then load it:

```ini
extension=crash_reporter
```

With the official Docker image, build it in the `-builder` image of the same
version and copy `modules/crash_reporter.so` into the extension directory of
the runtime image.

### Output

At startup it prints one of these lines:

```json
{"severity":"INFO","message":"crash reporter armed","source":"extension"}
{"severity":"WARNING","message":"crash reporter not armed: ...","source":"extension"}
```

A report has the fields of the Go module, plus:

| Field | Meaning |
|---|---|
| `php_frames` | The PHP call stack of the crashed thread, innermost first. Present only on a thread that has PHP globals. Each frame has `function` (`Class::method` for a method) and, for PHP code, `file` and `line`. A frame of an internal function, such as `posix_getpwnam`, has only `function`. A file without a function is top-level code |
| `php_frames_omitted` | Present only when frames were dropped from the far end: beyond 32 frames, or to leave room for `callers` within 4096 bytes |

```json
{"severity":"CRITICAL","message":"fatal signal","signal":11,"code":1,"fault_addr":"0x0","tid":64,"thread":"php-3","pc":"/usr/local/lib/trigger.so+0x1234","php_frames":[{"function":"posix_getpwnam"},{"function":"nest","file":"/app/worker.php","line":9},{"function":"{closure:/app/worker.php:12}","file":"/app/worker.php","line":14},{"function":"frankenphp_handle_request"},{"file":"/app/worker.php","line":12}],"callers":["/usr/local/lib/libphp.so+0x6c75a8","..."]}
```

The line of a PHP frame is the line it was executing when it called the next
frame. For the innermost PHP frame, when the crash is inside the engine itself
rather than in an internal function it called, the line may be an earlier one.
The frames are read from memory that the crash may have corrupted; if reading
faults, the frames read before stay.

## Testing

The test builds FrankenPHP with this module, crashes it in several ways, and
checks the output and the exit status. It needs only Docker:

```sh
docker build --pull --progress plain -t crash-reporter-test -f test/Dockerfile .
sh test/pid1.sh crash-reporter-test
```

The second command crashes FrankenPHP running as PID 1 of a container.

The image build also builds the PHP extension and runs `test/ext.sh`, which
crashes the same FrankenPHP binary with the extension loaded and the Go module
off. `test/pid1.sh` covers the extension as well. The `-O0` / `-O2` build
argument applies to the extension too.

This tests the latest FrankenPHP 1.x release. To test another one, pass
`--build-arg FRANKENPHP_VERSION=<version>`.

The workflow also builds the reporter with `-O0`
(`--build-arg CGO_OPTIMIZATION=-O0`), builds FrankenPHP as a
position-independent executable (`--build-arg GO_BUILDMODE=pie`), and runs on
Debian bookworm for its older glibc (`--build-arg DEBIAN_RELEASE=bookworm`).

The `testhooks` directory is a separate Go module that exposes HTTP endpoints
to crash the process from Go and from C code inside the FrankenPHP binary. It is
linked only into the test binary.

## License

MIT

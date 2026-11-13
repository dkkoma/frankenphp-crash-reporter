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
outside Go code it writes one line, once per process.

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

The Go runtime allows non-Go code in a cgo program to install its own signal
handlers, provided that the handler runs on the alternate signal stack and that
signals raised in Go code still reach the Go handler
(see ["Go programs that use cgo or SWIG"](https://pkg.go.dev/os/signal#hdr-Go_programs_that_use_cgo_or_SWIG)).
This module stays within that contract.

When the package is initialized, after the Go runtime has installed its
handlers, it:

1. finds the address range that holds Go code,
2. saves the Go handler for each of the five signals,
3. installs its own handler with the same mask and flags.

When a signal arrives, the handler looks at the interrupted program counter:

- **In Go code** (a nil dereference, for example): the signal goes straight to
  the Go handler. Panics stay recoverable and nothing is printed.
- **Outside Go code**: the handler writes the report, then calls the Go handler,
  which ends the process the way it always does.

The handler does not allocate, take locks, or call `dladdr`. Modules are
resolved from `/proc/self/maps`, and build IDs are read from the ELF headers
already mapped in memory. `backtrace()` is called once at install time so that
its lazy initialization does not happen inside the handler.

| Crash | Report | How the process ends |
|---|---|---|
| C code on a PHP thread or any other thread not created by Go | yes | default action of the signal (exit status 128 + signal) |
| C code called from a goroutine | yes | Go's fatal error output, exit status 2 |
| Go code (recoverable panic) | no | keeps running |
| Unrecovered Go panic | no | Go's panic output |

## Limitations

- SIGKILL cannot be caught, so an out-of-memory kill leaves no line.
- A stack overflow on a thread that has no alternate signal stack leaves no
  line, because the handler has no stack to run on. PHP threads are such
  threads.
- A crash before the package is initialized is not reported.
- If another library installs a handler for the same signals later, it replaces
  this one.
- A fatal signal sent from outside the process (`kill -SEGV`) is reported only
  if it happens to interrupt non-Go code.
- The Go text range is found by probing `runtime.FuncForPC` and is verified at
  startup. If the check fails, the reporter stays off and prints
  `crash reporter not armed`.

## Testing

The test builds FrankenPHP with this module, crashes it in several ways, and
checks the output and the exit status. It needs only Docker:

```sh
docker build --progress plain -f test/Dockerfile .
```

The `testhooks` directory is a separate Go module that exposes HTTP endpoints
to crash the process from Go and from C code inside the FrankenPHP binary. It is
linked only into the test binary.

## License

MIT

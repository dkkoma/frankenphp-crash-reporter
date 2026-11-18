#!/bin/sh
# Tests the PHP extension. FrankenPHP is the same binary as in run.sh, with the
# Go module linked in but not enabled.
set -u

. /crash-reporter-test/lib.sh

export PHP_INI_SCAN_DIR=:/crash-reporter-test/ext-ini

armed='"message":"crash reporter armed","source":"extension"'
worker=/crash-reporter-test/worker.php

if start "extension: Go panics from nil dereferences are still recoverable"; then
    errors=0
    for _ in $(seq 1 200); do
        curl -fs -o /dev/null "$url" || errors=$((errors + 1))
    done
    if [ "$errors" -ne 0 ]; then
        fail "$case_name: $errors of 200 requests failed"
    fi
    expect_body "$hooks/go-nil" "recovered=1"
    expect_body "$hooks/go-nil-on-c-thread" "recovered=1"
    stop
    expect_count "$armed" 1
    expect_count "crash reporter not armed" 0
    expect_count '"message":"fatal signal"' 0
    finish
fi

if start "extension: SIGSEGV on a PHP thread"; then
    crash "$url?crash=segv"
    expect_status 139
    expect_report '"signal":11,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+' \
        '"php_frames":[{"function":"posix_getpwnam"},{"function":"nest","file":"'"$worker"'","line":9},' \
        '"file":"'"$worker"'","line":14}' \
        '"callers":["/usr/local/lib/libphp.so+' '"/usr/local/bin/frankenphp+'
    finish
fi

if start "extension: SIGSEGV deep in PHP code"; then
    crash "$url?crash=segv&depth=200"
    expect_status 139
    expect_report '"signal":11,' '"php_frames":[{"function":"posix_getpwnam"},' '"php_frames_omitted":' \
        '"callers":["/usr/local/lib/libphp.so+'
    if [ "${#line}" -ge 4096 ]; then
        fail "$case_name: the fatal signal line is ${#line} bytes, want less than 4096"
    fi
    finish
fi

if start "extension: stack overflow on a PHP thread"; then
    crash "$url?crash=stack_overflow"
    expect_status 139
    expect_report '"signal":11,' '"code":2,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
    finish
fi

if start "extension: abort() on a PHP thread"; then
    crash "$url?crash=abort"
    expect_status 134
    expect_report '"signal":6,' '"thread":"php-' '/usr/local/lib/trigger.so+' '"php_frames":[{"function":"posix_getpwnam"},'
    finish
fi

if start "extension: SIGBUS on a PHP thread"; then
    crash "$url?crash=bus"
    expect_status 135
    expect_report '"signal":7,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
    finish
fi

if [ "$(uname -m)" = x86_64 ]; then
    if start "extension: SIGFPE on a PHP thread"; then
        crash "$url?crash=divide_by_zero"
        expect_status 136
        expect_report '"signal":8,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
        finish
    fi
fi

if start "extension: call through an unmapped function pointer on a PHP thread"; then
    crash "$url?crash=wildcall"
    expect_status 139
    expect_report '"signal":11,' '"thread":"php-' '"return_address":"/usr/local/lib/trigger.so+' '"php_frames":['
    finish
fi

if start "extension: SIGSEGV on a PHP thread with a handler installed after the extension that calls the previous one"; then
    expect_body "$url?crash=install_later_handler" "ok"
    expect_body "$hooks/go-nil" "recovered=1"
    expect_count "later handler ran" 1
    crash "$url?crash=segv"
    expect_status 139
    expect_report '"signal":11,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
    expect_count "later handler ran" 2
    finish
fi

if start "extension: SIGSEGV in C code of the frankenphp binary on a C thread"; then
    crash "$hooks/segv-on-c-thread"
    expect_status 139
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    expect_pc_in_symbol /crash-reporter-test/test-hooks-segv.symbol
    expect_count '"php_frames"' 0
    finish
fi

if start "extension: SIGSEGV in C code on a thread that recovered from a Go panic just before"; then
    crash "$hooks/go-nil-then-segv-on-c-thread"
    expect_status 139
    expect_report '"signal":11,' '"fault_addr":"0x10",' '"pc":"/usr/local/bin/frankenphp+'
    finish
fi

if start "extension: SIGSEGV in C code called from a Go thread"; then
    crash "$hooks/segv-in-cgo"
    expect_status 2
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    expect_count "signal arrived during cgo execution" 1
    finish
fi

if start "extension: abort() in C code called from a Go thread"; then
    crash "$hooks/abort-in-cgo"
    expect_status 2
    expect_report '"signal":6,' '"code":-6,' '/usr/local/bin/frankenphp+'
    expect_count "SIGABRT: abort" 1
    finish
fi

if start "extension: SIGILL in C code of the frankenphp binary on a C thread"; then
    crash "$hooks/ill-on-c-thread"
    expect_status 132
    expect_report '"signal":4,' '"pc":"/usr/local/bin/frankenphp+'
    finish
fi

# Fails under amd64 emulation on an arm64 host, as in run.sh.
if start "extension: SIGSEGV sent to a C thread"; then
    crash "$hooks/raise-segv-on-c-thread"
    expect_status 139
    expect_report '"signal":11,' '"code":-6,' '"fault_addr":null,'
    finish
fi

if start "extension: SIGSEGV on two C threads at the same time"; then
    crash "$hooks/segv-on-two-c-threads"
    expect_status 139
    reports=$(count '"message":"fatal signal","signal":11,')
    if [ "$reports" -lt 1 ] || [ "$reports" -gt 2 ]; then
        fail "$case_name: $reports fatal signal lines, want 1 or 2"
    fi
    finish
fi

export GOTRACEBACK=crash
if start "extension: Go aborts itself after an unrecovered panic"; then
    crash "$hooks/go-panic"
    expect_status 134
    expect_count "panic: test hook" 1
    expect_count '"message":"fatal signal"' 0
    finish
fi
unset GOTRACEBACK

export FRANKENPHP_CRASH_REPORTER=true
if start "extension: the Go module is enabled as well"; then
    crash "$url?crash=segv"
    expect_status 139
    expect_count "crash reporter not armed: the Go module is enabled" 1
    expect_count '"message":"crash reporter armed"}' 1
    expect_report '"signal":11,' '"thread":"php-'
    expect_count '"php_frames"' 0
    finish
fi
unset FRANKENPHP_CRASH_REPORTER

rm -f /etc/ld.so.preload

if [ "$failures" -ne 0 ]; then
    echo "crash reporter extension test: $failures failed"
    exit 1
fi
echo "crash reporter extension test: OK"

#!/bin/sh
set -u

log=/tmp/frankenphp.log
url=http://127.0.0.1:8080/
hooks=http://127.0.0.1:8081
failures=0

export CRASH_REPORTER_TEST_HOOKS=127.0.0.1:8081
printf '%s\n' /usr/local/lib/trigger.so /usr/local/lib/oversized-note.so > /etc/ld.so.preload

fail() {
    echo "NG: $1"
    failures=$((failures + 1))
}

start() {
    echo "== $1"
    case_name=$1
    failures_before=$failures
    frankenphp run --config /crash-reporter-test/Caddyfile --adapter caddyfile > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 300); do
        if curl -fs -o /dev/null "$url" && curl -s -o /dev/null "$hooks/"; then
            return 0
        fi
        sleep 0.1
    done
    fail "$case_name: frankenphp did not start"
    kill -KILL "$pid" 2>/dev/null
    wait "$pid"
    finish
    return 1
}

wait_exit() {
    sleep 30 && kill -KILL "$pid" 2>/dev/null &
    watchdog=$!
    wait "$pid"
    status=$?
    kill "$watchdog" 2>/dev/null
}

finish() {
    if [ "$failures" -ne "$failures_before" ]; then
        cat "$log"
    fi
}

count() {
    grep -c "$1" "$log"
}

expect_status() {
    if [ "$status" -ne "$1" ]; then
        fail "$case_name: exit status is $status, want $1"
    fi
}

expect_count() {
    if [ "$(count "$1")" -ne "$2" ]; then
        fail "$case_name: $(count "$1") lines match \"$1\", want $2"
        return 1
    fi
}

expect_body() {
    body=$(curl -s "$1")
    if [ "$body" != "$2" ]; then
        fail "$case_name: $1 responded \"$body\", want \"$2\""
    fi
}

expect_report() {
    expect_count '"message":"fatal signal"' 1 || return
    line=$(grep '"message":"fatal signal"' "$log" | head -n 1)
    echo "$line"
    if ! php -r 'json_decode($argv[1], flags: JSON_THROW_ON_ERROR);' "$line"; then
        fail "$case_name: the fatal signal line is not valid JSON"
    fi
    for want in '"process":"frankenphp"' "$@"; do
        case $line in
            *"$want"*) ;;
            *) fail "$case_name: the fatal signal line lacks $want" ;;
        esac
    done
}

crash() {
    curl -s -o /dev/null "$1"
    wait_exit
}

stop() {
    kill -INT "$pid"
    wait_exit
    expect_status 0
}

if start "disabled"; then
    stop
    expect_count "crash reporter armed" 0
    finish
fi

if start "disabled: SIGSEGV in C code of the frankenphp binary on a C thread"; then
    crash "$hooks/segv-on-c-thread"
    expect_status 139
    expect_count '"message":"fatal signal"' 0
    finish
fi

export LD_PRELOAD=/usr/local/lib/previous-handler.so
if start "disabled: SIGSEGV on a PHP thread with a handler installed before the program started"; then
    unset LD_PRELOAD
    crash "$url?crash=segv"
    expect_status 77
    expect_count "previous handler got the original fault" 1
    expect_count '"message":"fatal signal"' 0
    finish
fi
unset LD_PRELOAD

export LD_PRELOAD=/usr/local/lib/previous-handler.so
if start "disabled: abort() on a PHP thread with a handler installed before the program started"; then
    unset LD_PRELOAD
    crash "$url?crash=abort"
    expect_status 79
    expect_count "previous handler got SIGABRT" 1
    finish
fi
unset LD_PRELOAD

export LD_PRELOAD=/usr/local/lib/previous-handler.so PREVIOUS_HANDLER=ignore
if start "disabled: SIGSEGV on a PHP thread while SIGSEGV was ignored before the program started"; then
    unset LD_PRELOAD PREVIOUS_HANDLER
    curl -s -o /dev/null --max-time 2 "$url?crash=segv"
    if ! kill -0 "$pid" 2>/dev/null; then
        fail "$case_name: the process ended"
    fi
    kill -KILL "$pid"
    wait "$pid"
    finish
fi
unset LD_PRELOAD PREVIOUS_HANDLER

if start "disabled: SIGSEGV in C code called from a Go thread while os/signal subscribes to SIGSEGV"; then
    curl -s -o /dev/null "$hooks/notify-segv"
    crash "$hooks/segv-in-cgo"
    expect_status 2
    expect_count "signal arrived during cgo execution" 1
    finish
fi

if start "disabled: SIGSEGV on a PHP thread while os/signal subscribes to SIGSEGV"; then
    curl -s -o /dev/null "$hooks/notify-segv"
    curl -s -o /dev/null --max-time 2 "$url?crash=segv"
    if ! kill -0 "$pid" 2>/dev/null; then
        fail "$case_name: the process ended"
    fi
    kill -KILL "$pid"
    wait "$pid"
    finish
fi

if start "disabled: SIGSEGV in C code called from a Go thread while os/signal ignores SIGSEGV"; then
    curl -s -o /dev/null "$hooks/ignore-segv"
    crash "$hooks/segv-in-cgo"
    expect_status 2
    expect_count "signal arrived during cgo execution" 1
    finish
fi

if start "disabled: abort() in C code called from a Go thread"; then
    crash "$hooks/abort-in-cgo"
    expect_status 2
    expect_count "SIGABRT: abort" 1
    expect_count "signal arrived during cgo execution" 1
    finish
fi

if start "disabled: SIGSEGV sent to a C thread"; then
    crash "$hooks/raise-segv-on-c-thread"
    expect_status 139
    expect_count '"message":"fatal signal"' 0
    finish
fi

export FRANKENPHP_CRASH_REPORTER=true

if start "Go panics from nil dereferences are still recoverable"; then
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
    expect_count "crash reporter armed" 1
    expect_count "crash reporter not armed" 0
    expect_count '"message":"fatal signal"' 0
    finish
fi

if start "SIGSEGV on a PHP thread"; then
    crash "$url?crash=segv"
    expect_status 139
    expect_report '"signal":11,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+' \
        '"/usr/local/lib/trigger.so":"' '"/usr/local/lib/libphp.so":"' '"/usr/local/bin/frankenphp":"'
    finish
fi

if start "SIGSEGV in a module whose build ID note claims an oversized ID"; then
    crash "$url?crash_in_oversized_note=segv"
    expect_status 139
    expect_report '"signal":11,' '"pc":"/usr/local/lib/oversized-note.so+' '"/usr/local/lib/oversized-note.so":null'
    finish
fi

export LD_PRELOAD=/usr/local/lib/previous-handler.so
if start "SIGSEGV on a PHP thread with a handler installed before the program started"; then
    unset LD_PRELOAD
    expect_body "$hooks/go-nil" "recovered=1"
    expect_body "$hooks/go-nil-on-c-thread" "recovered=1"
    crash "$url?crash=segv"
    expect_status 77
    expect_count "crash reporter armed" 1
    expect_report '"signal":11,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
    expect_count "previous handler got the original fault" 1
    finish
fi
unset LD_PRELOAD

export LD_PRELOAD=/usr/local/lib/previous-handler.so
if start "abort() on a PHP thread with a handler installed before the program started"; then
    unset LD_PRELOAD
    crash "$url?crash=abort"
    expect_status 79
    expect_report '"signal":6,' '"thread":"php-' '/usr/local/lib/trigger.so+'
    expect_count "previous handler got SIGABRT" 1
    finish
fi
unset LD_PRELOAD

export LD_PRELOAD=/usr/local/lib/previous-handler.so PREVIOUS_HANDLER=plain
if start "SIGSEGV on a PHP thread with a handler without SA_SIGINFO installed before the program started"; then
    unset LD_PRELOAD PREVIOUS_HANDLER
    crash "$url?crash=segv"
    expect_status 78
    expect_report '"signal":11,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
    expect_count "plain previous handler got SIGSEGV" 1
    finish
fi
unset LD_PRELOAD PREVIOUS_HANDLER

export LD_PRELOAD=/usr/local/lib/previous-handler.so
if start "a handler installed before the program started recovers from a fault, then a second fault is fatal"; then
    unset LD_PRELOAD
    expect_body "$url?crash=protected" "ok"
    expect_count "previous handler recovered from the fault" 1
    expect_count '"message":"fatal signal"' 1
    expect_body "$url" "ok"
    crash "$url?crash=segv"
    expect_status 77
    expect_count "previous handler got the original fault" 1
    expect_count '"message":"fatal signal"' 2
    finish
fi
unset LD_PRELOAD

export LD_PRELOAD=/usr/local/lib/previous-handler.so PREVIOUS_HANDLER=ignore
if start "SIGSEGV on a PHP thread while SIGSEGV was ignored before the program started"; then
    unset LD_PRELOAD PREVIOUS_HANDLER
    curl -s -o /dev/null --max-time 2 "$url?crash=segv"
    if ! kill -0 "$pid" 2>/dev/null; then
        fail "$case_name: the process ended"
    fi
    expect_count '"message":"fatal signal"' 1
    kill -KILL "$pid"
    wait "$pid"
    finish
fi
unset LD_PRELOAD PREVIOUS_HANDLER

if start "call through an unmapped function pointer on a PHP thread"; then
    crash "$url?crash=wildcall"
    expect_status 139
    expect_report '"signal":11,' '"thread":"php-' '"return_address":"/usr/local/lib/trigger.so+'
    finish
fi

if start "SIGSEGV in C code of the frankenphp binary on a C thread"; then
    crash "$hooks/segv-on-c-thread"
    expect_status 139
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    finish
fi

if start "SIGSEGV in C code on a thread that recovered from a Go panic just before"; then
    crash "$hooks/go-nil-then-segv-on-c-thread"
    expect_status 139
    expect_report '"signal":11,' '"fault_addr":"0x10",' '"pc":"/usr/local/bin/frankenphp+'
    finish
fi

if start "SIGSEGV in C code called from a Go thread"; then
    crash "$hooks/segv-in-cgo"
    expect_status 2
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    expect_count "signal arrived during cgo execution" 1
    finish
fi

if start "SIGSEGV in C code called from a Go thread while os/signal subscribes to SIGSEGV"; then
    curl -s -o /dev/null "$hooks/notify-segv"
    crash "$hooks/segv-in-cgo"
    expect_status 139
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    expect_count "signal arrived during cgo execution" 0
    finish
fi

if start "SIGSEGV on a PHP thread while os/signal subscribes to SIGSEGV"; then
    curl -s -o /dev/null "$hooks/notify-segv"
    crash "$url?crash=segv"
    expect_status 139
    expect_report '"signal":11,' '"thread":"php-' '"pc":"/usr/local/lib/trigger.so+'
    finish
fi

if start "SIGSEGV in C code called from a Go thread while os/signal ignores SIGSEGV"; then
    curl -s -o /dev/null "$hooks/ignore-segv"
    crash "$hooks/segv-in-cgo"
    expect_status 139
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    expect_count "signal arrived during cgo execution" 0
    finish
fi

if start "abort() in C code called from a Go thread"; then
    crash "$hooks/abort-in-cgo"
    expect_status 2
    expect_report '"signal":6,' '"code":-6,' '/usr/local/bin/frankenphp+'
    expect_count "SIGABRT: abort" 1
    expect_count "signal arrived during cgo execution" 1
    finish
fi

# Fails under amd64 emulation on an arm64 host: the signal raised by the thread
# arrives with the si_code of a fault ("code":1). It passes on real amd64.
if start "SIGSEGV sent to a C thread"; then
    crash "$hooks/raise-segv-on-c-thread"
    expect_status 139
    expect_report '"signal":11,' '"code":-6,' '"fault_addr":null,'
    finish
fi

if start "SIGSEGV on two C threads at the same time"; then
    crash "$hooks/segv-on-two-c-threads"
    expect_status 139
    expect_report '"signal":11,' '"pc":"/usr/local/bin/frankenphp+'
    finish
fi

if start "abort() on a PHP thread"; then
    crash "$url?crash=abort"
    expect_status 134
    expect_report '"signal":6,' '"thread":"php-' '/usr/local/lib/trigger.so+'
    finish
fi

if start "SIGILL in C code of the frankenphp binary on a C thread"; then
    crash "$hooks/ill-on-c-thread"
    expect_status 132
    expect_report '"signal":4,' '"pc":"/usr/local/bin/frankenphp+'
    finish
fi

export GOTRACEBACK=crash

if start "Go aborts itself after an unrecovered panic"; then
    crash "$hooks/go-panic"
    expect_status 134
    expect_count "panic: test hook" 1
    expect_report '"signal":6,' '"code":-6,'
    finish
fi

rm -f /etc/ld.so.preload

if [ "$failures" -ne 0 ]; then
    echo "crash reporter test: $failures failed"
    exit 1
fi
echo "crash reporter test: OK"

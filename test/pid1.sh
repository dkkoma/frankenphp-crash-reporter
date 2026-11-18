#!/bin/sh
# Runs on the host: FrankenPHP has to be PID 1 of a container, which a build step cannot arrange.
set -u

image=${1:?usage: pid1.sh <image built from test/Dockerfile>}
failures=0
url=http://127.0.0.1:8080/
hooks=http://127.0.0.1:8081

fail() {
    echo "NG: $1"
    failures=$((failures + 1))
}

crash_as_pid1() {
    case_name=$1
    cid=$(docker run -d -e FRANKENPHP_CRASH_REPORTER="$2" -e LD_PRELOAD=/usr/local/lib/trigger.so \
        -e CRASH_REPORTER_TEST_HOOKS=127.0.0.1:8081 -e PHP_INI_SCAN_DIR="${4:-}" "$image" \
        frankenphp run --config /crash-reporter-test/Caddyfile --adapter caddyfile)
    echo "== $case_name"
    for _ in $(seq 1 100); do
        docker exec "$cid" curl -fs -o /dev/null http://127.0.0.1:8080/ && break
        sleep 0.2
    done
    docker exec "$cid" curl -s -o /dev/null --max-time 2 "$3"
    for _ in $(seq 1 10); do
        [ "$(docker inspect -f '{{.State.Running}}' "$cid")" = false ] && break
        sleep 1
    done
    running=$(docker inspect -f '{{.State.Running}}' "$cid")
    status=$(docker inspect -f '{{.State.ExitCode}}' "$cid")
    reports=$(docker logs "$cid" 2>&1 | grep -c '"message":"fatal signal"')
    docker logs "$cid" 2>&1 | grep '"message":"fatal signal"'
    docker rm -f "$cid" > /dev/null
    echo "running=$running status=$status"
}

expect_running() {
    if [ "$running" != true ]; then
        fail "$case_name: the process ended with status $status"
    fi
}

expect_reports() {
    if [ "$reports" -ne "$1" ]; then
        fail "$case_name: $reports fatal signal lines, want $1"
    fi
}

crash_as_pid1 "disabled: SIGSEGV on a PHP thread while FrankenPHP is PID 1" false "$url?crash=segv"
expect_running

crash_as_pid1 "SIGSEGV on a PHP thread while FrankenPHP is PID 1" true "$url?crash=segv"
if [ "$running" != false ] || [ "$status" -ne 139 ]; then
    fail "$case_name: running=$running, exit status is $status, want 139"
fi
expect_reports 1

crash_as_pid1 "disabled: abort() on a PHP thread while FrankenPHP is PID 1" false "$url?crash=abort"
running_without_reporter=$running
status_without_reporter=$status

crash_as_pid1 "abort() on a PHP thread while FrankenPHP is PID 1" true "$url?crash=abort"
if [ "$running_without_reporter" = false ]; then
    if [ "$running" != false ] || [ "$status" -ne "$status_without_reporter" ]; then
        fail "$case_name: running=$running, exit status is $status, want $status_without_reporter"
    fi
    expect_reports 1
elif [ "$running" = false ]; then
    if [ "$status" -ne 139 ]; then
        fail "$case_name: exit status is $status, want 139"
    fi
    expect_reports 2
else
    expect_reports 1
fi

crash_as_pid1 "disabled: SIGILL on a C thread while FrankenPHP is PID 1" false "$hooks/ill-on-c-thread"
expect_running

crash_as_pid1 "SIGILL on a C thread while FrankenPHP is PID 1" true "$hooks/ill-on-c-thread"
expect_running
expect_reports 1

# The PHP extension leaves the end of the process to the Go runtime, so it ends
# as it does without a reporter.
ext=:/crash-reporter-test/ext-ini

crash_as_pid1 "extension: SIGSEGV on a PHP thread while FrankenPHP is PID 1" false "$url?crash=segv" "$ext"
expect_running
expect_reports 1

crash_as_pid1 "extension: abort() on a PHP thread while FrankenPHP is PID 1" false "$url?crash=abort" "$ext"
if [ "$running" != "$running_without_reporter" ] || [ "$status" -ne "$status_without_reporter" ]; then
    fail "$case_name: running=$running, exit status is $status, want running=$running_without_reporter, $status_without_reporter"
fi
expect_reports 1

if [ "$failures" -ne 0 ]; then
    echo "crash reporter PID 1 test: $failures failed"
    exit 1
fi
echo "crash reporter PID 1 test: OK"

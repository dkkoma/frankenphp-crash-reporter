#!/bin/sh
# Runs on the host: FrankenPHP has to be PID 1 of a container, which a build step cannot arrange.
set -u

image=${1:?usage: pid1.sh <image built from test/Dockerfile>}
failures=0

fail() {
    echo "NG: $1"
    failures=$((failures + 1))
}

crash_as_pid1() {
    case_name=$1
    cid=$(docker run -d -e FRANKENPHP_CRASH_REPORTER="$2" -e LD_PRELOAD=/usr/local/lib/trigger.so "$image" \
        frankenphp run --config /crash-reporter-test/Caddyfile --adapter caddyfile)
    echo "== $case_name"
    for _ in $(seq 1 100); do
        docker exec "$cid" curl -fs -o /dev/null http://127.0.0.1:8080/ && break
        sleep 0.2
    done
    docker exec "$cid" curl -s -o /dev/null --max-time 2 "http://127.0.0.1:8080/?crash=segv"
    for _ in $(seq 1 10); do
        [ "$(docker inspect -f '{{.State.Running}}' "$cid")" = false ] && break
        sleep 1
    done
    running=$(docker inspect -f '{{.State.Running}}' "$cid")
    status=$(docker inspect -f '{{.State.ExitCode}}' "$cid")
    reports=$(docker logs "$cid" 2>&1 | grep -c '"message":"fatal signal"')
    docker logs "$cid" 2>&1 | grep '"message":"fatal signal"'
    docker rm -f "$cid" > /dev/null
}

crash_as_pid1 "disabled: SIGSEGV on a PHP thread while FrankenPHP is PID 1" false
if [ "$running" != true ]; then
    fail "$case_name: the process ended with status $status"
fi

crash_as_pid1 "SIGSEGV on a PHP thread while FrankenPHP is PID 1" true
if [ "$running" != false ] || [ "$status" -ne 139 ]; then
    fail "$case_name: running=$running, exit status is $status, want 139"
fi
if [ "$reports" -ne 1 ]; then
    fail "$case_name: $reports fatal signal lines, want 1"
fi

if [ "$failures" -ne 0 ]; then
    echo "crash reporter PID 1 test: $failures failed"
    exit 1
fi
echo "crash reporter PID 1 test: OK"

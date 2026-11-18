# Helpers shared by run.sh and ext.sh. Sourced, not run.

log=/tmp/frankenphp.log
url=http://127.0.0.1:8080/
hooks=http://127.0.0.1:8081
failures=0

export CRASH_REPORTER_TEST_HOOKS=127.0.0.1:8081
printf '%s\n' /usr/local/lib/trigger.so > /etc/ld.so.preload

fail() {
    echo "NG: $1"
    failures=$((failures + 1))
}

start() {
    echo "== $1"
    case_name=$1
    failures_before=$failures
    if [ -n "${stderr_fifo:-}" ]; then
        frankenphp run --config /crash-reporter-test/Caddyfile --adapter caddyfile > /dev/null 2> "$stderr_fifo" &
    else
        frankenphp run --config /crash-reporter-test/Caddyfile --adapter caddyfile > "$log" 2>&1 &
    fi
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
    for want in "$@"; do
        case $line in
            *"$want"*) ;;
            *) fail "$case_name: the fatal signal line lacks $want" ;;
        esac
    done
}

expect_pc_in_symbol() {
    read -r symbol_start symbol_size _ < "$1"
    pc=$(printf '%s' "$line" | sed 's/.*"pc":"[^"]*+0x\([0-9a-f]*\)".*/\1/')
    if [ "$((0x$pc))" -lt "$((0x$symbol_start))" ] || [ "$((0x$pc))" -ge "$((0x$symbol_start + 0x$symbol_size))" ]; then
        fail "$case_name: pc 0x$pc is outside the symbol at 0x$symbol_start (size 0x$symbol_size)"
    fi
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

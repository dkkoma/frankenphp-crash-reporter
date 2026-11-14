# syntax=docker/dockerfile:1.7

ARG FRANKENPHP_IMAGE=dunglas/frankenphp
ARG FRANKENPHP_VERSION=1

FROM ${FRANKENPHP_IMAGE}:${FRANKENPHP_VERSION}-builder AS builder

COPY --from=caddy:builder /usr/bin/xcaddy /usr/bin/xcaddy
COPY . /src/crash-reporter/
COPY test/trigger.c test/oversized_note.c test/previous_handler.c /tmp/
WORKDIR /go/src/app

RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=1 \
    XCADDY_GO_BUILD_FLAGS="-ldflags='-w -s' -tags=nobadger,nomysql,nopgx" \
    CGO_CFLAGS="$(php-config --includes)" \
    CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs)" \
    xcaddy build \
        --output /usr/local/bin/frankenphp \
        --with github.com/dunglas/frankenphp=./ \
        --with github.com/dunglas/frankenphp/caddy=./caddy/ \
        --with github.com/dkkoma/frankenphp-crash-reporter=/src/crash-reporter \
        --with github.com/dkkoma/frankenphp-crash-reporter/testhooks=/src/crash-reporter/testhooks \
    && gcc -O2 -fPIC -shared -Wall -Wextra -fno-omit-frame-pointer -fstack-protector-all -U_FORTIFY_SOURCE -o /trigger.so /tmp/trigger.c -ldl \
    && gcc -O2 -fPIC -shared -Wall -Wextra -o /oversized-note.so /tmp/oversized_note.c -ldl \
    && note_offset="$(objdump -h /oversized-note.so | awk '$2 == ".note.gnu.build-id" { print $6 }')" \
    && printf '\000\100\000\000' | dd of=/oversized-note.so bs=1 seek="$((0x${note_offset:?} + 4))" conv=notrunc \
    && readelf -n /oversized-note.so 2>&1 | head -5 \
    && gcc -O2 -fPIC -shared -Wall -Wextra -o /previous-handler.so /tmp/previous_handler.c

FROM ${FRANKENPHP_IMAGE}:${FRANKENPHP_VERSION}

RUN frankenphp version

COPY --from=builder /usr/local/bin/frankenphp /usr/local/bin/frankenphp
COPY --from=builder /trigger.so /usr/local/lib/trigger.so
COPY --from=builder /oversized-note.so /usr/local/lib/oversized-note.so
COPY --from=builder /previous-handler.so /usr/local/lib/previous-handler.so
COPY test/Caddyfile test/worker.php test/run.sh /crash-reporter-test/

RUN sh /crash-reporter-test/run.sh

# syntax=docker/dockerfile:1.7

ARG FRANKENPHP_IMAGE=dunglas/frankenphp
ARG FRANKENPHP_VERSION=1
ARG CGO_OPTIMIZATION=-O2

FROM ${FRANKENPHP_IMAGE}:${FRANKENPHP_VERSION}-builder AS builder

ARG CGO_OPTIMIZATION

COPY --from=caddy:builder /usr/bin/xcaddy /usr/bin/xcaddy
COPY . /src/crash-reporter/
COPY test/trigger.c test/oversized_note.c test/previous_handler.c test/unreadable_header.c /tmp/
WORKDIR /go/src/app

RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=1 \
    XCADDY_GO_BUILD_FLAGS="-ldflags='-w -s' -tags=nobadger,nomysql,nopgx" \
    CGO_CFLAGS="${CGO_OPTIMIZATION} -fstack-protector-strong $(php-config --includes)" \
    CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs)" \
    xcaddy build \
        --output /usr/local/bin/frankenphp \
        --with github.com/dunglas/frankenphp=./ \
        --with github.com/dunglas/frankenphp/caddy=./caddy/ \
        --with github.com/dkkoma/frankenphp-crash-reporter=/src/crash-reporter \
        --with github.com/dkkoma/frankenphp-crash-reporter/testhooks=/src/crash-reporter/testhooks \
    && gcc -O2 -fPIC -shared -Wall -Wextra -fno-omit-frame-pointer -fstack-protector-all -U_FORTIFY_SOURCE -o /trigger.so /tmp/trigger.c -ldl \
    && gcc -O2 -fPIC -shared -Wall -Wextra -Wl,--build-id=0x$(printf 'ab%.0s' $(seq 1 68)) -o /oversized-note.so /tmp/oversized_note.c -ldl \
    && readelf -n /oversized-note.so | grep 'Build ID' \
    && gcc -O2 -fPIC -shared -Wall -Wextra -o /previous-handler.so /tmp/previous_handler.c \
    && gcc -O2 -fPIC -shared -Wall -Wextra -Wl,-z,now -o /unreadable-header.so /tmp/unreadable_header.c -ldl

FROM ${FRANKENPHP_IMAGE}:${FRANKENPHP_VERSION}

RUN frankenphp version

COPY --from=builder /usr/local/bin/frankenphp /usr/local/bin/frankenphp
COPY --from=builder /trigger.so /usr/local/lib/trigger.so
COPY --from=builder /oversized-note.so /usr/local/lib/oversized-note.so
COPY --from=builder /previous-handler.so /usr/local/lib/previous-handler.so
COPY --from=builder /unreadable-header.so /usr/local/lib/unreadable-header.so
COPY test/Caddyfile test/worker.php test/run.sh /crash-reporter-test/

RUN sh /crash-reporter-test/run.sh

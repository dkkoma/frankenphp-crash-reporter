# syntax=docker/dockerfile:1.7

ARG FRANKENPHP_IMAGE=dunglas/frankenphp

FROM ${FRANKENPHP_IMAGE}:builder AS builder

COPY --from=caddy:builder /usr/bin/xcaddy /usr/bin/xcaddy
COPY . /src/crash-reporter/
COPY test/trigger.c /tmp/trigger.c
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
    && gcc -O2 -fPIC -shared -Wall -Wextra -o /trigger.so /tmp/trigger.c -ldl

FROM ${FRANKENPHP_IMAGE}

COPY --from=builder /usr/local/bin/frankenphp /usr/local/bin/frankenphp
COPY --from=builder /trigger.so /usr/local/lib/trigger.so
COPY test/Caddyfile test/worker.php test/run.sh /crash-reporter-test/

RUN sh /crash-reporter-test/run.sh

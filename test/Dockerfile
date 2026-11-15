# syntax=docker/dockerfile:1.7

ARG FRANKENPHP_IMAGE=dunglas/frankenphp
ARG FRANKENPHP_VERSION=1
ARG DEBIAN_RELEASE=
ARG CGO_OPTIMIZATION=-O2
ARG GO_BUILDMODE=exe

FROM ${FRANKENPHP_IMAGE}:${FRANKENPHP_VERSION}-builder${DEBIAN_RELEASE:+-}${DEBIAN_RELEASE} AS builder

ARG CGO_OPTIMIZATION
ARG GO_BUILDMODE

COPY --from=caddy:builder /usr/bin/xcaddy /usr/bin/xcaddy
COPY . /src/crash-reporter/
COPY test/trigger.c test/previous_handler.c /tmp/
WORKDIR /go/src/app

RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=1 \
    XCADDY_GO_BUILD_FLAGS="-buildmode=${GO_BUILDMODE} -tags=nobadger,nomysql,nopgx" \
    CGO_CFLAGS="${CGO_OPTIMIZATION} -fstack-protector-strong $(php-config --includes)" \
    CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs)" \
    xcaddy build \
        --output /usr/local/bin/frankenphp \
        --with github.com/dunglas/frankenphp=./ \
        --with github.com/dunglas/frankenphp/caddy=./caddy/ \
        --with github.com/dkkoma/frankenphp-crash-reporter=/src/crash-reporter \
        --with github.com/dkkoma/frankenphp-crash-reporter/testhooks=/src/crash-reporter/testhooks \
    && nm -S /usr/local/bin/frankenphp | grep ' test_hooks_segv$' > /test-hooks-segv.symbol \
    && readelf -h /usr/local/bin/frankenphp | grep 'Type:' \
    && cat /test-hooks-segv.symbol \
    && gcc -O2 -fPIC -shared -Wall -Wextra -fno-omit-frame-pointer -fstack-protector-all -U_FORTIFY_SOURCE -o /trigger.so /tmp/trigger.c -ldl \
    && gcc -O2 -fPIC -shared -Wall -Wextra -o /previous-handler.so /tmp/previous_handler.c

FROM ${FRANKENPHP_IMAGE}:${FRANKENPHP_VERSION}${DEBIAN_RELEASE:+-}${DEBIAN_RELEASE}

RUN frankenphp version

COPY --from=builder /usr/local/bin/frankenphp /usr/local/bin/frankenphp
COPY --from=builder /trigger.so /usr/local/lib/trigger.so
COPY --from=builder /previous-handler.so /usr/local/lib/previous-handler.so
COPY --from=builder /test-hooks-segv.symbol /crash-reporter-test/test-hooks-segv.symbol
COPY test/Caddyfile test/worker.php test/run.sh /crash-reporter-test/

RUN sh /crash-reporter-test/run.sh

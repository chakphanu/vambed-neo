# SPDX-License-Identifier: GPL-3.0-or-later
# vambed-neo image. Build with scripts/build.sh (reads pins.env).
ARG BASE_IMAGE=debian:bookworm-slim

FROM ${BASE_IMAGE} AS build
ARG MBELIB_NEO_URL
ARG MBELIB_NEO_COMMIT
ARG CODEC2_URL
ARG CODEC2_COMMIT
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git cmake make gcc libc6-dev pkg-config \
 && rm -rf /var/lib/apt/lists/*

# fetch <dir> <url> <commit>: shallow fetch of one commit, verified
RUN printf '%s\n' '#!/bin/sh' 'set -eu' \
      'test -n "$3" || { echo "fetch $1: no commit pinned" >&2; exit 1; }' \
      'git init -q "$1" && git -C "$1" fetch -q --depth 1 "$2" "$3"' \
      'git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD' \
      'got=$(git -C "$1" rev-parse HEAD)' \
      'test "$got" = "$3" || { echo "fetch $1: got $got, expected $3" >&2; exit 1; }' \
      'echo "fetch $1: $got"' > /usr/local/bin/fetch-pinned \
 && chmod 0755 /usr/local/bin/fetch-pinned

WORKDIR /src
RUN fetch-pinned codec2 "$CODEC2_URL" "$CODEC2_COMMIT" \
 && cmake -S codec2 -B codec2/build -DCMAKE_BUILD_TYPE=Release -DUNITTEST=OFF -DINSTALL_EXAMPLES=OFF \
 && cmake --build codec2/build -j"$(nproc)" \
 && cmake --install codec2/build

RUN fetch-pinned mbelib-neo "$MBELIB_NEO_URL" "$MBELIB_NEO_COMMIT" \
 && cmake -S mbelib-neo -B mbelib-neo/build -DCMAKE_BUILD_TYPE=Release -DMBELIB_ENABLE_NATIVE=OFF \
      -DMBELIB_BUILD_TESTS=OFF -DMBELIB_BUILD_EXAMPLES=OFF \
 && cmake --build mbelib-neo/build -j"$(nproc)" \
 && cmake --install mbelib-neo/build

COPY Makefile /src/vambed-neo/
COPY src /src/vambed-neo/src
RUN ldconfig \
 && PKG_CONFIG_PATH=/usr/local/lib/pkgconfig make -C /src/vambed-neo \
 && make -C /src/vambed-neo install PREFIX=/usr/local \
 && mkdir -p /out/lib \
 && cp -a /usr/local/lib/libmbe-neo.so* /usr/local/lib/libcodec2.so* /out/lib/

FROM ${BASE_IMAGE}
COPY --from=build /out/lib/ /usr/local/lib/
COPY --from=build /usr/local/bin/vambed-neo /usr/local/bin/vambed-neo
COPY LICENSE NOTICE README.md /usr/local/share/doc/vambed-neo/
RUN ldconfig && useradd --system --no-create-home --shell /usr/sbin/nologin vambed-neo
USER vambed-neo
EXPOSE 10100/udp
ENTRYPOINT ["/usr/local/bin/vambed-neo"]

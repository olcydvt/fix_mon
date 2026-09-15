# Multi-stage build. The runtime image carries only the binary and libsqlite3,
# no compiler and no source.
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make libsqlite3-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/

# The image has libsqlite3-dev, so the resolver takes the system route and the
# build needs no network access.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DFIXMON_SQLITE_PROVIDER=system \
    && cmake --build build -j"$(nproc)" \
    && ./build/fixmon_selftest

# ---------------------------------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 10001 -s /usr/sbin/nologin fixmon

COPY --from=build /src/build/fixmon /usr/local/bin/fixmon

# Logs are mounted read-only in compose; the collector never writes to them.
RUN mkdir -p /var/lib/fixmon && chown fixmon:fixmon /var/lib/fixmon
VOLUME /var/lib/fixmon

USER fixmon
EXPOSE 9109

# Probe the real scrape port. Checking --version would only prove the binary
# exists, not that the collector is still serving.
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/9109 && printf "GET /healthz HTTP/1.0\r\n\r\n" >&3 && head -1 <&3 | grep -q 200'

ENTRYPOINT ["/usr/local/bin/fixmon"]
CMD ["/etc/fixmon/fixmon.ini"]

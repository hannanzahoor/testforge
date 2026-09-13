# TestForge engine — multi-stage build.
#
# Stage 1 compiles; stage 2 carries only the binary and the runtime libraries
# it needs. The result is a small image with no compiler, no source and no
# build cache in it — which matters because this image is what CI runs, and a
# toolchain in a test-runner image is attack surface for no benefit.

# ---------------------------------------------------------------------------
# Stage 1: build
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# git and ca-certificates are needed by CMake FetchContent (GoogleTest over
# https). Everything else is the C++ toolchain.
RUN apt-get update && apt-get install --no-install-recommends -y \
        build-essential \
        ca-certificates \
        cmake \
        git \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the build system first so a source-only change does not invalidate the
# dependency-fetch layer.
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/

COPY include/ ./include/
COPY src/ ./src/
COPY examples/ ./examples/
COPY benchmarks/ ./benchmarks/
COPY tests/ ./tests/

ARG BUILD_TYPE=RelWithDebInfo
ARG BUILD_TESTS=ON

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
        -DTESTFORGE_BUILD_TESTS=${BUILD_TESTS} \
        -DTESTFORGE_BUILD_EXAMPLES=ON \
        -DTESTFORGE_BUILD_BENCHMARKS=ON \
    && cmake --build build --parallel

# Run the suite inside the image. A container that cannot pass its own tests
# should never be published.
RUN if [ "${BUILD_TESTS}" = "ON" ]; then ctest --test-dir build --output-on-failure; fi

# ---------------------------------------------------------------------------
# Stage 2: runtime
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# libstdc++ for the binary, and procps so the Linux diagnostics have
# something to report about beyond /proc.
#
# ca-certificates is deliberately NOT installed here. The built-in HTTP
# client links no TLS stack and refuses https:// outright (see
# HttpClient::Impl::perform), so a trust store in the runtime image would
# be cargo cult. The engine reaches the AI sidecar over plain HTTP on the
# compose network. The build stage still needs it, for FetchContent.
RUN apt-get update && apt-get install --no-install-recommends -y \
        libstdc++6 \
        procps \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --shell /usr/sbin/nologin testforge

WORKDIR /app

COPY --from=build /src/build/testforge      /usr/local/bin/testforge
COPY --from=build /src/build/testforge_bench /usr/local/bin/testforge_bench
COPY config/    /app/config/
COPY dashboard/ /app/dashboard/

# Reports and the results database are written at run time; give the
# unprivileged user somewhere to put them.
RUN mkdir -p /app/reports /app/data && chown -R testforge:testforge /app

# Not root: the diagnostics layer is explicitly designed to work without
# privileges, and running a test tool as root is an unnecessary risk.
USER testforge

ENV TESTFORGE_DB_PATH=/app/data/testforge.db \
    TESTFORGE_REPORT_DIR=/app/reports \
    TESTFORGE_SERVER_HOST=0.0.0.0

# 0.0.0.0 inside the container is fine — the container boundary is the network
# boundary, and docker-compose only publishes it to the host. See
# docs/security.md before exposing it further.
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD testforge version > /dev/null || exit 1

ENTRYPOINT ["testforge"]
CMD ["--help"]

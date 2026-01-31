FROM ubuntu:24.04 AS build

# Install dependencies.
RUN set -ex; \
    apt-get update; \
    apt-get install -y build-essential cmake tar git zip unzip curl pkg-config

# Copy source files.
COPY . /app

# Build.
WORKDIR /app/build
RUN cmake .. -DCMAKE_TOOLCHAIN_FILE=/app/libs/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release && make

FROM ubuntu:24.04

# Install dependencies.
RUN set -ex; \
    apt-get update; \
    apt-get install -y libssl3;

# Copy the build artificat.
COPY --from=build /app/build/bin/ardos /app/ardos
COPY --from=build /app/build/bin/libuv.s* /usr/local/lib/
COPY --from=build /usr/local/lib/lib* /usr/local/lib/

RUN ldconfig

# Run.
ENTRYPOINT ["./app/ardos"]

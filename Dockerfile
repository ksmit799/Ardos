FROM ubuntu:24.04 AS build

# Install dependencies.
RUN set -ex; \
    apt-get update; \
    apt-get install -y build-essential cmake tar git zip unzip curl pkg-config

# Copy source files.
COPY . /app

WORKDIR /app
RUN git submodule update --init --recursive

# Build.
WORKDIR /app/build
RUN cmake .. -DCMAKE_TOOLCHAIN_FILE=/app/libs/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release && make

FROM ubuntu:24.04

RUN set -ex; \
    apt-get update;

# Copy the build artificat.
COPY --from=build /app/build/bin/ardos /app/ardos

# Run.
ENTRYPOINT ["./app/ardos"]

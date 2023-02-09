FROM ubuntu:latest

# Install dependencies.
RUN set -ex; \
    apt-get update; \
    apt-get install -y cmake clang g++ gcc curl libuv1-dev;

# Copy source files.
COPY . /app
WORKDIR /app

# Setup directories.
RUN mkdir build
RUN mkdir repos

# Install/build MongoDB C Driver.
WORKDIR /app/repos
RUN curl -OL https://github.com/mongodb/mongo-c-driver/releases/download/1.23.2/mongo-c-driver-1.23.2.tar.gz
RUN tar -xzf mongo-c-driver-1.23.2.tar.gz
RUN mkdir mongo-c-driver-1.23.2/cmake-build
WORKDIR /app/repos/mongo-c-driver-1.23.2/cmake-build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_MONGODB_AWS_AUTH=OFF
RUN cmake --build . --target install

# Install/build MongoDB CXX Driver.
WORKDIR /app/repos
RUN curl -OL https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.7.0/mongo-cxx-driver-r3.7.0.tar.gz
RUN tar -xzf mongo-cxx-driver-r3.7.0.tar.gz
WORKDIR /app/repos/mongo-cxx-driver-r3.7.0/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_CXX_STANDARD=20
RUN cmake --build . --target install

# Build.
WORKDIR /app/build
RUN cmake .. && make

# Run.
ENTRYPOINT ["./build/ardos"]

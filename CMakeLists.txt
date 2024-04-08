cmake_minimum_required(VERSION 3.22)
project(ardos)

# C++ 20 standard.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Output binaries to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# Include OpenSSL.
find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIR})

# Include libuv.
add_subdirectory(libs/libuv)
include_directories(libs/libuv/include)

# Include uvw (libuv C++ wrapper).
include_directories(libs/uvw/src)

# Include JSON.
add_subdirectory(libs/json)
include_directories(libs/json/include)

# Include YAML-CPP.
add_subdirectory(libs/yaml-cpp)
include_directories(libs/yaml-cpp/include)

# Include AMQP-CPP (RabbitMQ).
add_subdirectory(libs/AMQP-CPP)
include_directories(libs/AMQP-CPP/include)

# Include spdlog.
add_subdirectory(libs/spdlog)
include_directories(libs/spdlog/include)

# Include Prometheus metrics.
find_package(prometheus-cpp CONFIG REQUIRED)

set(ARDOS_WANT_DB_SERVER ON CACHE BOOL "If on, Ardos will be built with the Database Server component.")
if (ARDOS_WANT_DB_SERVER)
    # Include MongoDB driver.
    find_package(mongocxx CONFIG REQUIRED)
    find_package(bsoncxx CONFIG REQUIRED)

    include_directories(${LIBMONGOCXX_INCLUDE_DIRS})
    include_directories(${LIBBSONCXX_INCLUDE_DIRS})

    add_definitions(-DARDOS_WANT_DB_SERVER)
endif ()

# Optimize windows build.
if (WIN32)
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS)
endif ()

# Ardos configuration variables.
set(ARDOS_USE_LEGACY_CLIENT OFF CACHE BOOL "If on, the Client Agent will be compatible with legacy OTP message types and clients.")
if (ARDOS_USE_LEGACY_CLIENT)
    add_definitions(-DARDOS_USE_LEGACY_CLIENT)
endif ()

# TODO: Don't use glob recurse.
# Each source directory should have its own CMAKE file.
file(GLOB_RECURSE ARDOS_SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.c")
file(GLOB_RECURSE ARDOS_HEADERS "${PROJECT_SOURCE_DIR}/src/*.h")

add_executable(ardos ${ARDOS_SOURCES} ${ARDOS_HEADERS})

# Include DC (Distributed Class) parser.
add_subdirectory(libs/dclass)
include_directories(libs/dclass)

target_link_libraries(ardos PRIVATE uv OpenSSL::SSL nlohmann_json::nlohmann_json yaml-cpp amqpcpp prometheus-cpp::pull)

if (ARDOS_WANT_DB_SERVER)
    target_link_libraries(ardos PRIVATE mongo::mongocxx_shared mongo::bsoncxx_shared)
endif ()

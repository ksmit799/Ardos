cmake_minimum_required(VERSION 3.22)
project(ardos)

# C++ 20 standard.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Output binaries to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# Find dependencies via vcpkg (or system packages).
find_package(OpenSSL REQUIRED)
find_package(libuv CONFIG REQUIRED)
find_package(uvw CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(amqpcpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)

set(ARDOS_WANT_DB_SERVER ON CACHE BOOL "If on, Ardos will be built with the Database Server component.")
if (ARDOS_WANT_DB_SERVER)
    # Include MongoDB driver.
    find_package(mongo-cxx-driver CONFIG REQUIRED)

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

target_link_libraries(ardos PRIVATE
    OpenSSL::SSL
    libuv::uv
    uvw::uvw
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    amqpcpp
    spdlog::spdlog
    prometheus-cpp::pull
)

if (ARDOS_WANT_DB_SERVER)
    target_link_libraries(ardos PRIVATE mongo::mongocxx_shared mongo::bsoncxx_shared)
endif ()

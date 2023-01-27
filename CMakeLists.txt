cmake_minimum_required(VERSION 3.24)
project(ardos)

# C++ 20 standard.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Output binaries to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# Include libuv.
add_subdirectory(libs/libuv)
include_directories(libs/libuv/include)

# Include uvw (libuv C++ wrapper).
include_directories(libs/uvw/src)

# Include YAML-CPP.
add_subdirectory(libs/yaml-cpp)
include_directories(libs/yaml-cpp/include)

# Include MongoDB driver.
find_package(libmongocxx REQUIRED)
find_package(libbsoncxx REQUIRED)

include_directories(${LIBMONGOCXX_INCLUE_DIR})
include_directories(${LIBBSONCXX_INCLUDE_DIR})

# Include AMQP-CPP (RabbitMQ).
add_subdirectory(libs/AMQP-CPP)
include_directories(libs/AMQP-CPP/include)

# Optimize windows build.
if (WIN32)
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS)
endif ()

# TODO: Don't use glob recurse.
# Each source directory should have its own CMAKE file.
file(GLOB_RECURSE ARDOS_SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.c")
file(GLOB_RECURSE ARDOS_HEADERS "${PROJECT_SOURCE_DIR}/src/*.h")

add_executable(ardos ${ARDOS_SOURCES} ${ARDOS_HEADERS})

# Include DC (Distributed Class) parser.
add_subdirectory(libs/dclass)
include_directories(libs/dclass)

target_link_libraries(ardos PRIVATE uv yaml-cpp ${LIBMONGOCXX_LIBRARIES} ${LIBBSONCXX_LIBRARIES} amqpcpp)
cmake_minimum_required(VERSION 3.16)
project(rb VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------- Options ----------------
option(RB_USE_POINTERS          "Use void* entries instead of uint32_t indices" OFF)
option(RB_SLOT_CACHELINE_PAD    "Pad slot stride up to a cache line"             OFF)
option(RB_SINGLE_THREADED       "Single-threaded / no atomics mode"              OFF)
option(RB_USE_ATOMICS           "Use C11 <stdatomic.h>"                          ON)
option(RB_ENABLE_STATS          "Compile in statistics counters"                 ON)
option(RB_ENABLE_THREAD_HELPERS "Build pthread spawn/join helper"                ON)
option(RB_ENABLE_NOTIFY         "Build eventfd/pipe wake-up helpers"             OFF)
option(RB_BUILD_TESTS           "Build unit tests"                               OFF)
option(RB_BUILD_BENCH           "Build benchmarks"                               OFF)

# ---------------- Numeric defaults ----------------
set(RB_SLOT_SIZE  2048 CACHE STRING "Slot size in bytes (incl. header)")
set(RB_NUM_SLOTS  64   CACHE STRING "Default scratchpad slot count")
set(RB_CAPACITY   64   CACHE STRING "Default ring capacity (power of two)")
set(RB_CACHE_LINE 64   CACHE STRING "Cache line size in bytes")

# ---------------- Library ----------------
add_library(rb STATIC
    src/rb.c
    $<$<BOOL:${RB_ENABLE_THREAD_HELPERS}>:src/rb_thread.c>
    $<$<BOOL:${RB_ENABLE_NOTIFY}>:src/rb_notify.c>)

target_include_directories(rb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(rb PUBLIC c_std_11)

target_compile_definitions(rb PUBLIC
    RB_SLOT_SIZE=${RB_SLOT_SIZE}
    RB_NUM_SLOTS=${RB_NUM_SLOTS}
    RB_CAPACITY=${RB_CAPACITY}
    RB_CACHE_LINE=${RB_CACHE_LINE}
    $<$<BOOL:${RB_USE_POINTERS}>:RB_USE_POINTERS=1>
    $<$<BOOL:${RB_SLOT_CACHELINE_PAD}>:RB_SLOT_CACHELINE_PAD=1>
    $<$<BOOL:${RB_SINGLE_THREADED}>:RB_SINGLE_THREADED=1>
    $<$<BOOL:${RB_USE_ATOMICS}>:RB_USE_ATOMICS=1>
    $<$<BOOL:${RB_ENABLE_STATS}>:RB_ENABLE_STATS=1>
    $<$<BOOL:${RB_ENABLE_THREAD_HELPERS}>:RB_ENABLE_THREAD_HELPERS=1>
    $<$<BOOL:${RB_ENABLE_NOTIFY}>:RB_ENABLE_NOTIFY=1>)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rb PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (RB_ENABLE_THREAD_HELPERS)
    find_package(Threads REQUIRED)
    target_link_libraries(rb PUBLIC Threads::Threads)
endif()

if (RB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (RB_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ---------------- Install ----------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS rb
    EXPORT rbTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES
    include/rb.h
    include/rb_config.h
    include/rb_port.h
    include/rb_thread.h
    include/rb_notify.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT rbTargets
    FILE rbTargets.cmake
    NAMESPACE rb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rb)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rb)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rbConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rb)

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
option(RB_ENABLE_NOTIFY         "Build futex/eventfd wake-up helpers"            ON)
option(RB_ENABLE_SIGNAL         "Build rb_signal primitive"                      OFF)
option(RB_SIZE_OPTIMIZED        "Prefer size over speed (-Os)"                   OFF)
option(RB_BUILD_TESTS           "Build unit tests"                               OFF)
option(RB_BUILD_BENCH           "Build benchmarks"                               OFF)
option(RB_BUILD_EXAMPLES        "Build examples"                                 OFF)

# ---------------- Numeric defaults ----------------
set(RB_SLOT_SIZE  2048 CACHE STRING "Slot size in bytes (incl. header)")
set(RB_NUM_SLOTS  64   CACHE STRING "Default scratchpad slot count")
set(RB_CAPACITY   64   CACHE STRING "Default ring capacity (power of two)")
set(RB_CACHE_LINE 64   CACHE STRING "Cache line size in bytes")

# ---------------- Core library — NO pthread ----------------
add_library(rb STATIC src/rb.c)

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
    $<$<BOOL:${RB_ENABLE_NOTIFY}>:RB_ENABLE_NOTIFY=1>)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rb PRIVATE
        -Wall -Wextra -Wpedantic
        -ffunction-sections -fdata-sections)
endif()

if (RB_SIZE_OPTIMIZED)
    target_compile_options(rb PRIVATE -Os)
endif()

# ---------------- Optional: thread helpers (separate static lib) ----------------
# Consumers who only use the ring link `rb`. Consumers who call
# rb_thread_spawn link `rb_thread_helpers`, which pulls in the helper
# library and Threads::Threads explicitly.
if (RB_ENABLE_THREAD_HELPERS)
    find_package(Threads REQUIRED)

    add_library(rb_thread STATIC src/rb_thread.c)
    target_link_libraries(rb_thread PRIVATE Threads::Threads)
    target_link_libraries(rb_thread PRIVATE rb)
    target_compile_definitions(rb_thread PUBLIC RB_ENABLE_THREAD_HELPERS=1)

    # Explicit opt-in target: the core `rb` target never propagates pthread.
    add_library(rb_thread_helpers INTERFACE)
    target_link_libraries(rb_thread_helpers INTERFACE rb_thread)

    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rb_thread PRIVATE
            -Wall -Wextra -Wpedantic
            -ffunction-sections -fdata-sections)
    endif()
endif()

# ---------------- Optional: signal primitive ----------------
if (RB_ENABLE_SIGNAL)
    target_sources(rb PRIVATE src/rb_signal.c)
    target_compile_definitions(rb PUBLIC RB_ENABLE_SIGNAL=1)
endif()

# ---------------- Subdirectories ----------------
if (RB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (RB_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if (RB_BUILD_EXAMPLES)
    add_subdirectory(examples)
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

if (RB_ENABLE_THREAD_HELPERS)
    install(TARGETS rb_thread rb_thread_helpers
        EXPORT rbTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES
    include/rb.h
    include/rb_config.h
    include/rb_port.h
    include/rb_thread.h
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

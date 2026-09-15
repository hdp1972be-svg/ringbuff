# Simple IPC demo (matches the tutorial in docs/API.md)
add_executable(ipc_writer ipc_writer.c)
target_link_libraries(ipc_writer PRIVATE rb rt)

add_executable(ipc_reader ipc_reader.c)
target_link_libraries(ipc_reader PRIVATE rb rt)

# Burst / overload demo (backpressure, drop policy)
add_executable(ipc_burst_writer ipc_burst_writer.c)
target_link_libraries(ipc_burst_writer PRIVATE rb rt)

add_executable(ipc_slow_reader ipc_slow_reader.c)
target_link_libraries(ipc_slow_reader PRIVATE rb rt)

# Notification demo (threads + futex)
add_executable(notify_threaded notify_threaded.c)
target_link_libraries(notify_threaded PRIVATE rb_thread)

# In-process backpressure demo
add_executable(backpressure_inproc backpressure_inproc.c)
target_link_libraries(backpressure_inproc PRIVATE rb_thread)

# Single-process backpressure benchmark (poll / futex / libuv)
add_executable(backpressure_bench backpressure_bench.c)
target_link_libraries(backpressure_bench PRIVATE rb_thread uv)

# Raw memcpy bandwidth reference (no IPC, no rb; measures the copy ceiling)
add_executable(ipc_bench_memcpy ipc_bench_memcpy.c)
target_link_libraries(ipc_bench_memcpy PRIVATE rt)
set_target_properties(ipc_bench_memcpy PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

# Two-process throughput benchmark (no artificial pacing, shared shm)
add_executable(ipc_bench_writer ipc_bench_writer.c)
target_link_libraries(ipc_bench_writer PRIVATE rb rt)

add_executable(ipc_bench_reader ipc_bench_reader.c)
target_link_libraries(ipc_bench_reader PRIVATE rb rt)


# GPU shared scratchpad demo (only if CUDA is available)
find_package(CUDAToolkit QUIET)
if (CUDAToolkit_FOUND)
    add_executable(gpu_shared gpu_shared.c)
    target_link_libraries(gpu_shared PRIVATE rb CUDA::cudart)
    message(STATUS "gpu_shared enabled (CUDA ${CUDAToolkit_VERSION})")
else()
    message(STATUS "gpu_shared skipped (no CUDA toolkit)")
endif()

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(t ipc_writer ipc_reader ipc_burst_writer ipc_slow_reader
                notify_threaded backpressure_inproc backpressure_bench
                ipc_bench_writer ipc_bench_reader ipc_bench_memcpy)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()


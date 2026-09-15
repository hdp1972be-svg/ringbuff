find_package(Threads REQUIRED)

add_executable(test_rb test_rb.c)
target_link_libraries(test_rb PRIVATE rb)

add_library(rb_per_slot STATIC ../src/rb.c)
target_include_directories(rb_per_slot PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include> $<INSTALL_INTERFACE:include>)
target_compile_features(rb_per_slot PUBLIC c_std_11)
target_compile_definitions(rb_per_slot PUBLIC
    RB_SLOT_SIZE=${RB_SLOT_SIZE} RB_NUM_SLOTS=${RB_NUM_SLOTS} RB_CAPACITY=${RB_CAPACITY} RB_CACHE_LINE=${RB_CACHE_LINE}
    RB_PER_SLOT_LAP=1
    $<$<BOOL:${RB_USE_POINTERS}>:RB_USE_POINTERS=1>
    $<$<BOOL:${RB_SLOT_CACHELINE_PAD}>:RB_SLOT_CACHELINE_PAD=1>
    $<$<BOOL:${RB_SINGLE_THREADED}>:RB_SINGLE_THREADED=1>
    $<$<BOOL:${RB_USE_ATOMICS}>:RB_USE_ATOMICS=1>
    $<$<BOOL:${RB_ENABLE_STATS}>:RB_ENABLE_STATS=1>
    $<$<BOOL:${RB_ENABLE_NOTIFY}>:RB_ENABLE_NOTIFY=1>
    $<$<BOOL:${RB_FUTEX_SHARED}>:RB_FUTEX_SHARED=1>)
target_compile_definitions(rb_per_slot PRIVATE RB_MEMCPY=${RB_MEMCPY} RB_MEMSET=${RB_MEMSET})

add_executable(test_rb_per_slot test_rb.c)
target_link_libraries(test_rb_per_slot PRIVATE rb_per_slot)

add_executable(test_rb_threaded test_rb_threaded.c)
target_link_libraries(test_rb_threaded PRIVATE rb_thread)

add_executable(test_rb_backpressure test_rb_backpressure.c)
target_link_libraries(test_rb_backpressure PRIVATE rb_thread)

add_executable(test_rb_signal test_rb_signal.c)
target_link_libraries(test_rb_signal PRIVATE rb Threads::Threads)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_rb            PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_rb_per_slot   PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_rb_threaded   PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_rb_backpressure PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_rb_signal     PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME rb_all          COMMAND test_rb)
add_test(NAME rb_per_slot     COMMAND test_rb_per_slot)
add_test(NAME rb_threaded     COMMAND test_rb_threaded)
add_test(NAME rb_backpressure COMMAND test_rb_backpressure)
add_test(NAME rb_signal       COMMAND test_rb_signal)

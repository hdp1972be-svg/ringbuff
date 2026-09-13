find_package(Threads REQUIRED)

add_executable(test_rb test_rb.c)
target_link_libraries(test_rb PRIVATE rb)

add_executable(test_rb_threaded test_rb_threaded.c)
target_link_libraries(test_rb_threaded PRIVATE rb_thread)

add_executable(test_rb_backpressure test_rb_backpressure.c)
target_link_libraries(test_rb_backpressure PRIVATE rb_thread)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_rb            PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_rb_threaded   PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_rb_backpressure PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME rb_all          COMMAND test_rb)
add_test(NAME rb_threaded     COMMAND test_rb_threaded)
add_test(NAME rb_backpressure COMMAND test_rb_backpressure)

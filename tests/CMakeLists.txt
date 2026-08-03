set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(ts_smart_ptrs_tests
    TestHarness.h
    TestTypes.h
    main.cpp
    tst_api.cpp
    tst_shared_ptr.cpp
    tst_weak_ptr.cpp
    tst_casts.cpp
    tst_copy_on_write.cpp
    tst_threading.cpp
    ../SafeC++.h
)

target_include_directories(ts_smart_ptrs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_smart_ptrs_tests PRIVATE Threads::Threads)

# One CTest entry per suite, the binary takes the suite name as its filter
foreach(suite SharedPtr WeakPtr Cast CopyOnWrite Api ScopedLock Threading)
    add_test(NAME ${suite} COMMAND ts_smart_ptrs_tests ${suite})
    # Make a sanitizer report fail the test instead of only printing a warning
    # a deadlock must fail the run instead of hanging it
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
    set_tests_properties(${suite} PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=abort_on_error=1")
endforeach()

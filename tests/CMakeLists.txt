set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(SafeCppTests
    TestHarness.h
    TestTypes.h
    OpaqueHandle.cpp
    main.cpp
    tst_api.cpp
    tst_aliasing.cpp
    tst_allocation.cpp
    tst_shared_ptr.cpp
    tst_weak_ptr.cpp
    tst_casts.cpp
    tst_copy_on_write.cpp
    tst_shared_copy_on_write.cpp
    tst_spin_lock.cpp
    tst_threading.cpp
    tst_throw_on_null.cpp
    tst_hash.cpp
    ../SafeCpp.h
)

# Built the other way from the rest, which the library allows within one program
set_source_files_properties(tst_throw_on_null.cpp PROPERTIES COMPILE_DEFINITIONS
    SAFECPP_THROW_ON_NULL=1)

# The tests consume the library exactly the way an external project does
target_link_libraries(SafeCppTests PRIVATE Safe::CPP Threads::Threads)

# Keep the test target's language requirement explicit.
target_compile_features(SafeCppTests PRIVATE cxx_std_23)

# One CTest entry per suite, the binary takes the suite name as its filter
foreach(suite SharedPtr WeakPtr Cast CopyOnWrite SharedCopyOnWrite SpinLock Api Aliasing Allocation
        ScopedLock Threading ThrowOnNull Hash)
    add_test(NAME ${suite} COMMAND SafeCppTests ${suite})
    # Make a sanitizer report fail the test instead of only printing a warning
    # a deadlock must fail the run instead of hanging it
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
    set_tests_properties(${suite} PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=abort_on_error=1")
endforeach()

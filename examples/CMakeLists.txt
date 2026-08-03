# The demo needs C++23 for <print>, the library itself only needs C++20
add_executable(SafeCppDemo demo.cpp)
target_link_libraries(SafeCppDemo PRIVATE Safe::CPP)
target_compile_features(SafeCppDemo PRIVATE cxx_std_23)

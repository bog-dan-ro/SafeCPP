# The demo uses <print>; Safe::CPP already exports the C++23 requirement.
add_executable(SafeCppDemo demo.cpp)
target_link_libraries(SafeCppDemo PRIVATE Safe::CPP)
target_compile_features(SafeCppDemo PRIVATE cxx_std_23)

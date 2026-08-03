cmake_minimum_required(VERSION 3.16)

project(ts_smart_ptrs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ts_smart_ptrs main.cpp
    SafeC++.h)

include(GNUInstallDirs)
install(TARGETS ts_smart_ptrs
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

option(BUILD_TESTING "Build the unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

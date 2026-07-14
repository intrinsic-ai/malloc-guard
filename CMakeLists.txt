cmake_minimum_required(VERSION 3.24)
project(malloc_guard_project CXX C) 

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RTSAN "Enable Clang RealtimeSanitizer (RTSan)" OFF)

if(ENABLE_RTSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RTSan requires the Clang compiler.")
    endif()
    add_compile_options(-fsanitize=realtime)
    add_link_options(-fsanitize=realtime)
    message(STATUS "RealtimeSanitizer (RTSan) enabled.")
endif()

find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(plthook URL https://github.com/metacall/plthook/archive/refs/tags/v0.2.0.tar.gz)
FetchContent_MakeAvailable(plthook)

add_library(plthook STATIC ${plthook_SOURCE_DIR}/plthook_elf.c)
set_target_properties(plthook PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(plthook PUBLIC ${plthook_SOURCE_DIR})
target_link_libraries(plthook PRIVATE ${CMAKE_DL_LIBS})

FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
FetchContent_MakeAvailable(googletest)

add_library(malloc_guard SHARED
    src/malloc_guard.cc
    src/malloc_guard_plthook_elf.cc
)
target_include_directories(malloc_guard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(malloc_guard PRIVATE ${plthook_SOURCE_DIR})
target_link_libraries(malloc_guard PUBLIC Threads::Threads ${CMAKE_DL_LIBS} PRIVATE plthook)

enable_testing()
add_executable(malloc_guard_test test/malloc_guard_test.cc)
target_link_libraries(malloc_guard_test PRIVATE malloc_guard gtest_main gmock ${CMAKE_DL_LIBS})
add_test(NAME malloc_guard_test COMMAND malloc_guard_test)

add_library(malloc_guard_test_lib SHARED test/malloc_guard_test_lib.cc)
add_library(malloc_guard_test_lib_allowed SHARED test/malloc_guard_test_lib.cc)
add_library(malloc_guard_test_custom_malloc_lib SHARED test/malloc_guard_test_custom_malloc.cc test/malloc_guard_test_lib.cc)
target_link_libraries(malloc_guard_test_custom_malloc_lib PRIVATE malloc_guard gtest_main ${CMAKE_DL_LIBS})


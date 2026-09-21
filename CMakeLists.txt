# Copyright 2026 Intrinsic Innovation LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.24)
project(malloc_guard_project CXX C) 

option(ENABLE_RTSAN "Enable Clang RealtimeSanitizer (RTSan)" OFF)

if(ENABLE_RTSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RTSan requires the Clang compiler.")
    endif()
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

include(CTest)

if(BUILD_TESTING)
    FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
endif()

add_library(malloc_guard SHARED
    src/malloc_guard.cc
    src/malloc_guard_plthook_elf.cc
)

target_compile_features(malloc_guard PUBLIC cxx_std_20)
add_library(malloc_guard::malloc_guard ALIAS malloc_guard)

if(ENABLE_RTSAN)
    target_compile_options(malloc_guard PUBLIC -fsanitize=realtime)
    target_link_options(malloc_guard PUBLIC -fsanitize=realtime)
endif()

target_include_directories(malloc_guard PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_include_directories(malloc_guard PRIVATE ${plthook_SOURCE_DIR})
target_link_libraries(malloc_guard PUBLIC Threads::Threads ${CMAKE_DL_LIBS} PRIVATE plthook)

if(BUILD_TESTING)
    add_executable(malloc_guard_test test/malloc_guard_test.cc)
    target_link_libraries(malloc_guard_test PRIVATE malloc_guard gtest_main gmock ${CMAKE_DL_LIBS})
    target_compile_features(malloc_guard_test PRIVATE cxx_std_20)
    add_test(NAME malloc_guard_test COMMAND malloc_guard_test)

    add_executable(malloc_guard_static_allocator_hook_test test/malloc_guard_static_allocator_hook_test.cc)
    target_link_libraries(malloc_guard_static_allocator_hook_test PRIVATE malloc_guard gtest_main gmock ${CMAKE_DL_LIBS})
    target_compile_features(malloc_guard_static_allocator_hook_test PRIVATE cxx_std_20)
    add_test(NAME malloc_guard_static_allocator_hook_test COMMAND malloc_guard_static_allocator_hook_test)

    add_library(malloc_guard_test_lib SHARED test/malloc_guard_test_lib.cc)
    target_compile_features(malloc_guard_test_lib PRIVATE cxx_std_20)
    
    add_library(malloc_guard_test_lib_allowed SHARED test/malloc_guard_test_lib.cc)
    target_compile_features(malloc_guard_test_lib_allowed PRIVATE cxx_std_20)
    
    add_library(malloc_guard_test_custom_malloc_lib SHARED test/malloc_guard_test_custom_malloc.cc test/malloc_guard_test_lib.cc)
    target_link_libraries(malloc_guard_test_custom_malloc_lib PRIVATE malloc_guard gtest_main ${CMAKE_DL_LIBS})
    target_compile_features(malloc_guard_test_custom_malloc_lib PRIVATE cxx_std_20)
endif()

include(GNUInstallDirs)
install(TARGETS malloc_guard
    EXPORT malloc_guardTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT malloc_guardTargets
    FILE malloc_guardTargets.cmake
    NAMESPACE malloc_guard::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/malloc_guard
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "malloc_guardConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/malloc_guardConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/malloc_guardConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/malloc_guard
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/malloc_guardConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/malloc_guardConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/malloc_guard
)

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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

cc_library(
    name = "malloc_guard",
    srcs = [
        "src/malloc_guard.cc",
        "src/malloc_guard_plthook_elf.cc",
    ],
    hdrs = [
        "include/intrinsic/malloc_guard.h",
        "include/intrinsic/malloc_guard_plthook_elf.h",
    ],
    strip_include_prefix = "include",
    deps = ["@plthook"],
)

cc_test(
    name = "malloc_guard_test",
    srcs = ["test/malloc_guard_test.cc"],
    data = [
        ":libmalloc_guard_test_custom_malloc_lib.so",
        ":libmalloc_guard_test_lib.so",
        ":libmalloc_guard_test_lib_allowed.so",
    ],
    linkopts = ["-ldl"],
    deps = [
        ":malloc_guard",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "malloc_guard_static_allocator_hook_test",
    srcs = ["test/malloc_guard_static_allocator_hook_test.cc"],
    linkopts = ["-ldl"],
    deps = [
        ":malloc_guard",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "libmalloc_guard_test_lib.so",
    srcs = ["test/malloc_guard_test_lib.cc"],
    linkshared = True,
)

cc_binary(
    name = "libmalloc_guard_test_lib_allowed.so",
    srcs = ["test/malloc_guard_test_lib.cc"],
    linkshared = True,
)

cc_binary(
    name = "libmalloc_guard_test_custom_malloc_lib.so",
    srcs = [
        "test/malloc_guard_test_custom_malloc.cc",
        "test/malloc_guard_test_lib.cc",
    ],
    linkshared = True,
)

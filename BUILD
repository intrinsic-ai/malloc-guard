load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test", "cc_binary")

package(
    default_visibility = ["//visibility:public"])

cc_library(
    name = "malloc_guard",
    srcs = [
        "src/malloc_guard.cc",
        "src/malloc_guard_plthook_elf.cc",
    ],
    hdrs = [
        "include/malloc_guard.h",
        "include/malloc_guard_plthook_elf.h",
    ],
    strip_include_prefix = "include",
    deps = ["@plthook"],
)

cc_test(
    name = "malloc_guard_test",
    srcs = ["test/malloc_guard_test.cc"],
    deps = [
        ":malloc_guard",
        "@com_google_googletest//:gtest_main",
    ],
    data = [
        ":libmalloc_guard_test_lib.so",
        ":libmalloc_guard_test_lib_allowed.so",
        ":libmalloc_guard_test_custom_malloc_lib.so",
    ],
    linkopts = ["-ldl"],
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


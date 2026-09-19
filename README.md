# malloc-guard

`malloc-guard` is a C++ library designed for real-time applications where dynamic heap memory allocation is strictly forbidden. It uses dynamic GOT/PLT hooking to intercept standard memory allocator calls (`malloc`, `free`, `realloc`, `calloc`, etc.) and allows you to enforce configurable reactions (e.g. Abort, Log, Store Violations, or Custom Callbacks) when unexpected allocations occur in real-time threads.

## Installation

### Prerequisites
* C++20 compatible compiler (e.g., clang, gcc)
* CMake 3.24+ or Bazel build system

### Integration via CMake

#### Option 1: FetchContent
You can include this library directly in your CMake project using `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    malloc_guard
    GIT_REPOSITORY https://github.com/intrinsic-ai/malloc-guard.git
    GIT_TAG        main # Or a specific tag/commit hash
)
FetchContent_MakeAvailable(malloc_guard)

add_executable(my_application main.cc)
target_link_libraries(my_application PRIVATE malloc_guard::malloc_guard)
```

#### Option 2: Build and Install
Alternatively, you can build and install the library system-wide or to a custom prefix:

```bash
mkdir build && cd build
cmake .. 
make -j
# To install locally or system-wide
sudo make install
```

Once installed (either globally or locally via `CMAKE_INSTALL_PREFIX`), you can link against the library in your projects:

```cmake
find_package(malloc_guard REQUIRED)

add_executable(my_application main.cc)
target_link_libraries(my_application PRIVATE malloc_guard::malloc_guard)
```

### Integration via Bazel

Add this repository to your Bazel workspace's `MODULE.bazel`:

```bazel
bazel_dep(name = "malloc_guard")

# If using a local checkout:
local_path_override(
    module_name = "malloc_guard",
    path = "path/to/malloc-guard",
)

# Or, if pulling directly from git:
# git_override(
#     module_name = "malloc_guard",
#     remote = "https://github.com/intrinsic-ai/malloc-guard.git",
#     commit = "<commit_hash>",
# )
```

Then depend on the target natively in your `BUILD` files:

```bazel
cc_binary(
    name = "my_application",
    srcs = ["main.cc"],
    deps = ["@malloc_guard//:malloc_guard"],
)
```

## Usage

### Import the library

Include the main header in your C++ source files:

```cpp
#include "intrinsic/malloc_guard.h"
```

### Protect real-time sections

You first need to enable the malloc hooks globally for the lifetime of your real-time threads. Then, use `intrinsic::MallocGuard` inside the real-time thread to intercept allocations:

```cpp
// 1. Install the global hooks (do this outside your real-time section)
intrinsic::ScopedMallocGuardHook hook;

void MyRealtimeFunction() {
  // 2. Set the reaction to abort for any allocations
  intrinsic::SetThreadLocalMallocGuardReaction(intrinsic::MallocGuardReaction::kAbort);
  
  // 3. Activate monitoring for this thread
  intrinsic::MallocGuard guard; 
  
  while (true) {
    // Critical real-time work
    // Any call to malloc/new here will trigger the abort reaction.
  }
}
```

---

## Documentation and related repositories

* [**Intrinsic Developer Community**](https://developer.intrinsic.ai): Complete guides, interactive tutorials, and API references.

---

## Contributing and community

Contributions are welcome! Please review:

* [CONTRIBUTING.md](CONTRIBUTING.md): Details on signing the Google Contributor License Agreement (CLA), community guidelines, C++20 coding standards, and pull request workflows.  
* [SECURITY.md](SECURITY.md): Instructions for reporting security vulnerabilities.

---

## License

This project is licensed under the [Apache 2.0 License](LICENSE).

---

> **Disclaimer**: This is not an officially supported Google product.

---

### Trademark notice

"Intrinsic" and "Intrinsic Core" are trademarks of Intrinsic Innovation LLC. See [TRADEMARK.md](TRADEMARK.md) for usage guidelines.

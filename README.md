# malloc-guard

`malloc-guard` is a C++ library designed for real-time applications where dynamic heap memory allocation is strictly forbidden. It uses dynamic GOT/PLT hooking to intercept standard memory allocator calls (`malloc`, `free`, `realloc`, `calloc`, etc.) and allows you to enforce configurable reactions (e.g. Abort, Log, Store Violations, or Custom Callbacks) when unexpected allocations occur in real-time threads.

## Installation

### Prerequisites
* C++20 compatible compiler (e.g., clang, gcc)
* CMake 3.24+ or Bazel build system

### Integration via CMake

**1. Build and Install**
```bash
mkdir build && cd build
cmake .. 
make -j
# To install locally or system-wide
sudo make install
```

**2. Link in downstream projects**
Once installed (either globally or locally via `CMAKE_INSTALL_PREFIX`), you can seamlessly combine the library into your projects:

```cmake
find_package(malloc_guard REQUIRED)

add_executable(my_application main.cc)
target_link_libraries(my_application PRIVATE malloc_guard::malloc_guard)
```

### Integration via Bazel

Add this repository to your Bazel workspace's `MODULE.bazel`:

```bazel
bazel_dep(name = "malloc_guard_playground")
local_path_override(
    module_name = "malloc_guard_playground",
    path = "path/to/malloc-guard",
)
```

Then depend on the target natively in your `BUILD` files:

```bazel
cc_binary(
    name = "my_application",
    srcs = ["main.cc"],
    deps = ["@malloc_guard_playground//:malloc_guard"],
)
```

## Usage

### Import the library

Include the main header in your C++ source files:

```cpp
#include "malloc_guard.h"
```

### Protect real-time sections

You first need to enable the malloc hooks globally for the lifetime of your real-time threads. Then, use `intrinsic::MallocGuard` inside the real-time thread to intercept allocations:

```cpp
// 1. Install the global hooks (do this outside your real-time section)
intrinsic::ScopedMallocGuardHook hook;

void MyRealtimeFunction() {
  // 2. Activate monitoring for this thread
  intrinsic::MallocGuard guard; 
  
  // 3. Set the reaction to abort for any allocations
  intrinsic::SetThreadLocalMallocGuardReaction(intrinsic::MallocGuardReaction::kAbort);
  
  while (true) {
    // Critical real-time work
    // Any call to malloc/new here will trigger the abort reaction.
  }
}
```

## How to contribute
If you'd like to contribute, please review the [How to Contribute](./CONTRIBUTING.md) guide.

## License
Licensed under the Apache License, Version 2.0. See the [LICENSE](LICENSE) file for details.

## Disclaimer
This is not an officially supported Google product. This project is not eligible for the [Google Open Source Software Vulnerability Rewards Program](https://bughunters.google.com/open-source-security).
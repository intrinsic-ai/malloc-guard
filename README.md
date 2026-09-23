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

## Testing

The section above covers using `malloc-guard` from another project. To build and
test the library itself, use either build system. Both sets of commands build
everything and run the full test suite.

### CMake

```bash
cmake -B build
cmake --build build --parallel
cd build && ctest --output-on-failure
```

### Bazel

```bash
bazel test //...
```

Note that the tests `dlopen()` shared libraries from the current working
directory. Run them through `ctest` or `bazel test`, which take care of this. If
you invoke a test binary directly, do so from the directory that contains the
test's shared libraries.

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

### Statically-linked allocators

Interception normally works by rewriting GOT/PLT entries, which only covers
allocator calls that go through the dynamic linker. Most projects link their
allocator dynamically and need nothing beyond the setup above. That includes
replacement allocators such as TCMalloc, jemalloc or mimalloc, as long as they
are loaded as shared libraries.

If your binary instead links its allocator **statically**, those call sites are
bound at link time and cannot be hooked that way.

For this case, link a translation unit that defines the following two functions
in the `intrinsic` namespace. MallocGuard picks them up automatically if they
are present, and calls them when hooks are installed and uninstalled:

```cpp
namespace intrinsic {
namespace {

// Forwards every allocation to MallocGuard. Your allocator calls this on each
// allocation, including outside of guarded scopes, so it bails out as early as
// possible.
void MyHook(size_t requested_size) {
  if (!AreMallocGuardHooksInstalled() || !MallocGuard::IsMallocGuarded())
      [[likely]] {
    return;
  }
  ReportAllocation(requested_size);
}

}  // namespace

// Register your allocator's native allocation hook here.
bool MallocGuardCustomSetup() {
  return MyAllocator::AddAllocationHook(&MyHook);
}

// Undo whatever MallocGuardCustomSetup() did.
bool MallocGuardCustomTeardown() {
  return MyAllocator::RemoveAllocationHook(&MyHook);
}

}  // namespace intrinsic
```

You must define **both** functions or neither; defining only one is a fatal
error. `MallocGuardCustomSetup()` and `MallocGuardCustomTeardown()` run in
non-real-time contexts, but the hook itself must assume it runs in a real-time
context and must not block.

### Limitations

MallocGuard detects the following unsupported setups and fails fast with a
diagnostic rather than silently under-reporting allocations:

* **`RTLD_DEEPBIND`** — libraries loaded with this flag prefer their own symbol
  definitions and bypass interception.
* **A dlopen'd library with its own allocator** — the library would allocate
  through an allocator MallocGuard does not observe.
* **`dlmopen()` with `LM_ID_NEWLM`** — the new namespace resolves to a different
  allocator.
* **Denylisting combined with a statically-linked allocator** — a native
  allocator hook fires process-wide and cannot attribute an allocation to the
  library that requested it, so the two features are mutually exclusive.

Note also that denylisting a library only suppresses allocations made from call
sites *inside* that library. If it allocates via `new`, the allocation is
actually performed by the C++ runtime, which is still hooked unless it is
denylisted too.

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

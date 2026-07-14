#ifndef INTRINSIC_MALLOC_GUARD_PLTHOOK_ELF_H_
#define INTRINSIC_MALLOC_GUARD_PLTHOOK_ELF_H_

#include <span>

namespace intrinsic {

struct HookTarget {
  const char* name;
  void* hook_func;
};

// Installs dynamic GOT/PLT hooks for the malloc_guard interceptor functions.
// This iterates over all loaded shared libraries and the main executable, then
// rewrites the GOT/PLT entries for libc memory allocation functions (such as
// malloc, free, dlopen) to point to the malloc_guard interceptors, unless
// the library is explicitly denylisted.
//
// targets: specifies a list of function names (e.g., "malloc") and their
// corresponding custom interceptor functions to be hooked.
//
// Returns true on success.
bool InstallDynamicGotHooks(std::span<const HookTarget> targets);

// Uninstalls dynamic GOT/PLT hooks that were previously installed.
// This iterates over all loaded shared libraries and the main executable, and
// restores the GOT/PLT entries for libc memory allocation functions back to
// their original real libc functions.
//
// targets: specifies the list of function names (e.g., "malloc") and their
// corresponding interceptor functions that were previously hooked.
//
// Returns true on success.
bool UninstallDynamicGotHooks(std::span<const HookTarget> targets);
}  // namespace intrinsic

#endif  // INTRINSIC_MALLOC_GUARD_PLTHOOK_ELF_H_

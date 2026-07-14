#include "malloc_guard_plthook_elf.h"

#include <assert.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "malloc_guard.h"
#include "plthook.h"

namespace intrinsic {

// Map to store the original functions per library that are being
// overwritten by the hooking mechanism
static std::unordered_map<std::string, std::unordered_map<std::string, void*>>
    original_functions_map;
static std::mutex original_functions_mutex;

// Callback executed by dl_iterate_phdr for each loaded shared library.
// Opens each library's GOT/PLT using plthook, checks if the
// library has been denylisted to be checked by the malloc_guard. If not,
// rewrites the pointers for the libc allocator functions to point to
// malloc_guard's interceptor functions.
//
// Returns 0 to continue to the next library, or 1 to abort iteration.
static int HookLibraryCallback(struct dl_phdr_info* info, size_t size,
                               void* data) {
  plthook_t* plthook = nullptr;
  const char* libname = info->dlpi_name;
  std::string libname_str = libname ? libname : "";

  {
    std::lock_guard<std::mutex> lock(original_functions_mutex);
    auto [it, inserted] = original_functions_map.insert({libname_str, {}});
    if (!inserted) {
      return 0;
    }
  }

  // dlpi_name can be empty for the main executable
  if (libname == nullptr || libname[0] == '\0') {
    if (plthook_open(&plthook, nullptr) != 0) {
      RtSafeLog("plthook_open failed for main executable: ", plthook_error());
      return 0;
    }
  } else {
    if (std::strcmp(libname, "linux-vdso.so.1") == 0) {
      return 0;
    }
    if (GetMallocGuardDenylist().contains(std::string(libname))) {
      return 0;
    }
    if (plthook_open(&plthook, libname) != 0) {
      RtSafeLog("plthook_open failed for ", libname, ": ", plthook_error());
      return 0;
    }
  }

  auto* targets = static_cast<std::span<const HookTarget>*>(data);
  if (targets) {
    assert(plthook != nullptr);
    for (const auto& target : *targets) {
      void* oldfunc = nullptr;
      int status =
          plthook_replace(plthook, target.name, target.hook_func, &oldfunc);

      if (status == PLTHOOK_SUCCESS) {
        std::lock_guard<std::mutex> map_lock(original_functions_mutex);
        original_functions_map[libname ? libname : ""][target.name] = oldfunc;
      } else if (status != PLTHOOK_FUNCTION_NOT_FOUND) {
        // PLTHOOK_FUNCTION_NOT_FOUND is expected and safe to ignore, since many
        // libraries do not call every target function (e.g., they might not
        // call dlopen) and therefore will not have a PLT entry for it.
        RtSafeLog("plthook_replace failed for ", target.name, " in ",
                  libname && libname[0] ? libname : "main executable", ": ",
                  plthook_error(), "");
      }
    }
  }

  plthook_close(plthook);
  return 0;
}

// Opens each library's GOT/PLT using plthook, checks if the
// library has been denylisted to be checked by the malloc_guard. If not,
// rewrites the pointers for the libc allocator functions back to their original
// unhooked state.
//
// Returns 0 to continue to the next library, or 1 to abort iteration.
static int UnhookLibraryCallback(struct dl_phdr_info* info, size_t size,
                                 void* data) {
  plthook_t* plthook = nullptr;
  const char* libname = info->dlpi_name;

  if (libname == nullptr || libname[0] == '\0') {
    if (plthook_open(&plthook, nullptr) != 0) {
      RtSafeLog("plthook_open failed for main executable: ", plthook_error());
      return 0;
    }
  } else {
    if (std::strcmp(libname, "linux-vdso.so.1") == 0) {
      return 0;
    }
    if (GetMallocGuardDenylist().contains(std::string(libname))) {
      return 0;
    }
    if (plthook_open(&plthook, libname) != 0) {
      RtSafeLog("plthook_open failed for ", libname, ": ", plthook_error());
      return 0;
    }
  }

  auto* targets = static_cast<std::span<const HookTarget>*>(data);
  if (targets) {
    assert(plthook != nullptr);
    for (const auto& target : *targets) {
      void* real_func = nullptr;
      {
        std::lock_guard<std::mutex> map_lock(original_functions_mutex);
        auto lib_it = original_functions_map.find(libname ? libname : "");
        if (lib_it != original_functions_map.end()) {
          auto func_it = lib_it->second.find(target.name);
          if (func_it != lib_it->second.end()) {
            real_func = func_it->second;
          }
        }
      }

      if (!real_func) {
        // Fallback if not found in our map
        real_func = dlsym(RTLD_DEFAULT, target.name);
      }

      if (real_func) {
        int status = plthook_replace(plthook, target.name, real_func, nullptr);
        if (status != PLTHOOK_SUCCESS && status != PLTHOOK_FUNCTION_NOT_FOUND) {
          RtSafeLog("plthook_replace unhook failed for ", target.name, " in ",
                    libname && libname[0] ? libname : "main executable", ": ",
                    plthook_error(), "");
        }
      }
    }
  }

  plthook_close(plthook);
  return 0;
}

bool InstallDynamicGotHooks(std::span<const HookTarget> targets) {
  // Looping over all shared libraries and the main executable
  if (dl_iterate_phdr(HookLibraryCallback, &targets) != 0) {
    RtSafeLog("Failed to install custom GOT hooks for dynamic loading.");
    return false;
  }
  RtSafeLog("Custom GOT hooks successfully installed for dynamic loading.");
  return true;
}

bool UninstallDynamicGotHooks(std::span<const HookTarget> targets) {
  if (dl_iterate_phdr(UnhookLibraryCallback, &targets) != 0) {
    RtSafeLog("Failed to remove custom GOT hooks.");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(original_functions_mutex);
    original_functions_map.clear();
  }
  RtSafeLog("Custom GOT hooks successfully removed.");
  return true;
}

}  // namespace intrinsic

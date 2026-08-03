// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "intrinsic/malloc_guard.h"

#include <asm/unistd_64.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "intrinsic/malloc_guard_plthook_elf.h"

extern "C" {
void* malloc_guard_intercept_malloc(std::size_t);
void* malloc_guard_intercept_calloc(std::size_t, std::size_t);
void* malloc_guard_intercept_realloc(void*, std::size_t);
void malloc_guard_intercept_free(void*);
int malloc_guard_intercept_posix_memalign(void**, std::size_t, std::size_t);
void* malloc_guard_intercept_dlopen(const char*, int);
void* malloc_guard_intercept_dlmopen(Lmid_t, const char*, int);
}

namespace intrinsic {

static const HookTarget kTargets[] = {
    {"malloc", (void*)malloc_guard_intercept_malloc},
    {"calloc", (void*)malloc_guard_intercept_calloc},
    {"realloc", (void*)malloc_guard_intercept_realloc},
    {"free", (void*)malloc_guard_intercept_free},
    {"posix_memalign", (void*)malloc_guard_intercept_posix_memalign},
    {"dlopen", (void*)malloc_guard_intercept_dlopen},
    {"dlmopen", (void*)malloc_guard_intercept_dlmopen}};

static constexpr const char* kAllocatorSymbols[] = {
    "malloc", "calloc", "realloc", "free", "posix_memalign"};

namespace {

// Count of how many times the malloc guard was enabled. Decreased when it was
// disabled again.
volatile std::atomic_int malloc_guard_install_counter = 0;
static_assert(decltype(malloc_guard_install_counter)::is_always_lock_free);
// Mutex to protect concurrent enabling/disabling of the malloc guard.
std::mutex malloc_guard_install_counter_mutex;

std::unordered_set<std::string> malloc_guard_denylist;
std::mutex malloc_guard_denylist_mutex;

// The global malloc guard reaction that is used when no thread local reaction
// is set.
volatile std::atomic<MallocGuardReaction> global_malloc_guard_reaction =
    MallocGuardReaction::kLog;
static_assert(decltype(global_malloc_guard_reaction)::is_always_lock_free);
// Thread local malloc guard reaction that takes precedence over the global
// reaction, but is optional.
thread_local std::optional<MallocGuardReaction>
    thread_local_malloc_guard_reaction = {};

// Custom callback storage
std::mutex global_malloc_guard_callback_mutex;
MallocGuardCallback global_malloc_guard_callback = nullptr;
thread_local MallocGuardCallback thread_local_malloc_guard_callback = nullptr;

// Keeps count how many MallocGuards were instantiated in a single thread.
// This variable needs to be an atomic. Otherwise, it can happen that the
// counter value is read before it is written in the malloc hook (maybe because
// the malloc function is exchanged at link time?). This issues occurred in one
// of the checks of the `CheckMallocHandlerInstalled()` function.
std::atomic_int64_t thread_local local_malloc_guard_counter = 0;
static_assert(decltype(local_malloc_guard_counter)::is_always_lock_free);
// Global variables to make sure that the compiler doesn't
// optimize the code used for testing that the malloc hook works.
void* volatile ensure_hooks_registered_malloc_ptr;
int* volatile ensure_hooks_registered_new_ptr;
void* ensure_hooks_registered_posix_memalign_ptr;

MallocGuardViolations& GetThreadLocalMallocViolationsPrivate() {
  static thread_local MallocGuardViolations malloc_guard_violations = {};
  return malloc_guard_violations;
}
constexpr size_t kMaxPosixThreadNameLen = 16;

std::array<char, kMaxPosixThreadNameLen> GetNameOfCurrentThread() {
  std::array<char, kMaxPosixThreadNameLen> buf{};
  int res =
      pthread_getname_np(pthread_self(), buf.data(), kMaxPosixThreadNameLen);
  if (res != 0) {
    buf[0] = '\0';
  }
  return buf;
}

static void HandleMallocGuardViolation(size_t size) {
  static thread_local bool in_malloc_hook = false;
  // Prevent recursive loop due to printing in MallocGuardReaction::kAbort mode.
  if (in_malloc_hook) {
    return;
  }
  in_malloc_hook = true;
  if (MallocGuard::IsMallocGuarded()) [[unlikely]] {
    const MallocGuardReaction reaction = GetCurrentMallocGuardReaction();

    switch (reaction) {
      case MallocGuardReaction::kNone:
        break;
      case MallocGuardReaction::kLog: {
        auto name = GetNameOfCurrentThread();
        RtSafeLog("ERROR: Encountered malloc in realtime thread '", name.data(),
                  "'. Allocated ", size, " bytes.");
        break;
      }
      case MallocGuardReaction::kLogWithTrace: {
        auto name = GetNameOfCurrentThread();
        RtSafeLog("ERROR: Encountered malloc in realtime thread '", name.data(),
                  "'. Allocated ", size, " bytes.");
        void* buffer[16];
        int nptrs = backtrace(buffer, 16);
        backtrace_symbols_fd(buffer, nptrs, STDERR_FILENO);
        break;
      }
      case MallocGuardReaction::kStoreViolationWithTrace: {
        MallocGuardViolations& violations =
            GetThreadLocalMallocViolationsPrivate();
        auto start = std::chrono::steady_clock::now();
        StackTrace stack_trace;
        stack_trace.num_frames =
            backtrace(stack_trace.stack_trace, kMaxMallocGuardStackFrames);
        violations.latest_violation_stack_trace = stack_trace;
        auto dur = std::chrono::steady_clock::now() - start;
        violations.stack_creation_duration = dur;
      }
        [[fallthrough]];
      case MallocGuardReaction::kStoreViolation: {
        MallocGuardViolations& violations =
            GetThreadLocalMallocViolationsPrivate();
        violations.num_violations++;
        violations.allocated_bytes += size;
      } break;
      case MallocGuardReaction::kAbort: {
        auto name = GetNameOfCurrentThread();
        RtSafeLog("FATAL: Encountered malloc in realtime thread '", name.data(),
                  "'. Bailing out.");
        std::exit(1);
        break;
      }
      case MallocGuardReaction::kCustomCallback: {
        MallocGuardCallback cb = thread_local_malloc_guard_callback;
        if (cb) {
          cb(size);
        } else {
          std::lock_guard<std::mutex> lock(global_malloc_guard_callback_mutex);
          if (global_malloc_guard_callback) {
            global_malloc_guard_callback(size);
          }
        }
        break;
      }
      default:
        break;
    }
  }
  in_malloc_hook = false;
}

}  // end anonymous namespace

namespace internal {

void RtSafeLogImpl(const char* msg) {
  (void)::write(STDERR_FILENO, msg, std::strlen(msg));
}

void RtSafeLogImpl(std::string_view msg) {
  (void)::write(STDERR_FILENO, msg.data(), msg.size());
}

void RtSafeLogImpl(size_t val) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%zu", val);
  RtSafeLogImpl(std::string_view(buf));
}

void RtSafeLogImpl(int64_t val) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%ld", val);
  RtSafeLogImpl(std::string_view(buf));
}

void RtSafeLogImpl(const void* ptr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%p", ptr);
  RtSafeLogImpl(std::string_view(buf));
}

}  // namespace internal

bool CheckMallocHandlerInstalled(bool silent) {
  bool success = true;
  ResetThreadLocalMallocViolations();
  auto installed_reaction = thread_local_malloc_guard_reaction;
  thread_local_malloc_guard_reaction = MallocGuardReaction::kStoreViolation;

  MallocGuard guard;
  {  // do a malloc
    ensure_hooks_registered_malloc_ptr = malloc(32);

    // ensure we saw the malloc
    if (GetThreadLocalMallocViolationsPrivate().num_violations != 1) {
      if (!silent) {
        RtSafeLog(
            "malloc() handler not installed correctly: "
            "call to raw malloc did not increment counter.");
      }
      success = false;
    }
    if (GetThreadLocalMallocViolationsPrivate().allocated_bytes != 32) {
      if (!silent) {
        RtSafeLog(
            "malloc() handler not installed correctly: "
            "call to raw malloc did not count 32 bytes of requested memory.");
      }
      success = false;
    }
    // free the memory
    free(const_cast<void*>(ensure_hooks_registered_malloc_ptr));
  }

  {  // do a new
    ensure_hooks_registered_new_ptr = new int;

    // ensure we saw the new
    if (GetThreadLocalMallocViolationsPrivate().num_violations != 2) {
      if (!silent) {
        RtSafeLog(
            "malloc() handler not installed "
            "correctly: call to new did not increment counter.");
      }
      success = false;
    }
    if (GetThreadLocalMallocViolationsPrivate().allocated_bytes !=
        32 + sizeof(int)) {
      if (!silent) {
        RtSafeLog(
            "malloc() handler not installed "
            "correctly: call to new did not count correct new bytes for an "
            "int.");
      }
      success = false;
    }
    // free the memory
    delete ensure_hooks_registered_new_ptr;
  }

  {  // check posix aligned memory allocation
    constexpr size_t kAlignBytes = 16;
    constexpr size_t kSize = 32;
    if (posix_memalign(&ensure_hooks_registered_posix_memalign_ptr, kAlignBytes,
                       kSize) != 0) {
      if (!silent) {
        RtSafeLog("posix_memalign() failed.");
      }
    }

    // ensure we saw the malloc
    if (GetThreadLocalMallocViolationsPrivate().num_violations != 3) {
      if (!silent) {
        RtSafeLog(
            "posix_memalign() handler not installed "
            "correctly: direct call to posix_memalign did not "
            "increment counter.");
      }
      success = false;
    }
    // free the memory
    free(ensure_hooks_registered_posix_memalign_ptr);
  }

  {  // check realloc is caught
    ensure_hooks_registered_malloc_ptr = malloc(32);
    ensure_hooks_registered_malloc_ptr =
        realloc(const_cast<void*>(ensure_hooks_registered_malloc_ptr), 64);
    // ensure we saw the realloc and malloc
    if (GetThreadLocalMallocViolationsPrivate().num_violations != 5) {
      if (!silent) {
        RtSafeLog(
            "realloc() handler not installed correctly: "
            "call to raw realloc did not increment counter.");
      }
      success = false;
    }
    // free the memory
    free(const_cast<void*>(ensure_hooks_registered_malloc_ptr));
  }

  {  // do an array new
    const size_t bytes_before =
        GetThreadLocalMallocViolationsPrivate().allocated_bytes.load();
    ensure_hooks_registered_new_ptr = new int[10];

    // ensure we saw the new
    if (GetThreadLocalMallocViolationsPrivate().num_violations != 6) {
      if (!silent) {
        RtSafeLog(
            "malloc() handler not installed "
            "correctly: call to array new did not increment counter.");
      }
      success = false;
    }
    if (GetThreadLocalMallocViolationsPrivate().allocated_bytes !=
        (sizeof(int) * 10) + bytes_before) {
      if (!silent) {
        RtSafeLog(
            "malloc() handler not installed "
            "correctly: call to array new did not count correct new bytes for "
            "an array of int.");
      }
      success = false;
    }
    // free the memory
    delete[] ensure_hooks_registered_new_ptr;
  }

  if (success) {
    if (!silent) {
      RtSafeLog("Installed malloc hook for MallocGuard successfully.");
    }
  }
  ResetThreadLocalMallocViolations();
  thread_local_malloc_guard_reaction = installed_reaction;
  if (!success) {
    return false;
  }
  return true;
}

bool InstallMallocGuardHooks() {
  // We intentionally lock the mutex for the entire duration of this function
  // to prevent race conditions during initialization, not just to protect the
  // counter increment.
  std::lock_guard<std::mutex> lock(malloc_guard_install_counter_mutex);
  int new_value = ++malloc_guard_install_counter;
  if (new_value != 1) {
    // MallocGuard is either disabled, or already enabled, nothing to do.
    return true;
  }

  static std::once_flag init_once;
  std::call_once(init_once, []() {});

  if (CheckMallocHandlerInstalled(true)) {
    // Handler is already installed, nothing to do.
    return true;
  }

  if (!intrinsic::InstallDynamicGotHooks(kTargets)) {
    --malloc_guard_install_counter;
    return false;
  }
  if (auto s = CheckMallocHandlerInstalled(false); !s) {
    RtSafeLog("Dynamic GOT hooks failed to intercept malloc.");
    --malloc_guard_install_counter;
    intrinsic::UninstallDynamicGotHooks(kTargets);
    return s;
  }

  return true;
}

bool UninstallMallocGuardHooks() {
  std::lock_guard<std::mutex> lock(malloc_guard_install_counter_mutex);
  int new_value = --malloc_guard_install_counter;
  if (new_value == 0) {
    RtSafeLog("Removing malloc_guard hook");
    if (!intrinsic::UninstallDynamicGotHooks(kTargets)) {
      return false;
    }
  }

  if (new_value < 0) {
    // Restore the counter to a valid state and then report the error. This also
    // works when this function is called in a parallel thread that decreases
    // the value below zero.
    malloc_guard_install_counter = 0;
    return false;
  }

  return true;
}

MallocGuard::MallocGuard(bool ignore) : ignore_(ignore) {
  if (!ignore) {
    if (!AreMallocGuardHooksInstalled()) {
      RtSafeLog(
          "Malloc hook is not enabled. Call InstallMallocGuardHooks() or "
          "use ScopedMallocGuardHook.");
      std::exit(1);
    }
#if __has_feature(realtime_sanitizer)
    if (local_malloc_guard_counter == 0) {
      __rtsan_realtime_enter();
    }
#endif
    local_malloc_guard_counter++;
  }
}

MallocGuard::~MallocGuard() {
  if (!ignore_) {
    local_malloc_guard_counter--;
#if __has_feature(realtime_sanitizer)
    if (local_malloc_guard_counter == 0) {
      __rtsan_realtime_exit();
    }
#endif
    if (local_malloc_guard_counter < 0) {
      RtSafeLog(
          "~MallocGuard() was called more often than "
          "MallocGuard(). This is a bug!");
      std::exit(1);
    }
  }
}

thread_local std::atomic_int64_t local_ignore_counter = 0;

bool MallocGuard::IsMallocGuarded() {
  return local_malloc_guard_counter > 0 && local_ignore_counter == 0;
}

ScopedMallocGuardIgnore::ScopedMallocGuardIgnore() {
  local_ignore_counter++;
#if __has_feature(realtime_sanitizer)
  if (local_malloc_guard_counter > 0 && local_ignore_counter == 1) {
    disabled_rtsan_ = true;
    __rtsan_disable();
  }
#endif
}

ScopedMallocGuardIgnore::~ScopedMallocGuardIgnore() {
  local_ignore_counter--;
#if __has_feature(realtime_sanitizer)
  if (disabled_rtsan_ && local_ignore_counter == 0) {
    __rtsan_enable();
    disabled_rtsan_ = false;
  }
#endif
  if (local_ignore_counter < 0) {
    RtSafeLog(
        "~ScopedMallocGuardIgnore() called more often than "
        "ScopedMallocGuardIgnore()!");
    std::exit(1);
  }
}

void SetMallocGuardDenylist(const std::unordered_set<std::string>& denylist) {
  std::lock_guard<std::mutex> lock(malloc_guard_denylist_mutex);
  malloc_guard_denylist = denylist;
}

std::unordered_set<std::string> GetMallocGuardDenylist() {
  std::lock_guard<std::mutex> lock(malloc_guard_denylist_mutex);
  return malloc_guard_denylist;
}

void SetGlobalMallocGuardReaction(MallocGuardReaction reaction) {
  global_malloc_guard_reaction = reaction;
}

void SetThreadLocalMallocGuardReaction(
    std::optional<MallocGuardReaction> reaction) {
  thread_local_malloc_guard_reaction = reaction;
}

std::optional<MallocGuardReaction> GetThreadLocalMallocGuardReaction() {
  return thread_local_malloc_guard_reaction;
}

void SetGlobalMallocGuardCallback(MallocGuardCallback callback) {
  std::lock_guard<std::mutex> lock(global_malloc_guard_callback_mutex);
  global_malloc_guard_callback = callback;
}

void SetThreadLocalMallocGuardCallback(MallocGuardCallback callback) {
  thread_local_malloc_guard_callback = callback;
}

MallocGuardReaction GetCurrentMallocGuardReaction() {
  return GetThreadLocalMallocGuardReaction().value_or(
      global_malloc_guard_reaction);
}

const MallocGuardViolations& GetThreadLocalMallocViolations() {
  return GetThreadLocalMallocViolationsPrivate();
}

void ResetThreadLocalMallocViolations() {
  GetThreadLocalMallocViolationsPrivate().num_violations = 0;
  GetThreadLocalMallocViolationsPrivate().allocated_bytes = 0;
  GetThreadLocalMallocViolationsPrivate().latest_violation_stack_trace.reset();
  GetThreadLocalMallocViolationsPrivate().stack_creation_duration.reset();
}

bool AreMallocGuardHooksInstalled() { return malloc_guard_install_counter > 0; }

ScopedThreadLocalReaction::ScopedThreadLocalReaction(
    MallocGuardReaction reaction)
    : previous_reaction_(GetThreadLocalMallocGuardReaction()) {
  previous_violations_ = GetThreadLocalMallocViolations();
  ResetThreadLocalMallocViolations();
  SetThreadLocalMallocGuardReaction(reaction);
}

ScopedThreadLocalReaction::~ScopedThreadLocalReaction() {
  GetThreadLocalMallocViolationsPrivate() = previous_violations_;
  SetThreadLocalMallocGuardReaction(previous_reaction_);
}

}  // namespace intrinsic

extern "C" {

static char calloc_fallback_buffer[1024];
static std::atomic<size_t> calloc_fallback_pos{0};

// malloc handler
void* malloc_guard_intercept_malloc(std::size_t size) {
  typedef void*(MallocFunction)(std::size_t);
  static std::atomic<MallocFunction*> real_malloc{nullptr};
  static thread_local bool malloc_in_init = false;

  MallocFunction* func = real_malloc.load(std::memory_order_acquire);
  if (!func) {
    // `dlsym` internally calls `calloc` and `malloc` to allocate memory.
    // To prevent infinite recursion when we call `dlsym` to find the real
    // allocator functions, we use a static fallback buffer to serve any
    // allocations requested during this initialization phase.
    // We don't need this handling for `realloc` or `posix_memalign` because
    // `dlsym` does not use them internally.
    if (malloc_in_init) {
      size_t current_pos =
          calloc_fallback_pos.fetch_add(size, std::memory_order_relaxed);
      if (current_pos + size > sizeof(calloc_fallback_buffer)) {
        intrinsic::RtSafeLog("Malloc fallback buffer exhausted.");
        std::exit(1);
      }
      return calloc_fallback_buffer + current_pos;
    }
    malloc_in_init = true;
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of malloc in the dynamic linker's search order. If we used RTLD_DEFAULT,
    // dlsym would return a pointer to this very interceptor function, leading
    // to infinite recursion.
    func = (MallocFunction*)::dlsym(RTLD_NEXT, "malloc");
    malloc_in_init = false;
    if (!func) {
      intrinsic::RtSafeLog("Could not find symbol for malloc.");
      std::exit(1);
    }
    real_malloc.store(func, std::memory_order_release);
  }

  if (intrinsic::AreMallocGuardHooksInstalled()) {
    intrinsic::HandleMallocGuardViolation(size);
  }
  return func(size);
}
void* malloc(std::size_t size) { return malloc_guard_intercept_malloc(size); }

// realloc handler
void* malloc_guard_intercept_realloc(void* ptr, std::size_t size) {
  typedef void*(ReallocFunction)(void* ptr, std::size_t);
  static std::atomic<ReallocFunction*> real_realloc{nullptr};

  ReallocFunction* func = real_realloc.load(std::memory_order_acquire);
  if (!func) {
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of realloc in the dynamic linker's search order. If we used RTLD_DEFAULT,
    // dlsym would return a pointer to this very interceptor function, leading
    // to infinite recursion.
    func = (ReallocFunction*)::dlsym(RTLD_NEXT, "realloc");
    if (!func) {
      intrinsic::RtSafeLog("Could not find symbol for realloc.");
      std::exit(1);
    }
    real_realloc.store(func, std::memory_order_release);
  }

  if (intrinsic::AreMallocGuardHooksInstalled()) {
    intrinsic::HandleMallocGuardViolation(size);
  }
  return func(ptr, size);
}
void* realloc(void* ptr, std::size_t size) {
  return malloc_guard_intercept_realloc(ptr, size);
}

// posix_memalign handler
int malloc_guard_intercept_posix_memalign(void** memptr, size_t alignment,
                                          size_t size) {
  typedef int(PosixMemalignFunction)(void**, size_t, size_t);
  static std::atomic<PosixMemalignFunction*> real_posix_memalign{nullptr};

  PosixMemalignFunction* func =
      real_posix_memalign.load(std::memory_order_acquire);
  if (!func) {
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of posix_memalign in the dynamic linker's search order. If we used
    // RTLD_DEFAULT, dlsym would return a pointer to this very interceptor
    // function, leading to infinite recursion.
    func = (PosixMemalignFunction*)::dlsym(RTLD_NEXT, "posix_memalign");
    if (!func) {
      intrinsic::RtSafeLog("Could not find symbol for posix_memalign.");
      std::exit(1);
    }
    real_posix_memalign.store(func, std::memory_order_release);
  }

  if (intrinsic::AreMallocGuardHooksInstalled()) {
    intrinsic::HandleMallocGuardViolation(size);
  }
  return func(memptr, alignment, size);
}
int posix_memalign(void** memptr, size_t alignment, size_t size) {
  return malloc_guard_intercept_posix_memalign(memptr, alignment, size);
}

// calloc handler
void* malloc_guard_intercept_calloc(std::size_t nmemb, std::size_t size) {
  typedef void*(CallocFunction)(std::size_t, std::size_t);
  static std::atomic<CallocFunction*> real_calloc{nullptr};
  static thread_local bool calloc_in_init = false;

  CallocFunction* func = real_calloc.load(std::memory_order_acquire);
  if (!func) {
    if (calloc_in_init) {
      size_t alloc_size = nmemb * size;
      size_t current_pos =
          calloc_fallback_pos.fetch_add(alloc_size, std::memory_order_relaxed);
      if (current_pos + alloc_size > sizeof(calloc_fallback_buffer)) {
        intrinsic::RtSafeLog("Calloc fallback buffer exhausted.");
        std::exit(1);
      }
      return calloc_fallback_buffer + current_pos;
    }
    calloc_in_init = true;
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of calloc in the dynamic linker's search order. If we used RTLD_DEFAULT,
    // dlsym would return a pointer to this very interceptor function, leading
    // to infinite recursion.
    func = (CallocFunction*)::dlsym(RTLD_NEXT, "calloc");
    calloc_in_init = false;
    if (!func) {
      intrinsic::RtSafeLog("Could not find symbol for calloc.");
      std::exit(1);
    }
    real_calloc.store(func, std::memory_order_release);
  }

  if (intrinsic::AreMallocGuardHooksInstalled()) {
    intrinsic::HandleMallocGuardViolation(nmemb * size);
  }
  return func(nmemb, size);
}
void* calloc(std::size_t nmemb, std::size_t size) {
  return malloc_guard_intercept_calloc(nmemb, size);
}

// free handler
void malloc_guard_intercept_free(void* ptr) {
  if (ptr >= calloc_fallback_buffer &&
      ptr < calloc_fallback_buffer + sizeof(calloc_fallback_buffer)) {
    return;  // Ignore fallback buffer memory
  }
  typedef void(FreeFunction)(void*);
  static std::atomic<FreeFunction*> real_free{nullptr};
  static thread_local bool free_in_init = false;

  FreeFunction* func = real_free.load(std::memory_order_acquire);
  if (!func) {
    if (free_in_init) {
      return;
    }
    free_in_init = true;
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of free in the dynamic linker's search order. If we used RTLD_DEFAULT,
    // dlsym would return a pointer to this very interceptor function, leading
    // to infinite recursion.
    func = (FreeFunction*)::dlsym(RTLD_NEXT, "free");
    free_in_init = false;
    if (!func) {
      return;
    }
    real_free.store(func, std::memory_order_release);
  }
  func(ptr);
}
void free(void* ptr) { malloc_guard_intercept_free(ptr); }

static thread_local int dl_intercept_depth = 0;

// dlopen handler
void* malloc_guard_intercept_dlopen(const char* filename, int flags) {
  typedef void*(DlopenFunction)(const char*, int);
  static std::atomic<DlopenFunction*> real_dlopen{nullptr};

  DlopenFunction* func = real_dlopen.load(std::memory_order_acquire);
  if (!func) {
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of dlopen in the dynamic linker's search order. If we used RTLD_DEFAULT,
    // dlsym would return a pointer to this very interceptor function, leading
    // to infinite recursion.
    func = (DlopenFunction*)::dlsym(RTLD_NEXT, "dlopen");
    if (!func) {
      intrinsic::RtSafeLog("Could not find symbol for dlopen.");
      std::exit(1);
    }
    real_dlopen.store(func, std::memory_order_release);
  }

  dl_intercept_depth++;
  void* handle = func(filename, flags);

  if (handle && filename != nullptr &&
      intrinsic::AreMallocGuardHooksInstalled()) {
    intrinsic::ScopedMallocGuardIgnore ignore;

    // Check if the newly loaded library introduces its own custom allocator.
    // We compare the symbol resolved within the new handle (local_sym) against
    // the globally active symbol (RTLD_DEFAULT, e.g.
    // malloc_guard_intercept_malloc) and the next in the search order
    // (RTLD_NEXT, e.g. glibc's malloc). The reason we need to compare against
    // both (RTLD_DEFAULT and RTLD_NEXT), is to distinguish between two cases.
    // * The dlopen loaded library calls a symbol we track, but not via
    //   its own custom implementation, rather via the one globally "active".
    //   Therefore local_sym would be equal to RTLD_DEFAULT or RTLD_NEXT.
    // * The dlopen loaded library calls a symbol we track, but also
    //   provides its own custom implementation (e.g. via tcmalloc).
    //   Therefore local_sym is a newly introduced and unknown symbol. Derived
    //   from this observation we conclude a custom allocator was dlopen loaded,
    //   which the MallocGuard does not support!
    for (const char* sym : intrinsic::kAllocatorSymbols) {
      void* local_sym = ::dlsym(handle, sym);
      if (local_sym != nullptr) {
        void* global_sym = ::dlsym(RTLD_DEFAULT, sym);
        void* next_sym = ::dlsym(RTLD_NEXT, sym);
        if (local_sym != global_sym && local_sym != next_sym) {
          intrinsic::RtSafeLog(
              "FATAL ERROR: Detected custom allocator in dynamically loaded "
              "library '",
              filename,
              "'. MallocGuard does not support this, consider excluding the "
              "library from checks using `SetMallocGuardDenylist()`");
          std::exit(1);
        }
      }
    }

    // Only re-apply GOT hooks when the outermost load finishes
    if (dl_intercept_depth == 1) {
      intrinsic::InstallDynamicGotHooks(intrinsic::kTargets);
    }
  }

  dl_intercept_depth--;
  return handle;
}
void* dlopen(const char* filename, int flags) {
  return malloc_guard_intercept_dlopen(filename, flags);
}

// dlmopen handler
void* malloc_guard_intercept_dlmopen(Lmid_t lmid, const char* filename,
                                     int flags) {
  typedef void*(DlmopenFunction)(Lmid_t, const char*, int);
  static std::atomic<DlmopenFunction*> real_dlmopen{nullptr};

  DlmopenFunction* func = real_dlmopen.load(std::memory_order_acquire);
  if (!func) {
    // We use RTLD_NEXT instead of RTLD_DEFAULT to find the *next* definition
    // of dlmopen in the dynamic linker's search order. If we used RTLD_DEFAULT,
    // dlsym would return a pointer to this very interceptor function, leading
    // to infinite recursion.
    func = (DlmopenFunction*)::dlsym(RTLD_NEXT, "dlmopen");
    if (!func) {
      intrinsic::RtSafeLog("Could not find symbol for dlmopen.");
      std::exit(1);
    }
    real_dlmopen.store(func, std::memory_order_release);
  }

  dl_intercept_depth++;
  void* handle = func(lmid, filename, flags);

  if (handle && filename != nullptr &&
      intrinsic::AreMallocGuardHooksInstalled()) {
    intrinsic::ScopedMallocGuardIgnore ignore;

    // Check if the newly loaded library introduces its own custom allocator.
    // We compare the symbol resolved within the new handle (local_sym) against
    // the globally active symbol (RTLD_DEFAULT, e.g.
    // malloc_guard_intercept_malloc) and the next in the search order
    // (RTLD_NEXT, e.g. glibc's malloc). The reason we need to compare against
    // both (RTLD_DEFAULT and RTLD_NEXT), is to distinguish between two cases.
    // * The dlopen loaded library calls a symbol we track, but not via
    //   its own custom implementation, rather via the one globally "active".
    //   Therefore local_sym would be equal to RTLD_DEFAULT or RTLD_NEXT.
    // * The dlopen loaded library calls a symbol we track, but also
    //   provides its own custom implementation (e.g. via tcmalloc).
    //   Therefore local_sym is a newly introduced and unknown symbol. Derived
    //   from this observation we conclude a custom allocator was dlopen loaded,
    //   which the MallocGuard does not support!
    for (const char* sym : intrinsic::kAllocatorSymbols) {
      void* local_sym = ::dlsym(handle, sym);
      if (local_sym != nullptr) {
        void* global_sym = ::dlsym(RTLD_DEFAULT, sym);
        void* next_sym = ::dlsym(RTLD_NEXT, sym);
        if (local_sym != global_sym && local_sym != next_sym) {
          intrinsic::RtSafeLog(
              "FATAL ERROR: Detected custom allocator in dynamically loaded "
              "library '",
              filename,
              "'. MallocGuard does not support this, consider excluding the "
              "library from checks using `SetMallocGuardDenylist()`");
          std::exit(1);
        }
      }
    }

    // Only re-apply hooks when the outermost load finishes
    if (dl_intercept_depth == 1) {
      intrinsic::InstallDynamicGotHooks(intrinsic::kTargets);
    }
  }

  dl_intercept_depth--;
  return handle;
}
void* dlmopen(Lmid_t lmid, const char* filename, int flags) {
  return malloc_guard_intercept_dlmopen(lmid, filename, flags);
}

}  // extern "C"

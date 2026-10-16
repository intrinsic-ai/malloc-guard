// Copyright 2026 Intrinsic Innovation LLC
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

#include <dlfcn.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using testing::AllOf;
using testing::Eq;
using testing::HasSubstr;

namespace intrinsic {

namespace {
// Declare constants as C-style strings, because we pass them to C-style APIs
// and rely on them being zero-terminated.
constexpr char kTestLibPath[] = "./libmalloc_guard_test_lib.so";
constexpr char kAllowedTestLibPath[] = "./libmalloc_guard_test_lib_allowed.so";
constexpr char kCustomMallocTestLibPath[] =
    "./libmalloc_guard_test_custom_malloc_lib.so";

void BadMalloc4Bytes() {
  // Global variable to make sure that the compiler doesn't optimize the
  // code.
  int32_t* volatile kNewPtr = new int32_t;
  *kNewPtr = 10;
  EXPECT_EQ(*kNewPtr, 10);
  delete kNewPtr;
}
}  // namespace

class MallocGuardTest : public ::testing::Test {
 protected:
  // Reset denylist before every test.
  MallocGuardTest() { SetMallocGuardDenylist({}); }
  ~MallocGuardTest() override {
    // Reset denylist after every test, too.
    SetMallocGuardDenylist({});
  }
};

TEST_F(MallocGuardTest, EnableSucceeds) {
  EXPECT_FALSE(AreMallocGuardHooksInstalled());
  ScopedMallocGuardHook hook;
  EXPECT_TRUE(AreMallocGuardHooksInstalled());
}

TEST_F(MallocGuardTest, DisableSucceeds) {
  {
    ScopedMallocGuardHook hook;
  }
  EXPECT_FALSE(AreMallocGuardHooksInstalled());
}

TEST_F(MallocGuardTest, MallocHookCountViolation) {
  EXPECT_FALSE(UninstallMallocGuardHooks());
}

TEST_F(MallocGuardTest, MallocGuardWithoutHookViolation) {
  EXPECT_DEATH([] { MallocGuard g; }(), "InstallMallocGuardHooks");
}

TEST_F(MallocGuardTest, DoesNotCountViolationsWithoutGuard) {
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();
  ScopedMallocGuardHook hook;
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  BadMalloc4Bytes();
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 0u);
}

TEST_F(MallocGuardTest, ResetViolationsWorks) {
  SetThreadLocalMallocGuardReaction(
      MallocGuardReaction::kStoreViolationWithTrace);
  ResetThreadLocalMallocViolations();
  ScopedMallocGuardHook hook;
  MallocGuard guard;
  BadMalloc4Bytes();
  EXPECT_NE(GetThreadLocalMallocViolations().num_violations, 0u);
  EXPECT_NE(GetThreadLocalMallocViolations().allocated_bytes, 0u);
  EXPECT_NE(GetThreadLocalMallocViolations().stack_creation_duration,
            std::chrono::nanoseconds::zero());
  ASSERT_TRUE(GetThreadLocalMallocViolations()
                  .latest_violation_stack_trace.has_value());

  EXPECT_NE(
      GetThreadLocalMallocViolations().latest_violation_stack_trace->num_frames,
      0u);
  ResetThreadLocalMallocViolations();
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 0u);
  EXPECT_FALSE(
      GetThreadLocalMallocViolations().stack_creation_duration.has_value());
  EXPECT_FALSE(GetThreadLocalMallocViolations()
                   .latest_violation_stack_trace.has_value());
}

TEST_F(MallocGuardTest, GenerateStackTraceInLog) {
  ::testing::internal::CaptureStderr();
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kLogWithTrace);
  ResetThreadLocalMallocViolations();
  ScopedMallocGuardHook hook;
  {
    MallocGuard guard;
    // Call in function so that we know the malloc is coming from there.
    BadMalloc4Bytes();
  }
  std::string output = ::testing::internal::GetCapturedStderr();
  EXPECT_THAT(output, HasSubstr("Encountered malloc in realtime thread"));
}

TEST_F(MallocGuardTest, MallocIsCaughtAndPrinted) {
  ::testing::internal::CaptureStderr();
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kLog);
  ResetThreadLocalMallocViolations();
  ScopedMallocGuardHook hook;
  {
    MallocGuard guard;
    BadMalloc4Bytes();
  }
  std::string output = ::testing::internal::GetCapturedStderr();
  EXPECT_THAT(output, AllOf(HasSubstr("Encountered malloc in realtime thread"),
                            HasSubstr("Allocated 4 bytes")));
}

TEST_F(MallocGuardTest, ReactionSwitchWorks) {
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kLog);
  EXPECT_EQ(GetCurrentMallocGuardReaction(), MallocGuardReaction::kLog);
  ResetThreadLocalMallocViolations();
  ScopedMallocGuardHook hook;
  {
    MallocGuard guard;
    // Global variable to make sure that the compiler doesn't optimize the code.
    int32_t* volatile kNewPtr = new int32_t;
    *kNewPtr = 10;
    EXPECT_EQ(*kNewPtr, 10);
    delete kNewPtr;
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    EXPECT_EQ(GetCurrentMallocGuardReaction(),
              MallocGuardReaction::kStoreViolation);
    kNewPtr = new int32_t;
    *kNewPtr = 20;
    EXPECT_EQ(*kNewPtr, 20);
    delete kNewPtr;
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kNone);
    EXPECT_EQ(GetCurrentMallocGuardReaction(), MallocGuardReaction::kNone);
    kNewPtr = new int32_t;
    *kNewPtr = 20;
    EXPECT_EQ(*kNewPtr, 20);
    delete kNewPtr;
    // We only expect one violation because the reaction was was not
    // "StoreViolation" for the other allocations.
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  }
}

TEST_F(MallocGuardTest, ScopedThreadLocalReactionWorks) {
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ScopedMallocGuardHook hook;
  MallocGuard guard;
  BadMalloc4Bytes();
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 4u);
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kLog);
  {
    ScopedThreadLocalReaction scoped_reaction(
        MallocGuardReaction::kStoreViolation);
    EXPECT_EQ(GetCurrentMallocGuardReaction(),
              MallocGuardReaction::kStoreViolation);
    // Allocations are zero because the scoped reaction resets the violation
    // storage (and restores the previous violations when the scoped_reaction
    // goes out of scope).
    EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 0u);
    // Global variable to make sure that the compiler doesn't optimize the code.
    int16_t* volatile kNewPtr = new int16_t;
    *kNewPtr = 10;
    EXPECT_EQ(*kNewPtr, 10);
    delete kNewPtr;
    EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 2u);
  }
  // ScopedThreadLocalReaction restores the previous reaction and the previous
  // violation data.
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 4u);
  EXPECT_EQ(GetCurrentMallocGuardReaction(), MallocGuardReaction::kLog);
}

TEST_F(MallocGuardTest, MallocCaughtLeadsToAbort) {
  ScopedMallocGuardHook hook;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kAbort);

  EXPECT_DEATH(
      [] {
        MallocGuard guard;
        int8_t* volatile kNewPtr = new int8_t;
        (void)kNewPtr;
      }(),
      "Encountered malloc");
}

TEST_F(MallocGuardTest, LocalTrumpsGlobalReaction) {
  ScopedMallocGuardHook hook;
  ResetThreadLocalMallocViolations();
  SetThreadLocalMallocGuardReaction({});
  SetGlobalMallocGuardReaction(MallocGuardReaction::kLog);
  EXPECT_EQ(GetCurrentMallocGuardReaction(), MallocGuardReaction::kLog);
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  EXPECT_EQ(GetCurrentMallocGuardReaction(),
            MallocGuardReaction::kStoreViolation);
  MallocGuard guard;
  BadMalloc4Bytes();

  EXPECT_NE(GetThreadLocalMallocViolations().num_violations, 0u);
}

TEST_F(MallocGuardTest, LocalReactionCanBeDeleted) {
  ScopedMallocGuardHook hook;
  ResetThreadLocalMallocViolations();
  SetGlobalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kAbort);
  SetThreadLocalMallocGuardReaction({});
  EXPECT_EQ(GetCurrentMallocGuardReaction(),
            MallocGuardReaction::kStoreViolation);
  MallocGuard guard;
  BadMalloc4Bytes();

  EXPECT_NE(GetThreadLocalMallocViolations().num_violations, 0u);
}

TEST_F(MallocGuardTest, MallocCaught) {
  ScopedMallocGuardHook hook;
  MallocGuard guard;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);

  int8_t* volatile kNewPtr = new int8_t;
  kNewPtr[0] = 10;
  EXPECT_EQ(kNewPtr[0], 10);
  delete kNewPtr;
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 1u);
}

TEST_F(MallocGuardTest, ArrayMallocCaught) {
  ScopedMallocGuardHook hook;
  MallocGuard guard;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);

  int8_t* volatile kNewPtr = new int8_t[40];
  kNewPtr[0] = 10;
  EXPECT_EQ(kNewPtr[0], 10);
  delete[] kNewPtr;
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes,
            40 * sizeof(*kNewPtr));
}

TEST_F(MallocGuardTest, NoThreadCrossTalk) {
  auto malloc_worker = [] {
    ScopedMallocGuardHook hook;
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    const size_t num_iterations = 1000000;
    {
      MallocGuard guard;
      for (size_t i = 0; i < num_iterations; i++) {
        int8_t* volatile kNewPtr = new int8_t;
        delete kNewPtr;
      }
    }
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, num_iterations);
    EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, num_iterations);
  };
  std::thread t1(malloc_worker);
  std::thread t2(malloc_worker);
  t1.join();
  t2.join();
}

TEST_F(MallocGuardTest, GlobalCallbackWorks) {
  ScopedMallocGuardHook hook;
  // The callback is a function pointer, not a std::function, so it cannot be a
  // capturing lambda. Therefore, this must be static
  static std::atomic<size_t> callback_called{0};
  // Reset to zero because the same test can run multiple times per process
  callback_called = 0;
  SetThreadLocalMallocGuardReaction({});
  SetGlobalMallocGuardReaction(MallocGuardReaction::kCustomCallback);
  SetGlobalMallocGuardCallback(+[](size_t bytes) { callback_called += bytes; });

  {
    MallocGuard guard;
    BadMalloc4Bytes();
  }

  EXPECT_EQ(callback_called.load(), 4u);
}

TEST_F(MallocGuardTest, ThreadLocalCallbackOverridesGlobalCallback) {
  ScopedMallocGuardHook hook;
  // Callbacks must be captureless lambdas to decay to function pointers,
  // so they can only modify static variables
  static std::atomic<size_t> global_called{0};
  static std::atomic<size_t> local_called{0};
  // Reset the counters in case the test runs multiple times in the same
  // process
  global_called = 0;
  local_called = 0;

  SetGlobalMallocGuardReaction(MallocGuardReaction::kCustomCallback);
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kCustomCallback);

  SetGlobalMallocGuardCallback(+[](size_t bytes) { global_called += bytes; });
  SetThreadLocalMallocGuardCallback(
      +[](size_t bytes) { local_called += bytes; });

  {
    MallocGuard guard;
    BadMalloc4Bytes();
  }

  EXPECT_EQ(global_called.load(), 0u);
  EXPECT_EQ(local_called.load(), 4u);
}

TEST_F(MallocGuardTest, ScopedMallocGuardIgnoreWorks) {
  ScopedMallocGuardHook hook;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();

  {
    MallocGuard guard;
    {
      ScopedMallocGuardIgnore ignore;
      BadMalloc4Bytes();
    }
  }

  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 0u);
}

TEST_F(MallocGuardTest, CallocCaught) {
  ScopedMallocGuardHook hook;
  MallocGuard guard;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();

  void* ptr = calloc(10, 4);
  EXPECT_NE(ptr, nullptr);
  free(ptr);

  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 40u);
}

TEST_F(MallocGuardTest, ReallocCaught) {
  ScopedMallocGuardHook hook;
  void* ptr = malloc(4);

  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();

  {
    MallocGuard guard;
    ptr = realloc(ptr, 8);
  }

  EXPECT_NE(ptr, nullptr);
  free(ptr);

  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 8u);
}

TEST_F(MallocGuardTest, PosixMemalignCaught) {
  ScopedMallocGuardHook hook;
  MallocGuard guard;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();

  void* ptr = nullptr;
  int res = posix_memalign(&ptr, 16, 32);
  EXPECT_EQ(res, 0);
  EXPECT_NE(ptr, nullptr);
  free(ptr);

  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 32u);
}

int Worker(unsigned int seed) {
  srand(seed);
  auto value = rand_r(&seed);
  std::vector<int> min;
  for (int i = 1; i < 3000; i++) {
    std::vector<int> vec;
    for (int j = 0; j < 10; j++) {
      vec.push_back(value++);
    }
    std::sort(vec.begin(), vec.end());
    min.push_back(vec[0]);
  }

  return min.front();
}

TEST_F(MallocGuardTest, MallocGuardStacksProperly) {
  ScopedMallocGuardHook hook;
  EXPECT_FALSE(MallocGuard::IsMallocGuarded());
  {
    MallocGuard outer;
    EXPECT_TRUE(MallocGuard::IsMallocGuarded());
    {
      MallocGuard inner;
      EXPECT_TRUE(MallocGuard::IsMallocGuarded());
    }
    EXPECT_TRUE(MallocGuard::IsMallocGuarded());
  }
  EXPECT_FALSE(MallocGuard::IsMallocGuarded());
}

TEST_F(MallocGuardTest, DeeplyNestedGuardAndIgnoreStacksProperly) {
  ScopedMallocGuardHook hook;
  EXPECT_FALSE(MallocGuard::IsMallocGuarded());
  {
    MallocGuard g1;
    EXPECT_TRUE(MallocGuard::IsMallocGuarded());
    {
      ScopedMallocGuardIgnore i1;
      EXPECT_FALSE(MallocGuard::IsMallocGuarded());
    }
    EXPECT_TRUE(MallocGuard::IsMallocGuarded());
    {
      MallocGuard g2;
      EXPECT_TRUE(MallocGuard::IsMallocGuarded());
      {
        ScopedMallocGuardIgnore i2;
        EXPECT_FALSE(MallocGuard::IsMallocGuarded());
        {
          ScopedMallocGuardIgnore i3;
          EXPECT_FALSE(MallocGuard::IsMallocGuarded());
        }
        EXPECT_FALSE(MallocGuard::IsMallocGuarded());
      }
      EXPECT_TRUE(MallocGuard::IsMallocGuarded());
    }
    EXPECT_TRUE(MallocGuard::IsMallocGuarded());
  }
  EXPECT_FALSE(MallocGuard::IsMallocGuarded());
}

TEST_F(MallocGuardTest, DynamicGotHookEmptyDenylistCatchesAllocation) {
  ScopedMallocGuardHook hook;

  void* handle = dlopen(kTestLibPath, RTLD_NOW);
  ASSERT_NE(handle, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle, "TestMalloc"));
  auto test_free = reinterpret_cast<void (*)(void*)>(dlsym(handle, "TestFree"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);

  // empty denylist, hence the malloc_guard should catch the allocation
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  }

  dlclose(handle);
}

TEST_F(MallocGuardTest, DynamicGotHookEmptyDenylistCatchesAllocationLazy) {
  ScopedMallocGuardHook hook;

  void* handle = dlopen(kTestLibPath, RTLD_LAZY);
  ASSERT_NE(handle, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle, "TestMalloc"));
  auto test_free = reinterpret_cast<void (*)(void*)>(dlsym(handle, "TestFree"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);

  // empty denylist, hence the malloc_guard should catch the allocation
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  }

  dlclose(handle);
}

TEST_F(MallocGuardTest, DynamicGotHookDenylistIgnoresAllocation) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({kAllowedTestLibPath});

  void* handle_allowed = dlopen(kAllowedTestLibPath, RTLD_NOW);
  ASSERT_NE(handle_allowed, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle_allowed, "TestMalloc"));
  auto test_free =
      reinterpret_cast<void (*)(void*)>(dlsym(handle_allowed, "TestFree"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);

  // Library was added to the denylist, should not intercept the malloc usage
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  }

  dlclose(handle_allowed);
}

TEST_F(MallocGuardTest, DynamicGotHookEmptyDenylistCatchesAllocationDlmopen) {
  ScopedMallocGuardHook hook;

  void* handle = dlmopen(LM_ID_BASE, kTestLibPath, RTLD_NOW);
  ASSERT_NE(handle, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle, "TestMalloc"));
  auto test_free = reinterpret_cast<void (*)(void*)>(dlsym(handle, "TestFree"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);

  // empty denylist, hence the malloc_guard should catch the allocation
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  }

  dlclose(handle);
}

TEST_F(MallocGuardTest, DynamicGotHookDenylistIgnoresAllocationDlmopen) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({kAllowedTestLibPath});

  void* handle_allowed = dlmopen(LM_ID_BASE, kAllowedTestLibPath, RTLD_NOW);
  ASSERT_NE(handle_allowed, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle_allowed, "TestMalloc"));
  auto test_free =
      reinterpret_cast<void (*)(void*)>(dlsym(handle_allowed, "TestFree"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);

  // Library was added to the denylist, should not intercept the malloc usage
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  }

  dlclose(handle_allowed);
}

TEST_F(MallocGuardTest,
       DynamicGotHookEmptyDenylistCatchesAllocationLeadsToAbort) {
  ScopedMallocGuardHook hook;

  void* handle = dlopen(kTestLibPath, RTLD_NOW);
  ASSERT_NE(handle, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle, "TestMalloc"));
  ASSERT_NE(test_malloc, nullptr);

  // empty denylist, hence the malloc_guard should catch the allocation
  EXPECT_DEATH(
      [test_malloc]() {
        SetThreadLocalMallocGuardReaction(MallocGuardReaction::kAbort);
        MallocGuard guard;
        void* ptr = test_malloc(32);
        (void)ptr;
      }(),
      "Encountered malloc");

  dlclose(handle);
}

TEST_F(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocation) {
  ScopedMallocGuardHook hook;

  EXPECT_DEATH(
      {
        void* handle = dlopen(kCustomMallocTestLibPath, RTLD_NOW);
        (void)handle;
      },
      "uses a different allocator");
}

TEST_F(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocationLazy) {
  ScopedMallocGuardHook hook;

  EXPECT_DEATH(
      {
        void* handle = dlopen(kCustomMallocTestLibPath, RTLD_LAZY);
        (void)handle;
      },
      "uses a different allocator");
}

TEST_F(MallocGuardTest, DynamicGotHookDiesWithDeepbind) {
  ScopedMallocGuardHook hook;

  // RTLD_DEEPBIND makes the loaded library prefer its own symbol definitions,
  // which defeats interception. MallocGuard refuses rather than silently
  // under-reporting.
  //
  // We deliberately load the custom-allocator library here: it would *also*
  // trip the "different allocator" check, so this additionally pins down that
  // the RTLD_DEEPBIND check takes precedence.
  EXPECT_DEATH(
      {
        void* handle =
            dlopen(kCustomMallocTestLibPath, RTLD_LAZY | RTLD_DEEPBIND);
        (void)handle;
      },
      "MallocGuard does not support loading libraries with RTLD_DEEPBIND.");
}

TEST_F(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocationLazyNoLoad) {
  ScopedMallocGuardHook hook;

  void* handle = dlopen(kCustomMallocTestLibPath, RTLD_LAZY | RTLD_NOLOAD);
  EXPECT_EQ(handle, nullptr);
}

TEST_F(MallocGuardTest,
       DynamicGotHookCatchesCustomMallocAllocationAlreadyLoadedNoLoad) {
  void* initial_handle = dlopen(kCustomMallocTestLibPath, RTLD_LAZY);
  ASSERT_NE(initial_handle, nullptr);

  ScopedMallocGuardHook hook;

  EXPECT_DEATH(
      {
        void* handle =
            dlopen(kCustomMallocTestLibPath, RTLD_LAZY | RTLD_NOLOAD);
        (void)handle;
      },
      "uses a different allocator");

  dlclose(initial_handle);
}

TEST_F(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocationLazyGlobal) {
  ScopedMallocGuardHook hook;

  EXPECT_DEATH(
      {
        void* handle =
            dlopen(kCustomMallocTestLibPath, RTLD_LAZY | RTLD_GLOBAL);
        (void)handle;
      },
      "uses a different allocator");
}

TEST_F(MallocGuardTest, CAndCppAllocationsExactByteCount) {
  ScopedMallocGuardHook hook;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();

  MallocGuard guard;

  // 1. malloc
  void* volatile p_malloc = malloc(32);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 32u);
  free(const_cast<void*>(p_malloc));

  // 2. calloc
  void* volatile p_calloc = calloc(4, 8);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 2u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 32u + 32u);
  free(const_cast<void*>(p_calloc));

  // 3. realloc
  void* volatile p_realloc = malloc(16);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 3u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 64u + 16u);
  p_realloc = realloc(const_cast<void*>(p_realloc), 64);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 4u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 80u + 64u);
  free(const_cast<void*>(p_realloc));

  // 4. posix_memalign
  void* p_memalign = nullptr;
  ASSERT_EQ(posix_memalign(&p_memalign, 64, 128), 0);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 5u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 144u + 128u);
  free(p_memalign);

  // 5. scalar new
  int32_t* volatile p_new = new int32_t(7);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 6u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes,
            272u + sizeof(int32_t));
  delete p_new;

  // 6. array new[]
  const size_t before_arr = GetThreadLocalMallocViolations().allocated_bytes;
  int32_t* volatile p_arr = new int32_t[10];
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 7u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes,
            before_arr + 10 * sizeof(int32_t));
  delete[] p_arr;

  // 7. ReportAllocation direct call
  const size_t before_report = GetThreadLocalMallocViolations().allocated_bytes;
  ReportAllocation(256);
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 8u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes,
            before_report + 256u);

  SetThreadLocalMallocGuardReaction(std::nullopt);
}

TEST_F(MallocGuardTest, SharedLibraryAllocationNoDoubleCount) {
  ScopedMallocGuardHook hook;

  void* handle = dlopen(kTestLibPath, RTLD_NOW);
  ASSERT_NE(handle, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle, "TestMalloc"));
  auto test_free = reinterpret_cast<void (*)(void*)>(dlsym(handle, "TestFree"));
  auto test_new = reinterpret_cast<int* (*)()>(dlsym(handle, "TestNew"));
  auto test_delete =
      reinterpret_cast<void (*)(int*)>(dlsym(handle, "TestDelete"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);
  ASSERT_NE(test_new, nullptr);
  ASSERT_NE(test_delete, nullptr);

  size_t violations_after_malloc = 0;
  size_t bytes_after_malloc = 0;
  size_t violations_after_new = 0;
  size_t bytes_after_new = 0;
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    violations_after_malloc = GetThreadLocalMallocViolations().num_violations;
    bytes_after_malloc = GetThreadLocalMallocViolations().allocated_bytes;

    int* new_ptr = test_new();
    test_delete(new_ptr);
    violations_after_new = GetThreadLocalMallocViolations().num_violations;
    bytes_after_new = GetThreadLocalMallocViolations().allocated_bytes;
  }
  EXPECT_EQ(violations_after_malloc, 1u);
  EXPECT_EQ(bytes_after_malloc, 32u);
  EXPECT_EQ(violations_after_new, 2u);
  EXPECT_EQ(bytes_after_new, 32u + sizeof(int));

  SetThreadLocalMallocGuardReaction(std::nullopt);
  dlclose(handle);
}

// Denylisting works at the granularity of GOT/PLT entries, so it can only
// suppress allocations made from call sites *inside* the denylisted library.
TEST_F(MallocGuardTest, DenylistScopeForCAndCppAllocation) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({kAllowedTestLibPath});

  void* handle_allowed = dlopen(kAllowedTestLibPath, RTLD_NOW);
  ASSERT_NE(handle_allowed, nullptr) << dlerror();

  auto test_malloc =
      reinterpret_cast<void* (*)(size_t)>(dlsym(handle_allowed, "TestMalloc"));
  auto test_free =
      reinterpret_cast<void (*)(void*)>(dlsym(handle_allowed, "TestFree"));
  auto test_new =
      reinterpret_cast<int* (*)()>(dlsym(handle_allowed, "TestNew"));
  auto test_delete =
      reinterpret_cast<void (*)(int*)>(dlsym(handle_allowed, "TestDelete"));
  ASSERT_NE(test_malloc, nullptr);
  ASSERT_NE(test_free, nullptr);
  ASSERT_NE(test_new, nullptr);
  ASSERT_NE(test_delete, nullptr);

  size_t violations_after_malloc = 0;
  size_t violations_after_new = 0;
  {
    SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
    ResetThreadLocalMallocViolations();
    MallocGuard guard;

    void* ptr = test_malloc(32);
    test_free(ptr);
    violations_after_malloc = GetThreadLocalMallocViolations().num_violations;

    int* new_ptr = test_new();
    test_delete(new_ptr);
    violations_after_new = GetThreadLocalMallocViolations().num_violations;
  }

  // The library calls malloc() itself, from its own (unhooked) GOT entry.
  EXPECT_EQ(violations_after_malloc, 0u);

  // `new`, however, is *not* suppressed: the library only calls
  // `operator new`, and the actual malloc() happens inside the C++ runtime
  // (libstdc++), which is not denylisted and therefore still hooked. Denylist a
  // library only to silence its direct C allocations; it cannot hide
  // allocations that the library delegates to a non-denylisted library.
  EXPECT_EQ(violations_after_new, 1u);

  SetThreadLocalMallocGuardReaction(std::nullopt);
  dlclose(handle_allowed);
}

}  // namespace intrinsic

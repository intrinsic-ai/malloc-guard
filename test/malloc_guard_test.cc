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

#include "malloc_guard.h"

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
void BadMalloc4Bytes() {
  // Global variable to make sure that the compiler doesn't optimize the
  // code.
  int32_t* volatile kNewPtr = new int32_t;
  *kNewPtr = 10;
  EXPECT_EQ(*kNewPtr, 10);
  delete kNewPtr;
}
}  // namespace

TEST(MallocGuardTest, EnableSucceeds) {
  EXPECT_FALSE(AreMallocGuardHooksInstalled());
  ScopedMallocGuardHook hook;
  EXPECT_TRUE(AreMallocGuardHooksInstalled());
}

TEST(MallocGuardTest, DisableSucceeds) {
  {
    ScopedMallocGuardHook hook;
  }
  EXPECT_FALSE(AreMallocGuardHooksInstalled());
}

TEST(MallocGuardTest, MallocHookCountViolation) {
  EXPECT_FALSE(UninstallMallocGuardHooks());
}

TEST(MallocGuardTest, MallocGuardWithoutHookViolation) {
  EXPECT_DEATH([] { MallocGuard g; }(), "InstallMallocGuardHooks");
}

TEST(MallocGuardTest, DoesNotCountViolationsWithoutGuard) {
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();
  ScopedMallocGuardHook hook;
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  BadMalloc4Bytes();
  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 0u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 0u);
}

TEST(MallocGuardTest, ResetViolationsWorks) {
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

TEST(MallocGuardTest, GenerateStackTraceInLog) {
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

TEST(MallocGuardTest, MallocIsCaughtAndPrinted) {
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

TEST(MallocGuardTest, ReactionSwitchWorks) {
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

TEST(MallocGuardTest, ScopedThreadLocalReactionWorks) {
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

TEST(MallocGuardTest, MallocCaughtLeadsToAbort) {
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

TEST(MallocGuardTest, LocalTrumpsGlobalReaction) {
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

TEST(MallocGuardTest, LocalReactionCanBeDeleted) {
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

TEST(MallocGuardTest, MallocCaught) {
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

TEST(MallocGuardTest, ArrayMallocCaught) {
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

TEST(MallocGuardTest, NoThreadCrossTalk) {
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

TEST(MallocGuardTest, GlobalCallbackWorks) {
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

TEST(MallocGuardTest, ThreadLocalCallbackOverridesGlobalCallback) {
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

TEST(MallocGuardTest, ScopedMallocGuardIgnoreWorks) {
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

TEST(MallocGuardTest, CallocCaught) {
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

TEST(MallocGuardTest, ReallocCaught) {
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

TEST(MallocGuardTest, PosixMemalignCaught) {
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

TEST(MallocGuardTest, MallocGuardStacksProperly) {
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

TEST(MallocGuardTest, DeeplyNestedGuardAndIgnoreStacksProperly) {
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

TEST(MallocGuardTest, DynamicGotHookEmptyDenylistCatchesAllocation) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  void* handle =
      dlopen("./libmalloc_guard_test_lib.so", RTLD_NOW | RTLD_DEEPBIND);
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

TEST(MallocGuardTest, DynamicGotHookEmptyDenylistCatchesAllocationLazy) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  void* handle =
      dlopen("./libmalloc_guard_test_lib.so", RTLD_LAZY | RTLD_DEEPBIND);
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

TEST(MallocGuardTest, DynamicGotHookDenylistIgnoresAllocation) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({"./libmalloc_guard_test_lib_allowed.so"});

  void* handle_allowed =
      dlopen("./libmalloc_guard_test_lib_allowed.so", RTLD_NOW | RTLD_DEEPBIND);
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
  SetMallocGuardDenylist({});
}

TEST(MallocGuardTest, DynamicGotHookEmptyDenylistCatchesAllocationDlmopen) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  void* handle = dlmopen(LM_ID_BASE, "./libmalloc_guard_test_lib.so",
                         RTLD_NOW | RTLD_DEEPBIND);
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

TEST(MallocGuardTest, DynamicGotHookDenylistIgnoresAllocationDlmopen) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({"./libmalloc_guard_test_lib_allowed.so"});

  void* handle_allowed =
      dlmopen(LM_ID_BASE, "./libmalloc_guard_test_lib_allowed.so",
              RTLD_NOW | RTLD_DEEPBIND);
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
  SetMallocGuardDenylist({});
}

TEST(MallocGuardTest,
     DynamicGotHookEmptyDenylistCatchesAllocationLeadsToAbort) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  void* handle =
      dlopen("./libmalloc_guard_test_lib.so", RTLD_NOW | RTLD_DEEPBIND);
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

TEST(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocation) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  EXPECT_DEATH(
      {
        void* handle = dlopen("./libmalloc_guard_test_custom_malloc_lib.so",
                              RTLD_NOW | RTLD_DEEPBIND);
        (void)handle;
      },
      "FATAL ERROR dynamic loaded library with custom allocator detector! "
      "Library: .*");
}

TEST(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocationLazy) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  EXPECT_DEATH(
      {
        void* handle = dlopen("./libmalloc_guard_test_custom_malloc_lib.so",
                              RTLD_LAZY | RTLD_DEEPBIND);
        (void)handle;
      },
      "FATAL ERROR dynamic loaded library with custom allocator detector! "
      "Library: .*");
}

TEST(MallocGuardTest,
     DynamicGotHookCatchesCustomMallocAllocationLazyNoDeepbind) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  EXPECT_DEATH(
      {
        void* handle =
            dlopen("./libmalloc_guard_test_custom_malloc_lib.so", RTLD_LAZY);
        (void)handle;
      },
      "FATAL ERROR dynamic loaded library with custom allocator detector! "
      "Library: .*");
}

TEST(MallocGuardTest,
     DynamicGotHookCatchesCustomMallocAllocationLazyNoLoadNoDeepbind) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  void* handle = dlopen("./libmalloc_guard_test_custom_malloc_lib.so",
                        RTLD_LAZY | RTLD_NOLOAD);
  EXPECT_EQ(handle, nullptr);
}

TEST(MallocGuardTest,
     DynamicGotHookCatchesCustomMallocAllocationAlreadyLoadedNoLoad) {
  void* initial_handle = dlopen("./libmalloc_guard_test_custom_malloc_lib.so",
                                RTLD_LAZY | RTLD_DEEPBIND);
  ASSERT_NE(initial_handle, nullptr);

  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  EXPECT_DEATH(
      {
        void* handle = dlopen("./libmalloc_guard_test_custom_malloc_lib.so",
                              RTLD_LAZY | RTLD_NOLOAD);
        (void)handle;
      },
      "FATAL ERROR dynamic loaded library with custom allocator detector! "
      "Library: .*");

  dlclose(initial_handle);
}

TEST(MallocGuardTest, DynamicGotHookCatchesCustomMallocAllocationLazyGlobal) {
  ScopedMallocGuardHook hook;
  SetMallocGuardDenylist({});

  EXPECT_DEATH(
      {
        void* handle = dlopen("./libmalloc_guard_test_custom_malloc_lib.so",
                              RTLD_LAZY | RTLD_GLOBAL);
        (void)handle;
      },
      "FATAL ERROR dynamic loaded library with custom allocator detector! "
      "Library: .*");
}

}  // namespace intrinsic

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

// Verifies the extension point used to support statically linked allocators.
//
// This lives in its own test binary because it *defines* the weak
// `MallocGuardCustomSetup()` / `MallocGuardCustomTeardown()` symbols. Their
// presence changes MallocGuard's behavior process-wide (for example, it
// disallows denylisting), so they must not leak into the other tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>

#include "intrinsic/malloc_guard.h"

namespace intrinsic {

std::atomic<int> setup_hook_counter = 0;
std::atomic<int> teardown_hook_counter = 0;

bool MallocGuardCustomSetup() {
  setup_hook_counter.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool MallocGuardCustomTeardown() {
  teardown_hook_counter.fetch_add(1, std::memory_order_relaxed);
  return true;
}

TEST(MallocGuardStaticAllocatorHooks, CallsHooks) {
  setup_hook_counter.store(0);
  teardown_hook_counter.store(0);
  {
    ScopedMallocGuardHook hook;
    EXPECT_EQ(setup_hook_counter.load(), 1);
    EXPECT_EQ(teardown_hook_counter.load(), 0);
  }
  EXPECT_EQ(setup_hook_counter.load(), 1);
  EXPECT_EQ(teardown_hook_counter.load(), 1);
}

TEST(MallocGuardStaticAllocatorHooks, ReportAllocationIsCounted) {
  ScopedMallocGuardHook hook;
  SetThreadLocalMallocGuardReaction(MallocGuardReaction::kStoreViolation);
  ResetThreadLocalMallocViolations();

  {
    MallocGuard guard;
    // Emulates what a statically linked allocator's hook does.
    ReportAllocation(128);
  }

  EXPECT_EQ(GetThreadLocalMallocViolations().num_violations, 1u);
  EXPECT_EQ(GetThreadLocalMallocViolations().allocated_bytes, 128u);
  SetThreadLocalMallocGuardReaction(std::nullopt);
}

TEST(MallocGuardStaticAllocatorHooks, DenylistLeadsToAbort) {
  ScopedMallocGuardHook hook;

  // Setting a denylist is not allowed with statically linked allocator hooks:
  // such a hook fires process-wide and cannot attribute an allocation to the
  // library that requested it.
  EXPECT_DEATH([]() { SetMallocGuardDenylist({"my_library.so"}); }(),
               "library denylisting.");
}

}  // namespace intrinsic

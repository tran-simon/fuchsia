// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fit/defer.h>
#include <zircon/syscalls-next.h>
#include <zircon/syscalls.h>

#include <zxtest/zxtest.h>

#include "test_thread.h"
#include "userpager.h"

namespace pager_tests {

// Tests that a VMO created with TRAP_DIRTY can be supplied, and generates VMO_DIRTY requests when
// written to.
VMO_VMAR_TEST(PagerWriteback, SimpleTrapDirty) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));

  TestThread t1([vmo, check_vmar]() -> bool { return check_buffer(vmo, 0, 1, check_vmar); });
  ASSERT_TRUE(t1.Start());
  ASSERT_TRUE(t1.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

  // Supply the page first and then attempt to write to it.
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));
  ASSERT_TRUE(t1.Wait());

  TestThread t2([vmo]() -> bool {
    uint8_t data = 0x77;
    return vmo->vmo().write(&data, 0, sizeof(data)) == ZX_OK;
  });
  ASSERT_TRUE(t2.Start());
  ASSERT_TRUE(t2.WaitForBlocked());

  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));

  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));
  ASSERT_TRUE(t2.Wait());

  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Writes to a VMO created without TRAP_DIRTY go through without blocking.
  Vmo* vmo_no_trap;
  ASSERT_TRUE(pager.CreateVmo(1, &vmo_no_trap));
  ASSERT_TRUE(pager.SupplyPages(vmo_no_trap, 0, 1));
  uint8_t data = 0xcc;
  ASSERT_OK(vmo_no_trap->vmo().write(&data, 0, sizeof(data)));

  // Verify that a non pager-backed vmo cannot be created with TRAP_DIRTY.
  zx_handle_t handle;
  ASSERT_EQ(ZX_ERR_INVALID_ARGS,
            zx_vmo_create(zx_system_get_page_size(), ZX_VMO_TRAP_DIRTY, &handle));
}

// Tests that writing to the VMO with zx_vmo_write generates DIRTY requests as expected.
TEST(PagerWriteback, DirtyRequestsOnVmoWrite) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 20;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  TestThread t([vmo]() -> bool {
    uint64_t data = 77;
    // write alternate pages {0, 2, 4, 6, 8}.
    for (uint64_t i = 0; i < kNumPages / 2; i += 2) {
      if (vmo->vmo().write(&data, i * zx_system_get_page_size(), sizeof(data)) != ZX_OK) {
        return false;
      }
    }
    // write consecutive runs of pages too.
    // pages written at this point are [0] [2,3,4] [6] [8].
    if (vmo->vmo().write(&data, 3 * zx_system_get_page_size(), sizeof(data)) != ZX_OK) {
      return false;
    }
    uint8_t buf[5 * zx_system_get_page_size()];
    // pages written are [11, 16).
    return vmo->vmo().write(&buf, 11 * zx_system_get_page_size(), sizeof(buf)) == ZX_OK;
  });
  ASSERT_TRUE(t.Start());

  for (uint64_t i = 0; i < kNumPages / 2; i += 2) {
    ASSERT_TRUE(t.WaitForBlocked());
    ASSERT_TRUE(pager.WaitForPageDirty(vmo, i, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.DirtyPages(vmo, i, 1));
  }

  ASSERT_TRUE(t.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 3, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 3, 1));

  ASSERT_TRUE(t.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 11, 5, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 11, 5));

  ASSERT_TRUE(t.Wait());

  // Verify dirty ranges.
  zx_vmo_dirty_range_t ranges[] = {{0, 1, 0}, {2, 3, 0}, {6, 1, 0}, {8, 1, 0}, {11, 5, 0}};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, ranges, sizeof(ranges) / sizeof(zx_vmo_dirty_range_t)));

  // No more requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));
}

// Tests that writing to the VMO through a VM mapping generates DIRTY requests as expected.
TEST(PagerWriteback, DirtyRequestsViaMapping) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 20;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  zx_vaddr_t ptr;
  TestThread t([vmo, &ptr]() -> bool {
    // Map the vmo.
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                   kNumPages * zx_system_get_page_size(), &ptr) != ZX_OK) {
      fprintf(stderr, "could not map vmo\n");
      return false;
    }

    auto buf = reinterpret_cast<uint8_t*>(ptr);
    // write alternate pages {0, 2, 4, 6, 8}.
    for (uint64_t i = 0; i < kNumPages / 2; i += 2) {
      buf[i * zx_system_get_page_size()] = 0xcc;
    }
    // write consecutive runs of pages too.
    // pages written at this point are [0] [2,3,4] [6] [8].
    buf[3 * zx_system_get_page_size()] = 0xcc;
    // pages written are [11, 16).
    for (uint64_t i = 11; i < 16; i++) {
      buf[i * zx_system_get_page_size()] = 0xcc;
    }
    return true;
  });

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, kNumPages * zx_system_get_page_size());
  });

  ASSERT_TRUE(t.Start());

  for (uint64_t i = 0; i < kNumPages / 2; i += 2) {
    ASSERT_TRUE(t.WaitForBlocked());
    ASSERT_TRUE(pager.WaitForPageDirty(vmo, i, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.DirtyPages(vmo, i, 1));
  }

  ASSERT_TRUE(t.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 3, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 3, 1));

  ASSERT_TRUE(t.WaitForBlocked());
  // We're touching pages one by one via the mapping, so we'll see page requests for individual
  // pages. Wait for the first page request and dirty the whole range.
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 11, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 11, 5));

  ASSERT_TRUE(t.Wait());

  // Verify dirty ranges.
  zx_vmo_dirty_range_t ranges[] = {{0, 1, 0}, {2, 3, 0}, {6, 1, 0}, {8, 1, 0}, {11, 5, 0}};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, ranges, sizeof(ranges) / sizeof(zx_vmo_dirty_range_t)));

  // No more requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));
}

// Tests that no DIRTY requests are generated on a read.
TEST(PagerWriteback, NoDirtyRequestsOnRead) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 3;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));

  zx_vaddr_t ptr;
  uint8_t tmp;
  TestThread t([vmo, &ptr, &tmp]() -> bool {
    // Map the vmo.
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                   kNumPages * zx_system_get_page_size(), &ptr) != ZX_OK) {
      fprintf(stderr, "could not map vmo\n");
      return false;
    }

    auto buf = reinterpret_cast<uint8_t*>(ptr);
    // Read pages.
    for (uint64_t i = 0; i < kNumPages; i++) {
      tmp = buf[i * zx_system_get_page_size()];
    }
    return true;
  });

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, kNumPages * zx_system_get_page_size());
  });

  ASSERT_TRUE(t.Start());

  ASSERT_TRUE(t.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  ASSERT_TRUE(t.Wait());

  // No dirty requests should be seen as none of the pages were dirtied.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));

  uint8_t buf[kNumPages * zx_system_get_page_size()];
  ASSERT_TRUE(vmo->vmo().read(buf, 0, kNumPages * zx_system_get_page_size()) == ZX_OK);

  // No dirty requests should be seen as none of the pages were dirtied.
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));

  // No remaining reads.
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  // No dirty pages.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));
}

// Tests that DIRTY requests are generated only on the first write.
TEST(PagerWriteback, DirtyRequestsRepeatedWrites) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  zx_vaddr_t ptr;
  TestThread t1([vmo, &ptr]() -> bool {
    // Map the vmo.
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                   zx_system_get_page_size(), &ptr) != ZX_OK) {
      fprintf(stderr, "could not map vmo\n");
      return false;
    }

    *reinterpret_cast<uint8_t*>(ptr) = 0xcc;
    return true;
  });

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, zx_system_get_page_size());
  });

  ASSERT_TRUE(t1.Start());

  ASSERT_TRUE(t1.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  ASSERT_TRUE(t1.Wait());

  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Write to the page again.
  TestThread t2([ptr]() -> bool {
    *reinterpret_cast<uint8_t*>(ptr) = 0xdd;
    return true;
  });

  ASSERT_TRUE(t2.Start());

  // No more requests seen.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  ASSERT_TRUE(t2.Wait());

  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
}

// Tests that DIRTY requests are generated on a write to a page that was previously read from.
TEST(PagerWriteback, DirtyRequestsOnWriteAfterRead) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  zx_vaddr_t ptr;
  uint8_t tmp;
  TestThread t1([vmo, &ptr, &tmp]() -> bool {
    // Map the vmo.
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                   zx_system_get_page_size(), &ptr) != ZX_OK) {
      fprintf(stderr, "could not map vmo\n");
      return false;
    }

    // Read from the page.
    tmp = *reinterpret_cast<uint8_t*>(ptr);
    return true;
  });

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, zx_system_get_page_size());
  });

  ASSERT_TRUE(t1.Start());

  // No read or dirty requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  ASSERT_TRUE(t1.Wait());

  // Now write to the page. This should trigger a dirty request.
  TestThread t2([ptr]() -> bool {
    *reinterpret_cast<uint8_t*>(ptr) = 0xdd;
    return true;
  });

  ASSERT_TRUE(t2.Start());

  ASSERT_TRUE(t2.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  ASSERT_TRUE(t1.Wait());

  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // No more requests.
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));
}

// Tests that no DIRTY requests are generated for clones of pager-backed VMOs.
TEST(PagerWriteback, NoDirtyRequestsForClones) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 3;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));

  auto clone = vmo->Clone();
  ASSERT_NOT_NULL(clone);

  // Write to the clone.
  TestThread t1([&vmo = clone->vmo()]() -> bool {
    uint8_t data[kNumPages * zx_system_get_page_size()];
    return vmo.write(data, 0, kNumPages * zx_system_get_page_size()) == ZX_OK;
  });
  ASSERT_TRUE(t1.Start());

  ASSERT_TRUE(t1.WaitForBlocked());
  // Writing the pages in the clone should trigger faults in the parent. Wait to see the first one.
  ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  // No dirty requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));

  ASSERT_TRUE(t1.Wait());

  // No dirty pages.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));

  // Write to the parent now. This should trigger dirty requests.
  TestThread t2([&vmo = vmo->vmo()]() -> bool {
    uint8_t data[kNumPages * zx_system_get_page_size()];
    return vmo.write(data, 0, kNumPages * zx_system_get_page_size()) == ZX_OK;
  });
  ASSERT_TRUE(t2.Start());

  ASSERT_TRUE(t2.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, kNumPages, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, kNumPages));

  // Should now see the pages dirty.
  zx_vmo_dirty_range_t range = {.offset = 0, .length = kNumPages};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // No remaining requests.
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));
}

// Tests that writes for overlapping ranges generate the expected DIRTY requests.
TEST(PagerWriteback, DirtyRequestsOverlap) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 20;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  TestThread t1([vmo]() -> bool {
    // write pages [4,9).
    uint8_t data[5 * zx_system_get_page_size()];
    memset(data, 0xaa, sizeof(data));
    return vmo->vmo().write((void*)&data, 4 * zx_system_get_page_size(), sizeof(data)) == ZX_OK;
  });
  ASSERT_TRUE(t1.Start());
  ASSERT_TRUE(t1.WaitForBlocked());

  TestThread t2([vmo]() -> bool {
    // write pages [2,9).
    uint8_t data[7 * zx_system_get_page_size()];
    memset(data, 0xbb, sizeof(data));
    return vmo->vmo().write((void*)&data, 2 * zx_system_get_page_size(), sizeof(data)) == ZX_OK;
  });
  ASSERT_TRUE(t2.Start());
  ASSERT_TRUE(t2.WaitForBlocked());

  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 4, 5, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 2, 2, ZX_TIME_INFINITE));

  // Dirty the range [4,9).
  ASSERT_TRUE(pager.DirtyPages(vmo, 4, 5));
  ASSERT_TRUE(t1.Wait());

  // Dirty the range [2,4).
  ASSERT_TRUE(pager.DirtyPages(vmo, 2, 2));
  ASSERT_TRUE(t2.Wait());

  // Verify dirty ranges.
  std::vector<zx_vmo_dirty_range_t> ranges;
  ranges.push_back({.offset = 2, .length = 7});
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, ranges.data(), ranges.size()));

  TestThread t3([vmo]() -> bool {
    // write pages [11,16).
    uint8_t data[5 * zx_system_get_page_size()];
    memset(data, 0xcc, sizeof(data));
    return vmo->vmo().write((void*)&data, 11 * zx_system_get_page_size(), sizeof(data)) == ZX_OK;
  });
  ASSERT_TRUE(t3.Start());
  ASSERT_TRUE(t3.WaitForBlocked());

  TestThread t4([vmo]() -> bool {
    // write pages [15,19).
    uint8_t data[4 * zx_system_get_page_size()];
    memset(data, 0xdd, sizeof(data));
    return vmo->vmo().write((void*)&data, 15 * zx_system_get_page_size(), sizeof(data)) == ZX_OK;
  });
  ASSERT_TRUE(t4.Start());
  ASSERT_TRUE(t4.WaitForBlocked());

  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 11, 5, ZX_TIME_INFINITE));
  // No remaining requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));

  // Dirty the range [11,16).
  ASSERT_TRUE(pager.DirtyPages(vmo, 11, 5));

  // This should terminate t3, and wake up t3 until it blocks again for the remaining range.
  ASSERT_TRUE(t3.Wait());
  ASSERT_TRUE(t4.WaitForBlocked());

  // Verify dirty ranges.
  ranges.push_back({.offset = 11, .length = 5});
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, ranges.data(), ranges.size()));

  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 16, 3, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 16, 3));

  ASSERT_TRUE(t4.Wait());

  // Verify dirty ranges.
  ranges.back().length = 8;
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, ranges.data(), ranges.size()));

  // No remaining requests.
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
}

// Tests that DIRTY requests are generated as expected for a VMO that has random offsets in various
// page states: {Empty, Clean, Dirty}.
TEST(PagerWriteback, DirtyRequestsRandomOffsets) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 10;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));

  int page_state[kNumPages];  // 0 for empty, 1 for clean, and 2 for dirty
  for (uint64_t i = 0; i < kNumPages; i++) {
    page_state[i] = rand() % 3;
    if (page_state[i] == 0) {
      // Page not present. Skip ahead.
      continue;
    } else if (page_state[i] == 1) {
      // Page is present and clean.
      ASSERT_TRUE(pager.SupplyPages(vmo, i, 1));
    } else {
      // Page is present and dirty.
      ASSERT_TRUE(pager.SupplyPages(vmo, i, 1));
      ASSERT_TRUE(pager.DirtyPages(vmo, i, 1));
    }
  }

  // Now write to the entire range. We should see a combination of read and dirty requests.
  TestThread t([&vmo = vmo->vmo()]() -> bool {
    uint8_t data[kNumPages * zx_system_get_page_size()];
    return vmo.write(data, 0, kNumPages * zx_system_get_page_size()) == ZX_OK;
  });
  ASSERT_TRUE(t.Start());

  uint64_t clean_start = 0;
  uint64_t clean_len = 0;
  for (uint64_t i = 0; i < kNumPages; i++) {
    if (page_state[i] == 0) {
      // Page is not present.
      // This might break an in-progress clean run, resolve that first.
      if (clean_len > 0) {
        ASSERT_TRUE(t.WaitForBlocked());
        ASSERT_TRUE(pager.WaitForPageDirty(vmo, clean_start, clean_len, ZX_TIME_INFINITE));
        ASSERT_TRUE(pager.DirtyPages(vmo, clean_start, clean_len));
      }
      // Should see a read request for this page now.
      ASSERT_TRUE(t.WaitForBlocked());
      ASSERT_TRUE(pager.WaitForPageRead(vmo, i, 1, ZX_TIME_INFINITE));
      ASSERT_TRUE(pager.SupplyPages(vmo, i, 1));

      // After the supply, visit this page again, as it might get combined into a subsequent clean
      // run. Set the page's state to clean, and decrement i.
      page_state[i] = 1;
      i--;

      clean_start = i + 1;
      clean_len = 0;

    } else if (page_state[i] == 1) {
      // Page is present and clean. Accumulate into the clean run.
      clean_len++;

    } else {
      // Page is present and dirty.
      // This might break an in-progress clean run, resolve that first.
      if (clean_len > 0) {
        ASSERT_TRUE(t.WaitForBlocked());
        ASSERT_TRUE(pager.WaitForPageDirty(vmo, clean_start, clean_len, ZX_TIME_INFINITE));
        ASSERT_TRUE(pager.DirtyPages(vmo, clean_start, clean_len));
      }
      clean_start = i + 1;
      clean_len = 0;
    }
  }

  // Resolve the last clean run if any.
  if (clean_len > 0) {
    ASSERT_TRUE(t.WaitForBlocked());
    ASSERT_TRUE(pager.WaitForPageDirty(vmo, clean_start, clean_len, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.DirtyPages(vmo, clean_start, clean_len));
  }

  // No remaining requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));
}

// Tests that ZX_PAGER_OP_FAIL can fail DIRTY page requests and propagate the failure up.
TEST(PagerWriteback, FailDirtyRequests) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 2;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  zx_vaddr_t ptr;
  TestThread t1([vmo, &ptr]() -> bool {
    // Map the vmo.
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                   zx_system_get_page_size(), &ptr) != ZX_OK) {
      fprintf(stderr, "could not map vmo\n");
      return false;
    }
    // Write page 0.
    auto buf = reinterpret_cast<uint8_t*>(ptr);
    buf[0] = 0xcc;
    return true;
  });

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, zx_system_get_page_size());
  });

  ASSERT_TRUE(t1.Start());

  ASSERT_TRUE(t1.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.FailPages(vmo, 0, 1));

  ASSERT_TRUE(t1.WaitForCrash(ptr, ZX_ERR_IO));

  TestThread t2([vmo]() -> bool {
    uint8_t data = 0xdd;
    // Write page 1.
    return vmo->vmo().write(&data, zx_system_get_page_size(), sizeof(data)) == ZX_OK;
  });

  ASSERT_TRUE(t2.Start());

  ASSERT_TRUE(t2.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 1, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.FailPages(vmo, 1, 1));

  ASSERT_TRUE(t2.WaitForFailure());

  // No pages should be dirty.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));
}

// Tests that no DIRTY requests are generated on a commit.
TEST(PagerWriteback, NoDirtyRequestsOnCommit) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 5;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));
  // Supply some pages.
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 2));

  // Commit the vmo.
  TestThread t([vmo]() -> bool {
    return vmo->vmo().op_range(ZX_VMO_OP_COMMIT, 0, kNumPages * zx_system_get_page_size(), nullptr,
                               0) == ZX_OK;
  });
  ASSERT_TRUE(t.Start());

  ASSERT_TRUE(t.WaitForBlocked());
  // Should see a read request for the uncommitted portion.
  ASSERT_TRUE(pager.WaitForPageRead(vmo, 2, kNumPages - 2, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.SupplyPages(vmo, 2, kNumPages - 2));

  // The thread should be able to exit now.
  ASSERT_TRUE(t.Wait());

  // No dirty requests should be seen as none of the pages were dirtied.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));

  // No remaining reads.
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  // No dirty pages.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));
}

// Tests that no DIRTY requests are generated when a mapping is created with MAP_RANGE.
TEST(PagerWriteback, NoDirtyRequestsOnMapRange) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  constexpr uint64_t kNumPages = 3;
  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(kNumPages, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

  zx_vaddr_t ptr;
  TestThread t1([vmo, &ptr]() -> bool {
    // Map the vmo, and populate mappings for all committed pages. We know the pages are
    // pre-committed so we should not block on reads. And we should not be generating any dirty
    // requests to block on either.
    return zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE, 0,
                                      vmo->vmo(), 0, kNumPages * zx_system_get_page_size(),
                                      &ptr) == ZX_OK;
  });
  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, kNumPages * zx_system_get_page_size());
  });

  ASSERT_TRUE(t1.Start());

  // No dirty requests should be seen as none of the pages were dirtied.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  // No reads either.
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  ASSERT_TRUE(t1.Wait());

  uint8_t tmp;
  TestThread t2([&ptr, &tmp]() -> bool {
    // Read the mapped pages. This will not block.
    auto buf = reinterpret_cast<uint8_t*>(ptr);
    for (uint64_t i = 0; i < kNumPages; i++) {
      tmp = buf[i * zx_system_get_page_size()];
    }
    return true;
  });

  ASSERT_TRUE(t2.Start());

  // No dirty or read requests.
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  ASSERT_TRUE(t2.Wait());

  // No dirty pages.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));

  TestThread t3([&ptr]() -> bool {
    // Now try to write to the vmo. This should result in write faults and dirty requests.
    auto buf = reinterpret_cast<uint8_t*>(ptr);
    for (uint64_t i = 0; i < kNumPages; i++) {
      buf[i * zx_system_get_page_size()] = 0xcc;
    }
    return true;
  });

  ASSERT_TRUE(t3.Start());

  // The thread will block on dirty requests for each page.
  for (uint64_t i = 0; i < kNumPages; i++) {
    ASSERT_TRUE(t3.WaitForBlocked());
    ASSERT_TRUE(pager.WaitForPageDirty(vmo, i, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.DirtyPages(vmo, i, 1));
  }

  // The thread should now exit.
  ASSERT_TRUE(t3.Wait());

  // All pages are dirty now.
  zx_vmo_dirty_range_t range = {.offset = 0, .length = kNumPages};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // No more dirty or read requests.
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));
}

// Tests that no DIRTY requests are generated when previously dirty pages are mapped and written to.
TEST(PagerWriteback, NoDirtyRequestsMapExistingDirty) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  // Dirty the page.
  TestThread t1([vmo]() -> bool {
    uint8_t data = 0xcc;
    return vmo->vmo().write(&data, 0, sizeof(data)) == ZX_OK;
  });

  ASSERT_TRUE(t1.Start());

  ASSERT_TRUE(t1.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  ASSERT_TRUE(t1.Wait());

  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Map the page and try writing to it.
  zx_vaddr_t ptr;
  TestThread t2([vmo, &ptr]() -> bool {
    // Map the vmo.
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                   zx_system_get_page_size(), &ptr) != ZX_OK) {
      fprintf(stderr, "could not map vmo\n");
      return false;
    }

    *reinterpret_cast<uint8_t*>(ptr) = 0xdd;
    return true;
  });

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, zx_system_get_page_size());
  });

  ASSERT_TRUE(t2.Start());

  // No read or dirty requests.
  uint64_t offset, length;
  ASSERT_FALSE(pager.GetPageDirtyRequest(vmo, 0, &offset, &length));
  ASSERT_FALSE(pager.GetPageReadRequest(vmo, 0, &offset, &length));

  ASSERT_TRUE(t2.Wait());

  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
}

// Tests that dirty ranges cannot be queried on a clone.
TEST(PagerWriteback, NoQueryOnClone) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  // Dirty the page.
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  // Create a clone.
  auto clone = vmo->Clone();
  ASSERT_NOT_NULL(clone);

  // Write to the clone.
  uint8_t data = 0x77;
  ASSERT_OK(clone->vmo().write(&data, 0, sizeof(data)));

  // Can query dirty ranges on the parent.
  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Cannot query dirty ranges on the clone.
  uint64_t num_ranges = 0;
  ASSERT_EQ(ZX_ERR_INVALID_ARGS,
            zx_pager_query_dirty_ranges(pager.pager().get(), clone->vmo().get(), 0,
                                        zx_system_get_page_size(), &range, sizeof(range),
                                        &num_ranges, nullptr));
}

// Tests that WRITEBACK_BEGIN/END clean pages as expected.
TEST(PagerWriteback, SimpleWriteback) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  // Dirty the page.
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Begin writeback on the page.
  ASSERT_TRUE(pager.WritebackBeginPages(vmo, 0, 1));

  // The page is still dirty.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // This should transition the page to clean, and a subsequent write should trigger
  // another dirty request.
  ASSERT_TRUE(pager.WritebackEndPages(vmo, 0, 1));

  // No dirty pages after writeback end.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));

  TestThread t([vmo]() -> bool {
    uint64_t data = 77;
    return vmo->vmo().write(&data, 0, sizeof(data)) == ZX_OK;
  });

  ASSERT_TRUE(t.Start());

  // We should see a dirty request now.
  ASSERT_TRUE(t.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  ASSERT_TRUE(t.Wait());

  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
}

// Tests that a write after WRITEBACK_BEGIN but before WRITEBACK_END is handled correctly.
TEST(PagerWriteback, DirtyDuringWriteback) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  // Dirty the page.
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Begin writeback on the page.
  ASSERT_TRUE(pager.WritebackBeginPages(vmo, 0, 1));

  // The page is still dirty.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Write to the page before ending writeback. This should generate a dirty request.
  TestThread t1([vmo]() -> bool {
    uint8_t data = 0xcc;
    return vmo->vmo().write(&data, 0, sizeof(data)) == ZX_OK;
  });

  ASSERT_TRUE(t1.Start());

  // Verify that we saw the dirty request but do not acknowledge it yet. The write will remain
  // blocked.
  ASSERT_TRUE(t1.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));

  // End the writeback. This should transition the page to clean.
  ASSERT_TRUE(pager.WritebackEndPages(vmo, 0, 1));
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));

  // The writing thread is still blocked.
  ASSERT_TRUE(t1.WaitForBlocked());

  // Now dirty the page, unblocking the writing thread.
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));
  ASSERT_TRUE(t1.Wait());

  // The page is dirty again.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Begin another writeback, and try writing again before ending it. This time acknowledge the
  // dirty request while the writeback is in progress.
  ASSERT_TRUE(pager.WritebackBeginPages(vmo, 0, 1));
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Write to the page before ending writeback. This should generate a dirty request.
  TestThread t2([vmo]() -> bool {
    uint8_t data = 0xdd;
    return vmo->vmo().write(&data, 0, sizeof(data)) == ZX_OK;
  });

  ASSERT_TRUE(t2.Start());

  // Verify that we saw the dirty request.
  ASSERT_TRUE(t2.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));

  // This should reset the page state to dirty so that it is not moved to clean when the writeback
  // ends later.
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  ASSERT_TRUE(t2.Wait());

  // Verify that the page is dirty.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));

  // Now end the writeback. This should *not* clean the page, as a write was accepted after
  // beginning the writeback.
  ASSERT_TRUE(pager.WritebackEndPages(vmo, 0, 1));
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
}

// Tests that mapping write permissions are cleared as expected on writeback.
TEST(PagerWriteback, WritebackWithMapping) {
  UserPager pager;
  ASSERT_TRUE(pager.Init());

  Vmo* vmo;
  ASSERT_TRUE(pager.CreateVmoWithOptions(1, ZX_VMO_TRAP_DIRTY, &vmo));
  ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

  zx_vaddr_t ptr;
  // Map the vmo.
  ASSERT_OK(zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo->vmo(), 0,
                                       zx_system_get_page_size(), &ptr));

  auto unmap = fit::defer([&]() {
    // Cleanup the mapping we created.
    zx::vmar::root_self()->unmap(ptr, zx_system_get_page_size());
  });

  // Write to the vmo. This will be trapped and generate a dirty request.
  auto buf = reinterpret_cast<uint8_t*>(ptr);
  uint8_t data = 0xaa;
  TestThread t1([buf, data]() -> bool {
    *buf = data;
    return true;
  });

  ASSERT_TRUE(t1.Start());

  ASSERT_TRUE(t1.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));

  // Dirty the page.
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));
  ASSERT_TRUE(t1.Wait());

  // Verify that the page is dirty.
  zx_vmo_dirty_range_t range = {.offset = 0, .length = 1};
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
  ASSERT_EQ(data, *buf);

  // Write to the page again. This should go through without any page faults / dirty requests.
  data = 0xbb;
  *buf = data;
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
  ASSERT_EQ(data, *buf);

  // Start a writeback.
  ASSERT_TRUE(pager.WritebackBeginPages(vmo, 0, 1));
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
  ASSERT_EQ(data, *buf);

  // Write to the page again. This should result in a fault / dirty request.
  TestThread t2([buf]() -> bool {
    *buf = 0xcc;
    return true;
  });

  ASSERT_TRUE(t2.Start());

  ASSERT_TRUE(t2.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));

  // Fail the dirty request so the writeback can complete.
  ASSERT_TRUE(pager.FailPages(vmo, 0, 1));
  ASSERT_TRUE(t2.WaitForCrash(ptr, ZX_ERR_IO));

  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
  ASSERT_EQ(data, *buf);

  // Complete the writeback, making the page clean.
  ASSERT_TRUE(pager.WritebackEndPages(vmo, 0, 1));
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, nullptr, 0));
  ASSERT_EQ(data, *buf);

  // Write to the page again. This should again be trapped.
  data = 0xdd;
  TestThread t3([buf, data]() -> bool {
    *buf = data;
    return true;
  });

  ASSERT_TRUE(t3.Start());

  ASSERT_TRUE(t3.WaitForBlocked());
  ASSERT_TRUE(pager.WaitForPageDirty(vmo, 0, 1, ZX_TIME_INFINITE));
  ASSERT_TRUE(pager.DirtyPages(vmo, 0, 1));

  ASSERT_TRUE(t3.Wait());

  // The page is dirty.
  ASSERT_TRUE(pager.VerifyDirtyRanges(vmo, &range, 1));
  ASSERT_EQ(data, *buf);
}

}  // namespace pager_tests

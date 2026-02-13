#include "nic/rocev2/memory_region.h"

#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <tracy/Tracy.hpp>

#include "nic/rocev2/protection_domain.h"
#include "nic/rocev2/types.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

static constexpr std::size_t kTestMemorySize = 64 * 1024;

// =============================================================================
// Memory Region Tests
// =============================================================================

static void test_register_mr_success() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_register_mr_success... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true, .local_write = true};
  [[maybe_unused]] auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);

  assert(lkey.has_value());
  assert(lkey.value() != 0);
  assert(mr_table.count() == 1);
  assert(mr_table.stats().registrations == 1);

  std::cout << "PASSED\n";
}

static void test_register_mr_zero_length() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_register_mr_zero_length... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{};
  [[maybe_unused]] auto lkey = mr_table.register_mr(1, 0x1000, 0, access);

  assert(!lkey.has_value());
  assert(mr_table.count() == 0);
  assert(mr_table.stats().registration_failures == 1);

  std::cout << "PASSED\n";
}

static void test_deregister_mr_success() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_deregister_mr_success... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  [[maybe_unused]] bool result = mr_table.deregister_mr(lkey.value());

  assert(result);
  assert(mr_table.count() == 0);
  assert(mr_table.stats().deregistrations == 1);

  std::cout << "PASSED\n";
}

static void test_deregister_mr_not_found() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_deregister_mr_not_found... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  [[maybe_unused]] bool result = mr_table.deregister_mr(0xDEAD);

  assert(!result);

  std::cout << "PASSED\n";
}

static void test_validate_lkey_success() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_lkey_success... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true, .local_write = true};
  [[maybe_unused]] auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  // Read access
  assert(mr_table.validate_lkey(lkey.value(), 0x1000, 100, false));

  // Write access
  assert(mr_table.validate_lkey(lkey.value(), 0x1000, 100, true));

  // Access at end of region
  assert(mr_table.validate_lkey(lkey.value(), 0x1000 + 4096 - 100, 100, false));

  std::cout << "PASSED\n";
}

static void test_validate_lkey_invalid_key() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_lkey_invalid_key... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  assert(!mr_table.validate_lkey(0xDEAD, 0x1000, 100, false));
  assert(mr_table.stats().access_errors == 1);

  std::cout << "PASSED\n";
}

static void test_validate_lkey_out_of_bounds() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_lkey_out_of_bounds... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true};
  [[maybe_unused]] auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  // Before region
  assert(!mr_table.validate_lkey(lkey.value(), 0x0FFF, 100, false));

  // After region
  assert(!mr_table.validate_lkey(lkey.value(), 0x1000 + 4096, 100, false));

  // Crossing end boundary
  assert(!mr_table.validate_lkey(lkey.value(), 0x1000 + 4000, 200, false));

  std::cout << "PASSED\n";
}

static void test_validate_lkey_no_write_permission() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_lkey_no_write_permission... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true, .local_write = false};
  [[maybe_unused]] auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  // Read should succeed
  assert(mr_table.validate_lkey(lkey.value(), 0x1000, 100, false));

  // Write should fail
  assert(!mr_table.validate_lkey(lkey.value(), 0x1000, 100, true));

  std::cout << "PASSED\n";
}

static void test_validate_rkey_success() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_rkey_success... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true, .remote_read = true, .remote_write = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  [[maybe_unused]] const auto* mr = mr_table.get_by_lkey(lkey.value());
  assert(mr != nullptr);

  // Remote read
  assert(mr_table.validate_rkey(mr->rkey, 1, 0x1000, 100, false));

  // Remote write
  assert(mr_table.validate_rkey(mr->rkey, 1, 0x1000, 100, true));

  std::cout << "PASSED\n";
}

static void test_validate_rkey_wrong_pd() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_rkey_wrong_pd... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.remote_read = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  [[maybe_unused]] const auto* mr = mr_table.get_by_lkey(lkey.value());
  assert(mr != nullptr);

  // Wrong PD should fail
  assert(!mr_table.validate_rkey(mr->rkey, 2, 0x1000, 100, false));

  std::cout << "PASSED\n";
}

static void test_validate_rkey_no_remote_access() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_validate_rkey_no_remote_access... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true, .remote_read = false, .remote_write = false};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  [[maybe_unused]] const auto* mr = mr_table.get_by_lkey(lkey.value());
  assert(mr != nullptr);

  // Remote access should fail
  assert(!mr_table.validate_rkey(mr->rkey, 1, 0x1000, 100, false));
  assert(!mr_table.validate_rkey(mr->rkey, 1, 0x1000, 100, true));

  std::cout << "PASSED\n";
}

static void test_get_by_lkey() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_get_by_lkey... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  [[maybe_unused]] const auto* mr = mr_table.get_by_lkey(lkey.value());

  assert(mr != nullptr);
  assert(mr->lkey == lkey.value());
  assert(mr->virtual_address == 0x1000);
  assert(mr->length == 4096);
  assert(mr->pd_handle == 1);

  std::cout << "PASSED\n";
}

static void test_get_by_rkey() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_get_by_rkey... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.remote_read = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  const auto* mr_by_lkey = mr_table.get_by_lkey(lkey.value());
  assert(mr_by_lkey != nullptr);

  [[maybe_unused]] const auto* mr_by_rkey = mr_table.get_by_rkey(mr_by_lkey->rkey);

  assert(mr_by_rkey != nullptr);
  assert(mr_by_rkey == mr_by_lkey);

  std::cout << "PASSED\n";
}

static void test_mr_reset() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_mr_reset... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  AccessFlags access{.local_read = true};
  (void) mr_table.register_mr(1, 0x1000, 4096, access);
  (void) mr_table.register_mr(2, 0x2000, 4096, access);
  assert(mr_table.count() == 2);

  mr_table.reset();

  assert(mr_table.count() == 0);
  assert(mr_table.stats().registrations == 0);

  std::cout << "PASSED\n";
}

static void test_max_mrs_limit() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_max_mrs_limit... " << std::flush;

  nic::HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  nic::SimpleHostMemory host_memory{mem_cfg};
  MrTableConfig config{.max_mrs = 2};
  MemoryRegionTable limited_table(config);

  AccessFlags access{.local_read = true};
  assert(limited_table.register_mr(1, 0x1000, 1024, access).has_value());
  assert(limited_table.register_mr(1, 0x2000, 1024, access).has_value());

  // Third should fail
  assert(!limited_table.register_mr(1, 0x3000, 1024, access).has_value());
  assert(limited_table.stats().registration_failures == 1);

  std::cout << "PASSED\n";
}

// =============================================================================
// Protection Domain Tests
// =============================================================================

static void test_pd_allocate_success() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_allocate_success... " << std::flush;

  PdTable pd_table;
  [[maybe_unused]] auto handle = pd_table.allocate();

  assert(handle.has_value());
  assert(handle.value() != 0);
  assert(pd_table.count() == 1);
  assert(pd_table.is_valid(handle.value()));

  std::cout << "PASSED\n";
}

static void test_pd_deallocate_success() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_deallocate_success... " << std::flush;

  PdTable pd_table;
  auto handle = pd_table.allocate();
  assert(handle.has_value());

  [[maybe_unused]] bool result = pd_table.deallocate(handle.value());

  assert(result);
  assert(pd_table.count() == 0);
  assert(!pd_table.is_valid(handle.value()));

  std::cout << "PASSED\n";
}

static void test_pd_deallocate_not_found() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_deallocate_not_found... " << std::flush;

  PdTable pd_table;
  [[maybe_unused]] bool result = pd_table.deallocate(0xDEAD);

  assert(!result);

  std::cout << "PASSED\n";
}

static void test_pd_get() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_get... " << std::flush;

  PdTable pd_table;
  auto handle = pd_table.allocate();
  assert(handle.has_value());

  [[maybe_unused]] auto* pd = pd_table.get(handle.value());

  assert(pd != nullptr);
  assert(pd->handle() == handle.value());

  std::cout << "PASSED\n";
}

static void test_pd_get_not_found() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_get_not_found... " << std::flush;

  PdTable pd_table;
  [[maybe_unused]] auto* pd = pd_table.get(0xDEAD);

  assert(pd == nullptr);

  std::cout << "PASSED\n";
}

static void test_pd_max_limit() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_max_limit... " << std::flush;

  PdTableConfig config{.max_pds = 2};
  PdTable limited_table(config);

  assert(limited_table.allocate().has_value());
  assert(limited_table.allocate().has_value());

  // Third should fail
  assert(!limited_table.allocate().has_value());
  assert(limited_table.stats().allocation_failures == 1);

  std::cout << "PASSED\n";
}

static void test_pd_reset() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_pd_reset... " << std::flush;

  PdTable pd_table;
  (void) pd_table.allocate();
  (void) pd_table.allocate();
  assert(pd_table.count() == 2);

  pd_table.reset();

  assert(pd_table.count() == 0);

  std::cout << "PASSED\n";
}

// =============================================================================
// Types Tests
// =============================================================================

static void test_advance_psn() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_advance_psn... " << std::flush;

  assert(advance_psn(0, 1) == 1);
  assert(advance_psn(kMaxPsn, 1) == 0);  // Wraparound
  assert(advance_psn(kMaxPsn - 1, 2) == 0);
  assert(advance_psn(0x100, 0x10) == 0x110);

  std::cout << "PASSED\n";
}

static void test_psn_in_window() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_psn_in_window... " << std::flush;

  assert(psn_in_window(100, 100, 100));   // At base
  assert(psn_in_window(150, 100, 100));   // In middle
  assert(psn_in_window(199, 100, 100));   // At end
  assert(!psn_in_window(200, 100, 100));  // Past window
  assert(!psn_in_window(99, 100, 100));   // Before base

  // Wraparound case
  assert(psn_in_window(0, kMaxPsn - 10, 20));  // Wrapped around

  std::cout << "PASSED\n";
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  std::cout << "\n=== RoCEv2 Memory Region Tests ===\n\n";

  // Memory Region tests
  test_register_mr_success();
  test_register_mr_zero_length();
  test_deregister_mr_success();
  test_deregister_mr_not_found();
  test_validate_lkey_success();
  test_validate_lkey_invalid_key();
  test_validate_lkey_out_of_bounds();
  test_validate_lkey_no_write_permission();
  test_validate_rkey_success();
  test_validate_rkey_wrong_pd();
  test_validate_rkey_no_remote_access();
  test_get_by_lkey();
  test_get_by_rkey();
  test_mr_reset();
  test_max_mrs_limit();

  // Protection Domain tests
  test_pd_allocate_success();
  test_pd_deallocate_success();
  test_pd_deallocate_not_found();
  test_pd_get();
  test_pd_get_not_found();
  test_pd_max_limit();
  test_pd_reset();

  // Types tests
  test_advance_psn();
  test_psn_in_window();

  std::cout << "\n=== All tests passed! ===\n\n";

  return 0;
}

static void WaitForTracyConnection() {
  const char* wait_env = std::getenv("NIC_WAIT_FOR_TRACY");
  if (!wait_env || wait_env[0] == '\0' || wait_env[0] == '0') {
    return;
  }

  const auto timeout = std::chrono::seconds(2);
  const auto start = std::chrono::steady_clock::now();
  while (!tracy::GetProfiler().IsConnected()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

/// @file pd_congestion_coverage_test.cpp
/// @brief Coverage tests for PdTable and ReliabilityManager/CongestionControlManager edge cases.
///
/// Exercises untested paths in protection_domain.cpp and congestion.cpp:
/// - PdTable: max limit, deallocate invalid, get invalid, reset
/// - ReliabilityManager: NAK syndromes (InvalidRequest, RemoteAccessError, RemoteOpError),
///   RNR retry exceeded, timeout retry exceeded
/// - CongestionControlManager: alpha update timing, clear_flow_state

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "nic/rocev2/congestion.h"
#include "nic/rocev2/protection_domain.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

// ============================================================
// PdTable tests
// ============================================================

/// Basic allocate and deallocate flow.
void test_pd_table_allocate_and_deallocate() {
  std::printf("  test_pd_table_allocate_and_deallocate...\n");
  NIC_TRACE_SCOPED(__func__);

  PdTable table;
  auto handle = table.allocate();
  assert(handle.has_value());
  assert(table.count() == 1);
  assert(table.is_valid(*handle));

  [[maybe_unused]] bool result = table.deallocate(*handle);
  assert(result);
  assert(table.count() == 0);
  assert(!table.is_valid(*handle));

  std::printf("    PASSED\n");
}

/// Allocating beyond max_pds should fail.
void test_pd_table_max_limit() {
  std::printf("  test_pd_table_max_limit...\n");
  NIC_TRACE_SCOPED(__func__);

  PdTableConfig config;
  config.max_pds = 2;
  PdTable table(config);

  [[maybe_unused]] auto h1 = table.allocate();
  assert(h1.has_value());
  [[maybe_unused]] auto h2 = table.allocate();
  assert(h2.has_value());
  assert(table.count() == 2);

  // Third allocation should fail
  [[maybe_unused]] auto h3 = table.allocate();
  assert(!h3.has_value());
  assert(table.count() == 2);

  // Verify failure counter incremented
  assert(table.stats().allocation_failures == 1);

  std::printf("    PASSED\n");
}

/// Deallocating an invalid handle should return false.
void test_pd_table_deallocate_invalid() {
  std::printf("  test_pd_table_deallocate_invalid...\n");
  NIC_TRACE_SCOPED(__func__);

  PdTable table;
  [[maybe_unused]] bool result = table.deallocate(9999);
  assert(!result);
  assert(table.count() == 0);

  std::printf("    PASSED\n");
}

/// get() with non-existent handle should return nullptr (both const and non-const).
void test_pd_table_get_invalid() {
  std::printf("  test_pd_table_get_invalid...\n");
  NIC_TRACE_SCOPED(__func__);

  PdTable table;

  // Non-const get
  [[maybe_unused]] ProtectionDomain* pd = table.get(9999);
  assert(pd == nullptr);

  // Const get
  const PdTable& const_table = table;
  [[maybe_unused]] const ProtectionDomain* const_pd = const_table.get(9999);
  assert(const_pd == nullptr);

  // Allocate one and verify valid handle works, then check invalid again
  auto handle = table.allocate();
  assert(handle.has_value());

  [[maybe_unused]] ProtectionDomain* valid_pd = table.get(*handle);
  assert(valid_pd != nullptr);
  assert(valid_pd->handle() == *handle);

  [[maybe_unused]] const ProtectionDomain* still_null = const_table.get(9999);
  assert(still_null == nullptr);

  std::printf("    PASSED\n");
}

/// reset() should clear all PDs and reset stats.
void test_pd_table_reset() {
  std::printf("  test_pd_table_reset...\n");
  NIC_TRACE_SCOPED(__func__);

  PdTable table;
  auto h1 = table.allocate();
  [[maybe_unused]] auto h2 = table.allocate();
  [[maybe_unused]] auto h3 = table.allocate();
  assert(h1.has_value() && h2.has_value() && h3.has_value());
  assert(table.count() == 3);
  assert(table.stats().allocations == 3);

  table.deallocate(*h1);
  assert(table.stats().deallocations == 1);

  table.reset();
  assert(table.count() == 0);
  assert(table.stats().allocations == 0);
  assert(table.stats().deallocations == 0);
  assert(table.stats().allocation_failures == 0);

  // Verify old handles are no longer valid
  assert(!table.is_valid(*h2));
  assert(!table.is_valid(*h3));

  std::printf("    PASSED\n");
}

/// Verify allocation/deallocation/failure counters track correctly.
void test_pd_table_stats() {
  std::printf("  test_pd_table_stats...\n");
  NIC_TRACE_SCOPED(__func__);

  PdTableConfig config;
  config.max_pds = 3;
  PdTable table(config);

  auto h1 = table.allocate();
  auto h2 = table.allocate();
  auto h3 = table.allocate();
  assert(table.stats().allocations == 3);
  assert(table.stats().allocation_failures == 0);

  // Try over-allocating
  [[maybe_unused]] auto h4 = table.allocate();
  assert(!h4.has_value());
  assert(table.stats().allocation_failures == 1);

  [[maybe_unused]] auto h5 = table.allocate();
  assert(!h5.has_value());
  assert(table.stats().allocation_failures == 2);

  // Deallocate one
  [[maybe_unused]] bool dealloc_result = table.deallocate(*h1);
  assert(dealloc_result);
  assert(table.stats().deallocations == 1);

  // Now we can allocate again
  auto h6 = table.allocate();
  assert(h6.has_value());
  assert(table.stats().allocations == 4);
  assert(table.stats().allocation_failures == 2);

  // Cleanup
  table.deallocate(*h2);
  table.deallocate(*h3);
  table.deallocate(*h6);
  assert(table.stats().deallocations == 4);
  assert(table.count() == 0);

  std::printf("    PASSED\n");
}

// ============================================================
// ReliabilityManager NAK tests
// ============================================================

/// process_nak with InvalidRequest syndrome should set RemoteInvalidRequestError.
void test_reliability_nak_invalid_request() {
  std::printf("  test_reliability_nak_invalid_request...\n");
  NIC_TRACE_SCOPED(__func__);

  ReliabilityManager manager;
  std::uint32_t qp_num = 1;
  std::uint32_t psn = 10;

  manager.add_pending(qp_num, psn, psn, 2001, WqeOpcode::Send, 100);

  AckResult result = manager.process_nak(qp_num, psn, AethSyndrome::InvalidRequest);
  assert(result.success);
  assert(result.error_status.has_value());
  assert(*result.error_status == WqeStatus::RemoteInvalidRequestError);
  assert(manager.stats().naks_received == 1);

  std::printf("    PASSED\n");
}

/// process_nak with RemoteAccessError syndrome should set RemoteAccessError status.
void test_reliability_nak_remote_access_error() {
  std::printf("  test_reliability_nak_remote_access_error...\n");
  NIC_TRACE_SCOPED(__func__);

  ReliabilityManager manager;
  std::uint32_t qp_num = 2;
  std::uint32_t psn = 20;

  manager.add_pending(qp_num, psn, psn, 3001, WqeOpcode::RdmaWrite, 200);

  AckResult result = manager.process_nak(qp_num, psn, AethSyndrome::RemoteAccessError);
  assert(result.success);
  assert(result.error_status.has_value());
  assert(*result.error_status == WqeStatus::RemoteAccessError);
  assert(manager.stats().naks_received == 1);

  std::printf("    PASSED\n");
}

/// process_nak with RemoteOpError syndrome should set RemoteOperationError status.
void test_reliability_nak_remote_op_error() {
  std::printf("  test_reliability_nak_remote_op_error...\n");
  NIC_TRACE_SCOPED(__func__);

  ReliabilityManager manager;
  std::uint32_t qp_num = 3;
  std::uint32_t psn = 30;

  manager.add_pending(qp_num, psn, psn, 4001, WqeOpcode::RdmaRead, 300);

  AckResult result = manager.process_nak(qp_num, psn, AethSyndrome::RemoteOpError);
  assert(result.success);
  assert(result.error_status.has_value());
  assert(*result.error_status == WqeStatus::RemoteOperationError);
  assert(manager.stats().naks_received == 1);

  std::printf("    PASSED\n");
}

/// RNR NAK should cause RnrRetryExceededError when retry count is exhausted.
void test_reliability_nak_rnr_retry_exceeded() {
  std::printf("  test_reliability_nak_rnr_retry_exceeded...\n");
  NIC_TRACE_SCOPED(__func__);

  ReliabilityConfig config;
  config.rnr_retry_count = 1;  // Allow only 1 RNR retry
  ReliabilityManager manager(config);

  std::uint32_t qp_num = 4;
  std::uint32_t psn = 40;
  manager.add_pending(qp_num, psn, psn, 5001, WqeOpcode::Send, 400);

  // First RNR NAK: retry_count goes to 1, still within limit
  AckResult result1 = manager.process_nak(qp_num, psn, AethSyndrome::RnrNak);
  assert(result1.success);
  assert(result1.needs_retransmit);
  assert(!result1.error_status.has_value());
  assert(manager.stats().rnr_retries == 1);

  // Second RNR NAK: retry_count goes to 2 > rnr_retry_count(1), should exceed
  AckResult result2 = manager.process_nak(qp_num, psn, AethSyndrome::RnrNak);
  assert(result2.success);
  assert(result2.error_status.has_value());
  assert(*result2.error_status == WqeStatus::RnrRetryExceededError);
  assert(manager.stats().retry_exceeded == 1);

  std::printf("    PASSED\n");
}

/// check_timeouts should return empty and mark retry_exceeded when retries exhausted.
void test_reliability_timeout_retry_exceeded() {
  std::printf("  test_reliability_timeout_retry_exceeded...\n");
  NIC_TRACE_SCOPED(__func__);

  ReliabilityConfig config;
  config.max_retries = 2;
  config.ack_timeout_us = 100;  // Short timeout for testing
  config.timeout_exponent = 0;  // No exponential backoff base
  ReliabilityManager manager(config);

  std::uint32_t qp_num = 5;
  std::uint32_t psn = 50;
  manager.add_pending(qp_num, psn, psn, 6001, WqeOpcode::Send, 0);

  // First timeout at time 200 (>= 100us): retry_count becomes 1, retransmit
  auto retransmit1 = manager.check_timeouts(qp_num, 200);
  assert(retransmit1.size() == 1);
  assert(retransmit1[0] == psn);
  assert(manager.stats().timeouts == 1);
  assert(manager.stats().retransmissions == 1);

  // Second timeout at time 600 (send_time was reset to 200, timeout doubles for retry):
  // retry_count becomes 2, still retransmit
  auto retransmit2 = manager.check_timeouts(qp_num, 600);
  assert(retransmit2.size() == 1);
  assert(manager.stats().timeouts == 2);
  assert(manager.stats().retransmissions == 2);

  // Third timeout: retry_count becomes 3 > max_retries(2), should mark exceeded
  auto retransmit3 = manager.check_timeouts(qp_num, 2000);
  assert(retransmit3.empty());
  assert(manager.stats().timeouts == 3);
  assert(manager.stats().retry_exceeded == 1);

  std::printf("    PASSED\n");
}

// ============================================================
// CongestionControlManager tests
// ============================================================

/// advance_time past alpha_update_period should update alpha.
void test_congestion_alpha_update() {
  std::printf("  test_congestion_alpha_update...\n");
  NIC_TRACE_SCOPED(__func__);

  DcqcnConfig config;
  config.enabled = true;
  config.alpha_update_period_us = 55;
  config.rate_increase_period_us = 50;
  config.initial_rate_mbps = 100000;
  CongestionControlManager manager(config);

  std::uint32_t qp_num = 1;

  // Trigger a CNP to create flow state and set alpha to be updated
  manager.handle_cnp_received(qp_num, 0);
  [[maybe_unused]] std::uint64_t rate_after_cnp = manager.get_current_rate(qp_num);
  assert(rate_after_cnp < config.initial_rate_mbps);

  // Advance time past the alpha_update_period
  manager.advance_time(60);

  // Alpha should have been updated (no CNP in this period, so alpha decreases)
  // Rate recovery also occurs since rate_increase_period_us=50 < 60
  [[maybe_unused]] std::uint64_t rate_after_recovery = manager.get_current_rate(qp_num);
  // Rate should have increased during recovery
  assert(rate_after_recovery >= rate_after_cnp);
  assert(manager.stats().rate_increases >= 1);

  std::printf("    PASSED\n");
}

/// handle_cnp then clear_flow_state should return rate to initial.
void test_congestion_clear_flow_state() {
  std::printf("  test_congestion_clear_flow_state...\n");
  NIC_TRACE_SCOPED(__func__);

  DcqcnConfig config;
  config.enabled = true;
  config.initial_rate_mbps = 100000;
  CongestionControlManager manager(config);

  std::uint32_t qp_num = 10;

  // Get initial rate (no flow state exists, should return initial rate)
  [[maybe_unused]] std::uint64_t initial_rate = manager.get_current_rate(qp_num);
  assert(initial_rate == config.initial_rate_mbps);

  // Handle CNP to decrease rate
  manager.handle_cnp_received(qp_num, 0);
  [[maybe_unused]] std::uint64_t reduced_rate = manager.get_current_rate(qp_num);
  assert(reduced_rate < initial_rate);

  // Clear flow state
  manager.clear_flow_state(qp_num);

  // Rate should return to initial (no flow state -> default)
  [[maybe_unused]] std::uint64_t restored_rate = manager.get_current_rate(qp_num);
  assert(restored_rate == config.initial_rate_mbps);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running PD and congestion coverage tests...\n");

  // PdTable tests
  test_pd_table_allocate_and_deallocate();
  test_pd_table_max_limit();
  test_pd_table_deallocate_invalid();
  test_pd_table_get_invalid();
  test_pd_table_reset();
  test_pd_table_stats();

  // ReliabilityManager NAK tests
  test_reliability_nak_invalid_request();
  test_reliability_nak_remote_access_error();
  test_reliability_nak_remote_op_error();
  test_reliability_nak_rnr_retry_exceeded();
  test_reliability_timeout_retry_exceeded();

  // CongestionControlManager tests
  test_congestion_alpha_update();
  test_congestion_clear_flow_state();

  std::printf("All PD and congestion coverage tests PASSED!\n");
  return 0;
}

static void WaitForTracyConnection() {
#ifdef TRACY_ENABLE
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
#endif
}

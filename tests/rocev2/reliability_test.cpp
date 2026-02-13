#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "nic/rocev2/congestion.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

// Test basic ACK processing
void test_ack_processing() {
  std::printf("  test_ack_processing...\n");

  ReliabilityManager manager;

  // Add some pending operations
  manager.add_pending(1, 0, 0, 1001, WqeOpcode::Send, 0);
  manager.add_pending(1, 1, 1, 1002, WqeOpcode::Send, 100);
  manager.add_pending(1, 2, 4, 1003, WqeOpcode::RdmaWrite, 200);  // Multi-packet

  // ACK up to PSN 1
  AckResult result = manager.process_ack(1, 1);
  assert(result.success);
  assert(result.completed_wr_ids.size() == 2);  // Operations 1001 and 1002
  assert(manager.stats().acks_received == 1);

  // ACK up to PSN 4 (completes the WRITE)
  result = manager.process_ack(1, 4);
  assert(result.success);
  assert(result.completed_wr_ids.size() == 1);  // Operation 1003
  assert(manager.stats().acks_received == 2);

  std::printf("    PASSED\n");
}

// Test NAK processing for PSN sequence error
void test_nak_psn_sequence_error() {
  std::printf("  test_nak_psn_sequence_error...\n");

  ReliabilityManager manager;

  // Add pending operation
  manager.add_pending(1, 5, 5, 1001, WqeOpcode::Send, 0);

  // Receive PSN sequence error NAK
  AckResult result = manager.process_nak(1, 5, AethSyndrome::PsnSeqError);
  assert(result.success);
  assert(result.needs_retransmit);
  assert(!result.error_status.has_value());  // Not fatal on first NAK
  assert(manager.stats().naks_received == 1);
  assert(manager.stats().retransmissions == 1);

  std::printf("    PASSED\n");
}

// Test NAK processing for RNR
void test_nak_rnr() {
  std::printf("  test_nak_rnr...\n");

  ReliabilityConfig config;
  config.rnr_retry_count = 3;
  ReliabilityManager manager{config};

  // Add pending operation
  manager.add_pending(1, 10, 10, 1001, WqeOpcode::Send, 0);

  // First RNR - should retry
  AckResult result = manager.process_nak(1, 10, AethSyndrome::RnrNak);
  assert(result.success);
  assert(result.needs_retransmit);
  assert(!result.error_status.has_value());
  assert(manager.stats().rnr_retries == 1);

  // Exhaust RNR retry count
  for (int retry_idx = 0; retry_idx < 3; ++retry_idx) {
    result = manager.process_nak(1, 10, AethSyndrome::RnrNak);
  }
  assert(result.error_status.has_value());
  assert(result.error_status.value() == WqeStatus::RnrRetryExceededError);
  assert(manager.stats().retry_exceeded == 1);

  std::printf("    PASSED\n");
}

// Test NAK for remote access error
void test_nak_remote_access_error() {
  std::printf("  test_nak_remote_access_error...\n");

  ReliabilityManager manager;

  manager.add_pending(1, 0, 0, 1001, WqeOpcode::RdmaWrite, 0);

  AckResult result = manager.process_nak(1, 0, AethSyndrome::RemoteAccessError);
  assert(result.success);
  assert(result.error_status.has_value());
  assert(result.error_status.value() == WqeStatus::RemoteAccessError);

  std::printf("    PASSED\n");
}

// Test timeout detection
void test_timeout_detection() {
  std::printf("  test_timeout_detection...\n");

  ReliabilityConfig config;
  config.ack_timeout_us = 1000;  // 1ms timeout
  config.max_retries = 2;
  ReliabilityManager manager{config};

  // Add pending operation at time 0
  manager.add_pending(1, 0, 0, 1001, WqeOpcode::Send, 0);

  // Check at 500us - no timeout (timeout is 1000us)
  auto retransmits = manager.check_timeouts(1, 500);
  assert(retransmits.empty());

  // Check at 1500us - should timeout (first timeout after 1000us)
  retransmits = manager.check_timeouts(1, 1500);
  assert(retransmits.size() == 1);
  assert(retransmits[0] == 0);
  assert(manager.stats().timeouts == 1);
  assert(manager.stats().retransmissions == 1);

  // Check again at 4000us - second timeout (timeout is now 2000us, send_time is 1500)
  // Need at least 1500 + 2000 = 3500us for second timeout
  retransmits = manager.check_timeouts(1, 4000);
  assert(retransmits.size() == 1);
  assert(manager.stats().timeouts == 2);

  // Check at 12000us - third timeout (timeout is now 4000us, send_time is 4000)
  // Need at least 4000 + 4000 = 8000us for third timeout
  // After 3 timeouts, max_retries (2) will be exceeded
  retransmits = manager.check_timeouts(1, 12000);
  assert(retransmits.empty());  // No more retransmits after exhausted
  assert(manager.stats().retry_exceeded == 1);

  std::printf("    PASSED\n");
}

// Test multiple pending operations
void test_multiple_pending() {
  std::printf("  test_multiple_pending...\n");

  ReliabilityManager manager;

  // Add operations for multiple QPs
  manager.add_pending(1, 0, 0, 1001, WqeOpcode::Send, 0);
  manager.add_pending(1, 1, 1, 1002, WqeOpcode::Send, 10);
  manager.add_pending(2, 0, 0, 2001, WqeOpcode::RdmaRead, 20);

  // ACK on QP 1
  AckResult result = manager.process_ack(1, 1);
  assert(result.completed_wr_ids.size() == 2);

  // ACK on QP 2
  result = manager.process_ack(2, 0);
  assert(result.completed_wr_ids.size() == 1);
  assert(result.completed_wr_ids[0] == 2001);

  std::printf("    PASSED\n");
}

// Test reliability manager reset
void test_reliability_reset() {
  std::printf("  test_reliability_reset...\n");

  ReliabilityManager manager;

  manager.add_pending(1, 0, 0, 1001, WqeOpcode::Send, 0);
  (void) manager.process_ack(1, 0);

  assert(manager.stats().acks_received == 1);

  manager.reset();

  assert(manager.stats().acks_received == 0);

  // Should work normally after reset
  manager.add_pending(1, 0, 0, 2001, WqeOpcode::Send, 0);
  AckResult result = manager.process_ack(1, 0);
  assert(result.completed_wr_ids.size() == 1);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running reliability tests...\n");

  test_ack_processing();
  test_nak_psn_sequence_error();
  test_nak_rnr();
  test_nak_remote_access_error();
  test_timeout_detection();
  test_multiple_pending();
  test_reliability_reset();

  std::printf("All reliability tests PASSED!\n");
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

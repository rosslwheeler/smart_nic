#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <tracy/Tracy.hpp>

#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/types.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

/// Helper to transition a QP from Reset through Init/Rtr to Rts.
static void transition_to_rts(RdmaQueuePair& qp, uint32_t sq_psn = 0x1000) {
  NIC_TRACE_SCOPED(__func__);
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  qp.modify(params);
  params.target_state = QpState::Rts;
  params.sq_psn = sq_psn;
  qp.modify(params);
}

// =============================================================================
// handle_ack() Tests
// =============================================================================

static void test_handle_ack_normal() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_handle_ack_normal... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp, 0x1000);

  // Add a pending operation (sq_psn_ is 0x1000 from transition_to_rts)
  // add_pending_operation captures sq_psn_ as the PSN for the op
  SendWqe wqe{.wr_id = 0xAA, .opcode = WqeOpcode::Send, .total_length = 64};
  qp.add_pending_operation(wqe, 1);

  assert(qp.pending_count() == 1);
  assert(qp.stats().send_completions == 0);

  // ACK with PSN 0x1000 (the op's PSN)
  // psn_in_window(op_last_psn=0x1000, last_acked_psn_=0, 0x1000-0+1=0x1001) => true
  qp.handle_ack(0x1000, AethSyndrome::Ack);

  assert(qp.pending_count() == 0);
  assert(qp.stats().send_completions == 1);

  std::cout << "PASSED\n";
}

static void test_handle_ack_rnr_nak() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_handle_ack_rnr_nak... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp);

  assert(qp.stats().rnr_naks_received == 0);

  qp.handle_ack(0x1000, AethSyndrome::RnrNak);

  assert(qp.stats().rnr_naks_received == 1);
  // State should remain RTS (RnrNak is not fatal)
  assert(qp.state() == QpState::Rts);

  std::cout << "PASSED\n";
}

static void test_handle_ack_psn_seq_error() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_handle_ack_psn_seq_error... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp);

  assert(qp.stats().sequence_errors == 0);
  assert(qp.stats().retransmits == 0);

  qp.handle_ack(0x1000, AethSyndrome::PsnSeqError);

  assert(qp.stats().sequence_errors == 1);
  assert(qp.stats().retransmits == 1);
  // State should remain RTS (PsnSeqError triggers retransmit, not error)
  assert(qp.state() == QpState::Rts);

  std::cout << "PASSED\n";
}

static void test_handle_ack_fatal_error() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_handle_ack_fatal_error... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp);

  assert(qp.stats().remote_errors == 0);

  // InvalidRequest is a fatal NAK - should move to Error state
  qp.handle_ack(0x1000, AethSyndrome::InvalidRequest);

  assert(qp.stats().remote_errors == 1);
  assert(qp.state() == QpState::Error);

  // Also test RemoteAccessError
  RdmaQueuePair qp2(2, config);
  transition_to_rts(qp2);
  qp2.handle_ack(0x1000, AethSyndrome::RemoteAccessError);
  assert(qp2.stats().remote_errors == 1);
  assert(qp2.state() == QpState::Error);

  // Also test RemoteOpError
  RdmaQueuePair qp3(3, config);
  transition_to_rts(qp3);
  qp3.handle_ack(0x1000, AethSyndrome::RemoteOpError);
  assert(qp3.stats().remote_errors == 1);
  assert(qp3.state() == QpState::Error);

  std::cout << "PASSED\n";
}

// =============================================================================
// add_pending_operation() Tests
// =============================================================================

static void test_add_pending_operation() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_add_pending_operation... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp, 0x2000);

  assert(qp.pending_count() == 0);

  SendWqe wqe1{.wr_id = 1, .opcode = WqeOpcode::Send, .total_length = 100};
  qp.add_pending_operation(wqe1, 1);
  assert(qp.pending_count() == 1);

  SendWqe wqe2{.wr_id = 2, .opcode = WqeOpcode::RdmaWrite, .total_length = 8192};
  qp.add_pending_operation(wqe2, 4);
  assert(qp.pending_count() == 2);

  SendWqe wqe3{.wr_id = 3, .opcode = WqeOpcode::Send, .total_length = 64};
  qp.add_pending_operation(wqe3, 1);
  assert(qp.pending_count() == 3);

  std::cout << "PASSED\n";
}

// =============================================================================
// check_timeouts() Tests
// =============================================================================

static void test_check_timeouts_no_timeout() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_check_timeouts_no_timeout... " << std::flush;

  // timeout exponent 14 => timeout_us = 4 << 14 = 65536 us
  RdmaQpConfig config;
  config.timeout = 14;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp, 0x1000);

  SendWqe wqe{.wr_id = 1, .opcode = WqeOpcode::Send, .total_length = 64};
  qp.add_pending_operation(wqe, 1);

  // Check at a time less than the timeout - should return empty
  auto retransmits = qp.check_timeouts(60000);  // 60000 < 65536
  assert(retransmits.empty());
  assert(qp.stats().retransmits == 0);
  assert(qp.state() == QpState::Rts);

  std::cout << "PASSED\n";
}

static void test_check_timeouts_retransmit() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_check_timeouts_retransmit... " << std::flush;

  // Use a small timeout exponent for easier testing
  // timeout exponent 0 => timeout_us = 4 << 0 = 4 us
  RdmaQpConfig config;
  config.timeout = 0;
  config.retry_count = 7;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp, 0x1000);

  SendWqe wqe{.wr_id = 42, .opcode = WqeOpcode::Send, .total_length = 64};
  // current_time_us_ is 0 at start, so pending op timestamp is 0
  qp.add_pending_operation(wqe, 1);

  // Advance past timeout: 4us timeout, check at 10us
  auto retransmits = qp.check_timeouts(10);
  assert(retransmits.size() == 1);
  assert(retransmits[0].wr_id == 42);
  assert(qp.stats().retransmits == 1);
  assert(qp.state() == QpState::Rts);  // Still healthy, retries remain

  std::cout << "PASSED\n";
}

static void test_check_timeouts_retry_exhausted() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_check_timeouts_retry_exhausted... " << std::flush;

  // timeout exponent 0 => timeout_us = 4 us
  RdmaQpConfig config;
  config.timeout = 0;
  config.retry_count = 1;  // Only 1 retry allowed
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp, 0x1000);

  SendWqe wqe{.wr_id = 99, .opcode = WqeOpcode::Send, .total_length = 64};
  qp.add_pending_operation(wqe, 1);

  // First timeout at time 10 - should retransmit (retry_count goes from 1 to 0)
  auto retransmits = qp.check_timeouts(10);
  assert(retransmits.size() == 1);
  assert(qp.stats().retransmits == 1);
  assert(qp.state() == QpState::Rts);

  // Second timeout at time 20 - retry exhausted, should go to Error
  // The op timestamp was updated to 10 by the first timeout, so 20 - 10 = 10 >= 4
  retransmits = qp.check_timeouts(20);
  assert(retransmits.empty());
  assert(qp.state() == QpState::Error);
  assert(qp.stats().local_errors == 1);

  std::cout << "PASSED\n";
}

// =============================================================================
// advance_time() Tests
// =============================================================================

static void test_advance_time() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_advance_time... " << std::flush;

  RdmaQpConfig config;
  config.timeout = 0;  // timeout_us = 4
  config.retry_count = 7;
  RdmaQueuePair qp(1, config);
  transition_to_rts(qp, 0x1000);

  // Advance time accumulates
  qp.advance_time(100);
  qp.advance_time(200);
  qp.advance_time(50);
  // Total should be 350us

  // Verify by adding a pending op (timestamp will be current_time_us_ = 350)
  SendWqe wqe{.wr_id = 1, .opcode = WqeOpcode::Send, .total_length = 64};
  qp.add_pending_operation(wqe, 1);

  // At time 353 (< 350 + 4 = 354), should not timeout
  auto retransmits = qp.check_timeouts(353);
  assert(retransmits.empty());

  // At time 354 (== 350 + 4), should timeout
  retransmits = qp.check_timeouts(354);
  assert(retransmits.size() == 1);

  std::cout << "PASSED\n";
}

// =============================================================================
// State Transition Tests
// =============================================================================

static void test_state_transitions_comprehensive() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_state_transitions_comprehensive... " << std::flush;

  RdmaQpModifyParams params;

  // --- Rts -> Sqd ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(1, config);
    transition_to_rts(qp);
    params.target_state = QpState::Sqd;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Sqd);
  }

  // --- Sqd -> Rts ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(2, config);
    transition_to_rts(qp);
    params.target_state = QpState::Sqd;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Sqd);
    params.target_state = QpState::Rts;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Rts);
  }

  // --- Sqd -> Reset ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(3, config);
    transition_to_rts(qp);
    params.target_state = QpState::Sqd;
    assert(qp.modify(params));
    params.target_state = QpState::Reset;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Reset);
  }

  // --- Sqd -> Error ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(4, config);
    transition_to_rts(qp);
    params.target_state = QpState::Sqd;
    assert(qp.modify(params));
    params.target_state = QpState::Error;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Error);
  }

  // --- Rts -> SqErr ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(5, config);
    transition_to_rts(qp);
    params.target_state = QpState::SqErr;
    assert(qp.modify(params));
    assert(qp.state() == QpState::SqErr);
  }

  // --- SqErr -> Reset ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(6, config);
    transition_to_rts(qp);
    params.target_state = QpState::SqErr;
    assert(qp.modify(params));
    params.target_state = QpState::Reset;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Reset);
  }

  // --- SqErr -> Error ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(7, config);
    transition_to_rts(qp);
    params.target_state = QpState::SqErr;
    assert(qp.modify(params));
    params.target_state = QpState::Error;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Error);
  }

  // --- Init -> Error ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(8, config);
    params.target_state = QpState::Init;
    assert(qp.modify(params));
    params.target_state = QpState::Error;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Error);
  }

  // --- Init -> Reset ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(9, config);
    params.target_state = QpState::Init;
    assert(qp.modify(params));
    params.target_state = QpState::Reset;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Reset);
  }

  // --- Rtr -> Error ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(10, config);
    params.target_state = QpState::Init;
    assert(qp.modify(params));
    params.target_state = QpState::Rtr;
    assert(qp.modify(params));
    params.target_state = QpState::Error;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Error);
  }

  // --- Rtr -> Reset ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(11, config);
    params.target_state = QpState::Init;
    assert(qp.modify(params));
    params.target_state = QpState::Rtr;
    assert(qp.modify(params));
    params.target_state = QpState::Reset;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Reset);
  }

  // --- Error -> Reset ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(12, config);
    transition_to_rts(qp);
    params.target_state = QpState::Error;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Error);
    params.target_state = QpState::Reset;
    assert(qp.modify(params));
    assert(qp.state() == QpState::Reset);
  }

  // --- Error -> anything other than Reset should fail ---
  {
    RdmaQpConfig config;
    RdmaQueuePair qp(13, config);
    transition_to_rts(qp);
    params.target_state = QpState::Error;
    assert(qp.modify(params));
    params.target_state = QpState::Init;
    assert(!qp.modify(params));
    assert(qp.state() == QpState::Error);
  }

  std::cout << "PASSED\n";
}

// =============================================================================
// Queue Depth Limit Tests
// =============================================================================

static void test_queue_depth_limit() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_queue_depth_limit... " << std::flush;

  RdmaQpConfig config;
  config.send_queue_depth = 4;
  config.recv_queue_depth = 3;
  RdmaQueuePair qp(1, config);

  // Move to Init so we can post WQEs
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);

  // Fill send queue to capacity
  for (std::size_t wqe_idx = 0; wqe_idx < 4; ++wqe_idx) {
    SendWqe wqe{.wr_id = wqe_idx};
    assert(qp.post_send(wqe));
  }
  // Next post should fail - queue full
  SendWqe overflow_send{.wr_id = 999};
  assert(!qp.post_send(overflow_send));
  assert(qp.send_queue_size() == 4);

  // Fill recv queue to capacity
  for (std::size_t wqe_idx = 0; wqe_idx < 3; ++wqe_idx) {
    RecvWqe wqe{.wr_id = wqe_idx, .total_length = 1024};
    assert(qp.post_recv(wqe));
  }
  // Next post should fail - queue full
  RecvWqe overflow_recv{.wr_id = 999, .total_length = 1024};
  assert(!qp.post_recv(overflow_recv));
  assert(qp.recv_queue_size() == 3);

  // Verify local_errors incremented for the failed posts
  // 2 failed posts from Reset state (in the original queue_pair_test) aren't counted here,
  // but we have 2 overflow failures: 1 send + 1 recv
  assert(qp.stats().local_errors == 2);

  std::cout << "PASSED\n";
}

// =============================================================================
// MTU All Values Test
// =============================================================================

static void test_mtu_all_values() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_mtu_all_values... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);

  // MTU 1 = 256 bytes
  params = {};
  params.path_mtu = 1;
  qp.modify(params);
  assert(qp.mtu_bytes() == 256);

  // MTU 2 = 512 bytes
  params = {};
  params.path_mtu = 2;
  qp.modify(params);
  assert(qp.mtu_bytes() == 512);

  // MTU 3 = 1024 bytes (default)
  params = {};
  params.path_mtu = 3;
  qp.modify(params);
  assert(qp.mtu_bytes() == 1024);

  // MTU 4 = 2048 bytes
  params = {};
  params.path_mtu = 4;
  qp.modify(params);
  assert(qp.mtu_bytes() == 2048);

  // MTU 5 = 4096 bytes
  params = {};
  params.path_mtu = 5;
  qp.modify(params);
  assert(qp.mtu_bytes() == 4096);

  // MTU 0 = invalid, defaults to 1024
  params = {};
  params.path_mtu = 0;
  qp.modify(params);
  assert(qp.mtu_bytes() == 1024);

  // MTU 6 = invalid, defaults to 1024
  params = {};
  params.path_mtu = 6;
  qp.modify(params);
  assert(qp.mtu_bytes() == 1024);

  // MTU 255 = invalid, defaults to 1024
  params = {};
  params.path_mtu = 255;
  qp.modify(params);
  assert(qp.mtu_bytes() == 1024);

  std::cout << "PASSED\n";
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  std::cout << "\n=== RoCEv2 Queue Pair Coverage Tests ===\n\n";

  // handle_ack() tests
  test_handle_ack_normal();
  test_handle_ack_rnr_nak();
  test_handle_ack_psn_seq_error();
  test_handle_ack_fatal_error();

  // add_pending_operation() tests
  test_add_pending_operation();

  // check_timeouts() tests
  test_check_timeouts_no_timeout();
  test_check_timeouts_retransmit();
  test_check_timeouts_retry_exhausted();

  // advance_time() tests
  test_advance_time();

  // State transition tests
  test_state_transitions_comprehensive();

  // Queue depth limit tests
  test_queue_depth_limit();

  // MTU tests
  test_mtu_all_values();

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

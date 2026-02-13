#include "nic/rocev2/queue_pair.h"

#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <tracy/Tracy.hpp>

#include "nic/rocev2/completion_queue.h"
#include "nic/rocev2/types.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

// =============================================================================
// Completion Queue Tests
// =============================================================================

static void test_cq_post_and_poll() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_cq_post_and_poll... " << std::flush;

  RdmaCompletionQueue cq(1);

  RdmaCqe cqe{
      .wr_id = 0x1234,
      .status = WqeStatus::Success,
      .opcode = WqeOpcode::Send,
      .qp_number = 1,
      .bytes_completed = 100,
  };

  assert(cq.post(cqe));
  assert(cq.count() == 1);
  assert(!cq.is_empty());

  auto polled = cq.poll_one();
  assert(polled.has_value());
  assert(polled->wr_id == 0x1234);
  assert(polled->status == WqeStatus::Success);
  assert(cq.is_empty());

  std::cout << "PASSED\n";
}

static void test_cq_poll_multiple() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_cq_poll_multiple... " << std::flush;

  RdmaCompletionQueue cq(1);

  for (std::uint64_t wr_idx = 0; wr_idx < 5; ++wr_idx) {
    RdmaCqe cqe{.wr_id = wr_idx, .status = WqeStatus::Success};
    assert(cq.post(cqe));
  }
  assert(cq.count() == 5);

  auto polled = cq.poll(3);
  assert(polled.size() == 3);
  assert(polled[0].wr_id == 0);
  assert(polled[1].wr_id == 1);
  assert(polled[2].wr_id == 2);
  assert(cq.count() == 2);

  std::cout << "PASSED\n";
}

static void test_cq_overflow() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_cq_overflow... " << std::flush;

  RdmaCqConfig config{.depth = 2};
  RdmaCompletionQueue cq(1, config);

  RdmaCqe cqe{.wr_id = 1};
  assert(cq.post(cqe));
  assert(cq.post(cqe));
  assert(!cq.post(cqe));  // Should fail - full
  assert(cq.is_full());
  assert(cq.stats().overflows == 1);

  std::cout << "PASSED\n";
}

static void test_cq_arm_and_notify() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_cq_arm_and_notify... " << std::flush;

  RdmaCompletionQueue cq(1);

  assert(!cq.is_armed());
  assert(!cq.should_notify());

  cq.arm();
  assert(cq.is_armed());
  assert(!cq.should_notify());  // No completions yet

  RdmaCqe cqe{.wr_id = 1};
  cq.post(cqe);
  assert(cq.should_notify());  // Now should notify

  cq.clear_arm();
  assert(!cq.is_armed());
  assert(!cq.should_notify());

  std::cout << "PASSED\n";
}

static void test_cq_reset() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_cq_reset... " << std::flush;

  RdmaCompletionQueue cq(1);

  RdmaCqe cqe{.wr_id = 1};
  cq.post(cqe);
  cq.arm();
  assert(cq.count() == 1);

  cq.reset();

  assert(cq.is_empty());
  assert(!cq.is_armed());
  assert(cq.stats().cqes_posted == 0);

  std::cout << "PASSED\n";
}

// =============================================================================
// Queue Pair State Machine Tests
// =============================================================================

static void test_qp_state_transitions() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_state_transitions... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  assert(qp.state() == QpState::Reset);

  // Reset -> Init
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  assert(qp.modify(params));
  assert(qp.state() == QpState::Init);

  // Init -> RTR
  params.target_state = QpState::Rtr;
  params.dest_qp_number = 2;
  params.rq_psn = 0x1000;
  assert(qp.modify(params));
  assert(qp.state() == QpState::Rtr);
  assert(qp.dest_qp_number() == 2);
  assert(qp.rq_psn() == 0x1000);

  // RTR -> RTS
  params.target_state = QpState::Rts;
  params.sq_psn = 0x2000;
  assert(qp.modify(params));
  assert(qp.state() == QpState::Rts);
  assert(qp.sq_psn() == 0x2000);

  std::cout << "PASSED\n";
}

static void test_qp_invalid_transition() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_invalid_transition... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Reset -> RTS (invalid, must go through Init/RTR)
  RdmaQpModifyParams params;
  params.target_state = QpState::Rts;
  assert(!qp.modify(params));
  assert(qp.state() == QpState::Reset);

  std::cout << "PASSED\n";
}

static void test_qp_reset_from_any_state() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_reset_from_any_state... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Get to RTS
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  qp.modify(params);
  params.target_state = QpState::Rts;
  qp.modify(params);
  assert(qp.state() == QpState::Rts);

  // Reset from RTS
  params.target_state = QpState::Reset;
  assert(qp.modify(params));
  assert(qp.state() == QpState::Reset);

  std::cout << "PASSED\n";
}

// =============================================================================
// Queue Pair WQE Tests
// =============================================================================

static void test_qp_post_send() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_post_send... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Transition to Init
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);

  SendWqe wqe{.wr_id = 0x5678, .opcode = WqeOpcode::Send};
  assert(qp.post_send(wqe));
  assert(qp.send_queue_size() == 1);
  assert(qp.stats().send_wqes_posted == 1);

  std::cout << "PASSED\n";
}

static void test_qp_post_recv() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_post_recv... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Transition to Init
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);

  RecvWqe wqe{.wr_id = 0x1234, .total_length = 1024};
  assert(qp.post_recv(wqe));
  assert(qp.recv_queue_size() == 1);
  assert(qp.stats().recv_wqes_posted == 1);

  std::cout << "PASSED\n";
}

static void test_qp_post_in_reset_fails() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_post_in_reset_fails... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // In Reset state
  SendWqe send_wqe{.wr_id = 1};
  assert(!qp.post_send(send_wqe));

  RecvWqe recv_wqe{.wr_id = 2};
  assert(!qp.post_recv(recv_wqe));

  std::cout << "PASSED\n";
}

static void test_qp_get_next_send() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_get_next_send... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Get to RTS
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  qp.modify(params);
  params.target_state = QpState::Rts;
  qp.modify(params);

  SendWqe wqe{.wr_id = 0x9999};
  qp.post_send(wqe);

  auto retrieved = qp.get_next_send();
  assert(retrieved.has_value());
  assert(retrieved->wr_id == 0x9999);
  assert(qp.send_queue_size() == 0);

  std::cout << "PASSED\n";
}

static void test_qp_consume_recv() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_consume_recv... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Get to RTR (can receive)
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  qp.modify(params);

  RecvWqe wqe{.wr_id = 0xAAAA, .total_length = 512};
  qp.post_recv(wqe);

  auto consumed = qp.consume_recv();
  assert(consumed.has_value());
  assert(consumed->wr_id == 0xAAAA);
  assert(qp.recv_queue_size() == 0);

  std::cout << "PASSED\n";
}

// =============================================================================
// PSN Tracking Tests
// =============================================================================

static void test_qp_psn_tracking() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_psn_tracking... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Set initial PSN
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  params.rq_psn = 0x1000;
  qp.modify(params);
  params.target_state = QpState::Rts;
  params.sq_psn = 0x2000;
  qp.modify(params);

  assert(qp.sq_psn() == 0x2000);
  assert(qp.rq_psn() == 0x1000);

  // Advance send PSN
  std::uint32_t psn1 = qp.next_send_psn();
  assert(psn1 == 0x2000);
  assert(qp.sq_psn() == 0x2001);

  std::uint32_t psn2 = qp.next_send_psn();
  assert(psn2 == 0x2001);
  assert(qp.sq_psn() == 0x2002);

  // Advance receive PSN
  assert(qp.expected_recv_psn() == 0x1000);
  qp.advance_recv_psn();
  assert(qp.expected_recv_psn() == 0x1001);

  std::cout << "PASSED\n";
}

static void test_qp_psn_wraparound() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_psn_wraparound... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  qp.modify(params);
  params.target_state = QpState::Rts;
  params.sq_psn = kMaxPsn;  // 0xFFFFFF
  qp.modify(params);

  std::uint32_t psn = qp.next_send_psn();
  assert(psn == kMaxPsn);
  assert(qp.sq_psn() == 0);  // Wrapped around

  std::cout << "PASSED\n";
}

// =============================================================================
// MTU Tests
// =============================================================================

static void test_qp_mtu_bytes() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_mtu_bytes... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Default MTU is 3 = 1024 bytes
  assert(qp.mtu_bytes() == 1024);

  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);
  params.target_state = QpState::Rtr;
  params.path_mtu = 5;  // 4096 bytes
  qp.modify(params);

  assert(qp.mtu_bytes() == 4096);

  std::cout << "PASSED\n";
}

// =============================================================================
// Statistics Tests
// =============================================================================

static void test_qp_packet_stats() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_packet_stats... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  qp.record_packet_sent(1000);
  qp.record_packet_sent(500);
  qp.record_packet_received(2000);

  assert(qp.stats().packets_sent == 2);
  assert(qp.stats().bytes_sent == 1500);
  assert(qp.stats().packets_received == 1);
  assert(qp.stats().bytes_received == 2000);

  std::cout << "PASSED\n";
}

// =============================================================================
// Reset Tests
// =============================================================================

static void test_qp_reset() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_qp_reset... " << std::flush;

  RdmaQpConfig config;
  RdmaQueuePair qp(1, config);

  // Get to RTS and post some WQEs
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  qp.modify(params);

  SendWqe send_wqe{.wr_id = 1};
  qp.post_send(send_wqe);
  RecvWqe recv_wqe{.wr_id = 2};
  qp.post_recv(recv_wqe);

  qp.reset();

  assert(qp.state() == QpState::Reset);
  assert(qp.send_queue_size() == 0);
  assert(qp.recv_queue_size() == 0);
  assert(qp.sq_psn() == 0);
  assert(qp.stats().send_wqes_posted == 0);

  std::cout << "PASSED\n";
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  std::cout << "\n=== RoCEv2 Queue Pair Tests ===\n\n";

  // Completion Queue tests
  test_cq_post_and_poll();
  test_cq_poll_multiple();
  test_cq_overflow();
  test_cq_arm_and_notify();
  test_cq_reset();

  // QP State Machine tests
  test_qp_state_transitions();
  test_qp_invalid_transition();
  test_qp_reset_from_any_state();

  // QP WQE tests
  test_qp_post_send();
  test_qp_post_recv();
  test_qp_post_in_reset_fails();
  test_qp_get_next_send();
  test_qp_consume_recv();

  // PSN tests
  test_qp_psn_tracking();
  test_qp_psn_wraparound();

  // MTU tests
  test_qp_mtu_bytes();

  // Stats tests
  test_qp_packet_stats();

  // Reset tests
  test_qp_reset();

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

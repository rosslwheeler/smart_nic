#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "nic/dma_engine.h"
#include "nic/rocev2/engine.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;
using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

/// Helper to set up an RdmaEngine with configurable options.
struct EngineSetup {
  std::unique_ptr<SimpleHostMemory> host_memory;
  std::unique_ptr<DMAEngine> dma_engine;
  std::unique_ptr<RdmaEngine> engine;

  explicit EngineSetup(RdmaEngineConfig config = {}) {
    NIC_TRACE_SCOPED(__func__);
    HostMemoryConfig mem_cfg{.size_bytes = 64 * 1024};
    host_memory = std::make_unique<SimpleHostMemory>(mem_cfg);
    dma_engine = std::make_unique<DMAEngine>(*host_memory);
    engine = std::make_unique<RdmaEngine>(config, *dma_engine, *host_memory);
  }

  /// Create a PD and return its handle.
  [[nodiscard]] std::uint32_t create_pd() {
    NIC_TRACE_SCOPED(__func__);
    auto pd = engine->create_pd();
    assert(pd.has_value());
    return *pd;
  }

  /// Create a CQ and return its number.
  [[nodiscard]] std::uint32_t create_cq(std::size_t depth = 256) {
    NIC_TRACE_SCOPED(__func__);
    auto cq = engine->create_cq(depth);
    assert(cq.has_value());
    return *cq;
  }

  /// Create a fully configured QP in Reset state.
  [[nodiscard]] std::uint32_t create_qp(std::uint32_t pd_handle,
                                        std::uint32_t send_cq_number,
                                        std::uint32_t recv_cq_number) {
    NIC_TRACE_SCOPED(__func__);
    RdmaQpConfig qp_config;
    qp_config.pd_handle = pd_handle;
    qp_config.send_cq_number = send_cq_number;
    qp_config.recv_cq_number = recv_cq_number;
    auto qp = engine->create_qp(qp_config);
    assert(qp.has_value());
    return *qp;
  }

  /// Transition a QP through Reset -> Init -> RTR -> RTS.
  void transition_qp_to_rts([[maybe_unused]] std::uint32_t qp_number,
                            std::uint32_t dest_qp_number) {
    NIC_TRACE_SCOPED(__func__);
    RdmaQpModifyParams params;

    params.target_state = QpState::Init;
    assert(engine->modify_qp(qp_number, params));

    params.target_state = QpState::Rtr;
    params.dest_qp_number = dest_qp_number;
    params.rq_psn = 0;
    params.dest_ip = std::array<std::uint8_t, 4>{192, 168, 1, 2};
    assert(engine->modify_qp(qp_number, params));

    params = RdmaQpModifyParams{};
    params.target_state = QpState::Rts;
    params.sq_psn = 0;
    assert(engine->modify_qp(qp_number, params));
  }
};

// ============================================
// Test: create_qp with invalid PD
// ============================================
void test_create_qp_invalid_pd() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_create_qp_invalid_pd...\n");

  EngineSetup setup;
  auto send_cq = setup.create_cq();
  auto recv_cq = setup.create_cq();

  // Use a PD handle that was never created.
  RdmaQpConfig qp_config;
  qp_config.pd_handle = 999;
  qp_config.send_cq_number = send_cq;
  qp_config.recv_cq_number = recv_cq;

  [[maybe_unused]] auto result = setup.engine->create_qp(qp_config);
  assert(!result.has_value());
  assert(setup.engine->stats().errors > 0);

  std::printf("    PASSED\n");
}

// ============================================
// Test: create_qp with invalid CQs
// ============================================
void test_create_qp_invalid_cq() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_create_qp_invalid_cq...\n");

  EngineSetup setup;
  auto pd_handle = setup.create_pd();

  // Both CQ numbers are invalid (never created).
  RdmaQpConfig qp_config;
  qp_config.pd_handle = pd_handle;
  qp_config.send_cq_number = 8888;
  qp_config.recv_cq_number = 9999;

  auto result = setup.engine->create_qp(qp_config);
  assert(!result.has_value());
  assert(setup.engine->stats().errors > 0);

  // Also test with only recv CQ invalid.
  auto send_cq = setup.create_cq();
  qp_config.send_cq_number = send_cq;
  qp_config.recv_cq_number = 9999;

  result = setup.engine->create_qp(qp_config);
  assert(!result.has_value());

  std::printf("    PASSED\n");
}

// ============================================
// Test: create_qp with max_qps exceeded
// ============================================
void test_create_qp_max_exceeded() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_create_qp_max_exceeded...\n");

  RdmaEngineConfig config;
  config.max_qps = 1;
  EngineSetup setup(config);

  auto pd_handle = setup.create_pd();
  auto send_cq = setup.create_cq();
  auto recv_cq = setup.create_cq();

  // First QP should succeed.
  auto qp1 = setup.create_qp(pd_handle, send_cq, recv_cq);
  (void) qp1;

  // Second QP should fail because max_qps=1.
  RdmaQpConfig qp_config;
  qp_config.pd_handle = pd_handle;
  qp_config.send_cq_number = send_cq;
  qp_config.recv_cq_number = recv_cq;
  [[maybe_unused]] auto result = setup.engine->create_qp(qp_config);
  assert(!result.has_value());
  assert(setup.engine->stats().errors > 0);

  std::printf("    PASSED\n");
}

// ============================================
// Test: register_mr with invalid PD
// ============================================
void test_register_mr_invalid_pd() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_register_mr_invalid_pd...\n");

  EngineSetup setup;

  AccessFlags access{.local_read = true, .local_write = true};
  [[maybe_unused]] auto result = setup.engine->register_mr(999, 0x1000, 4096, access);
  assert(!result.has_value());
  assert(setup.engine->stats().errors > 0);

  std::printf("    PASSED\n");
}

// ============================================
// Test: destroy_cq when CQ is in use by a QP
// ============================================
void test_destroy_cq_in_use() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_destroy_cq_in_use...\n");

  EngineSetup setup;
  auto pd_handle = setup.create_pd();
  auto send_cq = setup.create_cq();
  auto recv_cq = setup.create_cq();
  auto qp_number = setup.create_qp(pd_handle, send_cq, recv_cq);
  (void) qp_number;

  // Destroying either CQ should fail because the QP references them.
  assert(!setup.engine->destroy_cq(send_cq));
  assert(!setup.engine->destroy_cq(recv_cq));

  // After destroying the QP, CQ destruction should succeed.
  assert(setup.engine->destroy_qp(qp_number));
  assert(setup.engine->destroy_cq(send_cq));
  assert(setup.engine->destroy_cq(recv_cq));

  std::printf("    PASSED\n");
}

// ============================================
// Test: post_send on non-existent QP
// ============================================
void test_post_send_invalid_qp() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_post_send_invalid_qp...\n");

  EngineSetup setup;

  SendWqe wqe;
  wqe.wr_id = 1;
  wqe.opcode = WqeOpcode::Send;
  wqe.total_length = 64;

  assert(!setup.engine->post_send(9999, wqe));

  std::printf("    PASSED\n");
}

// ============================================
// Test: post_send when QP is not in RTS state
// ============================================
void test_post_send_not_rts() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_post_send_not_rts...\n");

  EngineSetup setup;
  auto pd_handle = setup.create_pd();
  auto send_cq = setup.create_cq();
  auto recv_cq = setup.create_cq();
  [[maybe_unused]] auto qp_number = setup.create_qp(pd_handle, send_cq, recv_cq);

  // QP is in Reset state -- transition to Init only (not RTS).
  RdmaQpModifyParams params;
  params.target_state = QpState::Init;
  assert(setup.engine->modify_qp(qp_number, params));

  // post_send should fail because QP is in Init, not RTS.
  SendWqe wqe;
  wqe.wr_id = 1;
  wqe.opcode = WqeOpcode::Send;
  wqe.total_length = 64;

  assert(!setup.engine->post_send(qp_number, wqe));
  assert(setup.engine->stats().errors > 0);

  std::printf("    PASSED\n");
}

// ============================================
// Test: post_send with invalid opcode
// ============================================
void test_post_send_invalid_opcode() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_post_send_invalid_opcode...\n");

  EngineSetup setup;
  auto pd_handle = setup.create_pd();
  auto send_cq = setup.create_cq();
  auto recv_cq = setup.create_cq();
  auto qp_number = setup.create_qp(pd_handle, send_cq, recv_cq);

  // Transition QP to RTS so post_send passes the can_send() check.
  setup.transition_qp_to_rts(qp_number, qp_number);

  // Use an invalid opcode by casting an out-of-range value.
  SendWqe wqe;
  wqe.wr_id = 1;
  wqe.opcode = static_cast<WqeOpcode>(255);
  wqe.total_length = 64;
  wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});

  assert(!setup.engine->post_send(qp_number, wqe));
  assert(setup.engine->stats().errors > 0);

  std::printf("    PASSED\n");
}

// ============================================
// Test: post_recv on non-existent QP
// ============================================
void test_post_recv_invalid_qp() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_post_recv_invalid_qp...\n");

  EngineSetup setup;

  RecvWqe wqe;
  wqe.wr_id = 1;
  wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 256});

  assert(!setup.engine->post_recv(9999, wqe));

  std::printf("    PASSED\n");
}

// ============================================
// Test: poll_cq on non-existent CQ
// ============================================
void test_poll_cq_invalid() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_poll_cq_invalid...\n");

  EngineSetup setup;

  auto cqes = setup.engine->poll_cq(9999, 10);
  assert(cqes.empty());

  std::printf("    PASSED\n");
}

// ============================================
// Test: modify_qp on non-existent QP
// ============================================
void test_modify_qp_invalid() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_modify_qp_invalid...\n");

  EngineSetup setup;

  RdmaQpModifyParams params;
  params.target_state = QpState::Init;

  assert(!setup.engine->modify_qp(9999, params));

  std::printf("    PASSED\n");
}

// ============================================
// Test: query_qp on non-existent QP
// ============================================
void test_query_qp_invalid() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_query_qp_invalid...\n");

  EngineSetup setup;

  [[maybe_unused]] auto* qp = setup.engine->query_qp(9999);
  assert(qp == nullptr);

  std::printf("    PASSED\n");
}

// ============================================
// Test: destroy_qp on non-existent QP
// ============================================
void test_destroy_qp_invalid() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_destroy_qp_invalid...\n");

  EngineSetup setup;

  assert(!setup.engine->destroy_qp(9999));

  std::printf("    PASSED\n");
}

// ============================================
// Test: advance_time with active QPs
// ============================================
void test_advance_time() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_advance_time...\n");

  EngineSetup setup;
  auto pd_handle = setup.create_pd();
  auto send_cq = setup.create_cq();
  auto recv_cq = setup.create_cq();
  auto qp_number = setup.create_qp(pd_handle, send_cq, recv_cq);

  // Transition QP to RTS so it has an active state.
  setup.transition_qp_to_rts(qp_number, qp_number);

  // Advance time -- should not crash and should iterate over QPs.
  setup.engine->advance_time(1000);
  setup.engine->advance_time(5000);

  // Create a second QP and advance again to cover the loop with multiple QPs.
  auto qp2 = setup.create_qp(pd_handle, send_cq, recv_cq);
  setup.transition_qp_to_rts(qp2, qp_number);
  setup.engine->advance_time(10000);

  std::printf("    PASSED\n");
}

// ============================================
// Test: create_cq with max_cqs exceeded
// ============================================
void test_create_cq_max_exceeded() {
  NIC_TRACE_SCOPED(__func__);
  std::printf("  test_create_cq_max_exceeded...\n");

  RdmaEngineConfig config;
  config.max_cqs = 1;
  EngineSetup setup(config);

  // First CQ should succeed.
  [[maybe_unused]] auto cq1 = setup.engine->create_cq(256);
  assert(cq1.has_value());

  // Second CQ should fail because max_cqs=1.
  [[maybe_unused]] auto cq2 = setup.engine->create_cq(256);
  assert(!cq2.has_value());
  assert(setup.engine->stats().errors > 0);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running RoCEv2 engine coverage tests...\n");

  test_create_qp_invalid_pd();
  test_create_qp_invalid_cq();
  test_create_qp_max_exceeded();
  test_register_mr_invalid_pd();
  test_destroy_cq_in_use();
  test_post_send_invalid_qp();
  test_post_send_not_rts();
  test_post_send_invalid_opcode();
  test_post_recv_invalid_qp();
  test_poll_cq_invalid();
  test_modify_qp_invalid();
  test_query_qp_invalid();
  test_destroy_qp_invalid();
  test_advance_time();
  test_create_cq_max_exceeded();

  std::printf("All RoCEv2 engine coverage tests PASSED!\n");
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

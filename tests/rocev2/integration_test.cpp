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

// Test helper to set up two QPs for communication
struct TwoQpSetup {
  std::unique_ptr<SimpleHostMemory> host_memory;
  std::unique_ptr<DMAEngine> dma_engine;
  std::unique_ptr<RdmaEngine> engine;
  std::uint32_t pd_handle{0};
  std::uint32_t send_cq{0};
  std::uint32_t recv_cq{0};
  std::uint32_t qp1{0};  // Requester
  std::uint32_t qp2{0};  // Responder
  std::uint32_t mr_lkey{0};

  TwoQpSetup() {
    HostMemoryConfig mem_cfg{.size_bytes = 256 * 1024};
    host_memory = std::make_unique<SimpleHostMemory>(mem_cfg);
    dma_engine = std::make_unique<DMAEngine>(*host_memory);

    RdmaEngineConfig engine_config;
    engine_config.mtu = 1024;
    engine = std::make_unique<RdmaEngine>(engine_config, *dma_engine, *host_memory);

    // Create PD
    auto pd = engine->create_pd();
    assert(pd.has_value());
    pd_handle = *pd;

    // Create CQs
    auto cq1 = engine->create_cq(256);
    auto cq2 = engine->create_cq(256);
    assert(cq1.has_value() && cq2.has_value());
    send_cq = *cq1;
    recv_cq = *cq2;

    // Create QP1 (requester)
    RdmaQpConfig qp_config;
    qp_config.pd_handle = pd_handle;
    qp_config.send_cq_number = send_cq;
    qp_config.recv_cq_number = recv_cq;
    auto qp1_opt = engine->create_qp(qp_config);
    assert(qp1_opt.has_value());
    qp1 = *qp1_opt;

    // Create QP2 (responder)
    auto qp2_opt = engine->create_qp(qp_config);
    assert(qp2_opt.has_value());
    qp2 = *qp2_opt;

    // Register MR with full access
    AccessFlags access{
        .local_read = true, .local_write = true, .remote_read = true, .remote_write = true};
    auto lkey = engine->register_mr(pd_handle, 0x1000, 64 * 1024, access);
    assert(lkey.has_value());
    mr_lkey = *lkey;

    // Transition QPs to RTS
    RdmaQpModifyParams params;
    params.target_state = QpState::Init;
    assert(engine->modify_qp(qp1, params));
    assert(engine->modify_qp(qp2, params));

    params.target_state = QpState::Rtr;
    params.dest_qp_number = qp2;
    params.rq_psn = 0;
    params.dest_ip = std::array<std::uint8_t, 4>{192, 168, 1, 2};
    assert(engine->modify_qp(qp1, params));

    params.dest_qp_number = qp1;
    params.dest_ip = std::array<std::uint8_t, 4>{192, 168, 1, 1};
    assert(engine->modify_qp(qp2, params));

    // Clear params and set only what's needed for RTS transition
    params = RdmaQpModifyParams{};
    params.target_state = QpState::Rts;
    params.sq_psn = 0;
    assert(engine->modify_qp(qp1, params));
    assert(engine->modify_qp(qp2, params));
  }

  // Transfer packets between the two QPs (loopback simulation)
  void transfer_packets() {
    auto packets = engine->generate_outgoing_packets();
    for (auto& pkt : packets) {
      // Route to destination based on dest_ip
      std::array<std::uint8_t, 4> src_ip = {192, 168, 1, 1};
      if (pkt.dest_ip[3] == 1) {
        src_ip = {192, 168, 1, 2};  // From QP2 to QP1
      }
      engine->process_incoming_packet(pkt.data, src_ip, pkt.dest_ip, pkt.src_port);
    }
  }
};

// Test basic engine initialization
void test_engine_initialization() {
  std::printf("  test_engine_initialization...\n");

  HostMemoryConfig mem_cfg{.size_bytes = 64 * 1024};
  SimpleHostMemory host_memory{mem_cfg};
  DMAEngine dma_engine{host_memory};

  RdmaEngineConfig config;
  RdmaEngine engine{config, dma_engine, host_memory};

  assert(engine.is_enabled());
  assert(engine.stats().pds_created == 0);
  assert(engine.stats().qps_created == 0);

  std::printf("    PASSED\n");
}

// Test PD and MR creation
void test_pd_mr_creation() {
  std::printf("  test_pd_mr_creation...\n");

  HostMemoryConfig mem_cfg{.size_bytes = 64 * 1024};
  SimpleHostMemory host_memory{mem_cfg};
  DMAEngine dma_engine{host_memory};

  RdmaEngineConfig config;
  RdmaEngine engine{config, dma_engine, host_memory};

  // Create PD
  auto pd = engine.create_pd();
  assert(pd.has_value());
  assert(engine.stats().pds_created == 1);

  // Register MR
  AccessFlags access{.local_read = true, .local_write = true};
  [[maybe_unused]] auto lkey = engine.register_mr(*pd, 0x1000, 4096, access);
  assert(lkey.has_value());
  assert(engine.stats().mrs_registered == 1);

  // Deregister MR
  assert(engine.deregister_mr(*lkey));

  // Destroy PD
  assert(engine.destroy_pd(*pd));

  std::printf("    PASSED\n");
}

// Test CQ and QP creation
void test_cq_qp_creation() {
  std::printf("  test_cq_qp_creation...\n");

  HostMemoryConfig mem_cfg{.size_bytes = 64 * 1024};
  SimpleHostMemory host_memory{mem_cfg};
  DMAEngine dma_engine{host_memory};

  RdmaEngineConfig config;
  RdmaEngine engine{config, dma_engine, host_memory};

  auto pd = engine.create_pd();
  assert(pd.has_value());

  auto send_cq = engine.create_cq(256);
  auto recv_cq = engine.create_cq(256);
  assert(send_cq.has_value() && recv_cq.has_value());
  assert(engine.stats().cqs_created == 2);

  RdmaQpConfig qp_config;
  qp_config.pd_handle = *pd;
  qp_config.send_cq_number = *send_cq;
  qp_config.recv_cq_number = *recv_cq;

  auto qp = engine.create_qp(qp_config);
  assert(qp.has_value());
  assert(engine.stats().qps_created == 1);

  // Query QP
  [[maybe_unused]] auto* queried = engine.query_qp(*qp);
  assert(queried != nullptr);
  assert(queried->state() == QpState::Reset);

  // Destroy QP
  assert(engine.destroy_qp(*qp));

  // Destroy CQs
  assert(engine.destroy_cq(*send_cq));
  assert(engine.destroy_cq(*recv_cq));

  std::printf("    PASSED\n");
}

// Test QP state transitions
void test_qp_state_transitions() {
  std::printf("  test_qp_state_transitions...\n");

  TwoQpSetup setup;

  [[maybe_unused]] auto* qp1 = setup.engine->query_qp(setup.qp1);
  assert(qp1 != nullptr);
  assert(qp1->state() == QpState::Rts);

  [[maybe_unused]] auto* qp2 = setup.engine->query_qp(setup.qp2);
  assert(qp2 != nullptr);
  assert(qp2->state() == QpState::Rts);

  std::printf("    PASSED\n");
}

// Test SEND/RECV loopback
void test_send_recv_loopback() {
  std::printf("  test_send_recv_loopback...\n");

  TwoQpSetup setup;

  // Write test data to host memory
  std::vector<std::byte> send_data(256);
  for (std::size_t idx = 0; idx < 256; ++idx) {
    send_data[idx] = static_cast<std::byte>(idx);
  }
  [[maybe_unused]] auto write_result = setup.host_memory->write(0x1000, send_data);
  assert(write_result.ok());

  // Post receive on QP2 (responder)
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2001;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 512});
  assert(setup.engine->post_recv(setup.qp2, recv_wqe));

  // Post send on QP1 (requester)
  SendWqe send_wqe;
  send_wqe.wr_id = 1001;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 256});
  send_wqe.total_length = 256;
  send_wqe.local_lkey = setup.mr_lkey;
  assert(setup.engine->post_send(setup.qp1, send_wqe));

  assert(setup.engine->stats().send_wqes_posted == 1);
  assert(setup.engine->stats().recv_wqes_posted == 1);

  // Transfer packets (send packet to responder)
  setup.transfer_packets();

  // Transfer ACK back
  setup.transfer_packets();

  // Check for completions
  auto send_cqes = setup.engine->poll_cq(setup.send_cq, 10);
  auto recv_cqes = setup.engine->poll_cq(setup.recv_cq, 10);

  // We should have at least a send completion
  assert(!send_cqes.empty());
  assert(send_cqes[0].wr_id == 1001);
  assert(send_cqes[0].status == WqeStatus::Success);

  std::printf("    PASSED\n");
}

// Test RDMA WRITE loopback
void test_rdma_write_loopback() {
  std::printf("  test_rdma_write_loopback...\n");

  TwoQpSetup setup;

  // Write test data at source
  std::vector<std::byte> write_data(128);
  for (std::size_t idx = 0; idx < 128; ++idx) {
    write_data[idx] = static_cast<std::byte>(0xAA + idx);
  }
  [[maybe_unused]] auto write_result = setup.host_memory->write(0x1000, write_data);
  assert(write_result.ok());

  // Get rkey for remote memory
  const MemoryRegion* mr = setup.engine->mr_table().get_by_lkey(setup.mr_lkey);
  assert(mr != nullptr);

  // Post RDMA WRITE
  SendWqe write_wqe;
  write_wqe.wr_id = 1002;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 128});
  write_wqe.total_length = 128;
  write_wqe.local_lkey = setup.mr_lkey;
  write_wqe.remote_address = 0x3000;
  write_wqe.rkey = mr->rkey;
  assert(setup.engine->post_send(setup.qp1, write_wqe));

  // Transfer WRITE packet
  setup.transfer_packets();

  // Transfer ACK
  setup.transfer_packets();

  // Verify data was written to remote address
  std::vector<std::byte> read_back(128);
  [[maybe_unused]] auto read_result = setup.host_memory->read(0x3000, read_back);
  assert(read_result.ok());
  for (std::size_t idx = 0; idx < 128; ++idx) {
    assert(read_back[idx] == static_cast<std::byte>(0xAA + idx));
  }

  std::printf("    PASSED\n");
}

// Test RDMA READ loopback
void test_rdma_read_loopback() {
  std::printf("  test_rdma_read_loopback...\n");

  TwoQpSetup setup;

  // Write test data at remote (QP2) side
  std::vector<std::byte> remote_data(64);
  for (std::size_t idx = 0; idx < 64; ++idx) {
    remote_data[idx] = static_cast<std::byte>(0xBB + idx);
  }
  [[maybe_unused]] auto write_result = setup.host_memory->write(0x4000, remote_data);
  assert(write_result.ok());

  // Get rkey
  const MemoryRegion* mr = setup.engine->mr_table().get_by_lkey(setup.mr_lkey);
  assert(mr != nullptr);

  // Post RDMA READ
  SendWqe read_wqe;
  read_wqe.wr_id = 1003;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x5000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = setup.mr_lkey;
  read_wqe.remote_address = 0x4000;
  read_wqe.rkey = mr->rkey;
  assert(setup.engine->post_send(setup.qp1, read_wqe));

  // Transfer READ request
  setup.transfer_packets();

  // Transfer READ responses
  setup.transfer_packets();

  // Verify data was read to local buffer
  std::vector<std::byte> local_buf(64);
  [[maybe_unused]] auto read_result = setup.host_memory->read(0x5000, local_buf);
  assert(read_result.ok());
  for (std::size_t idx = 0; idx < 64; ++idx) {
    assert(local_buf[idx] == static_cast<std::byte>(0xBB + idx));
  }

  std::printf("    PASSED\n");
}

// Test engine reset
void test_engine_reset() {
  std::printf("  test_engine_reset...\n");

  TwoQpSetup setup;

  assert(setup.engine->stats().qps_created == 2);
  assert(setup.engine->stats().pds_created == 1);

  setup.engine->reset();

  assert(setup.engine->stats().qps_created == 0);
  assert(setup.engine->stats().pds_created == 0);

  // Should be able to create resources again
  [[maybe_unused]] auto pd = setup.engine->create_pd();
  assert(pd.has_value());

  std::printf("    PASSED\n");
}

// Test disabled engine
void test_disabled_engine() {
  std::printf("  test_disabled_engine...\n");

  HostMemoryConfig mem_cfg{.size_bytes = 64 * 1024};
  SimpleHostMemory host_memory{mem_cfg};
  DMAEngine dma_engine{host_memory};

  RdmaEngineConfig config;
  config.enabled = false;
  RdmaEngine engine{config, dma_engine, host_memory};

  assert(!engine.is_enabled());

  // All operations should fail gracefully
  [[maybe_unused]] auto pd = engine.create_pd();
  assert(!pd.has_value());

  [[maybe_unused]] auto cq = engine.create_cq(256);
  assert(!cq.has_value());

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running RoCEv2 integration tests...\n");

  test_engine_initialization();
  test_pd_mr_creation();
  test_cq_qp_creation();
  test_qp_state_transitions();
  test_send_recv_loopback();
  test_rdma_write_loopback();
  test_rdma_read_loopback();
  test_engine_reset();
  test_disabled_engine();

  std::printf("All RoCEv2 integration tests PASSED!\n");
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

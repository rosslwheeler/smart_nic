#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "nic/rocev2/memory_region.h"
#include "nic/rocev2/packet.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/rdma_read.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;
using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

constexpr std::size_t kTestMemorySize = 64 * 1024;
constexpr std::uint32_t kRequesterQpNum = 1;
constexpr std::uint32_t kResponderQpNum = 2;

// Helper to set up QP for operations
void setup_qp_for_rdma([[maybe_unused]] RdmaQueuePair& qp, std::uint32_t dest_qp) {
  RdmaQpModifyParams params;

  params.target_state = QpState::Init;
  assert(qp.modify(params));

  params.target_state = QpState::Rtr;
  params.dest_qp_number = dest_qp;
  params.rq_psn = 0;
  params.dest_ip = std::array<std::uint8_t, 4>{192, 168, 1, 2};
  assert(qp.modify(params));

  params.target_state = QpState::Rts;
  params.sq_psn = 0;
  assert(qp.modify(params));
}

// Helper to create a test pattern
std::vector<std::byte> make_test_pattern(std::size_t size) {
  std::vector<std::byte> data(size);
  for (std::size_t idx = 0; idx < size; ++idx) {
    data[idx] = static_cast<std::byte>(idx & 0xFF);
  }
  return data;
}

// Test basic single-packet RDMA READ
void test_single_packet_read() {
  std::printf("  test_single_packet_read...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register local MR for receiving read data (needs local_write)
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  // Register remote MR for reading source data (needs remote_read)
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  assert(remote_mr != nullptr);
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data to remote buffer (the source of the read)
  std::vector<std::byte> test_data = make_test_pattern(256);
  (void) host_memory.write(0x2000, test_data);

  // Create RDMA READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1001;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 256});
  read_wqe.total_length = 256;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = rkey;
  read_wqe.signaled = true;

  // Generate READ_REQUEST packet
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);

  // Should generate exactly one request packet
  assert(request_packets.size() == 1);
  assert(processor.stats().read_requests_generated == 1);

  // Parse the request packet
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadRequest);
  assert(parser.bth().dest_qp == kResponderQpNum);
  assert(parser.has_reth());
  assert(parser.reth().virtual_address == 0x2000);
  assert(parser.reth().rkey == rkey);
  assert(parser.reth().dma_length == 256);

  // Process at responder - generates response packets
  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(req_result.success);
  assert(!req_result.needs_nak);
  assert(req_result.response_packets.size() == 1);  // Single response for 256 bytes
  assert(processor.stats().read_responses_generated == 1);

  // Parse response packet
  assert(parser.parse(req_result.response_packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadResponseOnly);
  assert(parser.bth().dest_qp == kRequesterQpNum);
  assert(parser.has_aeth());
  assert(parser.aeth().syndrome == AethSyndrome::Ack);
  assert(parser.payload().size() == 256);

  // Process response at requester
  [[maybe_unused]] ReadResponseResult resp_result =
      processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(resp_result.is_read_complete);
  assert(resp_result.cqe.has_value());
  assert(resp_result.cqe->status == WqeStatus::Success);
  assert(resp_result.cqe->wr_id == 1001);
  assert(resp_result.cqe->bytes_completed == 256);
  assert(resp_result.cqe->opcode == WqeOpcode::RdmaRead);
  assert(processor.stats().reads_completed == 1);

  // Verify data was written to local buffer
  std::vector<std::byte> local_data(256);
  (void) host_memory.read(0x1000, local_data);
  assert(local_data == test_data);

  std::printf("    PASSED\n");
}

// Test multi-packet RDMA READ
void test_multi_packet_read() {
  std::printf("  test_multi_packet_read...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Set small MTU for testing
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(requester_qp.modify(mtu_params));
  assert(responder_qp.modify(mtu_params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 8192, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 8192, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data to remote buffer (600 bytes - 3 response packets at 256 MTU)
  std::vector<std::byte> test_data = make_test_pattern(600);
  (void) host_memory.write(0x4000, test_data);

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1002;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  read_wqe.total_length = 600;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x4000;
  read_wqe.rkey = rkey;

  // Generate request
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Process request at responder
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));

  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(req_result.success);
  assert(req_result.response_packets.size() == 3);  // 3 response packets

  // First response - has AETH
  assert(parser.parse(req_result.response_packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadResponseFirst);
  assert(parser.bth().psn == 0);
  assert(parser.has_aeth());
  assert(parser.payload().size() == 256);

  ReadResponseResult resp_result = processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(!resp_result.is_read_complete);

  // Middle response - no AETH
  assert(parser.parse(req_result.response_packets[1]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadResponseMiddle);
  assert(parser.bth().psn == 1);
  assert(!parser.has_aeth());
  assert(parser.payload().size() == 256);

  resp_result = processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(!resp_result.is_read_complete);

  // Last response
  assert(parser.parse(req_result.response_packets[2]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadResponseLast);
  assert(parser.bth().psn == 2);
  assert(parser.payload().size() == 88);

  resp_result = processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(resp_result.is_read_complete);
  assert(resp_result.cqe.has_value());
  assert(resp_result.cqe->bytes_completed == 600);

  // Verify all data was read correctly
  std::vector<std::byte> local_data(600);
  (void) host_memory.read(0x1000, local_data);
  assert(local_data == test_data);

  std::printf("    PASSED\n");
}

// Test rkey validation error (no remote_read permission)
void test_rkey_validation_error() {
  std::printf("  test_rkey_validation_error...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register local memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  // Register remote memory WITHOUT remote_read permission
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = false};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1003;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = rkey;

  // Generate request
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Process request - should fail with remote access error
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));

  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(!req_result.success);
  assert(req_result.needs_nak);
  assert(req_result.syndrome == AethSyndrome::RemoteAccessError);
  assert(processor.stats().rkey_errors == 1);

  std::printf("    PASSED\n");
}

// Test invalid rkey
void test_invalid_rkey() {
  std::printf("  test_invalid_rkey...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register local memory only
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  // Create READ WQE with invalid rkey
  SendWqe read_wqe;
  read_wqe.wr_id = 1004;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = 0xDEADBEEF;  // Invalid rkey

  // Generate request
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Process request - should fail
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));

  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(!req_result.success);
  assert(req_result.needs_nak);
  assert(req_result.syndrome == AethSyndrome::RemoteAccessError);

  std::printf("    PASSED\n");
}

// Test zero-length READ
void test_zero_length_read() {
  std::printf("  test_zero_length_read...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register remote memory
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Create zero-length READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1005;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.total_length = 0;
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = rkey;

  // Generate request
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Process request
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));
  assert(parser.reth().dma_length == 0);

  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(req_result.success);
  assert(req_result.response_packets.size() == 1);

  // Response should be empty
  assert(parser.parse(req_result.response_packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadResponseOnly);
  assert(parser.payload().empty());

  // Process response
  [[maybe_unused]] ReadResponseResult resp_result =
      processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(resp_result.is_read_complete);
  assert(resp_result.cqe.has_value());
  assert(resp_result.cqe->bytes_completed == 0);

  std::printf("    PASSED\n");
}

// Test PSN sequence error on request
void test_psn_sequence_error() {
  std::printf("  test_psn_sequence_error...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1006;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = rkey;

  // Generate request
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Manually corrupt PSN to cause sequence error
  // The responder expects PSN 0, but we'll advance it first
  responder_qp.advance_recv_psn();  // Now expects PSN 1

  // Process request - should fail with PSN sequence error
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));
  assert(parser.bth().psn == 0);  // Request has PSN 0

  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(!req_result.success);
  assert(req_result.needs_nak);
  assert(req_result.syndrome == AethSyndrome::PsnSeqError);
  assert(processor.stats().sequence_errors == 1);

  std::printf("    PASSED\n");
}

// Test scatter-gather list support for local buffer
void test_scatter_gather_read() {
  std::printf("  test_scatter_gather_read...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register local memory (multiple regions for SGL)
  // MR covers 0x1000 to 0x3100 (8448 bytes) to include all SGL entries
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 8448, local_access);
  assert(local_lkey.has_value());

  // Register remote memory
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data to remote buffer
  std::vector<std::byte> test_data = make_test_pattern(300);
  (void) host_memory.write(0x4000, test_data);

  // Create READ WQE with multiple SGL entries
  SendWqe read_wqe;
  read_wqe.wr_id = 1007;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 100});
  read_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 100});
  read_wqe.sgl.push_back(SglEntry{.address = 0x3000, .length = 100});
  read_wqe.total_length = 300;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x4000;
  read_wqe.rkey = rkey;

  // Generate and process request
  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);

  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));

  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(req_result.success);
  assert(req_result.response_packets.size() == 1);  // 300 bytes fits in default MTU

  // Process response
  assert(parser.parse(req_result.response_packets[0]));
  [[maybe_unused]] ReadResponseResult resp_result =
      processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(resp_result.is_read_complete);

  // Verify data was scattered to all three local buffers
  std::vector<std::byte> buf1(100);
  std::vector<std::byte> buf2(100);
  std::vector<std::byte> buf3(100);
  (void) host_memory.read(0x1000, buf1);
  (void) host_memory.read(0x2000, buf2);
  (void) host_memory.read(0x3000, buf3);

  // Check each segment matches the corresponding part of test_data
  for (std::size_t idx = 0; idx < 100; ++idx) {
    assert(buf1[idx] == test_data[idx]);
    assert(buf2[idx] == test_data[100 + idx]);
    assert(buf3[idx] == test_data[200 + idx]);
  }

  std::printf("    PASSED\n");
}

// Test invalid WQE opcode for read processor
void test_invalid_wqe_opcode() {
  std::printf("  test_invalid_wqe_opcode...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);

  // Create WQE with wrong opcode (Send instead of RdmaRead)
  SendWqe wrong_wqe;
  wrong_wqe.wr_id = 1008;
  wrong_wqe.opcode = WqeOpcode::Send;  // Wrong opcode for read processor
  wrong_wqe.total_length = 64;

  ReadProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_read_request(requester_qp, wrong_wqe);

  // Should return empty - invalid opcode
  assert(packets.empty());

  std::printf("    PASSED\n");
}

// Test QP cannot receive (wrong state)
void test_qp_cannot_receive() {
  std::printf("  test_qp_cannot_receive...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  // Create requester QP in RTS state
  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);

  // Create responder QP but leave in Reset state
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  // Don't transition to RTS

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1009;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = rkey;

  ReadProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(packets.size() == 1);

  // Try to process at responder in Reset state
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  ReadRequestResult result = processor.process_read_request(responder_qp, parser);
  assert(!result.success);
  assert(result.needs_nak);
  assert(result.syndrome == AethSyndrome::InvalidRequest);

  std::printf("    PASSED\n");
}

// Test processor reset
void test_processor_reset() {
  std::printf("  test_processor_reset...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1010;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = remote_mr->rkey;

  ReadProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(packets.size() == 1);
  assert(processor.stats().read_requests_generated == 1);

  // Reset the processor
  processor.reset();

  // Stats should be cleared
  assert(processor.stats().read_requests_generated == 0);
  assert(processor.stats().reads_started == 0);

  std::printf("    PASSED\n");
}

// Test clear_read_state
void test_clear_read_state() {
  std::printf("  test_clear_read_state...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Set small MTU for multi-packet response
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(requester_qp.modify(mtu_params));
  assert(responder_qp.modify(mtu_params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());

  // Write test data (600 bytes - 3 response packets)
  std::vector<std::byte> test_data(600);
  (void) host_memory.write(0x4000, test_data);

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1011;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  read_wqe.total_length = 600;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x4000;
  read_wqe.rkey = remote_mr->rkey;

  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Process request at responder
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));
  ReadRequestResult req_result = processor.process_read_request(responder_qp, parser);
  assert(req_result.success);
  assert(req_result.response_packets.size() == 3);

  // Process first response at requester
  assert(parser.parse(req_result.response_packets[0]));
  ReadResponseResult resp_result = processor.process_read_response(requester_qp, parser);
  assert(resp_result.success);
  assert(!resp_result.is_read_complete);

  // Clear the read state
  processor.clear_read_state(kRequesterQpNum);

  // Now processing the next response should fail silently (no outstanding read)
  assert(parser.parse(req_result.response_packets[1]));
  resp_result = processor.process_read_response(requester_qp, parser);
  assert(!resp_result.success);  // No outstanding read request

  std::printf("    PASSED\n");
}

// Test response without outstanding read request
void test_response_no_request() {
  std::printf("  test_response_no_request...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_read = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());

  // Write test data
  std::vector<std::byte> test_data(64);
  (void) host_memory.write(0x2000, test_data);

  // Create READ WQE
  SendWqe read_wqe;
  read_wqe.wr_id = 1012;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = local_lkey.value();
  read_wqe.remote_address = 0x2000;
  read_wqe.rkey = remote_mr->rkey;

  // Create a separate processor for requester and responder to simulate
  // receiving a response without having sent a request
  ReadProcessor requester_processor{host_memory, mr_table};
  ReadProcessor responder_processor{host_memory, mr_table};

  // Generate request packet (don't process it to keep no request state)
  auto request_packets = responder_processor.generate_read_request(responder_qp, read_wqe);

  // Get response packets by processing with responder processor
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));
  auto req_result = responder_processor.process_read_request(requester_qp, parser);
  assert(req_result.success);
  assert(!req_result.response_packets.empty());

  // Try to process response at requester without having sent a request
  assert(parser.parse(req_result.response_packets[0]));
  [[maybe_unused]] auto resp_result =
      requester_processor.process_read_response(requester_qp, parser);
  assert(!resp_result.success);  // No outstanding read request

  std::printf("    PASSED\n");
}

// Test invalid opcode in response processing (not a READ_RESPONSE)
void test_invalid_response_opcode() {
  std::printf("  test_invalid_response_opcode...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true, .remote_read = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  const MemoryRegion* mr = mr_table.get_by_lkey(lkey.value());

  // Create and send a READ request (to establish request state)
  SendWqe read_wqe;
  read_wqe.wr_id = 1013;
  read_wqe.opcode = WqeOpcode::RdmaRead;
  read_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  read_wqe.total_length = 64;
  read_wqe.local_lkey = lkey.value();
  read_wqe.remote_address = 0x1000;
  read_wqe.rkey = mr->rkey;

  ReadProcessor processor{host_memory, mr_table};
  auto request_packets = processor.generate_read_request(requester_qp, read_wqe);
  assert(request_packets.size() == 1);

  // Try to process the REQUEST packet as a response (wrong opcode type)
  RdmaPacketParser parser;
  assert(parser.parse(request_packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcReadRequest);

  [[maybe_unused]] auto resp_result = processor.process_read_response(requester_qp, parser);
  assert(!resp_result.success);  // Not a READ_RESPONSE opcode

  std::printf("    PASSED\n");
}

// Test invalid opcode in request processing (not a READ_REQUEST)
void test_invalid_request_opcode() {
  std::printf("  test_invalid_request_opcode...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair requester_qp{kRequesterQpNum, config};
  RdmaQueuePair responder_qp{kResponderQpNum, config};
  setup_qp_for_rdma(requester_qp, kResponderQpNum);
  setup_qp_for_rdma(responder_qp, kRequesterQpNum);

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true, .remote_read = true};
  auto lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(lkey.has_value());

  const MemoryRegion* mr = mr_table.get_by_lkey(lkey.value());

  // Build a WRITE packet instead of READ_REQUEST
  RdmaPacketBuilder builder;
  builder.set_opcode(RdmaOpcode::kRcWriteOnly)
      .set_dest_qp(kResponderQpNum)
      .set_psn(0)
      .set_remote_address(0x1000)
      .set_rkey(mr->rkey)
      .set_dma_length(64);

  auto packet = builder.build();

  ReadProcessor processor{host_memory, mr_table};
  RdmaPacketParser parser;
  assert(parser.parse(packet));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteOnly);

  auto req_result = processor.process_read_request(responder_qp, parser);
  assert(!req_result.success);  // Not a READ_REQUEST opcode

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running RDMA READ tests...\n");

  test_single_packet_read();
  test_multi_packet_read();
  test_rkey_validation_error();
  test_invalid_rkey();
  test_zero_length_read();
  test_psn_sequence_error();
  test_scatter_gather_read();
  test_invalid_wqe_opcode();
  test_qp_cannot_receive();
  test_processor_reset();
  test_clear_read_state();
  test_response_no_request();
  test_invalid_response_opcode();
  test_invalid_request_opcode();

  std::printf("All RDMA READ tests PASSED!\n");
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

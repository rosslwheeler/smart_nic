#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "nic/rocev2/memory_region.h"
#include "nic/rocev2/packet.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/rdma_write.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;
using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

constexpr std::size_t kTestMemorySize = 64 * 1024;
constexpr std::uint32_t kSenderQpNum = 1;
constexpr std::uint32_t kReceiverQpNum = 2;

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

// Test basic single-packet RDMA WRITE
void test_single_packet_write() {
  std::printf("  test_single_packet_write...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Register memory regions
  // Local MR for reading source data
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  // Remote MR for writing destination (needs remote_write)
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  // Get rkey from the MR
  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  assert(remote_mr != nullptr);
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data to local buffer
  std::vector<std::byte> test_data = make_test_pattern(256);
  (void) host_memory.write(0x1000, test_data);

  // Create RDMA WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1001;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 256});
  write_wqe.total_length = 256;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;
  write_wqe.signaled = true;

  // Generate WRITE packets
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);

  // Should generate exactly one packet
  assert(packets.size() == 1);
  assert(processor.stats().write_packets_generated == 1);

  // Parse the packet
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteOnly);
  assert(parser.bth().dest_qp == kReceiverQpNum);
  assert(parser.has_reth());
  assert(parser.reth().virtual_address == 0x2000);
  assert(parser.reth().rkey == rkey);
  assert(parser.reth().dma_length == 256);
  assert(parser.payload().size() == 256);

  // Process at receiver (no recv WQE needed for plain WRITE)
  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::Ack);
  assert(!result.recv_cqe.has_value());  // No CQE for plain WRITE

  // Verify data was written to remote buffer
  std::vector<std::byte> recv_data(256);
  (void) host_memory.read(0x2000, recv_data);
  assert(recv_data == test_data);

  std::printf("    PASSED\n");
}

// Test multi-packet RDMA WRITE
void test_multi_packet_write() {
  std::printf("  test_multi_packet_write...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Set small MTU for testing
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 8192, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 8192, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data (600 bytes - 3 packets at 256 MTU)
  std::vector<std::byte> test_data = make_test_pattern(600);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1002;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  write_wqe.total_length = 600;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x4000;
  write_wqe.rkey = rkey;

  // Generate packets
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);

  // Should generate 3 packets
  assert(packets.size() == 3);

  RdmaPacketParser parser;

  // First packet - has RETH
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteFirst);
  assert(parser.bth().psn == 0);
  assert(parser.has_reth());
  assert(parser.reth().dma_length == 600);
  assert(parser.payload().size() == 256);

  WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Middle packet - no RETH
  assert(parser.parse(packets[1]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteMiddle);
  assert(parser.bth().psn == 1);
  assert(!parser.has_reth());
  assert(parser.payload().size() == 256);

  result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Last packet - no RETH
  assert(parser.parse(packets[2]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteLast);
  assert(parser.bth().psn == 2);
  assert(!parser.has_reth());
  assert(parser.payload().size() == 88);

  result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(processor.stats().writes_completed == 1);

  // Verify all data was written
  std::vector<std::byte> recv_data(600);
  (void) host_memory.read(0x4000, recv_data);
  assert(recv_data == test_data);

  std::printf("    PASSED\n");
}

// Test RDMA WRITE with immediate data
void test_write_with_immediate() {
  std::printf("  test_write_with_immediate...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(128);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE with immediate WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1003;
  write_wqe.opcode = WqeOpcode::RdmaWriteImm;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 128});
  write_wqe.total_length = 128;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;
  write_wqe.immediate_data = 0xCAFEBABE;

  // Post recv WQE (needed for immediate)
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2003;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x3000, .length = 64});  // Buffer not used for WRITE
  recv_wqe.total_length = 64;
  assert(receiver_qp.post_recv(recv_wqe));

  // Generate packets
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Verify packet has immediate opcode
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteOnlyImm);
  assert(parser.has_immediate());
  assert(parser.immediate() == 0xCAFEBABE);

  // Process at receiver
  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.recv_cqe.has_value());
  assert(result.recv_cqe->has_immediate);
  assert(result.recv_cqe->immediate_data == 0xCAFEBABE);
  assert(result.recv_cqe->wr_id == 2003);
  assert(result.recv_cqe->opcode == WqeOpcode::RdmaWriteImm);

  // Verify data was written
  std::vector<std::byte> recv_data(128);
  (void) host_memory.read(0x2000, recv_data);
  assert(recv_data == test_data);

  std::printf("    PASSED\n");
}

// Test rkey validation error
void test_rkey_validation_error() {
  std::printf("  test_rkey_validation_error...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Register local memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  // Register remote memory WITHOUT remote_write permission
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = false};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(64);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1004;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;

  // Generate packet
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Process - should fail with remote access error
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::RemoteAccessError);
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
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Register local memory only
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(64);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE with invalid rkey
  SendWqe write_wqe;
  write_wqe.wr_id = 1005;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = 0xDEADBEEF;  // Invalid rkey

  // Generate packet
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Process - should fail
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.syndrome == AethSyndrome::RemoteAccessError);

  std::printf("    PASSED\n");
}

// Test zero-length WRITE
void test_zero_length_write() {
  std::printf("  test_zero_length_write...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Register remote memory
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Create zero-length WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1006;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.total_length = 0;
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;

  // Generate packet
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Parse and verify
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteOnly);
  assert(parser.has_reth());
  assert(parser.reth().dma_length == 0);
  assert(parser.payload().empty());

  // Process
  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);

  std::printf("    PASSED\n");
}

// Test WRITE with immediate but no recv WQE (RNR)
void test_write_imm_rnr() {
  std::printf("  test_write_imm_rnr...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(64);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE with immediate - DON'T post recv WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1007;
  write_wqe.opcode = WqeOpcode::RdmaWriteImm;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;
  write_wqe.immediate_data = 0x12345678;

  // Generate packet
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Process - should get RNR NAK because no recv WQE for immediate
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  // The write itself succeeds, but we can't complete because of RNR
  assert(!result.success);
  assert(result.syndrome == AethSyndrome::RnrNak);

  std::printf("    PASSED\n");
}

// Test invalid WQE opcode for write processor
void test_invalid_wqe_opcode() {
  std::printf("  test_invalid_wqe_opcode...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);

  // Create WQE with wrong opcode (Send instead of RdmaWrite)
  SendWqe wrong_wqe;
  wrong_wqe.wr_id = 1008;
  wrong_wqe.opcode = WqeOpcode::Send;  // Wrong opcode for write processor
  wrong_wqe.total_length = 64;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, wrong_wqe);

  // Should return empty - invalid opcode
  assert(packets.empty());

  std::printf("    PASSED\n");
}

// Test SGL read failure (invalid lkey)
void test_sgl_read_failure() {
  std::printf("  test_sgl_read_failure...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);

  // Register remote memory
  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Create WRITE WQE with invalid local lkey
  SendWqe write_wqe;
  write_wqe.wr_id = 1009;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = 0xDEADBEEF;  // Invalid lkey
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);

  // Should return empty - SGL read failed
  assert(packets.empty());

  std::printf("    PASSED\n");
}

// Test QP cannot receive (wrong state)
void test_qp_cannot_receive() {
  std::printf("  test_qp_cannot_receive...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  // Create sender QP in RTS state
  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);

  // Create receiver QP but leave in Reset state
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  // Don't transition to RTS

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data
  std::vector<std::byte> test_data(64);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1010;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Try to process at receiver in Reset state
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::InvalidRequest);

  std::printf("    PASSED\n");
}

// Test middle packet without prior first packet (PSN error)
void test_middle_packet_no_first() {
  std::printf("  test_middle_packet_no_first...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Set small MTU for multi-packet message
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data (600 bytes - 3 packets)
  std::vector<std::byte> test_data(600);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1011;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  write_wqe.total_length = 600;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x4000;
  write_wqe.rkey = rkey;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 3);

  // Try to process MIDDLE packet without first
  // Middle packet has PSN=1, but receiver expects PSN=0, so PSN sequence error
  RdmaPacketParser parser;
  assert(parser.parse(packets[1]));
  assert(parser.bth().opcode == RdmaOpcode::kRcWriteMiddle);
  assert(parser.bth().psn == 1);

  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::PsnSeqError);  // PSN mismatch

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
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());

  // Write test data
  std::vector<std::byte> test_data(64);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1012;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = remote_mr->rkey;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);
  assert(processor.stats().write_packets_generated == 1);

  // Reset the processor
  processor.reset();

  // Stats should be cleared
  assert(processor.stats().write_packets_generated == 0);
  assert(processor.stats().writes_started == 0);

  std::printf("    PASSED\n");
}

// Test clear_write_state
void test_clear_write_state() {
  std::printf("  test_clear_write_state...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Set small MTU for multi-packet message
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data (600 bytes - 3 packets)
  std::vector<std::byte> test_data(600);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1013;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  write_wqe.total_length = 600;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x4000;
  write_wqe.rkey = rkey;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 3);

  // Process first packet to establish write state
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Clear the write state
  processor.clear_write_state(kReceiverQpNum);

  // Now processing middle packet should fail (no write state)
  assert(parser.parse(packets[1]));
  result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.syndrome == AethSyndrome::InvalidRequest);

  std::printf("    PASSED\n");
}

// Test multi-packet PSN mismatch
void test_multi_packet_psn_mismatch() {
  std::printf("  test_multi_packet_psn_mismatch...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Set small MTU for multi-packet message
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x4000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data (600 bytes - 3 packets)
  std::vector<std::byte> test_data(600);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1014;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  write_wqe.total_length = 600;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x4000;
  write_wqe.rkey = rkey;

  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 3);

  // Process first packet
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Skip middle packet and try to process last packet directly
  // This will fail because expected PSN is 1 but last packet has PSN 2
  assert(parser.parse(packets[2]));
  assert(parser.bth().psn == 2);
  result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::PsnSeqError);

  std::printf("    PASSED\n");
}

// Test PSN sequence error on write
void test_psn_sequence_error() {
  std::printf("  test_psn_sequence_error...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_rdma(sender_qp, kReceiverQpNum);
  setup_qp_for_rdma(receiver_qp, kSenderQpNum);

  // Set expected PSN to something other than 0
  RdmaQpModifyParams params;
  params.rq_psn = 100;
  assert(receiver_qp.modify(params));

  // Register memory
  AccessFlags local_access{.local_read = true, .local_write = true};
  auto local_lkey = mr_table.register_mr(1, 0x1000, 4096, local_access);
  assert(local_lkey.has_value());

  AccessFlags remote_access{.local_read = true, .local_write = true, .remote_write = true};
  auto remote_lkey = mr_table.register_mr(1, 0x2000, 4096, remote_access);
  assert(remote_lkey.has_value());

  const MemoryRegion* remote_mr = mr_table.get_by_lkey(remote_lkey.value());
  std::uint32_t rkey = remote_mr->rkey;

  // Write test data
  std::vector<std::byte> test_data(64);
  (void) host_memory.write(0x1000, test_data);

  // Create WRITE WQE
  SendWqe write_wqe;
  write_wqe.wr_id = 1015;
  write_wqe.opcode = WqeOpcode::RdmaWrite;
  write_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  write_wqe.total_length = 64;
  write_wqe.local_lkey = local_lkey.value();
  write_wqe.remote_address = 0x2000;
  write_wqe.rkey = rkey;

  // Generate packet (will have PSN 0)
  WriteProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_write_packets(sender_qp, write_wqe);
  assert(packets.size() == 1);

  // Process - should fail with PSN sequence error
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().psn == 0);

  [[maybe_unused]] WriteResult result = processor.process_write_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::PsnSeqError);
  assert(processor.stats().sequence_errors == 1);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running RDMA WRITE tests...\n");

  test_single_packet_write();
  test_multi_packet_write();
  test_write_with_immediate();
  test_rkey_validation_error();
  test_invalid_rkey();
  test_zero_length_write();
  test_write_imm_rnr();
  test_invalid_wqe_opcode();
  test_sgl_read_failure();
  test_qp_cannot_receive();
  test_middle_packet_no_first();
  test_processor_reset();
  test_clear_write_state();
  test_multi_packet_psn_mismatch();
  test_psn_sequence_error();

  std::printf("All RDMA WRITE tests PASSED!\n");
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

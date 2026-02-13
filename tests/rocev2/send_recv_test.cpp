#include "nic/rocev2/send_recv.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "nic/rocev2/completion_queue.h"
#include "nic/rocev2/memory_region.h"
#include "nic/rocev2/packet.h"
#include "nic/rocev2/protection_domain.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;
using namespace nic::rocev2;

static void WaitForTracyConnection();

namespace {

constexpr std::size_t kTestMemorySize = 64 * 1024;  // 64KB test memory
constexpr std::uint32_t kSenderQpNum = 1;
constexpr std::uint32_t kReceiverQpNum = 2;
constexpr std::uint32_t kSendCqNum = 1;
constexpr std::uint32_t kRecvCqNum = 2;

// Helper to set up QP for sending
void setup_qp_for_send([[maybe_unused]] RdmaQueuePair& qp, std::uint32_t dest_qp) {
  RdmaQpModifyParams params;

  // Reset -> Init
  params.target_state = QpState::Init;
  assert(qp.modify(params));

  // Init -> RTR
  params.target_state = QpState::Rtr;
  params.dest_qp_number = dest_qp;
  params.rq_psn = 0;
  params.dest_ip = std::array<std::uint8_t, 4>{192, 168, 1, 2};
  assert(qp.modify(params));

  // RTR -> RTS
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

// Test basic single-packet SEND
void test_single_packet_send() {
  std::printf("  test_single_packet_send...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  // Create sender QP
  RdmaQpConfig sender_config;
  sender_config.pd_handle = 1;
  sender_config.send_cq_number = kSendCqNum;
  sender_config.recv_cq_number = kRecvCqNum;
  RdmaQueuePair sender_qp{kSenderQpNum, sender_config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);

  // Create receiver QP
  RdmaQpConfig receiver_config;
  receiver_config.pd_handle = 1;
  receiver_config.send_cq_number = kSendCqNum;
  receiver_config.recv_cq_number = kRecvCqNum;
  RdmaQueuePair receiver_qp{kReceiverQpNum, receiver_config};
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Register memory regions
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data to send buffer
  std::vector<std::byte> test_data = make_test_pattern(256);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1001;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 256});
  send_wqe.total_length = 256;
  send_wqe.local_lkey = send_lkey.value();
  send_wqe.signaled = true;

  // Create RECV WQE and post it
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2001;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 512});
  recv_wqe.total_length = 512;
  assert(receiver_qp.post_recv(recv_wqe));

  // Process SEND
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);

  // Should generate exactly one packet (256 bytes < 1024 MTU)
  assert(packets.size() == 1);
  assert(processor.stats().send_packets_generated == 1);

  // Parse the packet
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendOnly);
  assert(parser.bth().dest_qp == kReceiverQpNum);
  assert(parser.payload().size() == 256);

  // Process at receiver
  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::Ack);
  assert(result.cqe.has_value());
  assert(result.cqe->wr_id == 2001);
  assert(result.cqe->bytes_completed == 256);
  assert(!result.cqe->is_send);

  // Verify data was written to receive buffer
  std::vector<std::byte> recv_data(256);
  (void) host_memory.read(0x2000, recv_data);
  assert(recv_data == test_data);

  std::printf("    PASSED\n");
}

// Test multi-packet SEND (message larger than MTU)
void test_multi_packet_send() {
  std::printf("  test_multi_packet_send...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  // Create QPs with small MTU for testing
  RdmaQpConfig sender_config;
  sender_config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, sender_config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);

  // Set MTU to 256 bytes for testing multi-packet
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));

  RdmaQpConfig receiver_config;
  receiver_config.pd_handle = 1;
  RdmaQueuePair receiver_qp{kReceiverQpNum, receiver_config};
  setup_qp_for_send(receiver_qp, kSenderQpNum);
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 8192, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x4000, 8192, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data (600 bytes - will need 3 packets with 256 MTU)
  std::vector<std::byte> test_data = make_test_pattern(600);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1002;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  send_wqe.total_length = 600;
  send_wqe.local_lkey = send_lkey.value();

  // Create RECV WQE
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2002;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x4000, .length = 1024});
  recv_wqe.total_length = 1024;
  assert(receiver_qp.post_recv(recv_wqe));

  // Generate packets
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);

  // Should generate 3 packets: 256 + 256 + 88 = 600 bytes
  assert(packets.size() == 3);

  // Verify packet opcodes
  RdmaPacketParser parser;

  // First packet
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendFirst);
  assert(parser.bth().psn == 0);
  assert(parser.payload().size() == 256);

  RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Middle packet
  assert(parser.parse(packets[1]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendMiddle);
  assert(parser.bth().psn == 1);
  assert(parser.payload().size() == 256);

  result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Last packet
  assert(parser.parse(packets[2]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendLast);
  assert(parser.bth().psn == 2);
  assert(parser.payload().size() == 88);

  result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.cqe.has_value());
  assert(result.cqe->bytes_completed == 600);

  // Verify all data was received correctly
  std::vector<std::byte> recv_data(600);
  (void) host_memory.read(0x4000, recv_data);
  assert(recv_data == test_data);

  std::printf("    PASSED\n");
}

// Test SEND with immediate data
void test_send_with_immediate() {
  std::printf("  test_send_with_immediate...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(128);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND with immediate WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1003;
  send_wqe.opcode = WqeOpcode::SendImm;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 128});
  send_wqe.total_length = 128;
  send_wqe.local_lkey = send_lkey.value();
  send_wqe.immediate_data = 0xDEADBEEF;

  // Create RECV WQE
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2003;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 256});
  recv_wqe.total_length = 256;
  assert(receiver_qp.post_recv(recv_wqe));

  // Process
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 1);

  // Verify packet has immediate opcode
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendOnlyImm);
  assert(parser.has_immediate());
  assert(parser.immediate() == 0xDEADBEEF);

  // Process at receiver
  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.cqe.has_value());
  assert(result.cqe->has_immediate);
  assert(result.cqe->immediate_data == 0xDEADBEEF);
  assert(result.cqe->opcode == WqeOpcode::SendImm);

  std::printf("    PASSED\n");
}

// Test zero-length SEND
void test_zero_length_send() {
  std::printf("  test_zero_length_send...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Register memory for receiver
  AccessFlags access{.local_read = true, .local_write = true};
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(recv_lkey.has_value());

  // Create zero-length SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1004;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.total_length = 0;

  // Create RECV WQE
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2004;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 64});
  recv_wqe.total_length = 64;
  assert(receiver_qp.post_recv(recv_wqe));

  // Process
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 1);

  // Parse and verify
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendOnly);
  assert(parser.payload().empty());

  // Process at receiver
  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.cqe.has_value());
  assert(result.cqe->bytes_completed == 0);

  std::printf("    PASSED\n");
}

// Test RNR NAK when no RECV WQE posted
void test_rnr_nak() {
  std::printf("  test_rnr_nak...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(send_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(64);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE - but don't post a RECV WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1005;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  send_wqe.total_length = 64;
  send_wqe.local_lkey = send_lkey.value();

  // Generate packet
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 1);

  // Try to process without a RECV WQE
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::RnrNak);
  assert(processor.stats().rnr_naks_sent == 1);

  std::printf("    PASSED\n");
}

// Test PSN sequence error
void test_psn_sequence_error() {
  std::printf("  test_psn_sequence_error...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Set expected PSN to something other than 0
  RdmaQpModifyParams params;
  params.rq_psn = 100;
  assert(receiver_qp.modify(params));

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(64);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1006;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  send_wqe.total_length = 64;
  send_wqe.local_lkey = send_lkey.value();

  // Post RECV WQE
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2006;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 128});
  recv_wqe.total_length = 128;
  assert(receiver_qp.post_recv(recv_wqe));

  // Generate packet (will have PSN 0)
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);

  // Process - should fail with PSN sequence error
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  assert(parser.bth().psn == 0);

  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::PsnSeqError);
  assert(processor.stats().sequence_errors == 1);

  std::printf("    PASSED\n");
}

// Test ACK generation
void test_ack_generation() {
  std::printf("  test_ack_generation...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair qp{kSenderQpNum, config};
  setup_qp_for_send(qp, kReceiverQpNum);

  SendRecvProcessor processor{host_memory, mr_table};

  // Generate ACK
  auto ack_packet = processor.generate_ack(qp, 42, AethSyndrome::Ack, 10);
  assert(!ack_packet.empty());

  // Parse and verify
  RdmaPacketParser parser;
  assert(parser.parse(ack_packet));
  assert(parser.bth().opcode == RdmaOpcode::kRcAck);
  assert(parser.bth().psn == 42);
  assert(parser.has_aeth());
  assert(parser.aeth().syndrome == AethSyndrome::Ack);
  assert(parser.aeth().msn == 10);

  // Generate NAK
  auto nak_packet = processor.generate_ack(qp, 99, AethSyndrome::RnrNak, 5);
  assert(parser.parse(nak_packet));
  assert(parser.bth().opcode == RdmaOpcode::kRcAck);
  assert(parser.bth().psn == 99);
  assert(parser.aeth().syndrome == AethSyndrome::RnrNak);

  std::printf("    PASSED\n");
}

// Test scatter-gather receive
void test_scatter_gather_recv() {
  std::printf("  test_scatter_gather_recv...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 8192, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data = make_test_pattern(300);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1007;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 300});
  send_wqe.total_length = 300;
  send_wqe.local_lkey = send_lkey.value();

  // Create RECV WQE with multiple scatter-gather entries
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2007;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 100});
  recv_wqe.sgl.push_back(SglEntry{.address = 0x3000, .length = 100});
  recv_wqe.sgl.push_back(SglEntry{.address = 0x4000, .length = 200});
  recv_wqe.total_length = 400;
  assert(receiver_qp.post_recv(recv_wqe));

  // Process
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 1);

  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(result.is_message_complete);
  assert(result.cqe->bytes_completed == 300);

  // Verify data was scattered correctly
  std::vector<std::byte> buf1(100), buf2(100), buf3(100);
  (void) host_memory.read(0x2000, buf1);
  (void) host_memory.read(0x3000, buf2);
  (void) host_memory.read(0x4000, buf3);

  // First 100 bytes should be in buf1
  assert(std::equal(buf1.begin(), buf1.end(), test_data.begin()));
  // Next 100 bytes in buf2
  assert(std::equal(buf2.begin(), buf2.end(), test_data.begin() + 100));
  // Next 100 bytes in buf3
  assert(std::equal(buf3.begin(), buf3.end(), test_data.begin() + 200));

  std::printf("    PASSED\n");
}

// Test invalid WQE opcode
void test_invalid_wqe_opcode() {
  std::printf("  test_invalid_wqe_opcode...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);

  // Create WQE with wrong opcode (RdmaWrite instead of Send)
  SendWqe wrong_wqe;
  wrong_wqe.wr_id = 1008;
  wrong_wqe.opcode = WqeOpcode::RdmaWrite;  // Wrong opcode for send_recv processor
  wrong_wqe.total_length = 64;

  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, wrong_wqe);

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
  setup_qp_for_send(sender_qp, kReceiverQpNum);

  // Create SEND WQE with invalid lkey
  SendWqe send_wqe;
  send_wqe.wr_id = 1009;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  send_wqe.total_length = 64;
  send_wqe.local_lkey = 0xDEADBEEF;  // Invalid lkey

  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);

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
  RdmaQpConfig sender_config;
  sender_config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, sender_config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);

  // Create receiver QP but leave in Reset state (cannot receive)
  RdmaQpConfig receiver_config;
  receiver_config.pd_handle = 1;
  RdmaQueuePair receiver_qp{kReceiverQpNum, receiver_config};
  // Don't transition to RTS - leave in Reset

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(send_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data(64);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1010;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  send_wqe.total_length = 64;
  send_wqe.local_lkey = send_lkey.value();

  // Generate packet
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 1);

  // Try to process at receiver in Reset state
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));

  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::InvalidRequest);

  std::printf("    PASSED\n");
}

// Test middle packet without prior first packet (PSN error because no state established)
void test_middle_packet_no_first() {
  std::printf("  test_middle_packet_no_first...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Set small MTU for multi-packet message
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data (600 bytes - 3 packets at 256 MTU)
  std::vector<std::byte> test_data(600);
  for (std::size_t idx = 0; idx < 600; ++idx) {
    test_data[idx] = static_cast<std::byte>(idx & 0xFF);
  }
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1011;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  send_wqe.total_length = 600;
  send_wqe.local_lkey = send_lkey.value();

  // Generate packets
  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 3);

  // Try to process MIDDLE packet (packets[1]) without first
  // Middle packet has PSN=1, but receiver expects PSN=0, so PSN sequence error
  RdmaPacketParser parser;
  assert(parser.parse(packets[1]));
  assert(parser.bth().opcode == RdmaOpcode::kRcSendMiddle);
  assert(parser.bth().psn == 1);  // Middle packet PSN

  [[maybe_unused]] RecvResult result = processor.process_recv_packet(receiver_qp, parser);
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
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  assert(send_lkey.has_value());

  // Write test data
  std::vector<std::byte> test_data(64);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1012;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 64});
  send_wqe.total_length = 64;
  send_wqe.local_lkey = send_lkey.value();

  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 1);
  assert(processor.stats().send_packets_generated == 1);

  // Reset the processor
  processor.reset();

  // Stats should be cleared
  assert(processor.stats().send_packets_generated == 0);
  assert(processor.stats().sends_started == 0);

  std::printf("    PASSED\n");
}

// Test clear_recv_state
void test_clear_recv_state() {
  std::printf("  test_clear_recv_state...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Set small MTU for multi-packet message
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data (600 bytes - 3 packets)
  std::vector<std::byte> test_data(600);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1013;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  send_wqe.total_length = 600;
  send_wqe.local_lkey = send_lkey.value();

  // Post recv WQE
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2013;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 1024});
  recv_wqe.total_length = 1024;
  assert(receiver_qp.post_recv(recv_wqe));

  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 3);

  // Process first packet to establish recv state
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Clear the recv state
  processor.clear_recv_state(kReceiverQpNum);

  // Now processing middle packet should fail (no recv state)
  assert(parser.parse(packets[1]));
  result = processor.process_recv_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.syndrome == AethSyndrome::InvalidRequest);

  std::printf("    PASSED\n");
}

// Test multi-packet PSN mismatch in middle of message
void test_multi_packet_psn_mismatch() {
  std::printf("  test_multi_packet_psn_mismatch...\n");

  HostMemoryConfig mem_cfg{.size_bytes = kTestMemorySize};
  SimpleHostMemory host_memory{mem_cfg};
  MemoryRegionTable mr_table;

  RdmaQpConfig config;
  config.pd_handle = 1;
  RdmaQueuePair sender_qp{kSenderQpNum, config};
  RdmaQueuePair receiver_qp{kReceiverQpNum, config};
  setup_qp_for_send(sender_qp, kReceiverQpNum);
  setup_qp_for_send(receiver_qp, kSenderQpNum);

  // Set small MTU for multi-packet message
  RdmaQpModifyParams mtu_params;
  mtu_params.path_mtu = 1;  // 256 bytes
  assert(sender_qp.modify(mtu_params));
  assert(receiver_qp.modify(mtu_params));

  // Register memory
  AccessFlags access{.local_read = true, .local_write = true};
  auto send_lkey = mr_table.register_mr(1, 0x1000, 4096, access);
  [[maybe_unused]] auto recv_lkey = mr_table.register_mr(1, 0x2000, 4096, access);
  assert(send_lkey.has_value());
  assert(recv_lkey.has_value());

  // Write test data (600 bytes - 3 packets)
  std::vector<std::byte> test_data(600);
  (void) host_memory.write(0x1000, test_data);

  // Create SEND WQE
  SendWqe send_wqe;
  send_wqe.wr_id = 1014;
  send_wqe.opcode = WqeOpcode::Send;
  send_wqe.sgl.push_back(SglEntry{.address = 0x1000, .length = 600});
  send_wqe.total_length = 600;
  send_wqe.local_lkey = send_lkey.value();

  // Post recv WQE
  RecvWqe recv_wqe;
  recv_wqe.wr_id = 2014;
  recv_wqe.sgl.push_back(SglEntry{.address = 0x2000, .length = 1024});
  recv_wqe.total_length = 1024;
  assert(receiver_qp.post_recv(recv_wqe));

  SendRecvProcessor processor{host_memory, mr_table};
  auto packets = processor.generate_send_packets(sender_qp, send_wqe);
  assert(packets.size() == 3);

  // Process first packet
  RdmaPacketParser parser;
  assert(parser.parse(packets[0]));
  RecvResult result = processor.process_recv_packet(receiver_qp, parser);
  assert(result.success);
  assert(!result.is_message_complete);

  // Skip middle packet and try to process last packet directly
  // This will fail because expected PSN is 1 but last packet has PSN 2
  assert(parser.parse(packets[2]));
  assert(parser.bth().psn == 2);
  result = processor.process_recv_packet(receiver_qp, parser);
  assert(!result.success);
  assert(result.needs_ack);
  assert(result.syndrome == AethSyndrome::PsnSeqError);

  std::printf("    PASSED\n");
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();
  std::printf("Running SEND/RECV tests...\n");

  test_single_packet_send();
  test_multi_packet_send();
  test_send_with_immediate();
  test_zero_length_send();
  test_rnr_nak();
  test_psn_sequence_error();
  test_ack_generation();
  test_scatter_gather_recv();
  test_invalid_wqe_opcode();
  test_sgl_read_failure();
  test_qp_cannot_receive();
  test_middle_packet_no_first();
  test_processor_reset();
  test_clear_recv_state();
  test_multi_packet_psn_mismatch();

  std::printf("All SEND/RECV tests PASSED!\n");
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

#include "nic/rocev2/packet.h"

#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <tracy/Tracy.hpp>

#include "nic/rocev2/types.h"
#include "nic/trace.h"

using namespace nic::rocev2;

static void WaitForTracyConnection();

// =============================================================================
// ICRC Calculator Tests
// =============================================================================

static void test_icrc_calculate() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_icrc_calculate... " << std::flush;

  // Simple test data
  std::array<std::byte, 4> data{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};

  [[maybe_unused]] std::uint32_t crc = IcrcCalculator::calculate(data);

  // CRC should be non-zero for non-trivial data
  assert(crc != 0);

  // Same data should produce same CRC
  [[maybe_unused]] std::uint32_t crc2 = IcrcCalculator::calculate(data);
  assert(crc == crc2);

  std::cout << "PASSED\n";
}

static void test_icrc_verify() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_icrc_verify... " << std::flush;

  // Create data and append CRC
  std::array<std::byte, 4> data{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

  std::uint32_t crc = IcrcCalculator::calculate(data);

  // Build packet with CRC appended (network byte order)
  std::array<std::byte, 8> packet_with_crc{};
  std::copy(data.begin(), data.end(), packet_with_crc.begin());
  packet_with_crc[4] = static_cast<std::byte>((crc >> 24) & 0xFF);
  packet_with_crc[5] = static_cast<std::byte>((crc >> 16) & 0xFF);
  packet_with_crc[6] = static_cast<std::byte>((crc >> 8) & 0xFF);
  packet_with_crc[7] = static_cast<std::byte>(crc & 0xFF);

  assert(IcrcCalculator::verify(packet_with_crc));

  // Corrupt one byte and verify fails
  packet_with_crc[2] = std::byte{0xFF};
  assert(!IcrcCalculator::verify(packet_with_crc));

  std::cout << "PASSED\n";
}

// =============================================================================
// Packet Builder Tests
// =============================================================================

static void test_builder_send_only() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_builder_send_only... " << std::flush;

  RdmaPacketBuilder builder;

  std::array<std::byte, 4> payload{
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};

  auto packet = builder.set_opcode(RdmaOpcode::kRcSendOnly)
                    .set_dest_qp(0x000123)
                    .set_psn(0x000456)
                    .set_ack_request(true)
                    .set_payload(payload)
                    .build();

  // Packet should be: BTH(12) + payload(4) + ICRC(4) = 20 bytes
  assert(packet.size() == 20);

  // Verify BTH opcode
  assert(static_cast<std::uint8_t>(packet[0])
         == static_cast<std::uint8_t>(RdmaOpcode::kRcSendOnly));

  // Verify dest_qp (bytes 5-7)
  [[maybe_unused]] std::uint32_t dest_qp = (static_cast<std::uint32_t>(packet[5]) << 16)
                                           | (static_cast<std::uint32_t>(packet[6]) << 8)
                                           | static_cast<std::uint32_t>(packet[7]);
  assert(dest_qp == 0x000123);

  // Verify PSN (bytes 9-11)
  [[maybe_unused]] std::uint32_t psn = (static_cast<std::uint32_t>(packet[9]) << 16)
                                       | (static_cast<std::uint32_t>(packet[10]) << 8)
                                       | static_cast<std::uint32_t>(packet[11]);
  assert(psn == 0x000456);

  // Verify ICRC
  assert(IcrcCalculator::verify(packet));

  std::cout << "PASSED\n";
}

static void test_builder_write_only_with_reth() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_builder_write_only_with_reth... " << std::flush;

  RdmaPacketBuilder builder;

  std::array<std::byte, 8> payload{};
  for (std::size_t byte_idx = 0; byte_idx < payload.size(); ++byte_idx) {
    payload[byte_idx] = static_cast<std::byte>(byte_idx);
  }

  auto packet = builder.set_opcode(RdmaOpcode::kRcWriteOnly)
                    .set_dest_qp(0x000ABC)
                    .set_psn(0x000789)
                    .set_remote_address(0x0000000012345678ULL)
                    .set_rkey(0xDEADBEEF)
                    .set_dma_length(8)
                    .set_payload(payload)
                    .build();

  // Packet should be: BTH(12) + RETH(16) + payload(8) + ICRC(4) = 40 bytes
  assert(packet.size() == 40);

  // Verify opcode
  assert(static_cast<std::uint8_t>(packet[0])
         == static_cast<std::uint8_t>(RdmaOpcode::kRcWriteOnly));

  // Verify RETH starts at offset 12
  // Virtual address (bytes 12-19)
  std::uint64_t va = 0;
  for (int byte_idx = 0; byte_idx < 8; ++byte_idx) {
    va = (va << 8) | static_cast<std::uint64_t>(packet[12 + byte_idx]);
  }
  assert(va == 0x0000000012345678ULL);

  // R_Key (bytes 20-23)
  [[maybe_unused]] std::uint32_t rkey = (static_cast<std::uint32_t>(packet[20]) << 24)
                                        | (static_cast<std::uint32_t>(packet[21]) << 16)
                                        | (static_cast<std::uint32_t>(packet[22]) << 8)
                                        | static_cast<std::uint32_t>(packet[23]);
  assert(rkey == 0xDEADBEEF);

  // DMA length (bytes 24-27)
  [[maybe_unused]] std::uint32_t dma_len = (static_cast<std::uint32_t>(packet[24]) << 24)
                                           | (static_cast<std::uint32_t>(packet[25]) << 16)
                                           | (static_cast<std::uint32_t>(packet[26]) << 8)
                                           | static_cast<std::uint32_t>(packet[27]);
  assert(dma_len == 8);

  // Verify ICRC
  assert(IcrcCalculator::verify(packet));

  std::cout << "PASSED\n";
}

static void test_builder_ack_with_aeth() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_builder_ack_with_aeth... " << std::flush;

  RdmaPacketBuilder builder;

  auto packet = builder.set_opcode(RdmaOpcode::kRcAck)
                    .set_dest_qp(0x000100)
                    .set_psn(0x000200)
                    .set_syndrome(AethSyndrome::Ack)
                    .set_msn(0x000300)
                    .build();

  // Packet should be: BTH(12) + AETH(4) + ICRC(4) = 20 bytes
  assert(packet.size() == 20);

  // Verify AETH syndrome at offset 12
  assert(static_cast<std::uint8_t>(packet[12]) == static_cast<std::uint8_t>(AethSyndrome::Ack));

  // Verify MSN (bytes 13-15)
  [[maybe_unused]] std::uint32_t msn = (static_cast<std::uint32_t>(packet[13]) << 16)
                                       | (static_cast<std::uint32_t>(packet[14]) << 8)
                                       | static_cast<std::uint32_t>(packet[15]);
  assert(msn == 0x000300);

  // Verify ICRC
  assert(IcrcCalculator::verify(packet));

  std::cout << "PASSED\n";
}

static void test_builder_send_with_immediate() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_builder_send_with_immediate... " << std::flush;

  RdmaPacketBuilder builder;

  std::array<std::byte, 4> payload{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

  auto packet = builder.set_opcode(RdmaOpcode::kRcSendOnlyImm)
                    .set_dest_qp(0x000001)
                    .set_psn(0x000002)
                    .set_immediate(0xCAFEBABE)
                    .set_payload(payload)
                    .build();

  // Packet should be: BTH(12) + IMM(4) + payload(4) + ICRC(4) = 24 bytes
  assert(packet.size() == 24);

  // Verify immediate data at offset 12
  [[maybe_unused]] std::uint32_t imm = (static_cast<std::uint32_t>(packet[12]) << 24)
                                       | (static_cast<std::uint32_t>(packet[13]) << 16)
                                       | (static_cast<std::uint32_t>(packet[14]) << 8)
                                       | static_cast<std::uint32_t>(packet[15]);
  assert(imm == 0xCAFEBABE);

  // Verify ICRC
  assert(IcrcCalculator::verify(packet));

  std::cout << "PASSED\n";
}

static void test_builder_reset() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_builder_reset... " << std::flush;

  RdmaPacketBuilder builder;

  builder.set_dest_qp(0x123456).set_psn(0xABCDEF);

  builder.reset();

  auto packet = builder.set_opcode(RdmaOpcode::kRcSendOnly).build();

  // After reset, dest_qp should be 0
  [[maybe_unused]] std::uint32_t dest_qp = (static_cast<std::uint32_t>(packet[5]) << 16)
                                           | (static_cast<std::uint32_t>(packet[6]) << 8)
                                           | static_cast<std::uint32_t>(packet[7]);
  assert(dest_qp == 0);

  std::cout << "PASSED\n";
}

// =============================================================================
// Packet Parser Tests
// =============================================================================

static void test_parser_send_only() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_parser_send_only... " << std::flush;

  // Build a packet
  RdmaPacketBuilder builder;
  std::array<std::byte, 4> payload{
      std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};

  auto packet = builder.set_opcode(RdmaOpcode::kRcSendOnly)
                    .set_dest_qp(0x000111)
                    .set_psn(0x000222)
                    .set_ack_request(true)
                    .set_solicited_event(true)
                    .set_payload(payload)
                    .build();

  // Parse it
  RdmaPacketParser parser;
  assert(parser.parse(packet));

  // Verify parsed fields
  assert(parser.bth().opcode == RdmaOpcode::kRcSendOnly);
  assert(parser.bth().dest_qp == 0x000111);
  assert(parser.bth().psn == 0x000222);
  assert(parser.bth().ack_request == true);
  assert(parser.bth().solicited_event == true);

  // Verify payload
  assert(parser.payload().size() == 4);
  assert(parser.payload()[0] == std::byte{0xAA});
  assert(parser.payload()[3] == std::byte{0xDD});

  // Verify no extended headers
  assert(!parser.has_reth());
  assert(!parser.has_aeth());
  assert(!parser.has_immediate());

  // Verify ICRC
  assert(parser.verify_icrc(packet));

  std::cout << "PASSED\n";
}

static void test_parser_write_only() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_parser_write_only... " << std::flush;

  RdmaPacketBuilder builder;
  std::array<std::byte, 8> payload{};

  auto packet = builder.set_opcode(RdmaOpcode::kRcWriteOnly)
                    .set_dest_qp(0x000333)
                    .set_psn(0x000444)
                    .set_remote_address(0xFEDCBA9876543210ULL)
                    .set_rkey(0x12345678)
                    .set_dma_length(100)
                    .set_payload(payload)
                    .build();

  RdmaPacketParser parser;
  assert(parser.parse(packet));

  assert(parser.bth().opcode == RdmaOpcode::kRcWriteOnly);
  assert(parser.has_reth());
  assert(parser.reth().virtual_address == 0xFEDCBA9876543210ULL);
  assert(parser.reth().rkey == 0x12345678);
  assert(parser.reth().dma_length == 100);

  std::cout << "PASSED\n";
}

static void test_parser_ack() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_parser_ack... " << std::flush;

  RdmaPacketBuilder builder;

  auto packet = builder.set_opcode(RdmaOpcode::kRcAck)
                    .set_dest_qp(0x000555)
                    .set_psn(0x000666)
                    .set_syndrome(AethSyndrome::PsnSeqError)
                    .set_msn(0x000777)
                    .build();

  RdmaPacketParser parser;
  assert(parser.parse(packet));

  assert(parser.bth().opcode == RdmaOpcode::kRcAck);
  assert(parser.has_aeth());
  assert(parser.aeth().syndrome == AethSyndrome::PsnSeqError);
  assert(parser.aeth().msn == 0x000777);

  std::cout << "PASSED\n";
}

static void test_parser_send_with_immediate() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_parser_send_with_immediate... " << std::flush;

  RdmaPacketBuilder builder;
  std::array<std::byte, 2> payload{std::byte{0x55}, std::byte{0x66}};

  auto packet = builder.set_opcode(RdmaOpcode::kRcSendOnlyImm)
                    .set_dest_qp(0x000888)
                    .set_psn(0x000999)
                    .set_immediate(0xDEADC0DE)
                    .set_payload(payload)
                    .build();

  RdmaPacketParser parser;
  assert(parser.parse(packet));

  assert(parser.bth().opcode == RdmaOpcode::kRcSendOnlyImm);
  assert(parser.has_immediate());
  assert(parser.immediate() == 0xDEADC0DE);
  assert(parser.payload().size() == 2);

  std::cout << "PASSED\n";
}

static void test_parser_invalid_short_packet() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_parser_invalid_short_packet... " << std::flush;

  // Too short for BTH + ICRC
  [[maybe_unused]] std::array<std::byte, 10> short_packet{};

  RdmaPacketParser parser;
  assert(!parser.parse(short_packet));

  std::cout << "PASSED\n";
}

// =============================================================================
// Opcode Helper Tests
// =============================================================================

static void test_opcode_helpers() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_opcode_helpers... " << std::flush;

  // First opcodes
  assert(opcode_is_first(RdmaOpcode::kRcSendFirst));
  assert(opcode_is_first(RdmaOpcode::kRcWriteFirst));
  assert(!opcode_is_first(RdmaOpcode::kRcSendOnly));

  // Middle opcodes
  assert(opcode_is_middle(RdmaOpcode::kRcSendMiddle));
  assert(opcode_is_middle(RdmaOpcode::kRcWriteMiddle));
  assert(!opcode_is_middle(RdmaOpcode::kRcSendLast));

  // Last opcodes
  assert(opcode_is_last(RdmaOpcode::kRcSendLast));
  assert(opcode_is_last(RdmaOpcode::kRcWriteLast));
  assert(!opcode_is_last(RdmaOpcode::kRcSendOnly));

  // Only opcodes
  assert(opcode_is_only(RdmaOpcode::kRcSendOnly));
  assert(opcode_is_only(RdmaOpcode::kRcWriteOnly));
  assert(!opcode_is_only(RdmaOpcode::kRcSendFirst));

  // Has payload
  assert(opcode_has_payload(RdmaOpcode::kRcSendOnly));
  assert(!opcode_has_payload(RdmaOpcode::kRcAck));
  assert(!opcode_has_payload(RdmaOpcode::kRcReadRequest));

  // Read response
  assert(opcode_is_read_response(RdmaOpcode::kRcReadResponseOnly));
  assert(opcode_is_read_response(RdmaOpcode::kRcReadResponseFirst));
  assert(!opcode_is_read_response(RdmaOpcode::kRcSendOnly));

  std::cout << "PASSED\n";
}

// =============================================================================
// Round-trip Tests
// =============================================================================

static void test_roundtrip_send() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_roundtrip_send... " << std::flush;

  // Build packet with specific values
  RdmaPacketBuilder builder;
  std::vector<std::byte> payload(64);
  for (std::size_t byte_idx = 0; byte_idx < payload.size(); ++byte_idx) {
    payload[byte_idx] = static_cast<std::byte>(byte_idx);
  }

  auto packet = builder.set_opcode(RdmaOpcode::kRcSendOnly)
                    .set_dest_qp(0x123456)
                    .set_psn(0xABCDEF)
                    .set_partition_key(0x7FFF)
                    .set_ack_request(true)
                    .set_solicited_event(true)
                    .set_pad_count(2)
                    .set_payload(payload)
                    .build();

  // Parse and verify all fields match
  RdmaPacketParser parser;
  assert(parser.parse(packet));

  assert(parser.bth().opcode == RdmaOpcode::kRcSendOnly);
  assert(parser.bth().dest_qp == 0x123456);
  assert(parser.bth().psn == 0xABCDEF);
  assert(parser.bth().partition_key == 0x7FFF);
  assert(parser.bth().ack_request == true);
  assert(parser.bth().solicited_event == true);
  assert(parser.bth().pad_count == 2);

  assert(parser.payload().size() == 64);
  for (std::size_t byte_idx = 0; byte_idx < 64; ++byte_idx) {
    assert(parser.payload()[byte_idx] == static_cast<std::byte>(byte_idx));
  }

  assert(parser.verify_icrc(packet));

  std::cout << "PASSED\n";
}

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  std::cout << "\n=== RoCEv2 Packet Tests ===\n\n";

  // ICRC tests
  test_icrc_calculate();
  test_icrc_verify();

  // Builder tests
  test_builder_send_only();
  test_builder_write_only_with_reth();
  test_builder_ack_with_aeth();
  test_builder_send_with_immediate();
  test_builder_reset();

  // Parser tests
  test_parser_send_only();
  test_parser_write_only();
  test_parser_ack();
  test_parser_send_with_immediate();
  test_parser_invalid_short_packet();

  // Opcode helper tests
  test_opcode_helpers();

  // Round-trip tests
  test_roundtrip_send();

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

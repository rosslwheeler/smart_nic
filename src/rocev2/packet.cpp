#include "nic/rocev2/packet.h"

#include <bit>
#include <cstring>

#include "nic/rocev2/formats.h"

namespace nic::rocev2 {

// =============================================================================
// CRC-32C Table (Castagnoli polynomial 0x1EDC6F41)
// =============================================================================

const std::array<std::uint32_t, 256> IcrcCalculator::kCrc32cTable = []() {
  std::array<std::uint32_t, 256> table{};
  constexpr std::uint32_t kPolynomial = 0x82F63B78;  // Reflected polynomial

  for (std::uint32_t table_idx = 0; table_idx < 256; ++table_idx) {
    std::uint32_t crc = table_idx;
    for (int bit_idx = 0; bit_idx < 8; ++bit_idx) {
      if (crc & 1) {
        crc = (crc >> 1) ^ kPolynomial;
      } else {
        crc >>= 1;
      }
    }
    table[table_idx] = crc;
  }
  return table;
}();

// =============================================================================
// IcrcCalculator
// =============================================================================

std::uint32_t IcrcCalculator::update_crc(std::uint32_t crc, std::byte byte) {
  std::uint8_t index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint8_t>(byte));
  return kCrc32cTable[index] ^ (crc >> 8);
}

std::uint32_t IcrcCalculator::calculate(std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);

  std::uint32_t crc = 0xFFFFFFFF;

  // For ICRC calculation, certain fields in BTH are masked:
  // - First 4 bytes of BTH: opcode(8), SE(1), M(1), pad(2), TVer(4), P_Key(16) -> mask first byte
  // - Reserved fields in BTH
  // Actually, ICRC masks: LRH (if present), variant fields in GRH/BTH
  // For simplicity in RoCEv2, we calculate over the entire packet as-is
  // (proper masking would require more context about IP/UDP headers)

  for (std::size_t byte_idx = 0; byte_idx < data.size(); ++byte_idx) {
    crc = update_crc(crc, data[byte_idx]);
  }

  return crc ^ 0xFFFFFFFF;
}

bool IcrcCalculator::verify(std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);

  if (data.size() < kIcrcSize) {
    return false;
  }

  // Calculate CRC over data excluding the ICRC field
  std::span<const std::byte> payload = data.subspan(0, data.size() - kIcrcSize);
  std::uint32_t calculated = calculate(payload);

  // Extract stored ICRC (last 4 bytes, network byte order)
  std::span<const std::byte> icrc_bytes = data.subspan(data.size() - kIcrcSize, kIcrcSize);
  bit_fields::NetworkBitReader reader(icrc_bytes);
  std::uint32_t stored = reader.read_aligned<std::uint32_t>();

  return calculated == stored;
}

// =============================================================================
// RdmaPacketBuilder
// =============================================================================

RdmaPacketBuilder::RdmaPacketBuilder() {
  NIC_TRACE_SCOPED(__func__);
}

RdmaPacketBuilder& RdmaPacketBuilder::set_opcode(RdmaOpcode opcode) {
  opcode_ = opcode;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_dest_qp(std::uint32_t dest_qp) {
  dest_qp_ = dest_qp & 0x00FFFFFF;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_psn(std::uint32_t psn) {
  psn_ = psn & kMaxPsn;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_partition_key(std::uint16_t pkey) {
  partition_key_ = pkey;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_ack_request(bool ack_req) {
  ack_request_ = ack_req;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_solicited_event(bool se) {
  solicited_event_ = se;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_pad_count(std::uint8_t pad) {
  pad_count_ = pad & 0x03;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_fecn(bool fecn) {
  fecn_ = fecn;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_becn(bool becn) {
  becn_ = becn;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_remote_address(std::uint64_t va) {
  remote_address_ = va;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_rkey(std::uint32_t rkey) {
  rkey_ = rkey;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_dma_length(std::uint32_t length) {
  dma_length_ = length;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_syndrome(AethSyndrome syndrome) {
  syndrome_ = syndrome;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_msn(std::uint32_t msn) {
  msn_ = msn & 0x00FFFFFF;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_immediate(std::uint32_t imm) {
  immediate_ = imm;
  has_immediate_ = true;
  return *this;
}

RdmaPacketBuilder& RdmaPacketBuilder::set_payload(std::span<const std::byte> data) {
  payload_.assign(data.begin(), data.end());
  return *this;
}

void RdmaPacketBuilder::reset() {
  NIC_TRACE_SCOPED(__func__);
  *this = RdmaPacketBuilder{};
}

bool RdmaPacketBuilder::needs_reth() const noexcept {
  // RETH is present in first/only WRITE and READ requests
  switch (opcode_) {
    case RdmaOpcode::kRcWriteFirst:
    case RdmaOpcode::kRcWriteOnly:
    case RdmaOpcode::kRcWriteOnlyImm:
    case RdmaOpcode::kRcReadRequest:
      return true;
    default:
      return false;
  }
}

bool RdmaPacketBuilder::needs_aeth() const noexcept {
  // AETH is present in ACK and read responses
  switch (opcode_) {
    case RdmaOpcode::kRcAck:
    case RdmaOpcode::kRcReadResponseFirst:
    case RdmaOpcode::kRcReadResponseLast:
    case RdmaOpcode::kRcReadResponseOnly:
      return true;
    default:
      return false;
  }
}

bool RdmaPacketBuilder::has_immediate_variant() const noexcept {
  switch (opcode_) {
    case RdmaOpcode::kRcSendLastImm:
    case RdmaOpcode::kRcSendOnlyImm:
    case RdmaOpcode::kRcWriteLastImm:
    case RdmaOpcode::kRcWriteOnlyImm:
      return true;
    default:
      return false;
  }
}

void RdmaPacketBuilder::write_bth(std::span<std::byte> buffer) const {
  NIC_TRACE_SCOPED(__func__);

  // Build the fecn_becn_reserved byte: FECN(1) | BECN(1) | reserved(6)
  std::uint8_t fecn_becn_reserved = 0;
  if (fecn_) {
    fecn_becn_reserved |= 0x80;
  }
  if (becn_) {
    fecn_becn_reserved |= 0x40;
  }

  bit_fields::NetworkBitWriter writer(buffer);
  writer.serialize(kBthFormat,
                   static_cast<std::uint64_t>(opcode_),             // opcode
                   solicited_event_ ? 1ULL : 0ULL,                  // solicited_event
                   0ULL,                                            // mig_req
                   static_cast<std::uint64_t>(pad_count_ & 0x03),   // pad_count
                   0ULL,                                            // transport_version
                   static_cast<std::uint64_t>(partition_key_),      // partition_key
                   static_cast<std::uint64_t>(fecn_becn_reserved),  // fecn_becn_reserved
                   static_cast<std::uint64_t>(dest_qp_),            // dest_qp
                   ack_request_ ? 1ULL : 0ULL,                      // ack_request
                   0ULL,                                            // _reserved_psn
                   static_cast<std::uint64_t>(psn_)                 // psn
  );
}

void RdmaPacketBuilder::write_reth(std::span<std::byte> buffer) const {
  NIC_TRACE_SCOPED(__func__);

  bit_fields::NetworkBitWriter writer(buffer);
  writer.serialize(kRethFormat,
                   static_cast<std::uint64_t>(remote_address_),  // virtual_address
                   static_cast<std::uint64_t>(rkey_),            // rkey
                   static_cast<std::uint64_t>(dma_length_)       // dma_length
  );
}

void RdmaPacketBuilder::write_aeth(std::span<std::byte> buffer) const {
  NIC_TRACE_SCOPED(__func__);

  bit_fields::NetworkBitWriter writer(buffer);
  writer.serialize(kAethFormat,
                   static_cast<std::uint64_t>(syndrome_),  // syndrome
                   static_cast<std::uint64_t>(msn_)        // msn
  );
}

void RdmaPacketBuilder::write_immediate(std::span<std::byte> buffer) const {
  NIC_TRACE_SCOPED(__func__);

  bit_fields::NetworkBitWriter writer(buffer);
  writer.serialize(kImmediateFormat,
                   static_cast<std::uint64_t>(immediate_)  // immediate
  );
}

std::vector<std::byte> RdmaPacketBuilder::build() {
  NIC_TRACE_SCOPED(__func__);

  // Calculate total size
  std::size_t total_size = kBthSize;
  if (needs_reth()) {
    total_size += kRethSize;
  }
  if (needs_aeth()) {
    total_size += kAethSize;
  }
  if (has_immediate_ && has_immediate_variant()) {
    total_size += kImmSize;
  }
  total_size += payload_.size();
  total_size += kIcrcSize;

  std::vector<std::byte> packet(total_size);
  std::size_t offset = 0;

  // Write BTH
  write_bth(std::span<std::byte>(packet).subspan(offset, kBthSize));
  offset += kBthSize;

  // Write RETH if needed
  if (needs_reth()) {
    write_reth(std::span<std::byte>(packet).subspan(offset, kRethSize));
    offset += kRethSize;
  }

  // Write AETH if needed
  if (needs_aeth()) {
    write_aeth(std::span<std::byte>(packet).subspan(offset, kAethSize));
    offset += kAethSize;
  }

  // Write immediate data if present
  if (has_immediate_ && has_immediate_variant()) {
    write_immediate(std::span<std::byte>(packet).subspan(offset, kImmSize));
    offset += kImmSize;
  }

  // Write payload
  if (!payload_.empty()) {
    std::copy(
        payload_.begin(), payload_.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += payload_.size();
  }

  // Calculate and write ICRC
  std::span<const std::byte> crc_data(packet.data(), offset);
  std::uint32_t icrc = IcrcCalculator::calculate(crc_data);
  bit_fields::NetworkBitWriter writer(std::span<std::byte>(packet).subspan(offset, kIcrcSize));
  writer.write_aligned<std::uint32_t>(icrc);

  return packet;
}

// =============================================================================
// RdmaPacketParser
// =============================================================================

bool RdmaPacketParser::parse(std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);

  if (data.size() < kBthSize + kIcrcSize) {
    return false;
  }

  if (!parse_bth(data)) {
    return false;
  }

  determine_headers();

  std::size_t offset = kBthSize;

  // Parse RETH if expected
  if (has_reth_) {
    if (offset + kRethSize > data.size() - kIcrcSize) {
      return false;
    }
    bit_fields::NetworkBitReader reth_reader(data.subspan(offset, kRethSize));
    auto reth_parsed = reth_reader.deserialize(kRethFormat);
    reth_.virtual_address = reth_parsed.get("virtual_address");
    reth_.rkey = static_cast<std::uint32_t>(reth_parsed.get("rkey"));
    reth_.dma_length = static_cast<std::uint32_t>(reth_parsed.get("dma_length"));
    offset += kRethSize;
  }

  // Parse AETH if expected
  if (has_aeth_) {
    if (offset + kAethSize > data.size() - kIcrcSize) {
      return false;
    }
    bit_fields::NetworkBitReader aeth_reader(data.subspan(offset, kAethSize));
    auto aeth_parsed = aeth_reader.deserialize(kAethFormat);
    aeth_.syndrome = static_cast<AethSyndrome>(aeth_parsed.get("syndrome"));
    aeth_.msn = static_cast<std::uint32_t>(aeth_parsed.get("msn"));
    offset += kAethSize;
  }

  // Parse immediate data if expected
  if (has_immediate_) {
    if (offset + kImmSize > data.size() - kIcrcSize) {
      return false;
    }
    bit_fields::NetworkBitReader imm_reader(data.subspan(offset, kImmSize));
    auto imm_parsed = imm_reader.deserialize(kImmediateFormat);
    immediate_ = static_cast<std::uint32_t>(imm_parsed.get("immediate"));
    offset += kImmSize;
  }

  // Remaining data (excluding ICRC) is payload
  std::size_t payload_end = data.size() - kIcrcSize;
  if (offset < payload_end) {
    payload_ = data.subspan(offset, payload_end - offset);
  } else {
    payload_ = {};
  }

  return true;
}

bool RdmaPacketParser::parse_bth(std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);

  if (data.size() < kBthSize) {
    return false;
  }

  bit_fields::NetworkBitReader reader(data);
  auto parsed = reader.deserialize(kBthFormat);

  bth_.opcode = static_cast<RdmaOpcode>(parsed.get("opcode"));
  bth_.solicited_event = parsed.get("solicited_event") != 0;
  bth_.mig_req = parsed.get("mig_req") != 0;
  bth_.pad_count = static_cast<std::uint8_t>(parsed.get("pad_count"));
  bth_.transport_version = static_cast<std::uint8_t>(parsed.get("transport_version"));
  bth_.partition_key = static_cast<std::uint16_t>(parsed.get("partition_key"));

  // Extract FECN and BECN from the combined byte
  std::uint8_t fecn_becn_reserved = static_cast<std::uint8_t>(parsed.get("fecn_becn_reserved"));
  bth_.fecn = (fecn_becn_reserved & 0x80) != 0;
  bth_.becn = (fecn_becn_reserved & 0x40) != 0;

  bth_.dest_qp = static_cast<std::uint32_t>(parsed.get("dest_qp"));
  bth_.ack_request = parsed.get("ack_request") != 0;
  bth_.psn = static_cast<std::uint32_t>(parsed.get("psn"));

  return true;
}

void RdmaPacketParser::determine_headers() {
  NIC_TRACE_SCOPED(__func__);

  has_reth_ = false;
  has_aeth_ = false;
  has_immediate_ = false;

  switch (bth_.opcode) {
    case RdmaOpcode::kRcWriteFirst:
    case RdmaOpcode::kRcWriteOnly:
    case RdmaOpcode::kRcReadRequest:
      has_reth_ = true;
      break;

    case RdmaOpcode::kRcWriteOnlyImm:
      has_reth_ = true;
      has_immediate_ = true;
      break;

    case RdmaOpcode::kRcSendLastImm:
    case RdmaOpcode::kRcSendOnlyImm:
    case RdmaOpcode::kRcWriteLastImm:
      has_immediate_ = true;
      break;

    case RdmaOpcode::kRcAck:
      has_aeth_ = true;
      break;

    case RdmaOpcode::kRcReadResponseFirst:
    case RdmaOpcode::kRcReadResponseLast:
    case RdmaOpcode::kRcReadResponseOnly:
      has_aeth_ = true;
      break;

    default:
      break;
  }
}

bool RdmaPacketParser::verify_icrc(std::span<const std::byte> data) const {
  NIC_TRACE_SCOPED(__func__);
  return IcrcCalculator::verify(data);
}

// =============================================================================
// Opcode helpers
// =============================================================================

bool opcode_is_first(RdmaOpcode op) noexcept {
  switch (op) {
    case RdmaOpcode::kRcSendFirst:
    case RdmaOpcode::kRcWriteFirst:
    case RdmaOpcode::kRcReadResponseFirst:
      return true;
    default:
      return false;
  }
}

bool opcode_is_middle(RdmaOpcode op) noexcept {
  switch (op) {
    case RdmaOpcode::kRcSendMiddle:
    case RdmaOpcode::kRcWriteMiddle:
    case RdmaOpcode::kRcReadResponseMiddle:
      return true;
    default:
      return false;
  }
}

bool opcode_is_last(RdmaOpcode op) noexcept {
  switch (op) {
    case RdmaOpcode::kRcSendLast:
    case RdmaOpcode::kRcSendLastImm:
    case RdmaOpcode::kRcWriteLast:
    case RdmaOpcode::kRcWriteLastImm:
    case RdmaOpcode::kRcReadResponseLast:
      return true;
    default:
      return false;
  }
}

bool opcode_is_only(RdmaOpcode op) noexcept {
  switch (op) {
    case RdmaOpcode::kRcSendOnly:
    case RdmaOpcode::kRcSendOnlyImm:
    case RdmaOpcode::kRcWriteOnly:
    case RdmaOpcode::kRcWriteOnlyImm:
    case RdmaOpcode::kRcReadResponseOnly:
      return true;
    default:
      return false;
  }
}

bool opcode_has_payload(RdmaOpcode op) noexcept {
  switch (op) {
    case RdmaOpcode::kRcAck:
    case RdmaOpcode::kRcReadRequest:
    case RdmaOpcode::kCnp:
      return false;
    default:
      return true;
  }
}

bool opcode_is_read_response(RdmaOpcode op) noexcept {
  switch (op) {
    case RdmaOpcode::kRcReadResponseFirst:
    case RdmaOpcode::kRcReadResponseMiddle:
    case RdmaOpcode::kRcReadResponseLast:
    case RdmaOpcode::kRcReadResponseOnly:
      return true;
    default:
      return false;
  }
}

}  // namespace nic::rocev2

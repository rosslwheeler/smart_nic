#pragma once

/// @file packet.h
/// @brief RoCEv2 packet building, parsing, and ICRC calculation.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "nic/rocev2/types.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Size constants for RoCEv2 headers.
inline constexpr std::size_t kBthSize = 12;   // Base Transport Header
inline constexpr std::size_t kRethSize = 16;  // RDMA Extended Transport Header
inline constexpr std::size_t kAethSize = 4;   // ACK Extended Transport Header
inline constexpr std::size_t kImmSize = 4;    // Immediate Data
inline constexpr std::size_t kIcrcSize = 4;   // Invariant CRC

/// Parsed Base Transport Header.
struct BthFields {
  RdmaOpcode opcode;
  bool solicited_event;
  bool mig_req;
  std::uint8_t pad_count;
  std::uint8_t transport_version;
  std::uint16_t partition_key;
  bool fecn;
  bool becn;
  std::uint32_t dest_qp;
  bool ack_request;
  std::uint32_t psn;
};

/// Parsed RDMA Extended Transport Header.
struct RethFields {
  std::uint64_t virtual_address;
  std::uint32_t rkey;
  std::uint32_t dma_length;
};

/// Parsed ACK Extended Transport Header.
struct AethFields {
  AethSyndrome syndrome;
  std::uint32_t msn;
};

/// RoCEv2 packet builder - constructs packets with proper headers.
class RdmaPacketBuilder {
public:
  RdmaPacketBuilder();

  /// Set BTH fields.
  RdmaPacketBuilder& set_opcode(RdmaOpcode opcode);
  RdmaPacketBuilder& set_dest_qp(std::uint32_t dest_qp);
  RdmaPacketBuilder& set_psn(std::uint32_t psn);
  RdmaPacketBuilder& set_partition_key(std::uint16_t pkey);
  RdmaPacketBuilder& set_ack_request(bool ack_req);
  RdmaPacketBuilder& set_solicited_event(bool se);
  RdmaPacketBuilder& set_pad_count(std::uint8_t pad);
  RdmaPacketBuilder& set_fecn(bool fecn);
  RdmaPacketBuilder& set_becn(bool becn);

  /// Set RETH fields (for WRITE/READ).
  RdmaPacketBuilder& set_remote_address(std::uint64_t va);
  RdmaPacketBuilder& set_rkey(std::uint32_t rkey);
  RdmaPacketBuilder& set_dma_length(std::uint32_t length);

  /// Set AETH fields (for ACK/NAK).
  RdmaPacketBuilder& set_syndrome(AethSyndrome syndrome);
  RdmaPacketBuilder& set_msn(std::uint32_t msn);

  /// Set immediate data.
  RdmaPacketBuilder& set_immediate(std::uint32_t imm);

  /// Set payload data.
  RdmaPacketBuilder& set_payload(std::span<const std::byte> data);

  /// Build the packet with all headers and ICRC.
  /// @return Complete RoCEv2 packet (BTH + optional headers + payload + ICRC).
  [[nodiscard]] std::vector<std::byte> build();

  /// Reset builder to initial state.
  void reset();

private:
  RdmaOpcode opcode_{RdmaOpcode::kRcSendOnly};
  std::uint32_t dest_qp_{0};
  std::uint32_t psn_{0};
  std::uint16_t partition_key_{kDefaultPkey};
  bool ack_request_{true};
  bool solicited_event_{false};
  std::uint8_t pad_count_{0};
  bool fecn_{false};
  bool becn_{false};

  // RETH fields
  std::uint64_t remote_address_{0};
  std::uint32_t rkey_{0};
  std::uint32_t dma_length_{0};

  // AETH fields
  AethSyndrome syndrome_{AethSyndrome::Ack};
  std::uint32_t msn_{0};

  // Immediate data
  std::uint32_t immediate_{0};
  bool has_immediate_{false};

  // Payload
  std::vector<std::byte> payload_;

  /// Check if opcode requires RETH header.
  [[nodiscard]] bool needs_reth() const noexcept;

  /// Check if opcode requires AETH header.
  [[nodiscard]] bool needs_aeth() const noexcept;

  /// Check if opcode has immediate data variant.
  [[nodiscard]] bool has_immediate_variant() const noexcept;

  /// Write BTH to buffer.
  void write_bth(std::span<std::byte> buffer) const;

  /// Write RETH to buffer.
  void write_reth(std::span<std::byte> buffer) const;

  /// Write AETH to buffer.
  void write_aeth(std::span<std::byte> buffer) const;

  /// Write immediate data to buffer.
  void write_immediate(std::span<std::byte> buffer) const;
};

/// RoCEv2 packet parser - extracts fields from received packets.
class RdmaPacketParser {
public:
  /// Parse a RoCEv2 packet (UDP payload).
  /// @param data Packet data starting from BTH.
  /// @return true if packet is valid.
  bool parse(std::span<const std::byte> data);

  /// Get parsed BTH fields.
  [[nodiscard]] const BthFields& bth() const noexcept { return bth_; }

  /// Get parsed RETH fields (valid only if has_reth() is true).
  [[nodiscard]] const RethFields& reth() const noexcept { return reth_; }

  /// Get parsed AETH fields (valid only if has_aeth() is true).
  [[nodiscard]] const AethFields& aeth() const noexcept { return aeth_; }

  /// Get immediate data (valid only if has_immediate() is true).
  [[nodiscard]] std::uint32_t immediate() const noexcept { return immediate_; }

  /// Get payload span (excluding headers and ICRC).
  [[nodiscard]] std::span<const std::byte> payload() const noexcept { return payload_; }

  /// Check if packet has RETH header.
  [[nodiscard]] bool has_reth() const noexcept { return has_reth_; }

  /// Check if packet has AETH header.
  [[nodiscard]] bool has_aeth() const noexcept { return has_aeth_; }

  /// Check if packet has immediate data.
  [[nodiscard]] bool has_immediate() const noexcept { return has_immediate_; }

  /// Verify ICRC of the packet.
  /// @param data Full packet data including ICRC.
  /// @return true if ICRC is valid.
  [[nodiscard]] bool verify_icrc(std::span<const std::byte> data) const;

private:
  BthFields bth_{};
  RethFields reth_{};
  AethFields aeth_{};
  std::uint32_t immediate_{0};
  std::span<const std::byte> payload_;
  bool has_reth_{false};
  bool has_aeth_{false};
  bool has_immediate_{false};

  /// Parse BTH from data.
  bool parse_bth(std::span<const std::byte> data);

  /// Determine header layout from opcode.
  void determine_headers();
};

/// Calculate ICRC (Invariant CRC) for RoCEv2 packets.
/// Uses CRC-32C (Castagnoli) polynomial: 0x1EDC6F41
class IcrcCalculator {
public:
  /// Calculate ICRC for a packet.
  /// @param data Packet data (BTH through payload, excluding ICRC).
  /// @return ICRC value (network byte order).
  [[nodiscard]] static std::uint32_t calculate(std::span<const std::byte> data);

  /// Verify ICRC of a complete packet.
  /// @param data Complete packet including ICRC.
  /// @return true if ICRC is valid.
  [[nodiscard]] static bool verify(std::span<const std::byte> data);

private:
  /// CRC-32C lookup table.
  static const std::array<std::uint32_t, 256> kCrc32cTable;

  /// Update CRC with a byte.
  [[nodiscard]] static std::uint32_t update_crc(std::uint32_t crc, std::byte byte);
};

/// Helper to determine opcode properties.
[[nodiscard]] bool opcode_is_first(RdmaOpcode op) noexcept;
[[nodiscard]] bool opcode_is_middle(RdmaOpcode op) noexcept;
[[nodiscard]] bool opcode_is_last(RdmaOpcode op) noexcept;
[[nodiscard]] bool opcode_is_only(RdmaOpcode op) noexcept;
[[nodiscard]] bool opcode_has_payload(RdmaOpcode op) noexcept;
[[nodiscard]] bool opcode_is_read_response(RdmaOpcode op) noexcept;

}  // namespace nic::rocev2

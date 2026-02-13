#pragma once

/// @file formats.h
/// @brief RoCEv2 packet format definitions for use with bit_fields library.

#include <bit_fields/bit_fields.h>

namespace nic::rocev2 {

// =============================================================================
// RoCEv2 / InfiniBand Transport Headers
// =============================================================================

/// Base Transport Header (BTH) - 96 bits / 12 bytes
///
/// Format:
///   Byte 0:       opcode (8 bits)
///   Byte 1:       SE(1) | M(1) | pad_count(2) | transport_version(4)
///   Bytes 2-3:    partition_key (16 bits)
///   Byte 4:       FECN(1) | BECN(1) | reserved(6)
///   Bytes 5-7:    dest_qp (24 bits)
///   Byte 8:       A(1) | reserved(7)
///   Bytes 9-11:   psn (24 bits)
inline constexpr bit_fields::PacketFormat<11> kBthFormat{{{
    bit_fields::FieldDef{"opcode", 8},
    bit_fields::FieldDef{"solicited_event", 1},
    bit_fields::FieldDef{"mig_req", 1},
    bit_fields::FieldDef{"pad_count", 2},
    bit_fields::FieldDef{"transport_version", 4},
    bit_fields::FieldDef{"partition_key", 16},
    bit_fields::FieldDef{"fecn_becn_reserved", 8},  // fecn(1)|becn(1)|reserved(6)
    bit_fields::FieldDef{"dest_qp", 24},
    bit_fields::FieldDef{"ack_request", 1},
    bit_fields::FieldDef{"_reserved_psn", 7},
    bit_fields::FieldDef{"psn", 24},
}}};

/// RDMA Extended Transport Header (RETH) - 128 bits / 16 bytes
///
/// Format:
///   Bytes 0-7:    virtual_address (64 bits)
///   Bytes 8-11:   rkey (32 bits)
///   Bytes 12-15:  dma_length (32 bits)
inline constexpr bit_fields::PacketFormat<3> kRethFormat{{{
    bit_fields::FieldDef{"virtual_address", 64},
    bit_fields::FieldDef{"rkey", 32},
    bit_fields::FieldDef{"dma_length", 32},
}}};

/// ACK Extended Transport Header (AETH) - 32 bits / 4 bytes
///
/// Format:
///   Byte 0:       syndrome (8 bits)
///   Bytes 1-3:    msn (24 bits)
inline constexpr bit_fields::PacketFormat<2> kAethFormat{{{
    bit_fields::FieldDef{"syndrome", 8},
    bit_fields::FieldDef{"msn", 24},
}}};

/// Immediate Data - 32 bits / 4 bytes
///
/// Format:
///   Bytes 0-3:    immediate (32 bits)
inline constexpr bit_fields::PacketFormat<1> kImmediateFormat{{{
    bit_fields::FieldDef{"immediate", 32},
}}};

/// Atomic ETH Header - 224 bits / 28 bytes
///
/// Format:
///   Bytes 0-7:    virtual_address (64 bits)
///   Bytes 8-11:   rkey (32 bits)
///   Bytes 12-19:  swap_or_add_data (64 bits)
///   Bytes 20-27:  compare_data (64 bits)
inline constexpr bit_fields::PacketFormat<4> kAtomicEthFormat{{{
    bit_fields::FieldDef{"virtual_address", 64},
    bit_fields::FieldDef{"rkey", 32},
    bit_fields::FieldDef{"swap_or_add_data", 64},
    bit_fields::FieldDef{"compare_data", 64},
}}};

/// Atomic ACK ETH Header - 64 bits / 8 bytes
///
/// Format:
///   Bytes 0-7:    original_data (64 bits)
inline constexpr bit_fields::PacketFormat<1> kAtomicAckEthFormat{{{
    bit_fields::FieldDef{"original_data", 64},
}}};

// =============================================================================
// Size constants (derived from formats)
// =============================================================================

/// Size of BTH header in bytes.
inline constexpr std::size_t kBthFormatSize = kBthFormat.total_bits() / 8;

/// Size of RETH header in bytes.
inline constexpr std::size_t kRethFormatSize = kRethFormat.total_bits() / 8;

/// Size of AETH header in bytes.
inline constexpr std::size_t kAethFormatSize = kAethFormat.total_bits() / 8;

/// Size of Immediate Data header in bytes.
inline constexpr std::size_t kImmediateFormatSize = kImmediateFormat.total_bits() / 8;

/// Size of Atomic ETH header in bytes.
inline constexpr std::size_t kAtomicEthFormatSize = kAtomicEthFormat.total_bits() / 8;

/// Size of Atomic ACK ETH header in bytes.
inline constexpr std::size_t kAtomicAckEthFormatSize = kAtomicAckEthFormat.total_bits() / 8;

// =============================================================================
// Static assertions to verify format sizes
// =============================================================================

static_assert(kBthFormatSize == 12, "BTH must be 12 bytes");
static_assert(kRethFormatSize == 16, "RETH must be 16 bytes");
static_assert(kAethFormatSize == 4, "AETH must be 4 bytes");
static_assert(kImmediateFormatSize == 4, "Immediate must be 4 bytes");
static_assert(kAtomicEthFormatSize == 28, "AtomicETH must be 28 bytes");
static_assert(kAtomicAckEthFormatSize == 8, "AtomicAckETH must be 8 bytes");

}  // namespace nic::rocev2

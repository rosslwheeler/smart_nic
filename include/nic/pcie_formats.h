#pragma once

/// @file pcie_formats.h
/// @brief PCIe configuration space register formats for use with bit_fields library.

#include <bit_fields/bit_fields.h>

namespace nic::pcie {

// =============================================================================
// PCIe Command Register (16 bits) - Offset 0x04
// =============================================================================

/// PCI Command Register format (16 bits).
inline constexpr bit_fields::RegisterFormat<12> kCommandRegisterFormat{{{
    bit_fields::FieldDef{"io_space_enable", 1},        // Bit 0
    bit_fields::FieldDef{"memory_space_enable", 1},    // Bit 1
    bit_fields::FieldDef{"bus_master_enable", 1},      // Bit 2
    bit_fields::FieldDef{"special_cycles", 1},         // Bit 3
    bit_fields::FieldDef{"mem_write_invalidate", 1},   // Bit 4
    bit_fields::FieldDef{"vga_palette_snoop", 1},      // Bit 5
    bit_fields::FieldDef{"parity_error_response", 1},  // Bit 6
    bit_fields::FieldDef{"_reserved0", 1},             // Bit 7
    bit_fields::FieldDef{"serr_enable", 1},            // Bit 8
    bit_fields::FieldDef{"fast_b2b_enable", 1},        // Bit 9
    bit_fields::FieldDef{"interrupt_disable", 1},      // Bit 10
    bit_fields::FieldDef{"_reserved1", 5},             // Bits 11-15
}}};

// =============================================================================
// PCIe Status Register (16 bits) - Offset 0x06
// =============================================================================

/// PCI Status Register format (16 bits).
/// Bits 8, 11-15 are RW1C (read-write-1-to-clear).
inline constexpr bit_fields::RegisterFormat<13> kStatusRegisterFormat{{{
    bit_fields::FieldDef{"_reserved0", 3},                // Bits 0-2
    bit_fields::FieldDef{"interrupt_status", 1},          // Bit 3 (RO)
    bit_fields::FieldDef{"capabilities_list", 1},         // Bit 4 (RO)
    bit_fields::FieldDef{"mhz_66_capable", 1},            // Bit 5 (RO)
    bit_fields::FieldDef{"_reserved1", 1},                // Bit 6
    bit_fields::FieldDef{"fast_b2b_capable", 1},          // Bit 7 (RO)
    bit_fields::FieldDef{"master_data_parity_error", 1},  // Bit 8 (RW1C)
    bit_fields::FieldDef{"devsel_timing", 2},             // Bits 9-10 (RO)
    bit_fields::FieldDef{"signaled_target_abort", 1},     // Bit 11 (RW1C)
    bit_fields::FieldDef{"received_target_abort", 1},     // Bit 12 (RW1C)
    bit_fields::FieldDef{"received_master_abort", 1},     // Bit 13 (RW1C)
    bit_fields::FieldDef{"signaled_system_error", 1},     // Bit 14 (RW1C)
    bit_fields::FieldDef{"detected_parity_error", 1},     // Bit 15 (RW1C)
}}};

// =============================================================================
// PCIe BAR Formats (32 bits each)
// =============================================================================

/// BAR io_space field values (bit 0).
namespace bar_io_space {
inline constexpr std::uint64_t kMemory = 0;  // 0: Memory space BAR
inline constexpr std::uint64_t kIO = 1;      // 1: I/O space BAR
}  // namespace bar_io_space

/// BAR type field values (bits 1-2 of memory BAR).
namespace bar_type {
inline constexpr std::uint64_t k32Bit = 0x00;  // 00: 32-bit address space
inline constexpr std::uint64_t k64Bit = 0x02;  // 10: 64-bit address space
}  // namespace bar_type

/// Memory BAR format (when bit 0 is 0).
/// Format:
///   Bit 0:    io_space (0 = memory)
///   Bits 1-2: type (00=32-bit, 10=64-bit)
///   Bit 3:    prefetchable
///   Bits 4-31: base_address (16-byte aligned minimum)
inline constexpr bit_fields::RegisterFormat<4> kMemoryBarFormat{{{
    bit_fields::FieldDef{"io_space", 1},       // Bit 0: 0 = memory
    bit_fields::FieldDef{"type", 2},           // Bits 1-2: 00=32-bit, 10=64-bit
    bit_fields::FieldDef{"prefetchable", 1},   // Bit 3
    bit_fields::FieldDef{"base_address", 28},  // Bits 4-31
}}};

/// I/O BAR format (when bit 0 is 1).
/// Format:
///   Bit 0:    io_space (1 = I/O)
///   Bit 1:    reserved
///   Bits 2-31: base_address (4-byte aligned)
inline constexpr bit_fields::RegisterFormat<3> kIoBarFormat{{{
    bit_fields::FieldDef{"io_space", 1},       // Bit 0: 1 = I/O
    bit_fields::FieldDef{"_reserved", 1},      // Bit 1
    bit_fields::FieldDef{"base_address", 30},  // Bits 2-31
}}};

// =============================================================================
// MSI-X Table Entry (128 bits / 16 bytes per entry)
// =============================================================================

/// x86 MSI/MSI-X message address base (Intel APIC).
namespace msi {
inline constexpr std::uint64_t kMessageAddressBase = 0xFEE00000;
}  // namespace msi

/// MSI-X Table Entry format.
/// Format:
///   Bytes 0-7:   message_address (64 bits)
///   Bytes 8-11:  message_data (32 bits)
///   Bytes 12-15: vector_control (32 bits, only bit 0 used for mask)
inline constexpr bit_fields::RegisterFormat<4> kMsixEntryFormat{{{
    bit_fields::FieldDef{"message_address", 64},  // Bytes 0-7
    bit_fields::FieldDef{"message_data", 32},     // Bytes 8-11
    bit_fields::FieldDef{"masked", 1},            // Bit 0 of vector_control
    bit_fields::FieldDef{"_reserved", 31},        // Bits 1-31 of vector_control
}}};

// =============================================================================
// PCIe Capability Header (32 bits)
// =============================================================================

/// PCIe Capability Header format.
/// Format:
///   Byte 0:    capability_id (8 bits)
///   Byte 1:    next_pointer (8 bits)
///   Bytes 2-3: capability_specific (16 bits)
inline constexpr bit_fields::RegisterFormat<3> kCapabilityHeaderFormat{{{
    bit_fields::FieldDef{"capability_id", 8},
    bit_fields::FieldDef{"next_pointer", 8},
    bit_fields::FieldDef{"capability_specific", 16},
}}};

/// Standard PCI capability IDs.
namespace capability_id {
inline constexpr std::uint64_t kPowerManagement = 0x01;
inline constexpr std::uint64_t kMsi = 0x05;
inline constexpr std::uint64_t kVendorSpecific = 0x09;
inline constexpr std::uint64_t kPciExpress = 0x10;
inline constexpr std::uint64_t kMsix = 0x11;
}  // namespace capability_id

// =============================================================================
// Size constants (derived from formats)
// =============================================================================

inline constexpr std::size_t kCommandRegisterSize = kCommandRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kStatusRegisterSize = kStatusRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kMemoryBarSize = kMemoryBarFormat.total_bits() / 8;
inline constexpr std::size_t kIoBarSize = kIoBarFormat.total_bits() / 8;
inline constexpr std::size_t kMsixEntrySize = kMsixEntryFormat.total_bits() / 8;

// =============================================================================
// Static assertions to verify format sizes
// =============================================================================

// Command register: full 16 bits defined
static_assert(kCommandRegisterFormat.total_bits() == 16, "Command register must be 16 bits");

// Status register: full 16 bits defined
static_assert(kStatusRegisterFormat.total_bits() == 16, "Status register must be 16 bits");

// BAR formats: 32 bits each
static_assert(kMemoryBarFormat.total_bits() == 32, "Memory BAR must be 32 bits");
static_assert(kIoBarFormat.total_bits() == 32, "I/O BAR must be 32 bits");

// MSI-X entry: 128 bits (16 bytes)
static_assert(kMsixEntryFormat.total_bits() == 128, "MSI-X entry must be 128 bits");

// Capability header: 32 bits
static_assert(kCapabilityHeaderFormat.total_bits() == 32, "Capability header must be 32 bits");

}  // namespace nic::pcie

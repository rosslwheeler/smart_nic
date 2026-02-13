#pragma once

/// @file register_formats.h
/// @brief NIC device register field definitions for use with bit_fields library.
///
/// These formats define the bit-level structure of NIC MMIO registers.
/// They complement the RegisterDef metadata in register.h with field introspection.

#include <bit_fields/bit_fields.h>

namespace nic {

// =============================================================================
// Device Control Register (CTRL) - Offset 0x0000
// =============================================================================

/// Link speed field values (used in CTRL and STATUS registers).
namespace link_speed {
inline constexpr std::uint64_t k10Gbps = 0x00;
inline constexpr std::uint64_t k25Gbps = 0x01;
inline constexpr std::uint64_t k40Gbps = 0x02;
inline constexpr std::uint64_t k50Gbps = 0x03;
inline constexpr std::uint64_t k100Gbps = 0x04;
inline constexpr std::uint64_t k200Gbps = 0x05;
inline constexpr std::uint64_t k400Gbps = 0x06;
inline constexpr std::uint64_t k800Gbps = 0x07;
}  // namespace link_speed

/// CTRL register format (32 bits).
/// Controls basic device operation.
inline constexpr bit_fields::RegisterFormat<16> kCtrlRegisterFormat{{{
    bit_fields::FieldDef{"full_duplex", 1},         // Bit 0: Force full duplex
    bit_fields::FieldDef{"_reserved0", 1},          // Bit 1
    bit_fields::FieldDef{"gio_master_disable", 1},  // Bit 2: Disable GIO master
    bit_fields::FieldDef{"link_reset", 1},          // Bit 3: Link reset
    bit_fields::FieldDef{"_reserved1", 2},          // Bits 4-5
    bit_fields::FieldDef{"set_link_up", 1},         // Bit 6: Force link up
    bit_fields::FieldDef{"speed_selection", 3},     // Bits 7-9: see link_speed namespace
    bit_fields::FieldDef{"force_speed", 1},         // Bit 10: Force speed (ignore auto-neg)
    bit_fields::FieldDef{"force_duplex", 1},        // Bit 11: Force duplex (ignore auto-neg)
    bit_fields::FieldDef{"_reserved2", 14},         // Bits 12-25
    bit_fields::FieldDef{"device_reset", 1},        // Bit 26: Software reset
    bit_fields::FieldDef{"rx_flow_control", 1},     // Bit 27: RX flow control enable
    bit_fields::FieldDef{"tx_flow_control", 1},     // Bit 28: TX flow control enable
    bit_fields::FieldDef{"_reserved3", 1},          // Bit 29
    bit_fields::FieldDef{"vlan_mode", 1},           // Bit 30: VLAN mode enable
    bit_fields::FieldDef{"phy_reset", 1},           // Bit 31: PHY reset
}}};

// =============================================================================
// Device Status Register (STATUS) - Offset 0x0008
// =============================================================================

/// STATUS register format (32 bits).
/// Indicates current device state (read-only).
inline constexpr bit_fields::RegisterFormat<11> kStatusRegisterFormat{{{
    bit_fields::FieldDef{"full_duplex", 1},    // Bit 0: Link is full duplex
    bit_fields::FieldDef{"link_up", 1},        // Bit 1: Link is up
    bit_fields::FieldDef{"function_id", 2},    // Bits 2-3: Function ID (for multi-function)
    bit_fields::FieldDef{"tx_paused", 1},      // Bit 4: TX is paused (flow control)
    bit_fields::FieldDef{"_reserved0", 1},     // Bit 5
    bit_fields::FieldDef{"speed", 3},          // Bits 6-8: see link_speed namespace
    bit_fields::FieldDef{"_reserved1", 3},     // Bits 9-11
    bit_fields::FieldDef{"auto_neg_done", 1},  // Bit 12: Auto-negotiation complete
    bit_fields::FieldDef{"_reserved2", 6},     // Bits 13-18
    bit_fields::FieldDef{"gio_master_en", 1},  // Bit 19: GIO master enabled
    bit_fields::FieldDef{"_reserved3", 12},    // Bits 20-31
}}};

// =============================================================================
// Interrupt Cause Register (ICR) - Offset 0x00C0
// =============================================================================

/// ICR register format (32 bits).
/// Each bit indicates a pending interrupt cause. Read-to-clear.
inline constexpr bit_fields::RegisterFormat<14> kIcrRegisterFormat{{{
    bit_fields::FieldDef{"tx_desc_written", 1},     // Bit 0: TX descriptor written back
    bit_fields::FieldDef{"tx_queue_empty", 1},      // Bit 1: TX queue empty
    bit_fields::FieldDef{"link_status_change", 1},  // Bit 2: Link status changed
    bit_fields::FieldDef{"rx_seq_error", 1},        // Bit 3: RX sequence error
    bit_fields::FieldDef{"rx_desc_min_thresh", 1},  // Bit 4: RX descriptor min threshold
    bit_fields::FieldDef{"_reserved0", 1},          // Bit 5
    bit_fields::FieldDef{"rx_overrun", 1},          // Bit 6: RX FIFO overrun
    bit_fields::FieldDef{"rx_timer", 1},            // Bit 7: RX timer expired
    bit_fields::FieldDef{"_reserved1", 1},          // Bit 8
    bit_fields::FieldDef{"mdio_access_done", 1},    // Bit 9: MDIO access complete
    bit_fields::FieldDef{"_reserved2", 5},          // Bits 10-14
    bit_fields::FieldDef{"tx_low_thresh", 1},       // Bit 15: TX descriptor low threshold
    bit_fields::FieldDef{"small_rx_packet", 1},     // Bit 16: Small packet received
    bit_fields::FieldDef{"_reserved3", 15},         // Bits 17-31
}}};

// =============================================================================
// Interrupt Mask Set/Read Register (IMS) - Offset 0x00D0
// =============================================================================

/// IMS register format (32 bits).
/// Writing 1 to a bit enables the corresponding interrupt. RW1S semantics.
/// Uses same bit positions as ICR.
inline constexpr bit_fields::RegisterFormat<14> kImsRegisterFormat{{{
    bit_fields::FieldDef{"tx_desc_written", 1},
    bit_fields::FieldDef{"tx_queue_empty", 1},
    bit_fields::FieldDef{"link_status_change", 1},
    bit_fields::FieldDef{"rx_seq_error", 1},
    bit_fields::FieldDef{"rx_desc_min_thresh", 1},
    bit_fields::FieldDef{"_reserved0", 1},
    bit_fields::FieldDef{"rx_overrun", 1},
    bit_fields::FieldDef{"rx_timer", 1},
    bit_fields::FieldDef{"_reserved1", 1},
    bit_fields::FieldDef{"mdio_access_done", 1},
    bit_fields::FieldDef{"_reserved2", 5},
    bit_fields::FieldDef{"tx_low_thresh", 1},
    bit_fields::FieldDef{"small_rx_packet", 1},
    bit_fields::FieldDef{"_reserved3", 15},
}}};

// =============================================================================
// RX Control Register (RCTL) - Offset 0x0100
// =============================================================================

/// RCTL register format (32 bits).
/// Controls receive operation.
inline constexpr bit_fields::RegisterFormat<23> kRctlRegisterFormat{{{
    bit_fields::FieldDef{"_reserved0", 1},          // Bit 0
    bit_fields::FieldDef{"rx_enable", 1},           // Bit 1: Receiver enable
    bit_fields::FieldDef{"store_bad_packets", 1},   // Bit 2: Store bad packets
    bit_fields::FieldDef{"unicast_promisc", 1},     // Bit 3: Unicast promiscuous mode
    bit_fields::FieldDef{"multicast_promisc", 1},   // Bit 4: Multicast promiscuous mode
    bit_fields::FieldDef{"long_packet_enable", 1},  // Bit 5: Accept packets > 1522 bytes
    bit_fields::FieldDef{"loopback_mode", 2},       // Bits 6-7: 00=normal, 01=MAC loopback
    bit_fields::FieldDef{"rx_desc_thresh", 2},      // Bits 8-9: Descriptor threshold
    bit_fields::FieldDef{"_reserved1", 2},          // Bits 10-11
    bit_fields::FieldDef{"multicast_offset", 2},    // Bits 12-13: Multicast filter offset
    bit_fields::FieldDef{"_reserved2", 1},          // Bit 14
    bit_fields::FieldDef{"broadcast_accept", 1},    // Bit 15: Accept broadcast packets
    bit_fields::FieldDef{"rx_buffer_size", 2},      // Bits 16-17: Buffer size
    bit_fields::FieldDef{"vlan_filter_enable", 1},  // Bit 18: VLAN filtering
    bit_fields::FieldDef{"_reserved3", 1},          // Bit 19
    bit_fields::FieldDef{"canonical_form_ind", 1},  // Bit 20: CFI enable
    bit_fields::FieldDef{"_reserved4", 1},          // Bit 21
    bit_fields::FieldDef{"discard_pause", 1},       // Bit 22: Discard pause frames
    bit_fields::FieldDef{"pass_mac_ctrl", 1},       // Bit 23: Pass MAC control frames
    bit_fields::FieldDef{"_reserved5", 1},          // Bit 24
    bit_fields::FieldDef{"buffer_size_ext", 1},     // Bit 25: Buffer size extension
    bit_fields::FieldDef{"strip_crc", 1},           // Bit 26: Strip Ethernet CRC
    bit_fields::FieldDef{"_reserved6", 5},          // Bits 27-31
}}};

// =============================================================================
// TX Control Register (TCTL) - Offset 0x0400
// =============================================================================

/// TCTL register format (32 bits).
/// Controls transmit operation.
inline constexpr bit_fields::RegisterFormat<10> kTctlRegisterFormat{{{
    bit_fields::FieldDef{"_reserved0", 1},         // Bit 0
    bit_fields::FieldDef{"tx_enable", 1},          // Bit 1: Transmitter enable
    bit_fields::FieldDef{"_reserved1", 1},         // Bit 2
    bit_fields::FieldDef{"pad_short_packets", 1},  // Bit 3: Pad packets to min length
    bit_fields::FieldDef{"collision_thresh", 8},   // Bits 4-11: Collision threshold
    bit_fields::FieldDef{"collision_dist", 10},    // Bits 12-21: Collision distance
    bit_fields::FieldDef{"software_xoff", 1},      // Bit 22: Software XOFF
    bit_fields::FieldDef{"_reserved2", 1},         // Bit 23
    bit_fields::FieldDef{"retx_on_late_coll", 1},  // Bit 24: Retransmit on late collision
    bit_fields::FieldDef{"_reserved3", 7},         // Bits 25-31
}}};

// =============================================================================
// Size constants
// =============================================================================

inline constexpr std::size_t kCtrlRegisterSize = kCtrlRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kStatusRegisterSize = kStatusRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kIcrRegisterSize = kIcrRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kImsRegisterSize = kImsRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kRctlRegisterSize = kRctlRegisterFormat.total_bits() / 8;
inline constexpr std::size_t kTctlRegisterSize = kTctlRegisterFormat.total_bits() / 8;

// =============================================================================
// Static assertions to verify format sizes
// =============================================================================

static_assert(kCtrlRegisterFormat.total_bits() == 32, "CTRL must be 32 bits");
static_assert(kStatusRegisterFormat.total_bits() == 32, "STATUS must be 32 bits");
static_assert(kIcrRegisterFormat.total_bits() == 32, "ICR must be 32 bits");
static_assert(kImsRegisterFormat.total_bits() == 32, "IMS must be 32 bits");
static_assert(kRctlRegisterFormat.total_bits() == 32, "RCTL must be 32 bits");
static_assert(kTctlRegisterFormat.total_bits() == 32, "TCTL must be 32 bits");

// =============================================================================
// Register offsets (matching register.h definitions)
// =============================================================================

namespace register_offset {
inline constexpr std::uint32_t kCtrl = 0x0000;
inline constexpr std::uint32_t kStatus = 0x0008;
inline constexpr std::uint32_t kIcr = 0x00C0;
inline constexpr std::uint32_t kIcs = 0x00C8;
inline constexpr std::uint32_t kIms = 0x00D0;
inline constexpr std::uint32_t kImc = 0x00D8;
inline constexpr std::uint32_t kRctl = 0x0100;
inline constexpr std::uint32_t kTctl = 0x0400;
}  // namespace register_offset

}  // namespace nic

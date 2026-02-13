#include "nic/register_formats.h"

#include <array>
#include <cstddef>

/// Test NIC register format definitions.
int main() {
  // Test 1: CTRL register format - all 32 bits defined
  {
    static_assert(nic::kCtrlRegisterFormat.total_bits() == 32);
    static_assert(nic::kCtrlRegisterSize == 4);

    // Create a CTRL value with link up and full duplex
    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::kCtrlRegisterFormat,
                     1ULL,                      // full_duplex
                     0ULL,                      // _reserved0
                     0ULL,                      // gio_master_disable
                     0ULL,                      // link_reset
                     0ULL,                      // _reserved1
                     1ULL,                      // set_link_up
                     nic::link_speed::k10Gbps,  // speed_selection
                     0ULL,                      // force_speed
                     0ULL,                      // force_duplex
                     0ULL,                      // _reserved2
                     0ULL,                      // device_reset
                     0ULL,                      // rx_flow_control
                     0ULL,                      // tx_flow_control
                     0ULL,                      // _reserved3
                     0ULL,                      // vlan_mode
                     0ULL                       // phy_reset
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::kCtrlRegisterFormat);
    constexpr bit_fields::ExpectedTable<3> expected{{
        {"full_duplex", 1},
        {"set_link_up", 1},
        {"speed_selection", nic::link_speed::k10Gbps},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 2: STATUS register format - verify read-only fields
  {
    static_assert(nic::kStatusRegisterFormat.total_bits() == 32);
    static_assert(nic::kStatusRegisterSize == 4);

    // Simulate link up at 1Gbps, full duplex
    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::kStatusRegisterFormat,
                     1ULL,                      // full_duplex
                     1ULL,                      // link_up
                     0ULL,                      // function_id
                     0ULL,                      // tx_paused
                     0ULL,                      // _reserved0
                     nic::link_speed::k10Gbps,  // speed
                     0ULL,                      // _reserved1
                     1ULL,                      // auto_neg_done
                     0ULL,                      // _reserved2
                     1ULL,                      // gio_master_en
                     0ULL                       // _reserved3
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::kStatusRegisterFormat);
    constexpr bit_fields::ExpectedTable<5> expected{{
        {"full_duplex", 1},
        {"link_up", 1},
        {"speed", nic::link_speed::k10Gbps},
        {"auto_neg_done", 1},
        {"gio_master_en", 1},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 3: ICR register format - interrupt causes
  {
    static_assert(nic::kIcrRegisterFormat.total_bits() == 32);
    static_assert(nic::kIcrRegisterSize == 4);

    // Simulate TX desc written and link status change
    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::kIcrRegisterFormat,
                     1ULL,  // tx_desc_written
                     0ULL,  // tx_queue_empty
                     1ULL,  // link_status_change
                     0ULL,  // rx_seq_error
                     0ULL,  // rx_desc_min_thresh
                     0ULL,  // _reserved0
                     0ULL,  // rx_overrun
                     0ULL,  // rx_timer
                     0ULL,  // _reserved1
                     0ULL,  // mdio_access_done
                     0ULL,  // _reserved2
                     0ULL,  // tx_low_thresh
                     0ULL,  // small_rx_packet
                     0ULL   // _reserved3
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::kIcrRegisterFormat);
    constexpr bit_fields::ExpectedTable<3> expected{{
        {"tx_desc_written", 1},
        {"link_status_change", 1},
        {"rx_overrun", 0},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 4: RCTL register format - receive control
  {
    static_assert(nic::kRctlRegisterFormat.total_bits() == 32);
    static_assert(nic::kRctlRegisterSize == 4);

    // Enable receiver with promiscuous mode
    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::kRctlRegisterFormat,
                     0ULL,  // _reserved0
                     1ULL,  // rx_enable
                     0ULL,  // store_bad_packets
                     1ULL,  // unicast_promisc
                     1ULL,  // multicast_promisc
                     0ULL,  // long_packet_enable
                     0ULL,  // loopback_mode
                     0ULL,  // rx_desc_thresh
                     0ULL,  // _reserved1
                     0ULL,  // multicast_offset
                     0ULL,  // _reserved2
                     1ULL,  // broadcast_accept
                     0ULL,  // rx_buffer_size
                     0ULL,  // vlan_filter_enable
                     0ULL,  // _reserved3
                     0ULL,  // canonical_form_ind
                     0ULL,  // _reserved4
                     0ULL,  // discard_pause
                     0ULL,  // pass_mac_ctrl
                     0ULL,  // _reserved5
                     0ULL,  // buffer_size_ext
                     0ULL,  // strip_crc
                     0ULL   // _reserved6
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::kRctlRegisterFormat);
    constexpr bit_fields::ExpectedTable<4> expected{{
        {"rx_enable", 1},
        {"unicast_promisc", 1},
        {"multicast_promisc", 1},
        {"broadcast_accept", 1},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 5: TCTL register format - transmit control
  {
    static_assert(nic::kTctlRegisterFormat.total_bits() == 32);
    static_assert(nic::kTctlRegisterSize == 4);

    // Enable transmitter with padding
    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::kTctlRegisterFormat,
                     0ULL,      // _reserved0
                     1ULL,      // tx_enable
                     0ULL,      // _reserved1
                     1ULL,      // pad_short_packets
                     0x0FULL,   // collision_thresh
                     0x3FFULL,  // collision_dist (max)
                     0ULL,      // software_xoff
                     0ULL,      // _reserved2
                     0ULL,      // retx_on_late_coll
                     0ULL       // _reserved3
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::kTctlRegisterFormat);
    constexpr bit_fields::ExpectedTable<4> expected{{
        {"tx_enable", 1},
        {"pad_short_packets", 1},
        {"collision_thresh", 0x0F},
        {"collision_dist", 0x3FF},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 6: Field lookup by name (compile-time verification)
  {
    static_assert(nic::kCtrlRegisterFormat.bit_offset_of("device_reset").has_value());
    static_assert(*nic::kCtrlRegisterFormat.bit_offset_of("device_reset") == 26);

    static_assert(nic::kCtrlRegisterFormat.field_width("speed_selection").has_value());
    static_assert(*nic::kCtrlRegisterFormat.field_width("speed_selection") == 3);

    static_assert(!nic::kCtrlRegisterFormat.bit_offset_of("nonexistent_field").has_value());
  }

  // Test 7: Register offset constants (compile-time verification)
  {
    static_assert(nic::register_offset::kCtrl == 0x0000);
    static_assert(nic::register_offset::kStatus == 0x0008);
    static_assert(nic::register_offset::kIcr == 0x00C0);
    static_assert(nic::register_offset::kIms == 0x00D0);
    static_assert(nic::register_offset::kRctl == 0x0100);
    static_assert(nic::register_offset::kTctl == 0x0400);
  }

  // Test 8: Round-trip for CTRL register with device reset
  {
    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::kCtrlRegisterFormat,
                     0ULL,  // full_duplex
                     0ULL,  // _reserved0
                     0ULL,  // gio_master_disable
                     0ULL,  // link_reset
                     0ULL,  // _reserved1
                     0ULL,  // set_link_up
                     0ULL,  // speed_selection
                     0ULL,  // force_speed
                     0ULL,  // force_duplex
                     0ULL,  // _reserved2
                     1ULL,  // device_reset
                     0ULL,  // rx_flow_control
                     0ULL,  // tx_flow_control
                     0ULL,  // _reserved3
                     0ULL,  // vlan_mode
                     0ULL   // phy_reset
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::kCtrlRegisterFormat);
    constexpr bit_fields::ExpectedTable<1> expected{{
        {"device_reset", 1},
    }};
    reader.assert_expected(parsed, expected);
  }

  return 0;
}

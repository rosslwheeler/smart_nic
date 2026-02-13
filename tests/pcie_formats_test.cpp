#include "nic/pcie_formats.h"

#include <array>
#include <cstddef>

#include "nic/bar.h"

/// Test PCIe register format definitions.
int main() {
  // Test 1: Command register format - serialize and deserialize
  {
    // Create a command register value with bus master and memory space enabled
    std::array<std::byte, 2> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);

    // io_space=0, memory_space=1, bus_master=1, special_cycles=0, etc.
    writer.serialize(nic::pcie::kCommandRegisterFormat,
                     0ULL,  // io_space_enable
                     1ULL,  // memory_space_enable
                     1ULL,  // bus_master_enable
                     0ULL,  // special_cycles
                     0ULL,  // mem_write_invalidate
                     0ULL,  // vga_palette_snoop
                     0ULL,  // parity_error_response
                     0ULL,  // _reserved0
                     0ULL,  // serr_enable
                     0ULL,  // fast_b2b_enable
                     0ULL,  // interrupt_disable
                     0ULL   // _reserved1
    );

    // Read it back and verify using ExpectedTable
    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);
    constexpr bit_fields::ExpectedTable<4> expected{{
        {"io_space_enable", 0},
        {"memory_space_enable", 1},
        {"bus_master_enable", 1},
        {"interrupt_disable", 0},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 2: Status register format - verify all 16 bits defined
  {
    static_assert(nic::pcie::kStatusRegisterFormat.total_bits() == 16);

    // Create a status with capabilities_list set
    std::array<std::byte, 2> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::pcie::kStatusRegisterFormat,
                     0ULL,  // _reserved0
                     0ULL,  // interrupt_status
                     1ULL,  // capabilities_list
                     0ULL,  // mhz_66_capable
                     0ULL,  // _reserved1
                     0ULL,  // fast_b2b_capable
                     0ULL,  // master_data_parity_error
                     0ULL,  // devsel_timing
                     0ULL,  // signaled_target_abort
                     0ULL,  // received_target_abort
                     0ULL,  // received_master_abort
                     0ULL,  // signaled_system_error
                     0ULL   // detected_parity_error
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kStatusRegisterFormat);
    constexpr bit_fields::ExpectedTable<2> expected{{
        {"capabilities_list", 1},
        {"interrupt_status", 0},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 3: Memory BAR format - parse a 64-bit prefetchable BAR
  {
    // Create a 64-bit prefetchable memory BAR at 0xFE000000
    nic::Bar memory_bar{
        .base_address = 0xFE000000,
        .size = 64 * 1024,
        .type = nic::BarType::Memory64,
        .prefetchable = true,
    };

    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::pcie::kMemoryBarFormat,
                     nic::pcie::bar_io_space::kMemory,
                     memory_bar.bar_type_field(),
                     std::uint64_t{memory_bar.prefetchable},
                     memory_bar.memory_bar_address_field());

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kMemoryBarFormat);
    constexpr bit_fields::ExpectedTable<4> expected{{
        {"io_space", nic::pcie::bar_io_space::kMemory},
        {"type", nic::pcie::bar_type::k64Bit},
        {"prefetchable", 1},
        {"base_address", 0x0FE00000},  // 0xFE000000 >> 4
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 4: I/O BAR format
  {
    // Create an I/O BAR at address 0x1000
    nic::Bar io_bar{
        .base_address = 0x1000,
        .size = 256,
        .type = nic::BarType::IO,
        .prefetchable = false,
    };

    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::pcie::kIoBarFormat,
                     nic::pcie::bar_io_space::kIO,
                     0ULL,  // _reserved
                     io_bar.io_bar_address_field());

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kIoBarFormat);
    constexpr bit_fields::ExpectedTable<2> expected{{
        {"io_space", nic::pcie::bar_io_space::kIO}, {"base_address", 0x0400},  // 0x1000 >> 2
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 5: MSI-X entry format - 128 bits
  {
    static_assert(nic::pcie::kMsixEntryFormat.total_bits() == 128);
    static_assert(nic::pcie::kMsixEntrySize == 16);

    std::array<std::byte, 16> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::pcie::kMsixEntryFormat,
                     nic::pcie::msi::kMessageAddressBase,
                     0x41ULL,  // message_data (vector + delivery mode)
                     0ULL,     // masked
                     0ULL      // _reserved
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kMsixEntryFormat);
    constexpr bit_fields::ExpectedTable<3> expected{{
        {"message_address", nic::pcie::msi::kMessageAddressBase},
        {"message_data", 0x41ULL},
        {"masked", 0},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 6: Capability header format
  {
    static_assert(nic::pcie::kCapabilityHeaderFormat.total_bits() == 32);

    std::array<std::byte, 4> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::pcie::kCapabilityHeaderFormat,
                     nic::pcie::capability_id::kMsi,
                     0x50ULL,   // next_pointer (config space offset)
                     0x0000ULL  // capability_specific
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kCapabilityHeaderFormat);
    constexpr bit_fields::ExpectedTable<2> expected{{
        {"capability_id", nic::pcie::capability_id::kMsi},
        {"next_pointer", 0x50},
    }};
    reader.assert_expected(parsed, expected);
  }

  // Test 7: Round-trip verification for command register
  {
    // Set all defined bits
    std::array<std::byte, 2> buffer{};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
    writer.serialize(nic::pcie::kCommandRegisterFormat,
                     1ULL,  // io_space_enable
                     1ULL,  // memory_space_enable
                     1ULL,  // bus_master_enable
                     0ULL,  // special_cycles
                     1ULL,  // mem_write_invalidate
                     0ULL,  // vga_palette_snoop
                     1ULL,  // parity_error_response
                     0ULL,  // _reserved0
                     1ULL,  // serr_enable
                     0ULL,  // fast_b2b_enable
                     1ULL,  // interrupt_disable
                     0ULL   // _reserved1
    );

    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);
    constexpr bit_fields::ExpectedTable<7> expected{{
        {"io_space_enable", 1},
        {"memory_space_enable", 1},
        {"bus_master_enable", 1},
        {"mem_write_invalidate", 1},
        {"parity_error_response", 1},
        {"serr_enable", 1},
        {"interrupt_disable", 1},
    }};
    reader.assert_expected(parsed, expected);
  }

  return 0;
}

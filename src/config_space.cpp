#include "nic/config_space.h"

#include <span>

#include "bit_fields/bit_fields.h"
#include "nic/trace.h"

using namespace nic;

void ConfigSpace::initialize(std::uint16_t vendor_id,
                             std::uint16_t device_id,
                             std::uint8_t revision,
                             const BarArray& bars,
                             const CapabilityList& caps) {
  NIC_TRACE_SCOPED(__func__);
  data_.fill(0);

  // Vendor and Device ID (use raw writes to bypass RO check)
  raw_write16(config_offset::kVendorId, vendor_id);
  raw_write16(config_offset::kDeviceId, device_id);

  // Command: start with interrupts disabled
  raw_write16(config_offset::kCommand, command_bits::kInterruptDisable);

  // Status: capabilities list present
  raw_write16(config_offset::kStatus, status_bits::kCapabilitiesList);

  // Revision ID
  raw_write8(config_offset::kRevisionId, revision);

  // Class code: Network Controller / Ethernet
  raw_write8(config_offset::kClassCode, 0x00);  // Prog IF
  raw_write8(config_offset::kClassCode + 1, class_code::kEthernetController);
  raw_write8(config_offset::kClassCode + 2, class_code::kNetworkController);

  // Header type: Type 0 (endpoint), single function
  raw_write8(config_offset::kHeaderType, 0x00);

  // Initialize BARs
  initialize_bars(bars);

  // Capabilities pointer
  write8(config_offset::kCapabilitiesPtr, caps.first_capability_offset());

  // Initialize capability structures
  initialize_capabilities(caps);

  // Interrupt pin: INTA#
  write8(config_offset::kInterruptPin, 0x01);
}

void ConfigSpace::initialize_bars(const BarArray& bars) {
  NIC_TRACE_SCOPED(__func__);
  for (std::size_t bar_index = 0; bar_index < kMaxBars; ++bar_index) {
    const auto& bar = bars[bar_index];

    auto dest = std::span<std::byte>(
        reinterpret_cast<std::byte*>(&data_[config_offset::kBarOffsets[bar_index]]),
        pcie::kMemoryBarSize);
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(dest);

    if (bar.is_enabled()) {
      if (bar.is_memory()) {
        writer.serialize(pcie::kMemoryBarFormat,
                         pcie::bar_io_space::kMemory,
                         bar.bar_type_field(),
                         std::uint64_t{bar.prefetchable},
                         bar.memory_bar_address_field());

        // For 64-bit BARs, write upper 32 bits to next BAR slot and skip it
        if (bar.is_64bit() && bar_index + 1 < kMaxBars) {
          auto upper_dest = std::span<std::byte>(
              reinterpret_cast<std::byte*>(&data_[config_offset::kBarOffsets[bar_index + 1]]),
              sizeof(std::uint32_t));
          bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> upper_writer(upper_dest);
          upper_writer.write_aligned(bar.upper_address_dword());
          ++bar_index;  // Skip the next slot (consumed by upper 32 bits)
        }
      } else {
        // I/O BAR
        writer.serialize(pcie::kIoBarFormat,
                         pcie::bar_io_space::kIO,
                         std::uint64_t{0},  // reserved
                         bar.io_bar_address_field());
      }
    } else {
      // Disabled BAR: writer already zero-initialized this slot.
      // For 64-bit BARs, also clear the upper slot to avoid stale data on reinit.
      if (bar.is_64bit() && bar_index + 1 < kMaxBars) {
        auto upper_dest = std::span<std::byte>(
            reinterpret_cast<std::byte*>(&data_[config_offset::kBarOffsets[bar_index + 1]]),
            sizeof(std::uint32_t));
        bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> upper_writer(upper_dest);
        // upper_writer zero-initializes the buffer
        ++bar_index;  // Skip the next slot (consumed by 64-bit BAR)
      }
    }
  }
}

void ConfigSpace::initialize_capabilities(const CapabilityList& caps) {
  NIC_TRACE_SCOPED(__func__);
  // Initialize standard capabilities
  for (const auto& cap : caps.standard) {
    auto dest = std::span<std::byte, 2>{reinterpret_cast<std::byte*>(&data_[cap.offset]), 2};
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(dest);
    writer.serialize(
        bit_fields::formats::kPciCapHeader, static_cast<std::uint8_t>(cap.id), cap.next);
    // Capability-specific data would be initialized here
  }

  // Initialize extended capabilities
  for (const auto& cap : caps.extended) {
    if (cap.offset >= config_offset::kExtendedConfigBase) {
      // Extended capability header format (little-endian, 32-bit):
      // [15:0]  = Capability ID
      // [19:16] = Version
      // [31:20] = Next capability offset
      auto dest = std::span<std::byte, 4>{reinterpret_cast<std::byte*>(&data_[cap.offset]), 4};
      bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(dest);
      writer.serialize(bit_fields::formats::kPcieExtCapHeader,
                       static_cast<std::uint16_t>(cap.id),
                       cap.version,
                       cap.next);
    }
  }
}

bool ConfigSpace::is_read_only(std::uint16_t offset) const noexcept {
  // Vendor ID (0x00-0x01), Device ID (0x02-0x03) are RO
  if (offset <= config_offset::kDeviceId + 1) {
    return true;
  }
  // Revision ID (0x08) and Class Code (0x09-0x0B) are RO
  if ((offset >= config_offset::kRevisionId) && (offset < config_offset::kCacheLineSize)) {
    return true;
  }
  // Header Type (0x0E) is RO
  if (offset == config_offset::kHeaderType) {
    return true;
  }
  return false;
}

// =============================================================================
// Field-level access using bit_fields formats
// =============================================================================

bool ConfigSpace::is_command_bit_set(std::string_view field_name) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!pcie::kCommandRegisterFormat.field_index(field_name).has_value()) {
    return false;
  }

  try {
    auto buffer = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(&data_[config_offset::kCommand]),
        pcie::kCommandRegisterSize);
    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    return reader.read_field(pcie::kCommandRegisterFormat, field_name) != 0;
  } catch (...) {
    return false;
  }
}

void ConfigSpace::set_command_bit(std::string_view field_name, bool value) noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto field_idx = pcie::kCommandRegisterFormat.field_index(field_name);
  if (!field_idx) {
    return;
  }

  try {
    // Read current register, deserialize all fields, modify target, reserialize
    auto buffer = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(&data_[config_offset::kCommand]),
        pcie::kCommandRegisterSize);
    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    auto parsed = reader.deserialize(pcie::kCommandRegisterFormat);

    // Update the target field
    parsed.values[*field_idx] = value ? 1 : 0;

    // Write back using BitWriter
    auto dest = std::span<std::byte>(reinterpret_cast<std::byte*>(&data_[config_offset::kCommand]),
                                     pcie::kCommandRegisterSize);
    bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(dest);
    for (std::size_t field_index = 0; field_index < parsed.values.size(); ++field_index) {
      writer.write_bits_runtime(pcie::kCommandRegisterFormat.fields[field_index].bit_width,
                                parsed.values[field_index]);
    }
  } catch (...) {
    // Silently fail - format/size mismatch should not happen with correct constants
  }
}

std::uint64_t ConfigSpace::get_status_field(std::string_view field_name) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!pcie::kStatusRegisterFormat.field_index(field_name).has_value()) {
    return 0;
  }

  try {
    auto buffer = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(&data_[config_offset::kStatus]),
        pcie::kStatusRegisterSize);
    bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
    return reader.read_field(pcie::kStatusRegisterFormat, field_name);
  } catch (...) {
    return 0;
  }
}

#include "nic/config_space.h"

#include <cassert>

#include "nic/device.h"

int main() {
  // Test 1: Device config space initialization after reset
  {
    nic::DeviceConfig config{};
    nic::Device device{config};

    // Before reset, device is uninitialized
    assert(device.state() == nic::DeviceState::Uninitialized);

    device.reset();

    // After reset, device is ready
    assert(device.state() == nic::DeviceState::Ready);
    assert(device.is_initialized());

    // Verify vendor/device ID in config space
    assert(device.read_config16(nic::config_offset::kVendorId) == config.identity.vendor_id);
    assert(device.read_config16(nic::config_offset::kDeviceId) == config.identity.device_id);
    assert(device.read_config8(nic::config_offset::kRevisionId) == config.identity.revision);
  }

  // Test 2: Class code is Network Controller / Ethernet
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    std::uint8_t class_code = device.read_config8(nic::config_offset::kClassCode + 2);
    std::uint8_t subclass = device.read_config8(nic::config_offset::kClassCode + 1);

    assert(class_code == nic::class_code::kNetworkController);
    assert(subclass == nic::class_code::kEthernetController);
  }

  // Test 3: Capabilities pointer is set
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // Status register should have capabilities list bit set
    std::uint16_t status = device.read_config16(nic::config_offset::kStatus);
    assert((status & nic::status_bits::kCapabilitiesList) != 0);

    // Capabilities pointer should point to first capability
    std::uint8_t cap_ptr = device.read_config8(nic::config_offset::kCapabilitiesPtr);
    assert(cap_ptr == 0x40);  // Power Management capability

    // Walk capability list
    std::uint8_t cap_id = device.read_config8(cap_ptr);
    assert(cap_id == static_cast<std::uint8_t>(nic::CapabilityId::PowerManagement));

    std::uint8_t next_cap = device.read_config8(cap_ptr + 1);
    assert(next_cap == 0x50);  // MSI-X capability
  }

  // Test 4: Config space read-only fields cannot be written
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    std::uint16_t original_vendor = device.read_config16(nic::config_offset::kVendorId);
    std::uint8_t original_revision = device.read_config8(nic::config_offset::kRevisionId);
    std::uint8_t original_header = device.read_config8(nic::config_offset::kHeaderType);

    // Attempt to write to vendor ID (should be ignored)
    device.write_config16(nic::config_offset::kVendorId, 0x1234);
    device.write_config8(nic::config_offset::kRevisionId, 0x99);
    device.write_config8(nic::config_offset::kHeaderType, 0xFF);

    // Vendor ID should remain unchanged
    assert(device.read_config16(nic::config_offset::kVendorId) == original_vendor);
    assert(device.read_config8(nic::config_offset::kRevisionId) == original_revision);
    assert(device.read_config8(nic::config_offset::kHeaderType) == original_header);
  }

  // Test 5: Command register is writable
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // Initially interrupts are disabled
    std::uint16_t cmd = device.read_config16(nic::config_offset::kCommand);
    assert((cmd & nic::command_bits::kInterruptDisable) != 0);

    // Enable bus master and memory space
    std::uint16_t new_cmd = cmd | nic::command_bits::kBusMaster | nic::command_bits::kMemorySpace;
    device.write_config16(nic::config_offset::kCommand, new_cmd);

    cmd = device.read_config16(nic::config_offset::kCommand);
    assert((cmd & nic::command_bits::kBusMaster) != 0);
    assert((cmd & nic::command_bits::kMemorySpace) != 0);
  }

  // Test 6: Out-of-bounds reads return all-ones
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // Read beyond config space
    assert(device.read_config32(nic::kConfigSpaceSize) == 0xFFFFFFFF);
    assert(device.read_config16(nic::kConfigSpaceSize + 100) == 0xFFFF);
    assert(device.read_config8(nic::kConfigSpaceSize + 200) == 0xFF);
  }

  return 0;
}

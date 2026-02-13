#include "nic/register.h"

#include <cassert>

#include "nic/device.h"

int main() {
  // Test 1: Register file initialization after reset
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // CTRL register should exist and be readable
    std::uint32_t ctrl = device.read_register(0x0000);
    assert(ctrl == 0x00000000);  // Reset value

    // STATUS register should exist
    std::uint32_t status = device.read_register(0x0008);
    assert(status == 0x00000000);  // Reset value
  }

  // Test 2: Register read/write
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // Write to CTRL register
    device.write_register(0x0000, 0x12345678);
    assert(device.read_register(0x0000) == 0x12345678);

    // Write to RCTL register
    device.write_register(0x0100, 0xABCDEF00);
    assert(device.read_register(0x0100) == 0xABCDEF00);
  }

  // Test 3: Read-only register ignores writes
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // STATUS is read-only
    std::uint32_t original = device.read_register(0x0008);
    device.write_register(0x0008, 0xFFFFFFFF);
    assert(device.read_register(0x0008) == original);
  }

  // Test 4: Unmapped register returns all-ones
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // Read from unmapped offset
    std::uint32_t value = device.read_register(0x9999);
    assert(value == 0xFFFFFFFF);
  }

  // Test 5: RegisterFile standalone usage
  {
    nic::RegisterFile regfile;

    regfile.add_register(nic::RegisterDef{
        .name = "TEST_RW",
        .offset = 0x100,
        .width = nic::RegisterWidth::Bits32,
        .access = nic::RegisterAccess::RW,
        .reset_value = 0xDEADBEEF,
        .write_mask = 0xFFFFFFFF,
    });

    regfile.add_register(nic::RegisterDef{
        .name = "TEST_RO",
        .offset = 0x104,
        .width = nic::RegisterWidth::Bits32,
        .access = nic::RegisterAccess::RO,
        .reset_value = 0x12340000,
        .write_mask = 0x00000000,
    });

    regfile.reset();

    // Check reset values
    assert(regfile.read32(0x100) == 0xDEADBEEF);
    assert(regfile.read32(0x104) == 0x12340000);

    // Write to RW register
    regfile.write32(0x100, 0x11111111);
    assert(regfile.read32(0x100) == 0x11111111);

    // Write to RO register should be ignored
    regfile.write32(0x104, 0x99999999);
    assert(regfile.read32(0x104) == 0x12340000);
  }

  // Test 6: RW1C (write-1-to-clear) semantics
  {
    nic::RegisterFile regfile;

    regfile.add_register(nic::RegisterDef{
        .name = "TEST_RW1C",
        .offset = 0x200,
        .width = nic::RegisterWidth::Bits32,
        .access = nic::RegisterAccess::RW1C,
        .reset_value = 0xFF00FF00,
        .write_mask = 0xFFFFFFFF,
    });

    regfile.reset();
    assert(regfile.read32(0x200) == 0xFF00FF00);

    // Write 1s to clear those bits
    regfile.write32(0x200, 0x0F000F00);

    // Bits that were written as 1 should be cleared
    assert(regfile.read32(0x200) == 0xF000F000);
  }

  // Test 7: RW1S (write-1-to-set) semantics
  {
    nic::RegisterFile regfile;

    regfile.add_register(nic::RegisterDef{
        .name = "TEST_RW1S",
        .offset = 0x300,
        .width = nic::RegisterWidth::Bits32,
        .access = nic::RegisterAccess::RW1S,
        .reset_value = 0x00000000,
        .write_mask = 0xFFFFFFFF,
    });

    regfile.reset();
    assert(regfile.read32(0x300) == 0x00000000);

    // Write 1s to set those bits
    regfile.write32(0x300, 0x0F0F0F0F);
    assert(regfile.read32(0x300) == 0x0F0F0F0F);

    // Write more 1s - should OR with existing value
    regfile.write32(0x300, 0xF0F0F0F0);
    assert(regfile.read32(0x300) == 0xFFFFFFFF);
  }

  return 0;
}

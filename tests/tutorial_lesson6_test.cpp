#include <cassert>
#include <iostream>

#include "nic/register.h"
#include "nic/trace.h"

using namespace nic;

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 6: Adding a New Hardware Register ===\n\n";

  RegisterFile registers;

  // Add standard registers
  std::cout << "--- Adding Registers ---\n";

  // Control register (RW)
  registers.add_register(RegisterDef{
      .name = "CTRL",
      .offset = 0x0000,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });
  std::cout << "Added CTRL @ 0x0000 (RW)\n";

  // Status register (RO)
  registers.add_register(RegisterDef{
      .name = "STATUS",
      .offset = 0x0004,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RO,
      .reset_value = 0x00000003,  // Link up + speed
      .write_mask = 0x00000000,
  });
  std::cout << "Added STATUS @ 0x0004 (RO)\n";

  // Interrupt Cause (RC - Read-to-Clear)
  registers.add_register(RegisterDef{
      .name = "ICR",
      .offset = 0x0010,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RC,
      .reset_value = 0x00000000,
  });
  std::cout << "Added ICR @ 0x0010 (RC)\n";

  // Interrupt Mask Set (RW1S - Write-1-to-Set)
  registers.add_register(RegisterDef{
      .name = "IMS",
      .offset = 0x0014,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW1S,
      .reset_value = 0x00000000,
  });
  std::cout << "Added IMS @ 0x0014 (RW1S)\n";

  // Custom packet counter register (RW with callback)
  registers.add_register(RegisterDef{
      .name = "PKT_COUNT",
      .offset = 0x0100,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW,
      .reset_value = 0x00000000,
  });
  std::cout << "Added PKT_COUNT @ 0x0100 (RW)\n";

  // Reset all registers
  registers.reset();
  std::cout << "\nRegisters reset to defaults\n";

  // Set up callback
  std::cout << "\n--- Setting Up Callback ---\n";
  int callback_count = 0;
  registers.set_write_callback(
      [&](std::uint32_t offset, std::uint64_t old_val, std::uint64_t new_val) {
        callback_count++;
        std::cout << "  [Callback] Write to 0x" << std::hex << offset << ": 0x" << old_val
                  << " -> 0x" << new_val << std::dec << "\n";

        // Special handling for CTRL register bit 0 (reset bit)
        if (offset == 0x0000 && (new_val & 0x01)) {
          std::cout << "  [Callback] RESET triggered!\n";
        }
      });

  // Test RW register
  std::cout << "\n--- Testing RW (CTRL) ---\n";
  std::cout << "Read CTRL: 0x" << std::hex << registers.read32(0x0000) << std::dec << "\n";
  registers.write32(0x0000, 0x12345678);
  std::cout << "Write CTRL: 0x12345678\n";
  std::cout << "Read CTRL: 0x" << std::hex << registers.read32(0x0000) << std::dec << "\n";

  // Test RO register (writes should be ignored)
  std::cout << "\n--- Testing RO (STATUS) ---\n";
  std::cout << "Read STATUS: 0x" << std::hex << registers.read32(0x0004) << std::dec << "\n";
  registers.write32(0x0004, 0xFFFFFFFF);
  std::cout << "Write STATUS: 0xFFFFFFFF (should be ignored)\n";
  std::cout << "Read STATUS: 0x" << std::hex << registers.read32(0x0004) << std::dec << "\n";

  // Test RW1S register
  std::cout << "\n--- Testing RW1S (IMS) ---\n";
  std::cout << "Read IMS: 0x" << std::hex << registers.read32(0x0014) << std::dec << "\n";
  registers.write32(0x0014, 0x0003);  // Set bits 0 and 1
  std::cout << "Write IMS: 0x0003 (set bits 0,1)\n";
  std::cout << "Read IMS: 0x" << std::hex << registers.read32(0x0014) << std::dec << "\n";
  registers.write32(0x0014, 0x0004);  // Set bit 2 (bits 0,1 should stay)
  std::cout << "Write IMS: 0x0004 (set bit 2)\n";
  std::cout << "Read IMS: 0x" << std::hex << registers.read32(0x0014) << std::dec << "\n";

  // Trigger reset via callback
  std::cout << "\n--- Triggering Reset via Callback ---\n";
  registers.write32(0x0000, 0x00000001);  // Set reset bit

  std::cout << "\nTotal callback invocations: " << callback_count << "\n";

  std::cout << "\n*** Lesson 6 Complete! ***\n";
  return 0;
}

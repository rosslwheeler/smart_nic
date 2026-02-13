#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <tracy/Tracy.hpp>

#include "nic/register.h"
#include "nic/trace.h"

static void WaitForTracyConnection();

// ---------------------------------------------------------------------------
// Test: RC (Read-to-Clear) register ignores writes, reads return value
// ---------------------------------------------------------------------------

static void test_rc_register() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_rc_register... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "TEST_RC",
      .offset = 0x100,
      .width = nic::RegisterWidth::Bits32,
      .access = nic::RegisterAccess::RC,
      .reset_value = 0x0000DEAD,
      .write_mask = 0x00000000,
  });

  regfile.reset();

  // Read should return the reset value
  assert(regfile.read32(0x100) == 0x0000DEAD);

  // Write to RC register -- should have no effect (apply_write returns old_value)
  regfile.write32(0x100, 0x12345678);
  assert(regfile.read32(0x100) == 0x0000DEAD);

  // Write a different value -- still no effect
  regfile.write32(0x100, 0xFFFFFFFF);
  assert(regfile.read32(0x100) == 0x0000DEAD);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: WO (Write-only) register reads as 0, writes are accepted
// ---------------------------------------------------------------------------

static void test_wo_register() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_wo_register... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "TEST_WO",
      .offset = 0x200,
      .width = nic::RegisterWidth::Bits32,
      .access = nic::RegisterAccess::WO,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  regfile.reset();

  // Read from WO register should return 0
  assert(regfile.read32(0x200) == 0);

  // Write a value
  regfile.write32(0x200, 0xABCD1234);

  // Read should still return 0 (write-only)
  assert(regfile.read32(0x200) == 0);

  // Write another value to confirm writes are accepted (no crash/assert)
  regfile.write32(0x200, 0x00FF00FF);
  assert(regfile.read32(0x200) == 0);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: 64-bit read/write round-trip
// ---------------------------------------------------------------------------

static void test_read64_write64() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_read64_write64... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "TEST_RW64",
      .offset = 0x300,
      .width = nic::RegisterWidth::Bits64,
      .access = nic::RegisterAccess::RW,
      .reset_value = 0x0000000000000000,
      .write_mask = 0xFFFFFFFFFFFFFFFF,
  });

  regfile.reset();

  // Reset value should be 0
  assert(regfile.read64(0x300) == 0x0000000000000000);

  // Write a 64-bit value and read it back
  regfile.write64(0x300, 0xDEADBEEFCAFEBABE);
  assert(regfile.read64(0x300) == 0xDEADBEEFCAFEBABE);

  // Write another value
  regfile.write64(0x300, 0x1234567890ABCDEF);
  assert(regfile.read64(0x300) == 0x1234567890ABCDEF);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: read64 on unmapped offset returns all-ones
// ---------------------------------------------------------------------------

static void test_read64_unmapped() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_read64_unmapped... " << std::flush;

  nic::RegisterFile regfile;

  // No registers defined -- all offsets are unmapped
  std::uint64_t value = regfile.read64(0x9999);
  assert(value == 0xFFFFFFFFFFFFFFFF);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: write64 to unmapped offset is silently ignored
// ---------------------------------------------------------------------------

static void test_write64_unmapped() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write64_unmapped... " << std::flush;

  nic::RegisterFile regfile;

  // No registers defined -- write should not crash
  regfile.write64(0x9999, 0xDEADBEEF);

  // Confirm the offset is still unmapped (reads all-ones)
  assert(regfile.read64(0x9999) == 0xFFFFFFFFFFFFFFFF);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: write64 to read-only register is ignored
// ---------------------------------------------------------------------------

static void test_write64_readonly() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write64_readonly... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "TEST_RO64",
      .offset = 0x400,
      .width = nic::RegisterWidth::Bits64,
      .access = nic::RegisterAccess::RO,
      .reset_value = 0x00000000CAFEBABE,
      .write_mask = 0x0000000000000000,
  });

  regfile.reset();

  // Confirm reset value
  assert(regfile.read64(0x400) == 0x00000000CAFEBABE);

  // Attempt to write -- should be ignored
  regfile.write64(0x400, 0xFFFFFFFFFFFFFFFF);
  assert(regfile.read64(0x400) == 0x00000000CAFEBABE);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: write callback fires with correct old/new values
// ---------------------------------------------------------------------------

static void test_write_callback() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write_callback... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "TEST_CB",
      .offset = 0x500,
      .width = nic::RegisterWidth::Bits32,
      .access = nic::RegisterAccess::RW,
      .reset_value = 0x00001111,
      .write_mask = 0xFFFFFFFF,
  });

  regfile.reset();

  bool callback_fired = false;
  std::uint32_t captured_offset = 0;
  std::uint64_t captured_old = 0;
  std::uint64_t captured_new = 0;

  regfile.set_write_callback(
      [&](std::uint32_t offset, std::uint64_t old_value, std::uint64_t new_value) {
        callback_fired = true;
        captured_offset = offset;
        captured_old = old_value;
        captured_new = new_value;
      });

  // Write to the register
  regfile.write32(0x500, 0x22222222);

  assert(callback_fired);
  assert(captured_offset == 0x500);
  assert(captured_old == 0x00001111);
  assert(captured_new == 0x22222222);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: has_register returns true for defined, false for undefined
// ---------------------------------------------------------------------------

static void test_has_register() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_has_register... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "DEFINED_REG",
      .offset = 0x600,
      .width = nic::RegisterWidth::Bits32,
      .access = nic::RegisterAccess::RW,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  assert(regfile.has_register(0x600) == true);
  assert(regfile.has_register(0x604) == false);
  assert(regfile.has_register(0x0000) == false);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: get_register_def returns correct def or nullptr
// ---------------------------------------------------------------------------

static void test_get_register_def() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_get_register_def... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "INSPECTABLE",
      .offset = 0x700,
      .width = nic::RegisterWidth::Bits64,
      .access = nic::RegisterAccess::RW1C,
      .reset_value = 0xFF00FF00,
      .write_mask = 0xFFFFFFFF,
  });

  // Defined register should return valid pointer
  const nic::RegisterDef* def = regfile.get_register_def(0x700);
  assert(def != nullptr);
  assert(def->name == "INSPECTABLE");
  assert(def->offset == 0x700);
  assert(def->width == nic::RegisterWidth::Bits64);
  assert(def->access == nic::RegisterAccess::RW1C);
  assert(def->reset_value == 0xFF00FF00);

  // Undefined register should return nullptr
  const nic::RegisterDef* undef = regfile.get_register_def(0x9999);
  assert(undef == nullptr);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: WO register with read64 returns 0
// ---------------------------------------------------------------------------

static void test_wo_read64() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_wo_read64... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "TEST_WO64",
      .offset = 0x800,
      .width = nic::RegisterWidth::Bits64,
      .access = nic::RegisterAccess::WO,
      .reset_value = 0x0000000000000000,
      .write_mask = 0xFFFFFFFFFFFFFFFF,
  });

  regfile.reset();

  // Write a 64-bit value
  regfile.write64(0x800, 0xAAAABBBBCCCCDDDD);

  // Read64 on WO register should return 0
  assert(regfile.read64(0x800) == 0);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Test: write_mask restricts which bits can be written
// ---------------------------------------------------------------------------

static void test_write_mask() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write_mask... " << std::flush;

  nic::RegisterFile regfile;

  regfile.add_register(nic::RegisterDef{
      .name = "MASKED_REG",
      .offset = 0x900,
      .width = nic::RegisterWidth::Bits32,
      .access = nic::RegisterAccess::RW,
      .reset_value = 0x00000000,
      .write_mask = 0x0000FFFF,  // Only lower 16 bits writable
  });

  regfile.reset();

  // Write all-ones -- only lower 16 bits should change
  regfile.write32(0x900, 0xFFFFFFFF);
  assert(regfile.read32(0x900) == 0x0000FFFF);

  // Write a pattern -- upper bits preserved (0), lower bits updated
  regfile.write32(0x900, 0xABCD1234);
  assert(regfile.read32(0x900) == 0x00001234);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  std::cout << "\n=== Register Coverage Tests ===\n\n";

  test_rc_register();
  test_wo_register();
  test_read64_write64();
  test_read64_unmapped();
  test_write64_unmapped();
  test_write64_readonly();
  test_write_callback();
  test_has_register();
  test_get_register_def();
  test_wo_read64();
  test_write_mask();

  std::cout << "\n=== All register coverage tests passed! ===\n\n";
  return 0;
}

static void WaitForTracyConnection() {
  const char* wait_env = std::getenv("NIC_WAIT_FOR_TRACY");
  if (!wait_env || wait_env[0] == '\0' || wait_env[0] == '0') {
    return;
  }
  const auto timeout = std::chrono::seconds(2);
  const auto start = std::chrono::steady_clock::now();
  while (!tracy::GetProfiler().IsConnected()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

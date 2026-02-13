#include <cassert>
#include <chrono>
#include <client/TracyProfiler.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <tracy/Tracy.hpp>

#include "bit_fields/bitstream.h"
#include "nic/bar.h"
#include "nic/capability.h"
#include "nic/config_space.h"
#include "nic/pcie_formats.h"
#include "nic/trace.h"

using bit_fields::BitReader;
using bit_fields::ExpectedCheck;
using bit_fields::ExpectedTable;
using bit_fields::WireOrder;

static void WaitForTracyConnection();

// Helper to create an initialized ConfigSpace with capabilities.
static nic::ConfigSpace make_initialized_config_space() {
  nic::ConfigSpace cs;
  nic::BarArray bars{};
  nic::CapabilityList caps;
  // Add standard capabilities so capabilities_list status bit is meaningful.
  caps.standard.push_back(nic::Capability{
      .id = nic::CapabilityId::PowerManagement,
      .offset = 0x40,
      .next = 0x50,
      .length = 8,
  });
  caps.standard.push_back(nic::Capability{
      .id = nic::CapabilityId::MSIX,
      .offset = 0x50,
      .next = 0x00,
      .length = 12,
  });
  cs.initialize(0x8086, 0x1572, 0x01, bars, caps);
  return cs;
}

// Helper: build a BitReader over the command register bytes in config space.
static BitReader<WireOrder::LittleEndian> command_reader(const nic::ConfigSpace& cs) {
  auto buffer = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(&cs.data()[nic::config_offset::kCommand]),
      nic::pcie::kCommandRegisterSize);
  return BitReader<WireOrder::LittleEndian>(buffer);
}

// Helper: build a BitReader over the status register bytes in config space.
static BitReader<WireOrder::LittleEndian> status_reader(const nic::ConfigSpace& cs) {
  auto buffer = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(&cs.data()[nic::config_offset::kStatus]),
      nic::pcie::kStatusRegisterSize);
  return BitReader<WireOrder::LittleEndian>(buffer);
}

// ---------------------------------------------------------------------------
// is_command_bit_set tests
// ---------------------------------------------------------------------------

static void test_is_command_bit_set_valid() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_is_command_bit_set_valid... " << std::flush;

  auto cs = make_initialized_config_space();

  // After initialize, interrupt_disable should be set (command starts with it).
  assert(cs.is_command_bit_set("interrupt_disable") == true);
  // bus_master_enable should NOT be set initially.
  assert(cs.is_command_bit_set("bus_master_enable") == false);
  assert(cs.is_command_bit_set("memory_space_enable") == false);
  assert(cs.is_command_bit_set("io_space_enable") == false);

  // Cross-check with bit_fields: deserialize raw command register and verify.
  auto reader = command_reader(cs);
  auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);
  constexpr ExpectedTable<4> kExpected{{
      {"interrupt_disable", 1},
      {"bus_master_enable", 0},
      {"memory_space_enable", 0},
      {"io_space_enable", 0},
  }};
  assert(reader.verify_expected(parsed, kExpected));

  std::cout << "PASSED\n";
}

static void test_is_command_bit_set_invalid_field() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_is_command_bit_set_invalid_field... " << std::flush;

  auto cs = make_initialized_config_space();
  // Invalid field name should return false.
  assert(cs.is_command_bit_set("nonexistent_field") == false);
  assert(cs.is_command_bit_set("") == false);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// set_command_bit tests
// ---------------------------------------------------------------------------

static void test_set_command_bit() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_set_command_bit... " << std::flush;

  auto cs = make_initialized_config_space();

  // Enable bus_master.
  assert(!cs.is_command_bit_set("bus_master_enable"));
  cs.set_command_bit("bus_master_enable", true);
  assert(cs.is_command_bit_set("bus_master_enable"));

  // Enable memory_space.
  cs.set_command_bit("memory_space_enable", true);
  assert(cs.is_command_bit_set("memory_space_enable"));

  // Disable interrupt_disable.
  cs.set_command_bit("interrupt_disable", false);
  assert(!cs.is_command_bit_set("interrupt_disable"));

  // Previously set bits should be preserved.
  assert(cs.is_command_bit_set("bus_master_enable"));
  assert(cs.is_command_bit_set("memory_space_enable"));

  // Verify the entire command register state with bit_fields assert_expected.
  auto reader = command_reader(cs);
  auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);
  constexpr ExpectedTable<4> kExpected{{
      {"bus_master_enable", 1},
      {"memory_space_enable", 1},
      {"interrupt_disable", 0},
      {"io_space_enable", 0},
  }};
  reader.assert_expected(parsed, kExpected);

  std::cout << "PASSED\n";
}

static void test_set_command_bit_invalid_field() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_set_command_bit_invalid_field... " << std::flush;

  auto cs = make_initialized_config_space();
  bool was_set = cs.is_command_bit_set("interrupt_disable");
  // Setting an invalid field should silently do nothing.
  cs.set_command_bit("nonexistent_field", true);
  cs.set_command_bit("", false);
  // Existing bits should be unchanged.
  assert(cs.is_command_bit_set("interrupt_disable") == was_set);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// get_status_field tests
// ---------------------------------------------------------------------------

static void test_get_status_field_valid() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_get_status_field_valid... " << std::flush;

  auto cs = make_initialized_config_space();
  // After initialize, capabilities_list should be set (1).
  assert(cs.get_status_field("capabilities_list") == 1);
  // interrupt_status should be 0.
  assert(cs.get_status_field("interrupt_status") == 0);

  // Cross-check with bit_fields: deserialize raw status register and verify.
  auto reader = status_reader(cs);
  auto parsed = reader.deserialize(nic::pcie::kStatusRegisterFormat);
  constexpr ExpectedTable<2> kExpected{{
      {"capabilities_list", 1},
      {"interrupt_status", 0},
  }};
  assert(reader.verify_expected(parsed, kExpected));

  std::cout << "PASSED\n";
}

static void test_get_status_field_invalid() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_get_status_field_invalid... " << std::flush;

  auto cs = make_initialized_config_space();
  assert(cs.get_status_field("nonexistent") == 0);
  assert(cs.get_status_field("") == 0);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// write16 tests
// ---------------------------------------------------------------------------

static void test_write16_writable() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write16_writable... " << std::flush;

  auto cs = make_initialized_config_space();
  // Write to cache line size (offset 0x0C, writable).
  cs.write16(nic::config_offset::kCacheLineSize, 0x4010);
  assert(cs.read16(nic::config_offset::kCacheLineSize) == 0x4010);

  std::cout << "PASSED\n";
}

static void test_write16_readonly() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write16_readonly... " << std::flush;

  auto cs = make_initialized_config_space();
  std::uint16_t original = cs.read16(nic::config_offset::kVendorId);
  cs.write16(nic::config_offset::kVendorId, 0xBEEF);
  assert(cs.read16(nic::config_offset::kVendorId) == original);

  std::cout << "PASSED\n";
}

static void test_write16_boundary() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write16_boundary... " << std::flush;

  auto cs = make_initialized_config_space();
  // offset + 1 >= kConfigSpaceSize, write should be ignored.
  cs.write16(nic::kConfigSpaceSize - 1, 0x1234);
  // Read at that boundary also returns 0xFFFF.
  assert(cs.read16(nic::kConfigSpaceSize - 1) == 0xFFFF);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// write32 tests
// ---------------------------------------------------------------------------

static void test_write32_writable() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write32_writable... " << std::flush;

  auto cs = make_initialized_config_space();
  // Write to subsystem vendor ID area (offset 0x2C, writable).
  cs.write32(nic::config_offset::kSubsystemVendorId, 0xDEADBEEF);
  assert(cs.read32(nic::config_offset::kSubsystemVendorId) == 0xDEADBEEF);

  std::cout << "PASSED\n";
}

static void test_write32_readonly() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write32_readonly... " << std::flush;

  auto cs = make_initialized_config_space();
  std::uint32_t original = cs.read32(nic::config_offset::kVendorId);
  cs.write32(nic::config_offset::kVendorId, 0xBADCAFE0);
  assert(cs.read32(nic::config_offset::kVendorId) == original);

  std::cout << "PASSED\n";
}

static void test_write32_boundary() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_write32_boundary... " << std::flush;

  auto cs = make_initialized_config_space();
  // offset + 3 >= kConfigSpaceSize, write should be ignored.
  cs.write32(nic::kConfigSpaceSize - 2, 0x12345678);
  // Read at that boundary returns 0xFFFFFFFF.
  assert(cs.read32(nic::kConfigSpaceSize - 2) == 0xFFFFFFFF);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// read boundary tests
// ---------------------------------------------------------------------------

static void test_read16_edge_boundary() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_read16_edge_boundary... " << std::flush;

  auto cs = make_initialized_config_space();
  // Offset 4094: offset+1 = 4095 < 4096, should succeed.
  std::uint16_t val = cs.read16(nic::kConfigSpaceSize - 2);
  (void) val;  // valid read, value unimportant
  // Offset 4095: offset+1 = 4096 >= 4096, should return 0xFFFF.
  assert(cs.read16(nic::kConfigSpaceSize - 1) == 0xFFFF);

  std::cout << "PASSED\n";
}

static void test_read32_edge_boundary() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_read32_edge_boundary... " << std::flush;

  auto cs = make_initialized_config_space();
  // Offset 4092: offset+3 = 4095 < 4096, should succeed.
  std::uint32_t val = cs.read32(nic::kConfigSpaceSize - 4);
  (void) val;  // valid read, value unimportant
  // Offset 4093: offset+3 = 4096 >= 4096, should return 0xFFFFFFFF.
  assert(cs.read32(nic::kConfigSpaceSize - 3) == 0xFFFFFFFF);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Additional command register field tests using bit_fields verify/assert
// ---------------------------------------------------------------------------

static void test_set_command_bit_multiple_fields() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_set_command_bit_multiple_fields... " << std::flush;

  auto cs = make_initialized_config_space();

  // Enable several command bits.
  cs.set_command_bit("io_space_enable", true);
  cs.set_command_bit("serr_enable", true);
  cs.set_command_bit("parity_error_response", true);

  // Verify all fields atomically using bit_fields deserialize + assert_expected.
  {
    auto reader = command_reader(cs);
    auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);
    constexpr ExpectedTable<5> kExpected{{
        {"io_space_enable", 1},
        {"serr_enable", 1},
        {"parity_error_response", 1},
        {"interrupt_disable", 1},
        {"bus_master_enable", 0},
    }};
    reader.assert_expected(parsed, kExpected);
  }

  // Disable one and verify others remain using verify_expected.
  cs.set_command_bit("serr_enable", false);
  {
    auto reader = command_reader(cs);
    auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);
    constexpr ExpectedTable<3> kAfterDisable{{
        {"serr_enable", 0},
        {"io_space_enable", 1},
        {"parity_error_response", 1},
    }};
    assert(reader.verify_expected(parsed, kAfterDisable));
  }

  std::cout << "PASSED\n";
}

static void test_get_status_field_multiple() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_get_status_field_multiple... " << std::flush;

  auto cs = make_initialized_config_space();

  // Verify several status fields after initialization using bit_fields.
  auto reader = status_reader(cs);
  auto parsed = reader.deserialize(nic::pcie::kStatusRegisterFormat);
  constexpr ExpectedTable<6> kExpected{{
      {"capabilities_list", 1},
      {"mhz_66_capable", 0},
      {"fast_b2b_capable", 0},
      {"master_data_parity_error", 0},
      {"signaled_target_abort", 0},
      {"detected_parity_error", 0},
  }};
  reader.assert_expected(parsed, kExpected);

  // Also verify using predicate-based checks.
  struct IsZero {
    bool operator()(std::uint64_t value) const { return value == 0; }
  };
  constexpr std::array<ExpectedCheck<IsZero>, 3> kZeroChecks{{
      {"interrupt_status", IsZero{}},
      {"mhz_66_capable", IsZero{}},
      {"fast_b2b_capable", IsZero{}},
  }};
  assert(reader.verify_expected(parsed, kZeroChecks));

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Round-trip: set_command_bit then verify raw bytes with bit_fields
// ---------------------------------------------------------------------------

static void test_command_register_round_trip() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "test_command_register_round_trip... " << std::flush;

  auto cs = make_initialized_config_space();

  // Set a known combination of command bits.
  cs.set_command_bit("bus_master_enable", true);
  cs.set_command_bit("memory_space_enable", true);
  cs.set_command_bit("interrupt_disable", false);
  cs.set_command_bit("fast_b2b_enable", true);

  // Deserialize the raw command register and verify every field.
  auto reader = command_reader(cs);
  auto parsed = reader.deserialize(nic::pcie::kCommandRegisterFormat);

  // Use verify_expected for the fields we set.
  constexpr ExpectedTable<4> kSet{{
      {"bus_master_enable", 1},
      {"memory_space_enable", 1},
      {"interrupt_disable", 0},
      {"fast_b2b_enable", 1},
  }};
  assert(reader.verify_expected(parsed, kSet));

  // Use predicate checks to verify fields we didn't touch are still zero.
  struct IsZero {
    bool operator()(std::uint64_t value) const { return value == 0; }
  };
  constexpr std::array<ExpectedCheck<IsZero>, 4> kZeroChecks{{
      {"io_space_enable", IsZero{}},
      {"special_cycles", IsZero{}},
      {"mem_write_invalidate", IsZero{}},
      {"vga_palette_snoop", IsZero{}},
  }};
  reader.assert_expected(parsed, kZeroChecks);

  std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  NIC_TRACE_SCOPED(__func__);
  WaitForTracyConnection();

  std::cout << "\n=== Config Space Coverage Tests ===\n\n";

  test_is_command_bit_set_valid();
  test_is_command_bit_set_invalid_field();
  test_set_command_bit();
  test_set_command_bit_invalid_field();
  test_get_status_field_valid();
  test_get_status_field_invalid();
  test_write16_writable();
  test_write16_readonly();
  test_write16_boundary();
  test_write32_writable();
  test_write32_readonly();
  test_write32_boundary();
  test_read16_edge_boundary();
  test_read32_edge_boundary();
  test_set_command_bit_multiple_fields();
  test_get_status_field_multiple();
  test_command_register_round_trip();

  std::cout << "\n=== All config space coverage tests passed! ===\n\n";
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

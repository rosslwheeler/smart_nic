#include "nic/bar.h"

#include <cassert>

#include "nic/config_space.h"
#include "nic/device.h"

int main() {
  // Test 1: Default BARs configuration
  {
    auto bars = nic::MakeDefaultBars();

    // BAR0 should be 64-bit memory, 64KB
    assert(bars[0].is_enabled());
    assert(bars[0].is_memory());
    assert(bars[0].is_64bit());
    assert(bars[0].size == 64 * 1024);
    assert(!bars[0].prefetchable);

    // BAR1 should be disabled (consumed by BAR0's upper 32 bits)
    assert(!bars[1].is_enabled());

    // BAR2 should be 64-bit memory, 16KB
    assert(bars[2].is_enabled());
    assert(bars[2].is_memory());
    assert(bars[2].is_64bit());
    assert(bars[2].size == 16 * 1024);

    // BAR3 should be disabled (consumed by BAR2's upper 32 bits)
    assert(!bars[3].is_enabled());

    // BAR4, BAR5 should be disabled
    assert(!bars[4].is_enabled());
    assert(!bars[5].is_enabled());
  }

  // Test 2: BAR type classification
  {
    nic::Bar mem32_bar{
        .base_address = 0x1000,
        .size = 4096,
        .type = nic::BarType::Memory32,
        .prefetchable = false,
    };
    assert(mem32_bar.is_enabled());
    assert(mem32_bar.is_memory());
    assert(!mem32_bar.is_64bit());

    nic::Bar mem64_bar{
        .base_address = 0x100000000ULL,
        .size = 4096,
        .type = nic::BarType::Memory64,
        .prefetchable = true,
    };
    assert(mem64_bar.is_enabled());
    assert(mem64_bar.is_memory());
    assert(mem64_bar.is_64bit());

    nic::Bar io_bar{
        .base_address = 0x1000,
        .size = 256,
        .type = nic::BarType::IO,
        .prefetchable = false,
    };
    assert(io_bar.is_enabled());
    assert(!io_bar.is_memory());

    nic::Bar disabled_bar{};
    assert(!disabled_bar.is_enabled());
  }

  // Test 3: BARs appear in config space
  {
    nic::DeviceConfig config{};
    nic::Device device{config};
    device.reset();

    // Read BAR0 from config space
    std::uint32_t bar0 = device.read_config32(nic::config_offset::kBar0);

    // BAR0 should indicate 64-bit memory (bits [2:1] = 10b = 0x4)
    assert((bar0 & 0x06) == 0x04);  // Type = 64-bit
    assert((bar0 & 0x01) == 0x00);  // Memory space (not I/O)

    // Read BAR2 from config space
    std::uint32_t bar2 = device.read_config32(nic::config_offset::kBar2);

    // BAR2 should also indicate 64-bit memory
    assert((bar2 & 0x06) == 0x04);
    assert((bar2 & 0x01) == 0x00);
  }

  // Test 4: Custom BAR configuration
  {
    nic::DeviceConfig config{};

    // Configure BAR4 as 32-bit I/O space
    config.bars[4] = nic::Bar{
        .base_address = 0x3000,
        .size = 256,
        .type = nic::BarType::IO,
        .prefetchable = false,
    };

    nic::Device device{config};
    device.reset();

    std::uint32_t bar4 = device.read_config32(nic::config_offset::kBar4);

    // BAR4 should indicate I/O space (bit 0 = 1)
    assert((bar4 & 0x01) == 0x01);
  }

  return 0;
}

#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#include "nic/dma_engine.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;

// Custom host memory that logs all operations
class TracingHostMemory final : public HostMemory {
public:
  explicit TracingHostMemory(HostMemoryConfig config)
    : config_(config), buffer_(config.size_bytes) {
    std::cout << "[TRACE] Created TracingHostMemory: " << config.size_bytes << " bytes\n";
  }

  [[nodiscard]] HostMemoryConfig config() const noexcept override { return config_; }

  [[nodiscard]] HostMemoryResult translate(HostAddress address,
                                           std::size_t length,
                                           HostMemoryView& view) override {
    std::cout << "[TRACE] translate: addr=0x" << std::hex << address << " len=" << std::dec
              << length << "\n";

    if (address + length > buffer_.size()) {
      std::cout << "[TRACE]   -> OUT_OF_BOUNDS\n";
      return {HostMemoryError::OutOfBounds, 0};
    }

    view.data = buffer_.data() + address;
    view.length = length;
    view.address = address;
    return {HostMemoryError::None, length};
  }

  [[nodiscard]] HostMemoryResult translate_const(HostAddress address,
                                                 std::size_t length,
                                                 ConstHostMemoryView& view) const override {
    std::cout << "[TRACE] translate_const: addr=0x" << std::hex << address << " len=" << std::dec
              << length << "\n";

    if (address + length > buffer_.size()) {
      return {HostMemoryError::OutOfBounds, 0};
    }

    view.data = buffer_.data() + address;
    view.length = length;
    view.address = address;
    return {HostMemoryError::None, length};
  }

  [[nodiscard]] HostMemoryResult read(HostAddress address,
                                      std::span<std::byte> buffer) const override {
    std::cout << "[TRACE] READ:  addr=0x" << std::hex << std::setfill('0') << std::setw(4)
              << address << " len=" << std::dec << buffer.size();

    if (address + buffer.size() > buffer_.size()) {
      std::cout << " -> OUT_OF_BOUNDS\n";
      return {HostMemoryError::OutOfBounds, 0};
    }

    std::memcpy(buffer.data(), buffer_.data() + address, buffer.size());

    // Show first few bytes
    std::cout << " data=[";
    for (size_t i = 0; i < std::min(buffer.size(), size_t{8}); ++i) {
      std::cout << std::hex << std::setw(2) << static_cast<int>(buffer[i]) << " ";
    }
    if (buffer.size() > 8) {
      std::cout << "...";
    }
    std::cout << "]\n" << std::dec;

    return {HostMemoryError::None, buffer.size()};
  }

  [[nodiscard]] HostMemoryResult write(HostAddress address,
                                       std::span<const std::byte> data) override {
    std::cout << "[TRACE] WRITE: addr=0x" << std::hex << std::setfill('0') << std::setw(4)
              << address << " len=" << std::dec << data.size();

    if (address + data.size() > buffer_.size()) {
      std::cout << " -> OUT_OF_BOUNDS\n";
      return {HostMemoryError::OutOfBounds, 0};
    }

    // Show first few bytes
    std::cout << " data=[";
    for (size_t i = 0; i < std::min(data.size(), size_t{8}); ++i) {
      std::cout << std::hex << std::setw(2) << static_cast<int>(data[i]) << " ";
    }
    if (data.size() > 8) {
      std::cout << "...";
    }
    std::cout << "]\n" << std::dec;

    std::memcpy(buffer_.data() + address, data.data(), data.size());
    return {HostMemoryError::None, data.size()};
  }

private:
  HostMemoryConfig config_;
  std::vector<std::byte> buffer_;
};

int main() {
  NIC_TRACE_SCOPED(__func__);
  std::cout << "=== Lesson 3: Adding DMA Tracing ===\n\n";

  // Use our tracing memory instead of SimpleHostMemory
  HostMemoryConfig config{.size_bytes = 1024};
  TracingHostMemory memory{config};
  DMAEngine dma{memory};

  std::cout << "\n--- Single Write ---\n";
  std::vector<std::byte> write_data = {
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  (void) dma.write(0x100, write_data);

  std::cout << "\n--- Single Read ---\n";
  std::vector<std::byte> read_data(4);
  (void) dma.read(0x100, read_data);

  std::cout << "\n--- Burst Write (simulated) ---\n";
  std::vector<std::byte> burst_data(32, std::byte{0x42});
  (void) dma.write(0x200, burst_data);

  std::cout << "\n--- Out of Bounds Access ---\n";
  std::vector<std::byte> oob_data(16);
  auto result = dma.read(0x1000, oob_data);  // Beyond 1024 bytes
  std::cout << "Result: " << (result.ok() ? "OK" : "ERROR") << "\n";

  std::cout << "\n--- DMA Counters ---\n";
  const auto& counters = dma.counters();
  std::cout << "Read ops:     " << counters.read_ops << "\n";
  std::cout << "Write ops:    " << counters.write_ops << "\n";
  std::cout << "Bytes read:   " << counters.bytes_read << "\n";
  std::cout << "Bytes written:" << counters.bytes_written << "\n";
  std::cout << "Errors:       " << counters.errors << "\n";

  std::cout << "\n*** Lesson 3 Complete! ***\n";
  return 0;
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace nic {
class Device;
}

namespace nic_driver {

// MMIO register offsets (example register map for a typical NIC)
namespace RegisterOffset {
constexpr std::uint32_t DeviceControl = 0x0000;
constexpr std::uint32_t DeviceStatus = 0x0004;
constexpr std::uint32_t InterruptCause = 0x0008;
constexpr std::uint32_t InterruptMask = 0x000C;

constexpr std::uint32_t MacAddrLow = 0x0100;
constexpr std::uint32_t MacAddrHigh = 0x0104;
constexpr std::uint32_t Mtu = 0x0108;

constexpr std::uint32_t RssKey = 0x0200;               // 40 bytes (10 x 32-bit)
constexpr std::uint32_t RssIndirectionTable = 0x0300;  // 128 bytes (32 x 32-bit)
constexpr std::uint32_t RssControl = 0x0400;

constexpr std::uint32_t TxQueueBase = 0x1000;  // Base for TX queue registers
constexpr std::uint32_t RxQueueBase = 0x2000;  // Base for RX queue registers

constexpr std::uint32_t OffloadControl = 0x3000;
constexpr std::uint32_t FlowControl = 0x3100;
constexpr std::uint32_t PtpControl = 0x3200;

constexpr std::uint32_t StatsBase = 0x4000;
constexpr std::uint32_t VfControl = 0x5000;
}  // namespace RegisterOffset

// Device control register bits
namespace DeviceControlBits {
constexpr std::uint32_t Reset = 1 << 0;
constexpr std::uint32_t Enable = 1 << 1;
constexpr std::uint32_t InterruptEnable = 1 << 2;
}  // namespace DeviceControlBits

// Device status register bits
namespace DeviceStatusBits {
constexpr std::uint32_t LinkUp = 1 << 0;
constexpr std::uint32_t Ready = 1 << 1;
}  // namespace DeviceStatusBits

// Queue register offsets (relative to TxQueueBase/RxQueueBase)
namespace QueueRegisterOffset {
constexpr std::uint32_t DescriptorBase = 0x00;
constexpr std::uint32_t DescriptorBaseHigh = 0x04;
constexpr std::uint32_t DescriptorLength = 0x08;
constexpr std::uint32_t Head = 0x10;
constexpr std::uint32_t Tail = 0x14;
constexpr std::uint32_t Enable = 0x18;
}  // namespace QueueRegisterOffset

// Per-queue register stride
constexpr std::uint32_t QueueRegisterStride = 0x40;

// MmioAdapter - Abstracts MMIO register accesses and maps them to NIC model API calls
class MmioAdapter {
public:
  explicit MmioAdapter(nic::Device& device);
  ~MmioAdapter();

  // Disable copy/move
  MmioAdapter(const MmioAdapter&) = delete;
  MmioAdapter& operator=(const MmioAdapter&) = delete;
  MmioAdapter(MmioAdapter&&) = delete;
  MmioAdapter& operator=(MmioAdapter&&) = delete;

  // Register access (32-bit reads/writes)
  std::uint32_t read32(std::uint32_t offset) const;
  void write32(std::uint32_t offset, std::uint32_t value);

  // Register access (64-bit reads/writes for addresses)
  std::uint64_t read64(std::uint32_t offset) const;
  void write64(std::uint32_t offset, std::uint64_t value);

  // Batch register operations
  void read_block(std::uint32_t offset, std::uint32_t* buffer, std::size_t count) const;
  void write_block(std::uint32_t offset, const std::uint32_t* buffer, std::size_t count);

  // Helper methods for specific register operations
  void set_device_control(std::uint32_t bits);
  void clear_device_control(std::uint32_t bits);
  std::uint32_t get_device_status() const;

  void set_mac_address(std::uint32_t low, std::uint32_t high);
  void get_mac_address(std::uint32_t& low, std::uint32_t& high) const;

  void configure_queue(bool is_tx,
                       std::uint16_t queue_id,
                       std::uint64_t base_addr,
                       std::uint32_t length);
  void enable_queue(bool is_tx, std::uint16_t queue_id);
  void disable_queue(bool is_tx, std::uint16_t queue_id);

  void update_queue_tail(bool is_tx, std::uint16_t queue_id, std::uint32_t tail);
  std::uint32_t get_queue_head(bool is_tx, std::uint16_t queue_id) const;

  void configure_rss_key(const std::uint32_t* key, std::size_t key_words);
  void configure_rss_indirection_table(const std::uint32_t* table, std::size_t table_words);
  void set_rss_control(std::uint32_t hash_types);

  void set_offload_control(std::uint32_t offload_flags);
  std::uint32_t get_offload_control() const;

  void set_flow_control(std::uint32_t fc_config);
  std::uint32_t get_flow_control() const;

  void set_ptp_control(std::uint32_t ptp_flags);
  std::uint32_t get_ptp_control() const;

  // Statistics access
  std::uint64_t read_stat_counter(std::uint32_t counter_id) const;
  void clear_stat_counter(std::uint32_t counter_id);

  // SR-IOV control
  void set_vf_control(std::uint32_t vf_flags);
  std::uint32_t get_vf_control() const;

  // Interrupt management
  void mask_interrupt(std::uint32_t interrupt_bits);
  void unmask_interrupt(std::uint32_t interrupt_bits);
  std::uint32_t read_interrupt_cause() const;
  void clear_interrupt_cause(std::uint32_t interrupt_bits);

private:
  nic::Device& device_;

  // Internal helpers for register validation
  bool is_valid_offset(std::uint32_t offset) const;
  std::uint32_t get_queue_register_offset(bool is_tx,
                                          std::uint16_t queue_id,
                                          std::uint32_t reg_offset) const;
};

}  // namespace nic_driver
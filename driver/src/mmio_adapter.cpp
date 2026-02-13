#include "nic_driver/mmio_adapter.h"

#include <cassert>
#include <cstring>

#include "nic/device.h"
#include "nic/trace.h"

namespace nic_driver {

MmioAdapter::MmioAdapter(nic::Device& device) : device_{device} {
  NIC_TRACE_SCOPED(__func__);
}

MmioAdapter::~MmioAdapter() {
  NIC_TRACE_SCOPED(__func__);
}

std::uint32_t MmioAdapter::read32(std::uint32_t offset) const {
  NIC_TRACE_SCOPED(__func__);
  assert(is_valid_offset(offset));

  // In a real hardware driver, this would be:
  //   return *reinterpret_cast<volatile std::uint32_t*>(mmio_base + offset);
  //
  // For the behavioral model, we map specific offsets to device API calls.
  // For simplicity, many reads return dummy values since the model
  // doesn't actually use a register map internally.

  switch (offset) {
    case RegisterOffset::DeviceStatus:
      return DeviceStatusBits::LinkUp | DeviceStatusBits::Ready;

    case RegisterOffset::InterruptCause:
      return 0;  // No pending interrupts

    case RegisterOffset::Mtu:
      return 1500;  // Default MTU

    default:
      // For other registers, return 0 (in real HW would read actual register)
      return 0;
  }
}

void MmioAdapter::write32(std::uint32_t offset, std::uint32_t value) {
  NIC_TRACE_SCOPED(__func__);
  assert(is_valid_offset(offset));

  // In a real hardware driver, this would be:
  //   *reinterpret_cast<volatile std::uint32_t*>(mmio_base + offset) = value;
  //
  // For the behavioral model, we interpret writes and call appropriate
  // device API methods.

  switch (offset) {
    case RegisterOffset::DeviceControl:
      if (value & DeviceControlBits::Reset) {
        device_.reset();
      }
      break;

    case RegisterOffset::InterruptMask:
      // Would configure interrupt mask in real hardware
      break;

    case RegisterOffset::Mtu:
      // MTU configuration handled at higher level
      break;

    default:
      // Other registers ignored in simplified model
      break;
  }
}

std::uint64_t MmioAdapter::read64(std::uint32_t offset) const {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t low = read32(offset);
  std::uint32_t high = read32(offset + 4);
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

void MmioAdapter::write64(std::uint32_t offset, std::uint64_t value) {
  NIC_TRACE_SCOPED(__func__);
  write32(offset, static_cast<std::uint32_t>(value & 0xFFFFFFFF));
  write32(offset + 4, static_cast<std::uint32_t>(value >> 32));
}

void MmioAdapter::read_block(std::uint32_t offset, std::uint32_t* buffer, std::size_t count) const {
  NIC_TRACE_SCOPED(__func__);
  for (std::size_t reg_index = 0; reg_index < count; ++reg_index) {
    buffer[reg_index] = read32(offset + reg_index * sizeof(std::uint32_t));
  }
}

void MmioAdapter::write_block(std::uint32_t offset,
                              const std::uint32_t* buffer,
                              std::size_t count) {
  NIC_TRACE_SCOPED(__func__);
  for (std::size_t reg_index = 0; reg_index < count; ++reg_index) {
    write32(offset + reg_index * sizeof(std::uint32_t), buffer[reg_index]);
  }
}

void MmioAdapter::set_device_control(std::uint32_t bits) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t current = read32(RegisterOffset::DeviceControl);
  write32(RegisterOffset::DeviceControl, current | bits);
}

void MmioAdapter::clear_device_control(std::uint32_t bits) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t current = read32(RegisterOffset::DeviceControl);
  write32(RegisterOffset::DeviceControl, current & ~bits);
}

std::uint32_t MmioAdapter::get_device_status() const {
  NIC_TRACE_SCOPED(__func__);
  return read32(RegisterOffset::DeviceStatus);
}

void MmioAdapter::set_mac_address(std::uint32_t low, std::uint32_t high) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::MacAddrLow, low);
  write32(RegisterOffset::MacAddrHigh, high);
}

void MmioAdapter::get_mac_address(std::uint32_t& low, std::uint32_t& high) const {
  NIC_TRACE_SCOPED(__func__);
  low = read32(RegisterOffset::MacAddrLow);
  high = read32(RegisterOffset::MacAddrHigh);
}

void MmioAdapter::configure_queue(bool is_tx,
                                  std::uint16_t queue_id,
                                  std::uint64_t base_addr,
                                  std::uint32_t length) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t queue_base_offset = get_queue_register_offset(is_tx, queue_id, 0);

  write64(queue_base_offset + QueueRegisterOffset::DescriptorBase, base_addr);
  write32(queue_base_offset + QueueRegisterOffset::DescriptorLength, length);
}

void MmioAdapter::enable_queue(bool is_tx, std::uint16_t queue_id) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t queue_enable_offset =
      get_queue_register_offset(is_tx, queue_id, QueueRegisterOffset::Enable);
  write32(queue_enable_offset, 1);
}

void MmioAdapter::disable_queue(bool is_tx, std::uint16_t queue_id) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t queue_enable_offset =
      get_queue_register_offset(is_tx, queue_id, QueueRegisterOffset::Enable);
  write32(queue_enable_offset, 0);
}

void MmioAdapter::update_queue_tail(bool is_tx, std::uint16_t queue_id, std::uint32_t tail) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t queue_tail_offset =
      get_queue_register_offset(is_tx, queue_id, QueueRegisterOffset::Tail);
  write32(queue_tail_offset, tail);
}

std::uint32_t MmioAdapter::get_queue_head(bool is_tx, std::uint16_t queue_id) const {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t queue_head_offset =
      get_queue_register_offset(is_tx, queue_id, QueueRegisterOffset::Head);
  return read32(queue_head_offset);
}

void MmioAdapter::configure_rss_key(const std::uint32_t* key, std::size_t key_words) {
  NIC_TRACE_SCOPED(__func__);
  write_block(RegisterOffset::RssKey, key, key_words);
}

void MmioAdapter::configure_rss_indirection_table(const std::uint32_t* table,
                                                  std::size_t table_words) {
  NIC_TRACE_SCOPED(__func__);
  write_block(RegisterOffset::RssIndirectionTable, table, table_words);
}

void MmioAdapter::set_rss_control(std::uint32_t hash_types) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::RssControl, hash_types);
}

void MmioAdapter::set_offload_control(std::uint32_t offload_flags) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::OffloadControl, offload_flags);
}

std::uint32_t MmioAdapter::get_offload_control() const {
  NIC_TRACE_SCOPED(__func__);
  return read32(RegisterOffset::OffloadControl);
}

void MmioAdapter::set_flow_control(std::uint32_t fc_config) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::FlowControl, fc_config);
}

std::uint32_t MmioAdapter::get_flow_control() const {
  NIC_TRACE_SCOPED(__func__);
  return read32(RegisterOffset::FlowControl);
}

void MmioAdapter::set_ptp_control(std::uint32_t ptp_flags) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::PtpControl, ptp_flags);
}

std::uint32_t MmioAdapter::get_ptp_control() const {
  NIC_TRACE_SCOPED(__func__);
  return read32(RegisterOffset::PtpControl);
}

std::uint64_t MmioAdapter::read_stat_counter(std::uint32_t counter_id) const {
  NIC_TRACE_SCOPED(__func__);
  return read64(RegisterOffset::StatsBase + counter_id * sizeof(std::uint64_t));
}

void MmioAdapter::clear_stat_counter(std::uint32_t counter_id) {
  NIC_TRACE_SCOPED(__func__);
  write64(RegisterOffset::StatsBase + counter_id * sizeof(std::uint64_t), 0);
}

void MmioAdapter::set_vf_control(std::uint32_t vf_flags) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::VfControl, vf_flags);
}

std::uint32_t MmioAdapter::get_vf_control() const {
  NIC_TRACE_SCOPED(__func__);
  return read32(RegisterOffset::VfControl);
}

void MmioAdapter::mask_interrupt(std::uint32_t interrupt_bits) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t current = read32(RegisterOffset::InterruptMask);
  write32(RegisterOffset::InterruptMask, current | interrupt_bits);
}

void MmioAdapter::unmask_interrupt(std::uint32_t interrupt_bits) {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t current = read32(RegisterOffset::InterruptMask);
  write32(RegisterOffset::InterruptMask, current & ~interrupt_bits);
}

std::uint32_t MmioAdapter::read_interrupt_cause() const {
  NIC_TRACE_SCOPED(__func__);
  return read32(RegisterOffset::InterruptCause);
}

void MmioAdapter::clear_interrupt_cause(std::uint32_t interrupt_bits) {
  NIC_TRACE_SCOPED(__func__);
  write32(RegisterOffset::InterruptCause, interrupt_bits);
}

bool MmioAdapter::is_valid_offset(std::uint32_t offset) const {
  NIC_TRACE_SCOPED(__func__);
  // In real driver, would validate offset is within MMIO region
  // For behavioral model, accept all offsets
  (void) offset;
  return true;
}

std::uint32_t MmioAdapter::get_queue_register_offset(bool is_tx,
                                                     std::uint16_t queue_id,
                                                     std::uint32_t reg_offset) const {
  NIC_TRACE_SCOPED(__func__);
  std::uint32_t base = is_tx ? RegisterOffset::TxQueueBase : RegisterOffset::RxQueueBase;
  return base + queue_id * QueueRegisterStride + reg_offset;
}

}  // namespace nic_driver
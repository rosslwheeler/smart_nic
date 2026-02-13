#include "nic/tx_rx.h"

#include <cassert>
#include <cstring>
#include <vector>

#include "nic/checksum.h"
#include "nic/completion_queue.h"
#include "nic/descriptor_ring.h"
#include "nic/dma_engine.h"
#include "nic/host_memory.h"
#include "nic/queue_pair.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;

namespace {

class TestHostMemory final : public HostMemory {
public:
  explicit TestHostMemory(HostMemoryConfig config) : config_(config), buffer_(config.size_bytes) {}

  [[nodiscard]] HostMemoryConfig config() const noexcept override { return config_; }

  [[nodiscard]] HostMemoryResult translate(HostAddress address,
                                           std::size_t length,
                                           HostMemoryView& view) override {
    HostMemoryResult result = translate_common(address, length);
    if (!result.ok()) {
      return result;
    }
    view.data = buffer_.data() + static_cast<std::size_t>(address);
    view.length = length;
    view.address = address;
    return result;
  }

  [[nodiscard]] HostMemoryResult translate_const(HostAddress address,
                                                 std::size_t length,
                                                 ConstHostMemoryView& view) const override {
    HostMemoryResult result = translate_common(address, length);
    if (!result.ok()) {
      return result;
    }
    view.data = buffer_.data() + static_cast<std::size_t>(address);
    view.length = length;
    view.address = address;
    return result;
  }

  [[nodiscard]] HostMemoryResult read(HostAddress address,
                                      std::span<std::byte> buffer) const override {
    if (fail_read_) {
      return {HostMemoryError::FaultInjected, 0};
    }
    HostMemoryResult result = translate_common(address, buffer.size());
    if (!result.ok()) {
      return result;
    }
    if (!buffer.empty()) {
      std::memcpy(buffer.data(), buffer_.data() + static_cast<std::size_t>(address), buffer.size());
    }
    return result;
  }

  [[nodiscard]] HostMemoryResult write(HostAddress address,
                                       std::span<const std::byte> data) override {
    if (fail_write_) {
      return {HostMemoryError::FaultInjected, 0};
    }
    HostMemoryResult result = translate_common(address, data.size());
    if (!result.ok()) {
      return result;
    }
    if (!data.empty()) {
      std::memcpy(buffer_.data() + static_cast<std::size_t>(address), data.data(), data.size());
    }
    return result;
  }

  void set_fail_read(bool fail) { fail_read_ = fail; }
  void set_fail_write(bool fail) { fail_write_ = fail; }

private:
  HostMemoryResult translate_common(HostAddress address, std::size_t length) const {
    std::size_t offset = static_cast<std::size_t>(address);
    if (offset > buffer_.size()) {
      return {HostMemoryError::OutOfBounds, 0};
    }
    if (length > buffer_.size() - offset) {
      return {HostMemoryError::OutOfBounds, 0};
    }
    return {HostMemoryError::None, length};
  }

  HostMemoryConfig config_{};
  std::vector<std::byte> buffer_{};
  bool fail_read_{false};
  bool fail_write_{false};
};

std::vector<std::byte> make_payload(std::size_t size) {
  NIC_TRACE_SCOPED(__func__);
  std::vector<std::byte> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = std::byte{static_cast<unsigned char>(i & 0xFF)};
  }
  return data;
}

std::vector<std::byte> serialize_tx(const TxDescriptor& desc) {
  NIC_TRACE_SCOPED(__func__);
  std::vector<std::byte> bytes(sizeof(TxDescriptor));
  std::memcpy(bytes.data(), &desc, sizeof(TxDescriptor));
  return bytes;
}

std::vector<std::byte> serialize_rx(const RxDescriptor& desc) {
  NIC_TRACE_SCOPED(__func__);
  std::vector<std::byte> bytes(sizeof(RxDescriptor));
  std::memcpy(bytes.data(), &desc, sizeof(RxDescriptor));
  return bytes;
}

void test_tx_rx_success() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 4,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 4,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 4, .queue_id = 0},
      .rx_completion = {.ring_size = 4, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(16);
  HostAddress tx_addr = 100;
  HostAddress rx_addr = 200;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 0,
      .checksum_value = compute_checksum(payload),
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 32,
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 0,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(!tx_comp->tso_performed);
  assert(!tx_comp->gso_performed);
  assert(tx_comp->segments_produced == 1);
  assert(!rx_comp->vlan_stripped);
  assert(!rx_comp->checksum_verified);
  assert(qp.stats().tx_packets == 1);
  assert(qp.stats().rx_packets == 1);

  std::vector<std::byte> rx_out(payload.size());
  assert(mem.read(rx_addr, rx_out).ok());
  assert(rx_out == payload);
}

void test_checksum_drop() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(8);
  HostAddress tx_addr = 50;
  HostAddress rx_addr = 100;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 1,
      .checksum_value = static_cast<std::uint16_t>(compute_checksum(payload) ^ 0xFFFF),
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 16,
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 2,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(!rx_comp.has_value());  // RX not posted on checksum drop
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::ChecksumError));
  assert(qp.stats().drops_checksum == 1);
}

void test_buffer_too_small_drop() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(32);
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 100;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 3,
      .checksum_value = 0,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 8,  // too small
      .checksum = ChecksumMode::None,
      .descriptor_index = 4,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::BufferTooSmall));
  assert(qp.stats().drops_buffer_small == 1);
}

void test_checksum_offload_bypass() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(8);
  HostAddress tx_addr = 50;
  HostAddress rx_addr = 100;
  assert(mem.write(tx_addr, payload).ok());

  // Enable checksum_offload to bypass mismatch.
  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 5,
      .checksum_value = static_cast<std::uint16_t>(compute_checksum(payload) ^ 0xFFFF),
      .checksum_offload = true,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 16,
      .checksum = ChecksumMode::None,
      .descriptor_index = 5,
      .checksum_offload = false,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(tx_comp->checksum_offloaded);
  assert(tx_comp->segments_produced == 1);
  assert(!tx_comp->tso_performed);
  assert(!tx_comp->gso_performed);
  assert(!rx_comp->checksum_verified);
}

void test_checksum_offload_failure() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(6);  // checksum unlikely to be zero
  HostAddress tx_addr = 40;
  HostAddress rx_addr = 80;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 14,
      .checksum_value = 0,
      .checksum_offload = true,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 6,
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 15,
      .checksum_offload = true,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(tx_comp->checksum_offloaded);
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::ChecksumError));
  assert(rx_comp->checksum_verified);
}

void test_checksum_offload_empty_payload_failure() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 512, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload;  // empty
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 20;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = 0,
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 18,
      .checksum_value = 0,
      .checksum_offload = true,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 0,
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 19,
      .checksum_offload = true,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::ChecksumError));
  assert(rx_comp->checksum_verified);
}

void test_tso_segmentation() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 4,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 4,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 4, .queue_id = 0},
      .rx_completion = {.ring_size = 4, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(12);
  HostAddress tx_addr = 50;
  HostAddress rx_addr0 = 100;
  HostAddress rx_addr1 = 200;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 6,
      .checksum_value = 0,
      .tso_enabled = true,
      .mss = 6,
  };

  RxDescriptor rx0{
      .buffer_address = rx_addr0,
      .buffer_length = 6,
      .checksum = ChecksumMode::None,
      .descriptor_index = 6,
  };
  RxDescriptor rx1{
      .buffer_address = rx_addr1,
      .buffer_length = 6,
      .checksum = ChecksumMode::None,
      .descriptor_index = 7,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx0)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx1)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(tx_comp->tso_performed);
  assert(!tx_comp->gso_performed);
  assert(tx_comp->segments_produced == 2);
  assert(qp.rx_completion().available() == 2);

  std::vector<std::byte> out0(6);
  std::vector<std::byte> out1(6);
  assert(mem.read(rx_addr0, out0).ok());
  assert(mem.read(rx_addr1, out1).ok());
  std::vector<std::byte> expected0(payload.begin(), payload.begin() + 6);
  std::vector<std::byte> expected1(payload.begin() + 6, payload.end());
  assert(out0 == expected0);
  assert(out1 == expected1);
}

void test_gso_segmentation_with_header() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 6,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 6,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 4, .queue_id = 0},
      .rx_completion = {.ring_size = 6, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(10);
  HostAddress tx_addr = 50;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 7,
      .checksum_value = 0,
      .gso_enabled = true,
      .mss = 3,
      .header_length = 2,
  };

  HostAddress rx_addr0 = 100;
  HostAddress rx_addr1 = 200;
  HostAddress rx_addr2 = 300;
  RxDescriptor rx0{.buffer_address = rx_addr0,
                   .buffer_length = 5,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 7};
  RxDescriptor rx1{.buffer_address = rx_addr1,
                   .buffer_length = 5,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 8};
  RxDescriptor rx2{.buffer_address = rx_addr2,
                   .buffer_length = 4,
                   .checksum = ChecksumMode::None,
                   .descriptor_index = 9};

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx0)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx1)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx2)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(tx_comp->gso_performed);
  assert(!tx_comp->tso_performed);
  assert(tx_comp->segments_produced == 3);
  assert(qp.rx_completion().available() == 3);

  std::vector<std::byte> header(payload.begin(), payload.begin() + 2);
  std::vector<std::byte> expected0 = header;
  expected0.insert(expected0.end(), payload.begin() + 2, payload.begin() + 5);
  std::vector<std::byte> expected1 = header;
  expected1.insert(expected1.end(), payload.begin() + 5, payload.begin() + 8);
  std::vector<std::byte> expected2 = header;
  expected2.insert(expected2.end(), payload.begin() + 8, payload.end());

  std::vector<std::byte> out0(expected0.size());
  std::vector<std::byte> out1(expected1.size());
  std::vector<std::byte> out2(expected2.size());
  assert(mem.read(rx_addr0, out0).ok());
  assert(mem.read(rx_addr1, out1).ok());
  assert(mem.read(rx_addr2, out2).ok());
  assert(out0 == expected0);
  assert(out1 == expected1);
  assert(out2 == expected2);
}

void test_gso_invalid_mss_no_segmentation() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(4);
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 20;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 20,
      .checksum_value = 0,
      .gso_enabled = true,
      .mss = 0,             // invalid -> no segmentation
      .header_length = 10,  // larger than payload, should be clamped
  };

  RxDescriptor rx_desc{.buffer_address = rx_addr,
                       .buffer_length = 4,
                       .checksum = ChecksumMode::None,
                       .descriptor_index = 21};

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(!tx_comp->gso_performed);
  assert(!tx_comp->tso_performed);
  assert(tx_comp->segments_produced == 1);
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));

  std::vector<std::byte> out(payload.size());
  assert(mem.read(rx_addr, out).ok());
  assert(out == payload);
}

void test_vlan_insert_strip() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 4096, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(4);
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 20;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 8,
      .checksum_value = 0,
      .vlan_insert = true,
      .vlan_tag = 0x123,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 8,
      .checksum = ChecksumMode::None,
      .descriptor_index = 9,
      .vlan_strip = true,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());
  assert(qp.process_once());

  std::vector<std::byte> out(payload.size());
  assert(mem.read(rx_addr, out).ok());
  assert(out == payload);
}

void test_vlan_insert_buffer_too_small() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(10);
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 20;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 16,
      .checksum_value = 0,
      .vlan_insert = true,
      .vlan_tag = 0x0777,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 6,  // too small even after strip
      .checksum = ChecksumMode::None,
      .descriptor_index = 17,
      .vlan_strip = true,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
  assert(tx_comp->vlan_inserted);
  assert(tx_comp->vlan_tag == 0x0777);
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::BufferTooSmall));
  assert(rx_comp->vlan_stripped);
  assert(rx_comp->vlan_tag == 0x0777);
}

void test_completion_offload_metadata() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = {std::byte{0xFF}, std::byte{0xFF}};  // checksum -> 0
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 20;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 10,
      .checksum_value = 0,  // ignored due to offload
      .checksum_offload = true,
      .vlan_insert = true,
      .vlan_tag = 0x0ABC,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 2,
      .checksum = ChecksumMode::Layer4,
      .descriptor_index = 11,
      .checksum_offload = true,
      .vlan_strip = true,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(rx_comp.has_value());

  assert(tx_comp->checksum_offloaded);
  assert(tx_comp->vlan_inserted);
  assert(tx_comp->vlan_tag == 0x0ABC);
  assert(tx_comp->segments_produced == 1);

  assert(rx_comp->checksum_verified);
  assert(rx_comp->vlan_stripped);
  assert(rx_comp->vlan_tag == 0x0ABC);
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::Success));
}

void test_gro_placeholder_flag() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> payload = make_payload(4);
  HostAddress tx_addr = 10;
  HostAddress rx_addr = 20;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 12,
      .checksum_value = 0,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 4,
      .checksum = ChecksumMode::None,
      .descriptor_index = 13,
      .gro_enabled = true,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto rx_comp = qp.rx_completion().poll_completion();
  assert(rx_comp.has_value());
  assert(rx_comp->gro_aggregated);
}

void test_doorbells_and_wraparound() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  Doorbell tx_db;
  Doorbell rx_db;
  Doorbell txc_db;
  Doorbell rxc_db;
  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
      .tx_doorbell = &tx_db,
      .rx_doorbell = &rx_db,
      .tx_completion_doorbell = &txc_db,
      .rx_completion_doorbell = &rxc_db,
  };
  QueuePair qp{qp_cfg, dma};

  auto payload_a = make_payload(4);
  auto payload_b = make_payload(4);
  HostAddress tx_addr_a = 100;
  HostAddress tx_addr_b = 200;
  HostAddress rx_addr_a = 300;
  HostAddress rx_addr_b = 400;
  assert(mem.write(tx_addr_a, payload_a).ok());
  assert(mem.write(tx_addr_b, payload_b).ok());

  TxDescriptor tx_a{.buffer_address = tx_addr_a,
                    .length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 0,
                    .checksum_value = 0};
  TxDescriptor tx_b{.buffer_address = tx_addr_b,
                    .length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 1,
                    .checksum_value = 0};
  RxDescriptor rx_a{.buffer_address = rx_addr_a,
                    .buffer_length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 0};
  RxDescriptor rx_b{.buffer_address = rx_addr_b,
                    .buffer_length = 4,
                    .checksum = ChecksumMode::None,
                    .descriptor_index = 1};

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_a)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_a)).ok());
  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_b)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_b)).ok());

  assert(qp.process_once());
  assert(qp.process_once());

  assert(tx_db.rings() == 2);
  assert(rx_db.rings() == 2);
  assert(qp.tx_completion().available() == 2);
  assert(qp.rx_completion().available() == 2);
}

void test_mtu_exceeded() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 16384, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
      .max_mtu = 1500,
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(2000);  // Exceeds MTU
  HostAddress tx_addr = 100;
  HostAddress rx_addr = 200;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 22,
      .checksum_value = 0,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 2000,
      .checksum = ChecksumMode::None,
      .descriptor_index = 23,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::MtuExceeded));
  assert(qp.stats().drops_mtu_exceeded == 1);
  assert(!qp.rx_completion().poll_completion().has_value());
}

void test_invalid_mss_too_large() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 16384, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
      .max_mtu = 16000,  // Large enough to not trigger MTU check
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(12000);  // Large payload to trigger segmentation
  HostAddress tx_addr = 100;
  HostAddress rx_addr = 200;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 24,
      .checksum_value = 0,
      .tso_enabled = true,
      .mss = 10000,  // Exceeds kMaxMss (9000)
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 12000,
      .checksum = ChecksumMode::None,
      .descriptor_index = 25,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::InvalidMss));
  assert(qp.stats().drops_invalid_mss == 1);
  assert(!qp.rx_completion().poll_completion().has_value());
}

void test_too_many_segments() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 8192, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 128,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 128, .queue_id = 0},
      .max_mtu = 10000,  // Large enough to not trigger MTU check
  };
  QueuePair qp{qp_cfg, dma};

  auto payload =
      make_payload(1000);  // 1000 bytes with MSS=10 = 100 segments (exceeds kMaxTsoSegments=64)
  HostAddress tx_addr = 100;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 26,
      .checksum_value = 0,
      .tso_enabled = true,
      .mss = 10,  // Will create 100 segments
  };

  // Push enough RX descriptors for all segments
  for (std::uint16_t i = 0; i < 128; ++i) {
    RxDescriptor rx_desc{
        .buffer_address = static_cast<HostAddress>(200 + (i * 16)),
        .buffer_length = 10,
        .checksum = ChecksumMode::None,
        .descriptor_index = i,
    };
    assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());
  }

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());

  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(tx_comp->status == static_cast<std::uint32_t>(CompletionCode::TooManySegments));
  assert(qp.stats().drops_too_many_segments == 1);
}

void test_tx_decode_failure() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor) - 1,
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  std::vector<std::byte> raw(qp_cfg.tx_ring.descriptor_size, std::byte{0xAB});
  assert(qp.tx_ring().push_descriptor(raw).ok());
  assert(!qp.process_once());
}

void test_rx_decode_failure() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor) - 1,
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(8);
  HostAddress tx_addr = 0;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 30,
      .checksum_value = 0,
  };

  std::vector<std::byte> rx_raw(qp_cfg.rx_ring.descriptor_size, std::byte{0xCD});
  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(rx_raw).ok());
  assert(!qp.process_once());
}

void test_header_length_invalid() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(8);
  HostAddress tx_addr = 0;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 31,
      .checksum_value = 0,
      .gso_enabled = true,
      .mss = 4,
      .header_length = 32,  // invalid
  };

  RxDescriptor rx_desc{
      .buffer_address = 100,
      .buffer_length = 16,
      .checksum = ChecksumMode::None,
      .descriptor_index = 31,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());
  assert(qp.process_once());
  assert(qp.stats().drops_invalid_mss == 1);
}

void test_header_covers_packet() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(8);
  HostAddress tx_addr = 0;
  HostAddress rx_addr = 128;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 32,
      .checksum_value = 0,
      .tso_enabled = true,
      .mss = 4,
      .header_length = static_cast<std::uint16_t>(payload.size()),
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 16,
      .checksum = ChecksumMode::None,
      .descriptor_index = 32,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());
  assert(qp.process_once());

  auto tx_comp = qp.tx_completion().poll_completion();
  assert(tx_comp.has_value());
  assert(tx_comp->segments_produced == 1);
  assert(!tx_comp->tso_performed);
}

void test_rx_ring_insufficient_descriptors() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 1,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(8);
  HostAddress tx_addr = 0;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 33,
      .checksum_value = 0,
      .gso_enabled = true,
      .mss = 4,
  };

  RxDescriptor rx_desc{
      .buffer_address = 128,
      .buffer_length = 8,
      .checksum = ChecksumMode::None,
      .descriptor_index = 33,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());
  assert(qp.process_once());
  assert(qp.stats().drops_no_rx_desc == 1);
}

void test_tx_pop_failure() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 512, .page_size = 64, .iommu_enabled = false};
  TestHostMemory mem{config};
  DMAEngine dma{mem};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = true},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 128,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
  };
  QueuePair qp{qp_cfg, dma};

  TxDescriptor tx_desc{
      .buffer_address = 0,
      .length = 0,
      .checksum = ChecksumMode::None,
      .descriptor_index = 34,
      .checksum_value = 0,
  };
  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  mem.set_fail_read(true);
  assert(!qp.process_once());
}

void test_rx_write_failure_and_interrupts() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  TestHostMemory mem{config};
  DMAEngine dma{mem};

  MsixTable table{1};
  MsixMapping mapping{1, 0};
  mapping.set_queue_vector(0, 0);
  CoalesceConfig coalesce_cfg{.packet_threshold = 1, .timer_threshold_us = 0};
  InterruptDispatcher dispatcher{table, mapping, coalesce_cfg, [](std::uint16_t, std::uint32_t) {}};

  QueuePairConfig qp_cfg{
      .queue_id = 0,
      .tx_ring = {.descriptor_size = sizeof(TxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .rx_ring = {.descriptor_size = sizeof(RxDescriptor),
                  .ring_size = 2,
                  .base_address = 0,
                  .queue_id = 0,
                  .host_backed = false},
      .tx_completion = {.ring_size = 2, .queue_id = 0},
      .rx_completion = {.ring_size = 2, .queue_id = 0},
      .interrupt_dispatcher = &dispatcher,
      .enable_tx_interrupts = true,
      .enable_rx_interrupts = true,
  };
  QueuePair qp{qp_cfg, dma};

  auto payload = make_payload(8);
  HostAddress tx_addr = 0;
  HostAddress rx_addr = 256;
  assert(mem.write(tx_addr, payload).ok());

  TxDescriptor tx_desc{
      .buffer_address = tx_addr,
      .length = static_cast<std::uint32_t>(payload.size()),
      .checksum = ChecksumMode::None,
      .descriptor_index = 35,
      .checksum_value = 0,
  };

  RxDescriptor rx_desc{
      .buffer_address = rx_addr,
      .buffer_length = 8,
      .checksum = ChecksumMode::None,
      .descriptor_index = 35,
  };

  assert(qp.tx_ring().push_descriptor(serialize_tx(tx_desc)).ok());
  assert(qp.rx_ring().push_descriptor(serialize_rx(rx_desc)).ok());
  mem.set_fail_write(true);
  assert(qp.process_once());
  auto rx_comp = qp.rx_completion().poll_completion();
  assert(rx_comp.has_value());
  assert(rx_comp->status == static_cast<std::uint32_t>(CompletionCode::Fault));
  assert(dispatcher.stats().interrupts_fired >= 1);

  const QueuePair& cqp = qp;
  (void) cqp.tx_ring();
  (void) cqp.rx_ring();
  (void) cqp.tx_completion();
  (void) cqp.rx_completion();
  (void) cqp.stats_summary();
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  test_tx_rx_success();
  test_checksum_drop();
  test_buffer_too_small_drop();
  test_doorbells_and_wraparound();
  test_checksum_offload_bypass();
  test_checksum_offload_failure();
  test_checksum_offload_empty_payload_failure();
  test_tso_segmentation();
  test_gso_segmentation_with_header();
  test_gso_invalid_mss_no_segmentation();
  test_vlan_insert_strip();
  test_vlan_insert_buffer_too_small();
  test_completion_offload_metadata();
  test_gro_placeholder_flag();
  test_mtu_exceeded();
  test_invalid_mss_too_large();
  test_too_many_segments();
  test_tx_decode_failure();
  test_rx_decode_failure();
  test_header_length_invalid();
  test_header_covers_packet();
  test_rx_ring_insufficient_descriptors();
  test_tx_pop_failure();
  test_rx_write_failure_and_interrupts();
  return 0;
}

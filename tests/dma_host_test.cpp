#include <cassert>
#include <cstring>
#include <vector>

#include "nic/completion_queue.h"
#include "nic/descriptor_ring.h"
#include "nic/dma_engine.h"
#include "nic/doorbell.h"
#include "nic/host_memory.h"
#include "nic/sgl.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;

namespace {

void test_host_memory() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};

  std::vector<std::byte> data(4);
  data[0] = std::byte{0xAA};
  data[1] = std::byte{0xBB};
  data[2] = std::byte{0xCC};
  data[3] = std::byte{0xDD};

  assert(mem.write(100, data).ok());

  std::vector<std::byte> out(4);
  assert(mem.read(100, out).ok());
  assert(out == data);

  HostMemoryResult bad = mem.read(2000, out);
  assert(!bad.ok());
}

void test_dma_engine() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 2048, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  std::vector<std::byte> src(16);
  for (std::size_t i = 0; i < src.size(); ++i) {
    src[i] = std::byte{static_cast<unsigned char>(i)};
  }
  assert(dma.write(0, src).ok());

  std::vector<std::byte> dst(16);
  assert(dma.read(0, dst).ok());
  assert(dst == src);

  std::vector<std::byte> burst_out(16);
  assert(dma.read_burst(0, burst_out, 4, 4).ok());
  assert(burst_out == src);

  SglEntry entries[] = {
      {0, 8},
      {8, 8},
  };
  SglView sgl{std::span<const SglEntry>(entries)};
  std::vector<std::byte> sgl_out(16);
  assert(dma.transfer_sgl(sgl, DmaDirection::Read, sgl_out).ok());
  assert(sgl_out == src);
}

void test_doorbell() {
  NIC_TRACE_SCOPED(__func__);
  Doorbell db;
  std::size_t calls = 0;
  DoorbellPayload last{};
  db.set_callback([&](const DoorbellPayload& payload) {
    ++calls;
    last = payload;
  });
  db.ring(DoorbellPayload{1, 123});
  assert(calls == 1);
  assert(last.queue_id == 1);
  assert(last.data == 123);
  db.set_masked(true);
  db.ring(DoorbellPayload{2, 456});
  assert(calls == 1);
}

void test_descriptor_ring_in_model() {
  NIC_TRACE_SCOPED(__func__);
  Doorbell db;
  DescriptorRingConfig cfg{
      .descriptor_size = 8,
      .ring_size = 4,
      .base_address = 0,
      .queue_id = 7,
      .host_backed = false,
  };
  DescriptorRing ring{cfg, &db};
  std::vector<std::byte> desc(8);
  desc[0] = std::byte{0x11};
  assert(ring.push_descriptor(desc).ok());
  std::vector<std::byte> out(8);
  assert(ring.pop_descriptor(out).ok());
  assert(out[0] == std::byte{0x11});
  assert(db.rings() == 1);
}

void test_descriptor_ring_host_backed() {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryConfig config{.size_bytes = 1024, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};
  DescriptorRingConfig cfg{
      .descriptor_size = 4,
      .ring_size = 2,
      .base_address = 100,
      .queue_id = 3,
      .host_backed = true,
  };
  DescriptorRing ring{cfg, dma};
  std::vector<std::byte> desc(4, std::byte{0xAA});
  assert(ring.push_descriptor(desc).ok());
  std::vector<std::byte> out(4);
  assert(ring.pop_descriptor(out).ok());
  assert(out[0] == std::byte{0xAA});
}

void test_completion_queue() {
  NIC_TRACE_SCOPED(__func__);
  Doorbell db;
  CompletionQueueConfig cfg{.ring_size = 2, .queue_id = 5};
  CompletionQueue cq{cfg, &db};
  CompletionEntry entry{.queue_id = 5, .descriptor_index = 1, .status = 0xDEAD};
  assert(cq.post_completion(entry));
  auto polled = cq.poll_completion();
  assert(polled.has_value());
  assert(polled->descriptor_index == 1);
  assert(db.rings() == 1);
}

}  // namespace

int main() {
  NIC_TRACE_SCOPED(__func__);
  test_host_memory();
  test_dma_engine();
  test_doorbell();
  test_descriptor_ring_in_model();
  test_descriptor_ring_host_backed();
  test_completion_queue();
  return 0;
}

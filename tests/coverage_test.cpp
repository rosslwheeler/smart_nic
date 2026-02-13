#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nic/capability.h"
#include "nic/checksum.h"
#include "nic/completion_queue.h"
#include "nic/config_space.h"
#include "nic/descriptor_ring.h"
#include "nic/device.h"
#include "nic/dma_engine.h"
#include "nic/dma_types.h"
#include "nic/doorbell.h"
#include "nic/host_memory.h"
#include "nic/log.h"
#include "nic/msix.h"
#include "nic/register.h"
#include "nic/sgl.h"
#include "nic/simple_host_memory.h"
#include "nic/trace.h"

using namespace nic;

namespace {

void test_trace_and_dma_types() {
  trace::initialize();
  trace::set_thread_name("coverage-test");
  trace::message("coverage-test");

  assert(ToDmaError(HostMemoryError::None) == DmaError::None);
  assert(ToDmaError(HostMemoryError::OutOfBounds) == DmaError::OutOfBounds);
  assert(ToDmaError(HostMemoryError::IommuFault) == DmaError::TranslationFault);
  assert(ToDmaError(HostMemoryError::FaultInjected) == DmaError::FaultInjected);
  assert(ToDmaError(static_cast<HostMemoryError>(0xFF)) == DmaError::InternalError);

  DmaResult ok{};
  DmaResult bad{DmaError::AccessError, 0, nullptr};
  assert(ok.ok());
  assert(!bad.ok());

  HostMemoryResult hm_ok{};
  HostMemoryResult hm_bad{HostMemoryError::OutOfBounds, 0};
  assert(hm_ok.ok());
  assert(!hm_bad.ok());

  trace_dma_error(DmaError::None);
  trace_dma_error(DmaError::OutOfBounds, "coverage_trace");
}

void test_checksum() {
  std::vector<std::byte> even = {
      std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
  std::uint16_t even_sum = compute_checksum(even);
  assert(even_sum == 0x0000);
  assert(verify_checksum(even, even_sum));
  assert(!verify_checksum(even, static_cast<std::uint16_t>(even_sum + 1)));

  std::vector<std::byte> odd = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  std::uint16_t odd_sum = compute_checksum(odd);
  assert(verify_checksum(odd, odd_sum));

  std::vector<std::byte> odd_carry = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
  std::uint16_t odd_carry_sum = compute_checksum(odd_carry);
  assert(verify_checksum(odd_carry, odd_carry_sum));
}

void test_simple_host_memory() {
  HostMemoryConfig config{.size_bytes = 16, .page_size = 0, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  auto cfg = mem.config();
  assert(cfg.page_size == 4096);

  std::vector<std::byte> data(4, std::byte{0xAA});
  assert(mem.write(0, data).ok());
  std::vector<std::byte> out(4);
  assert(mem.read(0, out).ok());
  assert(out == data);

  HostMemoryView view{};
  assert(mem.translate(0, data.size(), view).ok());
  assert(view.length == data.size());

  ConstHostMemoryView cview{};
  assert(mem.translate_const(0, data.size(), cview).ok());
  assert(cview.length == data.size());

  std::vector<std::byte> empty;
  HostMemoryResult empty_result = mem.read(0, std::span<std::byte>{empty});
  assert(empty_result.ok());
  assert(empty_result.bytes_processed == 0);

  HostMemoryResult oob_offset = mem.read(20, out);
  assert(oob_offset.error == HostMemoryError::OutOfBounds);

  HostMemoryResult oob_length = mem.read(15, out);
  assert(oob_length.error == HostMemoryError::OutOfBounds);

  auto translator = [](HostAddress address, std::size_t length) -> std::optional<HostAddress> {
    if (address + length <= 4) {
      return address + 4;
    }
    return std::nullopt;
  };
  SimpleHostMemory translated{config, translator};
  HostMemoryView mapped_view{};
  HostMemoryResult mapped_result = translated.translate(0, 4, mapped_view);
  assert(mapped_result.ok());
  assert(mapped_view.address == 4);

  ConstHostMemoryView mapped_const{};
  assert(translated.translate_const(0, 4, mapped_const).ok());
  assert(mapped_const.address == 4);

  HostMemoryResult iommu_fault = translated.translate(8, 4, mapped_view);
  assert(iommu_fault.error == HostMemoryError::IommuFault);

  auto fault_injector = [](HostAddress, std::size_t) { return true; };
  SimpleHostMemory faulted{config, {}, fault_injector};
  HostMemoryResult fault_result = faulted.read(0, out);
  assert(fault_result.error == HostMemoryError::FaultInjected);
}

void test_dma_engine_error_paths() {
  HostMemoryConfig config{.size_bytes = 64, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  std::vector<std::byte> data(8, std::byte{0x5A});
  assert(dma.write(0, data).ok());
  std::vector<std::byte> out(8);
  assert(dma.read(0, out).ok());
  assert(out == data);

  std::vector<std::byte> partial(6);
  DmaResult partial_read = dma.read_burst(0, partial, 4, 4);
  assert(partial_read.error == DmaError::AlignmentError);

  DmaResult invalid_stride = dma.read_burst(0, out, 0, 4);
  assert(invalid_stride.error == DmaError::AlignmentError);

  DmaResult write_invalid_stride = dma.write_burst(0, data, 4, 0);
  assert(write_invalid_stride.error == DmaError::AlignmentError);

  DmaResult write_partial = dma.write_burst(0, partial, 4, 4);
  assert(write_partial.error == DmaError::AlignmentError);

  SglView empty_sgl{};
  DmaResult empty_sgl_result = dma.transfer_sgl(empty_sgl, DmaDirection::Read, out);
  assert(empty_sgl_result.error == DmaError::AccessError);

  SglEntry entries[] = {{0, 4}, {4, 4}};
  SglView sgl{std::span<const SglEntry>(entries)};
  std::vector<std::byte> small(4);
  DmaResult small_buffer_result = dma.transfer_sgl(sgl, DmaDirection::Read, small);
  assert(small_buffer_result.error == DmaError::AccessError);

  SglEntry zero_entry[] = {{0, 0}, {0, 4}};
  SglView sgl_write{std::span<const SglEntry>(zero_entry)};
  DmaResult sgl_write_result = dma.transfer_sgl(sgl_write, DmaDirection::Write, out);
  assert(sgl_write_result.ok());

  auto translator = [](HostAddress, std::size_t) -> std::optional<HostAddress> {
    return std::nullopt;
  };
  SimpleHostMemory iommu_mem{config, translator};
  DMAEngine dma_error{iommu_mem};
  DmaResult read_error = dma_error.read(0, out);
  assert(!read_error.ok());
}

void test_descriptor_ring_errors() {
  Doorbell db;
  DescriptorRingConfig cfg{
      .descriptor_size = 4,
      .ring_size = 1,
      .base_address = 0,
      .queue_id = 9,
      .host_backed = false,
  };
  DescriptorRing ring{cfg, &db};

  std::vector<std::byte> desc(4, std::byte{0x11});
  std::vector<std::byte> small_desc(2, std::byte{0x22});

  assert(ring.is_empty());
  assert(ring.available() == 0);
  assert(ring.space() == 1);

  DmaResult size_mismatch_push = ring.push_descriptor(small_desc);
  assert(size_mismatch_push.error == DmaError::AccessError);

  assert(ring.push_descriptor(desc).ok());
  assert(ring.is_full());
  assert(ring.available() == 1);
  assert(ring.space() == 0);

  DmaResult full_push = ring.push_descriptor(desc);
  assert(full_push.error == DmaError::AccessError);

  std::vector<std::byte> out(4);
  assert(ring.pop_descriptor(out).ok());
  assert(ring.is_empty());

  DmaResult size_mismatch_pop = ring.pop_descriptor(small_desc);
  assert(size_mismatch_pop.error == DmaError::AccessError);

  DmaResult empty_pop = ring.pop_descriptor(out);
  assert(empty_pop.error == DmaError::AccessError);

  ring.reset();
  assert(ring.available() == 0);

  DescriptorRingConfig host_cfg{
      .descriptor_size = 4,
      .ring_size = 1,
      .base_address = 0,
      .queue_id = 1,
      .host_backed = true,
  };
  DescriptorRing host_ring{host_cfg, static_cast<Doorbell*>(nullptr)};
  DmaResult no_dma_push = host_ring.push_descriptor(desc);
  assert(no_dma_push.error == DmaError::InternalError);
}

void test_doorbell_msix_and_registers() {
  Doorbell db;
  assert(!db.is_masked());
  db.ring(DoorbellPayload{1, 10});
  assert(db.rings() == 1);
  assert(db.last_payload().has_value());
  db.set_masked(true);
  assert(db.is_masked());
  db.reset();
  assert(db.rings() == 0);
  assert(!db.last_payload().has_value());

  MsixTable table{1};
  MsixVector vec{.address = 0x1000, .data = 0xAA, .enabled = true, .masked = false};
  assert(table.set_vector(0, vec));
  assert(!table.set_vector(1, vec));
  assert(table.vector(0).has_value());
  assert(!table.vector(5).has_value());
  assert(table.mask(0, true));
  assert(!table.mask(3, true));
  assert(table.enable(0, false));
  assert(!table.enable(4, false));

  MsixMapping mapping{2, 7};
  assert(mapping.set_queue_vector(0, 3));
  assert(!mapping.set_queue_vector(2, 3));
  assert(mapping.queue_vector(0) == 3);
  assert(mapping.queue_vector(9) == 7);

  RegisterFile regfile;
  regfile.add_register(RegisterDef{
      .name = "TEST_WO",
      .offset = 0x400,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::WO,
      .reset_value = 0xA5A5A5A5,
      .write_mask = 0xFFFFFFFF,
  });
  regfile.add_register(RegisterDef{
      .name = "TEST_RC",
      .offset = 0x404,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RC,
      .reset_value = 0xABCD1234,
      .write_mask = 0xFFFFFFFF,
  });
  regfile.add_register(RegisterDef{
      .name = "TEST_WO64",
      .offset = 0x600,
      .width = RegisterWidth::Bits64,
      .access = RegisterAccess::WO,
      .reset_value = 0x0102030405060708ULL,
      .write_mask = 0xFFFFFFFFFFFFFFFFULL,
  });
  regfile.add_register(RegisterDef{
      .name = "TEST_RO64",
      .offset = 0x700,
      .width = RegisterWidth::Bits64,
      .access = RegisterAccess::RO,
      .reset_value = 0x0F0E0D0C0B0A0908ULL,
      .write_mask = 0xFFFFFFFFFFFFFFFFULL,
  });
  regfile.add_register(RegisterDef{
      .name = "TEST_64",
      .offset = 0x500,
      .width = RegisterWidth::Bits64,
      .access = RegisterAccess::RW,
      .reset_value = 0x1122334455667788ULL,
      .write_mask = 0xFFFFFFFFFFFFFFFFULL,
  });

  regfile.reset();
  assert(regfile.read32(0x400) == 0);
  assert(regfile.read32(0x404) == 0xABCD1234);
  regfile.write32(0x404, 0xFFFFFFFF);
  assert(regfile.read32(0x404) == 0xABCD1234);

  assert(regfile.read64(0x500) == 0x1122334455667788ULL);
  regfile.write64(0x500, 0xAABBCCDDEEFF0011ULL);
  assert(regfile.read64(0x500) == 0xAABBCCDDEEFF0011ULL);
  assert(regfile.read64(0x600) == 0);
  assert(regfile.read64(0x700) == 0x0F0E0D0C0B0A0908ULL);
  regfile.write64(0x700, 0xFFFFFFFFFFFFFFFFULL);
  assert(regfile.read64(0x700) == 0x0F0E0D0C0B0A0908ULL);
  assert(regfile.read64(0xDEAD) == 0xFFFFFFFFFFFFFFFFULL);

  bool callback_hit = false;
  regfile.set_write_callback(
      [&callback_hit](std::uint32_t, std::uint64_t, std::uint64_t) { callback_hit = true; });
  regfile.write32(0x400, 0x55AA55AA);
  assert(callback_hit);

  callback_hit = false;
  regfile.write64(0x500, 0x0101010101010101ULL);
  assert(callback_hit);

  regfile.write32(0xDEAD, 0x1234);
  regfile.write64(0xDEAD, 0x1234);
}

void test_completion_queue_limits() {
  CompletionQueueConfig cfg{.ring_size = 1, .queue_id = 2};
  CompletionQueue cq{cfg, nullptr};

  assert(cq.is_empty());
  assert(!cq.is_full());
  assert(cq.available() == 0);
  assert(cq.space() == 1);

  CompletionEntry entry{.queue_id = 2, .descriptor_index = 1, .status = 0};
  assert(cq.post_completion(entry));
  assert(cq.is_full());
  assert(!cq.post_completion(entry));

  auto polled = cq.poll_completion();
  assert(polled.has_value());
  assert(cq.is_empty());
  assert(!cq.poll_completion().has_value());
}

void test_dma_engine_success_paths() {
  HostMemoryConfig config{.size_bytes = 128, .page_size = 64, .iommu_enabled = false};
  SimpleHostMemory mem{config};
  DMAEngine dma{mem};

  std::vector<std::byte> data(8, std::byte{0x5A});
  assert(dma.write_burst(0, data, 4, 4).ok());

  std::vector<std::byte> out(8);
  assert(dma.read_burst(0, out, 4, 4).ok());
  assert(out == data);

  SglEntry entries[] = {{0, 4}, {4, 4}};
  SglView sgl{std::span<const SglEntry>(entries)};
  assert(dma.transfer_sgl(sgl, DmaDirection::Write, out).ok());
}

void test_log_and_capabilities() {
  auto& log = LogController::instance();
  log.set_level(LogLevel::Trace);
  assert(log.level() == LogLevel::Trace);
  assert(log.is_enabled(LogLevel::Error));
  assert(log.is_enabled(LogLevel::Trace));
  assert(!log.is_enabled(static_cast<LogLevel>(static_cast<std::uint8_t>(LogLevel::Trace) + 1)));

  assert(log_level_color(LogLevel::Error) == 0xFF0000);
  assert(log_level_color(LogLevel::Warning) == 0xFFA500);
  assert(log_level_color(LogLevel::Info) == 0x00FF00);
  assert(log_level_color(LogLevel::Debug) == 0x00FFFF);
  assert(log_level_color(LogLevel::Trace) == 0xCCCCCC);
  assert(log_level_color(static_cast<LogLevel>(0xFF)) == 0xFFFFFF);

  NIC_LOG_INFO("coverage-test");
  NIC_LOG_DEBUG("coverage-test");

  CapabilityList empty_caps{};
  assert(empty_caps.first_capability_offset() == 0);

  CapabilityList caps = MakeDefaultCapabilities();
  assert(!caps.standard.empty());
  assert(caps.first_capability_offset() == 0x40);
  assert(!caps.extended.empty());
}

void test_register_file_inline_helpers() {
  RegisterFile regfile;
  regfile.add_register(RegisterDef{
      .name = "INLINE_TEST",
      .offset = 0x900,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW,
      .reset_value = 0x1234,
      .write_mask = 0xFFFFFFFF,
  });

  std::vector<RegisterDef> defs = MakeDefaultNicRegisters();
  regfile.add_registers(defs);
  regfile.reset();

  assert(regfile.has_register(0x900));
  assert(!regfile.has_register(0xDEAD));
  assert(regfile.get_register_def(0x900) != nullptr);
  assert(regfile.get_register_def(0xDEAD) == nullptr);

  RegisterDef weird_access{
      .name = "WEIRD",
      .offset = 0xA00,
      .width = RegisterWidth::Bits32,
      .access = static_cast<RegisterAccess>(0xFF),
      .reset_value = 0x0,
      .write_mask = 0xFFFFFFFF,
  };
  regfile.add_register(weird_access);
  regfile.write32(0xA00, 0xFFFFFFFF);
}

void test_config_space_edge_cases() {
  ConfigSpace space;

  BarArray bars = MakeDefaultBars();
  bars[0].prefetchable = true;
  CapabilityList caps = MakeDefaultCapabilities();
  space.initialize(0x1234, 0xABCD, 0x01, bars, caps);

  std::uint16_t vendor_before = space.read16(config_offset::kVendorId);
  space.write16(config_offset::kVendorId, 0xFFFF);
  assert(space.read16(config_offset::kVendorId) == vendor_before);

  std::uint32_t vendor32_before = space.read32(config_offset::kVendorId);
  space.write32(config_offset::kVendorId, 0xFFFFFFFF);
  assert(space.read32(config_offset::kVendorId) == vendor32_before);

  space.write8(kConfigSpaceSize, 0xAA);
  space.write16(kConfigSpaceSize - 1, 0xBBBB);
  space.write32(kConfigSpaceSize - 2, 0xCCCCCCCC);
}

void test_device_inline_stats() {
  DeviceConfig cfg{};
  cfg.enable_queue_manager = false;
  cfg.enable_queue_pair = false;
  Device device{cfg};

  QueueManagerStats qm_stats = device.queue_manager_stats();
  RssStats rss_stats = device.rss_stats();
  InterruptStats int_stats = device.interrupt_stats();
  (void) qm_stats;
  (void) rss_stats;
  (void) int_stats;
}

}  // namespace

int main() {
  test_trace_and_dma_types();
  test_checksum();
  test_simple_host_memory();
  test_dma_engine_error_paths();
  test_descriptor_ring_errors();
  test_doorbell_msix_and_registers();
  test_completion_queue_limits();
  test_dma_engine_success_paths();
  test_log_and_capabilities();
  test_register_file_inline_helpers();
  test_config_space_edge_cases();
  test_device_inline_stats();
  return 0;
}

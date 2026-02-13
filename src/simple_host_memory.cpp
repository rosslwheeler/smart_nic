#include "nic/simple_host_memory.h"

#include <cstring>
#include <limits>

#include "nic/trace.h"

using namespace nic;

SimpleHostMemory::SimpleHostMemory(HostMemoryConfig config,
                                   AddressTranslator translator,
                                   FaultInjector fault_injector)
  : config_(config),
    translator_(std::move(translator)),
    fault_injector_(std::move(fault_injector)),
    buffer_(config.size_bytes) {
  NIC_TRACE_SCOPED(__func__);
  if (config_.page_size == 0) {
    config_.page_size = 4096;
  }
}

HostMemoryConfig SimpleHostMemory::config() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return config_;
}

HostMemoryResult SimpleHostMemory::translate(HostAddress address,
                                             std::size_t length,
                                             HostMemoryView& view) {
  NIC_TRACE_SCOPED(__func__);
  return translate_view(address, length, &view, nullptr);
}

HostMemoryResult SimpleHostMemory::translate_const(HostAddress address,
                                                   std::size_t length,
                                                   ConstHostMemoryView& view) const {
  NIC_TRACE_SCOPED(__func__);
  return translate_view(address, length, nullptr, &view);
}

HostMemoryResult SimpleHostMemory::read(HostAddress address, std::span<std::byte> buffer) const {
  NIC_TRACE_SCOPED(__func__);
  ConstHostMemoryView view{};
  HostMemoryResult result = translate_const(address, buffer.size(), view);
  if (!result.ok()) {
    return result;
  }

  if (view.length > 0) {
    std::memcpy(buffer.data(), view.data, view.length);
  }
  return result;
}

HostMemoryResult SimpleHostMemory::write(HostAddress address, std::span<const std::byte> data) {
  NIC_TRACE_SCOPED(__func__);
  HostMemoryView view{};
  HostMemoryResult result = translate(address, data.size(), view);
  if (!result.ok()) {
    return result;
  }

  if (view.length > 0) {
    std::memcpy(view.data, data.data(), view.length);
  }
  return result;
}

HostMemoryResult SimpleHostMemory::translate_view(HostAddress address,
                                                  std::size_t length,
                                                  HostMemoryView* mutable_view,
                                                  ConstHostMemoryView* const_view) const {
  NIC_TRACE_SCOPED(__func__);

  if (fault_injector_ && fault_injector_(address, length)) {
    return {HostMemoryError::FaultInjected, 0};
  }

  HostAddress mapped = address;
  if (translator_) {
    std::optional<HostAddress> translated = translator_(address, length);
    if (!translated.has_value()) {
      return {HostMemoryError::IommuFault, 0};
    }
    mapped = *translated;
  }

  std::size_t offset = static_cast<std::size_t>(mapped);
  if (offset > buffer_.size()) {
    return {HostMemoryError::OutOfBounds, 0};
  }

  if (length > buffer_.size() - offset) {
    return {HostMemoryError::OutOfBounds, 0};
  }

  if (mutable_view != nullptr) {
    mutable_view->data = const_cast<std::byte*>(buffer_.data()) + offset;
    mutable_view->length = length;
    mutable_view->address = mapped;
  } else if (const_view != nullptr) {
    const_view->data = buffer_.data() + offset;
    const_view->length = length;
    const_view->address = mapped;
  }

  return {HostMemoryError::None, length};
}

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "nic/host_memory.h"

namespace nic {

/// Simple host memory implementation backed by a contiguous buffer.
class SimpleHostMemory final : public HostMemory {
public:
  using AddressTranslator = std::function<std::optional<HostAddress>(HostAddress, std::size_t)>;
  using FaultInjector = std::function<bool(HostAddress, std::size_t)>;

  SimpleHostMemory(HostMemoryConfig config,
                   AddressTranslator translator = {},
                   FaultInjector fault_injector = {});

  ~SimpleHostMemory() override = default;

  [[nodiscard]] HostMemoryConfig config() const noexcept override;

  [[nodiscard]] HostMemoryResult translate(HostAddress address,
                                           std::size_t length,
                                           HostMemoryView& view) override;

  [[nodiscard]] HostMemoryResult translate_const(HostAddress address,
                                                 std::size_t length,
                                                 ConstHostMemoryView& view) const override;

  [[nodiscard]] HostMemoryResult read(HostAddress address,
                                      std::span<std::byte> buffer) const override;

  [[nodiscard]] HostMemoryResult write(HostAddress address,
                                       std::span<const std::byte> data) override;

private:
  HostMemoryConfig config_{};
  AddressTranslator translator_;
  FaultInjector fault_injector_;
  std::vector<std::byte> buffer_;

  [[nodiscard]] HostMemoryResult translate_view(HostAddress address,
                                                std::size_t length,
                                                HostMemoryView* mutable_view,
                                                ConstHostMemoryView* const_view) const;
};

}  // namespace nic

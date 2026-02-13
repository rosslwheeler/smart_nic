#include "nic/doorbell.h"

using namespace nic;

void Doorbell::ring(const DoorbellPayload& payload) {
  NIC_TRACE_SCOPED(__func__);
  if (masked_) {
    return;
  }
  last_payload_ = payload;
  ++rings_;
  if (callback_) {
    callback_(payload);
  }
}

void Doorbell::set_callback(DoorbellCallback callback) {
  NIC_TRACE_SCOPED(__func__);
  callback_ = std::move(callback);
}

void Doorbell::set_masked(bool masked) {
  NIC_TRACE_SCOPED(__func__);
  masked_ = masked;
}

bool Doorbell::is_masked() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return masked_;
}

std::optional<DoorbellPayload> Doorbell::last_payload() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return last_payload_;
}

std::size_t Doorbell::rings() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return rings_;
}

void Doorbell::reset() noexcept {
  NIC_TRACE_SCOPED(__func__);
  rings_ = 0;
  last_payload_ = std::nullopt;
}

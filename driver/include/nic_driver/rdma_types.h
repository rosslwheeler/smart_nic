#pragma once

/// @file rdma_types.h
/// @brief RDMA type definitions and handle wrappers for the NIC driver.

#include <cstdint>

#include "nic/rocev2/cqe.h"
#include "nic/rocev2/engine.h"
#include "nic/rocev2/queue_pair.h"
#include "nic/rocev2/types.h"
#include "nic/rocev2/wqe.h"

namespace nic_driver {

/// Protection Domain handle wrapper for type safety.
struct PdHandle {
  std::uint32_t value;
};

/// Memory Region handle wrapper containing both local and remote keys.
struct MrHandle {
  std::uint32_t lkey;
  std::uint32_t rkey;
};

/// Completion Queue handle wrapper for type safety.
struct CqHandle {
  std::uint32_t value;
};

/// Queue Pair handle wrapper for type safety.
struct QpHandle {
  std::uint32_t value;
};

// Re-export types from nic::rocev2 for driver API convenience.
using nic::rocev2::AccessFlags;
using nic::rocev2::OutgoingPacket;
using nic::rocev2::QpState;
using nic::rocev2::QpType;
using nic::rocev2::RdmaCqe;
using nic::rocev2::RdmaQpConfig;
using nic::rocev2::RdmaQpModifyParams;
using nic::rocev2::RecvWqe;
using nic::rocev2::SendWqe;
using nic::rocev2::WqeOpcode;
using nic::rocev2::WqeStatus;

}  // namespace nic_driver

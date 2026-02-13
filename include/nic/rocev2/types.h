#pragma once

/// @file types.h
/// @brief Core RDMA type definitions for RoCEv2 implementation.

#include <cstdint>

namespace nic::rocev2 {

/// RoCEv2 well-known UDP destination port.
inline constexpr std::uint16_t kRoceUdpPort = 4791;

/// Maximum PSN value (24-bit).
inline constexpr std::uint32_t kMaxPsn = 0x00FFFFFF;

/// Default partition key.
inline constexpr std::uint16_t kDefaultPkey = 0xFFFF;

/// Queue Pair types.
enum class QpType : std::uint8_t {
  Rc,  // Reliable Connection - implemented
  Uc,  // Unreliable Connection - future
  Ud,  // Unreliable Datagram - future
};

/// Queue Pair state machine states (IB Spec).
enum class QpState : std::uint8_t {
  Reset,  // Initial state
  Init,   // Initialized, can post RECV
  Rtr,    // Ready to Receive
  Rts,    // Ready to Send - fully operational
  Sqd,    // Send Queue Drained
  SqErr,  // Send Queue Error
  Error,  // General error state
};

/// RoCEv2/IB opcodes (Base Transport Header).
enum class RdmaOpcode : std::uint8_t {
  kRcSendFirst = 0x00,
  kRcSendMiddle = 0x01,
  kRcSendLast = 0x02,
  kRcSendLastImm = 0x03,
  kRcSendOnly = 0x04,
  kRcSendOnlyImm = 0x05,
  kRcWriteFirst = 0x06,
  kRcWriteMiddle = 0x07,
  kRcWriteLast = 0x08,
  kRcWriteLastImm = 0x09,
  kRcWriteOnly = 0x0A,
  kRcWriteOnlyImm = 0x0B,
  kRcReadRequest = 0x0C,
  kRcReadResponseFirst = 0x0D,
  kRcReadResponseMiddle = 0x0E,
  kRcReadResponseLast = 0x0F,
  kRcReadResponseOnly = 0x10,
  kRcAck = 0x11,
  kCnp = 0x81,
};

/// Work Queue Element operation types (user-facing).
enum class WqeOpcode : std::uint8_t {
  Send,
  SendImm,
  RdmaWrite,
  RdmaWriteImm,
  RdmaRead,
};

/// Completion status codes.
enum class WqeStatus : std::uint8_t {
  Success,
  LocalLengthError,
  LocalQpOperationError,
  LocalProtectionError,
  WrFlushError,
  MemoryWindowBindError,
  BadResponseError,
  LocalAccessError,
  RemoteInvalidRequestError,
  RemoteAccessError,
  RemoteOperationError,
  RetryExceededError,
  RnrRetryExceededError,
  TransportRetryExceededError,
  InvalidQpStateError,
};

/// AETH syndrome values (ACK Extended Header).
enum class AethSyndrome : std::uint8_t {
  Ack = 0x00,                // Normal ACK
  RnrNak = 0x20,             // Receiver Not Ready NAK (bits 0-4 encode min RNR timer)
  PsnSeqError = 0x60,        // PSN sequence error NAK
  InvalidRequest = 0x61,     // Invalid request NAK
  RemoteAccessError = 0x62,  // Remote access error NAK
  RemoteOpError = 0x63,      // Remote operation error NAK
};

/// Access flags for memory regions.
struct AccessFlags {
  bool local_read{true};
  bool local_write{false};
  bool remote_read{false};
  bool remote_write{false};
  bool zero_based{false};
};

/// Advance PSN with 24-bit wraparound.
[[nodiscard]] inline constexpr std::uint32_t advance_psn(std::uint32_t psn,
                                                         std::uint32_t increment = 1) noexcept {
  return (psn + increment) & kMaxPsn;
}

/// Check if PSN is in valid range [base, base + window).
[[nodiscard]] inline constexpr bool psn_in_window(std::uint32_t psn,
                                                  std::uint32_t base,
                                                  std::uint32_t window_size) noexcept {
  std::uint32_t diff = (psn - base) & kMaxPsn;
  return diff < window_size;
}

}  // namespace nic::rocev2

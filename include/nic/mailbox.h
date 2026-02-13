#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace nic {

enum class MailboxOpcode : std::uint16_t {
  VFReset = 1,
  GetStats = 2,
  SetMTU = 3,
  SetMacAddr = 4,
  SetVLAN = 5,
  GetResources = 6,
  ACK = 0xFF00,
  NACK = 0xFF01,
};

struct MailboxMessage {
  MailboxOpcode opcode{MailboxOpcode::ACK};
  std::uint16_t vf_id{0};
  std::uint32_t sequence{0};  ///< For request/response matching
  std::vector<std::byte> payload;

  [[nodiscard]] std::size_t size() const noexcept {
    return sizeof(opcode) + sizeof(vf_id) + sizeof(sequence) + payload.size();
  }
};

/// Mailbox for PF-VF communication in SR-IOV.
class Mailbox {
public:
  using MessageHandler = std::function<MailboxMessage(const MailboxMessage&)>;

  Mailbox();

  // Send message (async)
  bool send_to_vf(const MailboxMessage& msg);
  bool send_to_pf(const MailboxMessage& msg);

  // Receive message (poll)
  std::optional<MailboxMessage> receive_from_vf(std::uint16_t vf_id);
  std::optional<MailboxMessage> receive_from_pf(std::uint16_t vf_id);

  // Request-response pattern (synchronous)
  std::optional<MailboxMessage> send_and_receive(
      const MailboxMessage& msg,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{100});

  // Message handler for automatic processing
  void set_pf_handler(MessageHandler handler);
  void set_vf_handler(std::uint16_t vf_id, MessageHandler handler);

  // Process pending messages with handlers
  void process_pending();

  // Statistics
  [[nodiscard]] std::uint64_t messages_sent() const noexcept { return messages_sent_; }
  [[nodiscard]] std::uint64_t messages_received() const noexcept { return messages_received_; }
  [[nodiscard]] std::uint64_t messages_dropped() const noexcept { return messages_dropped_; }

private:
  static constexpr std::size_t kMaxQueueDepth = 16;

  MessageHandler pf_handler_;
  std::unordered_map<std::uint16_t, MessageHandler> vf_handlers_;

  std::queue<MailboxMessage> pf_inbox_;  ///< Messages to PF from VFs
  std::unordered_map<std::uint16_t, std::queue<MailboxMessage>>
      vf_inboxes_;  ///< Messages to VFs from PF

  std::uint32_t next_sequence_{1};
  std::uint64_t messages_sent_{0};
  std::uint64_t messages_received_{0};
  std::uint64_t messages_dropped_{0};

  std::uint32_t allocate_sequence() noexcept;
  bool enqueue_to_vf(const MailboxMessage& msg);
  bool enqueue_to_pf(const MailboxMessage& msg);
};

}  // namespace nic

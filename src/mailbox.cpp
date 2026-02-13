#include "nic/mailbox.h"

#include <thread>

#include "nic/trace.h"

using namespace nic;
using namespace std::chrono_literals;

Mailbox::Mailbox() {
  NIC_TRACE_SCOPED(__func__);
}

bool Mailbox::send_to_vf(const MailboxMessage& msg) {
  NIC_TRACE_SCOPED(__func__);
  return enqueue_to_vf(msg);
}

bool Mailbox::send_to_pf(const MailboxMessage& msg) {
  NIC_TRACE_SCOPED(__func__);
  return enqueue_to_pf(msg);
}

std::optional<MailboxMessage> Mailbox::receive_from_vf(std::uint16_t vf_id) {
  NIC_TRACE_SCOPED(__func__);

  if (pf_inbox_.empty()) {
    return std::nullopt;
  }

  // Find message from specified VF
  // For simplicity, just return the front message if it matches
  // A real implementation would have per-VF queues
  auto msg = pf_inbox_.front();
  if (msg.vf_id == vf_id) {
    pf_inbox_.pop();
    messages_received_ += 1;
    return msg;
  }

  return std::nullopt;
}

std::optional<MailboxMessage> Mailbox::receive_from_pf(std::uint16_t vf_id) {
  NIC_TRACE_SCOPED(__func__);

  auto it = vf_inboxes_.find(vf_id);
  if ((it == vf_inboxes_.end()) || it->second.empty()) {
    return std::nullopt;
  }

  auto msg = it->second.front();
  it->second.pop();
  messages_received_ += 1;
  return msg;
}

std::optional<MailboxMessage> Mailbox::send_and_receive(const MailboxMessage& msg,
                                                        std::chrono::milliseconds timeout) {
  NIC_TRACE_SCOPED(__func__);

  // Assign sequence number
  MailboxMessage request = msg;
  request.sequence = allocate_sequence();

  // Send to appropriate destination
  bool sent = false;
  if (msg.vf_id == 0) {
    // Message from VF to PF
    sent = send_to_pf(request);
  } else {
    // Message from PF to VF
    sent = send_to_vf(request);
  }

  if (!sent) {
    return std::nullopt;
  }

  // Poll for response with matching sequence
  // In a real implementation, this would use interrupts or proper blocking
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    std::optional<MailboxMessage> response;

    if (msg.vf_id == 0) {
      // Waiting for response from PF
      response = receive_from_pf(msg.vf_id);
    } else {
      // Waiting for response from VF
      response = receive_from_vf(msg.vf_id);
    }

    if (response.has_value() && response->sequence == request.sequence) {
      return response;
    }

    // Simple yield - in real code this would be more sophisticated
    std::this_thread::sleep_for(1ms);
  }

  return std::nullopt;  // Timeout
}

void Mailbox::set_pf_handler(MessageHandler handler) {
  NIC_TRACE_SCOPED(__func__);
  pf_handler_ = std::move(handler);
}

void Mailbox::set_vf_handler(std::uint16_t vf_id, MessageHandler handler) {
  NIC_TRACE_SCOPED(__func__);
  vf_handlers_[vf_id] = std::move(handler);
}

void Mailbox::process_pending() {
  NIC_TRACE_SCOPED(__func__);

  // Process PF inbox
  if (pf_handler_ && !pf_inbox_.empty()) {
    auto msg = pf_inbox_.front();
    pf_inbox_.pop();
    messages_received_ += 1;

    auto response = pf_handler_(msg);
    if ((response.opcode != MailboxOpcode::ACK) || !response.payload.empty()) {
      response.sequence = msg.sequence;  // Match request sequence
      enqueue_to_vf(response);
    }
  }

  // Process VF inboxes
  for (auto& [vf_id, inbox] : vf_inboxes_) {
    auto handler_it = vf_handlers_.find(vf_id);
    if ((handler_it != vf_handlers_.end()) && !inbox.empty()) {
      auto msg = inbox.front();
      inbox.pop();
      messages_received_ += 1;

      auto response = handler_it->second(msg);
      if ((response.opcode != MailboxOpcode::ACK) || !response.payload.empty()) {
        response.sequence = msg.sequence;  // Match request sequence
        enqueue_to_pf(response);
      }
    }
  }
}

std::uint32_t Mailbox::allocate_sequence() noexcept {
  return next_sequence_++;
}

bool Mailbox::enqueue_to_vf(const MailboxMessage& msg) {
  NIC_TRACE_SCOPED(__func__);

  auto& inbox = vf_inboxes_[msg.vf_id];
  if (inbox.size() >= kMaxQueueDepth) {
    messages_dropped_ += 1;
    return false;
  }

  inbox.push(msg);
  messages_sent_ += 1;
  return true;
}

bool Mailbox::enqueue_to_pf(const MailboxMessage& msg) {
  NIC_TRACE_SCOPED(__func__);

  if (pf_inbox_.size() >= kMaxQueueDepth) {
    messages_dropped_ += 1;
    return false;
  }

  pf_inbox_.push(msg);
  messages_sent_ += 1;
  return true;
}

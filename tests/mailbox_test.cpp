#include "nic/mailbox.h"

#include <cassert>
#include <cstring>
#include <thread>

using namespace nic;
using namespace std::chrono_literals;

int main() {
  // Basic message send/receive.
  Mailbox mailbox;

  MailboxMessage msg1{.opcode = MailboxOpcode::GetStats, .vf_id = 1, .sequence = 0, .payload = {}};
  assert(mailbox.send_to_vf(msg1));

  auto received = mailbox.receive_from_pf(1);
  assert(received.has_value());
  assert(received->opcode == MailboxOpcode::GetStats);
  assert(received->vf_id == 1);

  // VF to PF message.
  MailboxMessage msg2{.opcode = MailboxOpcode::VFReset, .vf_id = 2, .sequence = 0, .payload = {}};
  assert(mailbox.send_to_pf(msg2));

  auto received2 = mailbox.receive_from_vf(2);
  assert(received2.has_value());
  assert(received2->opcode == MailboxOpcode::VFReset);
  assert(received2->vf_id == 2);

  // Message with payload.
  MailboxMessage msg3{.opcode = MailboxOpcode::SetMTU, .vf_id = 3, .sequence = 0, .payload = {}};
  std::uint32_t mtu = 9000;
  msg3.payload.resize(sizeof(mtu));
  std::memcpy(msg3.payload.data(), &mtu, sizeof(mtu));

  assert(mailbox.send_to_vf(msg3));
  auto received3 = mailbox.receive_from_pf(3);
  assert(received3.has_value());
  assert(received3->payload.size() == sizeof(mtu));
  std::uint32_t received_mtu = 0;
  std::memcpy(&received_mtu, received3->payload.data(), sizeof(mtu));
  assert(received_mtu == 9000);

  // Message handler for automatic processing.
  Mailbox mailbox2;

  bool handler_called = false;
  mailbox2.set_pf_handler([&handler_called](const MailboxMessage& msg) -> MailboxMessage {
    handler_called = true;
    assert(msg.opcode == MailboxOpcode::GetStats);
    MailboxMessage response{
        .opcode = MailboxOpcode::ACK, .vf_id = msg.vf_id, .sequence = {}, .payload = {}};
    return response;
  });

  MailboxMessage request{
      .opcode = MailboxOpcode::GetStats, .vf_id = 5, .sequence = {}, .payload = {}};
  assert(mailbox2.send_to_pf(request));
  mailbox2.process_pending();

  assert(handler_called);

  // VF handler.
  Mailbox mailbox3;

  bool vf_handler_called = false;
  mailbox3.set_vf_handler(6, [&vf_handler_called](const MailboxMessage& msg) -> MailboxMessage {
    vf_handler_called = true;
    assert(msg.opcode == MailboxOpcode::SetMTU);
    return MailboxMessage{
        .opcode = MailboxOpcode::ACK, .vf_id = msg.vf_id, .sequence = {}, .payload = {}};
  });

  MailboxMessage vf_request{
      .opcode = MailboxOpcode::SetMTU, .vf_id = 6, .sequence = {}, .payload = {}};
  assert(mailbox3.send_to_vf(vf_request));
  mailbox3.process_pending();

  assert(vf_handler_called);

  // Queue depth limit.
  Mailbox mailbox4;

  // Fill up the queue.
  for (std::size_t i = 0; i < 16; ++i) {
    MailboxMessage fill_msg{
        .opcode = MailboxOpcode::GetStats, .vf_id = 7, .sequence = {}, .payload = {}};
    assert(mailbox4.send_to_vf(fill_msg));
  }

  // Next message should be dropped.
  MailboxMessage overflow_msg{
      .opcode = MailboxOpcode::GetStats, .vf_id = 7, .sequence = {}, .payload = {}};
  assert(!mailbox4.send_to_vf(overflow_msg));
  assert(mailbox4.messages_dropped() == 1);

  // Statistics tracking.
  Mailbox mailbox5;
  assert(mailbox5.messages_sent() == 0);
  assert(mailbox5.messages_received() == 0);

  MailboxMessage stat_msg{
      .opcode = MailboxOpcode::GetStats, .vf_id = 8, .sequence = {}, .payload = {}};
  mailbox5.send_to_vf(stat_msg);
  assert(mailbox5.messages_sent() == 1);

  mailbox5.receive_from_pf(8);
  assert(mailbox5.messages_received() == 1);

  // Send-and-receive pattern (simulated).
  // Note: This is a simplified test since we don't have a real async handler.
  Mailbox mailbox6;

  mailbox6.set_pf_handler([](const MailboxMessage& msg) -> MailboxMessage {
    MailboxMessage response{
        .opcode = MailboxOpcode::ACK, .vf_id = msg.vf_id, .sequence = msg.sequence, .payload = {}};
    return response;
  });

  // Send from VF to PF and expect response.
  MailboxMessage sync_request{.opcode = MailboxOpcode::GetResources,
                              .vf_id = 0,
                              .sequence = {},
                              .payload = {}};  // vf_id=0 means VF->PF

  // In a real scenario, we'd need async processing, but for this test we'll manually process.
  // Since send_and_receive polls for responses, we need to process in parallel.
  // For simplicity, we'll just test the basic flow without the full async mechanism.

  // Empty inbox paths.
  Mailbox mailbox7;
  assert(!mailbox7.receive_from_vf(1).has_value());
  assert(!mailbox7.receive_from_pf(1).has_value());

  // Mismatched VF id on PF inbox.
  Mailbox mailbox8;
  MailboxMessage mismatch{
      .opcode = MailboxOpcode::VFReset, .vf_id = 1, .sequence = {}, .payload = {}};
  assert(mailbox8.send_to_pf(mismatch));
  assert(!mailbox8.receive_from_vf(2).has_value());
  assert(mailbox8.receive_from_vf(1).has_value());

  // send_and_receive timeout paths.
  Mailbox mailbox9;
  MailboxMessage timeout_pf{
      .opcode = MailboxOpcode::GetStats, .vf_id = 0, .sequence = {}, .payload = {}};
  assert(!mailbox9.send_and_receive(timeout_pf, 2ms).has_value());

  MailboxMessage timeout_vf{
      .opcode = MailboxOpcode::GetStats, .vf_id = 3, .sequence = {}, .payload = {}};
  assert(!mailbox9.send_and_receive(timeout_vf, 2ms).has_value());

  // send_and_receive early failure on full queue.
  Mailbox mailbox10;
  for (std::size_t i = 0; i < 16; ++i) {
    MailboxMessage fill_msg{
        .opcode = MailboxOpcode::GetStats, .vf_id = 4, .sequence = {}, .payload = {}};
    assert(mailbox10.send_to_vf(fill_msg));
  }
  MailboxMessage should_fail{
      .opcode = MailboxOpcode::GetStats, .vf_id = 4, .sequence = {}, .payload = {}};
  assert(!mailbox10.send_and_receive(should_fail, 1ms).has_value());

  Mailbox mailbox11;
  for (std::size_t i = 0; i < 16; ++i) {
    MailboxMessage fill_msg{
        .opcode = MailboxOpcode::GetStats, .vf_id = 0, .sequence = {}, .payload = {}};
    assert(mailbox11.send_to_pf(fill_msg));
  }
  MailboxMessage should_fail_pf{
      .opcode = MailboxOpcode::GetStats, .vf_id = 0, .sequence = {}, .payload = {}};
  assert(!mailbox11.send_and_receive(should_fail_pf, 1ms).has_value());

  // process_pending responses that enqueue follow-ups.
  Mailbox mailbox12;
  mailbox12.set_pf_handler([](const MailboxMessage& msg) -> MailboxMessage {
    MailboxMessage response{
        .opcode = MailboxOpcode::NACK, .vf_id = msg.vf_id, .sequence = {}, .payload = {}};
    response.payload.push_back(std::byte{0x01});
    return response;
  });
  MailboxMessage pf_req{
      .opcode = MailboxOpcode::GetStats, .vf_id = 2, .sequence = {}, .payload = {}};
  assert(mailbox12.send_to_pf(pf_req));
  mailbox12.process_pending();
  auto pf_resp = mailbox12.receive_from_pf(2);
  assert(pf_resp.has_value());
  assert(pf_resp->opcode == MailboxOpcode::NACK);

  Mailbox mailbox13;
  mailbox13.set_vf_handler(3, [](const MailboxMessage& msg) -> MailboxMessage {
    MailboxMessage response{
        .opcode = MailboxOpcode::GetResources, .vf_id = msg.vf_id, .sequence = {}, .payload = {}};
    response.payload.push_back(std::byte{0x02});
    return response;
  });
  MailboxMessage vf_req{.opcode = MailboxOpcode::SetMTU, .vf_id = 3, .sequence = {}, .payload = {}};
  assert(mailbox13.send_to_vf(vf_req));
  mailbox13.process_pending();
  auto vf_resp = mailbox13.receive_from_vf(3);
  assert(vf_resp.has_value());
  assert(vf_resp->opcode == MailboxOpcode::GetResources);

  return 0;
}

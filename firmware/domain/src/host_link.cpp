// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/host_link.hpp"

#include <algorithm>
#include <utility>

namespace firmware::domain {

namespace {
constexpr size_t kMaxQueuedFrames = 8;
constexpr uint32_t kMaxMatchingCode = 999'999;
constexpr Identity kZeroIdentity{};
}  // namespace

HostLink::HostLink(const MonotonicClock& clock, PairingStore& store, uint32_t boot_identity)
    : clock_(clock), store_(store), feed_(clock, boot_identity) {
  feed_.RequireExplicitSession();
  feed_.SetControlHandler([this](const protocol::FrameStream::Result& r) { OnResult(r); });
}

void HostLink::Tick() {
  feed_.Tick();
  if (phase_ == LinkPhase::kAwaitingConfirmation &&
      clock_.NowMs() - pending_since_ms_ >= kPairingConfirmTimeoutMs) {
    ConfirmPairing(false);
  }
}

std::vector<std::vector<uint8_t>> HostLink::TakeOutgoing() { return std::exchange(outgoing_, {}); }

void HostLink::OnResult(const protocol::FrameStream::Result& result) {
  if (result.error == protocol::Error::kUnsupportedMajor) {
    diagnostics_.fault = LinkFault::kIncompatibleMajor;
    phase_ = LinkPhase::kIncompatible;
    CloseSession();
    SendError(LinkErrorCode::kIncompatible);
    return;
  }
  if (result.error != protocol::Error::kNone) return;
  const auto& envelope = result.decoded.envelope;
  switch (envelope.type) {
    case protocol::MessageType::kHello:
      OnHello(envelope);
      break;
    case protocol::MessageType::kPair:
      OnPair(envelope);
      break;
    case protocol::MessageType::kCommand:
      OnCommand(envelope);
      break;
    default:
      break;  // HelloReply/CommandReply/Error are Host-bound; ignore.
  }
}

void HostLink::OnHello(const protocol::Envelope& envelope) {
  if (!envelope.hello) return;
  const auto& hello = *envelope.hello;
  diagnostics_.host_major = hello.supported_major;
  diagnostics_.host_minor = hello.supported_minor;
  // A fresh Hello is a new connection attempt: the old session is over.
  CloseSession();
  if (hello.supported_major != diagnostics_.supported_major) {
    diagnostics_.fault = LinkFault::kIncompatibleMajor;
    diagnostics_.negotiated_minor.reset();
    phase_ = LinkPhase::kIncompatible;
    SendError(LinkErrorCode::kIncompatible);
    return;
  }
  diagnostics_.fault = LinkFault::kNone;
  diagnostics_.negotiated_minor = std::min(hello.supported_minor, diagnostics_.supported_minor);
  phase_ = LinkPhase::kNegotiated;
  if (hello.utc_ms) feed_.ApplyHostUtc(*hello.utc_ms);

  protocol::Envelope reply;
  reply.type = protocol::MessageType::kHelloReply;
  reply.hello = protocol::HelloBody{.supported_major = diagnostics_.supported_major,
                                    .supported_minor = diagnostics_.supported_minor};
  Send(std::move(reply));
}

void HostLink::OnPair(const protocol::Envelope& envelope) {
  if (!envelope.pair || phase_ == LinkPhase::kIdle || phase_ == LinkPhase::kIncompatible ||
      envelope.pair->matching_code > kMaxMatchingCode) {
    SendError(LinkErrorCode::kBadRequest);
    return;
  }
  const auto& pair = *envelope.pair;
  const auto paired_host = store_.PairedHost();
  const bool names_this_device = pair.intended_peer_identity == kZeroIdentity ||
                                 pair.intended_peer_identity == store_.DeviceIdentity();
  if (paired_host && *paired_host == pair.sender_identity && names_this_device) {
    OpenSession(envelope.session, pair.sender_identity, pair.matching_code);
    return;
  }
  if (paired_host || pair.intended_peer_identity != kZeroIdentity) {
    diagnostics_.fault = LinkFault::kNotPairedWithHost;
    SendError(LinkErrorCode::kNotPairedWithYou);
    return;
  }
  pending_host_ = pair.sender_identity;
  pending_session_ = envelope.session;
  pending_code_ = pair.matching_code;
  pending_since_ms_ = clock_.NowMs();
  phase_ = LinkPhase::kAwaitingConfirmation;
}

void HostLink::ConfirmPairing(bool accepted) {
  if (phase_ != LinkPhase::kAwaitingConfirmation) return;
  if (!accepted || !store_.SavePairedHost(pending_host_)) {
    diagnostics_.fault = LinkFault::kDeclined;
    phase_ = LinkPhase::kNegotiated;
    SendError(LinkErrorCode::kDeclined);
    return;
  }
  OpenSession(pending_session_, pending_host_, pending_code_);
}

void HostLink::OpenSession(const Identity& session, const Identity& host, uint32_t code) {
  session_ = session;
  host_ = host;
  cached_command_.reset();
  tx_sequence_ = 0;
  feed_.StartSession(session);
  phase_ = LinkPhase::kSessionOpen;
  diagnostics_.fault = LinkFault::kNone;

  protocol::Envelope reply;
  reply.type = protocol::MessageType::kPair;
  reply.session = session;
  reply.pair = protocol::PairBody{.sender_identity = store_.DeviceIdentity(),
                                  .intended_peer_identity = host,
                                  .matching_code = code};
  Send(std::move(reply));
}

void HostLink::CloseSession() {
  feed_.EndSession();
  if (phase_ == LinkPhase::kSessionOpen || phase_ == LinkPhase::kAwaitingConfirmation) {
    phase_ = LinkPhase::kNegotiated;
  }
}

void HostLink::Unpair() {
  store_.ClearPairedHost();
  CloseSession();
}

void HostLink::OnCommand(const protocol::Envelope& envelope) {
  if (!envelope.command || envelope.session != session_) return;
  const auto& command = *envelope.command;
  const bool retry = cached_command_ && *cached_command_ == command.command_identity;
  // A retry is answered from the cache even if the command itself closed the
  // session (unpair); anything new needs an open session.
  if (!retry && phase_ != LinkPhase::kSessionOpen) return;
  uint8_t result = 2;  // rejected
  bool close_after = false;
  if (retry) {
    result = cached_result_;  // idempotent retry: repeat the recorded outcome
  } else {
    if (command.operation == 1) {
      result = store_.PairedHost() ? 0 : 1;
      close_after = true;
    } else if (command.operation == 2) {
      result = (calibration_reset_ && calibration_reset_()) ? 0 : 2;
    }
    cached_command_ = command.command_identity;
    cached_result_ = result == 0 ? 1 : result;  // a retry finds it already applied
  }
  protocol::Envelope reply;
  reply.type = protocol::MessageType::kCommandReply;
  reply.session = session_;
  reply.command = protocol::CommandBody{.command_identity = command.command_identity,
                                        .operation = command.operation,
                                        .result = result};
  Send(std::move(reply));
  if (close_after) {
    store_.ClearPairedHost();
    CloseSession();
  }
}

void HostLink::SendError(LinkErrorCode code) {
  protocol::Envelope error;
  error.type = protocol::MessageType::kError;
  error.body = {static_cast<uint8_t>(code)};
  Send(std::move(error));
}

void HostLink::Send(protocol::Envelope envelope) {
  envelope.sequence = ++tx_sequence_;
  std::vector<uint8_t> frame;
  if (protocol::Encode(envelope, &frame) != protocol::Error::kNone) return;
  if (outgoing_.size() >= kMaxQueuedFrames) outgoing_.erase(outgoing_.begin());
  outgoing_.push_back(std::move(frame));
}

}  // namespace firmware::domain

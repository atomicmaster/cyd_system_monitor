// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "firmware/domain/host_feed.hpp"
#include "protocol.hpp"

namespace firmware::domain {

using Identity = std::array<uint8_t, protocol::kSessionIdBytes>;

// Persistent one-to-one Host pairing (NVS on the board, in memory in tests).
// Identities are confirmation data, not secrets.
class PairingStore {
 public:
  virtual ~PairingStore() = default;
  virtual Identity DeviceIdentity() = 0;
  virtual std::optional<Identity> PairedHost() = 0;
  virtual bool SavePairedHost(const Identity& host) = 0;
  virtual bool ClearPairedHost() = 0;
};

enum class LinkPhase { kIdle, kNegotiated, kAwaitingConfirmation, kSessionOpen, kIncompatible };

// Why the link refused the Host; surfaced on the About/diagnostics screen.
enum class LinkFault : uint8_t { kNone, kIncompatibleMajor, kNotPairedWithHost, kDeclined };

// Wire values of the one-byte Error body (see CORE_PROTOCOL.md).
enum class LinkErrorCode : uint8_t {
  kIncompatible = 1,
  kNotPairedWithYou = 2,
  kDeclined = 3,
  kBadRequest = 4,
};

constexpr uint64_t kPairingConfirmTimeoutMs = 60'000;

struct LinkDiagnostics {
  uint8_t supported_major = 1;
  uint8_t supported_minor = 0;
  std::optional<uint8_t> host_major;  // unset until a Hello decodes
  std::optional<uint8_t> host_minor;
  std::optional<uint8_t> negotiated_minor;
  LinkFault fault = LinkFault::kNone;
};

// Device side of the negotiated link: Hello/HelloReply, Pair with touch
// confirmation, session start, and the bounded unpair/calibration-reset
// commands. Owns the HostFeed so snapshots are only applied inside a
// session the link opened.
class HostLink {
 public:
  HostLink(const MonotonicClock& clock, PairingStore& store, uint32_t boot_identity);

  void Push(std::span<const uint8_t> bytes) { feed_.Push(bytes); }
  void Tick();
  // Frames to write to the Host, oldest first.
  std::vector<std::vector<uint8_t>> TakeOutgoing();

  // Touch confirmation of the pending code.
  void ConfirmPairing(bool accepted);
  // Local explicit unpair (touch UI); retained state is untouched.
  void Unpair();
  // Runs when a Host asks for a calibration reset; returns whether it applied.
  void SetCalibrationResetHandler(std::function<bool()> handler) {
    calibration_reset_ = std::move(handler);
  }

  LinkPhase phase() const { return phase_; }
  std::optional<uint32_t> pending_code() const {
    return phase_ == LinkPhase::kAwaitingConfirmation ? std::optional(pending_code_) : std::nullopt;
  }
  bool paired() { return store_.PairedHost().has_value(); }
  const LinkDiagnostics& diagnostics() const { return diagnostics_; }
  const HostFeed& feed() const { return feed_; }
  const HostState& state() const { return feed_.state(); }

 private:
  const MonotonicClock& clock_;
  PairingStore& store_;
  HostFeed feed_;
  LinkPhase phase_ = LinkPhase::kIdle;
  LinkDiagnostics diagnostics_;
  std::vector<std::vector<uint8_t>> outgoing_;
  uint32_t tx_sequence_ = 0;

  Identity pending_host_{};
  Identity pending_session_{};
  uint32_t pending_code_ = 0;
  uint64_t pending_since_ms_ = 0;

  Identity session_{};
  Identity host_{};
  std::optional<uint32_t> cached_command_;
  uint8_t cached_result_ = 0;

  std::function<bool()> calibration_reset_;

  void OnResult(const protocol::FrameStream::Result& result);
  void OnHello(const protocol::Envelope& envelope);
  void OnPair(const protocol::Envelope& envelope);
  void OnCommand(const protocol::Envelope& envelope);
  void OpenSession(const Identity& session, const Identity& host, uint32_t code);
  void CloseSession();
  void SendError(LinkErrorCode code);
  void Send(protocol::Envelope envelope);
};

}  // namespace firmware::domain

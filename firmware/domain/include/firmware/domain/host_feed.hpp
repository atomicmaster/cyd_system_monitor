// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "firmware/domain/host_state.hpp"
#include "protocol.hpp"

namespace firmware::domain {

// Monotonic time source. The simulator supplies a scripted clock; the board
// supplies esp_timer. Wall-clock (UTC) time only ever arrives from the Host.
class MonotonicClock {
 public:
  virtual ~MonotonicClock() = default;
  virtual uint64_t NowMs() const = 0;
};

// A partial frame idle for this long is treated as a false length.
constexpr uint64_t kPartialFrameIdleMs = 250;

struct FeedStats {
  uint32_t applied = 0;
  uint32_t rejected = 0;  // decode, version, or session/sequence failures
  protocol::Error last_error = protocol::Error::kNone;
};

// The single path from serial bytes to display state, shared by the ESP32
// serial adapter, the board replay adapter, and the simulator: framing ->
// decode -> session/sequence gate -> reducer. Rejected input never touches
// HostState (except marking an incompatible major).
class HostFeed {
 public:
  HostFeed(const MonotonicClock& clock, uint32_t boot_identity)
      : clock_(clock), reducer_(boot_identity) {}

  // A confirmed pairing/negotiation starts a fresh session at sequence one.
  void StartSession(const std::array<uint8_t, protocol::kSessionIdBytes>& session);
  // Negotiated links only accept snapshots inside an explicitly opened
  // session; before pairing (or after unpair) they are rejected.
  void RequireExplicitSession() { explicit_session_ = true; }
  void EndSession() { session_open_ = false; }
  bool session_open() const { return session_open_; }
  // Called for every framed result that is not an applied snapshot: control
  // messages, and decode errors (so the link can answer an incompatible
  // major with an Error message).
  using ControlHandler = std::function<void(const protocol::FrameStream::Result&)>;
  void SetControlHandler(ControlHandler handler) { control_handler_ = std::move(handler); }
  void ApplyHostUtc(uint64_t utc_ms);
  void Push(std::span<const uint8_t> bytes);
  // Advances staleness and abandons a partial frame that has sat idle: the
  // Host writes each frame in one burst, so a gap means a false length (after
  // corruption) rather than a slow frame.
  void Tick();

  const HostState& state() const { return reducer_.state(); }
  const FeedStats& stats() const { return stats_; }

 private:
  const MonotonicClock& clock_;
  HostStateReducer reducer_;
  protocol::FrameStream stream_;
  protocol::SessionReceiver session_;
  FeedStats stats_;
  uint64_t last_push_ms_ = 0;
  ControlHandler control_handler_;
  bool explicit_session_ = false;
  bool session_open_ = false;

  void Drain();
};

}  // namespace firmware::domain

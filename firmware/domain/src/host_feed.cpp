// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/host_feed.hpp"

namespace firmware::domain {

void HostFeed::StartSession(const std::array<uint8_t, protocol::kSessionIdBytes>& session) {
  session_.Reset(session);
  session_open_ = true;
}

void HostFeed::ApplyHostUtc(uint64_t utc_ms, std::optional<int16_t> local_offset_minutes) {
  reducer_.SetUtcAnchor(utc_ms, clock_.NowMs(), local_offset_minutes);
}

void HostFeed::Push(std::span<const uint8_t> bytes) {
  stream_.Push(bytes);
  last_push_ms_ = clock_.NowMs();
  Drain();
}

void HostFeed::Tick() {
  const uint64_t now = clock_.NowMs();
  reducer_.Advance(now);
  while (stream_.buffered_bytes() > 0 && now - last_push_ms_ >= kPartialFrameIdleMs) {
    stream_.DropLeadingByte();
    Drain();
  }
}

void HostFeed::Drain() {
  protocol::FrameStream::Result result;
  while (stream_.Next(&result)) {
    if (result.error == protocol::Error::kUnsupportedMajor) reducer_.MarkIncompatible();
    if (result.error != protocol::Error::kNone) {
      ++stats_.rejected;
      stats_.last_error = result.error;
      if (control_handler_) control_handler_(result);
      continue;
    }
    if (result.decoded.envelope.type != protocol::MessageType::kSnapshot) {
      if (control_handler_) control_handler_(result);
      continue;
    }
    if (explicit_session_ && !session_open_) {
      ++stats_.rejected;
      stats_.last_error = protocol::Error::kWrongSession;
      continue;
    }
    const auto gate = session_.Accept(result.decoded);
    if (gate != protocol::Error::kNone) {
      ++stats_.rejected;
      stats_.last_error = gate;
      continue;
    }
    const uint64_t now = clock_.NowMs();
    // The sample is as old as the Host says it is, however late it arrived.
    const uint64_t age = result.decoded.cpu.source_age_ms;
    reducer_.ApplySnapshot(result.decoded, now, now >= age ? now - age : 0);
    if (result.decoded.envelope.utc_ms) {
      reducer_.SetUtcAnchor(*result.decoded.envelope.utc_ms, now,
                            result.decoded.envelope.local_offset_minutes);
    }
    ++stats_.applied;
  }
}

}  // namespace firmware::domain

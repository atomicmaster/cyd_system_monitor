// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cyd::protocol {

// The first production USB slice is deliberately small.  All later metric
// fields are optional extensions; the Host always sends a complete snapshot.
constexpr std::size_t kMaxFrameBytes = 2048;
constexpr std::size_t kSessionIdBytes = 16;

enum class Error {
  kNone,
  kTruncated,
  kTooLarge,
  kBadCrc,
  kMalformed,
  kUnsupportedMajor,
  kWrongSession,
  kStaleSequence
};
enum class MessageType : uint8_t {
  kHello = 1,
  kHelloReply = 2,
  kPair = 3,
  kSnapshot = 4,
  kCommand = 5,
  kCommandReply = 6,
  kError = 7
};
enum class Availability : uint8_t {
  kAvailable,
  kUnsupported,
  kAbsent,
  kTemporarilyUnavailable,
  kError
};
enum class Freshness : uint8_t { kFresh, kStale };

struct Version {
  uint8_t major = 1;
  uint8_t minor = 0;
};
struct CpuSnapshot {
  Availability availability = Availability::kTemporarilyUnavailable;
  uint16_t utilization_per_mille = 0;
  uint32_t source_age_ms = 0;
  Freshness freshness = Freshness::kFresh;
  uint32_t configuration_revision = 0;
};
// Physical RAM Occupied: total minus free pages, as two byte counts. The
// display chooses the presentation; used never exceeds total.
struct RamSnapshot {
  Availability availability = Availability::kTemporarilyUnavailable;
  uint64_t used_bytes = 0;
  uint64_t total_bytes = 0;
  uint32_t source_age_ms = 0;
  Freshness freshness = Freshness::kFresh;
};
struct HelloBody {
  uint8_t supported_major = 1;
  uint8_t supported_minor = 0;
  uint32_t capability_bits = 0;
  uint32_t configuration_revision = 0;
  std::optional<uint64_t> utc_ms;
  std::optional<uint32_t> utc_uncertainty_ms;
};
struct PairBody {
  std::array<uint8_t, kSessionIdBytes> sender_identity{};
  std::array<uint8_t, kSessionIdBytes> intended_peer_identity{};
  uint32_t matching_code = 0;
};
struct CommandBody {
  uint32_t command_identity = 0;
  uint8_t operation = 0; // 1 unpair, 2 calibration reset
  // Required for CommandReply: 0 applied, 1 already applied, 2 rejected.
  std::optional<uint8_t> result;
};
struct Envelope {
  Version version{};
  MessageType type = MessageType::kHello;
  std::array<uint8_t, kSessionIdBytes> session{};
  uint32_t sequence = 0;
  // Optional wall-clock metadata is display/correlation context, never the
  // ordering key for retained evidence (boot identity + sequence are).
  std::optional<uint64_t> utc_ms;
  std::optional<uint32_t> utc_uncertainty_ms;
  // Host local offset from UTC in minutes east (wire value biased by +1440).
  std::optional<int16_t> local_offset_minutes;
  std::vector<uint8_t> body; // bounded opaque body for Error messages
  std::optional<HelloBody> hello;
  std::optional<PairBody> pair;
  std::optional<CommandBody> command;
};
struct Decoded {
  Envelope envelope;
  CpuSnapshot cpu{};
  bool has_cpu = false;
  // Optional metric groups (CORE_PROTOCOL.md "Metric groups"); absent when
  // the sender did not include them.
  RamSnapshot ram{};
  bool has_ram = false;
};

uint32_t Crc32(std::span<const uint8_t> bytes);
Error Encode(const Envelope &envelope, std::vector<uint8_t> *frame);
Error EncodeCpuSnapshot(const Envelope &envelope, const CpuSnapshot &cpu,
                        std::vector<uint8_t> *frame);
// A complete snapshot: CPU plus each optional metric group that is non-null.
Error EncodeSnapshot(const Envelope &envelope, const CpuSnapshot &cpu,
                     const RamSnapshot *ram, std::vector<uint8_t> *frame);
Error Decode(std::span<const uint8_t> frame, Decoded *result);
const char *ErrorName(Error error);

// Session sequence handling is separate from byte decoding so a reconnect can
// atomically discard all messages from the prior negotiated session.
class SessionReceiver {
public:
  Error Accept(const Decoded &decoded);
  void Reset(std::array<uint8_t, kSessionIdBytes> session);

private:
  std::array<uint8_t, kSessionIdBytes> session_{};
  uint32_t last_sequence_ = 0;
  bool active_ = false;
};

// Byte-stream framer for the serial link.  Bytes may arrive in any
// fragmentation.  A frame whose length or CRC is invalid cannot be trusted to
// mark its own end, so the framer discards one byte and rescans; a frame with
// a valid length and CRC but a bad body is consumed whole and reported.
class FrameStream {
public:
  struct Result {
    Error error = Error::kNone;
    Decoded decoded;
  };
  void Push(std::span<const uint8_t> bytes);
  // Returns false when more bytes are needed.
  bool Next(Result *result);
  // Gives up on a stalled partial frame: discards one byte so the caller can
  // rescan (call Next() again). For use after an idle gap in the byte stream.
  void DropLeadingByte();
  std::size_t dropped_bytes() const { return dropped_bytes_; }
  std::size_t buffered_bytes() const { return buffer_.size(); }

private:
  std::vector<uint8_t> buffer_;
  std::size_t dropped_bytes_ = 0;
};

} // namespace cyd::protocol

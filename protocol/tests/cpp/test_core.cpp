// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.hpp"

std::vector<uint8_t> ReadHexFixture(const std::string &name) {
  std::ifstream file(std::string(CYD_PROTOCOL_FIXTURE_DIR) + "/" + name);
  std::string line, hex;
  while (std::getline(file, line))
    if (!line.starts_with('#'))
      hex += line;
  std::vector<uint8_t> bytes;
  for (std::size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(
        static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

bool RunParityMatrix() {
  std::ifstream matrix(std::string(CYD_PROTOCOL_FIXTURE_DIR) +
                       "/parity-matrix.txt");
  std::string line;
  while (std::getline(matrix, line)) {
    if (line.empty() || line.starts_with('#'))
      continue;
    std::stringstream fields(line);
    std::string name, fixture, decoder, expected;
    std::getline(fields, name, '|');
    std::getline(fields, fixture, '|');
    std::getline(fields, decoder, '|');
    std::getline(fields, expected, '|');
    cyd::protocol::Decoded decoded;
    const auto actual =
        cyd::protocol::Decode(ReadHexFixture(fixture), &decoded);
    const auto expected_error =
        expected == "none"      ? cyd::protocol::Error::kNone
        : expected == "bad-crc" ? cyd::protocol::Error::kBadCrc
        : expected == "unsupported-major"
            ? cyd::protocol::Error::kUnsupportedMajor
        : expected == "malformed" ? cyd::protocol::Error::kMalformed
        : expected == "truncated" ? cyd::protocol::Error::kTruncated
        : expected == "too-large" ? cyd::protocol::Error::kTooLarge
                                  : cyd::protocol::Error::kStaleSequence;
    if (expected != "none" && expected != "bad-crc" &&
        expected != "unsupported-major" && expected != "malformed" &&
        expected != "truncated" && expected != "too-large")
      return false;
    if (actual != expected_error)
      return false;
  }
  return true;
}

bool RunSessionMatrix() {
  std::ifstream matrix(std::string(CYD_PROTOCOL_FIXTURE_DIR) +
                       "/session-parity-matrix.txt");
  std::string line;
  cyd::protocol::SessionReceiver receiver;
  while (std::getline(matrix, line)) {
    if (line.empty() || line.starts_with('#'))
      continue;
    std::stringstream fields(line);
    std::string action, session, sequence, expected;
    std::getline(fields, action, '|');
    std::getline(fields, session, '|');
    std::getline(fields, sequence, '|');
    std::getline(fields, expected, '|');
    std::array<uint8_t, cyd::protocol::kSessionIdBytes> id{};
    id[0] = static_cast<uint8_t>(std::stoul(session));
    if (action == "reset") {
      receiver.Reset(id);
      continue;
    }
    cyd::protocol::Decoded decoded;
    decoded.envelope.session = id;
    decoded.envelope.sequence = std::stoul(sequence);
    const auto actual = receiver.Accept(decoded);
    const char *name = cyd::protocol::ErrorName(actual);
    if (expected != name)
      return false;
  }
  return true;
}

// Stream framing: any fragmentation, garbage before/between frames, and a
// corrupted frame followed by a good one.
bool RunStreamTests(const std::vector<uint8_t> &good) {
  using cyd::protocol::Error;
  using cyd::protocol::FrameStream;
  {
    FrameStream stream;
    FrameStream::Result result;
    int frames = 0;
    for (int copy = 0; copy < 2; ++copy)
      for (auto byte : good) {
        stream.Push(std::span<const uint8_t>(&byte, 1));
        while (stream.Next(&result)) {
          if (result.error != Error::kNone)
            return false;
          ++frames;
        }
      }
    if (frames != 2 || stream.dropped_bytes() != 0 ||
        stream.buffered_bytes() != 0)
      return false;
  }
  {
    // Garbage (including a plausible-but-wrong length) then a good frame.
    std::vector<uint8_t> wire = {0xff, 0xff, 0x00, 0x00, 0x04};
    wire.insert(wire.end(), good.begin(), good.end());
    FrameStream stream;
    FrameStream::Result result;
    stream.Push(wire);
    if (!stream.Next(&result) || result.error != Error::kNone ||
        result.decoded.envelope.sequence != 7)
      return false;
    if (stream.Next(&result))
      return false;
  }
  {
    // A CRC-damaged frame is skipped.  Rescanning its bytes can find a
    // plausible length that waits for up to 1,022 bytes, so recovery is
    // bounded by that much later traffic rather than immediate.
    auto damaged = good;
    damaged[10] ^= 0x40;
    FrameStream stream;
    FrameStream::Result result;
    stream.Push(damaged);
    int recovered = 0;
    for (int i = 0; i < 40; ++i) {
      stream.Push(good);
      while (stream.Next(&result)) {
        if (result.error != Error::kNone)
          return false;
        ++recovered;
      }
    }
    if (recovered < 30 || stream.dropped_bytes() == 0)
      return false;
  }
  {
    // Valid length and CRC with a rejected body is consumed whole.
    auto bad = ReadHexFixture("cpu-snapshot-v1-incompatible-major.hex");
    std::vector<uint8_t> wire = bad;
    wire.insert(wire.end(), good.begin(), good.end());
    FrameStream stream;
    FrameStream::Result result;
    stream.Push(wire);
    if (!stream.Next(&result) || result.error != Error::kUnsupportedMajor ||
        stream.dropped_bytes() != 0)
      return false;
    if (!stream.Next(&result) || result.error != Error::kNone)
      return false;
  }
  return true;
}

int main() {
  cyd::protocol::Envelope e;
  e.type = cyd::protocol::MessageType::kSnapshot;
  e.sequence = 7;
  for (uint8_t i = 0; i < e.session.size(); ++i)
    e.session[i] = i;
  cyd::protocol::CpuSnapshot cpu{cyd::protocol::Availability::kAvailable, 723,
                                 1000, cyd::protocol::Freshness::kFresh, 4};
  std::vector<uint8_t> frame;
  if (cyd::protocol::EncodeCpuSnapshot(e, cpu, &frame) !=
      cyd::protocol::Error::kNone)
    return 1;
  const std::vector<uint8_t> golden = {
      0x00, 0x2f, 0xa6, 0x00, 0x01, 0x01, 0x00, 0x02, 0x04, 0x03,
      0x50, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x04, 0x07, 0x05,
      0xa5, 0x00, 0x00, 0x01, 0x19, 0x02, 0xd3, 0x02, 0x19, 0x03,
      0xe8, 0x03, 0x00, 0x04, 0x04, 0x6d, 0x86, 0x20, 0xdf};
  if (frame != golden)
    return 5;
  cyd::protocol::Decoded decoded;
  if (cyd::protocol::Decode(frame, &decoded) != cyd::protocol::Error::kNone ||
      !decoded.has_cpu || decoded.cpu.utilization_per_mille != 723 ||
      decoded.envelope.sequence != 7)
    return 2;
  if (!RunParityMatrix())
    return 21;
  if (!RunSessionMatrix())
    return 22;
  if (!RunStreamTests(frame))
    return 23;

  auto timed = e;
  timed.utc_ms = 1789765432123;
  timed.utc_uncertainty_ms = 42;
  std::vector<uint8_t> timed_frame;
  if (cyd::protocol::EncodeCpuSnapshot(timed, cpu, &timed_frame) !=
          cyd::protocol::Error::kNone ||
      cyd::protocol::Decode(timed_frame, &decoded) !=
          cyd::protocol::Error::kNone ||
      decoded.envelope.utc_ms != timed.utc_ms ||
      decoded.envelope.utc_uncertainty_ms != timed.utc_uncertainty_ms)
    return 14;

  // Control bodies are maps (not byte-string wrappers), and retain their
  // typed fields after the same bounded/canonical frame round trip.
  auto hello = e;
  hello.type = cyd::protocol::MessageType::kHello;
  hello.hello = cyd::protocol::HelloBody{1, 2, 0x21, 9, 1789765432123, 25};
  std::vector<uint8_t> control_frame;
  if (cyd::protocol::Encode(hello, &control_frame) !=
          cyd::protocol::Error::kNone ||
      cyd::protocol::Decode(control_frame, &decoded) !=
          cyd::protocol::Error::kNone ||
      !decoded.envelope.hello || decoded.envelope.hello->supported_minor != 2 ||
      decoded.envelope.hello->capability_bits != 0x21 ||
      decoded.envelope.hello->utc_uncertainty_ms != 25)
    return 15;

  auto pair = e;
  pair.type = cyd::protocol::MessageType::kPair;
  cyd::protocol::PairBody pair_body;
  pair_body.sender_identity.fill(0x11);
  pair_body.intended_peer_identity.fill(0x22);
  pair_body.matching_code = 123456;
  pair.pair = pair_body;
  if (cyd::protocol::Encode(pair, &control_frame) !=
          cyd::protocol::Error::kNone ||
      cyd::protocol::Decode(control_frame, &decoded) !=
          cyd::protocol::Error::kNone ||
      !decoded.envelope.pair ||
      decoded.envelope.pair->matching_code != 123456 ||
      decoded.envelope.pair->sender_identity != pair_body.sender_identity)
    return 16;

  auto command = e;
  command.type = cyd::protocol::MessageType::kCommandReply;
  command.command = cyd::protocol::CommandBody{42, 2, 1};
  if (cyd::protocol::Encode(command, &control_frame) !=
          cyd::protocol::Error::kNone ||
      cyd::protocol::Decode(control_frame, &decoded) !=
          cyd::protocol::Error::kNone ||
      !decoded.envelope.command ||
      decoded.envelope.command->command_identity != 42 ||
      decoded.envelope.command->operation != 2 ||
      decoded.envelope.command->result != 1)
    return 17;
  command.command->result = 3;
  if (cyd::protocol::Encode(command, &control_frame) !=
      cyd::protocol::Error::kMalformed)
    return 18;
  pair.pair->matching_code = 1000000;
  if (cyd::protocol::Encode(pair, &control_frame) !=
      cyd::protocol::Error::kMalformed)
    return 19;
  pair.pair->matching_code = 99999;
  if (cyd::protocol::Encode(pair, &control_frame) !=
      cyd::protocol::Error::kMalformed)
    return 20;

  // A stream may hand the frame decoder any proper prefix.  The caller can
  // keep buffering only when the error is truncated; no prefix is accepted.
  for (std::size_t prefix = 0; prefix < frame.size(); ++prefix) {
    if (cyd::protocol::Decode(std::span<const uint8_t>(frame.data(), prefix),
                              &decoded) != cyd::protocol::Error::kTruncated)
      return 6;
  }

  auto rewrite_crc = [](std::vector<uint8_t> *wire) {
    const auto crc = cyd::protocol::Crc32(
        std::span<const uint8_t>(wire->data() + 2, wire->size() - 6));
    for (int shift = 0; shift <= 24; shift += 8)
      (*wire)[wire->size() - 1 - shift / 8] = crc >> shift;
  };
  auto incompatible = frame;
  incompatible[4] = 2; // CBOR envelope key 0's major-version value.
  rewrite_crc(&incompatible);
  if (cyd::protocol::Decode(incompatible, &decoded) !=
      cyd::protocol::Error::kUnsupportedMajor)
    return 7;
  // Forward-compatible optional envelope keys must not prevent an older
  // decoder from applying the known snapshot body.
  auto unknown_optional = frame;
  unknown_optional[2] = 0xa7;
  unknown_optional.insert(unknown_optional.end() - 4, {0x08, 0x00});
  const auto unknown_length =
      static_cast<uint16_t>(unknown_optional.size() - 2);
  unknown_optional[0] = unknown_length >> 8;
  unknown_optional[1] = unknown_length;
  rewrite_crc(&unknown_optional);
  if (cyd::protocol::Decode(unknown_optional, &decoded) !=
      cyd::protocol::Error::kNone)
    return 16;
  auto invalid_availability = frame;
  // The exact canonical vector's CPU availability is the value at offset 32.
  invalid_availability[32] = 5;
  rewrite_crc(&invalid_availability);
  if (cyd::protocol::Decode(invalid_availability, &decoded) !=
      cyd::protocol::Error::kMalformed)
    return 9;
  auto noncanonical = frame;
  noncanonical[32] = 0x18;
  noncanonical.insert(noncanonical.begin() + 33, 0x00);
  const auto length = static_cast<uint16_t>(noncanonical.size() - 2);
  noncanonical[0] = length >> 8;
  noncanonical[1] = length;
  rewrite_crc(&noncanonical);
  if (cyd::protocol::Decode(noncanonical, &decoded) !=
      cyd::protocol::Error::kMalformed)
    return 10;
  auto oversized = frame;
  oversized[0] = 0x04;
  oversized[1] = 0x00;
  if (cyd::protocol::Decode(oversized, &decoded) !=
      cyd::protocol::Error::kTooLarge)
    return 11;

  cyd::protocol::SessionReceiver receiver;
  if (receiver.Accept(decoded) != cyd::protocol::Error::kNone ||
      receiver.Accept(decoded) != cyd::protocol::Error::kStaleSequence)
    return 12;
  frame.back() ^= 1;
  if (cyd::protocol::Decode(frame, &decoded) != cyd::protocol::Error::kBadCrc)
    return 13;
  std::printf("protocol_core_tests: ok (%zu-byte framed CPU snapshot)\n",
              frame.size());
  return 0;
}

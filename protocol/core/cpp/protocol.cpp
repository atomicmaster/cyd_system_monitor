// SPDX-License-Identifier: Apache-2.0
#include "protocol.hpp"

#include <algorithm>

namespace cyd::protocol {
namespace {
void UInt(std::vector<uint8_t> *out, uint8_t major, uint64_t value) {
  if (value < 24)
    out->push_back(static_cast<uint8_t>((major << 5) | value));
  else if (value <= 0xff) {
    out->push_back(static_cast<uint8_t>((major << 5) | 24));
    out->push_back(value);
  } else if (value <= 0xffff) {
    out->push_back(static_cast<uint8_t>((major << 5) | 25));
    out->push_back(value >> 8);
    out->push_back(value);
  } else if (value <= UINT32_MAX) {
    out->push_back(static_cast<uint8_t>((major << 5) | 26));
    for (int s = 24; s >= 0; s -= 8)
      out->push_back(value >> s);
  } else {
    out->push_back(static_cast<uint8_t>((major << 5) | 27));
    for (int s = 56; s >= 0; s -= 8)
      out->push_back(value >> s);
  }
}
void Bytes(std::vector<uint8_t> *out, std::span<const uint8_t> value) {
  UInt(out, 2, value.size());
  out->insert(out->end(), value.begin(), value.end());
}
bool ReadUInt(std::span<const uint8_t> in, std::size_t *p, uint8_t expected,
              uint64_t *value) {
  if (*p >= in.size() || (in[*p] >> 5) != expected)
    return false;
  uint8_t a = in[(*p)++] & 31;
  if (a < 24) {
    *value = a;
    return true;
  }
  std::size_t n = a == 24 ? 1 : a == 25 ? 2 : a == 26 ? 4 : a == 27 ? 8 : 0;
  if (!n || *p + n > in.size())
    return false;
  *value = 0;
  while (n--)
    *value = (*value << 8) | in[(*p)++];
  if ((a == 24 && *value < 24) || (a == 25 && *value <= 0xff) ||
      (a == 26 && *value <= 0xffff) || (a == 27 && *value <= UINT32_MAX))
    return false;
  return true;
}
bool ReadBytes(std::span<const uint8_t> in, std::size_t *p,
               std::vector<uint8_t> *value) {
  uint64_t n;
  if (!ReadUInt(in, p, 2, &n) || n > kMaxFrameBytes || *p + n > in.size())
    return false;
  value->assign(in.begin() + *p, in.begin() + *p + n);
  *p += n;
  return true;
}
bool Skip(std::span<const uint8_t> in, std::size_t *p, int depth = 0) {
  if (depth >= 4 || *p >= in.size())
    return false;
  uint8_t h = in[*p];
  uint8_t m = h >> 5;
  uint64_t n;
  if (m <= 1)
    return ReadUInt(in, p, m, &n);
  if (m == 2 || m == 3) {
    if (!ReadUInt(in, p, m, &n) || n > kMaxFrameBytes || *p + n > in.size())
      return false;
    *p += n;
    return true;
  }
  if (m == 4 || m == 5) {
    if (!ReadUInt(in, p, m, &n) || n > 16)
      return false;
    n *= m == 5 ? 2 : 1;
    for (; n > 0; --n) {
      if (!Skip(in, p, depth + 1))
        return false;
    }
    return true;
  }
  return false;
}
// A body's own map is not a nesting level: an unknown value's depth is counted
// from that value, wherever it sits.
bool SkipBody(std::span<const uint8_t> in, std::size_t *p) {
  if (*p < in.size() && (in[*p] >> 5) == 5) {
    uint64_t n;
    if (!ReadUInt(in, p, 5, &n) || n > 16)
      return false;
    for (n *= 2; n > 0; --n) {
      if (!Skip(in, p, 0))
        return false;
    }
    return true;
  }
  return Skip(in, p, 0);
}
void Map(std::vector<uint8_t> *out, uint8_t fields) {
  out->push_back(static_cast<uint8_t>(0xa0 | fields));
}
bool ValidCommand(const CommandBody &body, bool reply) {
  return body.operation >= 1 && body.operation <= 2 &&
         (reply ? body.result.has_value() && *body.result <= 2
                : !body.result.has_value());
}
Error EncodeControlBody(const Envelope &e, std::vector<uint8_t> *cbor) {
  switch (e.type) {
  case MessageType::kHello:
  case MessageType::kHelloReply: {
    if (!e.hello)
      return Error::kMalformed;
    const auto &b = *e.hello;
    Map(cbor, 4 + b.utc_ms.has_value() + b.utc_uncertainty_ms.has_value());
    UInt(cbor, 0, 0);
    UInt(cbor, 0, b.supported_major);
    UInt(cbor, 0, 1);
    UInt(cbor, 0, b.supported_minor);
    UInt(cbor, 0, 2);
    UInt(cbor, 0, b.capability_bits);
    UInt(cbor, 0, 3);
    UInt(cbor, 0, b.configuration_revision);
    if (b.utc_ms) {
      UInt(cbor, 0, 4);
      UInt(cbor, 0, *b.utc_ms);
    }
    if (b.utc_uncertainty_ms) {
      UInt(cbor, 0, 5);
      UInt(cbor, 0, *b.utc_uncertainty_ms);
    }
    return Error::kNone;
  }
  case MessageType::kPair: {
    if (!e.pair || e.pair->matching_code < 100000 ||
        e.pair->matching_code > 999999)
      return Error::kMalformed;
    Map(cbor, 3);
    UInt(cbor, 0, 0);
    Bytes(cbor, e.pair->sender_identity);
    UInt(cbor, 0, 1);
    Bytes(cbor, e.pair->intended_peer_identity);
    UInt(cbor, 0, 2);
    UInt(cbor, 0, e.pair->matching_code);
    return Error::kNone;
  }
  case MessageType::kCommand:
  case MessageType::kCommandReply: {
    if (!e.command ||
        !ValidCommand(*e.command, e.type == MessageType::kCommandReply))
      return Error::kMalformed;
    Map(cbor, e.type == MessageType::kCommandReply ? 3 : 2);
    UInt(cbor, 0, 0);
    UInt(cbor, 0, e.command->command_identity);
    UInt(cbor, 0, 1);
    UInt(cbor, 0, e.command->operation);
    if (e.command->result) {
      UInt(cbor, 0, 2);
      UInt(cbor, 0, *e.command->result);
    }
    return Error::kNone;
  }
  default:
    if (e.body.size() > 512)
      return Error::kTooLarge;
    Bytes(cbor, e.body);
    return Error::kNone;
  }
}
Error EncodeBody(const Envelope &e, const CpuSnapshot *cpu,
                 std::vector<uint8_t> *cbor) {
  cbor->clear();
  const uint8_t envelope_fields =
      6 + e.utc_ms.has_value() + e.utc_uncertainty_ms.has_value();
  cbor->push_back(static_cast<uint8_t>(0xa0 | envelope_fields));
  UInt(cbor, 0, 0);
  UInt(cbor, 0, e.version.major);
  UInt(cbor, 0, 1);
  UInt(cbor, 0, e.version.minor);
  UInt(cbor, 0, 2);
  UInt(cbor, 0, static_cast<uint8_t>(e.type));
  UInt(cbor, 0, 3);
  Bytes(cbor, e.session);
  UInt(cbor, 0, 4);
  UInt(cbor, 0, e.sequence);
  UInt(cbor, 0, 5);
  if (cpu) {
    Map(cbor, 5);
    UInt(cbor, 0, 0);
    UInt(cbor, 0, static_cast<uint8_t>(cpu->availability));
    UInt(cbor, 0, 1);
    UInt(cbor, 0, cpu->utilization_per_mille);
    UInt(cbor, 0, 2);
    UInt(cbor, 0, cpu->source_age_ms);
    UInt(cbor, 0, 3);
    UInt(cbor, 0, static_cast<uint8_t>(cpu->freshness));
    UInt(cbor, 0, 4);
    UInt(cbor, 0, cpu->configuration_revision);
  } else {
    const Error error = EncodeControlBody(e, cbor);
    if (error != Error::kNone)
      return error;
  }
  if (e.utc_ms) {
    UInt(cbor, 0, 6);
    UInt(cbor, 0, *e.utc_ms);
  }
  if (e.utc_uncertainty_ms) {
    UInt(cbor, 0, 7);
    UInt(cbor, 0, *e.utc_uncertainty_ms);
  }
  return cbor->size() + 6 <= kMaxFrameBytes ? Error::kNone : Error::kTooLarge;
}
bool ReadControlBody(std::span<const uint8_t> c, std::size_t *p, Envelope *e) {
  uint64_t count;
  if (!ReadUInt(c, p, 5, &count) || count > 16)
    return false;
  // Keys above a type's last known key are unknown optional fields.
  const uint64_t known_max =
      e->type == MessageType::kHello || e->type == MessageType::kHelloReply ? 5
      : e->type == MessageType::kPair || e->type == MessageType::kCommandReply
          ? 2
          : 1;
  uint64_t previous = UINT64_MAX;
  bool fields[6] = {};
  HelloBody hello;
  PairBody pair;
  CommandBody command;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t key, value;
    if (!ReadUInt(c, p, 0, &key) || (i && key <= previous))
      return false;
    previous = key;
    if (key > known_max) {
      if (!Skip(c, p))
        return false;
      continue;
    }
    fields[key] = true;
    if ((e->type == MessageType::kPair) && (key == 0 || key == 1)) {
      std::vector<uint8_t> identity;
      if (!ReadBytes(c, p, &identity) || identity.size() != kSessionIdBytes)
        return false;
      auto &destination =
          key == 0 ? pair.sender_identity : pair.intended_peer_identity;
      std::copy(identity.begin(), identity.end(), destination.begin());
      continue;
    }
    if (!ReadUInt(c, p, 0, &value))
      return false;
    if ((e->type == MessageType::kHello ||
         e->type == MessageType::kHelloReply)) {
      if ((key < 2 && value > UINT8_MAX) ||
          ((key == 2 || key == 3) && value > UINT32_MAX) ||
          (key == 5 && value > UINT32_MAX))
        return false;
      if (key == 0)
        hello.supported_major = value;
      if (key == 1)
        hello.supported_minor = value;
      if (key == 2)
        hello.capability_bits = value;
      if (key == 3)
        hello.configuration_revision = value;
      if (key == 4)
        hello.utc_ms = value;
      if (key == 5)
        hello.utc_uncertainty_ms = value;
    } else if (e->type == MessageType::kPair) {
      if (key != 2 || value < 100000 || value > 999999)
        return false;
      pair.matching_code = value;
    } else {
      if (key > (e->type == MessageType::kCommandReply ? 2 : 1) ||
          (key == 0 && value > UINT32_MAX) || (key > 0 && value > UINT8_MAX))
        return false;
      if (key == 0)
        command.command_identity = value;
      if (key == 1)
        command.operation = value;
      if (key == 2)
        command.result = value;
    }
  }
  if (e->type == MessageType::kHello || e->type == MessageType::kHelloReply) {
    if (!fields[0] || !fields[1] || !fields[2] || !fields[3])
      return false;
    e->hello = hello;
  } else if (e->type == MessageType::kPair) {
    if (!fields[0] || !fields[1] || !fields[2])
      return false;
    e->pair = pair;
  } else {
    if (!fields[0] || !fields[1] ||
        (e->type == MessageType::kCommandReply && !fields[2]) ||
        !ValidCommand(command, e->type == MessageType::kCommandReply))
      return false;
    e->command = command;
  }
  return true;
}
} // namespace
uint32_t Crc32(std::span<const uint8_t> b) {
  uint32_t c = 0xffffffff;
  for (uint8_t x : b) {
    c ^= x;
    for (int i = 0; i < 8; i++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}
Error Encode(const Envelope &e, std::vector<uint8_t> *out) {
  std::vector<uint8_t> c;
  Error x = EncodeBody(e, nullptr, &c);
  if (x != Error::kNone)
    return x;
  out->clear();
  uint32_t n = c.size() + 4;
  out->push_back(n >> 8);
  out->push_back(n);
  out->insert(out->end(), c.begin(), c.end());
  uint32_t z = Crc32(c);
  for (int s = 24; s >= 0; s -= 8)
    out->push_back(z >> s);
  return Error::kNone;
}
Error EncodeCpuSnapshot(const Envelope &e, const CpuSnapshot &cpu,
                        std::vector<uint8_t> *out) {
  std::vector<uint8_t> c;
  Error x = EncodeBody(e, &cpu, &c);
  if (x != Error::kNone)
    return x;
  out->clear();
  uint32_t n = c.size() + 4;
  out->push_back(n >> 8);
  out->push_back(n);
  out->insert(out->end(), c.begin(), c.end());
  uint32_t z = Crc32(c);
  for (int s = 24; s >= 0; s -= 8)
    out->push_back(z >> s);
  return Error::kNone;
}
Error Decode(std::span<const uint8_t> f, Decoded *r) {
  if (f.size() < 7)
    return Error::kTruncated;
  uint16_t n = (f[0] << 8) | f[1];
  if (n > kMaxFrameBytes - 2)
    return Error::kTooLarge;
  if (n + 2 != f.size())
    return Error::kTruncated;
  auto c = f.subspan(2, n - 4);
  uint32_t z = 0;
  for (auto b : f.subspan(f.size() - 4))
    z = (z << 8) | b;
  if (Crc32(c) != z)
    return Error::kBadCrc;
  std::size_t p = 0;
  uint64_t count;
  if (!ReadUInt(c, &p, 5, &count) || count > 16)
    return Error::kMalformed;
  Decoded d;
  bool got[8] = {};
  uint64_t previous_key = UINT64_MAX;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t k;
    if (!ReadUInt(c, &p, 0, &k) || (i && k <= previous_key))
      return Error::kMalformed;
    previous_key = k;
    if (k > 7) {
      if (!Skip(c, &p))
        return Error::kMalformed;
      continue;
    }
    if (got[k])
      return Error::kMalformed;
    got[k] = true;
    uint64_t v;
    if (k == 5 && (static_cast<uint8_t>(d.envelope.type) <
                       static_cast<uint8_t>(MessageType::kHello) ||
                   static_cast<uint8_t>(d.envelope.type) >
                       static_cast<uint8_t>(MessageType::kError)))
      return Error::kMalformed; // Unknown message type: its body is unknowable.
    if (k == 5 && d.envelope.type != MessageType::kSnapshot) {
      // Every non-snapshot body is bounded (512 bytes) before it is read.
      std::size_t end = p;
      if (!SkipBody(c, &end))
        return Error::kMalformed;
      if (end - p > 512)
        return Error::kTooLarge;
    }
    if (k == 3) {
      std::vector<uint8_t> b;
      if (!ReadBytes(c, &p, &b) || b.size() != 16)
        return Error::kMalformed;
      std::copy(b.begin(), b.end(), d.envelope.session.begin());
    } else if (k == 5) {
      if (p >= c.size())
        return Error::kMalformed;
      if ((c[p] >> 5) == 5 && d.envelope.type == MessageType::kSnapshot) {
        uint64_t m;
        if (!ReadUInt(c, &p, 5, &m) || m > 8)
          return Error::kMalformed;
        d.has_cpu = true;
        bool fields[5] = {};
        for (uint64_t j = 0; j < m; j++) {
          uint64_t q, w;
          if (!ReadUInt(c, &p, 0, &q))
            return Error::kMalformed;
          if (q > 4) {
            if (!Skip(c, &p))
              return Error::kMalformed;
            continue;
          }
          if (fields[q] || !ReadUInt(c, &p, 0, &w))
            return Error::kMalformed;
          if ((q == 0 || q == 3) && w > 0xff)
            return Error::kMalformed;
          if ((q == 1 && w > 1000) || ((q == 2 || q == 4) && w > UINT32_MAX))
            return Error::kMalformed;
          fields[q] = true;
          if (q == 0)
            d.cpu.availability = static_cast<Availability>(w);
          if (q == 1)
            d.cpu.utilization_per_mille = w;
          if (q == 2)
            d.cpu.source_age_ms = w;
          if (q == 3)
            d.cpu.freshness = static_cast<Freshness>(w);
          if (q == 4)
            d.cpu.configuration_revision = w;
        }
        if (!fields[0])
          return Error::kMalformed;
        for (bool field : fields) {
          if (!field)
            return Error::kMalformed;
        }
        if (static_cast<uint8_t>(d.cpu.availability) >
                static_cast<uint8_t>(Availability::kError) ||
            d.cpu.utilization_per_mille > 1000 ||
            static_cast<uint8_t>(d.cpu.freshness) >
                static_cast<uint8_t>(Freshness::kStale))
          return Error::kMalformed;
      } else if (d.envelope.type == MessageType::kHello ||
                 d.envelope.type == MessageType::kHelloReply ||
                 d.envelope.type == MessageType::kPair ||
                 d.envelope.type == MessageType::kCommand ||
                 d.envelope.type == MessageType::kCommandReply) {
        if (!ReadControlBody(c, &p, &d.envelope))
          return Error::kMalformed;
      } else if (!ReadBytes(c, &p, &d.envelope.body))
        return Error::kMalformed;
    } else {
      if (!ReadUInt(c, &p, 0, &v))
        return Error::kMalformed;
      if ((k == 0 || k == 1 || k == 2) && v > 0xff)
        return Error::kMalformed;
      // Major is the first envelope key; a future major may change everything
      // after it, so nothing after it is interpreted.
      if (k == 0 && v != 1)
        return Error::kUnsupportedMajor;
      if (k == 4 && v > UINT32_MAX)
        return Error::kMalformed;
      if (k == 0)
        d.envelope.version.major = v;
      if (k == 1)
        d.envelope.version.minor = v;
      if (k == 2)
        d.envelope.type = static_cast<MessageType>(v);
      if (k == 4)
        d.envelope.sequence = v;
      if (k == 6)
        d.envelope.utc_ms = v;
      if (k == 7) {
        if (v > UINT32_MAX)
          return Error::kMalformed;
        d.envelope.utc_uncertainty_ms = v;
      }
    }
  }
  if (p != c.size() || !got[0] || !got[1] || !got[2] || !got[3] || !got[4] ||
      !got[5])
    return Error::kMalformed;
  if (static_cast<uint8_t>(d.envelope.type) <
          static_cast<uint8_t>(MessageType::kHello) ||
      static_cast<uint8_t>(d.envelope.type) >
          static_cast<uint8_t>(MessageType::kError))
    return Error::kMalformed;
  if (d.envelope.type == MessageType::kSnapshot && !d.has_cpu)
    return Error::kMalformed; // A snapshot body is a field map, nothing else.
  *r = std::move(d);
  return Error::kNone;
}
const char *ErrorName(Error e) {
  switch (e) {
  case Error::kNone:
    return "none";
  case Error::kTruncated:
    return "truncated";
  case Error::kTooLarge:
    return "too-large";
  case Error::kBadCrc:
    return "bad-crc";
  case Error::kMalformed:
    return "malformed";
  case Error::kUnsupportedMajor:
    return "unsupported-major";
  case Error::kWrongSession:
    return "wrong-session";
  case Error::kStaleSequence:
    return "stale-sequence";
  }
  return "unknown";
}
Error SessionReceiver::Accept(const Decoded &d) {
  if (!active_) {
    Reset(d.envelope.session);
  }
  if (d.envelope.session != session_)
    return Error::kWrongSession;
  if (d.envelope.sequence <= last_sequence_)
    return Error::kStaleSequence;
  last_sequence_ = d.envelope.sequence;
  return Error::kNone;
}
void SessionReceiver::Reset(std::array<uint8_t, 16> s) {
  session_ = s;
  last_sequence_ = 0;
  active_ = true;
}
void FrameStream::Push(std::span<const uint8_t> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void FrameStream::DropLeadingByte() {
  if (buffer_.empty()) return;
  buffer_.erase(buffer_.begin());
  ++dropped_bytes_;
}

bool FrameStream::Next(Result *result) {
  // Smallest possible frame: 2 length + 1 CBOR + 4 CRC.
  while (buffer_.size() >= 2) {
    const std::size_t n = (buffer_[0] << 8) | buffer_[1];
    if (n < 5 || n > kMaxFrameBytes - 2) {
      buffer_.erase(buffer_.begin());
      ++dropped_bytes_;
      continue;
    }
    if (buffer_.size() < n + 2)
      return false;
    const std::span<const uint8_t> frame(buffer_.data(), n + 2);
    uint32_t crc = 0;
    for (auto b : frame.subspan(frame.size() - 4))
      crc = (crc << 8) | b;
    if (Crc32(frame.subspan(2, n - 4)) != crc) {
      buffer_.erase(buffer_.begin());
      ++dropped_bytes_;
      continue;
    }
    result->decoded = Decoded{};
    result->error = Decode(frame, &result->decoded);
    buffer_.erase(buffer_.begin(), buffer_.begin() + n + 2);
    return true;
  }
  return false;
}

} // namespace cyd::protocol

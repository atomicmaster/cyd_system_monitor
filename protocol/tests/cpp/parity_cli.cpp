// SPDX-License-Identifier: Apache-2.0
// Differential-parity harness, C++ side. Reads lines on stdin:
//   F <hex>  one candidate frame       S <hex>  a byte stream, one push
//   B <hex>  a byte stream, one byte per push
// and prints one canonical outcome line each. host/protocol's `parity_cli`
// prints the same format; protocol/tools/parity_diff.py compares them.
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace p = cyd::protocol;

namespace {

std::string Hex(const uint8_t *data, std::size_t size) {
  std::string out;
  char byte[3];
  for (std::size_t i = 0; i < size; ++i) {
    std::snprintf(byte, sizeof byte, "%02x", data[i]);
    out += byte;
  }
  return out.empty() ? "-" : out;
}
template <typename T> std::string Opt(const std::optional<T> &value) {
  return value ? std::to_string(*value) : "-";
}
const char *KindName(p::MessageType type) {
  switch (type) {
  case p::MessageType::kHello:
    return "hello";
  case p::MessageType::kHelloReply:
    return "hello-reply";
  case p::MessageType::kPair:
    return "pair";
  case p::MessageType::kSnapshot:
    return "snapshot";
  case p::MessageType::kCommand:
    return "command";
  case p::MessageType::kCommandReply:
    return "command-reply";
  case p::MessageType::kError:
    return "error";
  }
  return "?";
}

std::string Body(const p::Decoded &d) {
  const auto &e = d.envelope;
  std::ostringstream out;
  if (e.type == p::MessageType::kSnapshot) {
    out << static_cast<int>(d.cpu.availability) << ','
        << d.cpu.utilization_per_mille << ',' << d.cpu.source_age_ms << ','
        << static_cast<int>(d.cpu.freshness) << ','
        << d.cpu.configuration_revision;
    if (d.groups.ram) {
      const auto &g = *d.groups.ram;
      out << ";ram=" << static_cast<int>(g.availability) << ',' << g.used_bytes
          << ',' << g.total_bytes << ',' << g.source_age_ms << ','
          << static_cast<int>(g.freshness);
    }
    if (d.groups.storage) {
      const auto &g = *d.groups.storage;
      out << ";storage=" << static_cast<int>(g.capacity_availability) << ','
          << g.total_bytes << ',' << g.available_bytes << ','
          << g.capacity_age_ms << ',' << static_cast<int>(g.io_availability)
          << ',' << g.read_bytes_per_s << ',' << g.write_bytes_per_s << ','
          << g.io_age_ms;
    }
    if (d.groups.battery) {
      const auto &g = *d.groups.battery;
      out << ";battery=" << static_cast<int>(g.charge_availability) << ','
          << g.charge_per_mille << ',' << static_cast<int>(g.state) << ','
          << g.time_remaining_min << ',' << g.charge_age_ms << ','
          << static_cast<int>(g.condition_availability) << ','
          << g.max_capacity_per_mille << ',' << static_cast<int>(g.condition)
          << ',' << g.cycle_count << ',' << g.condition_age_ms;
    }
    if (d.groups.agents) {
      const auto &g = *d.groups.agents;
      out << ";agents=" << static_cast<int>(g.availability) << ','
          << g.source_age_ms << ',' << static_cast<int>(g.state[0]) << ','
          << static_cast<int>(g.state[1]) << ','
          << static_cast<int>(g.state[2]);
    }
    if (d.groups.network) {
      const auto &g = *d.groups.network;
      const auto mac = [](const auto &m) {
        return m ? Hex(m->data(), m->size()) : std::string("-");
      };
      out << ";network=" << static_cast<int>(g.traffic_availability) << ','
          << g.traffic_age_ms << ',' << static_cast<int>(g.route_kind) << ','
          << (g.via_tunnel ? 1 : 0) << ',' << g.rx_bytes_per_s << ','
          << g.tx_bytes_per_s << ',' << static_cast<int>(g.context_availability)
          << ',' << g.context_age_ms << ',' << static_cast<int>(g.coverage)
          << ',' << static_cast<int>(g.channel) << ',' << mac(g.station) << ','
          << Hex(g.ssid.data(), g.ssid.size()) << ',' << mac(g.bssid) << ',';
      if (g.links) {
        std::vector<uint8_t> kinds;
        for (const auto kind : *g.links)
          kinds.push_back(static_cast<uint8_t>(kind));
        out << Hex(kinds.data(), kinds.size());
      } else {
        out << '-';
      }
    }
  } else if (e.hello) {
    out << static_cast<int>(e.hello->supported_major) << ','
        << static_cast<int>(e.hello->supported_minor) << ','
        << e.hello->capability_bits << ',' << e.hello->configuration_revision
        << ',' << Opt(e.hello->utc_ms) << ','
        << Opt(e.hello->utc_uncertainty_ms);
  } else if (e.pair) {
    out << Hex(e.pair->sender_identity.data(), 16) << ','
        << Hex(e.pair->intended_peer_identity.data(), 16) << ','
        << e.pair->matching_code;
  } else if (e.command) {
    out << e.command->command_identity << ','
        << static_cast<int>(e.command->operation) << ','
        << (e.command->result ? std::to_string(*e.command->result) : "-");
  } else {
    out << Hex(e.body.data(), e.body.size());
  }
  return out.str();
}

std::string Summary(p::Error error, const p::Decoded &d) {
  if (error != p::Error::kNone)
    return std::string("err ") + p::ErrorName(error);
  const auto &e = d.envelope;
  std::ostringstream out;
  out << "ok " << KindName(e.type) << " v" << static_cast<int>(e.version.major)
      << '.' << static_cast<int>(e.version.minor)
      << " sess=" << Hex(e.session.data(), 16) << " seq=" << e.sequence
      << " utc=" << Opt(e.utc_ms) << " unc=" << Opt(e.utc_uncertainty_ms)
      << " off=" << Opt(e.local_offset_minutes) << " body=" << Body(d);
  return out.str();
}

std::string Reencode(const p::Decoded &d) {
  std::vector<uint8_t> frame;
  const auto error =
      d.envelope.type == p::MessageType::kSnapshot
          ? p::EncodeSnapshot(d.envelope, d.cpu, d.groups, &frame)
          : p::Encode(d.envelope, &frame);
  return error == p::Error::kNone ? Hex(frame.data(), frame.size())
                                  : std::string("err ") + p::ErrorName(error);
}

std::vector<uint8_t> FromHex(const std::string &hex) {
  std::vector<uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    bytes.push_back(
        static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

std::string RunStream(const std::vector<uint8_t> &bytes, bool bytewise) {
  p::FrameStream stream;
  std::string out;
  p::FrameStream::Result result;
  auto drain = [&] {
    while (stream.Next(&result))
      out += Summary(result.error, result.decoded) + ";";
  };
  if (bytewise) {
    for (auto b : bytes) {
      stream.Push(std::span<const uint8_t>(&b, 1));
      drain();
    }
  } else {
    stream.Push(bytes);
    drain();
  }
  while (stream.buffered_bytes() > 0) {
    stream.DropLeadingByte();
    drain();
  }
  return out + " dropped=" + std::to_string(stream.dropped_bytes());
}

} // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    const char mode = line.empty() ? '?' : line[0];
    const auto bytes = FromHex(line.size() > 2 ? line.substr(2) : "");
    if (mode == 'F') {
      p::Decoded decoded;
      const auto error = p::Decode(bytes, &decoded);
      std::string out = Summary(error, decoded);
      if (error == p::Error::kNone)
        out += " re=" + Reencode(decoded);
      std::puts(out.c_str());
    } else {
      std::puts(RunStream(bytes, mode == 'B').c_str());
    }
  }
  return 0;
}

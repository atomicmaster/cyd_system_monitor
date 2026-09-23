// SPDX-License-Identifier: Apache-2.0
//
// Line-driven harness around the real firmware HostLink, so the Rust Host
// client can be exercised against the exact C++ state machine the board
// runs (`./dev check host` drives it). Commands on stdin:
//   PUSH <hex>     bytes from the Host
//   CONFIRM 1|0    touch confirmation
//   TICK <ms>      advance the clock, then tick
//   UNPAIR         local unpair
// After each command: zero or more "OUT <hex>" frames, then one
// "STATE phase=<n> paired=<0|1> seen=<0|1> cpu=<per-mille> ram=<used>/<total>|-
// storage=<total>/<avail>/<read>/<write>|-
// battery=<charge-availability>/<per-mille>/<state>/<max-capacity>|-
// agents=<availability>/<codex>/<claude>/<grok>|-
// network=<traffic-availability>/<kind>/<via-tunnel>/<rx>/<tx>/<context>/<coverage>/<channel>|-
// code=<n|->" line.
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "firmware/domain/host_link.hpp"

using namespace firmware::domain;

namespace {

struct Clock : MonotonicClock {
  uint64_t now = 1000;
  uint64_t NowMs() const override { return now; }
};

struct Store : PairingStore {
  Identity device{};
  std::optional<Identity> host;
  Store() { device.fill(0xD0); }
  Identity DeviceIdentity() override { return device; }
  std::optional<Identity> PairedHost() override { return host; }
  bool SavePairedHost(const Identity& h) override {
    host = h;
    return true;
  }
  bool ClearPairedHost() override {
    host.reset();
    return true;
  }
};

std::vector<uint8_t> FromHex(const std::string& hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

}  // namespace

int main() {
  Clock clock;
  Store store;
  HostLink link(clock, store, 1);
  link.SetCalibrationResetHandler([] { return true; });

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string verb, arg;
    in >> verb >> arg;
    if (verb == "PUSH") {
      link.Push(FromHex(arg));
    } else if (verb == "CONFIRM") {
      link.ConfirmPairing(arg == "1");
    } else if (verb == "TICK") {
      clock.now += std::stoull(arg);
      link.Tick();
    } else if (verb == "UNPAIR") {
      link.Unpair();
    }
    for (const auto& frame : link.TakeOutgoing()) {
      std::printf("OUT ");
      for (uint8_t b : frame) std::printf("%02x", b);
      std::printf("\n");
    }
    const auto& state = link.state();
    std::printf("STATE phase=%d paired=%d seen=%d cpu=%u ram=", static_cast<int>(link.phase()),
                link.paired() ? 1 : 0, state.has_seen_host ? 1 : 0, state.cpu.value_per_mille);
    if (state.ram.has_data) {
      std::printf("%llu/%llu", static_cast<unsigned long long>(state.ram.used_bytes),
                  static_cast<unsigned long long>(state.ram.total_bytes));
    } else {
      std::printf("-");
    }
    std::printf(" storage=");
    if (state.storage.has_data) {
      std::printf("%llu/%llu/%llu/%llu", static_cast<unsigned long long>(state.storage.total_bytes),
                  static_cast<unsigned long long>(state.storage.available_bytes),
                  static_cast<unsigned long long>(state.storage.read_bytes_per_s),
                  static_cast<unsigned long long>(state.storage.write_bytes_per_s));
    } else {
      std::printf("-");
    }
    std::printf(" battery=");
    if (state.battery.has_data) {
      std::printf("%u/%u/%u/%u", static_cast<unsigned>(state.battery.charge_availability),
                  static_cast<unsigned>(state.battery.charge_per_mille),
                  static_cast<unsigned>(state.battery.state),
                  static_cast<unsigned>(state.battery.max_capacity_per_mille));
    } else {
      std::printf("-");
    }
    std::printf(" agents=");
    if (state.agents.has_data) {
      std::printf("%u/%u/%u/%u", static_cast<unsigned>(state.agents.availability),
                  static_cast<unsigned>(state.agents.state[0]),
                  static_cast<unsigned>(state.agents.state[1]),
                  static_cast<unsigned>(state.agents.state[2]));
    } else {
      std::printf("-");
    }
    std::printf(" network=");
    if (state.network.has_data) {
      const auto& n = state.network;
      std::printf("%u/%u/%u/%llu/%llu/%u/%u/%u", static_cast<unsigned>(n.traffic_availability),
                  static_cast<unsigned>(n.route_kind), n.via_tunnel ? 1u : 0u,
                  static_cast<unsigned long long>(n.rx_bytes_per_s),
                  static_cast<unsigned long long>(n.tx_bytes_per_s),
                  static_cast<unsigned>(n.context_availability), static_cast<unsigned>(n.coverage),
                  static_cast<unsigned>(n.channel));
    } else {
      std::printf("-");
    }
    std::printf(" code=");
    if (const auto code = link.pending_code()) {
      std::printf("%u\n", *code);
    } else {
      std::printf("-\n");
    }
    std::fflush(stdout);
  }
  return 0;
}

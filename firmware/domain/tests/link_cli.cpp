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
// "STATE phase=<n> paired=<0|1> seen=<0|1> cpu=<per-mille> code=<n|->" line.
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
    std::printf("STATE phase=%d paired=%d seen=%d cpu=%u code=", static_cast<int>(link.phase()),
                link.paired() ? 1 : 0, state.has_seen_host ? 1 : 0, state.cpu.value_per_mille);
    if (const auto code = link.pending_code()) {
      std::printf("%u\n", *code);
    } else {
      std::printf("-\n");
    }
    std::fflush(stdout);
  }
  return 0;
}

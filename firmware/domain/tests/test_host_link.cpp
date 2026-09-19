// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <vector>

#include "firmware/domain/host_link.hpp"

using namespace firmware::domain;
namespace proto = cyd::protocol;

namespace {

int g_failures = 0;
#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
      ++g_failures;                                       \
    }                                                     \
  } while (0)

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

Identity Id(uint8_t v) {
  Identity i;
  i.fill(v);
  return i;
}

std::vector<uint8_t> Frame(proto::Envelope e) {
  std::vector<uint8_t> f;
  CHECK(proto::Encode(e, &f) == proto::Error::kNone);
  return f;
}

std::vector<uint8_t> Hello(uint8_t major = 1, uint8_t minor = 0, std::optional<uint64_t> utc = {}) {
  proto::Envelope e;
  e.type = proto::MessageType::kHello;
  e.sequence = 1;
  e.hello = proto::HelloBody{.supported_major = major, .supported_minor = minor, .utc_ms = utc};
  return Frame(e);
}

std::vector<uint8_t> Pair(const Identity& session, const Identity& host, const Identity& intended,
                          uint32_t code) {
  proto::Envelope e;
  e.type = proto::MessageType::kPair;
  e.session = session;
  e.sequence = 2;
  e.pair = proto::PairBody{
      .sender_identity = host, .intended_peer_identity = intended, .matching_code = code};
  return Frame(e);
}

std::vector<uint8_t> Snapshot(const Identity& session, uint32_t seq, uint16_t per_mille) {
  proto::Envelope e;
  e.type = proto::MessageType::kSnapshot;
  e.session = session;
  e.sequence = seq;
  proto::CpuSnapshot cpu{proto::Availability::kAvailable, per_mille, 0, proto::Freshness::kFresh,
                         0};
  std::vector<uint8_t> f;
  CHECK(proto::EncodeCpuSnapshot(e, cpu, &f) == proto::Error::kNone);
  return f;
}

std::vector<uint8_t> Command(const Identity& session, uint32_t seq, uint32_t id, uint8_t op) {
  proto::Envelope e;
  e.type = proto::MessageType::kCommand;
  e.session = session;
  e.sequence = seq;
  e.command = proto::CommandBody{.command_identity = id, .operation = op};
  return Frame(e);
}

proto::Decoded Out(HostLink& link, size_t index = 0) {
  auto frames = link.TakeOutgoing();
  proto::Decoded d;
  if (index >= frames.size() || proto::Decode(frames[index], &d) != proto::Error::kNone) {
    std::printf("no decodable outgoing frame %zu (have %zu)\n", index, frames.size());
    ++g_failures;
  }
  return d;
}

uint8_t ErrorCode(const proto::Decoded& d) {
  CHECK(d.envelope.type == proto::MessageType::kError);
  return d.envelope.body.empty() ? 0 : d.envelope.body[0];
}

// Brings a fresh link to an open session for host H with session S.
void Pairing(HostLink& link, const Identity& s, const Identity& h) {
  link.Push(Hello());
  link.TakeOutgoing();
  link.Push(Pair(s, h, Id(0), 123456));
}

}  // namespace

int main() {
  const Identity S1 = Id(0x11), S2 = Id(0x22), H = Id(0xA1), H2 = Id(0xA2);

  {  // Nothing reaches the display before pairing.
    Clock clock;
    Store store;
    HostLink link(clock, store, 1);
    link.Push(Snapshot(S1, 1, 500));
    CHECK(!link.state().has_seen_host);
    link.Push(Hello());
    link.Push(Snapshot(S1, 1, 500));
    CHECK(!link.state().has_seen_host);
  }

  {  // Hello negotiates version, adopts Host UTC, and replies.
    Clock clock;
    Store store;
    HostLink link(clock, store, 1);
    link.Push(Hello(1, 3, 1'700'000'000'000ULL));
    auto reply = Out(link);
    CHECK(reply.envelope.type == proto::MessageType::kHelloReply);
    CHECK(reply.envelope.hello && reply.envelope.hello->supported_major == 1);
    CHECK(link.phase() == LinkPhase::kNegotiated);
    CHECK(link.diagnostics().host_minor == 3);
    CHECK(link.diagnostics().negotiated_minor == 0);
    CHECK(link.state().utc_anchor_ms == 1'700'000'000'000ULL);
  }

  {  // First pairing needs touch confirmation; then live data flows.
    Clock clock;
    Store store;
    HostLink link(clock, store, 1);
    Pairing(link, S1, H);
    CHECK(link.phase() == LinkPhase::kAwaitingConfirmation);
    CHECK(link.pending_code() == 123456u);
    CHECK(!store.host);
    CHECK(link.TakeOutgoing().empty());
    link.Push(Snapshot(S1, 1, 500));  // not yet
    CHECK(!link.state().has_seen_host);
    link.ConfirmPairing(true);
    auto pair = Out(link);
    CHECK(pair.envelope.type == proto::MessageType::kPair);
    CHECK(pair.envelope.session == S1);
    CHECK(pair.envelope.pair && pair.envelope.pair->sender_identity == store.device);
    CHECK(pair.envelope.pair->intended_peer_identity == H);
    CHECK(pair.envelope.pair->matching_code == 123456u);
    CHECK(store.host == H);
    CHECK(link.phase() == LinkPhase::kSessionOpen);
    link.Push(Snapshot(S1, 1, 500));
    CHECK(link.state().has_seen_host && link.state().cpu.value_per_mille == 500);
    link.Push(Snapshot(S2, 2, 900));  // another session is rejected
    CHECK(link.state().cpu.value_per_mille == 500);
  }

  {  // Declined and timed-out confirmation store nothing.
    Clock clock;
    Store store;
    HostLink link(clock, store, 1);
    Pairing(link, S1, H);
    link.ConfirmPairing(false);
    CHECK(ErrorCode(Out(link)) == 3);
    CHECK(!store.host && link.diagnostics().fault == LinkFault::kDeclined);
    Pairing(link, S1, H);
    clock.now += kPairingConfirmTimeoutMs;
    link.Tick();
    CHECK(ErrorCode(Out(link)) == 3);
    CHECK(!store.host && link.phase() == LinkPhase::kNegotiated);
  }

  {  // A paired Host reconnects without touch and gets a fresh session.
    Clock clock;
    Store store;
    store.host = H;
    HostLink link(clock, store, 1);
    Pairing(link, S1, H);
    CHECK(link.phase() == LinkPhase::kSessionOpen);
    CHECK(Out(link).envelope.type == proto::MessageType::kPair);
    link.Push(Snapshot(S1, 1, 300));
    link.Push(Hello());  // Host restarted: old session is over
    link.TakeOutgoing();
    link.Push(Snapshot(S1, 2, 999));
    CHECK(link.state().cpu.value_per_mille == 300);
    link.Push(Pair(S2, H, store.device, 654321));
    CHECK(link.phase() == LinkPhase::kSessionOpen);
    link.Push(Snapshot(S2, 1, 700));
    CHECK(link.state().cpu.value_per_mille == 700);
  }

  {  // One-to-one: a different Host is refused and gets no data.
    Clock clock;
    Store store;
    store.host = H;
    HostLink link(clock, store, 1);
    Pairing(link, S1, H2);
    link.TakeOutgoing();
    Pairing(link, S1, H2);
    CHECK(ErrorCode(Out(link)) == 2);
    CHECK(link.diagnostics().fault == LinkFault::kNotPairedWithHost);
    link.Push(Snapshot(S1, 1, 100));
    CHECK(!link.state().has_seen_host);
  }

  {  // Incompatible versions: visible cause, nothing applied.
    Clock clock;
    Store store;
    HostLink link(clock, store, 1);
    link.Push(Hello(2, 0));
    CHECK(ErrorCode(Out(link)) == 1);
    CHECK(link.phase() == LinkPhase::kIncompatible);
    CHECK(link.diagnostics().host_major == 2);
    CHECK(link.diagnostics().fault == LinkFault::kIncompatibleMajor);
    link.Push(Pair(S1, H, Id(0), 123456));
    CHECK(ErrorCode(Out(link)) == 4);
    // An envelope with a different major is also answered with Error.
    Clock c2;
    Store s2;
    HostLink l2(c2, s2, 1);
    auto f = Hello();
    // Rewrite major (key 0 value) to 2 and fix the CRC.
    std::vector<uint8_t> cbor(f.begin() + 2, f.end() - 4);
    CHECK(cbor.size() > 2 && cbor[1] == 0x00 && cbor[2] == 0x01);
    cbor[2] = 0x02;
    const uint32_t crc = proto::Crc32(cbor);
    std::vector<uint8_t> g = {static_cast<uint8_t>((cbor.size() + 4) >> 8),
                              static_cast<uint8_t>(cbor.size() + 4)};
    g.insert(g.end(), cbor.begin(), cbor.end());
    for (int s = 24; s >= 0; s -= 8) g.push_back(static_cast<uint8_t>(crc >> s));
    l2.Push(g);
    CHECK(ErrorCode(Out(l2)) == 1);
    CHECK(l2.phase() == LinkPhase::kIncompatible);
  }

  {  // Pair before Hello is refused.
    Clock clock;
    Store store;
    HostLink link(clock, store, 1);
    link.Push(Pair(S1, H, Id(0), 123456));  // no Hello yet
    CHECK(ErrorCode(Out(link)) == 4);
  }

  {  // Commands are idempotent per session and identity.
    Clock clock;
    Store store;
    store.host = H;
    HostLink link(clock, store, 1);
    int resets = 0;
    link.SetCalibrationResetHandler([&] {
      ++resets;
      return true;
    });
    Pairing(link, S1, H);
    link.TakeOutgoing();
    link.Push(Command(S1, 2, 77, 2));
    CHECK(Out(link).envelope.command->result == 0);
    link.Push(Command(S1, 3, 77, 2));  // retry
    CHECK(Out(link).envelope.command->result == 1);
    CHECK(resets == 1);
    link.Push(Command(S2, 4, 78, 2));  // wrong session
    CHECK(link.TakeOutgoing().empty() && resets == 1);
    link.Push(Command(S1, 6, 80, 1));  // unpair
    CHECK(Out(link).envelope.command->result == 0);
    CHECK(!store.host && link.phase() == LinkPhase::kNegotiated);
    link.Push(Command(S1, 7, 80, 1));  // retry after the session closed
    CHECK(Out(link).envelope.command->result == 1);
    link.Push(Snapshot(S1, 8, 100));
    CHECK(!link.state().has_seen_host);
  }

  {  // Retained state survives unpair; a stale display stays truthful.
    Clock clock;
    Store store;
    store.host = H;
    HostLink link(clock, store, 1);
    Pairing(link, S1, H);
    link.Push(Snapshot(S1, 1, 420));
    link.Unpair();
    CHECK(!store.host && link.state().cpu.value_per_mille == 420);
    clock.now += 4000;
    link.Tick();
    CHECK(link.state().connection == ConnectionState::kStale);
  }

  if (g_failures != 0) {
    std::printf("firmware_domain_host_link_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("firmware_domain_host_link_tests: ok");
  return 0;
}

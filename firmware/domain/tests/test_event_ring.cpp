// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <type_traits>

#include "firmware/domain/event_ring.hpp"
#include "firmware/domain/observation_event.hpp"

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

struct Ev {
  uint32_t window_id = 0;
  uint64_t seq = 0;
};

}  // namespace

int main() {
  using namespace firmware::domain;

  // Events come out oldest first, and a batch drain is bounded.
  {
    EventRing<Ev, 8> ring;
    for (uint64_t i = 1; i <= 5; ++i) ring.Push({1, i});
    Ev out[8];
    Expect(ring.PopBatch(out, 3) == 3 && out[0].seq == 1 && out[2].seq == 3,
           "a batch holds at most `max` events, oldest first");
    Expect(ring.size() == 2 && ring.PopBatch(out, 8) == 2 && out[0].seq == 4 && out[1].seq == 5,
           "the rest drain next");
    Expect(ring.PopBatch(out, 8) == 0 && ring.dropped() == 0, "an empty ring pops nothing");
  }

  // Overflow drops the oldest event and charges the drop to the window that
  // pushed, the one experiencing the overload.
  {
    EventRing<Ev, 4> ring;
    for (uint64_t i = 0; i < 4; ++i) ring.Push({1, i});
    for (uint64_t i = 4; i < 7; ++i) ring.Push({2, i});  // evicts three window-1 events
    Expect(ring.size() == 4 && ring.dropped() == 3, "a full ring drops and counts");
    Ev out[4];
    Expect(ring.PopBatch(out, 4) == 4 && out[0].seq == 3 && out[3].seq == 6,
           "the oldest events are the ones dropped");
    Expect(ring.TakeDrops(1) == 0, "the window whose events were evicted is not charged");
    Expect(ring.TakeDrops(2) == 3, "drops are charged to the pushing window");
    Expect(ring.TakeDrops(2) == 0, "taking drops clears them");
  }

  // The tally never wedges: when it is full the oldest slot folds into the
  // unattributed count and the new window takes its place.
  {
    EventRing<Ev, 1> ring;
    ring.Push({1, 1});
    for (uint32_t w = 2; w <= 7; ++w) ring.Push({w, w});  // six evictions, charged to 2..7
    Expect(ring.dropped() == 6, "six evictions");
    Expect(ring.TakeDrops(2) == 0 && ring.TakeDrops(3) == 0,
           "the oldest windows were folded out of the tally");
    Expect(ring.TakeDrops(4) == 1 && ring.TakeDrops(5) == 1 && ring.TakeDrops(6) == 1 &&
               ring.TakeDrops(7) == 1,
           "the newest four windows keep their own count");
    Expect(ring.TakeUnattributed() == 2 && ring.TakeUnattributed() == 0,
           "the folded drops land in the unattributed count, once");
    for (uint32_t w = 8; w <= 20; ++w) ring.Push({w, w});
    Expect(ring.TakeDrops(20) == 1 && ring.TakeDrops(17) == 1,
           "windows that never take their drops cannot wedge later ones");
  }

  // A key functor stands in for a `window_id` member.
  {
    struct Tagged {
      uint32_t tag;
    };
    struct TagKey {
      uint32_t operator()(const Tagged& t) const { return t.tag; }
    };
    EventRing<Tagged, 1, NoLock, TagKey> ring;
    ring.Push({9});
    ring.Push({10});
    Expect(ring.TakeDrops(10) == 1, "the key functor names the window");
  }

  // The observation envelope is plain data the ring can carry.
  {
    EventRing<ObservationEvent, 2> ring;
    ObservationEvent event;
    event.protocol = EventProtocol::kBle;
    event.window_id = 4;
    event.config_generation = 2;
    event.rssi = -71;
    event.metadata_kind = EventMetadataKind::kBle;
    event.ble.adv_type = 3;
    event.ble.addr = {1, 2, 3, 4, 5, 6};
    ring.Push(event);
    ObservationEvent out;
    Expect(ring.PopBatch(&out, 1) == 1 && out.window_id == 4 && out.rssi == -71 &&
               out.ble.addr[5] == 6 && out.metadata_kind == EventMetadataKind::kBle,
           "an observation event round-trips through the ring");
  }

  // Concurrent producer and consumer under a std::mutex lock: nothing is lost
  // or duplicated, and every drop is accounted for. This exercises the ring's
  // logic under real thread interleavings on the host; it does not prove the
  // ESP32 portMUX critical-section lock or ISR-context behaviour.
  {
    constexpr uint64_t kEvents = 300'000;
    constexpr uint32_t kPerWindow = 1000;
    EventRing<Ev, 64, MutexLock> ring;
    std::atomic<bool> done{false};
    uint64_t popped = 0;
    uint64_t taken = 0;
    bool ordered = true;
    uint64_t last_seq = 0;

    std::thread consumer([&] {
      Ev batch[16];
      for (;;) {
        const bool finished = done.load();
        const size_t n = ring.PopBatch(batch, 16);
        for (size_t i = 0; i < n; ++i) {
          if (batch[i].seq <= last_seq) ordered = false;  // seq starts at 1
          last_seq = batch[i].seq;
        }
        popped += n;
        if (n > 0) taken += ring.TakeDrops(batch[n - 1].window_id);
        if (finished && n == 0) break;
      }
    });
    std::thread producer([&] {
      for (uint64_t seq = 1; seq <= kEvents; ++seq) {
        ring.Push({static_cast<uint32_t>(seq / kPerWindow + 1), seq});
      }
      done.store(true);
    });
    producer.join();
    consumer.join();

    for (uint32_t w = 1; w <= kEvents / kPerWindow + 1; ++w) taken += ring.TakeDrops(w);
    taken += ring.TakeUnattributed();
    Expect(ordered, "events pop in push order with no duplicates");
    Expect(popped + ring.dropped() == kEvents, "every event is popped or counted as dropped");
    Expect(taken == ring.dropped(), "every drop is attributed or counted as unattributed");
  }

  if (failures == 0) {
    std::printf("firmware_domain_event_ring_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_event_ring_tests: %d failure(s)\n", failures);
  return 1;
}

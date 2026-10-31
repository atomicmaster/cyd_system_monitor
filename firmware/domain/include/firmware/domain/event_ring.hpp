// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace firmware::domain {

// Lock policies for EventRing: `lock()` and `unlock()`. The ESP32 side supplies
// a critical-section lock (a portMUX spinlock); this header stays free of
// ESP-IDF.
struct NoLock {
  void lock() {}
  void unlock() {}
};

// Host tests and tools. It proves nothing about ESP32 spinlock or ISR-safety
// behaviour.
class MutexLock {
 public:
  void lock() { mutex_.lock(); }
  void unlock() { mutex_.unlock(); }

 private:
  std::mutex mutex_;
};

// Default key: the event's own `window_id`.
template <typename T>
struct WindowIdOf {
  uint32_t operator()(const T& event) const { return event.window_id; }
};

// Fixed-storage, drop-oldest event ring (docs/extension-interfaces.md, Radio
// observation). Push never blocks, allocates or logs: when the ring is full it
// evicts the oldest event and tallies the drop against the window of the event
// being PUSHED, because that window is the one experiencing the overload (the
// evicted event may belong to an earlier window that is already reported). The
// tally is a small fixed set of recent windows; when it is full the oldest
// slot is folded into the unattributed count, so it can never wedge. A window
// takes its drops with TakeDrops when it closes, and the runner adds
// TakeUnattributed to that report, so every eviction reaches some report.
template <typename T, size_t N, typename Lock = NoLock, typename Key = WindowIdOf<T>>
class EventRing {
  static_assert(N > 0, "a zero-capacity ring cannot hold an event");
  static_assert(std::is_trivially_copyable_v<T>, "events are copied under the lock");

 public:
  static constexpr size_t kTallySlots = 4;

  // Producer side (radio task or ISR, as the Lock allows).
  void Push(const T& event) {
    Guard guard(lock_);
    if (size_ == N) {
      Tally(Key{}(event));
      head_ = (head_ + 1) % N;
      --size_;
      ++dropped_;
    }
    items_[(head_ + size_) % N] = event;
    ++size_;
  }

  // Consumer side: moves up to `max` events, oldest first, into `out`.
  size_t PopBatch(T* out, size_t max) {
    Guard guard(lock_);
    size_t count = 0;
    while (count < max && size_ > 0) {
      out[count++] = items_[head_];
      head_ = (head_ + 1) % N;
      --size_;
    }
    return count;
  }

  // The drops attributed to `window_id` since the last call; clears them.
  uint32_t TakeDrops(uint32_t window_id) {
    Guard guard(lock_);
    for (Slot& slot : tally_) {
      if (slot.count > 0 && slot.window_id == window_id) {
        const uint32_t count = slot.count;
        slot = {};
        return count;
      }
    }
    return 0;
  }

  // Drops folded out of the tally before their window took them; cleared by the call.
  uint32_t TakeUnattributed() {
    Guard guard(lock_);
    const uint32_t count = unattributed_;
    unattributed_ = 0;
    return count;
  }

  size_t size() const {
    Guard guard(lock_);
    return size_;
  }
  // Every eviction since construction, attributed or not.
  uint64_t dropped() const {
    Guard guard(lock_);
    return dropped_;
  }

 private:
  struct Slot {
    uint32_t window_id = 0;
    uint32_t count = 0;  // 0 marks a free slot
    uint64_t age = 0;    // creation order; the smallest is the oldest
  };

  class Guard {
   public:
    explicit Guard(Lock& lock) : lock_(lock) { lock_.lock(); }
    ~Guard() { lock_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    Lock& lock_;
  };

  static uint32_t SaturatingAdd(uint32_t a, uint32_t b) {
    return b > UINT32_MAX - a ? UINT32_MAX : a + b;
  }
  static uint32_t Saturating(uint32_t count) { return SaturatingAdd(count, 1); }

  void Tally(uint32_t window_id) {
    Slot* free_slot = nullptr;
    Slot* oldest = &tally_[0];
    for (Slot& slot : tally_) {
      if (slot.count > 0 && slot.window_id == window_id) {
        slot.count = Saturating(slot.count);
        return;
      }
      if (slot.count == 0) {
        if (free_slot == nullptr) free_slot = &slot;
      } else if (slot.age < oldest->age || oldest->count == 0) {
        oldest = &slot;
      }
    }
    if (free_slot == nullptr) {
      unattributed_ = SaturatingAdd(unattributed_, oldest->count);
      free_slot = oldest;
    }
    *free_slot = {window_id, 1, ++next_age_};
  }

  mutable Lock lock_;
  std::array<T, N> items_{};
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t dropped_ = 0;
  std::array<Slot, kTallySlots> tally_{};
  uint32_t unattributed_ = 0;
  uint64_t next_age_ = 0;
};

}  // namespace firmware::domain

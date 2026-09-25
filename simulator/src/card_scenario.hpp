// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared harness for the per-card scenarios (storage-cards, battery-cards,
// agents-cards, network-cards): a scripted clock, a HostFeed with an open
// session, snapshot frames carrying any metric groups, the registered card
// under test, and a transcript compared with simulator/scenarios/<name>.expected.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "expect.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/host_feed.hpp"
#include "protocol.hpp"

namespace simulator {

class ScriptedClock : public firmware::domain::MonotonicClock {
 public:
  uint64_t NowMs() const override { return now_ms_; }
  void Set(uint64_t now_ms) { now_ms_ = now_ms; }

 private:
  uint64_t now_ms_ = 1000;
};

// A complete snapshot frame of the scenario session (session bytes 0..15):
// a live CPU at 10% plus `groups`.
inline std::vector<uint8_t> SnapshotFrame(uint32_t sequence,
                                          const cyd::protocol::SnapshotGroups& groups) {
  namespace proto = cyd::protocol;
  proto::Envelope envelope;
  envelope.type = proto::MessageType::kSnapshot;
  envelope.sequence = sequence;
  for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
  const proto::CpuSnapshot cpu{proto::Availability::kAvailable, 100, 0, proto::Freshness::kFresh,
                               0};
  std::vector<uint8_t> frame;
  proto::EncodeSnapshot(envelope, cpu, groups, &frame);
  return frame;
}

inline const firmware::domain::CardDescriptor* FindCard(const std::string& id) {
  for (const auto& card : firmware::domain::Cards()) {
    if (id == card.id) return &card;
  }
  return nullptr;
}

class CardScenario {
 public:
  // What each transcript line records after the title, value and note.
  enum class Columns {
    kBar,     // " | bar=<per mille>"
    kShowBar  // " | show_bar=<0|1>" (cards without a proportion)
  };

  // `name` is the scenario name and its .expected file's stem.
  CardScenario(const char* card_id, const char* name, Columns columns)
      : name_(name), columns_(columns), feed_(clock_, 1), card_(FindCard(card_id)) {
    std::array<uint8_t, cyd::protocol::kSessionIdBytes> session{};
    for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
    feed_.StartSession(session);
    const std::string id = card_id;
    const char* article = std::string("aeiou").find(id[0]) != std::string::npos ? "an" : "a";
    listed_ = "the registry lists " + std::string(article) + " " + id + " card";
    result_.Expect(card_ != nullptr, listed_.c_str());
  }

  // False when the card is not registered; the scenario then just finishes.
  bool ready() const { return card_ != nullptr; }
  ScenarioResult& result() { return result_; }

  // Advances the clock to `ms`, renders the card and records it under `label`.
  firmware::domain::MetricCardView At(uint64_t ms, const char* label) {
    clock_.Set(ms);
    feed_.Tick();
    const auto view = card_->make(feed_.state(), ms);
    transcript_ << label << "\n  " << view.title << " | " << view.value << " | " << view.note;
    if (columns_ == Columns::kBar) {
      transcript_ << " | bar=" << view.bar_per_mille;
    } else {
      transcript_ << " | show_bar=" << (view.show_bar ? 1 : 0);
    }
    transcript_ << " | has_value=" << (view.has_value ? 1 : 0)
                << " | dimmed=" << (view.dimmed ? 1 : 0) << "\n";
    return view;
  }

  // Delivers a snapshot frame carrying `groups` at `ms`.
  void Push(uint64_t ms, uint32_t sequence, const cyd::protocol::SnapshotGroups& groups) {
    clock_.Set(ms);
    feed_.Push(SnapshotFrame(sequence, groups));
  }

  // Compares the transcript with the .expected file (or rewrites it when
  // CYD_UPDATE_EXPECTED is set) and reports the scenario result.
  int Finish() {
    const std::string scenario_title = "simulator " + name_;
    if (card_ != nullptr) {
      const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/" + name_ + ".expected";
      if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript_.str();
      std::ifstream expected_file(path);
      std::stringstream expected;
      expected << expected_file.rdbuf();
      const std::string what = "the transcript matches simulator/scenarios/" + name_ +
                               ".expected (rerun with CYD_UPDATE_EXPECTED=1 to review an "
                               "intended change)";
      result_.Expect(expected.str() == transcript_.str(), what.c_str());
    }
    return result_.Finish(scenario_title.c_str());
  }

 private:
  std::string name_;
  Columns columns_;
  ScriptedClock clock_;
  firmware::domain::HostFeed feed_;
  const firmware::domain::CardDescriptor* card_;
  std::string listed_;
  ScenarioResult result_;
  std::ostringstream transcript_;
};

}  // namespace simulator

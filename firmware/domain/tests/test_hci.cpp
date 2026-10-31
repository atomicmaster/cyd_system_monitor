// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

#include "firmware/domain/hci/hci.hpp"
using namespace firmware::domain::hci;
static void check(bool value, std::source_location loc = std::source_location::current()) {
  if (!value) {
    std::fprintf(stderr, "HCI test failed at line %u\n", loc.line());
    std::exit(1);
  }
}
struct Captured {
  Report report;
  std::vector<uint8_t> data;
};
static void capture(const Report& r, void* p) {
  Captured c{r, std::vector<uint8_t>(r.data, r.data + r.data_length)};
  c.report.data = nullptr;
  static_cast<std::vector<Captured>*>(p)->push_back(c);
}
static void report(const Report& r, void* p) { static_cast<std::vector<Report>*>(p)->push_back(r); }
static Event parse(const std::vector<uint8_t>& p, std::vector<Report>& r) {
  return Parse(p.data(), p.size(), report, &r);
}
static void bytes(Command c, Options o, std::vector<uint8_t> expected) {
  uint8_t b[20]{};
  auto n = Build(c, o, b, sizeof b);
  check(std::vector<uint8_t>(b, b + n) == expected);
}
int main() {
  Options o;
  bytes(Command::kReset, o, {1, 3, 12, 0});
  o.mask = {1, 2, 3, 4, 5, 6, 7, 8};
  bytes(Command::kSetEventMask, o, {1, 1, 12, 8, 1, 2, 3, 4, 5, 6, 7, 8});
  bytes(Command::kLeSetEventMask, o, {1, 1, 32, 8, 1, 2, 3, 4, 5, 6, 7, 8});
  bytes(Command::kLeSetScanParameters, o, {1, 11, 32, 7, 0, 80, 0, 80, 0, 0, 0});
  bytes(Command::kLeSetScanEnable, o, {1, 12, 32, 2, 0, 0});
  o.enable = true;
  bytes(Command::kLeSetScanEnable, o, {1, 12, 32, 2, 1, 0});
  uint8_t b[20];
  check(!Build(Command::kReset, o, b, 3));
  for (auto pair : {std::pair<int, int>{3, 4}, {0x4001, 4}, {4, 3}, {0x4000, 0x4001}, {4, 5}}) {
    o.interval = pair.first;
    o.window = pair.second;
    check(!Build(Command::kLeSetScanParameters, o, b, sizeof b));
  }
  for (unsigned op = 0; op <= 0xffff; ++op) {
    o.interval = o.window = 0x50;
    auto n = Build(static_cast<Command>(op), o, b, sizeof b);
    if (n) check(op == 0xc03 || op == 0xc01 || op == 0x2001 || op == 0x200b || op == 0x200c);
  }
  std::vector<Report> reports;
  auto e = parse({4, 14, 4, 1, 3, 12, 0}, reports);
  check(e.kind == Kind::kComplete && e.opcode == 0xc03 && e.credit == 1 && !e.status);
  e = parse({4, 15, 4, 7, 2, 11, 32}, reports);
  check(e.kind == Kind::kStatus && e.status == 7 && e.credit == 2 && e.opcode == 0x200b);
  std::vector<uint8_t> packet = {4, 0x3e, 14, 2, 1, 0, 1, 1, 2, 3, 4, 5, 6, 2, 9, 8, 0xd0};
  e = parse(packet, reports);
  check(e.kind == Kind::kAdvertising && reports.size() == 1 && reports[0].rssi == -48 &&
        reports[0].data_length == 2);
  packet[4] = 2;
  packet.insert(packet.end(), {1, 0, 6, 5, 4, 3, 2, 1, 0, 0xc0});
  packet[2] += 10;
  reports.clear();
  e = parse(packet, reports);
  check(e.kind == Kind::kAdvertising && reports.size() == 2);
  auto malformed = [&](std::vector<uint8_t> p) {
    reports.clear();
    const Event event = parse(p, reports);
    check(event.kind == Kind::kMalformed && reports.empty());
  };
  malformed({4, 14});
  malformed({4, 14, 4, 1});
  malformed({4, 0x3e, 2, 2, 0});
  auto bad = packet;
  bad[13] = 32;
  malformed(bad);
  bad = packet;
  bad.pop_back();
  --bad[2];
  malformed(bad);
  bad = packet;
  bad.push_back(0);
  ++bad[2];
  malformed(bad);
  e = parse({4, 1, 0}, reports);
  check(e.kind == Kind::kIgnored);
  Tracker t;
  auto first = t.Issue(Command::kReset, 0, 10);
  check(first == IssueResult::kIssued);
  auto began = t.TryBeginSend(1);
  t.MarkSent(1);
  check(began);
  e = {Kind::kComplete, 14, 4, 1, 0xc03};
  auto step = t.OnEvent(e, 2, 2);
  check(step.outcome == Outcome::kRejected);
  first = t.Issue(Command::kReset, 3, 10);
  check(first == IssueResult::kIssued);
  began = t.TryBeginSend(3);
  t.MarkSent(3);
  check(began);
  step = t.Poll(14);
  check(step.outcome == Outcome::kTimedOut);
  first = t.Issue(Command::kReset, 15, 10);
  check(first == IssueResult::kQuarantined);
  e = {Kind::kComplete, 14, 0, 1, 0};
  t.OnEvent(e, 15, 15);
  check(t.unexpected() == 0);
  first = t.Issue(Command::kLeSetEventMask, 15, 10);
  check(first == IssueResult::kIssued);
  began = t.TryBeginSend(15);
  t.MarkSent(15);
  check(began);
  e.opcode = 0xc03;
  step = t.OnEvent(e, 16, 16);
  check(step.outcome == Outcome::kNone && t.late() == 1);
  e.opcode = 0x2001;
  e.credit = 0;
  step = t.OnEvent(e, 17, 17);
  check(step.outcome == Outcome::kCompleted);
  first = t.Issue(Command::kReset, 18, 10);
  check(first == IssueResult::kNeedsControllerReset);
  e.credit = 1;
  t.OnEvent(e, 18, 18);
  first = t.Issue(Command::kReset, 19, 10);
  check(first == IssueResult::kIssued);
  step = t.Poll(30);
  check(step.outcome == Outcome::kTimedOut);
  first = t.Issue(Command::kReset, 30, 10);
  check(first == IssueResult::kIssued);
  BootSequencer boot;
  auto steps = BootSequence();
  check(steps[1].options.mask ==
            (std::array<uint8_t, 8>{0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0, 0x20}) &&
        steps[2].options.mask[0] == 2);
  for (int i = 0; i < 4; ++i) {
    check(boot.current()->command == steps[i].command);
    boot.OnResult({Outcome::kCompleted, 0});
  }
  check(boot.complete());
  BootSequencer failed;
  failed.OnResult({Outcome::kCompleted, 0});
  failed.OnResult({Outcome::kRejected, 5});
  check(failed.failed() && failed.step_index() == 1 && failed.failure().status == 5);
  BootSequencer timed;
  timed.OnResult({Outcome::kTimedOut, 0});
  check(timed.failed() && timed.step_index() == 0);
  // Interleaved reports and callback-only data views.
  std::vector<Captured> captured;
  auto parsed = Parse(packet.data(), packet.size(), capture, &captured);
  check(parsed.kind == Kind::kAdvertising && captured.size() == 2);
  check(captured[0].report.event_type == 0 && captured[0].report.address_type == 1 &&
        captured[0].report.address == (std::array<uint8_t, 6>{1, 2, 3, 4, 5, 6}) &&
        captured[0].report.data_length == 2 && captured[0].report.rssi == -48 &&
        captured[0].data == (std::vector<uint8_t>{9, 8}));
  check(captured[1].report.event_type == 1 && captured[1].report.address_type == 0 &&
        captured[1].report.address == (std::array<uint8_t, 6>{6, 5, 4, 3, 2, 1}) &&
        captured[1].report.data_length == 0 && captured[1].report.rssi == -64 &&
        captured[1].data.empty());
  bad = packet;
  bad[4] = 26;
  malformed(bad);
  e = parse({4, 14, 3, 1, 0, 0}, reports);
  check(e.kind == Kind::kComplete && e.opcode == 0 && e.credit == 1);
  e = parse({4, 16, 1, 0x42}, reports);
  check(e.kind == Kind::kHardwareError && e.status == 0x42);
  malformed({4, 16, 0});
  Tracker flow;
  auto issue = flow.Issue(Command::kReset, 0, 10);
  check(issue == IssueResult::kIssued);
  auto sent = flow.TryBeginSend(0);
  flow.MarkSent(0);
  check(sent);
  e = {Kind::kStatus, 15, 0, 1, 0xc03};
  auto result = flow.OnEvent(e, 1, 1);
  check(result.outcome == Outcome::kNone);
  e = {Kind::kComplete, 14, 0, 1, 0};
  result = flow.OnEvent(e, 2, 2);
  check(result.outcome == Outcome::kNone && flow.unexpected() == 0 && flow.credit() == 0);
  e = {Kind::kStatus, 15, 0, 1, 0};
  result = flow.OnEvent(e, 3, 3);
  check(result.outcome == Outcome::kNone && flow.unexpected() == 0);
  e = {Kind::kComplete, 14, 0, 1, 0xc03};
  result = flow.OnEvent(e, 4, 4);
  check(result.outcome == Outcome::kCompleted);
  Tracker wedge;
  issue = wedge.Issue(Command::kReset, 0, 10);
  sent = wedge.TryBeginSend(0);
  wedge.MarkSent(0);
  result = wedge.Poll(11);
  issue = wedge.Issue(Command::kSetEventMask, 31, 10);
  check(issue == IssueResult::kNeedsControllerReset);
  wedge.ResetController();
  issue = wedge.Issue(Command::kReset, 32, 10);
  check(issue == IssueResult::kIssued);
  Tracker late;
  issue = late.Issue(Command::kReset, 0, 10);
  sent = late.TryBeginSend(0);
  late.MarkSent(0);
  e = {Kind::kComplete, 14, 0, 1, 0xc03};
  result = late.OnEvent(e, 100, 100);
  check(result.outcome == Outcome::kTimedOut && late.late() == 1);
  Tracker stale;
  issue = stale.Issue(Command::kReset, 0, 10);
  sent = stale.TryBeginSend(0);
  stale.MarkSent(0);
  result = stale.Poll(11);
  e = {Kind::kComplete, 14, 0, 2, 0};
  result = stale.OnEvent(e, 12, 12);
  issue = stale.Issue(Command::kSetEventMask, 12, 10);
  sent = stale.TryBeginSend(12);
  stale.MarkSent(12);
  e = {Kind::kComplete, 14, 0, 1, 0xc03};
  result = stale.OnEvent(e, 13, 13);
  check(stale.late() == 1 && stale.credit() == 0);
  e = {Kind::kComplete, 14, 0, 1, 0xc01};
  result = stale.OnEvent(e, 14, 14);
  check(result.outcome == Outcome::kCompleted);
  Tracker reset;
  issue = reset.Issue(Command::kLeSetEventMask, 0, 10);
  sent = reset.TryBeginSend(0);
  reset.MarkSent(0);
  result = reset.Poll(11);
  e = {Kind::kComplete, 14, 0, 1, 0};
  result = reset.OnEvent(e, 12, 12);
  issue = reset.Issue(Command::kReset, 12, 10);
  sent = reset.TryBeginSend(12);
  reset.MarkSent(12);
  e = {Kind::kComplete, 14, 0, 1, 0xc03};
  result = reset.OnEvent(e, 13, 13);
  issue = reset.Issue(Command::kLeSetEventMask, 14, 10);
  check(issue == IssueResult::kIssued);
  reset.ResetController();
  e = {Kind::kComplete, 14, 0, 1, 0x200b};
  result = reset.OnEvent(e, 15, 15);
  check(reset.unexpected() == 1);
  issue = reset.Issue(Command::kReset, 16, 10);
  sent = reset.TryBeginSend(16);
  reset.MarkSent(16);
  e = {Kind::kStatus, 15, 0x0c, 1, 0xc03};
  result = reset.OnEvent(e, 17, 17);
  check(result.outcome == Outcome::kRejected && result.status == 0x0c);
  // H1: a Status(0) for an opcode that is no longer pending must not clear its
  // quarantine, or the original command's Complete could satisfy the next one.
  Tracker status;
  issue = status.Issue(Command::kLeSetScanEnable, 0, 10);
  check(issue == IssueResult::kIssued);
  sent = status.TryBeginSend(0);
  status.MarkSent(0);
  check(sent);
  result = status.Poll(11);
  check(result.outcome == Outcome::kTimedOut);
  e = {Kind::kStatus, 15, 0, 1, 0x200c};
  result = status.OnEvent(e, 12, 12);
  check(result.outcome == Outcome::kNone && status.late() == 0 && status.unexpected() == 0 &&
        status.credit() == 1);
  issue = status.Issue(Command::kLeSetScanEnable, 13, 10);
  check(issue == IssueResult::kQuarantined);
  e = {Kind::kComplete, 14, 0, 1, 0x200c};
  result = status.OnEvent(e, 14, 14);
  check(result.outcome == Outcome::kNone && status.late() == 1);
  issue = status.Issue(Command::kLeSetScanEnable, 15, 10);
  check(issue == IssueResult::kIssued);
  sent = status.TryBeginSend(15);
  status.MarkSent(15);
  check(sent);
  e = {Kind::kComplete, 14, 0, 1, 0x200c};
  result = status.OnEvent(e, 16, 16);
  check(result.outcome == Outcome::kCompleted);  // only the disable's own Complete does
  // A Status(0) for a different opcode while a command is pending leaves it pending.
  Tracker other;
  issue = other.Issue(Command::kLeSetScanEnable, 0, 10);
  sent = other.TryBeginSend(0);
  other.MarkSent(0);
  e = {Kind::kStatus, 15, 0, 1, 0xc03};
  result = other.OnEvent(e, 1, 1);
  check(result.outcome == Outcome::kNone && other.unexpected() == 0);
  // A nonzero Status for a pending command still rejects it.
  e = {Kind::kStatus, 15, 0x0c, 1, 0x200c};
  result = other.OnEvent(e, 2, 2);
  check(result.outcome == Outcome::kRejected && result.status == 0x0c);

  // H2: command events queued before a controller reset are dropped, not delivered.
  e = {Kind::kComplete, 14, 0, 1, 0xc03};
  check(IsStaleAfterReset(e, 99, 100));
  e.kind = Kind::kStatus;
  check(IsStaleAfterReset(e, 0, 100));
  check(!IsStaleAfterReset(e, 100, 100));
  check(!IsStaleAfterReset(e, 101, 100));
  e.kind = Kind::kAdvertising;
  check(!IsStaleAfterReset(e, 1, 100));
  e.kind = Kind::kHardwareError;
  check(!IsStaleAfterReset(e, 1, 100));
  e.kind = Kind::kComplete;
  check(!IsStaleAfterReset(e, 1, 0));  // before any reset nothing is stale
  // Ordering: a command event buffered before the command left must not satisfy it.
  Tracker order;
  issue = order.Issue(Command::kLeSetScanEnable, 0, 100);
  check(issue == IssueResult::kIssued);
  sent = order.TryBeginSend(1);
  const bool again = order.TryBeginSend(1);
  check(sent && !again);  // reserved: no second send
  e = {Kind::kComplete, 14, 0, 1, 0x200c};
  result = order.OnEvent(e, 2, 0);  // arrived at 0, before the send was reserved
  check(result.outcome == Outcome::kNone && order.stale() == 1 && order.unexpected() == 0);
  order.MarkSent(3);
  result = order.OnEvent(e, 4, 2);  // arrived after reserve but before the send_us
  check(result.outcome == Outcome::kNone && order.stale() == 2);
  result = order.OnEvent(e, 5, 3);  // arrived at the send instant or later: genuine
  check(result.outcome == Outcome::kCompleted && order.stale() == 2);

  // A fast reply processed before MarkSent is genuine when it arrived after the reservation.
  Tracker fast;
  issue = fast.Issue(Command::kLeSetScanEnable, 0, 100);
  sent = fast.TryBeginSend(10);
  check(sent);
  result = fast.OnEvent(e, 12, 11);
  check(result.outcome == Outcome::kCompleted && fast.stale() == 0);
  fast.MarkSent(11);  // too late to matter: the command is done
  issue = fast.Issue(Command::kLeSetScanEnable, 13, 100);
  check(issue == IssueResult::kIssued);

  // Reserved but not marked: a timeout still quarantines (the send may be in flight).
  Tracker inflight;
  issue = inflight.Issue(Command::kLeSetScanEnable, 0, 10);
  sent = inflight.TryBeginSend(1);
  result = inflight.Poll(12);
  check(result.outcome == Outcome::kTimedOut);
  issue = inflight.Issue(Command::kLeSetScanEnable, 13, 10);
  check(issue == IssueResult::kQuarantined);
  inflight.MarkSent(5);  // no pending command: ignored
  // Reservation is refused once the deadline has passed or without credit.
  Tracker expired;
  issue = expired.Issue(Command::kReset, 0, 10);
  sent = expired.TryBeginSend(11);
  check(!sent);
  // A Complete that predates the send does not complete a different opcode either.
  Tracker unrelated;
  issue = unrelated.Issue(Command::kLeSetScanEnable, 0, 100);
  sent = unrelated.TryBeginSend(1);
  unrelated.MarkSent(2);
  e = {Kind::kComplete, 14, 0, 1, 0xc03};
  result = unrelated.OnEvent(e, 3, 3);
  check(result.outcome == Outcome::kNone && unrelated.unexpected() == 1 && unrelated.stale() == 0);
  BootSequencer invalid(3, 4);
  check(invalid.failed() && invalid.step_index() == 3 &&
        invalid.failure().outcome == Outcome::kInvalid);
  std::puts("firmware_domain_hci_tests: ok");
}

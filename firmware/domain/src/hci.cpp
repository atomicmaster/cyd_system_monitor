// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/hci/hci.hpp"

namespace firmware::domain::hci {
size_t Build(Command command, const Options& o, uint8_t* out, size_t cap) {
  uint8_t p[8]{};
  size_t n = 0;
  switch (command) {
    case Command::kReset:
      break;
    case Command::kSetEventMask:
    case Command::kLeSetEventMask:
      n = 8;
      for (size_t i = 0; i < n; ++i) p[i] = o.mask[i];
      break;
    case Command::kLeSetScanParameters:
      if (o.interval < 4 || o.interval > 0x4000 || o.window < 4 || o.window > 0x4000 ||
          o.window > o.interval)
        return 0;
      n = 7;
      p[1] = o.interval;
      p[2] = o.interval >> 8;
      p[3] = o.window;
      p[4] = o.window >> 8;
      break;
    case Command::kLeSetScanEnable:
      n = 2;
      p[0] = o.enable;
      break;
    default:
      return 0;
  }
  if (!out || cap < n + 4) return 0;
  auto opcode = static_cast<uint16_t>(command);
  out[0] = 1;
  out[1] = opcode;
  out[2] = opcode >> 8;
  out[3] = n;
  for (size_t i = 0; i < n; ++i) out[i + 4] = p[i];
  return n + 4;
}
Event Parse(const uint8_t* p, size_t n, ReportCallback cb, void* ctx) {
  Event e;
  if (!p || n < 3 || p[0] != 4 || n != size_t(p[2]) + 3) return e;
  e.code = p[1];
  if (p[1] == 0x0e) {
    if (n < 6 || (n < 7 && (p[4] != 0 || p[5] != 0))) return e;
    e.kind = Kind::kComplete;
    e.credit = p[3];
    e.opcode = p[4] | (p[5] << 8);
    if (n >= 7) e.status = p[6];
  } else if (p[1] == 0x0f) {
    if (n != 7) return e;
    e.kind = Kind::kStatus;
    e.status = p[3];
    e.credit = p[4];
    e.opcode = p[5] | (p[6] << 8);
  } else if (p[1] == 0x10) {
    if (n != 4) return e;
    e.kind = Kind::kHardwareError;
    e.status = p[3];
  } else if (p[1] == 0x3e && n >= 4 && p[3] == 2) {
    if (n < 5 || !p[4] || p[4] > 0x19) return e;
    // Core v5.x Vol 4 Part E section 5.2: arrayed parameters are interleaved.
    size_t pos = 5;
    for (unsigned i = 0; i < p[4]; ++i) {
      if (n - pos < 10 || p[pos + 8] > 31 || n - pos < size_t(10 + p[pos + 8])) return e;
      pos += 10 + p[pos + 8];
    }
    if (pos != n) return e;
    pos = 5;
    for (unsigned i = 0; i < p[4]; ++i) {
      Report r{};
      r.event_type = p[pos];
      r.address_type = p[pos + 1];
      for (size_t j = 0; j < 6; ++j) r.address[j] = p[pos + 2 + j];
      r.data_length = p[pos + 8];
      r.data = p + pos + 9;
      r.rssi = static_cast<int8_t>(p[pos + 9 + r.data_length]);
      if (cb) cb(r, ctx);
      pos += 10 + r.data_length;
    }
    e.kind = Kind::kAdvertising;
  } else {
    if (p[1] == 0x3e && n < 4) return e;
    e.kind = Kind::kIgnored;
  }
  return e;
}
namespace {
int Index(uint16_t opcode) {
  switch (static_cast<Command>(opcode)) {
    case Command::kReset:
      return 0;
    case Command::kSetEventMask:
      return 1;
    case Command::kLeSetEventMask:
      return 2;
    case Command::kLeSetScanParameters:
      return 3;
    case Command::kLeSetScanEnable:
      return 4;
    default:
      return -1;
  }
}
}  // namespace
IssueResult Tracker::Issue(Command command, uint64_t now, uint64_t timeout) {
  int i = Index(static_cast<uint16_t>(command));
  if (i < 0 || timeout == 0 || UINT64_MAX - now < timeout) return IssueResult::kInvalid;
  if (pending_) return IssueResult::kBusy;
  for (size_t j = 0; j < quarantined_.size(); ++j)
    if (quarantined_[j] && now >= quarantine_until_[j]) quarantined_[j] = false;
  if (quarantined_[i]) return IssueResult::kQuarantined;
  if (!credit_) {
    for (bool active : quarantined_)
      if (active) return IssueResult::kNoCredit;
    return IssueResult::kNeedsControllerReset;
  }
  pending_ = true;
  sent_ = false;
  reserved_ = false;
  opcode_ = static_cast<uint16_t>(command);
  deadline_ = now + timeout;
  timeout_ = timeout;
  return IssueResult::kIssued;
}
bool Tracker::TryBeginSend(uint64_t now) {
  if (!pending_ || sent_ || reserved_ || now > deadline_ || !credit_) return false;
  reserved_ = true;
  floor_us_ = now;
  --credit_;
  return true;
}
void Tracker::MarkSent(uint64_t send_us) {
  if (!pending_ || !reserved_ || sent_) return;
  sent_ = true;
  if (send_us > floor_us_) floor_us_ = send_us;
}
Result Tracker::Poll(uint64_t now) {
  if (!pending_ || now <= deadline_) return {};
  if (sent_ || reserved_) {
    int i = Index(opcode_);
    quarantined_[i] = true;
    uint64_t remaining = UINT64_MAX - now;
    quarantine_until_[i] = timeout_ > remaining / 2 ? UINT64_MAX : now + 2 * timeout_;
  }
  pending_ = false;
  return {Outcome::kTimedOut, 0};
}
void Tracker::ResetController() {
  pending_ = false;
  sent_ = false;
  reserved_ = false;
  credit_ = 1;
  quarantined_.fill(false);
}
Result Tracker::OnEvent(const Event& e, uint64_t now, uint64_t arrival) {
  Result timeout = Poll(now);
  if (e.kind != Kind::kComplete && e.kind != Kind::kStatus) return timeout;
  const bool engaged = pending_ && (sent_ || reserved_);  // the packet is, or is about to be, out
  bool terminates = e.opcode != 0 && engaged && opcode_ == e.opcode &&
                    (e.kind == Kind::kComplete || e.status != 0);
  if (terminates && arrival < floor_us_) {
    ++stale_;  // buffered before this command left: it answers an earlier one
    return timeout;
  }
  credit_ = engaged && !terminates && e.credit ? e.credit - 1 : e.credit;
  // A Status with status 0 only says "accepted": the Complete follows. It never
  // ends a command, and for an opcode that is no longer pending it must not
  // touch the quarantine or the late/unexpected counts.
  if (e.kind == Kind::kStatus && e.status == 0) return timeout;
  if (e.opcode == 0) return timeout;
  int i = Index(e.opcode);
  if (i >= 0 && quarantined_[i]) {
    if (now < quarantine_until_[i]) {
      quarantined_[i] = false;
      ++late_;
      return timeout;
    }
    quarantined_[i] = false;
  }
  if (!engaged || opcode_ != e.opcode) {
    ++unexpected_;
    return timeout;
  }
  if (!terminates) return timeout;
  pending_ = false;
  if (e.kind == Kind::kComplete && e.status == 0 &&
      e.opcode == static_cast<uint16_t>(Command::kReset))
    quarantined_.fill(false);
  return {e.status ? Outcome::kRejected : Outcome::kCompleted, e.status};
}
std::array<BootStep, 4> BootSequence(uint16_t interval, uint16_t window) {
  Options general;
  general.mask = {0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0, 0x20};
  Options le;
  le.mask = {2, 0, 0, 0, 0, 0, 0, 0};
  Options scan;
  scan.interval = interval;
  scan.window = window;
  return {{{Command::kReset, {}},
           {Command::kSetEventMask, general},
           {Command::kLeSetEventMask, le},
           {Command::kLeSetScanParameters, scan}}};
}
BootSequencer::BootSequencer(uint16_t interval, uint16_t window)
    : steps_(BootSequence(interval, window)) {
  if (interval < 4 || interval > 0x4000 || window < 4 || window > 0x4000 || window > interval) {
    index_ = 3;
    failure_ = {Outcome::kInvalid, 0};
  }
}
const BootStep* BootSequencer::current() const {
  return complete() || failed() ? nullptr : &steps_[index_];
}
void BootSequencer::OnResult(Result r) {
  if (!current()) return;
  if (r.outcome == Outcome::kCompleted)
    ++index_;
  else if (r.outcome == Outcome::kRejected || r.outcome == Outcome::kTimedOut)
    failure_ = r;
}
bool BootSequencer::complete() const { return index_ == steps_.size(); }
bool BootSequencer::failed() const { return failure_.outcome != Outcome::kNone; }
}  // namespace firmware::domain::hci

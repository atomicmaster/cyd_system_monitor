// SPDX-License-Identifier: GPL-3.0-only

//! The cached sample of a slow source, with its age.
//!
//! A [`Collector`](crate::Collector) whose source is slow or rate-limited asks
//! it at most once per refresh interval and otherwise reports the cached
//! sample with the age of that sample. The policy every such collector shares
//! is fixed here:
//!
//! - A failed refresh keeps the last good sample with its true, growing age:
//!   it is never re-dated and never replaced by a zero. What an old retained
//!   sample means (still shown and marked stale by the device, or reported
//!   as an error after a retention limit) is the caller's decision, made with
//!   [`SampleCache::failing_since_ms`].
//! - A failed attempt waits a whole refresh interval like a good one, so a
//!   failing source is not asked on every one-second tick; the one exception
//!   is [`RefreshPolicy::retry_until_sampled`].
//! - [`SampleCache::expire`] (a wake) makes the next call refresh; a refresh
//!   that then fails keeps the old sample aged by the whole sleep.

/// When a [`SampleCache`] asks its source again.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RefreshPolicy {
    /// A sample is reused this long before the source is asked again; a
    /// failed attempt also waits this long.
    pub refresh_ms: u64,
    /// While no sample was ever taken, ask on every call instead of at the
    /// refresh cadence (a cheap source whose first read matters).
    pub retry_until_sampled: bool,
}

/// A cached value and how old it is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Cached<'a, T> {
    pub value: &'a T,
    pub age_ms: u64,
}

impl<T> Cached<'_, T> {
    /// The age as the wire's 32-bit field (saturating).
    pub fn wire_age_ms(&self) -> u32 {
        self.age_ms.min(u32::MAX.into()) as u32
    }
}

#[derive(Debug, Clone)]
pub struct SampleCache<T> {
    policy: RefreshPolicy,
    sample: Option<(T, u64)>,
    next_refresh_ms: u64,
    failing_since_ms: Option<u64>,
}

impl<T> SampleCache<T> {
    pub fn new(policy: RefreshPolicy) -> Self {
        Self {
            policy,
            sample: None,
            next_refresh_ms: 0,
            failing_since_ms: None,
        }
    }

    /// Whether the source should be asked now. When it should, the attempt
    /// is recorded: the next one is a refresh interval away. Report the
    /// outcome with [`succeed`](Self::succeed), [`fail`](Self::fail) or
    /// [`clear`](Self::clear).
    pub fn begin_refresh(&mut self, now_ms: u64) -> bool {
        let due = now_ms >= self.next_refresh_ms
            || (self.policy.retry_until_sampled && self.sample.is_none());
        if due {
            self.next_refresh_ms = now_ms.saturating_add(self.policy.refresh_ms);
        }
        due
    }

    /// The source answered: this value, sampled now.
    pub fn succeed(&mut self, value: T, now_ms: u64) {
        self.sample = Some((value, now_ms));
        self.failing_since_ms = None;
    }

    /// The source failed: the last good sample (if any) is kept with its age.
    pub fn fail(&mut self, now_ms: u64) {
        self.failing_since_ms.get_or_insert(now_ms);
    }

    /// The source answered that there is nothing to sample (a batteryless
    /// host): no value is kept, and it is not a failure.
    pub fn clear(&mut self) {
        self.sample = None;
        self.failing_since_ms = None;
    }

    /// [`begin_refresh`](Self::begin_refresh) and asking `source` in one
    /// step: `Some` is a success, `None` a failure.
    pub fn refresh_with(&mut self, now_ms: u64, source: impl FnOnce() -> Option<T>) {
        if self.begin_refresh(now_ms) {
            match source() {
                Some(value) => self.succeed(value, now_ms),
                None => self.fail(now_ms),
            }
        }
    }

    /// Makes the next call refresh (a wake, or a source known to have
    /// changed). The cached sample stays until a refresh replaces it.
    pub fn expire(&mut self) {
        self.next_refresh_ms = 0;
    }

    /// The cached sample and its age at `now_ms`, if any.
    pub fn get(&self, now_ms: u64) -> Option<Cached<'_, T>> {
        self.sample.as_ref().map(|(value, sampled_ms)| Cached {
            value,
            age_ms: now_ms.saturating_sub(*sampled_ms),
        })
    }

    /// When the current run of failed refreshes began; `None` after a
    /// success (or before any failure).
    pub fn failing_since_ms(&self) -> Option<u64> {
        self.failing_since_ms
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cache(retry_until_sampled: bool) -> SampleCache<u32> {
        SampleCache::new(RefreshPolicy {
            refresh_ms: 5_000,
            retry_until_sampled,
        })
    }

    #[test]
    fn a_sample_is_reused_with_its_age_until_the_refresh_is_due() {
        let mut c = cache(false);
        c.refresh_with(1_000, || Some(7));
        c.refresh_with(4_000, || panic!("not due"));
        let cached = c.get(4_000).unwrap();
        assert_eq!((*cached.value, cached.age_ms), (7, 3_000));
        c.refresh_with(6_000, || Some(8));
        assert_eq!(c.get(6_000).map(|c| (*c.value, c.age_ms)), Some((8, 0)));
    }

    #[test]
    fn a_failed_refresh_keeps_the_old_sample_ageing_and_says_since_when() {
        let mut c = cache(false);
        c.refresh_with(0, || Some(1));
        c.refresh_with(5_000, || None);
        c.refresh_with(10_000, || None);
        assert_eq!(
            c.get(12_000).map(|c| (*c.value, c.age_ms)),
            Some((1, 12_000))
        );
        assert_eq!(c.failing_since_ms(), Some(5_000));
        c.refresh_with(15_000, || Some(2));
        assert_eq!(c.failing_since_ms(), None);
    }

    #[test]
    fn an_unsampled_source_is_retried_every_call_only_when_the_policy_says_so() {
        let mut eager = cache(true);
        eager.refresh_with(0, || None);
        let mut asked = false;
        eager.refresh_with(1_000, || {
            asked = true;
            Some(3)
        });
        assert!(asked);
        eager.refresh_with(2_000, || panic!("sampled: back to the cadence"));

        let mut patient = cache(false);
        patient.refresh_with(0, || None);
        patient.refresh_with(1_000, || panic!("a failed attempt waits the cadence"));
        assert!(patient.get(1_000).is_none());
    }

    #[test]
    fn expire_forces_the_next_refresh_and_clear_drops_the_value() {
        let mut c = cache(false);
        c.refresh_with(0, || Some(1));
        c.expire();
        c.refresh_with(7_200_000, || None);
        assert_eq!(c.get(7_200_000).unwrap().wire_age_ms(), 7_200_000);
        c.clear();
        assert!(c.get(7_200_000).is_none() && c.failing_since_ms().is_none());
        assert_eq!(
            Cached {
                value: &0,
                age_ms: u64::MAX
            }
            .wire_age_ms(),
            u32::MAX
        );
    }
}

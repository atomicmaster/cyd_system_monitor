<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Store Host History in SQLite

The per-user Host daemon owns V1 Host History in a local SQLite database using WAL, versioned transactional migrations, and transactional time-bucket compaction. Firmware requests bounded series through the daemon protocol rather than depending on database tables, so storage schema changes do not become protocol compatibility promises.

The database retains one-second samples for 15 minutes, 10-second buckets through four hours, one-minute buckets through seven days, and five-minute buckets through 30 days. Compaction preserves extrema, mean, last value, sample count, and stale or unavailable duration. SQLite provides crash recovery and inspectable local operations without introducing a separate database service.

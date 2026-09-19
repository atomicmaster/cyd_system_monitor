// SPDX-License-Identifier: GPL-3.0-only

//! USB discovery, pairing, and session transport (see ADR 0002 and ADR
//! 0012). Collectors return bounded results to callers here; they do not
//! each open serial ports.

pub mod client;

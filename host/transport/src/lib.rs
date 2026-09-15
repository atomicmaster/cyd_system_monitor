// SPDX-License-Identifier: GPL-3.0-only

//! Home for USB discovery, pairing, and session transport (see ADR 0002
//! and ADR 0012). Empty at M0; TB02 adds the real USB session loop.
//! Collectors return bounded results to callers here; they do not each
//! open serial ports.

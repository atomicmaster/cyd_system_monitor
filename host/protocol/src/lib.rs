// SPDX-License-Identifier: GPL-3.0-only

//! Host-side codec home: framing, version negotiation, and encode/decode
//! against `protocol-generated` types (see `protocol/`). Empty at M0; C01
//! defines the first real USB message and this crate's decoder.

pub use protocol_generated::Smoke;

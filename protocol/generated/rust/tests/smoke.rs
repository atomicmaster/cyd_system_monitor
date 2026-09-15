// SPDX-License-Identifier: GPL-3.0-only
//
// Proves the Rust consumer builds against generated output. Not the
// product protocol's golden-vector suite; that arrives with C01.

#[test]
fn smoke_message_round_trips_field_values() {
    let smoke = protocol_generated::Smoke {
        sequence: 1,
        message: "hello".to_string(),
        ok: true,
    };
    assert_eq!(smoke.sequence, 1);
    assert_eq!(smoke.message, "hello");
    assert!(smoke.ok);
}

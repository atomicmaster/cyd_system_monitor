// SPDX-License-Identifier: GPL-3.0-only

//! The Host's persistent Pairing Identity and the one device it is paired
//! with. Confirmation data, not secrets: a plain file in the user's home.

use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Identity {
    pub host: [u8; 16],
    pub device: Option<[u8; 16]>,
}

pub fn random_bytes<const N: usize>() -> io::Result<[u8; N]> {
    let mut bytes = [0u8; N];
    fs::File::open("/dev/urandom")?.read_exact(&mut bytes)?;
    Ok(bytes)
}

/// The Host's state directory: `$CYD_HOME`, else `~/.cyd-host`.
pub fn state_dir() -> PathBuf {
    std::env::var_os("CYD_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cyd-host")))
        .unwrap_or_else(|| PathBuf::from(".cyd-host"))
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(text: &str) -> Option<[u8; 16]> {
    if text.len() != 32 {
        return None;
    }
    let mut out = [0u8; 16];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(text.get(2 * i..2 * i + 2)?, 16).ok()?;
    }
    Some(out)
}

fn parse(text: &str) -> Option<Identity> {
    let mut host = None;
    let mut device = None;
    for line in text.lines() {
        match line.split_once('=') {
            Some(("host", value)) => host = unhex(value.trim()),
            Some(("device", value)) => device = unhex(value.trim()),
            _ => {}
        }
    }
    Some(Identity {
        host: host?,
        device,
    })
}

/// Loads the identity from the default state directory, creating a fresh
/// Host identity on first use.
pub fn load_or_create() -> io::Result<Identity> {
    load_or_create_in(&state_dir())
}

pub fn save(identity: &Identity) -> io::Result<()> {
    save_in(&state_dir(), identity)
}

pub fn load_or_create_in(dir: &Path) -> io::Result<Identity> {
    if let Some(identity) = fs::read_to_string(dir.join("identity"))
        .ok()
        .and_then(|t| parse(&t))
    {
        return Ok(identity);
    }
    let identity = Identity {
        host: random_bytes()?,
        device: None,
    };
    save_in(dir, &identity)?;
    Ok(identity)
}

pub fn save_in(dir: &Path, identity: &Identity) -> io::Result<()> {
    fs::create_dir_all(dir)?;
    let mut text = format!("host={}\n", hex(&identity.host));
    if let Some(device) = identity.device {
        text.push_str(&format!("device={}\n", hex(&device)));
    }
    fs::write(dir.join("identity"), text)
}

/// The first four bytes of a Pairing Identity as hex: enough for the Operator
/// to tell displays apart, never the whole identity.
pub fn short_form(identity: &[u8; 16]) -> String {
    hex(&identity[..4])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_round_trips_through_text() {
        let original = Identity {
            host: [0xA5; 16],
            device: Some([0x5A; 16]),
        };
        let mut text = format!("host={}\n", hex(&original.host));
        text.push_str(&format!("device={}\n", hex(&original.device.unwrap())));
        assert_eq!(parse(&text), Some(original.clone()));
        assert_eq!(parse("host=zz\n"), None);
        assert_eq!(parse("device=00\n"), None);
        assert_eq!(short_form(&original.host), "a5a5a5a5");
    }
}

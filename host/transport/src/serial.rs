// SPDX-License-Identifier: GPL-3.0-only

//! Minimal raw 115200-8N1 serial port for macOS, so the Host needs no extra
//! dependency. Opening the port toggles DTR/RTS, which resets the board's
//! ESP32; callers wait for it to boot before talking.

use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::raw::{c_int, c_ulong, c_void};
use std::os::unix::fs::OpenOptionsExt;
use std::time::Duration;

const O_NOCTTY: i32 = 0x2_0000;
const TCSANOW: c_int = 0;
const CLOCAL: c_ulong = 0x8000;
const CREAD: c_ulong = 0x800;
const BAUD: c_ulong = 115_200;
const POLLIN: i16 = 0x1;

// macOS `struct termios` is 72 bytes: four tcflag_t (u64), cc_t[20] plus
// padding, then two speed_t (u64). Only c_cflag (index 2) is touched
// directly; the rest goes through libc.
type Termios = [u64; 9];

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

extern "C" {
    fn tcgetattr(fd: c_int, termios: *mut c_void) -> c_int;
    fn tcsetattr(fd: c_int, action: c_int, termios: *const c_void) -> c_int;
    fn cfmakeraw(termios: *mut c_void);
    fn cfsetspeed(termios: *mut c_void, speed: c_ulong) -> c_int;
    fn poll(fds: *mut PollFd, count: c_ulong, timeout_ms: c_int) -> c_int;
}

pub struct SerialPort {
    file: File,
}

impl SerialPort {
    pub fn open(path: &str) -> io::Result<SerialPort> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(O_NOCTTY)
            .open(path)?;
        let fd = file.as_raw_fd();
        let mut termios: Termios = [0; 9];
        // SAFETY: `termios` is a 72-byte buffer matching macOS `struct
        // termios`; `fd` is a valid open descriptor for its whole use.
        unsafe {
            if tcgetattr(fd, termios.as_mut_ptr().cast()) != 0 {
                return Err(io::Error::last_os_error());
            }
            cfmakeraw(termios.as_mut_ptr().cast());
            termios[2] |= CLOCAL | CREAD;
            if cfsetspeed(termios.as_mut_ptr().cast(), BAUD) != 0
                || tcsetattr(fd, TCSANOW, termios.as_ptr().cast()) != 0
            {
                return Err(io::Error::last_os_error());
            }
        }
        Ok(SerialPort { file })
    }

    /// Reads up to `buf.len()` bytes, waiting at most `timeout`. Returns 0 on
    /// timeout; an unplugged device surfaces as an error.
    pub fn read_timeout(&mut self, buf: &mut [u8], timeout: Duration) -> io::Result<usize> {
        let mut fd = PollFd {
            fd: self.file.as_raw_fd(),
            events: POLLIN,
            revents: 0,
        };
        // SAFETY: `fd` is a valid PollFd and the count is one.
        let ready = unsafe { poll(&mut fd, 1, timeout.as_millis() as c_int) };
        if ready < 0 {
            return Err(io::Error::last_os_error());
        }
        if ready == 0 {
            return Ok(0);
        }
        match self.file.read(buf) {
            // POLLHUP with nothing to read: the device went away.
            Ok(0) => Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "serial device closed",
            )),
            other => other,
        }
    }

    pub fn write_all(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.file.write_all(bytes)?;
        self.file.flush()
    }
}

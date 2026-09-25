// SPDX-License-Identifier: GPL-3.0-only

//! The Core Foundation calls the macOS sources share (storage, battery,
//! network): an owning wrapper for +1 references and type-checked readers for
//! borrowed ones. Every reader checks the value's type before using it and
//! returns `None` for null or a value of another type.

use std::ffi::{c_char, c_void, CStr};

/// A Core Foundation reference, owned or borrowed as documented at each use.
pub(crate) type CfRef = *const c_void;

const UTF8: u32 = 0x0800_0100;
const CF_NUMBER_SINT64: isize = 4;

#[link(name = "CoreFoundation", kind = "framework")]
extern "C" {
    fn CFStringCreateWithCString(alloc: CfRef, c_str: *const c_char, encoding: u32) -> CfRef;
    fn CFRelease(object: CfRef);
    fn CFGetTypeID(object: CfRef) -> usize;
    fn CFStringGetTypeID() -> usize;
    fn CFNumberGetTypeID() -> usize;
    fn CFBooleanGetTypeID() -> usize;
    fn CFDictionaryGetTypeID() -> usize;
    fn CFArrayGetCount(array: CfRef) -> isize;
    fn CFArrayGetValueAtIndex(array: CfRef, index: isize) -> CfRef;
    fn CFDictionaryGetValue(dict: CfRef, key: CfRef) -> CfRef;
    fn CFNumberGetValue(number: CfRef, kind: isize, out: *mut c_void) -> u8;
    fn CFBooleanGetValue(boolean: CfRef) -> u8;
    fn CFStringGetCString(s: CfRef, buf: *mut c_char, size: isize, encoding: u32) -> u8;
}

/// An owned Core Foundation reference (+1, from a Create or Copy call),
/// released exactly once on drop.
pub(crate) struct Cf(CfRef);

impl Cf {
    /// Takes ownership of `object`; `None` when it is null.
    ///
    /// # Safety
    /// `object` is null or a +1 reference that nothing else releases.
    pub(crate) unsafe fn owned(object: CfRef) -> Option<Cf> {
        // Lazily: an eagerly built `Cf(null)` would be dropped, releasing null.
        (!object.is_null()).then(|| Cf(object))
    }

    /// The reference, borrowed for as long as `self` lives.
    pub(crate) fn get(&self) -> CfRef {
        self.0
    }
}

impl Drop for Cf {
    fn drop(&mut self) {
        // SAFETY: a non-null owned reference (see `Cf::owned`), released once.
        unsafe { CFRelease(self.0) };
    }
}

/// A new CFString with `text`.
pub(crate) fn string(text: &CStr) -> Option<Cf> {
    // SAFETY: `text` is NUL-terminated; a null allocator is the default; the
    // result is a +1 reference or null.
    unsafe {
        Cf::owned(CFStringCreateWithCString(
            std::ptr::null(),
            text.as_ptr(),
            UTF8,
        ))
    }
}

fn has_type(value: CfRef, type_id: usize) -> bool {
    // SAFETY: callers pass null or a live reference; null is refused first.
    !value.is_null() && unsafe { CFGetTypeID(value) } == type_id
}

/// Whether `value` (null or a live reference) is a CFDictionary.
pub(crate) fn is_dictionary(value: CfRef) -> bool {
    // SAFETY: a type query with no arguments.
    has_type(value, unsafe { CFDictionaryGetTypeID() })
}

/// The text of a CFString that fits `N` bytes with its NUL (longer text is
/// `None`). `value` is null or a live reference.
pub(crate) fn string_value<const N: usize>(value: CfRef) -> Option<String> {
    // SAFETY: a type query with no arguments.
    if !has_type(value, unsafe { CFStringGetTypeID() }) {
        return None;
    }
    let mut buf = [0 as c_char; N];
    // SAFETY: a CFString (checked above); the buffer size is passed.
    if unsafe { CFStringGetCString(value, buf.as_mut_ptr(), N as isize, UTF8) } == 0 {
        return None;
    }
    // SAFETY: NUL-terminated on success.
    Some(
        unsafe { CStr::from_ptr(buf.as_ptr()) }
            .to_string_lossy()
            .into_owned(),
    )
}

/// The value of a CFNumber as a signed 64-bit integer. `value` is null or a
/// live reference.
pub(crate) fn i64_value(value: CfRef) -> Option<i64> {
    // SAFETY: a type query with no arguments.
    if !has_type(value, unsafe { CFNumberGetTypeID() }) {
        return None;
    }
    let mut out: i64 = 0;
    // SAFETY: a CFNumber (checked above); the out pointer is an i64.
    (unsafe { CFNumberGetValue(value, CF_NUMBER_SINT64, (&mut out as *mut i64).cast()) } != 0)
        .then_some(out)
}

/// The value of a CFBoolean. `value` is null or a live reference.
pub(crate) fn bool_value(value: CfRef) -> Option<bool> {
    // SAFETY: a type query with no arguments; then a checked CFBoolean.
    unsafe { has_type(value, CFBooleanGetTypeID()).then(|| CFBooleanGetValue(value) != 0) }
}

/// The value for `key` in a CFDictionary, borrowed from it (Get rule).
/// `dict` is a live CFDictionary.
pub(crate) fn dictionary_value(dict: CfRef, key: &CStr) -> Option<CfRef> {
    let key = string(key)?;
    // SAFETY: `dict` is a CFDictionary per the contract; `key` is live.
    let value = unsafe { CFDictionaryGetValue(dict, key.get()) };
    (!value.is_null()).then_some(value)
}

/// The elements of a CFArray, borrowed from it (Get rule). `array` is a live
/// CFArray.
pub(crate) fn array_values(array: CfRef) -> impl Iterator<Item = CfRef> {
    // SAFETY: `array` is a CFArray per the contract.
    let count = unsafe { CFArrayGetCount(array) };
    // SAFETY: every index is below the count.
    (0..count).map(move |i| unsafe { CFArrayGetValueAtIndex(array, i) })
}

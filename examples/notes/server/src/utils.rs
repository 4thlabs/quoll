use std::fmt::Write;
use std::sync::{Mutex, MutexGuard};

/// 32 random bytes, which no one can guess: a token or a key.
pub(crate) fn random_hex() -> String {
    let mut bytes = [0; 32];

    getrandom::fill(&mut bytes).expect("the system gives random bytes");
    hex(&bytes)
}

pub(crate) fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(2 * bytes.len());

    for byte in bytes {
        write!(text, "{byte:02x}").expect("writing to a `String` does not fail");
    }

    text
}

/// A poisoned lock is a bug: the call stops, rather than go on with what a panic left half written.
pub(crate) fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .expect("no code panics while holding a lock of the server")
}

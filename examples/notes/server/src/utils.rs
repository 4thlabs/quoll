use std::fmt::Write;
use std::num::NonZero;
use std::sync::{Arc, LazyLock, Mutex, MutexGuard};

use argon2::Argon2;
use argon2::password_hash::{self, PasswordHasher, PasswordVerifier};
use tokio::sync::Semaphore;
use tokio::task;

use crate::StoreError;

/// 32 random bytes, which no one can guess: a token or a key.
pub fn random_hex() -> String {
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

/// An Argon2 hash takes 19 MiB and a core: no more run at once than there are cores, so that a flood of logins waits
/// rather than exhaust the memory. A permit is held to the end of the hash's task, even when its request is dropped.
pub(crate) struct Passwords {
    permits: Arc<Semaphore>,
    #[cfg(test)]
    counts: Arc<tests::Counts>,
}

/// Checked for an unknown user, so that refusing one takes as long as a wrong password and tells no names.
static UNKNOWN_USER: LazyLock<String> = LazyLock::new(|| {
    Argon2::default()
        .hash_password_with_salt(b"", &[0; 16])
        .expect("the default parameters hash a password with a salt of 16 bytes")
        .to_string()
});

impl Default for Passwords {
    fn default() -> Self {
        Self::new(std::thread::available_parallelism().map_or(1, NonZero::get))
    }
}

impl Passwords {
    fn new(at_once: usize) -> Self {
        Self {
            permits: Arc::new(Semaphore::new(at_once)),
            #[cfg(test)]
            counts: Arc::default(),
        }
    }

    pub(crate) async fn hash(&self, password: String) -> Result<String, StoreError> {
        self.run(move || {
            Argon2::default()
                .hash_password(password.as_bytes())
                .map(|hash| hash.to_string())
        })
        .await?
        .map_err(StoreError::Hashing)
    }

    /// A user of no hash is unknown, and checked against `UNKNOWN_USER`.
    pub(crate) async fn verify(
        &self,
        password: String,
        hash: Option<String>,
    ) -> Result<bool, StoreError> {
        self.run(move || {
            let hash = hash.as_deref().unwrap_or(UNKNOWN_USER.as_str());

            match Argon2::default().verify_password(password.as_bytes(), hash) {
                Ok(()) => Ok(true),
                Err(password_hash::Error::PasswordInvalid) => Ok(false),
                Err(error) => Err(StoreError::StoredHash(error)),
            }
        })
        .await?
    }

    async fn run<T, F>(&self, hash: F) -> Result<T, StoreError>
    where
        T: Send + 'static,
        F: FnOnce() -> T + Send + 'static,
    {
        let permit = Arc::clone(&self.permits)
            .acquire_owned()
            .await
            .expect("the permits of the hashes are never closed");
        #[cfg(test)]
        let counts = Arc::clone(&self.counts);

        task::spawn_blocking(move || {
            #[cfg(test)]
            let running = tests::Running::start(&counts);

            // Made by the first hash of all, so that the first check of a known user costs as much as an unknown one's.
            LazyLock::force(&UNKNOWN_USER);

            let hashed = hash();

            // Counted out before the permit goes, so that the next hash is never counted beside this one.
            #[cfg(test)]
            drop(running);
            drop(permit);
            hashed
        })
        .await
        .map_err(StoreError::Task)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicUsize, Ordering};

    use super::*;

    #[derive(Debug, Default)]
    pub(super) struct Counts {
        running: AtomicUsize,
        peak: AtomicUsize,
    }

    pub(super) struct Running<'a>(&'a Counts);

    impl<'a> Running<'a> {
        pub(super) fn start(counts: &'a Counts) -> Self {
            let running = counts.running.fetch_add(1, Ordering::SeqCst) + 1;

            counts.peak.fetch_max(running, Ordering::SeqCst);
            Self(counts)
        }
    }

    impl Drop for Running<'_> {
        fn drop(&mut self) {
            self.0.running.fetch_sub(1, Ordering::SeqCst);
        }
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn the_hashes_of_passwords_run_a_few_at_once() {
        let passwords = Arc::new(Passwords::new(2));
        let hash = passwords
            .hash("correct horse".into())
            .await
            .expect("the password is hashed");
        let checks: Vec<_> = (0..8)
            .map(|i| {
                let passwords = Arc::clone(&passwords);
                // Half against the hash, half against the unknown user's: both hash.
                let hash = (i % 2 == 0).then(|| hash.clone());

                tokio::spawn(async move { passwords.verify("correct horse".into(), hash).await })
            })
            .collect();

        for (i, check) in checks.into_iter().enumerate() {
            let verified = check
                .await
                .expect("the check does not panic")
                .expect("the check does not fail");

            assert_eq!(verified, i % 2 == 0);
        }

        let peak = passwords.counts.peak.load(Ordering::SeqCst);

        assert!(
            (1..=2).contains(&peak),
            "at most 2 hashes ran at once, not {peak}"
        );
    }
}

use std::num::NonZero;
#[cfg(test)]
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, LazyLock};

use argon2::Argon2;
use argon2::password_hash::{self, PasswordHasher, PasswordVerifier};
use rusqlite::{OptionalExtension, params};
use tokio::sync::Semaphore;
use tokio::task;

use super::{Store, StoreError};
use crate::User;

/// An Argon2 hash takes 19 MiB and a core: no more run at once than there are cores, so that a flood of logins waits
/// rather than exhaust the memory. A permit is held to the end of the hash's task, even when its request is dropped.
#[derive(Debug)]
pub(super) struct Hashes {
    permits: Arc<Semaphore>,
    #[cfg(test)]
    running: AtomicUsize,
    #[cfg(test)]
    peak: AtomicUsize,
}

impl Hashes {
    pub(super) fn new(at_once: usize) -> Self {
        Self {
            permits: Arc::new(Semaphore::new(at_once)),
            #[cfg(test)]
            running: AtomicUsize::new(0),
            #[cfg(test)]
            peak: AtomicUsize::new(0),
        }
    }
}

#[cfg(test)]
struct Running<'a>(&'a Hashes);

#[cfg(test)]
impl<'a> Running<'a> {
    fn start(hashes: &'a Hashes) -> Self {
        let running = hashes.running.fetch_add(1, Ordering::SeqCst) + 1;

        hashes.peak.fetch_max(running, Ordering::SeqCst);
        Self(hashes)
    }
}

#[cfg(test)]
impl Drop for Running<'_> {
    fn drop(&mut self) {
        self.0.running.fetch_sub(1, Ordering::SeqCst);
    }
}

pub(super) fn hashes_at_once() -> usize {
    std::thread::available_parallelism().map_or(1, NonZero::get)
}

/// Checked for an unknown user, so that refusing one takes as long as a wrong password and tells no names.
pub(super) static UNKNOWN_USER: LazyLock<String> = LazyLock::new(|| {
    Argon2::default()
        .hash_password_with_salt(b"", &[0; 16])
        .expect("the default parameters hash a password with a salt of 16 bytes")
        .to_string()
});

impl Store {
    async fn hash<T, F>(&self, hash: F) -> Result<T, StoreError>
    where
        T: Send + 'static,
        F: FnOnce() -> T + Send + 'static,
    {
        let permit = Arc::clone(&self.hashes.permits)
            .acquire_owned()
            .await
            .expect("the permits of the hashes are never closed");
        #[cfg(test)]
        let hashes = Arc::clone(&self.hashes);

        Ok(task::spawn_blocking(move || {
            #[cfg(test)]
            let _running = Running::start(&hashes);
            let hashed = hash();

            drop(permit);
            hashed
        })
        .await?)
    }

    pub async fn add_user(
        &self,
        name: impl Into<String>,
        password: impl Into<String>,
    ) -> Result<User, StoreError> {
        let password = password.into();
        let hash = self
            .hash(move || Argon2::default().hash_password(password.as_bytes()))
            .await?
            .map_err(StoreError::Hashing)?
            .to_string();
        let name = name.into();

        self.run(move |inner| {
            inner.connection.execute(
                "INSERT INTO users (name, password_hash) VALUES (?1, ?2)",
                params![name, hash],
            )?;

            Ok(User {
                id: inner.connection.last_insert_rowid(),
                name,
            })
        })
        .await
    }

    pub async fn check_password(
        &self,
        name: impl Into<String>,
        password: impl Into<String>,
    ) -> Result<Option<User>, StoreError> {
        let name = name.into();
        let found = self
            .run(move |inner| {
                let found = inner
                    .connection
                    .query_row(
                        "SELECT id, password_hash FROM users WHERE name = ?1",
                        params![name],
                        |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?)),
                    )
                    .optional()?;

                Ok(found.map(|(id, hash)| (User { id, name }, hash)))
            })
            .await?;
        let password = password.into();

        // The hash is checked with the lock released: it takes its time, and the other calls go on meanwhile.
        self.hash(move || {
            let hash = found
                .as_ref()
                .map_or(UNKNOWN_USER.as_str(), |(_, hash)| hash);

            match Argon2::default().verify_password(password.as_bytes(), hash) {
                Ok(()) => Ok(found.map(|(user, _)| user)),
                Err(password_hash::Error::PasswordInvalid) => Ok(None),
                Err(error) => Err(StoreError::StoredHash(error)),
            }
        })
        .await?
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test(flavor = "multi_thread")]
    async fn the_hashes_of_passwords_run_a_few_at_once() {
        let store = Store::in_memory_hashing(2);
        let added = store
            .add_user("ana", "correct horse")
            .await
            .expect("ana is added");
        let checks: Vec<_> = (0..8)
            .map(|i| {
                let store = store.clone();
                // Half for ana, half for unknown users: both hash.
                let name = if i % 2 == 0 {
                    "ana".to_owned()
                } else {
                    format!("user {i}")
                };

                tokio::spawn(async move { store.check_password(name, "correct horse").await })
            })
            .collect();

        for (i, check) in checks.into_iter().enumerate() {
            let user = check
                .await
                .expect("the check does not panic")
                .expect("the check does not fail");

            assert_eq!(user.as_ref(), (i % 2 == 0).then_some(&added));
        }

        assert!(
            (1..=2).contains(&store.hashes.peak.load(Ordering::SeqCst)),
            "at most 2 hashes ran at once"
        );
    }
}

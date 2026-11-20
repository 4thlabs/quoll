use std::fmt;
use std::path::Path;
use std::sync::{Arc, LazyLock, Mutex};

use argon2::password_hash;
use notes_api::Stats;
use rusqlite::Connection;
use tokio::task::{self, JoinError};

use crate::utils::lock;

mod notes;
mod operators;
mod users;
mod watch;

pub use notes::PAGE_SIZE;

/// `AUTOINCREMENT`, so that a removed note's id names no other. No foreign key on `owner`: the tests open sessions for
/// users they never add.
const SCHEMA: &str = "
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE,
        password_hash TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS operators (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE,
        key_hash TEXT NOT NULL UNIQUE
    );
    CREATE TABLE IF NOT EXISTS notes (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        owner INTEGER NOT NULL,
        title TEXT NOT NULL,
        body TEXT NOT NULL
    );
    CREATE INDEX IF NOT EXISTS notes_by_owner ON notes (owner, id);
    CREATE TABLE IF NOT EXISTS tags (
        tag TEXT NOT NULL,
        note INTEGER NOT NULL,
        PRIMARY KEY (tag, note)
    ) WITHOUT ROWID;
    CREATE INDEX IF NOT EXISTS tags_by_note ON tags (note);
";

#[derive(Debug, Clone)]
pub struct Store {
    inner: Arc<Mutex<Inner>>,
    hashes: Arc<users::Hashes>,
}

/// One lock for both, so that a note is checked and watched, or changed and its watchers told, in one step.
#[derive(Debug)]
struct Inner {
    connection: Connection,
    watchers: watch::Watchers,
}

#[derive(Debug)]
pub enum StoreError {
    Database(rusqlite::Error),
    Hashing(password_hash::Error),
    StoredHash(password_hash::Error),
    Task(JoinError),
}

impl fmt::Display for StoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Database(_) => f.write_str("the database failed"),
            Self::Hashing(_) => f.write_str("a password could not be hashed"),
            Self::StoredHash(_) => f.write_str("the stored hash of a password cannot be checked"),
            Self::Task(_) => f.write_str("a call of the store did not end"),
        }
    }
}

impl std::error::Error for StoreError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Database(error) => Some(error),
            Self::Hashing(error) | Self::StoredHash(error) => Some(error),
            Self::Task(error) => Some(error),
        }
    }
}

impl From<rusqlite::Error> for StoreError {
    fn from(error: rusqlite::Error) -> Self {
        Self::Database(error)
    }
}

impl From<JoinError> for StoreError {
    fn from(error: JoinError) -> Self {
        Self::Task(error)
    }
}

impl Store {
    /// Makes the unknown user's hash too, so that the first refusal takes no longer than the next.
    pub async fn open(path: impl AsRef<Path>) -> Result<Self, StoreError> {
        let path = path.as_ref().to_owned();
        let connection = task::spawn_blocking(move || Connection::open(path)).await??;

        Self::with(connection).await
    }

    /// The unknown user's hash is made by the first check of one, as making it here would block.
    pub fn in_memory() -> Self {
        Self::in_memory_hashing(users::hashes_at_once())
    }

    fn in_memory_hashing(at_once: usize) -> Self {
        // A database in memory touches no file: opening it does not block, so that `App::in_memory` needs no `await`.
        let connection = Connection::open_in_memory().expect("SQLite opens a database in memory");

        connection
            .execute_batch(SCHEMA)
            .expect("the tables are created in an empty database");

        Self::from_connection(connection, at_once)
    }

    async fn with(connection: Connection) -> Result<Self, StoreError> {
        let connection = task::spawn_blocking(move || {
            connection.execute_batch(SCHEMA)?;
            LazyLock::force(&users::UNKNOWN_USER);

            Ok::<_, rusqlite::Error>(connection)
        })
        .await??;

        Ok(Self::from_connection(connection, users::hashes_at_once()))
    }

    fn from_connection(connection: Connection, hashes_at_once: usize) -> Self {
        Self {
            inner: Arc::new(Mutex::new(Inner {
                connection,
                watchers: watch::Watchers::default(),
            })),
            hashes: Arc::new(users::Hashes::new(hashes_at_once)),
        }
    }

    /// SQLite's calls block: they run on tokio's blocking threads.
    async fn run<T, F>(&self, call: F) -> Result<T, StoreError>
    where
        T: Send + 'static,
        F: FnOnce(&mut Inner) -> Result<T, StoreError> + Send + 'static,
    {
        let inner = Arc::clone(&self.inner);

        task::spawn_blocking(move || call(&mut lock(&inner))).await?
    }

    pub async fn stats(&self) -> Result<Stats, StoreError> {
        self.run(|inner| {
            let (users, notes) = inner.connection.query_row(
                "SELECT (SELECT count(*) FROM users), (SELECT count(*) FROM notes)",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?;

            Ok(Stats { users, notes })
        })
        .await
    }
}

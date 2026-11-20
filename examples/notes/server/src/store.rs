use std::collections::{BTreeSet, HashMap};
use std::fmt::{self, Write};
use std::num::NonZero;
use std::path::Path;
#[cfg(test)]
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, LazyLock, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use argon2::Argon2;
use argon2::password_hash::{self, PasswordHasher, PasswordVerifier};
use futures_util::{Stream, stream};
use notes_api::{NewNote, Note, NoteEvent, NoteFilter, Stats};
use rusqlite::{Connection, OptionalExtension, params};
use sha2::{Digest, Sha256};
use tokio::sync::{Semaphore, broadcast};
use tokio::task::{self, JoinError};

use crate::{Operator, User};

pub const PAGE_SIZE: u32 = 20;

pub const SESSION_LIFETIME: Duration = Duration::from_secs(24 * 60 * 60);

const CHANGES_KEPT: usize = 16;

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
    hashes: Arc<Hashes>,
}

/// One lock for both, so that a note is checked and watched, or changed and its watchers told, in one step.
#[derive(Debug)]
struct Inner {
    connection: Connection,
    watchers: HashMap<i64, broadcast::Sender<NoteEvent>>,
}

/// An Argon2 hash takes 19 MiB and a core: no more run at once than there are cores, so that a flood of logins waits
/// rather than exhaust the memory. A permit is held to the end of the hash's task, even when its request is dropped.
#[derive(Debug)]
struct Hashes {
    permits: Arc<Semaphore>,
    #[cfg(test)]
    running: AtomicUsize,
    #[cfg(test)]
    peak: AtomicUsize,
}

impl Hashes {
    fn new(at_once: usize) -> Self {
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
        Self::in_memory_hashing(hashes_at_once())
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
            LazyLock::force(&UNKNOWN_USER);

            Ok::<_, rusqlite::Error>(connection)
        })
        .await??;

        Ok(Self::from_connection(connection, hashes_at_once()))
    }

    fn from_connection(connection: Connection, hashes_at_once: usize) -> Self {
        Self {
            inner: Arc::new(Mutex::new(Inner {
                connection,
                watchers: HashMap::new(),
            })),
            hashes: Arc::new(Hashes::new(hashes_at_once)),
        }
    }

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

    /// SQLite's calls block: they run on tokio's blocking threads.
    async fn run<T, F>(&self, call: F) -> Result<T, StoreError>
    where
        T: Send + 'static,
        F: FnOnce(&mut Inner) -> Result<T, StoreError> + Send + 'static,
    {
        let inner = Arc::clone(&self.inner);

        task::spawn_blocking(move || call(&mut lock(&inner))).await?
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

    pub async fn add_operator(&self, name: impl Into<String>) -> Result<String, StoreError> {
        let key = random_hex();
        let hash = key_hash(&key);
        let name = name.into();

        self.run(move |inner| {
            inner.connection.execute(
                "INSERT INTO operators (name, key_hash) VALUES (?1, ?2)",
                params![name, hash],
            )?;

            Ok(())
        })
        .await?;

        Ok(key)
    }

    pub async fn operator(&self, key: &str) -> Result<Option<Operator>, StoreError> {
        let hash = key_hash(key);

        self.run(move |inner| {
            let name = inner
                .connection
                .query_row(
                    "SELECT name FROM operators WHERE key_hash = ?1",
                    params![hash],
                    |row| row.get(0),
                )
                .optional()?;

            Ok(name.map(|name| Operator { name }))
        })
        .await
    }

    pub async fn notes(&self, owner: i64, filter: &NoteFilter) -> Result<Vec<Note>, StoreError> {
        // `LIKE` compares ASCII letters whatever their case, and reads `%` and `_` as wildcards, escaped here.
        let contains = filter
            .contains
            .as_deref()
            .filter(|text| !text.is_empty())
            .map(|text| format!("%{}%", escape_like(text)));
        let tag = filter
            .tag
            .as_deref()
            .and_then(|tag| tag_of(tag.strip_prefix('#').unwrap_or(tag)));
        let offset = i64::from(filter.page.unwrap_or(1).max(1) - 1) * i64::from(PAGE_SIZE);

        self.run(move |inner| {
            let mut statement = inner.connection.prepare_cached(
                "SELECT id, title, body FROM notes
                 WHERE owner = ?1
                   AND (?2 IS NULL OR title LIKE ?2 ESCAPE '\\' OR body LIKE ?2 ESCAPE '\\')
                   AND (?3 IS NULL OR id IN (SELECT note FROM tags WHERE tag = ?3))
                 ORDER BY id
                 LIMIT ?4 OFFSET ?5",
            )?;
            let notes = statement
                .query_map(params![owner, contains, tag, PAGE_SIZE, offset], read_note)?
                .collect::<Result<_, _>>()?;

            Ok(notes)
        })
        .await
    }

    pub async fn note(&self, owner: i64, id: i64) -> Result<Option<Note>, StoreError> {
        self.run(move |inner| Ok(find(&inner.connection, owner, id)?))
            .await
    }

    pub async fn add(&self, owner: i64, note: NewNote) -> Result<Note, StoreError> {
        self.run(move |inner| {
            let transaction = inner.connection.transaction()?;

            transaction.execute(
                "INSERT INTO notes (owner, title, body) VALUES (?1, ?2, ?3)",
                params![owner, note.title, note.body],
            )?;

            let id = transaction.last_insert_rowid();

            tag(&transaction, id, &note)?;
            transaction.commit()?;

            Ok(Note {
                id,
                title: note.title,
                body: note.body,
            })
        })
        .await
    }

    pub async fn replace(
        &self,
        owner: i64,
        id: i64,
        note: NewNote,
    ) -> Result<Option<Note>, StoreError> {
        self.run(move |inner| {
            let transaction = inner.connection.transaction()?;
            let changed = transaction.execute(
                "UPDATE notes SET title = ?3, body = ?4 WHERE owner = ?1 AND id = ?2",
                params![owner, id, note.title, note.body],
            )?;

            if changed == 0 {
                return Ok(None);
            }

            transaction.execute("DELETE FROM tags WHERE note = ?1", params![id])?;
            tag(&transaction, id, &note)?;
            transaction.commit()?;

            let note = Note {
                id,
                title: note.title,
                body: note.body,
            };

            if let Some(watchers) = inner.watchers.get(&id) {
                // No watcher left is no error: the next `watch` drops the sender.
                let _ = watchers.send(NoteEvent::Edited(note.clone()));
            }

            Ok(Some(note))
        })
        .await
    }

    pub async fn remove(&self, owner: i64, id: i64) -> Result<(), StoreError> {
        self.run(move |inner| {
            let transaction = inner.connection.transaction()?;
            let removed = transaction.execute(
                "DELETE FROM notes WHERE owner = ?1 AND id = ?2",
                params![owner, id],
            )?;

            if removed == 0 {
                return Ok(());
            }

            transaction.execute("DELETE FROM tags WHERE note = ?1", params![id])?;
            transaction.commit()?;

            // The sender dropped here ends each watcher's stream, once it has read the removal.
            if let Some(watchers) = inner.watchers.remove(&id) {
                let _ = watchers.send(NoteEvent::Removed { id });
            }

            Ok(())
        })
        .await
    }

    pub async fn watch(
        &self,
        owner: i64,
        id: i64,
    ) -> Result<Option<impl Stream<Item = NoteEvent> + Send + 'static>, StoreError> {
        let Some(changes) = self
            .run(move |inner| {
                if find(&inner.connection, owner, id)?.is_none() {
                    return Ok(None);
                }

                // The senders of notes no one watches any more go, so that they do not pile up.
                inner
                    .watchers
                    .retain(|_, watchers| watchers.receiver_count() > 0);

                let watchers = inner
                    .watchers
                    .entry(id)
                    .or_insert_with(|| broadcast::channel(CHANGES_KEPT).0);

                Ok(Some(watchers.subscribe()))
            })
            .await?
        else {
            return Ok(None);
        };

        Ok(Some(stream::unfold(changes, |mut changes| async move {
            loop {
                match changes.recv().await {
                    Ok(event) => return Some((event, changes)),
                    // The oldest changes were missed: the ones that follow say the note as it is.
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return None,
                }
            }
        })))
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

fn hashes_at_once() -> usize {
    std::thread::available_parallelism().map_or(1, NonZero::get)
}

/// Checked for an unknown user, so that refusing one takes as long as a wrong password and tells no names.
static UNKNOWN_USER: LazyLock<String> = LazyLock::new(|| {
    Argon2::default()
        .hash_password_with_salt(b"", &[0; 16])
        .expect("the default parameters hash a password with a salt of 16 bytes")
        .to_string()
});

fn find(connection: &Connection, owner: i64, id: i64) -> rusqlite::Result<Option<Note>> {
    connection
        .query_row(
            "SELECT id, title, body FROM notes WHERE owner = ?1 AND id = ?2",
            params![owner, id],
            read_note,
        )
        .optional()
}

fn read_note(row: &rusqlite::Row<'_>) -> rusqlite::Result<Note> {
    Ok(Note {
        id: row.get(0)?,
        title: row.get(1)?,
        body: row.get(2)?,
    })
}

fn tag(connection: &Connection, id: i64, note: &NewNote) -> rusqlite::Result<()> {
    let mut statement =
        connection.prepare_cached("INSERT INTO tags (tag, note) VALUES (?1, ?2)")?;

    for tag in tags(note) {
        statement.execute(params![tag, id])?;
    }

    Ok(())
}

fn tags(note: &NewNote) -> BTreeSet<String> {
    [&note.title, &note.body]
        .into_iter()
        .flat_map(|text| text.split_whitespace())
        .filter_map(|word| word.strip_prefix('#').and_then(tag_of))
        .collect()
}

fn tag_of(word: &str) -> Option<String> {
    let end = word
        .find(|c: char| !(c.is_alphanumeric() || c == '-' || c == '_'))
        .unwrap_or(word.len());

    (end > 0).then(|| word[..end].to_lowercase())
}

fn escape_like(text: &str) -> String {
    let mut escaped = String::with_capacity(text.len());

    for c in text.chars() {
        if matches!(c, '\\' | '%' | '_') {
            escaped.push('\\');
        }

        escaped.push(c);
    }

    escaped
}

/// A fast hash keeps 32 random bytes out of reach, and a lookup by hash compares no byte of the key.
fn key_hash(key: &str) -> String {
    hex(&Sha256::digest(key.as_bytes()))
}

/// An expired session goes when its token is met, and all of them when a session opens, so that they do not pile up.
#[derive(Clone)]
pub struct Sessions {
    lifetime: Duration,
    open: Arc<Mutex<HashMap<String, Session>>>,
}

struct Session {
    user: User,
    opened: Instant,
}

impl Session {
    fn is_past(&self, lifetime: Duration) -> bool {
        self.opened.elapsed() >= lifetime
    }
}

impl Default for Sessions {
    fn default() -> Self {
        Self::with_lifetime(SESSION_LIFETIME)
    }
}

impl Sessions {
    pub fn with_lifetime(lifetime: Duration) -> Self {
        Self {
            lifetime,
            open: Arc::default(),
        }
    }

    pub fn open(&self, user: User) -> String {
        let token = random_hex();
        let mut open = lock(&self.open);

        open.retain(|_, session| !session.is_past(self.lifetime));
        open.insert(
            token.clone(),
            Session {
                user,
                opened: Instant::now(),
            },
        );
        token
    }

    pub fn user(&self, token: &str) -> Option<User> {
        let mut open = lock(&self.open);
        let session = open.get(token)?;

        if session.is_past(self.lifetime) {
            open.remove(token);
            return None;
        }

        Some(session.user.clone())
    }
}

/// Never shows the tokens.
impl fmt::Debug for Sessions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Sessions")
            .field("lifetime", &self.lifetime)
            .field("open", &lock(&self.open).len())
            .finish()
    }
}

fn random_hex() -> String {
    let mut bytes = [0; 32];

    getrandom::fill(&mut bytes).expect("the system gives random bytes");
    hex(&bytes)
}

fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(2 * bytes.len());

    for byte in bytes {
        write!(text, "{byte:02x}").expect("writing to a `String` does not fail");
    }

    text
}

/// A poisoned lock is a bug: the call stops, rather than go on with what a panic left half written.
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .expect("no code panics while holding a lock of the store")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn note(title: &str, body: &str) -> NewNote {
        NewNote {
            title: title.into(),
            body: body.into(),
        }
    }

    #[test]
    fn the_tags_of_a_note_are_its_hash_words() {
        let tags = tags(&note(
            "#Shopping list",
            "milk, #dairy, #2026-11 a#b # #fresh!",
        ));

        assert_eq!(
            tags.into_iter().collect::<Vec<_>>(),
            ["2026-11", "dairy", "fresh", "shopping"]
        );
    }

    #[test]
    fn a_tag_is_cut_at_its_first_other_character() {
        assert_eq!(tag_of("Milk,").as_deref(), Some("milk"));
        assert_eq!(tag_of("oat-milk_2!").as_deref(), Some("oat-milk_2"));
        assert_eq!(tag_of(""), None);
        assert_eq!(tag_of(",milk"), None);
    }

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

    #[test]
    fn the_wildcards_of_like_are_escaped() {
        assert_eq!(escape_like(r"50%_off\"), r"50\%\_off\\");
    }

    #[test]
    fn a_key_is_stored_as_its_sha_256() {
        assert_eq!(
            key_hash("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}

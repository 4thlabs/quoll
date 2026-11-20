//! Where the server keeps its users, its operators and their notes, in SQLite, and the sessions it opens, in memory.
//!
//! The store holds one connection, behind a lock. SQLite's calls block: each method runs them on tokio's blocking
//! threads, through `spawn_blocking`, and so does the hashing of a password, which takes its time on purpose. A user's
//! password is stored as an Argon2 hash, an operator's key as its SHA-256: a key is 32 random bytes, which a fast hash
//! keeps out of reach, and is looked up by its hash, so that no comparison of the key itself leaks its bytes.

use std::collections::{BTreeSet, HashMap};
use std::fmt::{self, Write};
use std::path::Path;
use std::sync::{Arc, LazyLock, Mutex, MutexGuard};

use argon2::Argon2;
use argon2::password_hash::{self, PasswordHasher, PasswordVerifier};
use futures_util::{Stream, stream};
use notes_api::{NewNote, Note, NoteEvent, NoteFilter, Stats};
use rusqlite::{Connection, OptionalExtension, params};
use sha2::{Digest, Sha256};
use tokio::sync::broadcast;
use tokio::task::{self, JoinError};

use crate::{Operator, User};

/// How many notes a page of a search holds.
pub const PAGE_SIZE: u32 = 20;

/// How many changes of a note a watcher may fall behind by: one further behind misses the oldest, and reads on from
/// the next, which says the note as it is.
const CHANGES_KEPT: usize = 16;

/// The tables, created when they are missing. A note's id is never given again, `AUTOINCREMENT`, so that a removed
/// note's id names no other. A note's owner is the id of the user a session stands for, with no foreign key to
/// `users`: the tests open sessions for users they never add.
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

/// The users, the operators and the notes, in SQLite, and the watchers of each note.
#[derive(Debug, Clone)]
pub struct Store {
    inner: Arc<Mutex<Inner>>,
}

/// What the lock of the store guards: the connection, and the watchers of the notes, so that a note is checked and
/// watched, or changed and its watchers told, in one step.
#[derive(Debug)]
struct Inner {
    connection: Connection,
    watchers: HashMap<i64, broadcast::Sender<NoteEvent>>,
}

/// Why the store failed: a call of SQLite, the hashing of a password, or a task of the store that did not end.
#[derive(Debug)]
pub enum StoreError {
    /// SQLite failed, or refused a change: a user or an operator whose name is taken.
    Database(rusqlite::Error),
    /// A password could not be hashed, or its stored hash is not one.
    Password(password_hash::Error),
    /// The blocking task of a call panicked, or was cancelled as the runtime stopped.
    Task(JoinError),
}

impl fmt::Display for StoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Database(_) => f.write_str("the database failed"),
            Self::Password(_) => f.write_str("a password could not be hashed"),
            Self::Task(_) => f.write_str("a call of the store did not end"),
        }
    }
}

impl std::error::Error for StoreError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Database(error) => Some(error),
            Self::Password(error) => Some(error),
            Self::Task(error) => Some(error),
        }
    }
}

impl From<rusqlite::Error> for StoreError {
    fn from(error: rusqlite::Error) -> Self {
        Self::Database(error)
    }
}

impl From<password_hash::Error> for StoreError {
    fn from(error: password_hash::Error) -> Self {
        Self::Password(error)
    }
}

impl From<JoinError> for StoreError {
    fn from(error: JoinError) -> Self {
        Self::Task(error)
    }
}

impl Store {
    /// Opens the database at `path`, created with its tables when it is missing.
    ///
    /// # Errors
    ///
    /// When SQLite cannot open the file, or create the tables in it.
    pub async fn open(path: impl AsRef<Path>) -> Result<Self, StoreError> {
        let path = path.as_ref().to_owned();
        let connection = task::spawn_blocking(move || Connection::open(path)).await??;

        Self::with(connection).await
    }

    /// A store with nothing in it, in memory, for the tests.
    ///
    /// # Panics
    ///
    /// When SQLite cannot open a database in memory, which needs no file and only fails with no memory left.
    pub fn in_memory() -> Self {
        // A database in memory touches no file: opening it does not block, so that `App::in_memory` needs no `await`.
        let connection = Connection::open_in_memory().expect("SQLite opens a database in memory");

        connection
            .execute_batch(SCHEMA)
            .expect("the tables are created in an empty database");

        Self::from_connection(connection)
    }

    /// The store of `connection`, once its tables are there.
    async fn with(connection: Connection) -> Result<Self, StoreError> {
        let connection = task::spawn_blocking(move || {
            connection.execute_batch(SCHEMA)?;

            Ok::<_, rusqlite::Error>(connection)
        })
        .await??;

        Ok(Self::from_connection(connection))
    }

    fn from_connection(connection: Connection) -> Self {
        Self {
            inner: Arc::new(Mutex::new(Inner {
                connection,
                watchers: HashMap::new(),
            })),
        }
    }

    /// Runs `call` with the lock of the store held, on a blocking thread.
    async fn run<T, F>(&self, call: F) -> Result<T, StoreError>
    where
        T: Send + 'static,
        F: FnOnce(&mut Inner) -> Result<T, StoreError> + Send + 'static,
    {
        let inner = Arc::clone(&self.inner);

        task::spawn_blocking(move || call(&mut lock(&inner))).await?
    }

    /// Adds a user who signs in with `name` and `password`, of which only an Argon2 hash is kept.
    ///
    /// # Errors
    ///
    /// When the name is another user's already, or the database or the hashing fails.
    pub async fn add_user(&self, name: &str, password: &str) -> Result<User, StoreError> {
        let password = password.to_owned();
        let hash =
            task::spawn_blocking(move || Argon2::default().hash_password(password.as_bytes()))
                .await??
                .to_string();
        let name = name.to_owned();

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

    /// The user `name`, if `password` is theirs. An unknown user takes as long to refuse as a wrong password, so that
    /// the time of the answer does not tell which names are users.
    ///
    /// # Errors
    ///
    /// When the database fails, or the stored hash is not one.
    pub async fn check_password(
        &self,
        name: &str,
        password: &str,
    ) -> Result<Option<User>, StoreError> {
        let name = name.to_owned();
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
        let password = password.to_owned();

        // The hash is checked with the lock released: it takes its time, and the other calls go on meanwhile.
        task::spawn_blocking(move || {
            let hash = found
                .as_ref()
                .map_or(UNKNOWN_USER.as_str(), |(_, hash)| hash);

            match Argon2::default().verify_password(password.as_bytes(), hash) {
                Ok(()) => Ok(found.map(|(user, _)| user)),
                Err(password_hash::Error::PasswordInvalid) => Ok(None),
                Err(error) => Err(error.into()),
            }
        })
        .await?
    }

    /// Adds an operator named `name`, and gives their key, 32 random bytes in hex, of which only a SHA-256 is kept:
    /// the key is shown once, here.
    ///
    /// # Errors
    ///
    /// When the name is another operator's already, or the database fails.
    ///
    /// # Panics
    ///
    /// When the system gives no random bytes.
    pub async fn add_operator(&self, name: &str) -> Result<String, StoreError> {
        let key = random_hex();
        let hash = key_hash(&key);
        let name = name.to_owned();

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

    /// The operator whose key is `key`, found by its hash.
    ///
    /// # Errors
    ///
    /// When the database fails.
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

    /// The notes of the user `owner` that match `filter`, a page of [`PAGE_SIZE`] at a time, in the order they were
    /// written.
    ///
    /// # Errors
    ///
    /// When the database fails.
    pub async fn notes(&self, owner: i64, filter: &NoteFilter) -> Result<Vec<Note>, StoreError> {
        // `LIKE` compares ASCII letters whatever their case, and reads `%` and `_` as wildcards, escaped here.
        let contains = filter
            .contains
            .as_deref()
            .map(|text| format!("%{}%", escape_like(text)));
        let tag = filter.tag.as_deref().map(normalized_tag);
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

    /// The note `id` of the user `owner`. A note of another user is none of theirs: `None`, as one that does not
    /// exist.
    ///
    /// # Errors
    ///
    /// When the database fails.
    pub async fn note(&self, owner: i64, id: i64) -> Result<Option<Note>, StoreError> {
        self.run(move |inner| Ok(find(&inner.connection, owner, id)?))
            .await
    }

    /// Adds a note of the user `owner`, under a new id, tagged by the `#words` of its title and its text.
    ///
    /// # Errors
    ///
    /// When the database fails.
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

    /// Writes the note `id` of the user `owner` again, and tells its watchers. A note that does not exist, or that is
    /// another user's, is `None`, and is left as it is.
    ///
    /// # Errors
    ///
    /// When the database fails.
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

    /// Removes the note `id` of the user `owner`, tells its watchers, and ends their streams. A note that does not
    /// exist, or that is another user's, is left as it is, and is no error: a removal asked twice removes once.
    ///
    /// # Errors
    ///
    /// When the database fails.
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

    /// The changes of the note `id` of the user `owner` from now on, until it is removed: its removal is the last.
    /// A note that does not exist, or that is another user's, has none: its stream ends at once.
    ///
    /// # Errors
    ///
    /// When the database fails.
    pub async fn watch(
        &self,
        owner: i64,
        id: i64,
    ) -> Result<impl Stream<Item = NoteEvent> + Send + 'static, StoreError> {
        let changes = self
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
            .await?;

        Ok(stream::unfold(changes, |changes| async move {
            let mut changes = changes?;

            loop {
                match changes.recv().await {
                    Ok(event) => return Some((event, Some(changes))),
                    // The oldest changes were missed: the ones that follow say the note as it is.
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return None,
                }
            }
        }))
    }

    /// How many users and notes the store holds.
    ///
    /// # Errors
    ///
    /// When the database fails.
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

/// The hash an unknown user's password is checked against, so that refusing them takes as long as a wrong password.
/// It hashes the empty password with a salt of zeros: whether a password matches it or not, an unknown user is refused.
static UNKNOWN_USER: LazyLock<String> = LazyLock::new(|| {
    Argon2::default()
        .hash_password_with_salt(b"", &[0; 16])
        .expect("the default parameters hash a password with a salt of 16 bytes")
        .to_string()
});

/// The note `id` of the user `owner`, if there is one.
fn find(connection: &Connection, owner: i64, id: i64) -> rusqlite::Result<Option<Note>> {
    connection
        .query_row(
            "SELECT id, title, body FROM notes WHERE owner = ?1 AND id = ?2",
            params![owner, id],
            read_note,
        )
        .optional()
}

/// The note of a row of `id`, `title` and `body`.
fn read_note(row: &rusqlite::Row<'_>) -> rusqlite::Result<Note> {
    Ok(Note {
        id: row.get(0)?,
        title: row.get(1)?,
        body: row.get(2)?,
    })
}

/// Tags the note `id` with the `#words` of `note`.
fn tag(connection: &Connection, id: i64, note: &NewNote) -> rusqlite::Result<()> {
    let mut statement =
        connection.prepare_cached("INSERT INTO tags (tag, note) VALUES (?1, ?2)")?;

    for tag in tags(note) {
        statement.execute(params![tag, id])?;
    }

    Ok(())
}

/// The tags of `note`: each word of its title or its text that starts with `#`, up to the first character that is
/// not a letter, a digit, `-` or `_`, in lowercase. `#milk,` is `milk`, and a lone `#` no tag.
fn tags(note: &NewNote) -> BTreeSet<String> {
    [&note.title, &note.body]
        .into_iter()
        .flat_map(|text| text.split_whitespace())
        .filter_map(|word| word.strip_prefix('#'))
        .map(|word| {
            let end = word
                .find(|c: char| !(c.is_alphanumeric() || c == '-' || c == '_'))
                .unwrap_or(word.len());

            normalized_tag(&word[..end])
        })
        .filter(|tag| !tag.is_empty())
        .collect()
}

/// A tag as the store keeps it: in lowercase, without its `#`.
fn normalized_tag(tag: &str) -> String {
    tag.strip_prefix('#').unwrap_or(tag).to_lowercase()
}

/// `text` with the wildcards of `LIKE`, and its escape, escaped.
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

/// The hash an operator's key is stored and looked up by: its SHA-256, in hex.
fn key_hash(key: &str) -> String {
    hex(&Sha256::digest(key.as_bytes()))
}

/// The tokens given out, each standing for its user, in memory: they end with the server.
#[derive(Clone, Default)]
pub struct Sessions {
    users: Arc<Mutex<HashMap<String, User>>>,
}

impl Sessions {
    /// Opens a session for `user`, and gives its token: 32 random bytes, in hex, which no one can guess.
    ///
    /// # Panics
    ///
    /// When the system gives no random bytes.
    pub fn open(&self, user: User) -> String {
        let token = random_hex();

        lock(&self.users).insert(token.clone(), user);
        token
    }

    /// The user `token` stands for, if it is the token of an open session.
    pub fn user(&self, token: &str) -> Option<User> {
        lock(&self.users).get(token).cloned()
    }
}

/// Shows how many sessions are open, never their tokens.
impl fmt::Debug for Sessions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Sessions")
            .field("open", &lock(&self.users).len())
            .finish()
    }
}

/// 32 random bytes, in hex: a token or a key no one can guess.
///
/// # Panics
///
/// When the system gives no random bytes.
fn random_hex() -> String {
    let mut bytes = [0; 32];

    getrandom::fill(&mut bytes).expect("the system gives random bytes");
    hex(&bytes)
}

/// `bytes` in lowercase hex.
fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(2 * bytes.len());

    for byte in bytes {
        write!(text, "{byte:02x}").expect("writing to a `String` does not fail");
    }

    text
}

/// Locks `mutex`. No code panics while holding a lock of the store, so a poisoned one is a bug: it stops the call,
/// rather than go on with what a panic left half written.
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

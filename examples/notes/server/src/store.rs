//! Where the server keeps its notes and its sessions: in memory, for the tests and the example.
//!
//! The methods of [`Store`] return a `Result` and are `async`, as those of a store on a database would be: in memory,
//! they never fail and never wait. A lock is held for a few steps on a map, and released before the method returns.

use std::collections::{BTreeMap, HashMap, btree_map};
use std::convert::Infallible;
use std::fmt::{self, Write};
use std::sync::{Arc, Mutex, MutexGuard};

use notes_api::{NewNote, Note};

use crate::User;

/// The notes of every user.
#[derive(Debug, Clone, Default)]
pub struct Store {
    notes: Arc<Mutex<Notes>>,
}

/// The notes, by id, and the id the next one gets.
#[derive(Debug, Default)]
struct Notes {
    last_id: i64,
    by_id: BTreeMap<i64, Kept>,
}

/// A note as the store keeps it: with the user it belongs to.
#[derive(Debug)]
struct Kept {
    owner: i64,
    title: String,
    body: String,
}

impl Kept {
    /// The note as it is read back.
    fn to_note(&self, id: i64) -> Note {
        Note {
            id,
            title: self.title.clone(),
            body: self.body.clone(),
        }
    }
}

impl Store {
    /// The note `id` of the user `owner`. A note of another user is none of theirs: `None`, as one that does not
    /// exist.
    ///
    /// # Errors
    ///
    /// None in memory.
    pub async fn note(&self, owner: i64, id: i64) -> Result<Option<Note>, Infallible> {
        let notes = lock(&self.notes);

        Ok(notes
            .by_id
            .get(&id)
            .filter(|kept| kept.owner == owner)
            .map(|kept| kept.to_note(id)))
    }

    /// Adds a note of the user `owner`, under a new id.
    ///
    /// # Errors
    ///
    /// None in memory.
    pub async fn add(&self, owner: i64, note: NewNote) -> Result<Note, Infallible> {
        let mut notes = lock(&self.notes);

        notes.last_id += 1;

        let id = notes.last_id;
        let kept = Kept {
            owner,
            title: note.title,
            body: note.body,
        };
        let note = kept.to_note(id);

        notes.by_id.insert(id, kept);
        Ok(note)
    }

    /// Removes the note `id` of the user `owner`. A note that does not exist, or that is another user's, is left as it
    /// is, and is no error: a removal asked twice removes once.
    ///
    /// # Errors
    ///
    /// None in memory.
    pub async fn remove(&self, owner: i64, id: i64) -> Result<(), Infallible> {
        let mut notes = lock(&self.notes);

        if let btree_map::Entry::Occupied(entry) = notes.by_id.entry(id)
            && entry.get().owner == owner
        {
            entry.remove();
        }

        Ok(())
    }
}

/// The tokens given out, each standing for its user.
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
        let mut bytes = [0; 32];

        getrandom::fill(&mut bytes).expect("the system gives random bytes");

        let mut token = String::with_capacity(2 * bytes.len());

        for byte in bytes {
            write!(token, "{byte:02x}").expect("writing to a `String` does not fail");
        }

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

/// Locks `mutex`. No code panics while holding a lock of the store, so a poisoned one is a bug: it stops the
/// request, rather than serve what a panic left half written.
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .expect("no code panics while holding a lock of the store")
}

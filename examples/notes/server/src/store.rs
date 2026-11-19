//! Where the server keeps its notes and its sessions: in memory, for the tests and the example.
//!
//! A store on a database fails, so the store's methods return a `Result` and are `async`, as its would be: in memory,
//! they never fail and never wait. A lock is held for a few steps on a map, never across an `await`, which the server's
//! `Send` futures would refuse anyway.

use std::collections::{BTreeMap, HashMap};
use std::convert::Infallible;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use notes_api::{NewNote, Note};

use crate::User;

/// The notes of every user.
#[derive(Clone, Default)]
pub struct Store {
    notes: Arc<Mutex<Notes>>,
}

/// The notes, by id, and the id the next one gets.
#[derive(Default)]
struct Notes {
    last_id: i64,
    by_id: BTreeMap<i64, Kept>,
}

/// A note as the store keeps it: with the user it belongs to.
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

        if notes.by_id.get(&id).is_some_and(|kept| kept.owner == owner) {
            notes.by_id.remove(&id);
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

        let token: String = bytes.iter().map(|byte| format!("{byte:02x}")).collect();

        lock(&self.users).insert(token.clone(), user);
        token
    }

    /// The user `token` stands for, if it is the token of an open session.
    pub fn user(&self, token: &str) -> Option<User> {
        lock(&self.users).get(token).cloned()
    }
}

/// Locks `mutex`, even when a thread panicked while holding it: nothing that runs under a lock of the store panics, so
/// what it holds is whole.
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

// The state of the README's server and its authentication, before the block of its handlers.
use std::collections::BTreeMap;
use std::sync::Mutex;

/// Who a request comes from, once its token is checked.
pub struct User {
    pub id: i64,
}

/// The notes of every user, in memory.
#[derive(Default)]
pub struct Store {
    kept: Mutex<Kept>,
}

/// The notes by their user's id and their own, and the id of the last one.
#[derive(Default)]
struct Kept {
    last_id: i64,
    notes: BTreeMap<(i64, i64), Note>,
}

impl Store {
    pub fn note(&self, user: i64, id: i64) -> Option<Note> {
        let kept = self.kept.lock().expect("no handler panics while holding the lock");

        kept.notes.get(&(user, id)).cloned()
    }

    pub fn add(&self, user: i64, note: NewNote) -> Note {
        let mut kept = self.kept.lock().expect("no handler panics while holding the lock");

        kept.last_id += 1;

        let note = Note {
            id: kept.last_id,
            title: note.title,
        };

        kept.notes.insert((user, note.id), note.clone());
        note
    }

    pub fn remove(&self, user: i64, id: i64) {
        let mut kept = self.kept.lock().expect("no handler panics while holding the lock");

        kept.notes.remove(&(user, id));
    }
}

#[derive(Default)]
pub struct App {
    pub store: Store,
}

impl quoll::Authenticate<quoll::Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> quoll::Result<User> {
        match token.as_str() {
            "ana-token" => Ok(User { id: 1 }),
            _ => Err(quoll::Error::unauthorized("unknown or expired token")),
        }
    }
}

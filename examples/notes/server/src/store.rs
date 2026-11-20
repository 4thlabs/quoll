use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fmt;
use std::sync::{Arc, Mutex};

use argon2::password_hash;
use futures_util::{Stream, stream};
use notes_api::{NewNote, Note, NoteEvent, NoteFilter, Stats};
use sha2::{Digest, Sha256};
use tokio::sync::broadcast;
use tokio::task::JoinError;

use crate::utils::{Passwords, hex, lock, random_hex};
use crate::{Operator, User};

pub const PAGE_SIZE: usize = 20;

const CHANGES_KEPT: usize = 16;

#[derive(Clone)]
pub struct Store {
    inner: Arc<Mutex<Inner>>,
    passwords: Arc<Passwords>,
}

/// One lock for all, so that a note is checked and watched, or changed and its watchers told, in one step.
#[derive(Default)]
struct Inner {
    users: HashMap<String, Account>,
    /// By the SHA-256 of their key.
    operators: HashMap<String, Operator>,
    /// By id, the order they were written in.
    notes: BTreeMap<i64, Stored>,
    watchers: HashMap<i64, broadcast::Sender<NoteEvent>>,
    last_user: i64,
    /// Never given again, so that a removed note's id names no other.
    last_note: i64,
}

#[derive(Clone)]
struct Account {
    user: User,
    password_hash: String,
}

struct Stored {
    owner: i64,
    note: Note,
    tags: BTreeSet<String>,
}

impl Stored {
    fn new(owner: i64, note: Note) -> Self {
        Self {
            owner,
            tags: tags(&note),
            note,
        }
    }

    fn contains(&self, lowercase: &str) -> bool {
        [&self.note.title, &self.note.body]
            .into_iter()
            .any(|text| text.to_ascii_lowercase().contains(lowercase))
    }
}

#[derive(Debug)]
pub enum StoreError {
    NameTaken,
    Hashing(password_hash::Error),
    StoredHash(password_hash::Error),
    Task(JoinError),
}

impl fmt::Display for StoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NameTaken => f.write_str("a user of this name exists"),
            Self::Hashing(_) => f.write_str("a password could not be hashed"),
            Self::StoredHash(_) => f.write_str("the stored hash of a password cannot be checked"),
            Self::Task(_) => f.write_str("the hash of a password did not end"),
        }
    }
}

impl std::error::Error for StoreError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::NameTaken => None,
            Self::Hashing(error) | Self::StoredHash(error) => Some(error),
            Self::Task(error) => Some(error),
        }
    }
}

/// Never shows the hashes of passwords and keys.
impl fmt::Debug for Store {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Store").finish_non_exhaustive()
    }
}

impl Store {
    pub fn in_memory() -> Self {
        Self {
            inner: Arc::default(),
            passwords: Arc::default(),
        }
    }

    pub async fn add_user(
        &self,
        name: impl Into<String>,
        password: impl Into<String>,
    ) -> Result<User, StoreError> {
        let password_hash = self.passwords.hash(password.into()).await?;
        let name = name.into();
        let mut inner = lock(&self.inner);

        if inner.users.contains_key(&name) {
            return Err(StoreError::NameTaken);
        }

        inner.last_user += 1;

        let user = User {
            id: inner.last_user,
            name: name.clone(),
        };

        inner.users.insert(
            name,
            Account {
                user: user.clone(),
                password_hash,
            },
        );
        Ok(user)
    }

    /// The hash is checked with the lock released: it takes its time, and the other calls go on meanwhile.
    pub async fn check_password(
        &self,
        name: &str,
        password: impl Into<String>,
    ) -> Result<Option<User>, StoreError> {
        let account = lock(&self.inner).users.get(name).cloned();
        let hash = account
            .as_ref()
            .map(|account| account.password_hash.clone());
        let verified = self.passwords.verify(password.into(), hash).await?;

        Ok(account.filter(|_| verified).map(|account| account.user))
    }

    pub fn add_operator(&self, name: impl Into<String>) -> String {
        let key = random_hex();
        let operator = Operator { name: name.into() };

        lock(&self.inner).operators.insert(key_hash(&key), operator);
        key
    }

    pub fn operator(&self, key: &str) -> Option<Operator> {
        lock(&self.inner).operators.get(&key_hash(key)).cloned()
    }

    pub fn stats(&self) -> Stats {
        let inner = lock(&self.inner);

        Stats {
            users: inner.users.len() as u64,
            notes: inner.notes.len() as u64,
        }
    }

    pub fn notes(&self, owner: i64, filter: &NoteFilter) -> Vec<Note> {
        let contains = filter
            .contains
            .as_deref()
            .filter(|text| !text.is_empty())
            .map(str::to_ascii_lowercase);
        let tag = filter
            .tag
            .as_deref()
            .and_then(|tag| tag_of(tag.strip_prefix('#').unwrap_or(tag)));
        let skip = ((filter.page.unwrap_or(1).max(1) - 1) as usize).saturating_mul(PAGE_SIZE);

        lock(&self.inner)
            .notes
            .values()
            .filter(|stored| stored.owner == owner)
            .filter(|stored| contains.as_ref().is_none_or(|text| stored.contains(text)))
            .filter(|stored| tag.as_ref().is_none_or(|tag| stored.tags.contains(tag)))
            .skip(skip)
            .take(PAGE_SIZE)
            .map(|stored| stored.note.clone())
            .collect()
    }

    pub fn note(&self, owner: i64, id: i64) -> Option<Note> {
        lock(&self.inner)
            .notes
            .get(&id)
            .filter(|stored| stored.owner == owner)
            .map(|stored| stored.note.clone())
    }

    pub fn add(&self, owner: i64, note: NewNote) -> Note {
        let mut inner = lock(&self.inner);

        inner.last_note += 1;

        let note = Note {
            id: inner.last_note,
            title: note.title,
            body: note.body,
        };

        inner
            .notes
            .insert(note.id, Stored::new(owner, note.clone()));
        note
    }

    pub fn replace(&self, owner: i64, id: i64, note: NewNote) -> Option<Note> {
        let mut inner = lock(&self.inner);
        let stored = inner
            .notes
            .get_mut(&id)
            .filter(|stored| stored.owner == owner)?;
        let note = Note {
            id,
            title: note.title,
            body: note.body,
        };

        *stored = Stored::new(owner, note.clone());

        if let Some(watchers) = inner.watchers.get(&id) {
            // No watcher left is no error: the next `watch` drops the sender.
            let _ = watchers.send(NoteEvent::Edited(note.clone()));
        }

        Some(note)
    }

    pub fn remove(&self, owner: i64, id: i64) {
        let mut inner = lock(&self.inner);

        if inner
            .notes
            .get(&id)
            .is_none_or(|stored| stored.owner != owner)
        {
            return;
        }

        inner.notes.remove(&id);

        // The sender dropped here ends each watcher's stream, once it has read the removal.
        if let Some(watchers) = inner.watchers.remove(&id) {
            let _ = watchers.send(NoteEvent::Removed { id });
        }
    }

    pub fn watch(
        &self,
        owner: i64,
        id: i64,
    ) -> Option<impl Stream<Item = NoteEvent> + Send + 'static> {
        let mut inner = lock(&self.inner);

        inner
            .notes
            .get(&id)
            .filter(|stored| stored.owner == owner)?;

        // The senders of notes no one watches any more go, so that they do not pile up.
        inner
            .watchers
            .retain(|_, watchers| watchers.receiver_count() > 0);

        let changes = inner
            .watchers
            .entry(id)
            .or_insert_with(|| broadcast::channel(CHANGES_KEPT).0)
            .subscribe();

        Some(stream::unfold(changes, |mut changes| async move {
            loop {
                match changes.recv().await {
                    Ok(event) => return Some((event, changes)),
                    // The oldest changes were missed: the ones that follow say the note as it is.
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return None,
                }
            }
        }))
    }
}

/// A fast hash keeps 32 random bytes out of reach, and a lookup by hash compares no byte of the key.
fn key_hash(key: &str) -> String {
    hex(&Sha256::digest(key.as_bytes()))
}

fn tags(note: &Note) -> BTreeSet<String> {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_tags_of_a_note_are_its_hash_words() {
        let tags = tags(&Note {
            id: 1,
            title: "#Shopping list".into(),
            body: "milk, #dairy, #2026-11 a#b # #fresh!".into(),
        });

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

    #[test]
    fn a_key_is_stored_as_its_sha_256() {
        assert_eq!(
            key_hash("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}

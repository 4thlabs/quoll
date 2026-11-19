//! The server of the notes example: the contract's routes served from memory, each user's notes behind their token.

use notes_api::*;
use quoll::rest::{Authenticate, Bearer, Created, Error, Result};

mod store;

pub use store::{Sessions, Store};

/// Who a request comes from, once its token is checked. Never sent: the server alone builds it.
#[derive(Clone)]
pub struct User {
    /// The user's id, which their notes are kept under.
    pub id: i64,
    /// Their name.
    pub name: String,
}

/// The service of every group: the notes and the open sessions.
#[derive(Clone)]
pub struct App {
    /// The notes of every user.
    pub store: Store,
    /// The tokens given out, each standing for its user.
    pub sessions: Sessions,
}

impl App {
    /// An app with no note and no session, kept in memory.
    pub fn in_memory() -> Self {
        Self {
            store: Store::default(),
            sessions: Sessions::default(),
        }
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Note> {
        self.store
            .note(user.id, id)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, user: &User, note: NewNote) -> Result<Created<Note>> {
        let note = self
            .store
            .add(user.id, note)
            .await
            .map_err(Error::internal)?;

        Ok(Created::at(format!("/api/notes/{}", note.id), note))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<()> {
        self.store
            .remove(user.id, id)
            .await
            .map_err(Error::internal)
    }
}

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions
            .user(&token)
            .ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

/// The router of the api, served by `app`.
pub fn router(app: App) -> axum::Router {
    Api::router(ApiServices { notes: app })
}

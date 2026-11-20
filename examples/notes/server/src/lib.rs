//! The server of the notes example: the contract's routes served from SQLite, each user's notes behind their token,
//! and the statistics behind an operator's key.

use std::time::Duration;

use notes_api::*;
use quoll::rest::{Authenticate, Bearer, Created, Error, Events, Form, Result};
use quoll::server;

mod store;

pub use store::{PAGE_SIZE, Sessions, Store, StoreError};

/// Who a request comes from, once its token is checked. Never sent: the server alone builds it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct User {
    /// The user's id, which their notes are kept under.
    pub id: i64,
    /// Their name.
    pub name: String,
}

/// An operator, known by its key.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Operator {
    /// The operator's name.
    pub name: String,
}

/// The service of every group: the store and the open sessions.
#[derive(Debug, Clone)]
pub struct App {
    /// The users, the operators and their notes.
    pub store: Store,
    /// The tokens given out, each standing for its user.
    pub sessions: Sessions,
}

impl App {
    /// An app with nothing in its store, kept in memory, and no session.
    ///
    /// # Panics
    ///
    /// When SQLite cannot open a database in memory.
    pub fn in_memory() -> Self {
        Self {
            store: Store::in_memory(),
            sessions: Sessions::default(),
        }
    }
}

impl AuthServer for App {
    async fn token(&self, login: Form<Login>) -> Result<TokenResponse> {
        let user = self
            .store
            .check_password(&login.user, &login.password)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::unauthorized("wrong user or password"))?;

        Ok(TokenResponse {
            access_token: self.sessions.open(user),
        })
    }
}

impl NotesServer for App {
    async fn search(&self, user: &User, filter: NoteFilter) -> Result<Vec<Note>> {
        self.store
            .notes(user.id, &filter)
            .await
            .map_err(Error::internal)
    }

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

    async fn update(&self, user: &User, id: i64, note: NewNote) -> Result<Note> {
        self.store
            .replace(user.id, id, note)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<()> {
        self.store
            .remove(user.id, id)
            .await
            .map_err(Error::internal)
    }

    async fn events(&self, user: &User, id: i64) -> Result<Events<NoteEvent>> {
        let changes = self
            .store
            .watch(user.id, id)
            .await
            .map_err(Error::internal)?;

        Ok(Events::new(changes))
    }
}

impl AdminServer for App {
    async fn stats(&self, _operator: &Operator) -> Result<Stats> {
        self.store.stats().await.map_err(Error::internal)
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

impl Authenticate<Key> for App {
    type Caller = Operator;

    async fn authenticate(&self, key: String) -> Result<Operator> {
        self.store
            .operator(&key)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::unauthorized("unknown key"))
    }
}

/// The router of the api, served by `app`.
pub fn router(app: App) -> axum::Router {
    Api::router(ApiServices {
        auth: app.clone(),
        notes: app.clone(),
        admin: app,
    })
}

/// The server, with its layers in any order: a request gets its id, then its span, then its deadline and its body's
/// limit.
pub fn server() -> server::Builder {
    server::builder()
        .with(server::request_id())
        .with(server::trace())
        .with(server::timeout(Duration::from_secs(10)))
        .with(server::body_limit(1 << 20))
}

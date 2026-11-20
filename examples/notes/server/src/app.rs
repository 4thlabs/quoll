use std::time::Duration;

use notes_api::{Api, ApiServices};
use quoll::server;

use crate::{Sessions, Store};

#[derive(Debug, Clone)]
pub struct App {
    pub store: Store,
    pub sessions: Sessions,
}

impl App {
    pub fn in_memory() -> Self {
        Self {
            store: Store::in_memory(),
            sessions: Sessions::default(),
        }
    }
}

pub fn router(app: App) -> axum::Router {
    Api::router(ApiServices {
        auth: app.clone(),
        notes: app.clone(),
        admin: app,
    })
}

/// The layers go in any order: a request gets its id, then its span, then its deadline and its body's limit.
pub fn server() -> server::Builder {
    server::builder()
        .with(server::request_id())
        .with(server::trace())
        .with(server::timeout(Duration::from_secs(10)))
        .with(server::body_limit(1 << 20))
}

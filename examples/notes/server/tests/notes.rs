//! The notes server, served on a free port and called by the contract's client.

use notes_api::{Api, ApiClient, NewNote, Notes};
use notes_server::{Sessions, User};
use quoll::rest;
use reqwest_middleware::ClientBuilder;

#[tokio::test]
async fn a_created_note_reads_back() -> anyhow::Result<()> {
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::bearer(token))
        .build();
    let api = Api::client_with(http, &url)?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
            body: String::new(),
        })
        .await?;
    let read = api.notes().get(created.id).await?;

    assert_eq!(read.title, "Milk");
    Ok(())
}

#[tokio::test]
async fn a_call_without_token_is_refused() -> anyhow::Result<()> {
    let url = rest::test::spawn(notes_server::router(notes_server::App::in_memory())).await;
    let api = Api::client(&url)?;

    let error = api.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::UNAUTHORIZED));
    Ok(())
}

/// The server, in memory, on a free port: its URL and its sessions, where a test opens the ones it calls with.
struct Served {
    url: String,
    sessions: Sessions,
}

impl Served {
    /// Serves an app with no note and no session.
    async fn start() -> Self {
        let app = notes_server::App::in_memory();
        let sessions = app.sessions.clone();
        let url = rest::test::spawn(notes_server::router(app)).await;

        Self { url, sessions }
    }

    /// A client signed in as `user`, through a session opened for them.
    fn client_as(&self, user: User) -> rest::Result<ApiClient> {
        self.client_with(self.sessions.open(user))
    }

    /// A client sending `token`.
    fn client_with(&self, token: String) -> rest::Result<ApiClient> {
        let http = ClientBuilder::new(reqwest::Client::new())
            .with(rest::client::bearer(token))
            .build();

        Api::client_with(http, &self.url)
    }
}

/// The user of most tests.
fn ana() -> User {
    User {
        id: 1,
        name: "ana".into(),
    }
}

/// Another user, whose notes are not ana's.
fn bob() -> User {
    User {
        id: 2,
        name: "bob".into(),
    }
}

/// A note with a title and an empty body.
fn titled(title: &str) -> NewNote {
    NewNote {
        title: title.into(),
        body: String::new(),
    }
}

#[tokio::test]
async fn a_created_note_has_its_body_and_a_new_id() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    let first = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
            body: "Two litres".into(),
        })
        .await?;
    let second = api.notes().create(titled("Bread")).await?;
    let read = api.notes().get(first.id).await?;

    assert_ne!(first.id, second.id);
    assert_eq!(
        (read.id, read.title.as_str(), read.body.as_str()),
        (first.id, "Milk", "Two litres")
    );
    Ok(())
}

#[tokio::test]
async fn a_removed_note_is_not_found() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let created = api.notes().create(titled("Milk")).await?;

    // A removal answers `()`, which the pattern checks.
    let () = api.notes().remove(created.id).await?;
    let error = api.notes().get(created.id).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::NOT_FOUND));
    assert_eq!(
        error.problem().and_then(|problem| problem.detail()),
        Some("no such note")
    );
    Ok(())
}

#[tokio::test]
async fn a_removal_asked_twice_succeeds_twice() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let created = api.notes().create(titled("Milk")).await?;

    api.notes().remove(created.id).await?;
    api.notes().remove(created.id).await?;
    Ok(())
}

#[tokio::test]
async fn a_note_of_another_user_is_not_found_and_not_removed() -> anyhow::Result<()> {
    let served = Served::start().await;
    let ana_api = served.client_as(ana())?;
    let bob_api = served.client_as(bob())?;
    let created = ana_api.notes().create(titled("Milk")).await?;

    let error = bob_api.notes().get(created.id).await.unwrap_err();

    bob_api.notes().remove(created.id).await?;

    let read = ana_api.notes().get(created.id).await?;

    assert_eq!(error.status(), Some(reqwest::StatusCode::NOT_FOUND));
    assert_eq!(read.title, "Milk");
    Ok(())
}

#[tokio::test]
async fn an_unknown_token_is_refused() -> anyhow::Result<()> {
    let served = Served::start().await;
    let token = served.sessions.open(ana());
    let api = served.client_with(format!("{token}0"))?;

    let error = api.notes().create(titled("Milk")).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::UNAUTHORIZED));
    Ok(())
}

#[test]
fn each_session_has_its_own_token() {
    let sessions = Sessions::default();

    let first = sessions.open(ana());
    let second = sessions.open(ana());

    assert_ne!(first, second);
    assert_eq!(first.len(), 64);
    assert!(first.bytes().all(|byte| byte.is_ascii_hexdigit()));
    assert_eq!(sessions.user(&first).map(|user| user.id), Some(1));
}

#[test]
fn the_sessions_debug_shows_no_token() {
    let sessions = Sessions::default();
    let token = sessions.open(ana());

    let shown = format!("{sessions:?}");

    assert_eq!(shown, "Sessions { open: 1 }");
    assert!(!shown.contains(&token));
}

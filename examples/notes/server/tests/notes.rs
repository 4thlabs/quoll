//! The notes server, served on a free port and called by the contract's client.

use notes_api::{Api, NewNote, Notes};
use notes_server::User;
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

/// The client of the server at `url`, sending `token`.
fn client(url: &str, token: String) -> rest::Result<notes_api::ApiClient> {
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::bearer(token))
        .build();

    Api::client_with(http, url)
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
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let api = client(&url, token)?;

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
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let api = client(&url, token)?;
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
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let api = client(&url, token)?;
    let created = api.notes().create(titled("Milk")).await?;

    api.notes().remove(created.id).await?;
    api.notes().remove(created.id).await?;
    Ok(())
}

#[tokio::test]
async fn a_note_of_another_user_is_not_found_and_not_removed() -> anyhow::Result<()> {
    let app = notes_server::App::in_memory();
    let ana = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let bob = app.sessions.open(User {
        id: 2,
        name: "bob".into(),
    });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let ana = client(&url, ana)?;
    let bob = client(&url, bob)?;
    let created = ana.notes().create(titled("Milk")).await?;

    let error = bob.notes().get(created.id).await.unwrap_err();
    bob.notes().remove(created.id).await?;
    let read = ana.notes().get(created.id).await?;

    assert_eq!(error.status(), Some(reqwest::StatusCode::NOT_FOUND));
    assert_eq!(read.title, "Milk");
    Ok(())
}

#[tokio::test]
async fn an_unknown_token_is_refused() -> anyhow::Result<()> {
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let api = client(&url, format!("{token}0"))?;

    let error = api.notes().create(titled("Milk")).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::UNAUTHORIZED));
    Ok(())
}

#[test]
fn each_session_has_its_own_token() {
    let sessions = notes_server::Sessions::default();

    let first = sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let second = sessions.open(User {
        id: 1,
        name: "ana".into(),
    });

    assert_ne!(first, second);
    assert_eq!(first.len(), 64);
    assert!(first.bytes().all(|byte| byte.is_ascii_hexdigit()));
    assert_eq!(sessions.user(&first).map(|user| user.id), Some(1));
}

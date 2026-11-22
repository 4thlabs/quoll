//! The whole answer of a route, on both sides: the status and the headers a handler gives its `Response`, what a
//! call's `.response()` reads of them, and the deadline a call gives itself with `.timeout`, each against a contract
//! served on a free port and called by the real client.

#![cfg(all(feature = "server", feature = "client"))]

use std::collections::HashSet;
use std::error::Error as _;
use std::future::IntoFuture;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{StreamExt, stream};
use http::header::{CACHE_CONTROL, CONTENT_TYPE, ETAG, LOCATION, X_CONTENT_TYPE_OPTIONS};
use http::{HeaderValue, StatusCode};
use quoll::client;
use quoll::rest::{self, Bytes, Call, Created, Error, Events, Response, Result, Status};
use reqwest_middleware::ClientBuilder;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use tokio::sync::Notify;

mod support {
    pub(crate) mod log;
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
    title: String,
}

rest::api! {
    api Api = "/api";

    group Notes {
        GET  "/notes/{id}"     => get(id: i64, header("If-None-Match") seen: Option<String>) -> Note;
        PUT  "/notes/{id}"     => put(id: i64, title: String) -> Note;
        POST "/notes"          => create(title: String) -> Created<Note>;
        GET  "/broken/{what}"  => broken(what: String) -> Note;
        GET  "/held"           => held();
        GET  "/slow"           => slow() -> u32;
        GET  "/events"         => events() -> Events<u32>;
        GET  "/logo"           => logo() -> Bytes;
    }

    /// A route the server below answers with a page of HTML.
    group Pages {
        GET "/page" => page() -> Note;
    }
}

/// The service: note 0 does not exist, each other has the `ETag` of its id; `held` waits for ever, `slow` tells
/// `entered` it has begun and waits until `release` is notified, and the head of `events` never comes on the first `held_heads` attempts.
#[derive(Clone, Default)]
struct App {
    written: Arc<Mutex<HashSet<i64>>>,
    entered: Arc<Notify>,
    release: Arc<Notify>,
    held_heads: usize,
    attempts: Arc<AtomicUsize>,
}

/// The `ETag` of note `id`.
fn etag(id: i64) -> String {
    format!("\"{id}\"")
}

impl NotesServer for App {
    async fn get(&self, id: i64, seen: Option<String>) -> Result<Response<Note>> {
        if id == 0 {
            return Err(Error::not_found("no such note"));
        }

        let note = Note {
            id,
            title: "Milk".into(),
        };
        let response = Response::new(note).header(ETAG, etag(id));

        if seen == Some(etag(id)) {
            return Ok(response.with_status(Status::NOT_MODIFIED));
        }

        Ok(response)
    }

    async fn put(&self, id: i64, title: String) -> Result<Response<Note>> {
        let created = self.written.lock().expect("not poisoned").insert(id);
        let response = Response::new(Note { id, title });

        if created {
            return Ok(response.with_status(Status::CREATED));
        }

        Ok(response)
    }

    async fn create(&self, title: String) -> Result<Response<Note>> {
        let note = Note { id: 7, title };
        let location = format!("/api/notes/{}", note.id);

        Ok(Response::new(note).header(LOCATION, location))
    }

    async fn broken(&self, what: String) -> Result<Response<Note>> {
        let note = Note {
            id: 1,
            title: "Milk".into(),
        };
        let response = Response::new(note);

        match what.as_str() {
            "value" => Ok(response.header("x-token", "secret\nline")),
            _ => Ok(response.header(CONTENT_TYPE, "text/html")),
        }
    }

    async fn held(&self) -> Result<Response<()>> {
        std::future::pending().await
    }

    async fn slow(&self) -> Result<Response<u32>> {
        self.entered.notify_one();
        self.release.notified().await;

        Ok(Response::new(1))
    }

    async fn events(&self) -> Result<Response<Events<u32>>> {
        let attempt = self.attempts.fetch_add(1, Ordering::SeqCst);

        if attempt < self.held_heads {
            std::future::pending::<()>().await;
        }

        let events = Events::new(stream::iter([1, 2]));

        Ok(Response::new(events).header(CACHE_CONTROL, "no-store"))
    }

    async fn logo(&self) -> Result<Response<Bytes>> {
        let logo = Bytes::new(b"<svg/>".to_vec()).with_content_type("image/svg+xml");

        Ok(Response::new(logo).header(X_CONTENT_TYPE_OPTIONS, "sniff"))
    }
}

impl PagesServer for App {
    async fn page(&self) -> Result<Response<Note>> {
        unreachable!("the page is served by a router of its own")
    }
}

/// The api served on a free port: its URL.
async fn serve(app: &App) -> String {
    quoll::test::spawn(Api::router(ApiServices {
        notes: app.clone(),
        pages: app.clone(),
    }))
    .await
}

/// A guard that fails a test rather than hang it, were a deadline not kept.
async fn guarded<F: IntoFuture>(call: F) -> F::Output {
    tokio::time::timeout(Duration::from_secs(10), call)
        .await
        .expect("the deadline ends the call")
}

/// Whether `error` comes from a deadline that passed: reqwest's own, or the one of the head of a stream.
fn timed_out(error: &Error) -> bool {
    let mut source = error.source();

    while let Some(error) = source {
        let reqwest = match error.downcast_ref::<reqwest_middleware::Error>() {
            Some(reqwest_middleware::Error::Reqwest(error)) => Some(error),
            _ => error.downcast_ref::<reqwest::Error>(),
        };

        if reqwest.is_some_and(reqwest::Error::is_timeout) || error.to_string().contains("deadline")
        {
            return true;
        }

        source = error.source();
    }

    false
}

#[tokio::test]
async fn a_response_gives_the_status_and_the_headers_a_handler_gave() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    let response = api.notes().get(3, None).response().await?;

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(response.headers()[ETAG], "\"3\"");
    assert_eq!(response.into_value()?.id, 3);

    let created = api.notes().create("Eggs".into()).response().await?;

    assert_eq!(created.status(), StatusCode::CREATED);
    assert_eq!(created.headers()[LOCATION], "/api/notes/7");
    assert_eq!(api.notes().create("Eggs".into()).await?.id, 7);
    Ok(())
}

#[tokio::test]
async fn a_header_a_handler_gives_replaces_the_one_quoll_puts() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    let response = api.notes().events().response().await?;
    let cache: Vec<&HeaderValue> = response.headers().get_all(CACHE_CONTROL).iter().collect();

    assert_eq!(cache, ["no-store"]);

    let events: Vec<u32> = response
        .into_value()?
        .map(|event| event.expect("an event"))
        .collect()
        .await;

    assert_eq!(events, [1, 2]);
    Ok(())
}

#[tokio::test]
async fn the_nosniff_of_bytes_stays_whatever_a_handler_gives() -> Result<()> {
    let url = serve(&App::default()).await;
    let response = Api::client(&url)?.notes().logo().response().await?;
    let options: Vec<&HeaderValue> = response
        .headers()
        .get_all(X_CONTENT_TYPE_OPTIONS)
        .iter()
        .collect();

    assert_eq!(options, ["nosniff"]);
    Ok(())
}

#[tokio::test]
async fn a_handler_answers_the_status_it_gives_in_place_of_the_contracts() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    let created = api.notes().put(4, "Milk".into()).response().await?;
    let replaced = api.notes().put(4, "Eggs".into()).response().await?;

    assert_eq!(created.status(), StatusCode::CREATED);
    assert_eq!(replaced.status(), StatusCode::OK);
    assert_eq!(replaced.into_value()?.title, "Eggs");
    Ok(())
}

#[tokio::test]
async fn a_304_has_its_headers_and_no_body() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    let response = api.notes().get(3, Some(etag(3))).response().await?;

    assert_eq!(response.status(), StatusCode::NOT_MODIFIED);
    assert_eq!(response.headers()[ETAG], "\"3\"");
    assert!(!response.headers().contains_key(CONTENT_TYPE));

    let error = response.into_value().expect_err("no value");

    assert_eq!(error.status(), Some(StatusCode::NOT_MODIFIED));
    assert_eq!(error.body(), Some(&b""[..]));

    let error = api
        .notes()
        .get(3, Some(etag(3)))
        .await
        .expect_err("no value");

    assert_eq!(error.status(), Some(StatusCode::NOT_MODIFIED));
    Ok(())
}

#[tokio::test]
async fn a_response_of_an_error_status_holds_its_error() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    let response = api.notes().get(0, None).response().await?;

    assert_eq!(response.status(), StatusCode::NOT_FOUND);
    assert_eq!(response.headers()[CONTENT_TYPE], "application/problem+json");

    let error = response.into_value().expect_err("an error status");

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(
        error.detail().and_then(|problem| problem.detail()),
        Some("no such note")
    );
    Ok(())
}

#[tokio::test]
async fn a_success_that_is_not_the_result_is_no_answer() -> Result<()> {
    let page = axum::Router::new().route(
        "/api/page",
        axum::routing::get(|| async { axum::response::Html("<p>Milk</p>") }),
    );
    let url = quoll::test::spawn(page).await;

    let error = Api::client(&url)?
        .pages()
        .page()
        .response()
        .await
        .expect_err("a page that is not a note");

    assert_eq!(error.status(), None);
    assert!(error.to_string().contains("not its result"), "{error}");
    Ok(())
}

#[tokio::test]
async fn a_header_a_handler_cannot_send_is_answered_500_and_logged_without_its_value() -> Result<()>
{
    let (log, _capture) = support::log::capture(false);
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    for what in ["value", "content-type"] {
        let error = api
            .notes()
            .broken(what.into())
            .await
            .expect_err("a header that cannot be sent");

        assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    }

    let log = log.text();

    assert!(log.contains("the header `x-token`"), "{log}");
    assert!(log.contains("`content-type`, which quoll writes"), "{log}");
    assert!(!log.contains("secret"), "{log}");
    Ok(())
}

#[tokio::test]
async fn a_calls_timeout_ends_it_on_a_client_with_no_middleware() -> Result<()> {
    let url = serve(&App::default()).await;

    let error = guarded(
        Api::client(&url)?
            .notes()
            .held()
            .timeout(Duration::from_millis(100)),
    )
    .await
    .expect_err("a call past its deadline");

    assert_eq!(error.status(), None);
    assert!(timed_out(&error), "{error:?}");
    Ok(())
}

#[tokio::test]
async fn a_calls_timeout_replaces_the_clients() -> Result<()> {
    let app = App::default();
    let url = serve(&app).await;
    let long = client::builder()
        .with(client::timeout(Duration::from_secs(30)))
        .build::<Api>(&url)?;
    let short = client::builder()
        .with(client::timeout(Duration::from_millis(100)))
        .build::<Api>(&url)?;

    let error = guarded(long.notes().held().timeout(Duration::from_millis(100)))
        .await
        .expect_err("a call past its own deadline");

    assert!(timed_out(&error), "{error:?}");

    let slow =
        tokio::spawn(async move { short.notes().slow().timeout(Duration::from_secs(30)).await });

    // The server answers past the client's deadline, which the call's own replaces.
    app.entered.notified().await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    app.release.notify_one();

    assert_eq!(guarded(slow).await.expect("the call ran")?, 1);
    Ok(())
}

#[tokio::test]
async fn a_calls_timeout_bounds_each_attempt_at_a_streams_head_which_retry_tries_again()
-> Result<()> {
    let app = App {
        held_heads: 1,
        ..App::default()
    };
    let url = serve(&app).await;
    let api = client::builder()
        .with(client::retry(2))
        .build::<Api>(&url)?;

    let mut events = guarded(api.notes().events().timeout(Duration::from_millis(200))).await?;

    assert_eq!(events.next().await.expect("an event")?, 1);
    assert_eq!(app.attempts.load(Ordering::SeqCst), 2);
    Ok(())
}

#[tokio::test]
async fn a_calls_timeout_bounds_it_on_a_chain_built_otherwise() -> Result<()> {
    let app = App {
        held_heads: usize::MAX,
        ..App::default()
    };
    let url = serve(&app).await;
    let http = ClientBuilder::new(reqwest::Client::new()).build();
    let api = Api::client_with(http, &url)?;

    let Err(error) = guarded(api.notes().events().timeout(Duration::from_millis(100))).await else {
        panic!("a head that never comes gave events");
    };

    assert_eq!(error.status(), None);
    assert!(timed_out(&error), "{error:?}");

    let error = guarded(api.notes().held().timeout(Duration::from_millis(100)))
        .await
        .expect_err("a call past its deadline");

    assert!(timed_out(&error), "{error:?}");
    Ok(())
}

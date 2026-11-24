//! The whole answer of a route, on both sides: the status and the headers a handler gives its `Response`, what a
//! call's `.response()` reads of them, and the deadline a call gives itself with `.timeout`, each against a contract
//! served on a free port and called by the real client.

#![cfg(all(feature = "server", feature = "client"))]

use std::collections::HashSet;
use std::error::Error as _;
use std::future::{Future, IntoFuture};
use std::pin::Pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{StreamExt, stream};
use http::header::{CACHE_CONTROL, CONTENT_TYPE, ETAG, LOCATION, X_CONTENT_TYPE_OPTIONS};
use http::{HeaderValue, StatusCode};
use quoll::{
    Accepted, ByteStream, Bytes, Call, Created, Error, Events, JsonLines, Response, Result, Status,
    Text, client,
};
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

/// A note that may be missing, under a name of its own: its type, not its name, may be empty.
type MaybeNote = Option<Note>;

quoll::api! {
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
        GET  "/drafts/{id}"    => draft(id: i64) -> Option<Note>;
        GET  "/maybe/{id}"     => maybe(id: i64) -> MaybeNote;
        POST "/drafts"         => publish(title: String) -> Created<Option<Note>>;
        PUT  "/jobs/{id}"      => start(id: i64) -> Accepted<()>;
        POST "/exports"        => export(title: String) -> Accepted<Note>;
    }

    /// Results that may be empty, each answered with a 204 by the server below.
    group Quiet {
        GET "/quiet/text"   => text() -> Text;
        GET "/quiet/bytes"  => bytes() -> Bytes;
        GET "/quiet/events" => events() -> Events<u32>;
        GET "/quiet/lines"  => lines() -> JsonLines<u32>;
        GET "/quiet/output" => output() -> ByteStream;
    }

    /// A route the server below answers with a page of HTML.
    group Pages {
        GET "/page" => page() -> Note;
        GET "/empty" => empty() -> Note;
        GET "/empty/value" => value() -> serde_json::Value;
        GET "/reset/text" => reset_text() -> Text;
        GET "/reset/events" => reset_events() -> Events<u32>;
        GET "/reset/bytes" => reset_bytes() -> Bytes;
        GET "/reset/output" => reset_output() -> ByteStream;
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

    async fn draft(&self, id: i64) -> Result<Response<Option<Note>>> {
        if id == 0 {
            let missing = Response::new(None).header(ETAG, etag(id));

            return Ok(missing.with_status(Status::NO_CONTENT));
        }

        let note = Note {
            id,
            title: "Draft".into(),
        };

        Ok(Response::new(Some(note)))
    }

    async fn maybe(&self, id: i64) -> Result<Response<MaybeNote>> {
        self.draft(id).await
    }

    async fn publish(&self, title: String) -> Result<Response<Option<Note>>> {
        if title.is_empty() {
            return Ok(Response::new(None).with_status(Status::NO_CONTENT));
        }
        if title == "reset" {
            return Ok(Response::new(None).with_status(Status::RESET_CONTENT));
        }

        Ok(Response::new(Some(Note { id: 8, title })))
    }

    async fn start(&self, id: i64) -> Result<Response<()>> {
        if id == 0 {
            return Ok(Response::new(()).with_status(Status::NO_CONTENT));
        }
        if id < 0 {
            return Ok(Response::new(()).with_status(Status::RESET_CONTENT));
        }

        Ok(Response::new(()))
    }

    async fn export(&self, title: String) -> Result<Response<Note>> {
        Ok(Response::new(Note { id: 9, title }))
    }

    async fn logo(&self) -> Result<Response<Bytes>> {
        let logo = Bytes::new(b"<svg/>".to_vec()).with_content_type("image/svg+xml");

        Ok(Response::new(logo).header(X_CONTENT_TYPE_OPTIONS, "sniff"))
    }
}

/// Each result given a body, which a 204 leaves unsent.
impl QuietServer for App {
    async fn text(&self) -> Result<Response<Text>> {
        Ok(Response::new(Text::from("unsent")).with_status(Status::NO_CONTENT))
    }

    async fn bytes(&self) -> Result<Response<Bytes>> {
        Ok(Response::new(Bytes::new(b"unsent".to_vec())).with_status(Status::NO_CONTENT))
    }

    async fn events(&self) -> Result<Response<Events<u32>>> {
        let events = Events::new(stream::iter([1, 2]));

        Ok(Response::new(events).with_status(Status::RESET_CONTENT))
    }

    async fn lines(&self) -> Result<Response<JsonLines<u32>>> {
        let lines = JsonLines::new(stream::iter([1, 2]));

        Ok(Response::new(lines).with_status(Status::NO_CONTENT))
    }

    async fn output(&self) -> Result<Response<ByteStream>> {
        let output = ByteStream::new(stream::iter([b"unsent".to_vec()]));

        Ok(Response::new(output).with_status(Status::NO_CONTENT))
    }
}

impl PagesServer for App {
    async fn page(&self) -> Result<Response<Note>> {
        unreachable!("the page is served by a router of its own")
    }

    async fn empty(&self) -> Result<Response<Note>> {
        unreachable!("the empty answer is served by a router of its own")
    }

    async fn value(&self) -> Result<Response<serde_json::Value>> {
        unreachable!("the empty answer is served by a router of its own")
    }

    async fn reset_text(&self) -> Result<Response<Text>> {
        unreachable!("the 205 is served by a router of its own")
    }

    async fn reset_events(&self) -> Result<Response<Events<u32>>> {
        unreachable!("the 205 is served by a router of its own")
    }

    async fn reset_bytes(&self) -> Result<Response<Bytes>> {
        unreachable!("the 205 is served by a router of its own")
    }

    async fn reset_output(&self) -> Result<Response<ByteStream>> {
        unreachable!("the 205 is served by a router of its own")
    }
}

/// The api served on a free port: its URL.
async fn serve(app: &App) -> String {
    quoll::test::spawn(Api::router(ApiServices {
        notes: app.clone(),
        quiet: app.clone(),
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
async fn a_204_of_a_route_of_an_option_is_none() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = Api::client(&url)?;

    let response = api.notes().draft(0).response().await?;

    assert_eq!(response.status(), StatusCode::NO_CONTENT);
    assert_eq!(response.headers()[ETAG], etag(0));
    assert_eq!(response.into_value()?, None);
    assert_eq!(api.notes().draft(0).await?, None);
    assert_eq!(api.notes().draft(2).await?.map(|note| note.id), Some(2));
    Ok(())
}

#[tokio::test]
async fn a_204_is_given_by_the_handlers_value_whatever_the_result_wraps_it_in() -> Result<()> {
    let url = serve(&App::default()).await;
    let notes = Api::client(&url)?.notes();

    let published = notes.publish(String::new()).response().await?;

    assert_eq!(published.status(), StatusCode::NO_CONTENT);
    assert_eq!(published.into_value()?, None);
    assert_eq!(
        notes.publish("Milk".into()).await?.map(|note| note.id),
        Some(8)
    );

    let reset = notes.publish("reset".into()).response().await?;

    assert_eq!(reset.status(), StatusCode::RESET_CONTENT);
    assert_eq!(reset.into_value()?, None);

    let started = notes.start(1).response().await?;
    let skipped = notes.start(0).response().await?;
    let reset = notes.start(-1).response().await?;

    assert_eq!(started.status(), StatusCode::ACCEPTED);
    assert_eq!(skipped.status(), StatusCode::NO_CONTENT);
    assert_eq!(reset.status(), StatusCode::RESET_CONTENT);
    skipped.into_value()?;
    reset.into_value()?;

    assert_eq!(notes.maybe(0).await?, None);
    assert_eq!(notes.maybe(3).await?.map(|note| note.id), Some(3));
    Ok(())
}

#[test]
fn the_document_lists_a_204_by_the_handlers_value() {
    let document = serde_json::to_value(Api::openapi()).expect("a document");
    let listed = |path: &str, method: &str| -> Vec<String> {
        document["paths"][path][method]["responses"]
            .as_object()
            .expect("its responses")
            .keys()
            .cloned()
            .collect()
    };

    for (path, method, status) in [
        ("/api/drafts", "post", "201"),
        ("/api/jobs/{id}", "put", "202"),
        ("/api/maybe/{id}", "get", "200"),
    ] {
        let listed = listed(path, method);

        assert!(
            listed.iter().any(|listed| listed == status),
            "{method} {path}: {listed:?}"
        );
        assert!(
            listed.iter().any(|listed| listed == "204"),
            "{method} {path}: {listed:?}"
        );
    }

    // A value that must be one, a `Created<Note>` or an `Accepted<Note>`.
    for (path, method) in [("/api/notes", "post"), ("/api/exports", "post")] {
        let listed = listed(path, method);

        assert!(
            !listed.iter().any(|listed| listed == "204"),
            "{method} {path}: {listed:?}"
        );
    }
}

#[tokio::test]
async fn a_204_of_a_result_that_may_be_empty_is_read_empty() -> Result<()> {
    let url = serve(&App::default()).await;
    let quiet = Api::client(&url)?.quiet();

    let text = quiet.text().response().await?;

    assert_eq!(text.status(), StatusCode::NO_CONTENT);
    assert_eq!(&*text.into_value()?, "");
    assert!(quiet.bytes().await?.into_vec().is_empty());

    let events = quiet.events().await?;
    let lines = quiet.lines().await?;
    let output = quiet.output().await?;

    assert!(events.collect::<Vec<_>>().await.is_empty());
    assert!(lines.collect::<Vec<_>>().await.is_empty());
    assert!(output.collect::<Vec<_>>().await.is_empty());
    Ok(())
}

#[tokio::test]
async fn a_204_of_another_api_on_a_route_of_a_value_keeps_its_status_and_its_headers() -> Result<()>
{
    let empty = axum::Router::new().route(
        "/api/empty",
        axum::routing::get(|| async { (StatusCode::NO_CONTENT, [(ETAG, "\"9\"")]) }),
    );
    let url = quoll::test::spawn(empty).await;
    let api = Api::client(&url)?;

    let response = api.pages().empty().response().await?;

    assert_eq!(response.status(), StatusCode::NO_CONTENT);
    assert_eq!(response.headers()[ETAG], "\"9\"");

    let error = response.into_value().expect_err("no note");

    assert_eq!(error.status(), None);
    assert!(error.to_string().contains("not its result"), "{error}");

    let error = api.pages().empty().await.expect_err("no note");

    assert_eq!(error.status(), None);
    Ok(())
}

#[tokio::test]
async fn a_204_is_null_for_an_option_alone_not_for_any_json() -> Result<()> {
    let empty = axum::Router::new().route(
        "/api/empty/value",
        axum::routing::get(|| async { StatusCode::NO_CONTENT }),
    );
    let url = quoll::test::spawn(empty).await;

    // A `Value` reads `null` as JSON, but a 204 is not the JSON `null`.
    let response = Api::client(&url)?.pages().value().response().await?;

    assert_eq!(response.status(), StatusCode::NO_CONTENT);

    let error = response.into_value().expect_err("no value");

    assert!(error.to_string().contains("not its result"), "{error}");
    Ok(())
}

#[tokio::test]
async fn the_body_another_api_sends_with_a_205_is_dropped() -> Result<()> {
    let reset = axum::Router::new()
        .route(
            "/api/reset/text",
            axum::routing::get(|| async {
                (
                    StatusCode::RESET_CONTENT,
                    [(CONTENT_TYPE, "text/plain")],
                    "unsent",
                )
            }),
        )
        .route(
            "/api/reset/bytes",
            axum::routing::get(|| async {
                (
                    StatusCode::RESET_CONTENT,
                    [(CONTENT_TYPE, "image/png")],
                    "unsent",
                )
            }),
        )
        .route(
            "/api/reset/output",
            axum::routing::get(|| async {
                (
                    StatusCode::RESET_CONTENT,
                    [(CONTENT_TYPE, "text/x-log")],
                    "unsent",
                )
            }),
        )
        .route(
            "/api/reset/events",
            axum::routing::get(|| async {
                (
                    StatusCode::RESET_CONTENT,
                    [(CONTENT_TYPE, "text/event-stream")],
                    "data: 1\n\n",
                )
            }),
        );
    let url = quoll::test::spawn(reset).await;
    let pages = Api::client(&url)?.pages();

    assert_eq!(&*pages.reset_text().await?, "");

    // Of no type: the type was the dropped body's.
    let bytes = pages.reset_bytes().await?;

    assert_eq!(bytes.content_type(), "application/octet-stream");
    assert!(bytes.into_vec().is_empty());

    let output = pages.reset_output().await?;

    assert_eq!(output.content_type(), "application/octet-stream");
    assert!(output.collect::<Vec<_>>().await.is_empty());
    assert!(
        pages
            .reset_events()
            .await?
            .collect::<Vec<_>>()
            .await
            .is_empty()
    );
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

/// What a middleware's call returns.
type Handled<'a> =
    Pin<Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>>;

/// A middleware of the application's that waits `wait` before it sends each request, as a rate limiter does.
fn waiting(wait: Duration) -> impl reqwest_middleware::Middleware {
    fn middleware<F>(handle: F) -> F
    where
        F: for<'a> Fn(
                reqwest::Request,
                &'a mut http::Extensions,
                reqwest_middleware::Next<'a>,
            ) -> Handled<'a>
            + Send
            + Sync
            + 'static,
    {
        handle
    }

    middleware(move |request, extensions, next| {
        Box::pin(async move {
            tokio::time::sleep(wait).await;
            next.run(request, extensions).await
        })
    })
}

#[tokio::test]
async fn a_calls_timeout_on_a_stream_starts_after_a_middleware_before_the_helpers() -> Result<()> {
    let url = serve(&App::default()).await;
    let api = client::builder()
        .with(waiting(Duration::from_millis(400)))
        .with(client::retry(2))
        .build::<Api>(&url)?;

    // The middleware waits past the call's deadline before the attempt starts: the deadline is the attempt's.
    let mut events = guarded(api.notes().events().timeout(Duration::from_millis(100))).await?;

    assert_eq!(events.next().await.expect("an event")?, 1);
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

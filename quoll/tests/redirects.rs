//! `rest::client::redirects`: redirects followed one hop at a time, through the middleware after it, between servers
//! on free ports, so that a credential reaches the api's own origin only.

#![cfg(all(feature = "server", feature = "client"))]

use std::error::Error as _;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::Router;
use axum::body::Bytes;
use axum::extract::Request;
use axum::response::{IntoResponse, Response};
use futures_util::StreamExt;
use quoll::rest::{self, Events, Result, Route, Text};
use reqwest::header::{AUTHORIZATION, CONTENT_TYPE, HeaderValue, LOCATION};
use reqwest::{Method, StatusCode};
use reqwest_middleware::ClientBuilder;

rest::api! {
    api Api = "/api";

    /// The key of an operator's scripts.
    scheme Key = header "X-Api-Key";

    #[auth(Bearer | Key)]
    group Moves {
        GET  "/away"      => away() -> String;
        GET  "/near"      => near() -> String;
        GET  "/loop"      => looping() -> String;
        GET  "/endless"   => endless() -> String;
        GET  "/utf8"      => utf8() -> String;
        GET  "/slow"      => slow() -> Events<u32>;
        POST "/see-other" => see_other(text: Text) -> String;
        POST "/temporary" => temporary(text: Text) -> String;
    }
}

/// A request a server got: its method, its path, whether it carried each credential and the headers added before
/// `redirects`, and its body.
#[derive(Debug, Clone, PartialEq)]
struct Got {
    method: Method,
    path: String,
    bearer: bool,
    key: bool,
    signature: bool,
    /// A header marked sensitive, added before `redirects`.
    secret: bool,
    /// A header not marked sensitive, added before `redirects`.
    plain: bool,
    agent: bool,
    id: bool,
    body: String,
}

impl Got {
    /// A `GET` of `path` with no body, carrying the credentials or not, and every other header.
    fn get(path: &str, credentials: bool) -> Self {
        Self {
            method: Method::GET,
            path: path.into(),
            bearer: credentials,
            key: credentials,
            signature: credentials,
            secret: credentials,
            plain: true,
            agent: true,
            id: true,
            body: String::new(),
        }
    }
}

/// What the servers got, in order.
type Log = Arc<Mutex<Vec<Got>>>;

/// Keeps `request` in `log`, and answers the JSON string of its path.
async fn keep(log: Log, request: Request) -> Response {
    let (parts, body) = request.into_parts();
    let body = axum::body::to_bytes(body, usize::MAX)
        .await
        .unwrap_or_else(|_| Bytes::new());
    let path = parts.uri.path().to_owned();

    log.lock()
        .expect("no test panics while holding it")
        .push(Got {
            method: parts.method,
            path: path.clone(),
            bearer: parts.headers.contains_key(AUTHORIZATION),
            key: parts.headers.contains_key("x-api-key"),
            signature: parts.headers.contains_key("x-signature"),
            secret: parts.headers.contains_key("x-secret"),
            plain: parts.headers.contains_key("x-plain"),
            agent: parts.headers.contains_key("user-agent"),
            id: parts.headers.contains_key("x-request-id"),
            body: String::from_utf8_lossy(&body).into_owned(),
        });

    (
        [(CONTENT_TYPE, "application/json")],
        serde_json::to_string(&path).expect("a string is JSON"),
    )
        .into_response()
}

/// A redirect of `status` to `to`.
fn redirect(status: StatusCode, to: String) -> Response {
    (status, [(LOCATION, to)]).into_response()
}

/// The api, which redirects each of its routes, and another server, which keeps what it gets: the api's client
/// through the middleware in the order the docs advise, `redirects(hops)` before the credentials and a signature,
/// with two headers added before it, one marked sensitive; and what both servers got.
async fn served(hops: usize) -> (ApiClient, Log) {
    let log = Log::default();
    let keeping = |log: &Log| {
        let log = Arc::clone(log);

        move |request: Request| keep(Arc::clone(&log), request)
    };

    let elsewhere = rest::test::spawn(Router::new().fallback(keeping(&log))).await;

    let api = Router::new()
        .route(
            "/api/away",
            axum::routing::get(move || async move {
                redirect(StatusCode::FOUND, format!("{elsewhere}/elsewhere"))
            }),
        )
        .route(
            "/api/near",
            axum::routing::get(|| async {
                redirect(StatusCode::MOVED_PERMANENTLY, "/api/here".into())
            }),
        )
        .route(
            "/api/loop",
            axum::routing::get(|| async { redirect(StatusCode::FOUND, "/api/loop".into()) }),
        )
        .route(
            "/api/endless",
            axum::routing::get(|| async { redirect(StatusCode::FOUND, "/api/hop/1".into()) }),
        )
        .route(
            "/api/hop/{n}",
            axum::routing::get(
                |axum::extract::Path(n): axum::extract::Path<u32>| async move {
                    redirect(StatusCode::FOUND, format!("/api/hop/{}", n + 1))
                },
            ),
        )
        .route(
            "/api/utf8",
            axum::routing::get(|| async {
                let to =
                    HeaderValue::from_bytes("/api/caf\u{e9}".as_bytes()).expect("a header's value");

                (StatusCode::FOUND, [(LOCATION, to)]).into_response()
            }),
        )
        .route(
            "/api/see-other",
            axum::routing::post(|| async { redirect(StatusCode::SEE_OTHER, "/api/landed".into()) }),
        )
        .route(
            "/api/temporary",
            axum::routing::post(|| async {
                redirect(StatusCode::TEMPORARY_REDIRECT, "/api/landed".into())
            }),
        )
        .fallback(keeping(&log));
    let url = rest::test::spawn(api).await;

    let sign = rest::client::on_request(|request: &mut reqwest::Request, _: &Route| {
        request.headers_mut().insert(
            "x-signature",
            "signed".try_into().expect("a header's value"),
        );
    });
    let http = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .expect("a client");
    let before = rest::client::on_request(|request: &mut reqwest::Request, _: &Route| {
        let mut secret = HeaderValue::from_static("s-1");

        secret.set_sensitive(true);

        request.headers_mut().insert("x-secret", secret);
        request
            .headers_mut()
            .insert("x-plain", HeaderValue::from_static("p-1"));
    });
    let http = ClientBuilder::new(http)
        .with(rest::client::trace())
        .with(rest::client::user_agent("notes-cli/0.1"))
        .with(rest::client::request_id())
        .with(rest::client::retry(1))
        .with(rest::client::timeout(Duration::from_secs(10)))
        .with(before)
        .with(rest::client::redirects(hops))
        .with(rest::client::bearer("t-1"))
        .with(rest::client::key::<Key>("k-1"))
        .with(sign)
        .build();

    (Api::client_with(http, &url).expect("the api's url"), log)
}

/// What the servers got.
fn got(log: &Log) -> Vec<Got> {
    log.lock().expect("no test panics while holding it").clone()
}

#[tokio::test]
async fn a_redirect_to_another_origin_gets_no_credential() -> Result<()> {
    let (api, log) = served(5).await;

    assert_eq!(api.moves().away().await?, "/elsewhere");

    assert_eq!(got(&log), [Got::get("/elsewhere", false)]);
    Ok(())
}

#[tokio::test]
async fn a_redirect_to_the_apis_own_origin_gets_every_credential() -> Result<()> {
    let (api, log) = served(5).await;

    assert_eq!(api.moves().near().await?, "/api/here");

    assert_eq!(got(&log), [Got::get("/api/here", true)]);
    Ok(())
}

#[tokio::test]
async fn a_303_is_followed_with_a_get_and_no_body_and_a_307_with_both() -> Result<()> {
    let (api, log) = served(5).await;

    api.moves().see_other(Text("a note".into())).await?;
    api.moves().temporary(Text("a note".into())).await?;

    let got: Vec<(Method, String)> = got(&log)
        .into_iter()
        .map(|got| (got.method, got.body))
        .collect();

    assert_eq!(
        got,
        [
            (Method::GET, String::new()),
            (Method::POST, "a note".into())
        ]
    );
    Ok(())
}

#[tokio::test]
async fn a_location_in_utf8_is_followed_as_reqwest_follows_it() -> Result<()> {
    let (api, log) = served(5).await;

    assert_eq!(api.moves().utf8().await?, "/api/caf%C3%A9");
    assert_eq!(got(&log), [Got::get("/api/caf%C3%A9", true)]);
    Ok(())
}

#[tokio::test]
async fn a_redirect_loop_is_an_error() {
    let (api, log) = served(5).await;

    let error = api.moves().looping().await.expect_err("a loop");

    assert_eq!(error.status(), None);
    let source = error.source().map(ToString::to_string).unwrap_or_default();

    assert!(source.contains("sent already"), "{error:?}");
    assert!(got(&log).is_empty());
}

#[tokio::test]
async fn redirects_past_the_limit_are_an_error() {
    let (api, log) = served(3).await;

    let error = api.moves().endless().await.expect_err("too many redirects");

    assert_eq!(error.status(), None);
    let source = error.source().map(ToString::to_string).unwrap_or_default();

    assert!(source.contains("more than 3 times"), "{error:?}");
    assert!(got(&log).is_empty());
}

#[tokio::test]
async fn the_deadline_of_a_streams_head_starts_again_on_each_hop() -> Result<()> {
    // Each hop answers after 60 % of the deadline: in time, but not twice in a row.
    const DEADLINE: Duration = Duration::from_millis(1000);
    const HOP: Duration = Duration::from_millis(600);

    let api = Router::new()
        .route(
            "/api/slow",
            axum::routing::get(|| async {
                tokio::time::sleep(HOP).await;

                redirect(StatusCode::FOUND, "/api/slow/here".into())
            }),
        )
        .route(
            "/api/slow/here",
            axum::routing::get(|| async {
                tokio::time::sleep(HOP).await;

                ([(CONTENT_TYPE, "text/event-stream")], "data: 1\n\n").into_response()
            }),
        );
    let url = rest::test::spawn(api).await;
    let http = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .expect("a client");
    let http = ClientBuilder::new(http)
        .with(rest::client::timeout(DEADLINE))
        .with(rest::client::redirects(5))
        .build();
    let api = Api::client_with(http, &url)?;

    let mut events = api.moves().slow().await?;

    assert_eq!(events.next().await.expect("an event")?, 1);
    Ok(())
}

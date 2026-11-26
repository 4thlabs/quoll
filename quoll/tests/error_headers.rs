//! The headers of a handler's error, from the handler to the client: a 429 with its `Retry-After`, which the client
//! reads back and `client::retry` waits; a 401 with its own challenge; what the server refuses or does not send; and a
//! fake that builds such an error.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use http::StatusCode;
use http::header::{ALLOW, CONNECTION, CONTENT_TYPE, RETRY_AFTER, WWW_AUTHENTICATE};
use quoll::client::{self, Attempt, Retry};
use quoll::{Authenticate, Bearer, Call, Error, ErrorKind, Response, Result};
use tokio::time::Instant;

mod support {
    pub(crate) mod log;
}

quoll::api! {
    api Api = "/api";

    group Limits {
        /// A 429 on the first `failures` calls, each with a `Retry-After` of 2 s, then the note.
        POST "/sign-in"     => sign_in() -> String;
        /// An error given a header of each kind it may carry, then one it cannot.
        GET "/errors/{what}" => error(what: &str) -> String;
        /// A 401 of the authentication, with a challenge of its own for an expired token.
        #[auth(Bearer)]
        GET "/me"           => me() -> String;
    }
}

/// The service: it counts the calls of `sign_in`, and answers a 429 to the first `failures`.
#[derive(Clone, Default)]
struct App {
    calls: Arc<AtomicU32>,
    failures: u32,
}

/// The error of too many sign-ins, which says when to try again.
fn too_many() -> Error {
    Error::new(StatusCode::TOO_MANY_REQUESTS, "too many failed sign-ins")
        .retry_after(Duration::from_secs(2))
}

impl LimitsServer for App {
    async fn sign_in(&self) -> Result<Response<String>> {
        let call = self.calls.fetch_add(1, Ordering::SeqCst);

        if call < self.failures {
            return Err(too_many());
        }

        Ok(Response::new("signed in".into()))
    }

    async fn error(&self, what: String) -> Result<Response<String>> {
        let error = match what.as_str() {
            "allow" => Error::new(StatusCode::METHOD_NOT_ALLOWED, "read only")
                .with_header(ALLOW, "GET")
                .with_header(ALLOW, "HEAD"),
            "internal" => Error::internal("the store is down").retry_after(Duration::from_secs(9)),
            "upstream" => Error::failed(
                ErrorKind::Status(StatusCode::TOO_MANY_REQUESTS),
                "another api limits us",
            )
            .retry_after(Duration::from_secs(9)),
            "connection" => too_many().with_header(CONNECTION, "close"),
            "content-type" => too_many().with_header(CONTENT_TYPE, "text/html"),
            _ => Error::not_found("no such error"),
        };

        Err(error)
    }

    async fn me(&self, _user: &()) -> Result<Response<String>> {
        Ok(Response::new("ana".into()))
    }
}

impl Authenticate<Bearer> for App {
    type Caller = ();

    async fn authenticate(&self, token: String) -> Result<()> {
        if token == "expired" {
            return Err(Error::unauthorized("the token has expired")
                .with_header(WWW_AUTHENTICATE, r#"Bearer error="invalid_token""#));
        }

        Err(Error::unauthorized("unknown token"))
    }
}

/// The api, served on a free port with `app`: its URL.
async fn serve(app: &App) -> String {
    quoll::test::spawn(Api::router(ApiServices {
        limits: app.clone(),
    }))
    .await
}

#[tokio::test]
async fn a_429_with_retry_after_from_a_handler_is_read_by_the_client() {
    let app = App {
        failures: 1,
        ..App::default()
    };
    let api = Api::client(&serve(&app).await).expect("a client");

    let error = api.limits().sign_in().await.expect_err("a 429");

    assert_eq!(error.status(), Some(StatusCode::TOO_MANY_REQUESTS));
    assert_eq!(error.headers()[RETRY_AFTER], "2");
    assert_eq!(
        error.detail().and_then(|problem| problem.detail()),
        Some("too many failed sign-ins")
    );

    let response = error.response().expect("a problem");

    assert_eq!(response.headers()[RETRY_AFTER], "2");
}

#[tokio::test(start_paused = true)]
async fn retry_waits_the_retry_after_of_a_handlers_429() {
    let app = App {
        failures: 2,
        ..App::default()
    };
    let decided = Arc::new(Mutex::new(Vec::new()));
    let retry = {
        let decided = Arc::clone(&decided);

        client::retry(2).when(move |attempt: &Attempt| {
            let decision = match attempt.status() {
                Some(StatusCode::TOO_MANY_REQUESTS) => attempt.retry_after(),
                _ => attempt.transient(),
            };

            decided
                .lock()
                .expect("no test panics with the lock")
                .push(decision);
            decision
        })
    };
    let api = client::builder()
        .with(retry)
        .build::<Api>(&serve(&app).await)
        .expect("a client");
    let started = Instant::now();

    let signed_in = api
        .limits()
        .sign_in()
        .await
        .expect("signed in on the third attempt");

    assert_eq!(signed_in, "signed in");
    assert_eq!(app.calls.load(Ordering::SeqCst), 3);
    assert_eq!(
        *decided.lock().expect("no test panics with the lock"),
        [
            Retry::After(Duration::from_secs(2)),
            Retry::After(Duration::from_secs(2)),
            Retry::No,
        ]
    );
    assert_eq!(started.elapsed(), Duration::from_secs(4));
}

#[tokio::test]
async fn a_header_given_more_than_once_is_sent_with_each_value() {
    let api = Api::client(&serve(&App::default()).await).expect("a client");

    let error = api.limits().error("allow").await.expect_err("a 405");
    let allowed: Vec<_> = error.headers().get_all(ALLOW).iter().collect();

    assert_eq!(error.status(), Some(StatusCode::METHOD_NOT_ALLOWED));
    assert_eq!(allowed, ["GET", "HEAD"]);
}

#[tokio::test]
async fn a_401_with_its_own_challenge_keeps_it_in_place_of_the_schemes() {
    let url = serve(&App::default()).await;
    let me = |token: &'static str| {
        let url = format!("{url}/api/me");

        async move {
            reqwest::Client::new()
                .get(url)
                .bearer_auth(token)
                .send()
                .await
                .expect("an answer")
        }
    };

    let expired = me("expired").await;
    let unknown = me("unknown").await;

    let challenges = |response: &reqwest::Response| -> Vec<String> {
        response
            .headers()
            .get_all(WWW_AUTHENTICATE)
            .iter()
            .map(|value| value.to_str().expect("text").to_owned())
            .collect()
    };

    assert_eq!(expired.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(challenges(&expired), [r#"Bearer error="invalid_token""#]);
    assert_eq!(unknown.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(challenges(&unknown), ["Bearer"]);
}

#[tokio::test]
async fn an_error_answered_with_a_status_of_the_servers_own_sends_no_header() {
    let api = Api::client(&serve(&App::default()).await).expect("a client");

    let internal = api.limits().error("internal").await.expect_err("a 500");
    let upstream = api.limits().error("upstream").await.expect_err("a 502");

    assert_eq!(internal.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    assert!(!internal.headers().contains_key(RETRY_AFTER));
    assert_eq!(upstream.status(), Some(StatusCode::BAD_GATEWAY));
    assert!(!upstream.headers().contains_key(RETRY_AFTER));
}

#[tokio::test]
async fn a_refused_header_is_left_out_and_logged_and_the_error_answered_as_it_is() {
    let (log, _capture) = support::log::capture(false);
    let api = Api::client(&serve(&App::default()).await).expect("a client");

    let connection = api.limits().error("connection").await.expect_err("a 429");
    let content_type = api.limits().error("content-type").await.expect_err("a 429");

    for error in [&connection, &content_type] {
        assert_eq!(error.status(), Some(StatusCode::TOO_MANY_REQUESTS));
        assert_eq!(error.headers()[RETRY_AFTER], "2", "{error:?}");
        assert!(!error.headers().contains_key(CONNECTION), "{error:?}");
        assert_eq!(
            error.headers()[CONTENT_TYPE],
            "application/problem+json",
            "{error:?}"
        );
    }

    let log = log.text();
    let warned = |refused: &str| {
        log.lines().any(|line| {
            line.contains("WARN")
                && line.contains(&format!(
                    "{refused}: a handler's error is answered without it"
                ))
        })
    };

    assert!(
        warned(
            "the header `connection` is hop-by-hop: it belongs to a connection, not to an answer"
        ),
        "{log}"
    );
    assert!(
        warned("the header `content-type` is written by quoll from the problem it answers"),
        "{log}"
    );
    assert!(!log.contains("text/html"), "{log}");
}

#[test]
fn the_debug_form_of_an_error_names_its_headers_and_a_refusal_without_their_values() {
    let error = too_many()
        .with_header(WWW_AUTHENTICATE, "Bearer secret-realm")
        .with_header(CONNECTION, "close");
    let shown = format!("{error:?}");

    assert!(
        shown.contains(r#"headers: ["retry-after", "www-authenticate"]"#),
        "{shown}"
    );
    assert!(
        shown.contains("refused: \"the header `connection` is hop-by-hop"),
        "{shown}"
    );
    assert!(
        !shown.contains("secret-realm") && !shown.contains("close"),
        "{shown}"
    );
    assert!(!format!("{:?}", Error::not_found("x")).contains("headers"));
}

/// Limits kept in memory, in place of a server: every sign-in is refused, with a `Retry-After`.
struct Fake;

impl Limits for Fake {
    fn sign_in(&self) -> impl Call<String> {
        async { Err(too_many()) }
    }

    fn error(&self, _what: &str) -> impl Call<String> {
        async { Err(Error::not_found("no such error")) }
    }

    fn me(&self) -> impl Call<String> {
        async { Ok("ana".into()) }
    }
}

/// Code of the application, written for any client of the group: how long to wait before signing in again.
async fn wait_before_signing_in(limits: &impl Limits) -> Option<Duration> {
    let error = limits.sign_in().await.err()?;
    let seconds = error.headers().get(RETRY_AFTER)?.to_str().ok()?;

    seconds.parse().ok().map(Duration::from_secs)
}

#[tokio::test]
async fn a_fake_builds_an_error_with_its_retry_after() {
    assert_eq!(
        wait_before_signing_in(&Fake).await,
        Some(Duration::from_secs(2))
    );
}

#[test]
fn an_error_keeps_its_headers_into_another_shape_and_back() {
    #[derive(Debug, serde::Deserialize)]
    struct Shape {
        message: String,
    }

    impl std::fmt::Display for Shape {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str(&self.message)
        }
    }

    let shaped: Error<Shape> = too_many().into_shape();

    assert_eq!(shaped.headers()[RETRY_AFTER], "2");

    let answered = Error::answered(
        StatusCode::TOO_MANY_REQUESTS,
        Shape {
            message: "slow down".into(),
        },
    )
    .retry_after(Duration::from_millis(2500));
    let response = answered.response().expect("an answer");

    assert_eq!(response.headers()[RETRY_AFTER], "3");
    assert_eq!(answered.headers()[RETRY_AFTER], "3");

    let carried: Error = answered.into();

    assert_eq!(carried.headers()[RETRY_AFTER], "3");
}

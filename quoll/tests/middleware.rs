//! The client's middleware beside authentication, `on_request`, `bearer_from`, `user_agent`, `request_id`, `retry`,
//! `timeout` and `trace`, each against a contract served on a free port and called by the real client.

#![cfg(all(feature = "server", feature = "client"))]

use std::error::Error as _;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::{StreamExt, stream};
use http::Extensions;
use quoll::client::{Attempt, Retry, TokenSource};
use quoll::rest::{self, Authenticate, Bearer, ByteStream, Error, Events, Result, Route, Text};
use reqwest::StatusCode;
use reqwest_middleware::{ClientBuilder, ClientWithMiddleware, Middleware, Next};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use tokio::sync::Notify;

mod support {
    pub(crate) mod log;
}

/// What the server saw of a request's headers.
#[derive(Debug, Default, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Seen {
    agent: Option<String>,
    id: Option<String>,
    signature: Option<String>,
}

rest::api! {
    api Api = "/api";

    group Calls {
        GET    "/seen"            => seen(
                                         header("User-Agent") agent: Option<String>,
                                         header("X-Request-Id") id: Option<String>,
                                         header("X-Signature") signature: Option<String>,
                                     ) -> Seen;
        GET    "/held"            => held();
        GET    "/events"          => events() -> Events<u32>;
        GET    "/output"          => output() -> ByteStream;
        GET    "/flaky/{status}"  => get(status: u16) -> u32;
        HEAD   "/flaky/{status}"  => head(status: u16);
        PUT    "/flaky/{status}"  => put(status: u16, text: Text) -> u32;
        DELETE "/flaky/{status}"  => delete(status: u16) -> u32;
        POST   "/flaky/{status}"  => post(status: u16, text: Text) -> u32;
        POST   "/flaky/{status}/keyed" => post_keyed(
                                         status: u16,
                                         header("Idempotency-Key") key: Option<String>,
                                         text: Text,
                                     ) -> u32;
    }

    #[auth(Bearer)]
    group Secure {
        GET  "/me" => me() -> String;
        POST "/me" => post_me(text: Text) -> String;
    }

    /// A key in the query, which a URL shows.
    scheme Token = query "token";

    #[auth(Token)]
    group Keyed {
        GET "/keyed" => keyed() -> String;
    }
}

/// The service: `flaky` routes fail with their status until `failures` attempts have been made, `held` waits for
/// ever, `events` sends 1, then 2 once `release` is notified, and `Secure` takes the token `fresh` alone.
#[derive(Clone, Default)]
struct App {
    failures: usize,
    attempts: Arc<AtomicUsize>,
    release: Arc<Notify>,
    tokens: Arc<Mutex<Vec<String>>>,
}

impl App {
    fn flaky(&self, status: u16) -> Result<u32> {
        let attempt = self.attempts.fetch_add(1, Ordering::SeqCst);

        if attempt < self.failures {
            let status = StatusCode::from_u16(status).expect("a status");

            return Err(Error::new(status, "not yet"));
        }

        Ok(u32::try_from(attempt + 1).expect("few attempts"))
    }
}

impl CallsServer for App {
    async fn seen(
        &self,
        agent: Option<String>,
        id: Option<String>,
        signature: Option<String>,
    ) -> Result<Seen> {
        Ok(Seen {
            agent,
            id,
            signature,
        })
    }

    async fn held(&self) -> Result<()> {
        std::future::pending().await
    }

    async fn events(&self) -> Result<Events<u32>> {
        let release = Arc::clone(&self.release);
        let second = stream::once(async move {
            release.notified().await;
            2
        });

        Ok(Events::new(stream::once(async { 1 }).chain(second)))
    }

    async fn output(&self) -> Result<ByteStream> {
        let release = Arc::clone(&self.release);
        let second = stream::once(async move {
            release.notified().await;
            b"2".to_vec()
        });

        Ok(ByteStream::new(
            stream::once(async { b"1".to_vec() }).chain(second),
        ))
    }

    async fn get(&self, status: u16) -> Result<u32> {
        self.flaky(status)
    }

    async fn head(&self, status: u16) -> Result<()> {
        self.flaky(status).map(|_| ())
    }

    async fn put(&self, status: u16, _: Text) -> Result<u32> {
        self.flaky(status)
    }

    async fn delete(&self, status: u16) -> Result<u32> {
        self.flaky(status)
    }

    async fn post(&self, status: u16, _: Text) -> Result<u32> {
        self.flaky(status)
    }

    async fn post_keyed(&self, status: u16, _: Option<String>, _: Text) -> Result<u32> {
        self.flaky(status)
    }
}

impl Authenticate<Bearer> for App {
    type Caller = ();

    async fn authenticate(&self, token: String) -> Result<()> {
        let valid = token == "fresh";

        self.tokens
            .lock()
            .expect("no test panics while holding it")
            .push(token);
        valid
            .then_some(())
            .ok_or_else(|| Error::unauthorized("the token has expired"))
    }
}

impl SecureServer for App {
    async fn me(&self, _: &()) -> Result<String> {
        Ok("ana".into())
    }

    async fn post_me(&self, _: &(), text: Text) -> Result<String> {
        Ok(text.0)
    }
}

impl Authenticate<Token> for App {
    type Caller = ();

    async fn authenticate(&self, _: String) -> Result<()> {
        Ok(())
    }
}

impl KeyedServer for App {
    async fn keyed(&self, _: &()) -> Result<String> {
        Ok("keyed".into())
    }
}

/// The api served on a port, with `app` as its service: its URL.
async fn serve(app: &App) -> String {
    quoll::test::spawn(Api::router(ApiServices {
        calls: app.clone(),
        secure: app.clone(),
        keyed: app.clone(),
    }))
    .await
}

/// A client of the api at `url` through `http`.
fn client(http: ClientWithMiddleware, url: &str) -> ApiClient {
    Api::client_with(http, url).expect("the url is the api's")
}

/// What a middleware's call returns.
type Handled<'a> =
    Pin<Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
fn middleware<F>(handle: F) -> F
where
    F: for<'a> Fn(reqwest::Request, &'a mut Extensions, Next<'a>) -> Handled<'a>
        + Send
        + Sync
        + 'static,
{
    handle
}

/// A middleware that keeps the `X-Request-Id` of each request it sees.
fn ids(seen: Arc<Mutex<Vec<Option<String>>>>) -> impl Middleware {
    middleware(move |request, extensions, next| {
        let id = request
            .headers()
            .get("x-request-id")
            .map(|id| id.to_str().expect("an id is text").to_owned());

        seen.lock()
            .expect("no test panics while holding it")
            .push(id);
        Box::pin(next.run(request, extensions))
    })
}

/// Whether `error`, or an error it comes from, is reqwest's for a deadline passed.
fn timed_out(error: &Error) -> bool {
    let mut source = error.source();

    while let Some(error) = source {
        let reqwest = match error.downcast_ref::<reqwest_middleware::Error>() {
            Some(reqwest_middleware::Error::Reqwest(error)) => Some(error),
            _ => error.downcast_ref::<reqwest::Error>(),
        };

        if reqwest.is_some_and(reqwest::Error::is_timeout) {
            return true;
        }

        source = error.source();
    }

    false
}

#[tokio::test]
async fn on_request_changes_the_requests_of_a_contract_only() -> Result<()> {
    let app = App::default();
    let url = serve(&app).await;
    let routes: Arc<Mutex<Vec<&'static str>>> = Arc::default();
    let sign = {
        let routes = Arc::clone(&routes);

        quoll::client::on_request(move |request: &mut reqwest::Request, route: &Route| {
            let signature = format!("{} {}", route.operation_id(), request.url().path());

            routes
                .lock()
                .expect("no test panics while holding it")
                .push(route.operation_id());
            request.headers_mut().insert(
                "x-signature",
                signature.try_into().expect("a header's value"),
            );
        })
    };
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(sign)
        .build();

    let seen = client(http.clone(), &url)
        .calls()
        .seen(None, None, None)
        .await?;

    assert_eq!(seen.signature.as_deref(), Some("calls_seen /api/seen"));

    // The same client, sending a request that no contract made.
    let other: Seen = http
        .get(format!("{url}/api/seen"))
        .send()
        .await
        .expect("the server answers")
        .json()
        .await
        .expect("a seen");

    assert_eq!(other.signature, None);
    assert_eq!(
        *routes.lock().expect("no test panics while holding it"),
        ["calls_seen"]
    );
    Ok(())
}

#[tokio::test]
async fn user_agent_is_sent_unless_the_request_has_one() -> Result<()> {
    let url = serve(&App::default()).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::user_agent("notes-cli/0.1"))
        .build();
    let api = client(http, &url);

    let seen = api.calls().seen(None, None, None).await?;

    assert_eq!(seen.agent.as_deref(), Some("notes-cli/0.1"));

    let own = api.calls().seen(Some("mine/1".into()), None, None).await?;

    assert_eq!(own.agent.as_deref(), Some("mine/1"));

    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::user_agent("notes\ncli"))
        .build();
    let error = client(http, &url)
        .calls()
        .seen(None, None, None)
        .await
        .expect_err("a name with a line break");

    assert_eq!(error.status(), None);
    Ok(())
}

#[tokio::test]
async fn request_id_is_new_on_each_call_and_kept_on_its_attempts() -> Result<()> {
    let app = App {
        failures: 1,
        ..App::default()
    };
    let url = serve(&app).await;
    let attempts: Arc<Mutex<Vec<Option<String>>>> = Arc::default();
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::request_id())
        .with(quoll::client::retry(2))
        .with(ids(Arc::clone(&attempts)))
        .build();
    let api = client(http, &url);

    let first = api.calls().seen(None, None, None).await?.id.expect("an id");
    let second = api.calls().seen(None, None, None).await?.id.expect("an id");

    assert_ne!(first, second);
    for id in [&first, &second] {
        assert_eq!(id.len(), 36, "{id}");
        assert_eq!(id.matches('-').count(), 4, "{id}");
    }

    let own = api.calls().seen(None, Some("call-7".into()), None).await?;

    assert_eq!(own.id.as_deref(), Some("call-7"));

    // One call, failed once then tried again: both attempts carry one id.
    attempts
        .lock()
        .expect("no test panics while holding it")
        .clear();
    assert_eq!(api.calls().get(503).await?, 2);

    let attempts = attempts
        .lock()
        .expect("no test panics while holding it")
        .clone();

    assert_eq!(attempts.len(), 2);
    assert!(attempts[0].is_some());
    assert_eq!(attempts[0], attempts[1]);
    Ok(())
}

/// A client through `retry(times)`, of a server whose `flaky` routes fail `failures` times: the client and the
/// service.
async fn flaky(failures: usize, times: u32) -> (ApiClient, App) {
    flaky_with(failures, quoll::client::retry(times)).await
}

/// A client through `retry`, of a server whose `flaky` routes fail `failures` times: the client and the service.
async fn flaky_with(failures: usize, retry: quoll::client::Retrying) -> (ApiClient, App) {
    let app = App {
        failures,
        ..App::default()
    };
    let url = serve(&app).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(retry)
        .build();

    (client(http, &url), app)
}

#[tokio::test]
async fn retry_tries_again_after_502_503_and_504() -> Result<()> {
    for status in [502, 503, 504] {
        let (api, app) = flaky(2, 3).await;

        assert_eq!(api.calls().get(status).await?, 3, "{status}");
        assert_eq!(app.attempts.load(Ordering::SeqCst), 3, "{status}");
    }
    Ok(())
}

#[tokio::test]
async fn retry_gives_up_after_its_times() {
    let (api, app) = flaky(10, 2).await;

    let error = api.calls().get(503).await.expect_err("always 503");

    assert_eq!(error.status(), Some(StatusCode::SERVICE_UNAVAILABLE));
    assert_eq!(app.attempts.load(Ordering::SeqCst), 3);
}

#[tokio::test]
async fn retry_leaves_other_failures_alone() {
    for status in [500, 429, 404] {
        let (api, app) = flaky(1, 3).await;

        let error = api.calls().get(status).await.expect_err("a failure");

        assert_eq!(error.status().map(|status| status.as_u16()), Some(status));
        assert_eq!(app.attempts.load(Ordering::SeqCst), 1, "{status}");
    }
}

#[tokio::test]
async fn retry_tries_again_get_head_put_and_delete_but_not_post() -> Result<()> {
    let (api, app) = flaky(1, 3).await;

    api.calls().head(503).await?;
    assert_eq!(app.attempts.load(Ordering::SeqCst), 2, "HEAD");

    let (api, app) = flaky(1, 3).await;

    assert_eq!(api.calls().put(503, Text("a body".into())).await?, 2);
    assert_eq!(app.attempts.load(Ordering::SeqCst), 2, "PUT");

    let (api, app) = flaky(1, 3).await;

    assert_eq!(api.calls().delete(503).await?, 2);
    assert_eq!(app.attempts.load(Ordering::SeqCst), 2, "DELETE");

    let (api, app) = flaky(1, 3).await;
    let error = api
        .calls()
        .post(503, Text("a body".into()))
        .await
        .expect_err("a POST is sent once");

    assert_eq!(error.status(), Some(StatusCode::SERVICE_UNAVAILABLE));
    assert_eq!(app.attempts.load(Ordering::SeqCst), 1, "POST");
    Ok(())
}

#[tokio::test]
async fn retry_when_tries_again_a_post_that_carries_an_idempotency_key() -> Result<()> {
    let keyed = || {
        quoll::client::retry(3).when(|attempt: &Attempt| {
            let request = attempt.request();
            let keyed = request.method() == http::Method::POST
                && request.headers().contains_key("Idempotency-Key");

            match attempt.status() {
                Some(status) if keyed && status.is_server_error() => Retry::Backoff,
                _ => attempt.transient(),
            }
        })
    };
    let (api, app) = flaky_with(2, keyed()).await;

    let attempts = api
        .calls()
        .post_keyed(500, Some("k-1".into()), Text("a body".into()))
        .await?;

    assert_eq!(attempts, 3);
    assert_eq!(app.attempts.load(Ordering::SeqCst), 3);

    let (api, app) = flaky_with(2, keyed()).await;
    let error = api
        .calls()
        .post_keyed(500, None, Text("a body".into()))
        .await
        .expect_err("a POST with no key is sent once");

    assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    assert_eq!(app.attempts.load(Ordering::SeqCst), 1);

    // The rule of `transient` still holds where the closure leaves it.
    let (api, app) = flaky_with(1, keyed()).await;

    assert_eq!(api.calls().get(503).await?, 2);
    assert_eq!(app.attempts.load(Ordering::SeqCst), 2);
    Ok(())
}

/// What the closure of `retry(..).when` is given of an attempt: its number, its operation and its status.
type Given = (u32, Option<&'static str>, Option<u16>);

#[tokio::test]
async fn retry_when_is_given_each_attempt_its_route_its_number_and_its_answer() -> Result<()> {
    let seen: Arc<Mutex<Vec<Given>>> = Arc::default();
    let retry = {
        let seen = Arc::clone(&seen);

        quoll::client::retry(5).when(move |attempt: &Attempt| {
            seen.lock().expect("no test panics while holding it").push((
                attempt.number(),
                attempt.route().map(Route::operation_id),
                attempt.status().map(|status| status.as_u16()),
            ));

            if attempt.number() < 2 {
                Retry::Backoff
            } else {
                Retry::No
            }
        })
    };
    let (api, app) = flaky_with(10, retry).await;

    let error = api.calls().get(503).await.expect_err("always 503");

    assert_eq!(error.status(), Some(StatusCode::SERVICE_UNAVAILABLE));
    assert_eq!(app.attempts.load(Ordering::SeqCst), 2);
    assert_eq!(
        *seen.lock().expect("no test panics while holding it"),
        [
            (1, Some("calls_get"), Some(503)),
            (2, Some("calls_get"), Some(503))
        ]
    );
    Ok(())
}

#[tokio::test]
async fn retry_when_is_given_the_error_of_an_attempt_with_no_answer() {
    let url = closes_each_connection(Arc::default()).await;
    let errors = Arc::new(AtomicUsize::new(0));
    let retry = {
        let errors = Arc::clone(&errors);

        quoll::client::retry(1).when(move |attempt: &Attempt| {
            assert_eq!(attempt.status(), None);
            assert!(attempt.headers().is_none());

            if attempt.error().is_some() {
                errors.fetch_add(1, Ordering::SeqCst);
            }

            attempt.transient()
        })
    };
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(retry)
        .build();

    let error = client(http, &url)
        .calls()
        .get(200)
        .await
        .expect_err("no server");

    assert_eq!(error.status(), None);
    assert_eq!(errors.load(Ordering::SeqCst), 2);
}

/// A middleware that answers its first request itself, 429 with `Retry-After: <value>`, and sends the others on.
fn too_many_once(value: &'static str) -> impl Middleware {
    let answered = AtomicUsize::new(0);

    middleware(move |request, extensions, next| {
        if answered.fetch_add(1, Ordering::SeqCst) > 0 {
            return Box::pin(next.run(request, extensions));
        }

        let answer = http::Response::builder()
            .status(StatusCode::TOO_MANY_REQUESTS)
            .header(http::header::RETRY_AFTER, value)
            .body("")
            .expect("an answer");

        Box::pin(async move { Ok(reqwest::Response::from(answer)) })
    })
}

/// The `retry` of a 429 waited out as its `Retry-After` says.
fn after_429() -> quoll::client::Retrying {
    quoll::client::retry(2).when(|attempt: &Attempt| match attempt.status() {
        Some(StatusCode::TOO_MANY_REQUESTS) => attempt.retry_after(),
        _ => attempt.transient(),
    })
}

#[tokio::test]
async fn retry_after_waits_as_long_as_the_server_says() -> Result<()> {
    let url = serve(&App::default()).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(after_429())
        .with(too_many_once("1"))
        .build();
    let started = Instant::now();

    assert_eq!(client(http, &url).calls().get(200).await?, 1);

    let waited = started.elapsed();

    // The backoff alone waits 100 ms at most before the second attempt.
    assert!(waited >= Duration::from_secs(1), "{waited:?}");
    Ok(())
}

#[tokio::test]
async fn retry_after_waits_no_longer_than_max_wait() -> Result<()> {
    let url = serve(&App::default()).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(after_429().max_wait(Duration::from_millis(300)))
        .with(too_many_once("3600"))
        .build();
    let started = Instant::now();

    let called = tokio::time::timeout(Duration::from_secs(30), client(http, &url).calls().get(200))
        .await
        .expect("a wait of 300 ms, not of an hour");

    assert_eq!(called?, 1);

    let waited = started.elapsed();

    assert!(waited >= Duration::from_millis(300), "{waited:?}");
    Ok(())
}

/// A server that closes each connection it accepts, before any answer, counting them in `accepted`: its URL.
async fn closes_each_connection(accepted: Arc<AtomicUsize>) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a free port");
    let url = format!("http://{}", listener.local_addr().expect("an address"));

    tokio::spawn(async move {
        loop {
            let (connection, _) = listener.accept().await.expect("a connection");

            accepted.fetch_add(1, Ordering::SeqCst);
            drop(connection);
        }
    });

    url
}

#[tokio::test]
async fn retry_tries_again_a_call_that_got_no_response() {
    let accepted = Arc::new(AtomicUsize::new(0));
    let url = closes_each_connection(Arc::clone(&accepted)).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::retry(2))
        .build();

    let error = client(http, &url)
        .calls()
        .get(200)
        .await
        .expect_err("no answer");

    assert_eq!(error.status(), None);
    assert_eq!(accepted.load(Ordering::SeqCst), 3);
}

#[tokio::test]
async fn timeout_ends_a_call_that_takes_longer() {
    let url = serve(&App::default()).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::timeout(Duration::from_millis(200)))
        .build();

    let error = client(http, &url)
        .calls()
        .held()
        .await
        .expect_err("a call past its deadline");

    assert_eq!(error.status(), None);
    assert!(timed_out(&error), "{error:?}");
}

#[tokio::test]
async fn timeout_leaves_a_stream_that_outlives_it() -> Result<()> {
    let app = App::default();
    let url = serve(&app).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::timeout(Duration::from_millis(200)))
        .build();
    let api = client(http, &url);

    let mut events = api.calls().events().await?;

    assert_eq!(events.next().await.expect("an event")?, 1);

    // A call started after the stream has passed its deadline: so has the stream.
    let error = api.calls().held().await.expect_err("past its deadline");

    assert!(timed_out(&error), "{error:?}");

    app.release.notify_one();
    assert_eq!(events.next().await.expect("an event")?, 2);
    assert!(events.next().await.is_none());

    let mut output = api.calls().output().await?;

    assert_eq!(output.next().await.expect("a chunk")?, b"1");

    let error = api.calls().held().await.expect_err("past its deadline");

    assert!(timed_out(&error), "{error:?}");

    app.release.notify_one();
    assert_eq!(output.next().await.expect("a chunk")?, b"2");
    assert!(output.next().await.is_none());
    Ok(())
}

/// A server that accepts each connection and never answers on it, holding it open: its URL, and how many it
/// accepted.
async fn silent() -> (String, Arc<AtomicUsize>) {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a free port");
    let url = format!("http://{}", listener.local_addr().expect("an address"));
    let accepted = Arc::new(AtomicUsize::new(0));

    {
        let accepted = Arc::clone(&accepted);

        tokio::spawn(async move {
            let mut held = Vec::new();

            loop {
                let (connection, _) = listener.accept().await.expect("a connection");

                accepted.fetch_add(1, Ordering::SeqCst);
                held.push(connection);
            }
        });
    }

    (url, accepted)
}

#[tokio::test]
async fn timeout_bounds_the_wait_for_a_streams_head_which_retry_tries_again() {
    let (url, accepted) = silent().await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::retry(2))
        .with(quoll::client::timeout(Duration::from_millis(100)))
        .build();

    // A guard that fails the test rather than hang it, were the head never bounded.
    let called = tokio::time::timeout(Duration::from_secs(10), client(http, &url).calls().events())
        .await
        .expect("the deadline ends the call");
    let Err(error) = called else {
        panic!("a server that never answers gave events");
    };

    assert_eq!(error.status(), None);
    assert!(format!("{error:?}").contains("deadline"), "{error:?}");
    assert_eq!(accepted.load(Ordering::SeqCst), 3);
}

#[tokio::test]
async fn timeout_bounds_the_wait_for_the_head_of_a_byte_stream_which_retry_tries_again() {
    let (url, accepted) = silent().await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::retry(2))
        .with(quoll::client::timeout(Duration::from_millis(100)))
        .build();

    // A guard that fails the test rather than hang it, were the head never bounded.
    let called = tokio::time::timeout(Duration::from_secs(10), client(http, &url).calls().output())
        .await
        .expect("the deadline ends the call");
    let Err(error) = called else {
        panic!("a server that never answers gave bytes");
    };

    assert_eq!(error.status(), None);
    assert!(format!("{error:?}").contains("deadline"), "{error:?}");
    assert_eq!(accepted.load(Ordering::SeqCst), 3);
}

#[tokio::test]
async fn trace_opens_a_span_named_by_the_operation() -> Result<()> {
    let (log, _capture) = support::log::capture(true);
    let url = serve(&App::default()).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::trace())
        .build();

    client(http.clone(), &url)
        .calls()
        .seen(None, None, None)
        .await?;
    http.get(format!("{url}/api/seen"))
        .send()
        .await
        .expect("the server answers");

    let log = log.text();
    let spans: Vec<&str> = log
        .lines()
        .filter(|line| line.contains("HTTP request"))
        .collect();

    assert_eq!(spans.len(), 2, "{log}");
    for field in [
        "otel.name=calls_seen",
        "operation_id=\"calls_seen\"",
        "group=\"Calls\"",
        "http.response.status_code=200",
    ] {
        assert!(spans[0].contains(field), "{field} in {}", spans[0]);
    }
    assert!(spans[1].contains("otel.name=GET"), "{}", spans[1]);
    Ok(())
}

/// A URL where nothing answers: a port kept bound, whose every connection is closed at once, with no response. A port
/// freed instead could be taken by another test, which would answer.
async fn dead_url() -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a free port");
    let address = listener.local_addr().expect("an address");

    tokio::spawn(async move {
        while let Ok((connection, _)) = listener.accept().await {
            drop(connection);
        }
    });

    format!("http://{address}")
}

#[tokio::test]
async fn trace_shows_no_query_of_a_call_that_got_no_answer() {
    let (log, _capture) = support::log::capture(true);
    let url = dead_url().await;

    for retried in [false, true] {
        let http = ClientBuilder::new(reqwest::Client::new()).with(quoll::client::trace());
        let http = if retried {
            http.with(quoll::client::retry(1))
        } else {
            http
        };
        let http = http.with(quoll::client::key::<Token>("secret-key")).build();

        let error = client(http, &url)
            .keyed()
            .keyed()
            .await
            .expect_err("a call that got no response");

        assert_eq!(error.status(), None);
    }

    let log = log.text();
    let spans: Vec<&str> = log
        .lines()
        .filter(|line| line.contains("HTTP request"))
        .collect();

    assert_eq!(spans.len(), 2, "{log}");
    for span in spans {
        assert!(span.contains("error.message="), "{span}");
        assert!(span.contains("/api/keyed"), "{span}");
        assert!(!span.contains("secret-key"), "{span}");
    }
}

/// A source that gives `stale` until it is refreshed, then `fresh`, or `stale` again when `renews` is false; it keeps
/// what it was asked. Its clones share all of it.
#[derive(Clone)]
struct Source {
    token: Arc<tokio::sync::Mutex<String>>,
    renews: bool,
    asked: Arc<AtomicUsize>,
    refused: Arc<Mutex<Vec<String>>>,
    renewed: Arc<AtomicUsize>,
}

impl Source {
    fn new(renews: bool) -> Self {
        Self {
            token: Arc::new(tokio::sync::Mutex::new("stale".into())),
            renews,
            asked: Arc::default(),
            refused: Arc::default(),
            renewed: Arc::default(),
        }
    }
}

impl TokenSource for Source {
    async fn token(&self) -> Result<String> {
        self.asked.fetch_add(1, Ordering::SeqCst);

        Ok(self.token.lock().await.clone())
    }

    async fn refresh(&self, refused: &str) -> Result<String> {
        self.refused
            .lock()
            .expect("no test panics while holding it")
            .push(refused.to_owned());

        let mut token = self.token.lock().await;

        if *token != refused || !self.renews {
            return Ok(token.clone());
        }

        self.renewed.fetch_add(1, Ordering::SeqCst);
        "fresh".clone_into(&mut token);
        Ok(token.clone())
    }
}

/// A client of the api served with `app`, whose bearer tokens come from `source`.
async fn from_source(app: &App, source: &Source) -> ApiClient {
    let url = serve(app).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::bearer_from(source.clone()))
        .build();

    client(http, &url)
}

/// The tokens the server was sent.
fn tokens(app: &App) -> Vec<String> {
    app.tokens
        .lock()
        .expect("no test panics while holding it")
        .clone()
}

#[tokio::test]
async fn bearer_from_refreshes_the_token_once_after_a_401() -> Result<()> {
    let app = App::default();
    let source = Source::new(true);
    let api = from_source(&app, &source).await;

    assert_eq!(api.secure().me().await?, "ana");
    assert_eq!(tokens(&app), ["stale", "fresh"]);
    assert_eq!(*source.refused.lock().expect("no test panics"), ["stale"]);

    // The body is sent again with the new token.
    assert_eq!(api.secure().post_me(Text("a body".into())).await?, "a body");
    assert_eq!(source.asked.load(Ordering::SeqCst), 2);
    Ok(())
}

#[tokio::test]
async fn bearer_from_answers_the_401_when_the_source_has_no_better_token() {
    let app = App::default();
    let source = Source::new(false);
    let api = from_source(&app, &source).await;

    let error = api.secure().me().await.expect_err("an expired token");

    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    assert_eq!(tokens(&app), ["stale"]);
}

#[tokio::test]
async fn bearer_from_lets_two_calls_refused_at_once_renew_the_token_once() -> Result<()> {
    let app = App::default();
    let source = Source::new(true);
    let api = from_source(&app, &source).await;

    let secure = api.secure();
    let (first, second) = tokio::join!(secure.me(), secure.me());

    assert_eq!((first?, second?), ("ana".into(), "ana".into()));
    assert_eq!(
        *source.refused.lock().expect("no test panics"),
        ["stale", "stale"]
    );
    assert_eq!(source.renewed.load(Ordering::SeqCst), 1);
    Ok(())
}

#[tokio::test]
async fn bearer_from_asks_nothing_for_a_route_without_bearer() -> Result<()> {
    let app = App::default();
    let source = Source::new(true);
    let api = from_source(&app, &source).await;

    api.calls().seen(None, None, None).await?;

    assert_eq!(source.asked.load(Ordering::SeqCst), 0);
    Ok(())
}

/// A source whose token cannot be sent.
struct Broken;

impl TokenSource for Broken {
    async fn token(&self) -> Result<String> {
        Ok("secret\nline".into())
    }

    async fn refresh(&self, refused: &str) -> Result<String> {
        Ok(refused.to_owned())
    }
}

#[tokio::test]
async fn bearer_from_fails_a_token_that_is_not_a_b64token_without_showing_it() {
    let app = App::default();
    let url = serve(&app).await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::bearer_from(Broken))
        .build();

    let error = client(http, &url)
        .secure()
        .me()
        .await
        .expect_err("a token with a line break");

    assert_eq!(error.status(), None);
    assert!(!format!("{error:?}").contains("secret"), "{error:?}");
    assert!(tokens(&app).is_empty());
}

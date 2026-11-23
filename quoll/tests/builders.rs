//! `quoll::client::Builder` and `quoll::server::Builder`: the helpers of quoll given in the wrong order work as in the
//! right one, any other keeps its place, and a server serves on a port until it is told to stop.

#![cfg(all(feature = "server", feature = "client"))]

use std::cell::Cell;
use std::future::Future;
use std::net::SocketAddr;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::extract::Request;
use axum::middleware::{Next, from_fn};
use axum::response::IntoResponse;
use quoll::{Error, JsonLines, Response, Result, client, server};
use reqwest::StatusCode;
use reqwest::header::{CONTENT_TYPE, HeaderValue, LOCATION};
use reqwest::redirect::Policy;
use tokio::net::TcpListener;
use tokio::sync::{Notify, oneshot};

use crate::support::log::Log;

mod support {
    pub(crate) mod log;
}

quoll::api! {
    api Api = "/api";

    group Calls {
        GET "/flaky"   => flaky(header("X-Request-Id") id: Option<String>) -> String;
        GET "/slow"    => slow() -> String;
        GET "/held"    => held() -> String;
        GET "/endless" => endless() -> JsonLines<u32>;
        GET "/away"    => away() -> String;
        POST "/text"   => text(text: quoll::Text);
    }
}

/// The deadline of `timeout` in these tests: short, as a route that waits for ever reaches it.
const DEADLINE: Duration = Duration::from_millis(100);

/// The body limit of `body_limit` in these tests.
const LIMIT: usize = 1024;

/// The origin a page of the tests of CORS is served from.
const ORIGIN: &str = "https://app.example.com";

/// The service: `flaky` answers 503 to its first two calls, keeping the id of each; `held` tells `entered` it has
/// begun, then answers once `release` is told.
#[derive(Clone, Default)]
struct App {
    ids: Arc<Mutex<Vec<String>>>,
    entered: Arc<Notify>,
    release: Arc<Notify>,
}

impl CallsServer for App {
    async fn flaky(&self, id: Option<String>) -> Result<Response<String>> {
        let mut ids = self.ids.lock().expect("no test panics with the lock");

        ids.push(id.unwrap_or_default());

        if ids.len() <= 2 {
            return Err(Error::new(StatusCode::SERVICE_UNAVAILABLE, "not yet"));
        }

        Ok(Response::new("served".into()))
    }

    async fn slow(&self) -> Result<Response<String>> {
        std::future::pending().await
    }

    async fn held(&self) -> Result<Response<String>> {
        self.entered.notify_one();
        self.release.notified().await;

        Ok(Response::new("released".into()))
    }

    async fn endless(&self) -> Result<Response<JsonLines<u32>>> {
        Ok(Response::new(JsonLines::new(
            futures_util::stream::pending(),
        )))
    }

    async fn away(&self) -> Result<Response<String>> {
        Err(Error::not_found(
            "the redirects are served by another router",
        ))
    }

    async fn text(&self, _text: quoll::Text) -> Result<Response<()>> {
        Ok(Response::new(()))
    }
}

/// The api's router, before any layer.
fn router(app: &App) -> axum::Router {
    Api::router(ApiServices { calls: app.clone() })
}

/// `builder` with a layer of the application's, which answers whether the request had an `X-Request-Id` when it saw
/// it, in `X-Saw-Id`.
fn saw_id(builder: server::Builder) -> server::Builder {
    builder.with(from_fn(|request: Request, next: Next| async move {
        let saw = if request.headers().contains_key("x-request-id") {
            "yes"
        } else {
            "no"
        };
        let mut response = next.run(request).await;

        response
            .headers_mut()
            .insert("x-saw-id", HeaderValue::from_static(saw));
        response
    }))
}

// The client.

#[tokio::test]
async fn request_id_given_after_retry_gives_every_attempt_one_id() -> Result<()> {
    let app = App::default();
    let url = quoll::test::spawn(router(&app)).await;
    let api = client::builder()
        .with(client::retry(2))
        .with(client::request_id())
        .build::<Api>(&url)?;

    assert_eq!(api.calls().flaky(None).await?, "served");

    let ids = app
        .ids
        .lock()
        .expect("no test panics with the lock")
        .clone();

    assert_eq!(ids.len(), 3, "{ids:?}");
    assert!(!ids[0].is_empty(), "{ids:?}");
    assert!(ids.iter().all(|id| *id == ids[0]), "{ids:?}");
    Ok(())
}

#[tokio::test]
async fn request_id_added_after_retry_by_hand_gives_each_attempt_its_own() -> Result<()> {
    let app = App::default();
    let url = quoll::test::spawn(router(&app)).await;
    let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
        .with(client::retry(2))
        .with(client::request_id())
        .build();

    Api::client_with(http, &url)?.calls().flaky(None).await?;

    let ids = app
        .ids
        .lock()
        .expect("no test panics with the lock")
        .clone();

    assert_eq!(ids.len(), 3, "{ids:?}");
    assert_ne!(ids[0], ids[1], "{ids:?}");
    Ok(())
}

/// An api at one origin whose `/api/away` redirects to `/landing` at another, which answers, in JSON, whether the
/// request carried `X-Signature`, `"signed"`, or the key `X-Api-Key`, `"keyed"`, or neither, `"clean"`: the URL of the
/// api.
async fn redirecting() -> String {
    let landing = axum::Router::new().route(
        "/landing",
        axum::routing::get(|request: Request| async move {
            let headers = request.headers();
            let body = if headers.contains_key("x-signature") {
                "\"signed\""
            } else if headers.contains_key("x-api-key") {
                "\"keyed\""
            } else {
                "\"clean\""
            };

            ([(CONTENT_TYPE, "application/json")], body)
        }),
    );
    let other = quoll::test::spawn(landing).await;
    let api = axum::Router::new().route(
        "/api/away",
        axum::routing::get(move || async move {
            let location = format!("{other}/landing");

            (StatusCode::FOUND, [(LOCATION, location)]).into_response()
        }),
    );

    quoll::test::spawn(api).await
}

/// Signs each request of the contract with a header that is not marked sensitive, which a hop to another origin would
/// keep, had the signature been added before `redirects`.
fn sign() -> impl reqwest_middleware::Middleware {
    client::on_request(|request: &mut reqwest::Request, _: &quoll::Route| {
        request
            .headers_mut()
            .insert("x-signature", HeaderValue::from_static("s-1"));
    })
}

#[tokio::test]
async fn a_credential_given_before_redirects_is_not_sent_to_another_origin() -> Result<()> {
    let url = redirecting().await;
    let api = client::builder()
        .with(sign())
        .with(client::redirects(5))
        .build::<Api>(&url)?;

    assert_eq!(api.calls().away().await?, "clean");
    Ok(())
}

/// A middleware of the application's that keeps, for each request it sees, whether it carries the signature of
/// [`sign`].
fn saw_signature(signed: Arc<Mutex<Vec<bool>>>) -> impl reqwest_middleware::Middleware {
    middleware(move |request, extensions, next| {
        signed
            .lock()
            .expect("no test panics with the lock")
            .push(request.headers().contains_key("x-signature"));

        next.run(request, extensions)
    })
}

/// What a middleware's call returns.
type Handled<'a> =
    Pin<Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
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

#[tokio::test]
async fn a_middleware_given_after_a_credential_comes_after_it_with_what_the_builder_adds()
-> Result<()> {
    let url = quoll::test::spawn(router(&App::default())).await;
    let signed = Arc::new(Mutex::new(Vec::new()));
    // Given a credential and neither `redirects` nor `timeout`, the builder adds both of its own.
    let api = client::builder()
        .with(sign())
        .with(saw_signature(Arc::clone(&signed)))
        .build::<Api>(&url)?;

    api.calls().text("milk".to_owned().into()).await?;

    assert_eq!(
        *signed.lock().expect("no test panics with the lock"),
        [true]
    );
    Ok(())
}

#[tokio::test]
async fn a_credential_added_before_redirects_by_hand_goes_to_another_origin() -> Result<()> {
    let url = redirecting().await;
    let http = reqwest::Client::builder()
        .redirect(Policy::none())
        .build()
        .expect("a client builds");
    let http = reqwest_middleware::ClientBuilder::new(http)
        .with(sign())
        .with(client::redirects(5))
        .build();

    assert_eq!(
        Api::client_with(http, &url)?.calls().away().await?,
        "signed"
    );
    Ok(())
}

#[tokio::test]
async fn with_redirects_the_reqwest_client_follows_none_whatever_it_is_set_to() -> Result<()> {
    let url = redirecting().await;
    let api = client::builder()
        .http(|http| http.redirect(Policy::limited(10)))
        .with(client::redirects(5))
        .with(sign())
        .build::<Api>(&url)?;

    // Followed by reqwest, below every middleware, the redirect would carry the signature to the other origin.
    assert_eq!(api.calls().away().await?, "clean");
    Ok(())
}

/// A contract whose route takes the key of a header scheme, which reqwest's own policy keeps on a redirect to another
/// origin.
mod keyed {
    quoll::api! {
        pub api Api = "/api";

        pub scheme Key = header "X-Api-Key";

        #[auth(Key)]
        pub group Keyed {
            GET "/away" => away() -> String;
        }
    }
}

#[tokio::test]
async fn a_credential_given_with_no_redirects_is_not_sent_to_another_origin() -> Result<()> {
    use keyed::Keyed as _;

    let url = redirecting().await;
    let api = client::builder()
        .with(client::key::<keyed::Key>("k-1"))
        .build::<keyed::Api>(&url)?;

    // The redirect is followed, by the `redirects` the builder adds, which gives the hop no key.
    assert_eq!(api.keyed().away().await?, "clean");
    Ok(())
}

#[tokio::test]
async fn a_credential_with_no_redirects_by_hand_goes_to_another_origin() -> Result<()> {
    use keyed::Keyed as _;

    let url = redirecting().await;
    let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
        .with(client::key::<keyed::Key>("k-1"))
        .build();

    // reqwest follows the redirect below every middleware, with the headers of the first request.
    assert_eq!(
        keyed::Api::client_with(http, &url)?.keyed().away().await?,
        "keyed"
    );
    Ok(())
}

#[tokio::test]
async fn the_http_client_of_a_builder_serves_the_clients_of_a_contract() -> Result<()> {
    let app = App::default();
    let url = quoll::test::spawn(router(&app)).await;
    let http = client::builder()
        .with(client::request_id())
        .with(client::retry(2))
        .build_http()?;

    assert_eq!(
        Api::client_with(http, &url)?.calls().flaky(None).await?,
        "served"
    );
    Ok(())
}

// The server.

/// `GET path` from a page of [`ORIGIN`], with no request id: the answer.
async fn get_from_page(url: &str, path: &str) -> reqwest::Response {
    reqwest::Client::new()
        .get(format!("{url}{path}"))
        .header("origin", ORIGIN)
        .send()
        .await
        .expect("the server answers")
}

/// The lines quoll's layers logged.
fn quoll_lines(log: &Log) -> Vec<String> {
    log.text()
        .lines()
        .filter(|line| line.contains("quoll::"))
        .map(str::to_owned)
        .collect()
}

/// The layers of quoll, given from the innermost to the outermost: the reverse of the order they work in.
fn reversed() -> server::Builder {
    server::builder()
        .with(server::body_limit(LIMIT))
        .with(server::timeout(DEADLINE))
        .with(server::compression())
        .with(server::cors([ORIGIN]).expect("an origin"))
        .with(server::trace())
        .with(server::request_id())
}

#[tokio::test]
async fn the_layers_given_in_reverse_make_a_deadline_s_answer_as_in_order() {
    let (log, _capture) = support::log::capture(false);
    let url = quoll::test::spawn(reversed().into_router(router(&App::default()))).await;

    let answer = get_from_page(&url, "/api/slow").await;

    assert_eq!(answer.status(), StatusCode::SERVICE_UNAVAILABLE);
    assert!(answer.headers().contains_key("x-request-id"));
    assert_eq!(answer.headers()["access-control-allow-origin"], ORIGIN);

    let lines = quoll_lines(&log);
    let answered: Vec<&String> = lines
        .iter()
        .filter(|line| line.contains("quoll::server::trace: answered"))
        .collect();

    assert_eq!(answered.len(), 1, "{lines:#?}");
    assert!(answered[0].contains("status=503"), "{lines:#?}");
    assert!(answered[0].contains("request_id="), "{lines:#?}");
}

#[tokio::test]
async fn the_layers_given_in_reverse_make_a_large_body_s_answer_as_in_order() {
    let (log, _capture) = support::log::capture(false);
    let url = quoll::test::spawn(reversed().into_router(router(&App::default()))).await;

    let answer = reqwest::Client::new()
        .post(format!("{url}/api/text"))
        .header("origin", ORIGIN)
        .header(CONTENT_TYPE, "text/plain; charset=utf-8")
        .body("x".repeat(LIMIT + 1))
        .send()
        .await
        .expect("the server answers");

    assert_eq!(answer.status(), StatusCode::PAYLOAD_TOO_LARGE);
    assert!(answer.headers().contains_key("x-request-id"));
    assert_eq!(answer.headers()["access-control-allow-origin"], ORIGIN);

    let lines = quoll_lines(&log);

    assert!(
        lines
            .iter()
            .any(|line| line.contains("quoll::server::trace: answered status=413")),
        "{lines:#?}"
    );
}

#[tokio::test]
async fn the_layers_added_in_reverse_by_hand_leave_a_deadline_s_answer_bare() {
    // What the builder's order avoids: `Router::layer` runs the last layer added first, so `body_limit` is the
    // outermost here, and `timeout` outside `request_id` and `cors`.
    let router = router(&App::default())
        .layer(server::request_id())
        .layer(server::trace())
        .layer(server::cors([ORIGIN]).expect("an origin"))
        .layer(server::compression())
        .layer(server::timeout(DEADLINE))
        .layer(server::body_limit(LIMIT));
    let url = quoll::test::spawn(router).await;

    let answer = get_from_page(&url, "/api/slow").await;

    assert_eq!(answer.status(), StatusCode::SERVICE_UNAVAILABLE);
    assert!(!answer.headers().contains_key("x-request-id"));
    assert!(!answer.headers().contains_key("access-control-allow-origin"));
}

#[tokio::test]
async fn a_layer_of_the_application_s_keeps_its_place() {
    let cases = [
        (saw_id(server::builder()).with(server::request_id()), "no"),
        (saw_id(server::builder().with(server::request_id())), "yes"),
        // `request_id` takes the first place, which `timeout` was given at: the layer stays between them.
        (
            saw_id(server::builder().with(server::timeout(DEADLINE))).with(server::request_id()),
            "yes",
        ),
    ];

    for (index, (builder, saw)) in cases.into_iter().enumerate() {
        let url = quoll::test::spawn(builder.into_router(router(&App::default()))).await;
        let answer = reqwest::get(format!("{url}/api/away"))
            .await
            .expect("the server answers");

        assert_eq!(answer.headers()["x-saw-id"], saw, "case {index}");
    }
}

/// Serves the api with `builder` on a free port of 127.0.0.1, on a task: its URL, the sender that stops it, and the
/// task, which ends with what `serve_on` returned.
async fn served(
    builder: server::Builder,
    app: &App,
) -> (
    String,
    oneshot::Sender<()>,
    tokio::task::JoinHandle<std::io::Result<()>>,
) {
    let listener = TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port of 127.0.0.1 is free");
    let url = format!(
        "http://{}",
        listener
            .local_addr()
            .expect("a bound listener has an address")
    );
    let (stop, stopped) = oneshot::channel::<()>();
    let builder = builder.shutdown(async move {
        // The sender dropped is a stop too.
        let _ = stopped.await;
    });
    let serving = tokio::spawn(builder.serve_on(listener, router(app)));

    (url, stop, serving)
}

#[tokio::test]
async fn a_server_serves_until_it_is_told_to_stop() -> Result<()> {
    let (url, stop, serving) = served(
        server::builder().with(server::request_id()),
        &App::default(),
    )
    .await;
    let api = Api::client(&url)?;

    assert_eq!(
        api.calls().flaky(None).await.unwrap_err().status(),
        Some(StatusCode::SERVICE_UNAVAILABLE)
    );

    stop.send(()).expect("the server waits for its signal");
    serving
        .await
        .expect("the server's task does not panic")
        .expect("the server stops with no error");

    assert!(reqwest::get(format!("{url}/api/away")).await.is_err());
    Ok(())
}

#[tokio::test]
async fn a_server_told_to_stop_ends_the_requests_it_serves() -> Result<()> {
    let app = App::default();
    let (url, stop, serving) = served(server::builder(), &app).await;
    let api = Api::client(&url)?;
    let call = tokio::spawn(async move { api.calls().held().await });

    app.entered.notified().await;
    stop.send(()).expect("the server waits for its signal");
    app.release.notify_one();

    assert_eq!(
        call.await.expect("the call's task does not panic")?,
        "released"
    );
    serving
        .await
        .expect("the server's task does not panic")
        .expect("the server stops with no error");
    Ok(())
}

#[tokio::test]
async fn a_server_told_to_stop_waits_no_longer_than_its_grace_for_a_stream() -> Result<()> {
    let (log, _capture) = support::log::capture(false);
    let app = App::default();
    let (url, stop, serving) = served(server::builder().grace(DEADLINE), &app).await;
    let lines = Api::client(&url)?.calls().endless().await?;

    stop.send(()).expect("the server waits for its signal");

    // The lines never end, and the server does not end them: it returns once its grace has passed, well before this
    // bound.
    tokio::time::timeout(Duration::from_secs(10), serving)
        .await
        .expect("the server returns after its grace")
        .expect("the server's task does not panic")
        .expect("the server stops with no error");
    drop(lines);

    let lines = quoll_lines(&log);
    let warned: Vec<&String> = lines
        .iter()
        .filter(|line| line.contains(" WARN "))
        .collect();

    assert_eq!(warned.len(), 1, "{lines:#?}");
    assert!(warned[0].contains("the grace ran out"), "{lines:#?}");
    assert!(warned[0].contains("grace=100ms"), "{lines:#?}");
    Ok(())
}

#[tokio::test]
async fn a_server_binds_an_address() {
    let any_port: SocketAddr = "127.0.0.1:0".parse().expect("an address");

    for served in [
        server::builder()
            .shutdown(async {})
            .serve("127.0.0.1:0", router(&App::default()))
            .await,
        server::builder()
            .shutdown(async {})
            .serve(String::from("127.0.0.1:0"), router(&App::default()))
            .await,
        server::builder()
            .shutdown(async {})
            .serve(any_port, router(&App::default()))
            .await,
    ] {
        assert!(served.is_ok(), "{served:?}");
    }
}

#[tokio::test]
async fn a_server_on_an_address_bound_already_fails() {
    let taken = TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port of 127.0.0.1 is free");
    let address = taken.local_addr().expect("a bound listener has an address");

    let refused = server::builder()
        .shutdown(async {})
        .serve(address, router(&App::default()))
        .await;

    assert_eq!(
        refused.expect_err("the address is bound already").kind(),
        std::io::ErrorKind::AddrInUse
    );
}

#[test]
fn a_setting_of_the_http_client_need_not_be_sync() {
    fn shared<T: Send + Sync>(_: &T) {}

    // A `Cell` is `Send` but not `Sync`: the setting is called once, by value, and the builder stays both.
    let seconds = Cell::new(30);
    let built = client::builder()
        .http(move |http| http.pool_idle_timeout(Duration::from_secs(seconds.get())));

    shared(&built);
    assert!(built.build_http().is_ok());
}

#[test]
fn a_builder_shows_what_it_holds() {
    let built = server::builder().with(server::request_id()).grace(DEADLINE);

    assert_eq!(
        format!("{built:?}"),
        "Builder { layers: 1, shutdown: \"Ctrl-C and SIGTERM\", grace: 100ms }"
    );
    assert_eq!(
        format!("{:?}", client::builder().with(client::retry(1))),
        "Builder { http: 0, middleware: 1 }"
    );
}

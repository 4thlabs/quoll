//! Bearer authentication, from the contract to the wire: the server authenticates before its handler runs, and the
//! client's middleware sends the token to the routes that take it.

#![cfg(all(feature = "server", feature = "client"))]

use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use http::Extensions;
use quoll::rest::{self, Authenticate, Bearer, Created, Error, Result, Route};
use reqwest::StatusCode;
use reqwest::header::{AUTHORIZATION, WWW_AUTHENTICATE};
use reqwest_middleware::{ClientBuilder, Next};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

mod support {
    pub(crate) mod log;
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
    owner: String,
    title: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct NewNote {
    title: String,
}

rest::api! {
    /// Notes, to test authentication with.
    api Api = "/api";

    /// The signed-in user's notes.
    #[auth(Bearer)]
    group Notes {
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: NewNote) -> Created<Note>;
        /// Open to all, in a group that is not.
        #[auth(none)]
        GET  "/public"     => public() -> String;
    }

    group Open {
        GET "/open" => open() -> String;
        /// The one route of the group that takes a token.
        #[auth(Bearer)]
        GET "/me"   => me() -> String;
    }
}

/// Who a request comes from, once its token is checked.
struct User {
    name: String,
}

/// The service of both groups, which counts the calls of its handlers.
#[derive(Clone, Default)]
struct App {
    handled: Arc<AtomicUsize>,
}

impl App {
    fn handled(&self) {
        self.handled.fetch_add(1, Ordering::SeqCst);
    }
}

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        match token.as_str() {
            "ana-token" => Ok(User { name: "ana".into() }),
            "banned-token" => Err(Error::new(StatusCode::FORBIDDEN, "this user is banned")),
            "store-down-token" => Err(Error::internal("the session store is down")),
            _ => Err(Error::unauthorized("unknown or expired token")),
        }
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Note> {
        self.handled();

        Ok(Note {
            id,
            owner: user.name.clone(),
            title: "Milk".into(),
        })
    }

    async fn create(&self, user: &User, note: NewNote) -> Result<Created<Note>> {
        self.handled();

        Ok(Created::new(Note {
            id: 1,
            owner: user.name.clone(),
            title: note.title,
        }))
    }

    async fn public(&self) -> Result<String> {
        self.handled();

        Ok("open to all".into())
    }
}

impl OpenServer for App {
    async fn open(&self) -> Result<String> {
        self.handled();

        Ok("open".into())
    }

    async fn me(&self, user: &User) -> Result<String> {
        self.handled();

        Ok(user.name.clone())
    }
}

/// A served api: its URL, and the count of its handlers' calls.
struct Served {
    url: String,
    handled: Arc<AtomicUsize>,
}

impl Served {
    fn handled(&self) -> usize {
        self.handled.load(Ordering::SeqCst)
    }
}

/// Serves the api on a free port.
async fn serve() -> Served {
    let app = App::default();
    let handled = Arc::clone(&app.handled);
    let router = Api::router(ApiServices {
        notes: app.clone(),
        open: app,
    });
    let url = quoll::test::spawn(router).await;

    Served { url, handled }
}

/// A client of the api that sends `token`.
fn client(url: &str, token: &str) -> Result<ApiClient> {
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::bearer(token))
        .build();

    Api::client_with(http, url)
}

/// What a 401 answers: the problem's detail, once its status, its content type and its challenge are checked.
async fn refused(response: reqwest::Response, case: &str) -> String {
    assert_eq!(response.status(), StatusCode::UNAUTHORIZED, "{case}");
    assert_eq!(
        response
            .headers()
            .get_all(WWW_AUTHENTICATE)
            .iter()
            .collect::<Vec<_>>(),
        ["Bearer"],
        "{case}"
    );
    assert_eq!(
        response.headers()["content-type"],
        "application/problem+json",
        "{case}"
    );

    let problem: Value = response.json().await.expect("a problem");
    assert_eq!(problem["status"], 401, "{case}");

    problem["detail"]
        .as_str()
        .expect("a problem with a detail")
        .to_owned()
}

#[tokio::test]
async fn a_valid_token_reaches_the_handler_with_its_caller() -> Result<()> {
    let served = serve().await;
    let api = client(&served.url, "ana-token")?;

    let note = api.notes().get(7).await?;
    let created = api
        .notes()
        .create(NewNote {
            title: "Bread".into(),
        })
        .await?;
    let me = api.open().me().await?;

    assert_eq!(note.owner, "ana");
    assert_eq!(note.id, 7);
    assert_eq!(created.owner, "ana");
    assert_eq!(me, "ana");
    assert_eq!(served.handled(), 3);
    Ok(())
}

#[tokio::test]
async fn a_request_without_a_valid_token_is_refused_before_its_handler() {
    let served = serve().await;
    let http = reqwest::Client::new();
    let get = || http.get(format!("{}/api/notes/1", served.url));

    let cases = [
        ("no header", get()),
        (
            "another scheme",
            get().header(AUTHORIZATION, "Basic YW5hOnB3"),
        ),
        ("an empty token", get().header(AUTHORIZATION, "Bearer ")),
        ("a scheme alone", get().header(AUTHORIZATION, "Bearer")),
        (
            "a malformed token",
            get().header(AUTHORIZATION, "Bearer a b"),
        ),
        (
            "two headers",
            get()
                .header(AUTHORIZATION, "Bearer ana-token")
                .header(AUTHORIZATION, "Bearer ana-token"),
        ),
        (
            "a token beside another scheme's header",
            get()
                .header(AUTHORIZATION, "Bearer ana-token")
                .header(AUTHORIZATION, "Basic YW5hOnB3"),
        ),
        (
            "an unknown token",
            get().header(AUTHORIZATION, "Bearer someone-else"),
        ),
    ];

    for (case, request) in cases {
        let response = request.send().await.expect("the server answers");
        let detail = refused(response, case).await;

        assert!(!detail.is_empty(), "{case}");
    }

    let response = reqwest::Client::new()
        .get(format!("{}/api/me", served.url))
        .send()
        .await
        .expect("the server answers");
    refused(response, "a route of an open group").await;

    assert_eq!(served.handled(), 0);
}

#[tokio::test]
async fn the_generated_client_without_a_token_gets_a_401() -> Result<()> {
    let served = serve().await;
    let api = Api::client(&served.url)?;

    let error = api.notes().get(1).await.expect_err("no token");

    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("the request carries no credential, which this route takes")
    );
    assert_eq!(served.handled(), 0);
    Ok(())
}

#[tokio::test]
async fn the_error_of_authenticate_is_answered_as_it_is() -> Result<()> {
    let served = serve().await;

    let unknown = client(&served.url, "someone-else")?.notes().get(1).await;
    let banned = client(&served.url, "banned-token")?.notes().get(1).await;
    let down = reqwest::Client::new()
        .get(format!("{}/api/notes/1", served.url))
        .bearer_auth("store-down-token")
        .send()
        .await
        .expect("the server answers");

    let unknown = unknown.expect_err("an unknown token");
    assert_eq!(unknown.status(), Some(StatusCode::UNAUTHORIZED));
    assert_eq!(
        unknown.problem().and_then(rest::Problem::detail),
        Some("unknown or expired token")
    );

    let banned = banned.expect_err("a banned user");
    assert_eq!(banned.status(), Some(StatusCode::FORBIDDEN));
    assert_eq!(
        banned.problem().and_then(rest::Problem::detail),
        Some("this user is banned")
    );

    // A failure of the server's own is no wrong token: a 500, with no challenge and nothing of its cause.
    assert_eq!(down.status(), StatusCode::INTERNAL_SERVER_ERROR);
    assert!(down.headers().get(WWW_AUTHENTICATE).is_none());
    let body = down.text().await.expect("a body");
    assert!(!body.contains("session store"), "{body}");

    assert_eq!(served.handled(), 0);
    Ok(())
}

#[tokio::test]
async fn authentication_comes_before_the_path_is_read() {
    let served = serve().await;

    let response = reqwest::Client::new()
        .get(format!("{}/api/notes/not-a-number", served.url))
        .send()
        .await
        .expect("the server answers");

    refused(response, "a path that does not read").await;
}

#[tokio::test]
async fn a_refused_request_has_its_body_left_unread() {
    let served = serve().await;
    let address = served.url.trim_start_matches("http://");
    let mut stream = tokio::net::TcpStream::connect(address)
        .await
        .expect("the server accepts");

    // A body is announced and never sent: a server that read it would wait for it forever.
    let request = format!(
        "POST /api/notes HTTP/1.1\r\nHost: {address}\r\nContent-Type: application/json\r\nContent-Length: 1000\r\n\r\n{{"
    );
    stream
        .write_all(request.as_bytes())
        .await
        .expect("the request is written");

    let mut head = Vec::new();
    let read = async {
        let mut buffer = [0; 1024];
        while !head.windows(4).any(|window| window == b"\r\n\r\n") {
            let read = stream.read(&mut buffer).await.expect("the answer is read");
            assert_ne!(read, 0, "the server closed without answering");
            head.extend_from_slice(&buffer[..read]);
        }
    };
    tokio::time::timeout(Duration::from_secs(10), read)
        .await
        .expect("the server answers without waiting for the body");

    let head = String::from_utf8_lossy(&head).to_lowercase();
    assert!(head.starts_with("http/1.1 401"), "{head}");
    assert!(head.contains("www-authenticate: bearer"), "{head}");
    assert_eq!(served.handled(), 0);
}

#[tokio::test]
async fn a_route_with_auth_none_takes_no_token() -> Result<()> {
    let served = serve().await;

    let public = Api::client(&served.url)?.notes().public().await?;
    let open = Api::client(&served.url)?.open().open().await?;

    assert_eq!(public, "open to all");
    assert_eq!(open, "open");
    Ok(())
}

#[tokio::test]
async fn nothing_answered_or_logged_shows_the_token() {
    let (log, _capture) = support::log::capture(false);
    let served = serve().await;
    let http = reqwest::Client::new();
    let get = |value: &str| {
        http.get(format!("{}/api/notes/1", served.url))
            .header(AUTHORIZATION, value)
    };

    let mut bodies = String::new();
    for value in [
        "Bearer secret-1",
        "Bearer secret 2",
        "Bearer secret-3=x",
        "Bearer store-down-token",
    ] {
        let response = get(value).send().await.expect("the server answers");
        bodies.push_str(&format!("{:?} ", response.headers()));
        bodies.push_str(&response.text().await.expect("a body"));
    }

    let log = log.text();
    assert!(log.contains("the session store is down"), "{log}");
    for shown in [&bodies, &log] {
        assert!(
            !shown.contains("secret") && !shown.contains("store-down-token"),
            "{shown}"
        );
    }
}

/// What a middleware saw of a request: its operation, the schemes of its route, and whether it carried a token.
type Seen = Arc<Mutex<Vec<(Option<&'static str>, Option<&'static [&'static str]>, bool)>>>;

/// A middleware that keeps what it sees of each request, put after `bearer` so that it sees what `bearer` sent.
fn recorder(
    seen: Seen,
) -> impl for<'a> Fn(
    reqwest::Request,
    &'a mut Extensions,
    Next<'a>,
) -> Pin<
    Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>,
> + Send
+ Sync
+ 'static {
    move |request, extensions, next| {
        let route = extensions.get::<Route>();
        seen.lock().expect("no test panics while holding it").push((
            route.map(Route::operation_id),
            route.map(Route::schemes),
            request.headers().contains_key(AUTHORIZATION),
        ));

        Box::pin(next.run(request, extensions))
    }
}

#[tokio::test]
async fn the_middleware_sends_the_token_to_the_routes_that_take_it_only() -> Result<()> {
    let served = serve().await;
    let seen = Seen::default();
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::bearer("ana-token"))
        .with(recorder(Arc::clone(&seen)))
        .build();
    let api = Api::client_with(http.clone(), &served.url)?;

    api.notes().get(1).await?;
    api.notes().public().await?;
    api.open().open().await?;
    api.open().me().await?;
    // A request that does not come from the client of a contract, as one to another host.
    http.get(format!("{}/api/me", served.url))
        .send()
        .await
        .expect("the server answers");

    let bearer: &[&str] = &["Bearer"];
    let none: &[&str] = &[];
    assert_eq!(
        *seen.lock().expect("no test panics while holding it"),
        [
            (Some("notes_get"), Some(bearer), true),
            (Some("notes_public"), Some(none), false),
            (Some("open_open"), Some(none), false),
            (Some("open_me"), Some(bearer), true),
            (None, None, false),
        ]
    );
    Ok(())
}

#[tokio::test]
async fn a_token_that_is_not_a_b64token_fails_the_call_without_showing_it() -> Result<()> {
    let served = serve().await;

    for token in ["secret\ntoken", "secret token", "secret,token"] {
        let api = client(&served.url, token)?;

        let error = api.notes().get(1).await.expect_err("not a b64token");
        let open = api.open().open().await?;

        let shown = format!("{error} {error:?}");
        assert_eq!(error.status(), None, "{token:?}: {shown}");
        assert!(!shown.contains("secret"), "{shown}");
        assert_eq!(open, "open");
    }

    assert_eq!(served.handled(), 3);
    Ok(())
}

//! The client a contract becomes, calling the server the same contract becomes, served on a port.

#![cfg(all(feature = "server", feature = "client"))]

use std::error::Error as _;
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex};

use http::Extensions;
use http::header::LOCATION;
use quoll::rest::{self, Created, Error, Response, Result, Route};
use reqwest::StatusCode;
use reqwest_middleware::{ClientBuilder, Next};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
    title: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct NewNote {
    title: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Shelf {
    KitchenTop,
    Office,
}

#[derive(Debug, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Found {
    contains: Option<String>,
    page: Option<u32>,
    tags: Vec<String>,
}

rest::api! {
    /// Notes, to test the client with.
    api Api = "/api";

    group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}" => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}" => remove(id: i64);
    }

    group Search {
        GET "/search?contains={contains}&page={page}&tag={tags}"
            => find(contains: Option<String>, page: Option<u32>, tags: Vec<String>) -> Found;
    }

    group Echo {
        GET "/echo/{shelf}/{text}" => echo(text: String, shelf: Shelf) -> String;
    }

    group Failures {
        GET "/fail/{kind}" => fail(kind: String);
    }

    /// Calls the api again, through its client, as a server calls another API.
    group Proxy {
        GET "/proxy/{id}" => note(id: i64) -> Note;
        GET "/proxy/dead"  => dead();
    }
}

#[derive(Default)]
struct App {
    notes: Mutex<Vec<Note>>,
}

impl App {
    fn notes(&self) -> std::sync::MutexGuard<'_, Vec<Note>> {
        self.notes
            .lock()
            .expect("no test panics while holding the notes")
    }
}

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<Note>> {
        self.notes()
            .iter()
            .find(|note| note.id == id)
            .cloned()
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Response<Note>> {
        let mut notes = self.notes();
        let note = Note {
            id: i64::try_from(notes.len()).expect("few notes") + 1,
            title: note.title,
        };
        notes.push(note.clone());

        let location = format!("/api/notes/{}", note.id);

        Ok(Response::new(note).header(LOCATION, location))
    }

    async fn update(&self, id: i64, note: NewNote) -> Result<Response<Note>> {
        let mut notes = self.notes();
        let found = notes
            .iter_mut()
            .find(|found| found.id == id)
            .ok_or_else(|| Error::not_found("no such note"))?;
        found.title = note.title;

        Ok(Response::new(found.clone()))
    }

    async fn remove(&self, id: i64) -> Result<Response<()>> {
        let mut notes = self.notes();
        let before = notes.len();
        notes.retain(|note| note.id != id);

        if notes.len() == before {
            return Err(Error::not_found("no such note"));
        }
        Ok(Response::new(()))
    }
}

struct Searcher;

impl SearchServer for Searcher {
    async fn find(
        &self,
        contains: Option<String>,
        page: Option<u32>,
        tags: Vec<String>,
    ) -> Result<Response<Found>> {
        Ok(Response::new(Found {
            contains,
            page,
            tags,
        }))
    }
}

struct Echoer;

impl EchoServer for Echoer {
    async fn echo(&self, text: String, shelf: Shelf) -> Result<Response<String>> {
        Ok(Response::new(format!("{shelf:?} {text}")))
    }
}

struct Failing;

impl FailuresServer for Failing {
    async fn fail(&self, kind: String) -> Result<Response<()>> {
        match kind.as_str() {
            "conflict" => Err(Error::new(StatusCode::CONFLICT, "already there")),
            _ => Err(Error::internal("the database password is hunter2")),
        }
    }
}

/// A service that calls an api through its client: the one it is served with, and one that is not served.
struct Proxying {
    api: ApiClient,
    dead: ApiClient,
}

impl ProxyServer for Proxying {
    async fn note(&self, id: i64) -> Result<Response<Note>> {
        self.api.notes().get(id).await.map(Response::new)
    }

    async fn dead(&self) -> Result<Response<()>> {
        self.dead
            .failures()
            .fail("conflict".into())
            .await
            .map(Response::new)
    }
}

/// A URL where nothing answers: a port kept bound, whose every connection is closed at once, with no response. A port
/// freed instead could be taken by another test, which would answer.
async fn dead_url() -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let address = listener.local_addr().expect("the listener has an address");

    tokio::spawn(async move {
        while let Ok((connection, _)) = listener.accept().await {
            drop(connection);
        }
    });

    format!("http://{address}")
}

/// Serves `router` on a free port, and gives the URL it is served at.
async fn serve_router(router: impl FnOnce(&str) -> axum::Router) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let url = format!(
        "http://{}",
        listener.local_addr().expect("the listener has an address")
    );
    let router = router(&url);

    tokio::spawn(async move { axum::serve(listener, router).await });

    url
}

/// Serves the api on a free port, and gives its URL.
async fn serve() -> String {
    let dead = dead_url().await;

    serve_router(|url| router(url, &dead)).await
}

fn router(url: &str, dead: &str) -> axum::Router {
    Api::router(ApiServices {
        notes: App::default(),
        search: Searcher,
        echo: Echoer,
        failures: Failing,
        proxy: Proxying {
            api: Api::client(url).expect("a valid url"),
            dead: Api::client(dead).expect("a valid url"),
        },
    })
}

#[tokio::test]
async fn a_note_is_created_read_updated_and_removed() -> Result<()> {
    let api = Api::client(&serve().await)?;
    let notes = api.notes();

    let created: Note = notes
        .create(NewNote {
            title: "Milk".into(),
        })
        .await?;
    assert_eq!(
        created,
        Note {
            id: 1,
            title: "Milk".into()
        }
    );
    assert_eq!(notes.get(1).await?, created);

    let updated = notes
        .update(
            1,
            NewNote {
                title: "Bread".into(),
            },
        )
        .await?;
    assert_eq!(updated.title, "Bread");

    let removed: () = notes.remove(1).await?;
    assert_eq!(removed, ());
    assert_eq!(
        notes.get(1).await.expect_err("removed").status(),
        Some(StatusCode::NOT_FOUND)
    );
    Ok(())
}

#[tokio::test]
async fn a_path_parameter_reads_back_as_it_was_written() -> Result<()> {
    let api = Api::client(&serve().await)?;
    let values = [
        "a/b",
        "what?",
        "50%",
        "%2F",
        "milk jar",
        "a+b&c=d#e",
        "é ü",
        "...",
        ".hidden",
    ];

    for value in values {
        let echoed = api.echo().echo(value.into(), Shelf::KitchenTop).await?;

        assert_eq!(echoed, format!("KitchenTop {value}"), "{value}");
    }
    Ok(())
}

#[tokio::test]
async fn a_path_parameter_that_would_call_another_route_is_refused() -> Result<()> {
    let api = Api::client(&serve().await)?;

    for value in ["", ".", ".."] {
        let error = api
            .echo()
            .echo(value.into(), Shelf::Office)
            .await
            .expect_err("not a segment");

        assert_eq!(error.status(), None, "{value}");
        assert!(error.to_string().contains("not a segment"), "{error}");
    }
    Ok(())
}

#[tokio::test]
async fn a_query_leaves_none_out_and_repeats_a_vec() -> Result<()> {
    let api = Api::client(&serve().await)?;
    let search = api.search();

    let found = search.find(None, None, Vec::new()).await?;
    assert_eq!(
        found,
        Found {
            contains: None,
            page: None,
            tags: Vec::new()
        }
    );

    let found = search
        .find(
            Some("milk & jar".into()),
            Some(2),
            vec!["a".into(), "b=c".into()],
        )
        .await?;
    assert_eq!(
        found,
        Found {
            contains: Some("milk & jar".into()),
            page: Some(2),
            tags: vec!["a".into(), "b=c".into()]
        }
    );

    let found = search.find(Some(String::new()), None, Vec::new()).await?;
    assert_eq!(found.contains.as_deref(), Some(""));
    Ok(())
}

#[tokio::test]
async fn an_error_is_read_back_with_its_status_and_its_problem() -> Result<()> {
    let api = Api::client(&serve().await)?;

    let error = api.notes().get(999).await.expect_err("no note 999");
    let problem = error.problem().expect("the server answers a problem");

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(problem.kind(), "about:blank");
    assert_eq!(problem.title(), Some("Not Found"));
    assert_eq!(problem.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(problem.detail(), Some("no such note"));
    assert_eq!(error.to_string(), "another api answered 404: no such note");
    assert_eq!(
        error.body_as::<serde_json::Value>(),
        Some(serde_json::json!({
            "type": "about:blank",
            "title": "Not Found",
            "status": 404,
            "detail": "no such note"
        }))
    );

    let error = api
        .failures()
        .fail("conflict".into())
        .await
        .expect_err("a conflict");
    assert_eq!(error.status(), Some(StatusCode::CONFLICT));
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("already there")
    );

    let error = api
        .failures()
        .fail("internal".into())
        .await
        .expect_err("an internal error");
    assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    assert_eq!(error.problem().and_then(rest::Problem::detail), None);
    Ok(())
}

#[tokio::test]
async fn a_handler_returning_what_its_client_received_answers_502() -> Result<()> {
    let api = Api::client(&serve().await)?;

    api.notes()
        .create(NewNote {
            title: "Milk".into(),
        })
        .await?;
    assert_eq!(api.proxy().note(1).await?.title, "Milk");

    for error in [
        api.proxy().note(999).await.expect_err("an upstream 404"),
        api.proxy()
            .dead()
            .await
            .expect_err("an upstream with no answer"),
    ] {
        assert_eq!(error.status(), Some(StatusCode::BAD_GATEWAY));
        assert_eq!(
            error.problem().and_then(rest::Problem::status),
            Some(StatusCode::BAD_GATEWAY)
        );
        assert_eq!(error.problem().and_then(rest::Problem::detail), None);
    }
    Ok(())
}

#[tokio::test]
async fn a_call_that_gets_no_response_has_no_status() -> Result<()> {
    let api = Api::client(&dead_url().await)?;

    let error = api.notes().get(1).await.expect_err("nothing answers");

    assert_eq!(error.status(), None);
    assert!(error.problem().is_none() && error.body().is_none());
    assert_eq!(error.to_string(), "the call `notes_get` got no response");
    assert!(
        error
            .source()
            .is_some_and(|source| source.is::<reqwest_middleware::Error>()),
        "{error:?}"
    );
    Ok(())
}

/// The error shape of an API that does not answer problems.
#[derive(Debug, PartialEq, Deserialize, JsonSchema)]
struct Teapot {
    message: String,
    code: u32,
}

/// What the third party answers with an error, longer than what the client keeps of it.
const LONG_BODY: usize = 1 << 20;

/// What the third party answers on a route with no result: more than a first read gives, less than the client reads.
const IGNORED_BODY: usize = 48 * 1024;

/// Another API, written without the contract: it answers the client's routes in its own way.
fn third_party(_url: &str) -> axum::Router {
    use axum::routing::{delete, get};

    axum::Router::new()
        .route(
            "/api/notes/4",
            get(|| async { (StatusCode::INTERNAL_SERVER_ERROR, "x".repeat(LONG_BODY)) }),
        )
        .route(
            "/api/notes/{id}",
            delete(|| async { "removed ".repeat(IGNORED_BODY / 8) }),
        )
        .route("/api/notes/1", get(|| async { "not json" }))
        .route(
            "/api/notes/2",
            get(|| async { axum::Json(serde_json::json!({ "id": "two" })) }),
        )
        .route(
            "/api/notes/3",
            get(|| async {
                (
                    StatusCode::IM_A_TEAPOT,
                    axum::Json(serde_json::json!({ "message": "short and stout", "code": 7 })),
                )
            }),
        )
}

#[tokio::test]
async fn a_success_whose_body_is_not_the_result_is_an_error_with_no_status() -> Result<()> {
    let api = Api::client(&serve_router(third_party).await)?;

    for id in [1, 2] {
        let error = api.notes().get(id).await.expect_err("not a note");

        assert_eq!(error.status(), None, "{id}");
        assert!(error.body().is_none(), "{id}");
        assert_eq!(
            error.to_string(),
            "the call `notes_get` got an answer that is not its result"
        );
        assert!(error.source().is_some(), "{id}");
    }
    Ok(())
}

#[tokio::test]
async fn an_error_of_another_shape_is_read_with_body_as() -> Result<()> {
    let api = Api::client(&serve_router(third_party).await)?;

    let error = api.notes().get(3).await.expect_err("a teapot");

    assert_eq!(error.status(), Some(StatusCode::IM_A_TEAPOT));
    assert!(error.problem().is_none(), "JSON, not a problem");
    assert_eq!(
        error.body_as::<Teapot>(),
        Some(Teapot {
            message: "short and stout".into(),
            code: 7
        })
    );
    assert_eq!(error.body_as::<Note>(), None);
    Ok(())
}

#[tokio::test]
async fn an_error_keeps_the_first_64_kib_of_its_body() -> Result<()> {
    let api = Api::client(&serve_router(third_party).await)?;

    let error = api.notes().get(4).await.expect_err("a long error");
    let body = error.body().expect("an error answer has a body");

    assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    assert_eq!(body.len(), 64 * 1024);
    assert!(body.iter().all(|&byte| byte == b'x'));
    Ok(())
}

#[tokio::test]
async fn a_route_with_no_result_reads_a_body_so_that_its_connection_serves_again() -> Result<()> {
    use axum::serve::ListenerExt;

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let url = format!(
        "http://{}",
        listener.local_addr().expect("the listener has an address")
    );
    let connections = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let counted = Arc::clone(&connections);
    let listener = listener.tap_io(move |_| {
        counted.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    });
    tokio::spawn(async move { axum::serve(listener, third_party("")).await });
    let api = Api::client(&url)?;

    for id in 5..=7 {
        api.notes().remove(id).await?;
    }

    assert_eq!(connections.load(std::sync::atomic::Ordering::SeqCst), 1);
    Ok(())
}

#[tokio::test]
async fn a_client_calls_an_api_served_below_a_path() -> Result<()> {
    let dead = dead_url().await;
    let url = serve_router(|url| {
        axum::Router::new().nest(
            "/tenants/acme",
            router(&format!("{url}/tenants/acme"), &dead),
        )
    })
    .await;

    for base in [
        format!("{url}/tenants/acme"),
        format!("{url}/tenants/acme/"),
    ] {
        let api = Api::client(&base)?;

        let echoed = api.echo().echo("x".into(), Shelf::Office).await?;
        assert_eq!(echoed, "Office x", "{base}");
    }
    Ok(())
}

type Seen = Arc<Mutex<Vec<(Route, String)>>>;

/// A middleware that keeps the route and the URL of each request it sees.
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
        let route = extensions
            .get::<Route>()
            .cloned()
            .expect("each request carries its route");
        seen.lock()
            .expect("no test panics while holding it")
            .push((route, request.url().to_string()));

        Box::pin(next.run(request, extensions))
    }
}

#[tokio::test]
async fn a_middleware_sees_the_route_of_each_request() -> Result<()> {
    let url = serve().await;
    let seen = Seen::default();
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(recorder(Arc::clone(&seen)))
        .build();
    let api = Api::client_with(http, &url)?;

    api.notes()
        .create(NewNote {
            title: "Milk".into(),
        })
        .await?;
    api.notes().get(1).await?;
    api.search()
        .find(None, Some(2), vec!["a b".into(), "c".into()])
        .await?;

    let seen = seen.lock().expect("no test panics while holding it");
    let routes: Vec<_> = seen
        .iter()
        .map(|(route, url)| {
            (
                route.operation_id(),
                route.group(),
                route.method().as_str(),
                route.path(),
                route.schemes(),
                url.strip_prefix(&*url_prefix(url)).unwrap_or(url),
            )
        })
        .collect();

    let none: &[&str] = &[];
    assert_eq!(
        routes,
        [
            (
                "notes_create",
                "Notes",
                "POST",
                "/api/notes",
                none,
                "/api/notes"
            ),
            (
                "notes_get",
                "Notes",
                "GET",
                "/api/notes/{id}",
                none,
                "/api/notes/1"
            ),
            (
                "search_find",
                "Search",
                "GET",
                "/api/search",
                none,
                "/api/search?page=2&tag=a+b&tag=c"
            ),
        ]
    );
    Ok(())
}

/// The scheme and the authority of a URL, `http://127.0.0.1:8080`.
fn url_prefix(url: &str) -> String {
    let url = reqwest::Url::parse(url).expect("a valid url");

    url.origin().ascii_serialization()
}

#[tokio::test]
async fn a_url_that_is_not_an_apis_is_an_error() {
    for url in ["", "localhost:8080", "ftp://localhost/api"] {
        let error = Api::client(url).expect_err("not an api's url");

        assert_eq!(error.status(), None, "{url}");
    }

    let http = ClientBuilder::new(reqwest::Client::new()).build();
    assert!(Api::client_with(http, "http://localhost/?a=1").is_err());
}

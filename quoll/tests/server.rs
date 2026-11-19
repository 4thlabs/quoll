//! The server a contract becomes, served on a port and called over HTTP.

#![cfg(feature = "server")]

use std::sync::Mutex;

use quoll::rest::{self, Created, Error, Result};
use reqwest::StatusCode;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
struct Note {
    id: i64,
    title: String,
}

#[derive(Debug, Serialize, Deserialize)]
struct NewNote {
    title: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum Shelf {
    Kitchen,
    Office,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Found {
    shelf: Shelf,
    name: String,
    contains: Option<String>,
    page: u32,
    tags: Vec<String>,
}

rest::api! {
    /// Notes, to test the server with.
    api Api = "/api";

    group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        POST   "/drafts"     => draft(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}" => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}" => remove(id: i64);
    }

    group Search {
        GET "/shelves/{shelf}/{name}?contains={contains}&page={page}&tag={tags}"
            => find(name: String, contains: Option<String>, shelf: Shelf, page: u32, tags: Vec<String>) -> Found;
    }

    group Failures {
        GET "/fail/{kind}" => fail(kind: String);
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

    fn add(&self, note: NewNote) -> Note {
        let mut notes = self.notes();
        let note = Note {
            id: i64::try_from(notes.len()).expect("few notes") + 1,
            title: note.title,
        };
        notes.push(note.clone());

        note
    }
}

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Note> {
        self.notes()
            .iter()
            .find(|note| note.id == id)
            .cloned()
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Created<Note>> {
        let note = self.add(note);

        Ok(Created::at(format!("/api/notes/{}", note.id), note))
    }

    async fn draft(&self, note: NewNote) -> Result<Created<Note>> {
        Ok(Created::new(self.add(note)))
    }

    async fn update(&self, id: i64, note: NewNote) -> Result<Note> {
        let mut notes = self.notes();
        let found = notes
            .iter_mut()
            .find(|found| found.id == id)
            .ok_or_else(|| Error::not_found("no such note"))?;
        found.title = note.title;

        Ok(found.clone())
    }

    async fn remove(&self, id: i64) -> Result<()> {
        let mut notes = self.notes();
        let before = notes.len();
        notes.retain(|note| note.id != id);

        if notes.len() == before {
            return Err(Error::not_found("no such note"));
        }
        Ok(())
    }
}

struct Searcher;

impl SearchServer for Searcher {
    async fn find(
        &self,
        name: String,
        contains: Option<String>,
        shelf: Shelf,
        page: u32,
        tags: Vec<String>,
    ) -> Result<Found> {
        Ok(Found {
            shelf,
            name,
            contains,
            page,
            tags,
        })
    }
}

struct Failing;

impl FailuresServer for Failing {
    async fn fail(&self, kind: String) -> Result<()> {
        match kind.as_str() {
            "bad_request" => Err(Error::bad_request("the request is wrong")),
            "unauthorized" => Err(Error::unauthorized("who are you")),
            "conflict" => Err(Error::new(StatusCode::CONFLICT, "already there")),
            "not_an_error" => Err(Error::new(StatusCode::OK, "all is well")),
            _ => Err(Error::internal("the database password is hunter2")),
        }
    }
}

/// Serves the api on a free port, and gives its URL.
async fn serve() -> String {
    let router = Api::router(ApiServices {
        notes: App::default(),
        search: Searcher,
        failures: Failing,
    });
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let address = listener.local_addr().expect("the listener has an address");

    tokio::spawn(async move { axum::serve(listener, router).await });

    format!("http://{address}/api")
}

/// The status, the content type and the body of an answer.
async fn answer(request: reqwest::RequestBuilder) -> (StatusCode, String, Value) {
    let response = request.send().await.expect("the server answers");
    let status = response.status();
    let content_type = response
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .map(|value| value.to_str().expect("an ASCII content type").to_owned())
        .unwrap_or_default();
    let body = response.bytes().await.expect("the body is read");
    let body = if body.is_empty() {
        Value::Null
    } else {
        serde_json::from_slice(&body).expect("the body is JSON")
    };

    (status, content_type, body)
}

/// Checks that an answer is a problem with this status, and gives its detail.
fn problem(answer: (StatusCode, String, Value), status: StatusCode) -> Option<String> {
    let (got, content_type, body) = answer;

    assert_eq!(got, status, "{body}");
    assert_eq!(content_type, "application/problem+json");
    assert_eq!(body["type"], "about:blank");
    assert_eq!(body["status"], status.as_u16());
    assert_eq!(body["title"].as_str(), status.canonical_reason(), "{body}");

    body.get("detail")
        .map(|detail| detail.as_str().expect("a detail is a string").to_owned())
}

#[tokio::test]
async fn a_note_is_created_read_updated_and_removed() {
    let url = serve().await;
    let http = reqwest::Client::new();

    let response = http
        .post(format!("{url}/notes"))
        .json(&json!({ "title": "Milk" }))
        .send()
        .await
        .expect("the server answers");
    assert_eq!(response.status(), StatusCode::CREATED);
    assert_eq!(response.headers()["location"], "/api/notes/1");
    let created: Note = response.json().await.expect("a note");
    assert_eq!(
        created,
        Note {
            id: 1,
            title: "Milk".into()
        }
    );

    let (status, content_type, body) = answer(http.get(format!("{url}/notes/1"))).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(content_type, "application/json");
    assert_eq!(body, json!({ "id": 1, "title": "Milk" }));

    let update = http
        .put(format!("{url}/notes/1"))
        .json(&json!({ "title": "Bread" }));
    let (status, _, body) = answer(update).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(body, json!({ "id": 1, "title": "Bread" }));

    let (status, content_type, body) = answer(http.delete(format!("{url}/notes/1"))).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    assert_eq!(content_type, "");
    assert_eq!(body, Value::Null);

    let detail = problem(
        answer(http.get(format!("{url}/notes/1"))).await,
        StatusCode::NOT_FOUND,
    );
    assert_eq!(detail.as_deref(), Some("no such note"));
}

#[tokio::test]
async fn created_without_a_location_sends_none() {
    let url = serve().await;

    let response = reqwest::Client::new()
        .post(format!("{url}/drafts"))
        .json(&json!({ "title": "Draft" }))
        .send()
        .await
        .expect("the server answers");

    assert_eq!(response.status(), StatusCode::CREATED);
    assert!(response.headers().get("location").is_none());
}

#[tokio::test]
async fn path_and_query_are_read_by_name() {
    let url = serve().await;
    let http = reqwest::Client::new();

    let request = http.get(format!(
        "{url}/shelves/kitchen/big%20jar?page=2&tag=a&contains=milk&tag=b"
    ));
    let (status, _, body) = answer(request).await;

    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(
        body,
        json!({ "shelf": "kitchen", "name": "big jar", "contains": "milk", "page": 2, "tags": ["a", "b"] })
    );

    let (status, _, body) = answer(http.get(format!("{url}/shelves/office/box?page=1"))).await;

    assert_eq!(status, StatusCode::OK, "{body}");
    assert_eq!(
        body,
        json!({ "shelf": "office", "name": "box", "contains": null, "page": 1, "tags": [] })
    );
}

#[tokio::test]
async fn malformed_input_answers_a_400_problem() {
    let url = serve().await;
    let http = reqwest::Client::new();

    let cases = [
        (http.get(format!("{url}/notes/one")), "path"),
        (
            http.get(format!("{url}/shelves/garage/box?page=1")),
            "unknown shelf",
        ),
        (
            http.get(format!("{url}/shelves/office/box?page=two")),
            "query value",
        ),
        (
            http.get(format!("{url}/shelves/office/box")),
            "missing query key",
        ),
        (
            http.post(format!("{url}/notes"))
                .header("content-type", "application/json")
                .body("{ not json"),
            "body syntax",
        ),
        (
            http.post(format!("{url}/notes"))
                .json(&json!({ "name": "Milk" })),
            "body shape",
        ),
    ];

    for (request, case) in cases {
        let detail = problem(answer(request).await, StatusCode::BAD_REQUEST);

        assert!(detail.is_some_and(|detail| !detail.is_empty()), "{case}");
    }
}

#[tokio::test]
async fn a_body_without_its_json_content_type_answers_415() {
    let url = serve().await;

    let request = reqwest::Client::new()
        .post(format!("{url}/notes"))
        .body(r#"{ "title": "Milk" }"#);

    problem(answer(request).await, StatusCode::UNSUPPORTED_MEDIA_TYPE);
}

#[tokio::test]
async fn each_error_answers_its_problem() {
    let url = serve().await;
    let http = reqwest::Client::new();
    let fail = |kind: &str| http.get(format!("{url}/fail/{kind}"));

    let cases = [
        (
            "bad_request",
            StatusCode::BAD_REQUEST,
            Some("the request is wrong"),
        ),
        (
            "unauthorized",
            StatusCode::UNAUTHORIZED,
            Some("who are you"),
        ),
        ("conflict", StatusCode::CONFLICT, Some("already there")),
        ("not_an_error", StatusCode::INTERNAL_SERVER_ERROR, None),
        ("internal", StatusCode::INTERNAL_SERVER_ERROR, None),
    ];

    for (kind, status, detail) in cases {
        let (got, content_type, body) = answer(fail(kind)).await;

        assert!(!body.to_string().contains("hunter2"), "{kind}: {body}");
        assert_eq!(
            problem((got, content_type, body), status).as_deref(),
            detail,
            "{kind}"
        );
    }
}

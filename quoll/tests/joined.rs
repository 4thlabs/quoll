//! A list written as the one value of a query key, `#[join(sep)]`: sent by the client the contract becomes to the
//! server it becomes, served on a port, and read from what a browser sends.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::{Arc, Mutex};

use axum::extract::State as Shared;
use axum::http::Uri;
use axum::routing::get;
use quoll::rest::{self, Response, Result};
use reqwest::StatusCode;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum State {
    Open,
    InReview,
}

/// What the server read of a call of `list`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
struct Listed {
    labels: Vec<String>,
    state: Option<State>,
}

rest::api! {
    api Api = "/api";

    group Issues {
        GET "/issues?labels={labels}&state={state}"
            => list(#[join(",")] labels: Vec<String>, state: Option<State>) -> Listed;
        GET "/spaced?ids={ids}" => spaced(#[join(" ")] ids: Vec<u32>) -> Vec<u32>;
        GET "/piped?page={page}&states={states}&tag={tags}"
            => piped(page: u32, #[join("|")] states: Vec<State>, tags: Vec<String>) -> Vec<State>;
    }
}

struct App;

impl IssuesServer for App {
    async fn list(&self, labels: Vec<String>, state: Option<State>) -> Result<Response<Listed>> {
        Ok(Response::new(Listed { labels, state }))
    }

    async fn spaced(&self, ids: Vec<u32>) -> Result<Response<Vec<u32>>> {
        Ok(Response::new(ids))
    }

    async fn piped(
        &self,
        _page: u32,
        states: Vec<State>,
        _tags: Vec<String>,
    ) -> Result<Response<Vec<State>>> {
        Ok(Response::new(states))
    }
}

/// The api served on a port, its URL and its client.
async fn served() -> (String, ApiClient) {
    let url = quoll::test::spawn(Api::router(ApiServices { issues: App })).await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api)
}

/// What the server answers to a query sent as it is, by a client other than quoll's.
async fn read(url: &str, path_and_query: &str) -> (StatusCode, Value) {
    let response = reqwest::get(format!("{url}/api{path_and_query}"))
        .await
        .expect("the server answers");

    (
        response.status(),
        response.json().await.expect("the body is JSON"),
    )
}

#[tokio::test]
async fn a_joined_list_round_trips_with_each_separator() {
    let (_, api) = served().await;
    let labels = vec!["bug".to_owned(), "good first issue".into(), "é&?=%".into()];

    assert_eq!(
        api.issues()
            .list(labels.clone(), Some(State::InReview))
            .await
            .unwrap(),
        Listed {
            labels,
            state: Some(State::InReview),
        }
    );
    assert_eq!(api.issues().spaced(vec![3, 1, 2]).await.unwrap(), [3, 1, 2]);
    assert_eq!(
        api.issues()
            .piped(1, vec![State::Open, State::InReview], vec!["a".into()])
            .await
            .unwrap(),
        [State::Open, State::InReview]
    );
}

#[tokio::test]
async fn an_empty_list_round_trips_and_leaves_its_key_out() {
    let (_, api) = served().await;

    assert_eq!(
        api.issues().list(Vec::new(), None).await.unwrap(),
        Listed {
            labels: Vec::new(),
            state: None,
        }
    );
    assert_eq!(
        api.issues().spaced(Vec::new()).await.unwrap(),
        Vec::<u32>::new()
    );
    // An empty value among others is one.
    assert_eq!(
        api.issues()
            .list(vec![String::new(), "a".into()], None)
            .await
            .unwrap()
            .labels,
        ["", "a"]
    );
}

#[tokio::test]
async fn the_client_writes_the_separator_between_escaped_values() {
    let queries = Arc::new(Mutex::new(Vec::new()));
    let router = axum::Router::new()
        .route(
            "/api/spaced",
            get(
                |Shared(queries): Shared<Arc<Mutex<Vec<String>>>>, uri: Uri| async move {
                    let query = uri.query().unwrap_or_default().to_owned();
                    queries
                        .lock()
                        .expect("no test panics while holding it")
                        .push(query);

                    axum::Json(Vec::<u32>::new())
                },
            ),
        )
        .route(
            "/api/piped",
            get(
                |Shared(queries): Shared<Arc<Mutex<Vec<String>>>>, uri: Uri| async move {
                    let query = uri.query().unwrap_or_default().to_owned();
                    queries
                        .lock()
                        .expect("no test panics while holding it")
                        .push(query);

                    axum::Json(Vec::<State>::new())
                },
            ),
        )
        .route(
            "/api/issues",
            get(
                |Shared(queries): Shared<Arc<Mutex<Vec<String>>>>, uri: Uri| async move {
                    let query = uri.query().unwrap_or_default().to_owned();
                    queries
                        .lock()
                        .expect("no test panics while holding it")
                        .push(query);

                    axum::Json(Listed {
                        labels: Vec::new(),
                        state: None,
                    })
                },
            ),
        )
        .with_state(queries.clone());
    let url = quoll::test::spawn(router).await;
    let api = Api::client(&url).expect("the url is the api's");

    api.issues()
        .list(
            vec!["bug".into(), "good first".into(), "a&b|c".into()],
            Some(State::Open),
        )
        .await
        .unwrap();
    api.issues().list(Vec::new(), None).await.unwrap();
    api.issues().spaced(vec![1, 22]).await.unwrap();
    api.issues()
        .piped(
            2,
            vec![State::Open, State::InReview],
            vec!["x".into(), "y".into()],
        )
        .await
        .unwrap();
    api.issues().piped(3, Vec::new(), Vec::new()).await.unwrap();

    assert_eq!(
        *queries.lock().expect("no test panics while holding it"),
        [
            "state=open&labels=bug,good%20first,a%26b%7Cc",
            "",
            "ids=1%2022",
            "page=2&tag=x&tag=y&states=open|in_review",
            "page=3",
        ]
    );
}

#[tokio::test]
async fn the_server_reads_a_joined_list_escaped_or_not() {
    let (url, _) = served().await;
    let labels = |value: Value| value["labels"].clone();

    for query in [
        "labels=bug,docs",
        // What a browser's `URLSearchParams` writes, and orval's clients with it.
        "labels=bug%2Cdocs",
    ] {
        let (status, listed) = read(&url, &format!("/issues?{query}")).await;

        assert_eq!(status, StatusCode::OK, "{query}");
        assert_eq!(
            labels(listed),
            serde_json::json!(["bug", "docs"]),
            "{query}"
        );
    }
    for query in ["", "?labels=", "?state=open"] {
        let (status, listed) = read(&url, &format!("/issues{query}")).await;

        assert_eq!(status, StatusCode::OK, "{query}");
        assert_eq!(labels(listed), serde_json::json!([]), "{query}");
    }
    for query in ["ids=1%202", "ids=1+2"] {
        let (status, ids) = read(&url, &format!("/spaced?{query}")).await;

        assert_eq!(
            (status, ids),
            (StatusCode::OK, serde_json::json!([1, 2])),
            "{query}"
        );
    }
    for query in ["states=open|in_review", "states=open%7Cin_review"] {
        let (status, states) = read(&url, &format!("/piped?page=1&{query}")).await;

        assert_eq!(
            (status, states),
            (StatusCode::OK, serde_json::json!(["open", "in_review"])),
            "{query}"
        );
    }
}

#[tokio::test]
async fn a_joined_list_that_does_not_read_is_a_bad_request() {
    let (url, _) = served().await;

    for (path_and_query, detail) in [
        ("/spaced?ids=1%20two", "the value `two` of a joined list"),
        ("/spaced?ids=1%20%202", "the value `` of a joined list"),
        (
            "/piped?page=1&states=open|closed",
            "the value `closed` of a joined list",
        ),
        ("/issues?labels=a&labels=b", "the query is not valid"),
    ] {
        let (status, problem) = read(&url, path_and_query).await;
        let text = problem["detail"].as_str().unwrap_or_default();

        assert_eq!(status, StatusCode::BAD_REQUEST, "{path_and_query}");
        assert!(text.contains(detail), "{path_and_query}: {text}");
    }
}

#[tokio::test]
async fn a_list_the_server_would_read_otherwise_is_refused_by_the_client() {
    let (_, api) = served().await;

    for (labels, refused) in [
        (
            vec!["bug".to_owned(), "a,b".into()],
            "holds the separator `,`",
        ),
        (vec![String::new()], "is one empty value"),
    ] {
        let error = api
            .issues()
            .list(labels.clone(), None)
            .await
            .expect_err("a list the server would not read back");

        assert_eq!(error.status(), None, "{labels:?}");
        assert!(
            error
                .to_string()
                .contains("the query key `labels` of `issues_list`"),
            "{error}"
        );
        assert!(error.to_string().contains(refused), "{error}");
    }

    let spaced = api
        .issues()
        .piped(1, vec![State::Open], vec!["a|b".into()])
        .await;
    assert!(
        spaced.is_ok(),
        "a key that is not joined holds a `|`: {spaced:?}"
    );
}

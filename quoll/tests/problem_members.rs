//! A handler's problem with a `type` and extension members of its own: answered by the server a contract becomes, as
//! an error and as the `event: problem` that ends a stream, read back by its client, and built by a fake.

#![cfg(all(feature = "server", feature = "client"))]

use futures_util::{StreamExt, stream};
use http::StatusCode;
use quoll::{Call, Error, Events, Problem, Response, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

quoll::api! {
    api Api = "/api";

    group Apps {
        PUT "/apps/{name}/start" => start(name: String);
        GET "/apps/{name}/logs"  => logs(name: String) -> Events<Line>;
        PUT "/apps/{name}/stop"  => stop(name: String);
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Line {
    text: String,
}

/// The extension members of the problem of an app that is running.
#[derive(Debug, PartialEq, Deserialize)]
struct Running {
    app: String,
    since: u32,
}

/// The problem of an app that is running: a 409 of its own `type`, with the app and since when.
fn running(name: &str) -> Error {
    Error::new(StatusCode::CONFLICT, format!("{name} is running"))
        .with_type("https://example.com/problems/app-running")
        .with_member("app", name)
        .with_member("since", 3)
}

struct App;

impl AppsServer for App {
    async fn start(&self, name: String) -> Result<Response<()>> {
        Err(running(&name))
    }

    async fn logs(&self, name: String) -> Result<Response<Events<Line>>> {
        let line = Line {
            text: "started".into(),
        };
        let lines = [Ok(line), Err(running(&name))];

        Ok(Response::new(Events::try_new(stream::iter(lines))))
    }

    async fn stop(&self, name: String) -> Result<Response<()>> {
        let stopping = Error::new(StatusCode::CONFLICT, format!("{name} is stopping"))
            .with_type("/problems/app stopping")
            .with_member("status", "stopping")
            .with_member("app", name);

        Err(stopping)
    }
}

/// The apps of [`running`], in place of a server.
struct Fake;

impl Apps for Fake {
    fn start(&self, name: String) -> impl Call<()> {
        async move { Err(running(&name).into_answered()) }
    }

    fn logs(&self, _name: String) -> impl Call<Events<Line>> {
        async { Err(Error::not_found("no logs in the fake")) }
    }

    fn stop(&self, _name: String) -> impl Call<()> {
        async { Ok(()) }
    }
}

async fn served() -> (String, ApiClient) {
    let url = quoll::test::spawn(Api::router(ApiServices { apps: App })).await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api)
}

/// What code written for the client reads of the error of `start`.
async fn read_start(
    apps: &impl Apps,
) -> (Option<StatusCode>, String, String, Option<Running>, String) {
    let error = apps
        .start("alpha".into())
        .await
        .expect_err("alpha is running");
    let problem = error.detail().expect("a problem");

    (
        error.status(),
        problem.kind().to_owned(),
        problem.detail().unwrap_or_default().to_owned(),
        error.body_as::<Running>(),
        error.to_string(),
    )
}

#[tokio::test]
async fn a_handlers_problem_has_its_type_and_its_members() {
    let (url, api) = served().await;

    let response = reqwest::Client::new()
        .put(format!("{url}/api/apps/alpha/start"))
        .send()
        .await
        .expect("the server answers");
    let content_type = response.headers()[http::header::CONTENT_TYPE].clone();
    let body: Value = response.json().await.expect("JSON");

    assert_eq!(content_type, "application/problem+json");
    assert_eq!(
        body,
        json!({
            "type": "https://example.com/problems/app-running",
            "title": "Conflict",
            "status": 409,
            "detail": "alpha is running",
            "app": "alpha",
            "since": 3,
        })
    );

    let (status, kind, detail, members, message) = read_start(&api.apps()).await;

    assert_eq!(status, Some(StatusCode::CONFLICT));
    assert_eq!(kind, "https://example.com/problems/app-running");
    assert_eq!(detail, "alpha is running");
    assert_eq!(
        members,
        Some(Running {
            app: "alpha".into(),
            since: 3
        })
    );
    assert_eq!(message, "another api answered 409: alpha is running");
}

#[tokio::test]
async fn a_fake_gives_what_the_real_client_reads() {
    let (_, api) = served().await;

    assert_eq!(read_start(&Fake).await, read_start(&api.apps()).await);
}

#[tokio::test]
async fn a_problem_that_ends_events_has_its_type_and_its_members() {
    let (url, api) = served().await;

    let body = reqwest::get(format!("{url}/api/apps/alpha/logs"))
        .await
        .expect("the server answers")
        .text()
        .await
        .expect("the body reads");

    assert!(
        body.ends_with(
            "\n\nevent: problem\ndata: {\"type\":\"https://example.com/problems/app-running\",\"title\":\"Conflict\",\"status\":409,\"detail\":\"alpha is running\",\"app\":\"alpha\",\"since\":3}\n\n"
        ),
        "{body}"
    );

    let items: Vec<_> = api
        .apps()
        .logs("alpha".into())
        .await
        .expect("the logs")
        .collect()
        .await;

    assert_eq!(items.len(), 2);
    assert_eq!(items[0].as_ref().expect("a line").text, "started");

    let error = items[1].as_ref().expect_err("the problem");

    assert_eq!(error.status(), Some(StatusCode::CONFLICT));
    assert_eq!(
        error.detail().map(Problem::kind),
        Some("https://example.com/problems/app-running")
    );
    assert_eq!(
        error.body_as::<Running>(),
        Some(Running {
            app: "alpha".into(),
            since: 3
        })
    );
}

#[tokio::test]
async fn a_refused_type_or_member_leaves_the_error_answered_with_the_rest() {
    let (url, api) = served().await;

    let response = reqwest::Client::new()
        .put(format!("{url}/api/apps/alpha/stop"))
        .send()
        .await
        .expect("the server answers");
    let body: Value = response.json().await.expect("JSON");

    assert_eq!(
        body,
        json!({
            "type": "about:blank",
            "title": "Conflict",
            "status": 409,
            "detail": "alpha is stopping",
            "app": "alpha",
        })
    );

    let error = api.apps().stop("alpha".into()).await.expect_err("a 409");

    assert_eq!(error.status(), Some(StatusCode::CONFLICT));
    assert_eq!(error.detail().map(Problem::kind), Some("about:blank"));
}

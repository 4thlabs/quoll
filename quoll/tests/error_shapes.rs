//! An api that answers its errors in a shape of its own, `#[error(E)]`: what its client reads in an error answer, the
//! nearest `#[error]` to a route winning, its fakes, and its errors carried up into a handler's.

#![cfg(all(feature = "server", feature = "client"))]

use std::fmt;

use gists::Gists as _;
use http::header::{ETAG, IF_NONE_MATCH};
use quoll::rest::{self, Call, Error, Problem, Response, Result};
use reqwest::StatusCode;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// What the third party answers on an error, as GitHub does.
#[derive(Debug, PartialEq, Deserialize)]
pub struct HubError {
    /// What went wrong.
    pub message: String,
    /// The page of the docs that says more.
    pub documentation_url: Option<String>,
}

impl fmt::Display for HubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

/// What its OAuth endpoint answers on an error: another shape, of one route.
#[derive(Debug, PartialEq, Deserialize)]
struct OAuthError {
    error: String,
    error_description: String,
}

impl fmt::Display for OAuthError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.error, self.error_description)
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Repo {
    name: String,
}

mod gists {
    use quoll::rest;

    rest::group! {
        /// A group of another block, which names the api's shape itself.
        #[openapi(none)]
        #[error(super::HubError)]
        pub group Gists {
            GET "/gists/{id}" => get(id: String) -> String;
        }
    }
}

rest::api! {
    /// The third party, which answers its errors as `HubError`.
    #[openapi(none)]
    #[error(HubError)]
    api Hub = "" { gists::Gists };

    group Repos {
        GET "/repos/{name}" => get(name: String) -> Repo;
        GET "/repos/{name}/topics" => topics(name: String, header("If-None-Match") tag: Option<String>) -> Vec<String>;
        #[error(OAuthError)]
        POST "/login/oauth/access_token" => token(code: String) -> String;
    }

    /// A group whose errors are problems, in an api whose errors are not.
    #[error(Problem)]
    group Mirror {
        GET "/mirror/{name}" => get(name: String) -> Repo;
    }
}

/// The third party's message for a request without a valid token.
const BAD_CREDENTIALS: &str =
    r#"{"message":"Bad credentials","documentation_url":"https://docs.example.com/rest"}"#;

/// What a proxy in front of the third party answers when it is down.
const PROXY_PAGE: &str = "<html><body>502 Bad Gateway</body></html>";

/// The tag of the topics, which a call that sends it back is answered 304 for.
const TOPICS_TAG: &str = "\"7\"";

/// The third party, written without the contract.
fn hub() -> axum::Router {
    use axum::http::{HeaderMap, header::CONTENT_TYPE};
    use axum::routing::{get, post};

    let json = [("content-type", "application/json; charset=utf-8")];

    axum::Router::new()
        .route(
            "/repos/quoll",
            get(|| async { axum::Json(serde_json::json!({ "name": "quoll" })) }),
        )
        .route(
            "/repos/secret",
            get(move || async move {
                (
                    StatusCode::UNAUTHORIZED,
                    [("x-ratelimit-remaining", "59")],
                    json,
                    BAD_CREDENTIALS,
                )
            }),
        )
        .route(
            "/repos/down",
            get(|| async {
                (
                    StatusCode::BAD_GATEWAY,
                    [(CONTENT_TYPE, "text/html")],
                    PROXY_PAGE,
                )
            }),
        )
        .route(
            "/repos/quoll/topics",
            get(|headers: HeaderMap| async move {
                let tag = [(ETAG, TOPICS_TAG)];

                if headers.get(IF_NONE_MATCH).is_some_and(|sent| sent == TOPICS_TAG) {
                    return (StatusCode::NOT_MODIFIED, tag, String::new());
                }

                (StatusCode::OK, tag, r#"["rust"]"#.to_owned())
            }),
        )
        .route(
            "/login/oauth/access_token",
            post(move || async move {
                (
                    StatusCode::BAD_REQUEST,
                    json,
                    r#"{"error":"bad_verification_code","error_description":"The code is expired."}"#,
                )
            }),
        )
        .route(
            "/mirror/quoll",
            get(|| async {
                (
                    StatusCode::NOT_FOUND,
                    [(CONTENT_TYPE, "application/problem+json")],
                    r#"{"type":"about:blank","status":404,"detail":"no mirror"}"#,
                )
            }),
        )
        .route(
            "/gists/{id}",
            get(move || async move { (StatusCode::NOT_FOUND, json, r#"{"message":"Not Found"}"#) }),
        )
}

/// `router` served on a port, and its URL.
async fn serve(router: axum::Router) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let url = format!(
        "http://{}",
        listener.local_addr().expect("the listener has an address")
    );

    tokio::spawn(async move { axum::serve(listener, router).await });

    url
}

#[tokio::test]
async fn an_error_answer_reads_as_the_apis_shape() -> Result<()> {
    let hub = Hub::client(&serve(hub()).await)?;

    let found: rest::Result<Repo, HubError> = hub.repos().get("quoll".into()).await;
    let error = hub.repos().get("secret".into()).await.expect_err("a 401");
    let answer = error.response().expect("the answer of a `HubError`");

    assert_eq!(found?.name, "quoll");
    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    assert_eq!(
        error.detail(),
        Some(&HubError {
            message: "Bad credentials".into(),
            documentation_url: Some("https://docs.example.com/rest".into()),
        })
    );
    assert_eq!(answer.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(answer.headers()["x-ratelimit-remaining"], "59");
    assert_eq!(error.body(), Some(BAD_CREDENTIALS.as_bytes()));
    assert_eq!(
        error.to_string(),
        "another api answered 401: Bad credentials"
    );
    Ok(())
}

#[tokio::test]
async fn a_body_that_is_not_the_shape_keeps_its_status_and_its_body() -> Result<()> {
    let hub = Hub::client(&serve(hub()).await)?;

    let error = hub.repos().get("down".into()).await.expect_err("a 502");

    assert_eq!(error.status(), Some(StatusCode::BAD_GATEWAY));
    assert!(error.detail().is_none() && error.response().is_none());
    assert_eq!(error.body(), Some(PROXY_PAGE.as_bytes()));
    assert_eq!(error.to_string(), "another api answered 502");
    Ok(())
}

#[tokio::test]
async fn the_nearest_error_to_a_route_wins() -> Result<()> {
    let hub = Hub::client(&serve(hub()).await)?;

    let token: rest::Result<String, OAuthError> = hub.repos().token("x".into()).await;
    let mirror: rest::Result<Repo> = hub.mirror().get("quoll".into()).await;
    let gist: rest::Result<String, HubError> = hub.gists().get("1".into()).await;

    let token = token.expect_err("a 400");
    let mirror = mirror.expect_err("a 404");
    let gist = gist.expect_err("a 404");

    assert_eq!(
        token.detail().map(|detail| detail.error.as_str()),
        Some("bad_verification_code")
    );
    assert_eq!(
        token.to_string(),
        "another api answered 400: bad_verification_code: The code is expired."
    );
    assert_eq!(mirror.detail().and_then(Problem::detail), Some("no mirror"));
    assert_eq!(
        gist.detail().map(|detail| detail.message.as_str()),
        Some("Not Found")
    );
    Ok(())
}

#[tokio::test]
async fn the_whole_answer_has_the_error_of_the_shape_as_its_body() -> Result<()> {
    let hub = Hub::client(&serve(hub()).await)?;

    let refused = hub.repos().get("secret".into()).response().await?;
    let fresh = hub.repos().topics("quoll".into(), None).response().await?;
    let same = hub
        .repos()
        .topics("quoll".into(), Some(TOPICS_TAG.into()))
        .response()
        .await?;

    assert_eq!(refused.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(refused.headers()["x-ratelimit-remaining"], "59");
    assert_eq!(
        refused
            .into_value()
            .expect_err("a 401")
            .detail()
            .map(|detail| detail.message.as_str()),
        Some("Bad credentials")
    );
    assert_eq!(fresh.headers()[ETAG], TOPICS_TAG);
    assert_eq!(fresh.into_value()?, ["rust"]);
    assert_eq!(same.status(), StatusCode::NOT_MODIFIED);
    assert_eq!(same.headers()[ETAG], TOPICS_TAG);

    let not_modified = same.into_value().expect_err("a 304 is no value");

    assert_eq!(not_modified.status(), Some(StatusCode::NOT_MODIFIED));
    assert!(not_modified.detail().is_none());
    Ok(())
}

/// The third party's repositories in memory, in place of its server.
struct FakeRepos;

impl Repos for FakeRepos {
    fn get(&self, name: String) -> impl Call<Repo, HubError> {
        async move {
            if name == "quoll" {
                return Ok(Repo { name });
            }

            Err(Error::answered(
                StatusCode::NOT_FOUND,
                HubError {
                    message: "Not Found".into(),
                    documentation_url: None,
                },
            ))
        }
    }

    fn topics(&self, _name: String, _tag: Option<String>) -> impl Call<Vec<String>, HubError> {
        async { Ok(vec!["rust".to_owned()]) }
    }

    fn token(&self, code: String) -> impl Call<String, OAuthError> {
        async move {
            if code.is_empty() {
                return Err(Error::bad_request("no code").into_shape());
            }

            Ok("token".to_owned())
        }
    }
}

#[tokio::test]
async fn a_fake_answers_an_error_of_the_shape() -> Result<()> {
    let repos = FakeRepos;

    let error = repos.get("nope".into()).await.expect_err("a 404");
    let response = repos.get("nope".into()).response().await?;

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(
        error.detail().map(|detail| detail.message.as_str()),
        Some("Not Found")
    );
    assert_eq!(error.to_string(), "another api answered 404: Not Found");
    assert_eq!(response.status(), StatusCode::NOT_FOUND);
    assert!(response.into_value().is_err());
    assert_eq!(repos.get("quoll".into()).await?.name, "quoll");

    let refused = repos.token(String::new()).await.expect_err("a 400");

    assert_eq!(refused.status(), Some(StatusCode::BAD_REQUEST));
    assert!(refused.detail().is_none());
    Ok(())
}

rest::api! {
    /// A server of quoll's that calls the third party.
    api Front = "/api";

    group Mirrors {
        GET "/repos/{name}" => get(name: String) -> Repo;
    }
}

/// Calls the third party through its client, carrying its errors up with `?`.
struct Proxy {
    hub: HubClient,
}

impl MirrorsServer for Proxy {
    async fn get(&self, name: String) -> Result<Response<Repo>> {
        let repo = self.hub.repos().get(name).await?;

        Ok(Response::new(repo))
    }
}

#[tokio::test]
async fn an_error_of_the_shape_goes_up_into_a_handlers_error_and_answers_502() -> Result<()> {
    let hub = Hub::client(&serve(hub()).await)?;
    let router = Front::router(FrontServices {
        mirrors: Proxy { hub },
    });
    let front = Front::client(&serve(router).await)?;

    let error = front
        .mirrors()
        .get("secret".into())
        .await
        .expect_err("a 502");

    assert_eq!(front.mirrors().get("quoll".into()).await?.name, "quoll");
    assert_eq!(error.status(), Some(StatusCode::BAD_GATEWAY));
    assert_eq!(error.detail().and_then(Problem::detail), None);
    Ok(())
}

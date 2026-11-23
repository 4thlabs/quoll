//! The fixed headers of a contract, `#[header("name" = value)]` on an api, a group or a route: what its client sends,
//! the nearest to the call winning, a caller's argument first and quoll's own `Accept` last, what `none` leaves, a
//! `Content-Type` that keeps its body's media type, and a group of another block that takes its api's, listed by any
//! api, whatever headers it fixes.

#![cfg(all(feature = "server", feature = "client"))]

use std::collections::BTreeMap;

use gists::Gists as _;
use quoll::rest::{self, Result};
use serde::{Deserialize, Serialize};

/// The headers the third party received, by their names in lowercase.
type Echo = BTreeMap<String, String>;

#[derive(Debug, Serialize, Deserialize)]
struct Repo {
    name: String,
}

/// The version of the third party's api this contract follows.
const VERSION: &str = "2022-11-28";

mod gists {
    use quoll::rest;

    rest::group! {
        /// A group of another block, which takes the headers of the api that lists it.
        #[openapi(none)]
        #[header("X-Group" = "gists")]
        #[header("X-Beta" = none)]
        pub group Gists {
            GET "/gists" => list() -> super::Echo;

            #[header("X-Version" = none)]
            GET "/gists/unversioned" => unversioned() -> super::Echo;

            #[header(none)]
            #[header("X-Route" = "bare")]
            GET "/gists/bare" => bare() -> super::Echo;
        }
    }
}

rest::api! {
    /// A third party that asks for headers of its own on every request.
    #[openapi(none)]
    #[header("Accept" = "application/vnd.hub+json")]
    #[header("X-Version" = VERSION)]
    #[header("X-Beta" = "files")]
    api Hub = "" { gists::Gists };

    group Repos {
        GET "/repos" => list() -> Echo;

        #[header("Accept" = "application/vnd.hub.raw+json")]
        GET "/readme" => readme() -> Echo;

        #[header("Accept" = none)]
        GET "/plain" => plain() -> Echo;

        #[header("X-Beta" = none)]
        GET "/stable" => stable() -> Echo;

        GET "/versioned" => versioned(header("X-Version") version: Option<String>) -> Echo;

        #[header("Content-Type" = "application/json; charset=utf-8")]
        POST "/repos" => create(repo: Repo) -> Echo;
    }

    #[header(none)]
    #[header("X-Group" = "health")]
    group Health {
        GET "/health" => check() -> Echo;

        #[header("X-Group" = none)]
        GET "/health/bare" => bare() -> Echo;
    }
}

rest::api! {
    /// A third party whose JSON bodies say their charset, as Slack's do.
    #[openapi(none)]
    #[header("Content-Type" = "application/json; charset=utf-8")]
    api Chat = "/api";

    group Messages {
        GET "/messages" => list() -> Echo;
        POST "/messages" => post(repo: Repo) -> Echo;
    }
}

rest::api! {
    /// A third party that fixes neither `X-Beta` nor `X-Version`, which the group `Gists` and one of its routes leave.
    #[openapi(none)]
    #[header("X-Other" = "other")]
    api Other = "" { gists::Gists };
}

/// The third party, which answers every request with the headers it received.
fn echo() -> axum::Router {
    use axum::http::HeaderMap;

    axum::Router::new().fallback(|headers: HeaderMap| async move {
        let echo: Echo = headers
            .iter()
            .map(|(name, value)| {
                let value = value.to_str().expect("a header of visible ASCII");

                (name.as_str().to_owned(), value.to_owned())
            })
            .collect();

        axum::Json(echo)
    })
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

/// The value of `name` in `echo`, if it was sent.
fn sent<'a>(echo: &'a Echo, name: &str) -> Option<&'a str> {
    echo.get(name).map(String::as_str)
}

#[tokio::test]
async fn a_route_sends_its_apis_headers() -> Result<()> {
    let hub = Hub::client(&serve(echo()).await)?;

    let echo = hub.repos().list().await?;

    assert_eq!(sent(&echo, "accept"), Some("application/vnd.hub+json"));
    assert_eq!(sent(&echo, "x-version"), Some(VERSION));
    assert_eq!(sent(&echo, "x-beta"), Some("files"));
    Ok(())
}

#[tokio::test]
async fn the_nearest_header_to_the_call_wins() -> Result<()> {
    let hub = Hub::client(&serve(echo()).await)?;

    let readme = hub.repos().readme().await?;
    let given = hub.repos().versioned(Some("2026-01-01".into())).await?;
    let left = hub.repos().versioned(None).await?;

    assert_eq!(
        sent(&readme, "accept"),
        Some("application/vnd.hub.raw+json")
    );
    assert_eq!(sent(&readme, "x-version"), Some(VERSION));
    assert_eq!(sent(&given, "x-version"), Some("2026-01-01"));
    assert_eq!(sent(&left, "x-version"), Some(VERSION));
    Ok(())
}

#[tokio::test]
async fn none_leaves_a_header_or_all_of_them() -> Result<()> {
    let hub = Hub::client(&serve(echo()).await)?;

    let plain = hub.repos().plain().await?;
    let stable = hub.repos().stable().await?;
    let health = hub.health().check().await?;
    let bare = hub.health().bare().await?;

    // `Accept` left, quoll's own is back.
    assert_eq!(sent(&plain, "accept"), Some("application/json"));
    assert_eq!(sent(&stable, "x-beta"), None);
    assert_eq!(sent(&stable, "x-version"), Some(VERSION));
    assert_eq!(sent(&health, "x-group"), Some("health"));
    assert_eq!(sent(&health, "x-version"), None);
    assert_eq!(sent(&health, "accept"), Some("application/json"));
    assert_eq!(sent(&bare, "x-group"), None);
    Ok(())
}

#[tokio::test]
async fn a_content_type_keeps_the_body_and_adds_a_parameter() -> Result<()> {
    let hub = Hub::client(&serve(echo()).await)?;
    let chat = Chat::client(&serve(echo()).await)?;
    let repo = || Repo {
        name: "quoll".into(),
    };

    let created = hub.repos().create(repo()).await?;
    let posted = chat.messages().post(repo()).await?;
    let listed = chat.messages().list().await?;

    assert_eq!(
        sent(&created, "content-type"),
        Some("application/json; charset=utf-8")
    );
    assert_eq!(
        sent(&posted, "content-type"),
        Some("application/json; charset=utf-8")
    );
    // A request with no body has no content type.
    assert_eq!(sent(&listed, "content-type"), None);
    Ok(())
}

#[tokio::test]
async fn a_group_of_another_block_takes_its_apis_headers() -> Result<()> {
    let hub = Hub::client(&serve(echo()).await)?;

    let list = hub.gists().list().await?;
    let bare = hub.gists().bare().await?;

    assert_eq!(sent(&list, "x-group"), Some("gists"));
    assert_eq!(sent(&list, "accept"), Some("application/vnd.hub+json"));
    assert_eq!(sent(&list, "x-version"), Some(VERSION));
    assert_eq!(sent(&list, "x-beta"), None);
    assert_eq!(sent(&bare, "x-route"), Some("bare"));
    assert_eq!(sent(&bare, "x-group"), None);
    assert_eq!(sent(&bare, "accept"), Some("application/json"));
    Ok(())
}

#[tokio::test]
async fn a_group_of_another_block_leaves_nothing_where_its_api_does_not_fix_the_header()
-> Result<()> {
    let hub = Hub::client(&serve(echo()).await)?;
    let other = Other::client(&serve(echo()).await)?;

    let at_hub = hub.gists().unversioned().await?;
    let list = other.gists().list().await?;
    let unversioned = other.gists().unversioned().await?;

    assert_eq!(sent(&at_hub, "x-version"), None);
    assert_eq!(sent(&at_hub, "x-beta"), None);
    assert_eq!(sent(&at_hub, "accept"), Some("application/vnd.hub+json"));
    assert_eq!(sent(&list, "x-other"), Some("other"));
    assert_eq!(sent(&list, "x-group"), Some("gists"));
    assert_eq!(sent(&list, "x-beta"), None);
    assert_eq!(sent(&unversioned, "x-other"), Some("other"));
    assert_eq!(sent(&unversioned, "x-version"), None);
    assert_eq!(sent(&unversioned, "x-group"), Some("gists"));
    assert_eq!(sent(&unversioned, "accept"), Some("application/json"));
    Ok(())
}

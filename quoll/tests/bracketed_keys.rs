//! Literal query keys that hold brackets, `expand[]`, `filter[name]`, `page[size]`: sent by the client the contract
//! becomes to the server it becomes, read from what a browser or curl sends, and named so in the OpenAPI document.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::{Arc, Mutex};

use axum::extract::State as Shared;
use axum::http::Uri;
use axum::routing::get;
use quoll::{Response, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

/// What the server read of a call of `articles`.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
struct Articles {
    name: Option<String>,
    size: Option<u32>,
    fields: Vec<String>,
}

/// A query struct whose fields take bracketed keys by their serde names.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
struct Search {
    #[serde(rename = "filter[name]")]
    name: Option<String>,
    #[serde(rename = "page[size]")]
    size: Option<u32>,
}

quoll::api! {
    api Api = "/api";

    group Shop {
        GET "/customers/{id}?expand[]={expand}" => customer(id: String, expand: Vec<String>) -> Vec<String>;
        GET "/articles?filter[name]={name}&page[size]={size}&fields[articles]={fields}"
            => articles(name: Option<String>, size: Option<u32>, #[join(",")] fields: Vec<String>) -> Articles;
        GET "/search" => search(query filter: Search) -> Search;
    }
}

struct App;

impl ShopServer for App {
    async fn customer(&self, _id: String, expand: Vec<String>) -> Result<Response<Vec<String>>> {
        Ok(Response::new(expand))
    }

    async fn articles(
        &self,
        name: Option<String>,
        size: Option<u32>,
        fields: Vec<String>,
    ) -> Result<Response<Articles>> {
        Ok(Response::new(Articles { name, size, fields }))
    }

    async fn search(&self, filter: Search) -> Result<Response<Search>> {
        Ok(Response::new(filter))
    }
}

/// The api served on a port, its URL and its client.
async fn served() -> (String, ApiClient) {
    let url = quoll::test::spawn(Api::router(ApiServices { shop: App })).await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api)
}

/// What the server answers to a query sent as it is, by a client other than quoll's.
async fn read(url: &str, path_and_query: &str) -> Value {
    let response = reqwest::get(format!("{url}/api{path_and_query}"))
        .await
        .expect("the server answers");

    assert!(response.status().is_success(), "{path_and_query}");

    response.json().await.expect("the body is JSON")
}

#[tokio::test]
async fn bracketed_keys_round_trip() {
    let (_, api) = served().await;

    assert_eq!(
        api.shop()
            .customer(
                "cus_1".into(),
                vec!["sources".into(), "invoice.subscription".into()]
            )
            .await
            .unwrap(),
        ["sources", "invoice.subscription"]
    );
    assert_eq!(
        api.shop()
            .articles(
                Some("rust".into()),
                Some(20),
                vec!["title".into(), "body".into()]
            )
            .await
            .unwrap(),
        Articles {
            name: Some("rust".into()),
            size: Some(20),
            fields: vec!["title".into(), "body".into()],
        }
    );
    assert_eq!(
        api.shop().articles(None, None, Vec::new()).await.unwrap(),
        Articles::default()
    );

    let search = Search {
        name: Some("a b".into()),
        size: Some(5),
    };
    assert_eq!(api.shop().search(search.clone()).await.unwrap(), search);
}

#[tokio::test]
async fn the_server_reads_brackets_as_they_are_or_escaped() {
    let (url, _) = served().await;

    // As curl sends them with `--globoff`, and as a server-side client may.
    assert_eq!(
        read(&url, "/customers/c?expand[]=a&expand[]=b").await,
        json!(["a", "b"])
    );
    // As a browser's `URLSearchParams` writes them, which orval's clients use.
    assert_eq!(
        read(&url, "/customers/c?expand%5B%5D=a&expand%5b%5d=b").await,
        json!(["a", "b"])
    );
    assert_eq!(
        read(
            &url,
            "/articles?filter[name]=x&page%5Bsize%5D=3&fields[articles]=a,b"
        )
        .await,
        json!({"name": "x", "size": 3, "fields": ["a", "b"]})
    );
    // A key without its brackets is another key, which the route does not read.
    assert_eq!(read(&url, "/customers/c?expand=a").await, json!([]));
    // A query struct's field reads its key by its serde name.
    assert_eq!(
        read(&url, "/search?filter[name]=x&page%5Bsize%5D=3").await,
        json!({"filter[name]": "x", "page[size]": 3})
    );
}

#[tokio::test]
async fn the_client_escapes_the_brackets_of_a_key() {
    let queries = Arc::new(Mutex::new(Vec::new()));
    let record = |Shared(queries): Shared<Arc<Mutex<Vec<String>>>>, uri: Uri| async move {
        let query = uri.query().unwrap_or_default().to_owned();

        queries
            .lock()
            .expect("no test panics while holding it")
            .push(query);
    };
    let articles = move |queries, uri| async move {
        record(queries, uri).await;

        axum::Json(Articles::default())
    };
    let search = move |queries, uri| async move {
        record(queries, uri).await;

        axum::Json(Search::default())
    };
    let router = axum::Router::new()
        .route("/api/articles", get(articles))
        .route("/api/search", get(search))
        .with_state(Arc::clone(&queries));
    let url = quoll::test::spawn(router).await;
    let api = Api::client(&url).expect("the url is the api's");

    api.shop()
        .articles(
            Some("a b".into()),
            Some(2),
            vec!["title".into(), "body".into()],
        )
        .await
        .unwrap();
    api.shop()
        .search(Search {
            name: Some("a b".into()),
            size: Some(2),
        })
        .await
        .unwrap();

    let queries = queries.lock().expect("no test panics while holding it");
    assert_eq!(
        *queries,
        [
            "filter%5Bname%5D=a+b&page%5Bsize%5D=2&fields%5Barticles%5D=title,body",
            "filter%5Bname%5D=a+b&page%5Bsize%5D=2",
        ]
    );
}

#[test]
fn the_document_names_a_parameter_by_its_key() {
    let document: Value = serde_json::from_str(&Api::openapi().to_json()).unwrap();
    let names = |path: &str| -> Vec<Value> {
        document["paths"][path]["get"]["parameters"]
            .as_array()
            .expect("the route has parameters")
            .iter()
            .filter(|parameter| parameter["in"] == "query")
            .map(|parameter| parameter["name"].clone())
            .collect()
    };

    assert_eq!(names("/api/customers/{id}"), [json!("expand[]")]);
    assert_eq!(
        names("/api/articles"),
        [
            json!("filter[name]"),
            json!("page[size]"),
            json!("fields[articles]")
        ]
    );
    assert_eq!(
        names("/api/search"),
        [json!("filter[name]"), json!("page[size]")]
    );
}

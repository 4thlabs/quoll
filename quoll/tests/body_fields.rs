//! The fixed fields of a body, `#[body("name" = value)]` on a route: what the client adds to its JSON and to its
//! form, a field of the body's type of the same name replaced, and how the server picks, among two routes of one
//! method and path, the one whose fields the body holds, the handler's type reading the body without them.

#![cfg(all(feature = "server", feature = "client"))]

use std::collections::BTreeMap;

use quoll::rest::{self, Form, Response, Result};
use reqwest::StatusCode;
use serde::{Deserialize, Serialize};

/// A message to send, which says nothing of whether it streams.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, schemars::JsonSchema)]
pub struct MessageRequest {
    model: String,
}

/// A request whose type has a field the contract fixes too.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Overriding {
    model: String,
    stream: bool,
}

/// A form entry.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Entry {
    title: String,
}

/// A request whose type refuses any field it does not know.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Strict {
    model: String,
}

/// Which route answered, and what its handler read.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Answered {
    route: String,
    model: String,
}

rest::api! {
    /// Another API whose one path answers twice, as its body says it streams or not.
    #[openapi(none)]
    api Messages = "/v1";

    group Chat {
        GET "/messages" => history() -> Answered;

        #[body("stream" = false)]
        POST "/messages" => create(request: MessageRequest) -> Answered;

        #[body("stream" = true)]
        POST "/messages" => stream(request: MessageRequest) -> Answered;

        #[body("stream" = true)]
        POST "/override" => overriding(request: Overriding) -> Answered;

        #[body("kind" = "note")]
        #[body("n" = 2)]
        POST "/entries" => entry(entry: Form<Entry>) -> Answered;

        #[body("kind" = "note")]
        #[body("n" = -1)]
        POST "/entries" => old_entry(entry: Form<Entry>) -> Answered;

        #[body("stream" = true)]
        POST "/strict" => strict(request: Strict) -> Answered;

        #[body("stream" = true)]
        POST "/list" => list(items: Vec<u32>);
    }
}

rest::api! {
    /// An api with a document, whose route alone on its path fixes a field.
    api Documented = "/api";

    group Streams {
        #[body("stream" = true)]
        #[body("version" = 1.5)]
        POST "/messages" => stream(request: MessageRequest);
    }
}

/// The service of `Messages`, which says which route answered.
struct Service;

/// What `route` answers, with the model its handler read.
fn answered(route: &str, model: String) -> Result<Response<Answered>> {
    Ok(Response::new(Answered {
        route: route.to_owned(),
        model,
    }))
}

impl ChatServer for Service {
    async fn history(&self) -> Result<Response<Answered>> {
        answered("history", String::new())
    }

    async fn create(&self, request: MessageRequest) -> Result<Response<Answered>> {
        answered("create", request.model)
    }

    async fn stream(&self, request: MessageRequest) -> Result<Response<Answered>> {
        answered("stream", request.model)
    }

    async fn overriding(&self, request: Overriding) -> Result<Response<Answered>> {
        answered(&format!("overriding {}", request.stream), request.model)
    }

    async fn entry(&self, entry: Form<Entry>) -> Result<Response<Answered>> {
        answered("entry", entry.0.title)
    }

    async fn old_entry(&self, entry: Form<Entry>) -> Result<Response<Answered>> {
        answered("old_entry", entry.0.title)
    }

    async fn strict(&self, request: Strict) -> Result<Response<Answered>> {
        answered("strict", request.model)
    }

    async fn list(&self, _items: Vec<u32>) -> Result<Response<()>> {
        Ok(Response::new(()))
    }
}

/// The body a request carried, as the third party received it.
#[derive(Debug, Deserialize)]
struct Received {
    content_type: String,
    body: String,
}

/// A third party that answers every request with its content type and its body, in an error's answer, which the
/// client keeps as it is.
async fn echo() -> String {
    use axum::http::HeaderMap;

    let router = axum::Router::new().fallback(|headers: HeaderMap, body: String| async move {
        let content_type = headers
            .get("content-type")
            .and_then(|value| value.to_str().ok())
            .unwrap_or_default()
            .to_owned();

        let echo = serde_json::json!({ "content_type": content_type, "body": body });

        (axum::http::StatusCode::IM_A_TEAPOT, axum::Json(echo))
    });

    quoll::test::spawn(router).await
}

/// What `call` sent, read from the echo of a third party, the body of the error it returns.
async fn sent<T: std::fmt::Debug>(call: impl rest::Call<T>) -> Received {
    let response = call.response().await.expect("the echo answers");
    let error = response.into_value().expect_err("the echo is an error");
    let body = error.body().expect("the echo has a body");

    serde_json::from_slice(body).expect("the echo is JSON")
}

#[tokio::test]
async fn the_client_adds_the_fields_to_its_json_and_its_form() -> Result<()> {
    let url = echo().await;
    let api = Messages::client(&url)?;
    let request = MessageRequest {
        model: "m".to_owned(),
    };

    let created = sent(api.chat().create(request.clone())).await;
    let streamed = sent(api.chat().stream(request)).await;
    let entry = sent(api.chat().entry(Form(Entry {
        title: "a b".to_owned(),
    })))
    .await;

    assert_eq!(created.content_type, "application/json");
    assert_eq!(created.body, r#"{"stream":false,"model":"m"}"#);
    assert_eq!(streamed.body, r#"{"stream":true,"model":"m"}"#);
    assert_eq!(entry.content_type, "application/x-www-form-urlencoded");
    assert_eq!(entry.body, "kind=note&n=2&title=a+b");
    Ok(())
}

#[tokio::test]
async fn a_field_of_the_body_of_the_same_name_is_replaced() -> Result<()> {
    let url = echo().await;
    let api = Messages::client(&url)?;
    let request = Overriding {
        model: "m".to_owned(),
        stream: false,
    };

    let received = sent(api.chat().overriding(request)).await;

    assert_eq!(received.body, r#"{"stream":true,"model":"m"}"#);
    Ok(())
}

#[tokio::test]
async fn a_body_that_is_no_struct_or_map_is_refused_before_it_is_sent() -> Result<()> {
    let url = echo().await;
    let api = Messages::client(&url)?;

    let error = api
        .chat()
        .list(vec![1, 2])
        .await
        .expect_err("a list is refused");
    let sources = format!("{:?}", std::error::Error::source(&error));

    assert!(error.to_string().contains("chat_list"), "{error}");
    assert!(sources.contains("this one is a sequence"), "{sources}");
    Ok(())
}

/// The `Messages` server, served on a port, and its URL.
async fn serve() -> String {
    let router = Messages::router(MessagesServices { chat: Service });

    quoll::test::spawn(router).await
}

#[tokio::test]
async fn the_server_picks_the_route_by_the_field_its_handler_does_not_read() -> Result<()> {
    let api = Messages::client(&serve().await)?;
    let request = MessageRequest {
        model: "m".to_owned(),
    };

    let created = api.chat().create(request.clone()).await?;
    let streamed = api.chat().stream(request).await?;
    let entry = api
        .chat()
        .entry(Form(Entry {
            title: "t".to_owned(),
        }))
        .await?;
    let old_entry = api
        .chat()
        .old_entry(Form(Entry {
            title: "t".to_owned(),
        }))
        .await?;

    assert_eq!(api.chat().history().await?.route, "history");
    assert_eq!(created.route, "create");
    assert_eq!(created.model, "m");
    assert_eq!(streamed.route, "stream");
    assert_eq!(entry.route, "entry");
    assert_eq!(old_entry.route, "old_entry");
    Ok(())
}

/// The status and the body of a request of `body` to `path`, of `content_type`, sent as any client would.
async fn post(url: &str, path: &str, content_type: &str, body: &str) -> (StatusCode, String) {
    let response = reqwest::Client::new()
        .post(format!("{url}/v1{path}"))
        .header("content-type", content_type)
        .body(body.to_owned())
        .send()
        .await
        .expect("the server answers");
    let status = response.status();

    (
        status,
        response.text().await.expect("the answer has a body"),
    )
}

#[tokio::test]
async fn a_body_without_the_fields_of_any_route_is_a_bad_request_that_names_none() {
    let url = serve().await;
    let json = "application/json";

    let (missing, missing_body) = post(&url, "/messages", json, r#"{"model":"m"}"#).await;
    let (twice, _) = post(&url, "/messages", json, r#"{"stream":true,"stream":true}"#).await;
    let (alone, alone_body) =
        post(&url, "/override", json, r#"{"model":"m","stream":false}"#).await;
    let (form, _) = post(
        &url,
        "/entries",
        "application/x-www-form-urlencoded",
        "kind=note&n=3&title=t",
    )
    .await;

    assert_eq!(missing, StatusCode::BAD_REQUEST);
    assert!(
        missing_body
            .contains("does not hold the fixed fields of any route of this method and path"),
        "{missing_body}"
    );
    assert!(!missing_body.contains("chat_"), "{missing_body}");
    assert_eq!(twice, StatusCode::BAD_REQUEST);
    assert_eq!(alone, StatusCode::BAD_REQUEST);
    assert!(alone_body.contains(r#"\"stream\": true"#), "{alone_body}");
    assert_eq!(form, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn a_body_that_does_not_read_is_a_bad_request_of_its_own() {
    let url = serve().await;
    let json = "application/json";

    let (syntax, syntax_body) = post(&url, "/messages", json, r#"{"stream":tru"#).await;
    let (kind, kind_body) = post(&url, "/messages", json, r#"{"model":"m","stream":"yes"}"#).await;
    let (alone, alone_body) = post(&url, "/override", json, r#"{"stream":tru"#).await;

    for (status, body) in [
        (syntax, syntax_body),
        (kind, kind_body),
        (alone, alone_body),
    ] {
        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert!(body.contains("the body is not valid"), "{body}");
    }
}

#[tokio::test]
async fn a_content_type_no_route_reads_is_refused_with_the_ones_they_read() {
    let url = serve().await;
    let form = "application/x-www-form-urlencoded";

    let (text, text_body) = post(&url, "/messages", "text/plain", r#"{"stream":true}"#).await;
    let (json, json_body) = post(&url, "/entries", "application/json", r#"{"kind":"note"}"#).await;
    let (latin, latin_body) = post(
        &url,
        "/entries",
        &format!("{form}; charset=iso-8859-1"),
        "kind=note&n=2",
    )
    .await;

    assert_eq!(text, StatusCode::UNSUPPORTED_MEDIA_TYPE);
    assert!(
        text_body.contains("read as `application/json`:"),
        "{text_body}"
    );
    assert_eq!(json, StatusCode::UNSUPPORTED_MEDIA_TYPE);
    assert!(
        json_body.contains(&format!("read as `{form}`:")),
        "{json_body}"
    );
    assert_eq!(latin, StatusCode::UNSUPPORTED_MEDIA_TYPE);
    assert!(latin_body.contains("charset=utf-8"), "{latin_body}");
}

#[tokio::test]
async fn a_type_that_refuses_unknown_fields_refuses_the_fixed_one() -> Result<()> {
    let api = Messages::client(&serve().await)?;

    let error = api
        .chat()
        .strict(Strict {
            model: "m".to_owned(),
        })
        .await
        .expect_err("the type refuses the field");
    let detail = error.detail().and_then(|problem| problem.detail());

    assert_eq!(error.status(), Some(StatusCode::BAD_REQUEST));
    assert!(
        format!("{detail:?}").contains("unknown field `stream`"),
        "{detail:?}"
    );
    Ok(())
}

#[test]
fn the_document_describes_a_fixed_field_as_a_const_property_of_its_body() {
    let document = serde_json::to_value(Documented::openapi()).expect("the document is JSON");
    let schema = &document["paths"]["/api/messages"]["post"]["requestBody"]["content"]["application/json"]
        ["schema"];
    let fields: BTreeMap<String, serde_json::Value> =
        serde_json::from_value(schema["allOf"][1]["properties"].clone()).expect("an object");

    assert_eq!(
        schema["allOf"][0]["$ref"],
        "#/components/schemas/MessageRequest"
    );
    assert_eq!(fields["stream"], serde_json::json!({ "const": true }));
    assert_eq!(fields["version"], serde_json::json!({ "const": 1.5 }));
    assert_eq!(
        schema["allOf"][1]["required"],
        serde_json::json!(["stream", "version"])
    );
}

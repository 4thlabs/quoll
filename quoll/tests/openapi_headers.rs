//! The fixed headers of a contract in its OpenAPI document, whatever the features: each one a route sends, its api's,
//! its group's or its own, a header parameter of its value, `const`, that a caller need not send, and nothing for a
//! header a group leaves that its api does not fix.

use std::sync::LazyLock;

use serde_json::{Value, json};

/// The version of the api the contract follows.
const VERSION: &str = "2023-06-01";

mod files {
    quoll::group! {
        /// A group of another block, which takes its api's headers.
        #[header("X-Beta" = none)]
        pub group Files {
            GET "/files" => list() -> Vec<String>;
        }
    }
}

quoll::api! {
    #[header("X-Version" = VERSION)]
    #[header("X-Beta" = "files-2025-04-14")]
    #[header("Accept" = "application/vnd.example+json")]
    #[header("Content-Type" = "application/json; charset=utf-8")]
    pub api Api = "/v1" { files::Files };

    pub group Messages {
        POST "/messages" => create(text: String) -> u32;

        #[header("X-Beta" = "tokens-2025-01-01")]
        POST "/messages/count" => count(text: String) -> u32;

        // The argument describes the header, which a caller may send.
        GET "/messages" => list(header("X-Version") version: Option<String>) -> Vec<String>;

        #[header(none)]
        GET "/health" => health();
    }
}

quoll::api! {
    /// An api that does not fix the `X-Beta` its group leaves, which leaves nothing.
    #[header("X-Version" = VERSION)]
    pub api Other = "/v2" { files::Files };
}

static DOCUMENT: LazyLock<Value> =
    LazyLock::new(|| serde_json::from_str(&Api::openapi().to_json()).unwrap());

/// The parameters of an operation.
fn parameters(path: &str, method: &str) -> &'static Value {
    &DOCUMENT["paths"][path][method]["parameters"]
}

/// A fixed header, as the document describes it.
fn fixed(name: &str, value: &str) -> Value {
    json!({ "name": name, "in": "header", "required": false, "schema": { "type": "string", "const": value } })
}

#[test]
fn the_document_is_valid_openapi_3_1() {
    let schema: Value = serde_json::from_str(include_str!("openapi/schema-3.1.json")).unwrap();
    let validator = jsonschema::validator_for(&schema).unwrap();
    let errors: Vec<String> = validator
        .iter_errors(&DOCUMENT)
        .map(|error| format!("{} at {}", error, error.instance_path()))
        .collect();

    assert!(errors.is_empty(), "{errors:#?}");
}

#[test]
fn each_fixed_header_a_route_sends_is_a_parameter_of_its_value() {
    // `Accept` and `Content-Type` are what OpenAPI says of the answer and of the body, not parameters.
    assert_eq!(
        parameters("/v1/messages", "post"),
        &json!([
            fixed("X-Version", VERSION),
            fixed("X-Beta", "files-2025-04-14")
        ])
    );
    assert_eq!(
        parameters("/v1/messages/count", "post"),
        &json!([
            fixed("X-Beta", "tokens-2025-01-01"),
            fixed("X-Version", VERSION)
        ])
    );
    assert_eq!(parameters("/v1/health", "get"), &Value::Null);
}

#[test]
fn a_header_an_argument_reads_is_described_by_the_argument() {
    assert_eq!(
        parameters("/v1/messages", "get"),
        &json!([
            { "name": "X-Version", "in": "header", "required": false, "schema": { "type": "string" } },
            fixed("X-Beta", "files-2025-04-14"),
        ])
    );
}

#[test]
fn a_group_of_another_block_takes_its_apis_headers() {
    assert_eq!(
        parameters("/v1/files", "get"),
        &json!([fixed("X-Version", VERSION)])
    );
}

#[test]
fn a_group_listed_by_an_api_that_does_not_fix_what_it_leaves_adds_nothing() {
    let document: Value = serde_json::from_str(&Other::openapi().to_json()).unwrap();

    assert_eq!(
        document["paths"]["/v2/files"]["get"]["parameters"],
        json!([fixed("X-Version", VERSION)])
    );
}

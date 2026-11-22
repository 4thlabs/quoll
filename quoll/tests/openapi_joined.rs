//! A list joined into the one value of a query key in the OpenAPI document, whatever the features: a parameter of
//! the style its separator says, `explode: false`, never required, whose schema is the list's.

use std::sync::LazyLock;

use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Debug, Clone, Copy, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum State {
    Open,
    Closed,
}

/// A query struct whose lists are joined.
#[rest::query]
#[derive(Debug, Default, Serialize, Deserialize, JsonSchema)]
#[serde(default)]
struct IssueFilter {
    /// The labels an issue has all of.
    #[join(",")]
    labels: Vec<String>,
    #[join(" ")]
    #[serde(rename = "milestone")]
    milestones: Vec<u32>,
    #[join("|")]
    states: Vec<State>,
    tag: Vec<String>,
    #[join(",")]
    #[serde(default = "open")]
    opened: Vec<State>,
}

/// A result with a property of the name of the mark, which is no mark.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Flags {
    #[serde(rename = "x-quoll-join")]
    flag: bool,
}

/// The states a search reads when none is given.
fn open() -> Vec<State> {
    vec![State::Open]
}

rest::api! {
    api Api = "/api";

    group Issues {
        GET "/issues?labels={labels}&ids={ids}&states={states}&tag={tags}"
            => list(
                #[join(",")] labels: Vec<String>,
                #[join(" ")] ids: Vec<u32>,
                #[join("|")] states: Vec<State>,
                tags: Vec<String>,
            ) -> Vec<String>;
        GET "/search" => search(query filter: IssueFilter) -> IssueFilter;
        GET "/flags" => flags() -> Flags;
    }
}

static DOCUMENT: LazyLock<Value> =
    LazyLock::new(|| serde_json::from_str(&Api::openapi().to_json()).unwrap());

/// The parameter of `/api/issues` named `name`.
fn parameter(name: &str) -> &'static Value {
    parameter_of("/api/issues", name)
}

/// The parameter of `path` named `name`.
fn parameter_of(path: &str, name: &str) -> &'static Value {
    DOCUMENT["paths"][path]["get"]["parameters"]
        .as_array()
        .expect("the route has parameters")
        .iter()
        .find(|parameter| parameter["name"] == name)
        .expect("a parameter of that name")
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
fn a_joined_list_is_a_parameter_of_its_style_not_exploded() {
    assert_eq!(
        *parameter("labels"),
        json!({
            "name": "labels",
            "in": "query",
            "required": false,
            "style": "form",
            "explode": false,
            "schema": { "type": "array", "items": { "type": "string" } },
        })
    );
    assert_eq!(
        *parameter("ids"),
        json!({
            "name": "ids",
            "in": "query",
            "required": false,
            "style": "spaceDelimited",
            "explode": false,
            "schema": {
                "type": "array",
                "items": { "type": "integer", "format": "uint32", "minimum": 0 },
            },
        })
    );
    assert_eq!(parameter("states")["style"], "pipeDelimited");
    assert_eq!(parameter("states")["explode"], false);
    assert_eq!(
        parameter("states")["schema"]["items"],
        json!({ "$ref": "#/components/schemas/State" })
    );
}

#[test]
fn a_list_that_is_not_joined_repeats_its_key_as_before() {
    let tag = parameter("tag");

    assert_eq!(tag.get("style"), None);
    assert_eq!(tag.get("explode"), None);
    assert_eq!(tag["schema"]["type"], "array");
}

#[test]
fn a_joined_field_of_a_query_struct_is_a_parameter_of_its_style() {
    assert_eq!(
        *parameter_of("/api/search", "labels"),
        json!({
            "name": "labels",
            "in": "query",
            "required": false,
            "description": "The labels an issue has all of.",
            "style": "form",
            "explode": false,
            "schema": { "type": "array", "items": { "type": "string" } },
        })
    );
    assert_eq!(
        *parameter_of("/api/search", "milestone"),
        json!({
            "name": "milestone",
            "in": "query",
            "required": false,
            "style": "spaceDelimited",
            "explode": false,
            "schema": {
                "type": "array",
                "items": { "type": "integer", "format": "uint32", "minimum": 0 },
            },
        })
    );
    assert_eq!(
        parameter_of("/api/search", "states")["style"],
        "pipeDelimited"
    );
    assert_eq!(
        parameter_of("/api/search", "states")["schema"]["items"],
        json!({ "$ref": "#/components/schemas/State" })
    );

    let tag = parameter_of("/api/search", "tag");
    assert_eq!((tag.get("style"), tag.get("explode")), (None, None));
}

#[test]
fn a_joined_field_is_described_as_its_text_where_the_struct_is_json() {
    let labels = &DOCUMENT["components"]["schemas"]["IssueFilter"]["properties"]["labels"];

    assert_eq!(labels["type"], json!(["string", "null"]), "{labels}");
    assert_eq!(labels["description"], "The labels an issue has all of.");
}

#[test]
fn a_joined_field_with_a_default_is_described_with_the_lists_schema() {
    let opened = parameter_of("/api/search", "opened");

    assert_eq!(
        opened["schema"],
        json!({ "type": "array", "items": { "$ref": "#/components/schemas/State" } }),
        "{opened}"
    );
}

#[test]
fn the_mark_of_a_joined_field_is_left_in_no_schema() {
    let search = DOCUMENT["paths"]["/api/search"].to_string();
    let filter = DOCUMENT["components"]["schemas"]["IssueFilter"].to_string();

    assert!(!search.contains("x-quoll-join"), "{search}");
    assert!(!filter.contains("x-quoll-join"), "{filter}");
}

#[test]
fn a_property_of_the_name_of_the_mark_is_kept() {
    let flags = &DOCUMENT["components"]["schemas"]["Flags"];

    assert_eq!(
        flags["properties"]["x-quoll-join"],
        json!({ "type": "boolean" }),
        "{flags}"
    );
}

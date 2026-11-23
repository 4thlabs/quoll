//! A list joined into the one value of a query key in the OpenAPI document, whatever the features: a parameter of
//! the style its separator says, `explode: false`, never required, whose schema is the list's.

use std::sync::LazyLock;

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Debug, Clone, Copy, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum State {
    Open,
    Closed,
}

quoll::api! {
    api Api = "/api";

    group Issues {
        GET "/issues?labels={labels}&ids={ids}&states={states}&tag={tags}"
            => list(
                #[join(",")] labels: Vec<String>,
                #[join(" ")] ids: Vec<u32>,
                #[join("|")] states: Vec<State>,
                tags: Vec<String>,
            ) -> Vec<String>;
    }
}

static DOCUMENT: LazyLock<Value> =
    LazyLock::new(|| serde_json::from_str(&Api::openapi().to_json()).unwrap());

/// The parameter of `/api/issues` named `name`.
fn parameter(name: &str) -> &'static Value {
    DOCUMENT["paths"]["/api/issues"]["get"]["parameters"]
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

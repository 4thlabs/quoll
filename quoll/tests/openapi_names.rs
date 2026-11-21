//! Two types of one schemars name, one of which only one side of the document sees: what the document guarantees is
//! that it is valid and that each reference points at the schema of its own type on its own side, whatever the names
//! it gives them, which may mislead (`#[schemars(rename = "...")]` avoids it).

use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;

mod old {
    /// A note of the old shape, which only a result holds.
    #[derive(Default, serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
    pub struct Note {
        pub x: i64,
    }
}

mod new {
    /// A note of the new shape, which both sides hold.
    #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
    pub struct Note {
        pub y: String,
    }
}

/// A type that writes an old note but never reads one.
#[derive(Serialize, Deserialize, JsonSchema)]
struct Holder {
    #[serde(skip_deserializing)]
    old: old::Note,
    n: i64,
}

rest::api! {
    api Api = "/api";

    group Notes {
        PUT "/holder" => holder(holder: Holder) -> Holder;
        PUT "/note"   => note(note: new::Note) -> new::Note;
        PUT "/notes"  => notes(notes: Vec<new::Note>) -> Vec<new::Note>;
    }
}

/// The schema `reference` points at in `document`.
fn resolve<'a>(document: &'a Value, reference: &Value) -> &'a Value {
    let name = reference["$ref"]
        .as_str()
        .and_then(|reference| reference.strip_prefix("#/components/schemas/"))
        .unwrap_or_else(|| panic!("not a reference to a schema: {reference}"));
    let schema = &document["components"]["schemas"][name];
    assert!(schema.is_object(), "no schema {name}");

    schema
}

fn properties(schema: &Value) -> Vec<&str> {
    schema["properties"]
        .as_object()
        .map(|properties| properties.keys().map(String::as_str).collect())
        .unwrap_or_default()
}

#[test]
fn each_reference_points_at_its_own_types_schema_on_its_own_side() {
    let document: Value = serde_json::from_str(&Api::openapi().to_json()).unwrap();
    let schema: Value = serde_json::from_str(include_str!("openapi/schema-3.1.json")).unwrap();
    let validator = jsonschema::validator_for(&schema).unwrap();
    assert!(validator.is_valid(&document), "{document}");

    let body = |path: &str| {
        &document["paths"][path]["put"]["requestBody"]["content"]["application/json"]["schema"]
    };
    let result = |path: &str| {
        &document["paths"][path]["put"]["responses"]["200"]["content"]["application/json"]["schema"]
    };

    // The new note, read and written.
    assert_eq!(properties(resolve(&document, body("/api/note"))), ["y"]);
    assert_eq!(properties(resolve(&document, result("/api/note"))), ["y"]);
    assert_eq!(
        properties(resolve(&document, &body("/api/notes")["items"])),
        ["y"]
    );
    assert_eq!(
        properties(resolve(&document, &result("/api/notes")["items"])),
        ["y"]
    );

    // The holder reads no old note, and writes one, of the old shape.
    let read = resolve(&document, body("/api/holder"));
    let written = resolve(&document, result("/api/holder"));
    assert_eq!(properties(read), ["n"]);
    assert_eq!(properties(written), ["n", "old"]);
    // Its default and `readOnly` beside the reference make it an `allOf`.
    assert_eq!(
        properties(resolve(
            &document,
            &written["properties"]["old"]["allOf"][0]
        )),
        ["x"]
    );
}

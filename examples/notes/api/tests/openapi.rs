use std::path::Path;

use serde_json::Value;

const SNAPSHOT: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/openapi.snapshot.json");

#[test]
fn the_document_is_the_snapshot() {
    let written = notes_api::Api::openapi().to_json_pretty() + "\n";

    if std::env::var_os("SNAPSHOT").is_some_and(|value| value == "overwrite") {
        std::fs::write(SNAPSHOT, &written).expect("the snapshot is written");
        return;
    }

    let snapshot = std::fs::read_to_string(Path::new(SNAPSHOT))
        .expect("the snapshot is there: write it with SNAPSHOT=overwrite");
    // Compared as JSON, so that the order of the keys, which a feature of serde_json changes, does not count.
    let expected: Value = serde_json::from_str(&snapshot).expect("the snapshot is JSON");
    let actual: Value = serde_json::from_str(&written).expect("the document is JSON");

    assert!(
        actual == expected,
        "the document is not the snapshot: run `SNAPSHOT=overwrite cargo test -p notes-api --test openapi` and review its diff"
    );
}

#[test]
fn the_document_is_valid_openapi_3_1() {
    let schema: Value = serde_json::from_str(include_str!(
        "../../../../quoll/tests/openapi/schema-3.1.json"
    ))
    .expect("the schema of OpenAPI 3.1 is JSON");
    let validator = jsonschema::validator_for(&schema).expect("the schema of OpenAPI 3.1 compiles");
    let document: Value =
        serde_json::from_str(&notes_api::Api::openapi().to_json()).expect("the document is JSON");

    let errors: Vec<String> = validator
        .iter_errors(&document)
        .map(|error| format!("{error} at {}", error.instance_path()))
        .collect();
    assert!(errors.is_empty(), "{errors:#?}");

    let schemas = document["components"]["schemas"]
        .as_object()
        .expect("the document has schemas");
    for (name, schema) in schemas {
        assert!(
            jsonschema::draft202012::meta::is_valid(schema),
            "the schema {name} is not JSON Schema 2020-12: {schema}"
        );
    }
}

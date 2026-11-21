//! The schemas of the document that hold a `$ref` beside other keywords, which JSON Schema 2020-12 allows but
//! generators that follow OpenAPI 3.0 read as the `$ref` alone, dropping the rest: each is rewritten as an `allOf` of
//! the reference and of the other keywords.
//!
//! schemars writes one for a newtype variant of an internally tagged enum, the variant's struct beside its tag, and for
//! a field whose type is a struct and which has a `///` comment or a default. The annotations stay on the outer
//! schema, where generators read a field's comment, its default and whether it is read or written only.

use serde_json::{Map, Value, json};

/// The keywords that annotate a schema rather than constrain it, which stay beside the `allOf`.
const ANNOTATIONS: [&str; 8] = [
    "title",
    "description",
    "default",
    "examples",
    "deprecated",
    "readOnly",
    "writeOnly",
    "$comment",
];

/// The keywords whose value is a schema.
const SCHEMA: [&str; 11] = [
    "items",
    "additionalProperties",
    "unevaluatedProperties",
    "unevaluatedItems",
    "contains",
    "propertyNames",
    "not",
    "if",
    "then",
    "else",
    "contentSchema",
];

/// The keywords whose value is an array of schemas.
const ARRAY: [&str; 4] = ["allOf", "anyOf", "oneOf", "prefixItems"];

/// The keywords whose value is a map of schemas.
const MAP: [&str; 4] = [
    "properties",
    "patternProperties",
    "dependentSchemas",
    "$defs",
];

/// Rewrites each schema of `document` that holds a `$ref` beside other keywords: under `components/schemas`, and in
/// the parameters, bodies, results and `x-item` of its operations and of its components.
pub(super) fn rewrite(document: &mut Value) {
    if let Some(paths) = document.get_mut("paths").and_then(Value::as_object_mut) {
        for item in paths.values_mut() {
            path_item(item);
        }
    }

    let Some(components) = document.get_mut("components") else {
        return;
    };

    for_each(components.get_mut("schemas"), schema);
    for_each(components.get_mut("responses"), response);
    for_each(components.get_mut("parameters"), parameter);
    for_each(components.get_mut("requestBodies"), body);
    for_each(components.get_mut("headers"), parameter);
}

/// Calls `f` on each value of `map`, when it is one.
fn for_each(map: Option<&mut Value>, f: fn(&mut Value)) {
    let Some(map) = map.and_then(Value::as_object_mut) else {
        return;
    };

    for value in map.values_mut() {
        f(value);
    }
}

/// A path's item: its parameters and its operations.
fn path_item(item: &mut Value) {
    let Some(item) = item.as_object_mut() else {
        return;
    };

    for (key, value) in item.iter_mut() {
        if key == "parameters" {
            parameters(value);
        } else if let Some(operation) = value.as_object_mut() {
            if let Some(value) = operation.get_mut("parameters") {
                parameters(value);
            }
            if let Some(value) = operation.get_mut("requestBody") {
                body(value);
            }
            for_each(operation.get_mut("responses"), response);
        }
    }
}

fn parameters(parameters: &mut Value) {
    let Some(parameters) = parameters.as_array_mut() else {
        return;
    };

    for value in parameters {
        parameter(value);
    }
}

/// A parameter or a header, whose schema is `schema` or that of its single media type.
fn parameter(parameter: &mut Value) {
    if let Some(value) = parameter.get_mut("schema") {
        schema(value);
    }
    for_each(parameter.get_mut("content"), media_type);
}

fn body(body: &mut Value) {
    for_each(body.get_mut("content"), media_type);
}

fn response(response: &mut Value) {
    for_each(response.get_mut("headers"), parameter);
    for_each(response.get_mut("content"), media_type);
}

/// A media type: its schema, and the schema of a stream's items, `x-item`.
fn media_type(media_type: &mut Value) {
    for key in ["schema", "x-item"] {
        if let Some(value) = media_type.get_mut(key) {
            schema(value);
        }
    }
}

/// Rewrites `value` and each schema it holds, those of its keywords only: an object in a `default`, a `const` or
/// `examples`, or a property named `$ref`, is no schema.
fn schema(value: &mut Value) {
    let Some(object) = value.as_object_mut() else {
        return;
    };

    for (key, value) in object.iter_mut() {
        let key = key.as_str();

        if SCHEMA.contains(&key) {
            schema(value);
        } else if ARRAY.contains(&key) {
            for value in value.as_array_mut().into_iter().flatten() {
                schema(value);
            }
        } else if MAP.contains(&key) {
            for value in value.as_object_mut().into_iter().flat_map(Map::values_mut) {
                schema(value);
            }
        }
    }

    let siblings = object.len() > 1 && object.get("$ref").is_some_and(Value::is_string);
    if siblings {
        all_of(object);
    }
}

/// `object`, a `$ref` beside other keywords, as an `allOf` of the reference and of the keywords that constrain, the
/// annotations beside it.
fn all_of(object: &mut Map<String, Value>) {
    let mut rest = std::mem::take(object);
    let reference = rest.remove("$ref");

    for key in ANNOTATIONS {
        if let Some(value) = rest.remove(key) {
            object.insert(key.into(), value);
        }
    }

    let mut members = vec![json!({ "$ref": reference })];
    if !rest.is_empty() {
        members.push(Value::Object(rest));
    }
    object.insert("allOf".into(), Value::Array(members));
}

#[cfg(test)]
mod tests {
    use schemars::JsonSchema;
    use schemars::generate::Contract;
    use serde::{Deserialize, Serialize};

    use super::*;

    fn reference(name: &str) -> Value {
        json!({ "$ref": format!("#/components/schemas/{name}") })
    }

    /// `schema`, rewritten.
    fn rewritten(mut value: Value) -> Value {
        schema(&mut value);
        value
    }

    /// The schemas of `T` and of what it refers to, as the document's generator for `contract` writes them, rewritten.
    fn schemas<T: JsonSchema>(contract: Contract) -> Value {
        let mut generator = super::super::generator(contract);
        generator.subschema_for::<T>();
        let mut document = json!({ "components": { "schemas": generator.take_definitions(true) } });

        rewrite(&mut document);
        document["components"]["schemas"].take()
    }

    #[derive(Serialize, Deserialize, JsonSchema)]
    struct Note {
        id: i64,
    }

    #[derive(Serialize, Deserialize, JsonSchema)]
    #[serde(tag = "kind", rename_all = "snake_case")]
    enum Internal {
        Edited(Note),
        Removed { id: i64 },
    }

    #[derive(Serialize, Deserialize, JsonSchema)]
    #[serde(tag = "kind", content = "note", rename_all = "snake_case")]
    enum Adjacent {
        Edited(Note),
    }

    #[derive(Serialize, Deserialize, JsonSchema)]
    struct Shelf {
        /// The note on top.
        top: Note,
        #[serde(default = "first")]
        bottom: Note,
    }

    fn first() -> Note {
        Note { id: 1 }
    }

    #[test]
    fn a_newtype_variant_of_an_internally_tagged_enum_is_its_struct_and_its_tag() {
        let schemas = schemas::<Internal>(Contract::Serialize);

        assert_eq!(
            schemas["Internal"]["oneOf"][0],
            json!({
                "allOf": [
                    reference("Note"),
                    {
                        "type": "object",
                        "properties": { "kind": { "type": "string", "const": "edited" } },
                        "required": ["kind"],
                    },
                ],
            })
        );
        assert_eq!(
            schemas["Internal"]["oneOf"][1]["required"],
            json!(["kind", "id"])
        );
    }

    #[test]
    fn an_adjacently_tagged_enum_refers_to_its_content_alone() {
        let schemas = schemas::<Adjacent>(Contract::Serialize);

        assert_eq!(
            schemas["Adjacent"]["oneOf"][0]["properties"]["note"],
            reference("Note")
        );
    }

    #[test]
    fn a_fields_comment_and_default_stay_beside_the_all_of() {
        let schemas = schemas::<Shelf>(Contract::Deserialize);

        assert_eq!(
            schemas["Shelf"]["properties"]["top"],
            json!({ "description": "The note on top.", "allOf": [reference("Note")] })
        );
        assert_eq!(
            schemas["Shelf"]["properties"]["bottom"],
            json!({ "default": { "id": 1 }, "allOf": [reference("Note")] })
        );
    }

    #[test]
    fn a_reference_alone_is_kept() {
        assert_eq!(rewritten(reference("Note")), reference("Note"));
    }

    #[test]
    fn only_the_keywords_that_constrain_go_into_the_all_of() {
        assert_eq!(
            rewritten(json!({
                "$ref": "#/components/schemas/Note",
                "title": "Top",
                "description": "The note on top.",
                "deprecated": true,
                "readOnly": true,
                "examples": [{ "id": 1 }],
                "minProperties": 1,
            })),
            json!({
                "title": "Top",
                "description": "The note on top.",
                "deprecated": true,
                "readOnly": true,
                "examples": [{ "id": 1 }],
                "allOf": [reference("Note"), { "minProperties": 1 }],
            })
        );
    }

    #[test]
    fn every_subschema_is_rewritten_but_values() {
        let sibling = json!({ "$ref": "#/components/schemas/Note", "description": "A note." });
        let wrapped = json!({ "description": "A note.", "allOf": [reference("Note")] });

        assert_eq!(
            rewritten(json!({
                "properties": { "$ref": sibling, "items": { "type": "array", "items": sibling } },
                "additionalProperties": sibling,
                "prefixItems": [sibling],
                "anyOf": [sibling, { "type": "null" }],
                "default": sibling,
                "const": sibling,
                "examples": [sibling],
            })),
            json!({
                "properties": { "$ref": wrapped, "items": { "type": "array", "items": wrapped } },
                "additionalProperties": wrapped,
                "prefixItems": [wrapped],
                "anyOf": [wrapped, { "type": "null" }],
                "default": sibling,
                "const": sibling,
                "examples": [sibling],
            })
        );
    }

    #[test]
    fn every_schema_of_the_document_is_rewritten() {
        let sibling = json!({ "$ref": "#/components/schemas/Note", "description": "A note." });
        let wrapped = json!({ "description": "A note.", "allOf": [reference("Note")] });
        let media = json!({ "application/json": { "schema": sibling } });
        let mut document = json!({
            "paths": {
                "/notes": {
                    "parameters": [{ "schema": sibling }],
                    "post": {
                        "parameters": [{ "schema": sibling }, { "content": media }],
                        "requestBody": { "content": media },
                        "responses": {
                            "200": {
                                "headers": { "X-Note": { "schema": sibling } },
                                "content": {
                                    "text/event-stream": { "schema": { "type": "string" }, "x-item": sibling },
                                },
                            },
                            "default": { "$ref": "#/components/responses/Problem", "description": "An error." },
                        },
                    },
                },
            },
            "components": {
                "schemas": { "Shelf": sibling },
                "responses": { "Problem": { "content": media } },
            },
        });

        rewrite(&mut document);

        let media = json!({ "application/json": { "schema": wrapped } });
        assert_eq!(
            document,
            json!({
                "paths": {
                    "/notes": {
                        "parameters": [{ "schema": wrapped }],
                        "post": {
                            "parameters": [{ "schema": wrapped }, { "content": media }],
                            "requestBody": { "content": media },
                            "responses": {
                                "200": {
                                    "headers": { "X-Note": { "schema": wrapped } },
                                    "content": {
                                        "text/event-stream": { "schema": { "type": "string" }, "x-item": wrapped },
                                    },
                                },
                                "default": { "$ref": "#/components/responses/Problem", "description": "An error." },
                            },
                        },
                    },
                },
                "components": {
                    "schemas": { "Shelf": wrapped },
                    "responses": { "Problem": { "content": media } },
                },
            })
        );
    }
}

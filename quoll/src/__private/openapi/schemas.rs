//! The schemas of the document, gathered from its two generators: what clients send, as serde reads it, and what they
//! receive, as serde writes it.
//!
//! A type has one schema when both describe it alike, under the name schemars gives it. When its two schemas differ, as
//! a field with `#[serde(default)]` does, which a client may leave out but always receives, the document holds both:
//! `NoteInput`, what a client sends, and `NoteOutput`, what it receives, with a number after them when a type of the
//! contract already has that name. A type that refers to one of them differs too, and is split the same way.

use std::collections::{BTreeMap, BTreeSet};

use serde_json::{Map, Value};

/// Where a schema of `components/schemas` is referred to from.
const PREFIX: &str = "#/components/schemas/";

/// The schemas of both generators, by the names they give, which are one for a type: both see every type in one order.
pub(super) struct Schemas {
    /// What clients send.
    pub(super) read: Map<String, Value>,
    /// What clients receive.
    pub(super) written: Map<String, Value>,
}

impl Schemas {
    /// The schemas `paths` and the other `components` refer to, each under its name: the references of `paths` are
    /// renamed where a type has two schemas, those of parameters and bodies to the one a client sends, those of
    /// responses to the one it receives.
    pub(super) fn gather(
        self,
        paths: &mut Value,
        components: &Map<String, Value>,
    ) -> Map<String, Value> {
        let mut read_roots = BTreeSet::new();
        let mut written_roots = BTreeSet::new();

        for_each_operation(paths, |operation| {
            for key in ["parameters", "requestBody"] {
                if let Some(value) = operation.get(key) {
                    references(value, &mut read_roots);
                }
            }
            if let Some(value) = operation.get("responses") {
                references(value, &mut written_roots);
            }
        });
        for component in components.values() {
            references(component, &mut written_roots);
        }

        let read_used = closure(read_roots, &self.read);
        let written_used = closure(written_roots, &self.written);
        let split = self.split(&read_used, &written_used);

        let mut taken: BTreeSet<String> = read_used
            .union(&written_used)
            .filter(|name| !split.contains(*name))
            .cloned()
            .collect();
        let mut read_names = BTreeMap::new();
        let mut written_names = BTreeMap::new();
        for name in &split {
            read_names.insert(name.clone(), fresh(&format!("{name}Input"), &mut taken));
            written_names.insert(name.clone(), fresh(&format!("{name}Output"), &mut taken));
        }

        for_each_operation(paths, |operation| {
            for key in ["parameters", "requestBody"] {
                if let Some(value) = operation.get_mut(key) {
                    rename(value, &read_names);
                }
            }
            if let Some(value) = operation.get_mut("responses") {
                rename(value, &written_names);
            }
        });

        let mut schemas = Map::new();
        for (used, defined, names) in [
            (&read_used, &self.read, &read_names),
            (&written_used, &self.written, &written_names),
        ] {
            for name in used {
                let Some(schema) = defined.get(name) else {
                    continue;
                };
                let key = names.get(name).unwrap_or(name);
                if schemas.contains_key(key) {
                    continue;
                }

                let mut schema = schema.clone();
                rename(&mut schema, names);
                schemas.insert(key.clone(), schema);
            }
        }

        schemas
    }

    /// The names used on both sides whose two schemas differ, or refer to one that does.
    fn split(
        &self,
        read_used: &BTreeSet<String>,
        written_used: &BTreeSet<String>,
    ) -> BTreeSet<String> {
        let both: Vec<&String> = read_used.intersection(written_used).collect();
        let mut split: BTreeSet<String> = both
            .iter()
            .filter(|name| self.read.get(**name) != self.written.get(**name))
            .map(|name| (*name).clone())
            .collect();

        loop {
            let more: Vec<String> = both
                .iter()
                .filter(|name| !split.contains(**name))
                .filter(|name| {
                    let mut referred = BTreeSet::new();
                    for schema in [self.read.get(**name), self.written.get(**name)]
                        .into_iter()
                        .flatten()
                    {
                        references(schema, &mut referred);
                    }

                    referred.iter().any(|referred| split.contains(referred))
                })
                .map(|name| (*name).clone())
                .collect();
            if more.is_empty() {
                return split;
            }

            split.extend(more);
        }
    }
}

/// Calls `f` on each operation of `paths`.
fn for_each_operation(paths: &mut Value, mut f: impl FnMut(&mut Map<String, Value>)) {
    let Some(paths) = paths.as_object_mut() else {
        return;
    };

    for item in paths.values_mut() {
        let Some(item) = item.as_object_mut() else {
            continue;
        };

        for operation in item.values_mut() {
            if let Some(operation) = operation.as_object_mut() {
                f(operation);
            }
        }
    }
}

/// Adds to `names` the name of each schema `value` refers to.
fn references(value: &Value, names: &mut BTreeSet<String>) {
    match value {
        Value::Object(object) => {
            for (key, value) in object {
                match (key.as_str(), value) {
                    ("$ref", Value::String(reference)) => {
                        if let Some(name) = reference.strip_prefix(PREFIX) {
                            names.insert(decode(name));
                        }
                    }
                    _ => references(value, names),
                }
            }
        }
        Value::Array(values) => {
            for value in values {
                references(value, names);
            }
        }
        _ => {}
    }
}

/// The names `roots` refer to, and the names those refer to in `defined`, all the way down.
fn closure(roots: BTreeSet<String>, defined: &Map<String, Value>) -> BTreeSet<String> {
    let mut used = BTreeSet::new();
    let mut next: Vec<String> = roots.into_iter().collect();

    while let Some(name) = next.pop() {
        if !used.insert(name.clone()) {
            continue;
        }

        let mut referred = BTreeSet::new();
        if let Some(schema) = defined.get(&name) {
            references(schema, &mut referred);
        }
        next.extend(referred.into_iter().filter(|name| !used.contains(name)));
    }

    used
}

/// Points each reference of `value` to a name of `names` at its new name.
fn rename(value: &mut Value, names: &BTreeMap<String, String>) {
    match value {
        Value::Object(object) => {
            for (key, value) in object.iter_mut() {
                match (key.as_str(), &mut *value) {
                    ("$ref", Value::String(reference)) => {
                        let renamed = reference
                            .strip_prefix(PREFIX)
                            .and_then(|name| names.get(&decode(name)));
                        if let Some(renamed) = renamed {
                            *reference = format!("{PREFIX}{}", encode(renamed));
                        }
                    }
                    _ => rename(value, names),
                }
            }
        }
        Value::Array(values) => {
            for value in values {
                rename(value, names);
            }
        }
        _ => {}
    }
}

/// `wanted`, or it followed by the first number from 2 that makes it a name no other schema has, which it takes.
fn fresh(wanted: &str, taken: &mut BTreeSet<String>) -> String {
    let mut name = wanted.to_owned();
    let mut number = 2;

    while taken.contains(&name) {
        name = format!("{wanted}{number}");
        number += 1;
    }

    taken.insert(name.clone());
    name
}

/// A name as a JSON pointer writes it, `~` and `/` escaped.
fn encode(name: &str) -> String {
    name.replace('~', "~0").replace('/', "~1")
}

/// A name of a JSON pointer, `~` and `/` unescaped.
fn decode(name: &str) -> String {
    name.replace("~1", "/").replace("~0", "~")
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn map(value: Value) -> Map<String, Value> {
        value.as_object().unwrap().clone()
    }

    fn reference(name: &str) -> Value {
        json!({ "$ref": format!("{PREFIX}{name}") })
    }

    /// A document with a route that reads `read` and answers `written`.
    fn paths(read: &str, written: &str) -> Value {
        json!({
            "/notes": {
                "post": {
                    "requestBody": { "content": { "application/json": { "schema": reference(read) } } },
                    "responses": { "200": { "content": { "application/json": { "schema": reference(written) } } } },
                },
            },
        })
    }

    #[test]
    fn a_type_alike_on_both_sides_has_one_schema() {
        let note = json!({ "type": "object", "required": ["id"] });
        let schemas = Schemas {
            read: map(json!({ "Note": note, "Unused": {} })),
            written: map(json!({ "Note": note })),
        };
        let mut paths = paths("Note", "Note");
        let expected = paths.clone();

        let gathered = schemas.gather(&mut paths, &Map::new());

        assert_eq!(gathered, map(json!({ "Note": note })));
        assert_eq!(paths, expected);
    }

    #[test]
    fn a_type_that_differs_has_both_schemas_and_so_do_those_that_refer_to_it() {
        let schemas = Schemas {
            read: map(json!({
                "Tagged": { "required": [] },
                "Note": { "properties": { "tags": reference("Tagged") } },
                "TaggedInput": { "description": "a type of the contract's" },
            })),
            written: map(json!({
                "Tagged": { "required": ["tags"] },
                "Note": { "properties": { "tags": reference("Tagged") } },
                "TaggedInput": { "description": "a type of the contract's" },
            })),
        };
        let mut paths = paths("Note", "Note");
        paths["/notes"]["get"] = json!({
            "parameters": [{ "schema": reference("TaggedInput") }],
            "responses": {},
        });

        let gathered = schemas.gather(&mut paths, &Map::new());

        assert_eq!(
            gathered,
            map(json!({
                "NoteInput": { "properties": { "tags": reference("TaggedInput2") } },
                "TaggedInput2": { "required": [] },
                "TaggedInput": { "description": "a type of the contract's" },
                "NoteOutput": { "properties": { "tags": reference("TaggedOutput") } },
                "TaggedOutput": { "required": ["tags"] },
            }))
        );
        assert_eq!(
            paths["/notes"]["post"]["requestBody"]["content"]["application/json"]["schema"],
            reference("NoteInput")
        );
        assert_eq!(
            paths["/notes"]["post"]["responses"]["200"]["content"]["application/json"]["schema"],
            reference("NoteOutput")
        );
    }

    #[test]
    fn a_type_used_on_one_side_keeps_its_name() {
        let schemas = Schemas {
            read: map(json!({ "Login": { "required": [] } })),
            written: map(json!({ "Login": { "required": ["user"] }, "Token": {} })),
        };
        let mut paths = paths("Login", "Token");

        let gathered = schemas.gather(&mut paths, &Map::new());

        assert_eq!(
            gathered,
            map(json!({ "Login": { "required": [] }, "Token": {} }))
        );
    }

    #[test]
    fn the_components_refer_to_what_clients_receive() {
        let schemas = Schemas {
            read: map(json!({ "Problem": { "type": "object" } })),
            written: map(json!({ "Problem": { "type": "object" } })),
        };
        let components = map(
            json!({ "responses": { "Problem": { "content": { "schema": reference("Problem") } } } }),
        );

        let gathered = schemas.gather(&mut json!({}), &components);

        assert_eq!(gathered, map(json!({ "Problem": { "type": "object" } })));
    }
}

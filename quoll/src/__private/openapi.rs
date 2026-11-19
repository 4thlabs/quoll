//! The description of an api that `Api::openapi` hands to the crate, which builds its OpenAPI 3.1 document: what the
//! macro knows of each route, and the schemas of its types, which only schemars knows once they are compiled.

use std::borrow::Cow;
use std::collections::BTreeSet;

use schemars::generate::SchemaSettings;
use schemars::{JsonSchema, Schema, SchemaGenerator, json_schema};
use serde_json::{Map, Value, json};

use crate::rest::openapi::{Document, SecurityScheme};

/// The version of OpenAPI the document follows.
const OPENAPI: &str = "3.1.1";

/// Writes the schema of a type the contract names, adding the schemas it refers to to the generator's.
pub type SchemaFn = fn(&mut SchemaGenerator) -> Schema;

/// The schema of `T`: a reference to its schema under `components/schemas` for a struct or an enum.
pub fn schema<T: ?Sized + JsonSchema>(generator: &mut SchemaGenerator) -> Schema {
    generator.subschema_for::<T>()
}

/// The schema of `T` itself, never a reference: a query struct's, whose fields are unfolded.
pub fn inline<T: ?Sized + JsonSchema>(generator: &mut SchemaGenerator) -> Schema {
    T::json_schema(generator)
}

/// An api, as `rest::api!` describes it.
#[derive(Debug)]
pub struct Api {
    /// The `///` lines of the api, as rustc gives them.
    pub docs: &'static [&'static str],
    /// The name of the contract's crate.
    pub title: &'static str,
    /// The version of the contract's crate.
    pub version: &'static str,
    /// The schemes the contract declares, in the order it writes them.
    pub schemes: &'static [SchemeRef],
    /// The groups, in the order the contract writes them.
    pub groups: &'static [Group],
}

/// A group: a tag of the document.
#[derive(Debug)]
pub struct Group {
    /// Its name.
    pub name: &'static str,
    /// Its `///` lines.
    pub docs: &'static [&'static str],
    /// Its routes, in the order the contract writes them.
    pub routes: &'static [Route],
}

/// A route: an operation of the document.
#[derive(Debug)]
pub struct Route {
    /// The operation id, `notes_get`.
    pub id: &'static str,
    /// The method, in lowercase as OpenAPI writes it.
    pub method: &'static str,
    /// The path under the api's prefix, without the query: `/api/notes/{id}`.
    pub path: &'static str,
    /// Its `///` lines.
    pub docs: &'static [&'static str],
    /// Its parameters, in the order the contract writes them; the body is not one.
    pub params: &'static [Param],
    /// Its body, if it reads one.
    pub body: Option<Body>,
    /// Its result.
    pub reply: Reply,
    /// Its authentication, if it takes a credential.
    pub auth: Option<Auth>,
}

/// A parameter of a route.
#[derive(Debug)]
pub struct Param {
    /// Its name on the wire: the path's `{name}`, the query's key, the header's name; the argument's for a query
    /// struct.
    pub name: &'static str,
    /// Where it is read from.
    pub place: Place,
    /// Whether it must be there: it is not an `Option` nor a `Vec`.
    pub required: bool,
    /// Its schema, the `T` of an `Option<T>`; the whole struct of a query struct.
    pub schema: SchemaFn,
}

/// Where a parameter is read from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Place {
    /// A `{name}` of the path.
    Path,
    /// A key of the query.
    Query,
    /// The whole query, a key per field.
    QueryStruct,
    /// A header.
    Header,
}

/// The body of a route.
#[derive(Debug)]
pub enum Body {
    /// The JSON of a type.
    Json(SchemaFn),
    /// `Form<T>`.
    Form(SchemaFn),
    /// `Text`.
    Text,
    /// `Bytes`.
    Bytes,
}

/// The result of a route.
#[derive(Debug)]
pub enum Reply {
    /// Nothing: a 204.
    Empty,
    /// The JSON of a type.
    Json(SchemaFn),
    /// `Created<T>`.
    Created(SchemaFn),
    /// `Accepted<T>`.
    Accepted(SchemaFn),
    /// `Text`.
    Text,
    /// `Bytes`.
    Bytes,
    /// `Events<T>`, with the schema of each item.
    Events(SchemaFn),
    /// `JsonLines<T>`, with the schema of each item.
    JsonLines(SchemaFn),
}

/// The authentication of a route.
#[derive(Debug)]
pub struct Auth {
    /// The schemes it takes, any of them.
    pub schemes: &'static [SchemeRef],
    /// Whether a request with no credential passes.
    pub optional: bool,
    /// The scopes it needs, all of them.
    pub scopes: &'static [&'static str],
}

/// A scheme: its name and how the document describes it.
#[derive(Debug)]
pub struct SchemeRef {
    /// `Scheme::NAME`.
    pub name: &'static str,
    /// `Scheme::openapi`.
    pub openapi: fn() -> SecurityScheme,
}

/// Whether two names of schemes are one, which a compile-time check of the api refuses: the document names each
/// scheme by its name.
pub const fn same_name(first: &str, second: &str) -> bool {
    let (first, second) = (first.as_bytes(), second.as_bytes());
    if first.len() != second.len() {
        return false;
    }

    let mut i = 0;
    while i < first.len() {
        if first[i] != second[i] {
            return false;
        }
        i += 1;
    }

    true
}

/// The responses of errors a route may list, under `components/responses`: their name, status and description.
const ERRORS: &[(&str, &str, &str)] = &[
    (
        "BadRequest",
        "400",
        "A parameter or the body is missing, or does not read as its type.",
    ),
    (
        "Unauthorized",
        "401",
        "The request carries no valid credential of a scheme the route takes.",
    ),
    (
        "Forbidden",
        "403",
        "The caller lacks a scope the route needs, or a request carrying a cookie came from another origin.",
    ),
    (
        "UnsupportedMediaType",
        "415",
        "The body does not have the content type the route reads.",
    ),
];

/// The document of `api`.
pub fn document(api: &Api) -> Document {
    let mut settings = SchemaSettings::draft2020_12();
    settings.definitions_path = "/components/schemas".into();
    settings.meta_schema = None;

    let mut builder = Builder {
        generator: settings.into_generator(),
        schemes: Map::new(),
        errors: BTreeSet::new(),
    };
    // Registered first, so that the problem keeps its name and a type of the contract's named `Problem` takes another.
    builder.generator.subschema_for::<ProblemSchema>();

    for scheme in api.schemes {
        builder.scheme(scheme);
    }

    let mut paths = Map::new();
    let mut tags = Vec::new();

    for group in api.groups {
        let mut tag = Map::new();
        tag.insert("name".into(), group.name.into());
        if let Some(description) = text(group.docs) {
            tag.insert("description".into(), description.into());
        }
        tags.push(Value::Object(tag));

        for route in group.routes {
            let operation = builder.operation(group, route);
            let item = paths
                .entry(route.path)
                .or_insert_with(|| Value::Object(Map::new()));

            if let Some(item) = item.as_object_mut() {
                item.insert(route.method.into(), operation);
            }
        }
    }

    let mut info = Map::new();
    info.insert("title".into(), api.title.into());
    info.insert("version".into(), api.version.into());
    if let Some(description) = text(api.docs) {
        info.insert("description".into(), description.into());
    }

    let mut document = Map::new();
    document.insert("openapi".into(), OPENAPI.into());
    document.insert("info".into(), Value::Object(info));
    if !tags.is_empty() {
        document.insert("tags".into(), Value::Array(tags));
    }
    document.insert("paths".into(), Value::Object(paths));
    document.insert("components".into(), builder.components());

    Document::new(Value::Object(document))
}

/// What the document gathers as it describes the routes.
struct Builder {
    /// The schemas of the contract's types.
    generator: SchemaGenerator,
    /// The schemes, by name.
    schemes: Map<String, Value>,
    /// The names of the responses of errors some route lists.
    errors: BTreeSet<&'static str>,
}

impl Builder {
    fn scheme(&mut self, scheme: &SchemeRef) {
        if self.schemes.contains_key(scheme.name) {
            return;
        }

        let described = serde_json::to_value((scheme.openapi)())
            .expect("a security scheme serializes as a map of strings");

        self.schemes.insert(scheme.name.into(), described);
    }

    fn operation(&mut self, group: &Group, route: &Route) -> Value {
        let mut operation = Map::new();
        operation.insert("operationId".into(), route.id.into());
        operation.insert("tags".into(), json!([group.name]));

        let (summary, description) = summary(route.docs);
        if let Some(summary) = summary {
            operation.insert("summary".into(), summary.into());
        }
        if let Some(description) = description {
            operation.insert("description".into(), description.into());
        }

        let parameters: Vec<Value> = route
            .params
            .iter()
            .flat_map(|param| self.parameters(param))
            .collect();
        if !parameters.is_empty() {
            operation.insert("parameters".into(), Value::Array(parameters));
        }

        if let Some(body) = &route.body {
            operation.insert("requestBody".into(), self.body(body));
        }

        operation.insert("responses".into(), self.responses(route));

        if let Some(auth) = &route.auth {
            operation.insert("security".into(), self.security(auth));
        }

        Value::Object(operation)
    }

    fn parameters(&mut self, param: &Param) -> Vec<Value> {
        let schema = (param.schema)(&mut self.generator).to_value();
        let place = match param.place {
            Place::Path => "path",
            Place::Query => "query",
            Place::Header => "header",
            Place::QueryStruct => return unfold(param.name, schema),
        };

        vec![json!({
            "name": param.name,
            "in": place,
            "required": param.required || param.place == Place::Path,
            "schema": not_null(schema),
        })]
    }

    fn body(&mut self, body: &Body) -> Value {
        match body {
            Body::Json(schema) => json!({
                "required": true,
                "content": { "application/json": { "schema": schema(&mut self.generator) } },
            }),
            Body::Form(schema) => json!({
                "required": true,
                "content": {
                    "application/x-www-form-urlencoded": { "schema": schema(&mut self.generator) },
                },
            }),
            Body::Text => json!({
                "required": true,
                "content": { "text/plain": { "schema": { "type": "string" } } },
            }),
            Body::Bytes => json!({
                "description": "Bytes of any media type, which `Content-Type` says, but `application/x-www-form-urlencoded`, `multipart/form-data` and `text/plain`.",
                "required": true,
                "content": { "application/octet-stream": {} },
            }),
        }
    }

    fn responses(&mut self, route: &Route) -> Value {
        let mut responses = Map::new();
        let (status, success) = self.success(&route.reply);
        responses.insert(status.into(), success);

        let reads = !route.params.is_empty() || route.body.is_some();
        let forbidden = route.auth.as_ref().is_some_and(|auth| {
            let changes = !matches!(route.method, "get" | "head" | "options");

            !auth.scopes.is_empty()
                || (changes
                    && auth
                        .schemes
                        .iter()
                        .any(|scheme| (scheme.openapi)().is_cookie()))
        });
        let listed = [reads, route.auth.is_some(), forbidden, route.body.is_some()];

        for ((name, status, _), listed) in ERRORS.iter().zip(listed) {
            if listed {
                self.errors.insert(name);
                responses.insert((*status).into(), reference(name));
            }
        }

        self.errors.insert("Problem");
        responses.insert("default".into(), reference("Problem"));

        Value::Object(responses)
    }

    fn success(&mut self, reply: &Reply) -> (&'static str, Value) {
        let json = |schema: Schema| json!({ "application/json": { "schema": schema } });

        match reply {
            Reply::Empty => ("204", json!({ "description": "No Content" })),
            Reply::Json(schema) => (
                "200",
                json!({ "description": "OK", "content": json(schema(&mut self.generator)) }),
            ),
            Reply::Created(schema) => (
                "201",
                json!({
                    "description": "Created",
                    "headers": {
                        "Location": {
                            "description": "Where the created resource is read, when the server says.",
                            "schema": { "type": "string", "format": "uri-reference" },
                        },
                    },
                    "content": json(schema(&mut self.generator)),
                }),
            ),
            Reply::Accepted(schema) => (
                "202",
                json!({ "description": "Accepted", "content": json(schema(&mut self.generator)) }),
            ),
            Reply::Text => (
                "200",
                json!({
                    "description": "OK",
                    "content": { "text/plain": { "schema": { "type": "string" } } },
                }),
            ),
            Reply::Bytes => (
                "200",
                json!({
                    "description": "Bytes of the media type `Content-Type` says.",
                    "content": { "*/*": {} },
                }),
            ),
            Reply::Events(item) => (
                "200",
                json!({
                    "description": "Server-sent events, the data of each the JSON of an item, whose schema is `x-item`. A failure ends the stream with an event `problem`, whose data is a `Problem`.",
                    "content": {
                        "text/event-stream": {
                            "schema": { "type": "string" },
                            "x-item": item(&mut self.generator),
                        },
                    },
                }),
            ),
            Reply::JsonLines(item) => (
                "200",
                json!({
                    "description": "JSON Lines, each the JSON of an item, whose schema is `x-item`. A failure aborts the body.",
                    "content": {
                        "application/jsonl": {
                            "schema": { "type": "string" },
                            "x-item": item(&mut self.generator),
                        },
                    },
                }),
            ),
        }
    }

    fn security(&mut self, auth: &Auth) -> Value {
        let mut requirements = Vec::new();

        for scheme in auth.schemes {
            self.scheme(scheme);
            requirements.push(json!({ scheme.name: auth.scopes }));
        }
        if auth.optional {
            requirements.push(json!({}));
        }

        Value::Array(requirements)
    }

    fn components(mut self) -> Value {
        let mut components = Map::new();
        components.insert(
            "schemas".into(),
            Value::Object(self.generator.take_definitions(true)),
        );

        let problem = json!({
            "application/problem+json": { "schema": { "$ref": "#/components/schemas/Problem" } },
        });
        let mut responses = Map::new();
        for (name, _, description) in ERRORS {
            if !self.errors.contains(name) {
                continue;
            }

            let mut response = json!({ "description": description, "content": problem });
            if *name == "Unauthorized" {
                response["headers"] = json!({
                    "WWW-Authenticate": {
                        "description": "A challenge per scheme the route takes, in the order it takes them.",
                        "schema": { "type": "string" },
                    },
                });
            }
            responses.insert((*name).into(), response);
        }
        if self.errors.contains("Problem") {
            responses.insert(
                "Problem".into(),
                json!({ "description": "An error, as an RFC 9457 problem.", "content": problem }),
            );
        }
        if !responses.is_empty() {
            components.insert("responses".into(), Value::Object(responses));
        }

        if !self.schemes.is_empty() {
            components.insert("securitySchemes".into(), Value::Object(self.schemes));
        }

        Value::Object(components)
    }
}

/// A reference to a response under `components/responses`.
fn reference(name: &str) -> Value {
    json!({ "$ref": format!("#/components/responses/{name}") })
}

/// The parameters of a query struct, named `name`, whose schema is `schema`: one per field, sorted by name, when it is
/// a plain object; else one parameter holding it, written as a form.
fn unfold(name: &str, schema: Value) -> Vec<Value> {
    let plain = schema.get("type") == Some(&Value::from("object"))
        && ["allOf", "anyOf", "oneOf", "$ref"]
            .iter()
            .all(|key| schema.get(key).is_none());
    let properties = schema.get("properties").and_then(Value::as_object);
    let (true, Some(properties)) = (plain, properties) else {
        return vec![json!({
            "name": name,
            "in": "query",
            "required": true,
            "style": "form",
            "explode": true,
            "schema": schema,
        })];
    };

    let required: BTreeSet<&str> = schema
        .get("required")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .collect();
    let mut keys: Vec<&String> = properties.keys().collect();
    keys.sort();

    keys.into_iter()
        .map(|key| {
            let mut property = not_null(properties[key].clone());
            let description = property
                .as_object_mut()
                .and_then(|property| property.remove("description"));
            let mut parameter = json!({
                "name": key,
                "in": "query",
                "required": required.contains(key.as_str()),
                "schema": property,
            });

            if let Some(description) = description {
                parameter["description"] = description;
            }

            parameter
        })
        .collect()
}

/// The schema of a parameter, which is never `null`: an `Option` is a parameter that may be missing, and a `null`
/// would be sent as the text `null`.
fn not_null(mut schema: Value) -> Value {
    let Some(object) = schema.as_object_mut() else {
        return schema;
    };

    if let Some(Value::Array(types)) = object.get_mut("type") {
        types.retain(|ty| ty != "null");
        if types.len() == 1 {
            let only = types.remove(0);
            object.insert("type".into(), only);
        }
    }

    if let Some(Value::Array(values)) = object.get_mut("enum") {
        values.retain(|value| !value.is_null());
    }

    if let Some(Value::Array(alternatives)) = object.get_mut("anyOf") {
        alternatives.retain(|alternative| alternative != &json!({ "type": "null" }));
        if alternatives.len() == 1 {
            let only = alternatives.remove(0);
            object.remove("anyOf");
            if let Value::Object(only) = only {
                for (key, value) in only {
                    object.entry(key).or_insert(value);
                }
            }
        }
    }

    if object.get("default") == Some(&Value::Null) {
        object.remove("default");
    }

    schema
}

/// The `///` lines, as rustc gives them, without the space each starts with.
fn lines(docs: &[&'static str]) -> Vec<&'static str> {
    docs.iter()
        .map(|line| line.strip_prefix(' ').unwrap_or(line))
        .collect()
}

/// The text of the `///` lines, if there is any.
fn text(docs: &[&'static str]) -> Option<String> {
    let text = lines(docs).join("\n");
    let text = text.trim();

    (!text.is_empty()).then(|| text.to_owned())
}

/// The summary of the `///` lines, their first paragraph on one line, and their description, the paragraphs after it.
fn summary(docs: &[&'static str]) -> (Option<String>, Option<String>) {
    let lines = lines(docs);
    let start = lines
        .iter()
        .position(|line| !line.trim().is_empty())
        .unwrap_or(lines.len());
    let lines = &lines[start..];
    let end = lines
        .iter()
        .position(|line| line.trim().is_empty())
        .unwrap_or(lines.len());

    let summary: Vec<&str> = lines[..end].iter().map(|line| line.trim()).collect();
    let summary = (!summary.is_empty()).then(|| summary.join(" "));
    let description = text(&lines[end..]);

    (summary, description)
}

/// The RFC 9457 problem of every error, as the server answers it, under `components/schemas/Problem`.
struct ProblemSchema;

impl JsonSchema for ProblemSchema {
    fn schema_name() -> Cow<'static, str> {
        "Problem".into()
    }

    fn schema_id() -> Cow<'static, str> {
        "quoll::rest::Problem".into()
    }

    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        json_schema!({
            "description": "An RFC 9457 problem: what went wrong with a request, the body of every error, as `application/problem+json`.",
            "type": "object",
            "properties": {
                "type": {
                    "description": "A URI naming the kind of problem: `about:blank` for a problem that says no more than its status.",
                    "type": "string",
                    "format": "uri-reference",
                    "default": "about:blank",
                },
                "title": {
                    "description": "A short summary of the kind of problem.",
                    "type": "string",
                },
                "status": {
                    "description": "The status of the answer.",
                    "type": "integer",
                    "minimum": 100,
                    "maximum": 599,
                },
                "detail": {
                    "description": "What went wrong with this request.",
                    "type": "string",
                },
                "instance": {
                    "description": "A URI naming this occurrence of the problem.",
                    "type": "string",
                    "format": "uri-reference",
                },
            },
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_first_paragraph_is_the_summary() {
        assert_eq!(summary(&[]), (None, None));
        assert_eq!(
            summary(&[" Notes matching", " the filter."]),
            (Some("Notes matching the filter.".into()), None)
        );
        assert_eq!(
            summary(&["", " Notes.", "", " A page at a time.", "", " Sorted."]),
            (
                Some("Notes.".into()),
                Some("A page at a time.\n\nSorted.".into())
            )
        );
    }

    #[test]
    fn a_parameter_is_never_null() {
        assert_eq!(
            not_null(json!({ "type": ["string", "null"] })),
            json!({ "type": "string" })
        );
        assert_eq!(
            not_null(json!({ "type": ["integer", "null"], "format": "uint32", "default": null })),
            json!({ "type": "integer", "format": "uint32" })
        );
        assert_eq!(
            not_null(
                json!({ "anyOf": [{ "$ref": "#/components/schemas/Kind" }, { "type": "null" }], "description": "Its kind." })
            ),
            json!({ "$ref": "#/components/schemas/Kind", "description": "Its kind." })
        );
        assert_eq!(
            not_null(json!({ "type": ["string", "null"], "enum": ["a", "b", null] })),
            json!({ "type": "string", "enum": ["a", "b"] })
        );
    }

    #[test]
    fn a_plain_query_struct_is_unfolded_by_field() {
        let schema = json!({
            "type": "object",
            "properties": {
                "page": { "type": ["integer", "null"], "format": "uint32" },
                "contains": { "description": "A word.", "type": ["string", "null"] },
                "tag": { "type": "string" },
            },
            "required": ["tag"],
        });

        assert_eq!(
            unfold("filter", schema),
            vec![
                json!({ "name": "contains", "in": "query", "required": false, "description": "A word.", "schema": { "type": "string" } }),
                json!({ "name": "page", "in": "query", "required": false, "schema": { "type": "integer", "format": "uint32" } }),
                json!({ "name": "tag", "in": "query", "required": true, "schema": { "type": "string" } }),
            ]
        );
    }

    #[test]
    fn a_query_struct_that_is_not_plain_is_one_form() {
        let schema =
            json!({ "type": "object", "allOf": [{ "$ref": "#/components/schemas/Page" }] });

        assert_eq!(
            unfold("filter", schema.clone()),
            vec![
                json!({ "name": "filter", "in": "query", "required": true, "style": "form", "explode": true, "schema": schema })
            ]
        );
    }

    #[test]
    fn names_are_compared_whole() {
        assert!(same_name("Key", "Key"));
        assert!(!same_name("Key", "Keys"));
        assert!(!same_name("Key", "key"));
    }
}

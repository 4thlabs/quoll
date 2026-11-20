//! The description of an api that `Api::openapi` hands to the crate, which builds its OpenAPI 3.1 document: what the
//! macro knows of each route, and the schemas of its types, which only schemars knows once they are compiled.

use std::any::TypeId;
use std::borrow::Cow;
use std::collections::BTreeSet;

use schemars::generate::{Contract, SchemaSettings};
use schemars::{JsonSchema, Schema, SchemaGenerator, json_schema};
use serde_json::{Map, Value, json};

use crate::rest::openapi::{Document, SecurityScheme};

mod schemas;

/// The version of OpenAPI the document follows.
const OPENAPI: &str = "3.1.1";

/// Writes the schema of a type the contract names, adding the schemas it refers to to the generator's.
pub type SchemaFn = fn(&mut SchemaGenerator) -> Schema;

/// A type the OpenAPI document describes, which has a schema.
#[diagnostic::on_unimplemented(
    message = "`{Self}` has no schema, which the OpenAPI document of the contract describes it with",
    label = "no `schemars::JsonSchema`",
    note = "derive `schemars::JsonSchema` for `{Self}`, or write `#[openapi(none)]` on the api of a contract that is only called, which has no document"
)]
pub trait Documented: JsonSchema {}

impl<T: JsonSchema + ?Sized> Documented for T {}

/// The schema of `T`: a reference to its schema under `components/schemas` for a struct or an enum.
pub fn schema<T: ?Sized + Documented>(generator: &mut SchemaGenerator) -> Schema {
    generator.subschema_for::<T>()
}

/// The schema of `T` itself, never a reference: a query struct's, whose fields are unfolded.
pub fn inline<T: ?Sized + Documented>(generator: &mut SchemaGenerator) -> Schema {
    T::json_schema(generator)
}

/// An api, as `rest::api!` describes it.
#[derive(Debug)]
pub struct Api {
    /// The name of the api, `Api`, which a message names.
    pub name: &'static str,
    /// The `///` comments of the api, as rustc gives them.
    pub docs: &'static [&'static str],
    /// The name of the contract's crate.
    pub title: &'static str,
    /// The version of the contract's crate.
    pub version: &'static str,
    /// What every path starts with, `/api`, or nothing.
    pub prefix: &'static str,
    /// The schemes the contract declares, in the order it writes them.
    pub schemes: &'static [SchemeRef],
    /// The groups, in the order the api lists them, then the ones it writes.
    pub groups: &'static [&'static Group],
}

/// The document of a group, which `rest::group!` writes unless the group says `#[openapi(none)]`.
#[diagnostic::on_unimplemented(
    message = "the group has no OpenAPI document, which the document of the api that lists it holds",
    label = "a group with `#[openapi(none)]`",
    note = "remove `#[openapi(none)]` from the group's `rest::group!`, or write it on the api, whose document is then left out"
)]
pub trait GroupDocument {
    /// The group, as the document describes it.
    const GROUP: Group;
}

/// A group: a tag of the document.
#[derive(Debug)]
pub struct Group {
    /// Its name.
    pub name: &'static str,
    /// Its `///` comments.
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
    /// The path below the api's prefix, without the query: `/notes/{id}`, and empty for `"/"`.
    pub path: &'static str,
    /// Its `///` comments.
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
    /// Its schema, the `T` of an `Option<T>`; the whole struct of a query struct.
    pub schema: SchemaFn,
}

/// Where a parameter is read from, and whether it must be there: it is not an `Option` nor a `Vec`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Place {
    /// A `{name}` of the path, always there.
    Path,
    /// A key of the query.
    Query {
        /// Whether it must be there.
        required: bool,
    },
    /// The whole query, a key per field, each required as its schema says.
    QueryStruct,
    /// A header.
    Header {
        /// Whether it must be there.
        required: bool,
    },
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
    /// The schemes it takes, any of them, in the order of its `#[auth(...)]`.
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
    /// Whether the scheme answers 403 to a request from another origin that changes things: a cookie scheme the
    /// contract declares, which checks it as it reads its cookie. A route's scheme says `false`, and the document
    /// finds out from the api's declared schemes.
    pub checks_origin: bool,
    /// The scheme's type, which tells two schemes of one name apart.
    pub type_id: fn() -> TypeId,
    /// The scheme's type, as a message names it.
    pub type_name: fn() -> &'static str,
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

/// Whether `name` is the key of a component, as OpenAPI requires a scheme's name to be: ASCII letters, digits, `.`,
/// `-` and `_`, at least one of them. A compile-time check of the api refuses another.
pub const fn is_component_name(name: &str) -> bool {
    let bytes = name.as_bytes();
    if bytes.is_empty() {
        return false;
    }

    let mut i = 0;
    while i < bytes.len() {
        if !(bytes[i].is_ascii_alphanumeric() || matches!(bytes[i], b'.' | b'-' | b'_')) {
            return false;
        }
        i += 1;
    }

    true
}

/// `scheme` with the `///` comments of a declared scheme as its description, when it has any.
pub fn described(scheme: SecurityScheme, docs: &[&str]) -> SecurityScheme {
    match text(docs) {
        Some(description) => scheme.with_description(description),
        None => scheme,
    }
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

/// Where a schema is used: what a client sends, which the server reads, or what it receives, which the server writes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Side {
    /// A parameter or a body, described as serde reads it.
    Read,
    /// A result or the item of a stream, described as serde writes it.
    Written,
}

/// The document of `api`.
///
/// # Panics
///
/// When two schemes of different types have one name, which names a scheme in the document: the macro refuses them
/// in one block, and the groups of other blocks reach the api only here.
pub fn document(api: &'static Api) -> Document {
    let refs = schemes_of(api);
    let mut builder = Builder {
        schemes: refs.iter().map(|scheme| (scheme.openapi)()).collect(),
        refs,
        read: generator(Contract::Deserialize),
        written: generator(Contract::Serialize),
        errors: BTreeSet::new(),
    };

    let mut paths = Map::new();
    let mut tags = Vec::new();

    for &group in api.groups {
        let mut tag = Map::new();
        tag.insert("name".into(), group.name.into());
        if let Some(description) = text(group.docs) {
            tag.insert("description".into(), description.into());
        }
        tags.push(Value::Object(tag));

        for route in group.routes {
            let operation = builder.operation(group, route);
            let path = match (api.prefix, route.path) {
                ("", "") => "/".to_owned(),
                (prefix, path) => format!("{prefix}{path}"),
            };
            let item = paths
                .entry(path)
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

    let mut components = builder.responses_and_schemes();
    let mut paths = Value::Object(paths);
    let schemas = schemas::Schemas {
        read: builder.read.take_definitions(true),
        written: builder.written.take_definitions(true),
    }
    .gather(&mut paths, &components);
    components.insert("schemas".into(), Value::Object(schemas));

    let mut document = Map::new();
    document.insert("openapi".into(), OPENAPI.into());
    document.insert("info".into(), Value::Object(info));
    if !tags.is_empty() {
        document.insert("tags".into(), Value::Array(tags));
    }
    document.insert("paths".into(), paths);
    document.insert("components".into(), Value::Object(components));

    Document::new(Value::Object(document))
}

/// The schemes of `api`, each once by its name: the declared ones, in the order the contract writes them, then the
/// others its routes take, in the order they come.
///
/// # Panics
///
/// When two schemes of different types have one name, which names a scheme in the document. The macro refuses them
/// when it sees both, in one contract's block; the groups an api lists come from other blocks, whose schemes it does
/// not see.
fn schemes_of(api: &'static Api) -> Vec<&'static SchemeRef> {
    let mut schemes: Vec<&'static SchemeRef> = api.schemes.iter().collect();
    let taken = api
        .groups
        .iter()
        .flat_map(|group| group.routes)
        .filter_map(|route| route.auth.as_ref())
        .flat_map(|auth| auth.schemes);

    for scheme in taken {
        match schemes.iter().find(|listed| listed.name == scheme.name) {
            Some(listed) if (listed.type_id)() == (scheme.type_id)() => {}
            Some(listed) => panic!(
                "the schemes `{}` and `{}` of the api `{}` have one `NAME`, `{}`, which names a scheme in the OpenAPI document: give each scheme its own",
                (listed.type_name)(),
                (scheme.type_name)(),
                api.name,
                scheme.name,
            ),
            None => schemes.push(scheme),
        }
    }

    schemes
}

/// A generator of schemas under `components/schemas`, for `contract`, with the problem of errors registered first, so
/// that it keeps its name and a type of the contract's named `Problem` takes another.
fn generator(contract: Contract) -> SchemaGenerator {
    let mut settings = SchemaSettings::draft2020_12();
    settings.definitions_path = "/components/schemas".into();
    settings.meta_schema = None;
    settings.contract = contract;

    let mut generator = settings.into_generator();
    generator.subschema_for::<ProblemSchema>();

    generator
}

/// What the document gathers as it describes the routes.
struct Builder {
    /// The schemas of what clients send.
    read: SchemaGenerator,
    /// The schemas of what clients receive.
    written: SchemaGenerator,
    /// The api's schemes, each once: the declared ones, then the others its routes take.
    refs: Vec<&'static SchemeRef>,
    /// The api's schemes, as they describe themselves, in the order of `refs`.
    schemes: Vec<SecurityScheme>,
    /// The names of the responses of errors some route lists.
    errors: BTreeSet<&'static str>,
}

impl Builder {
    /// The schema of a type used on `side`. Both generators see every type, in one order, so that they give each type
    /// one name.
    fn schema(&mut self, schema: SchemaFn, side: Side) -> Value {
        let read = schema(&mut self.read).to_value();
        let written = schema(&mut self.written).to_value();

        match side {
            Side::Read => read,
            Side::Written => written,
        }
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

        let parameters = self.parameters(route);
        if !parameters.is_empty() {
            operation.insert("parameters".into(), Value::Array(parameters));
        }

        if let Some(body) = &route.body {
            operation.insert("requestBody".into(), self.body(body));
        }

        operation.insert("responses".into(), self.responses(route));

        if let Some(auth) = &route.auth {
            operation.insert("security".into(), security(auth));
        }

        Value::Object(operation)
    }

    /// The route's parameters, each name of each place once: a query struct may name a key the route's string names
    /// too, which reads the same key. The key of a query scheme is the credential, which `security` describes.
    fn parameters(&mut self, route: &Route) -> Vec<Value> {
        let credentials: BTreeSet<String> = self
            .route_schemes(route)
            .filter_map(SecurityScheme::query_key)
            .map(str::to_owned)
            .collect();
        let mut seen: BTreeSet<(String, &str)> = BTreeSet::new();
        let mut parameters = Vec::new();

        for param in route.params {
            for (name, place, parameter) in self.parameter(param) {
                let credential = place == "query" && credentials.contains(&name);

                if !credential && seen.insert((name, place)) {
                    parameters.push(parameter);
                }
            }
        }

        parameters
    }

    /// The parameters of `param`, each with its name and its place: one, or one per field of a query struct.
    fn parameter(&mut self, param: &Param) -> Vec<(String, &'static str, Value)> {
        let schema = self.schema(param.schema, Side::Read);
        let (place, required) = match param.place {
            Place::Path => ("path", true),
            Place::Query { required } => ("query", required),
            Place::Header { required } => ("header", required),
            Place::QueryStruct => return unfold(param.name, schema),
        };
        let parameter = json!({
            "name": param.name,
            "in": place,
            "required": required,
            "schema": not_null(schema),
        });

        vec![(param.name.to_owned(), place, parameter)]
    }

    fn body(&mut self, body: &Body) -> Value {
        match body {
            Body::Json(schema) => json!({
                "required": true,
                "content": { "application/json": { "schema": self.schema(*schema, Side::Read) } },
            }),
            Body::Form(schema) => json!({
                "required": true,
                "content": {
                    "application/x-www-form-urlencoded": { "schema": self.schema(*schema, Side::Read) },
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
        let changes = !matches!(route.method, "get" | "head" | "options");
        let forbidden = route.auth.as_ref().is_some_and(|auth| {
            let checks_origin = auth
                .schemes
                .iter()
                .filter_map(|scheme| self.index(scheme))
                .any(|index| self.refs[index].checks_origin);

            !auth.scopes.is_empty() || (changes && checks_origin)
        });
        let listed = [reads, route.auth.is_some(), forbidden, route.body.is_some()];

        for ((name, status, _), listed) in ERRORS.iter().zip(listed) {
            if listed {
                self.errors.insert(name);
                responses.insert((*status).into(), reference(name));
            }
        }

        responses.insert("default".into(), reference("Problem"));

        Value::Object(responses)
    }

    /// The schemes the route takes, as they describe themselves.
    fn route_schemes(&self, route: &Route) -> impl Iterator<Item = &SecurityScheme> {
        route
            .auth
            .iter()
            .flat_map(|auth| auth.schemes)
            .filter_map(|scheme| self.index(scheme))
            .map(|index| &self.schemes[index])
    }

    /// Where `scheme` is among the api's, found by its name.
    fn index(&self, scheme: &SchemeRef) -> Option<usize> {
        self.refs
            .iter()
            .position(|listed| listed.name == scheme.name)
    }

    fn success(&mut self, reply: &Reply) -> (&'static str, Value) {
        match reply {
            Reply::Empty => ("204", json!({ "description": "No Content" })),
            Reply::Json(schema) => ("200", self.json("OK", *schema)),
            Reply::Created(schema) => {
                let mut created = self.json("Created", *schema);
                created["headers"] = json!({
                    "Location": {
                        "description": "Where the created resource is read, when the server says.",
                        "schema": { "type": "string", "format": "uri-reference" },
                    },
                });

                ("201", created)
            }
            Reply::Accepted(schema) => ("202", self.json("Accepted", *schema)),
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
                            "x-item": self.schema(*item, Side::Written),
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
                            "x-item": self.schema(*item, Side::Written),
                        },
                    },
                }),
            ),
        }
    }

    /// A JSON result, `description`, of the type `schema` describes.
    fn json(&mut self, description: &str, schema: SchemaFn) -> Value {
        json!({
            "description": description,
            "content": { "application/json": { "schema": self.schema(schema, Side::Written) } },
        })
    }

    /// The components but the schemas: the responses of errors the routes list, and the api's schemes.
    fn responses_and_schemes(&self) -> Map<String, Value> {
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
        // Every route lists it as its `default`.
        responses.insert(
            "Problem".into(),
            json!({ "description": "An error, as an RFC 9457 problem.", "content": problem }),
        );

        let mut components = Map::new();
        components.insert("responses".into(), Value::Object(responses));

        let mut schemes = Map::new();
        for (scheme, described) in self.refs.iter().zip(&self.schemes) {
            let described = serde_json::to_value(described)
                .expect("a security scheme serializes as a map of strings");

            schemes.entry(scheme.name).or_insert(described);
        }
        if !schemes.is_empty() {
            components.insert("securitySchemes".into(), Value::Object(schemes));
        }

        components
    }
}

/// The requirements of a route: any of its schemes, each with all its scopes, or none at all when its credential is
/// optional.
fn security(auth: &Auth) -> Value {
    let mut requirements: Vec<Value> = auth
        .schemes
        .iter()
        .map(|scheme| json!({ scheme.name: auth.scopes }))
        .collect();

    if auth.optional {
        requirements.push(json!({}));
    }

    Value::Array(requirements)
}

/// A reference to a response under `components/responses`.
fn reference(name: &str) -> Value {
    json!({ "$ref": format!("#/components/responses/{name}") })
}

/// The parameters of a query struct, named `name`, whose schema is `schema`: one per field, sorted by name, when it is
/// a plain object; else one parameter holding it, written as a form.
fn unfold(name: &str, schema: Value) -> Vec<(String, &'static str, Value)> {
    let plain = schema.get("type") == Some(&Value::from("object"))
        && ["allOf", "anyOf", "oneOf", "$ref"]
            .iter()
            .all(|key| schema.get(key).is_none());
    let properties = schema.get("properties").and_then(Value::as_object);
    let (true, Some(properties)) = (plain, properties) else {
        // Not required: an empty query is what the server reads when every field may be missing, and what it refuses
        // otherwise, which the schema says.
        let parameter = json!({
            "name": name,
            "in": "query",
            "required": false,
            "style": "form",
            "explode": true,
            "schema": schema,
        });

        return vec![(name.to_owned(), "query", parameter)];
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

            (key.clone(), "query", parameter)
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
        match types.len() {
            // A schema of `null` alone says nothing a parameter can be: any value.
            0 => {
                object.remove("type");
            }
            1 => {
                let only = types.remove(0);
                object.insert("type".into(), only);
            }
            _ => {}
        }
    }
    if object.get("type") == Some(&Value::from("null")) {
        object.remove("type");
    }

    if let Some(Value::Array(values)) = object.get_mut("enum") {
        values.retain(|value| !value.is_null());
        if values.is_empty() {
            object.remove("enum");
        }
    }
    if object.get("const") == Some(&Value::Null) {
        object.remove("const");
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

/// The lines of `///` comments, as rustc gives them: an attribute per `///` line, or one holding the lines of a
/// `/** */` block, with the `*` its lines may start with, all without the indentation they share, as rustdoc reads
/// them.
fn lines(docs: &[&str]) -> Vec<String> {
    let lines: Vec<&str> = docs.iter().flat_map(|doc| block_lines(doc)).collect();
    let indent = lines
        .iter()
        .filter(|line| !line.trim().is_empty())
        .map(|line| line.len() - line.trim_start().len())
        .min()
        .unwrap_or(0);

    lines
        .iter()
        .map(|line| line.get(indent..).unwrap_or_default().trim_end().to_owned())
        .collect()
}

/// The lines of one attribute: itself for a `///` line; for a `/** */` block, its lines, without the `*` that starts
/// each of them, when each does.
fn block_lines(doc: &str) -> Vec<&str> {
    let lines: Vec<&str> = doc.split('\n').collect();
    let starred = lines.len() > 1
        && lines
            .iter()
            .filter(|line| !line.trim().is_empty())
            .all(|line| line.trim_start().starts_with('*'));

    if !starred {
        return lines;
    }

    lines
        .into_iter()
        .map(|line| match line.trim_start().strip_prefix('*') {
            Some(rest) => rest,
            None => line,
        })
        .collect()
}

/// The text of the `///` comments, if there is any.
fn text(docs: &[&str]) -> Option<String> {
    let text = lines(docs).join("\n");
    let text = text.trim();

    (!text.is_empty()).then(|| text.to_owned())
}

/// The summary of the `///` comments, their first paragraph on one line, and their description, the paragraphs after
/// it.
fn summary(docs: &[&str]) -> (Option<String>, Option<String>) {
    let lines = lines(docs);
    let start = lines
        .iter()
        .position(|line| !line.is_empty())
        .unwrap_or(lines.len());
    let lines = &lines[start..];
    let end = lines
        .iter()
        .position(String::is_empty)
        .unwrap_or(lines.len());

    let summary: Vec<&str> = lines[..end].iter().map(|line| line.trim()).collect();
    let summary = (!summary.is_empty()).then(|| summary.join(" "));
    let rest = lines[end..].join("\n");
    let rest = rest.trim();
    let description = (!rest.is_empty()).then(|| rest.to_owned());

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
    fn a_block_comment_is_split_into_its_lines() {
        let block = "\n     Notes.\n\n     A page at a time:\n\n         sorted.\n     ";

        assert_eq!(
            summary(&[block]),
            (
                Some("Notes.".into()),
                Some("A page at a time:\n\n    sorted.".into())
            )
        );
        assert_eq!(
            summary(&["\n     * Notes.\n     *\n     * More.\n     "]),
            (Some("Notes.".into()), Some("More.".into()))
        );
        assert_eq!(
            summary(&[" * One.", " * Two."]),
            (Some("* One. * Two.".into()), None),
            "a list of `///` lines keeps its stars"
        );
        assert_eq!(text(&[" One.", " Two."]), Some("One.\nTwo.".into()));
    }

    #[test]
    fn a_declared_scheme_is_described_by_its_comments() {
        let scheme = described(
            SecurityScheme::header("X-Api-Key"),
            &[" The key.", " Of scripts."],
        );

        assert_eq!(
            scheme,
            SecurityScheme::header("X-Api-Key").with_description("The key.\nOf scripts.")
        );
        assert_eq!(
            described(SecurityScheme::bearer(), &[]),
            SecurityScheme::bearer()
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
        assert_eq!(
            not_null(json!({ "type": ["null"], "description": "Always null." })),
            json!({ "description": "Always null." })
        );
        assert_eq!(not_null(json!({ "type": "null" })), json!({}));
        assert_eq!(not_null(json!({ "enum": [null] })), json!({}));
        assert_eq!(not_null(json!({ "const": null })), json!({}));
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

        let parameters: Vec<Value> = unfold("filter", schema)
            .into_iter()
            .map(|(_, _, parameter)| parameter)
            .collect();

        assert_eq!(
            parameters,
            vec![
                json!({ "name": "contains", "in": "query", "required": false, "description": "A word.", "schema": { "type": "string" } }),
                json!({ "name": "page", "in": "query", "required": false, "schema": { "type": "integer", "format": "uint32" } }),
                json!({ "name": "tag", "in": "query", "required": true, "schema": { "type": "string" } }),
            ]
        );
    }

    #[test]
    fn a_query_struct_that_is_not_plain_is_one_form_that_may_be_missing() {
        let schema =
            json!({ "type": "object", "allOf": [{ "$ref": "#/components/schemas/Page" }] });

        assert_eq!(
            unfold("filter", schema.clone()),
            vec![(
                "filter".to_owned(),
                "query",
                json!({ "name": "filter", "in": "query", "required": false, "style": "form", "explode": true, "schema": schema })
            )]
        );
    }

    #[test]
    fn names_are_compared_whole() {
        assert!(same_name("Key", "Key"));
        assert!(!same_name("Key", "Keys"));
        assert!(!same_name("Key", "key"));
    }

    #[test]
    fn a_component_name_is_letters_digits_and_three_signs() {
        for name in ["Bearer", "api_key", "Key-2", "v1.Key"] {
            assert!(is_component_name(name), "{name}");
        }
        for name in ["", "Api Key", "Key/2", "clé", "Key#"] {
            assert!(!is_component_name(name), "{name}");
        }
    }
}

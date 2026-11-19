//! The OpenAPI 3.1 document of a contract that says everything a contract can, whatever the features: each part as
//! the server answers it, and the whole valid against the official schema of OpenAPI 3.1.

use std::sync::LazyLock;

use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{self, Accepted, Bytes, Created, Events, Form, JsonLines, Result, Scheme, Text};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

/// A note.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
    title: String,
}

/// The filter of a search.
#[derive(Debug, Default, Serialize, Deserialize, JsonSchema)]
struct NoteFilter {
    /// A word the notes hold.
    contains: Option<String>,
    page: Option<u32>,
    #[serde(default)]
    tag: Vec<String>,
    sort: Order,
}

#[derive(Debug, Default, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Order {
    #[default]
    Newest,
    Oldest,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Login {
    user: String,
    password: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum Change {
    Edited { id: i64 },
    Removed { id: i64 },
}

/// A type named as the problem of errors, which takes another name.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Problem {
    code: u32,
}

mod archive {
    /// A note of the archive, named as the other `Note`.
    #[derive(Debug, serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
    pub struct Note {
        year: u16,
    }
}

/// A request signed with a key the server knows.
struct Hmac;

impl Scheme for Hmac {
    type Credential = String;

    const NAME: &'static str = "Hmac";

    fn read(request: &Parts) -> Result<Option<String>> {
        Ok(request
            .headers
            .get("x-signature")
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned))
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature").with_description("`<key id>:<signature>`")
    }
}

mod auth {
    pub(crate) use super::Hmac;
}

rest::api! {
    /// Notes, kept per user.
    ///
    /// Each user sees their own.
    api Api = "/api";

    /// The key of an operator's scripts.
    scheme Key = header "X-Api-Key";
    scheme Session = cookie "__Host-session";
    scheme Token = query "token";
    scheme Oidc = openid "https://id.example.com/.well-known/openid-configuration";

    group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> String;
    }

    /// The signed-in user's notes.
    #[auth(Bearer | Session)]
    group Notes {
        /// Notes matching the filter,
        /// a page at a time.
        ///
        /// Sorted as the filter says.
        ///
        /// Empty when none matches.
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        GET    "/notes/{id}?fields={fields}&v={version}"
               => get(id: i64, fields: Vec<String>, version: Option<u32>, header("If-None-Match") tag: Option<String>, header("X-Trace") trace: String) -> Note;
        POST   "/notes"             => create(note: Note) -> Created<Note>;
        DELETE "/notes/{id}"        => remove(id: i64);
        GET    "/notes/{id}/events" => events(id: i64) -> Events<Change>;
        #[auth(none)]
        GET    "/public"            => public() -> Text;
    }

    group Files {
        PUT  "/files/{name}" => upload(name: String, file: Bytes) -> Accepted<archive::Note>;
        GET  "/files/{name}" => download(name: String) -> Bytes;
        POST "/notes/text"   => text(body: Text) -> Problem;
        GET  "/lines"        => lines() -> JsonLines<String>;
        HEAD "/files/{name}" => exists(name: String);
    }

    #[auth(Key | crate::auth::Hmac, scopes("admin", "audit"))]
    group Admin {
        GET "/admin/stats" => stats() -> u64;
        #[auth(Basic, optional)]
        GET "/admin/hello" => hello() -> String;
        #[auth(Token | Oidc)]
        POST "/admin/reindex" => reindex();
    }
}

static DOCUMENT: LazyLock<Value> =
    LazyLock::new(|| serde_json::from_str(&Api::openapi().to_json()).unwrap());

fn operation(path: &str, method: &str) -> &'static Value {
    let operation = &DOCUMENT["paths"][path][method];
    assert!(operation.is_object(), "no {method} {path}: {}", *DOCUMENT);

    operation
}

fn responses(path: &str, method: &str) -> Vec<&'static str> {
    operation(path, method)["responses"]
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect()
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

    for (name, schema) in DOCUMENT["components"]["schemas"].as_object().unwrap() {
        assert!(
            jsonschema::draft202012::meta::is_valid(schema),
            "{name}: {schema}"
        );
    }
}

#[test]
fn the_validator_refuses_a_document_that_is_not_openapi() {
    let schema: Value = serde_json::from_str(include_str!("openapi/schema-3.1.json")).unwrap();
    let validator = jsonschema::validator_for(&schema).unwrap();
    let mut document = DOCUMENT.clone();
    document["paths"]["/api/lines"]["get"]["responses"]["200"]
        .as_object_mut()
        .unwrap()
        .remove("description");

    assert!(!validator.is_valid(&document));
}

#[test]
fn the_info_is_the_crates_and_the_apis() {
    assert_eq!(DOCUMENT["openapi"], "3.1.1");
    assert_eq!(
        DOCUMENT["info"],
        json!({
            "title": "quoll",
            "version": env!("CARGO_PKG_VERSION"),
            "description": "Notes, kept per user.\n\nEach user sees their own.",
        })
    );

    let named = Api::openapi().with_title("Notes").with_version("2.0.0");
    let named = serde_json::to_value(&named).unwrap();

    assert_eq!(named["info"]["title"], "Notes");
    assert_eq!(named["info"]["version"], "2.0.0");
}

#[test]
fn the_json_is_the_document_indented_or_not() {
    let document = Api::openapi();
    let pretty: Value = serde_json::from_str(&document.to_json_pretty()).unwrap();

    assert_eq!(pretty, *DOCUMENT);
    assert_eq!(serde_json::to_value(&document).unwrap(), *DOCUMENT);
    assert!(document.to_json_pretty().contains("\n  \"info\": {"));
    assert!(!document.to_json().contains('\n'));
}

#[test]
fn each_group_is_a_tag_and_each_route_an_operation() {
    assert_eq!(
        DOCUMENT["tags"],
        json!([
            { "name": "Auth" },
            { "name": "Notes", "description": "The signed-in user's notes." },
            { "name": "Files" },
            { "name": "Admin" },
        ])
    );

    let search = operation("/api/notes", "get");
    assert_eq!(search["operationId"], "notes_search");
    assert_eq!(search["tags"], json!(["Notes"]));
    assert_eq!(
        search["summary"],
        "Notes matching the filter, a page at a time."
    );
    assert_eq!(
        search["description"],
        "Sorted as the filter says.\n\nEmpty when none matches."
    );

    let get = operation("/api/notes/{id}", "get");
    assert_eq!(get["operationId"], "notes_get");
    assert!(get.get("summary").is_none() && get.get("description").is_none());

    assert_eq!(
        operation("/api/token", "post")["summary"],
        "Trades a login for a token."
    );
    assert_eq!(
        operation("/api/files/{name}", "head")["operationId"],
        "files_exists"
    );
}

#[test]
fn parameters_are_written_where_they_are_read() {
    assert_eq!(
        operation("/api/notes/{id}", "get")["parameters"],
        json!([
            { "name": "id", "in": "path", "required": true, "schema": { "type": "integer", "format": "int64" } },
            { "name": "fields", "in": "query", "required": false, "schema": { "type": "array", "items": { "type": "string" } } },
            { "name": "v", "in": "query", "required": false, "schema": { "type": "integer", "format": "uint32", "minimum": 0 } },
            { "name": "If-None-Match", "in": "header", "required": false, "schema": { "type": "string" } },
            { "name": "X-Trace", "in": "header", "required": true, "schema": { "type": "string" } },
        ])
    );
}

#[test]
fn a_query_struct_is_unfolded_into_its_keys() {
    let parameters = &operation("/api/notes", "get")["parameters"];

    assert_eq!(
        *parameters,
        json!([
            { "name": "contains", "in": "query", "required": false, "description": "A word the notes hold.", "schema": { "type": "string" } },
            { "name": "page", "in": "query", "required": false, "schema": { "type": "integer", "format": "uint32", "minimum": 0 } },
            { "name": "sort", "in": "query", "required": true, "schema": { "$ref": "#/components/schemas/Order" } },
            { "name": "tag", "in": "query", "required": false, "schema": { "type": "array", "items": { "type": "string" }, "default": [] } },
        ])
    );
    assert!(
        DOCUMENT["components"]["schemas"]
            .get("NoteFilter")
            .is_none(),
        "the struct itself is no schema of the document"
    );
}

#[test]
fn each_body_is_written_with_its_content_type() {
    assert_eq!(
        operation("/api/notes", "post")["requestBody"],
        json!({ "required": true, "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Note" } } } })
    );
    assert_eq!(
        operation("/api/token", "post")["requestBody"],
        json!({ "required": true, "content": { "application/x-www-form-urlencoded": { "schema": { "$ref": "#/components/schemas/Login" } } } })
    );
    assert_eq!(
        operation("/api/notes/text", "post")["requestBody"]["content"],
        json!({ "text/plain": { "schema": { "type": "string" } } })
    );

    let upload = &operation("/api/files/{name}", "put")["requestBody"];
    assert_eq!(upload["content"], json!({ "application/octet-stream": {} }));
    assert!(
        upload["description"]
            .as_str()
            .unwrap()
            .contains("`text/plain`")
    );
}

#[test]
fn each_result_is_written_with_its_status_and_content_type() {
    let ok = |path: &str, method: &str, status: &str| {
        operation(path, method)["responses"][status].clone()
    };

    assert_eq!(
        ok("/api/notes/{id}", "get", "200")["content"],
        json!({ "application/json": { "schema": { "$ref": "#/components/schemas/Note" } } })
    );
    assert_eq!(
        ok("/api/notes", "get", "200")["content"]["application/json"]["schema"],
        json!({ "type": "array", "items": { "$ref": "#/components/schemas/Note" } })
    );

    let created = ok("/api/notes", "post", "201");
    assert_eq!(
        created["content"]["application/json"]["schema"]["$ref"],
        "#/components/schemas/Note"
    );
    assert_eq!(
        created["headers"]["Location"]["schema"],
        json!({ "type": "string", "format": "uri-reference" })
    );

    assert_eq!(
        ok("/api/files/{name}", "put", "202")["content"]["application/json"]["schema"]["$ref"],
        "#/components/schemas/Note2"
    );
    assert_eq!(
        ok("/api/notes/{id}", "delete", "204"),
        json!({ "description": "No Content" })
    );
    assert_eq!(
        ok("/api/files/{name}", "head", "204"),
        json!({ "description": "No Content" })
    );
    assert_eq!(
        ok("/api/public", "get", "200")["content"],
        json!({ "text/plain": { "schema": { "type": "string" } } })
    );
    assert_eq!(
        ok("/api/files/{name}", "get", "200")["content"],
        json!({ "*/*": {} })
    );
}

#[test]
fn a_stream_carries_the_schema_of_its_items_in_x_item() {
    let events = &operation("/api/notes/{id}/events", "get")["responses"]["200"];

    assert_eq!(
        events["content"],
        json!({ "text/event-stream": { "schema": { "type": "string" }, "x-item": { "$ref": "#/components/schemas/Change" } } })
    );
    assert!(
        events["description"]
            .as_str()
            .unwrap()
            .contains("event `problem`")
    );
    assert_eq!(
        operation("/api/lines", "get")["responses"]["200"]["content"],
        json!({ "application/jsonl": { "schema": { "type": "string" }, "x-item": { "type": "string" } } })
    );
    assert!(DOCUMENT["components"]["schemas"]["Change"]["oneOf"].is_array());
}

#[test]
fn each_route_lists_the_errors_quoll_answers_itself() {
    // Nothing read, no credential.
    assert_eq!(responses("/api/lines", "get"), ["200", "default"]);
    // An argument.
    assert_eq!(
        responses("/api/files/{name}", "get"),
        ["200", "400", "default"]
    );
    // A body.
    assert_eq!(
        responses("/api/token", "post"),
        ["200", "400", "415", "default"]
    );
    // A credential, with a cookie on a method that only reads.
    assert_eq!(
        responses("/api/notes/{id}", "get"),
        ["200", "400", "401", "default"]
    );
    // A cookie on a method that changes things.
    assert_eq!(
        responses("/api/notes", "post"),
        ["201", "400", "401", "403", "415", "default"]
    );
    assert_eq!(
        responses("/api/notes/{id}", "delete"),
        ["204", "400", "401", "403", "default"]
    );
    // Scopes.
    assert_eq!(
        responses("/api/admin/stats", "get"),
        ["200", "401", "403", "default"]
    );
    // An optional credential, which a wrong one still fails.
    assert_eq!(
        responses("/api/admin/hello", "get"),
        ["200", "401", "default"]
    );

    let get = &operation("/api/notes/{id}", "get")["responses"];
    assert_eq!(
        get["400"],
        json!({ "$ref": "#/components/responses/BadRequest" })
    );
    assert_eq!(
        get["401"],
        json!({ "$ref": "#/components/responses/Unauthorized" })
    );
    assert_eq!(
        get["default"],
        json!({ "$ref": "#/components/responses/Problem" })
    );

    let components = &DOCUMENT["components"]["responses"];
    let problem = json!({ "application/problem+json": { "schema": { "$ref": "#/components/schemas/Problem" } } });
    for name in [
        "BadRequest",
        "Unauthorized",
        "Forbidden",
        "UnsupportedMediaType",
        "Problem",
    ] {
        assert_eq!(components[name]["content"], problem, "{name}");
    }
    assert!(components["Unauthorized"]["headers"]["WWW-Authenticate"].is_object());
}

#[test]
fn the_problem_keeps_its_name_beside_a_type_named_as_it() {
    let schemas = &DOCUMENT["components"]["schemas"];

    assert_eq!(
        schemas["Problem"]["properties"]["type"]["default"],
        "about:blank"
    );
    assert_eq!(
        schemas["Problem"]["properties"]["status"]["type"],
        "integer"
    );
    assert_eq!(
        schemas["Problem2"]["properties"]["code"]["format"],
        "uint32"
    );
    assert_eq!(
        operation("/api/notes/text", "post")["responses"]["200"]["content"]["application/json"]["schema"]
            ["$ref"],
        "#/components/schemas/Problem2"
    );
}

#[test]
fn two_types_of_one_name_take_a_suffix_in_the_order_the_contract_names_them() {
    let schemas = &DOCUMENT["components"]["schemas"];

    assert_eq!(schemas["Note"]["description"], "A note.");
    assert_eq!(
        schemas["Note2"]["description"],
        "A note of the archive, named as the other `Note`."
    );
}

#[test]
fn each_scheme_is_described_by_its_name() {
    assert_eq!(
        DOCUMENT["components"]["securitySchemes"],
        json!({
            "Bearer": { "type": "http", "scheme": "bearer" },
            "Basic": { "type": "http", "scheme": "basic" },
            "Key": { "type": "apiKey", "in": "header", "name": "X-Api-Key", "description": "The key of an operator's scripts." },
            "Session": { "type": "apiKey", "in": "cookie", "name": "__Host-session" },
            "Token": { "type": "apiKey", "in": "query", "name": "token" },
            "Oidc": { "type": "openIdConnect", "openIdConnectUrl": "https://id.example.com/.well-known/openid-configuration" },
            "Hmac": { "type": "apiKey", "in": "header", "name": "X-Signature", "description": "`<key id>:<signature>`" },
        })
    );
}

#[test]
fn each_route_takes_any_of_its_schemes() {
    assert_eq!(
        operation("/api/notes/{id}", "get")["security"],
        json!([{ "Bearer": [] }, { "Session": [] }])
    );
    assert_eq!(
        operation("/api/admin/stats", "get")["security"],
        json!([{ "Key": ["admin", "audit"] }, { "Hmac": ["admin", "audit"] }])
    );
    assert_eq!(
        operation("/api/admin/hello", "get")["security"],
        json!([{ "Basic": [] }, {}])
    );
    assert_eq!(
        operation("/api/admin/reindex", "post")["security"],
        json!([{ "Token": [] }, { "Oidc": [] }])
    );
    assert!(operation("/api/public", "get").get("security").is_none());
    assert!(operation("/api/token", "post").get("security").is_none());
}

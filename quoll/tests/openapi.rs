//! The OpenAPI 3.1 document of a contract that says everything a contract can, whatever the features: each part as
//! the server answers it, and the whole valid against the official schema of OpenAPI 3.1.

use std::sync::LazyLock;

use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{
    self, Accepted, ByteStream, Bytes, Created, Events, Form, JsonLines, Result, Scheme, Text,
};
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
    Moved(Note),
}

/// A type named as the problem of errors, which takes another name.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Problem {
    code: u32,
}

/// Labels, which a client may leave out and always receives: two schemas.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Labels {
    #[serde(default)]
    tags: Vec<String>,
}

/// A shelf of labels, which differs as they do.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Shelf {
    labels: Labels,
}

/// A search, whose `token` the query scheme reads.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Find {
    q: String,
    token: Option<String>,
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

/// A session cookie of the application's own, which checks no origin.
struct Crumb;

impl Scheme for Crumb {
    type Credential = String;

    const NAME: &'static str = "Crumb";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::cookie("crumb")
    }
}

/// A note under a name that cannot be a component's.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(rename = "Old Note")]
struct Renamed {
    text: String,
}

/// A page of notes, whose schema is named as Rust writes it.
#[derive(Debug, Serialize, Deserialize)]
struct Page {
    notes: Vec<Note>,
}

impl JsonSchema for Page {
    fn schema_name() -> std::borrow::Cow<'static, str> {
        "Page<Note>".into()
    }

    fn json_schema(generator: &mut schemars::SchemaGenerator) -> schemars::Schema {
        let notes = generator.subschema_for::<Vec<Note>>();

        schemars::json_schema!({ "type": "object", "properties": { "notes": notes } })
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
        GET  "/output"       => output() -> ByteStream;
        HEAD "/files/{name}" => exists(name: String);
        PUT  "/shelves"      => shelve(shelf: Shelf) -> Shelf;
        /**
         * Labels, one block.
         *
         * Read back as written.
         */
        PUT  "/labels"       => label(labels: Labels) -> Labels;
        #[auth(Token)]
        GET  "/find"         => find(query find: Find) -> Vec<String>;
        GET  "/old"          => old() -> Renamed;
        GET  "/page"         => page() -> Page;
        GET  "/drafts/{id}"  => draft(id: i64) -> Option<Page>;
        #[auth(Crumb)]
        POST "/crumbs"       => crumb();
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
fn a_reference_beside_other_keywords_is_an_all_of() {
    let moved = &DOCUMENT["components"]["schemas"]["Change"]["oneOf"][2];

    assert_eq!(
        moved,
        &json!({
            "allOf": [
                { "$ref": "#/components/schemas/Note" },
                {
                    "type": "object",
                    "properties": { "kind": { "type": "string", "const": "moved" } },
                    "required": ["kind"],
                },
            ],
        })
    );
}

#[test]
fn a_byte_stream_is_bytes_of_any_type_said_to_stream() {
    let output = &operation("/api/output", "get")["responses"]["200"];

    assert_eq!(output["content"], json!({ "*/*": {} }));
    assert!(
        output["description"]
            .as_str()
            .unwrap()
            .contains("sent as they come")
    );
}

#[test]
fn a_result_that_may_be_empty_lists_a_204() {
    let draft = &operation("/api/drafts/{id}", "get")["responses"];

    assert_eq!(
        responses("/api/drafts/{id}", "get"),
        ["200", "204", "400", "default"]
    );
    assert_eq!(draft["204"], json!({ "description": "No Content" }));

    for (path, method) in [
        ("/api/public", "get"),
        ("/api/files/{name}", "get"),
        ("/api/notes/{id}/events", "get"),
        ("/api/output", "get"),
    ] {
        assert!(responses(path, method).contains(&"204"), "{method} {path}");
    }

    // A value, whatever its status: a 204 is no answer of it.
    for (path, method) in [
        ("/api/page", "get"),
        ("/api/notes", "post"),
        ("/api/files/{name}", "put"),
    ] {
        assert!(!responses(path, method).contains(&"204"), "{method} {path}");
    }
}

#[test]
fn each_route_lists_the_errors_quoll_answers_itself() {
    // Nothing read, no credential, a result that may be empty.
    assert_eq!(responses("/api/lines", "get"), ["200", "204", "default"]);
    // An argument, bytes that may be empty.
    assert_eq!(
        responses("/api/files/{name}", "get"),
        ["200", "204", "400", "default"]
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
            "Crumb": { "type": "apiKey", "in": "cookie", "name": "crumb" },
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

#[test]
fn a_type_read_and_written_alike_has_one_schema() {
    let schemas = &DOCUMENT["components"]["schemas"];

    assert!(schemas.get("Note").is_some());
    assert!(schemas.get("NoteInput").is_none() && schemas.get("NoteOutput").is_none());
    assert_eq!(
        operation("/api/notes", "post")["requestBody"]["content"]["application/json"]["schema"]["$ref"],
        "#/components/schemas/Note"
    );
}

#[test]
fn a_type_read_and_written_otherwise_has_both_schemas() {
    let schemas = &DOCUMENT["components"]["schemas"];

    // A client may leave out what it always receives.
    assert_eq!(schemas["LabelsInput"].get("required"), None);
    assert_eq!(schemas["LabelsOutput"]["required"], json!(["tags"]));
    assert!(schemas.get("Labels").is_none());

    // A type that holds one of them is split too, each side referring to its own.
    assert_eq!(
        schemas["ShelfInput"]["properties"]["labels"]["$ref"],
        "#/components/schemas/LabelsInput"
    );
    assert_eq!(
        schemas["ShelfOutput"]["properties"]["labels"]["$ref"],
        "#/components/schemas/LabelsOutput"
    );

    let shelve = operation("/api/shelves", "put");
    assert_eq!(
        shelve["requestBody"]["content"]["application/json"]["schema"]["$ref"],
        "#/components/schemas/ShelfInput"
    );
    assert_eq!(
        shelve["responses"]["200"]["content"]["application/json"]["schema"]["$ref"],
        "#/components/schemas/ShelfOutput"
    );
}

#[test]
fn a_block_comment_is_split_into_paragraphs() {
    let label = operation("/api/labels", "put");

    assert_eq!(label["summary"], "Labels, one block.");
    assert_eq!(label["description"], "Read back as written.");
}

#[test]
fn the_key_of_a_query_scheme_is_no_parameter() {
    assert_eq!(
        operation("/api/find", "get")["parameters"],
        json!([{ "name": "q", "in": "query", "required": true, "schema": { "type": "string" } }])
    );
    assert_eq!(
        operation("/api/find", "get")["security"],
        json!([{ "Token": [] }])
    );
}

#[test]
fn a_name_that_cannot_be_a_components_is_written_with_underscores() {
    let schemas = &DOCUMENT["components"]["schemas"];

    assert!(schemas.get("Old_Note").is_some(), "{schemas}");
    assert!(schemas.get("Page_Note_").is_some(), "{schemas}");
    assert_eq!(
        operation("/api/old", "get")["responses"]["200"]["content"]["application/json"]["schema"]["$ref"],
        "#/components/schemas/Old_Note"
    );
    assert_eq!(
        operation("/api/page", "get")["responses"]["200"]["content"]["application/json"]["schema"]
            ["$ref"],
        "#/components/schemas/Page_Note_"
    );
    for name in schemas.as_object().unwrap().keys() {
        assert!(
            name.chars()
                .all(|c| c.is_ascii_alphanumeric() || "._-".contains(c)),
            "{name}"
        );
    }
}

#[test]
fn only_a_declared_cookie_scheme_answers_403_for_another_origin() {
    // The application's own cookie scheme checks no origin, and lists no 403.
    assert_eq!(responses("/api/crumbs", "post"), ["204", "401", "default"]);
    // The declared one does.
    assert_eq!(
        responses("/api/notes/{id}", "delete"),
        ["204", "400", "401", "403", "default"]
    );
}

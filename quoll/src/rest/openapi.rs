//! The OpenAPI 3.1 document of a contract, which `Api::openapi()` returns, and the security schemes it describes.
//!
//! The document holds the api's paths, under its prefix, with their parameters, bodies and results; the schemas of
//! the contract's types, from schemars; the security schemes and each route's requirements; and the RFC 9457 problem
//! that errors answer. Each type a contract names implements `schemars::JsonSchema`, whatever the features of
//! `quoll`: a type that does not fails to compile on the contract.
//!
//! - **Schemas** sit under `components/schemas`, named by schemars: `Note`, or `Note2` for a second type named
//!   `Note` in another module, in the order the contract names them. `#[schemars(rename = "OtherNote")]` gives such a
//!   type a name of its own. A type is described as the server reads it, as schemars does by default.
//! - **Operations** are named `group_function`, `notes_get`, and tagged by their group. The first paragraph of a
//!   route's `///` is its summary, the rest its description; a group's is the description of its tag, the api's the
//!   document's.
//! - **Parameters**: a query struct is unfolded, one query parameter per field. An `Option` and a `Vec` are not
//!   required, and a parameter is never `null`.
//! - **Bodies**: JSON, a form, `text/plain`, or `application/octet-stream` for `Bytes`, which accept any media type but
//!   the three a browser sends to another site without asking.
//! - **Results**: 200, 201 with an optional `Location`, 202 or 204; `Bytes` as `*/*`. A stream result,
//!   `text/event-stream` or `application/jsonl`, carries the schema of its items in the extension `x-item`.
//! - **Errors**: each route lists the statuses quoll answers itself, 400 when it reads an argument, 401 when it takes a
//!   credential, 403 when it needs scopes or takes a cookie on a method that changes things, 415 when it reads a
//!   body, and `default` for any other, each an `application/problem+json` problem.
//! - **Security**: each scheme under `components/securitySchemes`, by its [`NAME`](crate::rest::Scheme::NAME), as its
//!   [`openapi`](crate::rest::Scheme::openapi) describes it. A route takes any of its schemes, each with its scopes,
//!   and an empty requirement when its credential is optional.
//! - **`info`**: the title and the version of the contract's crate, unless [`Document::with_title`] and
//!   [`Document::with_version`] say others.

use serde::ser::{Serialize, SerializeMap, Serializer};
use serde_json::Value;

/// The OpenAPI 3.1 document of a contract, from `Api::openapi()`, which a binary of the contract's crate prints for
/// the generators of TypeScript:
///
/// ```
/// use quoll::rest;
///
/// rest::api! {
///     /// Notes, kept per user.
///     pub api Api = "/api";
///
///     pub group Notes {
///         /// A note's text.
///         GET "/notes/{id}" => get(id: i64) -> String;
///     }
/// }
///
/// fn main() {
///     println!("{}", Api::openapi().to_json_pretty());
/// }
/// ```
#[derive(Debug, Clone, PartialEq)]
pub struct Document {
    value: Value,
}

impl Document {
    /// The document of `value`, which the crate builds.
    pub(crate) fn new(value: Value) -> Self {
        Self { value }
    }

    /// The document in JSON, on one line.
    pub fn to_json(&self) -> String {
        self.value.to_string()
    }

    /// The document in JSON, indented, as a file is written.
    pub fn to_json_pretty(&self) -> String {
        serde_json::to_string_pretty(&self.value)
            .expect("a JSON value, whose keys are strings, always serializes")
    }

    /// The document with `title` as its `info.title`, which is the name of the contract's crate otherwise.
    #[must_use]
    pub fn with_title(self, title: impl Into<String>) -> Self {
        self.with_info("title", title.into())
    }

    /// The document with `version` as its `info.version`, which is the version of the contract's crate otherwise.
    #[must_use]
    pub fn with_version(self, version: impl Into<String>) -> Self {
        self.with_info("version", version.into())
    }

    fn with_info(mut self, key: &str, value: String) -> Self {
        if let Some(info) = self.value.get_mut("info").and_then(Value::as_object_mut) {
            info.insert(key.to_owned(), Value::String(value));
        }

        self
    }
}

impl Serialize for Document {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.value.serialize(serializer)
    }
}

/// How the OpenAPI document describes a [`Scheme`](crate::rest::Scheme): what its
/// [`openapi`](crate::rest::Scheme::openapi) returns, under `components/securitySchemes`.
///
/// ```
/// use quoll::rest::openapi::SecurityScheme;
///
/// let signature = SecurityScheme::header("X-Signature").with_description("`<key id>:<signature>`");
/// # let _ = signature;
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct SecurityScheme {
    kind: Kind,
    description: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
enum Kind {
    Http(String),
    ApiKey { place: &'static str, name: String },
    OpenIdConnect(String),
}

impl SecurityScheme {
    /// `Authorization: Bearer`, as [`Bearer`](crate::rest::Bearer) reads it: `{type: http, scheme: bearer}`.
    pub fn bearer() -> Self {
        Self::http("bearer")
    }

    /// `Authorization: Basic`, as [`Basic`](crate::rest::Basic) reads it: `{type: http, scheme: basic}`.
    pub fn basic() -> Self {
        Self::http("basic")
    }

    /// Another scheme of `Authorization`, by its name, as the IANA registry of HTTP authentication schemes names it:
    /// `{type: http, scheme}`.
    pub fn http(scheme: impl Into<String>) -> Self {
        Self::of(Kind::Http(scheme.into()))
    }

    /// A key in the header `name`: `{type: apiKey, in: header, name}`.
    pub fn header(name: impl Into<String>) -> Self {
        Self::api_key("header", name.into())
    }

    /// A key in the cookie `name`: `{type: apiKey, in: cookie, name}`.
    pub fn cookie(name: impl Into<String>) -> Self {
        Self::api_key("cookie", name.into())
    }

    /// A key in the query key `name`: `{type: apiKey, in: query, name}`.
    pub fn query(name: impl Into<String>) -> Self {
        Self::api_key("query", name.into())
    }

    /// A token of the OpenID Connect provider whose discovery document is at `url`: `{type: openIdConnect,
    /// openIdConnectUrl}`.
    pub fn openid(url: impl Into<String>) -> Self {
        Self::of(Kind::OpenIdConnect(url.into()))
    }

    /// The scheme with `description`, in CommonMark, which says what the credential is.
    #[must_use]
    pub fn with_description(mut self, description: impl Into<String>) -> Self {
        self.description = Some(description.into());
        self
    }

    /// Whether the credential is a cookie, which a browser sends with a request a page of another site makes.
    pub(crate) fn is_cookie(&self) -> bool {
        matches!(&self.kind, Kind::ApiKey { place, .. } if *place == "cookie")
    }

    fn api_key(place: &'static str, name: String) -> Self {
        Self::of(Kind::ApiKey { place, name })
    }

    fn of(kind: Kind) -> Self {
        Self {
            kind,
            description: None,
        }
    }
}

impl Serialize for SecurityScheme {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(None)?;

        match &self.kind {
            Kind::Http(scheme) => {
                map.serialize_entry("type", "http")?;
                map.serialize_entry("scheme", scheme)?;
            }
            Kind::ApiKey { place, name } => {
                map.serialize_entry("type", "apiKey")?;
                map.serialize_entry("in", place)?;
                map.serialize_entry("name", name)?;
            }
            Kind::OpenIdConnect(url) => {
                map.serialize_entry("type", "openIdConnect")?;
                map.serialize_entry("openIdConnectUrl", url)?;
            }
        }
        if let Some(description) = &self.description {
            map.serialize_entry("description", description)?;
        }

        map.end()
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    #[test]
    fn each_scheme_is_written_as_openapi_says() {
        let cases = [
            (
                SecurityScheme::bearer(),
                json!({ "type": "http", "scheme": "bearer" }),
            ),
            (
                SecurityScheme::basic(),
                json!({ "type": "http", "scheme": "basic" }),
            ),
            (
                SecurityScheme::http("digest"),
                json!({ "type": "http", "scheme": "digest" }),
            ),
            (
                SecurityScheme::header("X-Api-Key"),
                json!({ "type": "apiKey", "in": "header", "name": "X-Api-Key" }),
            ),
            (
                SecurityScheme::cookie("session"),
                json!({ "type": "apiKey", "in": "cookie", "name": "session" }),
            ),
            (
                SecurityScheme::query("token"),
                json!({ "type": "apiKey", "in": "query", "name": "token" }),
            ),
            (
                SecurityScheme::openid("https://id.example.com/.well-known/openid-configuration"),
                json!({
                    "type": "openIdConnect",
                    "openIdConnectUrl": "https://id.example.com/.well-known/openid-configuration",
                }),
            ),
        ];

        for (scheme, expected) in cases {
            assert_eq!(serde_json::to_value(&scheme).unwrap(), expected);
        }
    }

    #[test]
    fn a_description_is_written_beside_the_scheme() {
        let scheme = SecurityScheme::bearer().with_description("A session's token.");

        assert_eq!(
            serde_json::to_value(&scheme).unwrap(),
            json!({ "type": "http", "scheme": "bearer", "description": "A session's token." })
        );
    }

    #[test]
    fn only_a_cookie_is_a_cookie() {
        assert!(SecurityScheme::cookie("session").is_cookie());
        assert!(!SecurityScheme::header("cookie").is_cookie());
        assert!(!SecurityScheme::bearer().is_cookie());
    }

    #[test]
    fn the_title_and_the_version_are_replaced() {
        let document =
            Document::new(json!({ "info": { "title": "notes-api", "version": "0.1.0" } }))
                .with_title("Notes")
                .with_version("2.0.0");

        assert_eq!(
            document.to_json(),
            r#"{"info":{"title":"Notes","version":"2.0.0"}}"#
        );
    }
}

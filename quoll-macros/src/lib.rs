//! The proc macros of `quoll`, which re-exports them: users depend on `quoll`, never on this crate.

mod client;
mod descriptor;
mod docs;
mod expand;
mod known;
mod model;
mod openapi;
mod path;
mod scheme;
mod server;
mod syntax;

use proc_macro::TokenStream;

use crate::syntax::{Contract, GroupContract};

/// Reads a REST contract and checks it.
///
/// ```text
/// rest::api! {
///     /// Notes, kept per user.
///     pub api Api = "/api";
///
///     #[auth(Bearer)]
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> Note;
///     }
/// }
/// ```
///
/// An argument is a `{name}` of the path or a `key={name}` of the query by its name, a query struct as `query filter:
/// NoteFilter`, a header as `header("Last-Event-ID") last: Option<String>`, or else the body: JSON, or `Form<T>`,
/// `Text` or `Bytes`. A group or a route takes a credential with `#[auth(Bearer)]`, and a route takes none with
/// `#[auth(none)]`, which replaces its group's. A scheme is `Bearer`, `Basic`, one the contract declares, `pub scheme
/// Key = header "X-Api-Key";`, or a type implementing `rest::Scheme`; `#[auth(Bearer | Key)]` takes either,
/// `#[auth(Bearer, optional)]` a request with none, and `#[auth(Bearer, scopes("repo"))]` a caller with the scope.
///
/// The api may list groups that `rest::group!` blocks write in other modules, by their paths, after its prefix:
/// `pub api Api = "/api" { auth::Auth, notes::Notes };`, the groups of its own block coming after them.
///
/// A mistake in the contract is a compile error on the token at fault. The api becomes its type, with its comments;
/// with the feature `server` of `quoll`, a server trait per group, `ApiServices` and `Api::router` come too; with the
/// feature `client`, a trait per group, its client, `ApiClient`, `Api::client` and `Api::client_with`. Whatever the
/// features, `Api::openapi` gives the OpenAPI 3.1 document, for which each type the contract names implements
/// `schemars::JsonSchema`.
#[proc_macro]
pub fn api(input: TokenStream) -> TokenStream {
    match syn::parse::<Contract>(input) {
        Ok(Contract(api)) => expand::api(&api).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

/// Reads a group of a REST contract written in a module of its own, which an api lists, and checks it.
///
/// ```text
/// rest::group! {
///     /// The signed-in user's notes.
///     #[auth(Bearer)]
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> Note;
///     }
/// }
///
/// rest::api! {
///     pub api Api = "/api" { notes::Notes };
/// }
/// ```
///
/// The group is written as in `rest::api!`, and becomes the same items: its server trait, `NotesServer`, its client's
/// trait and its client, `Notes` and `NotesClient`, each behind its feature. A scheme it takes is the crate's, or a
/// type implementing `rest::Scheme` named by its path or imported, as a scheme an api declares: `#[auth(crate::Key)]`.
/// The group has an OpenAPI document, for which each type it names implements `schemars::JsonSchema`, unless it says
/// `#[openapi(none)]`, as the group of an api that has none.
///
/// The api that lists it, by its path, reads its routes through a hidden descriptor, `__QuollGroupNotes`, and serves
/// and calls them under its own prefix; it checks them, at compile time, against its own and its other groups'.
#[proc_macro]
pub fn group(input: TokenStream) -> TokenStream {
    match syn::parse::<GroupContract>(input) {
        Ok(GroupContract(alone)) => expand::alone_group(&alone).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

//! The proc macros of `quoll`, which re-exports them: users depend on `quoll`, never on this crate.

mod borrowed;
mod client;
mod descriptor;
mod docs;
mod expand;
mod fields;
mod headers;
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
/// quoll::api! {
///     /// Notes, kept per user.
///     pub api Api = "/api" { auth::Auth };
///
///     /// The key of an operator's scripts.
///     pub scheme Key = header "X-Api-Key";
///
///     /// The signed-in user's notes.
///     #[auth(Bearer)]
///     pub group Notes {
///         /// A note, by its id.
///         GET "/notes/{id}" => get(id: i64) -> Note;
///     }
/// }
/// ```
///
/// The block holds, in any order:
///
/// - the api, once: its visibility, its name, its prefix, `""` for none, and the groups of other modules it lists,
///   by their paths, which `quoll::group!` blocks write. `#[openapi(none)]` on it leaves out its OpenAPI document,
///   `#[server(none)]` its server side and its document, for an API that is only called, and `#[client(none)]` its
///   client side, for one that is only served: a side left out is not written whatever the features of `quoll`;
/// - the schemes it declares, `header`, `cookie`, `query` or `openid`, each with its visibility and its name;
/// - its groups, each a table of routes, with `#[auth(...)]` on the group or on a route.
///
/// `#[error(E)]` on the api, a group or a route names the shape of the errors another API answers, which the client
/// reads: the nearest to a route wins, and a route without one has RFC 9457 problems.
///
/// `#[header("name" = value)]` on the api, a group or a route fixes a header its requests send, the value a string or
/// a `&str` constant's name: the nearest to a route wins, below an argument of the same header, `#[header("name" =
/// none)]` leaves one of its parent's, and `#[header(none)]` all of them.
///
/// `#[body("name" = value)]` on a route fixes a field of its body, the value a bool, a number or a string, which the
/// client adds and by which the server tells two routes of one method and path apart.
///
/// A route is `METHOD "path?query" => function(arguments) -> Result;`, with no `->` for a route that returns nothing.
/// An argument is a `{name}` of the path or a `key={name}` of the query by its name, a query struct as `query filter:
/// NoteFilter`, a header as `header("Last-Event-ID") last: Option<String>`, or else the body. The docs of
/// the crate `quoll` show each form, with what it means.
///
/// A `&` on an argument's type, `owner: &str` or `note: &NewNote`, makes the client borrow it: the server and the
/// document receive the owned type, `String` for a `&str`, `Vec<T>` for a `&[T]` and `X` for any other `&X`.
///
/// A mistake in the contract is a compile error on the token at fault, which says what to write instead.
///
/// The contract becomes these items, in the module that writes it, each with the contract's `///` comments:
///
/// | Item                            | Feature of `quoll` | What it is                                                              |
/// | ------------------------------- | ------------------ | ----------------------------------------------------------------------- |
/// | `struct Api`                    | whatever           | the api's type                                                          |
/// | `Api::openapi()`                | whatever           | the OpenAPI 3.1 document, unless `#[openapi(none)]` or `#[server(none)]` |
/// | `struct Key`, ...               | whatever           | each scheme it declares                                                 |
/// | `trait NotesServer`             | `server`           | a handler per route, with the caller                                    |
/// | `struct ApiServices`            | `server`           | a field per group: its service                                          |
/// | `Api::router(services)`         | `server`           | the `axum::Router` of the api                                           |
/// | `trait Notes`                   | `client`           | a method per route, for the client and fakes                            |
/// | `struct NotesClient`            | `client`           | the client of the group                                                 |
/// | `struct ApiClient`              | `client`           | a method per group: `api.notes()`                                       |
/// | `Api::client(url)`              | `client`           | a client with no middleware                                             |
/// | `Api::client_with(http, url)`   | `client`           | a client on a `ClientWithMiddleware`                                    |
/// | `impl client::Contract for Api` | `client`           | what `client::builder().build::<Api>(url)` builds                       |
///
/// The items of the feature `server` are not written with `#[server(none)]`, nor those of `client` with
/// `#[client(none)]`: the feature writes hidden stand-ins under the names of the group's trait and of `Api::router`,
/// `Api::client` and `Api::client_with`, so that using one is an error that says the contract has no such side.
///
/// Each handler of `NotesServer` returns `impl Future<Output = Result<Response<T>>> + Send`, and an implementation
/// writes `async fn`, which returns `Ok(Response::new(note))`; each method of `Notes` returns `impl Call<T, E>`, `E`
/// the shape of the route's errors, `Problem` by default, and a fake writes an `async move` block. A hidden descriptor
/// of each group, `__QuollGroupNotes`, holds its routes, which the api that lists it reads.
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
/// // notes.rs
/// quoll::group! {
///     /// The signed-in user's notes.
///     #[auth(Bearer | crate::Key)]
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> Note;
///     }
/// }
///
/// // lib.rs
/// quoll::api! {
///     pub api Api = "/api" { notes::Notes };
///
///     pub scheme Key = header "X-Api-Key";
/// }
/// ```
///
/// The block holds one group, written as in `quoll::api!`, with no api and no scheme. It becomes the same items in its
/// module: its server trait, `NotesServer`, its client's trait and its client, `Notes` and `NotesClient`, each behind
/// its feature. A scheme the api declares is named by its path or an import, `#[auth(crate::Key)]`, as any type
/// implementing `quoll::Scheme`. The group has an OpenAPI document, for which each type it names implements
/// `schemars::JsonSchema`, unless it says `#[openapi(none)]` or `#[server(none)]`, as the group of an api that has
/// none. It has both sides unless it says `#[server(none)]` or `#[client(none)]`, and an api lists it only when it has
/// each side the api has: a side of the group's that the api does not have is left out of that api. Its client's trait
/// is written in its block, before an api lists it: the group says the shape of its errors itself, `#[error(E)]`, and an
/// api with an `#[error(...)]` lists only groups that say one. The group takes the fixed headers, `#[header(...)]`, of
/// the api that lists it, and any api may list it: a `none` of the group's leaves a header where that api fixes it,
/// and does nothing elsewhere.
///
/// The api lists the group by its path, `notes::Notes`. A group named alone, `{ Notes }`, is one in the api's module,
/// or brought by a glob import, `use notes::*;`. The api reads the group's routes through its hidden descriptor,
/// `__QuollGroupNotes`, serves and calls them under its own prefix, and checks them at compile time against its own
/// and its other groups'. Two apis may list one group.
#[proc_macro]
pub fn group(input: TokenStream) -> TokenStream {
    match syn::parse::<GroupContract>(input) {
        Ok(GroupContract(alone)) => expand::alone_group(&alone).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

//! A REST contract for Rust, written once in a macro: an axum server, a reqwest client and an OpenAPI 3.1 document
//! come from it.
//!
//! The contract is a route table, written with [`rest::api!`]. The server implements a trait per group, the client
//! calls a method per route, and `Api::openapi()` gives the document, which TypeScript generators such as orval read.
//! A mistake on either side, a route without its handler, a call with the wrong arguments, does not compile.
//!
//! An API that is only called, a third-party one, is a contract too: its api says `#[openapi(none)]`, and it is a
//! typed client alone, whose types need serde and no schema: see [OpenAPI](rest#openapi).
//!
//! # A contract
//!
//! ```
//! use quoll::rest::{self, Created};
//! use schemars::JsonSchema;
//! use serde::{Deserialize, Serialize};
//!
//! #[derive(Serialize, Deserialize, JsonSchema)]
//! pub struct Note {
//!     pub id: i64,
//!     pub title: String,
//! }
//!
//! #[derive(Serialize, Deserialize, JsonSchema)]
//! pub struct NewNote {
//!     pub title: String,
//! }
//!
//! rest::api! {
//!     /// Notes, kept per user.
//!     pub api Api = "/api";
//!
//!     /// The signed-in user's notes.
//!     #[auth(Bearer)]
//!     pub group Notes {
//!         GET    "/notes/{id}" => get(id: i64) -> Note;
//!         POST   "/notes"      => create(note: NewNote) -> Created<Note>;
//!         DELETE "/notes/{id}" => remove(id: i64);
//!     }
//! }
//!
//! let document = Api::openapi().to_json_pretty();
//!
//! assert!(document.contains(r#""operationId": "notes_get""#));
//! ```
//!
//! # Its server
//!
//! With the feature `server`, the group is a trait, `NotesServer`, whose handlers receive the caller that
//! [`Authenticate`](rest::Authenticate) makes of the request's credential. `Api::router` gives the `axum::Router`,
//! which [`server::builder()`] serves with its layers:
//!
#![cfg_attr(feature = "server", doc = "```no_run")]
#![cfg_attr(not(feature = "server"), doc = "```ignore")]
//! # use quoll::rest::{self, Created};
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct Note { pub id: i64, pub title: String }
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct NewNote { pub title: String }
//! # rest::api! {
//! #     pub api Api = "/api";
//! #     #[auth(Bearer)]
//! #     pub group Notes {
//! #         GET    "/notes/{id}" => get(id: i64) -> Note;
//! #         POST   "/notes"      => create(note: NewNote) -> Created<Note>;
//! #         DELETE "/notes/{id}" => remove(id: i64);
//! #     }
//! # }
//! use http::header::LOCATION;
//! use quoll::rest::{Authenticate, Bearer, Error, Response, Result};
//! use quoll::server;
//!
//! struct App;
//!
//! /// Who a request comes from, once its token is checked.
//! struct User {
//!     id: i64,
//! }
//!
//! impl Authenticate<Bearer> for App {
//!     type Caller = User;
//!
//!     async fn authenticate(&self, token: String) -> Result<User> {
//!         match token.as_str() {
//!             "mF_9.B5f-4.1JqM" => Ok(User { id: 1 }),
//!             _ => Err(Error::unauthorized("unknown or expired token")),
//!         }
//!     }
//! }
//!
//! impl NotesServer for App {
//!     async fn get(&self, user: &User, id: i64) -> Result<Response<Note>> {
//!         if id != user.id {
//!             return Err(Error::not_found("no such note"));
//!         }
//!
//!         Ok(Response::new(Note { id, title: "Milk".into() }))
//!     }
//!
//!     async fn create(&self, _user: &User, note: NewNote) -> Result<Response<Note>> {
//!         let note = Note { id: 2, title: note.title };
//!         let location = format!("/api/notes/{}", note.id);
//!
//!         Ok(Response::new(note).header(LOCATION, location))
//!     }
//!
//!     async fn remove(&self, _user: &User, _id: i64) -> Result<Response<()>> {
//!         Ok(Response::new(()))
//!     }
//! }
//!
//! #[tokio::main]
//! async fn main() -> std::io::Result<()> {
//!     server::builder()
//!         .with(server::request_id())
//!         .with(server::trace())
//!         .serve("0.0.0.0:8080", Api::router(ApiServices { notes: App }))
//!         .await
//! }
//! ```
//!
//! # Its client
//!
//! With the feature `client`, the group is a trait, `Notes`, which the client implements, and a test's fake too.
//! [`client::builder()`] builds the client with its middleware:
//!
#![cfg_attr(feature = "client", doc = "```no_run")]
#![cfg_attr(not(feature = "client"), doc = "```ignore")]
//! # use quoll::rest::{self, Created};
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct Note { pub id: i64, pub title: String }
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct NewNote { pub title: String }
//! # rest::api! {
//! #     pub api Api = "/api";
//! #     #[auth(Bearer)]
//! #     pub group Notes {
//! #         GET    "/notes/{id}" => get(id: i64) -> Note;
//! #         POST   "/notes"      => create(note: NewNote) -> Created<Note>;
//! #         DELETE "/notes/{id}" => remove(id: i64);
//! #     }
//! # }
//! use quoll::client;
//!
//! #[tokio::main]
//! async fn main() -> quoll::rest::Result<()> {
//!     let api = client::builder()
//!         .with(client::bearer("mF_9.B5f-4.1JqM"))
//!         .with(client::retry(3))
//!         .build::<Api>("http://127.0.0.1:8080")?;
//!
//!     let note = api.notes().get(1).await?;
//!
//!     println!("{}", note.title);
//!     Ok(())
//! }
//! ```
//!
//! # What the crate holds
//!
//! - [`rest`]: the contract, [`rest::api!`] and [`rest::group!`], and what it names: its results, its schemes, its
//!   error and its OpenAPI document.
//! - [`client`], with the feature `client`: the builder of a client and its middleware, on reqwest-middleware.
//! - [`server`], with the feature `server`: the builder of a server and its layers, on tower.
//! - [`test`], with the feature `server`: a router served on a free port, which a test calls with the real client.
//!
//! What the macro generates for a contract, its traits, its router, its client and its document, is listed in the
//! doc of [`rest::api!`].
//!
//! # Features
//!
//! Both are off by default. A crate that holds the contract alone needs neither: the document is written without
//! them, and the server and the command line turn on the one they need.
//!
//! - `server`: the code of the server, on axum and tokio, with [`server`] and [`test`].
//! - `client`: the code of the client, on reqwest and reqwest-middleware, with [`client`]. It turns on two features of
//!   reqwest: `rustls`, TLS through rustls with the platform's roots, so that a client calls an `https` URL as it is,
//!   and `system-proxy`, the proxy settings of Windows and macOS, beside the `HTTP_PROXY` variables reqwest always
//!   reads. `Api::client_with` keeps the reqwest client the application built, with its own settings.
//!
//! Built without a feature, these docs leave out its modules, and the links to them lead to this section.
//!
#![cfg_attr(feature = "client", doc = "[`client`]: client")]
#![cfg_attr(feature = "client", doc = "[`client::builder()`]: client::builder")]
#![cfg_attr(not(feature = "client"), doc = "[`client`]: crate#features")]
#![cfg_attr(not(feature = "client"), doc = "[`client::builder()`]: crate#features")]
#![cfg_attr(feature = "server", doc = "[`server`]: server")]
#![cfg_attr(feature = "server", doc = "[`server::builder()`]: server::builder")]
#![cfg_attr(feature = "server", doc = "[`test`]: mod@test")]
#![cfg_attr(not(feature = "server"), doc = "[`server`]: crate#features")]
#![cfg_attr(not(feature = "server"), doc = "[`server::builder()`]: crate#features")]
#![cfg_attr(not(feature = "server"), doc = "[`test`]: crate#features")]
#![cfg_attr(docsrs, feature(doc_cfg))]

#[cfg(feature = "client")]
pub mod client;
#[cfg(any(feature = "client", feature = "server"))]
mod order;
pub mod rest;
#[cfg(feature = "server")]
pub mod server;
#[cfg(feature = "server")]
pub mod test;

#[doc(hidden)]
pub mod __private;

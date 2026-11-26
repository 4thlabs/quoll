//! What a contract must not say: each file is one mistake, with its message beside it, in a directory that says which
//! features of `quoll` it is built with.
//!
//! - `tests/compile_fail/`, built with each of the four feature sets, so that its messages are the same whatever the
//!   features: the mistakes the macro finds itself, those of an [`ErrorKind`](quoll::ErrorKind), in the files
//!   `error_kind_*`: a match with no `_`, a status written as a number, the text of `as_str` kept past its kind, and
//!   an `Error::failed` with no kind or no shape; those of `#[server(...)]` and `#[client(...)]`, a contract with no
//!   side, a word other than `none`, one twice, one on a scheme, a group of the api's block or a route,
//!   an `#[openapi(...)]` beside `#[server(none)]` and an `#[error(...)]` in a contract with `#[client(none)]`; a
//!   route `-> TcpTunnel` in a contract with a server side, in the files `tcp_tunnel_*`, with the tunnel as a body, a
//!   parameter or the `T` of a `Created<T>`, or on `HEAD`; and a `quoll::group!` listed without a side its api has,
//!   its server, its client or its document, one error whatever the features, as the api reaches its groups by their
//!   descriptors alone; a scheme of an api that calls that does not implement `ClientScheme`, and an `impl Scheme`
//!   written as before the split, with a `read`.
//! - `tests/compile_fail_server/`, built with the feature `server`: what only the server's code checks, a route left
//!   out of a service, a handler of the wrong signature, a status of no content on a route of a value or one that is
//!   not a status, a body that is not `Deserialize`, a service that does not authenticate, and an argument the server
//!   cannot receive owned, `&mut X`, `&&X` or `&impl Trait`, which a client alone takes; a service of a listed group
//!   that lacks the group's trait, in `listed_service_without_its_trait`, and the router of an api that lists a group
//!   with `#[server(none)]` called, in `router_of_a_group_not_served`; and the server's trait of a contract with
//!   `#[server(none)]` implemented, rustc's own error, as the contract writes none.
//! - `tests/compile_fail_server_alone/`, built with the feature `server` and not `client`: what the server reports
//!   among the errors of rustc's own that the client's code would change, `&dyn Trait` and a `&X` whose `X` has no
//!   size, `&OsStr`.
//! - `tests/compile_fail_client/`, built with the feature `client`: what only the client's checks, a fake of the wrong
//!   signature, a result that is not `Deserialize`, `build` or `key` on the wrong type, a call not awaited, a borrowed
//!   argument given by value, and a call that outlives what it borrows; and the client of a contract with
//!   `#[client(none)]` built, in `client_none_has_no_client`, or its trait faked, in `client_none_has_no_fake`, each
//!   rustc's own error, as the contract writes none.
//! - `tests/compile_fail_neither/`, built with neither feature: what each side would repeat a message of its own for.
//!   A type the contract names `Created`, `Accepted`, `Form`, `Text`, `Bytes`, `Events`, `JsonLines`, `ByteStream` or
//!   `TcpTunnel` that is not quoll's; a scheme of an api or a `quoll::group!` that serves that does not implement
//!   `ServerScheme`, which the server's code would repeat; the mistakes of the OpenAPI document, a type with no schema and two schemes of one name, and a call to
//!   the document of a contract that has none; and the mistakes of a group of another block: a group listed and not in
//!   scope, or not a group, a group or a scheme less visible than what names it, and a scheme twice in an alternative.
//!
//! A message of rustc's own is kept as rustc writes it, the name it gives a hidden item included: the future of a
//! trait's method is `{anon_assoc#0}` in `handler_wrong_return`, `handler_not_async`, `fake_not_async`,
//! `fake_returns_created` and `fake_returns_accepted`. A rustc that names it otherwise changes those `.stderr`, which
//! are accepted again with `TRYBUILD=overwrite`.

#[test]
fn a_mistake_is_an_error_on_the_token_at_fault() {
    let cases = trybuild::TestCases::new();

    cases.compile_fail("tests/compile_fail/*.rs");
}

#[test]
fn a_valid_contract_compiles() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass/*.rs");
}

#[cfg(feature = "server")]
#[test]
fn a_service_implements_every_route_as_the_contract_says() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass_server/*.rs");
    cases.compile_fail("tests/compile_fail_server/*.rs");
}

#[cfg(all(feature = "server", not(feature = "client")))]
#[test]
fn an_argument_the_server_cannot_receive_owned_is_named_on_its_type() {
    let cases = trybuild::TestCases::new();

    cases.compile_fail("tests/compile_fail_server_alone/*.rs");
}

#[cfg(not(feature = "server"))]
#[test]
fn without_the_feature_server_no_server_code_is_written() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass_without_server/*.rs");
}

#[cfg(feature = "client")]
#[test]
fn a_fake_implements_every_route_as_the_client_does() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass_client/*.rs");
    cases.compile_fail("tests/compile_fail_client/*.rs");
}

#[cfg(not(feature = "client"))]
#[test]
fn without_the_feature_client_no_client_code_is_written() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass_without_client/*.rs");
}

#[cfg(all(feature = "server", feature = "client"))]
#[test]
fn with_both_features_both_sides_are_written_side_by_side() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass_both/*.rs");
}

#[cfg(not(any(feature = "server", feature = "client")))]
#[test]
fn with_neither_feature_only_the_api_type_is_written() {
    let cases = trybuild::TestCases::new();

    cases.pass("tests/pass_neither/*.rs");
}

#[cfg(not(any(feature = "server", feature = "client")))]
#[test]
fn with_neither_feature_the_mistakes_each_side_would_repeat_are_one_error() {
    let cases = trybuild::TestCases::new();

    cases.compile_fail("tests/compile_fail_neither/*.rs");
}

//! What a contract must not say: each file of `tests/compile_fail/` is one mistake, with its message beside it.
//!
//! What only the server's code checks, a route left out of a service or a handler of the wrong signature, is in
//! `tests/compile_fail_server/`, which builds with the feature `server`; what only the client's checks, a fake of the
//! wrong signature, in `tests/compile_fail_client/`, which builds with the feature `client`. A type the contract names
//! `Created`, `Form`, `Text` or `Bytes` that is not quoll's is in `tests/compile_fail_neither/`, built with neither
//! feature: the server's code and the client's each repeat its message.

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
fn a_type_named_as_one_of_quolls_is_quolls_with_neither_feature() {
    let cases = trybuild::TestCases::new();

    cases.compile_fail("tests/compile_fail_neither/*.rs");
}

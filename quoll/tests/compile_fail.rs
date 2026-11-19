//! What a contract must not say: each file of `tests/compile_fail/` is one mistake, with its message beside it.
//!
//! What only the server's code checks, a route left out of a service or a handler of the wrong signature, is in
//! `tests/compile_fail_server/`, which builds with the feature `server`.

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

//! What a contract must not say: each file of `tests/compile_fail/` is one mistake, with its message beside it.

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

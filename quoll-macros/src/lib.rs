//! The proc macros of `quoll`, which re-exports them: users depend on `quoll`, never on this crate.

// The model is read in full by the generators of the server, the client and OpenAPI, which come in later changes; today
// the tests read it.
#[cfg_attr(
    not(test),
    expect(dead_code, reason = "its generators are not written yet")
)]
mod model;
mod path;
mod syntax;

use proc_macro::TokenStream;
use quote::quote;

use crate::model::Api;
use crate::syntax::Contract;

/// Reads a REST contract and checks it.
///
/// ```text
/// rest::api! {
///     /// Notes, kept per user.
///     pub api Api = "/api";
///
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> Note;
///     }
/// }
/// ```
///
/// A mistake in the contract is a compile error on the token at fault. The code of the server, the client and the
/// OpenAPI document comes with the features that need it; for now the api becomes its type, with its comments.
#[proc_macro]
pub fn api(input: TokenStream) -> TokenStream {
    match syn::parse::<Contract>(input) {
        Ok(Contract(api)) => expand(&api).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

/// The api's type, with its comments.
fn expand(api: &Api) -> proc_macro2::TokenStream {
    let Api {
        docs, vis, name, ..
    } = api;
    let docs = docs.0.iter().map(|line| quote! { #[doc = #line] });

    quote! {
        #(#docs)*
        #[derive(
            ::core::fmt::Debug,
            ::core::clone::Clone,
            ::core::marker::Copy,
            ::core::default::Default,
        )]
        #vis struct #name;
    }
}

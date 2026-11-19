//! The proc macros of `quoll`, which re-exports them: users depend on `quoll`, never on this crate.

mod model;
mod path;
mod server;
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
/// A mistake in the contract is a compile error on the token at fault. The api becomes its type, with its comments;
/// with the feature `server` of `quoll`, a trait per group, `ApiServices` and `Api::router` come too. The client and
/// the OpenAPI document come in later changes.
#[proc_macro]
pub fn api(input: TokenStream) -> TokenStream {
    match syn::parse::<Contract>(input) {
        Ok(Contract(api)) => expand(&api).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

/// What a contract becomes: the api's type, and the server's code behind the feature `server` of `quoll`.
fn expand(api: &Api) -> proc_macro2::TokenStream {
    let api_type = api_type(api);
    let server = server::expand(api);

    quote! {
        #api_type
        #server
    }
}

/// `#[doc = ...]` attributes, one per line.
pub(crate) fn doc_attrs(lines: &[String]) -> proc_macro2::TokenStream {
    quote! { #(#[doc = #lines])* }
}

/// The api's type, with its comments.
fn api_type(api: &Api) -> proc_macro2::TokenStream {
    let Api {
        docs, vis, name, ..
    } = api;
    // A crate that denies `missing_docs` must not fail on what the macro writes.
    let fallback = vec![format!(" The REST contract `{name}`, from `rest::api!`.")];
    let lines = if docs.0.is_empty() {
        &fallback
    } else {
        &docs.0
    };
    let docs = doc_attrs(lines);

    quote! {
        #docs
        #[derive(
            ::core::fmt::Debug,
            ::core::clone::Clone,
            ::core::marker::Copy,
            ::core::default::Default,
        )]
        #vis struct #name;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn expansion(contract: proc_macro2::TokenStream) -> String {
        let Contract(api) = syn::parse2(contract).expect("the contract is valid");

        expand(&api).to_string()
    }

    #[test]
    fn the_api_type_has_the_contracts_docs() {
        let tokens = expansion(quote! {
            #[doc = " Notes."]
            pub api Api = "/api";
        });

        assert!(tokens.contains("\" Notes.\""), "{tokens}");
        assert!(!tokens.contains("from `rest::api!`"), "{tokens}");
    }

    #[test]
    fn the_api_type_is_documented_even_when_the_contract_is_not() {
        let tokens = expansion(quote! { pub api Api = "/api"; });

        assert!(
            tokens.contains("The REST contract `Api`, from `rest::api!`."),
            "{tokens}"
        );
    }

    #[test]
    fn the_server_goes_through_the_gate_of_its_feature() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> String;
            }
        });
        let gate = tokens
            .find(":: quoll :: __private :: server !")
            .expect("the server's code is gated");

        for item in ["trait NotesServer", "struct ApiServices", "fn router"] {
            let at = tokens.find(item).expect("the item is written");
            assert!(at > gate, "{item} is written outside the gate: {tokens}");
        }
    }
}

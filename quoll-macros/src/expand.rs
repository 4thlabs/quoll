//! What a contract becomes, put together from the modules of each side: the api's type, its schemes, its groups, their
//! checks, the OpenAPI document, the server's code and the client's.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::Ident;
use syn::spanned::Spanned;

use crate::descriptor::{self, Cx};
use crate::docs::doc_attrs;
use crate::known::Known;
use crate::model::Api;
use crate::{client, openapi, scheme, server};

/// What a contract becomes: the api's type, its schemes, each group its block writes, its OpenAPI document, the checks
/// of the groups it lists, the server's code behind the feature `server` of `quoll`, and the client's behind its
/// feature `client`.
pub(crate) fn api(api: &Api) -> TokenStream {
    let api_type = api_type(api);
    let schemes = scheme::expand(api);
    let groups = api.groups.iter().map(|written| {
        group(&Cx {
            group: written,
            api: Some(api),
            openapi: api.openapi,
        })
    });
    let checks = descriptor::api_checks(api);
    let error = api.error.as_ref().map(named);
    let openapi = openapi::expand(api);
    let server = server::expand(api);
    let client = client::expand(api);

    quote! {
        #api_type
        #schemes
        #(#groups)*
        #checks
        #error
        #openapi
        #server
        #client
    }
}

/// What a group of `rest::group!` becomes, alone in its block: the items of [`group`], with no api above it.
pub(crate) fn alone_group(alone: &Api) -> TokenStream {
    group(&Cx {
        group: &alone.groups[0],
        api: None,
        openapi: alone.openapi,
    })
}

/// What a group becomes: its descriptor, the check of the types it names and of the challenges of its schemes, its
/// OpenAPI document, its server's code and its client's.
fn group(cx: &Cx) -> TokenStream {
    let descriptor = descriptor::expand(cx);
    let known = known_types(cx);
    let errors = cx
        .group
        .error
        .iter()
        .chain(
            cx.group
                .routes
                .iter()
                .filter_map(|route| route.error.as_ref()),
        )
        .map(named);
    let alone = if cx.api.is_none() {
        descriptor::alone_checks(cx)
    } else {
        TokenStream::new()
    };
    let challenges = scheme::challenge_checks(cx.group);
    let openapi = openapi::group(cx);
    let server = server::group(cx);
    let client = client::group(cx);

    quote! {
        #descriptor
        #known
        #(#errors)*
        #alone
        #challenges
        #openapi
        #server
        #client
    }
}

/// Checks that each type of quoll's a group names, which the macro recognizes by its name, is the crate's. The types
/// are the contract's tokens, so that the user's import names them; and the check is written whatever the features
/// of `quoll`, so that the import is used, and the type checked, with neither of them. The schemes a group takes are
/// checked by its descriptor, which names each one.
fn known_types(cx: &Cx) -> TokenStream {
    let mut checks = Vec::new();

    for route in &cx.group.routes {
        let result = route
            .reply
            .as_deref()
            .and_then(|returned| returned.known.map(|known| (&returned.served, known)));
        let body = route
            .body()
            .and_then(|(param, _)| Known::of(&param.ty).map(|known| (&param.ty, known)));

        for (ty, known) in result.into_iter().chain(body) {
            let check = Ident::new(known.check, Span::call_site());

            checks.push(quote_spanned! {ty.span()=> ::quoll::__private::known::#check::<#ty>(); });
        }
    }

    if checks.is_empty() {
        return TokenStream::new();
    }

    quote! {
        const _: () = {
            #(#checks)*
        };
    }
}

/// Names the type of an `#[error(E)]` whatever the features of `quoll`, so that it is checked, and its import used,
/// with neither of them: only the client reads it, and checks that it is the shape of an api's errors.
fn named(error: &syn::Type) -> TokenStream {
    quote_spanned! {error.span()=>
        const _: ::core::marker::PhantomData<#error> = ::core::marker::PhantomData;
    }
}

/// The api's type, with its comments.
fn api_type(api: &Api) -> TokenStream {
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
    use crate::syntax::Contract;

    fn expansion(contract: proc_macro2::TokenStream) -> String {
        let Contract(parsed) = syn::parse2(contract).expect("the contract is valid");

        api(&parsed).to_string()
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

    #[test]
    fn the_client_goes_through_the_gate_of_its_feature() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> String;
            }
        });
        let gate = tokens
            .find(":: quoll :: __private :: client !")
            .expect("the client's code is gated");

        for item in [
            "trait Notes :",
            "struct NotesClient",
            "struct ApiClient",
            "fn client (",
            "fn client_with (",
        ] {
            let at = tokens.find(item).expect("the item is written");
            assert!(at > gate, "{item} is written outside the gate: {tokens}");
        }
    }

    #[test]
    fn each_route_carries_its_operation_id_and_its_path_as_written() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            pub group AdminTools {
                GET "/admin/{service}?q={query}" => r#match(service: String, query: Option<String>) -> String;
            }
        });

        // Once, in the group's descriptor, below the prefix, which the api's tables put it under.
        let route = r#"id : "admin_tools_match" , function : "match" , method : :: quoll :: __private :: group :: Verb :: Get , path : "/admin/{service}""#;
        let table = r#"table ! (__QUOLL_ROUTES , "/api" , __QuollGroupAdminTools)"#;

        assert_eq!(tokens.matches(route).count(), 1, "{tokens}");
        assert_eq!(tokens.matches(table).count(), 2, "{tokens}");
    }

    #[test]
    fn a_route_carries_the_schemes_it_takes() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            #[auth(Bearer)]
            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> String;
                #[auth(none)]
                GET "/public" => public() -> String;
            }
        });

        assert!(
            tokens.contains(r#"path : "/notes/{id}" , headers : & [] , query : & [] , schemes : & [< :: quoll :: rest :: Bearer as :: quoll :: rest :: Scheme > :: NAME] , scheme_types : & [:: core :: any :: TypeId :: of :: < :: quoll :: rest :: Bearer > ()]"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(r#"path : "/public" , headers : & [] , query : & [] , schemes : & [] , scheme_types : & []"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(". route :: < :: quoll :: rest :: Bearer , _ , _ >"),
            "{tokens}"
        );
        assert!(tokens.contains(". route :: < () , _ , _ >"), "{tokens}");
        assert!(
            tokens.contains("trait NotesServer : :: core :: marker :: Send + :: core :: marker :: Sync + 'static + :: quoll :: rest :: Authenticate < :: quoll :: rest :: Bearer >"),
            "{tokens}"
        );
    }
}

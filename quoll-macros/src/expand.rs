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
use crate::{client, headers, openapi, scheme, server};

/// What a contract becomes: the api's type, its schemes, each group its block writes, its OpenAPI document, the checks
/// of the groups it lists, the server's code behind the feature `server` of `quoll`, and the client's behind its
/// feature `client`.
pub(crate) fn api(api: &Api) -> TokenStream {
    let api_type = api_type(api);
    let schemes = scheme::expand(api);
    let groups = api
        .groups
        .iter()
        .map(|written| group(&Cx::new(written, api, false)));
    let checks = descriptor::api_checks(api);
    let error = api.error.as_ref().map(named);
    let fixed = headers::api_checks(&api.headers);
    let openapi = openapi::expand(api);
    let server = api.server.then(|| server::expand(api));
    let client = api.client.then(|| client::expand(api));

    quote! {
        #api_type
        #schemes
        #(#groups)*
        #checks
        #error
        #fixed
        #openapi
        #server
        #client
    }
}

/// What a group of `quoll::group!` becomes, alone in its block: the items of [`group`], with no api above it.
pub(crate) fn alone_group(alone: &Api) -> TokenStream {
    group(&Cx::new(&alone.groups[0], alone, true))
}

/// What a group becomes: its descriptor, the check of the types it names and of the challenges of its schemes, its
/// OpenAPI document, its server's code and its client's, each side when the group has it.
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
    // Only the server answers a scheme's challenge, on a 401.
    let challenges = cx.server.then(|| scheme::challenge_checks(cx.group));
    let sides = scheme::side_checks(cx.group, cx.server, cx.client);
    let fixed = headers::group_checks(cx);
    let openapi = openapi::group(cx);
    let named_types = named_types(cx);
    let server = cx.server.then(|| server::group(cx));
    let client = cx.client.then(|| client::group(cx));

    quote! {
        #descriptor
        #known
        #named_types
        #(#errors)*
        #alone
        #challenges
        #sides
        #fixed
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

/// Names each type of the group's arguments and results, as the contract writes them, in a function nothing calls,
/// when the group has no document, which names them otherwise: a contract crate built with neither feature of `quoll`,
/// or with the feature of a side it does not have, still uses the import of each type it names. A parameter of a
/// function takes any type an argument does, `&impl Trait` and `&dyn Trait` among them, with no bound on it.
fn named_types(cx: &Cx) -> TokenStream {
    if cx.openapi {
        return TokenStream::new();
    }

    let types: Vec<&syn::Type> = cx
        .group
        .routes
        .iter()
        .flat_map(|route| {
            let params = route.params.iter().map(|param| &param.written);
            let result = route.reply.as_deref().map(|returned| &returned.served);

            params.chain(result)
        })
        .collect();

    if types.is_empty() {
        return TokenStream::new();
    }

    // The function is the macro's, its type alone the contract's: a lint on a signature, clippy's `ptr_arg` on a
    // `&String` or `box_collection` on a `Box<String>`, is not the user's to fix in a function that nothing calls.
    let names = types.iter().enumerate().map(|(index, ty)| {
        let name = Ident::new(&format!("__quoll_names_{index}"), Span::mixed_site());

        quote! { fn #name(_: #ty) {} }
    });

    quote! {
        #[allow(dead_code, clippy::all, clippy::pedantic, clippy::nursery)]
        const _: () = {
            #(#names)*
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
    let fallback = vec![format!(" The REST contract `{name}`, from `quoll::api!`.")];
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
        assert!(!tokens.contains("from `quoll::api!`"), "{tokens}");
    }

    #[test]
    fn the_api_type_is_documented_even_when_the_contract_is_not() {
        let tokens = expansion(quote! { pub api Api = "/api"; });

        assert!(
            tokens.contains("The REST contract `Api`, from `quoll::api!`."),
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
    fn server_none_writes_no_server_side_and_no_document() {
        let tokens = expansion(quote! {
            #[server(none)]
            pub api Api = "/api";

            pub scheme Key = header "X-Api-Key";

            #[auth(Key)]
            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> String;
            }
        });

        for item in [
            ":: quoll :: __private :: server !",
            "NotesServer",
            "ApiServices",
            "fn router",
            "fn openapi () -> :: quoll :: openapi :: Document",
            "schema ::",
            "CHALLENGE",
            "Served for",
        ] {
            assert!(!tokens.contains(item), "{item} is written: {tokens}");
        }
        // An empty document, which no api that builds reads: an api with a document refuses the group by `Served`.
        assert!(
            tokens.contains(r#"GroupDocument for __QuollGroupNotes { const GROUP : :: quoll :: __private :: openapi :: Group = :: quoll :: __private :: openapi :: Group { name : "Notes" , docs : & [] , routes : & [] , } ; }"#),
            "{tokens}"
        );
        assert!(tokens.contains("struct NotesClient {"), "{tokens}");
        assert!(tokens.contains("Called for __QuollGroupNotes"), "{tokens}");
    }

    #[test]
    fn client_none_writes_no_client_side() {
        let tokens = expansion(quote! {
            #[client(none)]
            pub api Api = "/api";

            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> String;
            }
        });

        for item in [
            ":: quoll :: __private :: client !",
            "trait Notes :",
            "NotesClient",
            "ApiClient",
            "fn client",
            "Called for",
        ] {
            assert!(!tokens.contains(item), "{item} is written: {tokens}");
        }
        assert!(tokens.contains("trait NotesServer : :: core"), "{tokens}");
        assert!(
            tokens.contains("fn openapi () -> :: quoll :: openapi :: Document"),
            "{tokens}"
        );
        assert!(tokens.contains("Served for __QuollGroupNotes"), "{tokens}");
    }

    #[test]
    fn an_api_checks_only_the_sides_it_has_of_the_groups_it_lists() {
        let called = expansion(quote! {
            #[server(none)]
            pub api Api = "/api" { notes::Notes };
        });
        let served = expansion(quote! {
            #[client(none)]
            #[openapi(none)]
            pub api Api = "/api" { notes::Notes };
        });
        let documented = expansion(quote! {
            pub api Api = "/api" { notes::Notes };
        });

        assert!(!called.contains("is_served"), "{called}");
        assert!(
            called.contains("fn called () where notes :: __QuollGroupNotes"),
            "{called}"
        );
        assert!(
            served.contains("is_served :: < notes :: __QuollGroupNotes >"),
            "{served}"
        );
        assert!(!served.contains("fn called"), "{served}");
        // The server side is checked beside the document, so that its message comes first.
        assert!(documented.contains("is_served"), "{documented}");
        assert!(documented.contains("GroupDocument"), "{documented}");
        assert!(documented.contains("fn called"), "{documented}");
    }

    #[test]
    fn an_api_reaches_the_groups_it_lists_by_their_descriptors_alone() {
        let tokens = expansion(quote! {
            pub api Api = "/api" { notes::Notes };
        });

        // A group with no side writes no item under the side's names, which the api therefore never names.
        assert!(!tokens.contains("notes :: NotesServer"), "{tokens}");
        assert!(!tokens.contains("notes :: NotesClient"), "{tokens}");
        assert!(
            tokens.contains("notes :: __QuollGroupNotes : :: quoll :: __private :: server :: Serve < __QuollServiceNotes >"),
            "{tokens}"
        );
        assert!(
            tokens.contains("-> < notes :: __QuollGroupNotes as :: quoll :: __private :: group :: Called > :: Client where notes :: __QuollGroupNotes : :: quoll :: __private :: group :: Called"),
            "{tokens}"
        );
        // With the feature `client`, that bound checks the group's client side; without it, a check of its own.
        assert!(
            tokens.contains(":: quoll :: __private :: without_client ! { const _ : () = { # [allow (dead_code)] fn called () where notes :: __QuollGroupNotes : :: quoll :: __private :: group :: Called { } } ; }"),
            "{tokens}"
        );
    }

    #[test]
    fn a_group_with_no_document_names_its_types_whatever_the_features() {
        let tokens = expansion(quote! {
            #[server(none)]
            pub api Api = "/api";

            pub group Notes {
                GET "/notes/{id}?at={at}" => get(id: &str, at: IpAddr) -> BTreeMap<String, Note>;
            }
        });
        let documented = expansion(quote! {
            pub api Api = "/api";

            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> Note;
            }
        });

        let names = tokens
            .find("fn __quoll_names_0")
            .expect("the types are named");
        let gate = tokens
            .find(":: quoll :: __private :: client ! { # [doc = \" The client of the group")
            .expect("the client is written");
        assert!(names < gate, "{tokens}");
        assert!(
            tokens.contains("clippy :: nursery)] const _ : () = { fn __quoll_names_0"),
            "{tokens}"
        );
        for ty in [
            "(_ : & str)",
            "(_ : IpAddr)",
            "(_ : BTreeMap < String , Note >)",
        ] {
            assert!(tokens.contains(ty), "{ty} is not named: {tokens}");
        }
        assert!(!documented.contains("__quoll_names"), "{documented}");
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
            tokens.contains(r#"path : "/notes/{id}" , headers : & [] , query : & [] , schemes : & [< :: quoll :: Bearer as :: quoll :: Scheme > :: NAME] , scheme_types : & [:: core :: any :: TypeId :: of :: < :: quoll :: Bearer > ()]"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(r#"path : "/public" , headers : & [] , query : & [] , schemes : & [] , scheme_types : & []"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(". route :: < :: quoll :: Bearer , _ , _ >"),
            "{tokens}"
        );
        assert!(tokens.contains(". route :: < () , _ , _ >"), "{tokens}");
        assert!(
            tokens.contains("trait NotesServer : :: core :: marker :: Send + :: core :: marker :: Sync + 'static + :: quoll :: Authenticate < :: quoll :: Bearer >"),
            "{tokens}"
        );
    }
}

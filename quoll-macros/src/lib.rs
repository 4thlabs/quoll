//! The proc macros of `quoll`, which re-exports them: users depend on `quoll`, never on this crate.

mod client;
mod known;
mod model;
mod openapi;
mod path;
mod scheme;
mod server;
mod syntax;

use proc_macro::TokenStream;
use proc_macro2::Span;
use quote::{quote, quote_spanned};
use syn::Ident;
use syn::ext::IdentExt;
use syn::spanned::Spanned;

use crate::known::Known;
use crate::model::{Api, Group, Route};
use crate::syntax::Contract;

/// Reads a REST contract and checks it.
///
/// ```text
/// rest::api! {
///     /// Notes, kept per user.
///     pub api Api = "/api";
///
///     #[auth(Bearer)]
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> Note;
///     }
/// }
/// ```
///
/// An argument is a `{name}` of the path or a `key={name}` of the query by its name, a query struct as `query filter:
/// NoteFilter`, a header as `header("Last-Event-ID") last: Option<String>`, or else the body: JSON, or `Form<T>`,
/// `Text` or `Bytes`. A group or a route takes a credential with `#[auth(Bearer)]`, and a route takes none with
/// `#[auth(none)]`, which replaces its group's. A scheme is `Bearer`, `Basic`, one the contract declares, `pub scheme
/// Key = header "X-Api-Key";`, or a type implementing `rest::Scheme`; `#[auth(Bearer | Key)]` takes either,
/// `#[auth(Bearer, optional)]` a request with none, and `#[auth(Bearer, scopes("repo"))]` a caller with the scope.
///
/// A mistake in the contract is a compile error on the token at fault. The api becomes its type, with its comments;
/// with the feature `server` of `quoll`, a server trait per group, `ApiServices` and `Api::router` come too; with the
/// feature `client`, a trait per group, its client, `ApiClient`, `Api::client` and `Api::client_with`. Whatever the
/// features, `Api::openapi` gives the OpenAPI 3.1 document, for which each type the contract names implements
/// `schemars::JsonSchema`.
#[proc_macro]
pub fn api(input: TokenStream) -> TokenStream {
    match syn::parse::<Contract>(input) {
        Ok(Contract(api)) => expand(&api).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

/// What a contract becomes: the api's type, the check of the types it names, its OpenAPI document, the server's code
/// behind the feature `server` of `quoll`, and the client's behind its feature `client`.
fn expand(api: &Api) -> proc_macro2::TokenStream {
    let api_type = api_type(api);
    let schemes = scheme::expand(api);
    let known = known_types(api);
    let openapi = openapi::expand(api);
    let server = server::expand(api);
    let client = client::expand(api);

    quote! {
        #api_type
        #schemes
        #known
        #openapi
        #server
        #client
    }
}

/// Checks that each type of quoll's the contract names, which the macro recognizes by its name, is the crate's, and,
/// for a contract with no OpenAPI document, whose `Api::openapi` checks it, that each scheme of the application's it
/// names is a scheme. The types are the contract's tokens, so that the user's import names them; and the check is
/// written whatever the features of `quoll`, so that the import is used, and the type checked, with neither of them.
fn known_types(api: &Api) -> proc_macro2::TokenStream {
    // With a document, `Api::openapi` names each scheme once, which checks it.
    let mut checks = if api.openapi {
        Vec::new()
    } else {
        scheme::checks(api)
    };

    for route in api.groups.iter().flat_map(|group| &group.routes) {
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
        return proc_macro2::TokenStream::new();
    }

    quote! {
        const _: () = {
            #(#checks)*
        };
    }
}

/// `#[doc = ...]` attributes, one per line.
pub(crate) fn doc_attrs(lines: &[String]) -> proc_macro2::TokenStream {
    quote! { #(#[doc = #lines])* }
}

/// The comments of a route's method, on the server's trait and the client's: the contract's, then the method and the
/// path, `GET /api/notes/{id}`, and the scheme it takes.
pub(crate) fn route_docs(api: &Api, route: &Route) -> proc_macro2::TokenStream {
    let mut line = format!(" `{} {}`", route.method.as_str(), shown_path(api, route));
    if let Some(auth) = &route.auth {
        let words: Vec<String> = auth
            .schemes
            .iter()
            .map(|scheme| format!("`{}`", scheme.word()))
            .collect();

        line.push_str(&format!(", authenticated with {}", words.join(" or ")));
        if auth.optional {
            line.push_str(", or by no credential");
        }
        if !auth.scopes.is_empty() {
            let scopes: Vec<String> = auth
                .scopes
                .iter()
                .map(|scope| format!("`{scope}`"))
                .collect();
            let noun = if scopes.len() == 1 { "scope" } else { "scopes" };

            line.push_str(&format!(
                ", for a caller with the {noun} {}",
                scopes.join(" and ")
            ));
        }
    }

    let mut lines = route.docs.0.clone();
    if !lines.is_empty() {
        lines.push(String::new());
    }

    lines.push(line);

    doc_attrs(&lines)
}

/// `static NAME: rest::Route = ...;`, the route a request calls: the client puts it in the extensions of each request
/// it sends, and the server in those of each answer, where middleware and layers read it.
pub(crate) fn route_static_item(
    api: &Api,
    group: &Group,
    route: &Route,
    name: &Ident,
) -> proc_macro2::TokenStream {
    let operation_id = route.operation_id(group);
    let group_name = group.name.unraw().to_string();
    let method = Ident::new(route.method.as_str(), Span::call_site());
    let path = route.full_path(&api.prefix);
    // The name and the type of each scheme come from the scheme itself, which writes its name once.
    let schemes: Vec<_> = route.schemes().map(scheme::scheme_type).collect();
    let names = schemes
        .iter()
        .map(|scheme| quote! { <#scheme as ::quoll::rest::Scheme>::NAME });
    let types = schemes
        .iter()
        .map(|scheme| quote! { ::core::any::TypeId::of::<#scheme>() });
    let streams = route
        .reply
        .as_deref()
        .and_then(|returned| returned.known)
        .is_some_and(|known| known.stream);

    quote! {
        static #name: ::quoll::rest::Route = ::quoll::__private::route(
            #operation_id,
            #group_name,
            ::quoll::__private::http::Method::#method,
            #path,
            &[#(#names),*],
            &[#(#types),*],
            #streams,
        );
    }
}

/// The path with its query, as the docs show it: `/api/notes?page={page}`.
fn shown_path(api: &Api, route: &Route) -> String {
    let mut path = route.full_path(&api.prefix);
    let query = route
        .query_params()
        .map(|(param, key)| format!("{key}={{{}}}", param.name.unraw()));

    for (index, pair) in query.enumerate() {
        path.push(if index == 0 { '?' } else { '&' });
        path.push_str(&pair);
    }

    path
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

        // Once for the server, once for the client.
        let route = r#""admin_tools_match" , "AdminTools" , :: quoll :: __private :: http :: Method :: GET , "/api/admin/{service}""#;

        assert_eq!(tokens.matches(route).count(), 2, "{tokens}");
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
            tokens.contains(r#""/api/notes/{id}" , & [< :: quoll :: rest :: Bearer as :: quoll :: rest :: Scheme > :: NAME] , & [:: core :: any :: TypeId :: of :: < :: quoll :: rest :: Bearer > ()]"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(r#""/api/public" , & [] , & []"#),
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

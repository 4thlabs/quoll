//! The proc macros of `quoll`, which re-exports them: users depend on `quoll`, never on this crate.

mod client;
mod model;
mod path;
mod server;
mod syntax;

use proc_macro::TokenStream;
use quote::{quote, quote_spanned};
use syn::ext::IdentExt;
use syn::spanned::Spanned;

use crate::model::{Api, BodyKind, Reply, Route, Scheme};
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
/// `#[auth(none)]`, which replaces its group's.
///
/// A mistake in the contract is a compile error on the token at fault. The api becomes its type, with its comments;
/// with the feature `server` of `quoll`, a server trait per group, `ApiServices` and `Api::router` come too; with the
/// feature `client`, a trait per group, its client, `ApiClient`, `Api::client` and `Api::client_with`. The OpenAPI
/// document comes in a later change.
#[proc_macro]
pub fn api(input: TokenStream) -> TokenStream {
    match syn::parse::<Contract>(input) {
        Ok(Contract(api)) => expand(&api).into(),
        Err(error) => error.into_compile_error().into(),
    }
}

/// What a contract becomes: the api's type, the check of the types it names by their name, the server's code behind
/// the feature `server` of `quoll`, and the client's behind its feature `client`.
fn expand(api: &Api) -> proc_macro2::TokenStream {
    let api_type = api_type(api);
    let known = known_types(api);
    let server = server::expand(api);
    let client = client::expand(api);

    quote! {
        #api_type
        #known
        #server
        #client
    }
}

/// Checks that each type the contract names `Created`, `Accepted`, `Form`, `Text`, `Bytes`, `Events` or `JsonLines`,
/// which the macro reads by its name, is the crate's. The types are the contract's tokens, so that the user's import
/// names them; and the check is written whatever the features of `quoll`, so that the import is used, and the type
/// checked, with neither of them.
fn known_types(api: &Api) -> proc_macro2::TokenStream {
    let mut checks = Vec::new();

    for route in api.groups.iter().flat_map(|group| &group.routes) {
        let result = match &route.reply {
            Reply::None | Reply::Json(_) => None,
            Reply::Created { outer, .. } => Some((outer, quote! { created })),
            Reply::Accepted { outer, .. } => Some((outer, quote! { accepted })),
            Reply::Text(ty) => Some((ty, quote! { text })),
            Reply::Bytes(ty) => Some((ty, quote! { bytes })),
            Reply::Events(ty) => Some((ty, quote! { events })),
            Reply::JsonLines(ty) => Some((ty, quote! { json_lines })),
        };
        if let Some((ty, check)) = result {
            checks.push(quote_spanned! {ty.span()=> ::quoll::__private::known::#check::<#ty>(); });
        }

        let Some((param, kind)) = route.body() else {
            continue;
        };
        let ty = &param.ty;
        let check = match kind {
            BodyKind::Json => continue,
            BodyKind::Form => quote! { form },
            BodyKind::Text => quote! { text },
            BodyKind::Bytes => quote! { bytes },
        };

        checks.push(quote_spanned! {ty.span()=> ::quoll::__private::known::#check::<#ty>(); });
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

/// The type of a scheme the crate gives, by which the generated code names it.
pub(crate) fn scheme_type(scheme: Scheme) -> proc_macro2::TokenStream {
    match scheme {
        Scheme::Bearer => quote! { ::quoll::rest::Bearer },
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
    if let Some(scheme) = route.auth {
        line.push_str(&format!(", authenticated with `{}`", scheme.word()));
    }

    let mut lines = route.docs.0.clone();
    if !lines.is_empty() {
        lines.push(String::new());
    }

    lines.push(line);

    doc_attrs(&lines)
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

        assert!(
            tokens.contains(r#""admin_tools_match" , "AdminTools" , :: quoll :: __private :: client :: Method :: GET , "/api/admin/{service}""#),
            "{tokens}"
        );
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
            tokens.contains(r#""/api/notes/{id}" , & [< :: quoll :: rest :: Bearer as :: quoll :: __private :: Given > :: NAME]"#),
            "{tokens}"
        );
        assert!(tokens.contains(r#""/api/public" , & []"#), "{tokens}");
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

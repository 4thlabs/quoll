//! What the OpenAPI document of a contract becomes, on both sides and whatever the features: `Api::openapi`, which
//! hands a description of the api to `quoll::__private::openapi::document`, with the checks that the schemes of the
//! api have names of their own, which can name them in the document.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::spanned::Spanned;
use syn::{LitStr, Type, ext::IdentExt};

use crate::model::{
    Api, DeclaredKind, Group, Param, ParamKind, Route, SchemeKind, is_vec, option_inner,
};
use crate::scheme::scheme_type;

/// `Api::openapi`, unless the contract says `#[openapi(none)]`. Its schemes are listed once, each type named once, so
/// that a type that is not a scheme is one error; the checks of their names sit beside them.
pub(crate) fn expand(api: &Api) -> TokenStream {
    if !api.openapi {
        return TokenStream::new();
    }

    let name = &api.name;
    let shown = name.unraw().to_string();
    let vis = &api.vis;
    let docs = &api.docs.0;
    let listed = listed(api);
    let refs = listed.schemes.iter().map(|scheme| {
        let ty = &scheme.ty;
        let checks_origin = scheme.checks_origin;

        quote_spanned! {scheme.span=>
            ::quoll::__private::openapi::SchemeRef {
                name: <#ty as ::quoll::rest::Scheme>::NAME,
                openapi: <#ty as ::quoll::rest::Scheme>::openapi,
                checks_origin: #checks_origin,
            }
        }
    });
    let checks = name_checks(api, &listed.schemes);
    let groups = api
        .groups
        .iter()
        .zip(&listed.routes)
        .map(|(group, indices)| group_description(api, group, indices));

    quote! {
        impl #name {
            /// The OpenAPI 3.1 document of the contract: its paths, parameters, bodies and results, the schemas of
            /// its types, its security schemes and the problem of its errors. Its title and its version are the
            /// contract crate's, as Cargo gives them; built otherwise, the api's name and `0.0.0`.
            #vis fn openapi() -> ::quoll::rest::openapi::Document {
                const SCHEMES: &[::quoll::__private::openapi::SchemeRef] = &[#(#refs),*];
                const _: () = {
                    #(#checks)*
                };
                static API: ::quoll::__private::openapi::Api = ::quoll::__private::openapi::Api {
                    docs: &[#(#docs),*],
                    title: match ::core::option_env!("CARGO_PKG_NAME") {
                        ::core::option::Option::Some(name) => name,
                        ::core::option::Option::None => #shown,
                    },
                    version: match ::core::option_env!("CARGO_PKG_VERSION") {
                        ::core::option::Option::Some(version) => version,
                        ::core::option::Option::None => "0.0.0",
                    },
                    schemes: SCHEMES,
                    groups: &[#(#groups),*],
                };

                ::quoll::__private::openapi::document(&API)
            }
        }
    }
}

/// A scheme of the api, as the document lists it.
struct ListedScheme {
    /// The scheme as the contract writes it, which errors show.
    word: String,
    /// Its type.
    ty: TokenStream,
    /// Whether it is a type of the application's, whose name the macro does not know.
    foreign: bool,
    /// Whether it refuses a request from another origin that changes things, with a 403: a declared cookie scheme.
    checks_origin: bool,
    /// Where an error about it points.
    span: Span,
}

/// The schemes of the api, each once, and the indices of those each route takes.
struct Listed {
    /// Each scheme once, by its word: the declared ones, in the order the contract writes them, then the others the
    /// routes take.
    schemes: Vec<ListedScheme>,
    /// For each group, for each of its routes, the indices in `schemes` of those the route takes, in its order.
    routes: Vec<Vec<Vec<usize>>>,
}

fn listed(api: &Api) -> Listed {
    let mut schemes: Vec<ListedScheme> = api
        .schemes
        .iter()
        .map(|scheme| {
            let name = &scheme.name;

            ListedScheme {
                word: name.unraw().to_string(),
                ty: quote! { #name },
                foreign: false,
                checks_origin: matches!(scheme.kind, DeclaredKind::Cookie(_)),
                span: name.span(),
            }
        })
        .collect();
    let mut routes = Vec::with_capacity(api.groups.len());

    for group in &api.groups {
        let mut indices = Vec::with_capacity(group.routes.len());

        for route in &group.routes {
            let taken = route
                .schemes()
                .map(|scheme| {
                    let word = scheme.word();
                    if let Some(index) = schemes.iter().position(|listed| listed.word == word) {
                        return index;
                    }

                    schemes.push(ListedScheme {
                        word,
                        ty: scheme_type(scheme),
                        foreign: scheme.kind == SchemeKind::Foreign,
                        checks_origin: false,
                        span: scheme.span(),
                    });
                    schemes.len() - 1
                })
                .collect();
            indices.push(taken);
        }
        routes.push(indices);
    }

    Listed { schemes, routes }
}

/// The compile-time checks of the names of the schemes, which name them in the document's `securitySchemes`: each
/// one is a component's name, and no two are one. The macro knows the names of `Bearer`, `Basic` and the declared
/// schemes, which it keeps apart, so each check is about a scheme of the application's.
fn name_checks(api: &Api, schemes: &[ListedScheme]) -> Vec<TokenStream> {
    let mut checks = Vec::new();

    for (index, second) in schemes.iter().enumerate() {
        if second.foreign {
            let message = LitStr::new(
                &format!(
                    "the scheme `{}` has a `NAME` that cannot name it in the OpenAPI document: write it with ASCII letters, digits, `.`, `-` and `_`",
                    second.word
                ),
                second.span,
            );

            checks.push(quote_spanned! {second.span=>
                ::core::assert!(
                    ::quoll::__private::openapi::is_component_name(SCHEMES[#index].name),
                    #message
                );
            });
        }

        for (first_index, first) in schemes[..index].iter().enumerate() {
            if !first.foreign && !second.foreign {
                continue;
            }

            let message = LitStr::new(
                &format!(
                    "the schemes `{}` and `{}` of the api `{}` have one `NAME`, which names a scheme in the OpenAPI document: give each scheme its own, and name a scheme by one path throughout the api",
                    first.word,
                    second.word,
                    api.name.unraw()
                ),
                second.span,
            );

            checks.push(quote_spanned! {second.span=>
                ::core::assert!(
                    !::quoll::__private::openapi::same_name(
                        SCHEMES[#first_index].name,
                        SCHEMES[#index].name,
                    ),
                    #message
                );
            });
        }
    }

    checks
}

/// The type a parameter's or a body's schema describes: the `T` of an `Option<T>` for a query key or a header, the
/// `T` of a `Form<T>`; none for text and bytes.
fn param_schema(param: &Param) -> Option<&Type> {
    match &param.kind {
        ParamKind::Path | ParamKind::QueryStruct => Some(&param.ty),
        ParamKind::Query { .. } | ParamKind::Header { .. } => {
            Some(option_inner(&param.ty).unwrap_or(&param.ty))
        }
        ParamKind::Body(kind) if !kind.schema => None,
        ParamKind::Body(_) => Some(param.inner.as_ref().unwrap_or(&param.ty)),
    }
}

fn group_description(api: &Api, group: &Group, indices: &[Vec<usize>]) -> TokenStream {
    let name = group.name.unraw().to_string();
    let docs = &group.docs.0;
    let routes = group
        .routes
        .iter()
        .zip(indices)
        .map(|(route, indices)| route_description(api, group, route, indices));

    quote! {
        ::quoll::__private::openapi::Group {
            name: #name,
            docs: &[#(#docs),*],
            routes: &[#(#routes),*],
        }
    }
}

fn route_description(api: &Api, group: &Group, route: &Route, indices: &[usize]) -> TokenStream {
    let id = route.operation_id(group);
    let method = route.method.as_str().to_ascii_lowercase();
    let path = route.full_path(&api.prefix);
    let docs = &route.docs.0;
    let params = route.params.iter().filter_map(param_description);
    let body = match route.body() {
        Some((param, kind)) => {
            let variant = syn::Ident::new(kind.document, proc_macro2::Span::call_site());

            match param_schema(param).map(schema_fn) {
                Some(schema) => quote! { ::core::option::Option::Some(
                    ::quoll::__private::openapi::Body::#variant(#schema)
                ) },
                None => quote! { ::core::option::Option::Some(
                    ::quoll::__private::openapi::Body::#variant
                ) },
            }
        }
        None => quote! { ::core::option::Option::None },
    };
    let reply = match route.reply.as_deref() {
        None => quote! { ::quoll::__private::openapi::Reply::Empty },
        Some(returned) => {
            let kind = returned.kind();
            let variant = syn::Ident::new(kind.document, proc_macro2::Span::call_site());

            if kind.schema {
                let schema = schema_fn(returned.inner.as_ref().unwrap_or(&returned.served));

                quote! { ::quoll::__private::openapi::Reply::#variant(#schema) }
            } else {
                quote! { ::quoll::__private::openapi::Reply::#variant }
            }
        }
    };
    let auth = match &route.auth {
        None => quote! { ::core::option::Option::None },
        Some(auth) => {
            let optional = auth.optional;
            let scopes = &auth.scopes;

            quote! {
                ::core::option::Option::Some(::quoll::__private::openapi::Auth {
                    schemes: &[#(#indices),*],
                    optional: #optional,
                    scopes: &[#(#scopes),*],
                })
            }
        }
    };

    quote! {
        ::quoll::__private::openapi::Route {
            id: #id,
            method: #method,
            path: #path,
            docs: &[#(#docs),*],
            params: &[#(#params),*],
            body: #body,
            reply: #reply,
            auth: #auth,
        }
    }
}

/// A parameter of the document: any argument but the body.
fn param_description(param: &Param) -> Option<TokenStream> {
    let (name, place, schema) = match &param.kind {
        ParamKind::Path => (
            param.name.unraw().to_string(),
            quote! { Path },
            schema_fn(&param.ty),
        ),
        ParamKind::Query { key } => {
            let required = option_inner(&param.ty).is_none() && !is_vec(&param.ty);

            (
                key.clone(),
                quote! { Query { required: #required } },
                schema_fn(param_schema(param)?),
            )
        }
        ParamKind::QueryStruct => {
            let ty = &param.ty;
            let schema = quote_spanned! {ty.span()=> ::quoll::__private::openapi::inline::<#ty> };

            (
                param.name.unraw().to_string(),
                quote! { QueryStruct },
                schema,
            )
        }
        ParamKind::Header { name } => {
            let required = option_inner(&param.ty).is_none();

            (
                name.clone(),
                quote! { Header { required: #required } },
                schema_fn(param_schema(param)?),
            )
        }
        ParamKind::Body(_) => return None,
    };

    Some(quote! {
        ::quoll::__private::openapi::Param {
            name: #name,
            place: ::quoll::__private::openapi::Place::#place,
            schema: #schema,
        }
    })
}

/// The function that writes the schema of `ty`, spanned on it, so that a type with no schema is an error on its name.
fn schema_fn(ty: &Type) -> TokenStream {
    quote_spanned! {ty.span()=> ::quoll::__private::openapi::schema::<#ty> }
}

#[cfg(test)]
mod tests {
    use quote::quote;

    use super::*;
    use crate::syntax::Contract;

    fn expansion(contract: TokenStream) -> String {
        let Contract(api) = syn::parse2(contract).expect("the contract is valid");

        expand(&api).to_string()
    }

    #[test]
    fn each_route_is_described_with_its_parameters() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            #[auth(Bearer, optional)]
            pub group Notes {
                GET "/notes/{id}?page={page}" => get(id: i64, page: Option<u32>, header("X-Trace") trace: String) -> Note;
                POST "/notes" => create(note: Form<NewNote>) -> Created<Note>;
                PUT "/files/{name}" => upload(name: String, file: Bytes) -> Events<Change>;
            }
        });

        for expected in [
            r#"id : "notes_get" , method : "get" , path : "/api/notes/{id}""#,
            r#"name : "id" , place : :: quoll :: __private :: openapi :: Place :: Path , schema : :: quoll :: __private :: openapi :: schema :: < i64 >"#,
            r#"name : "page" , place : :: quoll :: __private :: openapi :: Place :: Query { required : false } , schema : :: quoll :: __private :: openapi :: schema :: < u32 >"#,
            r#"name : "X-Trace" , place : :: quoll :: __private :: openapi :: Place :: Header { required : true }"#,
            "Body :: Form (:: quoll :: __private :: openapi :: schema :: < NewNote >)",
            "Reply :: Created (:: quoll :: __private :: openapi :: schema :: < Note >)",
            "Body :: Bytes)",
            "Reply :: Events (:: quoll :: __private :: openapi :: schema :: < Change >)",
            "optional : true",
        ] {
            assert!(tokens.contains(expected), "{expected} in {tokens}");
        }
        assert!(
            !tokens.contains("schema :: < Bytes >"),
            "bytes have no schema: {tokens}"
        );
    }

    #[test]
    fn a_scheme_of_the_application_is_checked_against_the_others() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            pub scheme Key = header "X-Api-Key";

            #[auth(Bearer | crate::auth::Hmac)]
            pub group Notes {
                GET "/notes" => list() -> Vec<Note>;
                #[auth(crate::auth::Hmac)]
                GET "/stats" => stats() -> u64;
            }
        });

        // Each scheme once, in the order: declared, then taken by the routes.
        assert_eq!(
            tokens
                .matches("< crate :: auth :: Hmac as :: quoll :: rest :: Scheme > :: NAME")
                .count(),
            1,
            "{tokens}"
        );
        assert!(tokens.contains("schemes : & [1usize , 2usize]"), "{tokens}");
        assert!(tokens.contains("schemes : & [2usize]"), "{tokens}");
        assert!(
            tokens.contains("is_component_name (SCHEMES [2usize] . name)"),
            "{tokens}"
        );
        assert!(
            tokens.contains("same_name (SCHEMES [0usize] . name , SCHEMES [2usize] . name ,)"),
            "{tokens}"
        );
        assert!(
            tokens.contains("same_name (SCHEMES [1usize] . name , SCHEMES [2usize] . name ,)"),
            "{tokens}"
        );
        assert!(
            !tokens.contains("same_name (SCHEMES [0usize] . name , SCHEMES [1usize] . name ,)"),
            "the macro keeps its own schemes apart: {tokens}"
        );
        assert!(
            !tokens.contains("is_component_name (SCHEMES [0usize]"),
            "the macro checks its own names: {tokens}"
        );
    }

    #[test]
    fn the_title_and_the_version_have_a_fallback_outside_cargo() {
        let tokens = expansion(quote! {
            pub api Notes = "/api";
        });

        assert!(
            tokens.contains(r#"match :: core :: option_env ! ("CARGO_PKG_NAME") { :: core :: option :: Option :: Some (name) => name , :: core :: option :: Option :: None => "Notes" , }"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(r#":: core :: option :: Option :: None => "0.0.0""#),
            "{tokens}"
        );
    }

    #[test]
    fn only_a_declared_cookie_scheme_checks_the_origin() {
        let tokens = expansion(quote! {
            pub api Api = "/api";

            pub scheme Session = cookie "session";
            pub scheme Key = header "X-Api-Key";

            #[auth(Session | Key | crate::Crumb)]
            pub group Notes {
                POST "/notes" => create();
            }
        });

        assert_eq!(
            tokens.matches("checks_origin : true").count(),
            1,
            "{tokens}"
        );
        assert_eq!(
            tokens.matches("checks_origin : false").count(),
            2,
            "{tokens}"
        );
        assert!(
            tokens.find("checks_origin : true") < tokens.find("< Key as"),
            "the first is the cookie's: {tokens}"
        );
    }

    #[test]
    fn a_contract_with_openapi_none_has_no_document() {
        let tokens = expansion(quote! {
            #[openapi(none)]
            pub api Api = "";

            pub group Repos {
                GET "/repos" => list() -> Vec<Repo>;
            }
        });

        assert!(tokens.is_empty(), "{tokens}");
    }
}

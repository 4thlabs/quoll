//! What the OpenAPI document of a contract becomes, on both sides and whatever the features: `Api::openapi`, which
//! hands a description of the api to `quoll::__private::openapi::document`, and the check that the schemes of the api
//! have names of their own.

use proc_macro2::TokenStream;
use quote::{quote, quote_spanned};
use syn::spanned::Spanned;
use syn::{LitStr, Type, ext::IdentExt};

use crate::model::{Api, Group, Param, ParamKind, Route, SchemeKind, is_vec, option_inner, peel};
use crate::scheme::scheme_type;

/// `Api::openapi`, unless the contract says `#[openapi(none)]`.
pub(crate) fn expand(api: &Api) -> TokenStream {
    if !api.openapi {
        return TokenStream::new();
    }

    let name = &api.name;
    let vis = &api.vis;
    let docs = &api.docs.0;
    let declared = api.schemes.iter().map(|scheme| {
        let name = &scheme.name;

        scheme_ref(&quote! { #name })
    });
    let groups = api.groups.iter().map(|group| group_description(api, group));

    quote! {
        impl #name {
            /// The OpenAPI 3.1 document of the contract: its paths, parameters, bodies and results, the schemas of
            /// its types, its security schemes and the problem of its errors. Its title and its version are the
            /// contract crate's.
            #vis fn openapi() -> ::quoll::rest::openapi::Document {
                static API: ::quoll::__private::openapi::Api = ::quoll::__private::openapi::Api {
                    docs: &[#(#docs),*],
                    title: ::core::env!("CARGO_PKG_NAME"),
                    version: ::core::env!("CARGO_PKG_VERSION"),
                    schemes: &[#(#declared),*],
                    groups: &[#(#groups),*],
                };

                ::quoll::__private::openapi::document(&API)
            }
        }
    }
}

/// The checks of the document, written in the api's `const _` block: no two schemes of the api, one of which is the
/// application's, have one name. A type with no schema is an error on its name in `Api::openapi`, which names the
/// schema of each type the contract describes.
pub(crate) fn checks(api: &Api) -> Vec<TokenStream> {
    if !api.openapi {
        return Vec::new();
    }

    name_checks(api)
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
        ParamKind::Body(kind) if kind.document == "Form" => type_argument(&param.ty),
        ParamKind::Body(_) => Some(&param.ty),
    }
}

/// The type a result's schema describes: itself for JSON, the `T` that a type of quoll's wraps.
fn result_type(served: &Type, known: bool) -> &Type {
    if known {
        type_argument(served).unwrap_or(served)
    } else {
        served
    }
}

/// The one type argument of a type's last segment: the `T` of `Form<T>`.
fn type_argument(ty: &Type) -> Option<&Type> {
    let Type::Path(path) = peel(ty) else {
        return None;
    };
    let syn::PathArguments::AngleBracketed(args) = &path.path.segments.last()?.arguments else {
        return None;
    };

    args.args.iter().find_map(|arg| match arg {
        syn::GenericArgument::Type(ty) => Some(ty),
        _ => None,
    })
}

/// A compile-time check per pair of schemes of the api that might have one name: each name is a key of the
/// document's `securitySchemes`. The macro knows the names of `Bearer`, `Basic` and the declared schemes, which it
/// keeps apart, so a pair holds a scheme of the application's.
fn name_checks(api: &Api) -> Vec<TokenStream> {
    // Each scheme once, by its word: the declared ones, then those the routes take.
    let mut schemes: Vec<(String, TokenStream, bool)> = api
        .schemes
        .iter()
        .map(|scheme| {
            let name = &scheme.name;

            (name.unraw().to_string(), quote! { #name }, false)
        })
        .collect();

    for scheme in api
        .groups
        .iter()
        .flat_map(|group| &group.routes)
        .flat_map(|route| route.schemes())
    {
        let word = scheme.word();
        if schemes.iter().any(|(seen, ..)| *seen == word) {
            continue;
        }

        schemes.push((
            word,
            scheme_type(scheme),
            scheme.kind == SchemeKind::Foreign,
        ));
    }

    let mut checks = Vec::new();
    for (index, (second_word, second, second_foreign)) in schemes.iter().enumerate() {
        for (first_word, first, first_foreign) in &schemes[..index] {
            if !first_foreign && !second_foreign {
                continue;
            }

            let message = LitStr::new(
                &format!(
                    "the schemes `{first_word}` and `{second_word}` of the api `{}` have one `NAME`, which names a scheme in the OpenAPI document: give each scheme its own, and name a scheme by one path throughout the api",
                    api.name.unraw()
                ),
                second.span(),
            );

            checks.push(quote_spanned! {second.span()=>
                ::core::assert!(
                    !::quoll::__private::openapi::same_name(
                        <#first as ::quoll::rest::Scheme>::NAME,
                        <#second as ::quoll::rest::Scheme>::NAME,
                    ),
                    #message
                );
            });
        }
    }

    checks
}

/// The description of a scheme: its name and its `openapi`, which the scheme gives.
fn scheme_ref(scheme: &TokenStream) -> TokenStream {
    quote! {
        ::quoll::__private::openapi::SchemeRef {
            name: <#scheme as ::quoll::rest::Scheme>::NAME,
            openapi: <#scheme as ::quoll::rest::Scheme>::openapi,
        }
    }
}

fn group_description(api: &Api, group: &Group) -> TokenStream {
    let name = group.name.unraw().to_string();
    let docs = &group.docs.0;
    let routes = group
        .routes
        .iter()
        .map(|route| route_description(api, group, route));

    quote! {
        ::quoll::__private::openapi::Group {
            name: #name,
            docs: &[#(#docs),*],
            routes: &[#(#routes),*],
        }
    }
}

fn route_description(api: &Api, group: &Group, route: &Route) -> TokenStream {
    let id = route.operation_id(group);
    let method = route.method.as_str().to_ascii_lowercase();
    let path = route.full_path(&api.prefix);
    let docs = &route.docs.0;
    let params = route.params.iter().filter_map(param_description);
    let body = match route.body() {
        Some((param, kind)) => {
            let variant = syn::Ident::new(kind.document, proc_macro2::Span::call_site());
            let schema = param_schema(param).map(schema_fn);

            match schema {
                Some(schema) => quote! { ::core::option::Option::Some(
                    ::quoll::__private::openapi::Body::#variant(#schema)
                ) },
                None if kind.schema => quote! { ::core::option::Option::None },
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
                let schema = schema_fn(result_type(&returned.served, returned.known.is_some()));

                quote! { ::quoll::__private::openapi::Reply::#variant(#schema) }
            } else {
                quote! { ::quoll::__private::openapi::Reply::#variant }
            }
        }
    };
    let auth = match &route.auth {
        None => quote! { ::core::option::Option::None },
        Some(auth) => {
            let schemes = auth
                .schemes
                .iter()
                .map(|scheme| scheme_ref(&scheme_type(scheme)));
            let optional = auth.optional;
            let scopes = &auth.scopes;

            quote! {
                ::core::option::Option::Some(::quoll::__private::openapi::Auth {
                    schemes: &[#(#schemes),*],
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
    let (name, place, required, schema) = match &param.kind {
        ParamKind::Path => (
            param.name.unraw().to_string(),
            "Path",
            true,
            schema_fn(&param.ty),
        ),
        ParamKind::Query { key } => (
            key.clone(),
            "Query",
            option_inner(&param.ty).is_none() && !is_vec(&param.ty),
            schema_fn(param_schema(param)?),
        ),
        ParamKind::QueryStruct => {
            let ty = &param.ty;
            let schema = quote_spanned! {ty.span()=> ::quoll::__private::openapi::inline::<#ty> };

            (param.name.unraw().to_string(), "QueryStruct", true, schema)
        }
        ParamKind::Header { name } => (
            name.clone(),
            "Header",
            option_inner(&param.ty).is_none(),
            schema_fn(param_schema(param)?),
        ),
        ParamKind::Body(_) => return None,
    };
    let place = syn::Ident::new(place, proc_macro2::Span::call_site());

    Some(quote! {
        ::quoll::__private::openapi::Param {
            name: #name,
            place: ::quoll::__private::openapi::Place::#place,
            required: #required,
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
        let checks = checks(&api);

        format!("{} {}", expand(&api), quote! { #(#checks)* })
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
            r#"name : "id" , place : :: quoll :: __private :: openapi :: Place :: Path , required : true , schema : :: quoll :: __private :: openapi :: schema :: < i64 >"#,
            r#"name : "page" , place : :: quoll :: __private :: openapi :: Place :: Query , required : false , schema : :: quoll :: __private :: openapi :: schema :: < u32 >"#,
            r#"name : "X-Trace" , place : :: quoll :: __private :: openapi :: Place :: Header , required : true"#,
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
            }
        });

        assert!(
            tokens.contains("< Key as :: quoll :: rest :: Scheme > :: NAME , < crate :: auth :: Hmac as :: quoll :: rest :: Scheme > :: NAME"),
            "{tokens}"
        );
        assert!(
            tokens.contains("< :: quoll :: rest :: Bearer as :: quoll :: rest :: Scheme > :: NAME , < crate :: auth :: Hmac as :: quoll :: rest :: Scheme > :: NAME"),
            "{tokens}"
        );
        assert!(
            !tokens.contains("same_name (< Key as :: quoll :: rest :: Scheme > :: NAME , < :: quoll :: rest :: Bearer"),
            "the macro keeps its own schemes apart: {tokens}"
        );
    }
}

//! What a scheme the contract declares becomes, on both sides and whatever the features: a type implementing
//! `rest::Scheme`, which reads its credential through a helper of `quoll`, and which says where its credential
//! travels, for the client's `client::key`.

use proc_macro2::TokenStream;
use quote::{quote, quote_spanned};
use syn::ext::IdentExt;

use crate::docs::doc_attrs;
use crate::model::{Api, DeclaredKind, DeclaredScheme, Group, Scheme, SchemeKind, reach};

/// The types of the schemes the api declares.
pub(crate) fn expand(api: &Api) -> TokenStream {
    let schemes = api.schemes.iter().map(declared);

    quote! { #(#schemes)* }
}

fn declared(scheme: &DeclaredScheme) -> TokenStream {
    let DeclaredScheme {
        docs, vis, name, ..
    } = scheme;
    let shown = name.unraw().to_string();
    let (travels, read, place, challenge, openapi) = match &scheme.kind {
        DeclaredKind::Header(header) => {
            let lower = header.to_ascii_lowercase();

            (
                vec![format!(" A credential in the header `{header}`, read as a `String`.")],
                quote! { ::quoll::__private::auth::header(request, #lower) },
                quote! { Header(#lower) },
                None,
                quote! { header(#header) },
            )
        }
        DeclaredKind::Cookie(cookie) => (
            vec![
                format!(
                    " A credential in the cookie `{cookie}`, read as a `String`: the first of that name that is not empty, which a browser sends for the longest path."
                ),
                String::new(),
                " A request that carries it with a method other than `GET`, `HEAD` or `OPTIONS` comes from the server's own origin, as its `Sec-Fetch-Site` says, or its `Origin` compared with its `Host`, or is refused with a 403. Behind a reverse proxy, the proxy passes the browser's `Host`, as nginx does with `proxy_set_header Host $host`. Only the server's own origin is trusted for now.".to_owned(),
                String::new(),
                " The application sets the cookie `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`, and names it with the prefix `__Host-`, as `__Host-session`, which a page of a subdomain cannot set.".to_owned(),
            ],
            quote! { ::quoll::__private::auth::cookie(request, #cookie) },
            quote! { Cookie(#cookie) },
            None,
            quote! { cookie(#cookie) },
        ),
        DeclaredKind::Query(key) => (
            vec![
                format!(" A credential in the query key `{key}`, read as a `String`."),
                String::new(),
                format!(
                    " A route that takes it reads no other value of `{key}`: the macro refuses a `{key}={{...}}` in its string, but cannot see the fields of a query struct, of which none is named `{key}`."
                ),
            ],
            quote! { ::quoll::__private::auth::query(request, #key) },
            quote! { Query(#key) },
            None,
            quote! { query(#key) },
        ),
        DeclaredKind::OpenId(url) => (
            vec![format!(
                " An OpenID Connect token in `Authorization: Bearer`, read as a `String`, from the provider at `{url}`."
            )],
            quote! { ::quoll::__private::auth::openid(request) },
            quote! { Bearer },
            Some(quote! {
                const CHALLENGE: ::quoll::__private::http::HeaderValue =
                    ::quoll::__private::http::HeaderValue::from_static("Bearer");
            }),
            quote! { openid(#url) },
        ),
    };
    let comments = &docs.0;
    let checks_origin = matches!(scheme.kind, DeclaredKind::Cookie(_));
    let reach = reach(vis);

    let mut lines = docs.0.clone();
    if !lines.is_empty() {
        lines.push(String::new());
    }
    lines.extend(travels);
    let docs = doc_attrs(&lines);

    quote! {
        #docs
        #[derive(
            ::core::fmt::Debug,
            ::core::clone::Clone,
            ::core::marker::Copy,
            ::core::cmp::PartialEq,
            ::core::cmp::Eq,
            ::core::hash::Hash,
            ::core::default::Default,
        )]
        #vis struct #name;

        impl ::quoll::rest::Scheme for #name {
            type Credential = ::std::string::String;

            const NAME: &'static str = #shown;

            #challenge

            fn read(
                request: &::quoll::__private::http::request::Parts,
            ) -> ::quoll::rest::Result<::core::option::Option<::std::string::String>> {
                #read
            }

            fn openapi() -> ::quoll::rest::openapi::SecurityScheme {
                ::quoll::__private::openapi::described(
                    ::quoll::rest::openapi::SecurityScheme::#openapi,
                    &[#(#comments),*],
                )
            }
        }

        impl ::quoll::__private::auth::Declared for #name {
            const PLACE: ::quoll::__private::auth::Place = ::quoll::__private::auth::Place::#place;
            const CHECKS_ORIGIN: bool = #checks_origin;
            const REACH: u8 = #reach;
        }
    }
}

/// The type of a scheme, by which the generated code names it: the crate's, `::quoll::rest::Bearer`, or the path the
/// contract writes, spanned on it, for a declared scheme or one of the application's.
pub(crate) fn scheme_type(scheme: &Scheme) -> TokenStream {
    let span = scheme.span();

    match scheme.kind {
        SchemeKind::Bearer => quote_spanned! {span=> ::quoll::rest::Bearer },
        SchemeKind::Basic => quote_spanned! {span=> ::quoll::rest::Basic },
        SchemeKind::Declared(_) | SchemeKind::Foreign => {
            let path = &scheme.path;

            quote! { #path }
        }
    }
}

/// The schemes of a route as one type: the scheme, or a tuple of the schemes of an alternative; `()` for none.
pub(crate) fn schemes_type(schemes: &[Scheme]) -> TokenStream {
    match schemes {
        [] => quote! { () },
        [scheme] => scheme_type(scheme),
        schemes => {
            let types = schemes.iter().map(scheme_type);

            quote! { (#(#types,)*) }
        }
    }
}

/// The challenge of each scheme of the application's a group takes, evaluated in a constant: a `CHALLENGE` that is not
/// a header's value fails as the contract compiles, whatever the features, rather than as the server is built, when
/// the code that sends it is instantiated. The crate's schemes and the declared ones have a challenge the macro checks.
pub(crate) fn challenge_checks(group: &Group) -> TokenStream {
    let mut seen: Vec<String> = Vec::new();
    let mut checks = Vec::new();

    for scheme in group.routes.iter().flat_map(|route| route.schemes()) {
        if scheme.kind != SchemeKind::Foreign || seen.contains(&scheme.word()) {
            continue;
        }

        seen.push(scheme.word());

        let ty = scheme_type(scheme);

        // `forget`, as a `HeaderValue` cannot be dropped in a constant.
        checks.push(quote_spanned! {scheme.span()=>
            ::core::mem::forget(<#ty as ::quoll::rest::Scheme>::CHALLENGE);
        });
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

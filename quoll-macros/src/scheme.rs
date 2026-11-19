//! What a scheme the contract declares becomes, on both sides and whatever the features: a type implementing
//! `rest::Scheme`, which reads its credential through a helper of `quoll`, and which says where its credential
//! travels, for the client's `rest::client::key`.

use proc_macro2::TokenStream;
use quote::{quote, quote_spanned};
use syn::ext::IdentExt;

use crate::doc_attrs;
use crate::model::{Api, DeclaredKind, DeclaredScheme, Scheme, SchemeKind};

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
    let (travels, read, place, challenge) = match &scheme.kind {
        DeclaredKind::Header(header) => {
            let lower = header.to_ascii_lowercase();

            (
                format!(" A credential in the header `{header}`, read as a `String`."),
                quote! { ::quoll::__private::auth::header(request, #lower) },
                quote! { Header(#lower) },
                None,
            )
        }
        DeclaredKind::Cookie(cookie) => (
            format!(
                " A credential in the cookie `{cookie}`, read as a `String`. A request that carries it with a method other than `GET`, `HEAD` or `OPTIONS` comes from the server's own origin, or is refused with a 403."
            ),
            quote! { ::quoll::__private::auth::cookie(request, #cookie) },
            quote! { Cookie(#cookie) },
            None,
        ),
        DeclaredKind::Query(key) => (
            format!(" A credential in the query key `{key}`, read as a `String`."),
            quote! { ::quoll::__private::auth::query(request, #key) },
            quote! { Query(#key) },
            None,
        ),
        DeclaredKind::OpenId(url) => (
            format!(
                " An OpenID Connect token in `Authorization: Bearer`, read as a `String`, from the provider at `{url}`."
            ),
            quote! { ::quoll::__private::auth::openid(request) },
            quote! { Bearer },
            Some(quote! {
                const CHALLENGE: ::quoll::__private::http::HeaderValue =
                    ::quoll::__private::http::HeaderValue::from_static("Bearer");
            }),
        ),
    };

    let mut lines = docs.0.clone();
    if !lines.is_empty() {
        lines.push(String::new());
    }
    lines.push(travels);
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
        }

        impl ::quoll::__private::auth::Declared for #name {
            const PLACE: ::quoll::__private::auth::Place = ::quoll::__private::auth::Place::#place;
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

/// Checks that each scheme of the application's the contract names is a scheme, whatever the features of `quoll`, so
/// that a type that is not one is an error on its name.
pub(crate) fn checks(api: &Api) -> Vec<TokenStream> {
    let mut seen: Vec<String> = Vec::new();
    let mut checks = Vec::new();

    for scheme in api
        .groups
        .iter()
        .flat_map(|group| &group.routes)
        .flat_map(|route| route.schemes())
    {
        if scheme.kind != SchemeKind::Foreign || seen.contains(&scheme.word()) {
            continue;
        }

        seen.push(scheme.word());

        let path = &scheme.path;
        checks.push(
            quote_spanned! {scheme.span()=> ::quoll::__private::auth::is_scheme::<#path>(); },
        );
    }

    checks
}

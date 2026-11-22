//! What the fixed headers of a contract become: the headers each route takes, as the client, the descriptor and the
//! document name them, whatever the features, and the compile-time checks of the values written as constants, which
//! the macro does not see.

use proc_macro2::TokenStream;
use quote::{ToTokens, quote, quote_spanned};
use syn::LitStr;
use syn::spanned::Spanned;

use crate::descriptor::Cx;
use crate::model::headers::{RouteHeaders, VALUE_MESSAGE};
use crate::model::{Fixed, Group, Headers, Route, Value};

/// A fixed header, `::quoll::__private::headers::Fixed`.
fn fixed(header: &Fixed) -> TokenStream {
    let name = header.name.value();
    let value = match &header.value {
        Value::Literal(value) => value.to_token_stream(),
        Value::Const(path) => path.to_token_stream(),
    };

    quote_spanned! {header.value_span()=>
        ::quoll::__private::headers::Fixed { name: #name, value: #value }
    }
}

/// The api's fixed headers, a `&'static [Fixed]`.
pub(crate) fn api_headers(headers: &Headers) -> TokenStream {
    let headers = headers.set.iter().map(fixed);

    quote! { const { &[#(#headers),*] } }
}

/// What `route` takes of the fixed headers, beside its api's, a `::quoll::__private::headers::RouteHeaders`, for a
/// constant's initializer or a `const` block.
pub(crate) fn route_headers(group: &Group, route: &Route) -> TokenStream {
    let headers = RouteHeaders::of(group, route);
    let own = headers.own.iter().map(|header| fixed(header));
    let inherits = headers.inherits;
    let leaves = headers.leaves.iter().map(|name| name.value());

    quote! {
        ::quoll::__private::headers::RouteHeaders {
            own: &[#(#own),*],
            inherits: #inherits,
            leaves: &[#(#leaves),*],
        }
    }
}

/// The compile-time checks of the fixed headers of the api: each value written as a constant is one a header takes.
pub(crate) fn api_checks(headers: &Headers) -> TokenStream {
    let checks = headers.set.iter().filter_map(value_check);

    wrap(checks.collect())
}

/// The compile-time checks of the fixed headers of a group and its routes: each value written as a constant is one a
/// header takes, and a `Content-Type` written as one, the route's own or one it takes of its group or its api,
/// keeps the media type of the route's body.
pub(crate) fn group_checks(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let mut checks: Vec<TokenStream> = group.headers.set.iter().filter_map(value_check).collect();

    for route in &group.routes {
        checks.extend(route.headers.set.iter().filter_map(value_check));

        let headers = RouteHeaders::of(group, route);
        let own = headers
            .own
            .iter()
            .copied()
            .find(|header| header.is("Content-Type"));
        let inherited = cx
            .api
            .and_then(|api| headers.api_header(&api.headers, "Content-Type"));
        let Some(content_type) = own.or(inherited) else {
            continue;
        };
        let Value::Const(path) = &content_type.value else {
            continue;
        };
        let Some(media_type) = route.body().and_then(|(_, kind)| kind.media_type) else {
            continue;
        };

        let span = if own.is_some() {
            path.span()
        } else {
            route.span
        };
        let shown = shown(path);
        let message = LitStr::new(
            &format!(
                "the `Content-Type` `{shown}` the route `{}` takes is not of the media type of its body, `{media_type}`, in UTF-8, which a fixed one keeps, adding a parameter as `{media_type}; charset=utf-8`",
                route.name
            ),
            span,
        );

        checks.push(quote_spanned! {span=>
            ::core::assert!(
                ::quoll::__private::headers::keeps_media_type(#path, #media_type),
                #message
            );
        });
    }

    wrap(checks)
}

/// The check of a value written as a constant, spanned on its name.
fn value_check(header: &Fixed) -> Option<TokenStream> {
    let Value::Const(path) = &header.value else {
        return None;
    };
    let message = LitStr::new(
        &format!(
            "the value of `{}`, `{}`, is not one of a header: {VALUE_MESSAGE}",
            header.name.value(),
            shown(path)
        ),
        path.span(),
    );

    Some(quote_spanned! {path.span()=>
        ::core::assert!(::quoll::__private::headers::is_value(#path), #message);
    })
}

/// A constant's path, as a message shows it.
fn shown(path: &syn::Path) -> String {
    path.to_token_stream().to_string().replace(' ', "")
}

/// The checks in one constant, or nothing.
fn wrap(checks: Vec<TokenStream>) -> TokenStream {
    if checks.is_empty() {
        return TokenStream::new();
    }

    quote! {
        const _: () = {
            #(#checks)*
        };
    }
}

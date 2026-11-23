//! What a group says of itself to the api that lists it: the descriptor `__QuollGroupNotes`, a type implementing
//! `quoll::__private::group::Group` whose routes the api reads by type, and, on the api's side, how it names the items
//! of its groups, builds their tables of routes under its prefix, and checks the groups of other blocks against each
//! other at compile time.

use proc_macro2::{Span, TokenStream};
use quote::{format_ident, quote, quote_spanned};
use syn::{Ident, LitStr, ext::IdentExt};

use crate::headers::{api_headers, route_headers};
use crate::model::{Api, Group, Method, Route, Scheme, SchemeKind, reach, shown_vis, snake_case};
use crate::scheme::scheme_type;

/// A group as its generators see it: the group, and the api of its block when it has one.
pub(crate) struct Cx<'a> {
    /// The group.
    pub(crate) group: &'a Group,
    /// The api of the block that writes the group; `None` for a `rest::group!`, which any api may list.
    pub(crate) api: Option<&'a Api>,
    /// Whether the group has an OpenAPI document: its api's, or its own `#[openapi(none)]`.
    pub(crate) openapi: bool,
}

impl Cx<'_> {
    /// The api's prefix, when the block has an api: what the docs show before a route's path.
    pub(crate) fn prefix(&self) -> Option<&str> {
        self.api.map(|api| api.prefix.as_str())
    }

    /// The shape of `route`'s errors, the nearest `#[error(E)]` to it: its own, its group's, then its api's, which a
    /// `rest::group!` has none of. `None` for an RFC 9457 problem.
    pub(crate) fn error<'a>(&'a self, route: &'a Route) -> Option<&'a syn::Type> {
        route
            .error
            .as_ref()
            .or(self.group.error.as_ref())
            .or(self.api.and_then(|api| api.error.as_ref()))
    }
}

/// `__QuollGroupNotes` for the group `Notes`: the descriptor of its routes.
pub(crate) fn descriptor_name(name: &Ident) -> Ident {
    format_ident!("__QuollGroup{}", name.unraw(), span = name.span())
}

/// The descriptor of a group, with what it says of its routes.
pub(crate) fn expand(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let vis = &group.vis;
    let descriptor = descriptor_name(&group.name);
    let name = group.name.unraw().to_string();
    let docs = format!(
        " The routes of the group `{name}`, which an api that lists the group reads. Not part of the API."
    );
    let group_reach = reach(vis);
    let shown = shown_vis(vis);
    // A route with no `#[error]` of its own or its group's has problems, which it cannot take from an api.
    let unshaped = group
        .routes
        .iter()
        .find(|route| group.error.is_none() && route.error.is_none())
        .map(|route| route.name.unraw().to_string());
    let unshaped = match unshaped {
        Some(function) => quote! { ::core::option::Option::Some(#function) },
        None => quote! { ::core::option::Option::None },
    };
    let routes = group.routes.iter().map(|route| route_info(group, route));

    quote! {
        #[doc = #docs]
        #[doc(hidden)]
        #[derive(::core::fmt::Debug, ::core::clone::Clone, ::core::marker::Copy)]
        #vis struct #descriptor;

        impl ::quoll::__private::group::Group for #descriptor {
            const INFO: ::quoll::__private::group::GroupInfo = ::quoll::__private::group::GroupInfo {
                name: #name,
                reach: #group_reach,
                vis: #shown,
                unshaped: #unshaped,
                routes: &[#(#routes),*],
            };
        }
    }
}

/// A route below the api's prefix, as the api reads it.
fn route_info(group: &Group, route: &Route) -> TokenStream {
    let id = route.operation_id(group);
    let function = route.name.unraw().to_string();
    let verb = Ident::new(verb(route.method), Span::call_site());
    let path = below_prefix(route);
    let headers = route.headers().map(|(_, header)| header);
    let query = route.query_params().map(|(_, key)| key);
    let schemes: Vec<TokenStream> = route.schemes().map(scheme_type).collect();
    let names = schemes
        .iter()
        .map(|scheme| quote! { <#scheme as ::quoll::rest::Scheme>::NAME });
    let types = schemes
        .iter()
        .map(|scheme| quote! { ::core::any::TypeId::of::<#scheme>() });
    let infos = route
        .schemes()
        .zip(&schemes)
        .map(|(scheme, ty)| scheme_info(scheme, ty));
    let streams = route
        .reply
        .as_deref()
        .and_then(|returned| returned.known)
        .is_some_and(|known| known.stream);
    let body = match route.body().map(|(_, kind)| kind.media_type) {
        None => quote! { ::quoll::__private::group::BodyInfo::None },
        Some(Some(media_type)) => quote! { ::quoll::__private::group::BodyInfo::Of(#media_type) },
        Some(None) => quote! { ::quoll::__private::group::BodyInfo::Bytes },
    };
    let fixed = route_headers(group, route);

    quote! {
        ::quoll::__private::group::RouteInfo {
            id: #id,
            function: #function,
            method: ::quoll::__private::group::Verb::#verb,
            path: #path,
            headers: &[#(#headers),*],
            query: &[#(#query),*],
            schemes: &[#(#names),*],
            scheme_types: &[#(#types),*],
            scheme_infos: &[#(#infos),*],
            streams: #streams,
            body: #body,
            fixed: #fixed,
        }
    }
}

/// What a scheme says of itself to the checks of the api that lists the group: the crate's `Bearer` and `Basic` by
/// what the macro knows, any other by its type, through `quoll::__private::group::Probe`, which knows a declared
/// scheme's place and visibility.
fn scheme_info(scheme: &Scheme, ty: &TokenStream) -> TokenStream {
    let authorization = match scheme.kind {
        SchemeKind::Bearer => Some("Bearer"),
        SchemeKind::Basic => Some("Basic"),
        SchemeKind::Declared(_) | SchemeKind::Foreign => None,
    };

    if let Some(authorization) = authorization {
        return quote! {
            ::quoll::__private::group::SchemeInfo {
                reads: ::core::option::Option::Some(
                    ::quoll::__private::group::Reads::Authorization(#authorization),
                ),
                reach: ::core::option::Option::None,
            }
        };
    }

    quote! {
        {
            #[allow(unused_imports)]
            use ::quoll::__private::group::Unknown as _;

            ::quoll::__private::group::SchemeInfo {
                reads: <::quoll::__private::group::Probe<#ty>>::READS,
                reach: <::quoll::__private::group::Probe<#ty>>::REACH,
            }
        }
    }
}

/// The variant of `quoll::__private::group::Verb` of a method.
fn verb(method: Method) -> &'static str {
    match method {
        Method::Get => "Get",
        Method::Post => "Post",
        Method::Put => "Put",
        Method::Patch => "Patch",
        Method::Delete => "Delete",
        Method::Head => "Head",
        Method::Options => "Options",
    }
}

/// The route's path below the api's prefix, `/notes/{id}`, and empty for `"/"`, which is the prefix itself.
pub(crate) fn below_prefix(route: &Route) -> String {
    if route.path.is_empty() {
        String::new()
    } else {
        route.full_path("")
    }
}

/// The compile-time checks of a `rest::group!` that the api of a block makes of its own groups: each scheme an
/// alternative takes once, by its name, and, for a group with a document, the name of each scheme of the application's
/// fit to name it there.
pub(crate) fn alone_checks(cx: &Cx) -> TokenStream {
    let mut checks = Vec::new();
    let mut seen: Vec<String> = Vec::new();

    for route in &cx.group.routes {
        let schemes: Vec<_> = route.schemes().collect();

        for (index, second) in schemes.iter().enumerate() {
            let ty = scheme_type(second);
            let span = second.span();
            let foreign = second.kind == SchemeKind::Foreign;

            if foreign && cx.openapi && !seen.contains(&second.word()) {
                seen.push(second.word());

                let message = LitStr::new(
                    &format!(
                        "the scheme `{}` has a `NAME` that cannot name it in the OpenAPI document: write it with ASCII letters, digits, `.`, `-` and `_`",
                        second.word()
                    ),
                    span,
                );

                checks.push(quote_spanned! {span=>
                    ::core::assert!(
                        ::quoll::__private::openapi::is_component_name(<#ty as ::quoll::rest::Scheme>::NAME),
                        #message
                    );
                });
            }

            // A pair of the crate's schemes is told apart by the macro: any pair with a scheme of the application's
            // is checked, whichever comes first.
            for first in &schemes[..index] {
                if !foreign && first.kind != SchemeKind::Foreign {
                    continue;
                }

                let first_ty = scheme_type(first);
                let message = LitStr::new(
                    &format!(
                        "`{}` and `{}` have one `NAME`, which names one scheme: an alternative takes each scheme once, so remove one of them",
                        first.word(),
                        second.word()
                    ),
                    span,
                );

                checks.push(quote_spanned! {span=>
                    ::core::assert!(
                        !::quoll::__private::openapi::same_name(
                            <#first_ty as ::quoll::rest::Scheme>::NAME,
                            <#ty as ::quoll::rest::Scheme>::NAME,
                        ),
                        #message
                    );
                });
            }
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

/// A group of an api, as the api's generators name its items: one its block writes, by its name, or one it lists,
/// by its path.
pub(crate) struct GroupRef<'a> {
    /// The name of the group.
    pub(crate) name: &'a Ident,
    /// The path the list gives, for a group of another block.
    pub(crate) path: Option<&'a syn::Path>,
}

impl GroupRef<'_> {
    /// An item of the group's: `notes::NotesServer` for a group listed as `notes::Notes`, `NotesServer` for one the
    /// block writes. Spanned on the list's path, where an error about the group points.
    pub(crate) fn item(&self, item: &Ident) -> TokenStream {
        let Some(path) = self.path else {
            return quote! { #item };
        };

        let span = self.span();
        let item = Ident::new(&item.to_string(), span);
        let colon = path.leading_colon;
        let module = path
            .segments
            .iter()
            .take(path.segments.len() - 1)
            .map(|segment| &segment.ident);

        quote_spanned! {span=> #colon #(#module ::)* #item }
    }

    /// The group's descriptor.
    pub(crate) fn descriptor(&self) -> TokenStream {
        self.item(&descriptor_name(self.name))
    }

    /// The group's server trait.
    pub(crate) fn server_trait(&self) -> TokenStream {
        self.item(&format_ident!("{}Server", self.name.unraw()))
    }

    /// The group's client.
    pub(crate) fn client(&self) -> TokenStream {
        self.item(&format_ident!("{}Client", self.name.unraw()))
    }

    /// The group's field in `ApiServices`, and its method on `ApiClient`.
    pub(crate) fn field(&self) -> Ident {
        let snake = snake_case(self.name);
        let span = self.name.span();

        match syn::parse_str::<Ident>(&snake) {
            Ok(_) => Ident::new(&snake, span),
            Err(_) => Ident::new_raw(&snake, span),
        }
    }

    /// Where an error about the group points: its name, in the list or in its block.
    pub(crate) fn span(&self) -> Span {
        self.name.span()
    }
}

impl Api {
    /// The groups of the api: the ones it lists, in the order of the list, then the ones its block writes.
    pub(crate) fn refs(&self) -> Vec<GroupRef<'_>> {
        let listed = self.listed.iter().map(|path| GroupRef {
            name: Api::listed_name(path),
            path: Some(path),
        });
        let written = self.groups.iter().map(|group| GroupRef {
            name: &group.name,
            path: None,
        });

        listed.chain(written).collect()
    }
}

/// A block that writes the table of a group's routes under the api's prefix, as a `static`, and gives a reference to
/// it.
pub(crate) fn table(api: &Api, group: &GroupRef) -> TokenStream {
    let descriptor = group.descriptor();
    let prefix = &api.prefix;
    let table = Ident::new("__QUOLL_ROUTES", Span::mixed_site());

    quote! {
        {
            ::quoll::__private::group::table!(#table, #prefix, #descriptor);

            &#table
        }
    }
}

/// The compile-time checks of the groups of other blocks the api lists, which the macro does not see: each against
/// the api, and against the groups before it and the ones the block writes, after the check that it is a group. A
/// group the block writes that takes a scheme the macro knows by its type alone, one of another block, is checked
/// against the api too: what its arguments and its fixed headers read of that scheme's place.
pub(crate) fn api_checks(api: &Api) -> TokenStream {
    let typed: Vec<&Group> = api
        .groups
        .iter()
        .filter(|group| {
            group
                .routes
                .iter()
                .flat_map(Route::schemes)
                .any(|scheme| scheme.kind == SchemeKind::Foreign)
        })
        .collect();

    if api.listed.is_empty() && typed.is_empty() {
        return TokenStream::new();
    }

    let name = api.name.unraw().to_string();
    let prefix = &api.prefix;
    let api_reach = reach(&api.vis);
    let shown = shown_vis(&api.vis);
    let headers = api_headers(&api.headers);
    let refs = api.refs();
    let written: Vec<TokenStream> = refs[api.listed.len()..]
        .iter()
        .map(|group| {
            let descriptor = group.descriptor();

            quote! { &<#descriptor as ::quoll::__private::group::Group>::INFO }
        })
        .collect();
    let checks = refs[..api.listed.len()].iter().enumerate().map(|(index, group)| {
        let descriptor = group.descriptor();
        let earlier = refs[..index].iter().map(|earlier| {
            let descriptor = earlier.descriptor();

            quote! { &<#descriptor as ::quoll::__private::group::Group>::INFO }
        });
        let earlier = earlier.chain(written.iter().cloned());

        quote_spanned! {group.span()=>
            const _: () = if let ::core::option::Option::Some(error) = ::quoll::__private::group::check(
                &API,
                &[#(#earlier),*],
                &<#descriptor as ::quoll::__private::group::Group>::INFO,
            ) {
                ::core::panic!("{}", error.as_str());
            };
        }
    });
    let typed = typed.into_iter().map(|group| {
        let descriptor = descriptor_name(&group.name);

        // Against no group: the macro has checked the block's groups against each other.
        quote_spanned! {group.name.span()=>
            const _: () = if let ::core::option::Option::Some(error) = ::quoll::__private::group::check(
                &API,
                &[],
                &<#descriptor as ::quoll::__private::group::Group>::INFO,
            ) {
                ::core::panic!("{}", error.as_str());
            };
        }
    });
    let is_group = api.listed.iter().map(is_group);
    let errors = refs[..api.listed.len()]
        .iter()
        .filter_map(|group| takes_error(api, group));

    quote! {
        #(#is_group)*
        #(#errors)*

        const _: () = {
            const API: ::quoll::__private::group::ApiInfo = ::quoll::__private::group::ApiInfo {
                name: #name,
                prefix: #prefix,
                reach: #api_reach,
                vis: #shown,
                headers: #headers,
            };

            #(#checks)*
            #(#typed)*
        };
    }
}

/// For a group of the list of an api with an `#[error(E)]`, a check that each route of the group has the shape of its
/// errors named: a `rest::group!` writes its client's trait before any api lists it, and cannot take the api's. An api
/// whose shape is quoll's `rest::Problem`, told by its type, needs none: a route with none has problems.
fn takes_error(api: &Api, group: &GroupRef) -> Option<TokenStream> {
    let error = api.error.as_ref()?;
    let descriptor = group.descriptor();
    let shown = quote! { #error }.to_string().replace(' ', "");

    Some(quote_spanned! {group.span()=>
        const _: () = {
            // Unused where the shape is quoll's `Problem`, whose constant is its type's own.
            #[allow(unused_imports)]
            use ::quoll::__private::group::OtherShape as _;

            if let ::core::option::Option::Some(error) = ::quoll::__private::group::check_error(
                &<#descriptor as ::quoll::__private::group::Group>::INFO,
                ::quoll::__private::group::Shape::<#error>::IS_PROBLEM,
                #shown,
            ) {
                ::core::panic!("{}", error.as_str());
            }
        };
    })
}

/// For each group of the list, a check that its path names a group: its descriptor, named as its path names it, from
/// where the api is, a block or a module, implements `Group`, whose message says what a group is.
fn is_group(path: &syn::Path) -> TokenStream {
    let group = GroupRef {
        name: Api::listed_name(path),
        path: Some(path),
    };
    let descriptor = group.descriptor();

    quote_spanned! {group.span()=>
        const _: () = ::quoll::__private::group::is_group::<#descriptor>();
    }
}

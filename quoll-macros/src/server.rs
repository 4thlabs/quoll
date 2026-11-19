//! What the server side of a contract becomes: a trait per group, `ApiServices` and `Api::router`.
//!
//! All of it goes through `::quoll::__private::server!`, which writes it out only when `quoll` has the feature
//! `server`. The router's code reads each argument through a helper of `quoll`, and calls the group's service.

use proc_macro2::{Span, TokenStream};
use quote::{format_ident, quote, quote_spanned};
use syn::{Ident, ext::IdentExt, spanned::Spanned};

use crate::doc_attrs;
use crate::model::{Api, Group, Method, Param, ParamKind, Reply, Route};
use crate::path::Segment;

/// The server's code of the api, behind the feature `server` of `quoll`.
pub(crate) fn expand(api: &Api) -> TokenStream {
    let traits = api.groups.iter().map(|group| group_trait(api, group));
    let services = services_struct(api);
    let router = router_fn(api);

    quote! {
        ::quoll::__private::server! {
            #(#traits)*
            #services
            #router
        }
    }
}

/// `NotesServer` for the group `Notes`.
fn trait_name(group: &Group) -> Ident {
    format_ident!("{}Server", group.name.unraw(), span = group.name.span())
}

/// `ApiServices` for the api `Api`.
fn services_name(api: &Api) -> Ident {
    format_ident!("{}Services", api.name.unraw(), span = api.name.span())
}

fn group_trait(api: &Api, group: &Group) -> TokenStream {
    let vis = &group.vis;
    let name = trait_name(group);
    let fallback = format!(" The server of the group `{}`.", group.name.unraw());
    let usage = format!(
        " Implemented by the group's service, which `{}` holds for `{}::router`.",
        services_name(api),
        api.name.unraw(),
    );
    let mut lines = if group.docs.0.is_empty() {
        vec![fallback]
    } else {
        group.docs.0.clone()
    };
    lines.extend([String::new(), usage]);
    let docs = doc_attrs(&lines);
    let methods = group.routes.iter().map(|route| method(api, route));

    quote! {
        #docs
        #vis trait #name: ::core::marker::Send + ::core::marker::Sync + 'static {
            #(#methods)*
        }
    }
}

fn method(api: &Api, route: &Route) -> TokenStream {
    let line = format!(" `{} {}`", route.method.as_str(), shown_path(api, route));
    let mut lines = route.docs.0.clone();
    if !lines.is_empty() {
        lines.push(String::new());
    }
    lines.push(line);
    let docs = doc_attrs(&lines);
    let name = &route.name;
    let args = route
        .params
        .iter()
        .map(|Param { name, ty, .. }| quote! { #name: #ty });
    let output = match &route.reply {
        Reply::None => quote! { () },
        Reply::Json(ty) => quote! { #ty },
        Reply::Created(ty) => quote! { ::quoll::rest::Created<#ty> },
    };

    quote_spanned! {route.name.span()=>
        #docs
        fn #name(&self, #(#args),*)
            -> impl ::core::future::Future<Output = ::quoll::rest::Result<#output>> + ::core::marker::Send;
    }
}

/// The path with its query, as the docs show it: `/api/notes?page={page}`.
fn shown_path(api: &Api, route: &Route) -> String {
    let mut path = route.full_path(&api.prefix);
    let query = route.params.iter().filter_map(|param| match &param.kind {
        ParamKind::Query { key } => Some(format!("{key}={{{}}}", param.name.unraw())),
        ParamKind::Path | ParamKind::Body => None,
    });

    for (index, pair) in query.enumerate() {
        path.push(if index == 0 { '?' } else { '&' });
        path.push_str(&pair);
    }

    path
}

fn services_struct(api: &Api) -> TokenStream {
    let vis = &api.vis;
    let name = services_name(api);
    let docs = format!(
        " The services of the api `{}`, a field per group, which `{}::router` serves.",
        api.name.unraw(),
        api.name.unraw(),
    );
    let generics = api.groups.iter().map(|group| &group.name);
    let fields = api.groups.iter().map(|group| {
        let field = group.field();
        let ty = &group.name;
        let docs = format!(
            " The service of the group `{}`, implementing `{}`.",
            group.name.unraw(),
            trait_name(group),
        );

        quote! {
            #[doc = #docs]
            #vis #field: #ty,
        }
    });

    quote! {
        #[doc = #docs]
        #[derive(::core::fmt::Debug, ::core::clone::Clone)]
        #vis struct #name<#(#generics),*> {
            #(#fields)*
        }
    }
}

fn router_fn(api: &Api) -> TokenStream {
    let Api {
        vis, name, prefix, ..
    } = api;
    let services_type = services_name(api);
    let docs = if prefix.is_empty() {
        " An axum router serving every route of the api, each by the service of its group."
            .to_owned()
    } else {
        format!(
            " An axum router serving every route of the api under `{prefix}`, each by the service of its group."
        )
    };
    let services = Ident::new("services", Span::mixed_site());
    let router = Ident::new("router", Span::mixed_site());
    let bounds = api.groups.iter().map(|group| {
        let name = trait_name(group);
        quote! { impl #name }
    });
    let locals: Vec<Ident> = api
        .groups
        .iter()
        .map(|group| {
            Ident::new(
                &format!("service_{}", group.field().unraw()),
                Span::mixed_site(),
            )
        })
        .collect();
    let fields = api.groups.iter().map(Group::field);
    let groups = api.groups.iter().zip(&locals).map(|(group, local)| {
        let routes = group.routes.iter().map(|route| route_call(api, route));

        quote! {
            let #router = ::quoll::__private::server::Group::new(#router, #local)
                #(#routes)*
                .finish();
        }
    });

    quote! {
        impl #name {
            #[doc = #docs]
            #vis fn router(#services: #services_type<#(#bounds),*>) -> ::quoll::__private::axum::Router {
                let #services_type { #(#fields: #locals),* } = #services;
                let #router = ::quoll::__private::axum::Router::new();
                #(#groups)*

                #router
            }
        }
    }
}

/// `.route(method, path, handler)`: the handler reads the path, the query and the body, calls the service and answers.
fn route_call(api: &Api, route: &Route) -> TokenStream {
    let service = Ident::new("service", Span::mixed_site());
    let request = Ident::new("request", Span::mixed_site());
    let query = Ident::new("query", Span::mixed_site());

    let method = method_filter(route.method);
    let path = route.full_path(&api.prefix);
    let mut reads = Vec::new();

    let path_params: Vec<&Param> = route
        .path
        .iter()
        .filter_map(|segment| match segment {
            Segment::Param(name) => route.params.iter().find(|param| param.name.unraw() == name),
            Segment::Literal(_) => None,
        })
        .collect();
    if let Some(first) = path_params.first() {
        let names = path_params.iter().map(|param| &param.name);
        let types = path_params.iter().map(|param| &param.ty);
        reads.push(quote_spanned! {first.ty.span()=>
            let (#(#names,)*) = #request.path::<(#(#types,)*)>().await?;
        });
    }

    let query_params: Vec<_> = route
        .params
        .iter()
        .filter_map(|param| match &param.kind {
            ParamKind::Query { key } => Some((param, key)),
            ParamKind::Path | ParamKind::Body => None,
        })
        .collect();
    if !query_params.is_empty() {
        reads.push(quote! { let #query = #request.query(); });
    }
    for (param, key) in query_params {
        let (name, ty) = (&param.name, &param.ty);
        reads.push(quote_spanned! {ty.span()=>
            let #name = #query.get::<#ty>(#key)?;
        });
    }

    if let Some(body) = route.body() {
        let (name, ty) = (&body.name, &body.ty);
        reads.push(quote_spanned! {ty.span()=>
            let #name = #request.json::<#ty>().await?;
        });
    }

    let function = &route.name;
    let args = route.params.iter().map(|param| &param.name);
    let call = quote! { #service.#function(#(#args),*).await };
    let answer = match &route.reply {
        Reply::None => quote_spanned! {route.span=> ::quoll::__private::server::no_content(#call) },
        Reply::Json(ty) => quote_spanned! {ty.span()=> ::quoll::__private::server::ok(#call) },
        Reply::Created(ty) => {
            quote_spanned! {ty.span()=> ::quoll::__private::server::created(#call) }
        }
    };

    let request_pattern = if route.params.is_empty() {
        quote! { _ }
    } else if path_params.is_empty() {
        quote! { #request }
    } else {
        quote! { mut #request }
    };

    quote! {
        .route(#method, #path, |#service, #request_pattern| async move {
            #(#reads)*
            #answer
        })
    }
}

fn method_filter(method: Method) -> TokenStream {
    let name = Ident::new(method.as_str(), Span::call_site());

    quote! { ::quoll::__private::server::MethodFilter::#name }
}

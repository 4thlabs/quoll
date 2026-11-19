//! What the server side of a contract becomes: a trait per group, `ApiServices` and `Api::router`.
//!
//! All of it goes through `::quoll::__private::server!`, which writes it out only when `quoll` has the feature
//! `server`. The router's code reads each argument through a helper of `quoll`, and calls the group's service.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::{Ident, ext::IdentExt, spanned::Spanned};

use crate::doc_attrs;
use crate::model::{Api, Group, Method, Param, ParamKind, Reply, Route, is_vec};

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

fn group_trait(api: &Api, group: &Group) -> TokenStream {
    let vis = &group.vis;
    let name = group.server_trait();
    let fallback = format!(" The server of the group `{}`.", group.name.unraw());
    let usage = format!(
        " Implemented by the group's service, which `{}` holds for `{}::router`.",
        api.services(),
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
    let name = api.services();
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
            group.server_trait(),
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
    let services_type = api.services();
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
        let name = group.server_trait();
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

    let method = method_filter(route.method);
    let path = route.full_path(&api.prefix);
    let mut reads = Vec::new();

    let path_params: Vec<&Param> = route.path_params().collect();
    if !path_params.is_empty() {
        // Read by name, not by position: a router nested under `/tenants/{tenant}` gives `tenant` too.
        let path_type = Ident::new("__QuollPath", Span::mixed_site());
        let names = path_params.iter().map(|param| &param.name);
        let fields = path_params.iter().map(|param| {
            let (name, ty) = (&param.name, &param.ty);
            let key = name.unraw().to_string();

            quote_spanned! {ty.span()=>
                #[serde(rename = #key)]
                #name: #ty,
            }
        });

        reads.push(quote! {
            #[derive(::quoll::__private::serde::Deserialize)]
            #[serde(crate = "::quoll::__private::serde")]
            struct #path_type {
                #(#fields)*
            }
            let #path_type { #(#names),* } = #request.path::<#path_type>().await?;
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
        // An item, not a local: its name is spelled so that it hides none of the crate's types.
        let query_type = Ident::new("__QuollQuery", Span::mixed_site());
        let names = query_params.iter().map(|(param, _)| &param.name);
        let fields = query_params.iter().map(|(param, key)| {
            let (name, ty) = (&param.name, &param.ty);
            let default = is_vec(ty).then(|| quote! { , default });

            quote_spanned! {ty.span()=>
                #[serde(rename = #key #default)]
                #name: #ty,
            }
        });

        reads.push(quote! {
            #[derive(::quoll::__private::serde::Deserialize)]
            #[serde(crate = "::quoll::__private::serde")]
            struct #query_type {
                #(#fields)*
            }
            let #query_type { #(#names),* } = #request.query::<#query_type>()?;
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

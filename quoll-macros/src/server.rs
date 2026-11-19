//! What the server side of a contract becomes: a trait per group, `ApiServices` and `Api::router`.
//!
//! All of it goes through `::quoll::__private::server!`, which writes it out only when `quoll` has the feature
//! `server`. The router's code authenticates the request and reads each argument through a helper of `quoll`, and
//! calls the group's service.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::{Ident, ext::IdentExt, spanned::Spanned};

use crate::model::{Api, Group, Method, Param, Reply, Route, is_vec};
use crate::{doc_attrs, route_docs, scheme_type};

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

    let schemes = group.schemes();
    if !schemes.is_empty() {
        let names: Vec<String> = schemes
            .iter()
            .map(|scheme| format!("`Authenticate<{}>`", scheme.word()))
            .collect();
        lines.push(format!(
            " It also implements {}, which gives the caller of each route that takes it.",
            names.join(" and "),
        ));
    }

    let docs = doc_attrs(&lines);
    let methods = group.routes.iter().map(|route| method(api, route));
    let authenticate = schemes.into_iter().map(|scheme| {
        let scheme = scheme_type(scheme);

        quote! { + ::quoll::rest::Authenticate<#scheme> }
    });

    quote! {
        #docs
        #vis trait #name: ::core::marker::Send + ::core::marker::Sync + 'static #(#authenticate)* {
            #(#methods)*
        }
    }
}

fn method(api: &Api, route: &Route) -> TokenStream {
    let docs = route_docs(api, route);
    let name = &route.name;
    let args = route
        .params
        .iter()
        .map(|Param { name, ty, .. }| quote! { #name: #ty });
    let output = match route.reply.served() {
        None => quote! { () },
        Some(ty) => quote! { #ty },
    };
    let caller = route.auth.map(|scheme| {
        let scheme = scheme_type(scheme);
        // Its own hygiene, so that a route's argument may be named `caller` too.
        let caller = Ident::new("caller", Span::mixed_site());

        quote! { #caller: &<Self as ::quoll::rest::Authenticate<#scheme>>::Caller, }
    });

    quote_spanned! {route.name.span()=>
        #docs
        fn #name(&self, #caller #(#args),*)
            -> impl ::core::future::Future<Output = ::quoll::rest::Result<#output>> + ::core::marker::Send;
    }
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

/// `.route::<Schemes, _, _>(method, path, handler)`: the handler authenticates the request, reads the path, the query
/// and the body, calls the service and answers; a 401 carries the challenges of `Schemes`.
fn route_call(api: &Api, route: &Route) -> TokenStream {
    let service = Ident::new("service", Span::mixed_site());
    let request = Ident::new("request", Span::mixed_site());
    let caller = Ident::new("caller", Span::mixed_site());

    let method = method_filter(route.method);
    let path = route.full_path(&api.prefix);
    let challenges = route.auth.map_or_else(|| quote! { () }, scheme_type);
    let mut reads = Vec::new();

    // Before anything else is read: a request that is not authenticated learns nothing of the route's arguments, and
    // its body is never read.
    let caller_arg = route.auth.map(|scheme| {
        let scheme = scheme_type(scheme);

        reads.push(quote! {
            let #caller = #request.authenticate::<#scheme, _>(&*#service).await?;
        });

        quote! { &#caller, }
    });

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

    let query_params: Vec<_> = route.query_params().collect();
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

    if let Some(param) = route.query_struct() {
        let (name, ty) = (&param.name, &param.ty);

        reads.push(quote_spanned! {ty.span()=>
            let #name = #request.query::<#ty>()?;
        });
    }

    for (param, header) in route.headers() {
        let (name, ty) = (&param.name, &param.ty);

        reads.push(quote_spanned! {ty.span()=>
            let #name = #request.header::<#ty>(#header)?;
        });
    }

    if let Some((param, kind)) = route.body() {
        let (name, ty) = (&param.name, &param.ty);
        let read = Ident::new(kind.method, Span::call_site());

        reads.push(quote_spanned! {ty.span()=>
            let #name = #request.#read::<#ty>().await?;
        });
    }

    let function = &route.name;
    let args = route.params.iter().map(|param| &param.name);
    let call = quote! { #service.#function(#caller_arg #(#args),*).await };
    let answer = match &route.reply {
        Reply::None => quote_spanned! {route.span=> ::quoll::__private::server::no_content(#call) },
        Reply::Some(returned) => {
            let served = &returned.served;
            let answer = Ident::new(returned.kind.answer, Span::call_site());

            quote_spanned! {served.span()=> ::quoll::__private::server::#answer(#call) }
        }
    };

    let request_pattern = if route.params.is_empty() && route.auth.is_none() {
        quote! { _ }
    } else if path_params.is_empty() {
        quote! { #request }
    } else {
        quote! { mut #request }
    };

    quote! {
        .route::<#challenges, _, _>(#method, #path, |#service, #request_pattern| async move {
            #(#reads)*
            #answer
        })
    }
}

fn method_filter(method: Method) -> TokenStream {
    let name = Ident::new(method.as_str(), Span::call_site());

    quote! { ::quoll::__private::server::MethodFilter::#name }
}

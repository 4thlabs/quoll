//! What the server side of a contract becomes: a trait per group, with the function of its descriptor that serves its
//! routes, and `ApiServices` and `Api::router`, which serve the api's groups by their descriptors.
//!
//! All of it goes through `::quoll::__private::server!`, which writes it out only when `quoll` has the feature
//! `server`. The router's code authenticates the request and reads each argument through a helper of `quoll`, and
//! calls the group's service.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::{Ident, ext::IdentExt, spanned::Spanned};

use crate::descriptor::{Cx, GroupRef, descriptor_name, table};
use crate::docs::{doc_attrs, route_docs};
use crate::fields::route_fields;
use crate::model::{Api, Bounds, Param, Route, is_vec};
use crate::scheme::{scheme_type, schemes_type};

/// The server's code of a group, behind the feature `server` of `quoll`: its trait, and the function of its
/// descriptor that serves its routes.
pub(crate) fn group(cx: &Cx) -> TokenStream {
    let group_trait = group_trait(cx);
    let serve = serve_fn(cx);

    quote! {
        ::quoll::__private::server! {
            #group_trait
            #serve
        }
    }
}

/// The server's code of the api, behind the feature `server` of `quoll`: `ApiServices` and `Api::router`.
pub(crate) fn expand(api: &Api) -> TokenStream {
    let services = services_struct(api);
    let router = router_fn(api);

    quote! {
        ::quoll::__private::server! {
            #services
            #router
        }
    }
}

fn group_trait(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let vis = &group.vis;
    let name = group.server_trait();
    let fallback = format!(" The server of the group `{}`.", group.name.unraw());
    let usage = match cx.api {
        Some(api) => format!(
            " Implemented by the group's service, which `{}` holds for `{}::router`.",
            api.services(),
            api.name.unraw(),
        ),
        None => " Implemented by the group's service, which the services of each api that lists the group hold for its router.".to_owned(),
    };
    let mut lines = if group.docs.0.is_empty() {
        vec![fallback]
    } else {
        group.docs.0.clone()
    };
    lines.extend([String::new(), usage]);

    let bounds = group.bounds();
    if !bounds.0.is_empty() {
        let names: Vec<String> = bounds
            .0
            .iter()
            .map(|bound| format!("`Authenticate<{}>`", bound.scheme.word()))
            .collect();
        lines.push(format!(
            " It also implements {}, which gives the caller of each route that takes it.",
            names.join(" and "),
        ));
    }

    let docs = doc_attrs(&lines);
    let methods = group
        .routes
        .iter()
        .map(|route| method(cx.prefix(), &bounds, route));
    let authenticate = bounds.0.iter().map(|bound| {
        let scheme = scheme_type(bound.scheme);
        let span = bound.scheme.span();
        let caller = match (bound.same_as, bound.scoped) {
            (Some(first), _) => {
                let first = scheme_type(first);

                quote_spanned! {span=> , Caller = <Self as ::quoll::Authenticate<#first>>::Caller }
            }
            (None, true) => quote_spanned! {span=> , Caller: ::quoll::Scoped },
            (None, false) => quote! {},
        };

        // Spanned on the scheme in its `#[auth(...)]`, where an error on the bound points.
        quote_spanned! {span=> + ::quoll::Authenticate<#scheme #caller> }
    });

    quote! {
        #docs
        #vis trait #name: ::core::marker::Send + ::core::marker::Sync + 'static #(#authenticate)* {
            #(#methods)*
        }
    }
}

fn method(prefix: Option<&str>, bounds: &Bounds, route: &Route) -> TokenStream {
    let docs = route_docs(prefix, route);
    let name = &route.name;
    let args = route
        .params
        .iter()
        .map(|Param { name, ty, .. }| quote! { #name: #ty });
    // The value a handler returns in its response: the `T` of a `Created<T>` or an `Accepted<T>`, whose status the
    // contract says, as the client returns it.
    let output = match route.reply.as_deref() {
        None => quote! { () },
        Some(returned) => {
            let ty = &returned.value;

            quote! { #ty }
        }
    };
    let caller = bounds.caller_of(route).map(|scheme| {
        let span = scheme.span();
        let scheme = scheme_type(scheme);
        // Its own hygiene, so that a route's argument may be named `caller` too.
        let caller = Ident::new("caller", Span::mixed_site().located_at(span));
        let ty = quote_spanned! {span=> <Self as ::quoll::Authenticate<#scheme>>::Caller };
        let optional = route.auth.as_ref().is_some_and(|auth| auth.optional);

        // Spanned on the scheme, as a service that does not authenticate it is the mistake an error names then.
        if optional {
            quote_spanned! {span=> #caller: ::core::option::Option<&#ty>, }
        } else {
            quote_spanned! {span=> #caller: &#ty, }
        }
    });

    quote_spanned! {route.name.span()=>
        #docs
        fn #name(&self, #caller #(#args),*)
            -> impl ::core::future::Future<Output = ::quoll::Result<::quoll::Response<#output>>> + ::core::marker::Send;
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
    let refs = api.refs();
    let generics = refs.iter().map(|group| group.name);
    let fields = refs.iter().map(|group| {
        let field = group.field();
        let ty = group.name;
        let docs = format!(
            " The service of the group `{}`, implementing `{}Server`.",
            group.name.unraw(),
            group.name.unraw(),
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
            " An axum router serving every route of the api under `{prefix}`, each by the service of its group, and a problem 404 for any other path under it."
        )
    };
    let services = Ident::new("services", Span::mixed_site());
    let router = Ident::new("router", Span::mixed_site());
    let refs = api.refs();
    let bounds = refs.iter().map(|group| {
        let name = group.server_trait();
        quote! { impl #name }
    });
    let locals: Vec<Ident> = refs
        .iter()
        .map(|group| {
            Ident::new(
                &format!("service_{}", group.field().unraw()),
                Span::mixed_site(),
            )
        })
        .collect();
    let fields = refs.iter().map(GroupRef::field);
    let groups = refs.iter().zip(&locals).map(|(group, local)| {
        let descriptor = group.descriptor();
        let table = table(api, group);

        quote_spanned! {group.span()=>
            let #router = #descriptor::serve(#router, #local, #table);
        }
    });

    quote! {
        impl #name {
            #[doc = #docs]
            #vis fn router(#services: #services_type<#(#bounds),*>) -> ::quoll::__private::axum::Router {
                let #services_type { #(#fields: #locals),* } = #services;
                let #router = ::quoll::__private::server::Routes::under(#prefix);
                #(#groups)*

                #router.finish()
            }
        }
    }
}

/// The function of a group's descriptor that serves its routes, by the service given, on a router: each route takes
/// its `quoll::Route` from the table of the api, under its prefix.
fn serve_fn(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let descriptor = descriptor_name(&group.name);
    let server_trait = group.server_trait();
    let count = group.routes.len();
    let router = Ident::new("router", Span::mixed_site());
    let service = Ident::new("service", Span::mixed_site());
    let routes_table = Ident::new("routes", Span::mixed_site());
    let routes = group
        .routes
        .iter()
        .enumerate()
        .map(|(index, route)| route_call(&routes_table, index, route));

    quote! {
        impl #descriptor {
            /// Adds the group's routes, served by `service`, to `router`, each with its route of `routes`, the table
            /// of the api that lists the group.
            pub fn serve(
                #router: ::quoll::__private::server::Routes,
                #service: impl #server_trait,
                #routes_table: &'static [::quoll::Route; #count],
            ) -> ::quoll::__private::server::Routes {
                ::quoll::__private::server::Group::new(#router, #service)
                    #(#routes)*
                    .finish()
            }
        }
    }
}

/// `.route::<Schemes, _, _>(&ROUTE, handler)`: the handler authenticates the request, reads the path, the query and
/// the body, calls the service and answers; a 401 carries the challenges of `Schemes`, and every answer the route.
fn route_call(routes: &Ident, index: usize, route: &Route) -> TokenStream {
    let service = Ident::new("service", Span::mixed_site());
    let request = Ident::new("request", Span::mixed_site());
    let caller = Ident::new("caller", Span::mixed_site());

    let challenges = schemes_type(route.auth.as_ref().map_or(&[], |auth| &auth.schemes));
    let mut reads = Vec::new();

    // Before anything else is read: a request that is not authenticated learns nothing of the route's arguments, and
    // its body is never read.
    let caller_arg = route.auth.as_ref().map(|auth| {
        if auth.optional {
            reads.push(quote! {
                let #caller = #request.authenticate_optional::<#challenges, _>(&*#service).await?;
            });

            return quote! { #caller.as_ref(), };
        }

        reads.push(quote! {
            let #caller = #request.authenticate::<#challenges, _>(&*#service).await?;
        });

        if !auth.scopes.is_empty() {
            let scopes = &auth.scopes;

            reads.push(quote! {
                ::quoll::__private::server::authorize(&#caller, &[#(#scopes),*])?;
            });
        }

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
            let join = param.join().map(|join| {
                let with = join.deserialize_with();

                quote! { , deserialize_with = #with }
            });

            quote_spanned! {ty.span()=>
                #[serde(rename = #key #default #join)]
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
        // Spanned on the type, as a type that is not `Deserialize` is the mistake an error names then.
        let read = Ident::new(kind.method, ty.span());
        // A body that may hold fixed fields is read with the route's, which may be none.
        let fields = kind.format.map(|_| route_fields(route));

        reads.push(quote_spanned! {ty.span()=>
            let #name = #request.#read::<#ty>(#fields).await?;
        });
    }

    let function = &route.name;
    let args = route.params.iter().map(|param| &param.name);
    let call = quote! { #service.#function(#caller_arg #(#args),*).await };
    let answer = match route.reply.as_deref() {
        None => quote_spanned! {route.span=> ::quoll::__private::server::no_content(#call) },
        Some(returned) => {
            let served = &returned.served;
            // Spanned on the type, as a type that is not `Serialize` is the mistake an error names then.
            let answer = Ident::new(returned.kind().answer, served.span());
            // Its own hygiene, located at the type, as the whole call of the helper is.
            let result = Ident::new("result", Span::mixed_site().located_at(served.span()));

            // The type named, and the call made of the type's tokens alone, so that a bound the type does not meet,
            // on it or on its items, points at it rather than at the whole contract.
            quote_spanned! {served.span()=>
                let #result = #call;
                ::quoll::__private::server::#answer::<#served>(#result)
            }
        }
    };

    let request_pattern = if route.params.is_empty() && route.auth.is_none() {
        quote! { _ }
    } else if path_params.is_empty() {
        quote! { #request }
    } else {
        quote! { mut #request }
    };

    let handler = quote! {
        |#service, #request_pattern| async move {
            #(#reads)*
            #answer
        }
    };

    // A route with fixed fields is served beside the ones of its method and path, by the fields its body holds.
    let format = route
        .body()
        .and_then(|(_, kind)| kind.format)
        .filter(|_| !route.fields.is_empty());
    let Some(format) = format else {
        return quote! {
            .route::<#challenges, _, _>(&#routes[#index], #handler)
        };
    };

    let fields = route_fields(route);
    let format = Ident::new(format, Span::call_site());

    quote! {
        .route_with::<#challenges, _, _>(
            &#routes[#index],
            #fields,
            ::quoll::__private::server::Format::#format,
            #handler,
        )
    }
}

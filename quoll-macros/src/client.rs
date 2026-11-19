//! What the client side of a contract becomes: a trait per group, its client, `ApiClient`, `Api::client` and
//! `Api::client_with`.
//!
//! All of it goes through `::quoll::__private::client!`, which writes it out only when `quoll` has the feature
//! `client`. Each method writes the route's path, query and body through a helper of `quoll`, which sends the request
//! and reads its answer.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::{Ident, ext::IdentExt, spanned::Spanned};

use crate::model::{Api, Group, Param, Route};
use crate::path::Segment;
use crate::scheme::scheme_type;
use crate::{doc_attrs, route_docs};

/// The client's code of the api, behind the feature `client` of `quoll`.
pub(crate) fn expand(api: &Api) -> TokenStream {
    let traits = api.groups.iter().map(|group| group_trait(api, group));
    let clients = api.groups.iter().map(|group| group_client(api, group));
    let api_client = api_client(api);

    quote! {
        ::quoll::__private::client! {
            #(#traits)*
            #(#clients)*
            #api_client
        }
    }
}

fn group_trait(api: &Api, group: &Group) -> TokenStream {
    let vis = &group.vis;
    let name = &group.name;
    let fallback = format!(" The client of the group `{}`.", group.name.unraw());
    let usage = format!(
        " Implemented by `{}`, which `{}::{}` gives, and by a fake in a test.",
        group.client(),
        api.client(),
        group.field().unraw(),
    );
    let mut lines = if group.docs.0.is_empty() {
        vec![fallback]
    } else {
        group.docs.0.clone()
    };
    lines.extend([String::new(), usage]);
    let docs = doc_attrs(&lines);
    let methods = group.routes.iter().map(|route| {
        let docs = route_docs(api, route);
        let signature = signature(route);

        quote! {
            #docs
            #signature;
        }
    });

    quote! {
        #docs
        #vis trait #name: ::core::marker::Send + ::core::marker::Sync {
            #(#methods)*
        }
    }
}

/// `fn get(&self, id: i64) -> impl Future<Output = Result<Note>> + Send`: a `Created<T>` or an `Accepted<T>` gives
/// its `T`, as the 201 or the 202 is only the server's answer.
fn signature(route: &Route) -> TokenStream {
    let name = &route.name;
    let args = route
        .params
        .iter()
        .map(|Param { name, ty, .. }| quote! { #name: #ty });
    let output = match route.reply.as_deref() {
        None => quote! { () },
        Some(returned) => {
            let ty = &returned.value;

            quote! { #ty }
        }
    };

    quote_spanned! {route.name.span()=>
        fn #name(&self, #(#args),*)
            -> impl ::core::future::Future<Output = ::quoll::rest::Result<#output>> + ::core::marker::Send
    }
}

fn group_client(api: &Api, group: &Group) -> TokenStream {
    let vis = &group.vis;
    let name = &group.name;
    let client = group.client();
    let docs = format!(
        " The client of the group `{}`, which `{}::{}` gives: it implements `{}`.",
        group.name.unraw(),
        api.client(),
        group.field().unraw(),
        group.name.unraw(),
    );
    let methods = group.routes.iter().map(|route| method(api, group, route));

    quote! {
        #[doc = #docs]
        #[derive(::core::fmt::Debug, ::core::clone::Clone)]
        #vis struct #client {
            client: ::quoll::__private::client::Client,
        }

        impl #name for #client {
            #(#methods)*
        }
    }
}

/// A route's method on the group's client: the route, then its path, query and body written from the arguments.
fn method(api: &Api, group: &Group, route: &Route) -> TokenStream {
    let signature = signature(route);
    let route_static = Ident::new("__QUOLL_ROUTE", Span::mixed_site());
    let operation_id = route.operation_id(group);
    let group_name = group.name.unraw().to_string();
    let method = Ident::new(route.method.as_str(), Span::call_site());
    let path = route.full_path(&api.prefix);
    // The name and the type of each scheme come from the scheme itself, which writes its name once.
    let schemes: Vec<_> = route.schemes().map(scheme_type).collect();
    let names = schemes
        .iter()
        .map(|scheme| quote! { <#scheme as ::quoll::rest::Scheme>::NAME });
    let types = schemes
        .iter()
        .map(|scheme| quote! { ::core::any::TypeId::of::<#scheme>() });

    let path_calls = path_calls(api, route);
    let (query_type, query_call) = query(route).unzip();
    let query_struct = route.query_struct().map(|param| {
        let (name, ty) = (&param.name, &param.ty);

        quote_spanned! {ty.span()=> .query(&#name)? }
    });
    let headers = route.headers().map(|(param, header)| {
        let (name, ty) = (&param.name, &param.ty);

        // Lowercase, as `from_static` takes it; built at compile time from the name the macro has checked.
        let header = header.to_ascii_lowercase();

        quote_spanned! {ty.span()=>
            .header(const { ::quoll::__private::client::HeaderName::from_static(#header) }, &#name)?
        }
    });
    let body = route.body().map(|(param, kind)| {
        let (name, ty) = (&param.name, &param.ty);
        // Spanned on the type, as a type that is not `Serialize` is the mistake an error names then.
        let write = Ident::new(kind.method, ty.span());
        let fallible = kind.fallible.then(|| quote! { ? });

        quote_spanned! {ty.span()=> .#write(#name) #fallible }
    });
    let accept = route.reply.as_deref().map(|returned| {
        let media_type = Ident::new(returned.kind().accept, returned.served.span());

        quote! { .accept(::quoll::__private::client::#media_type) }
    });
    let answer = match route.reply.as_deref() {
        None => quote_spanned! {route.span=> .none().await },
        Some(returned) => {
            let value = &returned.value;
            // Spanned on the type, as a type that is not `Deserialize` is the mistake an error names then.
            let read = Ident::new(returned.kind().read, value.span());

            quote_spanned! {value.span()=> .#read::<#value>().await }
        }
    };

    quote! {
        #signature {
            static #route_static: ::quoll::rest::Route = ::quoll::__private::client::route(
                #operation_id,
                #group_name,
                ::quoll::__private::client::Method::#method,
                #path,
                &[#(#names),*],
                &[#(#types),*],
            );

            #query_type

            async move {
                ::quoll::__private::client::Call::new(&self.client, &#route_static)
                    #(#path_calls)*
                    #query_call
                    #query_struct
                    #(#headers)*
                    #body
                    #accept
                    .send()
                    .await?
                    #answer
            }
        }
    }
}

/// The calls that write the path: `.path("/api/notes")` for the text between parameters, `.segment("id", &id)?` for
/// each parameter.
fn path_calls(api: &Api, route: &Route) -> Vec<TokenStream> {
    let mut calls = Vec::new();
    let mut text = api.prefix.clone();
    let mut params = route.path_params();

    for segment in &route.path {
        match segment {
            Segment::Literal(literal) => {
                text.push('/');
                text.push_str(literal);
            }
            Segment::Param(key) => {
                if !text.is_empty() {
                    calls.push(quote! { .path(#text) });
                    text.clear();
                }

                let param = params
                    .next()
                    .expect("the route has an argument for each `{name}` of its path");
                let (name, ty) = (&param.name, &param.ty);
                calls.push(quote_spanned! {ty.span()=> .segment(#key, &#name)? });
            }
        }
    }

    if route.path.is_empty() && text.is_empty() {
        text.push('/');
    }
    if !text.is_empty() {
        calls.push(quote! { .path(#text) });
    }

    calls
}

/// The struct the query is written from, a field per key, and the call that writes it, when the route has a query.
fn query(route: &Route) -> Option<(TokenStream, TokenStream)> {
    let params: Vec<(&Param, &str)> = route.query_params().collect();
    if params.is_empty() {
        return None;
    }

    // An item, not a local: its name is spelled so that it hides none of the crate's types.
    let query_type = Ident::new("__QuollQuery", Span::mixed_site());
    let names = params.iter().map(|(param, _)| &param.name);
    let fields = params.iter().map(|(param, key)| {
        let (name, ty) = (&param.name, &param.ty);

        quote_spanned! {ty.span()=>
            #[serde(rename = #key)]
            #name: #ty,
        }
    });
    let item = quote! {
        #[derive(::quoll::__private::serde::Serialize)]
        #[serde(crate = "::quoll::__private::serde")]
        struct #query_type {
            #(#fields)*
        }
    };
    let call = quote! { .query(&#query_type { #(#names),* })? };

    Some((item, call))
}

/// `ApiClient`, its method per group, and `Api::client` and `Api::client_with`, which build it.
fn api_client(api: &Api) -> TokenStream {
    let Api { vis, name, .. } = api;
    let client = api.client();
    let docs = format!(
        " A client of the api `{}`, which `{}::client` and `{}::client_with` build: a method per group gives the group's client. Its clones share its connections, and cost what cloning an `Arc` does.",
        name.unraw(),
        name.unraw(),
        name.unraw(),
    );
    let groups = api.groups.iter().map(|group| {
        let method = group.field();
        let group_client = group.client();
        let docs = format!(" The client of the group `{}`.", group.name.unraw());

        quote! {
            #[doc = #docs]
            #vis fn #method(&self) -> #group_client {
                #group_client {
                    client: ::core::clone::Clone::clone(&self.client),
                }
            }
        }
    });
    let url = Ident::new("url", Span::mixed_site());
    let http = Ident::new("http", Span::mixed_site());

    quote! {
        #[doc = #docs]
        #[derive(::core::fmt::Debug, ::core::clone::Clone)]
        #vis struct #client {
            client: ::quoll::__private::client::Client,
        }

        impl #client {
            #(#groups)*
        }

        impl #name {
            /// A client of the api served at `url`, as `http://localhost:8080`, with no middleware. Each route's
            /// path is written after the path of `url`.
            ///
            /// # Errors
            ///
            /// When `url` is not an `http` or `https` URL with no query, or reqwest cannot build its client.
            #vis fn client(#url: &str) -> ::quoll::rest::Result<#client> {
                ::quoll::__private::client::Client::new(#url).map(|client| #client { client })
            }

            /// A client of the api served at `url`, as `http://localhost:8080`, sending its requests through
            /// `http` and its middleware. Each route's path is written after the path of `url`.
            ///
            /// # Errors
            ///
            /// When `url` is not an `http` or `https` URL with no query.
            #vis fn client_with(
                #http: ::quoll::__private::reqwest_middleware::ClientWithMiddleware,
                #url: &str,
            ) -> ::quoll::rest::Result<#client> {
                ::quoll::__private::client::Client::with(#http, #url).map(|client| #client { client })
            }
        }
    }
}

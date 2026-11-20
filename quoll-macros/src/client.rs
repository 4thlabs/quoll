//! What the client side of a contract becomes: a trait per group, its client, with the function of its descriptor that
//! builds it, and `ApiClient`, `Api::client` and `Api::client_with`, which give each group's client by its descriptor.
//!
//! All of it goes through `::quoll::__private::client!`, which writes it out only when `quoll` has the feature
//! `client`. Each method writes the route's path, query and body through a helper of `quoll`, which sends the request
//! and reads its answer.

use proc_macro2::{Span, TokenStream};
use quote::{quote, quote_spanned};
use syn::{Ident, ext::IdentExt, spanned::Spanned};

use crate::descriptor::{Cx, descriptor_name, table};
use crate::model::{Api, Param, Route};
use crate::path::Segment;
use crate::{doc_attrs, route_docs};

/// The client's code of a group, behind the feature `client` of `quoll`: its trait, its client, and the function of
/// its descriptor that builds the client.
pub(crate) fn group(cx: &Cx) -> TokenStream {
    let group_trait = group_trait(cx);
    let group_client = group_client(cx);

    quote! {
        ::quoll::__private::client! {
            #group_trait
            #group_client
        }
    }
}

/// The client's code of the api, behind the feature `client` of `quoll`.
pub(crate) fn expand(api: &Api) -> TokenStream {
    let api_client = api_client(api);

    quote! {
        ::quoll::__private::client! {
            #api_client
        }
    }
}

/// Where the client of a group is given: `` `ApiClient::notes` `` in the api's block, any api's client otherwise.
fn given_by(cx: &Cx) -> String {
    match cx.api {
        Some(api) => format!("`{}::{}`", api.client(), cx.group.field().unraw()),
        None => "the client of each api that lists the group".to_owned(),
    }
}

fn group_trait(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let vis = &group.vis;
    let name = &group.name;
    let fallback = format!(" The client of the group `{}`.", group.name.unraw());
    let usage = format!(
        " Implemented by `{}`, which {} gives, and by a fake in a test.",
        group.client(),
        given_by(cx),
    );
    let mut lines = if group.docs.0.is_empty() {
        vec![fallback]
    } else {
        group.docs.0.clone()
    };
    lines.extend([String::new(), usage]);
    let docs = doc_attrs(&lines);
    let methods = group.routes.iter().map(|route| {
        let docs = route_docs(cx.prefix(), route);
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

fn group_client(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let vis = &group.vis;
    let name = &group.name;
    let client = group.client();
    let descriptor = descriptor_name(&group.name);
    let count = group.routes.len();
    let docs = format!(
        " The client of the group `{}`, which {} gives: it implements `{}`.",
        group.name.unraw(),
        given_by(cx),
        group.name.unraw(),
    );
    let methods = group
        .routes
        .iter()
        .enumerate()
        .map(|(index, route)| method(index, route));
    let http = Ident::new("client", Span::mixed_site());
    let prefix = Ident::new("prefix", Span::mixed_site());
    let routes = Ident::new("routes", Span::mixed_site());

    quote! {
        #[doc = #docs]
        #[derive(::core::fmt::Debug, ::core::clone::Clone)]
        #vis struct #client {
            client: ::quoll::__private::client::Client,
            prefix: &'static str,
            routes: &'static [::quoll::rest::Route; #count],
        }

        impl #name for #client {
            #(#methods)*
        }

        impl #descriptor {
            /// The group's client, calling through `client` the routes of `routes`, the table of the api that lists
            /// the group, each path under `prefix`, the api's.
            pub fn client(
                #http: ::quoll::__private::client::Client,
                #prefix: &'static str,
                #routes: &'static [::quoll::rest::Route; #count],
            ) -> #client {
                #client {
                    client: #http,
                    prefix: #prefix,
                    routes: #routes,
                }
            }
        }
    }
}

/// A route's method on the group's client: the route, then its path, query and body written from the arguments.
fn method(index: usize, route: &Route) -> TokenStream {
    let signature = signature(route);

    let path_calls = path_calls(route);
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
            #query_type

            async move {
                ::quoll::__private::client::Call::new(&self.client, &self.routes[#index])
                    .path(self.prefix)
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

/// The calls that write the path after the api's prefix: `.path("/notes")` for the text between parameters,
/// `.segment("id", &id)?` for each parameter. The route `"/"` is the prefix alone, or `/` with none, which the call
/// writes itself.
fn path_calls(route: &Route) -> Vec<TokenStream> {
    let mut calls = Vec::new();
    let mut text = String::new();
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

/// `ApiClient`, its method per group, and `Api::client` and `Api::client_with`, which build it, as does
/// `client::Builder::build` through `client::Contract`.
fn api_client(api: &Api) -> TokenStream {
    let Api { vis, name, .. } = api;
    let client = api.client();
    let docs = format!(
        " A client of the api `{}`, which `{}::client` and `{}::client_with` build: a method per group gives the group's client. Its clones share its connections, and cost what cloning an `Arc` does.",
        name.unraw(),
        name.unraw(),
        name.unraw(),
    );
    let groups = api.refs().into_iter().map(|group| {
        let method = group.field();
        let group_client = group.client();
        let descriptor = group.descriptor();
        let table = table(api, &group);
        let prefix = &api.prefix;
        let docs = format!(" The client of the group `{}`.", group.name.unraw());

        let call = quote_spanned! {group.span()=>
            #descriptor::client(::core::clone::Clone::clone(&self.client), #prefix, #table)
        };

        quote! {
            #[doc = #docs]
            #vis fn #method(&self) -> #group_client {
                #call
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

        impl ::quoll::client::Contract for #name {
            type Client = #client;

            fn client_with(
                #http: ::quoll::__private::reqwest_middleware::ClientWithMiddleware,
                #url: &str,
            ) -> ::quoll::rest::Result<#client> {
                #name::client_with(#http, #url)
            }
        }
    }
}

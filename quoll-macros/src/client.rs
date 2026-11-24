//! What the client side of a contract becomes: a trait per group, its client, with the items of its descriptor's
//! `Called` that build it, and `ApiClient`, `Api::client` and `Api::client_with`, which give each group's client: by
//! its name for a group the block writes, by its descriptor for one the api lists.
//!
//! All of it goes through `::quoll::__private::client!`, which writes it out only when `quoll` has the feature
//! `client`. Each method writes the route's path, query and body through a helper of `quoll`, and returns the call,
//! which sends the request and reads its answer once it is awaited.

use proc_macro2::{Span, TokenStream};
use quote::{format_ident, quote, quote_spanned};
use syn::{Ident, Lifetime, Type, ext::IdentExt, spanned::Spanned};

use crate::borrowed;
use crate::descriptor::{Cx, table};
use crate::docs::{doc_attrs, route_docs};
use crate::fields::route_fields;
use crate::headers::{api_headers, route_headers};
use crate::model::{Api, Param, Route, peel};
use crate::path::Segment;

/// The client's code of a group, behind the feature `client` of `quoll`: its trait and its client. The function that
/// builds the client is an item of the descriptor's `Called`, which [`called_items`] writes in the descriptor's impl,
/// outside this gate, as the impl is there whatever the features.
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

/// The shapes of the errors of the group's routes, each once, in the order the routes first name them: the type each
/// `#[error(E)]` the routes take names, or `quoll::Problem`; and for each route, the index of its own among them.
fn shapes(cx: &Cx) -> (Vec<TokenStream>, Vec<usize>) {
    let mut shapes: Vec<(String, TokenStream)> = Vec::new();
    let mut taken = Vec::new();

    for route in &cx.group.routes {
        let shape = error_type(cx, route);
        let shown = shape.to_string();
        let index = match shapes.iter().position(|(seen, _)| *seen == shown) {
            Some(index) => index,
            None => {
                shapes.push((shown, shape));
                shapes.len() - 1
            }
        };

        taken.push(index);
    }

    (shapes.into_iter().map(|(_, shape)| shape).collect(), taken)
}

/// `__QUOLL_SHAPE_0`, the constant of the group's client that reads the errors of the `index`-th of its [`shapes`].
fn shape_const(index: usize) -> Ident {
    Ident::new(&format!("__QUOLL_SHAPE_{index}"), Span::mixed_site())
}

/// The constants of the group's client that read the errors of each of its shapes, once per shape, spanned on the
/// type an `#[error(E)]` names: a type that is not a shape is one error, there, whatever the routes that take it.
fn shape_consts(shapes: Vec<TokenStream>) -> TokenStream {
    let consts = shapes.into_iter().enumerate().map(|(index, shape)| {
        let name = shape_const(index);
        let of = quote_spanned! {shape.span()=> ::quoll::__private::client::shaped::<#shape>() };

        quote! {
            const #name: ::quoll::__private::client::Shaped<#shape> = #of;
        }
    });

    quote! { #(#consts)* }
}

/// The shape of `route`'s errors, as the client's code names it: its `#[error(E)]`'s type, or `quoll::Problem`.
fn error_type(cx: &Cx, route: &Route) -> TokenStream {
    match cx.error(route) {
        Some(error) => quote! { #error },
        None => quote! { ::quoll::Problem },
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
        let signature = signature(cx, route);

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

/// `fn get(&self, id: i64) -> impl quoll::Call<Note, Problem>`, of the shape of the route's errors: a `Created<T>` or
/// an `Accepted<T>` gives its `T`, as the 201 or the 202 is only the server's answer.
///
/// The arguments are the contract's types as written, `&str` for a `&str`, which the client borrows. The call keeps
/// the default capture: it may borrow its arguments until it is awaited, so that a fake reads them in its
/// `async move`.
fn signature(cx: &Cx, route: &Route) -> TokenStream {
    let name = &route.name;
    let args = route
        .params
        .iter()
        .map(|Param { name, written, .. }| quote! { #name: #written });
    let output = match route.reply.as_deref() {
        None => quote! { () },
        Some(returned) => {
            let ty = &returned.value;

            quote! { #ty }
        }
    };

    let error = error_type(cx, route);

    quote_spanned! {route.name.span()=>
        fn #name(&self, #(#args),*) -> impl ::quoll::Call<#output, #error>
    }
}

fn group_client(cx: &Cx) -> TokenStream {
    let group = cx.group;
    let vis = &group.vis;
    let name = &group.name;
    let client = group.client();
    let docs = format!(
        " The client of the group `{}`, which {} gives: it implements `{}`.",
        group.name.unraw(),
        given_by(cx),
        group.name.unraw(),
    );
    let (shapes, taken) = shapes(cx);
    let methods = group
        .routes
        .iter()
        .zip(taken)
        .enumerate()
        .map(|(index, (route, shape))| method(cx, index, route, &shape_const(shape)));
    let shape_consts = shape_consts(shapes);

    quote! {
        #[doc = #docs]
        #[derive(::core::fmt::Debug, ::core::clone::Clone)]
        #vis struct #client {
            client: ::quoll::__private::client::Client,
            prefix: &'static str,
            routes: &'static [::quoll::Route],
            headers: &'static [::quoll::__private::headers::Fixed],
        }

        impl #client {
            #shape_consts
        }

        impl #name for #client {
            #(#methods)*
        }
    }
}

/// The items of the descriptor's `quoll::__private::group::Called`, behind the feature `client` of `quoll`: the
/// group's client, and the function that builds it, which the api's client calls by the descriptor alone.
pub(crate) fn called_items(cx: &Cx) -> TokenStream {
    let client = cx.group.client();
    let count = cx.group.routes.len();
    let http = Ident::new("client", Span::mixed_site());
    let prefix = Ident::new("prefix", Span::mixed_site());
    let routes = Ident::new("routes", Span::mixed_site());
    let headers = Ident::new("headers", Span::mixed_site());

    quote! {
        ::quoll::__private::client! {
            type Client = #client;

            fn group_client(
                #http: ::quoll::__private::client::Client,
                #prefix: &'static str,
                #routes: &'static [::quoll::Route],
                #headers: &'static [::quoll::__private::headers::Fixed],
            ) -> #client {
                // The api's table has a route per route of the group, as `table!` builds it from the descriptor: the
                // slice keeps one signature of `Called` for every group, whose length the type no longer says.
                ::core::debug_assert_eq!(#routes.len(), #count, "a route of the table per route of the group");

                #client {
                    client: #http,
                    prefix: #prefix,
                    routes: #routes,
                    headers: #headers,
                }
            }
        }
    }
}

/// A route's method on the group's client: the route, then its path, query and body written from the arguments.
fn method(cx: &Cx, index: usize, route: &Route, shape: &Ident) -> TokenStream {
    let signature = signature(cx, route);
    let error = error_type(cx, route);

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
        // A body the contract writes `&T` is written from the borrow.
        let method = match peel(&param.written) {
            Type::Reference(_) => kind.borrowed,
            _ => kind.method,
        };
        // Spanned on the type, as a type that is not `Serialize` is the mistake an error names then.
        let write = Ident::new(method, ty.span());
        let fallible = kind.fallible.then(|| quote! { ? });
        // A body that may hold fixed fields is written with the route's, which may be none.
        let fields = kind.format.map(|_| {
            let fields = route_fields(route);

            quote! { , #fields }
        });

        quote_spanned! {ty.span()=> .#write(#name #fields) #fallible }
    });
    let fixed = route_headers(cx.group, route);
    let accept = route.reply.as_deref().map(|returned| {
        let media_type = Ident::new(returned.kind().accept, returned.served.span());

        quote! { .accept(::quoll::__private::client::#media_type) }
    });
    let answer = Ident::new("answer", Span::mixed_site());
    let read = match route.reply.as_deref() {
        None => quote_spanned! {route.span=> #answer.none() },
        Some(returned) => {
            let value = &returned.value;
            // Spanned on the type, as a type that is not `Deserialize` is the mistake an error names then.
            let read = Ident::new(returned.kind().read, value.span());
            // A JSON result reads a 204 as `null` when its type says it may: an `Option<T>` or `()`.
            let nullable = (returned.kind().read == "json").then(|| {
                quote! {
                    {
                        #[allow(unused_imports)]
                        use ::quoll::__private::known::NotNullable as _;

                        <::quoll::__private::known::Nullable<#value>>::IS
                    }
                }
            });

            quote_spanned! {value.span()=> #answer.#read::<#value>(#nullable) }
        }
    };
    let request = Ident::new("request", Span::mixed_site());

    quote! {
        #signature {
            #query_type

            let #request = ::quoll::__private::client::Request::write(
                &self.client,
                &self.routes[#index],
                move |#request| ::core::result::Result::Ok(
                    #request
                        .path(self.prefix)
                        #(#path_calls)*
                        #query_call
                        #query_struct
                        #(#headers)*
                        #body
                        .fixed(self.headers, &const { #fixed })
                        #accept
                ),
            );

            ::quoll::__private::client::Pending::<_, #error>::new(#request, |#answer| #read, Self::#shape)
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

/// The struct the query is written from, a field per key but the joined ones, and the calls that write it, the
/// joined keys after the others, when the route has a query.
fn query(route: &Route) -> Option<(TokenStream, TokenStream)> {
    let (joined, params): (Vec<_>, Vec<_>) = route
        .query_params()
        .partition(|(param, _)| param.join().is_some());
    if joined.is_empty() && params.is_empty() {
        return None;
    }

    let joins = joined.iter().map(|(param, key)| {
        let (name, ty) = (&param.name, &param.ty);
        let join = Ident::new(
            param.join().expect("partitioned by its join").variant(),
            Span::call_site(),
        );

        quote_spanned! {ty.span()=>
            .join(#key, ::quoll::__private::join::Join::#join, &#name)?
        }
    });
    if params.is_empty() {
        return Some((TokenStream::new(), quote! { #(#joins)* }));
    }

    // An item, not a local: its name is spelled so that it hides none of the crate's types.
    let query_type = Ident::new("__QuollQuery", Span::mixed_site());
    let names = params.iter().map(|(param, _)| &param.name);
    // The arguments as the client takes them, borrowed ones with the struct's lifetime.
    let lifetime = Lifetime::new("'__quoll", Span::mixed_site());
    let borrows = params
        .iter()
        .any(|(param, _)| borrowed::borrows(&param.written));
    let generics = borrows.then(|| quote! { <#lifetime> });
    let fields = params.iter().map(|(param, key)| {
        let name = &param.name;
        let ty = borrowed::with_lifetime(&param.written, &lifetime);

        quote_spanned! {param.written.span()=>
            #[serde(rename = #key)]
            #name: #ty,
        }
    });
    let item = quote! {
        #[derive(::quoll::__private::serde::Serialize)]
        #[serde(crate = "::quoll::__private::serde")]
        struct #query_type #generics {
            #(#fields)*
        }
    };
    let call = quote! { .query(&#query_type { #(#names),* })? #(#joins)* };

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
        let descriptor = group.descriptor();
        let table = table(api, &group);
        let prefix = &api.prefix;
        let headers = api_headers(&api.headers);
        let group_client = format_ident!("{}Client", group.name.unraw(), span = group.name.span());
        let docs = if group.is_written() {
            format!(" The client of the group `{}`.", group.name.unraw())
        } else {
            format!(
                " The client of the group `{}`, [`{}`].",
                group.name.unraw(),
                group.shown(&group_client.to_string()),
            )
        };

        // A group the block writes has the api's sides, and its client is named. One the api lists is reached by its
        // descriptor alone, never by its path, which a group with no client side does not write: the bound is the one
        // error of such a group, quoll's, on the group in the list, and the docs name its client.
        // The link of its docs, to a group of a private module, stays a link of the api's own crate.
        let (returned, bound, links) = if group.is_written() {
            (quote! { #group_client }, None, None)
        } else {
            (
                quote! { <#descriptor as ::quoll::__private::group::Called>::Client },
                Some(quote_spanned! {group.span()=> where #descriptor: ::quoll::__private::group::Called }),
                Some(quote! { #[allow(rustdoc::private_intra_doc_links)] }),
            )
        };
        let call = quote_spanned! {group.span()=>
            <#descriptor as ::quoll::__private::group::Called>::group_client(
                ::core::clone::Clone::clone(&self.client),
                #prefix,
                #table,
                #headers,
            )
        };

        quote! {
            #[doc = #docs]
            #links
            #vis fn #method(&self) -> #returned #bound {
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
            #vis fn client(#url: &str) -> ::quoll::Result<#client> {
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
            ) -> ::quoll::Result<#client> {
                ::quoll::__private::client::Client::with(#http, #url).map(|client| #client { client })
            }
        }

        impl ::quoll::client::Contract for #name {
            type Client = #client;

            fn client_with(
                #http: ::quoll::__private::reqwest_middleware::ClientWithMiddleware,
                #url: &str,
            ) -> ::quoll::Result<#client> {
                #name::client_with(#http, #url)
            }
        }
    }
}

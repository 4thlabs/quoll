//! The spelling of `rest::api!`: a block of items, read into the model.
//!
//! ```text
//! /// Notes, kept per user.
//! pub api Api = "/api";
//!
//! /// The signed-in user's notes.
//! #[auth(Bearer)]
//! pub group Notes {
//!     /// One note.
//!     GET "/notes/{id}" => get(id: i64) -> Note;
//! }
//! ```

use syn::{
    Attribute, Error, Expr, Ident, Lit, LitStr, Meta, Result, Token, Type, Visibility, braced,
    ext::IdentExt,
    parenthesized,
    parse::{Parse, ParseStream},
    punctuated::Punctuated,
};

use crate::model::{
    Api, ArgDecl, Auth, Docs, ErrorList, Group, Method, Route, RouteDecl, Scheme, Source,
};

/// What the macro is given: one api, with its groups.
#[derive(Debug)]
pub(crate) struct Contract(pub(crate) Api);

impl Parse for Contract {
    fn parse(input: ParseStream) -> Result<Self> {
        let mut errors = ErrorList::default();

        match read_items(input, &mut errors) {
            Ok(api) => errors.finish().map(|()| Self(api)),
            Err(error) => Err(errors.fail(error)),
        }
    }
}

/// Reads every item. A mistake of syntax ends the reading, and the caller adds what was gathered before it; a mistake
/// of meaning is gathered and the reading goes on.
fn read_items(input: ParseStream, errors: &mut ErrorList) -> Result<Api> {
    let mut api: Option<ApiDecl> = None;
    let mut groups = Vec::new();

    while !input.is_empty() {
        let Attrs { docs, auth } = parse_attrs(input)?;
        let vis: Visibility = input.parse()?;
        let keyword = input.call(Ident::parse_any).map_err(|error| {
            Error::new(
                error.span(),
                "expected `api` or `group`: an item of a contract starts with one of them",
            )
        })?;

        if keyword == "api" {
            if let Some((attr, _)) = &auth {
                errors.push(Error::new_spanned(
                    attr,
                    "an api takes no `#[auth(...)]`: put it on a group, or on a route",
                ));
            }

            let decl = ApiDecl::parse(input, docs, vis, &keyword)?;
            match &api {
                Some(first) => errors.push(Error::new(
                    keyword.span(),
                    format!(
                        "the contract has one api, and `{}` is already it: remove this one",
                        first.name
                    ),
                )),
                None => api = Some(decl),
            }
        } else if keyword == "group" {
            let auth = auth.and_then(|(_, auth)| keep(errors, auth));
            groups.push(parse_group(input, docs, vis, auth, errors)?);
        } else {
            return Err(unexpected_item(&keyword));
        }
    }

    let Some(api) = api else {
        return Err(Error::new(
            proc_macro2::Span::call_site(),
            "a contract names its api: write `pub api Api = \"/api\";`",
        ));
    };

    Api::new(api.docs, api.vis, api.name, &api.prefix, groups).map_err(|error| fold(errors, error))
}

/// The value of `result`, or `None` once its error is gathered.
fn keep<T>(errors: &mut ErrorList, result: Result<T>) -> Option<T> {
    result.map_err(|error| errors.push(error)).ok()
}

/// Moves the errors gathered so far into `error`, which they come before.
fn fold(errors: &mut ErrorList, error: Error) -> Error {
    std::mem::take(errors).fail(error)
}

fn unexpected_item(keyword: &Ident) -> Error {
    let hint = if Method::from_name(&keyword.to_string()).is_some() {
        "a route goes inside a group: `pub group Name { ... }`"
    } else {
        "an item of a contract starts with `api` or `group`"
    };

    Error::new(keyword.span(), format!("unexpected `{keyword}`: {hint}"))
}

struct ApiDecl {
    docs: Docs,
    vis: Visibility,
    name: Ident,
    prefix: LitStr,
}

impl ApiDecl {
    /// After the `api` keyword: `Name = "/prefix";`.
    fn parse(input: ParseStream, docs: Docs, vis: Visibility, keyword: &Ident) -> Result<Self> {
        let name: Ident = input.parse().map_err(|error| {
            Error::new(
                error.span(),
                format!(
                    "`{keyword}` takes the name of the api's type: write `api Api = \"/api\";`"
                ),
            )
        })?;
        input.parse::<Token![=]>()?;
        let prefix: LitStr = input.parse().map_err(|error| {
            Error::new(
                error.span(),
                "the api takes its prefix as a string: write `api Api = \"/api\";`",
            )
        })?;
        input.parse::<Token![;]>()?;

        Ok(Self {
            docs,
            vis,
            name,
            prefix,
        })
    }
}

/// After the `group` keyword: `Name { routes }`, whose routes take `auth` unless they say otherwise. A mistake in one
/// route is gathered, the others are still read.
fn parse_group(
    input: ParseStream,
    docs: Docs,
    vis: Visibility,
    auth: Option<Auth>,
    errors: &mut ErrorList,
) -> Result<Group> {
    let name: Ident = input.parse().map_err(|error| {
        Error::new(
            error.span(),
            "`group` takes a name: write `group Notes { ... }`",
        )
    })?;
    let content;
    braced!(content in input);

    let mut routes = Vec::new();
    while !content.is_empty() {
        match parse_route(&content, auth) {
            Ok(Ok(route)) => routes.push(route),
            Ok(Err(error)) => errors.push(error),
            Err(error) => return Err(error),
        }
    }

    Ok(Group {
        docs,
        vis,
        name,
        routes,
    })
}

/// One route, which takes `group_auth` unless it says otherwise. The outer error is a mistake of syntax, after which
/// nothing can be read; the inner one a mistake of the route's meaning, which leaves the next route readable.
fn parse_route(input: ParseStream, group_auth: Option<Auth>) -> Result<Result<Route>> {
    let Attrs { docs, auth } = parse_attrs(input)?;
    let method = parse_method(input)?;
    let path: LitStr = input.parse().map_err(|error| {
        Error::new(error.span(), "a route's path is a string after its method: write `GET \"/notes/{id}\" => get(id: i64);`")
    })?;
    input.parse::<Token![=>]>()?;
    let name: Ident = input.parse().map_err(|error| {
        Error::new(
            error.span(),
            "`=>` is followed by the name of the route's function: write `=> get(id: i64)`",
        )
    })?;

    let content;
    parenthesized!(content in input);
    let args = Punctuated::<Arg, Token![,]>::parse_terminated(&content)?;
    let reply = if input.peek(Token![->]) {
        input.parse::<Token![->]>()?;
        Some(input.parse::<Type>()?)
    } else {
        None
    };
    input.parse::<Token![;]>()?;

    let (auth, auth_error) = match auth.map(|(_, auth)| auth).transpose() {
        Ok(auth) => (auth, None),
        Err(error) => (None, Some(error)),
    };
    let route = Route::resolve(
        RouteDecl {
            docs,
            method,
            path,
            name,
            args: args.into_iter().map(|Arg(arg)| arg).collect(),
            reply,
            auth,
        },
        group_auth,
    );

    let Some(mut error) = auth_error else {
        return Ok(route);
    };

    if let Err(other) = route {
        error.combine(other);
    }

    Ok(Err(error))
}

fn parse_method(input: ParseStream) -> Result<Method> {
    let expected = || {
        let names: Vec<_> = Method::ALL.iter().map(|method| method.as_str()).collect();
        format!("expected a method, one of {}", names.join(", "))
    };
    let ident = input
        .call(Ident::parse_any)
        .map_err(|error| Error::new(error.span(), expected()))?;

    Method::from_name(&ident.to_string()).ok_or_else(|| Error::new(ident.span(), expected()))
}

/// An argument: `name: Type`, `query name: Type` for a query struct, or `header("Name") name: Type`.
struct Arg(ArgDecl);

impl Parse for Arg {
    fn parse(input: ParseStream) -> Result<Self> {
        let expected = "an argument is written `name: Type`, `query name: Type` or `header(\"Name\") name: Type`";
        let first: Ident = input
            .parse()
            .map_err(|error| Error::new(error.span(), expected))?;

        let (source, name) = if first == "query" && input.peek(Ident) {
            (Source::Query(first.span()), input.parse::<Ident>()?)
        } else if first == "header" && input.peek(syn::token::Paren) {
            let content;
            parenthesized!(content in input);

            let header: LitStr = content.parse().map_err(|error| {
                Error::new(
                    error.span(),
                    "a header is named by a string: write `header(\"Last-Event-ID\") last: Option<String>`",
                )
            })?;

            if !content.is_empty() {
                return Err(content.error(
                    "a header takes its name alone: write `header(\"Last-Event-ID\") last: Option<String>`",
                ));
            }

            let name: Ident = input.parse().map_err(|error| {
                Error::new(
                    error.span(),
                    "`header(\"...\")` is followed by the argument's name: write `header(\"Last-Event-ID\") last: Option<String>`",
                )
            })?;

            (Source::Header(header), name)
        } else {
            (Source::Named, first)
        };

        input.parse::<Token![:]>().map_err(|error| {
            Error::new(
                error.span(),
                format!("expected `:` after `{name}`: {expected}"),
            )
        })?;

        Ok(Self(ArgDecl {
            source,
            name,
            ty: input.parse()?,
        }))
    }
}

/// The attributes before an item: its `///` comments, and its `#[auth(...)]` with what it says, or the mistake in it.
struct Attrs {
    docs: Docs,
    auth: Option<(Attribute, Result<Auth>)>,
}

/// The attributes before an item. Any attribute other than a comment or `#[auth(...)]` is refused, which stops the
/// reading; a second `#[auth(...)]` is a mistake of the item's `auth`, gathered with the others.
fn parse_attrs(input: ParseStream) -> Result<Attrs> {
    let attrs = input.call(Attribute::parse_outer)?;
    let mut lines = Vec::with_capacity(attrs.len());
    let mut auth: Option<(Attribute, Result<Auth>)> = None;

    for attr in attrs {
        if attr.path().is_ident("auth") {
            auth = Some(match auth {
                None => {
                    let read = parse_auth(&attr);

                    (attr, read)
                }
                Some((first, read)) => {
                    let twice = Error::new_spanned(
                        &attr,
                        "an item takes one `#[auth(...)]`: remove this one",
                    );
                    let error = match read {
                        Ok(_) => twice,
                        Err(mut error) => {
                            error.combine(twice);
                            error
                        }
                    };

                    (first, Err(error))
                }
            });
            continue;
        }

        let line = match &attr.meta {
            Meta::NameValue(meta) if meta.path.is_ident("doc") => match &meta.value {
                Expr::Lit(expr) => match &expr.lit {
                    Lit::Str(line) => Some(line.value()),
                    _ => None,
                },
                _ => None,
            },
            _ => None,
        };
        let Some(line) = line else {
            return Err(Error::new_spanned(
                attr,
                "an attribute is not supported here: only `///` comments are kept, and `#[auth(...)]` on a group or a route",
            ));
        };

        lines.push(line);
    }

    Ok(Attrs {
        docs: Docs(lines),
        auth,
    })
}

/// What an `#[auth(...)]` says: `Bearer`, or `none`. What comes later, another scheme, alternatives, `optional` and
/// scopes, is refused with a message saying so.
fn parse_auth(attr: &Attribute) -> Result<Auth> {
    const WRITE: &str =
        "write `#[auth(Bearer)]`, or `#[auth(none)]` on a route that takes no credential";

    let Meta::List(list) = &attr.meta else {
        return Err(Error::new_spanned(
            attr,
            format!("`#[auth]` takes a scheme: {WRITE}"),
        ));
    };

    list.parse_args_with(|input: ParseStream| {
        let word = input.call(Ident::parse_any).map_err(|error| {
            Error::new(error.span(), format!("`#[auth]` takes a scheme: {WRITE}"))
        })?;
        let auth = if word == "none" {
            Auth::None
        } else if word == "Bearer" {
            Auth::Scheme(Scheme::Bearer)
        } else {
            return Err(Error::new(
                word.span(),
                format!("the scheme `{word}` is not supported yet: {WRITE}"),
            ));
        };

        if input.peek(Token![|]) {
            let bar: Token![|] = input.parse()?;
            return Err(Error::new_spanned(
                bar,
                format!("alternatives of schemes are not supported yet: a route takes one scheme, {WRITE}"),
            ));
        }

        if input.peek(Token![,]) {
            input.parse::<Token![,]>()?;
            let option = input.call(Ident::parse_any).map_err(|error| {
                Error::new(error.span(), format!("`#[auth]` takes a scheme alone: {WRITE}"))
            })?;
            return Err(Error::new(
                option.span(),
                format!("`{option}` is not supported yet in `#[auth(...)]`: {WRITE}"),
            ));
        }

        if !input.is_empty() {
            return Err(input.error(format!("`#[auth]` takes a scheme alone: {WRITE}")));
        }

        Ok(auth)
    })
}

#[cfg(test)]
mod tests {
    use quote::{ToTokens, quote};

    use super::*;
    use crate::model::{BodyKind, ParamKind, Reply};
    use crate::path::Segment;

    fn parse(tokens: proc_macro2::TokenStream) -> Result<Api> {
        syn::parse2::<Contract>(tokens).map(|contract| contract.0)
    }

    fn messages(tokens: proc_macro2::TokenStream) -> Vec<String> {
        parse(tokens)
            .expect_err("the contract is wrong")
            .into_iter()
            .map(|error| error.to_string())
            .collect()
    }

    fn text(ty: &impl ToTokens) -> String {
        ty.to_token_stream().to_string()
    }

    #[test]
    fn the_notes_contract_is_read_into_its_model() {
        let api = parse(quote! {
            /// Notes, kept per user.
            pub api Api = "/api";

            pub group Auth {
                /// Trades a login for a token.
                POST "/token" => token(login: Login) -> TokenResponse;
            }

            /// The signed-in user's notes.
            pub group Notes {
                GET "/notes?contains={contains}&page={page}"
                    => list(contains: Option<String>, page: Option<u32>) -> Vec<Note>;
                GET "/notes/{id}" => get(id: i64) -> Note;
                POST "/notes" => create(note: NewNote) -> Created<Note>;
                DELETE "/notes/{id}" => remove(id: i64);
            }
        })
        .unwrap();

        assert_eq!(api.name, "Api");
        assert_eq!(api.prefix, "/api");
        assert_eq!(api.docs.0, [" Notes, kept per user."]);
        assert_eq!(api.groups.len(), 2);

        let notes = &api.groups[1];
        assert_eq!(notes.docs.0, [" The signed-in user's notes."]);
        assert_eq!(notes.routes.len(), 4);

        assert_eq!(text(&notes.vis), "pub");

        let list = &notes.routes[0];
        assert!(list.docs.0.is_empty());
        assert_eq!(text(&list.params[1].ty), "Option < u32 >");
        assert_eq!(list.method, Method::Get);
        assert_eq!(list.name, "list");
        assert!(list.path.iter().eq([&Segment::Literal("notes".into())]));
        assert_eq!(
            list.params
                .iter()
                .map(|param| param.kind.clone())
                .collect::<Vec<_>>(),
            [
                ParamKind::Query {
                    key: "contains".into()
                },
                ParamKind::Query { key: "page".into() },
            ]
        );
        assert!(list.body().is_none());
        assert!(matches!(&list.reply, Reply::Json(ty) if text(ty) == "Vec < Note >"));

        let create = &notes.routes[2];
        assert!(
            matches!(&create.reply, Reply::Created { outer, .. } if text(outer) == "Created < Note >")
        );
        assert_eq!(create.reply.value().map(text), Some("Note".into()));
        assert_eq!(
            create
                .body()
                .map(|(body, kind)| (body.name.to_string(), kind)),
            Some(("note".into(), BodyKind::Json))
        );

        let token = &api.groups[0].routes[0];
        assert_eq!(token.docs.0, [" Trades a login for a token."]);

        let remove = &notes.routes[3];
        assert!(matches!(remove.reply, Reply::None));
        assert_eq!(remove.params[0].kind, ParamKind::Path);
    }

    #[test]
    fn arguments_are_matched_by_name_not_by_position() {
        let api = parse(quote! {
            pub api Api = "/api";
            pub group Repos {
                PUT "/repos/{owner}/{repo}?force={force}"
                    => update(patch: Patch, force: bool, repo: String, owner: String);
            }
        })
        .unwrap();

        let kinds: Vec<_> = api.groups[0].routes[0]
            .params
            .iter()
            .map(|param| (param.name.to_string(), param.kind.clone()))
            .collect();
        assert_eq!(
            kinds,
            [
                ("patch".into(), ParamKind::Body(BodyKind::Json)),
                (
                    "force".into(),
                    ParamKind::Query {
                        key: "force".into()
                    }
                ),
                ("repo".into(), ParamKind::Path),
                ("owner".into(), ParamKind::Path),
            ]
        );
    }

    #[test]
    fn a_delete_may_carry_a_body() {
        let api = parse(quote! {
            pub api Api = "";
            pub group Notes { DELETE "/notes" => remove_many(ids: Vec<i64>); }
        })
        .unwrap();

        assert!(api.groups[0].routes[0].body().is_some());
    }

    #[test]
    fn one_body_only_and_never_on_a_get() {
        let two = messages(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(a: A, b: B); }
        });
        assert!(two[0].contains("one body"), "{two:?}");

        let get = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/x" => f(a: A); }
        });
        assert!(get[0].contains("GET takes no body"), "{get:?}");
    }

    #[test]
    fn a_query_struct_a_header_and_each_body_are_read() {
        let api = parse(quote! {
            pub api Api = "/api";
            pub group Notes {
                GET "/notes/{id}"
                    => search(header("Last-Event-ID") last: Option<String>, query filter: NoteFilter, id: i64);
                POST "/token" => token(login: Form<Login>);
                PUT "/notes/{id}/text" => write(id: i64, text: Text);
                PUT "/notes/{id}/file" => upload(id: i64, file: rest::Bytes);
                POST "/notes" => create(note: NewNote);
            }
        })
        .unwrap();
        let routes = &api.groups[0].routes;

        let kinds: Vec<_> = routes[0]
            .params
            .iter()
            .map(|param| (param.name.to_string(), param.kind.clone()))
            .collect();
        assert_eq!(
            kinds,
            [
                (
                    "last".into(),
                    ParamKind::Header {
                        name: "Last-Event-ID".into()
                    }
                ),
                ("filter".into(), ParamKind::QueryStruct),
                ("id".into(), ParamKind::Path),
            ]
        );
        assert_eq!(
            routes[0].query_struct().map(|param| param.name.to_string()),
            Some("filter".into())
        );
        assert!(routes[0].body().is_none());

        let bodies: Vec<_> = routes[1..]
            .iter()
            .map(|route| route.body().map(|(_, kind)| kind))
            .collect();
        assert_eq!(
            bodies,
            [
                Some(BodyKind::Form),
                Some(BodyKind::Text),
                Some(BodyKind::Bytes),
                Some(BodyKind::Json),
            ]
        );
    }

    #[test]
    fn query_and_header_are_names_too() {
        let api = parse(quote! {
            pub api Api = "/api";
            pub group G {
                GET "/a?q={query}&h={header}" => a(query: String, header: String);
            }
        })
        .unwrap();

        let kinds: Vec<_> = api.groups[0].routes[0]
            .params
            .iter()
            .map(|param| param.kind.clone())
            .collect();
        assert_eq!(
            kinds,
            [
                ParamKind::Query { key: "q".into() },
                ParamKind::Query { key: "h".into() },
            ]
        );
    }

    #[test]
    fn a_query_struct_is_the_whole_query() {
        let with_string = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a?page={page}" => a(page: u32, query filter: Filter); }
        });
        assert!(
            with_string[0].contains("the route's string has a query too"),
            "{with_string:?}"
        );

        let two = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => a(query filter: Filter, query more: More); }
        });
        assert!(
            two[0].contains("one query struct, and `filter` is already it"),
            "{two:?}"
        );

        for ty in [
            quote! { Option<Filter> },
            quote! { &Filter },
            quote! { (A, B) },
            quote! { Vec<Filter> },
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/a" => a(query filter: #ty); }
            });
            assert!(all[0].contains("a field per key"), "{ty}: {all:?}");
        }
    }

    #[test]
    fn a_header_has_a_name_http_takes_and_is_read_once() {
        for name in ["", "X Page", "X-Pagé", "X:Page", "X-Page\n"] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/a" => a(header(#name) page: u32); }
            });
            assert!(
                all[0].contains("is not the name of a header"),
                "{name:?}: {all:?}"
            );
        }

        let twice = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => a(header("X-Page") page: u32, header("x-page") other: u32); }
        });
        assert!(
            twice[0].contains("the header `X-Page` is already read"),
            "{twice:?}"
        );

        let token = parse(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => a(header("X-Weird_Name.v2!") page: u32); }
        });
        assert!(token.is_ok());
    }

    #[test]
    fn a_header_quoll_or_http_writes_is_no_arguments() {
        for name in [
            "Content-Type",
            "content-length",
            "HOST",
            "Transfer-Encoding",
            "Connection",
            "Keep-Alive",
            "TE",
            "Upgrade",
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { POST "/a" => a(header(#name) value: String, note: Note); }
            });
            assert!(
                all[0].contains("is written by quoll or by HTTP itself"),
                "{name}: {all:?}"
            );
        }
    }

    #[test]
    fn authorization_is_the_schemes_on_a_route_that_takes_one() {
        let bearer = messages(quote! {
            pub api Api = "/api";
            #[auth(Bearer)]
            pub group G { GET "/a" => a(header("authorization") token: String); }
        });
        assert!(
            bearer[0].contains("carries the credential of `Bearer`"),
            "{bearer:?}"
        );

        let open = parse(quote! {
            pub api Api = "/api";
            #[auth(Bearer)]
            pub group G {
                #[auth(none)]
                GET "/a" => a(header("Authorization") token: String);
            }
            pub group H { GET "/b" => b(header("Authorization") token: Option<String>); }
        });
        assert!(open.is_ok(), "{open:?}");
    }

    #[test]
    fn a_query_struct_is_not_a_scalar() {
        for ty in [
            quote! { String },
            quote! { u32 },
            quote! { bool },
            quote! { std::primitive::f64 },
            quote! { uuid::Uuid },
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/a" => a(query q: #ty); }
            });
            assert!(all[0].contains("a field per key"), "{ty}: {all:?}");
        }
    }

    #[test]
    fn a_header_is_read_from_text_or_is_an_option_of_it() {
        for ty in [
            quote! { Vec<String> },
            quote! { Option<Vec<String>> },
            quote! { &str },
            quote! { Text },
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/a" => a(header("X-Tags") tags: #ty); }
            });
            assert!(
                all[0].contains("the header `X-Tags` is read from text"),
                "{ty}: {all:?}"
            );
        }
    }

    #[test]
    fn an_argument_comes_from_one_place() {
        let header = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a/{id}" => a(header("X-Id") id: i64); }
        });
        assert!(
            header[0].contains("`id` is a header, and the path's `{id}` names it too"),
            "{header:?}"
        );

        let query = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a?page={page}" => a(query page: Filter); }
        });
        assert!(
            query[0]
                .contains("`page` is the query struct, and the query's `page={page}` names it too"),
            "{query:?}"
        );
    }

    #[test]
    fn a_path_parameter_is_none_of_the_types_read_otherwise() {
        for ty in [
            quote! { HashMap<String, String> },
            quote! { std::collections::BTreeSet<i64> },
            quote! { Text },
            quote! { Form<Login> },
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/a/{id}" => a(id: #ty); }
            });
            assert!(all[0].contains("read from text"), "{ty}: {all:?}");
        }
    }

    #[test]
    fn a_header_is_named_by_a_string_alone() {
        let ident = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => a(header(X) page: u32); }
        });
        assert!(ident[0].contains("named by a string"), "{ident:?}");

        let more = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => a(header("X", "Y") page: u32); }
        });
        assert!(more[0].contains("takes its name alone"), "{more:?}");

        let nameless = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => a(header("X"): u32); }
        });
        assert!(
            nameless[0].contains("followed by the argument's name"),
            "{nameless:?}"
        );
    }

    #[test]
    fn mistakes_in_several_routes_are_all_reported() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group G {
                GET "/a/{x}" => a();
                GET "/b/{y}" => b();
            }
        });

        assert_eq!(all.len(), 2, "{all:?}");
    }

    #[test]
    fn the_same_method_and_path_twice_is_refused_across_groups() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group A { GET "/notes/{id}" => get(id: i64); }
            pub group B { GET "/notes/{note}" => other(note: i64); }
        });

        assert!(all[0].contains("already the route `A::get`"), "{all:?}");
    }

    #[test]
    fn a_function_name_is_taken_once_per_group() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group A { GET "/a" => f(); GET "/b" => f(); }
        });

        assert!(all[0].contains("`f` already"), "{all:?}");
    }

    #[test]
    fn a_contract_needs_exactly_one_api() {
        let none = messages(quote! { pub group G { GET "/a" => f(); } });
        assert!(none[0].contains("names its api"), "{none:?}");

        let two = messages(quote! { pub api A = "/a"; pub api B = "/b"; });
        assert!(two[0].contains("has one api"), "{two:?}");
    }

    #[test]
    fn a_type_through_macro_rules_is_classified_as_written() {
        // `$t:ty` of a `macro_rules!` arrives in an invisible group, and a parenthesis may wrap a type too.
        let group = |tokens: proc_macro2::TokenStream| {
            proc_macro2::Group::new(proc_macro2::Delimiter::None, tokens)
        };
        let created = group(quote! { Created<Note> });
        let api = parse(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(note: NewNote) -> #created; }
        })
        .unwrap();
        let reply = &api.groups[0].routes[0].reply;
        assert!(matches!(reply, Reply::Created { .. }));
        assert_eq!(reply.value().map(text), Some("Note".into()));

        let events = group(quote! { Events<Note> });
        let all = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/x" => f() -> #events; }
        });
        assert!(all[0].contains("`Events` is not supported"), "{all:?}");

        let form = group(quote! { Form<Login> });
        let api = parse(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(login: #form); }
        })
        .unwrap();
        assert!(matches!(
            api.groups[0].routes[0].body(),
            Some((_, BodyKind::Form))
        ));

        let unit = group(quote! { () });
        let all = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/x" => f() -> #unit; }
        });
        assert!(all[0].contains("no `->`"), "{all:?}");

        let vec = group(quote! { Vec<i64> });
        let all = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/x/{ids}" => f(ids: #vec); }
        });
        assert!(all[0].contains("read from text"), "{all:?}");
    }

    #[test]
    fn errors_gathered_are_kept_when_the_reading_stops() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group G {
                GET "/a/{x}" => a();
            }
            #[derive(Debug)]
            pub group H { }
        });
        assert_eq!(all.len(), 2, "{all:?}");
        assert!(all[0].contains("no argument named `x`"), "{all:?}");
        assert!(all[1].contains("only `///` comments"), "{all:?}");

        let missing = messages(quote! {
            pub group G {
                GET "/a/{x}" => a();
            }
        });
        assert_eq!(missing.len(), 2, "{missing:?}");
        assert!(missing[1].contains("names its api"), "{missing:?}");
    }

    #[test]
    fn a_bad_prefix_does_not_hide_the_duplicates() {
        let all = messages(quote! {
            pub api Api = "api";
            pub group G { GET "/a" => f(); GET "/b" => f(); }
        });

        assert_eq!(all.len(), 2, "{all:?}");
        assert!(all[0].contains("the prefix"), "{all:?}");
        assert!(all[1].contains("`f` already"), "{all:?}");
    }

    #[test]
    fn names_are_compared_without_their_raw_marker() {
        let functions = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => get(); GET "/b" => r#get(); }
        });
        assert!(functions[0].contains("`r#get` already"), "{functions:?}");

        let groups = messages(quote! {
            pub api Api = "/api";
            pub group Notes { }
            pub group r#Notes { }
        });
        assert!(groups[0].contains("already defined"), "{groups:?}");
    }

    #[test]
    fn a_group_cannot_take_the_name_of_the_api() {
        let all = messages(quote! {
            pub api Notes = "/api";
            pub group Notes { }
        });

        assert!(all[0].contains("is the name of the api"), "{all:?}");
    }

    #[test]
    fn what_is_left_for_later_says_what_is_supported() {
        let auth = messages(quote! {
            pub api Api = "/api";
            #[auth(Basic)]
            pub group G { GET "/a" => f(); }
        });
        assert!(
            auth[0].contains("the scheme `Basic` is not supported yet"),
            "{auth:?}"
        );

        let events = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => f() -> Events<Note>; }
        });
        assert!(
            events[0]
                .contains("nothing (no `->`), `T`, `Created<T>`, `Accepted<T>`, `Text` or `Bytes`"),
            "{events:?}"
        );
    }

    #[test]
    fn each_result_is_read_into_its_kind() {
        let api = parse(quote! {
            pub api Api = "/api";

            pub group Files {
                POST "/exports" => export() -> rest::Accepted<Job>;
                GET "/readme" => readme() -> Text;
                GET "/logo" => logo() -> rest::Bytes;
                GET "/texts" => texts() -> Vec<Text>;
            }
        })
        .unwrap();
        let routes = &api.groups[0].routes;

        assert!(
            matches!(&routes[0].reply, Reply::Accepted { outer, .. } if text(outer) == "rest :: Accepted < Job >")
        );
        assert_eq!(routes[0].reply.value().map(text), Some("Job".into()));
        assert_eq!(
            routes[0].reply.served().map(text),
            Some("rest :: Accepted < Job >".into())
        );

        assert!(matches!(&routes[1].reply, Reply::Text(ty) if text(ty) == "Text"));
        assert_eq!(routes[1].reply.value().map(text), Some("Text".into()));

        assert!(matches!(&routes[2].reply, Reply::Bytes(ty) if text(ty) == "rest :: Bytes"));
        assert_eq!(
            routes[2].reply.value().map(text),
            Some("rest :: Bytes".into())
        );

        // Only the result's own type is read so: a `Vec` of text is JSON, which does not compile later.
        assert!(matches!(&routes[3].reply, Reply::Json(_)));
    }

    #[test]
    fn accepted_wraps_one_type() {
        let bare = messages(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f() -> Accepted; }
        });
        assert!(
            bare[0].contains("`Accepted` wraps the type of the body: write `Accepted<Note>`"),
            "{bare:?}"
        );

        let two = messages(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f() -> Accepted<Job, Note>; }
        });
        assert!(
            two[0].contains("`Accepted` wraps one type, the body"),
            "{two:?}"
        );
    }

    #[test]
    fn a_head_route_returns_nothing_whatever_its_result() {
        for result in [
            quote! { String },
            quote! { Created<Note> },
            quote! { Accepted<Job> },
            quote! { Text },
            quote! { Bytes },
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { HEAD "/x" => f() -> #result; }
            });
            assert!(all[0].contains("a HEAD answer has no body"), "{all:?}");
        }
    }

    fn schemes(api: &Api) -> Vec<Vec<Option<Scheme>>> {
        api.groups
            .iter()
            .map(|group| group.routes.iter().map(|route| route.auth).collect())
            .collect()
    }

    #[test]
    fn a_route_takes_its_groups_authentication_unless_it_says_otherwise() {
        let api = parse(quote! {
            pub api Api = "/api";

            #[auth(Bearer)]
            pub group Notes {
                GET "/notes/{id}" => get(id: i64) -> Note;
                /// Open to all.
                #[auth(none)]
                GET "/notes/public" => public() -> Vec<Note>;
            }

            pub group Auth {
                POST "/token" => token(login: Login) -> String;
                #[auth(Bearer)]
                DELETE "/token" => revoke();
            }

            #[auth(none)]
            pub group Health {
                GET "/health" => health();
            }
        })
        .unwrap();

        let bearer = Some(Scheme::Bearer);
        assert_eq!(
            schemes(&api),
            [vec![bearer, None], vec![None, bearer], vec![None]]
        );
        assert_eq!(api.groups[0].routes[1].docs.0, [" Open to all."]);
        assert_eq!(api.groups[0].schemes(), [Scheme::Bearer]);
        assert_eq!(api.groups[1].schemes(), [Scheme::Bearer]);
        assert!(api.groups[2].schemes().is_empty());
    }

    #[test]
    fn what_auth_does_not_take_yet_says_so() {
        let cases = [
            (
                quote! { #[auth(Key)] },
                "the scheme `Key` is not supported yet",
            ),
            (
                quote! { #[auth(Bearer | Key)] },
                "alternatives of schemes are not supported yet",
            ),
            (
                quote! { #[auth(Bearer, optional)] },
                "`optional` is not supported yet",
            ),
            (
                quote! { #[auth(Bearer, scopes("repo"))] },
                "`scopes` is not supported yet",
            ),
            (quote! { #[auth(Bearer Key)] }, "takes a scheme alone"),
            (quote! { #[auth()] }, "`#[auth]` takes a scheme"),
            (quote! { #[auth] }, "`#[auth]` takes a scheme"),
            (quote! { #[auth = "Bearer"] }, "`#[auth]` takes a scheme"),
            (quote! { #[auth("Bearer")] }, "`#[auth]` takes a scheme"),
        ];

        for (attr, message) in cases {
            let on_group = messages(quote! {
                pub api Api = "/api";
                #attr
                pub group G { GET "/a" => f(); }
            });
            let on_route = messages(quote! {
                pub api Api = "/api";
                pub group G {
                    #attr
                    GET "/a" => f();
                }
            });

            assert!(on_group[0].contains(message), "{attr}: {on_group:?}");
            assert!(on_route[0].contains(message), "{attr}: {on_route:?}");
        }
    }

    #[test]
    fn a_mistake_in_auth_leaves_the_rest_read() {
        let all = messages(quote! {
            pub api Api = "/api";
            #[auth(Key)]
            pub group G {
                #[auth(Basic)]
                GET "/a/{x}" => a();
                GET "/b/{y}" => b();
            }
        });

        assert_eq!(all.len(), 4, "{all:?}");
        assert!(all[0].contains("`Key`"), "{all:?}");
        assert!(all[1].contains("`Basic`"), "{all:?}");
        assert!(all[2].contains("no argument named `x`"), "{all:?}");
        assert!(all[3].contains("no argument named `y`"), "{all:?}");
    }

    #[test]
    fn auth_is_on_a_group_or_a_route_and_once() {
        let api = messages(quote! {
            #[auth(Bearer)]
            pub api Api = "/api";
        });
        assert!(api[0].contains("an api takes no `#[auth(...)]`"), "{api:?}");

        // Gathered, so that the mistakes after it are reported in the same build.
        let twice = messages(quote! {
            pub api Api = "/api";
            #[auth(Bearer)]
            #[auth(none)]
            pub group G {
                #[auth(Key)]
                #[auth(Bearer)]
                GET "/a/{x}" => a();
            }
        });
        assert_eq!(twice.len(), 4, "{twice:?}");
        assert!(twice[0].contains("one `#[auth(...)]`"), "{twice:?}");
        assert!(twice[1].contains("`Key`"), "{twice:?}");
        assert!(twice[2].contains("one `#[auth(...)]`"), "{twice:?}");
        assert!(twice[3].contains("no argument named `x`"), "{twice:?}");
    }
}

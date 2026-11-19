//! The spelling of `rest::api!`: a block of items, read into the model.
//!
//! ```text
//! /// Notes, kept per user.
//! pub api Api = "/api";
//!
//! /// The key of an operator's scripts.
//! pub scheme Key = header "X-Api-Key";
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
    Api, ArgDecl, Auth, DeclaredScheme, Docs, ErrorList, Group, MAX_ALTERNATIVES, Method, Route,
    RouteAuth, RouteDecl, Scheme, Source,
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
    let mut schemes = Vec::new();
    let mut groups = Vec::new();

    while !input.is_empty() {
        let Attrs {
            docs,
            auth,
            openapi,
        } = parse_attrs(input)?;
        let vis: Visibility = input.parse()?;
        let keyword = input.call(Ident::parse_any).map_err(|error| {
            Error::new(
                error.span(),
                "expected `api`, `scheme` or `group`: an item of a contract starts with one of them",
            )
        })?;

        if keyword == "api" {
            if let Some((attr, _)) = &auth {
                errors.push(Error::new_spanned(
                    attr,
                    "an api takes no `#[auth(...)]`: put it on a group, or on a route",
                ));
            }

            let openapi = match openapi {
                Some((_, read)) => keep(errors, read).is_none_or(|document| document),
                None => true,
            };
            let decl = ApiDecl::parse(input, docs, vis, openapi, &keyword)?;
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
        } else if keyword == "scheme" {
            refuse_openapi(errors, openapi.as_ref(), "a scheme");
            if let Some((attr, _)) = &auth {
                errors.push(Error::new_spanned(
                    attr,
                    "a scheme takes no `#[auth(...)]`: put it on a group, or on a route, that takes the scheme",
                ));
            }

            match parse_scheme(input, docs, vis)? {
                Ok(scheme) => schemes.push(scheme),
                Err(error) => errors.push(error),
            }
        } else if keyword == "group" {
            refuse_openapi(errors, openapi.as_ref(), "a group");
            let auth = auth.and_then(|(_, auth)| keep(errors, auth));
            groups.push(parse_group(input, docs, vis, auth.as_ref(), errors)?);
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

    let openapi = api.openapi;
    let mut api = Api::new(api.docs, api.vis, api.name, &api.prefix, schemes, groups)
        .map_err(|error| fold(errors, error))?;
    api.openapi = openapi;

    Ok(api)
}

/// Refuses an `#[openapi(...)]` on `what`, which is not the api.
fn refuse_openapi(errors: &mut ErrorList, openapi: Option<&(Attribute, Result<bool>)>, what: &str) {
    if let Some((attr, _)) = openapi {
        errors.push(Error::new_spanned(
            attr,
            format!("{what} takes no `#[openapi(...)]`: `#[openapi(none)]` goes on the api, whose whole document it leaves out"),
        ));
    }
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
        "an item of a contract starts with `api`, `scheme` or `group`"
    };

    Error::new(keyword.span(), format!("unexpected `{keyword}`: {hint}"))
}

struct ApiDecl {
    docs: Docs,
    vis: Visibility,
    openapi: bool,
    name: Ident,
    prefix: LitStr,
}

impl ApiDecl {
    /// After the `api` keyword: `Name = "/prefix";`.
    fn parse(
        input: ParseStream,
        docs: Docs,
        vis: Visibility,
        openapi: bool,
        keyword: &Ident,
    ) -> Result<Self> {
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
            openapi,
            name,
            prefix,
        })
    }
}

/// After the `scheme` keyword: `Name = kind "value";`. The outer error is a mistake of syntax, after which nothing can
/// be read; the inner one a mistake of the scheme's meaning, which leaves the next item readable.
fn parse_scheme(input: ParseStream, docs: Docs, vis: Visibility) -> Result<Result<DeclaredScheme>> {
    const WRITE: &str = "write `pub scheme Key = header \"X-Api-Key\";`";

    let name: Ident = input.parse().map_err(|error| {
        Error::new(
            error.span(),
            format!("`scheme` takes the name of the scheme's type: {WRITE}"),
        )
    })?;
    input.parse::<Token![=]>().map_err(|error| {
        Error::new(
            error.span(),
            format!("expected `=` after the scheme's name: {WRITE}"),
        )
    })?;
    let kind = input.call(Ident::parse_any).map_err(|error| {
        Error::new(
            error.span(),
            format!("a scheme says where its credential travels, `header`, `cookie`, `query` or `openid`: {WRITE}"),
        )
    })?;
    let value: LitStr = input
        .parse()
        .map_err(|error| Error::new(error.span(), format!("`{kind}` takes a string: {WRITE}")))?;
    input.parse::<Token![;]>()?;

    Ok(DeclaredScheme::new(docs, vis, name, &kind, &value))
}

/// After the `group` keyword: `Name { routes }`, whose routes take `auth` unless they say otherwise. A mistake in one
/// route is gathered, the others are still read.
fn parse_group(
    input: ParseStream,
    docs: Docs,
    vis: Visibility,
    auth: Option<&Auth>,
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
fn parse_route(input: ParseStream, group_auth: Option<&Auth>) -> Result<Result<Route>> {
    let Attrs {
        docs,
        auth,
        openapi,
    } = parse_attrs(input)?;
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

    let (auth, mut attr_error) = match auth.map(|(_, auth)| auth).transpose() {
        Ok(auth) => (auth, None),
        Err(error) => (None, Some(error)),
    };
    if let Some((attr, _)) = &openapi {
        let refused = Error::new_spanned(
            attr,
            "a route takes no `#[openapi(...)]`: `#[openapi(none)]` goes on the api, whose whole document it leaves out",
        );

        match &mut attr_error {
            Some(error) => error.combine(refused),
            None => attr_error = Some(refused),
        }
    }
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

    let Some(mut error) = attr_error else {
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

/// The attributes before an item: its `///` comments, its `#[auth(...)]` with what it says, or the mistake in it, and
/// its `#[openapi(...)]` with whether the api has a document, or the mistake in it.
struct Attrs {
    docs: Docs,
    auth: Option<(Attribute, Result<Auth>)>,
    openapi: Option<(Attribute, Result<bool>)>,
}

/// The attributes before an item. Any attribute other than a comment, `#[auth(...)]` or `#[openapi(...)]` is refused,
/// which stops the reading; a second `#[auth(...)]` or `#[openapi(...)]` is a mistake of the item's, gathered with the
/// others.
fn parse_attrs(input: ParseStream) -> Result<Attrs> {
    let attrs = input.call(Attribute::parse_outer)?;
    let mut lines = Vec::with_capacity(attrs.len());
    let mut auth: Option<(Attribute, Result<Auth>)> = None;
    let mut openapi: Option<(Attribute, Result<bool>)> = None;

    for attr in attrs {
        if attr.path().is_ident("openapi") {
            openapi = Some(match openapi {
                None => {
                    let read = parse_openapi(&attr);

                    (attr, read)
                }
                Some((first, read)) => {
                    let twice = Error::new_spanned(
                        &attr,
                        "an api takes one `#[openapi(...)]`: remove this one",
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
                "an attribute is not supported here: only `///` comments are kept, `#[auth(...)]` on a group or a route, and `#[openapi(none)]` on the api",
            ));
        };

        lines.push(line);
    }

    Ok(Attrs {
        docs: Docs(lines),
        auth,
        openapi,
    })
}

/// What an `#[openapi(...)]` says: `none`, which leaves out the api's document. Whether the api has one.
fn parse_openapi(attr: &Attribute) -> Result<bool> {
    const WRITE: &str = "write `#[openapi(none)]`, which leaves out the OpenAPI document of a contract that is only called, whose types need no schema";

    let Meta::List(list) = &attr.meta else {
        return Err(Error::new_spanned(
            attr,
            format!("`#[openapi]` says what the api's document is: {WRITE}"),
        ));
    };
    let word: Ident = syn::parse2(list.tokens.clone()).map_err(|_| {
        Error::new_spanned(
            &list.tokens,
            format!("`#[openapi(...)]` takes `none`: {WRITE}"),
        )
    })?;

    if word != "none" {
        return Err(Error::new_spanned(
            word,
            format!("`#[openapi(...)]` takes `none`: {WRITE}"),
        ));
    }

    Ok(false)
}

/// How an `#[auth(...)]` is written, which its errors say.
const WRITE_AUTH: &str = "write `#[auth(Bearer)]`, `#[auth(Bearer | Key)]`, `#[auth(Bearer, optional)]` or `#[auth(Bearer, scopes(\"repo\"))]`, or `#[auth(none)]` on a route that takes no credential";

/// What an `#[auth(...)]` says: `none`, or schemes, `A | B`, then its options, `optional` or `scopes("...")`.
fn parse_auth(attr: &Attribute) -> Result<Auth> {
    let Meta::List(list) = &attr.meta else {
        return Err(Error::new_spanned(
            attr,
            format!("`#[auth]` takes a scheme: {WRITE_AUTH}"),
        ));
    };

    list.parse_args_with(|input: ParseStream| {
        let first = parse_scheme_path(input)?;

        if first.is_ident("none") {
            if !input.is_empty() {
                return Err(input.error(
                    "`#[auth(none)]` takes nothing else: a route with `none` takes no credential, and no option",
                ));
            }

            return Ok(Auth::None);
        }

        let mut schemes = vec![Scheme::new(first)];
        while input.peek(Token![|]) {
            let bar: Token![|] = input.parse()?;
            let path = parse_scheme_path(input)?;

            if path.is_ident("none") {
                return Err(Error::new_spanned(
                    path,
                    "`none` is not a scheme of an alternative: a route takes `none` alone, `#[auth(none)]`",
                ));
            }

            let scheme = Scheme::new(path);
            if schemes.iter().any(|first| first.word() == scheme.word()) {
                return Err(Error::new(
                    scheme.span(),
                    format!("`{}` is already a scheme of this alternative: remove it", scheme.word()),
                ));
            }
            if schemes.len() == MAX_ALTERNATIVES {
                return Err(Error::new_spanned(
                    bar,
                    format!("an alternative takes up to {MAX_ALTERNATIVES} schemes: remove one, or give the route a scheme of its own that reads them all"),
                ));
            }

            schemes.push(scheme);
        }

        let mut optional: Option<Ident> = None;
        let mut scopes: Option<(Ident, Vec<String>)> = None;
        while input.peek(Token![,]) {
            input.parse::<Token![,]>()?;
            if input.is_empty() {
                break;
            }

            let option = input.call(Ident::parse_any).map_err(|error| {
                Error::new(
                    error.span(),
                    format!("after its schemes, `#[auth]` takes `optional` or `scopes(\"...\")`: {WRITE_AUTH}"),
                )
            })?;

            if option == "optional" {
                if optional.is_some() {
                    return Err(Error::new(option.span(), "`optional` is written twice: remove one"));
                }
                optional = Some(option);
            } else if option == "scopes" {
                if scopes.is_some() {
                    return Err(Error::new(
                        option.span(),
                        "`scopes` is written twice: put every scope in one `scopes(...)`",
                    ));
                }
                scopes = Some((option, parse_scopes(input)?));
            } else {
                return Err(Error::new(
                    option.span(),
                    format!("`{option}` is not an option of `#[auth]`, which takes `optional` or `scopes(\"...\")`: {WRITE_AUTH}"),
                ));
            }
        }

        if !input.is_empty() {
            return Err(input.error(format!(
                "expected `|` and a scheme, or `,` and an option: {WRITE_AUTH}"
            )));
        }

        if let (Some(_), Some((scopes, _))) = (&optional, &scopes) {
            return Err(Error::new(
                scopes.span(),
                "a route with `optional` takes no `scopes`: a request with no credential has no scope to check, so make the route require its credential",
            ));
        }

        Ok(Auth::Takes(RouteAuth {
            schemes,
            optional: optional.is_some(),
            scopes: scopes.map(|(_, scopes)| scopes).unwrap_or_default(),
        }))
    })
}

/// A scheme in `#[auth(...)]`: a path to its type, `Bearer`, `Key` or `crate::auth::Hmac`.
fn parse_scheme_path(input: ParseStream) -> Result<syn::Path> {
    input.call(syn::Path::parse_mod_style).map_err(|error| {
        Error::new(
            error.span(),
            format!("`#[auth]` takes a scheme: {WRITE_AUTH}"),
        )
    })
}

/// After `scopes`: `("repo", "admin")`, each scope an OAuth `scope-token` written once.
fn parse_scopes(input: ParseStream) -> Result<Vec<String>> {
    const WRITE: &str =
        "write `scopes(\"repo\")`, or `scopes(\"repo\", \"admin\")` for a caller that needs both";

    let content;
    let paren = syn::parenthesized!(content in input);
    let items = Punctuated::<LitStr, Token![,]>::parse_terminated(&content)
        .map_err(|error| Error::new(error.span(), format!("`scopes` takes strings: {WRITE}")))?;

    if items.is_empty() {
        return Err(Error::new(
            paren.span.join(),
            format!("`scopes` takes at least one scope: {WRITE}"),
        ));
    }

    let mut scopes: Vec<String> = Vec::with_capacity(items.len());
    for item in &items {
        let scope = item.value();
        // The `scope-token` of RFC 6749: printable ASCII but space, `"` and `\`.
        let valid = !scope.is_empty()
            && scope
                .bytes()
                .all(|byte| byte.is_ascii_graphic() && byte != b'"' && byte != b'\\');

        if !valid {
            return Err(Error::new(
                item.span(),
                format!(
                    "`{scope}` is not a scope: a scope is printable ASCII with no space, `\"` or `\\`, as in \"repo\" or \"notes:read\""
                ),
            ));
        }
        if scopes.contains(&scope) {
            return Err(Error::new(
                item.span(),
                format!("the scope `{scope}` is written twice: remove one"),
            ));
        }

        scopes.push(scope);
    }

    Ok(scopes)
}

#[cfg(test)]
mod tests {
    use quote::{ToTokens, quote};

    use super::*;
    use crate::known::{BodyKind, ResultKind};
    use crate::model::{DeclaredKind, Location, ParamKind, Returned, SchemeKind};
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

    /// The type a route's result is written with, and how it is answered.
    fn reply(reply: &Option<Box<Returned>>) -> Option<(String, ResultKind)> {
        reply
            .as_deref()
            .map(|returned| (text(&returned.served), returned.kind()))
    }

    /// How a route's result is answered.
    fn kind(reply: &Option<Box<Returned>>) -> Option<ResultKind> {
        reply.as_deref().map(Returned::kind)
    }

    /// The type the client returns.
    fn value(reply: &Option<Box<Returned>>) -> Option<String> {
        reply.as_deref().map(|returned| text(&returned.value))
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
        assert_eq!(
            reply(&list.reply),
            Some(("Vec < Note >".into(), ResultKind::JSON))
        );

        let create = &notes.routes[2];
        assert_eq!(
            reply(&create.reply),
            Some(("Created < Note >".into(), ResultKind::CREATED))
        );
        assert_eq!(value(&create.reply), Some("Note".into()));
        assert_eq!(
            create
                .body()
                .map(|(body, kind)| (body.name.to_string(), kind)),
            Some(("note".into(), BodyKind::JSON))
        );

        let token = &api.groups[0].routes[0];
        assert_eq!(token.docs.0, [" Trades a login for a token."]);

        let remove = &notes.routes[3];
        assert!(remove.reply.is_none());
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
                ("patch".into(), ParamKind::Body(BodyKind::JSON)),
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
                Some(BodyKind::FORM),
                Some(BodyKind::TEXT),
                Some(BodyKind::BYTES),
                Some(BodyKind::JSON),
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
        assert_eq!(kind(reply), Some(ResultKind::CREATED));
        assert_eq!(value(reply), Some("Note".into()));

        let events = group(quote! { Events<Note> });
        let api = parse(quote! {
            pub api Api = "/api";
            pub group G { GET "/x" => f() -> #events; }
        })
        .unwrap();
        assert_eq!(
            kind(&api.groups[0].routes[0].reply),
            Some(ResultKind::EVENTS)
        );

        let form = group(quote! { Form<Login> });
        let api = parse(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(login: #form); }
        })
        .unwrap();
        assert_eq!(
            api.groups[0].routes[0].body().map(|(_, kind)| kind),
            Some(BodyKind::FORM)
        );

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
    fn each_result_is_read_into_its_kind() {
        let api = parse(quote! {
            pub api Api = "/api";

            pub group Files {
                POST "/exports" => export() -> rest::Accepted<Job>;
                GET "/readme" => readme() -> Text;
                GET "/logo" => logo() -> rest::Bytes;
                GET "/texts" => texts() -> Vec<Text>;
                GET "/events" => events() -> Events<Note>;
                POST "/lines" => lines() -> rest::JsonLines<Note>;
            }
        })
        .unwrap();
        let routes = &api.groups[0].routes;

        let read = |i: usize| (reply(&routes[i].reply), value(&routes[i].reply));
        let some = |served: &str, kind, value: &str| {
            (Some((served.to_owned(), kind)), Some(value.to_owned()))
        };

        assert_eq!(
            read(0),
            some("rest :: Accepted < Job >", ResultKind::ACCEPTED, "Job")
        );
        assert_eq!(read(1), some("Text", ResultKind::TEXT, "Text"));
        assert_eq!(
            read(2),
            some("rest :: Bytes", ResultKind::BYTES, "rest :: Bytes")
        );

        // Only the result's own type is read so: a `Vec` of text is JSON, which does not compile later.
        assert_eq!(
            read(3),
            some("Vec < Text >", ResultKind::JSON, "Vec < Text >")
        );

        // A stream is the same type on both sides: the client returns it as the server's trait does.
        assert_eq!(
            read(4),
            some("Events < Note >", ResultKind::EVENTS, "Events < Note >")
        );
        assert_eq!(
            read(5),
            some(
                "rest :: JsonLines < Note >",
                ResultKind::JSON_LINES,
                "rest :: JsonLines < Note >"
            )
        );
    }

    #[test]
    fn a_stream_wraps_the_type_of_one_item() {
        for wrapper in ["Events", "JsonLines"] {
            let name = Ident::new(wrapper, proc_macro2::Span::call_site());
            let bare = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/x" => f() -> #name; }
            });
            assert!(
                bare[0].contains(&format!(
                    "`{wrapper}` wraps the type of each item: write `{wrapper}<Note>`"
                )),
                "{bare:?}"
            );

            let two = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/x" => f() -> #name<Note, Job>; }
            });
            assert!(
                two[0].contains(&format!("`{wrapper}` wraps one type, each item")),
                "{two:?}"
            );

            let body = messages(quote! {
                pub api Api = "/api";
                pub group G { POST "/x" => f(notes: #name<Note>); }
            });
            assert!(
                body[0].contains(&format!(
                    "`{wrapper}` is a stream a route answers, and a route does not read a stream as its body"
                )),
                "{body:?}"
            );

            let path = messages(quote! {
                pub api Api = "/api";
                pub group G { GET "/x/{id}" => f(id: #name<i64>); }
            });
            assert!(path[0].contains("is read from text"), "{path:?}");
        }
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
            quote! { Events<Note> },
            quote! { JsonLines<Note> },
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub group G { HEAD "/x" => f() -> #result; }
            });
            assert!(all[0].contains("a HEAD answer has no body"), "{all:?}");
        }
    }

    /// The schemes each route takes, as the contract writes them.
    fn schemes(api: &Api) -> Vec<Vec<Vec<String>>> {
        api.groups
            .iter()
            .map(|group| {
                group
                    .routes
                    .iter()
                    .map(|route| route.schemes().map(Scheme::word).collect())
                    .collect()
            })
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

        let bearer = vec!["Bearer".to_owned()];
        assert_eq!(
            schemes(&api),
            [
                vec![bearer.clone(), vec![]],
                vec![vec![], bearer],
                vec![vec![]]
            ]
        );
        assert_eq!(api.groups[0].routes[1].docs.0, [" Open to all."]);
    }

    #[test]
    fn auth_takes_alternatives_optional_and_scopes() {
        let api = parse(quote! {
            pub api Api = "/api";
            pub scheme Key = header "X-Api-Key";

            pub group G {
                #[auth(Bearer | Key | crate::auth::Hmac)]
                GET "/a" => a();
                #[auth(Basic, optional)]
                GET "/b" => b();
                #[auth(Bearer, scopes("repo", "notes:read"),)]
                GET "/c" => c();
            }
        })
        .unwrap();
        let routes = &api.groups[0].routes;
        let auth = |index: usize| routes[index].auth.as_ref().expect("an auth");

        assert_eq!(schemes(&api)[0][0], ["Bearer", "Key", "crate::auth::Hmac"]);
        let kinds: Vec<&SchemeKind> = auth(0).schemes.iter().map(|scheme| &scheme.kind).collect();
        assert_eq!(
            kinds,
            [
                &SchemeKind::Bearer,
                &SchemeKind::Declared(Location::Header("x-api-key".into())),
                &SchemeKind::Foreign
            ]
        );
        assert!(auth(1).optional);
        assert_eq!(auth(1).schemes[0].kind, SchemeKind::Basic);
        assert_eq!(auth(2).scopes, ["repo", "notes:read"]);
        assert!(!auth(2).optional);
    }

    #[test]
    fn a_mistake_in_auth_says_how_to_write_it() {
        let cases = [
            (quote! { #[auth(Bearer Key)] }, "expected `|` and a scheme"),
            (quote! { #[auth()] }, "`#[auth]` takes a scheme"),
            (quote! { #[auth] }, "`#[auth]` takes a scheme"),
            (quote! { #[auth = "Bearer"] }, "`#[auth]` takes a scheme"),
            (quote! { #[auth("Bearer")] }, "`#[auth]` takes a scheme"),
            (
                quote! { #[auth(none | Bearer)] },
                "`#[auth(none)]` takes nothing else",
            ),
            (
                quote! { #[auth(Bearer | none)] },
                "`none` is not a scheme of an alternative",
            ),
            (
                quote! { #[auth(none, optional)] },
                "`#[auth(none)]` takes nothing else",
            ),
            (
                quote! { #[auth(Bearer | Bearer)] },
                "`Bearer` is already a scheme of this alternative",
            ),
            (quote! { #[auth(A | B | C | D | E)] }, "up to 4 schemes"),
            (
                quote! { #[auth(Bearer, optional, optional)] },
                "`optional` is written twice",
            ),
            (
                quote! { #[auth(Bearer, scopes("a"), scopes("b"))] },
                "`scopes` is written twice",
            ),
            (
                quote! { #[auth(Bearer, required)] },
                "`required` is not an option of `#[auth]`",
            ),
            (
                quote! { #[auth(Bearer, scopes())] },
                "takes at least one scope",
            ),
            (
                quote! { #[auth(Bearer, scopes(repo))] },
                "`scopes` takes strings",
            ),
            (
                quote! { #[auth(Bearer, scopes("a b"))] },
                "`a b` is not a scope",
            ),
            (quote! { #[auth(Bearer, scopes(""))] }, "`` is not a scope"),
            (
                quote! { #[auth(Bearer, scopes("a", "a"))] },
                "the scope `a` is written twice",
            ),
            (
                quote! { #[auth(Bearer, optional, scopes("a"))] },
                "a route with `optional` takes no `scopes`",
            ),
            (
                quote! { #[auth(Bearer, scopes("a"), optional)] },
                "a route with `optional` takes no `scopes`",
            ),
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
            #[auth(Bearer | )]
            pub group G {
                #[auth(Basic, x)]
                GET "/a/{x}" => a();
                GET "/b/{y}" => b();
            }
        });

        assert_eq!(all.len(), 4, "{all:?}");
        assert!(all[0].contains("`#[auth]` takes a scheme"), "{all:?}");
        assert!(all[1].contains("`x` is not an option"), "{all:?}");
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

        let scheme = messages(quote! {
            pub api Api = "/api";
            #[auth(Bearer)]
            pub scheme Key = header "X-Api-Key";
        });
        assert!(
            scheme[0].contains("a scheme takes no `#[auth(...)]`"),
            "{scheme:?}"
        );

        // Gathered, so that the mistakes after it are reported in the same build.
        let twice = messages(quote! {
            pub api Api = "/api";
            #[auth(Bearer)]
            #[auth(none)]
            pub group G {
                #[auth(Bearer, x)]
                #[auth(Bearer)]
                GET "/a/{x}" => a();
            }
        });
        assert_eq!(twice.len(), 4, "{twice:?}");
        assert!(twice[0].contains("one `#[auth(...)]`"), "{twice:?}");
        assert!(twice[1].contains("`x` is not an option"), "{twice:?}");
        assert!(twice[2].contains("one `#[auth(...)]`"), "{twice:?}");
        assert!(twice[3].contains("no argument named `x`"), "{twice:?}");
    }

    #[test]
    fn openapi_none_leaves_out_the_document_of_the_api() {
        let with = parse(quote! {
            /// A third-party API, only called.
            #[openapi(none)]
            pub api Api = "";
        })
        .unwrap();
        let without = parse(quote! { pub api Api = "/api"; }).unwrap();

        assert!(!with.openapi);
        assert_eq!(with.docs.0, [" A third-party API, only called."]);
        assert!(without.openapi);
    }

    #[test]
    fn openapi_takes_none_on_the_api_once() {
        for attr in [
            quote! { #[openapi] },
            quote! { #[openapi = "none"] },
            quote! { #[openapi()] },
            quote! { #[openapi(all)] },
            quote! { #[openapi(none, none)] },
        ] {
            let errors = messages(quote! {
                #attr
                pub api Api = "/api";
            });

            assert_eq!(errors.len(), 1, "{attr}: {errors:?}");
            assert!(errors[0].contains("write `#[openapi(none)]`"), "{errors:?}");
        }

        let twice = messages(quote! {
            #[openapi(none)]
            #[openapi(none)]
            pub api Api = "/api";
        });
        assert!(twice[0].contains("one `#[openapi(...)]`"), "{twice:?}");

        // Gathered, so that the mistakes after it are reported in the same build.
        let elsewhere = messages(quote! {
            pub api Api = "/api";
            #[openapi(none)]
            pub scheme Key = header "X-Api-Key";
            #[openapi(none)]
            pub group G {
                #[openapi(none)]
                GET "/a/{x}" => a();
            }
        });
        assert_eq!(elsewhere.len(), 4, "{elsewhere:?}");
        assert!(
            elsewhere[0].contains("a scheme takes no `#[openapi(...)]`"),
            "{elsewhere:?}"
        );
        assert!(
            elsewhere[1].contains("a group takes no `#[openapi(...)]`"),
            "{elsewhere:?}"
        );
        assert!(
            elsewhere[2].contains("a route takes no `#[openapi(...)]`"),
            "{elsewhere:?}"
        );
        assert!(
            elsewhere[3].contains("no argument named `x`"),
            "{elsewhere:?}"
        );
    }

    #[test]
    fn a_scheme_is_declared_with_where_its_credential_travels() {
        let api = parse(quote! {
            pub api Api = "/api";
            /// The key of an operator's scripts.
            pub scheme Key = header "X-Api-Key";
            pub(crate) scheme Session = cookie "session";
            scheme Token = query "token";
            pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";
        })
        .unwrap();
        let kinds: Vec<(String, DeclaredKind)> = api
            .schemes
            .iter()
            .map(|scheme| (scheme.name.to_string(), scheme.kind.clone()))
            .collect();

        assert_eq!(
            kinds,
            [
                ("Key".into(), DeclaredKind::Header("X-Api-Key".into())),
                ("Session".into(), DeclaredKind::Cookie("session".into())),
                ("Token".into(), DeclaredKind::Query("token".into())),
                (
                    "Login".into(),
                    DeclaredKind::OpenId(
                        "https://accounts.example.com/.well-known/openid-configuration".into()
                    )
                ),
            ]
        );
        assert_eq!(
            api.schemes[0].docs.0,
            [" The key of an operator's scripts."]
        );
        assert_eq!(text(&api.schemes[1].vis), "pub (crate)");
    }

    #[test]
    fn a_mistake_in_a_scheme_says_how_to_write_it() {
        let cases = [
            (
                quote! { pub scheme = header "X"; },
                "`scheme` takes the name",
            ),
            (quote! { pub scheme Key header "X"; }, "expected `=`"),
            (
                quote! { pub scheme Key = "X"; },
                "says where its credential travels",
            ),
            (
                quote! { pub scheme Key = header X; },
                "`header` takes a string",
            ),
            (
                quote! { pub scheme Key = body "X"; },
                "`body` is not a kind of scheme",
            ),
            (
                quote! { pub scheme Key = header "X Key"; },
                "is not the name of a header",
            ),
            (
                quote! { pub scheme Key = header "Content-Type"; },
                "written by quoll or by HTTP",
            ),
            (
                quote! { pub scheme Key = header "authorization"; },
                "read by `Bearer` and `Basic`",
            ),
            (
                quote! { pub scheme Key = header "Cookie"; },
                "read by `cookie` schemes",
            ),
            (
                quote! { pub scheme Key = cookie "a;b"; },
                "is not the name of a cookie",
            ),
            (quote! { pub scheme Key = query ""; }, "is not a query key"),
            (
                quote! { pub scheme Key = query "a&b"; },
                "is not a query key",
            ),
            (
                quote! { pub scheme Key = openid "accounts.example.com"; },
                "the URL of its discovery document",
            ),
            (
                quote! { pub scheme Key = openid "https://"; },
                "the URL of its discovery document",
            ),
            (
                quote! { pub scheme Bearer = header "X"; },
                "`Bearer` is a word `#[auth(...)]` already takes",
            ),
            (
                quote! { pub scheme Basic = header "X"; },
                "`Basic` is a word",
            ),
            (quote! { pub scheme none = header "X"; }, "`none` is a word"),
            (quote! { pub scheme Clé = header "X"; }, "which is ASCII"),
        ];

        for (item, message) in cases {
            let all = messages(quote! {
                pub api Api = "/api";
                #item
            });

            assert!(all[0].contains(message), "{item}: {all:?}");
        }
    }

    #[test]
    fn a_scheme_has_its_own_name() {
        let cases = [
            (
                quote! {
                    pub scheme Key = header "X-A";
                    pub scheme Key = header "X-B";
                },
                "the scheme `Key` is already declared",
            ),
            (
                quote! { pub scheme Api = header "X-A"; },
                "is already the type of the api",
            ),
            (
                quote! { pub scheme ApiClient = header "X-A"; },
                "is already the client of the api",
            ),
            (
                quote! {
                    pub scheme Notes = header "X-A";
                    pub group Notes { }
                },
                "is already the scheme `Notes`",
            ),
        ];

        for (items, message) in cases {
            let all = messages(quote! {
                pub api Api = "/api";
                #items
            });

            assert!(all[0].contains(message), "{items}: {all:?}");
        }
    }

    #[test]
    fn a_scheme_reaches_as_far_as_the_groups_that_take_it() {
        let all = messages(quote! {
            pub api Api = "/api";
            scheme Key = header "X-Api-Key";
            #[auth(Key)]
            pub group G { GET "/a" => a(); }
        });
        assert!(
            all[0].contains("the scheme `Key` is less visible than the group `G`"),
            "{all:?}"
        );
        assert!(all[0].contains("write `pub scheme Key`"), "{all:?}");

        let private = parse(quote! {
            api Api = "/api";
            scheme Key = header "X-Api-Key";
            #[auth(Key)]
            group G { GET "/a" => a(); }
        });
        assert!(private.is_ok(), "{private:?}");
    }

    #[test]
    fn two_schemes_of_an_alternative_read_two_places() {
        let cases = [
            (
                quote! { #[auth(Bearer | Login)] },
                "`Bearer` and `Login` both read the header `Authorization: Bearer`",
            ),
            (
                quote! { #[auth(Key | Other)] },
                "`Key` and `Other` both read the header `x-api-key`",
            ),
        ];

        for (attr, message) in cases {
            let all = messages(quote! {
                pub api Api = "/api";
                pub scheme Key = header "X-Api-Key";
                pub scheme Other = header "x-api-KEY";
                pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";
                pub group G {
                    #attr
                    GET "/a" => a();
                }
            });

            assert!(all[0].contains(message), "{attr}: {all:?}");
        }

        let apart = parse(quote! {
            pub api Api = "/api";
            pub scheme Key = header "X-Api-Key";
            pub scheme Session = cookie "session";
            pub scheme Token = query "token";
            pub group G {
                #[auth(Bearer | Basic | Key | Session)]
                GET "/a" => a();
                #[auth(Token | Hmac)]
                GET "/b" => b();
            }
        });
        assert!(apart.is_ok(), "{apart:?}");
    }

    #[test]
    fn no_argument_reads_the_credential_of_a_scheme_the_route_takes() {
        let cases = [
            (
                quote! { #[auth(Key)] GET "/a" => a(header("x-api-key") key: String); },
                "the header `x-api-key` carries the credential of `Key`",
            ),
            (
                quote! { #[auth(Bearer | Session)] GET "/a" => a(header("Cookie") all: String); },
                "the header `Cookie` carries the credential of `Session`",
            ),
            (
                quote! { #[auth(Token)] GET "/a?token={value}" => a(value: String); },
                "the query key `token` carries the credential of `Token`",
            ),
            (
                quote! { #[auth(Key | Hmac)] GET "/a" => a(header("Authorization") all: String); },
                "carries the credential of `Key` or `Hmac`",
            ),
        ];

        for (route, message) in cases {
            let all = messages(quote! {
                pub api Api = "/api";
                pub scheme Key = header "X-Api-Key";
                pub scheme Session = cookie "session";
                pub scheme Token = query "token";
                pub group G { #route }
            });

            assert!(all[0].contains(message), "{route}: {all:?}");
        }

        let elsewhere = parse(quote! {
            pub api Api = "/api";
            pub scheme Key = header "X-Api-Key";
            pub scheme Token = query "token";
            pub group G {
                #[auth(Bearer)]
                GET "/a?token={value}" => a(header("X-Api-Key") key: String, value: String);
                GET "/b" => b(header("Cookie") all: String);
            }
        });
        assert!(elsewhere.is_ok(), "{elsewhere:?}");
    }

    #[test]
    fn a_scheme_read_by_its_name_is_not_named_by_a_path() {
        for (attr, message) in [
            (
                quote! { #[auth(Key | self::Key)] },
                "`self::Key` names the scheme `Key`",
            ),
            (
                quote! { #[auth(crate::Key)] },
                "`crate::Key` names the scheme `Key`",
            ),
            (
                quote! { #[auth(rest::Bearer)] },
                "`rest::Bearer` names the scheme `Bearer`",
            ),
            (
                quote! { #[auth(::quoll::rest::Basic)] },
                "names the scheme `Basic`",
            ),
        ] {
            let all = messages(quote! {
                pub api Api = "/api";
                pub scheme Key = header "X-Api-Key";
                pub group G {
                    #attr
                    GET "/a" => a();
                }
            });

            assert!(all[0].contains(message), "{attr}: {all:?}");
        }

        let other = parse(quote! {
            pub api Api = "/api";
            pub scheme Key = header "X-Api-Key";
            pub group G {
                #[auth(Key | crate::auth::Hmac)]
                GET "/a" => a();
                #[auth(crate::auth::Bearer)]
                GET "/b" => b();
            }
        });
        assert!(other.is_ok(), "{other:?}");
    }

    #[test]
    fn a_group_ties_the_callers_of_an_alternative_to_its_first_scheme() {
        let api = parse(quote! {
            pub api Api = "/api";
            pub scheme Key = header "X-Api-Key";
            pub group G {
                #[auth(Key)]
                GET "/a" => a();
                #[auth(Bearer | Key, scopes("x"))]
                GET "/b" => b();
                #[auth(Basic)]
                GET "/c" => c();
            }
        })
        .unwrap();
        let group = &api.groups[0];
        let bounds = group.bounds();
        let shown: Vec<(String, Option<String>, bool)> = bounds
            .0
            .iter()
            .map(|bound| {
                (
                    bound.scheme.word(),
                    bound.same_as.map(Scheme::word),
                    bound.scoped,
                )
            })
            .collect();

        assert_eq!(
            shown,
            [
                ("Key".into(), None, true),
                ("Bearer".into(), Some("Key".into()), false),
                ("Basic".into(), None, false),
            ]
        );
        let callers: Vec<Option<String>> = group
            .routes
            .iter()
            .map(|route| bounds.caller_of(route).map(Scheme::word))
            .collect();
        assert_eq!(
            callers,
            [Some("Key".into()), Some("Key".into()), Some("Basic".into())]
        );
    }
}

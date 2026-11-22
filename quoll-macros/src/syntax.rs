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
    Api, ArgDecl, Auth, DeclaredScheme, Docs, ErrorList, Fixed, Group, HeaderAttr, Headers,
    MAX_ALTERNATIVES, Method, Route, RouteAuth, RouteDecl, Scheme, Source, Value, headers,
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

/// What `rest::group!` is given: one group, from which the api that lists it builds its router, its client and its
/// document.
#[derive(Debug)]
pub(crate) struct GroupContract(pub(crate) Api);

impl Parse for GroupContract {
    fn parse(input: ParseStream) -> Result<Self> {
        let mut errors = ErrorList::default();

        match read_group(input, &mut errors) {
            Ok(api) => errors.finish().map(|()| Self(api)),
            Err(error) => Err(errors.fail(error)),
        }
    }
}

/// Reads the one group of a `rest::group!`, which may say `#[openapi(none)]`.
fn read_group(input: ParseStream, errors: &mut ErrorList) -> Result<Api> {
    let mut group: Option<(Group, bool)> = None;

    while !input.is_empty() {
        let Attrs {
            docs,
            auth,
            openapi,
            error,
            headers,
        } = parse_attrs(input)?;
        let vis: Visibility = input.parse()?;
        let keyword = input.call(Ident::parse_any).map_err(|error| {
            Error::new(
                error.span(),
                "expected `group`: a `rest::group!` holds one group, `pub group Notes { ... }`",
            )
        })?;

        if keyword == "api" {
            return Err(Error::new(
                keyword.span(),
                "a `rest::group!` holds a group, which an api lists: write the api in a `rest::api!`, as `pub api Api = \"/api\" { notes::Notes };`",
            ));
        }
        if keyword == "scheme" {
            return Err(Error::new(
                keyword.span(),
                "a scheme is declared in the `rest::api!` of the api: name it here by its path, as `#[auth(crate::Key)]`, or import it",
            ));
        }
        if keyword != "group" {
            return Err(unexpected_item(&keyword));
        }

        let document = match openapi {
            Some((_, read)) => keep(errors, read).is_none_or(|document| document),
            None => true,
        };
        let auth = auth.and_then(|(_, auth)| keep(errors, auth));
        let error = error.and_then(|(_, error)| keep(errors, error));
        let headers = keep(errors, headers).unwrap_or_default();
        let read = parse_group(input, docs, vis, auth.as_ref(), error, headers, errors)?;

        match &group {
            Some((first, _)) => errors.push(Error::new(
                read.name.span(),
                format!(
                    "a `rest::group!` holds one group, and `{}` is already it: put `{}` in a `rest::group!` of its own",
                    first.name, read.name
                ),
            )),
            None => group = Some((read, document)),
        }
    }

    let Some((group, document)) = group else {
        return Err(Error::new(
            proc_macro2::Span::call_site(),
            "a `rest::group!` holds a group: write `pub group Notes { ... }`",
        ));
    };

    let mut api = Api::alone(group).map_err(|error| fold(errors, error))?;
    api.openapi = document;
    headers::check(&api.groups, None, errors);

    Ok(api)
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
            error,
            headers,
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
            let error = error.and_then(|(_, error)| keep(errors, error));
            let mut decl = ApiDecl::parse(input, docs, vis, openapi, error, &keyword)?;
            decl.headers = keep(errors, headers).unwrap_or_default();
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
            if let Some((attr, _)) = &error {
                errors.push(Error::new_spanned(
                    attr,
                    "a scheme takes no `#[error(...)]`: put it on the api, a group or a route, whose errors it reads",
                ));
            }
            if !headers.as_ref().is_ok_and(Headers::is_empty) {
                errors.push(Error::new(
                    keyword.span(),
                    "a scheme takes no `#[header(...)]`: put it on the api, a group or a route, whose requests send it",
                ));
            }

            match parse_scheme(input, docs, vis)? {
                Ok(scheme) => schemes.push(scheme),
                Err(error) => errors.push(error),
            }
        } else if keyword == "group" {
            refuse_openapi(errors, openapi.as_ref(), "a group");
            let auth = auth.and_then(|(_, auth)| keep(errors, auth));
            let error = error.and_then(|(_, error)| keep(errors, error));
            let headers = keep(errors, headers).unwrap_or_default();
            groups.push(parse_group(
                input,
                docs,
                vis,
                auth.as_ref(),
                error,
                headers,
                errors,
            )?);
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
    let error = api.error;
    let api_headers = api.headers;
    let mut api = Api::new(
        api.docs,
        api.vis,
        api.name,
        &api.prefix,
        schemes,
        api.listed,
        groups,
    )
    .map_err(|error| fold(errors, error))?;
    api.openapi = openapi;
    api.error = error;
    api.headers = api_headers;
    headers::check(&api.groups, Some((&api, &api.headers)), errors);

    Ok(api)
}

/// Refuses an `#[openapi(...)]` on `what`, which is not the api.
fn refuse_openapi(errors: &mut ErrorList, openapi: Option<&(Attribute, Result<bool>)>, what: &str) {
    if let Some((attr, _)) = openapi {
        errors.push(openapi_elsewhere(attr, what));
    }
}

/// The error of an `#[openapi(...)]` on `what`, which is not the api.
fn openapi_elsewhere(attr: &Attribute, what: &str) -> Error {
    Error::new_spanned(
        attr,
        format!(
            "{what} takes no `#[openapi(...)]`: `#[openapi(none)]` goes on the api, whose whole document it leaves out"
        ),
    )
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
    error: Option<Type>,
    headers: Headers,
    name: Ident,
    prefix: LitStr,
    listed: Vec<syn::Path>,
}

impl ApiDecl {
    /// After the `api` keyword: `Name = "/prefix";`, or `Name = "/prefix" { notes::Notes, ... };` with the groups of
    /// other blocks it lists.
    fn parse(
        input: ParseStream,
        docs: Docs,
        vis: Visibility,
        openapi: bool,
        error: Option<Type>,
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
        let listed = if input.peek(syn::token::Brace) {
            parse_list(input)?
        } else {
            Vec::new()
        };
        input.parse::<Token![;]>()?;

        Ok(Self {
            docs,
            vis,
            openapi,
            error,
            headers: Headers::default(),
            name,
            prefix,
            listed,
        })
    }
}

/// The groups an api lists, `{ auth::Auth, notes::Notes }`: each a path to the group a `rest::group!` writes.
fn parse_list(input: ParseStream) -> Result<Vec<syn::Path>> {
    const WRITE: &str = "an api lists the groups of `rest::group!` blocks by their paths: write `pub api Api = \"/api\" { auth::Auth, notes::Notes };`";

    let content;
    let braces = braced!(content in input);
    let paths = Punctuated::<syn::Path, Token![,]>::parse_terminated_with(&content, |input| {
        input
            .call(syn::Path::parse_mod_style)
            .map_err(|error| Error::new(error.span(), WRITE))
    })?;

    if paths.is_empty() {
        return Err(Error::new(
            braces.span.join(),
            format!("the list of groups is empty: remove the braces, or {WRITE}"),
        ));
    }

    Ok(paths.into_iter().collect())
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

/// After the `group` keyword: `Name { routes }`, whose routes take `auth` unless they say otherwise, and the group's
/// own `#[error(E)]`, `error`, and `#[header(...)]`s, `headers`. A mistake in one route is gathered, the others are
/// still read.
fn parse_group(
    input: ParseStream,
    docs: Docs,
    vis: Visibility,
    auth: Option<&Auth>,
    error: Option<Type>,
    headers: Headers,
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
        error,
        headers,
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
        error,
        headers,
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
        let refused = openapi_elsewhere(attr, "a route");

        match &mut attr_error {
            Some(error) => error.combine(refused),
            None => attr_error = Some(refused),
        }
    }

    let error = match error.map(|(_, error)| error).transpose() {
        Ok(error) => error,
        Err(refused) => {
            match &mut attr_error {
                Some(error) => error.combine(refused),
                None => attr_error = Some(refused),
            }

            None
        }
    };

    let headers = match headers {
        Ok(headers) => headers,
        Err(refused) => {
            match &mut attr_error {
                Some(error) => error.combine(refused),
                None => attr_error = Some(refused),
            }

            Headers::default()
        }
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
            error,
            headers,
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

/// The attributes before an item: its `///` comments, its `#[auth(...)]` with what it says, or the mistake in it, its
/// `#[openapi(...)]` with whether the api has a document, or the mistake in it, its `#[error(...)]` with the type it
/// names, or the mistake in it, and what its `#[header(...)]`s say, or the mistakes in them.
struct Attrs {
    docs: Docs,
    auth: Option<(Attribute, Result<Auth>)>,
    openapi: Option<(Attribute, Result<bool>)>,
    error: Option<(Attribute, Result<Type>)>,
    headers: Result<Headers>,
}

/// The attributes before an item. Any attribute other than a comment, `#[auth(...)]`, `#[openapi(...)]`,
/// `#[error(...)]` or `#[header(...)]` is refused, which stops the reading; a second of any of them but
/// `#[header(...)]`, which an item may have several of, is a mistake of the item's, gathered with the others.
fn parse_attrs(input: ParseStream) -> Result<Attrs> {
    let attrs = input.call(Attribute::parse_outer)?;
    let mut lines = Vec::with_capacity(attrs.len());
    let mut auth: Option<(Attribute, Result<Auth>)> = None;
    let mut openapi: Option<(Attribute, Result<bool>)> = None;
    let mut error: Option<(Attribute, Result<Type>)> = None;
    let mut headers: Vec<HeaderAttr> = Vec::new();
    let mut header_errors = ErrorList::default();

    for attr in attrs {
        if attr.path().is_ident("header") {
            match parse_header(&attr) {
                Ok(header) => headers.push(header),
                Err(error) => header_errors.push(error),
            }
            continue;
        }

        if attr.path().is_ident("error") {
            error = Some(match error {
                None => {
                    let read = parse_error(&attr);

                    (attr, read)
                }
                Some((first, read)) => {
                    let twice = Error::new_spanned(
                        &attr,
                        "an item takes one `#[error(...)]`: remove this one",
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
                "an attribute is not supported here: only `///` comments are kept, `#[auth(...)]` on a group or a route, `#[error(...)]` and `#[header(...)]` on the api, a group or a route, and `#[openapi(none)]` on the api",
            ));
        };

        lines.push(line);
    }

    let headers = match header_errors.finish() {
        Ok(()) => Headers::new(headers),
        Err(error) => Err(error),
    };

    Ok(Attrs {
        docs: Docs(lines),
        auth,
        openapi,
        error,
        headers,
    })
}

/// What a `#[header(...)]` says: a header the item fixes, `#[header("X-GitHub-Api-Version" = "2022-11-28")]`, with a
/// string or a constant's name for its value; one it leaves of what it would take, `#[header("Name" = none)]`; or
/// `#[header(none)]`, which leaves them all.
fn parse_header(attr: &Attribute) -> Result<HeaderAttr> {
    const WRITE: &str = "write `#[header(\"X-GitHub-Api-Version\" = \"2022-11-28\")]`, with a string or a constant's name for the value, `#[header(\"Name\" = none)]` to leave a header, or `#[header(none)]` to leave them all";

    let Meta::List(list) = &attr.meta else {
        return Err(Error::new_spanned(
            attr,
            format!("`#[header]` names a header: {WRITE}"),
        ));
    };
    let refused = || {
        Error::new_spanned(
            &list.tokens,
            format!("`#[header(...)]` takes a header: {WRITE}"),
        )
    };

    list.parse_args_with(|input: ParseStream| {
        if input.peek(Ident) {
            let none: Ident = input.parse()?;

            if none != "none" || !input.is_empty() {
                return Err(refused());
            }

            return Ok(HeaderAttr::LeaveAll(none.span()));
        }

        let name: LitStr = input.parse().map_err(|_| refused())?;
        input.parse::<Token![=]>().map_err(|_| refused())?;
        headers::check_name(&name)?;

        let attr = if input.peek(LitStr) {
            let value: LitStr = input.parse()?;

            headers::check_value(&value)?;

            HeaderAttr::Set(Fixed {
                name,
                value: Value::Literal(value),
            })
        } else {
            let path: syn::Path = input.parse().map_err(|_| refused())?;

            if path.is_ident("none") {
                HeaderAttr::Leave(name)
            } else {
                HeaderAttr::Set(Fixed {
                    name,
                    value: Value::Const(path),
                })
            }
        };

        if !input.is_empty() {
            return Err(refused());
        }

        Ok(attr)
    })
}

/// What an `#[error(...)]` says: the type of the errors an api, a group or a route answers, `#[error(GitHubError)]`.
fn parse_error(attr: &Attribute) -> Result<Type> {
    const WRITE: &str =
        "write `#[error(GitHubError)]`, the type the body of an error answer reads as";

    let Meta::List(list) = &attr.meta else {
        return Err(Error::new_spanned(
            attr,
            format!("`#[error]` names the type of the errors: {WRITE}"),
        ));
    };

    let refused = || {
        Error::new_spanned(
            &list.tokens,
            format!("`#[error(...)]` takes a type: {WRITE}"),
        )
    };
    let ty: Type = syn::parse2(list.tokens.clone()).map_err(|_| refused())?;

    // A type a struct holds, by its path: not `_`, `impl Trait`, `dyn Trait` nor `!`, which no body reads as.
    match &ty {
        Type::Path(_) => Ok(ty),
        _ => Err(refused()),
    }
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
                // Spanned on all that follows `none`, which is what to remove.
                let rest: proc_macro2::TokenStream = input.parse()?;

                return Err(Error::new_spanned(
                    rest,
                    "`#[auth(none)]` takes nothing else: a route with `none` takes no credential, and no option, so write `#[auth(none)]` alone",
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
    fn a_form_wraps_one_type_which_the_model_keeps() {
        let bare = messages(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(login: Form); }
        });
        assert!(
            bare[0].contains("`Form` wraps the type of the form: write `Form<Note>`"),
            "{bare:?}"
        );

        let api = parse(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(login: Form<Login>) -> Created<Note>; }
        })
        .unwrap();
        let route = &api.groups[0].routes[0];
        let inner = |ty: Option<&Type>| ty.map(|ty| quote! { #ty }.to_string());

        assert_eq!(inner(route.params[0].inner.as_ref()), Some("Login".into()));
        assert_eq!(
            inner(
                route
                    .reply
                    .as_deref()
                    .and_then(|returned| returned.inner.as_ref())
            ),
            Some("Note".into())
        );
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

    /// The shape of each route's errors, by group, as the client names it: empty for a problem.
    fn errors(api: &Api) -> Vec<Vec<String>> {
        let block = api.name != "__QuollAlone";

        api.groups
            .iter()
            .map(|group| {
                let cx = crate::descriptor::Cx {
                    group,
                    api: block.then_some(api),
                    openapi: true,
                };

                group
                    .routes
                    .iter()
                    .map(|route| cx.error(route).map(text).unwrap_or_default())
                    .collect()
            })
            .collect()
    }

    #[test]
    fn a_route_takes_the_nearest_error_shape() {
        let api = parse(quote! {
            #[error(HubError)]
            pub api Api = "";

            pub group Repos {
                GET "/repos" => list();
                #[error(crate::OAuthError)]
                POST "/token" => token();
            }

            #[error(rest::Problem)]
            pub group Mirror {
                GET "/mirror" => get();
                #[error(MirrorError)]
                GET "/mirror/status" => status();
            }
        })
        .unwrap();
        let alone = syn::parse2::<GroupContract>(quote! {
            pub group Gists {
                GET "/gists" => list();
                #[error(HubError)]
                GET "/gists/starred" => starred();
            }
        })
        .unwrap()
        .0;

        assert_eq!(
            errors(&api),
            [
                vec!["HubError", "crate :: OAuthError"],
                vec!["rest :: Problem", "MirrorError"]
            ]
        );
        assert_eq!(errors(&alone), [vec!["", "HubError"]]);
    }

    #[test]
    fn error_names_one_type_on_the_api_a_group_or_a_route() {
        assert_eq!(
            messages(quote! {
                #[error(A)]
                #[error(B)]
                pub api Api = "";

                #[error]
                pub group G { #[error("A")] GET "/a" => a(); }

                #[error(A)]
                pub scheme Key = header "X-Key";

                #[error(_)]
                pub group H { #[error(impl Display)] GET "/h" => h(); }
            }),
            [
                "an item takes one `#[error(...)]`: remove this one",
                "`#[error]` names the type of the errors: write `#[error(GitHubError)]`, the type the body of an error answer reads as",
                "`#[error(...)]` takes a type: write `#[error(GitHubError)]`, the type the body of an error answer reads as",
                "a scheme takes no `#[error(...)]`: put it on the api, a group or a route, whose errors it reads",
                "`#[error(...)]` takes a type: write `#[error(GitHubError)]`, the type the body of an error answer reads as",
                "`#[error(...)]` takes a type: write `#[error(GitHubError)]`, the type the body of an error answer reads as",
            ]
        );
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
    fn two_routes_of_one_operation_id_are_refused() {
        let errors = messages(quote! {
            pub api Api = "/api";
            pub group Note {
                GET "/a" => tags_list();
            }
            pub group NoteTags {
                GET "/b" => list();
                GET "/c" => other();
            }
        });

        assert_eq!(errors.len(), 1, "{errors:?}");
        assert!(
            errors[0].contains("the operation id `note_tags_list` of the route `list` of `NoteTags` is already the one of the route `tags_list` of `Note`"),
            "{errors:?}"
        );
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

    /// The headers each route of each group sends, its own then its api's, as `name: value`, the constants by their
    /// names.
    fn sent(api: &Api) -> Vec<Vec<Vec<String>>> {
        let block = api.name != "__QuollAlone";

        api.groups
            .iter()
            .map(|group| {
                group
                    .routes
                    .iter()
                    .map(|route| {
                        let headers = crate::model::headers::RouteHeaders::of(group, route);

                        headers
                            .sent(block.then_some(&api.headers))
                            .into_iter()
                            .map(|fixed| {
                                let value = match &fixed.value {
                                    Value::Literal(value) => value.value(),
                                    Value::Const(path) => text(&path),
                                };

                                format!("{}: {value}", fixed.name.value())
                            })
                            .collect()
                    })
                    .collect()
            })
            .collect()
    }

    #[test]
    fn a_route_sends_the_nearest_fixed_headers() {
        let api = parse(quote! {
            #[header("Accept" = "application/vnd.github+json")]
            #[header("X-GitHub-Api-Version" = VERSION)]
            #[header("X-Beta" = "files")]
            pub api Api = "";

            #[header("X-Group" = "repos")]
            pub group Repos {
                GET "/repos" => list();
                #[header("accept" = "application/vnd.github.raw+json")]
                #[header("X-Beta" = none)]
                GET "/readme" => readme();
                #[header(none)]
                GET "/health" => health();
            }

            #[header("X-Beta" = none)]
            #[header("X-Group" = "gists")]
            pub group Gists {
                GET "/gists" => list();
            }
        })
        .unwrap();

        assert_eq!(
            sent(&api),
            [
                vec![
                    vec![
                        "X-Group: repos",
                        "Accept: application/vnd.github+json",
                        "X-GitHub-Api-Version: VERSION",
                        "X-Beta: files"
                    ],
                    vec![
                        "accept: application/vnd.github.raw+json",
                        "X-Group: repos",
                        "X-GitHub-Api-Version: VERSION"
                    ],
                    vec![],
                ],
                vec![vec![
                    "X-Group: gists",
                    "Accept: application/vnd.github+json",
                    "X-GitHub-Api-Version: VERSION"
                ]],
            ]
        );
    }

    #[test]
    fn a_header_is_named_once_by_an_item_and_none_goes_below_the_api() {
        assert_eq!(
            messages(quote! {
                #[header("Accept" = none)]
                pub api Api = "";

                #[header("X-Version" = "1")]
                #[header("x-version" = "2")]
                pub group G { GET "/g" => g(); }

                #[header(none)]
                #[header(none)]
                #[header("X-Beta" = none)]
                pub group H { GET "/h" => h(); }

                #[header("X-Key" = "1")]
                pub scheme Key = header "X-Key";
            }),
            [
                "the header `x-version` is already named here: an item names a header once",
                "an item takes one `#[header(none)]`: remove this one",
                "`#[header(none)]` already leaves every header this item would take, `X-Beta` among them: remove this `none`",
                "a scheme takes no `#[header(...)]`: put it on the api, a group or a route, whose requests send it",
                "an api takes no header to leave: `none` goes on a group or a route, which leaves what its api or its group fixes",
            ]
        );
    }

    #[test]
    fn header_takes_a_name_and_a_value_a_header_takes() {
        assert_eq!(
            messages(quote! {
                pub api Api = "";

                #[header]
                pub group A { GET "/a" => a(); }

                #[header(Accept = "json")]
                pub group B { GET "/b" => b(); }

                #[header("X Version" = "1")]
                pub group C { GET "/c" => c(); }

                #[header("Content-Length" = "1")]
                pub group D { GET "/d" => d(); }

                #[header("X-Version" = " 1")]
                pub group E { GET "/e" => e(); }

                #[header("X-Version" = 1)]
                pub group F { GET "/f" => f(); }

                #[header(none, "X-Version")]
                pub group H { GET "/h" => h(); }
            }),
            [
                "`#[header]` names a header: write `#[header(\"X-GitHub-Api-Version\" = \"2022-11-28\")]`, with a string or a constant's name for the value, `#[header(\"Name\" = none)]` to leave a header, or `#[header(none)]` to leave them all",
                "`#[header(...)]` takes a header: write `#[header(\"X-GitHub-Api-Version\" = \"2022-11-28\")]`, with a string or a constant's name for the value, `#[header(\"Name\" = none)]` to leave a header, or `#[header(none)]` to leave them all",
                "`X Version` is not the name of a header: a name is ASCII letters, digits and the signs ! # $ % & ' * + - . ^ _ ` | ~, as in \"X-GitHub-Api-Version\"",
                "the header `Content-Length` is written by quoll or by HTTP itself, so a contract does not fix it",
                "a header's value is visible ASCII, with spaces and tabs inside it only, which HTTP drops at either end: give the value as it is sent",
                "`#[header(...)]` takes a header: write `#[header(\"X-GitHub-Api-Version\" = \"2022-11-28\")]`, with a string or a constant's name for the value, `#[header(\"Name\" = none)]` to leave a header, or `#[header(none)]` to leave them all",
                "`#[header(...)]` takes a header: write `#[header(\"X-GitHub-Api-Version\" = \"2022-11-28\")]`, with a string or a constant's name for the value, `#[header(\"Name\" = none)]` to leave a header, or `#[header(none)]` to leave them all",
            ]
        );
    }

    #[test]
    fn none_leaves_only_a_header_the_parent_fixes() {
        assert_eq!(
            messages(quote! {
                #[header("X-Beta" = "files")]
                pub api Api = "";

                #[header("X-Bta" = none)]
                pub group A {
                    #[header("X-Beta" = none)]
                    GET "/a" => a();
                }

                #[header(none)]
                pub group B {
                    #[header("X-Beta" = none)]
                    GET "/b" => b();
                }

                pub group C {
                    #[header("X-Group" = none)]
                    GET "/c" => c();
                }
            }),
            [
                "the api `Api` fixes no header `X-Bta`, so the group has none to leave: check its name, or remove this `none`",
                "neither the group `B` nor the api `Api` gives the route a header `X-Beta`, so it has none to leave: check its name, or remove this `none`",
                "neither the group `C` nor the api `Api` gives the route a header `X-Group`, so it has none to leave: check its name, or remove this `none`",
            ]
        );
    }

    #[test]
    fn a_group_alone_leaves_what_its_api_may_fix() {
        let alone = syn::parse2::<GroupContract>(quote! {
            #[header("X-Beta" = none)]
            pub group Gists {
                #[header("X-Version" = none)]
                GET "/gists" => list();
            }
        });

        assert!(alone.is_ok());

        let refused = syn::parse2::<GroupContract>(quote! {
            #[header("X-Beta" = none)]
            pub group Gists {
                #[header("X-Beta" = none)]
                GET "/gists" => list();
            }
        })
        .expect_err("the group leaves the header already");

        assert_eq!(
            refused.to_string(),
            "neither the group `Gists` gives the route a header `X-Beta`, so it has none to leave: check its name, or remove this `none`"
        );
    }

    #[test]
    fn a_fixed_content_type_keeps_the_bodys_media_type() {
        assert_eq!(
            messages(quote! {
                #[header("Content-Type" = "application/json; charset=utf-8")]
                pub api Api = "";

                pub group A {
                    GET "/a" => list();
                    POST "/a" => create(note: Note);
                    POST "/a/form" => form(login: Form<Login>);
                    POST "/a/bytes" => upload(file: Bytes);
                    #[header("Content-Type" = none)]
                    POST "/a/raw" => raw(file: Bytes);
                    #[header("Content-Type" = "text/plain")]
                    POST "/a/text" => text(note: Note);
                    #[header("Content-Type" = "text/plain; charset=utf-8")]
                    POST "/a/plain" => plain(note: Text);
                    #[header("Content-Type" = "text/plain; charset=latin1")]
                    POST "/a/latin" => latin(note: Text);
                    #[header("Content-Type" = "application/json; charset")]
                    POST "/a/param" => param(note: Note);
                    #[header("Content-Type" = "application/json")]
                    GET "/a/none" => none();
                }
            }),
            [
                "the `Content-Type` `application/json; charset=utf-8` of the api `Api` is not the media type of the route's body, `application/x-www-form-urlencoded`, in UTF-8, which a fixed one keeps, adding a parameter as `application/x-www-form-urlencoded; charset=utf-8`: write `#[header(\"Content-Type\" = none)]` on the route",
                "the route's body, `Bytes`, carries its content type, which a fixed `Content-Type` of the api `Api` would not keep: write `#[header(\"Content-Type\" = none)]` on the route, and give the `Bytes` its content type",
                "the `Content-Type` `text/plain` is not the media type of the route's body, `application/json`, in UTF-8, which a fixed one keeps, adding a parameter as `application/json; charset=utf-8`: write `application/json`, with the parameters the api asks for",
                "the `Content-Type` `text/plain; charset=latin1` is not the media type of the route's body, `text/plain`, in UTF-8, which a fixed one keeps, adding a parameter as `text/plain; charset=utf-8`: write `text/plain`, with the parameters the api asks for",
                "the `Content-Type` `application/json; charset` is not the media type of the route's body, `application/json`, in UTF-8, which a fixed one keeps, adding a parameter as `application/json; charset=utf-8`: write `application/json`, with the parameters the api asks for",
                "the route sends no body, and so no `Content-Type`: remove this header",
            ]
        );
    }

    #[test]
    fn a_fixed_header_carries_no_credential_of_the_routes_schemes() {
        assert_eq!(
            messages(quote! {
                #[header("X-Api-Key" = "public")]
                pub api Api = "";

                pub scheme Key = header "X-Api-Key";
                pub scheme Session = cookie "session";

                pub group A {
                    #[auth(Key)]
                    GET "/a" => a();
                    #[auth(Bearer)]
                    #[header("Authorization" = "Bearer public")]
                    GET "/b" => b();
                    #[auth(Session)]
                    #[header("Cookie" = "theme=dark")]
                    GET "/c" => c();
                    #[auth(Session)]
                    #[header("Authorization" = "Token public")]
                    GET "/e" => e();
                    GET "/d" => d();
                }
            }),
            [
                "the route `A::a` takes the header `X-Api-Key` of the api `Api`, which carries the credential of `Key`: the client's middleware sends it, so write `#[header(\"X-Api-Key\" = none)]` on the route",
                "the header `Authorization` carries the credential of `Bearer`, which the route takes: the client's middleware sends it, so the contract does not fix it",
                "the header `Cookie` carries the credential of `Session`, which the route takes: the client's middleware sends it, so the contract does not fix it",
                "the header `Authorization` carries the credentials of a route that takes a scheme, as it takes `Session`, which the route takes: the client's middleware sends it, so the contract does not fix it",
            ]
        );
    }
}

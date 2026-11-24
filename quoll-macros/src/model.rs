//! The model of an api: what the generators read, whatever the spelling it was written in.
//!
//! The `quoll::api!` syntax reads into it (`syntax.rs`); a spelling with attributes on traits could build the same
//! values, and the generators would not know. Nothing here depends on how a contract looks: only on what it says.
//! Every constructor checks what a spelling cannot get wrong without the mistake being a contract's: names matched,
//! one body, no route twice.

use proc_macro2::Span;
use syn::{Attribute, Error, Ident, LitStr, Result, Type, Visibility, ext::IdentExt};

use crate::known::{BodyKind, Known, ResultKind};
use crate::path::{Segment, Template, literal_problem};

mod auth;
pub(crate) mod fields;
pub(crate) mod headers;

pub(crate) use auth::{
    Auth, Bounds, DeclaredKind, DeclaredScheme, Location, MAX_ALTERNATIVES, RouteAuth, Scheme,
    SchemeKind,
};
pub(crate) use fields::{BodyField, FieldValue};
pub(crate) use headers::{Fixed, HeaderAttr, Headers, Value};

/// The `///` comments of an item, one string per line, as rustc gives them (with their leading space).
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub(crate) struct Docs(pub(crate) Vec<String>);

/// A whole api: its type, its prefix and its groups.
#[derive(Debug)]
pub(crate) struct Api {
    /// The comments on the api.
    pub(crate) docs: Docs,
    /// The visibility of the type the api becomes.
    pub(crate) vis: Visibility,
    /// The name of that type.
    pub(crate) name: Ident,
    /// What every path starts with: empty, or a `/` and segments with no `{name}`.
    pub(crate) prefix: String,
    /// The schemes the contract declares, in the order they are written.
    pub(crate) schemes: Vec<DeclaredScheme>,
    /// The groups of other blocks the api lists, `{ auth::Auth, notes::Notes }`, by their paths, in the order of the
    /// list.
    pub(crate) listed: Vec<syn::Path>,
    /// The groups the block writes, in the order they are written.
    pub(crate) groups: Vec<Group>,
    /// Whether the api has an OpenAPI document, `Api::openapi`: not with `#[openapi(none)]`, for a served api with no
    /// document, nor with `#[server(none)]`, as the document describes what is served.
    pub(crate) openapi: bool,
    /// Whether the contract has a server side, written when `quoll` has the feature `server`: not with
    /// `#[server(none)]`, for a third-party API that is only called.
    pub(crate) server: bool,
    /// Whether the contract has a client side, written when `quoll` has the feature `client`: not with
    /// `#[client(none)]`, for an API that is only served.
    pub(crate) client: bool,
    /// The shape of the errors of its routes, its `#[error(E)]`, which the groups of its block take unless they or
    /// their routes name their own; `None` for an RFC 9457 problem.
    pub(crate) error: Option<Type>,
    /// Its `#[header(...)]`s, which every route takes unless its group or itself leaves them.
    pub(crate) headers: Headers,
}

/// What an api, or the group of a `quoll::group!`, has of a contract: its document and its two sides.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct Sides {
    /// Whether it has an OpenAPI document.
    pub(crate) openapi: bool,
    /// Whether it has a server side.
    pub(crate) server: bool,
    /// Whether it has a client side.
    pub(crate) client: bool,
}

/// Routes that go together: one service on the server, one trait on the client, one tag in OpenAPI.
#[derive(Debug)]
pub(crate) struct Group {
    /// The comments on the group.
    pub(crate) docs: Docs,
    /// The visibility of what the group becomes.
    pub(crate) vis: Visibility,
    /// The name of the group.
    pub(crate) name: Ident,
    /// The group's own `#[error(E)]`, which its routes take unless they name their own.
    pub(crate) error: Option<Type>,
    /// The group's own `#[header(...)]`s, which its routes take unless they leave them.
    pub(crate) headers: Headers,
    /// The routes, in the order they are written.
    pub(crate) routes: Vec<Route>,
}

/// An HTTP method of a route.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum Method {
    /// `GET`.
    Get,
    /// `POST`.
    Post,
    /// `PUT`.
    Put,
    /// `PATCH`.
    Patch,
    /// `DELETE`.
    Delete,
    /// `HEAD`.
    Head,
    /// `OPTIONS`.
    Options,
}

impl Method {
    /// Every method, in the order the spec lists them.
    pub(crate) const ALL: [Self; 7] = [
        Self::Get,
        Self::Post,
        Self::Put,
        Self::Patch,
        Self::Delete,
        Self::Head,
        Self::Options,
    ];

    /// The method written as HTTP does.
    pub(crate) const fn as_str(self) -> &'static str {
        match self {
            Self::Get => "GET",
            Self::Post => "POST",
            Self::Put => "PUT",
            Self::Patch => "PATCH",
            Self::Delete => "DELETE",
            Self::Head => "HEAD",
            Self::Options => "OPTIONS",
        }
    }

    /// The method named `text`, in capitals.
    pub(crate) fn from_name(text: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|method| method.as_str() == text)
    }

    /// Whether a request with this method carries a body.
    pub(crate) const fn takes_body(self) -> bool {
        matches!(self, Self::Post | Self::Put | Self::Patch | Self::Delete)
    }
}

/// A route: a method and a path that become a function.
#[derive(Debug)]
pub(crate) struct Route {
    /// The comments on the route.
    pub(crate) docs: Docs,
    /// The HTTP method.
    pub(crate) method: Method,
    /// The name of the function the route becomes.
    pub(crate) name: Ident,
    /// The segments of the path, below the api's prefix.
    pub(crate) path: Vec<Segment>,
    /// The arguments, in the order they are written, each with the place it is read from.
    pub(crate) params: Vec<Param>,
    /// The indices in `params` of the path's parameters, in the order of the path.
    pub(crate) path_order: Vec<usize>,
    /// What the route returns: nothing, a 204 with no body, or a type.
    pub(crate) reply: Option<Box<Returned>>,
    /// The authentication the route takes: its own attribute's, else its group's; `None` when it takes no
    /// credential.
    pub(crate) auth: Option<RouteAuth>,
    /// The route's own `#[error(E)]`, which wins over its group's and its api's.
    pub(crate) error: Option<Type>,
    /// The route's own `#[header(...)]`s, which win over its group's and its api's.
    pub(crate) headers: Headers,
    /// The fixed fields of its body, `#[body("name" = value)]`, in the order they are written.
    pub(crate) fields: Vec<BodyField>,
    /// The span of the route's string, where a mistake about the route as a whole is pointed.
    pub(crate) span: Span,
}

impl Route {
    /// The route's path under the api's `prefix`, its parameters written `{name}`: `/api/notes/{id}`. The route `"/"`
    /// is the prefix itself, `/api`, or `/` when there is none.
    pub(crate) fn full_path(&self, prefix: &str) -> String {
        let mut path = prefix.to_owned();

        if self.path.is_empty() && prefix.is_empty() {
            path.push('/');
        }
        for segment in &self.path {
            path.push('/');
            match segment {
                Segment::Literal(text) => path.push_str(text),
                Segment::Param(name) => {
                    path.push('{');
                    path.push_str(name);
                    path.push('}');
                }
            }
        }

        path
    }

    /// The operation id, the group's name in snake case and the function's: `notes_get`.
    pub(crate) fn operation_id(&self, group: &Group) -> String {
        format!("{}_{}", snake_case(&group.name), self.name.unraw())
    }

    /// The path's parameters, in the order of the path.
    pub(crate) fn path_params(&self) -> impl Iterator<Item = &Param> {
        self.path_order.iter().map(|&index| &self.params[index])
    }

    /// The query's parameters, each with its key on the wire, in the order they are written.
    pub(crate) fn query_params(&self) -> impl Iterator<Item = (&Param, &str)> {
        self.params.iter().filter_map(|param| match &param.kind {
            ParamKind::Query { key, .. } => Some((param, key.as_str())),
            _ => None,
        })
    }

    /// The query struct, if the route has one.
    pub(crate) fn query_struct(&self) -> Option<&Param> {
        self.params
            .iter()
            .find(|param| param.kind == ParamKind::QueryStruct)
    }

    /// The headers, each with its name as the contract writes it, in the order they are written.
    pub(crate) fn headers(&self) -> impl Iterator<Item = (&Param, &str)> {
        self.params.iter().filter_map(|param| match &param.kind {
            ParamKind::Header { name } => Some((param, name.as_str())),
            _ => None,
        })
    }

    /// The argument read from the body, with what the body is, if the route has one.
    pub(crate) fn body(&self) -> Option<(&Param, BodyKind)> {
        self.params.iter().find_map(|param| match param.kind {
            ParamKind::Body(kind) => Some((param, kind)),
            _ => None,
        })
    }
}

/// An argument of a route.
#[derive(Debug)]
pub(crate) struct Param {
    /// The name of the argument.
    pub(crate) name: Ident,
    /// Its type, as the server receives it: each `&X` of the contract replaced by its owned type.
    pub(crate) ty: Type,
    /// Its type as the contract writes it, which the client takes.
    pub(crate) written: Type,
    /// Why the server cannot receive it owned, a `&mut X` or a `&dyn Trait`, which the server's code refuses.
    pub(crate) refused: Option<Error>,
    /// Where its value is read from.
    pub(crate) kind: ParamKind,
    /// The type a body of quoll's wraps: the `T` of a `Form<T>`.
    pub(crate) inner: Option<Type>,
}

impl Param {
    /// The separator of its `#[join(sep)]`, for a query key that has one.
    pub(crate) fn join(&self) -> Option<Join> {
        match self.kind {
            ParamKind::Query { join, .. } => join,
            _ => None,
        }
    }
}

/// How `#[join(sep)]` writes a list as one query value: the three styles OpenAPI describes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Join {
    /// `#[join(",")]`: `labels=bug,docs`, `style: form, explode: false`.
    Comma,
    /// `#[join(" ")]`: `labels=bug%20docs`, `style: spaceDelimited`.
    Space,
    /// `#[join("|")]`: `labels=bug|docs`, `style: pipeDelimited`.
    Pipe,
}

impl Join {
    /// The separator a contract writes, `","`, `" "` or `"|"`.
    pub(crate) fn from_separator(separator: &str) -> Option<Self> {
        match separator {
            "," => Some(Self::Comma),
            " " => Some(Self::Space),
            "|" => Some(Self::Pipe),
            _ => None,
        }
    }

    /// What serde's `deserialize_with` names, whose `T` it infers from the field: `::quoll::__private::join::comma`.
    pub(crate) fn deserialize_with(self) -> &'static str {
        match self {
            Self::Comma => "::quoll::__private::join::comma",
            Self::Space => "::quoll::__private::join::space",
            Self::Pipe => "::quoll::__private::join::pipe",
        }
    }

    /// The variant of `quoll::__private::join::Join` the generated code names.
    pub(crate) fn variant(self) -> &'static str {
        match self {
            Self::Comma => "Comma",
            Self::Space => "Space",
            Self::Pipe => "Pipe",
        }
    }
}

/// `#[join(sep)]` on an argument, with the attribute, where a mistake about it is pointed.
#[derive(Debug)]
pub(crate) struct JoinAttr {
    /// The separator it names.
    pub(crate) join: Join,
    /// The attribute as written.
    pub(crate) attr: Attribute,
}

/// Where an argument comes from on the wire.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ParamKind {
    /// A `{name}` of the path.
    Path,
    /// A `key={name}` of the query. An `Option` may be missing; a `Vec` repeats the key, or is joined into one value.
    Query {
        /// The key on the wire.
        key: String,
        /// The separator of `#[join(sep)]`, which writes the `Vec` as one value.
        join: Option<Join>,
    },
    /// `query name: Type`: the whole query, a field per key.
    QueryStruct,
    /// `header("Name") name: Type`: a header, missing when its type is an `Option`.
    Header {
        /// The header's name, as the contract writes it.
        name: String,
    },
    /// The body.
    Body(BodyKind),
}

/// What a body of type `ty` is: its JSON, unless the last segment of its path names a body the macro recognizes.
fn body_kind(ty: &Type) -> BodyKind {
    Known::of(ty)
        .and_then(|known| known.body)
        .unwrap_or(BodyKind::JSON)
}

/// The type a route returns: its JSON, or a type of quoll's the macro recognizes as a result.
#[derive(Debug)]
pub(crate) struct Returned {
    /// The type as the contract writes it, which the server's trait returns.
    pub(crate) served: Type,
    /// The type the client returns: the one `served` wraps for a `Created<T>` or an `Accepted<T>`, whose status is
    /// only the server's answer, else `served`.
    pub(crate) value: Type,
    /// The type of quoll's it is, a result, which the generated code checks; `None` for JSON.
    pub(crate) known: Option<&'static Known>,
    /// The type it wraps, when it is a type of quoll's that wraps one: the `T` of `Created<T>` or `Events<T>`.
    pub(crate) inner: Option<Type>,
}

impl Returned {
    /// How the server answers it and the client reads it.
    pub(crate) fn kind(&self) -> ResultKind {
        self.known
            .and_then(|known| known.result)
            .unwrap_or(ResultKind::JSON)
    }
}

/// An argument as a spelling writes it.
#[derive(Debug)]
pub(crate) struct ArgDecl {
    /// Where it says its value comes from.
    pub(crate) source: Source,
    /// Its name.
    pub(crate) name: Ident,
    /// Its type, as the server receives it: each `&X` of the contract replaced by its owned type.
    pub(crate) ty: Type,
    /// Its type as the contract writes it, which the client takes.
    pub(crate) written: Type,
    /// Why the server cannot receive it owned, which the server's code refuses.
    pub(crate) refused: Option<Error>,
    /// Its `#[join(sep)]`, if it has one.
    pub(crate) join: Option<JoinAttr>,
}

/// Where an argument says its value comes from.
#[derive(Debug)]
pub(crate) enum Source {
    /// Nowhere: its name places it, in the path, in the query, or else in the body.
    Named,
    /// `query name: Type`, a query struct; the span is the `query`'s.
    Query(Span),
    /// `header("Name") name: Type`, with the header's name.
    Header(LitStr),
}

/// A route as a spelling writes it, before its arguments are matched to its string.
#[derive(Debug)]
pub(crate) struct RouteDecl {
    /// The comments on the route.
    pub(crate) docs: Docs,
    /// The method.
    pub(crate) method: Method,
    /// The string with the path and the query.
    pub(crate) path: LitStr,
    /// The name of the function.
    pub(crate) name: Ident,
    /// The arguments, in the order they are written.
    pub(crate) args: Vec<ArgDecl>,
    /// The type after `->`, if there is one.
    pub(crate) reply: Option<Type>,
    /// The route's own `#[auth(...)]`, if it has one.
    pub(crate) auth: Option<Auth>,
    /// The route's own `#[error(E)]`, if it has one.
    pub(crate) error: Option<Type>,
    /// The route's own `#[header(...)]`s.
    pub(crate) headers: Headers,
    /// Its `#[body(...)]`s, each name once.
    pub(crate) fields: Vec<BodyField>,
}

impl Route {
    /// Matches the arguments of a route to its string, by name: a `{name}` of the path, a `key={name}` of the query,
    /// and the one argument left is the body. The route takes its own authentication, or else `group_auth`, its
    /// group's.
    ///
    /// # Errors
    ///
    /// An error spanned on the argument or the string at fault: an argument twice, a `{name}` with no argument or
    /// whose argument says it comes from elsewhere, a path argument or a header that is not read from text, a query
    /// struct beside a query in the string or another query struct, a header name that is not one or that is read
    /// twice, an argument that is nowhere, a second body or a body the method does not take, a result that is not
    /// supported, fixed fields on a route with no body or with a `Text` or `Bytes` body.
    pub(crate) fn resolve(decl: RouteDecl, group_auth: Option<&Auth>) -> Result<Self> {
        let RouteDecl {
            docs,
            method,
            path,
            name,
            args,
            reply,
            auth,
            error,
            headers,
            fields,
        } = decl;
        let template = Template::parse(&path)?;

        // A route's attribute replaces its group's.
        let auth = match auth.as_ref().or(group_auth) {
            Some(Auth::Takes(auth)) => Some(auth.clone()),
            Some(Auth::None) | None => None,
        };

        check_names_unique(&args)?;
        let params = match_args(method, &path, &template, auth.as_ref(), args)?;
        let path_order = template
            .path_names()
            .map(|path_name| {
                params
                    .iter()
                    .position(|param| param.name.unraw() == path_name)
                    .expect("`match_args` gave each `{name}` of the path its argument")
            })
            .collect();
        let reply = Returned::read(method, reply)?;
        let body = params.iter().find_map(|param| match param.kind {
            ParamKind::Body(kind) => Some(kind),
            _ => None,
        });
        fields::check_body(&fields, body)?;

        Ok(Self {
            docs,
            method,
            name,
            path: template.segments,
            params,
            path_order,
            reply,
            auth,
            error,
            headers,
            fields,
            span: path.span(),
        })
    }
}

fn check_names_unique(args: &[ArgDecl]) -> Result<()> {
    for (index, arg) in args.iter().enumerate() {
        let name = &arg.name;
        let twice = args[..index]
            .iter()
            .any(|earlier| earlier.name.unraw() == name.unraw());
        if twice {
            return Err(Error::new(
                name.span(),
                format!("the argument `{name}` is written twice: each argument has its own name"),
            ));
        }
    }

    Ok(())
}

fn match_args(
    method: Method,
    path: &LitStr,
    template: &Template,
    auth: Option<&RouteAuth>,
    args: Vec<ArgDecl>,
) -> Result<Vec<Param>> {
    let find = |name: &str| args.iter().find(|arg| arg.name.unraw() == name);

    for arg in &args {
        if let Some(attr) = &arg.join {
            check_join(attr, arg, template)?;
        }
    }
    for name in template.path_names() {
        let Some(arg) = find(name) else {
            return Err(missing_argument(path, name, &Place::Path));
        };
        check_named(arg, &format!("the path's `{{{name}}}`"))?;
        check_read_from_text(&arg.ty, &format!("the path parameter `{name}`"))?;
    }
    for item in &template.query {
        let Some(arg) = find(&item.name) else {
            return Err(missing_argument(path, &item.name, &Place::Query(&item.key)));
        };
        check_named(
            arg,
            &format!("the query's `{}={{{}}}`", item.key, item.name),
        )?;
    }

    let mut params = Vec::with_capacity(args.len());
    let mut query: Option<Ident> = None;
    let mut headers: Vec<LitStr> = Vec::new();
    let mut body: Option<Ident> = None;

    for ArgDecl {
        source,
        name,
        ty,
        written,
        refused,
        join,
    } in args
    {
        let key = name.unraw().to_string();
        let kind = match source {
            Source::Query(span) => {
                check_query_struct(span, template, &name, &ty, query.as_ref())?;
                query = Some(name.clone());
                ParamKind::QueryStruct
            }
            Source::Header(header) => {
                check_header(&header, &headers, auth)?;
                check_header_type(&ty, &header.value())?;

                let kind = ParamKind::Header {
                    name: header.value(),
                };

                headers.push(header);
                kind
            }
            Source::Named if template.path_names().any(|path_name| path_name == key) => {
                ParamKind::Path
            }
            Source::Named => match template.query.iter().find(|item| item.name == key) {
                Some(item) => ParamKind::Query {
                    key: item.key.clone(),
                    join: join.as_ref().map(|attr| attr.join),
                },
                None => {
                    check_body(method, &name, &ty, body.as_ref())?;
                    body = Some(name.clone());
                    ParamKind::Body(body_kind(&ty))
                }
            },
        };
        let inner = match (&kind, Known::of(&ty)) {
            (ParamKind::Body(_), Some(known)) => known
                .wraps
                .map(|what| wrapped(&ty, known.name, what))
                .transpose()?,
            _ => None,
        };
        params.push(Param {
            name,
            ty,
            written,
            refused,
            kind,
            inner,
        });
    }

    Ok(params)
}

/// A `{name}` of the path or the query is the argument of that name, which says nowhere else it comes from.
fn check_named(arg: &ArgDecl, place: &str) -> Result<()> {
    let what = match &arg.source {
        Source::Named => return Ok(()),
        Source::Query(_) => "the query struct",
        Source::Header(_) => "a header",
    };
    let name = &arg.name;

    Err(Error::new(
        name.span(),
        format!(
            "`{name}` is {what}, and {place} names it too: an argument comes from one place, so rename one of them"
        ),
    ))
}

/// `#[join(sep)]` is on a key of the route's query whose type is a `Vec` of a type read from text. Checked before
/// anything else of the argument, so that the mistake is pointed at the attribute.
fn check_join(attr: &JoinAttr, arg: &ArgDecl, template: &Template) -> Result<()> {
    let ArgDecl { name, ty, .. } = arg;
    let key = name.unraw().to_string();
    let place = match &arg.source {
        Source::Query(_) => Some("the query struct".to_owned()),
        Source::Header(_) => Some("a header".to_owned()),
        Source::Named if template.path_names().any(|path_name| path_name == key) => {
            Some(format!("the path's `{{{key}}}`"))
        }
        Source::Named if template.query.iter().any(|item| item.name == key) => None,
        Source::Named => Some("not in the route's string".to_owned()),
    };
    if let Some(place) = place {
        return Err(Error::new_spanned(
            &attr.attr,
            format!(
                "`#[join]` writes a list as the one value of a query key, and `{key}` is {place}: write it as a `key={{{key}}}` of the route's query, or remove the `#[join]`"
            ),
        ));
    }

    check_joined_type(attr, &key, ty)
}

/// The type of an argument that `#[join(sep)]` writes, `what`, is a `Vec` of a type read from text.
fn check_joined_type(attr: &JoinAttr, what: &str, ty: &Type) -> Result<()> {
    let Some(element) = vec_inner(ty) else {
        return Err(Error::new_spanned(
            &attr.attr,
            format!(
                "`#[join]` writes a list, which `{what}` is not: make it a `Vec`, as `{what}: Vec<String>`"
            ),
        ));
    };

    check_read_from_text(element, &format!("each value of `{what}`"))
}

/// A query struct is the whole query: the string has none, and the route no other query struct.
fn check_query_struct(
    span: Span,
    template: &Template,
    name: &Ident,
    ty: &Type,
    earlier: Option<&Ident>,
) -> Result<()> {
    if !template.query.is_empty() {
        return Err(Error::new(
            span,
            format!(
                "`{name}` is the whole query, and the route's string has a query too: remove the `?...` from the string and make its keys fields of the struct, or write `{name}` as an argument of the string's query"
            ),
        ));
    }
    if let Some(earlier) = earlier {
        return Err(Error::new(
            span,
            format!(
                "a route has one query struct, and `{earlier}` is already it: put the fields of `{name}` in it"
            ),
        ));
    }

    let is_struct = match peel(ty) {
        Type::Path(path) => path.qself.is_none(),
        _ => false,
    };
    let reserved = not_text(ty) || reserved_name(ty, SCALARS).is_some();

    if is_struct && !reserved {
        return Ok(());
    }

    Err(Error::new_spanned(
        ty,
        format!(
            "the query struct `{name}` is read with a field per key, which this type is not: write a struct of your own, as `query {name}: NoteFilter`"
        ),
    ))
}

/// The headers that quoll writes and reads itself, or that HTTP keeps to one connection, which no argument takes: a
/// value given by the caller would clash with quoll's own or make the request malformed.
const RESERVED_HEADERS: &[&str] = &[
    "Content-Type",
    "Content-Length",
    "Transfer-Encoding",
    "Host",
    "Connection",
    "Keep-Alive",
    "Proxy-Connection",
    "TE",
    "Trailer",
    "Upgrade",
];

/// A header's name is one HTTP takes and that quoll leaves to the route, and the route reads each header once.
/// `Authorization` is the scheme's, on a route that takes one.
fn check_header(header: &LitStr, earlier: &[LitStr], auth: Option<&RouteAuth>) -> Result<()> {
    let name = header.value();

    check_header_name(header)?;

    if let Some(auth) = auth
        && name.eq_ignore_ascii_case("Authorization")
    {
        let words: Vec<String> = auth
            .schemes
            .iter()
            .map(|scheme| format!("`{}`", scheme.word()))
            .collect();

        return Err(Error::new(
            header.span(),
            format!(
                "the header `Authorization` carries the credential of {}, which the route takes: the handler receives its caller, and the client's middleware sends it",
                words.join(" or ")
            ),
        ));
    }
    if let Some(first) = earlier
        .iter()
        .find(|first| first.value().eq_ignore_ascii_case(&name))
    {
        return Err(Error::new(
            header.span(),
            format!(
                "the header `{}` is already read by another argument: a route reads a header once",
                first.value()
            ),
        ));
    }

    Ok(())
}

/// Whether `text` is a `token` of RFC 9110, as a header's or a cookie's name: ASCII letters, digits and the signs
/// ! # $ % & ' * + - . ^ _ ` | ~.
pub(crate) fn is_token(text: &str) -> bool {
    let tchar = |c: char| c.is_ascii_alphanumeric() || "!#$%&'*+-.^_`|~".contains(c);

    !text.is_empty() && text.chars().all(tchar)
}

/// A header's name is one HTTP takes, and that quoll leaves to the contract.
pub(crate) fn check_header_name(header: &LitStr) -> Result<()> {
    let name = header.value();

    if !is_token(&name) {
        return Err(Error::new(
            header.span(),
            format!(
                "`{name}` is not the name of a header: a name is ASCII letters, digits and the signs ! # $ % & ' * + - . ^ _ ` | ~, as in \"Last-Event-ID\""
            ),
        ));
    }
    if let Some(reserved) = RESERVED_HEADERS
        .iter()
        .find(|reserved| reserved.eq_ignore_ascii_case(&name))
    {
        return Err(Error::new(
            header.span(),
            format!(
                "the header `{reserved}` is written by quoll or by HTTP itself, so no argument takes it: read what the route needs from another header"
            ),
        ));
    }

    Ok(())
}

/// A header's type is read from text, as a path parameter's is, or is an `Option` of one for a header that may be
/// missing.
fn check_header_type(ty: &Type, header: &str) -> Result<()> {
    let inner = option_inner(ty).unwrap_or(ty);

    check_read_from_text(inner, &format!("the header `{header}`")).map_err(|_| {
        Error::new_spanned(
            ty,
            format!(
                "the header `{header}` is read from text, which this type is not: use an integer, a `String`, a UUID or an enum of unit variants, or an `Option` of one for a header that may be missing"
            ),
        )
    })
}

/// The `T` of an `Option<T>`.
pub(crate) fn option_inner(ty: &Type) -> Option<&Type> {
    single_arg(ty, "Option")
}

/// The `T` of a `Vec<T>`.
pub(crate) fn vec_inner(ty: &Type) -> Option<&Type> {
    single_arg(ty, "Vec")
}

/// The one type argument of `ty`, when the last segment of its path is `name`.
fn single_arg<'a>(ty: &'a Type, name: &str) -> Option<&'a Type> {
    let Type::Path(path) = peel(ty) else {
        return None;
    };
    let last = path
        .path
        .segments
        .last()
        .filter(|last| last.ident == name)?;
    let syn::PathArguments::AngleBracketed(args) = &last.arguments else {
        return None;
    };

    match args.args.first() {
        Some(syn::GenericArgument::Type(inner)) if args.args.len() == 1 => Some(inner),
        _ => None,
    }
}

/// Where a name is written in the route's string.
enum Place<'a> {
    Path,
    /// The query, with the key the name is bound to.
    Query(&'a str),
}

fn missing_argument(path: &LitStr, name: &str, place: &Place) -> Error {
    let place = match place {
        Place::Path => format!("`{{{name}}}` of the path"),
        Place::Query(key) => format!("`{key}={{{name}}}` of the query"),
    };

    Error::new(
        path.span(),
        format!("the {place} has no argument named `{name}`: add `{name}: Type` to the arguments"),
    )
}

/// The type itself, without the parentheses or the invisible group a `$t:ty` of a `macro_rules!` arrives in.
pub(crate) fn peel(mut ty: &Type) -> &Type {
    loop {
        match ty {
            Type::Paren(inner) => ty = &inner.elem,
            Type::Group(inner) => ty = &inner.elem,
            _ => return ty,
        }
    }
}

/// The types of the standard library a path parameter or a header is not read from: the collections and `Option`.
const STD_NOT_TEXT: &[&str] = &[
    "Vec",
    "VecDeque",
    "LinkedList",
    "HashMap",
    "BTreeMap",
    "HashSet",
    "BTreeSet",
    "BinaryHeap",
    "Option",
];

/// Whether `ty` is one of the types a path parameter or a header is not read from: [`STD_NOT_TEXT`], and the types
/// of quoll's the contract reads otherwise.
fn not_text(ty: &Type) -> bool {
    reserved_name(ty, STD_NOT_TEXT).is_some() || Known::of(ty).is_some()
}

/// The types read from one piece of text, which a query struct is not.
const SCALARS: &[&str] = &[
    "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128", "usize", "f32",
    "f64", "bool", "char", "str", "String", "Uuid",
];

/// Checks what the macro can see of a type read from text, `what`: a path, none of the types of [`not_text`]. A
/// struct of the user's cannot be told from an enum of unit variants here: axum refuses it as the request is read.
fn check_read_from_text(ty: &Type, what: &str) -> Result<()> {
    let readable = match peel(ty) {
        Type::Path(path) => path.qself.is_none() && !not_text(ty),
        _ => false,
    };
    if readable {
        return Ok(());
    }

    Err(Error::new_spanned(
        ty,
        format!(
            "{what} is read from text, which this type is not: use an integer, a `String`, a UUID or an enum of unit variants"
        ),
    ))
}

fn check_body(method: Method, name: &Ident, ty: &Type, earlier: Option<&Ident>) -> Result<()> {
    if !method.takes_body() {
        return Err(Error::new(
            name.span(),
            format!(
                "`{name}` is neither a `{{{name}}}` of the path nor a `key={{{name}}}` of the query, and {} takes no body: write it in the path or the query, or as `header(\"Name\") {name}: Type`",
                method.as_str(),
            ),
        ));
    }
    if let Some(earlier) = earlier {
        return Err(Error::new(
            name.span(),
            format!(
                "a route has one body, and `{earlier}` is already it: write `{name}` as a `{{{name}}}` of the path or a `key={{{name}}}` of the query, or put both in one type"
            ),
        ));
    }
    if let Some(known) = Known::of(ty)
        && known.body.is_none()
    {
        let result = known.name;
        let message = if known.stream {
            format!(
                "`{result}` is a stream a route answers, and a route does not read a stream as its body: take all the items at once, a `Vec<Note>` as JSON, or `Bytes`"
            )
        } else {
            format!(
                "`{result}` is what a route answers, not a body it reads: take the type it wraps as the body, without `{result}`"
            )
        };

        return Err(Error::new_spanned(ty, message));
    }

    Ok(())
}

impl Returned {
    /// What a route of `method` returns, `ty`: none without a `->`.
    fn read(method: Method, ty: Option<Type>) -> Result<Option<Box<Self>>> {
        let Some(ty) = ty else {
            return Ok(None);
        };

        if method == Method::Head {
            return Err(Error::new_spanned(
                &ty,
                "a HEAD answer has no body, so a HEAD route returns nothing: remove the `->` and its type",
            ));
        }

        if matches!(peel(&ty), Type::Tuple(tuple) if tuple.elems.is_empty()) {
            return Err(Error::new_spanned(
                &ty,
                "a route that returns nothing has no `->`: remove `-> ()`",
            ));
        }
        // A type of quoll's that is no result, a `Form`, is read as JSON, and does not compile on serde's traits.
        let Some((known, kind)) =
            Known::of(&ty).and_then(|known| known.result.map(|kind| (known, kind)))
        else {
            return Ok(Some(Box::new(Self {
                value: ty.clone(),
                served: ty,
                known: None,
                inner: None,
            })));
        };

        let inner = known
            .wraps
            .map(|what| wrapped(&ty, known.name, what))
            .transpose()?;

        // A `Created<T>` or an `Accepted<T>` answers the JSON of its `T`, which a type of quoll's is not.
        if kind.unwrapped
            && let Some(inner) = &inner
            && let Some(other) = Known::of(inner)
        {
            return Err(Error::new_spanned(
                inner,
                format!(
                    "a `{wrapper}<T>` answers the JSON of its `T`, which a `{other}` is not: write the type of the JSON value, or return `-> {other}` with the status its handler gives, `Status::{status}`",
                    wrapper = known.name,
                    other = other.name,
                    status = if known.name == "Created" {
                        "CREATED"
                    } else {
                        "ACCEPTED"
                    },
                ),
            ));
        }

        let value = match &inner {
            Some(inner) if kind.unwrapped => inner.clone(),
            _ => ty.clone(),
        };

        Ok(Some(Box::new(Self {
            served: ty,
            value,
            known: Some(known),
            inner,
        })))
    }
}

/// Whether a type is a `Vec`, which takes every value of a query key and is empty when the key is missing.
pub(crate) fn is_vec(ty: &Type) -> bool {
    reserved_name(ty, &["Vec"]).is_some()
}

/// The last segment of a type's path, if it is one of `names`.
fn reserved_name<'a>(ty: &Type, names: &[&'a str]) -> Option<&'a str> {
    let Type::Path(path) = peel(ty) else {
        return None;
    };
    let last = path.path.segments.last()?;

    names.iter().copied().find(|name| last.ident == name)
}

/// The `T` of a type of quoll's that wraps one, `wrapper`, whose last segment the caller has read; an error when it has
/// no single `T`, which is `what` the wrapper sends: the body or each item.
fn wrapped(ty: &Type, wrapper: &str, what: &str) -> Result<Type> {
    let args = match peel(ty) {
        Type::Path(path) => path.path.segments.last().map(|last| &last.arguments),
        _ => None,
    };
    let Some(syn::PathArguments::AngleBracketed(args)) = args else {
        return Err(Error::new_spanned(
            ty,
            format!("`{wrapper}` wraps the type of {what}: write `{wrapper}<Note>`"),
        ));
    };

    let mut types = args.args.iter().filter_map(|arg| match arg {
        syn::GenericArgument::Type(ty) => Some(ty),
        _ => None,
    });
    match (types.next(), types.next(), args.args.len()) {
        (Some(inner), None, 1) => Ok(inner.clone()),
        _ => Err(Error::new_spanned(
            ty,
            format!("`{wrapper}` wraps one type, {what}: write `{wrapper}<Note>`"),
        )),
    }
}

impl Api {
    /// Builds an api from its groups, checking what spans them: names and routes are not repeated.
    ///
    /// # Errors
    ///
    /// An error spanned on the second of two groups with a name, of two routes with a name in a group, or of two routes
    /// with a method and a path in the api; or on the prefix, when it is not empty or `/` and segments.
    #[expect(
        clippy::too_many_arguments,
        reason = "the parts of the api's declaration, which the syntax reads apart, and its groups"
    )]
    pub(crate) fn new(
        docs: Docs,
        vis: Visibility,
        name: Ident,
        prefix: &LitStr,
        schemes: Vec<DeclaredScheme>,
        listed: Vec<syn::Path>,
        groups: Vec<Group>,
        sides: Sides,
    ) -> Result<Self> {
        let mut errors = ErrorList::default();
        let prefix = check_prefix(prefix).unwrap_or_else(|error| {
            errors.push(error);
            String::new()
        });
        let mut api = Self {
            docs,
            vis,
            name,
            prefix,
            schemes,
            listed,
            groups,
            openapi: sides.openapi,
            server: sides.server,
            client: sides.client,
            error: None,
            headers: Headers::default(),
        };
        api.check_unique(&mut errors);
        auth::resolve(&mut api, &mut errors);

        errors.finish().map(|()| api)
    }

    /// The group of a `quoll::group!`, checked as the api of a block of its own would check it: its routes, its
    /// names and its schemes, none of which it declares. The api is a stand-in, which nothing generates.
    ///
    /// # Errors
    ///
    /// As [`Api::new`], for the one group.
    pub(crate) fn alone(group: Group, sides: Sides) -> Result<Self> {
        let mut errors = ErrorList::default();
        let mut api = Self {
            docs: Docs::default(),
            vis: Visibility::Inherited,
            name: Ident::new("__QuollAlone", Span::call_site()),
            prefix: String::new(),
            schemes: Vec::new(),
            listed: Vec::new(),
            groups: vec![group],
            openapi: sides.openapi,
            server: sides.server,
            client: sides.client,
            error: None,
            headers: Headers::default(),
        };
        api.check_unique(&mut errors);
        auth::resolve(&mut api, &mut errors);

        errors.finish().map(|()| api)
    }

    /// The name a group of the list goes by, the last segment of its path, which gives its field and the names of its
    /// items.
    pub(crate) fn listed_name(path: &syn::Path) -> &Ident {
        &path
            .segments
            .last()
            .expect("a path of the list has a segment, as the syntax reads it")
            .ident
    }

    fn check_unique(&self, errors: &mut ErrorList) {
        let mut groups: Vec<&Ident> = vec![&self.name];
        let mut fields: Vec<(String, &Ident)> = Vec::new();

        let mut names = Names::new(self);

        // A group of the list lives in its own module, beside items of its own: its name meets the api's in its fields
        // and in the names of its other groups, and, for a group named alone, whose items are in the api's module, in
        // the names of the items the api writes there too.
        for path in &self.listed {
            let name = Self::listed_name(path);
            let alone = path.get_ident().is_some();

            if alone && same_name(&self.name, name) {
                errors.push(Error::new(
                    name.span(),
                    format!(
                        "the group `{name}` is the name of the api, and both become types: each one has its own name"
                    ),
                ));
            } else if let Some(first) = groups[1..].iter().find(|first| same_name(first, name)) {
                errors.push(Error::new(
                    name.span(),
                    format!(
                        "the group `{first}` is already listed: an api lists a group once, and each of its groups has its own name"
                    ),
                ));
            } else {
                let group = Group {
                    docs: Docs::default(),
                    vis: Visibility::Inherited,
                    name: name.clone(),
                    error: None,
                    headers: Headers::default(),
                    routes: Vec::new(),
                };

                if let Err(error) = check_field(&group, &fields) {
                    errors.push(error);
                }
                if alone && let Err(error) = names.add_group(&group) {
                    errors.push(error);
                }
            }
            groups.push(name);
            fields.push((snake_case(name), name));
        }
        let mut seen: Vec<Seen> = Vec::new();
        let mut ids: Vec<(String, &Ident, &Ident)> = Vec::new();

        for scheme in &self.schemes {
            if let Err(error) = names.add_scheme(scheme) {
                errors.push(error);
            }
        }

        for group in &self.groups {
            // A group given twice is one mistake, which its routes' operation ids do not repeat.
            let repeated = groups[1..]
                .iter()
                .any(|first| same_name(first, &group.name));
            if let Some(first) = groups
                .iter()
                .position(|first| same_name(first, &group.name))
            {
                let what = if first == 0 {
                    "is the name of the api, and both become types"
                } else {
                    "is already defined"
                };
                errors.push(Error::new(
                    group.name.span(),
                    format!(
                        "the group `{}` {what}: each one has its own name",
                        group.name
                    ),
                ));
            } else {
                let checks = [
                    check_field(group, &fields),
                    check_visibility(self, group),
                    names.add_group(group),
                ];
                for error in checks.into_iter().filter_map(Result::err) {
                    errors.push(error);
                }
            }
            groups.push(&group.name);
            fields.push((snake_case(&group.name), &group.name));

            let mut functions: Vec<&Ident> = Vec::new();
            for route in &group.routes {
                if functions.iter().any(|first| same_name(first, &route.name)) {
                    errors.push(Error::new(
                        route.name.span(),
                        format!(
                            "the group `{}` has a route `{}` already: each route has its own function name",
                            group.name, route.name
                        ),
                    ));
                } else if !repeated {
                    let id = route.operation_id(group);

                    if let Some((_, first_group, first_route)) =
                        ids.iter().find(|(first, ..)| *first == id)
                    {
                        errors.push(Error::new(
                            route.name.span(),
                            format!(
                                "the operation id `{id}` of the route `{}` of `{}` is already the one of the route `{first_route}` of `{first_group}`: rename one of them, as an operation id, the group in snake case and the function, names one route of the api",
                                route.name, group.name
                            ),
                        ));
                    }
                    ids.push((id, &group.name, &route.name));
                }
                functions.push(&route.name);

                let current = Seen {
                    method: route.method,
                    shape: shape_of(&route.path),
                    path: &route.path,
                    fields: &route.fields,
                    group: &group.name,
                    route: &route.name,
                };
                if let Some(error) = seen
                    .iter()
                    .find_map(|first| first.clash(&current, route.span, self.server))
                {
                    errors.push(error);
                }
                seen.push(current);
            }
        }
    }
}

/// The names of the items the macro writes, each defined once in the module of the contract.
struct Names(Vec<(String, String)>);

impl Names {
    /// The api's names: its type, its services and its client.
    fn new(api: &Api) -> Self {
        let name = api.name.unraw();

        Self(vec![
            (name.to_string(), format!("the type of the api `{name}`")),
            (
                api.services().to_string(),
                format!("the services of the api `{name}`"),
            ),
            (
                api.client().to_string(),
                format!("the client of the api `{name}`"),
            ),
        ])
    }

    /// Adds the name of a declared scheme, the type it becomes.
    ///
    /// # Errors
    ///
    /// Spanned on the scheme's name, when it is already another item's.
    fn add_scheme(&mut self, scheme: &DeclaredScheme) -> Result<()> {
        let name = scheme.name.unraw().to_string();

        let what = format!("the scheme `{name}`");

        if let Some((_, first)) = self.0.iter().find(|(taken, _)| *taken == name) {
            let message = if *first == what {
                format!("the scheme `{name}` is already declared: each scheme has its own name")
            } else {
                format!(
                    "`{name}`, the scheme, is already {first}: rename the scheme, as each item the contract writes has its own name"
                )
            };

            return Err(Error::new(scheme.name.span(), message));
        }

        self.0.push((name, what));
        Ok(())
    }

    /// Adds the names a group gives: its own, which its client's trait takes, its server's trait and its client.
    ///
    /// # Errors
    ///
    /// Spanned on the group's name, when one of them is already another item's.
    fn add_group(&mut self, group: &Group) -> Result<()> {
        let name = group.name.unraw();
        let given = [
            (name.to_string(), format!("the name of the group `{name}`")),
            (
                group.server_trait().to_string(),
                format!("the server trait of the group `{name}`"),
            ),
            (
                group.client().to_string(),
                format!("the client of the group `{name}`"),
            ),
        ];

        for (item, what) in given {
            if let Some((_, first)) = self.0.iter().find(|(taken, _)| *taken == item) {
                return Err(Error::new(
                    group.name.span(),
                    format!(
                        "`{item}`, {what}, is already {first}: rename the group, as each item the contract writes has its own name"
                    ),
                ));
            }
            self.0.push((item, what));
        }

        Ok(())
    }
}

/// How far a visibility reaches: private, a module's, the crate's, everyone's. `pub(super)` and `pub(in path)` are
/// counted as a module's, without comparing their paths.
pub(crate) fn reach(vis: &Visibility) -> u8 {
    match vis {
        Visibility::Inherited => 0,
        Visibility::Public(_) => 3,
        Visibility::Restricted(restricted) => {
            let path = &restricted.path;
            if path.is_ident("crate") {
                2
            } else if path.is_ident("self") {
                0
            } else {
                1
            }
        }
    }
}

/// A group reaches as far as its api: the api's router names the group's server trait, and its client the group's
/// client.
fn check_visibility(api: &Api, group: &Group) -> Result<()> {
    if reach(&group.vis) >= reach(&api.vis) {
        return Ok(());
    }

    Err(Error::new(
        group.name.span(),
        format!(
            "the group `{}` is less visible than the api `{}`, whose router and client name the group's items: write `{} group {}`",
            group.name,
            api.name,
            shown_vis(&api.vis),
            group.name,
        ),
    ))
}

/// A visibility as the contract writes it: `pub`, `pub(crate)`.
pub(crate) fn shown_vis(vis: &Visibility) -> String {
    quote::quote!(#vis)
        .to_string()
        .replace("pub (", "pub(")
        .replace(" :: ", "::")
}

/// A route already read, against which the next ones are checked.
struct Seen<'a> {
    method: Method,
    shape: Vec<Segment>,
    path: &'a [Segment],
    fields: &'a [BodyField],
    group: &'a Ident,
    route: &'a Ident,
}

impl Seen<'_> {
    /// The error when `next`, whose string is at `span`, cannot sit beside this route in one api: the same method and
    /// path, or, when the api is `served`, the same path with its parameters named otherwise, which a router takes for
    /// one path.
    fn clash(&self, next: &Seen<'_>, span: Span, served: bool) -> Option<Error> {
        let Self { group, route, .. } = self;

        if self.method == next.method
            && self.shape == next.shape
            && !fields::tell_apart(self.fields, next.fields)
        {
            return Some(Error::new(
                span,
                format!(
                    "{} on this path is already the route `{group}::{route}`: a method and a path belong to one route{}",
                    next.method.as_str(),
                    fields::why_not_apart(self.fields, next.fields),
                ),
            ));
        }

        if !served || self.shape != next.shape {
            return None;
        }

        let (first, other) = renamed_param(self.path, next.path)?;
        Some(Error::new(
            span,
            format!(
                "this path is the one of the route `{group}::{route}`, with `{{{other}}}` written for `{{{first}}}`: a router takes them for one path, so name the parameter `{{{first}}}` here too"
            ),
        ))
    }
}

/// The names of the first parameter that two paths of one shape name in two ways.
fn renamed_param<'a>(first: &'a [Segment], next: &'a [Segment]) -> Option<(&'a str, &'a str)> {
    for pair in first.iter().zip(next) {
        match pair {
            (Segment::Literal(a), Segment::Literal(b)) if a == b => {}
            (Segment::Param(a), Segment::Param(b)) if a == b => {}
            (Segment::Param(a), Segment::Param(b)) => return Some((a, b)),
            _ => return None,
        }
    }

    None
}

/// The field of `ApiServices` and the client's method a group becomes: its name in snake case, `AdminTools` giving
/// `admin_tools`.
pub(crate) fn snake_case(name: &Ident) -> String {
    let name = name.unraw().to_string();
    let chars: Vec<char> = name.chars().collect();
    let mut snake = String::with_capacity(name.len() + 4);

    for (index, &c) in chars.iter().enumerate() {
        if !c.is_uppercase() {
            snake.push(c);
            continue;
        }

        let previous = index.checked_sub(1).map(|before| chars[before]);
        let next = chars.get(index + 1);
        let after_word = previous.is_some_and(|p| p.is_lowercase() || p.is_ascii_digit());
        let ends_capitals =
            previous.is_some_and(char::is_uppercase) && next.is_some_and(|n| n.is_lowercase());
        if after_word || ends_capitals {
            snake.push('_');
        }
        snake.extend(c.to_lowercase());
    }

    snake
}

impl Api {
    /// `ApiServices` for the api `Api`: the services of its groups.
    pub(crate) fn services(&self) -> Ident {
        quote::format_ident!("{}Services", self.name.unraw(), span = self.name.span())
    }

    /// `ApiClient` for the api `Api`: the client that gives each group's.
    pub(crate) fn client(&self) -> Ident {
        quote::format_ident!("{}Client", self.name.unraw(), span = self.name.span())
    }
}

impl Group {
    /// `NotesServer` for the group `Notes`: the trait its service implements.
    pub(crate) fn server_trait(&self) -> Ident {
        quote::format_ident!("{}Server", self.name.unraw(), span = self.name.span())
    }

    /// `NotesClient` for the group `Notes`: the client implementing the group's trait.
    pub(crate) fn client(&self) -> Ident {
        quote::format_ident!("{}Client", self.name.unraw(), span = self.name.span())
    }

    /// The group's name in snake case, as an identifier: raw when it is a keyword, `Type` giving `r#type`.
    pub(crate) fn field(&self) -> Ident {
        let snake = snake_case(&self.name);
        let span = self.name.span();

        match syn::parse_str::<Ident>(&snake) {
            Ok(_) => Ident::new(&snake, span),
            Err(_) => Ident::new_raw(&snake, span),
        }
    }
}

/// Checks that the group's snake-case name is a field that no group before it takes.
fn check_field(group: &Group, fields: &[(String, &Ident)]) -> Result<()> {
    let snake = snake_case(&group.name);

    if matches!(snake.as_str(), "self" | "super" | "crate") {
        return Err(Error::new(
            group.name.span(),
            format!(
                "the group `{}` becomes the field `{snake}`, which Rust does not allow even as a raw name: rename the group",
                group.name
            ),
        ));
    }
    if let Some((_, first)) = fields.iter().find(|(field, _)| *field == snake) {
        return Err(Error::new(
            group.name.span(),
            format!(
                "the groups `{first}` and `{}` both become the field `{snake}`: rename one of them",
                group.name
            ),
        ));
    }

    Ok(())
}

/// Whether two names are one, `r#get` and `get` being the same function.
fn same_name(first: &Ident, second: &Ident) -> bool {
    first.unraw() == second.unraw()
}

/// A path with its parameter names erased: `/notes/{id}` and `/notes/{note}` are one path to a router.
fn shape_of(path: &[Segment]) -> Vec<Segment> {
    path.iter()
        .map(|segment| match segment {
            Segment::Param(_) => Segment::Param(String::new()),
            literal => literal.clone(),
        })
        .collect()
}

fn check_prefix(prefix: &LitStr) -> Result<String> {
    let value = prefix.value();
    if value.is_empty() {
        return Ok(value);
    }

    let wrong = |what: &str| {
        Error::new(
            prefix.span(),
            format!("the prefix {what}: write it as \"/api\", or \"\" for none"),
        )
    };
    if !value.starts_with('/') || value == "/" {
        return Err(wrong("starts with `/` and has a segment"));
    }
    if value.ends_with('/') || value.contains("//") {
        return Err(wrong("has no empty segment (`//` or a final `/`)"));
    }
    if let Some(problem) = value[1..].split('/').find_map(literal_problem) {
        return Err(Error::new(
            prefix.span(),
            format!("in the prefix, {problem} (a prefix is written \"/api\", or \"\" for none)"),
        ));
    }

    Ok(value)
}

/// Errors gathered so that one run of the macro reports them all.
#[derive(Debug, Default)]
pub(crate) struct ErrorList(Option<Error>);

impl ErrorList {
    /// Adds an error to the ones already gathered.
    pub(crate) fn push(&mut self, error: Error) {
        match &mut self.0 {
            Some(all) => all.combine(error),
            None => self.0 = Some(error),
        }
    }

    /// Ends with the errors gathered and one more, the one that stopped the reading.
    pub(crate) fn fail(mut self, last: Error) -> Error {
        self.push(last);

        self.0.expect("an error was just pushed")
    }

    /// Ends with the errors gathered: `Ok` when there are none.
    pub(crate) fn finish(self) -> Result<()> {
        self.0.map_or(Ok(()), Err)
    }
}

#[cfg(test)]
mod tests {
    use proc_macro2::Span;

    use super::*;

    fn ident(name: &str) -> Ident {
        syn::parse_str(name).expect("a valid identifier")
    }

    #[test]
    fn a_group_name_becomes_its_snake_case_field() {
        let cases = [
            ("Notes", "notes"),
            ("AdminTools", "admin_tools"),
            ("HTTPServer", "http_server"),
            ("AdminV2", "admin_v2"),
            ("V2Admin", "v2_admin"),
            ("Admin_Tools", "admin_tools"),
            ("r#Notes", "notes"),
        ];

        for (name, field) in cases {
            assert_eq!(snake_case(&ident(name)), field, "{name}");
        }
    }

    #[test]
    fn a_field_that_is_a_keyword_is_raw() {
        let group = Group {
            docs: Docs::default(),
            vis: Visibility::Inherited,
            name: Ident::new("Type", Span::call_site()),
            error: None,
            headers: Headers::default(),
            routes: Vec::new(),
        };

        assert_eq!(group.field().to_string(), "r#type");
    }

    #[test]
    fn the_first_parameter_two_paths_name_otherwise_is_found() {
        let literal = |text: &str| Segment::Literal(text.into());
        let param = |name: &str| Segment::Param(name.into());

        let notes_id = [literal("notes"), param("id")];
        let notes_key_files = [literal("notes"), param("key"), literal("files")];
        let tags_key = [literal("tags"), param("key")];
        let notes_new = [literal("notes"), literal("new")];

        assert_eq!(
            renamed_param(&notes_id, &notes_key_files),
            Some(("id", "key"))
        );
        assert_eq!(renamed_param(&notes_id, &tags_key), None);
        assert_eq!(renamed_param(&notes_id, &notes_new), None);
        assert_eq!(renamed_param(&notes_id, &notes_id), None);
    }
}

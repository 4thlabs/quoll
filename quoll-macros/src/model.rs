//! The model of an api: what the generators read, whatever the spelling it was written in.
//!
//! The `rest::api!` syntax reads into it (`syntax.rs`); a spelling with attributes on traits could build the same
//! values, and the generators would not know. Nothing here depends on how a contract looks: only on what it says.
//! Every constructor checks what a spelling cannot get wrong without the mistake being a contract's: names matched,
//! one body, no route twice.

use proc_macro2::Span;
use syn::{Error, Ident, LitStr, Result, Type, Visibility, ext::IdentExt};

use crate::path::{Segment, Template, literal_problem};

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
    /// The groups, in the order they are written.
    pub(crate) groups: Vec<Group>,
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
    /// The routes, in the order they are written.
    pub(crate) routes: Vec<Route>,
}

/// A scheme a route is authenticated with: how the server reads the credential, and how the client sends it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum Scheme {
    /// `Bearer`: a token in `Authorization: Bearer`, the crate's `rest::Bearer`.
    Bearer,
}

impl Scheme {
    /// The word the contract writes for the scheme, which the docs of what is generated show. The name on the wire,
    /// which `rest::Route::schemes` gives, is the crate's: the generated code takes it from the scheme's type.
    pub(crate) const fn word(self) -> &'static str {
        match self {
            Self::Bearer => "Bearer",
        }
    }
}

/// The authentication a group or a route says it takes, as written in its attribute.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Auth {
    /// `#[auth(none)]`: no credential, whatever the group takes.
    None,
    /// `#[auth(Bearer)]`: a credential of this scheme.
    Scheme(Scheme),
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
    /// What the route answers.
    pub(crate) reply: Reply,
    /// The scheme the route is authenticated with: its own attribute's, else its group's; `None` when it takes no
    /// credential.
    pub(crate) auth: Option<Scheme>,
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

    /// The path's parameters, in the order of the path.
    pub(crate) fn path_params(&self) -> impl Iterator<Item = &Param> {
        self.path_order.iter().map(|&index| &self.params[index])
    }

    /// The query's parameters, each with its key on the wire, in the order they are written.
    pub(crate) fn query_params(&self) -> impl Iterator<Item = (&Param, &str)> {
        self.params.iter().filter_map(|param| match &param.kind {
            ParamKind::Query { key } => Some((param, key.as_str())),
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
    /// Its type, as written.
    pub(crate) ty: Type,
    /// Where its value is read from.
    pub(crate) kind: ParamKind,
}

/// Where an argument comes from on the wire.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ParamKind {
    /// A `{name}` of the path.
    Path,
    /// A `key={name}` of the query. An `Option` may be missing; a `Vec` repeats the key.
    Query {
        /// The key on the wire.
        key: String,
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

/// What a body is on the wire, which its type says: `Form<T>`, `Text`, `Bytes`, or any other type for JSON.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BodyKind {
    /// Any type but the three below: its JSON, `application/json`.
    Json,
    /// `Form<T>`: `application/x-www-form-urlencoded`.
    Form,
    /// `Text`: `text/plain`, UTF-8.
    Text,
    /// `Bytes`: `application/octet-stream`.
    Bytes,
}

impl BodyKind {
    /// What a body of type `ty` is: the last segment of its path names it.
    fn of(ty: &Type) -> Self {
        match reserved_name(ty, &["Form", "Text", "Bytes"]) {
            Some("Form") => Self::Form,
            Some("Text") => Self::Text,
            Some("Bytes") => Self::Bytes,
            _ => Self::Json,
        }
    }
}

/// What a route answers, and so its status and its body.
#[derive(Debug)]
pub(crate) enum Reply {
    /// No `->`: 204, no body.
    None,
    /// `T`: 200, JSON.
    Json(Type),
    /// `Created<T>`: 201, JSON. The type is the whole `Created<T>`, as the contract writes it, which the server's trait
    /// returns; [`Reply::value`] gives the `T`, which the client returns.
    Created(Type),
}

impl Reply {
    /// The type of the value the route answers, which the client returns: the `T` of a `Created<T>`.
    pub(crate) fn value(&self) -> Option<&Type> {
        match self {
            Self::None => None,
            Self::Json(ty) => Some(ty),
            Self::Created(ty) => Some(
                created_inner(ty)
                    .ok()
                    .flatten()
                    .expect("`Reply::read` checked that the type is a `Created<T>`"),
            ),
        }
    }
}

/// An argument as a spelling writes it.
#[derive(Debug)]
pub(crate) struct ArgDecl {
    /// Where it says its value comes from.
    pub(crate) source: Source,
    /// Its name.
    pub(crate) name: Ident,
    /// Its type.
    pub(crate) ty: Type,
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
    /// supported.
    pub(crate) fn resolve(decl: RouteDecl, group_auth: Option<Auth>) -> Result<Self> {
        let RouteDecl {
            docs,
            method,
            path,
            name,
            args,
            reply,
            auth,
        } = decl;
        let template = Template::parse(&path)?;

        check_names_unique(&args)?;
        let params = match_args(method, &path, &template, args)?;
        let path_order = template
            .path_names()
            .map(|path_name| {
                params
                    .iter()
                    .position(|param| param.name.unraw() == path_name)
                    .expect("`match_args` gave each `{name}` of the path its argument")
            })
            .collect();
        let reply = Reply::read(method, reply)?;
        // A route's attribute replaces its group's.
        let auth = match auth.or(group_auth) {
            Some(Auth::Scheme(scheme)) => Some(scheme),
            Some(Auth::None) | None => None,
        };

        Ok(Self {
            docs,
            method,
            name,
            path: template.segments,
            params,
            path_order,
            reply,
            auth,
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
    args: Vec<ArgDecl>,
) -> Result<Vec<Param>> {
    let find = |name: &str| args.iter().find(|arg| arg.name.unraw() == name);

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

    for ArgDecl { source, name, ty } in args {
        let key = name.unraw().to_string();
        let kind = match source {
            Source::Query(span) => {
                check_query_struct(span, template, &name, &ty, query.as_ref())?;
                query = Some(name.clone());
                ParamKind::QueryStruct
            }
            Source::Header(header) => {
                check_header(&header, &headers)?;
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
                },
                None => {
                    check_body(method, &name, body.as_ref())?;
                    body = Some(name.clone());
                    ParamKind::Body(BodyKind::of(&ty))
                }
            },
        };
        params.push(Param { name, ty, kind });
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
    let reserved = reserved_name(ty, NOT_TEXT).is_some();
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

/// A header's name is one HTTP takes, and the route reads each header once.
fn check_header(header: &LitStr, earlier: &[LitStr]) -> Result<()> {
    let name = header.value();
    // The `token` of RFC 9110: letters, digits and these signs.
    let tchar = |c: char| c.is_ascii_alphanumeric() || "!#$%&'*+-.^_`|~".contains(c);

    if name.is_empty() || !name.chars().all(tchar) {
        return Err(Error::new(
            header.span(),
            format!(
                "`{name}` is not the name of a header: a name is ASCII letters, digits and `-`, as in \"Last-Event-ID\""
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
fn option_inner(ty: &Type) -> Option<&Type> {
    let Type::Path(path) = peel(ty) else {
        return None;
    };
    let last = path
        .path
        .segments
        .last()
        .filter(|last| last.ident == "Option")?;
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
fn peel(mut ty: &Type) -> &Type {
    loop {
        match ty {
            Type::Paren(inner) => ty = &inner.elem,
            Type::Group(inner) => ty = &inner.elem,
            _ => return ty,
        }
    }
}

/// The types a path parameter or a header is not read from: the collections of the standard library, `Option`, and
/// the types the contract reads otherwise.
const NOT_TEXT: &[&str] = &[
    "Vec",
    "VecDeque",
    "LinkedList",
    "HashMap",
    "BTreeMap",
    "HashSet",
    "BTreeSet",
    "BinaryHeap",
    "Option",
    "Created",
    "Form",
    "Text",
    "Bytes",
];

/// Checks what the macro can see of a type read from text, `what`: a path, with none of the names of [`NOT_TEXT`]. A
/// struct of the user's cannot be told from an enum of unit variants here: axum refuses it as the request is read.
fn check_read_from_text(ty: &Type, what: &str) -> Result<()> {
    let readable = match peel(ty) {
        Type::Path(path) => path.qself.is_none() && reserved_name(ty, NOT_TEXT).is_none(),
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

fn check_body(method: Method, name: &Ident, earlier: Option<&Ident>) -> Result<()> {
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

    Ok(())
}

impl Reply {
    fn read(method: Method, ty: Option<Type>) -> Result<Self> {
        let Some(ty) = ty else {
            return Ok(Self::None);
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
        if let Some(wrapper) =
            reserved_name(&ty, &["Accepted", "Events", "JsonLines", "Text", "Bytes"])
        {
            return Err(Error::new_spanned(
                &ty,
                format!(
                    "`{wrapper}` is not supported as a result: a route returns nothing (no `->`), `T` or `Created<T>`"
                ),
            ));
        }
        if created_inner(&ty)?.is_some() {
            return Ok(Self::Created(ty));
        }

        Ok(Self::Json(ty))
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

/// The `T` of a `Created<T>`; `None` when the type is something else, an error when `Created` has no single `T`.
fn created_inner(ty: &Type) -> Result<Option<&Type>> {
    let Type::Path(path) = peel(ty) else {
        return Ok(None);
    };
    let Some(last) = path
        .path
        .segments
        .last()
        .filter(|last| last.ident == "Created")
    else {
        return Ok(None);
    };

    let syn::PathArguments::AngleBracketed(args) = &last.arguments else {
        return Err(Error::new_spanned(
            ty,
            "`Created` wraps the type of the body: write `Created<Note>`",
        ));
    };
    let mut types = args.args.iter().filter_map(|arg| match arg {
        syn::GenericArgument::Type(ty) => Some(ty),
        _ => None,
    });
    match (types.next(), types.next(), args.args.len()) {
        (Some(inner), None, 1) => Ok(Some(inner)),
        _ => Err(Error::new_spanned(
            ty,
            "`Created` wraps one type, the body: write `Created<Note>`",
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
    pub(crate) fn new(
        docs: Docs,
        vis: Visibility,
        name: Ident,
        prefix: &LitStr,
        groups: Vec<Group>,
    ) -> Result<Self> {
        let mut errors = ErrorList::default();
        let prefix = check_prefix(prefix).unwrap_or_else(|error| {
            errors.push(error);
            String::new()
        });
        let api = Self {
            docs,
            vis,
            name,
            prefix,
            groups,
        };
        api.check_unique(&mut errors);

        errors.finish().map(|()| api)
    }

    fn check_unique(&self, errors: &mut ErrorList) {
        let mut groups: Vec<&Ident> = vec![&self.name];
        let mut fields: Vec<(String, &Ident)> = Vec::new();
        let mut seen: Vec<Seen> = Vec::new();
        let mut names = Names::new(self);

        for group in &self.groups {
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
                }
                functions.push(&route.name);

                let current = Seen {
                    method: route.method,
                    shape: shape_of(&route.path),
                    path: &route.path,
                    group: &group.name,
                    route: &route.name,
                };
                if let Some(error) = seen
                    .iter()
                    .find_map(|first| first.clash(&current, route.span))
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
fn reach(vis: &Visibility) -> u8 {
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

    let vis = &api.vis;
    let vis = quote::quote!(#vis)
        .to_string()
        .replace("pub (", "pub(")
        .replace(" :: ", "::");
    Err(Error::new(
        group.name.span(),
        format!(
            "the group `{}` is less visible than the api `{}`, whose router and client name the group's items: write `{} group {}`",
            group.name, api.name, vis, group.name,
        ),
    ))
}

/// A route already read, against which the next ones are checked.
struct Seen<'a> {
    method: Method,
    shape: Vec<Segment>,
    path: &'a [Segment],
    group: &'a Ident,
    route: &'a Ident,
}

impl Seen<'_> {
    /// The error when `next`, whose string is at `span`, cannot sit beside this route in one router: the same method
    /// and path, or the same path with its parameters named otherwise, which a router takes for one path.
    fn clash(&self, next: &Seen<'_>, span: Span) -> Option<Error> {
        let Self { group, route, .. } = self;

        if self.method == next.method && self.shape == next.shape {
            return Some(Error::new(
                span,
                format!(
                    "{} on this path is already the route `{group}::{route}`: a method and a path belong to one route",
                    next.method.as_str()
                ),
            ));
        }

        if self.shape != next.shape {
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

    /// The schemes its routes take, each once, in the order they first appear: the service implements
    /// `Authenticate` for each.
    pub(crate) fn schemes(&self) -> Vec<Scheme> {
        let mut schemes = Vec::new();

        for scheme in self.routes.iter().filter_map(|route| route.auth) {
            if !schemes.contains(&scheme) {
                schemes.push(scheme);
            }
        }

        schemes
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

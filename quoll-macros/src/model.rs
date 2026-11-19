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
    /// What the route answers.
    pub(crate) reply: Reply,
    /// The span of the route's string, where a mistake about the route as a whole is pointed.
    pub(crate) span: Span,
}

impl Route {
    /// The argument read from the body, if the route has one.
    pub(crate) fn body(&self) -> Option<&Param> {
        self.params
            .iter()
            .find(|param| matches!(param.kind, ParamKind::Body))
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
    /// The body, as JSON.
    Body,
}

/// What a route answers, and so its status and its body.
#[derive(Debug)]
pub(crate) enum Reply {
    /// No `->`: 204, no body.
    None,
    /// `T`: 200, JSON.
    Json(Type),
    /// `Created<T>`: 201, JSON; the type is the `T`.
    Created(Type),
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
    /// The arguments, `(name, type)`, in the order they are written.
    pub(crate) args: Vec<(Ident, Type)>,
    /// The type after `->`, if there is one.
    pub(crate) reply: Option<Type>,
}

impl Route {
    /// Matches the arguments of a route to its string, by name: a `{name}` of the path, a `key={name}` of the query,
    /// and the one argument left is the body.
    ///
    /// # Errors
    ///
    /// An error spanned on the argument or the string at fault: an argument twice, a `{name}` with no argument, a path
    /// argument that is not read from text, an argument that is nowhere, a second body or a body the method does not
    /// take, a result or a body that is not supported.
    pub(crate) fn resolve(decl: RouteDecl) -> Result<Self> {
        let RouteDecl {
            docs,
            method,
            path,
            name,
            args,
            reply,
        } = decl;
        let template = Template::parse(&path)?;

        check_names_unique(&args)?;
        let params = match_args(method, &path, &template, args)?;
        let reply = Reply::read(reply)?;

        Ok(Self {
            docs,
            method,
            name,
            path: template.segments,
            params,
            reply,
            span: path.span(),
        })
    }
}

fn check_names_unique(args: &[(Ident, Type)]) -> Result<()> {
    for (index, (name, _)) in args.iter().enumerate() {
        let twice = args[..index]
            .iter()
            .any(|(earlier, _)| earlier.unraw() == name.unraw());
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
    args: Vec<(Ident, Type)>,
) -> Result<Vec<Param>> {
    let find = |name: &str| args.iter().position(|(arg, _)| arg.unraw() == name);

    for name in template.path_names() {
        let Some(index) = find(name) else {
            return Err(missing_argument(path, name, &Place::Path));
        };
        check_read_from_text(&args[index].0, &args[index].1)?;
    }
    for item in &template.query {
        if find(&item.name).is_none() {
            return Err(missing_argument(path, &item.name, &Place::Query(&item.key)));
        }
    }

    let mut params = Vec::with_capacity(args.len());
    let mut body: Option<Ident> = None;

    for (name, ty) in args {
        let key = name.unraw().to_string();
        let kind = if template.path_names().any(|path_name| path_name == key) {
            ParamKind::Path
        } else if let Some(item) = template.query.iter().find(|item| item.name == key) {
            ParamKind::Query {
                key: item.key.clone(),
            }
        } else {
            check_body(method, &name, &ty, body.as_ref())?;
            body = Some(name.clone());
            ParamKind::Body
        };
        params.push(Param { name, ty, kind });
    }

    Ok(params)
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

fn check_read_from_text(name: &Ident, ty: &Type) -> Result<()> {
    let readable = match peel(ty) {
        Type::Path(path) => {
            path.qself.is_none()
                && !path
                    .path
                    .segments
                    .last()
                    .is_some_and(|last| last.ident == "Vec" || last.ident == "Option")
        }
        _ => false,
    };
    if readable {
        return Ok(());
    }

    Err(Error::new_spanned(
        ty,
        format!(
            "the path parameter `{name}` is read from text, which this type is not: use an integer, a `String`, a UUID or an enum of unit variants"
        ),
    ))
}

fn check_body(method: Method, name: &Ident, ty: &Type, earlier: Option<&Ident>) -> Result<()> {
    if !method.takes_body() {
        return Err(Error::new(
            name.span(),
            format!(
                "`{name}` is neither a `{{{name}}}` of the path nor a `key={{{name}}}` of the query, and {} takes no body: write it in the path or the query",
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

    if let Some(wrapper) = reserved_name(ty, &["Form", "Text", "Bytes"]) {
        return Err(Error::new_spanned(
            ty,
            format!(
                "`{wrapper}` is not supported as a body: a body is the JSON of its type, written as `{name}: Type`"
            ),
        ));
    }

    Ok(())
}

impl Reply {
    fn read(ty: Option<Type>) -> Result<Self> {
        let Some(ty) = ty else {
            return Ok(Self::None);
        };

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
        if let Some(inner) = created_inner(&ty)? {
            return Ok(Self::Created(inner));
        }

        Ok(Self::Json(ty))
    }
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
fn created_inner(ty: &Type) -> Result<Option<Type>> {
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
        (Some(inner), None, 1) => Ok(Some(inner.clone())),
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
        let mut seen: Vec<(Method, Vec<Segment>, &Ident, &Ident)> = Vec::new();

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
            }
            groups.push(&group.name);

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

                let shape = shape_of(&route.path);
                let same = |(method, path, ..): &&(Method, Vec<Segment>, &Ident, &Ident)| {
                    *method == route.method && *path == shape
                };
                if let Some((.., first_group, first)) = seen.iter().find(same) {
                    errors.push(Error::new(
                        route.span,
                        format!(
                            "{} on this path is already the route `{first_group}::{first}`: a method and a path belong to one route",
                            route.method.as_str()
                        ),
                    ));
                }
                seen.push((route.method, shape, &group.name, &route.name));
            }
        }
    }
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

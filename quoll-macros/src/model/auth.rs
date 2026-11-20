//! Authentication in the model: the schemes a contract declares, the ones a route takes, and the checks that span
//! them, once every scheme is known.

use proc_macro2::Span;
use quote::ToTokens;
use syn::{Error, Ident, LitStr, Path, Result, Visibility, ext::IdentExt};

use super::{Api, Docs, ErrorList, Group, Route, check_header_name, is_token, reach, shown_vis};

/// The most schemes an alternative takes: `#[auth(A | B | C | D)]`, which the crate implements for tuples of up to
/// four.
pub(crate) const MAX_ALTERNATIVES: usize = 4;

/// A scheme a route takes, as its `#[auth(...)]` names it.
#[derive(Debug, Clone)]
pub(crate) struct Scheme {
    /// The path as the contract writes it, where an error about the scheme points.
    pub(crate) path: Path,
    /// Which scheme it is.
    pub(crate) kind: SchemeKind,
}

/// Which scheme a route takes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum SchemeKind {
    /// `Bearer`, the crate's `rest::Bearer`.
    Bearer,
    /// `Basic`, the crate's `rest::Basic`.
    Basic,
    /// A scheme the contract declares, with where its credential travels.
    Declared(Location),
    /// A type of the application implementing `rest::Scheme`, or a scheme not declared, which rustc does not find.
    Foreign,
}

/// Where a credential travels, for the schemes the macro knows: two schemes of one alternative do not read the same
/// place, nor a route's argument the place of a scheme it takes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Location {
    /// `Authorization`, with the scheme of its value: `Bearer` or `Basic`.
    Authorization(&'static str),
    /// A header, lowercase.
    Header(String),
    /// A cookie.
    Cookie(String),
    /// A key of the query.
    Query(String),
}

impl Scheme {
    /// The scheme a path names: `Bearer` and `Basic` alone are the crate's, any other path a type, which may be a
    /// scheme the contract declares, as the api finds out.
    pub(crate) fn new(path: Path) -> Self {
        let kind = match path.get_ident() {
            Some(ident) if ident == "Bearer" => SchemeKind::Bearer,
            Some(ident) if ident == "Basic" => SchemeKind::Basic,
            _ => SchemeKind::Foreign,
        };

        Self { path, kind }
    }

    /// The scheme as the contract writes it, which errors and docs show: `Bearer`, `Key`, `crate::auth::Hmac`.
    pub(crate) fn word(&self) -> String {
        self.path
            .to_token_stream()
            .to_string()
            .replace(" :: ", "::")
            .replace(":: ", "::")
            .replace(" < ", "<")
            .replace(" >", ">")
    }

    /// Whether two schemes are one: the crate's `Bearer` or `Basic`, a declared scheme by its name, or a type of the
    /// application's by its path as written.
    pub(crate) fn same(&self, other: &Self) -> bool {
        match (&self.kind, &other.kind) {
            (SchemeKind::Bearer, SchemeKind::Bearer) | (SchemeKind::Basic, SchemeKind::Basic) => {
                true
            }
            (SchemeKind::Declared(_), SchemeKind::Declared(_))
            | (SchemeKind::Foreign, SchemeKind::Foreign) => self.word() == other.word(),
            _ => false,
        }
    }

    /// The span of the scheme in its `#[auth(...)]`.
    pub(crate) fn span(&self) -> Span {
        syn::spanned::Spanned::span(&self.path)
    }

    /// Where its credential travels, when the macro knows it.
    pub(crate) fn place(&self) -> Option<Location> {
        match &self.kind {
            SchemeKind::Bearer => Some(Location::Authorization("Bearer")),
            SchemeKind::Basic => Some(Location::Authorization("Basic")),
            SchemeKind::Declared(place) => Some(place.clone()),
            SchemeKind::Foreign => None,
        }
    }
}

impl Location {
    /// The place as an error names it.
    fn shown(&self) -> String {
        match self {
            Self::Authorization(scheme) => format!("the header `Authorization: {scheme}`"),
            Self::Header(name) => format!("the header `{name}`"),
            Self::Cookie(name) => format!("the cookie `{name}`"),
            Self::Query(key) => format!("the query key `{key}`"),
        }
    }

    /// Whether two schemes reading these places would read one credential.
    fn same(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Header(first), Self::Header(second)) => first.eq_ignore_ascii_case(second),
            _ => self == other,
        }
    }
}

/// The authentication of a route: the schemes it takes, of which the first one whose credential a request carries
/// decides, whether a request may carry none, and the scopes its caller needs.
#[derive(Debug, Clone)]
pub(crate) struct RouteAuth {
    /// The schemes, in the order they are written: one, or up to four alternatives.
    pub(crate) schemes: Vec<Scheme>,
    /// `optional`: the handler receives `Option<&Caller>`, `None` for a request with no credential.
    pub(crate) optional: bool,
    /// `scopes(...)`: the scopes the caller needs, every one of them.
    pub(crate) scopes: Vec<String>,
}

/// The authentication a group or a route says it takes, as written in its attribute.
#[derive(Debug, Clone)]
pub(crate) enum Auth {
    /// `#[auth(none)]`: no credential, whatever the group takes.
    None,
    /// `#[auth(Bearer)]` and the others: a credential of these schemes.
    Takes(RouteAuth),
}

/// A scheme the contract declares: `pub scheme Key = header "X-Api-Key";`.
#[derive(Debug)]
pub(crate) struct DeclaredScheme {
    /// The comments on the scheme.
    pub(crate) docs: Docs,
    /// The visibility of the type it becomes.
    pub(crate) vis: Visibility,
    /// The name of that type, and of the scheme.
    pub(crate) name: Ident,
    /// How its credential travels.
    pub(crate) kind: DeclaredKind,
}

/// How the credential of a declared scheme travels.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum DeclaredKind {
    /// `header "X-Api-Key"`, with the name as written.
    Header(String),
    /// `cookie "session"`.
    Cookie(String),
    /// `query "token"`.
    Query(String),
    /// `openid "https://..."`, with the URL of the OpenID Connect discovery document.
    OpenId(String),
}

impl DeclaredKind {
    /// Where the credential travels.
    pub(crate) fn place(&self) -> Location {
        match self {
            Self::Header(name) => Location::Header(name.to_ascii_lowercase()),
            Self::Cookie(name) => Location::Cookie(name.clone()),
            Self::Query(key) => Location::Query(key.clone()),
            Self::OpenId(_) => Location::Authorization("Bearer"),
        }
    }
}

/// What a declared scheme is written with: `scheme Name = kind "value";`.
const WRITE_SCHEME: &str = "write `pub scheme Key = header \"X-Api-Key\";`, or `cookie \"session\"`, `query \"token\"`, `openid \"https://.../.well-known/openid-configuration\"`";

impl DeclaredScheme {
    /// A scheme declared with the name `name`, of the kind `kind` and its string `value`.
    ///
    /// # Errors
    ///
    /// Spanned on the token at fault: a name of the crate's schemes, `none` or one that is not ASCII; a kind that is
    /// none of the four; a header or a cookie whose name is not a token, a header that quoll, HTTP or another scheme
    /// reads, an empty query key or one that needs escaping, a URL that is not `http(s)://`.
    pub(crate) fn new(
        docs: Docs,
        vis: Visibility,
        name: Ident,
        kind: &Ident,
        value: &LitStr,
    ) -> Result<Self> {
        let shown = name.unraw().to_string();

        if matches!(shown.as_str(), "Bearer" | "Basic" | "none") {
            return Err(Error::new(
                name.span(),
                format!(
                    "`{shown}` is a word `#[auth(...)]` already takes: name the scheme otherwise, as `Key`"
                ),
            ));
        }
        if !shown.is_ascii() {
            return Err(Error::new(
                name.span(),
                format!(
                    "the scheme `{shown}` gives its name to HTTP's `WWW-Authenticate`, which is ASCII: name it with ASCII letters and digits"
                ),
            ));
        }

        let text = value.value();
        let kind = match kind.unraw().to_string().as_str() {
            "header" => {
                check_scheme_header(value)?;
                DeclaredKind::Header(text)
            }
            "cookie" => {
                if !is_token(&text) {
                    return Err(Error::new(
                        value.span(),
                        format!(
                            "`{text}` is not the name of a cookie: a name is ASCII letters, digits and the signs ! # $ % & ' * + - . ^ _ ` | ~, as in \"session\""
                        ),
                    ));
                }
                DeclaredKind::Cookie(text)
            }
            "query" => {
                let plain = |c: char| c.is_ascii_alphanumeric() || "-._~".contains(c);
                if text.is_empty() || !text.chars().all(plain) {
                    return Err(Error::new(
                        value.span(),
                        format!(
                            "`{text}` is not a query key a scheme reads: it is ASCII letters, digits and `-._~`, as in \"token\""
                        ),
                    ));
                }
                DeclaredKind::Query(text)
            }
            "openid" => {
                let url = text
                    .strip_prefix("https://")
                    .or_else(|| text.strip_prefix("http://"));
                if url.is_none_or(|rest| rest.is_empty() || rest.contains(char::is_whitespace)) {
                    return Err(Error::new(
                        value.span(),
                        "an OpenID Connect scheme takes the URL of its discovery document, `https://.../.well-known/openid-configuration`",
                    ));
                }
                DeclaredKind::OpenId(text)
            }
            _ => {
                return Err(Error::new(
                    kind.span(),
                    format!(
                        "`{kind}` is not a kind of scheme: a credential travels in a `header`, a `cookie`, a `query` key, or `openid`; {WRITE_SCHEME}"
                    ),
                ));
            }
        };

        Ok(Self {
            docs,
            vis,
            name,
            kind,
        })
    }
}

/// The header of a header scheme: a name, which neither quoll, HTTP nor another scheme reads.
fn check_scheme_header(value: &LitStr) -> Result<()> {
    check_header_name(value)?;

    let name = value.value();
    let taken = [
        (
            "Authorization",
            "`Bearer` and `Basic`",
            "take one of them in `#[auth(...)]`",
        ),
        (
            "Cookie",
            "`cookie` schemes",
            "declare the cookie, as `pub scheme Session = cookie \"session\";`",
        ),
    ];

    if let Some((header, what, instead)) = taken
        .iter()
        .find(|(header, ..)| header.eq_ignore_ascii_case(&name))
    {
        return Err(Error::new(
            value.span(),
            format!(
                "the header `{header}` is read by {what}: {instead}, or implement `quoll::rest::Scheme` for a scheme of your own"
            ),
        ));
    }

    Ok(())
}

/// Finds the schemes the contract declares among the ones its routes take, and checks what spans them.
pub(crate) fn resolve(api: &mut Api, errors: &mut ErrorList) {
    let declared: Vec<(String, Location)> = api
        .schemes
        .iter()
        .map(|scheme| (scheme.name.unraw().to_string(), scheme.kind.place()))
        .collect();

    for group in &mut api.groups {
        for route in &mut group.routes {
            let Some(auth) = &mut route.auth else {
                continue;
            };

            for scheme in &mut auth.schemes {
                if let Some(ident) = scheme.path.get_ident() {
                    if let Some((_, place)) =
                        declared.iter().find(|(name, _)| ident.unraw() == name)
                    {
                        scheme.kind = SchemeKind::Declared(place.clone());
                    }
                    continue;
                }

                // A path that ends with a declared scheme's name, or goes through `rest` to the crate's, would be
                // a second name for one scheme: `Key | self::Key` would take it twice. An application's own type
                // of the name of a declared scheme takes another name.
                let segments: Vec<String> = scheme
                    .path
                    .segments
                    .iter()
                    .map(|segment| segment.ident.unraw().to_string())
                    .collect();
                let [.., before, last] = segments.as_slice() else {
                    continue;
                };
                let crate_given = before == "rest" && (last == "Bearer" || last == "Basic");

                if !crate_given && !declared.iter().any(|(name, _)| name == last) {
                    continue;
                }

                let other = if crate_given {
                    ""
                } else {
                    ", or give a type of your own another name than the contract's scheme"
                };
                errors.push(Error::new_spanned(
                    &scheme.path,
                    format!(
                        "`{}` names the scheme `{last}`, which `#[auth(...)]` takes by its name alone: write `{last}`{other}",
                        scheme.word()
                    ),
                ));
            }
        }
    }

    for group in &api.groups {
        check_visibility(api, group, errors);

        for route in &group.routes {
            if let Err(error) = check_places(route) {
                errors.push(error);
            }
        }
    }
}

/// A declared scheme reaches as far as each group that takes it, whose server trait names it.
fn check_visibility(api: &Api, group: &Group, errors: &mut ErrorList) {
    let mut reported: Vec<&Scheme> = Vec::new();

    for scheme in group.routes.iter().flat_map(|route| route.schemes()) {
        let Some(declared) = api.schemes.iter().find(|declared| {
            scheme
                .path
                .get_ident()
                .is_some_and(|ident| ident.unraw() == declared.name.unraw())
        }) else {
            continue;
        };

        if reach(&declared.vis) >= reach(&group.vis)
            || reported.iter().any(|seen| seen.same(scheme))
        {
            continue;
        }

        reported.push(scheme);
        errors.push(Error::new(
            declared.name.span(),
            format!(
                "the scheme `{}` is less visible than the group `{}` that takes it, whose server trait names it: write `{} scheme {}`",
                declared.name,
                group.name,
                shown_vis(&group.vis),
                declared.name,
            ),
        ));
    }
}

/// Two schemes of an alternative read two places, and no argument of the route reads the place of one of its schemes.
fn check_places(route: &Route) -> Result<()> {
    let schemes: Vec<&Scheme> = route.schemes().collect();

    for (index, scheme) in schemes.iter().enumerate() {
        let Some(place) = scheme.place() else {
            continue;
        };

        if let Some(first) = schemes[..index]
            .iter()
            .find(|first| first.place().is_some_and(|first| first.same(&place)))
        {
            return Err(Error::new(
                scheme.span(),
                format!(
                    "`{}` and `{}` both read {}, so a request's credential would be both: take one of them",
                    first.word(),
                    scheme.word(),
                    place.shown(),
                ),
            ));
        }

        for (param, header) in route.headers() {
            let read = match &place {
                Location::Header(name) => name.eq_ignore_ascii_case(header),
                Location::Cookie(_) => header.eq_ignore_ascii_case("Cookie"),
                Location::Authorization(_) | Location::Query(_) => false,
            };

            if read {
                return Err(Error::new(
                    param.name.span(),
                    format!(
                        "the header `{header}` carries the credential of `{}`, which the route takes: the handler receives its caller, and the client's middleware sends it",
                        scheme.word()
                    ),
                ));
            }
        }

        if let Location::Query(key) = &place
            && let Some((param, _)) = route.query_params().find(|(_, query)| query == key)
        {
            return Err(Error::new(
                param.name.span(),
                format!(
                    "the query key `{key}` carries the credential of `{}`, which the route takes: name the argument's key otherwise",
                    scheme.word()
                ),
            ));
        }
    }

    Ok(())
}

impl Route {
    /// The schemes the route takes, in the order its `#[auth(...)]` writes them: none for a route that takes no
    /// credential.
    pub(crate) fn schemes(&self) -> impl Iterator<Item = &Scheme> {
        self.auth.iter().flat_map(|auth| &auth.schemes)
    }
}

/// The bounds of a group's server trait, from the schemes its routes take: `Authenticate<S>` for each, the callers of
/// the schemes of an alternative being one, and `Scoped` on a caller that a route with scopes needs.
#[derive(Debug)]
pub(crate) struct Bound<'a> {
    /// The scheme.
    pub(crate) scheme: &'a Scheme,
    /// The scheme whose caller this one gives, when it is in an alternative with a scheme that comes before it in the
    /// group.
    pub(crate) same_as: Option<&'a Scheme>,
    /// Whether its caller implements `Scoped`.
    pub(crate) scoped: bool,
}

/// The bounds of a group, one per scheme its routes take, each once, in the order they first appear.
#[derive(Debug)]
pub(crate) struct Bounds<'a>(pub(crate) Vec<Bound<'a>>);

impl Group {
    /// The bounds of the group's server trait. The schemes of an alternative give the caller of the first of them that
    /// appears in the group.
    pub(crate) fn bounds(&self) -> Bounds<'_> {
        let mut schemes: Vec<&Scheme> = Vec::new();
        for scheme in self.routes.iter().flat_map(Route::schemes) {
            if !schemes.iter().any(|seen| seen.same(scheme)) {
                schemes.push(scheme);
            }
        }

        let index = |scheme: &Scheme| {
            schemes
                .iter()
                .position(|seen| seen.same(scheme))
                .expect("each scheme of a route is among the group's")
        };

        // Each scheme starts as its own caller; an alternative joins its schemes under the first of them.
        let mut root: Vec<usize> = (0..schemes.len()).collect();
        let find = |root: &[usize], mut at: usize| {
            while root[at] != at {
                at = root[at];
            }
            at
        };
        let auths: Vec<&RouteAuth> = self
            .routes
            .iter()
            .filter_map(|route| route.auth.as_ref())
            .collect();

        for auth in &auths {
            let mut members: Vec<usize> = auth
                .schemes
                .iter()
                .map(|scheme| find(&root, index(scheme)))
                .collect();
            members.sort_unstable();
            members.dedup();

            if let Some((&first, rest)) = members.split_first() {
                for &member in rest {
                    root[member] = first;
                }
            }
        }

        let mut scoped = vec![false; schemes.len()];
        for auth in auths.iter().filter(|auth| !auth.scopes.is_empty()) {
            scoped[find(&root, index(&auth.schemes[0]))] = true;
        }

        let bounds = schemes
            .iter()
            .enumerate()
            .map(|(at, scheme)| {
                let first = find(&root, at);

                Bound {
                    scheme,
                    same_as: (first != at).then(|| schemes[first]),
                    scoped: first == at && scoped[at],
                }
            })
            .collect();

        Bounds(bounds)
    }
}

impl<'a> Bounds<'a> {
    /// The scheme whose caller the handlers of `route` receive: the first of the group's schemes in the alternative of
    /// the route's first scheme.
    pub(crate) fn caller_of(&self, route: &Route) -> Option<&'a Scheme> {
        let first = route.schemes().next()?;
        let bound = self.0.iter().find(|bound| bound.scheme.same(first))?;

        Some(bound.same_as.unwrap_or(bound.scheme))
    }
}

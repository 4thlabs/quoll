//! Fixed headers in the model: what the `#[header(...)]`s of an api, a group or a route say, what a route takes of
//! them, the nearest to it winning, and the checks that span the levels, once the api of a block is known.

use proc_macro2::Span;
use syn::{Error, LitStr, Path, Result, ext::IdentExt};

use super::{Api, ErrorList, Group, Location, RESERVED_HEADERS, Route, is_token};

/// A fixed header's value, as the contract writes it.
#[derive(Debug, Clone)]
pub(crate) enum Value {
    /// A string, which the macro checks.
    Literal(LitStr),
    /// A constant's name, which the generated code checks at compile time.
    Const(Path),
}

/// A header an item fixes, `#[header("X-GitHub-Api-Version" = "2022-11-28")]`.
#[derive(Debug, Clone)]
pub(crate) struct Fixed {
    /// Its name, as written.
    pub(crate) name: LitStr,
    /// Its value.
    pub(crate) value: Value,
}

impl Fixed {
    /// Where its value is written.
    pub(crate) fn value_span(&self) -> Span {
        match &self.value {
            Value::Literal(value) => value.span(),
            Value::Const(path) => syn::spanned::Spanned::span(path),
        }
    }

    /// Whether it is the header `name`, whose case does not count.
    pub(crate) fn is(&self, name: &str) -> bool {
        self.name.value().eq_ignore_ascii_case(name)
    }
}

/// One `#[header(...)]`, as the syntax reads it.
#[derive(Debug)]
pub(crate) enum HeaderAttr {
    /// `"name" = value`.
    Set(Fixed),
    /// `"name" = none`, which leaves a header the item would take.
    Leave(LitStr),
    /// `none`, which leaves every one, spanned on the `none`.
    LeaveAll(Span),
}

/// What the `#[header(...)]`s of an item say: the headers it fixes, and the ones it leaves of what it would take.
#[derive(Debug, Default, Clone)]
pub(crate) struct Headers {
    /// `#[header(none)]`: it takes none of its parent's, spanned on the `none`.
    pub(crate) leaves_all: Option<Span>,
    /// `#[header("name" = none)]`: the names it leaves, as written.
    pub(crate) leaves: Vec<LitStr>,
    /// Its own headers, in the order they are written.
    pub(crate) set: Vec<Fixed>,
}

impl Headers {
    /// The headers of an item from its attributes, in the order they are written.
    ///
    /// # Errors
    ///
    /// A header named twice, `none` twice, or a header left beside a `#[header(none)]`, spanned on the second.
    pub(crate) fn new(attrs: Vec<HeaderAttr>) -> Result<Self> {
        let mut errors = ErrorList::default();
        let mut headers = Self::default();

        for attr in attrs {
            let name = match &attr {
                HeaderAttr::Set(fixed) => &fixed.name,
                HeaderAttr::Leave(name) => name,
                HeaderAttr::LeaveAll(span) => {
                    if headers.leaves_all.is_some() {
                        errors.push(Error::new(
                            *span,
                            "an item takes one `#[header(none)]`: remove this one",
                        ));
                    }
                    headers.leaves_all = Some(*span);
                    continue;
                }
            };

            if headers
                .names()
                .any(|first| first.eq_ignore_ascii_case(&name.value()))
            {
                errors.push(Error::new(
                    name.span(),
                    format!(
                        "the header `{}` is already named here: an item names a header once",
                        name.value()
                    ),
                ));
                continue;
            }

            match attr {
                HeaderAttr::Set(fixed) => headers.set.push(fixed),
                HeaderAttr::Leave(name) => headers.leaves.push(name),
                HeaderAttr::LeaveAll(_) => {}
            }
        }

        if headers.leaves_all.is_some() {
            for name in &headers.leaves {
                errors.push(Error::new(
                    name.span(),
                    format!(
                        "`#[header(none)]` already leaves every header this item would take, `{}` among them: remove this `none`",
                        name.value()
                    ),
                ));
            }
        }

        errors.finish().map(|()| headers)
    }

    /// Whether it says nothing.
    pub(crate) fn is_empty(&self) -> bool {
        self.leaves_all.is_none() && self.leaves.is_empty() && self.set.is_empty()
    }

    /// The names it fixes or leaves.
    fn names(&self) -> impl Iterator<Item = String> + '_ {
        self.set
            .iter()
            .map(|fixed| fixed.name.value())
            .chain(self.leaves.iter().map(LitStr::value))
    }

    /// The header `name` it fixes.
    pub(crate) fn get(&self, name: &str) -> Option<&Fixed> {
        self.set.iter().find(|fixed| fixed.is(name))
    }

    /// Whether it leaves the header `name`.
    fn leaves(&self, name: &str) -> bool {
        self.leaves
            .iter()
            .any(|left| left.value().eq_ignore_ascii_case(name))
    }
}

/// A header's name in `#[header(...)]` is one HTTP takes, and that quoll leaves to the contract: `Content-Type` is
/// the body's, which a fixed one keeps.
pub(crate) fn check_name(name: &LitStr) -> Result<()> {
    let text = name.value();

    if !is_token(&text) {
        return Err(Error::new(
            name.span(),
            format!(
                "`{text}` is not the name of a header: a name is ASCII letters, digits and the signs ! # $ % & ' * + - . ^ _ ` | ~, as in \"X-GitHub-Api-Version\""
            ),
        ));
    }
    if let Some(reserved) = RESERVED_HEADERS
        .iter()
        .filter(|reserved| !reserved.eq_ignore_ascii_case("Content-Type"))
        .find(|reserved| reserved.eq_ignore_ascii_case(&text))
    {
        return Err(Error::new(
            name.span(),
            format!(
                "the header `{reserved}` is written by quoll or by HTTP itself, so a contract does not fix it"
            ),
        ));
    }

    Ok(())
}

/// A fixed header's value written as a string is visible ASCII, with spaces and tabs inside it only, as the generated
/// code checks a constant's.
pub(crate) fn check_value(value: &LitStr) -> Result<()> {
    let text = value.value();
    let blank = |c: char| c == ' ' || c == '\t';
    let visible = |c: char| blank(c) || c.is_ascii_graphic();

    if text.starts_with(blank) || text.ends_with(blank) || !text.chars().all(visible) {
        return Err(Error::new(value.span(), VALUE_MESSAGE));
    }

    Ok(())
}

/// What a fixed header's value is, which the errors of the macro and of the generated code say.
pub(crate) const VALUE_MESSAGE: &str = "a header's value is visible ASCII, with spaces and tabs inside it only, which HTTP drops at either end: give the value as it is sent";

/// The fixed headers a route sends, beside its api's: its own and its group's, the route's winning, and what it
/// leaves of the api's.
#[derive(Debug)]
pub(crate) struct RouteHeaders<'a> {
    /// Its own headers and its group's, each name once.
    pub(crate) own: Vec<&'a Fixed>,
    /// Whether it takes its api's: not under a `#[header(none)]` of its group or its own.
    pub(crate) inherits: bool,
    /// The names it leaves of the api's, its group's and its own.
    pub(crate) leaves: Vec<&'a LitStr>,
}

impl<'a> RouteHeaders<'a> {
    /// What the route takes of the fixed headers of its group and its own.
    pub(crate) fn of(group: &'a Group, route: &'a Route) -> Self {
        let mut own: Vec<&Fixed> = route.headers.set.iter().collect();

        if route.headers.leaves_all.is_none() {
            for fixed in &group.headers.set {
                let name = fixed.name.value();

                if !route.headers.leaves(&name) && route.headers.get(&name).is_none() {
                    own.push(fixed);
                }
            }
        }

        Self {
            own,
            inherits: group.headers.leaves_all.is_none() && route.headers.leaves_all.is_none(),
            leaves: group
                .headers
                .leaves
                .iter()
                .chain(&route.headers.leaves)
                .collect(),
        }
    }

    /// The header `name` the route takes of `api`'s, when it does not fix it itself.
    pub(crate) fn api_header<'b>(&self, api: &'b Headers, name: &str) -> Option<&'b Fixed> {
        let taken = self.inherits
            && !self
                .leaves
                .iter()
                .any(|left| left.value().eq_ignore_ascii_case(name))
            && !self.own.iter().any(|own| own.is(name));

        taken.then(|| api.get(name)).flatten()
    }

    /// The headers the route sends of `api`'s, which `None` is for a `rest::group!`, and its own: its own first.
    pub(crate) fn sent<'b>(&'b self, api: Option<&'b Headers>) -> Vec<&'b Fixed> {
        let mut sent: Vec<&Fixed> = self.own.clone();

        if let Some(api) = api {
            sent.extend(
                api.set
                    .iter()
                    .filter(|fixed| self.api_header(api, &fixed.name.value()).is_some()),
            );
        }

        sent
    }
}

/// Checks the fixed headers of an api's block, or of a `rest::group!`, whose api is `None`: what each item leaves is
/// set by its parent, a `Content-Type` keeps the media type of its route's body, and no header a route sends carries
/// the credential of a scheme it takes. What depends on the api of a `rest::group!` is checked by that api, at compile
/// time, but what the group leaves of the api's: any api may list it, and a `none` does nothing where the api does not
/// fix the header.
pub(crate) fn check(groups: &[Group], api: Option<(&Api, &Headers)>, errors: &mut ErrorList) {
    if let Some((_, headers)) = api {
        if let Some(span) = headers.leaves_all {
            errors.push(Error::new(span, NONE_ON_API));
        }
        for name in &headers.leaves {
            errors.push(Error::new(name.span(), NONE_ON_API));
        }
    }

    // A header of a group, where the error points, is reported once, whichever of its routes take the scheme.
    let mut reported: Vec<*const Fixed> = Vec::new();

    for group in groups {
        if let Some((api, headers)) = api {
            for name in &group.headers.leaves {
                if headers.get(&name.value()).is_none() {
                    errors.push(Error::new(
                        name.span(),
                        format!(
                            "the api `{}` fixes no header `{}`, so the group has none to leave: check its name, or remove this `none`",
                            api.name.unraw(),
                            name.value()
                        ),
                    ));
                }
            }
        }

        for route in &group.routes {
            check_leaves(group, route, api, errors);

            let headers = RouteHeaders::of(group, route);

            if let Err(error) = check_content_type(group, route, &headers, api) {
                errors.push(error);
            }
            if let Err((fixed, error)) = check_credentials(group, route, &headers, api) {
                let shared = group.headers.set.iter().any(|own| std::ptr::eq(own, fixed));

                if !shared || !reported.contains(&std::ptr::from_ref(fixed)) {
                    reported.push(fixed);
                    errors.push(error);
                }
            }
        }
    }
}

/// The message of a `none` on an api, which has no parent.
const NONE_ON_API: &str = "an api takes no header to leave: `none` goes on a group or a route, which leaves what its api or its group fixes";

/// Each header a route leaves is one its group fixes, or one its api fixes that its group does not leave. In a
/// `rest::group!`, whose api is not known, a header its group does not fix may be any api's.
fn check_leaves(
    group: &Group,
    route: &Route,
    api: Option<(&Api, &Headers)>,
    errors: &mut ErrorList,
) {
    for name in &route.headers.leaves {
        let text = name.value();

        if group.headers.get(&text).is_some() {
            continue;
        }

        let blocked = group.headers.leaves_all.is_some() || group.headers.leaves(&text);
        let set = match api {
            Some((_, headers)) => headers.get(&text).is_some(),
            // Any api may list a `rest::group!`, which leaves nothing where the api does not fix the header.
            None => true,
        };

        if blocked {
            // A `rest::group!` has no api of its own: what it leaves is what any api that lists it would give.
            let what = match (group.headers.leaves_all.is_some(), api) {
                (true, Some(_)) => "every header of its api, `#[header(none)]`".to_owned(),
                (true, None) => "every header it would take, `#[header(none)]`".to_owned(),
                (false, _) => format!("the header `{text}`"),
            };

            errors.push(Error::new(
                name.span(),
                format!(
                    "the group `{}` already leaves {what}, so the route has none to leave: remove this `none`",
                    group.name.unraw(),
                ),
            ));
        } else if let (Some((api, _)), false) = (api, set) {
            errors.push(Error::new(
                name.span(),
                format!(
                    "neither the group `{}` nor the api `{}` gives the route a header `{text}`, so it has none to leave: check its name, or remove this `none`",
                    group.name.unraw(),
                    api.name.unraw(),
                ),
            ));
        }
    }
}

/// The `Content-Type` a route takes keeps the media type of its body, adding a parameter: a route's own on a route
/// with a body of quoll's that has a media type, one of its group's or its api's on any route with a body, the others
/// sending none. A value written as a constant is checked by the generated code.
fn check_content_type(
    group: &Group,
    route: &Route,
    headers: &RouteHeaders,
    api: Option<(&Api, &Headers)>,
) -> Result<()> {
    let own = headers
        .own
        .iter()
        .copied()
        .find(|fixed| fixed.is("Content-Type"));
    let inherited = api.and_then(|(_, api)| headers.api_header(api, "Content-Type"));
    let Some(fixed) = own.or(inherited) else {
        return Ok(());
    };
    let is_routes = route.headers.get("Content-Type").is_some();
    let from = if is_routes {
        String::new()
    } else if own.is_some() {
        format!(" of the group `{}`", group.name.unraw())
    } else {
        let name = api
            .map(|(api, _)| api.name.unraw().to_string())
            .unwrap_or_default();
        format!(" of the api `{name}`")
    };
    let instead = "write `#[header(\"Content-Type\" = none)]` on the route";

    let Some((_, kind)) = route.body() else {
        if !is_routes {
            return Ok(());
        }

        return Err(Error::new(
            fixed.name.span(),
            "the route sends no body, and so no `Content-Type`: remove this header",
        ));
    };

    let Some(media_type) = kind.media_type else {
        let span = if is_routes {
            fixed.name.span()
        } else {
            route.span
        };
        let instead = if is_routes {
            "remove this header"
        } else {
            instead
        };

        return Err(Error::new(
            span,
            format!(
                "the route's body, `Bytes`, carries its content type, which a fixed `Content-Type`{from} would not keep: {instead}, and give the `Bytes` its content type"
            ),
        ));
    };

    let Value::Literal(value) = &fixed.value else {
        return Ok(());
    };
    if keeps_media_type(&value.value(), media_type) {
        return Ok(());
    }

    let (span, instead) = if is_routes {
        (
            value.span(),
            format!("write `{media_type}`, with the parameters the api asks for"),
        )
    } else {
        (route.span, instead.to_owned())
    };

    Err(Error::new(
        span,
        format!(
            "the `Content-Type` `{}`{from} is not the media type of the route's body, `{media_type}`, in UTF-8, which a fixed one keeps, adding a parameter as `{media_type}; charset=utf-8`: {instead}",
            value.value()
        ),
    ))
}

/// Whether the content type `value` has the media type `media_type`, whatever its parameters, as
/// `quoll::__private::headers::keeps_media_type` says: no charset but UTF-8, which quoll writes its bodies in.
pub(crate) fn keeps_media_type(value: &str, media_type: &str) -> bool {
    let parts = split_params(value);
    let essence = parts[0].trim();

    // A parameter is `name=value`, neither of them empty, a quoted string closed, and a charset is UTF-8.
    let parameter = |param: &&str| {
        let Some((name, value)) = param.split_once('=') else {
            return false;
        };
        let (name, value) = (name.trim(), value.trim());
        let Some(unquoted) = unquoted(value) else {
            return false;
        };

        !name.is_empty()
            && !value.is_empty()
            && !(name.eq_ignore_ascii_case("charset") && !unquoted.eq_ignore_ascii_case("utf-8"))
    };

    essence.eq_ignore_ascii_case(media_type) && parts[1..].iter().all(parameter)
}

/// The text of a parameter's value, as `quoll::__private::headers::unquoted` reads it: a token, which holds no quote,
/// or what a quoted string holds, whose last quote closes it and whose quotes inside are escaped. `None` for any
/// other, a quoted string left open or closed by an escaped quote, `"\"`, or a token with a quote in it.
fn unquoted(value: &str) -> Option<&str> {
    let Some(quoted) = value.strip_prefix('"') else {
        return (!value.contains('"')).then_some(value);
    };
    let bytes = quoted.as_bytes();
    let mut i = 0;

    while i < bytes.len() {
        match bytes[i] {
            // A quoted pair, `\"`, is one character of the string.
            b'\\' => i += 1,
            b'"' if i == bytes.len() - 1 => return Some(&quoted[..i]),
            b'"' => return None,
            _ => {}
        }
        i += 1;
    }

    None
}

/// A content type cut at each `;` outside a quoted string, as in `profile="a;b"`: its media type, then its parameters.
fn split_params(value: &str) -> Vec<&str> {
    let bytes = value.as_bytes();
    let mut parts = Vec::new();
    let mut quoted = false;
    let mut start = 0;
    let mut i = 0;

    while i < bytes.len() {
        match bytes[i] {
            // A quoted pair, `\"`, is one character of the string.
            b'\\' if quoted => i += 1,
            b'"' => quoted = !quoted,
            b';' if !quoted => {
                parts.push(&value[start..i]);
                start = i + 1;
            }
            _ => {}
        }
        i += 1;
    }

    parts.push(&value[start..]);
    parts
}

/// No header a route sends carries the credential of a scheme it takes, which the client's middleware sends: an
/// `Authorization` on a route that takes a scheme, the header of a header scheme, a `Cookie` beside a cookie scheme.
fn check_credentials<'a>(
    group: &Group,
    route: &Route,
    headers: &'a RouteHeaders<'a>,
    api: Option<(&Api, &'a Headers)>,
) -> std::result::Result<(), (&'a Fixed, Error)> {
    let Some(auth) = &route.auth else {
        return Ok(());
    };

    for fixed in headers.sent(api.map(|(_, headers)| headers)) {
        let name = fixed.name.value();
        let reads = auth.schemes.iter().find(|scheme| match scheme.place() {
            Some(Location::Authorization(_)) => name.eq_ignore_ascii_case("Authorization"),
            Some(Location::Header(header)) => header.eq_ignore_ascii_case(&name),
            Some(Location::Cookie(_)) => name.eq_ignore_ascii_case("Cookie"),
            _ => false,
        });
        // Any scheme may read `Authorization`, one of the application's among them.
        let carries = match (reads, auth.schemes.first()) {
            (Some(scheme), _) => format!("carries the credential of `{}`", scheme.word()),
            (None, Some(scheme)) if name.eq_ignore_ascii_case("Authorization") => format!(
                "carries the credentials of a route that takes a scheme, as it takes `{}`",
                scheme.word()
            ),
            _ => continue,
        };

        if headers.own.iter().any(|own| std::ptr::eq(*own, fixed)) {
            let message = format!(
                "the header `{name}` {carries}, which the route takes: the client's middleware sends it, so the contract does not fix it"
            );

            return Err((fixed, Error::new(fixed.name.span(), message)));
        }

        let api = api
            .map(|(api, _)| api.name.unraw().to_string())
            .unwrap_or_default();
        let message = format!(
            "the route `{}::{}` takes the header `{name}` of the api `{api}`, which {carries}: the client's middleware sends it, so write `#[header(\"{name}\" = none)]` on the route",
            group.name.unraw(),
            route.name.unraw(),
        );

        return Err((fixed, Error::new(route.span, message)));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::keeps_media_type;

    #[test]
    fn a_content_type_keeps_its_media_type_as_the_generated_check_says() {
        let kept = [
            "application/json",
            "Application/JSON ; charset=UTF-8",
            "application/json; profile=\"https://example.com/schema;v1\"; charset=utf-8",
            "application/json; note=\"a \\\" ; b\"",
        ];
        let refused = [
            "text/plain",
            "application/json; charset=iso-8859-1",
            "application/json;",
            "application/json; charset",
            "application/json; =x",
            "application/json; profile=\"open",
            r#"application/json; x="\""#,
            r#"application/json; x=a"b"#,
            r#"application/json; x="a"b""#,
        ];

        for value in kept {
            assert!(keeps_media_type(value, "application/json"), "{value}");
        }
        for value in refused {
            assert!(!keeps_media_type(value, "application/json"), "{value}");
        }
    }
}

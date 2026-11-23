//! What a group says of itself to the api that lists it, which `rest::group!` and `rest::api!` write: the routes of the
//! group below the api's prefix, the table of `rest::Route` an api builds from them under its prefix at compile time,
//! and the checks an api makes, at compile time too, of the routes of a group it lists, which the macro does not see.

use std::any::TypeId;
use std::marker::PhantomData;

use http::Method;

use crate::__private::auth::{Declared, Place};
use crate::__private::headers::{Fixed, RouteHeaders, keeps_media_type};
use crate::rest::{Problem, Route};

/// A group, as the descriptor `rest::group!` writes for it describes it: the type `__QuollGroupNotes` of the group
/// `Notes`.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not a group of a contract",
    label = "not a group",
    note = "an api lists the groups that `rest::group!` writes, by their path, `pub api Api = \"/api\" {{ notes::Notes }};`, or by their name alone where a glob import, `use notes::*;`, brings the items of the group"
)]
pub trait Group: 'static {
    /// What the group is.
    const INFO: GroupInfo;
}

/// A group: its name, its visibility and its routes.
#[derive(Debug)]
pub struct GroupInfo {
    /// Its name, `Notes`.
    pub name: &'static str,
    /// How far its visibility reaches: 0 private, 1 a module's, 2 the crate's, 3 everyone's.
    pub reach: u8,
    /// Its visibility as the contract writes it, `pub`, which a message shows.
    pub vis: &'static str,
    /// The function of its first route that has the shape of its errors named nowhere, `#[error(E)]` on the group
    /// or on the route, `None` when each has one: such a route takes no api's, and has problems.
    pub unshaped: Option<&'static str>,
    /// Its routes, in the order the contract writes them.
    pub routes: &'static [RouteInfo],
}

/// A route of a group, below the prefix of the api that lists the group.
#[derive(Debug)]
pub struct RouteInfo {
    /// The operation id, `notes_get`.
    pub id: &'static str,
    /// The route's function, `get`.
    pub function: &'static str,
    /// The method.
    pub method: Verb,
    /// The path below the api's prefix, its parameters written `{name}`: `/notes/{id}`, and empty for `"/"`.
    pub path: &'static str,
    /// The headers its arguments read, as the contract writes them.
    pub headers: &'static [&'static str],
    /// The keys of the query its string reads.
    pub query: &'static [&'static str],
    /// The names of the schemes it takes, in the order of its `#[auth(...)]`.
    pub schemes: &'static [&'static str],
    /// The types of the same schemes.
    pub scheme_types: &'static [TypeId],
    /// What each of the same schemes says of itself to the api's checks.
    pub scheme_infos: &'static [SchemeInfo],
    /// Whether its result is a stream.
    pub streams: bool,
    /// Its body.
    pub body: BodyInfo,
    /// The fixed headers it takes, beside its api's.
    pub fixed: RouteHeaders,
}

/// The body of a route, as a fixed `Content-Type` sees it.
#[derive(Debug, Clone, Copy)]
pub enum BodyInfo {
    /// None.
    None,
    /// A body of this media type: its JSON, a form or text.
    Of(&'static str),
    /// `Bytes`, which carry their content type.
    Bytes,
}

/// A scheme a route takes, as its type says: where its credential travels, when the macro knows it, and, for a scheme
/// a contract declares, how far its visibility reaches.
#[derive(Debug, Clone, Copy)]
pub struct SchemeInfo {
    /// Where its credential travels: the crate's `Bearer` and `Basic`, and the declared schemes.
    pub reads: Option<Reads>,
    /// How far a declared scheme's visibility reaches.
    pub reach: Option<u8>,
}

/// What a scheme says of itself at compile time, by its type: a declared scheme, which implements
/// [`Declared`], through the inherent constants of `Probe<S>`; any other through those of [`Unknown`], which the
/// generated code imports beside it.
#[derive(Debug)]
pub struct Probe<S: ?Sized>(PhantomData<S>);

impl<S: Declared> Probe<S> {
    /// Where its credential travels.
    pub const READS: Option<Reads> = Some(Reads::of(S::PLACE));
    /// How far its visibility reaches.
    pub const REACH: Option<u8> = Some(S::REACH);
    /// Whether it refuses a request from another origin that changes things.
    pub const CHECKS_ORIGIN: bool = S::CHECKS_ORIGIN;
}

/// What a scheme that the contract does not declare says of itself: nothing.
pub trait Unknown {
    /// Where its credential travels: unknown.
    const READS: Option<Reads> = None;
    /// How far its visibility reaches: not checked.
    const REACH: Option<u8> = None;
    /// Whether it refuses a request from another origin: not by quoll.
    const CHECKS_ORIGIN: bool = false;
}

impl<S: ?Sized> Unknown for Probe<S> {}

/// A method of a route, which can be copied at compile time, unlike `http::Method`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verb {
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

impl Verb {
    /// The method, as `http` has it.
    const fn method(self) -> Method {
        match self {
            Self::Get => Method::GET,
            Self::Post => Method::POST,
            Self::Put => Method::PUT,
            Self::Patch => Method::PATCH,
            Self::Delete => Method::DELETE,
            Self::Head => Method::HEAD,
            Self::Options => Method::OPTIONS,
        }
    }

    /// The method as HTTP writes it.
    const fn as_str(self) -> &'static str {
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
}

/// The length of the path of `route` under `prefix`: `/` alone when both are empty.
const fn full_len(prefix: &str, route: &RouteInfo) -> usize {
    match prefix.len() + route.path.len() {
        0 => 1,
        len => len,
    }
}

/// The length of the paths of `routes` under `prefix`, one after the other: the size of [`paths`].
pub const fn paths_len(prefix: &str, routes: &[RouteInfo]) -> usize {
    let mut len = 0;
    let mut i = 0;

    while i < routes.len() {
        len += full_len(prefix, &routes[i]);
        i += 1;
    }

    len
}

/// The paths of `routes` under `prefix`, one after the other, which [`routes`] cuts into the path of each route.
pub const fn paths<const N: usize>(prefix: &str, routes: &[RouteInfo]) -> [u8; N] {
    let mut bytes = [0; N];
    let mut at = 0;
    let mut i = 0;

    while i < routes.len() {
        if prefix.is_empty() && routes[i].path.is_empty() {
            bytes[at] = b'/';
            at += 1;
        }

        at = copy(&mut bytes, at, prefix);
        at = copy(&mut bytes, at, routes[i].path);
        i += 1;
    }

    bytes
}

/// Copies `text` into `bytes` at `at`, and gives where it ends.
const fn copy<const N: usize>(bytes: &mut [u8; N], mut at: usize, text: &str) -> usize {
    let text = text.as_bytes();
    let mut i = 0;

    while i < text.len() {
        bytes[at] = text[i];
        at += 1;
        i += 1;
    }

    at
}

/// The routes of a group under an api's prefix, `rest::Route`s, the path of each cut from `paths`, which [`paths`]
/// wrote for the same `routes` and the same prefix: the table the api's router and client take a route from.
pub const fn routes<const N: usize>(
    prefix: &str,
    group: &'static str,
    paths: &'static [u8],
    routes: &'static [RouteInfo],
) -> [Route; N] {
    const UNSET: Route = Route {
        operation_id: "",
        group: "",
        method: Method::GET,
        path: "",
        schemes: &[],
        scheme_types: &[],
        streams: false,
    };

    let mut table = [UNSET; N];
    let mut rest = paths;
    let mut i = 0;

    while i < N {
        let route = &routes[i];
        let (path, after) = rest.split_at(full_len(prefix, route));
        let Ok(path) = core::str::from_utf8(path) else {
            panic!("the paths of a group are cut where its routes' paths end");
        };
        let full = Route {
            operation_id: route.id,
            group,
            method: route.method.method(),
            path,
            schemes: route.schemes,
            scheme_types: route.scheme_types,
            streams: route.streams,
        };

        // A `Route` holds a method, which may not be dropped at compile time: the one replaced never had to be.
        core::mem::forget(core::mem::replace(&mut table[i], full));
        rest = after;
        i += 1;
    }

    table
}

/// Writes the table of the routes of the group `$group`, a type implementing [`Group`], under the api's `$prefix`: a
/// `static` named `$name`, of one `rest::Route` per route, built at compile time.
#[doc(hidden)]
#[macro_export]
macro_rules! __table {
    ($name:ident, $prefix:expr, $group:ty) => {
        static $name: [::quoll::rest::Route;
            <$group as ::quoll::__private::group::Group>::INFO
                .routes
                .len()] = {
            const ROUTES: &[::quoll::__private::group::RouteInfo] =
                <$group as ::quoll::__private::group::Group>::INFO.routes;
            const LEN: usize = ::quoll::__private::group::paths_len($prefix, ROUTES);
            static PATHS: [u8; LEN] = ::quoll::__private::group::paths::<LEN>($prefix, ROUTES);

            ::quoll::__private::group::routes::<{ ROUTES.len() }>(
                $prefix,
                <$group as ::quoll::__private::group::Group>::INFO.name,
                &PATHS,
                ROUTES,
            )
        };
    };
}

pub use crate::__table as table;

/// Checks, at compile time, that `G` is a group: the descriptor of a group an api lists.
pub const fn is_group<G: Group>() {}

/// The shape `E` of an api's errors, as its checks tell it by its type: [`IS_PROBLEM`](Shape::IS_PROBLEM) is `true`
/// for quoll's `rest::Problem` alone, by the impl of that type, and `false` for any other, by [`OtherShape`], whose
/// constant a path reaches only where no impl of the type's own has one.
#[derive(Debug)]
pub struct Shape<E>(PhantomData<fn() -> E>);

impl Shape<Problem> {
    /// The default shape: a route with none of its own has it, as the api does.
    pub const IS_PROBLEM: bool = true;
}

/// The constant of any shape but quoll's `rest::Problem`, which the code of an api brings into scope.
pub trait OtherShape {
    /// Another shape than problems.
    const IS_PROBLEM: bool = false;
}

impl<E> OtherShape for Shape<E> {}

/// Checks, at compile time, a group an api of the shape `shape` lists, problems when `problems` is set: the message
/// of a route of the group whose shape is named nowhere, unless the api's is problems. A `rest::group!` writes its
/// client's trait before any api lists it, and cannot take the api's.
pub const fn check_error(group: &GroupInfo, problems: bool, shape: &str) -> Option<Message> {
    let Some(route) = group.unshaped else {
        return None;
    };

    if problems {
        return None;
    }

    Some(
        Message::new()
            .push("the route `")
            .push(group.name)
            .push("::")
            .push(route)
            .push("` takes no `#[error(...)]`, and a `rest::group!` cannot take its api's: write `#[error(")
            .push(shape)
            .push(")]` on the group `")
            .push(group.name)
            .push("` in its block, or on each of its routes, the type named from the group's module, or the shape its errors have"),
    )
}

/// The api that lists groups, as its checks see it.
#[derive(Debug)]
pub struct ApiInfo {
    /// Its name, `Api`.
    pub name: &'static str,
    /// Its prefix, `/api`.
    pub prefix: &'static str,
    /// How far its visibility reaches, as [`GroupInfo::reach`].
    pub reach: u8,
    /// Its visibility as the contract writes it, `pub`.
    pub vis: &'static str,
    /// Its fixed headers, `#[header("name" = value)]`, which the routes of its groups take.
    pub headers: &'static [Fixed],
}

/// Checks, at compile time, a group an api lists against the api and the groups before it: the message of the first
/// mistake, which says what to write instead, and which the generated code panics with, so that the error points at
/// the group in the api's list.
///
/// A mistake is when the group is less visible than the api, takes a declared scheme less visible than itself, has a route of a
/// method and a path that another route has, or of a path another route names with other parameters, or of an
/// operation id another route has; when a route takes two schemes of an api that read one place, or reads the
/// credential of a scheme it takes in an argument or sends it in a fixed header; or when a route takes the api's
/// `Content-Type` that its body does not keep. A header the group or a route leaves is not checked: any api may list
/// the group, and a `none` does nothing where the api does not fix the header.
pub const fn check(api: &ApiInfo, earlier: &[&GroupInfo], group: &GroupInfo) -> Option<Message> {
    if group.reach < api.reach {
        return Some(
            Message::new()
                .push("the group `")
                .push(group.name)
                .push("` is less visible than the api `")
                .push(api.name)
                .push("`, whose router and client name the group's items: write `")
                .push(api.vis)
                .push(" group ")
                .push(group.name)
                .push("`"),
        );
    }

    let mut i = 0;
    while i < group.routes.len() {
        let route = &group.routes[i];

        if let Some(error) = check_schemes(api, group, route) {
            return Some(error);
        }
        if let Some(error) = check_content_type(api, group, route) {
            return Some(error);
        }

        let mut g = 0;
        while g < earlier.len() {
            let mut j = 0;
            while j < earlier[g].routes.len() {
                if let Some(error) =
                    check_pair(api, earlier[g], &earlier[g].routes[j], group, route)
                {
                    return Some(error);
                }
                j += 1;
            }
            g += 1;
        }
        i += 1;
    }

    None
}

/// Checks a route of `group` against a route of an earlier group: one method and path, one path with its parameters
/// named otherwise, one operation id.
const fn check_pair(
    api: &ApiInfo,
    first_group: &GroupInfo,
    first: &RouteInfo,
    group: &GroupInfo,
    route: &RouteInfo,
) -> Option<Message> {
    if same(first.id, route.id) {
        return Some(
            Message::new()
                .push("the operation id `")
                .push(route.id)
                .push("` of the route `")
                .push(route.function)
                .push("` of `")
                .push(group.name)
                .push("` is already the one of the route `")
                .push(first.function)
                .push("` of `")
                .push(first_group.name)
                .push("`: rename one of them, as an operation id, the group in snake case and the function, names one route of the api"),
        );
    }

    if !same_shape(first.path, route.path) {
        return None;
    }

    if first.method as u8 == route.method as u8 {
        return Some(
            Message::new()
                .push(route.method.as_str())
                .push(" ")
                .push(api.prefix)
                .push(route.path)
                .push(", the route `")
                .push(group.name)
                .push("::")
                .push(route.function)
                .push("`, is already the route `")
                .push(first_group.name)
                .push("::")
                .push(first.function)
                .push("`: a method and a path belong to one route"),
        );
    }

    if let Some((named, other)) = renamed(first.path, route.path) {
        return Some(
            Message::new()
                .push("the path of the route `")
                .push(group.name)
                .push("::")
                .push(route.function)
                .push("` is the one of the route `")
                .push(first_group.name)
                .push("::")
                .push(first.function)
                .push("`, with `{")
                .push(other)
                .push("}` written for `{")
                .push(named)
                .push("}`: a router takes them for one path, so name the parameter `{")
                .push(named)
                .push("}` in both"),
        );
    }

    None
}

/// Where a scheme's credential travels, for the schemes the macro knows: the crate's and the declared ones.
#[derive(Debug, Clone, Copy)]
pub enum Reads {
    /// `Authorization`, with the scheme of its value.
    Authorization(&'static str),
    /// A header, lowercase.
    Header(&'static str),
    /// A cookie.
    Cookie(&'static str),
    /// A key of the query.
    Query(&'static str),
}

impl Reads {
    /// Where the credential of a declared scheme travels.
    const fn of(place: Place) -> Self {
        match place {
            Place::Header(header) => Self::Header(header),
            Place::Cookie(cookie) => Self::Cookie(cookie),
            Place::Query(key) => Self::Query(key),
            Place::Bearer => Self::Authorization("Bearer"),
        }
    }

    /// Whether two schemes reading these places read one credential.
    const fn same(self, other: Self) -> bool {
        match (self, other) {
            (Self::Authorization(first), Self::Authorization(second))
            | (Self::Cookie(first), Self::Cookie(second))
            | (Self::Query(first), Self::Query(second)) => same(first, second),
            (Self::Header(first), Self::Header(second)) => same_ignoring_case(first, second),
            _ => false,
        }
    }

    /// Adds the place, as a message names it.
    const fn shown(self, message: Message) -> Message {
        match self {
            Self::Authorization(scheme) => message
                .push("the header `Authorization: ")
                .push(scheme)
                .push("`"),
            Self::Header(name) => message.push("the header `").push(name).push("`"),
            Self::Cookie(name) => message.push("the cookie `").push(name).push("`"),
            Self::Query(key) => message.push("the query key `").push(key).push("`"),
        }
    }
}

/// The header `name` of `headers`.
const fn fixed(headers: &[Fixed], name: &str) -> Option<Fixed> {
    let mut i = 0;
    while i < headers.len() {
        if same_ignoring_case(headers[i].name, name) {
            return Some(headers[i]);
        }
        i += 1;
    }

    None
}

/// The header `name` a route takes of its api's: not one it fixes itself, leaves, or under a `#[header(none)]`.
const fn api_header(api: &ApiInfo, route: &RouteInfo, name: &str) -> Option<Fixed> {
    let headers = route.fixed;

    if !headers.inherits || fixed(headers.own, name).is_some() {
        return None;
    }

    let mut i = 0;
    while i < headers.leaves.len() {
        if same_ignoring_case(headers.leaves[i], name) {
            return None;
        }
        i += 1;
    }

    fixed(api.headers, name)
}

/// Checks that a route that takes its api's `Content-Type` has a body that keeps its media type.
const fn check_content_type(
    api: &ApiInfo,
    group: &GroupInfo,
    route: &RouteInfo,
) -> Option<Message> {
    let Some(content_type) = api_header(api, route, "Content-Type") else {
        return None;
    };
    let message = Message::new()
        .push("the route `")
        .push(group.name)
        .push("::")
        .push(route.function)
        .push("` takes the `Content-Type` `")
        .push(content_type.value)
        .push("` of the api `")
        .push(api.name)
        .push("`, ");

    match route.body {
        BodyInfo::None => None,
        BodyInfo::Bytes => Some(message.push(
            "which its body, `Bytes`, would not keep, as they carry their content type: write `#[header(\"Content-Type\" = none)]` on the route, and give the `Bytes` its content type",
        )),
        BodyInfo::Of(media_type) => {
            if keeps_media_type(content_type.value, media_type) {
                return None;
            }

            Some(
                message
                    .push("which is not the media type of its body, `")
                    .push(media_type)
                    .push("`, in UTF-8: write `#[header(\"Content-Type\" = none)]` on the route, or a `Content-Type` of its body's media type"),
            )
        }
    }
}

/// Checks the schemes of a route of `group` the macro of the group did not know, by what their types say: a declared
/// one as visible as the group, each reading another place than the other schemes of the route, than its arguments,
/// and than the fixed headers it sends.
const fn check_schemes(api: &ApiInfo, group: &GroupInfo, route: &RouteInfo) -> Option<Message> {
    let mut i = 0;
    while i < route.scheme_infos.len() {
        let name = route.schemes[i];
        let info = route.scheme_infos[i];

        if let Some(reach) = info.reach
            && reach < group.reach
        {
            return Some(
                Message::new()
                    .push("the scheme `")
                    .push(name)
                    .push("` is less visible than the group `")
                    .push(group.name)
                    .push("` that takes it, whose server trait names it: write `")
                    .push(group.vis)
                    .push(" scheme ")
                    .push(name)
                    .push("`"),
            );
        }

        let Some(place) = info.reads else {
            i += 1;
            continue;
        };

        let mut j = 0;
        while j < i {
            if let Some(first) = route.scheme_infos[j].reads
                && first.same(place)
            {
                let message = Message::new()
                    .push("`")
                    .push(route.schemes[j])
                    .push("` and `")
                    .push(name)
                    .push("`, which the route `")
                    .push(group.name)
                    .push("::")
                    .push(route.function)
                    .push("` takes, both read ");

                return Some(
                    place
                        .shown(message)
                        .push(", so a request's credential would be both: take one of them"),
                );
            }
            j += 1;
        }

        if let Some(error) = check_arguments(group, route, name, place) {
            return Some(error);
        }

        let header = match place {
            Reads::Header(name) => Some(name),
            Reads::Cookie(_) => Some("Cookie"),
            Reads::Authorization(_) | Reads::Query(_) => None,
        };

        if let Some(header) = header
            && let Some(error) = check_fixed(api, group, route, header, name, true)
        {
            return Some(error);
        }
        i += 1;
    }

    check_authorization(api, group, route)
}

/// Checks that a route that takes a scheme sends no fixed `Authorization`, which the client's middleware sends: the
/// credential of a scheme that reads it, or the header of any credential, which a scheme the macro does not know may
/// read.
const fn check_authorization(
    api: &ApiInfo,
    group: &GroupInfo,
    route: &RouteInfo,
) -> Option<Message> {
    if route.schemes.is_empty() {
        return None;
    }

    let mut i = 0;
    while i < route.scheme_infos.len() {
        if let Some(Reads::Authorization(_)) = route.scheme_infos[i].reads {
            return check_fixed(api, group, route, "Authorization", route.schemes[i], true);
        }
        i += 1;
    }

    check_fixed(api, group, route, "Authorization", route.schemes[0], false)
}

/// Checks that no fixed header `name` a route sends, its own or its api's, is the client's middleware's: the
/// credential of `scheme`, which the route takes, when `reads` says that the scheme reads it, or else the
/// `Authorization` of any credential on a route that takes a scheme, as `scheme`.
const fn check_fixed(
    api: &ApiInfo,
    group: &GroupInfo,
    route: &RouteInfo,
    name: &str,
    scheme: &str,
    reads: bool,
) -> Option<Message> {
    let message = Message::new()
        .push("the route `")
        .push(group.name)
        .push("::")
        .push(route.function);
    let own = fixed(route.fixed.own, name);
    let (header, message) = match own {
        Some(header) => (
            header,
            message.push("` fixes the header `").push(header.name),
        ),
        None => match api_header(api, route, name) {
            Some(header) => (
                header,
                message
                    .push("` takes the header `")
                    .push(header.name)
                    .push("` of the api `")
                    .push(api.name),
            ),
            None => return None,
        },
    };
    let message = if reads {
        message
            .push("`, which carries the credential of `")
            .push(scheme)
            .push("`, a scheme it takes")
    } else {
        message
            .push("`, which carries the credentials of a route that takes a scheme, as it takes `")
            .push(scheme)
            .push("`")
    };

    if own.is_some() {
        return Some(
            message.push(": the client's middleware sends it, so the contract does not fix it"),
        );
    }

    Some(
        message
            .push(": the client's middleware sends it, so write `#[header(\"")
            .push(header.name)
            .push("\" = none)]` on the route"),
    )
}

/// Checks that no argument of a route reads the credential of `scheme`, which it takes and which travels in `place`.
const fn check_arguments(
    group: &GroupInfo,
    route: &RouteInfo,
    scheme: &str,
    place: Reads,
) -> Option<Message> {
    let mut i = 0;
    while i < route.headers.len() {
        let header = route.headers[i];
        let read = match place {
            Reads::Header(name) => same_ignoring_case(name, header),
            Reads::Cookie(_) => same_ignoring_case("Cookie", header),
            Reads::Authorization(_) | Reads::Query(_) => false,
        };

        if read {
            return Some(
                Message::new()
                    .push("the header `")
                    .push(header)
                    .push("` carries the credential of `")
                    .push(scheme)
                    .push("`, which the route `")
                    .push(group.name)
                    .push("::")
                    .push(route.function)
                    .push("` takes: the handler receives its caller, and the client's middleware sends it"),
            );
        }
        i += 1;
    }

    let Reads::Query(key) = place else {
        return None;
    };

    let mut i = 0;
    while i < route.query.len() {
        if same(route.query[i], key) {
            return Some(
                Message::new()
                    .push("the query key `")
                    .push(key)
                    .push("` carries the credential of `")
                    .push(scheme)
                    .push("`, which the route `")
                    .push(group.name)
                    .push("::")
                    .push(route.function)
                    .push("` takes: name the argument's key otherwise"),
            );
        }
        i += 1;
    }

    None
}

/// Whether two strings are one.
const fn same(first: &str, second: &str) -> bool {
    let (first, second) = (first.as_bytes(), second.as_bytes());
    if first.len() != second.len() {
        return false;
    }

    let mut i = 0;
    while i < first.len() {
        if first[i] != second[i] {
            return false;
        }
        i += 1;
    }

    true
}

/// Whether two names are one, their ASCII letters compared without their case, as a header's are.
const fn same_ignoring_case(first: &str, second: &str) -> bool {
    first.eq_ignore_ascii_case(second)
}

/// The segment of `path` that starts at `at`, after its `/`, and where the next one starts; `None` at the end.
const fn segment(path: &str, at: usize) -> Option<(&str, usize)> {
    let bytes = path.as_bytes();
    if at >= bytes.len() {
        return None;
    }

    // Past the `/`.
    let start = at + 1;
    let mut end = start;
    while end < bytes.len() && bytes[end] != b'/' {
        end += 1;
    }

    let (_, rest) = path.split_at(start);
    let (segment, _) = rest.split_at(end - start);

    Some((segment, end))
}

/// Whether a segment is a `{name}`.
const fn is_param(segment: &str) -> bool {
    matches!(segment.as_bytes().first(), Some(b'{'))
}

/// Whether two paths are one to a router: their segments, their parameters' names aside.
const fn same_shape(first: &str, second: &str) -> bool {
    let (mut a, mut b) = (0, 0);

    loop {
        match (segment(first, a), segment(second, b)) {
            (None, None) => return true,
            (Some((x, next_a)), Some((y, next_b))) => {
                let alike = if is_param(x) || is_param(y) {
                    is_param(x) && is_param(y)
                } else {
                    same(x, y)
                };
                if !alike {
                    return false;
                }

                a = next_a;
                b = next_b;
            }
            _ => return false,
        }
    }
}

/// The names, without their braces, of the first parameter that two paths of one shape name in two ways.
const fn renamed<'a>(first: &'a str, second: &'a str) -> Option<(&'a str, &'a str)> {
    let (mut a, mut b) = (0, 0);

    while let (Some((x, next_a)), Some((y, next_b))) = (segment(first, a), segment(second, b)) {
        if is_param(x) && !same(x, y) {
            return Some((unbraced(x), unbraced(y)));
        }

        a = next_a;
        b = next_b;
    }

    None
}

/// The name of a `{name}`.
const fn unbraced(param: &str) -> &str {
    let (_, name) = param.split_at(1);
    let (name, _) = name.split_at(name.len() - 1);

    name
}

/// A message built at compile time, cut at its capacity, which no message of a contract reaches.
#[derive(Debug)]
pub struct Message {
    bytes: [u8; 1024],
    len: usize,
}

impl Message {
    const fn new() -> Self {
        Self {
            bytes: [0; 1024],
            len: 0,
        }
    }

    const fn push(mut self, text: &str) -> Self {
        let text = text.as_bytes();
        let mut i = 0;

        while i < text.len() && self.len < self.bytes.len() {
            self.bytes[self.len] = text[i];
            self.len += 1;
            i += 1;
        }

        self
    }

    /// The message.
    pub const fn as_str(&self) -> &str {
        let (written, _) = self.bytes.split_at(self.len);

        match core::str::from_utf8(written) {
            Ok(text) => text,
            Err(error) => {
                // Cut in a character: up to it.
                let (valid, _) = written.split_at(error.valid_up_to());

                match core::str::from_utf8(valid) {
                    Ok(text) => text,
                    Err(_) => "",
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROUTES: &[RouteInfo] = &[
        RouteInfo {
            id: "notes_get",
            function: "get",
            method: Verb::Get,
            path: "/notes/{id}",
            headers: &[],
            query: &[],
            schemes: &[],
            scheme_types: &[],
            scheme_infos: &[],
            streams: false,
            body: BodyInfo::None,
            fixed: NO_HEADERS,
        },
        RouteInfo {
            id: "notes_home",
            function: "home",
            method: Verb::Get,
            path: "",
            headers: &[],
            query: &[],
            schemes: &["Bearer"],
            scheme_types: &[],
            scheme_infos: &[SchemeInfo {
                reads: Some(Reads::Authorization("Bearer")),
                reach: None,
            }],
            streams: true,
            body: BodyInfo::None,
            fixed: NO_HEADERS,
        },
    ];

    const NO_HEADERS: RouteHeaders = RouteHeaders {
        own: &[],
        inherits: true,
        leaves: &[],
    };

    #[test]
    fn a_table_puts_each_route_under_the_prefix() {
        const LEN: usize = paths_len("/api", ROUTES);
        static PATHS: [u8; LEN] = paths::<LEN>("/api", ROUTES);
        static TABLE: [Route; 2] = routes::<2>("/api", "Notes", &PATHS, ROUTES);

        assert_eq!(TABLE[0].path(), "/api/notes/{id}");
        assert_eq!(TABLE[0].operation_id(), "notes_get");
        assert_eq!(TABLE[0].group(), "Notes");
        assert_eq!(TABLE[1].path(), "/api");
        assert_eq!(TABLE[1].schemes(), ["Bearer"]);
        assert!(TABLE[1].streams);
    }

    #[test]
    fn the_root_of_an_api_with_no_prefix_is_a_slash() {
        const LEN: usize = paths_len("", ROUTES);
        static PATHS: [u8; LEN] = paths::<LEN>("", ROUTES);
        static TABLE: [Route; 2] = routes::<2>("", "Notes", &PATHS, ROUTES);

        assert_eq!(TABLE[0].path(), "/notes/{id}");
        assert_eq!(TABLE[1].path(), "/");
    }

    #[test]
    fn paths_of_one_shape_are_told_by_their_segments() {
        assert!(same_shape("/notes/{id}", "/notes/{key}"));
        assert!(same_shape("", ""));
        assert!(!same_shape("/notes/{id}", "/notes/new"));
        assert!(!same_shape("/notes/{id}", "/notes/{id}/files"));
        assert!(!same_shape("/notes", ""));
        assert_eq!(
            renamed("/notes/{id}/files", "/notes/{key}/files"),
            Some(("id", "key"))
        );
        assert_eq!(renamed("/notes/{id}", "/notes/{id}"), None);
    }

    #[test]
    fn a_message_is_cut_at_its_capacity_on_a_character() {
        let mut message = Message::new();
        for _ in 0..600 {
            message = message.push("é");
        }

        assert_eq!(message.as_str().chars().count(), 512);
    }
}

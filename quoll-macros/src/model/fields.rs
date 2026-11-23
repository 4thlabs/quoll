//! Fixed fields of a body in the model: what the `#[body(...)]`s of a route say, the checks on the route that takes
//! them, and when two routes of one method and path are told apart by them.

use syn::{Error, LitStr, Result, ext::IdentExt};

use super::{Api, ErrorList, Method, Route, Segment, shape_of};
use crate::known::BodyKind;

/// The value of a fixed field, as the contract writes it.
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum FieldValue {
    /// `true` or `false`.
    Bool(bool),
    /// An integer an `i64` holds.
    Int(i64),
    /// An integer above `i64::MAX`.
    Uint(u64),
    /// A number with a fraction or an exponent.
    Float(f64),
    /// A string.
    Str(String),
}

impl FieldValue {
    /// Whether it tells a route apart from one that fixes `other` for the same field, as
    /// `quoll::__private::fields::Value::differs_from` says: both of one kind, numbers compared as numbers, and
    /// different.
    #[expect(clippy::cast_precision_loss, reason = "a float is compared as a float")]
    #[expect(
        clippy::float_cmp,
        reason = "the values are the ones the contract writes"
    )]
    pub(crate) fn differs_from(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Bool(a), Self::Bool(b)) => a != b,
            (Self::Str(a), Self::Str(b)) => a != b,
            (Self::Int(a), Self::Int(b)) => a != b,
            (Self::Uint(a), Self::Uint(b)) => a != b,
            (Self::Int(_), Self::Uint(_)) | (Self::Uint(_), Self::Int(_)) => true,
            (Self::Float(a), Self::Float(b)) => a != b,
            (Self::Int(a), Self::Float(b)) | (Self::Float(b), Self::Int(a)) => *a as f64 != *b,
            (Self::Uint(a), Self::Float(b)) | (Self::Float(b), Self::Uint(a)) => *a as f64 != *b,
            _ => false,
        }
    }

    /// Whether it is of the kind of `other`: a bool, a number or a string.
    fn same_kind(&self, other: &Self) -> bool {
        let kind = |value: &Self| match value {
            Self::Bool(_) => 0,
            Self::Int(_) | Self::Uint(_) | Self::Float(_) => 1,
            Self::Str(_) => 2,
        };

        kind(self) == kind(other)
    }
}

impl std::fmt::Display for FieldValue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Bool(value) => write!(f, "{value}"),
            Self::Int(value) => write!(f, "{value}"),
            Self::Uint(value) => write!(f, "{value}"),
            Self::Float(value) => write!(f, "{value:?}"),
            Self::Str(value) => write!(f, "{value:?}"),
        }
    }
}

/// A field a route fixes in its body, `#[body("stream" = true)]`.
#[derive(Debug, Clone)]
pub(crate) struct BodyField {
    /// Its name, as written.
    pub(crate) name: LitStr,
    /// Its value.
    pub(crate) value: FieldValue,
}

/// The fields of a route, from its `#[body(...)]`s, in the order they are written.
///
/// # Errors
///
/// A field named twice, spanned on the second.
pub(crate) fn unique(fields: Vec<BodyField>) -> Result<Vec<BodyField>> {
    let mut errors = ErrorList::default();
    let mut kept: Vec<BodyField> = Vec::with_capacity(fields.len());

    for field in fields {
        if kept
            .iter()
            .any(|first| first.name.value() == field.name.value())
        {
            errors.push(Error::new(
                field.name.span(),
                format!(
                    "the field `{}` is already fixed here: a route fixes a field once",
                    field.name.value()
                ),
            ));
            continue;
        }

        kept.push(field);
    }

    errors.finish().map(|()| kept)
}

/// A route that fixes fields has a body they are added to: a struct or a map that serde writes, its JSON or a
/// `Form<T>`, not `Text` or `Bytes`.
pub(crate) fn check_body(fields: &[BodyField], body: Option<BodyKind>) -> Result<()> {
    let Some(first) = fields.first() else {
        return Ok(());
    };

    match body {
        None => Err(Error::new(
            first.name.span(),
            "the route sends no body, to which `#[body(...)]` adds its field: give the route a body, or remove this attribute",
        )),
        Some(kind) if kind.format.is_none() => Err(Error::new(
            first.name.span(),
            format!(
                "a `{}` body is no struct or map, to which `#[body(...)]` adds its field: send a type of the application, as JSON or a `Form<T>`",
                kind.document
            ),
        )),
        Some(_) => Ok(()),
    }
}

/// Whether a route that fixes `first` is told apart from one that fixes `second`: a field both fix, to values that
/// differ.
pub(crate) fn tell_apart(first: &[BodyField], second: &[BodyField]) -> bool {
    first.iter().any(|a| {
        second
            .iter()
            .any(|b| a.name.value() == b.name.value() && a.value.differs_from(&b.value))
    })
}

/// Why two routes of one method and path are not told apart, when they fix fields: what the message of their clash
/// adds.
pub(crate) fn why_not_apart(first: &[BodyField], second: &[BodyField]) -> String {
    if first.is_empty() && second.is_empty() {
        return String::new();
    }

    let kinds = first.iter().find_map(|a| {
        second
            .iter()
            .find(|b| a.name.value() == b.name.value() && !a.value.same_kind(&b.value))
    });

    match kinds {
        Some(field) => format!(
            ", unless a field both fix in their `#[body(...)]` tells them apart, and `{}` takes values of two kinds, which a form writes alike: give it values of one kind, two bools, two numbers or two strings",
            field.name.value()
        ),
        None => ", unless a field both fix in their `#[body(...)]` tells them apart, of one kind and two values: fix one in both, as `#[body(\"stream\" = false)]` and `#[body(\"stream\" = true)]`".to_owned(),
    }
}

/// Two routes of one method and path, told apart by their fields, are not described by the OpenAPI document yet: in
/// a block with a document, the api's or a `quoll::group!`'s, the second of each pair does not compile.
pub(crate) fn check_document(api: &Api, what: &str, errors: &mut ErrorList) {
    if !api.openapi {
        return;
    }

    let mut seen: Vec<(Method, Vec<Segment>, String)> = Vec::new();

    for group in &api.groups {
        for route in &group.routes {
            let shape = shape_of(&route.path);
            let first = seen
                .iter()
                .find(|(method, first, _)| *method == route.method && *first == shape);

            if let Some((_, _, first)) = first {
                errors.push(shared_in_document(route, first, what));
            } else {
                seen.push((
                    route.method,
                    shape,
                    format!("{}::{}", group.name.unraw(), route.name.unraw()),
                ));
            }
        }
    }
}

/// The error of `route`, which shares its method and path with the route `first` in a block with a document.
fn shared_in_document(route: &Route, first: &str, what: &str) -> Error {
    Error::new(
        route.span,
        format!(
            "{} on this path is the route `{first}` too, told apart by their `#[body(...)]`, which the OpenAPI document does not describe yet: write `#[openapi(none)]` on {what}, for a contract that is only called",
            route.method.as_str()
        ),
    )
}

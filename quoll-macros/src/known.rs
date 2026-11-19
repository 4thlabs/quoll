//! The types of quoll a contract names by their name, which the macro recognizes by the last segment of their path:
//! what each is as a result and as a body, the helpers the generated code calls for it, and the check that it is
//! quoll's. A new type of the kind is one row of [`KNOWN`], and its helpers in `quoll`.

use syn::Type;

use crate::model::peel;

/// A type the macro recognizes by its name.
#[derive(Debug)]
pub(crate) struct Known {
    /// The last segment of its path.
    pub(crate) name: &'static str,
    /// The function of `quoll::__private::known` that checks it is quoll's.
    pub(crate) check: &'static str,
    /// What its one type argument is, for a type that takes one, which the message of a type with none or two
    /// names: the body, each item.
    pub(crate) wraps: Option<&'static str>,
    /// What it is as a result, when it is one.
    pub(crate) result: Option<ResultKind>,
    /// What it is as a body, when it is one.
    pub(crate) body: Option<BodyKind>,
    /// Whether it is a stream, which a route answers but never reads as its body.
    pub(crate) stream: bool,
}

/// How a result is answered by the server and read by the client: the helpers the generated code calls.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ResultKind {
    /// The function of `quoll::__private::server` that answers it.
    pub(crate) answer: &'static str,
    /// The content type the client asks for, a constant of `quoll::__private::client`.
    pub(crate) accept: &'static str,
    /// The method of the client's `Answer` that reads it.
    pub(crate) read: &'static str,
    /// Whether the client returns the type it wraps, as the status is only the server's answer.
    pub(crate) unwrapped: bool,
    /// The variant of `quoll::__private::openapi::Reply` that describes it.
    pub(crate) document: &'static str,
    /// Whether the document has the schema of the type: its own for JSON, the one it wraps for the others, none for
    /// text and bytes.
    pub(crate) schema: bool,
}

impl ResultKind {
    /// Any type the macro does not recognize: 200, its JSON.
    pub(crate) const JSON: Self = Self::new("ok", "JSON", "json", "Json", true);
    /// `Created<T>`: 201, the JSON of the `T` the client returns.
    pub(crate) const CREATED: Self = Self::unwrapped("created", "Created");
    /// `Accepted<T>`: 202, the JSON of the `T` the client returns.
    pub(crate) const ACCEPTED: Self = Self::unwrapped("accepted", "Accepted");
    /// `Text`: 200, `text/plain` in UTF-8.
    pub(crate) const TEXT: Self = Self::new("ok_text", "TEXT", "text", "Text", false);
    /// `Bytes`: 200, of the content type they carry.
    pub(crate) const BYTES: Self = Self::new("ok_bytes", "ANY", "bytes", "Bytes", false);
    /// `Events<T>`: 200, server-sent events.
    pub(crate) const EVENTS: Self = Self::new("events", "EVENT_STREAM", "events", "Events", true);
    /// `JsonLines<T>`: 200, JSON Lines.
    pub(crate) const JSON_LINES: Self = Self::new(
        "json_lines",
        "ANY_JSON_LINES",
        "json_lines",
        "JsonLines",
        true,
    );

    const fn new(
        answer: &'static str,
        accept: &'static str,
        read: &'static str,
        document: &'static str,
        schema: bool,
    ) -> Self {
        Self {
            answer,
            accept,
            read,
            unwrapped: false,
            document,
            schema,
        }
    }

    const fn unwrapped(answer: &'static str, document: &'static str) -> Self {
        Self {
            answer,
            accept: "JSON",
            read: "json",
            unwrapped: true,
            document,
            schema: true,
        }
    }
}

/// How a body is read by the server and written by the client: the helpers the generated code calls.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct BodyKind {
    /// The method of the server's `Request` that reads it, and of the client's `Call` that writes it.
    pub(crate) method: &'static str,
    /// Whether the client's `Call` may fail to write it, and so returns a `Result`.
    pub(crate) fallible: bool,
    /// The variant of `quoll::__private::openapi::Body` that describes it.
    pub(crate) document: &'static str,
    /// Whether the document has the schema of the type: its own for JSON, the `T` of a `Form<T>`, none for text and
    /// bytes.
    pub(crate) schema: bool,
}

impl BodyKind {
    /// Any type the macro does not recognize: its JSON, `application/json`.
    pub(crate) const JSON: Self = Self::fallible("json", "Json", true);
    /// `Form<T>`: `application/x-www-form-urlencoded`.
    pub(crate) const FORM: Self = Self::fallible("form", "Form", true);
    /// `Text`: `text/plain`, UTF-8, which is always written.
    pub(crate) const TEXT: Self = Self {
        method: "text",
        fallible: false,
        document: "Text",
        schema: false,
    };
    /// `Bytes`: any content type but the three a browser sends to another site without asking.
    pub(crate) const BYTES: Self = Self::fallible("bytes", "Bytes", false);

    const fn fallible(method: &'static str, document: &'static str, schema: bool) -> Self {
        Self {
            method,
            fallible: true,
            document,
            schema,
        }
    }
}

/// The types the macro recognizes.
pub(crate) const KNOWN: &[Known] = &[
    Known {
        name: "Created",
        check: "created",
        wraps: Some("the body"),
        result: Some(ResultKind::CREATED),
        body: None,
        stream: false,
    },
    Known {
        name: "Accepted",
        check: "accepted",
        wraps: Some("the body"),
        result: Some(ResultKind::ACCEPTED),
        body: None,
        stream: false,
    },
    Known {
        name: "Form",
        check: "form",
        wraps: Some("the form"),
        result: None,
        body: Some(BodyKind::FORM),
        stream: false,
    },
    Known {
        name: "Text",
        check: "text",
        wraps: None,
        result: Some(ResultKind::TEXT),
        body: Some(BodyKind::TEXT),
        stream: false,
    },
    Known {
        name: "Bytes",
        check: "bytes",
        wraps: None,
        result: Some(ResultKind::BYTES),
        body: Some(BodyKind::BYTES),
        stream: false,
    },
    Known {
        name: "Events",
        check: "events",
        wraps: Some("each item"),
        result: Some(ResultKind::EVENTS),
        body: None,
        stream: true,
    },
    Known {
        name: "JsonLines",
        check: "json_lines",
        wraps: Some("each item"),
        result: Some(ResultKind::JSON_LINES),
        body: None,
        stream: true,
    },
];

impl Known {
    /// The type `ty` names, when the last segment of its path is one the macro recognizes.
    pub(crate) fn of(ty: &Type) -> Option<&'static Self> {
        let Type::Path(path) = peel(ty) else {
            return None;
        };
        let last = path.path.segments.last()?;

        KNOWN.iter().find(|known| last.ident == known.name)
    }
}

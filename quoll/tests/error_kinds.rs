//! The kind of every error a handler, a client or a fake builds: always given, its status one of the kinds, which
//! `status()` gives; and what a handler answers for each.

use std::collections::HashSet;
use std::fmt;

use http::StatusCode;
use quoll::{Error, ErrorKind};
use serde::Deserialize;

/// Another API's shape of errors.
#[derive(Debug, Deserialize)]
struct Shape {
    message: String,
}

impl fmt::Display for Shape {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

#[test]
fn every_error_has_a_kind_and_its_status_is_the_kind() {
    let errors: Vec<Error> = vec![
        Error::not_found("no such note"),
        Error::bad_request("bad"),
        Error::unauthorized("who"),
        Error::new(StatusCode::CONFLICT, "taken"),
        // A status that is not an error is kept, as `status()` keeps it; a handler answers 500 for it.
        Error::new(StatusCode::OK, "not an error"),
        Error::internal("the database is down"),
        Error::failed(
            ErrorKind::Status(StatusCode::BAD_GATEWAY),
            "a page of nginx",
        ),
        Error::failed(ErrorKind::Connect, "refused"),
        Error::failed(ErrorKind::Decode, "not a note"),
    ];

    let kinds: Vec<ErrorKind> = errors.iter().map(Error::kind).collect();

    for error in &errors {
        let status = match error.kind() {
            ErrorKind::Status(status) => Some(status),
            _ => None,
        };

        assert_eq!(error.status(), status, "{error:?}");
    }

    assert_eq!(
        kinds,
        [
            ErrorKind::Status(StatusCode::NOT_FOUND),
            ErrorKind::Status(StatusCode::BAD_REQUEST),
            ErrorKind::Status(StatusCode::UNAUTHORIZED),
            ErrorKind::Status(StatusCode::CONFLICT),
            ErrorKind::Status(StatusCode::OK),
            ErrorKind::Status(StatusCode::INTERNAL_SERVER_ERROR),
            ErrorKind::Status(StatusCode::BAD_GATEWAY),
            ErrorKind::Connect,
            ErrorKind::Decode,
        ]
    );
}

#[test]
fn an_answer_is_status_with_or_without_its_detail_through_every_shape() {
    let answered = Error::answered(
        StatusCode::FORBIDDEN,
        Shape {
            message: "rate limited".into(),
        },
    );
    let no_detail: Error<Shape> = Error::failed(ErrorKind::Status(StatusCode::NOT_FOUND), "");
    let shaped: Error<Shape> = Error::not_found("no such repository").into_shape();
    let carried: Error = Error::<Shape>::failed(ErrorKind::Timeout, "slow").into();

    assert_eq!(answered.kind(), ErrorKind::Status(StatusCode::FORBIDDEN));
    assert!(answered.detail().is_some());
    assert_eq!(no_detail.kind(), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert!(no_detail.detail().is_none());
    assert_eq!(no_detail.to_string(), "another api answered 404");
    assert_eq!(shaped.kind(), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert_eq!(carried.kind(), ErrorKind::Timeout);
    assert_eq!(carried.to_string(), "slow");

    let up: Error = answered.into();

    assert_eq!(up.kind(), ErrorKind::Status(StatusCode::FORBIDDEN));
    assert!(
        up.detail().is_none(),
        "a shape that is not a problem is left"
    );
}

#[test]
fn a_status_is_matched_by_its_constant_and_by_a_guard() {
    fn what(error: &Error) -> &'static str {
        match error.kind() {
            ErrorKind::Status(StatusCode::NOT_FOUND) => "missing",
            ErrorKind::Status(status) if status.is_server_error() => "in trouble",
            ErrorKind::Status(_) => "refused",
            ErrorKind::Timeout => "slow",
            _ => "failed",
        }
    }

    assert_eq!(what(&Error::not_found("x")), "missing");
    assert_eq!(what(&Error::internal("x")), "in trouble");
    assert_eq!(what(&Error::unauthorized("x")), "refused");
    assert_eq!(what(&Error::failed(ErrorKind::Timeout, "x")), "slow");
    assert_eq!(what(&Error::failed(ErrorKind::Closed, "x")), "failed");
}

#[test]
fn a_kind_is_copy_eq_and_hash_with_its_status() {
    let kind = ErrorKind::Status(StatusCode::NOT_FOUND);
    let copied = kind;

    let kinds: HashSet<ErrorKind> = [kind, copied, ErrorKind::Timeout].into();

    assert_eq!(kind, copied);
    assert_eq!(kinds.len(), 2);
}

#[test]
fn a_kind_is_written_as_error_type_records_it_and_says_whether_the_other_api_answered() {
    let cases = [
        (ErrorKind::Status(StatusCode::NOT_FOUND), "404", true),
        (
            ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE),
            "503",
            true,
        ),
        (ErrorKind::Connect, "connect", false),
        (ErrorKind::Timeout, "timeout", false),
        (ErrorKind::Closed, "closed", false),
        (ErrorKind::Body, "body", true),
        (ErrorKind::Decode, "decode", true),
        (ErrorKind::Middleware, "middleware", false),
        (ErrorKind::Invalid, "invalid", false),
    ];

    for (kind, text, answered) in cases {
        assert_eq!(kind.as_str(), text);
        assert_eq!(kind.to_string(), text);
        assert_eq!(kind.answered(), answered, "{kind}");
    }
}

#[cfg(feature = "server")]
#[tokio::test]
async fn a_handler_answers_502_for_a_failure_of_its_client_and_500_for_invalid() {
    async fn answered(kind: ErrorKind) -> StatusCode {
        let router = axum::Router::new().route(
            "/",
            axum::routing::get(move || async move {
                let error: Error = Error::failed(kind, "failed");

                axum::response::IntoResponse::into_response(error)
            }),
        );
        let url = quoll::test::spawn(router).await;

        reqwest::get(url).await.expect("an answer").status()
    }

    for kind in [
        ErrorKind::Status(StatusCode::NOT_FOUND),
        ErrorKind::Connect,
        ErrorKind::Timeout,
        ErrorKind::Closed,
        ErrorKind::Body,
        ErrorKind::Decode,
        ErrorKind::Middleware,
    ] {
        assert_eq!(answered(kind).await, StatusCode::BAD_GATEWAY, "{kind}");
    }

    assert_eq!(
        answered(ErrorKind::Invalid).await,
        StatusCode::INTERNAL_SERVER_ERROR
    );
}

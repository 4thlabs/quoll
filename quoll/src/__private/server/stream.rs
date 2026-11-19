//! The answers of a route that streams its result: server-sent events and JSON Lines, each value sent as it comes.

use std::error::Error as StdError;
use std::time::Duration;

use axum::body::Body;
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use futures_core::Stream;
use futures_util::{StreamExt, stream};
use http::{HeaderName, HeaderValue, header};
use serde::Serialize;

use crate::__private::content_type::JSON_LINES;
use crate::__private::known::{IsEvents, IsJsonLines, Items, Streamed};
use crate::rest::{Error, Events, JsonLines, Result};

/// How long an event stream goes with nothing sent before it sends a comment, so that a proxy that closes a
/// connection it sees idle keeps it: nginx waits 60 seconds by default.
const KEEP_ALIVE: Duration = Duration::from_secs(15);

/// A stream result whose items the server writes as JSON: one of quoll's, of a `T` that is `Serialize`.
///
/// The bound is on the result rather than on its items, so that a `T` that is not `Serialize` is an error on the
/// result's type, as the contract writes it, rather than on the whole contract.
pub trait Written: Streamed<Item: 'static> {
    /// The JSON of `item`.
    ///
    /// # Errors
    ///
    /// When `item` cannot be written as JSON.
    fn to_json(item: &Self::Item) -> serde_json::Result<String>;
}

impl<T: Serialize + 'static> Written for Events<T> {
    fn to_json(item: &T) -> serde_json::Result<String> {
        serde_json::to_string(item)
    }
}

impl<T: Serialize + 'static> Written for JsonLines<T> {
    fn to_json(item: &T) -> serde_json::Result<String> {
        serde_json::to_string(item)
    }
}

/// A 200 with server-sent events, `text/event-stream`: each value of the handler's stream as the JSON `data` of an
/// event, and a comment when 15 seconds pass with none. Nothing in between holds them back: axum's `Sse` sends
/// `Cache-Control: no-cache`, and `X-Accel-Buffering: no` tells a proxy as much.
///
/// # Errors
///
/// The handler's error. An item that fails once the answer has begun is logged, and cuts the answer short.
pub fn events<E: IsEvents + Written>(result: Result<E>) -> Result<Response> {
    let events = sent(result?.into_items(), |value| {
        let json = E::to_json(value).map_err(Error::internal)?;

        // Compact JSON holds no line break, so the data of the event is one line.
        Ok(Event::default().data(json))
    });
    let keep_alive = KeepAlive::new().interval(KEEP_ALIVE);

    Ok(unbuffered(
        Sse::new(events).keep_alive(keep_alive).into_response(),
    ))
}

/// A 200 with JSON Lines, `application/jsonl`: each value of the handler's stream as its JSON on one line, ended by
/// `\n`, with `Cache-Control: no-cache` as server-sent events have it, and `X-Accel-Buffering: no`.
///
/// # Errors
///
/// The handler's error. An item that fails once the answer has begun is logged, and cuts the answer short.
pub fn json_lines<L: IsJsonLines + Written>(result: Result<L>) -> Result<Response> {
    let lines = sent(result?.into_items(), |value| {
        let mut line = L::to_json(value).map_err(Error::internal)?;

        line.push('\n');
        Ok(line)
    });
    let headers = [
        (header::CONTENT_TYPE, HeaderValue::from_static(JSON_LINES)),
        (header::CACHE_CONTROL, HeaderValue::from_static("no-cache")),
    ];

    Ok(unbuffered(
        (headers, Body::from_stream(lines)).into_response(),
    ))
}

/// `response` with `X-Accel-Buffering: no`, which tells nginx, and the proxies that read it as nginx does, to send
/// each piece of the stream as it comes rather than hold it in a buffer.
fn unbuffered(mut response: Response) -> Response {
    response.headers_mut().insert(
        HeaderName::from_static("x-accel-buffering"),
        HeaderValue::from_static("no"),
    );

    response
}

/// What the server sends of `items`: each value as `write` writes it, until the stream ends or an item fails. A
/// failure is logged and sent as the body's error, once what came before it is written, which cuts the answer short;
/// the stream is dropped then.
///
/// The stream is asked for its next value only when the body is asked for its next piece, once the connection has
/// taken the last one; and it is dropped with the body, when the connection closes.
fn sent<T, W, F>(items: Items<T>, write: F) -> impl Stream<Item = Result<W>> + Send + 'static
where
    T: 'static,
    W: Send + 'static,
    F: Fn(&T) -> Result<W> + Send + 'static,
{
    stream::unfold(Some((items, write)), |state| async move {
        let (mut items, write) = state?;
        let written = match items.next().await? {
            Ok(value) => write(&value),
            Err(error) => Err(error),
        };

        let Err(error) = written else {
            return Some((written, Some((items, write))));
        };

        // hyper drops what it has not written yet when a body fails: a turn first lets it send what came before.
        tokio::task::yield_now().await;

        Some((Err(cut(error)), None))
    })
}

/// Logs `error`, an item of a stream that failed once its 200 was sent, with its source: the answer is cut short, so
/// that the client reads an error rather than an end it would take for the last value.
fn cut(error: Error) -> Error {
    tracing::error!(
        error = &error as &dyn StdError,
        "a stream was cut short after its 200: one of its items failed"
    );

    error
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::__private::server::logged_by;

    #[test]
    fn a_stream_cut_short_is_logged_once_with_its_source() {
        let (log, _) = logged_by(|| cut(Error::internal("the store went away")));

        assert_eq!(log.lines().count(), 1, "{log}");
        assert!(log.contains("cut short after its 200"), "{log}");
        assert!(log.contains("the store went away"), "{log}");
        assert!(!log.contains("answered"), "{log}");
    }
}

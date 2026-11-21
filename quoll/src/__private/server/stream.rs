//! The answers of a route that streams its result: server-sent events and JSON Lines, each value sent as it comes, and
//! bytes, each chunk sent as it comes.

use std::error::Error as StdError;
use std::future;
use std::time::Duration;

use axum::body::Body;
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use futures_core::Stream;
use futures_util::{StreamExt, stream};
use http::{HeaderName, HeaderValue, header};
use serde::Serialize;
use tokio::sync::watch;

use super::problem_json;
use crate::__private::content_type::{JSON_LINES, media_type_value};
use crate::__private::known::{IsByteStream, IsEvents, IsJsonLines, Items, Streamed};
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

/// A 200 with server-sent events, `text/event-stream`: an empty comment first, then each value of the handler's stream
/// as the JSON `data` of an event, and a comment when 15 seconds pass with none. Nothing in between holds them back:
/// axum's `Sse` sends `Cache-Control: no-cache`, and `X-Accel-Buffering: no` tells a proxy as much.
///
/// The comment goes out with the head, before the handler's stream is asked for a value: a proxy that holds the head of
/// an answer until its first bytes, as Vite's does, sends it at once, and the client sees the stream open.
///
/// An item that fails once the 200 is sent is logged, and ends the events with a last one, `event: problem`, whose
/// `data` is the RFC 9457 problem the error would answer: the client reads it as that error. Every event before it
/// is sent, and the body then ends as any other.
///
/// # Errors
///
/// The handler's error.
pub fn events<E: IsEvents + Written>(result: Result<E>) -> Result<Response> {
    let write = |value: E::Item| {
        let json = E::to_json(&value).map_err(Error::internal)?;

        // Compact JSON holds no line break, so the data of the event is one line.
        Ok(Event::default().data(json))
    };
    let events = sent(result?.into_items(), write, |error| {
        logged(&error, "ended with an `event: problem`");

        let (_, problem) = problem_json(&error);
        let problem = String::from_utf8(problem).expect("JSON is UTF-8");

        Failed::Last(Event::default().event("problem").data(problem))
    });
    let opening = stream::once(future::ready(Ok(Event::default().comment(""))));
    let keep_alive = KeepAlive::new().interval(KEEP_ALIVE);

    let mut response = unbuffered(
        Sse::new(opening.chain(events))
            .keep_alive(keep_alive)
            .into_response(),
    );

    response.extensions_mut().insert(EventsBody);

    Ok(response)
}

/// A 200 with JSON Lines, `application/jsonl`: each value of the handler's stream as its JSON on one line, ended by
/// `\n`, with `Cache-Control: no-cache` as server-sent events have it, and `X-Accel-Buffering: no`.
///
/// An item that fails once the 200 is sent is logged, and cuts the answer short: JSON Lines have no way to say an
/// error, so the body fails, and the client reads an error rather than an end. The lines sent just before it may not
/// reach the client.
///
/// # Errors
///
/// The handler's error.
pub fn json_lines<L: IsJsonLines + Written>(result: Result<L>) -> Result<Response> {
    let write = |value: L::Item| {
        let mut line = L::to_json(&value).map_err(Error::internal)?;

        line.push('\n');
        Ok(line)
    };
    let lines = sent(result?.into_items(), write, |error| {
        logged(&error, "cut short");

        Failed::Cut(error)
    });
    let headers = [
        (header::CONTENT_TYPE, HeaderValue::from_static(JSON_LINES)),
        (header::CACHE_CONTROL, HeaderValue::from_static("no-cache")),
    ];

    Ok(unbuffered(
        (headers, Body::from_stream(lines)).into_response(),
    ))
}

/// A 200 with the chunks of the handler's stream, each sent as it comes, of their content type, with
/// `X-Content-Type-Options: nosniff` as a `Bytes` has it, `Cache-Control: no-cache` and `X-Accel-Buffering: no`.
///
/// A chunk that fails once the 200 is sent is logged, and cuts the answer short, as JSON Lines are: the client reads
/// an error rather than an end.
///
/// # Errors
///
/// The handler's error, or a 500 when the content type is not a media type.
pub fn byte_stream<B: IsByteStream>(result: Result<B>) -> Result<Response> {
    let (chunks, content_type) = result?.into_parts();
    let content_type = media_type_value(content_type).map_err(|content_type| {
        Error::internal(format!(
            "the content type of a `ByteStream`, {content_type:?}, is not a media type"
        ))
    })?;
    let chunks = sent(chunks, Ok, |error| {
        logged(&error, "cut short");

        Failed::Cut(error)
    });
    let headers = [
        (header::CONTENT_TYPE, content_type),
        (
            header::X_CONTENT_TYPE_OPTIONS,
            HeaderValue::from_static("nosniff"),
        ),
        (header::CACHE_CONTROL, HeaderValue::from_static("no-cache")),
    ];

    Ok(unbuffered(
        (headers, Body::from_stream(chunks)).into_response(),
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

/// The mark of an answer whose body is an `Events` result's, which only quoll sets.
#[derive(Debug, Clone, Copy)]
struct EventsBody;

/// `response` as the server sends it once `stopping` holds `true`, or once its sender is dropped: when it is the answer
/// of an `Events` result, its body ends after the event it is sending, so that the client, and any proxy between,
/// reads a clean end rather than a connection cut by the end of the grace. Any other answer is left as it is, a
/// `JsonLines` and a `ByteStream` among them: a clean end would make a finite one cut by the stop look complete.
///
/// The events end with nothing more, as a stream that has no more values: a stop is no failure, and is no
/// `event: problem`. An `Events` stream that opens once the server is told to stop ends at once.
pub(crate) fn ended_on_stop(mut response: Response, stopping: &watch::Receiver<bool>) -> Response {
    if response.extensions_mut().remove::<EventsBody>().is_none() {
        return response;
    }

    let mut stopping = stopping.clone();
    let stopped = async move {
        // A sender dropped is a stop too: the server has returned.
        let _ = stopping.wait_for(|stopping| *stopping).await;
    };

    // Each event of axum's `Sse` is one frame of the body, so the body ends between two of them.
    response.map(|body| Body::from_stream(body.into_data_stream().take_until(stopped)))
}

/// What the server sends once an item has failed.
enum Failed<W> {
    /// A last piece, after which the body ends as any other.
    Last(W),
    /// The body's error, which cuts the answer short.
    Cut(Error),
}

/// What the server sends of `items`: each value as `write` writes it, until the stream ends or an item fails, and
/// then what `failed` makes of its error. The stream is dropped then.
///
/// The stream is asked for its next value only when the body is asked for its next piece, once the connection has
/// taken the last one; and it is dropped with the body, when the connection closes.
fn sent<T, W, F, E>(
    items: Items<T>,
    write: F,
    failed: E,
) -> impl Stream<Item = Result<W>> + Send + 'static
where
    T: 'static,
    W: Send + 'static,
    F: Fn(T) -> Result<W> + Send + 'static,
    E: Fn(Error) -> Failed<W> + Send + 'static,
{
    stream::unfold(Some((items, write, failed)), |state| async move {
        let (mut items, write, failed) = state?;
        let written = match items.next().await? {
            Ok(value) => write(value),
            Err(error) => Err(error),
        };

        let error = match written {
            Ok(written) => return Some((Ok(written), Some((items, write, failed)))),
            Err(error) => error,
        };

        drop(items);

        match failed(error) {
            Failed::Last(last) => Some((Ok(last), None)),
            Failed::Cut(error) => {
                // hyper drops what it has not written yet when a body fails: a turn first may let it send what came
                // before. Best effort only, which no test relies on: a full send buffer keeps it.
                tokio::task::yield_now().await;

                Some((Err(error), None))
            }
        }
    })
}

/// Logs `error`, an item of a stream that failed once its 200 was sent, with its source, and how the stream `ended`.
fn logged(error: &Error, ended: &str) {
    tracing::error!(
        error = error as &dyn StdError,
        "a stream {ended} after its 200: one of its items failed"
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::__private::server::logged_by;

    #[test]
    fn a_stream_cut_short_is_logged_once_with_its_source() {
        let (log, ()) = logged_by(|| logged(&Error::internal("the store went away"), "cut short"));

        assert_eq!(log.lines().count(), 1, "{log}");
        assert!(log.contains("cut short after its 200"), "{log}");
        assert!(log.contains("the store went away"), "{log}");
        assert!(!log.contains("answered"), "{log}");
    }
}

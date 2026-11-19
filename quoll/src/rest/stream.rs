//! The results a route streams, a value at a time: server-sent events and JSON Lines.
//!
//! Each is one type on both sides. The server builds it from a stream of values, which it sends as they come; the
//! client gets the same type, a stream of `Result<T>`, which reads them as they arrive.

use std::fmt;
use std::pin::Pin;
use std::task::{Context, Poll};

use futures_core::Stream;

use crate::rest::Result;

/// The items of a stream, as a handler gave them or as a client reads them.
enum Items<T> {
    /// Values, each an item.
    Values(Pin<Box<dyn Stream<Item = T> + Send>>),
    /// Values and errors: an error ends what the server sends.
    Results(Pin<Box<dyn Stream<Item = Result<T>> + Send>>),
}

impl<T> Items<T> {
    fn values<S>(values: S) -> Self
    where
        S: Stream<Item = T> + Send + 'static,
    {
        Self::Values(Box::pin(values))
    }

    fn results<S>(results: S) -> Self
    where
        S: Stream<Item = Result<T>> + Send + 'static,
    {
        Self::Results(Box::pin(results))
    }

    fn poll_next(&mut self, cx: &mut Context<'_>) -> Poll<Option<Result<T>>> {
        match self {
            Self::Values(values) => values.as_mut().poll_next(cx).map(|value| value.map(Ok)),
            Self::Results(results) => results.as_mut().poll_next(cx),
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        match self {
            Self::Values(values) => values.size_hint(),
            Self::Results(results) => results.size_hint(),
        }
    }
}

/// Server-sent events, `text/event-stream`, the JSON of a `T` in each: what a route returns to send values as they
/// come, as the changes of a note as they happen.
///
/// The server builds it from a stream of values, [`Events::new`], or of results, [`Events::try_new`]. The client
/// gets the same type: a [`Stream`] of `Result<T>`, one item per event.
///
/// ```
/// use futures_util::{StreamExt, stream};
/// use quoll::rest::Events;
///
/// # #[tokio::main(flavor = "current_thread")]
/// # async fn main() -> quoll::rest::Result<()> {
/// let mut events = Events::new(stream::iter(["milk", "eggs"]));
///
/// while let Some(event) = events.next().await {
///     println!("{}", event?);
/// }
/// # Ok(())
/// # }
/// ```
///
/// The server answers 200 and sends each value as it comes, as an event whose `data` is its JSON, with no `id` and
/// no name. It sends a comment when 15 seconds pass with no value, so that a proxy does not close a connection it sees
/// idle. It asks the stream for a value only once the connection has taken the last one: a client that reads slowly
/// slows it down, and one that goes away drops it, with the connection.
///
/// The client asks for `text/event-stream` with `Accept`, and an answer of another content type is an error with no
/// status, as an answer that is not a route's result is. Each event is an item, its `data` read as the JSON of a `T`,
/// as an `EventSource` reads a message: an event with a name other than `message`, a comment, and the fields `id`,
/// `retry` and any other are skipped, and lines may end with `\r\n`, `\n` or `\r`. An event whose `data` does not read
/// as a `T` is an error with no status, and the stream goes on; a body that cannot be read is an error with no status,
/// and the stream ends after it. An event has no limit of size, as a JSON result has none.
///
/// Dropping the stream closes its connection. The client does not reconnect when the stream ends, as an
/// `EventSource` does: the caller calls the route again. A route that resumes where a stream ended reads the id it
/// resumes from in an argument of its own, `header("Last-Event-ID") last: Option<String>`.
pub struct Events<T> {
    items: Items<T>,
}

impl<T> Events<T> {
    /// Events sending `values`, the JSON of one in each, until the stream ends.
    pub fn new<S>(values: S) -> Self
    where
        S: Stream<Item = T> + Send + 'static,
    {
        Self {
            items: Items::values(values),
        }
    }

    /// Events sending the values of `results`, until the stream ends or gives an error.
    ///
    /// The status of the answer is sent by then: the server logs the error and cuts the answer short, so that the
    /// client reads an error before the end of its stream, rather than an end it would take for the last value.
    ///
    /// ```
    /// use futures_util::stream;
    /// use quoll::rest::{Error, Events};
    ///
    /// let events = Events::try_new(stream::iter([Ok("milk"), Err(Error::internal("the store went away"))]));
    /// # let _ = events;
    /// ```
    pub fn try_new<S>(results: S) -> Self
    where
        S: Stream<Item = Result<T>> + Send + 'static,
    {
        Self {
            items: Items::results(results),
        }
    }
}

/// A value of each event, or the error of one.
impl<T> Stream for Events<T> {
    type Item = Result<T>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<T>>> {
        self.items.poll_next(cx)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.items.size_hint()
    }
}

/// Shows the type alone: what the stream gives is known only as it is read.
impl<T> fmt::Debug for Events<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Events").finish_non_exhaustive()
    }
}

/// JSON Lines, `application/jsonl`, the JSON of a `T` on each line: what a route returns to send many values one at a
/// time, as an export too large to be held whole.
///
/// The server builds it from a stream of values, [`JsonLines::new`], or of results, [`JsonLines::try_new`]. The
/// client gets the same type: a [`Stream`] of `Result<T>`, one item per line.
///
/// ```
/// use futures_util::{StreamExt, stream};
/// use quoll::rest::JsonLines;
///
/// # #[tokio::main(flavor = "current_thread")]
/// # async fn main() -> quoll::rest::Result<()> {
/// let mut lines = JsonLines::new(stream::iter(1..=3));
///
/// while let Some(line) = lines.next().await {
///     println!("{}", line?);
/// }
/// # Ok(())
/// # }
/// ```
///
/// The server answers 200 and sends each value as it comes, its JSON on one line ended by `\n`. It asks the stream for
/// a value only once the connection has taken the last one: a client that reads slowly slows it down, and one that
/// goes away drops it, with the connection. It sends nothing while no value comes.
///
/// The client asks for `application/jsonl` with `Accept`, and an answer of another content type is an error with no
/// status, as an answer that is not a route's result is. Each line is an item, read as the JSON of a `T`: a line may
/// end with `\r\n`, an empty line is skipped, and a last line with no line break is read too. A line that does not
/// read as a `T` is an error with no status, and the stream goes on; a body that cannot be read is an error with no
/// status, and the stream ends after it. A line has no limit of length, as a JSON result has none.
///
/// Dropping the stream closes its connection. The client does not call the route again when the stream ends.
pub struct JsonLines<T> {
    items: Items<T>,
}

impl<T> JsonLines<T> {
    /// Lines sending `values`, the JSON of one on each, until the stream ends.
    pub fn new<S>(values: S) -> Self
    where
        S: Stream<Item = T> + Send + 'static,
    {
        Self {
            items: Items::values(values),
        }
    }

    /// Lines sending the values of `results`, until the stream ends or gives an error.
    ///
    /// The status of the answer is sent by then: the server logs the error and cuts the answer short, so that the
    /// client reads an error before the end of its stream, rather than an end it would take for the last line.
    ///
    /// ```
    /// use futures_util::stream;
    /// use quoll::rest::{Error, JsonLines};
    ///
    /// let lines = JsonLines::try_new(stream::iter([Ok(1), Err(Error::internal("the store went away"))]));
    /// # let _ = lines;
    /// ```
    pub fn try_new<S>(results: S) -> Self
    where
        S: Stream<Item = Result<T>> + Send + 'static,
    {
        Self {
            items: Items::results(results),
        }
    }
}

/// A value of each line, or the error of one.
impl<T> Stream for JsonLines<T> {
    type Item = Result<T>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<T>>> {
        self.items.poll_next(cx)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.items.size_hint()
    }
}

/// Shows the type alone: what the stream gives is known only as it is read.
impl<T> fmt::Debug for JsonLines<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("JsonLines").finish_non_exhaustive()
    }
}

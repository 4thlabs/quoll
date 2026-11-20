//! The results a route streams, a value at a time: server-sent events and JSON Lines, and bytes as they come.
//!
//! Each is one type on both sides, thin over the same [`Items`]. The server builds it from a stream of values, or of
//! chunks of bytes, which it sends as they come; the client gets the same type, a stream of `Result<T>`, which reads
//! them as they arrive.

use std::borrow::Cow;
use std::fmt;
use std::pin::Pin;
use std::task::{Context, Poll};

use futures_core::Stream;
use pin_project_lite::pin_project;
use sync_wrapper::SyncWrapper;

use crate::__private::content_type::{self, OCTET_STREAM};
use crate::rest::Result;

/// The items of a stream result, as a handler gave them or as a client reads them: one box, polled through `&mut`
/// alone, so that it is `Sync` whatever the stream it holds. It gives no size hint, which `&self` asks for: the box
/// is reached through `&mut` only.
pub struct Items<T>(SyncWrapper<Pin<Box<dyn Stream<Item = Result<T>> + Send>>>);

impl<T> Items<T> {
    /// The items of `results`.
    pub(crate) fn new<S>(results: S) -> Self
    where
        S: Stream<Item = Result<T>> + Send + 'static,
    {
        Self(SyncWrapper::new(Box::pin(results)))
    }

    /// The values of `values`, each an item that is no error.
    fn values<S>(values: S) -> Self
    where
        S: Stream<Item = T> + Send + 'static,
    {
        Self::new(Oks { values })
    }
}

impl<T> Stream for Items<T> {
    type Item = Result<T>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<T>>> {
        self.0.get_mut().as_mut().poll_next(cx)
    }
}

pin_project! {
    /// The values of a stream, each as an `Ok`.
    struct Oks<S> {
        #[pin]
        values: S,
    }
}

impl<S: Stream> Stream for Oks<S> {
    type Item = Result<S::Item>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.project()
            .values
            .poll_next(cx)
            .map(|value| value.map(Ok))
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
/// The server answers 200, with `Cache-Control: no-cache` and `X-Accel-Buffering: no`, so that neither a cache nor
/// a proxy such as nginx holds the events back. It sends each value as it comes, as an event whose `data` is its JSON,
/// with no `id` and no name, and a comment when 15 seconds pass with no value, so that a proxy does not close a
/// connection it sees idle. It asks the stream for a value only once the connection has taken the last one: a client
/// that reads slowly slows it down, and one that goes away drops it, with the connection.
///
/// When an item fails once the 200 is sent, the server logs it and ends the stream with a last event, named
/// `problem`, whose `data` is the RFC 9457 problem the error would have answered: every value before it is sent.
///
/// The client asks for `text/event-stream` with `Accept`, and an answer of another content type is an error with no
/// status, as an answer that is not a route's result is. Each event that has data is an item, its `data` read as the
/// JSON of a `T`, whatever its name, which the stream does not give: an event with no data, a comment, and the fields
/// `id`, `retry` and any other are skipped, and lines may end with `\r\n`, `\n` or `\r`. An event whose `data` does not
/// read as a `T` is an error with no status, and the stream goes on.
///
/// The one name with a meaning is `problem`, from quoll's server or another: the client reads it as the error of an
/// answer, with the problem's status, 500 when it names none, and the problem, and the stream ends after it. An event,
/// or a line, longer than 1 MiB, and a body that cannot be read, are an error with no status, and the stream ends
/// after it too.
///
/// Dropping the stream closes its connection. The client does not reconnect when the stream ends, as an
/// `EventSource` does: the caller calls the route again. Events carry no `id` and the client keeps none, so a stream
/// does not resume where the last one ended: that is not supported yet.
pub struct Events<T> {
    pub(crate) items: Items<T>,
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
    /// The status of the answer is sent by then: the server logs the error and sends, after every value before it, a
    /// last event, `event: problem`, with the problem the error would have answered: an internal error's has no
    /// detail. The client reads that event as the error, then the end of its stream.
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
            items: Items::new(results),
        }
    }
}

/// A value of each event, or the error of one.
impl<T> Stream for Events<T> {
    type Item = Result<T>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<T>>> {
        Pin::new(&mut self.items).poll_next(cx)
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
/// The server answers 200, with `Cache-Control: no-cache` and `X-Accel-Buffering: no`, so that neither a cache nor
/// a proxy such as nginx holds the lines back. It sends each value as it comes, its JSON on one line ended by `\n`,
/// and nothing while no value comes. It asks the stream for a value only once the connection has taken the last one: a
/// client that reads slowly slows it down, and one that goes away drops it, with the connection.
///
/// The client asks for `application/jsonl` with `Accept`, and reads `application/x-ndjson` and
/// `application/x-jsonlines` too, which some APIs send, and `application/json`, which others send their lines as, the
/// Docker Engine API among them: an answer of another content type is an error with no status, as an answer that is
/// not a route's result is. Each line is an item, read as the JSON of a `T`: a line may end with
/// `\r\n`, an empty line is skipped, and a last line with no line break is read too. A line that does not read as a
/// `T` is an error with no status, and the stream goes on. A line longer than 1 MiB, and a body that cannot be read,
/// are an error with no status, and the stream ends after it.
///
/// An answer of `application/json` is read so too, line by line: a document written over several lines, as a
/// pretty-printed array, is read as broken lines, each an item that does not read as a `T`.
///
/// Dropping the stream closes its connection. The client does not call the route again when the stream ends.
pub struct JsonLines<T> {
    pub(crate) items: Items<T>,
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
    /// The status of the answer is sent by then, and JSON Lines have no way to say an error: the server logs it and
    /// cuts the answer short, so that the client reads an error, never an end it would take for the last line. The
    /// lines sent just before the error may not reach the client, and when none has, the call itself is the error.
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
            items: Items::new(results),
        }
    }
}

/// A value of each line, or the error of one.
impl<T> Stream for JsonLines<T> {
    type Item = Result<T>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<T>>> {
        Pin::new(&mut self.items).poll_next(cx)
    }
}

/// Shows the type alone: what the stream gives is known only as it is read.
impl<T> fmt::Debug for JsonLines<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("JsonLines").finish_non_exhaustive()
    }
}

/// Bytes sent as they come, of one content type: what a route returns to stream what is not JSON, as the output of a
/// process as it runs, a live feed, or a file too large to be held whole.
///
/// The server builds it from a stream of chunks, [`ByteStream::new`], or of results, [`ByteStream::try_new`], of
/// `application/octet-stream` unless [`ByteStream::with_content_type`] says another. The client gets the same type: a
/// [`Stream`] of `Result<Vec<u8>>`, the chunks of the body as they arrive, and the [content type](Self::content_type)
/// of the answer.
///
/// ```
/// use futures_util::{StreamExt, stream};
/// use quoll::rest::ByteStream;
///
/// # #[tokio::main(flavor = "current_thread")]
/// # async fn main() -> quoll::rest::Result<()> {
/// let chunks = vec![b"hello ".to_vec(), b"world\n".to_vec()];
/// let mut output = ByteStream::new(stream::iter(chunks)).with_content_type("text/plain; charset=utf-8");
///
/// assert_eq!(output.content_type(), "text/plain; charset=utf-8");
///
/// while let Some(chunk) = output.next().await {
///     print!("{}", String::from_utf8_lossy(&chunk?));
/// }
/// # Ok(())
/// # }
/// ```
///
/// The server answers 200 with its content type, `X-Content-Type-Options: nosniff` as for [`Bytes`](crate::rest::Bytes),
/// `Cache-Control: no-cache` and `X-Accel-Buffering: no`, so that neither a browser, a cache nor a proxy such as nginx
/// reads it otherwise or holds it back. It sends each chunk as it comes, as it is. It asks the stream for a chunk only
/// once the connection has taken the last one: a client that reads slowly slows it down, and one that goes away drops
/// it, with the connection.
///
/// The client asks for any content type, `Accept: */*`, and reads an answer of any, as it reads [`Bytes`](crate::rest::Bytes):
/// its content type, or `application/octet-stream` when it has none, or one that is not a media type. Each chunk is
/// one the connection gave, of whatever size, never empty: what the bytes mean, their lines or their frames, is the
/// caller's to read. A body that cannot be read is an error with no status, and the stream ends after it.
///
/// Dropping the stream closes its connection. The client does not call the route again when the stream ends.
pub struct ByteStream {
    pub(crate) chunks: Items<Vec<u8>>,
    pub(crate) content_type: Cow<'static, str>,
}

impl ByteStream {
    /// Bytes sending `chunks`, each as it comes, until the stream ends: `application/octet-stream`, bytes of no known
    /// type.
    pub fn new<S>(chunks: S) -> Self
    where
        S: Stream<Item = Vec<u8>> + Send + 'static,
    {
        Self {
            chunks: Items::values(chunks),
            content_type: Cow::Borrowed(OCTET_STREAM),
        }
    }

    /// Bytes sending the chunks of `results`, until the stream ends or gives an error: `application/octet-stream`.
    ///
    /// The status of the answer is sent by then, and bytes have no way to say an error: the server logs it and cuts the
    /// answer short, so that the client reads an error, never an end it would take for the last chunk. The chunks sent
    /// just before the error may not reach the client, and when none has, the call itself is the error.
    ///
    /// ```
    /// use futures_util::stream;
    /// use quoll::rest::{ByteStream, Error};
    ///
    /// let output = ByteStream::try_new(stream::iter([Ok(b"a".to_vec()), Err(Error::internal("the process died"))]));
    /// # let _ = output;
    /// ```
    pub fn try_new<S>(results: S) -> Self
    where
        S: Stream<Item = Result<Vec<u8>>> + Send + 'static,
    {
        Self {
            chunks: Items::new(results),
            content_type: Cow::Borrowed(OCTET_STREAM),
        }
    }

    /// The same chunks, of `content_type`, a media type as `Content-Type` writes it: `text/plain; charset=utf-8`, or
    /// `application/vnd.docker.multiplexed-stream`.
    ///
    /// It is checked when it is sent: a server answers a content type that is not a media type with a 500, logged, before
    /// it sends any chunk. As for [`Bytes`](crate::rest::Bytes), a server never answers a content type a client gave as
    /// it is: a browser runs `text/html` on the API's own site.
    #[must_use]
    pub fn with_content_type(self, content_type: impl Into<String>) -> Self {
        Self {
            chunks: self.chunks,
            content_type: Cow::Owned(content_type.into()),
        }
    }

    /// What the bytes are, as `Content-Type` writes it, as it was given or received: `application/octet-stream` when
    /// nothing said it.
    pub fn content_type(&self) -> &str {
        &self.content_type
    }

    /// Whether the bytes are of `media_type`, a type and a subtype, whatever the case and the parameters of their
    /// content type.
    ///
    /// ```
    /// use futures_util::stream;
    /// use quoll::rest::ByteStream;
    ///
    /// let logs = ByteStream::new(stream::empty()).with_content_type("Application/Vnd.Docker.Raw-Stream");
    ///
    /// assert!(logs.has_type("application/vnd.docker.raw-stream"));
    /// assert!(!logs.has_type("application/vnd.docker.multiplexed-stream"));
    /// ```
    pub fn has_type(&self, media_type: &str) -> bool {
        content_type::essence(&self.content_type).eq_ignore_ascii_case(media_type.trim())
    }
}

/// Each chunk of the body, or the error of the body.
impl Stream for ByteStream {
    type Item = Result<Vec<u8>>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<Vec<u8>>>> {
        Pin::new(&mut self.chunks).poll_next(cx)
    }
}

/// Shows the content type: the chunks are known only as they are read.
impl fmt::Debug for ByteStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ByteStream")
            .field("content_type", &self.content_type)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;

    use super::*;

    fn is_send_and_sync<T: Send + Sync>(_: &T) {}

    /// A stream that is `Send` but not `Sync`, as one holding a `Cell` is.
    struct NotSync(Cell<u8>);

    impl Stream for NotSync {
        type Item = u8;

        fn poll_next(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Option<u8>> {
            Poll::Ready(Some(self.0.get()))
        }
    }

    #[test]
    fn a_stream_result_is_send_and_sync_whatever_its_stream() {
        is_send_and_sync(&Events::new(NotSync(Cell::new(1))));
        is_send_and_sync(&JsonLines::new(NotSync(Cell::new(1))));
        is_send_and_sync(&Events::<u8>::try_new(futures_util::stream::empty()));
        is_send_and_sync(&ByteStream::new(futures_util::stream::empty()));
    }
}

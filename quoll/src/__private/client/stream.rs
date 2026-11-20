//! The results a client reads as a stream: the body of an answer split into its items as its chunks come, server-sent
//! events or JSON Lines, each read as the JSON of an item; or the chunks themselves, as they come.

use std::mem;

use futures_util::{StreamExt, stream};
use serde::de::DeserializeOwned;

use http::StatusCode;

use super::{no_answer, read_problem};
use crate::__private::known::Items;
use crate::rest::{Error, Result, Route};

/// The most one item holds, 1 MiB: the `data` of an event, or a line of either body. A stream runs for long, and a
/// server that never ends an item would have the client hold all it sends: past it, the stream ends with an error.
const ITEM_LIMIT: usize = 1024 * 1024;

/// An item longer than [`ITEM_LIMIT`]: what is left of the body is not read.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TooLong;

/// What a piece of a stream's body is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Piece {
    /// An item, the JSON of a value.
    Item,
    /// `event: problem`: the RFC 9457 problem the server ends the stream with, in JSON.
    Problem,
}

/// How the body of a stream is split into the JSON of its items.
pub(crate) trait Framing: Send + 'static {
    /// Adds the next chunk of the body.
    fn push(&mut self, chunk: &[u8]);

    /// What `read` makes of the next whole piece, and of its JSON, lent where it lies, when the chunks so far hold
    /// one; an error once a piece is longer than [`ITEM_LIMIT`], whole or not.
    fn next_item<R>(&mut self, read: impl FnOnce(Piece, &[u8]) -> R) -> Option<Result<R, TooLong>>;

    /// Says the body has ended: what is left makes an item, or is dropped.
    fn finish(&mut self);
}

/// The items of `response`'s body, split by `framing` and read as the JSON of a `T`, as its chunks come: a chunk is
/// read only when the stream is asked for an item that the chunks so far do not hold.
///
/// An item that does not read as a `T` is an error, and the stream goes on. A problem, an item longer than
/// [`ITEM_LIMIT`], and a body that cannot be read, are an error, and the stream ends after it, dropping the answer and
/// what it had of an item. Dropping the stream drops the answer too, which closes its connection.
pub(crate) fn items<T, F>(
    response: reqwest::Response,
    route: &'static Route,
    framing: F,
) -> Items<T>
where
    T: DeserializeOwned + Send + 'static,
    F: Framing,
{
    let state = Some((Some(response), framing));

    let items = stream::unfold(state, move |state| async move {
        let (mut response, mut framing) = state?;

        loop {
            let read = |piece, json: &[u8]| match piece {
                Piece::Item => Ok(serde_json::from_slice::<T>(json)
                    .map_err(|error| no_answer(route, NOT_AN_ITEM, error))),
                Piece::Problem => Err(problem(route, json)),
            };

            match framing.next_item(read) {
                Some(Ok(Ok(item))) => return Some((item, Some((response, framing)))),
                Some(Ok(Err(problem))) => return Some((Err(problem), None)),
                Some(Err(TooLong)) => {
                    let error = no_answer(
                        route,
                        "got an item longer than the client reads",
                        "an event or a line holds more than 1 MiB",
                    );

                    return Some((Err(error), None));
                }
                None => {}
            }

            match response.as_mut()?.chunk().await {
                Ok(Some(chunk)) => framing.push(&chunk),
                Ok(None) => {
                    response = None;
                    framing.finish();
                }
                Err(error) => {
                    let error = no_answer(route, "got an answer whose body cannot be read", error);

                    return Some((Err(error), None));
                }
            }
        }
    });

    // Asked again once it has ended, the stream says so again rather than panic.
    Items::new(items.fuse())
}

/// The chunks of `response`'s body, each as the connection gave it, read only when the stream is asked for one.
///
/// A body that cannot be read is an error, and the stream ends after it, dropping the answer. Dropping the stream
/// drops the answer too, which closes its connection.
pub(crate) fn chunks(response: reqwest::Response, route: &'static Route) -> Items<Vec<u8>> {
    let chunks = stream::unfold(Some(response), move |response| async move {
        let mut response = response?;

        loop {
            match response.chunk().await {
                // hyper may hand over an empty chunk, which says nothing to the caller.
                Ok(Some(chunk)) if chunk.is_empty() => {}
                Ok(Some(chunk)) => return Some((Ok(Vec::from(chunk)), Some(response))),
                Ok(None) => return None,
                Err(error) => {
                    let error = no_answer(route, "got an answer whose body cannot be read", error);

                    return Some((Err(error), None));
                }
            }
        }
    });

    // Asked again once it has ended, the stream says so again rather than panic.
    Items::new(chunks.fuse())
}

/// What the error of an item that does not read as the route's says.
const NOT_AN_ITEM: &str = "got an item that does not read as its type";

/// The error an `event: problem` stands for, whose data is `json`: the problem's status, 500 when it names none that
/// is an error, and the problem, as an error answer gives them.
fn problem(route: &Route, json: &[u8]) -> Error {
    let Some(problem) = read_problem(json) else {
        return no_answer(
            route,
            "got an `event: problem` that is not a problem",
            "its data is not a JSON object",
        );
    };
    let status = problem
        .status()
        .filter(|status| status.is_client_error() || status.is_server_error())
        .unwrap_or(StatusCode::INTERNAL_SERVER_ERROR);

    Error::upstream(status, Some(problem), json.to_vec())
}

/// A body split into lines, as its chunks come.
#[derive(Debug, Default)]
struct Lines {
    /// What the body holds from the start of the line being read.
    buffer: Vec<u8>,
    /// Where the line being read starts in the buffer.
    start: usize,
    /// How far the buffer has been searched for the end of that line.
    searched: usize,
    /// Whether a `\r` alone ends a line, as in server-sent events.
    cr_breaks: bool,
    /// Whether the last line ended with a `\r`: a `\n` right after it ends nothing, as `\r\n` is one line break.
    after_cr: bool,
}

impl Lines {
    /// Lines ended by `\n` alone, or by `\r` too when `cr_breaks` is set; `\r\n` is one line break.
    fn new(cr_breaks: bool) -> Self {
        Self {
            cr_breaks,
            ..Self::default()
        }
    }

    /// Adds `chunk`, dropping the lines read already: each byte is moved once per chunk at most.
    fn push(&mut self, chunk: &[u8]) {
        self.buffer.drain(..self.start);
        self.searched -= self.start;
        self.start = 0;
        self.buffer.extend_from_slice(chunk);
    }

    /// The next whole line, without its line break; an error once a line is longer than [`ITEM_LIMIT`], whole or
    /// not. The buffer is searched from where the last search stopped.
    fn next_line(&mut self) -> Option<Result<&[u8], TooLong>> {
        if self.after_cr {
            match self.buffer.get(self.searched) {
                None => return None,
                Some(b'\n') => {
                    self.searched += 1;
                    self.start = self.searched;
                }
                Some(_) => {}
            }

            self.after_cr = false;
        }

        let cr_breaks = self.cr_breaks;
        let found = self.buffer[self.searched..]
            .iter()
            .position(|&byte| byte == b'\n' || (cr_breaks && byte == b'\r'));
        let Some(offset) = found else {
            self.searched = self.buffer.len();

            // A line not ended yet, which the next chunks may end.
            return (self.buffer.len() - self.start > ITEM_LIMIT).then_some(Err(TooLong));
        };

        let at = self.searched + offset;
        let line = self.start..at;

        self.after_cr = self.buffer[at] == b'\r';
        self.searched = at + 1;
        self.start = self.searched;

        if line.len() > ITEM_LIMIT {
            return Some(Err(TooLong));
        }

        Some(Ok(&self.buffer[line]))
    }

    /// What is left of the body once it has ended, a last line with no line break, which is read then: what is left
    /// after it is nothing.
    fn rest(&mut self) -> &[u8] {
        let rest = self.start..self.buffer.len();

        self.start = self.buffer.len();
        self.searched = self.buffer.len();
        &self.buffer[rest]
    }
}

/// Server-sent events, read as an `EventSource` reads them: each message is an item, its `data`.
#[derive(Debug)]
pub(crate) struct EventStream {
    lines: Lines,
    /// Whether no line has been read yet: the first may start with a byte order mark, which is dropped.
    first: bool,
    /// The `data` of the event being read, each of its lines followed by `\n`.
    data: Vec<u8>,
    /// Whether the event being read is named `problem`, which ends the stream; any other name is an item's.
    problem: bool,
}

impl Default for EventStream {
    fn default() -> Self {
        Self {
            lines: Lines::new(true),
            first: true,
            data: Vec::new(),
            problem: false,
        }
    }
}

impl Framing for EventStream {
    fn push(&mut self, chunk: &[u8]) {
        self.lines.push(chunk);
    }

    fn next_item<R>(&mut self, read: impl FnOnce(Piece, &[u8]) -> R) -> Option<Result<R, TooLong>> {
        while let Some(line) = self.lines.next_line() {
            let Ok(mut line) = line else {
                return Some(Err(TooLong));
            };

            if mem::take(&mut self.first) {
                line = line.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(line);
            }

            // A blank line ends the event: one with data is an item whatever its name, but a problem; one with none is
            // dropped.
            if line.is_empty() {
                let piece = if mem::take(&mut self.problem) {
                    Piece::Problem
                } else {
                    Piece::Item
                };

                if !self.data.is_empty() {
                    // Without the line break the last line of data adds.
                    let item = read(piece, &self.data[..self.data.len() - 1]);

                    self.data.clear();
                    return Some(Ok(item));
                }

                self.data.clear();
                continue;
            }

            // A comment, as the keep-alive the server sends.
            if line.starts_with(b":") {
                continue;
            }

            let (field, value) = match line.iter().position(|&byte| byte == b':') {
                Some(colon) => {
                    let value = &line[colon + 1..];

                    (&line[..colon], value.strip_prefix(b" ").unwrap_or(value))
                }
                None => (line, &b""[..]),
            };

            match field {
                b"data" => {
                    self.data.extend_from_slice(value);
                    self.data.push(b'\n');

                    // The line break the last line of data adds is not the event's.
                    if self.data.len() > ITEM_LIMIT + 1 {
                        return Some(Err(TooLong));
                    }
                }
                b"event" => self.problem = value == b"problem",
                // `id`, `retry`, and any field an `EventSource` does not know.
                _ => {}
            }
        }

        None
    }

    fn finish(&mut self) {
        // An event the body ends before its blank line is dropped, as an `EventSource` drops it.
    }
}

/// JSON Lines: each line that is not blank is an item, the last one too when no line break ends it.
#[derive(Debug)]
pub(crate) struct JsonLineStream {
    lines: Lines,
    /// Whether the body has ended: what is left of it is a last line with no line break.
    ended: bool,
}

impl Default for JsonLineStream {
    fn default() -> Self {
        Self {
            lines: Lines::new(false),
            ended: false,
        }
    }
}

impl Framing for JsonLineStream {
    fn push(&mut self, chunk: &[u8]) {
        self.lines.push(chunk);
    }

    fn next_item<R>(&mut self, read: impl FnOnce(Piece, &[u8]) -> R) -> Option<Result<R, TooLong>> {
        while let Some(line) = self.lines.next_line() {
            let Ok(line) = line else {
                return Some(Err(TooLong));
            };

            // JSON allows the `\r` of a `\r\n` around a value, as any blank.
            if !line.trim_ascii().is_empty() {
                return Some(Ok(read(Piece::Item, line)));
            }
        }

        if !self.ended {
            return None;
        }

        let rest = self.lines.rest();

        (!rest.trim_ascii().is_empty()).then(|| Ok(read(Piece::Item, rest)))
    }

    fn finish(&mut self) {
        self.ended = true;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The items `framing` reads from `chunks`, as text, once the body has ended.
    fn read<F: Framing>(mut framing: F, chunks: &[&[u8]]) -> Vec<String> {
        let mut items = Vec::new();
        let mut next = |framing: &mut F| {
            while let Some(item) = framing.next_item(|piece, item| {
                let text = String::from_utf8(item.to_vec()).expect("the items are text");

                match piece {
                    Piece::Item => text,
                    Piece::Problem => format!("problem {text}"),
                }
            }) {
                items.push(item.expect("no item is too long"));
            }
        };

        for chunk in chunks {
            framing.push(chunk);
            next(&mut framing);
        }

        framing.finish();
        next(&mut framing);
        items
    }

    /// The items of `body` cut into chunks of `size` bytes.
    fn in_chunks<F: Framing + Default>(body: &[u8], size: usize) -> Vec<String> {
        let chunks: Vec<&[u8]> = body.chunks(size).collect();

        read(F::default(), &chunks)
    }

    #[test]
    fn an_event_is_its_data() {
        let items = read(
            EventStream::default(),
            &[b"data: 1\n\ndata:2\n\ndata:  3\n\n"],
        );

        assert_eq!(items, ["1", "2", " 3"]);
    }

    #[test]
    fn lines_of_data_join_with_a_line_break() {
        let items = read(
            EventStream::default(),
            &[b"data: {\"a\":\ndata: 1}\n\ndata\ndata\n\n"],
        );

        assert_eq!(items, ["{\"a\":\n1}", "\n"]);
    }

    #[test]
    fn an_event_reads_the_same_in_chunks_of_any_size() {
        let body = b"\xEF\xBB\xBFdata: \"caf\xC3\xA9\"\r\n\r\n: ping\r\nid: 7\r\ndata: [1,\r\ndata: 2]\r\r\ndata: 3\n\n";

        for size in 1..=body.len() {
            assert_eq!(
                in_chunks::<EventStream>(body, size),
                ["\"caf\u{e9}\"", "[1,\n2]", "3"],
                "{size}"
            );
        }
    }

    #[test]
    fn lines_end_with_crlf_lf_or_cr() {
        let items = read(
            EventStream::default(),
            &[b"data: 1\r\n\r\ndata: 2\r\rdata: 3\n\n"],
        );

        assert_eq!(items, ["1", "2", "3"]);
    }

    #[test]
    fn comments_unknown_fields_and_events_without_data_are_skipped() {
        let body = b": a comment\n\nid: 1\nretry: 10\nkind: x\ncolor\ndata: 1\n\nevent: ping\n\nevent: message\ndata: 3\n\nevent\ndata: 4\n\n";
        let items = read(EventStream::default(), &[body]);

        assert_eq!(items, ["1", "3", "4"]);
    }

    #[test]
    fn an_event_of_any_name_is_an_item_but_a_problem() {
        let body = b"event: message_start\ndata: 1\n\nevent: delta\ndata: 2\n\nevent: problem\ndata: {}\n\nevent: problems\ndata: 3\n\ndata: 4\n\n";
        let items = read(EventStream::default(), &[body]);

        // The name of a problem holds for its own event alone.
        assert_eq!(items, ["1", "2", "problem {}", "3", "4"]);
    }

    #[test]
    fn an_event_with_no_data_or_no_end_is_dropped() {
        let items = read(
            EventStream::default(),
            &[b"event: message\n\nid: 2\n\ndata: 1\n\ndata: cut"],
        );

        assert_eq!(items, ["1"]);
    }

    #[test]
    fn a_byte_order_mark_is_dropped_on_the_first_line_only() {
        let items = read(
            EventStream::default(),
            &[b"\xEF\xBB\xBFdata: 1\n\n\xEF\xBB\xBFdata: 2\n\n"],
        );

        assert_eq!(items, ["1"]);
    }

    #[test]
    fn a_json_line_is_an_item() {
        let items = read(JsonLineStream::default(), &[b"1\n{\"a\":2}\n\"three\"\n"]);

        assert_eq!(items, ["1", "{\"a\":2}", "\"three\""]);
    }

    #[test]
    fn a_json_line_reads_the_same_in_chunks_of_any_size() {
        let body = b"{\"a\":1}\r\n\n  \n[2,3]\n\"caf\xC3\xA9\"";

        for size in 1..=body.len() {
            assert_eq!(
                in_chunks::<JsonLineStream>(body, size),
                ["{\"a\":1}\r", "[2,3]", "\"caf\u{e9}\""],
                "{size}"
            );
        }
    }

    #[test]
    fn blank_json_lines_are_skipped_and_a_blank_end_is_nothing() {
        let items = read(JsonLineStream::default(), &[b"\n\n1\n \t\r\n2\n  "]);

        assert_eq!(items, ["1", "2"]);
        assert!(read(JsonLineStream::default(), &[b""]).is_empty());
    }

    #[test]
    fn a_lone_cr_does_not_end_a_json_line() {
        let items = read(JsonLineStream::default(), &[b"1\r2\n"]);

        assert_eq!(items, ["1\r2"]);
    }

    /// What `framing` gives once it holds `body`: the items before the first error, and the error.
    fn until_too_long<F: Framing>(mut framing: F, body: &[u8]) -> (usize, Option<TooLong>) {
        let mut items = 0;

        framing.push(body);

        while let Some(item) = framing.next_item(|_, _| ()) {
            match item {
                Ok(_) => items += 1,
                Err(error) => return (items, Some(error)),
            }
        }

        (items, None)
    }

    /// `len` bytes of a JSON string, quotes included.
    fn string(len: usize) -> Vec<u8> {
        let mut text = vec![b'a'; len];

        text[0] = b'"';
        text[len - 1] = b'"';
        text
    }

    #[test]
    fn a_line_of_up_to_1_mib_is_read_and_a_longer_one_is_an_error() {
        let whole = [string(ITEM_LIMIT), b"\n".to_vec()].concat();
        let longer = [string(ITEM_LIMIT + 1), b"\n".to_vec()].concat();

        assert_eq!(until_too_long(JsonLineStream::default(), &whole), (1, None));
        assert_eq!(
            until_too_long(JsonLineStream::default(), &longer),
            (0, Some(TooLong))
        );

        let event = [b"data: ".to_vec(), string(ITEM_LIMIT - 6), b"\n\n".to_vec()].concat();
        let longer = [b"data: ".to_vec(), string(ITEM_LIMIT - 5), b"\n\n".to_vec()].concat();

        assert_eq!(until_too_long(EventStream::default(), &event), (1, None));
        assert_eq!(
            until_too_long(EventStream::default(), &longer),
            (0, Some(TooLong))
        );
    }

    #[test]
    fn a_line_that_never_ends_is_an_error_once_past_1_mib() {
        let endless = string(ITEM_LIMIT + 1);

        assert_eq!(
            until_too_long(EventStream::default(), &endless),
            (0, Some(TooLong))
        );
        assert_eq!(
            until_too_long(JsonLineStream::default(), &endless),
            (0, Some(TooLong))
        );

        // Up to the limit, the line may still end.
        assert_eq!(
            until_too_long(JsonLineStream::default(), &string(ITEM_LIMIT)),
            (0, None)
        );
    }

    #[test]
    fn the_data_of_an_event_is_at_most_1_mib_whatever_its_lines() {
        let line = [b"data: ".to_vec(), string(1024), b"\n".to_vec()].concat();
        let many = [line.repeat(1100), b"\n".to_vec()].concat();

        assert_eq!(
            until_too_long(EventStream::default(), &many),
            (0, Some(TooLong))
        );
    }
}

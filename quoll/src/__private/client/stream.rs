//! The results a client reads as a stream: the body of an answer split into its items as its chunks come, server-sent
//! events or JSON Lines, each read as the JSON of an item.

use std::mem;

use futures_core::Stream;
use futures_util::{StreamExt, stream};
use serde::de::DeserializeOwned;

use super::no_answer;
use crate::rest::{Result, Route};

/// How the body of a stream is split into the JSON of its items.
pub(crate) trait Framing: Send + 'static {
    /// Adds the next chunk of the body.
    fn push(&mut self, chunk: &[u8]);

    /// The JSON of the next whole item, when the chunks so far hold one.
    fn next_item(&mut self) -> Option<Vec<u8>>;

    /// Says the body has ended: what is left makes an item, or is dropped.
    fn finish(&mut self);
}

/// The items of `response`'s body, split by `framing` and read as the JSON of a `T`, as its chunks come: a chunk is
/// read only when the stream is asked for an item that the chunks so far do not hold.
///
/// An item that does not read as a `T` is an error, and the stream goes on. A body that cannot be read is an error,
/// and the stream ends after it, leaving out what it had of an item. Dropping the stream drops the answer, which
/// closes its connection.
pub(crate) fn items<T, F>(
    response: reqwest::Response,
    route: &'static Route,
    framing: F,
) -> impl Stream<Item = Result<T>> + Send + 'static
where
    T: DeserializeOwned + Send + 'static,
    F: Framing,
{
    let state = (Some(response), framing);

    stream::unfold(state, move |(mut response, mut framing)| async move {
        loop {
            if let Some(item) = framing.next_item() {
                let item = serde_json::from_slice(&item)
                    .map_err(|error| no_answer(route, NOT_AN_ITEM, error));

                return Some((item, (response, framing)));
            }

            match response.as_mut()?.chunk().await {
                Ok(Some(chunk)) => framing.push(&chunk),
                Ok(None) => {
                    response = None;
                    framing.finish();
                }
                Err(error) => {
                    let error = no_answer(route, "got an answer whose body cannot be read", error);

                    return Some((Err(error), (None, framing)));
                }
            }
        }
    })
    .fuse()
}

/// What the error of an item that does not read as the route's says.
const NOT_AN_ITEM: &str = "got an item that does not read as its type";

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

    /// The next whole line, without its line break.
    fn next_line(&mut self) -> Option<&[u8]> {
        while let Some(&byte) = self.buffer.get(self.searched) {
            let at = self.searched;

            self.searched += 1;

            if mem::take(&mut self.after_cr) && byte == b'\n' {
                self.start = self.searched;
                continue;
            }

            if byte == b'\n' || (self.cr_breaks && byte == b'\r') {
                let line = self.start..at;

                self.after_cr = byte == b'\r';
                self.start = self.searched;
                return Some(&self.buffer[line]);
            }
        }

        None
    }

    /// What is left of the body once it has ended: a last line with no line break, or nothing.
    fn take_rest(&mut self) -> Vec<u8> {
        let rest = self.buffer.split_off(self.start);

        *self = Self::new(self.cr_breaks);
        rest
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
    /// Whether the event being read is a message: it has no name, or its name is `message`.
    message: bool,
}

impl Default for EventStream {
    fn default() -> Self {
        Self {
            lines: Lines::new(true),
            first: true,
            data: Vec::new(),
            message: true,
        }
    }
}

impl Framing for EventStream {
    fn push(&mut self, chunk: &[u8]) {
        self.lines.push(chunk);
    }

    fn next_item(&mut self) -> Option<Vec<u8>> {
        while let Some(mut line) = self.lines.next_line() {
            if mem::take(&mut self.first) {
                line = line.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(line);
            }

            // A blank line ends the event: a message with data is an item, any other event is dropped.
            if line.is_empty() {
                let message = mem::replace(&mut self.message, true);
                let mut data = mem::take(&mut self.data);

                if message && data.pop().is_some() {
                    return Some(data);
                }

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
                }
                b"event" => self.message = value.is_empty() || value == b"message",
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
    /// The last line, once the body has ended without a line break after it.
    rest: Option<Vec<u8>>,
}

impl Default for JsonLineStream {
    fn default() -> Self {
        Self {
            lines: Lines::new(false),
            rest: None,
        }
    }
}

impl Framing for JsonLineStream {
    fn push(&mut self, chunk: &[u8]) {
        self.lines.push(chunk);
    }

    fn next_item(&mut self) -> Option<Vec<u8>> {
        while let Some(line) = self.lines.next_line() {
            // JSON allows the `\r` of a `\r\n` around a value, as any blank.
            if !line.trim_ascii().is_empty() {
                return Some(line.to_vec());
            }
        }

        self.rest.take()
    }

    fn finish(&mut self) {
        let rest = self.lines.take_rest();

        if !rest.trim_ascii().is_empty() {
            self.rest = Some(rest);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The items `framing` reads from `chunks`, as text, once the body has ended.
    fn read<F: Framing>(mut framing: F, chunks: &[&[u8]]) -> Vec<String> {
        let mut items = Vec::new();
        let mut next = |framing: &mut F| {
            while let Some(item) = framing.next_item() {
                items.push(String::from_utf8(item).expect("the items are text"));
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
    fn comments_unknown_fields_and_other_events_are_skipped() {
        let body = b": a comment\n\nid: 1\nretry: 10\nkind: x\ncolor\ndata: 1\n\nevent: ping\ndata: 2\n\nevent: message\ndata: 3\n\nevent\ndata: 4\n\n";
        let items = read(EventStream::default(), &[body]);

        assert_eq!(items, ["1", "3", "4"]);
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
}

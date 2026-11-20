//! Docker's frames: the output of a container with no TTY, each piece of it after a header of 8 bytes, the stream it
//! was written to, three zeros, and the size of the piece, a big-endian `u32`.

use std::fmt;

const HEADER: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Output {
    Stdout,
    Stderr,
}

/// Splits frames as their bytes come, cut anywhere: a payload is given as it comes, never held whole, so that a long
/// line of a followed container shows as soon as its bytes do.
#[derive(Debug, Default)]
pub struct Demux {
    header: [u8; HEADER],
    /// How much of the next header has come.
    filled: usize,
    /// The output of the frame being read, and how many of its bytes are still to come.
    payload: Option<(Output, usize)>,
}

impl Demux {
    /// The pieces of payload `chunk` holds, each with its output, in order.
    pub fn push<'a>(&mut self, mut chunk: &'a [u8]) -> Result<Vec<(Output, &'a [u8])>, FrameError> {
        let mut pieces = Vec::new();

        while !chunk.is_empty() {
            if let Some((output, left)) = self.payload {
                let (piece, rest) = chunk.split_at(left.min(chunk.len()));
                let left = left - piece.len();

                pieces.push((output, piece));
                self.payload = (left > 0).then_some((output, left));
                chunk = rest;
                continue;
            }

            let (part, rest) = chunk.split_at((HEADER - self.filled).min(chunk.len()));

            self.header[self.filled..self.filled + part.len()].copy_from_slice(part);
            self.filled += part.len();
            chunk = rest;

            if self.filled < HEADER {
                break;
            }

            self.filled = 0;

            // Stdin, 0, goes with stdout, as Docker's own `stdcopy` writes it.
            let output = match self.header[0] {
                0 | 1 => Output::Stdout,
                2 => Output::Stderr,
                other => return Err(FrameError::UnknownStream(other)),
            };
            let size = u32::from_be_bytes([
                self.header[4],
                self.header[5],
                self.header[6],
                self.header[7],
            ]);

            self.payload = (size > 0).then_some((output, size as usize));
        }

        Ok(pieces)
    }

    /// Says the bytes have ended: an error when they ended inside a frame.
    pub fn finish(&self) -> Result<(), FrameError> {
        if self.filled > 0 || self.payload.is_some() {
            return Err(FrameError::Truncated);
        }

        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameError {
    /// A header naming a stream other than stdin, stdout and stderr: the bytes are not frames.
    UnknownStream(u8),
    /// The logs ended inside a frame.
    Truncated,
}

impl fmt::Display for FrameError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownStream(stream) => {
                write!(f, "the logs hold a frame of an unknown stream, {stream}")
            }
            Self::Truncated => f.write_str("the logs ended inside a frame"),
        }
    }
}

impl std::error::Error for FrameError {}

#[cfg(test)]
mod tests {
    use super::*;

    /// A frame of `payload`, written to `stream`.
    fn frame(stream: u8, payload: &[u8]) -> Vec<u8> {
        let size = u32::try_from(payload.len()).expect("a small payload");
        let mut frame = vec![stream, 0, 0, 0];

        frame.extend(size.to_be_bytes());
        frame.extend(payload);
        frame
    }

    /// What `demux` gives of `chunks`, the pieces of each output put end to end.
    fn read(demux: &mut Demux, chunks: &[&[u8]]) -> (Vec<u8>, Vec<u8>) {
        let (mut stdout, mut stderr) = (Vec::new(), Vec::new());

        for chunk in chunks {
            for (output, piece) in demux.push(chunk).expect("frames") {
                match output {
                    Output::Stdout => stdout.extend(piece),
                    Output::Stderr => stderr.extend(piece),
                }
            }
        }

        (stdout, stderr)
    }

    #[test]
    fn frames_cut_anywhere_give_each_output_its_bytes() {
        let bytes = [
            frame(1, b"hello\n"),
            frame(2, b"oops\n"),
            frame(0, b""),
            frame(1, b"bye\n"),
        ]
        .concat();

        for cut in 1..bytes.len() {
            for second in cut..bytes.len() {
                let mut demux = Demux::default();
                let chunks = [&bytes[..cut], &bytes[cut..second], &bytes[second..]];

                let (stdout, stderr) = read(&mut demux, &chunks);

                assert_eq!(stdout, b"hello\nbye\n", "cut at {cut} and {second}");
                assert_eq!(stderr, b"oops\n", "cut at {cut} and {second}");
                assert_eq!(demux.finish(), Ok(()));
            }
        }
    }

    #[test]
    fn a_payload_is_given_as_its_bytes_come() {
        let bytes = frame(1, b"a long line\n");
        let mut demux = Demux::default();

        let pieces = demux.push(&bytes[..10]).expect("frames");

        assert_eq!(pieces, [(Output::Stdout, &b"a "[..])]);
    }

    #[test]
    fn logs_that_end_inside_a_frame_are_an_error() {
        let bytes = frame(2, b"cut");

        for cut in 1..bytes.len() {
            let mut demux = Demux::default();

            demux.push(&bytes[..cut]).expect("frames");

            assert_eq!(demux.finish(), Err(FrameError::Truncated), "cut at {cut}");
        }
    }

    #[test]
    fn a_frame_of_an_unknown_stream_is_an_error() {
        let mut demux = Demux::default();

        assert_eq!(
            demux.push(&frame(7, b"x")),
            Err(FrameError::UnknownStream(7))
        );
    }
}

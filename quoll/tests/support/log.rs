//! What the tracing of a test logs: the events of its thread, which its server and its client run on, as
//! `#[tokio::test]` gives each test a runtime of one thread.
//!
//! One subscriber serves the whole test binary, set once as the global default, and writes each event into the log of
//! the thread it happens on, if that thread captures one. A subscriber per test, set as its thread's default, would
//! race: tracing caches whether a callsite is enabled across every subscriber, and a test running beside another, with
//! none, may find it cached as disabled and log nothing.

use std::cell::RefCell;
use std::sync::{Arc, Mutex, Once};

use tracing_subscriber::fmt::format::FmtSpan;

/// What a test's thread logged, while it captured it.
#[derive(Clone, Default)]
pub(crate) struct Log(Arc<Mutex<Vec<u8>>>);

thread_local! {
    /// The log of the test running on this thread, while it captures one.
    static CAPTURED: RefCell<Option<Log>> = const { RefCell::new(None) };
}

/// Captures the log of its thread until it is dropped.
pub(crate) struct Capture;

impl Drop for Capture {
    fn drop(&mut self) {
        CAPTURED.with_borrow_mut(|captured| *captured = None);
    }
}

/// Captures what is logged on this thread into a new log, until the guard is dropped. The first call of the binary
/// sets the subscriber, which logs the close of each span when `span_closes` is set: every call of one binary passes
/// the same.
pub(crate) fn capture(span_closes: bool) -> (Log, Capture) {
    static INSTALLED: Once = Once::new();

    INSTALLED.call_once(|| {
        let span_events = if span_closes {
            FmtSpan::CLOSE
        } else {
            FmtSpan::NONE
        };
        let subscriber = tracing_subscriber::fmt()
            .with_writer(|| ThreadLog)
            .with_ansi(false)
            .with_span_events(span_events)
            .finish();

        tracing::subscriber::set_global_default(subscriber)
            .expect("no other subscriber is set in this binary");
    });

    let log = Log::default();

    CAPTURED.with_borrow_mut(|captured| *captured = Some(log.clone()));
    (log, Capture)
}

impl Log {
    /// The text logged so far.
    pub(crate) fn text(&self) -> String {
        String::from_utf8(self.0.lock().expect("no writer panics").clone())
            .expect("the log is text")
    }
}

/// Writes an event into the log its thread captures, or nowhere.
struct ThreadLog;

impl std::io::Write for ThreadLog {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        CAPTURED.with_borrow(|captured| {
            if let Some(Log(log)) = captured {
                log.lock()
                    .expect("no writer panics")
                    .extend_from_slice(bytes);
            }
        });

        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

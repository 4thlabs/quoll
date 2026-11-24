// A status is an `http::StatusCode`, as `status()` gives it: a number is not one.
use quoll::{Error, ErrorKind};

fn missing(error: &Error) -> bool {
    error.kind() == ErrorKind::Status(404)
}

fn main() {
    let _ = missing;
}

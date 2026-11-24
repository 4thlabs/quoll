// `as_str` lends the code of a status from the kind: it is not `'static`.
use quoll::{Error, ErrorKind};

fn error_type(error: &Error) -> &'static str {
    error.kind().as_str()
}

fn main() {
    let _ = (error_type, ErrorKind::Timeout);
}

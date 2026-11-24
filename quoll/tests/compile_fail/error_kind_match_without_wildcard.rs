// Every kind of today, and no `_`: a kind added in a version would break the match, so rustc refuses it.
use quoll::{Error, ErrorKind};

fn what(error: &Error) -> &'static str {
    match error.kind() {
        ErrorKind::Status(_) => "an error status",
        ErrorKind::Connect => "refused",
        ErrorKind::Timeout => "slow",
        ErrorKind::Closed => "closed",
        ErrorKind::Body => "cut",
        ErrorKind::Decode => "does not read",
        ErrorKind::Middleware => "a middleware",
        ErrorKind::Invalid => "a mistake",
    }
}

fn main() {
    let _ = what;
}

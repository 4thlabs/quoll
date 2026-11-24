// `failed` is of every shape: with nothing that says which, rustc asks for it.
use quoll::{Error, ErrorKind};

fn main() {
    let error = Error::failed(ErrorKind::Decode, "not a note");

    assert_eq!(error.kind(), ErrorKind::Decode);
}

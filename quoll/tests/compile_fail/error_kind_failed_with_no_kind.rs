// The kind is not optional: a fake says which failure it plays.
use quoll::Error;

fn main() {
    let _: Error = Error::failed("the daemon is down");
}

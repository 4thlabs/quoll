//! The `web.dev` of the tests, as a bundler's server: listens on a port of its own, which it prints, until it is
//! stopped.

use std::io::Write;
use std::net::TcpListener;

fn main() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("a port is free");
    let address = listener.local_addr().expect("the listener has an address");

    println!("web-dev: listening on {address}");
    std::io::stdout().flush().expect("stdout is written");

    for stream in listener.incoming() {
        drop(stream);
    }
}

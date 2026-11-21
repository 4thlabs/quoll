//! The server of the contract: answers every request with its version and the contract's document, on the address
//! the tests give it in `FIXTURE_SERVER_ADDRESS`.

use std::io::{Read, Write};
use std::net::TcpListener;

/// What the tests change to see the server built and started again.
const VERSION: &str = "server 1";

fn main() {
    let address =
        std::env::var("FIXTURE_SERVER_ADDRESS").expect("the tests give the server its address");
    let listener = TcpListener::bind(&address).expect("the server's address is free");

    for stream in listener.incoming() {
        let Ok(mut stream) = stream else {
            continue;
        };
        let body = format!("{VERSION} {}", contract::DOCUMENT);
        let mut request = [0; 1024];

        // A connection that only checks the server answers sends nothing, and closes. A client that went away is no
        // failure of the server.
        if stream.read(&mut request).is_ok_and(|read| read > 0) {
            let _ = write!(
                stream,
                "HTTP/1.0 200 OK\r\nContent-Length: {}\r\n\r\n{body}",
                body.len()
            );
        }
    }
}

use quoll::rest::{self, Bearer};

fn main() {
    // `key` sends the credential of a scheme the contract declares, which `Bearer` is not.
    let _ = rest::client::key::<Bearer>("token");
}

use quoll::client;
use quoll::rest::Bearer;

fn main() {
    // `key` sends the credential of a scheme the contract declares, which `Bearer` is not.
    let _ = client::key::<Bearer>("token");
}

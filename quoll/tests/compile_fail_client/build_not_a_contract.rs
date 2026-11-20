use quoll::client;

/// A type of the application's, which no contract names as its api.
struct Notes;

fn main() {
    let _ = client::builder().build::<Notes>("http://localhost:8080");
}

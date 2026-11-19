use quoll::rest;

// A header scheme in a header HTTP writes.
rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "Content-Type";
}

fn main() {}

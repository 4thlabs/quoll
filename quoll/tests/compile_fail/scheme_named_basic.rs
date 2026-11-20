use quoll::rest;

// A scheme named as one of the crate's.
rest::api! {
    pub api Api = "/api";

    pub scheme Basic = header "X-Login";
}

fn main() {}

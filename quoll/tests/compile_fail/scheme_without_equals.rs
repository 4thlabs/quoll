use quoll::rest;

// A scheme without its `=`.
rest::api! {
    pub api Api = "/api";

    pub scheme Key header "X-Api-Key";
}

fn main() {}

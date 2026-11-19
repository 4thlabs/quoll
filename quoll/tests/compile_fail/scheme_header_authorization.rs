use quoll::rest;

// A header scheme in `Authorization`, which `Bearer` and `Basic` read.
rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "Authorization";
}

fn main() {}

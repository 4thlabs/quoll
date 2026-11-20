use quoll::rest;

// A header name that is not a token.
rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "X Api Key";
}

fn main() {}

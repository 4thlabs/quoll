use quoll::rest;

// A cookie name that is not a token.
rest::api! {
    pub api Api = "/api";

    pub scheme Session = cookie "my session";
}

fn main() {}

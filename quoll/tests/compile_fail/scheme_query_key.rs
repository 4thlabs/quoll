use quoll::rest;

// A query key that needs escaping.
rest::api! {
    pub api Api = "/api";

    pub scheme Token = query "the token";
}

fn main() {}

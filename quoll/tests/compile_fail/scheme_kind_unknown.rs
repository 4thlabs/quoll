use quoll::rest;

// A credential in a place no scheme reads.
rest::api! {
    pub api Api = "/api";

    pub scheme Key = body "key";
}

fn main() {}

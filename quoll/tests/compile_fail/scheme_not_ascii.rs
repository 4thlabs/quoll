use quoll::rest;

// A scheme name that is not ASCII, which `WWW-Authenticate` could not carry.
rest::api! {
    pub api Api = "/api";

    pub scheme Clé = header "X-Api-Key";
}

fn main() {}

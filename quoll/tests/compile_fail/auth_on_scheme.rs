use quoll::rest;

// `#[auth]` on a scheme.
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}

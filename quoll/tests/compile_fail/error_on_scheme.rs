use quoll::rest;

// `#[error]` on a scheme.
rest::api! {
    pub api Api = "/api";

    #[error(String)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}

use quoll::rest;

// `#[openapi(none)]` on a scheme, which the document describes as any other.
rest::api! {
    pub api Api = "/api";

    #[openapi(none)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}

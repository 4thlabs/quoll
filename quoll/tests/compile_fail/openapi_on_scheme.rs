// `#[openapi(none)]` on a scheme, which the document describes as any other.
quoll::api! {
    pub api Api = "/api";

    #[openapi(none)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}

// `#[auth]` on a scheme.
quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}

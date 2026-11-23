// `#[error]` on a scheme.
quoll::api! {
    pub api Api = "/api";

    #[error(String)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}

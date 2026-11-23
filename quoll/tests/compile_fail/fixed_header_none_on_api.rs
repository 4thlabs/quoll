quoll::api! {
    // An api has no parent to leave a header of.
    #[openapi(none)]
    #[header("Accept" = none)]
    pub api Api = "";
}

fn main() {}

use quoll::rest;

mod other {
    use quoll::rest;

    rest::api! {
        #[openapi(none)]
        pub api Other = "";

        pub scheme Key = header "X-Api-Key";
    }
}

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    // An argument that reads the credential of a scheme of another block.
    #[auth(other::Key)]
    pub group Admin {
        GET "/stats" => stats(header("X-Api-Key") key: String) -> u32;
    }
}

fn main() {}

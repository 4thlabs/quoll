mod other {
    quoll::api! {
        #[openapi(none)]
        pub api Other = "";

        pub scheme Key = header "X-Api-Key";
    }
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    // An argument that reads the credential of a scheme of another block.
    #[auth(other::Key)]
    pub group Admin {
        GET "/stats" => stats(header("X-Api-Key") key: String) -> u32;
    }
}

fn main() {}

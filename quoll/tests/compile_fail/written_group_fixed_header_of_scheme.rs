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

    // A scheme of another block, which the macro knows by its type alone.
    #[auth(other::Key)]
    #[header("X-Api-Key" = "public")]
    pub group Admin {
        GET "/stats" => stats() -> u32;
    }
}

fn main() {}

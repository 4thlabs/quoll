mod admin {
    quoll::group! {
        #[openapi(none)]
        #[auth(crate::Key)]
        pub group Admin {
            GET "/stats" => stats() -> u32;
        }
    }
}

quoll::api! {
    // The api's header is the credential of a scheme a route of the group takes.
    #[openapi(none)]
    #[header("X-Api-Key" = "public")]
    pub api Api = "" { admin::Admin };

    pub scheme Key = header "X-Api-Key";
}

fn main() {}
